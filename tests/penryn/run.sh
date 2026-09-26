#!/usr/bin/env bash
#
# Build the SIGILL trap test and run it, optionally on a remote target too.
#
#   tests/penryn/run.sh                                    # build + run here
#   tests/penryn/run.sh --host ubuntu@1.2.3.4 --key ~/id   # ...and on the target
#   tests/penryn/run.sh --threads 2 --rounds 3000000
#
# On a host that HAS SSE4.2 nothing faults and "emulated" reads 0: that run is
# the CONTROL, and what it establishes is that the references in trap_test.c
# agree with real silicon.  On a Core 2 / Penryn every one of those instructions
# raises a genuine #UD, the handler feeds the trap frame to the hypervisor's own
# SseTryEmulate, and "emulated" counts the faults it serviced.  That is the only
# way to exercise the decoder at volume without booting a VM per run.
#
# The binary is linked STATIC so one build runs on both machines: the target has
# no compiler, and matching two distributions' glibc is not worth doing.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PKG="$REPO/Core2AgainPkg"
B="$HERE/build"

HOST=""; KEY=""; THREADS=2; ROUNDS=300000
while [ $# -gt 0 ]; do
  case "$1" in
    --host)    HOST="$2";    shift 2 ;;
    --key)     KEY="$2";     shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --rounds)  ROUNDS="$2";  shift 2 ;;
    *) echo "usage: $0 [--host user@ip --key path] [--threads N] [--rounds N]" >&2; exit 1 ;;
  esac
done

rm -rf "$B"; mkdir -p "$B"
cp "$PKG"/SseDecoder.c "$PKG"/Sse41.c "$PKG"/SseString.c "$B/"
cp "$REPO/tests/stub/Hypervisor.h" "$HERE/trap_test.c" "$B/"

# The decoder needs more of the EDK II environment than the pure emulators do:
# a GUEST_REGS, the VMCS accessors it uses to reach RSP, and the capability
# queries.  All stubbed, none of them reached by these cases.  Kept here rather
# than in the shared stub because only this test compiles the decoder.
cat >> "$B/Hypervisor.h" <<'STUB'

/* --- added by tests/penryn/run.sh --- */
#define CONST           const
#define BIT13           0x2000
#define SIZE_4KB        0x1000
#define FX_XMM0_OFFSET  160
#define FX_AREA_SIZE    512
#define VMCS_GUEST_RSP  0x681C
typedef struct { UINT64 r[16]; } GUEST_REGS;
static inline UINT8 AsmVmRead  (UINT64 f, UINTN *v) { (void)f; *v = 0; return 0; }
static inline UINT8 AsmVmWrite (UINT64 f, UINTN  v) { (void)f; (void)v; return 0; }
BOOLEAN GuestReadLinear  (UINT64, UINT64, VOID *, UINTN);
BOOLEAN GuestWriteLinear (UINT64, UINT64, VOID *, UINTN);
UINT64  HvReadGpr        (GUEST_REGS *, UINT32);
VOID    HvWriteGpr       (GUEST_REGS *, UINT32, UINT64);
BOOLEAN HvEmulateSse41   (VOID);
BOOLEAN HvEmulateSse42   (VOID);
BOOLEAN HvEmulatePopcnt  (VOID);
STUB

cd "$B"
STATIC_LIBS=""
if ! echo 'int main(void){return 0;}' | gcc -static -x c - -o /dev/null 2>/dev/null; then
  # No static libc installed.  Fetch one into a scratch prefix rather than
  # asking for root; a dynamic binary built here will not run on the target.
  echo "no static libc; fetching one into $B (no root needed)" >&2
  mkdir -p rpms/root
  ( cd rpms && dnf download -q glibc-static libxcrypt-static >/dev/null 2>&1 &&
    for r in *.x86_64.rpm; do rpm2cpio "$r" | ( cd root && cpio -idmu --quiet ); done )
  STATIC_LIBS="-L$B/rpms/root/usr/lib64"
fi

gcc -O1 -Wall -Werror -c SseDecoder.c -o dec.o
gcc -O1 -Wall -Werror -c Sse41.c      -o s41.o
gcc -O1 -Wall -Werror -c SseString.c  -o sst.o
# shellcheck disable=SC2086
gcc -O2 -static -pthread -Wall -o trap_test trap_test.c dec.o s41.o sst.o $STATIC_LIBS
echo "built $B/trap_test"

echo
echo "=== local: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs) ==="
./trap_test "$THREADS" "$ROUNDS" | tail -8

if [ -n "$HOST" ]; then
  SSH=(ssh -o StrictHostKeyChecking=no -o BatchMode=yes)
  SCP=(scp -o StrictHostKeyChecking=no -q)
  [ -n "$KEY" ] && { SSH+=(-i "$KEY"); SCP+=(-i "$KEY"); }
  echo
  echo "=== remote: $HOST ==="
  "${SSH[@]}" "$HOST" 'mkdir -p ~/hvtest'
  "${SCP[@]}" trap_test "$HOST:~/hvtest/"
  "${SSH[@]}" "$HOST" "grep -m1 'model name' /proc/cpuinfo; ~/hvtest/trap_test $THREADS $ROUNDS | tail -8"
fi
