#!/bin/bash
# Compare uPIMulator's ALU, condition codes and DMA against UPMEM's own
# functional simulator.
#
# Both sides run the same kernel sources. The reference uses backend=simulator,
# which depends only on libc and cannot reach a DPU rank, so no hardware is
# touched by either side.
#
# Usage: runner/tests/alu/run.sh [work-dir]

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/../.."
upim="$runner/../golang/uPIMulator"
sdk="$upim/sdk"
work="${1:-/tmp/upim-alu}"

export PATH="/home/alex/go-sdk/bin:$PATH"

rm -rf "$work"
mkdir -p "$work"

failures=0

# build_kernel <name> <source>: produces $work/<name>_dpu for the reference and
# $work/<name>/ as a uPIMulator program directory.
build_kernel() {
    local name="$1" src="$2"

    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS=1 -o "$work/${name}_dpu" "$src"

    # -g0 because the linker's lexer cannot tokenise DWARF hex literals, and
    # NR_THREADS because the 2025 compiler driver no longer predefines it.
    dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS=1 -DNR_THREADS=24 \
        -I"$sdk/misc" -I"$sdk/stdlib" -I"$sdk/syslib" \
        -o "$work/$name.c.o" "$src"

    # The linker's grammar has no rule for .stack_sizes, and clang offers no
    # flag to suppress it.
    sed -i '/\.section\t\.stack_sizes/,+2d' "$work/$name.c.o"

    mkdir -p "$work/$name"
    "$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$work/$name" \
        --benchmark "$(echo "$name" | tr '[:lower:]' '[:upper:]')" \
        --benchmark_asm "$work/$name.c.o" --link_only true \
        --num_tasklets 1 --num_channels 1 --num_ranks_per_channel 1 \
        --num_dpus_per_rank 1 >"$work/$name.link.log" 2>&1 ||
        { echo "$name: link failed; see $work/$name.link.log" >&2; exit 1; }
}

# By default the simulator runs the DPU ELF the host loads, the same file
# the machine runs. UPIM_ELF=0 runs the image of a uPIMulator linker run instead.
elf="${UPIM_ELF:-1}"

# compare <name> <host-binary> [env...]: runs both backends and diffs.
compare() {
    local name="$1" host="$2"; shift 2

    env "$@" ALU_PROFILE="backend=simulator" UPMEM_PROFILE="backend=simulator" \
        "$host" >>"$work/$name.ref.txt"

    env "$@" "$runner/upimulator" run $([ "$elf" = 1 ] || echo --program-dir "$work/$name") --tasklets 1 \
        -- "$host" 2>&1 | grep -v check_symbols >>"$work/$name.upim.txt"
}

report() {
    local name="$1"
    if diff -q "$work/$name.ref.txt" "$work/$name.upim.txt" >/dev/null; then
        echo "PASS $name: all $(wc -l <"$work/$name.ref.txt") results match the reference"
    else
        echo "FAIL $name: $(diff "$work/$name.ref.txt" "$work/$name.upim.txt" | grep -c '^<') results differ" >&2
        diff "$work/$name.ref.txt" "$work/$name.upim.txt" >&2
        failures=$((failures + 1))
    fi
}

echo "building"
build_kernel alu "$here/kernel.c"
build_kernel cond "$here/kernel_cond.c"
build_kernel mram "$here/mram.c"

gcc -O2 -Wall -o "$work/alu_host" "$here/host.c" $(dpu-pkg-config --cflags --libs dpu)
gcc -O2 -Wall -o "$work/cond_host" "$here/host_cond.c" $(dpu-pkg-config --cflags --libs dpu)
gcc -O2 -Wall -o "$work/mram_host" "$here/host_mram.c" $(dpu-pkg-config --cflags --libs dpu)

echo "running"
compare alu "$work/alu_host" ALU_DPU_BIN="$work/alu_dpu"
compare cond "$work/cond_host" COND_DPU_BIN="$work/cond_dpu"

# The DMA test is a sweep: every combination of an aligned and an unaligned
# address on each side.
for off in 0 4 8 12; do
    for woff in 0 4; do
        compare mram "$work/mram_host" MB="$work/mram_dpu" OFF="$off" WOFF="$woff"
    done
done

report alu
report cond
report mram

exit $((failures > 0))
