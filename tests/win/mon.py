#!/usr/bin/env python3
"""Send one QEMU monitor (HMP) command over a unix socket.

Used by run.sh in place of socat, which is not always installed.

    mon.py /path/to/monitor.sock "screendump out.ppm"
"""
import socket
import sys
import time

path, cmd = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(path)
time.sleep(0.2)
try:
    s.recv(65536)                      # banner
except OSError:
    pass
s.sendall((cmd + "\n").encode())
time.sleep(0.5)
try:
    sys.stdout.write(s.recv(65536).decode("utf-8", "replace"))
except OSError:
    pass
s.close()
