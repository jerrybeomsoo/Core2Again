#!/usr/bin/env bash
#
# Boot an OS under the hypervisor in QEMU and watch what happens.
#
#   tests/win/run.sh --kvm --image /path/to/windows.qcow2
#   tests/win/run.sh --tcg --image /path/to/windows.qcow2   # see the warning below
#   tests/win/run.sh --kvm                                  # smoke test, no OS
#
# Options:
#   --kvm | --tcg     accelerator (default kvm)
#   --image PATH      OS disk.  NEVER written: a qcow2 overlay is created over it
#   --smp N           processors (default 2 - exercises AP startup)
#   --seconds N       how long to let it run (default 240)
#   --shots N         how many screenshots to take across that time (default 12)
#
# Core2AgainLoader.efi is the removable-media boot option.  It loads Core2Again.efi
# as a resident driver, connects all controllers, then chainloads the Windows
# boot manager - so the OS boots with the hypervisor already hooked into
# ExitBootServices, the same hook point used by the release USB boot chain.
#
# ABOUT --tcg: QEMU's TCG does not implement VMX at all ("TCG doesn't support
# requested features").  Under TCG the hypervisor cannot virtualize anything; it
# should detect that, decline, and leave the machine alone.  A --tcg run
# therefore tests FAIL-SAFE behaviour - does the OS still boot untouched - and
# nothing else.  Only --kvm exercises the hypervisor.
#
# Observation is by screenshot, because after ExitBootServices there is no
# console: the hypervisor's markers and Windows' own output are both only ever
# pixels.  Shots land in Build/wintest/.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

ACCEL="kvm"; IMAGE=""; SMP=2; SECONDS_RUN=240; SHOTS=12; USEHV=1; CLOVER=""; TAG=""; CPUOVR=""; LEGACY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --kvm) ACCEL="kvm"; shift ;;
    --tcg) ACCEL="tcg"; shift ;;
    --no-hv) USEHV=0; shift ;;                 # control: boot the OS with no hypervisor
    --clover) CLOVER="$2"; shift 2 ;;          # historical Clover ISO comparison
    --tag) TAG="-$2"; shift 2 ;;
    --cpu) CPUOVR="$2"; shift 2 ;;      # override the guest CPU model
    --legacy) LEGACY=1; shift ;;         # SeaBIOS + Clover, historical test path
    --image) IMAGE="$2"; shift 2 ;;
    --smp) SMP="$2"; shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --shots) SHOTS="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"
for F in "$OVMF_CODE" "$OVMF_VARS"; do
  [ -f "$F" ] || { echo "error: no OVMF firmware at '$F'" >&2; exit 1; }
done

HV="$REPO/dist/Core2Again-RELEASE.efi"
LOADER="$REPO/dist/Core2AgainLoader.efi"
for F in "$HV" "$LOADER"; do
  [ -f "$F" ] || { echo "error: missing $F - run scripts/dist.sh first" >&2; exit 1; }
done

OUT="$REPO/Build/wintest$TAG"; rm -rf "$OUT"; mkdir -p "$OUT"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/esp/EFI/BOOT"
# Core2AgainLoader.efi is always the boot file, even for a control run.  Windows' own ESP
# holds \EFI\Microsoft\Boot\bootmgfw.efi and NOT \EFI\BOOT\BOOTX64.EFI, and we
# start every run from a pristine OVMF_VARS with no "Windows Boot Manager" NVRAM
# entry - so firmware BDS finds nothing bootable on its own.  Core2AgainLoader scans for
# bootmgfw explicitly.  Omitting only Core2Again.efi therefore isolates the
# hypervisor as the single variable; Core2AgainLoader reports the missing driver and
# chainloads Windows regardless.
cp "$LOADER" "$WORK/esp/EFI/BOOT/BOOTX64.EFI"
if [ "$USEHV" = 1 ]; then
  cp "$HV" "$WORK/esp/Core2Again.efi"
else
  echo "CONTROL: chainloader present, hypervisor driver omitted"
fi
cp "$OVMF_VARS" "$WORK/vars.fd"

DISK_ARGS=()
if [ -n "$IMAGE" ]; then
  [ -r "$IMAGE" ] || { echo "error: cannot read image '$IMAGE'" >&2; exit 1; }
  # Overlay, so the user's image is never written to.  -snapshot would do the
  # same but an explicit overlay makes it obvious and lets the run be inspected
  # afterwards.
  # Parse the JSON, do not pattern-match it.  A sed for "format": "..." picks up
  # the protocol layer and yields "file", and creating the overlay with
  # -F file makes QEMU read the qcow2 as a raw file - so the guest sees the
  # qcow2 header where the MBR should be and the disk appears blank.  That cost
  # several runs that looked like firmware failing to enumerate the disk.
  FMT="$(qemu-img info --output=json "$IMAGE" |
         python3 -c 'import json,sys; print(json.load(sys.stdin)["format"])')"
  qemu-img create -q -f qcow2 -F "${FMT:-qcow2}" -b "$IMAGE" "$WORK/overlay.qcow2"
  DISK_ARGS=(-drive "file=$WORK/overlay.qcow2,format=qcow2,media=disk,if=ide,index=2")
  echo "OS image : $IMAGE (read-only; writes go to a throwaway overlay)"
else
  echo "OS image : none - smoke test only (driver load + marker checkpoints)"
fi

if [ "$ACCEL" = "kvm" ]; then
  # Default to a host CPU minus the features Penryn does not have.  Windows
  # enables PCID, SMEP, SMAP, UMIP and FSGSBASE when offered them, and this
  # hypervisor was written for a CPU that has none of them - so a plain
  # -cpu host tests a machine the target will never be.  Pass --cpu host,+vmx
  # to get the unrestricted one back.
  CPU="${CPUOVR:-host,+vmx,-pcid,-invpcid,-smep,-smap,-umip,-fsgsbase}"
else
  CPU="${CPUOVR:-qemu64}"      # TCG has no VMX to offer
  echo "NOTE: TCG cannot provide VMX. This run tests fail-safe behaviour only."
fi

echo "accel    : $ACCEL   cpu: $CPU   smp: $SMP   for ${SECONDS_RUN}s"
echo "output   : $OUT"
echo "disks    : ${DISK_ARGS[*]:-none}"
echo

# Legacy mode is retained for historical Clover ISO comparisons. The release
# path uses tests/legacy/run.sh with the OpenDuet USB image instead. In this
# mode, a legacy BIOS loads CloverEFI, which is then the firmware, and Clover loads
# EFI/CLOVER/drivers64/Core2Again.efi itself.  No OVMF, no Core2AgainLoader, and no
# UEFI firmware owning the APs - the configuration bug 13 was about.
if [ "$LEGACY" = 1 ]; then
  [ -n "$CLOVER" ] || { echo "error: --legacy needs --clover <iso>" >&2; exit 1; }
  FW_ARGS=(-boot d
           -drive "file=$CLOVER,media=cdrom,if=ide,index=0,readonly=on")
else
  FW_ARGS=(-drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE"
           -drive "if=pflash,format=raw,unit=1,file=$WORK/vars.fd"
           -drive "file=fat:rw:$WORK/esp,format=raw,media=disk,if=ide,index=1")
fi

# Clover 2.4k is from 2019 and expects a PC of that era; q35 with a modern ICH9
# is not one.  Legacy runs therefore use i440fx, which is also closer to the
# chipset the target machine actually has.
MACHINE="q35"
[ "$LEGACY" = 1 ] && MACHINE="pc"

qemu-system-x86_64 \
  -machine "$MACHINE",accel="$ACCEL" -cpu "$CPU" -m 4096 -smp "$SMP" \
  "${FW_ARGS[@]}" \
  "${DISK_ARGS[@]}" \
  -serial "file:$OUT/serial.log" \
  -monitor "unix:$WORK/mon,server,nowait" \
  -display none -no-reboot \
  -D "$OUT/qemu.log" -d guest_errors 2>"$OUT/qemu.err" &
QPID=$!

mon () { timeout 15 python3 "$HERE/mon.py" "$WORK/mon" "$1" 2>/dev/null || true; }

# Wait for the monitor socket, then shoot the screen at a regular cadence.  The
# marker display is the only channel the hypervisor has after ExitBootServices,
# so a shot every few seconds is the transcript of the boot.
for _ in $(seq 1 30); do [ -S "$WORK/mon" ] && break; sleep 1; done

INTERVAL=$(( SECONDS_RUN / SHOTS )); [ "$INTERVAL" -lt 1 ] && INTERVAL=1
for I in $(seq 1 "$SHOTS"); do
  sleep "$INTERVAL"
  kill -0 "$QPID" 2>/dev/null || { echo "qemu exited early after $((I*INTERVAL))s"; break; }
  N=$(printf '%02d' "$I")
  mon "screendump $OUT/shot-$N.ppm" >/dev/null
  [ -f "$OUT/shot-$N.ppm" ] && command -v convert >/dev/null 2>&1 &&
    convert "$OUT/shot-$N.ppm" "$OUT/shot-$N.png" 2>/dev/null && rm -f "$OUT/shot-$N.ppm"
  echo "  t=$((I*INTERVAL))s  shot $N"
done

mon "quit" >/dev/null
sleep 2; kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true

echo
echo "=== serial ==="
[ -s "$OUT/serial.log" ] && tr -d '\000' < "$OUT/serial.log" | grep -aE 'CORE2AGAIN|HV\]|error|Error' | head -30 \
  || echo "(no serial output)"
echo
echo "=== qemu stderr ==="
[ -s "$OUT/qemu.err" ] && head -12 "$OUT/qemu.err" || echo "(clean)"
echo "=== guest errors (QEMU) ==="
[ -s "$OUT/qemu.log" ] && tail -15 "$OUT/qemu.log" || echo "(none)"
echo
echo "screens: $(ls "$OUT"/shot-* 2>/dev/null | wc -l) in $OUT"
