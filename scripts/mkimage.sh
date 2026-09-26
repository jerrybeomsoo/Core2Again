#!/usr/bin/env bash
#
# mkimage.sh - build a bootable USB image for a machine with no UEFI firmware.
#
# The hypervisor is a UEFI driver and virtualizes at ExitBootServices, so it
# needs a UEFI environment; the target machines have a legacy BIOS.  This image
# supplies one.  The boot chain is:
#
#   BIOS  ->  boot0 (MBR)  ->  boot1f32 (FAT32 PBR)  ->  boot (OpenDuetPkg)
#         ->  EFI/OC/OpenCore.efi (Core2AgainLoader)  ->  \Core2Again.efi
#         ->  \EFI\Microsoft\Boot\bootmgfw.efi on the internal disk
#
# That fourth line is not a typo and not the UEFI removable-media path.
# OpenDuetPkg's BDS loads EFI/OC/OpenCore.efi and nothing else; an image with
# the loader only at \EFI\BOOT\BOOTX64.EFI reaches a working UEFI console and
# stops there with "BOOT MISMATCH!" / "BOOT FAIL!".
#
# OpenDuetPkg is a DUET derivative - the same lineage CloverEFI forked from -
# and provides a real DXE core and BDS from a BIOS boot.  Nothing here is
# Clover: no menu, no config.plist, no ACPI or SMBIOS patching, no kexts.
#
# Everything runs unprivileged: the FAT filesystem is built in a plain file
# with mkfs.vfat and populated with mtools, then spliced into the disk image.
# No loop device, no qemu-nbd, no root.
#
#   scripts/mkimage.sh                       # blockio variant, RELEASE
#   scripts/mkimage.sh --variant native      # native PCI/SATA/USB drivers
#   scripts/mkimage.sh --hv dist/Core2Again-DEBUG.efi --out dist/core2again-debug.img
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OC_VER="1.0.7"
OC_URL="https://github.com/acidanthera/OpenCorePkg/releases/download/${OC_VER}/OpenCore-${OC_VER}-RELEASE.zip"
OC_SHA="2ffab6ebf58c7aefb0bcb3a1a385d207746823d6dd87d44bd666e1286939943e"

# Pinned so a re-download cannot quietly change the boot chain under us.
declare -A DUET_SHA=(
  [boot0]=6b73131518f31fc204e8b228481e75905de30835d348d0aa7407023f05ec168e
  [boot1f32]=7fc1cc76becc70d45c663e423c4773c760c20974bb89e2679ffffa6ac323d5ac
  [bootX64]=b8f6a7c3faca16d61402ba2743d9145ba7cda106bb21bcb6bf184825b4aa86bc
  [bootX64-blockio]=fb9f7f9b80d53c75091e26ebbdd17b7db7ef193fac2b0327d2c58175429f8417
)

VARIANT=blockio
HV="$REPO/dist/Core2Again-RELEASE.efi"
LOADER="$REPO/dist/Core2AgainLoader.efi"
SIZE_MB=256
OUT=""
DUETDIR="$REPO/Build/duet"

while [ $# -gt 0 ]; do
  case "$1" in
    --variant) VARIANT="$2"; shift 2 ;;   # blockio | native
    --hv)      HV="$2";      shift 2 ;;
    --loader)  LOADER="$2";  shift 2 ;;
    --size)    SIZE_MB="$2"; shift 2 ;;
    --out)     OUT="$2";     shift 2 ;;
    --duet)    DUETDIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

case "$VARIANT" in
  blockio) EFILDR="bootX64-blockio" ;;
  native)  EFILDR="bootX64" ;;
  *) echo "error: --variant must be 'blockio' or 'native'" >&2; exit 1 ;;
esac
[ -n "$OUT" ] || OUT="$REPO/dist/core2again-legacy-$VARIANT.img"

for T in mkfs.vfat mcopy mmd sfdisk python3; do
  command -v "$T" >/dev/null || { echo "error: missing tool '$T'" >&2; exit 1; }
done
for F in "$HV" "$LOADER"; do
  [ -f "$F" ] || { echo "error: no such file '$F' (run scripts/dist.sh first)" >&2; exit 1; }
done
for F in "$REPO/LICENSE" "$REPO/licenses/edk2.txt" "$REPO/licenses/opencore.txt"; do
  [ -f "$F" ] || { echo "error: license notice missing: $F" >&2; exit 1; }
done

# ---------------------------------------------------------------- DUET files
mkdir -p "$DUETDIR"
need_fetch=0
for F in boot0 boot1f32 "$EFILDR"; do
  [ -f "$DUETDIR/$F" ] || need_fetch=1
done
if [ "$need_fetch" = 1 ]; then
  echo "fetching OpenDuetPkg binaries from OpenCore $OC_VER ..."
  ZIP="$DUETDIR/OpenCore-$OC_VER.zip"
  [ -f "$ZIP" ] || curl -fsSL -o "$ZIP" "$OC_URL"
  echo "$OC_SHA  $ZIP" | sha256sum -c - >/dev/null || {
    echo "error: OpenCore zip checksum mismatch" >&2; exit 1; }
  python3 - "$ZIP" "$DUETDIR" <<'PY'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for n in z.namelist():
    if n.startswith('Utilities/LegacyBoot/') and not n.endswith('/'):
        open(os.path.join(sys.argv[2], os.path.basename(n)), 'wb').write(z.read(n))
PY
fi
for F in boot0 boot1f32 "$EFILDR"; do
  [ -f "$DUETDIR/$F" ] || { echo "error: $DUETDIR/$F missing" >&2; exit 1; }
  echo "${DUET_SHA[$F]}  $DUETDIR/$F" | sha256sum -c - >/dev/null || {
    echo "error: $F does not match the pinned checksum" >&2; exit 1; }
done

# ------------------------------------------------------------------- layout
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
PART_START=2048                                   # 1 MiB, the usual alignment
TOTAL_SECT=$(( SIZE_MB * 2048 ))
PART_SECT=$(( TOTAL_SECT - PART_START ))

echo "building $(basename "$OUT")  [$VARIANT, ${SIZE_MB}M]"
echo "  hypervisor : $(basename "$HV")"
echo "  loader     : $(basename "$LOADER")"

# 1. The FAT32 volume, as a standalone file.  mkfs.vfat needs no privilege on a
#    regular file, and mtools writes into it without mounting anything.
dd if=/dev/zero of="$WORK/part.img" bs=512 count="$PART_SECT" status=none
mkfs.vfat -F 32 -n CORE2AGAIN "$WORK/part.img" >/dev/null

# 2. Replace the FAT32 boot record with boot1f32, keeping the BPB mkfs.vfat
#    just wrote.  boot1f32 ships with a zeroed BPB (bytes 3..89) because it has
#    to be told the geometry of the volume it is installed on; splicing the real
#    one in is what makes it able to find and load the file named "boot".
python3 - "$WORK/part.img" "$DUETDIR/boot1f32" <<'PY'
import sys, os
part, b1 = sys.argv[1], sys.argv[2]
with open(part, 'r+b') as f:
    orig = f.read(512)
    new  = bytearray(open(b1, 'rb').read())
    new[3:90]    = orig[3:90]        # OEM name + BPB, as installed
    new[496:510] = os.urandom(14)    # scratch area boot1f32 expects randomised
    f.seek(0); f.write(new)
PY

# 3. Contents.  Core2AgainLoader loads the hypervisor from the ROOT of its own volume
#    (Core2AgainLoader.c: FileDevicePath(Li->DeviceHandle, L"\\Core2Again.efi")),
#    so the driver goes at the top level and not beside the loader.
cp "$DUETDIR/$EFILDR" "$WORK/boot"
mcopy -i "$WORK/part.img" "$WORK/boot" ::/boot
mcopy -i "$WORK/part.img" "$HV"     ::/Core2Again.efi
mmd   -i "$WORK/part.img" ::/EFI

# The loader goes at THREE paths, because "the EFI application a firmware boots
# by default" is not one path but three, and which one applies is a property of
# the firmware rather than of the disk:
#
#   EFI/OC/OpenCore.efi      OpenDuetPkg's BDS loads exactly this and nothing
#                            else.  It is not the UEFI removable-media path;
#                            acidanthera hardcoded it.  Without this file the
#                            image reaches a UEFI console and stops there with
#                            "BOOT MISMATCH!" then "BOOT FAIL!".
#   EFI/BOOT/BOOTX64.EFI     the actual UEFI removable-media default.  Used by
#                            a real UEFI machine and by upstream DuetPkg.
#   EFI/CLOVER/CLOVERX64.efi what a CloverEFI "boot" blob launches.
#
# They are the same 36 KB file, so covering all three costs ~72 KB and makes
# the image boot whichever DUET-class blob is dropped in as "boot".
mmd   -i "$WORK/part.img" ::/EFI/OC
mcopy -i "$WORK/part.img" "$LOADER" ::/EFI/OC/OpenCore.efi
mmd   -i "$WORK/part.img" ::/EFI/BOOT
mcopy -i "$WORK/part.img" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
mmd   -i "$WORK/part.img" ::/EFI/CLOVER
mcopy -i "$WORK/part.img" "$LOADER" ::/EFI/CLOVER/CLOVERX64.efi

# Spares, so switching builds on the target is a rename and not a rebuild.
mmd -i "$WORK/part.img" ::/hv
for F in "$REPO"/dist/Core2Again-*.efi; do
  if [ -f "$F" ]; then
    mcopy -i "$WORK/part.img" "$F" "::/hv/$(basename "$F")"
  fi
done

cat > "$WORK/README.TXT" <<'TXT'
Legacy-BIOS boot image for Core2Again.

  BIOS -> boot0 (MBR) -> boot1f32 (PBR) -> boot (OpenDuetPkg = UEFI)
       -> EFI\OC\OpenCore.efi (Core2AgainLoader) -> Core2Again.efi
       -> Windows bootmgfw.efi, found on any attached disk

The loader is the same file at EFI\OC\OpenCore.efi, EFI\BOOT\BOOTX64.EFI and
EFI\CLOVER\CLOVERX64.efi.  OpenDuetPkg uses the first; the others are there so
this image still boots if "boot" is replaced with a different DUET build.

To use a different hypervisor build, copy one of hv\*.efi over
Core2Again.efi in this directory.  Nothing else needs to change.

This volume is plain FAT32: mount it and edit it like any USB stick.
LICENSE.TXT, EDK2.TXT and OPENCORE.TXT contain the bundled license notices.
TXT
mcopy -i "$WORK/part.img" "$WORK/README.TXT" ::/README.TXT
mcopy -i "$WORK/part.img" "$REPO/LICENSE" ::/LICENSE.TXT
mcopy -i "$WORK/part.img" "$REPO/licenses/edk2.txt" ::/EDK2.TXT
mcopy -i "$WORK/part.img" "$REPO/licenses/opencore.txt" ::/OPENCORE.TXT

# 4. The disk: MBR partition table, one active FAT32 primary.  boot0 chains to
#    the active partition's boot record, so the flag is load-bearing.
dd if=/dev/zero of="$OUT" bs=512 count="$TOTAL_SECT" status=none
sfdisk --quiet "$OUT" <<EOF
label: dos
unit: sectors
start=$PART_START, size=$PART_SECT, type=0c, bootable
EOF

dd if="$WORK/part.img" of="$OUT" bs=512 seek="$PART_START" conv=notrunc status=none

# 5. boot0 into the MBR's code area only - the partition table sfdisk just
#    wrote lives at 446 and must survive.
dd if="$DUETDIR/boot0" of="$OUT" bs=1 count=446 conv=notrunc status=none

# 6. Give the disk an MBR signature.  OpenCore's own install procedure copies
#    446 bytes of boot0 over the MBR, which zeroes the signature at 440..443,
#    and a disk whose signature is zero is one every OS has to guess about -
#    Windows keys its drive-letter database on it, and a UEFI device path for
#    such a partition comes out as HD(1,MBR,0x00000000), which is ambiguous the
#    moment a second one is attached.  In this blob 440..445 is padding (the
#    last thing boot0 uses is the "boot0af: " string, ending at 439), so the
#    signature fits without displacing code - verified by booting an image with
#    one written.
#
#    This does NOT silence DUET's "BOOT MISMATCH!".  That was the obvious guess
#    and it is wrong: an image carrying a real signature prints it just the
#    same.  The message is harmless here for a different reason - see
#    docs/legacy-boot.md.
#
#    The same pass fixes the FAT32 BPB's "hidden sectors" field.  mkfs.vfat
#    built the volume as a standalone file, so it recorded 0 sectors preceding
#    the partition; the partition in fact starts at LBA 2048.  Booting does not
#    depend on this - boot1f32 recomputes the value at runtime as
#    reserved_sectors + the partition start LBA taken from the MBR entry boot0
#    passes it in SI - but a filesystem that misreports where it begins is
#    wrong, and it is wrong in a way that only shows up in whatever tool trusts
#    the field next.
python3 - "$OUT" "$PART_START" <<'SIGEOF'
import sys, os
start = int(sys.argv[2])
with open(sys.argv[1], 'r+b') as f:
    sig = os.urandom(4)
    while sig == b'\x00\x00\x00\x00':
        sig = os.urandom(4)
    f.seek(440)
    f.write(sig + b'\x00\x00')
    f.seek(start * 512 + 28)
    f.write(start.to_bytes(4, 'little'))
SIGEOF

# 7. Check the image that actually ships, not the pieces it was made from.
#    Everything above can succeed against the staging file and still produce an
#    unbootable disk if the partition offset is wrong, so re-open the finished
#    image through the partition table and confirm each stage of the chain is
#    where the stage before it will look for it.
echo -n "  verifying ... "
fail=0
sig="$(python3 -c "
import sys
d = open(sys.argv[1],'rb').read(512)
ok = d[510:512] == b'\x55\xaa' and d[:4] != b'\x00\x00\x00\x00' and d[446] == 0x80
print('ok' if ok else 'bad')" "$OUT")"
[ "$sig" = ok ] || { echo "FAIL: MBR is not bootable boot0 with an active partition"; fail=1; }

for F in ::/boot ::/Core2Again.efi ::/EFI/OC/OpenCore.efi \
         ::/EFI/BOOT/BOOTX64.EFI ::/EFI/CLOVER/CLOVERX64.efi \
         ::/LICENSE.TXT ::/EDK2.TXT ::/OPENCORE.TXT; do
  mdir -i "$OUT@@${PART_START}S" "$F" >/dev/null 2>&1 ||
    { echo "FAIL: $F missing from the finished image"; fail=1; }
done

# boot1f32 finds its payload by the 8.3 name "BOOT" in the root directory, and
# it is the BPB spliced in above that tells it where the root directory is.  If
# either is wrong the machine hangs with no message at all - boot1f32's failure
# path is a bare hlt - so confirm the FAT32 boot record carries both.
python3 - "$OUT" "$PART_START" <<'CHKEOF'
import sys
off = int(sys.argv[2]) * 512
with open(sys.argv[1], 'rb') as f:
    f.seek(off); bs = f.read(512)
bad = []
if bs[0x5A:0x65] != b'BOOT       ': bad.append('boot1f32 filename')
if bs[0x52:0x5A] != b'FAT32   ':    bad.append('BPB not spliced in')
if bs[510:512]   != b'\x55\xaa':    bad.append('no PBR signature')
if bad:
    print('FAIL: ' + ', '.join(bad)); sys.exit(1)
CHKEOF
[ $? -eq 0 ] || fail=1
[ "$fail" = 0 ] && echo "ok" || { echo "  image is not bootable"; exit 1; }

echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
echo
echo "test:  qemu-system-x86_64 -machine pc -m 4096 -drive file=$OUT,format=raw,if=ide"
echo "write: sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
