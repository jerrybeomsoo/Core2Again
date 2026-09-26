#!/usr/bin/env bash
#
# Boot the legacy-BIOS USB image (scripts/mkimage.sh) under SeaBIOS.
#
# This is the only test configuration that matches the target machine: no OVMF,
# no UEFI firmware at all.  SeaBIOS loads our MBR, DUET supplies UEFI from
# inside the image, and Windows boots as a UEFI OS on a BIOS-only machine.
#
#   tests/legacy/run.sh                             # smoke test, no OS
#   tests/legacy/run.sh --image ~/win11.qcow2       # full boot to desktop
#   tests/legacy/run.sh --image ~/win11.qcow2 --smp 4 --seconds 300
#
# Every run ends by asking the monitor where each vCPU is executing, which is
# how the multi-core claim is checked - see cpucheck.py.
#
# The OS image is never written to: QEMU gets a throwaway qcow2 overlay.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

IMG="$REPO/dist/core2again-legacy-blockio.img"
OSIMAGE=""
SMP=2
SECONDS_RUN=240
SHOTS=12
ACCEL=kvm
CPUOVR=""
OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --img)     IMG="$2";         shift 2 ;;   # the USB image under test
    --image)   OSIMAGE="$2";     shift 2 ;;   # Windows disk (read-only)
    --smp)     SMP="$2";         shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --shots)   SHOTS="$2";       shift 2 ;;
    --accel)   ACCEL="$2";       shift 2 ;;
    --cpu)     CPUOVR="$2";      shift 2 ;;
    --out)     OUT="$2";         shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

[ -f "$IMG" ] || { echo "error: no image at '$IMG' (run scripts/mkimage.sh)" >&2; exit 1; }
[ -n "$OUT" ] || OUT="$REPO/Build/legacy-test/$(date +%H%M%S)"
mkdir -p "$OUT"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# Never run QEMU against dist/ directly: it takes a write lock on the file and
# a second run then fails with "Failed to get write lock" on an image that is
# perfectly fine.  Copy first; the image is small.
cp "$IMG" "$WORK/usb.img"

DISKS=(-drive "file=$WORK/usb.img,format=raw,if=ide,index=0")
if [ -n "$OSIMAGE" ]; then
  [ -f "$OSIMAGE" ] || { echo "error: no OS image at '$OSIMAGE'" >&2; exit 1; }
  FMT="$(qemu-img info --output=json "$OSIMAGE" |
         python3 -c 'import json,sys; print(json.load(sys.stdin)["format"])')"
  qemu-img create -q -f qcow2 -F "${FMT:-qcow2}" -b "$OSIMAGE" "$WORK/os.qcow2"
  DISKS+=(-drive "file=$WORK/os.qcow2,format=qcow2,if=ide,index=1")
fi

if [ "$ACCEL" = kvm ]; then
  # The target is a Penryn: no PCID, SMEP, SMAP, UMIP or FSGSBASE.  Windows
  # turns those on when offered, so a plain -cpu host tests a machine that
  # does not exist.
  CPU="${CPUOVR:-host,+vmx,-pcid,-invpcid,-smep,-smap,-umip,-fsgsbase}"
else
  CPU="${CPUOVR:-qemu64}"
  echo "NOTE: TCG has no VMX. This run tests fail-safe behaviour only."
fi

# i440fx, not q35: the target is a 2008 chipset and SeaBIOS boots it the same
# way.  DUET's BlockIo variant reaches disks through INT 13h, so what matters
# is that the BIOS enumerates them, which i440fx IDE does.
echo "image  : $(basename "$IMG")"
echo "os     : ${OSIMAGE:-none (smoke test)}"
echo "accel  : $ACCEL   cpu: $CPU   smp: $SMP   for ${SECONDS_RUN}s"
echo "out    : $OUT"
echo

qemu-system-x86_64 \
  -machine pc,accel="$ACCEL" -cpu "$CPU" -m 4096 -smp "$SMP" \
  "${DISKS[@]}" \
  -monitor "unix:$WORK/mon,server,nowait" \
  -debugcon "file:$OUT/seabios.log" -global isa-debugcon.iobase=0x402 \
  -display none -no-reboot \
  -D "$OUT/qemu.log" -d guest_errors 2>"$OUT/qemu.err" &
QPID=$!

mon () { timeout 15 python3 "$REPO/tests/win/mon.py" "$WORK/mon" "$1" 2>/dev/null || true; }
for _ in $(seq 1 30); do [ -S "$WORK/mon" ] && break; sleep 1; done

INTERVAL=$(( SECONDS_RUN / SHOTS )); [ "$INTERVAL" -lt 1 ] && INTERVAL=1
QEMU_EARLY=0
for I in $(seq 1 "$SHOTS"); do
  sleep "$INTERVAL"
  kill -0 "$QPID" 2>/dev/null || { echo "qemu exited early after $((I*INTERVAL))s"; QEMU_EARLY=1; break; }
  N=$(printf '%02d' "$I")
  mon "screendump $OUT/shot-$N.ppm" >/dev/null
  if [ -f "$OUT/shot-$N.ppm" ]; then
    magick "$OUT/shot-$N.ppm" "$OUT/shot-$N.png" 2>/dev/null && rm -f "$OUT/shot-$N.ppm"
  fi
  echo "  t=$((I*INTERVAL))s  shot $N"
done

# Windows reports its processor count in Task Manager and nowhere else that a
# screenshot can reach.  Ctrl+Shift+Esc opens it; Ctrl+Shift+Tab-free keyboard
# navigation is unreliable across builds, so shoot both the default tab and the
# state a few seconds later, and read the count off whichever came out legible.
# Where is each processor executing?  This is the multi-core check, and it needs
# nothing from inside the guest - no keyboard, no legible screenshot.  See
# cpucheck.py for why RIP answers the question.
#
# Only with an OS attached.  A smoke test has nothing to boot, so every vCPU is
# sitting in DUET's BDS at a firmware address and the check would report a
# failure that means nothing.
CPUCHECK_STATUS=0
if [ -n "$OSIMAGE" ] && [ "$QEMU_EARLY" = 0 ] && kill -0 "$QPID" 2>/dev/null; then
  echo
  echo "vCPU check:"
  if python3 "$HERE/cpucheck.py" "$WORK/mon" 8 1 "${SMP%%,*}" | tee "$OUT/cpucheck.txt"; then
    :
  else
    CPUCHECK_STATUS=$?
  fi
  echo
elif [ -n "$OSIMAGE" ]; then
  echo "cpucheck: FAIL - qemu exited before the vCPU check" | tee "$OUT/cpucheck.txt"
  CPUCHECK_STATUS=1
fi

mon "quit" >/dev/null
sleep 2; kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true

echo
echo "screens in $OUT"
grep -aE "Booting from|Boot failed" "$OUT/seabios.log" 2>/dev/null | tail -3 || true
if [ "$QEMU_EARLY" != 0 ] || [ "$CPUCHECK_STATUS" != 0 ]; then
  exit 1
fi
