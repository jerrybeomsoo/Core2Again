#!/usr/bin/env bash
#
# Rebuild a Clover ISO with the hypervisor installed as a Clover driver for
# historical boot comparisons: EFI/CLOVER/drivers64/Core2Again.efi.
#
#   tests/win/mkclover.sh <original.iso> <Core2Again.efi> <out.iso>
#
# drivers64 is the CloverEFI (legacy-BIOS) driver directory used in the older
# T9900 boot test; drivers64UEFI is the UEFI one. The driver goes in both so
# the same ISO works booted either way.
#
# The ISO is rebuilt from a full extraction rather than patched in place.  These
# Clover ISOs carry damaged Rock Ridge records under Library/PreferencePanes
# (macOS resources), and xorriso will only load such a tree with
# -error_behavior image_loading best_effort - which silently drops the long
# filenames and most of the tree.  The resulting ISO boots CloverEFI far enough
# to reach the EDK II front page and no further, which looks like a hypervisor
# problem and is not one.  7z reads the tree correctly, so extract, add, rebuild.
#
set -euo pipefail
SRC="${1:?original iso}"; DRV="${2:?hypervisor efi}"; OUT="${3:?output iso}"
for T in 7z xorriso; do
  command -v "$T" >/dev/null || { echo "error: $T not found" >&2; exit 1; }
done

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
7z x -o"$WORK/tree" "$SRC" >/dev/null

# 7z materialises the El Torito image as a "[BOOT]" directory.  The real boot
# file is already in the tree at usr/standalone/i386/cdboot; the extra copy must
# not end up on the new ISO.
rm -rf "$WORK/tree/[BOOT]"
[ -f "$WORK/tree/usr/standalone/i386/cdboot" ] || {
  echo "error: no usr/standalone/i386/cdboot in $SRC - not a CloverEFI ISO?" >&2
  exit 1
}

install -D -m 0644 "$DRV" "$WORK/tree/EFI/CLOVER/drivers64/Core2Again.efi"
install -D -m 0644 "$DRV" "$WORK/tree/EFI/CLOVER/drivers64UEFI/Core2Again.efi"

rm -f "$OUT"
xorriso -as mkisofs -quiet -V CLOVERCD -o "$OUT" \
        -b usr/standalone/i386/cdboot -no-emul-boot -boot-load-size 4 \
        "$WORK/tree"

echo "wrote $OUT"
xorriso -indev "$OUT" -lsl /EFI/CLOVER/drivers64/ 2>/dev/null | grep -i core2again || true
xorriso -indev "$OUT" -report_el_torito plain 2>/dev/null | grep -i "boot img" || true
