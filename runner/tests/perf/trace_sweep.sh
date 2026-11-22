#!/bin/bash
# Record when each tasklet's reads and writes start and finish, on the machine
# and in the simulator, and print what each tasklet waited per iteration.
#
#   TASKLETS=4 GAP=300 runner/tests/perf/trace_sweep.sh [work-dir]
#   TASKLETS=2 GAP=900 READS=2 runner/tests/perf/trace_sweep.sh   # PrIM va's pattern
#
# The "read" column is both reads when READS=2.
#
# See trace_kernel.c. The hardware run asks for backend=hw deliberately.

set -eu
# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-trace}"
t="${TASKLETS:-4}"
export ITERS="${ITERS:-30}" SIZE="${SIZE:-1024}" GAP="${GAP:-300}" READS="${READS:-1}" TASKLETS="$t"

export PATH="/home/alex/go-sdk/bin:$PATH"
mkdir -p "$work"
gcc -O2 -Wall -o "$work/host" "$here/trace_host.c" $(dpu-pkg-config --cflags --libs dpu)
dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -o "$work/dpu_$t" "$here/trace_kernel.c"
dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS="$t" -DNR_THREADS=24 \
    -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" -o "$work/k_$t.c.o" "$here/trace_kernel.c"
sed -i '/\.section\t\.stack_sizes/,+2d' "$work/k_$t.c.o"
if [ "${SKIP_LINK:-0}" != 1 ] || [ ! -s "$work/bin_$t/iram.bin" ]; then
    rm -rf "$work/bin_$t"; mkdir -p "$work/bin_$t"
    "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/bin_$t" \
        --benchmark TRACE --benchmark_asm "$work/k_$t.c.o" --link_only true \
        --num_tasklets "$t" --num_channels 1 --num_ranks_per_channel 1 \
        --num_dpus_per_rank 1 >"$work/link_$t.log" 2>&1 ||
        { echo "link failed; see $work/link_$t.log" >&2; exit 1; }
fi
if [ "${SKIP_HW:-0}" != 1 ] || [ ! -s "$work/hw_$t.tsv" ]; then
    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw XBIN="$work/dpu_$t" "$work/host" >"$work/hw_$t.tsv"
fi
XBIN="$work/dpu_$t" "$runner/upimulator" run $([ "${UPIM_ELF:-1}" = 1 ] || echo --program-dir "$work/bin_$t") --tasklets "$t" --dpus 1 \
    -- "$work/host" 2>/dev/null | grep -v check_symbols >"$work/sim_$t.tsv"

for side in hw sim; do
    echo "$side: cycles a tasklet spent in its reads and its write, per iteration"
    awk -F'\t' 'NR>1 {r[$1][$2]=$5-$3; w[$1][$2]=$6-$5; if ($2>mi) mi=$2; if ($1>mt) mt=$1}
        END {printf "%4s", "iter"; for (t=0;t<=mt;t++) printf "   t%-2d read write", t; print "";
             for (i=0;i<=mi;i++) {printf "%4d", i; for (t=0;t<=mt;t++) printf " %9d %5d", r[t][i], w[t][i]; print ""}}' \
        "$work/${side}_$t.tsv"
done
