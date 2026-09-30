#!/usr/bin/env python3
"""Run a diagnostic command through the opt-in QEMU console or QMP socket."""
import argparse
import json
from pathlib import Path
import socket
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("command")
parser.add_argument("--socket", default="out/test-aosp_arm64-BP4A.251205.006/debug.sock")
parser.add_argument("--qmp", action="store_true")
parser.add_argument("--timeout", type=float, default=10)
parser.add_argument("--output")
args = parser.parse_args()
data = bytearray()
with socket.socket(socket.AF_UNIX) as conn:
    conn.settimeout(args.timeout)
    conn.connect(args.socket)
    if args.qmp:
        stream = conn.makefile("rb")
        greeting = json.loads(stream.readline())
        if "QMP" not in greeting:
            raise SystemExit("Socket did not return a QMP greeting")
        def request(payload):
            conn.sendall(json.dumps(payload).encode() + b"\n")
            while True:
                response = json.loads(stream.readline())
                if "error" in response:
                    raise SystemExit(json.dumps(response))
                if "return" in response:
                    return response
        request({"execute": "qmp_capabilities"})
        data.extend(json.dumps(request(json.loads(args.command))).encode())
    else:
        marker = "__QEMU_COMMAND_DONE__"
        conn.sendall(("\n" + args.command + "\necho " + marker + "\n").encode())
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            conn.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                chunk = conn.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data.extend(chunk)
            if ("\r\n" + marker + "\r\n").encode() in data:
                break
if args.output:
    Path(args.output).write_bytes(data)
print(data.decode(errors="replace"))
