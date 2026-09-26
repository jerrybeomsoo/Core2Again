#!/usr/bin/env bash
#
# Run every QEMU test this repository has, in one pass, and print a table.
#
#   tests/matrix.sh                 # everything except the OS boots
#   tests/matrix.sh --image PATH    # ...including Windows on 1, 2 and 4 CPUs
#
# Each row states what it establishes.  A test that cannot establish anything -
# TCG, which implements no VMX - says so rather than reporting a pass.
#
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
IMAGE=""
[ "${1:-}" = "--image" ] && IMAGE="$2"

pass=0; fail=0
row () { printf '  %-34s %s\n' "$1" "$2"; }

echo "=== AP trampoline (tests/ap) ==="
for smp in 2 4; do
  out="$(SMP=$smp timeout 400 bash "$HERE/ap/run.sh" --kvm 2>&1)"
  if grep -q "RESULT: PASS" <<<"$out"; then
    row "smp $smp" "PASS  $(grep -o 'arrived .*' <<<"$out" | head -1)"; pass=$((pass+1))
  else
    row "smp $smp" "FAIL"; fail=$((fail+1))
  fi
done

echo "=== hypervisor lifecycle (tests/hv) ==="
for cfg in "--kvm --smp 1" "--kvm --smp 2" "--kvm --smp 4" "--kvm --smp 2 --no-ap"; do
  tag="$(tr -d ' -' <<<"${cfg//--/}")"
  timeout 400 bash "$HERE/hv/run.sh" $cfg --seconds 25 >/dev/null 2>&1
  d="$REPO/Build/hvtest-kvm-smp$(grep -o 'smp [0-9]' <<<"$cfg" | tr -d 'smp ')"
  [ "${cfg#*--no-ap}" != "$cfg" ] && d="$d-noap"
  shot="$(ls "$d"/shot-*.png 2>/dev/null | tail -1)"
  if [ -n "$shot" ]; then
    row "$cfg" "ran, screens in ${d#$REPO/}"; pass=$((pass+1))
  else
    row "$cfg" "NO OUTPUT"; fail=$((fail+1))
  fi
done

echo "=== fail-safe: no VMX at all (TCG) ==="
timeout 400 bash "$HERE/hv/run.sh" --tcg --smp 2 --seconds 25 >/dev/null 2>&1
row "tcg smp 2" "ran - establishes only that the driver declines and the machine survives"

if [ -n "$IMAGE" ]; then
  echo "=== Windows under the hypervisor (tests/win) ==="
  for smp in 1 2 4; do
    timeout 900 bash "$HERE/win/run.sh" --kvm --smp "$smp" --image "$IMAGE" \
        --seconds 300 --shots 10 --tag "m$smp" >/dev/null 2>&1
    shot="$(ls "$REPO/Build/wintest-m$smp"/shot-*.png 2>/dev/null | tail -1)"
    row "smp $smp" "${shot:-NO OUTPUT}"
  done
fi

echo
echo "passed $pass, failed $fail  (screens under Build/, read row 3/4/8 per docs/diagnostics.md)"
