#!/bin/bash
# PrIM's own kernels with PrIM's own hosts, on the machine and in the simulator.
#
#   runner/tests/prim/run_prim.sh [kernels...]            (default: all eleven)
#   TASKLETS_LIST="1 4 16" REPS=5 runner/tests/prim/run_prim.sh RED SEL
#
# Nothing of PrIM is edited. The kernel is golang/uPIMulator/benchmark/<K>/dpu/
# task.c compiled with -Dmain=prim_main and linked with probe_main.c, which
# counts cycles or instructions over the whole kernel in OUT_COUNT. The host
# is that benchmark's host/app.c, built for 1 DPU; it checks its own results,
# and a point counts only if it prints that they are equal. The count comes
# from launch_probe.c on the machine and from the shim in the simulator
# (PROBE_OUT); a kernel launched more than once (SCAN-*, MLP, TRNS) reports
# the sum over its launches.
#
# va and hst have their own hosts in run.sh; this covers the other eleven.
# The machine runs ask for backend=hw deliberately. SKIP_HW=1 skips them,
# SKIP_SIM=1 skips the simulator (to size an input on the machine first).

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
bench="$runner/../golang/uPIMulator/benchmark"
work="${WORK_DIR:-/tmp/upim-prim-own}"
tasklets="${TASKLETS_LIST:-1 4 16}"
reps="${REPS:-5}"
kernels="${*:-BS GEMV HST-L MLP RED SCAN-RSS SCAN-SSA SEL TRNS TS UNI}"

mkdir -p "$work"

# The macros of each benchmark's Makefile (its defaults), the host's own
# arguments, and an input size that keeps one run to a few million cycles.
# -w 0 -e 1: no warm-up launch, one timed repetition.
# MLP feeds each layer's output to the next, so its matrix must be square.
config() {
    case "$1" in
    BS)       dflags="";                                 hflags="-DPROBLEM_SIZE=2";                        args="-i 512" ;;
    GEMV)     dflags="-DBL=10";                          hflags="-DBL=10";                                 args="-m 64 -n 512" ;;
    HST-L)    dflags="-DBL=8 -DNR_HISTO=1";              hflags="-DBL=8 -DENERGY=0";                       args="-i 16384 -b 256 -f $work/hst_l_input.iml" ;;
    MLP)      dflags="-DBL=10";                          hflags="-DBL=10";                                 args="-m 128 -n 128" ;;
    RED)      dflags="-DBL=10 -DSINGLE -DHAND -DINT64 -DPERF=0"
              hflags="-DBL=10 -DSINGLE -DHAND -DINT64 -DENERGY=0 -DPERF=0";                                args="-i 65536" ;;
    SCAN-RSS) dflags="-DBL=10 -DINT64";                  hflags="-DBL=10 -DINT64 -DENERGY=0";              args="-i 32768" ;;
    SCAN-SSA) dflags="-DBL=10 -DINT64";                  hflags="-DBL=10 -DINT64 -DENERGY=0";              args="-i 32768" ;;
    SEL)      dflags="-DBL=10";                          hflags="-DBL=10 -DENERGY=0";                      args="-i 65536" ;;
    TRNS)     dflags="";                                 hflags="-DENERGY=0";                              args="-m 16 -n 8 -o 64 -p 1" ;;
    TS)       dflags="";                                 hflags="";                                        args="-n 512 -m 64" ;;
    UNI)      dflags="-DBL=10";                          hflags="-DBL=10 -DENERGY=0";                      args="-i 65536" ;;
    *) echo "run_prim: unknown kernel $1" >&2; exit 2 ;;
    esac
}

# HST-L reads an image PrIM does not ship. Any 16-bit big-endian values
# serve: the host computes its reference from the same file.
if [ ! -f "$work/hst_l_input.iml" ]; then
    python3 -c "
import struct, random
random.seed(1)
with open('$work/hst_l_input.iml', 'wb') as f:
    f.write(b''.join(struct.pack('>H', random.randrange(4096)) for _ in range(1 << 20)))"
fi

gcc -O2 -shared -fPIC -o "$work/liblaunch_probe.so" "$runner/scripts/launch_probe.c" \
    $(dpu-pkg-config --cflags dpu) -ldl
gcc -O2 -shared -fPIC -o "$work/libdpuload_override.so" "$runner/scripts/dpuload_override.c" \
    $(dpu-pkg-config --cflags dpu) -ldl

# run_once <hw|sim> <mode> <kernel ELF> <tasklets>: print the summed count,
# or FAIL when the host does not report equal results.
run_once() {
    local side="$1" mode="$2" elf="$3" t="$4" out counts
    counts="$work/counts.$$.$side"
    rm -f "$counts"
    if [ "$side" = hw ]; then
        out=$(cd "$work" && XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
            LD_PRELOAD="$work/liblaunch_probe.so:$work/libdpuload_override.so" \
            DPU_LOAD_OVERRIDE="$elf" PROBE_OUT="$counts" PROBE_MODE="$mode" \
            "$host" -w 0 -e 1 $args 2>&1) || true
    else
        out=$(cd "$work" && DPU_LOAD_OVERRIDE="$elf" PROBE_OUT="$counts" PROBE_MODE="$mode" \
            "$runner/upimulator" run --tasklets "$t" -- "$host" -w 0 -e 1 $args 2>&1) || true
    fi
    if ! printf '%s\n' "$out" | grep -qi 'are equal' || [ ! -s "$counts" ]; then
        echo FAIL
        return
    fi
    awk -F'\t' '{s += $3} END {print s}' "$counts"
    rm -f "$counts"
}

# The machine repeats to well under 1% on most kernels, but not on all; the
# median of several runs, with the smallest and largest, says how far.
hw_median() {
    local mode="$1" elf="$2" t="$3" i v vals=""
    for ((i = 0; i < reps; i++)); do
        v=$(run_once hw "$mode" "$elf" "$t")
        [ "$v" = FAIL ] && { echo FAIL FAIL FAIL; return; }
        vals="$vals$v\n"
    done
    printf "$vals" | sort -n | awk -v n="$reps" '
        NR == 1 {lo = $1} NR == int((n+1)/2) {med = $1} {hi = $1}
        END {print med, lo, hi}'
}

for k in $kernels; do
    config "$k"

    echo
    echo "$k, 1 DPU, $args"
    printf '%-9s %12s %12s %9s %12s %12s %9s  %s\n' \
        tasklets "cycles hw" "cycles sim" "error" "instr hw" "instr sim" "error" "hw cycles min..max"

    for t in $tasklets; do
        elf="$work/${k}_dpu_$t"
        dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" $dflags -I"$bench/$k/support" \
            -Dmain=prim_main -c -o "$elf.task.o" "$bench/$k/dpu/task.c"
        dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -c -o "$elf.probe.o" "$here/probe_main.c"
        dpu-upmem-dpurte-clang -O2 -DNR_TASKLETS="$t" -o "$elf" "$elf.task.o" "$elf.probe.o"

        # Some hosts read one result per tasklet, so the host is built for the
        # kernel's tasklet count too.
        host="$work/${k}_host_$t"
        gcc -w -std=c11 -O3 -I"$bench/$k/support" -o "$host" "$bench/$k/host/app.c" \
            -DNR_DPUS=1 -DNR_TASKLETS="$t" $hflags $(dpu-pkg-config --cflags --libs dpu) -lm

        if [ "${SKIP_HW:-0}" = 1 ]; then
            hc=-; hlo=-; hhi=-; hi=-
        else
            read -r hc hlo hhi <<<"$(hw_median 0 "$elf" "$t")"
            read -r hi _ _ <<<"$(hw_median 1 "$elf" "$t")"
        fi
        if [ "${SKIP_SIM:-0}" = 1 ]; then
            sc=-; si=-
        else
            sc=$(run_once sim 0 "$elf" "$t")
            si=$(run_once sim 1 "$elf" "$t")
        fi

        awk -v t="$t" -v hc="$hc" -v sc="$sc" -v hi="$hi" -v si="$si" -v lo="$hlo" -v up="$hhi" 'BEGIN{
            ce = (hc+0 > 0 && sc+0 > 0) ? sprintf("%+.2f%%", 100*(sc-hc)/hc) : "-"
            ie = (hi+0 > 0 && si+0 > 0) ? sprintf("%+.2f%%", 100*(si-hi)/hi) : "-"
            printf "%-9s %12s %12s %9s %12s %12s %9s  %s..%s\n", t, hc, sc, ce, hi, si, ie, lo, up
        }'
    done
done
