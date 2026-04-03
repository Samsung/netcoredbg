#!/usr/bin/env python3
"""
Test breakpoints in netcoredbg DAP (vscode) mode.

Sends DAP JSON messages to netcoredbg and checks whether breakpoints
on regular function lines and CE block lines actually fire.

Expected result (demonstrating the bug):
  - Line 8  (regular function): breakpoint fires  OK
  - Line 17 (CE block):         breakpoint slides to cctor or never fires
"""

import json
import subprocess
import sys
import os
import re
import select
import threading

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(SCRIPT_DIR, "FSharpCeBreakpointRepro", "bin", "Debug", "net8.0", "FSharpCeBreakpointRepro.dll")
SOURCE = os.path.join(SCRIPT_DIR, "FSharpCeBreakpointRepro", "Program.fs")
NETCOREDBG = os.environ.get("NETCOREDBG", os.path.expanduser("~/.local/bin/netcoredbg/netcoredbg"))

class DAPClient:
    """Buffered DAP protocol reader/writer."""

    def __init__(self, proc):
        self.proc = proc
        self.fd = proc.stdout.fileno()
        self.buf = b""
        self._seq = 0

    def _raw_read(self, timeout_secs=15):
        """Non-blocking read from stdout fd."""
        ready, _, _ = select.select([self.fd], [], [], timeout_secs)
        if not ready:
            raise TimeoutError(f"Timeout, buf={self.buf[:80]!r}")
        chunk = os.read(self.fd, 65536)
        if not chunk:
            raise EOFError("netcoredbg closed stdout")
        return chunk

    def _ensure(self, n, timeout=15):
        """Ensure at least n bytes in buffer."""
        while len(self.buf) < n:
            self.buf += self._raw_read(timeout)

    def _read_until(self, marker, timeout=15):
        while marker not in self.buf:
            self.buf += self._raw_read(timeout)
        idx = self.buf.index(marker) + len(marker)
        data, self.buf = self.buf[:idx], self.buf[idx:]
        return data

    def recv(self):
        """Read one DAP message."""
        header = self._read_until(b"\r\n\r\n")
        m = re.search(rb"Content-Length:\s*(\d+)", header)
        if not m:
            raise ValueError(f"Bad header: {header!r}")
        length = int(m.group(1))
        self._ensure(length)
        body, self.buf = self.buf[:length], self.buf[length:]
        msg = json.loads(body)
        # msg received
        return msg

    def send(self, command, arguments=None):
        self._seq += 1
        msg = {
            "type": "request",
            "seq": self._seq,
            "command": command,
            "arguments": arguments or {}
        }
        body = json.dumps(msg).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        self.proc.stdin.write(header + body)
        self.proc.stdin.flush()
        return self._seq

    def wait_for(self, msg_type, command=None, event=None, timeout_msgs=100):
        """Read messages until matching one found."""
        for _ in range(timeout_msgs):
            try:
                msg = self.recv()
            except (TimeoutError, EOFError):
                return None
            t = msg.get("type")
            if t == msg_type:
                if command and msg.get("command") == command:
                    return msg
                if event and msg.get("event") == event:
                    return msg
                if not command and not event:
                    return msg
        return None


def main():
    print(f"DLL:        {DLL}")
    print(f"Source:     {SOURCE}")
    print(f"netcoredbg: {NETCOREDBG}")
    print()

    if not os.path.exists(DLL):
        print("ERROR: Build first with 'dotnet build -c Debug'")
        sys.exit(1)

    proc = subprocess.Popen(
        [NETCOREDBG, "--interpreter=vscode"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    dap = DAPClient(proc)

    try:
        # 1. Initialize
        dap.send("initialize", {
            "clientID": "repro-test",
            "adapterID": "coreclr",
            "linesStartAt1": True,
            "columnsStartAt1": True,
            "pathFormat": "path",
        })
        resp = dap.wait_for("response", command="initialize")
        print(f"1. initialize: {'OK' if resp and resp.get('success') else 'FAIL'}")

        # 2. Launch with stopAtEntry
        dap.send("launch", {
            "name": "repro",
            "type": "coreclr",
            "request": "launch",
            "program": "dotnet",
            "args": ["exec", DLL],
            "cwd": os.path.dirname(DLL),
            "stopAtEntry": True,
            "justMyCode": True,
        })
        resp = dap.wait_for("response", command="launch")
        print(f"2. launch: {'OK' if resp and resp.get('success') else 'FAIL'}")

        # 3. Set breakpoints
        dap.send("setBreakpoints", {
            "source": {"name": "Program.fs", "path": SOURCE},
            "breakpoints": [
                {"line": 8},   # regular function
                {"line": 17},  # CE block
            ],
            "lines": [8, 17],
            "sourceModified": False,
        })
        resp = dap.wait_for("response", command="setBreakpoints")
        bps = resp.get("body", {}).get("breakpoints", []) if resp else []
        print("3. setBreakpoints response:")
        for bp in bps:
            print(f"   line {bp.get('line', '?'):>3}: verified={bp.get('verified')}, "
                  f"id={bp.get('id')}, msg={bp.get('message', '')}")

        # 4. configurationDone
        dap.send("configurationDone")
        resp = dap.wait_for("response", command="configurationDone")
        print(f"4. configurationDone: {'OK' if resp and resp.get('success') else 'FAIL'}")

        # 5. Wait for stopped (entry)
        stopped = dap.wait_for("event", event="stopped")
        reason = stopped.get("body", {}).get("reason", "?") if stopped else "none"
        print(f"5. stopped (entry): reason={reason}")

        # 6. Continue and track hits
        hit_lines = []
        for attempt in range(5):
            dap.send("continue", {"threadId": 1})
            dap.wait_for("response", command="continue")

            stopped = dap.wait_for("event", event="stopped", timeout_msgs=300)
            if not stopped:
                print(f"   continue #{attempt+1}: no stopped event (program likely exited)")
                break

            body = stopped.get("body", {})
            reason = body.get("reason", "?")
            tid = body.get("threadId", 1)
            if reason == "breakpoint":
                # Use threadId from the stopped event directly
                dap.send("stackTrace", {"threadId": tid, "startFrame": 0, "levels": 5})
                st = dap.wait_for("response", command="stackTrace")
                frames = st.get("body", {}).get("stackFrames", []) if st else []
                if frames:
                    line = frames[0].get("line", "?")
                    name = frames[0].get("name", "?")
                    hit_lines.append(line)
                    src = frames[0].get("source", {}).get("name", "?")
                    print(f"   continue #{attempt+1}: BREAKPOINT at {src}:{line} in {name}")
                else:
                    print(f"   continue #{attempt+1}: breakpoint hit (no stack frames, tid={tid})")
                    print(f"      raw response: {json.dumps(st, indent=2)[:300]}")
            elif reason in ("exited", "terminated"):
                print(f"   continue #{attempt+1}: {reason}")
                break
            else:
                print(f"   continue #{attempt+1}: {reason} (tid={tid})")

        # Summary
        print()
        print("=" * 60)
        print("SUMMARY")
        print("=" * 60)
        print(f"  Line  8 (regular function): {'HIT' if 8 in hit_lines else 'MISSED'}")
        print(f"  Line 17 (CE block body):    {'HIT' if 17 in hit_lines else 'MISSED'}")
        if 15 in hit_lines:
            print(f"  Line 15 (CE module init):   HIT (bp slid from line 17 to 15)")
        print()
        if 17 not in hit_lines:
            print("BUG CONFIRMED: Line 17 breakpoint inside CE block does not fire.")
            print("The PDB has sequence points in ceTests@17.Invoke for lines 17-20,")
            print("but netcoredbg doesn't resolve the bp into the closure class.")
        else:
            print("CE breakpoint fired! Bug may be fixed in this version.")

        # Disconnect
        dap.send("disconnect", {"terminateDebuggee": True})

    finally:
        proc.terminate()
        proc.wait(timeout=5)

if __name__ == "__main__":
    main()
