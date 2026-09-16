#!/bin/bash
# Check that the shim covers every libdpu entry point a host binary imports.
#
# An imported dpu_* symbol that the shim does not define resolves to the real
# /lib/libdpu.so instead, which on this machine defaults to real hardware. So an
# uncovered symbol is not a missing feature, it is a silent hardware run. This
# check turns that into a refusal to start.
#
# Usage: check_symbols.sh <host-binary>
# Exit:  0 all imported symbols covered, 1 otherwise.

set -u

BIN="${1:?usage: check_symbols.sh <host-binary>}"
SYMBOLS="$(dirname "$(readlink -f "$0")")/../SYMBOLS"

if [ ! -f "$BIN" ]; then
    echo "check_symbols: no such binary: $BIN" >&2
    exit 1
fi

covered=$(grep -vE '^\s*#|^\s*$' "$SYMBOLS" | awk '{print $1}' | sort -u)
imported=$(nm -D --undefined-only "$BIN" 2>/dev/null | grep -oE '\bdpu_[a-z_0-9]+' | sort -u)

if [ -z "$imported" ]; then
    echo "check_symbols: $BIN imports no dpu_* symbols; nothing to intercept"
    exit 0
fi

missing=$(comm -23 <(echo "$imported") <(echo "$covered"))

if [ -n "$missing" ]; then
    echo "check_symbols: REFUSING TO RUN $BIN" >&2
    echo "  These libdpu symbols are imported but not implemented by the shim." >&2
    echo "  They would resolve to the real libdpu and allocate REAL HARDWARE:" >&2
    echo "$missing" | sed 's/^/    /' >&2
    echo "  Implement them in the shim and add them to runner/SYMBOLS." >&2
    exit 1
fi

echo "check_symbols: $(echo "$imported" | wc -l) imported dpu_* symbols, all covered"
exit 0
