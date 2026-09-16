#!/bin/bash
# Build the runner: the Go simulator library, the libdpu shim, and the test host.
#
# Usage: scripts/build.sh

set -eu

here="$(dirname "$(readlink -f "$0")")"
runner="$here/.."
src="$runner/../golang/uPIMulator/src"

export PATH="/home/alex/go-sdk/bin:$PATH"

mkdir -p "$runner/build"

echo "building libupimulator.so (simulator core, Go)"
(cd "$src" && go build -buildmode=c-shared -o "$runner/build/libupimulator.so" ./cshared)

echo "building libdpusim.so (libdpu shim, C)"
# Deliberately no -ldpu: the shim must not be able to reach the real driver.
gcc -shared -fPIC -O2 -Wall -Wextra \
    -o "$runner/build/libdpusim.so" "$runner/shim/dpusim.c" \
    $(dpu-pkg-config --cflags dpu) -I"$runner/build" \
    -L"$runner/build" -lupimulator -Wl,-rpath,'$ORIGIN'

if ldd "$runner/build/libdpusim.so" | grep -q "libdpu\.so"; then
    echo "build.sh: the shim links libdpu, which defeats the isolation" >&2
    exit 1
fi

echo "building the test host"
gcc -O2 -Wall -o "$runner/build/va_host" "$runner/tests/va_host.c" \
    $(dpu-pkg-config --cflags --libs dpu) 2>/dev/null

echo "done. Try:"
echo "  ./upimulator run --program-dir <bin-dir> --tasklets 16 -- ./build/va_host <bin-dir> 0 4096 4"
