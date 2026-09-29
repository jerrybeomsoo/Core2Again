#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$REPO/Build/bootgate"
mkdir -p "$OUT"
cp "$REPO/Core2AgainPkg/BootGate.c" "$HERE/Hypervisor.h" "$HERE/test.c" "$OUT/"
gcc -std=c11 -O2 -Wall -Wextra -Werror "$OUT/BootGate.c" "$OUT/test.c" -o "$OUT/test_bootgate"
"$OUT/test_bootgate"
