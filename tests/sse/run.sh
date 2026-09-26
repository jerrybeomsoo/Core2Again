#!/usr/bin/env bash
#
# Run SseLive.efi as a guest under the shipping hypervisor.
#
#   tests/sse/run.sh --host ubuntu@1.2.3.4 --key ~/id_target
#   tests/sse/run.sh                          # locally (see the warning)
#
# WHAT IT ESTABLISHES DEPENDS ENTIRELY ON THE CPU IT RUNS ON.
#
# On a Core 2 / Penryn the guest's CPUID and its ISA disagree - the driver
# advertises SSE4.2 and POPCNT that the silicon does not have - so every one of
# those instructions raises a real #UD and is decoded, emulated and resumed by
# the hypervisor.  That is the only configuration in which the decoder runs at
# all, and this is the only test of it end to end.
#
# On any development host the physical CPU HAS SSE4.2, nothing faults, and the
# run proves only that the app and the driver load.  Heartbeat row 1 (SSE
# instructions emulated) tells you which of the two you got: non-zero means the
# decoder really ran.
#
# The target needs KVM with nested VMX (kvm_intel.nested=1) so the driver can
# be a hypervisor inside a guest.  Penryn has no EPT and no unrestricted guest,
# which this hypervisor is built for anyway.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

HOST=""; KEY=""; SMP=2; SECONDS_RUN=90; SHOTS=6
while [ $# -gt 0 ]; do
  case "$1" in
    --host)    HOST="$2";        shift 2 ;;
    --key)     KEY="$2";         shift 2 ;;
    --smp)     SMP="$2";         shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    *) echo "usage: $0 [--host user@ip --key path] [--smp N] [--seconds N]" >&2; exit 1 ;;
  esac
done

echo "=== building (HV_BUILD_TESTS) ==="
HV_EXTRA_BUILD_ARGS="-D HV_BUILD_TESTS=TRUE" "$REPO/scripts/build.sh" RELEASE >/dev/null
BIN="$REPO/Build/Core2AgainPkg/RELEASE_GCC/X64"
for F in "$BIN/SseLive.efi" "$BIN/Core2Again.efi"; do
  [ -f "$F" ] || { echo "error: missing $F" >&2; exit 1; }
done

run_qemu () {                       # $1 = esp dir, $2 = output dir
  local ESP="$1" OUT="$2"
  qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 2048 -smp "$SMP" \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$OUT/vars.fd" \
    -drive file=fat:rw:"$ESP",format=raw,media=disk,if=ide,index=1 \
    -serial "file:$OUT/serial.log" -monitor "unix:$OUT/mon,server,nowait" \
    -display none -no-reboot -D "$OUT/qemu.log" -d guest_errors 2>"$OUT/qemu.err"
}

if [ -n "$HOST" ]; then
  SSH=(ssh -o StrictHostKeyChecking=no -o BatchMode=yes)
  SCP=(scp -o StrictHostKeyChecking=no -q)
  [ -n "$KEY" ] && { SSH+=(-i "$KEY"); SCP+=(-i "$KEY"); }

  "${SSH[@]}" "$HOST" 'mkdir -p ~/hvtest/bin ~/hvtest/out'
  "${SCP[@]}" "$BIN/SseLive.efi" "$BIN/Core2Again.efi" "$REPO/tests/win/mon.py" "$HOST:~/hvtest/bin/"
  "${SCP[@]}" "$HERE/remote.sh" "$HOST:~/hvtest/"
  "${SSH[@]}" "$HOST" "bash ~/hvtest/remote.sh SseLive.efi sse $SMP $SECONDS_RUN $SHOTS"
  exit $?
fi

echo "WARNING: this host almost certainly has SSE4.2, so nothing will #UD and"
echo "         the decoder will not run.  Use --host to reach a Penryn." >&2
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"
OUT="$REPO/Build/ssetest"; rm -rf "$OUT"; mkdir -p "$OUT/esp/EFI/BOOT"
cp "$BIN/SseLive.efi"         "$OUT/esp/EFI/BOOT/BOOTX64.EFI"
cp "$BIN/Core2Again.efi" "$OUT/esp/Core2Again.efi"
cp "$OVMF_VARS" "$OUT/vars.fd"
timeout "$SECONDS_RUN" bash -c "$(declare -f run_qemu); OVMF_CODE=$OVMF_CODE run_qemu '$OUT/esp' '$OUT'" || true
echo "=== serial ==="
tr -d '\000' < "$OUT/serial.log" 2>/dev/null | grep -aE 'SSE' || echo "(none)"
