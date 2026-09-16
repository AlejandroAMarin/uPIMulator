#!/bin/bash
# Build and export the environment that makes a real-hardware allocation impossible.
#
# Source this, or use it to print the variables. It is the environmental half of
# the isolation harness; the shim is the other half.
#
# Why each variable, verified against libdpu.so.2025.1:
#
#   UPMEM_PROFILE=backend=nohw
#     Profile precedence is UPMEM_PROFILE > the profile string passed to
#     dpu_alloc() > UPMEM_PROFILE_BASE, and duplicate keys overwrite. So this
#     slot beats a backend= hard-coded in the application. "nohw" is not one of
#     the six valid backend names, so any dpu_alloc that reaches the real libdpu
#     fails with DPU_ERR_INVALID_PROFILE (28) instead of allocating. Our shim
#     never reads it; it exists so a hole in our interception is loud.
#     NOTE: with no backend= key at all, libdpu probes for ranks and picks HW.
#     This machine has 40, so the default is hardware. Never run unprotected.
#
#   UPMEM_RUNTIME_LIBRARY_PATH=<dir containing only libdpufsim.so>
#     Backends are dlopen'd, not linked. find_library searches ONLY this
#     directory when the variable is set, with no fallback to /usr/lib. So
#     libdpuhw.so -- the one library that can reach the ranks, and the sole user
#     of libudev and the dpu_rank subsystem -- becomes unloadable.
#
# Usage:  source isolation_env.sh        # export into the current shell
#         isolation_env.sh --print       # print KEY=VALUE lines

set -u

_upim_isolation_setup() {
    local libdir="${UPIM_SAFE_LIBDIR:-${TMPDIR:-/tmp}/upimulator-safe-lib-$(id -u)}"

    mkdir -p "$libdir"

    # Only the functional simulator is reachable. It depends on libc alone, so
    # it cannot touch hardware even if something does load it.
    local fsim
    fsim=$(ls /usr/lib/libdpufsim.so* 2>/dev/null | head -1)
    if [ -n "$fsim" ] && [ ! -e "$libdir/libdpufsim.so" ]; then
        ln -sf "$fsim" "$libdir/libdpufsim.so"
    fi

    if [ -e "$libdir/libdpuhw.so" ]; then
        echo "isolation_env: refusing to proceed, $libdir contains libdpuhw.so" >&2
        return 1
    fi

    UPIM_SAFE_LIBDIR="$libdir"
    UPMEM_RUNTIME_LIBRARY_PATH="$libdir"
    UPMEM_PROFILE="backend=nohw"
}

if [ "${1:-}" = "--print" ]; then
    _upim_isolation_setup || exit 1
    echo "UPMEM_RUNTIME_LIBRARY_PATH=$UPMEM_RUNTIME_LIBRARY_PATH"
    echo "UPMEM_PROFILE=$UPMEM_PROFILE"
else
    _upim_isolation_setup && export UPMEM_RUNTIME_LIBRARY_PATH UPMEM_PROFILE UPIM_SAFE_LIBDIR
fi
