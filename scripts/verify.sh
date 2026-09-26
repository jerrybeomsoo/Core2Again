#!/usr/bin/env bash
#
# Post-build sanity checks on a built image.  All of these have bitten this
# project before, and none of them shows up as a build error.
#
#   scripts/verify.sh [path/to/Core2Again.efi]
#
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${1:-$REPO/Build/Core2AgainPkg/RELEASE_GCC/X64/Core2Again.efi}"
[ -f "$IMG" ] || { echo "no image at $IMG (build first)" >&2; exit 1; }

DIS="$(mktemp)"; trap 'rm -f "$DIS"' EXIT
objdump -d "$IMG" > "$DIS"
fail=0

# Is this the hypervisor, or one of the small helper images (Core2AgainLoader, the test
# applications) that has no VM-exit path at all?  Checks 2 and 3 are meaningless
# for those, and reporting FAIL on a module that works perfectly is worse than
# not checking it.
IS_HV=0
grep -qE "vmresume|vmlaunch" "$DIS" && IS_HV=1

# 1. The host must not contain the instructions it exists to emulate.  One
#    auto-vectorised PCMPISTRI in host code is an unrecoverable #UD.
echo -n "SSE4.1/4.2/POPCNT in host code ... "
hits=$(grep -oiE "[[:space:]](popcnt|crc32|pcmp[ei]str[im]|pcmpgtq|ptest|pblend[vw]|blend[vp][ps]|pmov[sz]x[bwd][wdq]|pmul(dq|ld)|pm(in|ax)(sb|sd|uw|ud)|packusdw|round[ps][sdp]|insertps|extractps|pinsr[bdq]|pextr[bdq]|mpsadbw|phminposuw|movntdqa|dpp[sd])[[:space:]]" "$DIS" | sort -u || true)
if [ -n "$hits" ]; then echo "FAIL"; echo "$hits"; fail=1; else echo "none (ok)"; fi

# 2. The assembly hands HandleVmExit its arguments in RCX/RDX with Win64 shadow
#    space.  Without EFIAPI (ms_abi) a GCC build reads them from RDI/RSI and
#    dies on the first VM-exit.
echo -n "HandleVmExit MS-ABI handoff     ... "
if [ "$IS_HV" = 0 ]; then
  echo "n/a (no VM-exit path in this image)"
elif grep -qE "lea +0x200\(%rsp\),%rcx" "$DIS" && grep -qE "sub +\\\$0x20,%rsp" "$DIS"; then
  echo "ok"
else
  echo "FAIL: exit handler does not set up rcx/rdx + shadow space"; fail=1
fi

# 3. Look for the trampoline's own opening bytes (cli; cld; mov ax,cs; mov ds,ax).
#    The image is a stripped PE/COFF, so there is no symbol table to ask - an
#    earlier version of this check used nm and therefore always answered "no".
echo -n "AP trampoline embedded          ... "
if [ "$IS_HV" = 0 ]; then
  echo "n/a (not the hypervisor)"
elif python3 - "$IMG" <<'PYEOF'
import sys
sys.exit(0 if b"\xFA\xFC\x8C\xC8\x8E\xD8\x8E\xD0" in open(sys.argv[1],'rb').read() else 1)
PYEOF
then echo "yes"; else echo "no (single-core build)"; fi

# 4. A RELEASE image must contain no diagnostics: no framebuffer drawing, no
#    counters, nothing recorded.  The digit font is the cheapest proof - it is
#    data only the drawing code refers to, so its presence means the whole
#    instrument came along.  Checked here because "did the debug build ship by
#    mistake" is not otherwise visible in a stripped binary.
echo -n "diagnostics compiled out        ... "
HASFONT=1
if [ "$IS_HV" = 0 ]; then
  echo "n/a (not the hypervisor)"
else
  python3 -c "import sys; sys.exit(0 if bytes.fromhex('0e11131519110e') in open(sys.argv[1],'rb').read() else 1)" "$IMG" && HASFONT=0
  case "$IMG" in
    *DEBUG*|*apdebug*)
      if [ "$HASFONT" = 0 ]; then echo "present, as intended for this build"
      else echo "FAIL - diagnostics missing from a debug image"; fail=1; fi ;;
    *)
      if [ "$HASFONT" = 0 ]; then echo "FAIL - drawing code is in a RELEASE image"; fail=1
      else echo "yes"; fi ;;
  esac
fi

exit $fail
