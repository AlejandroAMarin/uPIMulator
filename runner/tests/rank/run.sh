#!/bin/bash
# Check that a set spanning several ranks behaves in the simulator as on the
# machine: how dpu_alloc lays the DPUs on ranks, DPU_RANK_FOREACH, and a copy,
# a push and a launch that reach one rank only. host.c prints the layout and
# checks every DPU's result; the two outputs must be identical.
#
# Usage: runner/tests/rank/run.sh [DPU counts...]     (default: 199 65 64 1)
#
# The machine runs ask for backend=hw deliberately. SKIP_HW=1 runs the
# simulator only and checks that host.c passes.

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
work="${WORK_DIR:-/tmp/upim-rank}"
counts="${*:-199 65 64 1}"

mkdir -p "$work"
dpu-upmem-dpurte-clang -O2 -DNR_TASKLETS=1 -o "$work/rank_dpu" "$here/kernel.c"
gcc -O2 -Wall -o "$work/rank_host" "$here/host.c" $(dpu-pkg-config --cflags --libs dpu)

failures=0

for n in $counts; do
    "$runner/upimulator" run --tasklets 1 -- "$work/rank_host" "$work/rank_dpu" "$n" 2>&1 |
        grep -v '^check_symbols' >"$work/sim_$n.txt" || true

    if [ "${SKIP_HW:-0}" = 1 ]; then
        if grep -q '^PASS' "$work/sim_$n.txt"; then
            echo "$n DPUs: simulator passes"
        else
            echo "$n DPUs: SIMULATOR FAILS"; cat "$work/sim_$n.txt"; failures=$((failures + 1))
        fi
        continue
    fi

    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw "$work/rank_host" "$work/rank_dpu" "$n" >"$work/hw_$n.txt" 2>&1 || true

    if ! grep -q '^PASS' "$work/hw_$n.txt"; then
        echo "$n DPUs: THE MACHINE FAILS"; cat "$work/hw_$n.txt"; failures=$((failures + 1))
    elif diff "$work/hw_$n.txt" "$work/sim_$n.txt" >"$work/diff_$n.txt"; then
        echo "$n DPUs: $(head -1 "$work/sim_$n.txt" | sed 's/^set: //'), same as the machine"
    else
        echo "$n DPUs: DIFFERS FROM THE MACHINE"; cat "$work/diff_$n.txt"; failures=$((failures + 1))
    fi
done

[ "$failures" = 0 ] && echo "all passed" || { echo "$failures failed"; exit 1; }
