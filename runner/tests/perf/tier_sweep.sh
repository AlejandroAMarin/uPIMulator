#!/bin/bash
# Compare BIMSA section by section, hardware against the simulator.
#
#   runner/tests/perf/tier_sweep.sh [work-dir]
#   TIERS="1 2" NR_TASKLETS=1 runner/tests/perf/tier_sweep.sh
#
# sweep.sh compares a whole kernel. This splits it: BIMSA's own probes record
# where a pair's time goes (tier 1) and where the aligner's time goes inside
# that (tier 2). A model that is right overall for the wrong reasons shows up
# here as one section out by much more than the total.
#
# It builds gap-affine on purpose. The tier-1 and tier-2 probes all sit in the
# gap-affine path, so an edit-distance build reports no calls for any of them
# and 99.6% of the time in "other". Gap-affine is also the larger kernel: at
# 4062 instructions it only fits IRAM because the linker drops what nothing
# reaches. See HANDOFF.md.
#
# The hardware runs ask for backend=hw deliberately.

set -eu

# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.
elf="${UPIM_ELF:-1}"

here="$(dirname "$(readlink -f "$0")")"
bimsa="${BIMSA_DIR:-/home/alex/PNM-simulator/bimsa}/upmem"
work="${1:-/tmp/upim-tier}"
tasklets="${NR_TASKLETS:-1}"
tiers="${TIERS:-1 2}"
input="${INPUT:-/tmp/bimsa/pairs16.seq}"
seqsize="${SEQSIZE:-200}"

mkdir -p "$work"

for tier in $tiers; do
    vars="NR_TASKLETS=$tasklets NR_DPUS=1 AFFINE=1 MISMATCH=1 GAP_OPEN=1 GAP_EXTEND=1"
    vars="$vars PERF=1 PERF_TIER=$tier PERF_CYCLES=1 PERF_INSTRUCTIONS=0"
    vars="$vars DYNAMIC=0 BATCH_SIZE=0"

    (cd "$bimsa" && make clean >/dev/null 2>&1)
    "$here/build_bimsa.sh" "$work/t$tier" $vars >/dev/null
    (cd "$bimsa" && make $vars >/dev/null 2>&1)

    hw="$work/t$tier.hw.txt"
    sim="$work/t$tier.sim.txt"

    (cd "$bimsa" && UPMEM_PROFILE=backend=hw ./bin/bimsa_host \
        -i "$input" -s "$seqsize" -e 1 -w 0) >"$hw" 2>&1
    (cd "$bimsa" && "$here/../../upimulator" run $([ "$elf" = 1 ] || echo --program-dir "$work/t$tier/bin") \
        --tasklets "$tasklets" --dpus 1 -- ./bin/bimsa_host \
        -i "$input" -s "$seqsize" -e 1 -w 0) >"$sim" 2>&1

    for f in "$hw" "$sim"; do
        grep -q "Results are equal" "$f" ||
            { echo "tier $tier: $f did not produce the right answer" >&2; exit 1; }
    done

    echo
    echo "tier $tier, gap-affine, $tasklets tasklet(s), 1 DPU"
    printf '%-10s %14s %14s %9s %8s\n' section hardware uPIMulator error calls
    # Only the section rows: a name, a cycle count, a share, a call count and a
    # cost per call. The reconcile and gap lines have a different shape.
    sections() { awk '/^\[PERF\] [a-z_]+ +[0-9]+ +[0-9.]+% +[0-9]+ /{print $2, $3, $5}' "$1" | sort; }

    join -j 1 <(sections "$hw") <(sections "$sim") |
        awk '$2 + 0 > 0 {printf "%-10s %14s %14s %+8.2f%% %8s\n", $1, $2, $4, 100*($4-$2)/$2, $3}'
done
