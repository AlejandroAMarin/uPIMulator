#!/bin/bash
# Compare the cost of a small MRAM read at a random address, hardware against
# the simulator, over tasklet counts and the compute between reads.
#
#   runner/tests/perf/scatter_sweep.sh [work-dir]
#
# GIMSA's binary search and BIMSA's extend are both about 3.5% fast in the
# simulator, and both make small reads. This separates three suspects: where
# the read lands (seq, a 64 KB stride, random in 8 MB, random in 32 MB), how
# many tasklets compete for the DMA engine, and how idle the engine is when a
# read arrives (XGAPS).
#
# The hardware runs ask for backend=hw deliberately.

set -eu
# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
# PROBE=mix runs mix_kernel.c and mix_host.c instead: a write after each read.
probe="${PROBE:-scatter}"
work="${1:-/tmp/upim-$probe}"
iters="${ITERS:-200}"
tasklets="${TASKLETS_LIST:-1 4 12 16}"
export XSIZES="${XSIZES:-8,16,64}"
export XGAPS="${XGAPS:-0,16}"
export XMODES="${XMODES:-seq,stride,rand8m,rand}"
# SKIP_HW=1 reuses the hardware numbers already in the work directory, so a
# model change set through UPIM_T_RP and the like costs one simulator run.
skip_hw="${SKIP_HW:-0}"
# SKIP_LINK=1 reuses the linked images too. The link does not depend on the
# timing constants, and it builds the SDK in the source tree, so two sweeps
# that link at the same time break each other.
skip_link="${SKIP_LINK:-0}"

export PATH="/home/alex/go-sdk/bin:$PATH"

[ "$skip_hw" = 1 ] || rm -rf "$work"
mkdir -p "$work"
gcc -O2 -Wall -o "$work/host" "$here/${probe}_host.c" $(dpu-pkg-config --cflags --libs dpu)

for t in $tasklets; do
    echo "building for $t tasklets" >&2
    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -o "$work/dpu_$t" "$here/${probe}_kernel.c"
    dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS="$t" -DNR_THREADS=24 \
        -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" -o "$work/k_$t.c.o" "$here/${probe}_kernel.c"
    sed -i '/\.section\t\.stack_sizes/,+2d' "$work/k_$t.c.o"

    if [ "$skip_link" = 1 ] && [ -s "$work/bin_$t/iram.bin" ]; then
        echo "link, $t tasklets (reused)" >&2
    else
    rm -rf "$work/bin_$t"; mkdir -p "$work/bin_$t"
    "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/bin_$t" \
        --benchmark SCATTER --benchmark_asm "$work/k_$t.c.o" --link_only true \
        --num_tasklets "$t" --num_channels 1 --num_ranks_per_channel 1 \
        --num_dpus_per_rank 1 >"$work/link_$t.log" 2>&1 ||
        { echo "link failed for $t; see $work/link_$t.log" >&2; exit 1; }
    fi

    if [ "$skip_hw" = 1 ] && [ -s "$work/hw_$t.tsv" ]; then
        echo "hardware, $t tasklets (reused)" >&2
    else
        echo "hardware, $t tasklets" >&2
        XPROFILE=backend=hw UPMEM_PROFILE=backend=hw ITERS="$iters" TASKLETS="$t" \
            XBIN="$work/dpu_$t" "$work/host" >"$work/hw_$t.tsv"
    fi

    echo "uPIMulator, $t tasklets" >&2
    ITERS="$iters" TASKLETS="$t" XBIN="$work/dpu_$t" \
        "$runner/upimulator" run $([ "${UPIM_ELF:-1}" = 1 ] || echo --program-dir "$work/bin_$t") --tasklets "$t" --dpus 1 \
        -- "$work/host" 2>/dev/null | grep -v check_symbols >"$work/sim_$t.tsv"
done

echo
echo "cycles per DMA, whole DPU"
printf '%-7s %6s %4s %3s %10s %10s %8s %7s\n' mode bytes gap t hardware uPIMulator error instr
for t in $tasklets; do
    paste <(tail -n +2 "$work/hw_$t.tsv") <(tail -n +2 "$work/sim_$t.tsv" | cut -f4,5) |
        awk -F'\t' -v t="$t" '{e=100*($6-$4)/$4
            flag=($5!=$7)?" instr differ":""
            printf "%-7s %6s %4s %3s %10.2f %10.2f %+7.2f%% %7.2f%s\n",$1,$2,$3,t,$4,$6,e,$5,flag}'
done
