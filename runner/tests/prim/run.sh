#!/bin/bash
# Compare PrIM kernels on this machine's hardware against uPIMulator.
#
#   runner/tests/prim/run.sh [work-dir]
#   KERNELS="va hst" TASKLETS_LIST="1 4 16" NELEM=16384 runner/tests/prim/run.sh
#
# BIMSA is the study's first workload and it is a wavefront: a small working
# set, a lot of arithmetic on it, and short scattered transfers. These are
# deliberately unlike it and unlike each other:
#
#   va   long sequential transfers, almost no arithmetic between them
#   hst  a private histogram in WRAM indexed by the data, then a reduction
#        across tasklets through a shared array and two barriers
#
# One workload agreeing to 2% is not a validated simulator. Both sides run the
# same kernel source with the same macros, and every point is gated on the
# result being right.
#
# The hardware runs ask for backend=hw deliberately.

set -eu

# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.
elf="${UPIM_ELF:-1}"

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-prim}"
tasklets="${TASKLETS_LIST:-1 2 4 8 16}"
elems="${NELEM:-16384}"
kernels="${KERNELS:-va hst}"
reps="${REPS:-5}"

export PATH="/home/alex/go-sdk/bin:$PATH"

rm -rf "$work"; mkdir -p "$work"

# The block size and element type come from each benchmark's own CMakeLists, so
# both sides build what PrIM itself builds.
va_bench="$upim/benchmark/VA";    va_flags="-DBL=10 -DINT32"
hst_bench="$upim/benchmark/HST-S"; hst_flags="-DBL=10"

# count <mode> <command...>: run the host and print the counter, or FAIL.
count() {
    local mode="$1"; shift
    local out
    out=$(NELEM="$elems" PROBE_MODE="$mode" "$@" 2>/dev/null) || { echo FAIL; return; }
    if [ "$(printf '%s\n' "$out" | awk -F'\t' '/^mismatches/{print $2}')" != 0 ]; then
        echo FAIL
        return
    fi
    printf '%s\n' "$out" | awk -F'\t' '/^count/{print $2}'
}

# median <reps> <mode> <command...>: the hardware repeats to about 1% on these
# kernels, which is short work by BIMSA's standard, so one reading is not
# enough to call a 1% difference. The simulator is exact from run to run and
# needs no repeats. It prints the median, then the smallest and largest
# reading: va at 2 tasklets is bistable on the machine (701.6k to 729.2k over
# 20 runs), and a median alone hides that.
median() {
    local reps="$1" mode="$2"; shift 2
    local i vals
    vals=""
    for ((i = 0; i < reps; i++)); do
        local v
        v=$(count "$mode" "$@")
        [ "$v" = FAIL ] && { echo FAIL; return; }
        vals="$vals$v\n"
    done
    printf "$vals" | sort -n | awk -v n="$reps" '
        NR == 1 {lo = $1} NR == int((n+1)/2) {med = $1} {hi = $1}
        END {print med, lo, hi}'
}

for k in $kernels; do
    eval "bench=\$${k}_bench; kflags=\$${k}_flags"
    flags="-w -O2 -I$bench/support $kflags"

    gcc -O2 -Wall -o "$work/${k}_host" "$here/${k}_host.c" $(dpu-pkg-config --cflags --libs dpu)

    echo
    echo "$k, $elems elements, 1 DPU"
    printf '%-9s %14s %14s %9s %14s %14s %9s  %s\n' \
        tasklets "cycles hw" "cycles sim" "error" "instr hw" "instr sim" "error" "hw cycles min..max"

    for t in $tasklets; do
        dpu-upmem-dpurte-clang $flags -DNR_TASKLETS="$t" \
            -o "$work/${k}_dpu_$t" "$here/${k}_kernel.c"
        dpu-upmem-dpurte-clang $flags -S -g0 -DNR_TASKLETS="$t" -DNR_THREADS=24 \
            -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" \
            -o "$work/${k}_$t.c.o" "$here/${k}_kernel.c"
        sed -i '/\.section\t\.stack_sizes/,+2d' "$work/${k}_$t.c.o"

        mkdir -p "$work/${k}_bin_$t"
        "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/${k}_bin_$t" \
            --benchmark VA --benchmark_asm "$work/${k}_$t.c.o" --link_only true \
            --num_tasklets "$t" --num_channels 1 --num_ranks_per_channel 1 \
            --num_dpus_per_rank 1 >"$work/${k}_link_$t.log" 2>&1 ||
            { echo "link failed for $k at $t; see $work/${k}_link_$t.log" >&2; exit 1; }

        hw() {
            XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
                XBIN="$work/${k}_dpu_$t" "$work/${k}_host"
        }
        sim() {
            XBIN="$work/${k}_dpu_$t" "$runner/upimulator" run \
                $([ "$elf" = 1 ] || echo --program-dir "$work/${k}_bin_$t") --tasklets "$t" --dpus 1 \
                -- "$work/${k}_host"
        }

            read -r hc hlo hhi <<<"$(median "$reps" 0 hw)"; read -r hi _ _ <<<"$(median "$reps" 1 hw)"
        sc=$(count 0 sim); si=$(count 1 sim)

        awk -v t="$t" -v hc="$hc" -v sc="$sc" -v hi="$hi" -v si="$si" -v lo="$hlo" -v up="$hhi" 'BEGIN{
            ce = (hc+0 > 0 && sc != "FAIL") ? sprintf("%+.2f%%", 100*(sc-hc)/hc) : "-"
            ie = (hi+0 > 0 && si != "FAIL") ? sprintf("%+.2f%%", 100*(si-hi)/hi) : "-"
            printf "%-9s %14s %14s %9s %14s %14s %9s  %s..%s\n", t, hc, sc, ce, hi, si, ie, lo, up
        }'
    done
done
