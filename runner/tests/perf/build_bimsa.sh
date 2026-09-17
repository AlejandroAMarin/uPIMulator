#!/bin/bash
# Build BIMSA's kernel for uPIMulator from the same command make would use for
# the hardware, so both sides of the study run the same source with the same
# macros. Anything else makes the comparison meaningless: the compiler lays out
# a loop differently at the slightest change and the instruction counts diverge
# for reasons that have nothing to do with the simulator.
#
# Usage: build_bimsa.sh <out-dir> [make variables...]
#   e.g. build_bimsa.sh /tmp/bimsa/instr NR_TASKLETS=1 PERF=1 PERF_TIER=3 \
#                       PERF_INSTRUCTIONS=1 PERF_CYCLES=0
#
# Writes <out-dir>/task.c.o and links <out-dir>/bin as a program directory.

set -eu

here="$(dirname "$(readlink -f "$0")")"
upim="$here/../../../golang/uPIMulator"
sdk="$upim/sdk"
bimsa="${BIMSA_DIR:-/home/alex/PNM-simulator/bimsa}/upmem"

out="$1"; shift

export PATH="/home/alex/go-sdk/bin:$PATH"

mkdir -p "$out"

# Ask make what it would run, rather than restating 40 macros here and letting
# them drift.
cmd=$(cd "$bimsa" && make -n "$@" 2>/dev/null | grep "dpu-upmem-dpurte-clang" | head -1)
if [ -z "$cmd" ]; then
    echo "build_bimsa: make -n produced no DPU compile command." >&2
    echo "  It only emits one when the target is out of date; run 'make clean' first." >&2
    exit 1
fi

tasklets=$(printf '%s\n' $cmd | sed -n 's/^-DNR_TASKLETS=//p')
[ -n "$tasklets" ] || { echo "build_bimsa: no -DNR_TASKLETS in the command" >&2; exit 1; }

# -S because the linker reads assembly, not ELF. -g0 because its lexer cannot
# tokenise the hex literals in DWARF. NR_THREADS because the 2025 compiler
# driver no longer predefines it. The SDK includes come first so the bundled
# headers win over the system ones.
cmd=${cmd/ -g / -S -g0 }
cmd=${cmd/-o bin\/bimsa /-o $out\/task.c.o }
cmd=${cmd/-Isupport /-Isupport -I$sdk\/misc -I$sdk\/stdlib -I$sdk\/syslib -DNR_THREADS=24 }

(cd "$bimsa" && eval "$cmd -w")

# The linker's grammar has no rule for .stack_sizes and clang cannot suppress it.
sed -i '/\.section\t\.stack_sizes/,+2d' "$out/task.c.o"

rm -rf "$out/bin"
mkdir -p "$out/bin"
"$upim/build/uPIMulator" --root_dirpath "$upim" --bin_dirpath "$out/bin" \
    --benchmark BIMSA --benchmark_asm "$out/task.c.o" --link_only true \
    --num_tasklets "$tasklets" --num_channels 1 --num_ranks_per_channel 1 \
    --num_dpus_per_rank 1 >"$out/link.log" 2>&1 ||
    { echo "build_bimsa: link failed; see $out/link.log" >&2; exit 1; }

echo "built $out/bin for $tasklets tasklet(s)"
