#!/usr/bin/env bash
#
# Assemble scripts/ap_trampoline.S into Core2AgainPkg/ApTrampoline.h.
#
# That header is the single source of truth for the trampoline: the driver
# (ApStartup.c) includes it to start real APs, and the QEMU harness (tests/ap)
# includes it to start one where a fault names itself instead of resetting the
# machine.  Generating both from the same .S is what keeps the thing under test
# byte-for-byte identical to the thing that ships.
#
#   scripts/gen_ap_trampoline.sh            # regenerate the header
#   scripts/gen_ap_trampoline.sh --disasm   # disassembly, to eyeball it first
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
gcc -c -x assembler-with-cpp -o "$TMP/t.o" "$HERE/ap_trampoline.S"
objcopy -O binary --only-section=.text "$TMP/t.o" "$TMP/t.bin"

if [ "${1:-}" = "--disasm" ]; then
  # The blob changes CPU mode twice, so no single disassembler setting reads all
  # of it: decode each section with the width it actually executes in.  Offsets
  # come from the .S labels.
  P32=$(objdump -d "$TMP/t.o" | awk '/<prot32>:/{print strtonum("0x" $1)}')
  E64=$(objdump -d "$TMP/t.o" | awk '/<entry64>:/{print strtonum("0x" $1)}')
  echo "=== real mode (16-bit), 0x0 .. $(printf 0x%x "$P32") ==="
  objdump -D -b binary -m i8086       -Mintel --stop-address="$P32"  "$TMP/t.bin" | tail -n +7
  echo "=== protected mode (32-bit), $(printf 0x%x "$P32") .. $(printf 0x%x "$E64") ==="
  objdump -D -b binary -m i386        -Mintel --start-address="$P32" --stop-address="$E64" "$TMP/t.bin" | tail -n +7
  echo "=== long mode (64-bit), from $(printf 0x%x "$E64") ==="
  objdump -D -b binary -m i386:x86-64 -Mintel --start-address="$E64" "$TMP/t.bin" | tail -n +7
  exit 0
fi

OUT="$HERE/../Core2AgainPkg/ApTrampoline.h"
python3 "$HERE/gen_ap_header.py" "$TMP/t.bin" "$HERE/ap_trampoline.S" > "$OUT"
echo "wrote $OUT ($(stat -c%s "$TMP/t.bin") bytes of code)"
