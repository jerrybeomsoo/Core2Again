#!/usr/bin/env bash
#
# Drive the shipping hypervisor through ExitBootServices under QEMU - no OS.
#
#   tests/hv/run.sh --kvm --smp 2      # the real test: nested VMX
#   tests/hv/run.sh --tcg              # no VMX at all; must decline and survive
#
# HvLifecycle.efi loads the driver, calls ExitBootServices (where the hook wakes
# the APs and virtualizes the BSP), then spins on CPUID so every iteration is a
# VM exit and the driver's heartbeat keeps repainting.  The verdict is read off
# a screenshot, which is the same instrumentation the real hardware uses.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

ACCEL="kvm"; SMP=2; SECONDS_RUN=45; SHOTS=6; APSTART="TRUE"; APDBG="FALSE"
while [ $# -gt 0 ]; do
  case "$1" in
    --kvm) ACCEL="kvm"; shift ;;
    --tcg) ACCEL="tcg"; shift ;;
    --no-ap) APSTART="FALSE"; shift ;;   # single-core build, for A/B
    --ap-debug) APDBG="TRUE"; shift ;;   # stage markers through HvStartAps
    --smp) SMP="$2"; shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"

echo "=== building (HV_BUILD_TESTS) ==="
HV_EXTRA_BUILD_ARGS="-D HV_BUILD_TESTS=TRUE -D HV_AP_STARTUP=$APSTART -D HV_AP_DEBUG=$APDBG" "$REPO/scripts/build.sh" RELEASE >/dev/null
BIN="$REPO/Build/Core2AgainPkg/RELEASE_GCC/X64"
for F in "$BIN/HvLifecycle.efi" "$BIN/Core2Again.efi"; do
  [ -f "$F" ] || { echo "error: missing $F" >&2; exit 1; }
done

# Build the suffix with plain ifs.  A $( [ ... ] && echo x ) inside an
# assignment returns non-zero when the test is false, and under `set -e` that
# kills the script at the assignment - silently, leaving the PREVIOUS run's
# screenshots in place to be misread as this run's result.
SUFFIX=""
[ "$APSTART" = FALSE ] && SUFFIX="$SUFFIX-noap"
[ "$APDBG" = TRUE ]    && SUFFIX="$SUFFIX-apdbg"
OUT="$REPO/Build/hvtest-$ACCEL-smp$SMP$SUFFIX"; rm -rf "$OUT"; mkdir -p "$OUT"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/esp/EFI/BOOT"
cp "$BIN/HvLifecycle.efi"     "$WORK/esp/EFI/BOOT/BOOTX64.EFI"
cp "$BIN/Core2Again.efi" "$WORK/esp/Core2Again.efi"
cp "$OVMF_VARS" "$WORK/vars.fd"

if [ "$ACCEL" = "kvm" ]; then CPU="host,+vmx"; else CPU="qemu64"; fi
echo "=== accel=$ACCEL cpu=$CPU smp=$SMP ap_startup=$APSTART for ${SECONDS_RUN}s ==="

qemu-system-x86_64 \
  -machine q35,accel="$ACCEL" -cpu "$CPU" -m 2048 -smp "$SMP" \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,unit=1,file="$WORK/vars.fd" \
  -drive file=fat:rw:"$WORK/esp",format=raw,media=disk \
  -serial "file:$OUT/serial.log" \
  -monitor "unix:$WORK/mon,server,nowait" \
  -display none -no-reboot -D "$OUT/qemu.log" -d guest_errors &
QPID=$!
mon () { timeout 15 python3 "$REPO/tests/win/mon.py" "$WORK/mon" "$1" 2>/dev/null || true; }
for _ in $(seq 1 30); do [ -S "$WORK/mon" ] && break; sleep 1; done

INTERVAL=$(( SECONDS_RUN / SHOTS )); [ "$INTERVAL" -lt 1 ] && INTERVAL=1
for I in $(seq 1 "$SHOTS"); do
  sleep "$INTERVAL"
  kill -0 "$QPID" 2>/dev/null || { echo "!! qemu exited after $((I*INTERVAL))s"; break; }
  N=$(printf '%02d' "$I")
  mon "screendump $OUT/shot-$N.ppm" >/dev/null
  [ -f "$OUT/shot-$N.ppm" ] && magick "$OUT/shot-$N.ppm" "$OUT/shot-$N.png" 2>/dev/null && rm -f "$OUT/shot-$N.ppm"
done
mon "quit" >/dev/null; sleep 2; kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true

echo "=== serial ==="
tr -d '\000' < "$OUT/serial.log" 2>/dev/null | grep -aE 'LIFE|CORE2AGAIN' | head -20 || echo "(none)"
echo "=== guest errors ==="; [ -s "$OUT/qemu.log" ] && tail -8 "$OUT/qemu.log" || echo "(none)"
echo "=== screens in $OUT ==="; ls "$OUT"/shot-*.png 2>/dev/null | wc -l
