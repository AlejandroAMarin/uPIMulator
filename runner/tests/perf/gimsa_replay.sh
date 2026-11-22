#!/bin/bash
# GIMSA's phases, one DPU at a time, hardware against the simulator.
#
#   UPIM_RECORD=/tmp/rec/dpu_%u.rec UPIM_RECORD_DPU=0,40,80,120,160,198 \
#       runner/upimulator run --tasklets 12 --dpus 199 -- ./bin/gimsa_phases_profile_...exe ...
#   runner/tests/perf/gimsa_replay.sh <kernel ELF> /tmp/rec/dpu_*.rec
#
# GIMSA needs 199 DPUs, so the machine cannot time one of them alone. The
# runner's shim records every copy one DPU got and every launch it ran;
# runner/scripts/replay_host.c replays that on one DPU, on the machine
# (REPS times, the median taken) and in the simulator. The phase counters
# are the 12 x 32 bytes the host reads back last: per tasklet the cycles in
# the table lookup, in the binary search, the search steps and the queries.
# Build the kernel with DPU_PHASE_PROFILE, as the phases-profile host does.
#
# The hardware runs ask for backend=hw deliberately.

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
elf="$1"; shift
work="${WORK_DIR:-/tmp/upim-gimsa-replay}"
reps="${REPS:-3}"
tasklets="${TASKLETS:-12}"

mkdir -p "$work"
gcc -O2 -Wall -o "$work/replay_host" "$runner/scripts/replay_host.c" $(dpu-pkg-config --cflags --libs dpu)

for record in "$@"; do
    name="$(basename "$record" .rec)"
    for r in $(seq "$reps"); do
        if [ "${SKIP_HW:-0}" != 1 ] || [ ! -s "$work/${name}_hw_$r.out" ]; then
            XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
                "$work/replay_host" "$elf" "$record" "$work/${name}_hw_$r.out" 2>/dev/null
        fi
    done
    "$runner/upimulator" run --tasklets "$tasklets" --dpus 1 -- \
        "$work/replay_host" "$elf" "$record" "$work/${name}_sim.out" >/dev/null 2>&1
done

python3 - "$work" "$reps" "$tasklets" "$@" <<'PY'
import os, statistics, struct, sys
work, reps, nt = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
def phases(path):
    data = open(path, 'rb').read(); pos = 0; last = None
    while pos < len(data):
        (n,) = struct.unpack_from('<I', data, pos); pos += 4
        if n == nt * 32:
            last = [sum(struct.unpack_from('<4Q', data, pos + 32 * t)[i] for t in range(nt)) for i in range(4)]
        pos += n
    return last
print(f"{'DPU':>10} {'queries':>8} {'lookup hw':>10} {'sim':>10} {'error':>7} {'search hw':>10} {'sim':>10} {'error':>7}")
tot = [0, 0, 0, 0]
for record in sys.argv[4:]:
    name = os.path.basename(record)[:-4]
    hw = [phases(f"{work}/{name}_hw_{r}.out") for r in range(1, reps + 1)]
    hc = statistics.median(h[0] for h in hw); hb = statistics.median(h[1] for h in hw)
    s = phases(f"{work}/{name}_sim.out")
    tot = [tot[0] + hc, tot[1] + s[0], tot[2] + hb, tot[3] + s[1]]
    print(f"{name:>10} {s[3]:>8} {hc:>10.0f} {s[0]:>10} {(s[0]/hc-1)*100:>+6.2f}% {hb:>10.0f} {s[1]:>10} {(s[1]/hb-1)*100:>+6.2f}%")
print(f"{'all':>10} {'':>8} {tot[0]:>10.0f} {tot[1]:>10} {(tot[1]/tot[0]-1)*100:>+6.2f}% {tot[2]:>10.0f} {tot[3]:>10} {(tot[3]/tot[2]-1)*100:>+6.2f}%")
PY
