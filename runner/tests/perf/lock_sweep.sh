#!/bin/bash
# What a contended lock costs, hardware against the simulator. See
# lock_kernel.c.
#
#   TASKLETS_LIST="2 4 8 16" runner/tests/perf/lock_sweep.sh [work-dir]
#
# The hardware runs ask for backend=hw deliberately.

set -eu
# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-lock}"
tasklets="${TASKLETS_LIST:-2 4 8 16}"
# LKMODE picks what a call is; see lock_kernel.c.
lkmode="${LKMODE:-0}"

export PATH="/home/alex/go-sdk/bin:$PATH"
mkdir -p "$work"
gcc -O2 -Wall -o "$work/host" "$here/lock_host.c" $(dpu-pkg-config --cflags --libs dpu)

for t in $tasklets; do
    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -DLKMODE="$lkmode" -o "$work/dpu_$t" "$here/lock_kernel.c"
    dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS="$t" -DNR_THREADS=24 -DLKMODE="$lkmode" \
        -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" -o "$work/k_$t.c.o" "$here/lock_kernel.c"
    sed -i '/\.section\t\.stack_sizes/,+2d' "$work/k_$t.c.o"
    rm -rf "$work/bin_$t"; mkdir -p "$work/bin_$t"
    "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/bin_$t" \
        --benchmark LOCK --benchmark_asm "$work/k_$t.c.o" --link_only true \
        --num_tasklets "$t" --num_channels 1 --num_ranks_per_channel 1 \
        --num_dpus_per_rank 1 >"$work/link_$t.log" 2>&1 ||
        { echo "link failed; see $work/link_$t.log" >&2; exit 1; }
    if [ "${SKIP_HW:-0}" != 1 ] || [ ! -s "$work/hw_$t.tsv" ]; then
        TASKLETS="$t" XPROFILE=backend=hw UPMEM_PROFILE=backend=hw XBIN="$work/dpu_$t" \
            "$work/host" >"$work/hw_$t.tsv"
    fi
    TASKLETS="$t" XBIN="$work/dpu_$t" "$runner/upimulator" run $([ "${UPIM_ELF:-1}" = 1 ] || echo --program-dir "$work/bin_$t") \
        --tasklets "$t" --dpus 1 -- "$work/host" 2>/dev/null | grep -v check_symbols >"$work/sim_$t.tsv"
    echo
    echo "$t tasklets: cycles from the start until each tasklet's calls are done"
    paste <(tail -n +2 "$work/hw_$t.tsv") <(tail -n +2 "$work/sim_$t.tsv" | cut -f3) |
        awk -F'\t' 'BEGIN{printf "%6s %8s %10s %10s\n","calls","tasklet","hardware","uPIMulator"}
            {printf "%6s %8s %10s %10s\n",$1,$2,$3,$4}'
done
