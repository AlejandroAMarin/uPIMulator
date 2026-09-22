#!/bin/bash
# Compare the cost of a register-bank conflict, hardware against the simulator.
#
#   runner/tests/perf/regbank_sweep.sh [work-dir]
#
# The register file has an even bank and an odd bank, and each serves at most
# two of one instruction's registers in a cycle, counting the destination with
# the two sources. Three registers of one parity therefore need a second issue
# slot. Read the one-tasklet column first: there the revolver gives a thread one
# slot every 11 cycles, so a lost slot is 11 cycles and cannot be mistaken for
# anything else. At 16 tasklets the same lost slot is worth one cycle, because
# another tasklet would have used it.
#
# This was worth a fifth of BIMSA's time at 16 tasklets and nothing had ever
# checked it. See "Register-file bank conflicts" in README.md.
#
# The hardware runs ask for backend=hw deliberately.

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-regbank}"
loops="${LOOPS:-2000}"
tasklets="${TASKLETS_LIST:-1 16}"

export PATH="/home/alex/go-sdk/bin:$PATH"

rm -rf "$work"; mkdir -p "$work"
gcc -O2 -Wall -o "$work/host" "$here/regbank_host.c" $(dpu-pkg-config --cflags --libs dpu)

for t in $tasklets; do
    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS="$t" -o "$work/dpu_$t" "$here/regbank_kernel.c"
    dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS="$t" -DNR_THREADS=24 \
        -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" -o "$work/k_$t.c.o" "$here/regbank_kernel.c"
    sed -i '/\.section\t\.stack_sizes/,+2d' "$work/k_$t.c.o"

    mkdir -p "$work/bin_$t"
    "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/bin_$t" \
        --benchmark REGBANK --benchmark_asm "$work/k_$t.c.o" --link_only true \
        --num_tasklets "$t" --num_channels 1 --num_ranks_per_channel 1 \
        --num_dpus_per_rank 1 >"$work/link_$t.log" 2>&1 ||
        { echo "link failed for $t; see $work/link_$t.log" >&2; exit 1; }

    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw LOOPS="$loops" TASKLETS="$t" \
        XBIN="$work/dpu_$t" "$work/host" | tail -n +2 >"$work/hw_$t.tsv"

    LOOPS="$loops" TASKLETS="$t" XBIN="$work/dpu_$t" \
        "$runner/upimulator" run --program-dir "$work/bin_$t" --tasklets "$t" --dpus 1 \
        -- "$work/host" 2>/dev/null | grep -E '^[0-9]+	' >"$work/sim_$t.tsv"

    echo
    echo "$t tasklet(s): cycles per instruction"
    paste <(cut -f2,4 "$work/hw_$t.tsv") <(cut -f2 "$work/sim_$t.tsv") |
        awk -F'\t' 'BEGIN{printf "%10s %11s %8s  %s\n","hardware","uPIMulator","error","case"}
                    {e=($1>0)?100*($3-$1)/$1:0
                     printf "%10.3f %11.3f %+7.2f%%  %s\n",$1,$3,e,$2}'
done
