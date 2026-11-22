#!/bin/bash
# A read's latency at one tasklet, and refresh lock-in, hardware against the
# simulator.
#
#   runner/tests/perf/latency_sweep.sh [work-dir]
#   SKIP_HW=1 UPIM_DMA_READ_ACK_CYCLES=6 runner/tests/perf/latency_sweep.sh
#
# refresh_kernel.c gives the mean latency of the reads refresh did not touch,
# exact to about a cycle, for five sizes. stair_kernel.c gives the cycles of
# a read loop with 0 to 24 more instructions in it, which the machine locks
# to the refresh period. SKIP_HW=1 reuses the hardware numbers in the work
# directory, so a model change costs only the simulator runs.
#
# The hardware runs ask for backend=hw deliberately.

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
work="${1:-/tmp/upim-latency}"
sizes="${SIZES:-8 16 64 256 1024}"

mkdir -p "$work"
for probe in refresh stair; do
    gcc -O2 -Wall -o "$work/${probe}_host" "$here/${probe}_host.c" $(dpu-pkg-config --cflags --libs dpu)
    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS=1 -o "$work/${probe}_dpu" "$here/${probe}_kernel.c"
done

hw() { [ "${SKIP_HW:-0}" = 1 ] && [ -s "$1" ] || "${@:2}" >"$1"; }
sim() { "$runner/upimulator" run --tasklets 1 --dpus 1 -- "$@" 2>/dev/null | grep -v check_symbols; }

# clean_mean <file>: the mean latency of the reads no refresh delayed.
clean_mean() {
    awk '{l[NR] = $2 - $1} END {
        n = asort(l); med = l[int(n / 2)]; s = 0; c = 0
        for (i = 1; i <= n; i++) if (l[i] < med + 32) { s += l[i]; c++ }
        printf "%.1f", s / c }' "$1"
}

echo "read latency at one tasklet, reads refresh did not delay [cycles]"
printf '%6s %10s %10s %8s\n' bytes hardware uPIMulator diff
for s in $sizes; do
    hw "$work/refresh_hw_$s.tsv" env SIZE="$s" XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
        XBIN="$work/refresh_dpu" "$work/refresh_host"
    SIZE="$s" XBIN="$work/refresh_dpu" sim "$work/refresh_host" >"$work/refresh_sim_$s.tsv"
    h=$(clean_mean "$work/refresh_hw_$s.tsv"); u=$(clean_mean "$work/refresh_sim_$s.tsv")
    printf '%6s %10s %10s %+8.1f\n' "$s" "$h" "$u" "$(echo "$u - $h" | bc)"
done

echo
echo "a read loop with m more instructions, cycles per iteration"
hw "$work/stair_hw.tsv" env XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
    XBIN="$work/stair_dpu" "$work/stair_host"
XBIN="$work/stair_dpu" sim "$work/stair_host" >"$work/stair_sim.tsv"
paste "$work/stair_hw.tsv" "$work/stair_sim.tsv" | awk '
    BEGIN { printf "%3s %10s %10s %8s\n", "m", "hardware", "uPIMulator", "diff" }
    { d = $5 - $2; s += d * d; printf "%3s %10s %10s %+8.2f\n", $1, $2, $5, d }
    END { printf "rms %.2f cycles\n", sqrt(s / NR) }'
