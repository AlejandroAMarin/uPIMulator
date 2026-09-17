#!/bin/bash
# M4: sweep the DPU count, holding the work on each DPU constant.
#
#   sweep_dpus.sh <base-pair.seq> <seq-size> <pairs-per-dpu> <tasklets> \
#                 <cycles|instructions> <dpu counts...>
#
#   e.g. sweep_dpus.sh inputs/n2_l129_e2.seq 200 4 4 cycles 1 2 4 8
#
# BIMSA splits its pairs evenly across NR_DPUS, so the input is regenerated for
# each point to keep pairs-per-DPU fixed. A DPU that is given the same work
# should take the same number of cycles however many other DPUs run beside it:
# on the hardware they share nothing during a kernel. Any drift with the DPU
# count is the model composing badly, and that is what this looks for.
#
# The hardware runs ask for backend=hw deliberately.

set -eu

here="$(dirname "$(readlink -f "$0")")"
bimsa="${BIMSA_DIR:-/home/alex/PNM-simulator/bimsa}/upmem"
work="${WORK_DIR:-/tmp/upim-m4-dpus}"

base="$1"; seqsize="$2"; per_dpu="$3"; tasklets="$4"; mode="$5"; shift 5
dpu_counts="$*"

case "$mode" in
    cycles)       perf_flags="PERF_CYCLES=1 PERF_INSTRUCTIONS=0" ;;
    instructions) perf_flags="PERF_CYCLES=0 PERF_INSTRUCTIONS=1" ;;
    *) echo "sweep_dpus: mode must be cycles or instructions" >&2; exit 2 ;;
esac

mkdir -p "$work"

total() { awk '/^\[PERF\] total/ {print $3; exit}' "$1"; }
live()  { sed -n 's/.*, \([0-9]*\) live tasklets.*/\1/p' "$1" | head -1; }
gate()  {
    grep -q "CPU recovery for 0 pairs (0.00%)" "$1" && grep -q "Results are equal" "$1"
}

printf '%-6s %6s %14s %14s %9s %12s %12s %8s\n' \
    dpus pairs "hardware" "uPIMulator" "error" "hw/dpu-tl" "sim/dpu-tl" "live"

for nd in $dpu_counts; do
    pairs=$((per_dpu * nd))

    # BIMSA's input is one pair per two lines, so repeating the base file
    # repeats the pair.
    input="$work/pairs$pairs.seq"
    : >"$input"
    for _ in $(seq 1 "$pairs"); do cat "$base" >>"$input"; done

    vars="NR_TASKLETS=$tasklets NR_DPUS=$nd PERF=1 PERF_TIER=3 $perf_flags DYNAMIC=0 BATCH_SIZE=0"

    (cd "$bimsa" && make clean >/dev/null 2>&1)
    "$here/build_bimsa.sh" "$work/d$nd" $vars >/dev/null
    (cd "$bimsa" && make $vars >/dev/null 2>&1)

    hw="$work/d$nd.hw.txt"
    sim="$work/d$nd.sim.txt"

    (cd "$bimsa" && UPMEM_PROFILE=backend=hw ./bin/bimsa_host \
        -i "$input" -s "$seqsize" -e 1 -w 0) >"$hw" 2>&1

    (cd "$bimsa" && UPIM_STATS="$work/d$nd.stats.tsv" \
        "$here/../../upimulator" run --program-dir "$work/d$nd/bin" \
        --tasklets "$tasklets" --dpus "$nd" -- ./bin/bimsa_host \
        -i "$input" -s "$seqsize" -e 1 -w 0) >"$sim" 2>&1

    if ! gate "$hw"; then printf '%-6s %6s %14s\n' "$nd" "$pairs" "HW NOT VALID"; continue; fi
    if ! gate "$sim"; then printf '%-6s %6s %14s\n' "$nd" "$pairs" "SIM NOT VALID"; continue; fi

    h=$(total "$hw"); s=$(total "$sim")
    hl=$(live "$hw"); sl=$(live "$sim")

    if [ "$hl" != "$sl" ]; then
        printf '%-6s %6s %14s %14s %9s %12s %12s %8s\n' \
            "$nd" "$pairs" "$h" "$s" "LIVE DIFFERS" "-" "-" "$hl/$sl"
        continue
    fi

    # Each reading covers the whole DPU, so the sum over DPUs and tasklets
    # divided by how many recorded work is the per-DPU figure.
    awk -v nd="$nd" -v p="$pairs" -v h="$h" -v s="$s" -v l="$hl" 'BEGIN{
        printf "%-6s %6s %14s %14s %+8.2f%% %12.0f %12.0f %8s\n",
               nd, p, h, s, 100.0*(s-h)/h, h/l, s/l, l }'
done
