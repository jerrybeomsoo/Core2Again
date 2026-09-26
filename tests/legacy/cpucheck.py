#!/usr/bin/env python3
"""Ask the QEMU monitor where every vCPU is executing, repeatedly, and judge it.

Windows' processor count is only visible in its own GUI, which needs a working
keyboard in the guest and a legible screenshot.  The question underneath it -
"is the second processor running the operating system, or is it parked?" - can
be answered from outside without touching the guest at all: sample RIP on every
vCPU several times and see where each one spends its time.

Three regions matter, and telling them apart is the whole job:

  0xFFFF8...    the canonical kernel half.  Windows is running on this vCPU.
  low, < 4 GB   firmware-owned memory.  Our hypervisor is a DXE_RUNTIME_DRIVER,
                so its exit handler lives here - a vCPU sampled at such an
                address is most likely inside a VM exit, which is the
                hypervisor working, not a processor that failed to start.
  0x00007F...   the low canonical half: an application.  Also perfectly normal.
  anything else a processor that never left where it was parked.

Sampling twice is not enough, because catching a healthy vCPU mid-exit looks
identical to catching a dead one.  A vCPU passes if it is seen in the guest
kernel at least once across the samples.

    cpucheck.py /path/to/monitor.sock [samples] [gap-seconds] [expected-cpus]
"""
import collections
import re
import socket
import sys
import time

PATH = sys.argv[1]
SAMPLES = int(sys.argv[2]) if len(sys.argv) > 2 else 6
GAP = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
EXPECTED = int(sys.argv[4]) if len(sys.argv) > 4 else None

KERNEL = 0xFFFF800000000000      # Windows maps the kernel here and above
USER_TOP = 0x0000800000000000    # the low canonical half: guest user space
FIRMWARE_TOP = 0x100000000       # below 4 GB: firmware, and our driver with it


def hmp(cmd, settle=1.5):
    """Send one HMP command and read until the output stops arriving."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect(PATH)
    time.sleep(0.2)
    try:
        s.recv(1 << 16)                      # banner
    except OSError:
        pass
    s.sendall((cmd + "\n").encode())
    buf, deadline = b"", time.time() + settle
    while time.time() < deadline:
        try:
            chunk = s.recv(1 << 16)
        except OSError:
            break
        if chunk:
            buf += chunk
            deadline = time.time() + 0.4     # keep reading while it flows
    s.close()
    return buf.decode("utf-8", "replace")


def rips(text):
    """{cpu index: RIP} from 'info registers -a' output."""
    out, cur = {}, None
    for line in text.splitlines():
        m = re.match(r"\s*CPU#(\d+)", line)
        if m:
            cur = int(m.group(1))
            continue
        m = re.search(r"RIP=([0-9a-fA-F]+)", line)
        if m and cur is not None:
            out[cur] = int(m.group(1), 16)
    return out


seen = collections.defaultdict(list)
for i in range(SAMPLES):
    for cpu, rip in rips(hmp("info registers -a")).items():
        seen[cpu].append(rip)
    if i != SAMPLES - 1:
        time.sleep(GAP)

if not seen:
    print("cpucheck: no register output from the monitor")
    sys.exit(2)

fail = 0
for cpu in sorted(seen):
    hits = seen[cpu]
    kern = sum(1 for r in hits if r >= KERNEL)
    fw = sum(1 for r in hits if r < FIRMWARE_TOP)
    user = sum(1 for r in hits if FIRMWARE_TOP <= r < USER_TOP)
    other = len(hits) - kern - fw - user
    where = f"kernel {kern}/{len(hits)}"
    if user:
        where += f", user {user}"
    if fw:
        where += f", in firmware/hypervisor {fw}"
    if other:
        where += f", elsewhere {other}"
    ok = kern > 0
    if not ok:
        fail = 1
    print(f"  CPU#{cpu}: {where}  {'OK' if ok else 'NEVER in guest kernel'}"
          f"   [{', '.join(f'{r:#x}' for r in hits[:4])}]")

n = len(seen)
if EXPECTED is not None:
    expected_ids = set(range(EXPECTED))
    missing = sorted(expected_ids - seen.keys())
    unexpected = sorted(seen.keys() - expected_ids)
    for cpu in missing:
        print(f"  CPU#{cpu}: MISSING from monitor output")
    for cpu in unexpected:
        print(f"  CPU#{cpu}: UNEXPECTED in monitor output")
    if missing or unexpected:
        fail = 1
print(f"cpucheck: PASS - all {n} vCPUs are executing the guest kernel" if not fail
      else "cpucheck: FAIL - a vCPU was missing or never reached the guest kernel")
sys.exit(1 if fail else 0)
