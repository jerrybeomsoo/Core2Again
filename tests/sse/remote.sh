#!/usr/bin/env bash
# Target-side half of tests/sse/run.sh.  Lives on the machine under test.
#   remote.sh <app.efi> <tag> <smp> <seconds> <shots>
set -uo pipefail
HERE=~/hvtest
APP="${1:-SseLive.efi}"; TAG="${2:-sse}"; SMP="${3:-2}"; SECS="${4:-90}"; SHOTS="${5:-6}"
OUT="$HERE/out/$TAG"; rm -rf "$OUT"; mkdir -p "$OUT"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W/esp/EFI/BOOT"
cp "$HERE/bin/$APP" "$W/esp/EFI/BOOT/BOOTX64.EFI"
cp "$HERE/bin/Core2Again.efi" "$W/esp/Core2Again.efi"
cp /usr/share/OVMF/OVMF_VARS.fd "$W/vars.fd"
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 2048 -smp "$SMP" \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,unit=1,file="$W/vars.fd" \
  -drive file=fat:rw:"$W/esp",format=raw,media=disk,if=ide,index=1 \
  -serial "file:$OUT/serial.log" -monitor "unix:$W/mon,server,nowait" \
  -display none -no-reboot -D "$OUT/qemu.log" -d guest_errors 2>"$OUT/qemu.err" &
Q=$!
for _ in $(seq 1 30); do [ -S "$W/mon" ] && break; sleep 1; done
IV=$(( SECS / SHOTS )); [ "$IV" -lt 1 ] && IV=1
for I in $(seq 1 "$SHOTS"); do
  sleep "$IV"
  kill -0 "$Q" 2>/dev/null || { echo "qemu exited after $((I*IV))s"; break; }
  timeout 20 python3 "$HERE/bin/mon.py" "$W/mon" \
      "screendump $OUT/shot-$(printf '%02d' "$I").ppm" >/dev/null 2>&1 || true
done
timeout 20 python3 "$HERE/bin/mon.py" "$W/mon" quit >/dev/null 2>&1 || true
sleep 2; kill -9 "$Q" 2>/dev/null; wait "$Q" 2>/dev/null
echo "=== serial ==="
tr -d '\000' < "$OUT/serial.log" 2>/dev/null | grep -aE 'SSE' || echo "(none)"
echo "=== $(ls "$OUT"/shot-* 2>/dev/null | wc -l) screenshots in $OUT ==="
