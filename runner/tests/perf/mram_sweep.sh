#!/bin/bash
# Compare MRAM access latency on hardware against the simulator.
#
#   runner/tests/perf/mram_sweep.sh [work-dir]
#
# With one tasklet a DMA blocks the only thread there is, so a cycle count is
# the latency itself rather than something the scheduler hid. That makes this
# the probe for the part of the timing model that kernel cycle counts depend
# on most: BIMSA's -4.9% came from here.
#
# The hardware run asks for backend=hw deliberately.

set -eu
# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-mram}"
iters="${ITERS:-300}"

export PATH="/home/alex/go-sdk/bin:$PATH"

rm -rf "$work"; mkdir -p "$work"

echo "building"
dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS=1 -o "$work/mram_dpu" "$here/mram_kernel.c"
dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS=1 -DNR_THREADS=24 \
    -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" -o "$work/mram.c.o" "$here/mram_kernel.c"
sed -i '/\.section\t\.stack_sizes/,+2d' "$work/mram.c.o"
gcc -O2 -Wall -o "$work/mram_host" "$here/mram_host.c" $(dpu-pkg-config --cflags --libs dpu)

mkdir -p "$work/bin"
"$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/bin" \
    --benchmark MRAMP --benchmark_asm "$work/mram.c.o" --link_only true \
    --num_tasklets 1 --num_channels 1 --num_ranks_per_channel 1 \
    --num_dpus_per_rank 1 >"$work/link.log" 2>&1 ||
    { echo "link failed; see $work/link.log" >&2; exit 1; }

echo "measuring hardware"
XPROFILE=backend=hw UPMEM_PROFILE=backend=hw ITERS="$iters" TASKLETS=1 \
    XBIN="$work/mram_dpu" "$work/mram_host" >"$work/hw.tsv"

echo "measuring uPIMulator"
ITERS="$iters" TASKLETS=1 XBIN="$work/mram_dpu" \
    "$runner/upimulator" run $([ "${UPIM_ELF:-1}" = 1 ] || echo --program-dir "$work/bin") --tasklets 1 --dpus 1 \
    -- "$work/mram_host" 2>/dev/null | grep -v check_symbols >"$work/sim.tsv"

for pattern in sequential scattered; do
    echo
    echo "$pattern: cycles per DMA, one tasklet"
    paste <(awk -F'\t' -v p="$pattern" 'NR>1 && $1==p {print $2"\t"$3}' "$work/hw.tsv") \
          <(awk -F'\t' -v p="$pattern" 'NR>1 && $1==p {print $3}' "$work/sim.tsv") |
        awk 'BEGIN{printf "%8s %10s %10s %8s\n","bytes","hardware","uPIMulator","error"}
             {printf "%8s %10.1f %10.1f %+7.1f%%\n",$1,$2,$3,100*($3-$2)/$2}'
done
