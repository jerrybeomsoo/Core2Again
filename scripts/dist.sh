#!/usr/bin/env bash
#
# Build every shipping variant into dist/.
#
#   dist/Core2Again-RELEASE.efi     <- the one to deploy
#   dist/Core2Again-DEBUG.efi       <- same code, unoptimised, asserts live
#   dist/Core2Again-singlecore.efi  <- APs left alone (needs numproc=1)
#   dist/Core2Again-apdebug.efi     <- parks each AP at its SIPI exit,
#                                            so the AP status word can be read
#   dist/Core2AgainLoader.efi                       <- chainloader; also the boot target
#                                            of the legacy images below
#   dist/core2again-legacy-blockio.img            <- bootable USB image for a machine
#   dist/core2again-legacy-native.img               with no UEFI firmware (see
#                                            docs/legacy-boot.md)
#   dist/core2again-legacy-*.img.gz               <- matching compressed copies
#
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

build_one () {                # $1=target  $2=ap  $3=output name  $4=extra defines
  echo "=== $3 ==="
  ( export EDK2_PATH="${EDK2_PATH:-$REPO/../edk2}"
    export HV_EXTRA_BUILD_ARGS="-D HV_AP_STARTUP=$2 ${4:-}"
    "$REPO/scripts/build.sh" "$1" )
  cp -f "Build/Core2AgainPkg/$1_GCC/X64/Core2Again.efi" "dist/$3"
}

mkdir -p dist
build_one RELEASE TRUE  Core2Again-RELEASE.efi
build_one DEBUG   TRUE  Core2Again-DEBUG.efi
build_one RELEASE FALSE Core2Again-singlecore.efi
build_one RELEASE TRUE  Core2Again-apdebug.efi     "-D HV_AP_DEBUG=TRUE"
cp -f "Build/Core2AgainPkg/RELEASE_GCC/X64/Core2AgainLoader.efi" dist/Core2AgainLoader.efi

# The legacy-BIOS images.  These need the .efi files above, so they are built
# last; mkimage.sh fetches the DUET binaries once and caches them in Build/.
echo "=== core2again-legacy-blockio.img ==="
"$REPO/scripts/mkimage.sh" --variant blockio >/dev/null
echo "=== core2again-legacy-native.img ==="
"$REPO/scripts/mkimage.sh" --variant native  >/dev/null

# Keep the documented gzip deployment path in step with the images built above.
# Publish each compressed copy only after checking it expands to this run's
# image; otherwise a successful dist can leave an older boot chain in .img.gz.
for variant in blockio native; do
  image="dist/core2again-legacy-$variant.img"
  compressed="$image.gz"
  staging="$compressed.tmp"
  gzip -n -c "$image" > "$staging"
  gzip -cd "$staging" | cmp - "$image"
  mv -f "$staging" "$compressed"
done

echo
echo "dist/:"
ls -l dist/
