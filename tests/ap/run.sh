#!/usr/bin/env bash
#
# Build tests/ap/ApTest.efi and run it under QEMU with two processors.
#
#   tests/ap/run.sh              # TCG: slow to boot, but names any exception
#   tests/ap/run.sh --kvm        # KVM: fast, but a triple fault is just a reset
#
# The trampoline is architectural code - real mode, a GDT, CR0/CR4/CR3/EFER, two
# far transfers - so QEMU reproduces its faults exactly.  That matters because on
# the target machine the same fault is a silent power reset that reports nothing;
# here it is a "qemu: fatal:" line naming the exception and the faulting CS:EIP.
#
# Prefer TCG for diagnosis: under KVM the guest's exceptions are handled inside
# the kernel and -d int never sees them.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

ACCEL="tcg"
[ "${1:-}" = "--kvm" ] && ACCEL="kvm"

# More processors exercises the atomic slot claim and the per-AP stack maths.
SMP="${SMP:-2}"

OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"
for F in "$OVMF_CODE" "$OVMF_VARS"; do
  [ -f "$F" ] || { echo "error: no OVMF firmware at '$F' (set OVMF_CODE/OVMF_VARS)" >&2; exit 1; }
done
command -v qemu-system-x86_64 >/dev/null || { echo "error: qemu-system-x86_64 not found" >&2; exit 1; }

echo "=== building ApTest.efi ==="
HV_EXTRA_BUILD_ARGS="-D HV_BUILD_TESTS=TRUE" "$REPO/scripts/build.sh" RELEASE >/dev/null
EFI="$REPO/Build/Core2AgainPkg/RELEASE_GCC/X64/ApTest.efi"
[ -f "$EFI" ] || { echo "error: $EFI was not produced" >&2; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/esp/EFI/BOOT"
# Boot it as the removable-media default.  Relying on the UEFI Shell and a
# startup.nsh would tie the harness to whether the OVMF build ships a shell.
cp "$EFI" "$WORK/esp/EFI/BOOT/BOOTX64.EFI"
cp "$OVMF_VARS" "$WORK/vars.fd"

echo "=== booting QEMU (-smp $SMP, accel=$ACCEL) ==="
set +e
timeout 180 qemu-system-x86_64 \
  -machine q35,accel="$ACCEL" -m 512 -smp "$SMP" \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,unit=1,file="$WORK/vars.fd" \
  -drive file=fat:rw:"$WORK/esp",format=raw,media=disk \
  -serial stdio -display none -no-reboot \
  -d int,cpu_reset -D "$WORK/qemu.log" \
  >"$WORK/serial.txt" 2>"$WORK/qemu.err"
RC=$?
set -e

echo
echo "=== test output ==="
grep -aE 'AP trampoline test|^(page|cr3|stacks|entry|apic|first|sending|progress|slot|arrived|RESULT)' \
     "$WORK/serial.txt" || echo "(no test output - it never ran)"

if grep -qa 'RESULT: PASS' "$WORK/serial.txt"; then
  echo
  echo "PASS"
  exit 0
fi

echo
echo "=== FAILED (qemu exit $RC) ==="
if grep -qa 'qemu: fatal' "$WORK/qemu.err" "$WORK/serial.txt" 2>/dev/null; then
  echo "--- triple fault ---"
  grep -a -A 24 'qemu: fatal' "$WORK/qemu.err" "$WORK/serial.txt" 2>/dev/null | head -40
fi
if [ -s "$WORK/qemu.log" ]; then
  echo "--- last exceptions (check the CS:IP against the trampoline disassembly) ---"
  grep -a '^ *[0-9]*: v=' "$WORK/qemu.log" | tail -12
fi
mkdir -p "$REPO/Build/aptest"
cp "$WORK/qemu.log" "$WORK/serial.txt" "$WORK/qemu.err" "$REPO/Build/aptest/" 2>/dev/null || true
echo "full logs: $REPO/Build/aptest/"
exit 1
