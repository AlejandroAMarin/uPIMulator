#!/bin/bash
# Run a command under strace and fail if it touches the UPMEM hardware.
#
# This is the observed layer of the isolation harness: whatever we believe about
# symbol interception, this proves that no rank device or rank sysfs node was
# opened during the run. Use it as a gate, not as a diagnostic.
#
# Usage: assert_no_hardware.sh <command> [args...]
# Exit:  the command's exit status, or 1 if hardware was touched.

set -u

if [ $# -lt 1 ]; then
    echo "usage: assert_no_hardware.sh <command> [args...]" >&2
    exit 1
fi

if ! command -v strace >/dev/null; then
    echo "assert_no_hardware: strace is required and not installed" >&2
    exit 1
fi

trace=$(mktemp -t upim_hwcheck.XXXXXX)
trap 'rm -f "$trace"' EXIT

# -f follows the children; openat covers every path the hw backend would use
# (it reaches the ranks through libudev, so watch the sysfs tree too, not just
# /dev). ptrace_scope=1 allows tracing our own descendants without root.
strace -f -e trace=openat -o "$trace" "$@"
status=$?

hits=$(grep -E '/dev/dpu_rank|/sys/class/dpu_rank|/sys/devices/platform/dpu_region' "$trace" \
       | grep -v 'ENOENT' || true)

if [ -n "$hits" ]; then
    echo "" >&2
    echo "assert_no_hardware: HARDWARE WAS TOUCHED -- results are invalid" >&2
    echo "$hits" | head -20 | sed 's/^/    /' >&2
    echo "  full trace: $trace" >&2
    trap - EXIT
    exit 1
fi

echo "assert_no_hardware: clean, no rank device or rank sysfs access"
exit $status
