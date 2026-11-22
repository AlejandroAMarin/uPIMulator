#!/bin/bash
# GIMSA's kernel time on the machine and in the simulator: the reference
# point for later changes to the simulated architecture.
#
#   runner/tests/perf/gimsa_reference.sh [sets...]
#   N=50000 REPS=3 runner/tests/perf/gimsa_reference.sh rice_L66 human_L132
#   SKIP_HW=1 runner/tests/perf/gimsa_reference.sh        (simulator only)
#
# Sets: rice_L66 rice_L88 rice_L132 human_L66 human_L88 human_L132 (default:
# all six). Each is the first N reads of a 10M Illumina set, one replica,
# 12 tasklets, the kernel_lut mode, K = 22. Every run must verify 100% against
# the unbounded oracle, or its point is FAIL.
#
# GIMSA's own kernel is compiled with -Dmain=prim_main and linked with
# runner/tests/prim/probe_main.c, which counts cycles over the whole kernel in
# OUT_COUNT; launch_probe.c reads it after every launch on the machine, the
# shim does in the simulator (PROBE_OUT). A launch lasts as long as its slowest
# DPU, so the kernel time is the sum over launches of the largest count,
# divided by the DPU clock (fck / clock_division in sysfs; 350 MHz here).
#
# The class column applies the accuracy rule (2026-10-07): inside 1% is
# on-target, up to 3% is valid (usable, but off target), above 3% is INVALID.
#
# Hosts and kernels are built from the GIMSA tree into WORK_DIR; GIMSA's own
# bin/ is left alone. The query slices go to WORK_DIR, and each slice's oracle
# goes next to the full one, as upimref_<set>_N<N>_K22.intv.bin, because the
# host finds it by the query file's name.
#
# Hardware runs ask for backend=hw deliberately. The simulator runs one set
# at a time: the human index needs about 28 GB in the host and as much again
# in the simulator. GOMEMLIMIT makes the Go collector return the transfer
# buffers: rice peaks at 28 GB with 16 GiB and at 39 GB without a limit. The
# limit must sit above the simulator's live heap, which is mostly MRAM (12 GB
# for rice, 32 GB for human): at 16 GiB a human run collects without pause and
# finished no launch in 3.5 hours. Human uses 40 GiB.

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
gimsa="${GIMSA_DIR:-/home/alex/PNM-simulator/gimsa}"
work="${WORK_DIR:-/tmp/upim-gimsa-ref}"
n="${N:-50000}"
reps="${REPS:-3}"
sets="${*:-rice_L66 rice_L88 rice_L132 human_L66 human_L88 human_L132}"

fck=$(cat /sys/class/dpu_rank/dpu_rank0/fck_frequency 2>/dev/null || echo 700)
div=$(cat /sys/class/dpu_rank/dpu_rank0/clock_division 2>/dev/null || echo 2)
mhz=$((fck / div))

# set -> organism, reference, query set, -d (DPUs of one replica), -u (SUBLUT_BITS),
# and the Go heap limit of the simulator run
config() {
    case "$1" in
    rice_*)  org=rice;  db=asian_rice;        dpus=199; sublut=8; golimit=16GiB ;;
    human_*) org=human; db=clean_humangenome; dpus=541; sublut=4; golimit=40GiB ;;
    esac
    case "$1" in
    rice_L66)   q=Ill_SRR10724346_asianrice_10M_L66 ;;
    rice_L88)   q=Ill_SRR10838753_asianrice_10M_L88 ;;
    rice_L132)  q=Ill_SRR8241153_asianrice_10M_L132 ;;
    human_L66)  q=Ill_SRR622461_homo_10M_L66 ;;
    human_L88)  q=Ill_SRR622457_homo_10M_L88 ;;
    human_L132) q=Ill_ERR2990063_human_10M_L132 ;;
    *) echo "gimsa_reference: unknown set $1" >&2; exit 2 ;;
    esac
}

mkdir -p "$work/queries"
gcc -O2 -shared -fPIC -o "$work/liblaunch_probe.so" "$runner/scripts/launch_probe.c" \
    $(dpu-pkg-config --cflags dpu) -ldl
gcc -O2 -shared -fPIC -o "$work/libdpuload_override.so" "$runner/scripts/dpuload_override.c" \
    $(dpu-pkg-config --cflags dpu) -ldl

# build <org> <SUBLUT_BITS>: the host and the probed kernel, with the flags of
# GIMSA's Makefiles (make gimsa_kernel_lut, make -C src/UPMEM-partitioner kernel_lut).
build() {
    local o="$1" b="$2" p="$gimsa/src/UPMEM-partitioner" stk="" i
    [ -x "$work/$o/gimsa_kernel_lut.exe" ] && [ -f "$work/$o/gimsa_kernel_lut_probe" ] && return
    mkdir -p "$work/$o"
    (cd "$gimsa" && make -s gimsa_kernel_lut SUBLUT_BITS="$b" BIN_DIR="$work/$o" >"$work/$o/host_build.log" 2>&1)
    for i in $(seq 0 11); do stk="$stk -Wl,--defsym=STACK_SIZE_TASKLET_$i=512"; done
    dpu-upmem-dpurte-clang -w -g -I"$p/support" -O2 -DNR_TASKLETS=12 -DNR_DPUS=2500 -DSTACK_SIZE=512 \
        -DDPU_WRAM_ALLOC_PROFILE=0 -DSUBLUT_BITS="$b" -DKERNEL_LUT=1 -Dmain=prim_main \
        -c -o "$work/$o/kernel.o" "$p/gimsa_kernel.c"
    dpu-upmem-dpurte-clang -w -g -O2 -DNR_TASKLETS=12 -c -o "$work/$o/probe.o" "$runner/tests/prim/probe_main.c"
    dpu-upmem-dpurte-clang -O2 -DNR_TASKLETS=12 $stk -o "$work/$o/gimsa_kernel_lut_probe" \
        "$work/$o/kernel.o" "$work/$o/probe.o"
}

# slice <set>: the first N reads and their oracle
slice() {
    local gt="$gimsa/dataset/${db}_bitwise/groundtruth_unbounded"
    query="$work/queries/upimref_$1_N$n.faencoded"
    [ -s "$query" ] && [ -s "$gt/upimref_$1_N${n}_K22.intv.bin" ] && return
    python3 -I - "$gimsa/dataset/$q.faencoded" "$gt/${q}_K22.intv.bin" "$n" \
        "$query" "$gt/upimref_$1_N${n}_K22.intv.bin" <<'PY'
import struct, sys
src_q, src_gt, n, dst_q, dst_gt = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
with open(src_q) as f, open(dst_q, 'w') as o:
    total, chunks, length = (int(f.readline()) for _ in range(3))
    assert n <= total
    o.write(f"{n}\n{chunks}\n{length}\n")
    for _ in range(n * chunks):
        o.write(f.readline())
with open(src_gt, 'rb') as f, open(dst_gt, 'wb') as o:
    hdr = bytearray(f.read(64))
    assert hdr[:7] == b'INTVGT1' and struct.unpack_from('<Q', hdr, 24)[0] >= n
    struct.pack_into('<Q', hdr, 24, n)
    o.write(hdr)
    o.write(f.read(16 * n))
PY
}

# run_once <hw|sim> <set> <rep>: print "<kernel cycles> <launches> <peak RSS kB>",
# or FAIL when the host does not verify every query.
run_once() {
    local side="$1" s="$2" r="$3" counts log
    counts="$work/$s.$side.$r.tsv"
    log="$work/$s.$side.$r.log"
    rm -f "$counts"
    local args=(-r "dataset/$db.fa" -q "$query" -k 22 -c 16384 -l 16 -d "$dpus" -b "$n" -t 4 -u "$sublut")
    if [ "$side" = hw ]; then
        (cd "$gimsa" && /usr/bin/time -v env XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
            LD_PRELOAD="$work/liblaunch_probe.so:$work/libdpuload_override.so" \
            DPU_LOAD_OVERRIDE="$work/$org/gimsa_kernel_lut_probe" PROBE_OUT="$counts" PROBE_MODE=0 \
            "$work/$org/gimsa_kernel_lut.exe" "${args[@]}") >"$log" 2>&1 || true
    else
        (cd "$gimsa" && /usr/bin/time -v env GOMEMLIMIT="${GOMEMLIMIT:-$golimit}" \
            DPU_LOAD_OVERRIDE="$work/$org/gimsa_kernel_lut_probe" PROBE_OUT="$counts" PROBE_MODE=0 \
            "$runner/upimulator" run --tasklets 12 --dpus "$dpus" -- \
            "$work/$org/gimsa_kernel_lut.exe" "${args[@]}") >"$log" 2>&1 || true
    fi
    if ! grep -q "PIM accuracy 100.000000: $n" "$log" || [ ! -s "$counts" ]; then
        echo FAIL
        return
    fi
    awk -F'\t' -v rss="$(awk '/Maximum resident/ {print $NF}' "$log")" '
        $3 > m[$1] {m[$1] = $3}
        END {for (l in m) {s += m[l]; c++} print s, c, rss}' "$counts"
}

printf '%-11s %8s %13s %13s %9s %9s %8s %-10s  %s\n' set launches "cycles hw" "cycles sim" "ms hw" "ms sim" error class "hw cycles min..max"
for s in $sets; do
    config "$s"
    build "$org" "$sublut"
    slice "$s"

    hc=-; hlo=-; hhi=-; launches=-
    if [ "${SKIP_HW:-0}" != 1 ]; then
        vals=""
        for ((i = 1; i <= reps; i++)); do
            read -r c l _ <<<"$(run_once hw "$s" "$i")"
            [ "$c" = FAIL ] && { vals=FAIL; break; }
            vals="$vals$c\n"; launches=$l
        done
        if [ "$vals" = FAIL ]; then
            hc=FAIL
        else
            read -r hc hlo hhi <<<"$(printf "$vals" | sort -n | awk -v n="$reps" '
                NR == 1 {lo = $1} NR == int((n+1)/2) {med = $1} {hi = $1} END {print med, lo, hi}')"
        fi
    fi
    sc=-
    if [ "${SKIP_SIM:-0}" != 1 ]; then
        read -r sc sl _ <<<"$(run_once sim "$s" 1)"
        [ "$launches" = - ] && launches="${sl:--}"
    fi

    awk -v s="$s" -v l="$launches" -v hc="$hc" -v sc="$sc" -v lo="$hlo" -v hi="$hhi" -v mhz="$mhz" 'BEGIN{
        hm = (hc+0 > 0) ? sprintf("%.3f", hc / mhz / 1000) : "-"
        sm = (sc+0 > 0) ? sprintf("%.3f", sc / mhz / 1000) : "-"
        e  = (hc+0 > 0 && sc+0 > 0) ? sprintf("%+.2f%%", 100 * (sc - hc) / hc) : "-"
        a  = (hc+0 > 0 && sc+0 > 0) ? 100 * (sc - hc) / hc : ""
        if (a < 0) a = -a
        c  = (a == "") ? "-" : (a <= 1) ? "on-target" : (a <= 3) ? "valid" : "INVALID"
        printf "%-11s %8s %13s %13s %9s %9s %8s %-10s  %s..%s\n", s, l, hc, sc, hm, sm, e, c, lo, hi
    }'
done
