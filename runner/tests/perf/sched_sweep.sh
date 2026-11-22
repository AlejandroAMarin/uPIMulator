#!/bin/bash
# The order the machine issues its threads in, hardware against the simulator.
#
#   runner/tests/perf/sched_sweep.sh [work-dir]
#   TASKLETS_LIST="4 8 16" runner/tests/perf/sched_sweep.sh
#
# Two probes, both read through who gets a contended atomic bit first:
# ring_kernel.c shows the rotation the boot chain leaves, grant_kernel.c the
# rotation after a barrier resumes its waiters, and the order the barrier's
# own bit was granted in. Each line prints the orders and whether the
# simulator's are the same as the machine's. The orders are deterministic on
# the machine, so any difference is a model error.
#
# The hardware runs ask for backend=hw deliberately.

set -eu
# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs.

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
work="${1:-/tmp/upim-sched}"
tasklets="${TASKLETS_LIST:-3 4 5 6 7 8 9 10 11 12 13 14 15 16}"

mkdir -p "$work"
for probe in ring grant; do
    gcc -O2 -Wall -o "$work/${probe}_host" "$here/${probe}_host.c" $(dpu-pkg-config --cflags --libs dpu)
done

# leave_order <file>: the tasklets sorted by the time they left the barrier.
leave_order() {
    head -1 "$1" | tr ' ' '\n' | tail -n +2 | awk -F: '{print $2, $1}' | sort -n | awk '{printf "%s ", $2}'
}

printf '%3s  %-5s %-48s %s\n' t probe hardware same
for t in $tasklets; do
    for probe in ring grant; do
        dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -o "$work/${probe}_$t" "$here/${probe}_kernel.c"
        TASKLETS="$t" XPROFILE=backend=hw UPMEM_PROFILE=backend=hw XBIN="$work/${probe}_$t" \
            "$work/${probe}_host" >"$work/${probe}_hw_$t.txt"
        TASKLETS="$t" XBIN="$work/${probe}_$t" "$runner/upimulator" run --tasklets "$t" --dpus 1 -- \
            "$work/${probe}_host" 2>/dev/null | grep -v check_symbols >"$work/${probe}_sim_$t.txt"

        hw_order="$(grep '^order' "$work/${probe}_hw_$t.txt" | cut -d' ' -f2-)"
        sim_order="$(grep '^order' "$work/${probe}_sim_$t.txt" | cut -d' ' -f2-)"
        same=$([ "$hw_order" = "$sim_order" ] && echo yes || echo "no, $sim_order")
        printf '%3s  %-5s %-48s %s\n' "$t" "$probe" "$hw_order" "$same"
        if [ "$probe" = grant ]; then
            hw_leave="$(leave_order "$work/grant_hw_$t.txt")"
            sim_leave="$(leave_order "$work/grant_sim_$t.txt")"
            same=$([ "$hw_leave" = "$sim_leave" ] && echo yes || echo "no, $sim_leave")
            printf '%3s  %-5s %-48s %s\n' "$t" leave "$hw_leave" "$same"
        fi
    done
done
