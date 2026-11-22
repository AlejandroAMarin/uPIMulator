#!/bin/bash
# Does a DMA transfer slow another thread's WRAM accesses? See wram_kernel.c.
#
#   TASKLETS_LIST="2 4" runner/tests/perf/wram_sweep.sh [work-dir]
#
# The hardware runs ask for backend=hw deliberately.

set -eu
# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-wram}"
tasklets="${TASKLETS_LIST:-2 4}"

export PATH="/home/alex/go-sdk/bin:$PATH"
mkdir -p "$work"
gcc -O2 -Wall -o "$work/host" "$here/wram_host.c" $(dpu-pkg-config --cflags --libs dpu)

for t in $tasklets; do
    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -o "$work/dpu_$t" "$here/wram_kernel.c"
    dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS="$t" -DNR_THREADS=24 \
        -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" -o "$work/k_$t.c.o" "$here/wram_kernel.c"
    sed -i '/\.section\t\.stack_sizes/,+2d' "$work/k_$t.c.o"
    rm -rf "$work/bin_$t"; mkdir -p "$work/bin_$t"
    "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/bin_$t" \
        --benchmark WRAMP --benchmark_asm "$work/k_$t.c.o" --link_only true \
        --num_tasklets "$t" --num_channels 1 --num_ranks_per_channel 1 \
        --num_dpus_per_rank 1 >"$work/link_$t.log" 2>&1 ||
        { echo "link failed; see $work/link_$t.log" >&2; exit 1; }
    if [ "${SKIP_HW:-0}" != 1 ] || [ ! -s "$work/hw_$t.tsv" ]; then
        XPROFILE=backend=hw UPMEM_PROFILE=backend=hw XBIN="$work/dpu_$t" "$work/host" >"$work/hw_$t.tsv"
    fi
    XBIN="$work/dpu_$t" "$runner/upimulator" run $([ "${UPIM_ELF:-1}" = 1 ] || echo --program-dir "$work/bin_$t") --tasklets "$t" \
        --dpus 1 -- "$work/host" 2>/dev/null | grep -v check_symbols >"$work/sim_$t.tsv"
    echo
    echo "$t tasklets: tasklet 0's loop, cycles per instruction"
    paste <(tail -n +2 "$work/hw_$t.tsv") <(tail -n +2 "$work/sim_$t.tsv" | cut -f3,5) |
        awk -F'\t' 'BEGIN{printf "%-5s %-6s %9s %9s %8s %9s %9s\n","fg","bg","hardware","uPIMul","error","hw_xfers","sim_xfers"}
            {printf "%-5s %-6s %9.3f %9.3f %+7.2f%% %9s %9s\n",$1,$2,$3,$6,100*($6-$3)/$3,$5,$7}'
done
