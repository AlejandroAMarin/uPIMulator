#!/bin/bash
# M4: measure BIMSA on this machine's hardware and on uPIMulator and report the
# error, over a sweep of tasklet counts.
#
#   sweep.sh <input.seq> <seq-size> <mode> [tasklet counts...]
#     mode: cycles | instructions
#   e.g. sweep.sh inputs/pairs16.seq 200 cycles 1 2 4 8 16
#
# Both sides run the same kernel source with the same macros: the uPIMulator
# image is built from the very command make would use for the hardware. Every
# data point is gated on "CPU recovery for 0 pairs (0.00%)" and on the two runs
# agreeing about how many tasklets did work, because the counter is one
# register the whole DPU shares -- each tasklet's reading counts every
# tasklet's work, so BIMSA's printed sum is inflated by the number of live
# tasklets. That cancels in the ratio only while both sides have the same
# number live.
#
# The hardware runs ask for backend=hw deliberately. Nothing else here may.

set -eu

# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.
elf="${UPIM_ELF:-1}"

here="$(dirname "$(readlink -f "$0")")"
bimsa="${BIMSA_DIR:-/home/alex/PNM-simulator/bimsa}/upmem"
work="${WORK_DIR:-/tmp/upim-m4}"

input="$1"; seqsize="$2"; mode="$3"; shift 3
tasklets="$*"

case "$mode" in
    cycles)       perf_flags="PERF_CYCLES=1 PERF_INSTRUCTIONS=0" ;;
    instructions) perf_flags="PERF_CYCLES=0 PERF_INSTRUCTIONS=1" ;;
    *) echo "sweep: mode must be cycles or instructions" >&2; exit 2 ;;
esac

mkdir -p "$work"

# total <file>: the [PERF] total row's sum column.
total() { awk '/^\[PERF\] total/ {print $3; exit}' "$1"; }
# live <file>: how many tasklets recorded work.
live()  { sed -n 's/.*, \([0-9]*\) live tasklets.*/\1/p' "$1" | head -1; }
# gate <file>: the run is only analysable if the DPU did all the work and the
# results were right.
gate()  {
    grep -q "CPU recovery for 0 pairs (0.00%)" "$1" && grep -q "Results are equal" "$1"
}

printf '%-9s %14s %14s %9s %8s %8s\n' \
    tasklets "hardware" "uPIMulator" "error" "hw_live" "sim_live"

for nt in $tasklets; do
    vars="NR_TASKLETS=$nt NR_DPUS=1 PERF=1 PERF_TIER=3 $perf_flags DYNAMIC=0 BATCH_SIZE=0"

    (cd "$bimsa" && make clean >/dev/null 2>&1)
    "$here/build_bimsa.sh" "$work/t$nt" $vars >/dev/null
    (cd "$bimsa" && make $vars >/dev/null 2>&1)

    hw="$work/t$nt.hw.txt"
    sim="$work/t$nt.sim.txt"

    (cd "$bimsa" && UPMEM_PROFILE=backend=hw ./bin/bimsa_host \
        -i "$input" -s "$seqsize" -e 1 -w 0) >"$hw" 2>&1

    (cd "$bimsa" && UPIM_STATS="$work/t$nt.stats.tsv" \
        "$here/../../upimulator" run $([ "$elf" = 1 ] || echo --program-dir "$work/t$nt/bin") \
        --tasklets "$nt" --dpus 1 -- ./bin/bimsa_host \
        -i "$input" -s "$seqsize" -e 1 -w 0) >"$sim" 2>&1

    if ! gate "$hw"; then printf '%-9s %14s\n' "$nt" "HW NOT VALID"; continue; fi
    if ! gate "$sim"; then printf '%-9s %14s\n' "$nt" "SIM NOT VALID"; continue; fi

    h=$(total "$hw"); s=$(total "$sim")
    hl=$(live "$hw"); sl=$(live "$sim")

    err=$(awk -v h="$h" -v s="$s" 'BEGIN{printf "%+.2f%%", 100.0*(s-h)/h}')
    printf '%-9s %14s %14s %9s %8s %8s\n' "$nt" "$h" "$s" "$err" "$hl" "$sl"
done
