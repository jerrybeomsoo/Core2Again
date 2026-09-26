#!/usr/bin/env bash
#
# Differential test: run each software emulator against the real instruction,
# executed by this host, and compare results and flags.
#
#   tests/run.sh
#
# Requires a host CPU that actually has SSE4.1/SSE4.2 (Nehalem or later).  The
# emulator sources are pure functions over 16-byte buffers, so they build in
# userspace against a small stub of the EDK II environment - no firmware, no
# hardware, no VM.  This is what found the Equal-Ordered, SNaN and negative-zero
# bugs; run it past any change to Sse41.c / SseString.c / SseDecoder.c.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG="$HERE/../Core2AgainPkg"
B="$HERE/build"

grep -q sse4_2 /proc/cpuinfo || { echo "host CPU has no SSE4.2: nothing to compare against" >&2; exit 1; }

rm -rf "$B"; mkdir -p "$B"
cp "$PKG"/SseString.c "$PKG"/Sse41.c "$PKG"/SseDecoder.c "$B/"
cp "$HERE/stub/Hypervisor.h" "$HERE/test_scalar.c" "$B/"
python3 "$HERE/gen.py" "$B"

cd "$B"
gcc -c -O1 -Wall -Werror SseString.c -o sse_string.o
gcc -c -O1 -Wall -Werror Sse41.c     -o sse41.o
gcc -O1 -o test_str    test_str.c    sse_string.o          -msse4.2
gcc -O1 -o test41      test41.c      sse41.o               -msse4.2
gcc -O1 -o test_scalar test_scalar.c sse41.o sse_string.o  -msse4.2

rc=0
./test_str    || rc=1
./test41      || rc=1
./test_scalar || rc=1
[ $rc -eq 0 ] && echo "all emulators match hardware"
exit $rc
