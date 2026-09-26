#!/usr/bin/env bash
#
# Build Core2AgainPkg on Linux with GCC.
#
#   scripts/build.sh                 # DEBUG and RELEASE
#   scripts/build.sh DEBUG           # just one target
#   EDK2_PATH=/path/to/edk2 scripts/build.sh
#
# The package is found through PACKAGES_PATH, so this repository never has to be
# copied into the EDK II tree - edk2 stays a pristine, separate checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDK2="${EDK2_PATH:-$REPO/../edk2}"

if [ ! -f "$EDK2/edksetup.sh" ]; then
  echo "error: no EDK II tree at '$EDK2'." >&2
  echo "       clone one and re-run with EDK2_PATH=/path/to/edk2 $0" >&2
  exit 1
fi

case "${1:-both}" in
  DEBUG|debug)     TARGETS="DEBUG" ;;
  RELEASE|release) TARGETS="RELEASE" ;;
  both|BOTH|"")    TARGETS="DEBUG RELEASE" ;;
  *) echo "usage: $0 [DEBUG|RELEASE|both]" >&2; exit 1 ;;
esac

# Run from the repo root: edk2 builds the module in the *current* directory if
# that directory contains .inf files, ignoring -p.
cd "$REPO"

export WORKSPACE="$REPO"
export PACKAGES_PATH="$REPO:$EDK2"
export EDK_TOOLS_PATH="$EDK2/BaseTools"
export CONF_PATH="$EDK2/Conf"
export PYTHON_COMMAND="${PYTHON_COMMAND:-python3}"
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$PATH"

command -v nasm >/dev/null || { echo "error: nasm not found (MdePkg/BaseLib needs it)" >&2; exit 1; }
[ -x "$EDK_TOOLS_PATH/BinWrappers/PosixLike/build" ] || chmod +x "$EDK_TOOLS_PATH"/BinWrappers/PosixLike/* 2>/dev/null || true

for T in $TARGETS; do
  echo "=== $T ==="
  # HV_EXTRA_BUILD_ARGS lets scripts/dist.sh select the AP-startup variant.
  # shellcheck disable=SC2086
  build -a X64 -t GCC -b "$T" ${HV_EXTRA_BUILD_ARGS:-} \
        -p Core2AgainPkg/Core2Again.dsc
  echo "--> $REPO/Build/Core2AgainPkg/${T}_GCC/X64/Core2Again.efi"
done
