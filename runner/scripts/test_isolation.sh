#!/bin/bash
# Prove the isolation environment blocks a real hardware allocation.
#
# Builds tests/alloc_probe.c, which asks for one DPU with the default profile --
# the case that selects real hardware on a machine with ranks -- and runs it
# with both environment layers active, under strace.
#
# For hardware to be touched here, BOTH layers must fail at once:
#   1. UPMEM_PROFILE=backend=nohw poisons the highest-precedence profile slot
#   2. UPMEM_RUNTIME_LIBRARY_PATH hides libdpuhw.so, which is dlopen'd, not
#      linked, and has no fallback search path
# strace then independently confirms no rank was opened, and rank ownership is
# sampled before and after.
#
# Exit: 0 isolation holds, 1 otherwise.

set -u

here="$(dirname "$(readlink -f "$0")")"
probe="${TMPDIR:-/tmp}/upim_alloc_probe.$$"
trap 'rm -f "$probe"' EXIT

echo "=== building the probe ==="
gcc -O0 -o "$probe" "$here/../tests/alloc_probe.c" $(dpu-pkg-config --cflags --libs dpu) || {
    echo "test_isolation: build failed" >&2; exit 1; }

owned_before=$(cat /sys/class/dpu_rank/dpu_rank*/is_owned 2>/dev/null | paste -sd, -)
echo "ranks owned before: $(echo "$owned_before" | tr -cd '1' | wc -c) of 40"

echo
echo "=== running the probe under both isolation layers ==="
source "$here/isolation_env.sh"
echo "  UPMEM_PROFILE=$UPMEM_PROFILE"
echo "  UPMEM_RUNTIME_LIBRARY_PATH=$UPMEM_RUNTIME_LIBRARY_PATH"
echo

"$here/assert_no_hardware.sh" "$probe"
hw_status=$?
out=$("$probe" 2>&1)
echo "probe says: $out"

owned_after=$(cat /sys/class/dpu_rank/dpu_rank*/is_owned 2>/dev/null | paste -sd, -)
echo "ranks owned after:  $(echo "$owned_after" | tr -cd '1' | wc -c) of 40"

echo
if echo "$out" | grep -q ALLOC_SUCCEEDED; then
    echo "RESULT: FAILED -- the probe obtained real hardware" >&2
    exit 1
fi
if [ "$owned_before" != "$owned_after" ]; then
    echo "RESULT: FAILED -- rank ownership changed during the run" >&2
    exit 1
fi
if [ $hw_status -ne 0 ]; then
    echo "RESULT: FAILED -- strace saw rank access" >&2
    exit 1
fi

echo "RESULT: isolation holds. Allocation refused, no rank touched."
exit 0
