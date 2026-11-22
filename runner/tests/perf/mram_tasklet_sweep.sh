#!/bin/bash
# Compare MRAM throughput under tasklet contention, hardware against simulator.
#
#   runner/tests/perf/mram_tasklet_sweep.sh [work-dir]
#
# mram_sweep.sh uses one tasklet, so a DMA blocks the only thread and the
# number it reports is a latency. This script runs the same kernel with more
# tasklets, which the revolver keeps ready, so the number it reports is the
# throughput of the memory controller instead. That separates the two halves
# of the timing model: BIMSA agrees to 1% below 8 tasklets and is 3% slow
# above, and only contention can explain a gap that grows with the thread
# count.
#
# The hardware run asks for backend=hw deliberately.

set -eu
# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-mramt}"
iters="${ITERS:-200}"
sizes="${XSIZES:-64,512,2048}"
pattern="${XPATTERN:-0}"
tasklets="${TASKLETS_LIST:-1 2 4 8 16}"

export PATH="/home/alex/go-sdk/bin:$PATH"

# SKIP_HW=1 reuses the hardware numbers already in the work directory. The
# hardware repeats to about 0.1% here, so re-measuring it while fitting the
# simulator only costs time.
skip_hw="${SKIP_HW:-0}"
[ "$skip_hw" = 1 ] || rm -rf "$work"
mkdir -p "$work"
gcc -O2 -Wall -o "$work/mram_host" "$here/mram_host.c" $(dpu-pkg-config --cflags --libs dpu)

for t in $tasklets; do
    echo "building for $t tasklets" >&2
    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -o "$work/dpu_$t" "$here/mram_kernel.c"
    dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS="$t" -DNR_THREADS=24 \
        -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" -o "$work/k_$t.c.o" "$here/mram_kernel.c"
    sed -i '/\.section\t\.stack_sizes/,+2d' "$work/k_$t.c.o"

    mkdir -p "$work/bin_$t"
    "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/bin_$t" \
        --benchmark MRAMP --benchmark_asm "$work/k_$t.c.o" --link_only true \
        --num_tasklets "$t" --num_channels 1 --num_ranks_per_channel 1 \
        --num_dpus_per_rank 1 >"$work/link_$t.log" 2>&1 ||
        { echo "link failed for $t; see $work/link_$t.log" >&2; exit 1; }

    if [ "$skip_hw" = 1 ] && [ -s "$work/hw_$t.tsv" ]; then
        echo "hardware, $t tasklets (reused)" >&2
    else
    echo "hardware, $t tasklets" >&2
    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw ITERS="$iters" TASKLETS="$t" \
        XSIZES="$sizes" XPATTERN="$pattern" XBIN="$work/dpu_$t" \
        "$work/mram_host" >"$work/hw_$t.tsv"
    fi

    echo "uPIMulator, $t tasklets" >&2
    ITERS="$iters" TASKLETS="$t" XSIZES="$sizes" XPATTERN="$pattern" XBIN="$work/dpu_$t" \
        "$runner/upimulator" run $([ "${UPIM_ELF:-1}" = 1 ] || echo --program-dir "$work/bin_$t") --tasklets "$t" --dpus 1 \
        -- "$work/mram_host" 2>/dev/null | grep -v check_symbols >"$work/sim_$t.tsv"
done

echo
echo "cycles per DMA, whole DPU (lower is faster; a DPU-wide counter, not a sum)"
printf '%8s %9s %12s %12s %9s\n' bytes tasklets hardware uPIMulator error
for t in $tasklets; do
    paste <(awk -F'\t' 'NR>1 {print $2"\t"$3}' "$work/hw_$t.tsv") \
          <(awk -F'\t' 'NR>1 {print $3}' "$work/sim_$t.tsv") |
        awk -v t="$t" '{printf "%8s %9s %12.2f %12.2f %+8.2f%%\n",$1,t,$2,$3,100*($3-$2)/$2}'
done
