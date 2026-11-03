#!/usr/bin/env python3
"""Minimal native FreeBSD DAP launch/breakpoint acceptance test."""

import argparse
import json
import queue
import subprocess
import threading
import time
from pathlib import Path


class Adapter:
    def __init__(self, executable: Path, log: Path):
        self.process = subprocess.Popen(
            [str(executable), "--interpreter=vscode", f"--engineLogging={log}"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.messages = queue.Queue()
        self.pending = []
        self.seq = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            while True:
                headers = {}
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise EOFError("adapter closed")
                    if line == b"\r\n":
                        break
                    key, value = line.decode().split(":", 1)
                    headers[key.lower()] = value.strip()
                size = int(headers["content-length"])
                self.messages.put(json.loads(self.process.stdout.read(size)))
        except Exception as error:
            self.messages.put(error)

    def send(self, command, arguments):
        self.seq += 1
        body = json.dumps(
            {"seq": self.seq, "type": "request", "command": command, "arguments": arguments}
        ).encode()
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.process.stdin.flush()
        return self.seq

    def wait(self, predicate, timeout=30):
        for index, message in enumerate(self.pending):
            if predicate(message):
                return self.pending.pop(index)
        deadline = time.monotonic() + timeout
        while True:
            message = self.messages.get(timeout=max(0, deadline - time.monotonic()))
            if isinstance(message, Exception):
                raise message
            if message.get("type") == "response" and not message.get("success"):
                raise AssertionError(message)
            if predicate(message):
                return message
            self.pending.append(message)

    def request(self, command, arguments, timeout=30):
        seq = self.send(command, arguments)
        return self.wait(lambda message: message.get("request_seq") == seq, timeout)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("adapter", type=Path)
    parser.add_argument("project", type=Path)
    parser.add_argument("--line-marker", default="BREAKPOINT")
    args = parser.parse_args()

    adapter_path = args.adapter.resolve(strict=True)
    project = args.project.resolve(strict=True)
    source = project / "Program.cs"
    program = next((project / "bin" / "Debug").glob("*/debug-smoke.dll"))
    line = next(
        number
        for number, text in enumerate(source.read_text().splitlines(), 1)
        if args.line_marker in text
    )
    log = project / "netcoredbg-engine.log"
    debugger = Adapter(adapter_path, log)
    try:
        debugger.request(
            "initialize",
            {
                "adapterID": "coreclr",
                "linesStartAt1": True,
                "columnsStartAt1": True,
                "pathFormat": "path",
            },
        )
        launch = debugger.send(
            "launch",
            {
                "program": str(program),
                "cwd": str(project),
                "console": "internalConsole",
                "stopAtEntry": False,
                "justMyCode": True,
            },
        )
        debugger.wait(lambda message: message.get("request_seq") == launch)
        debugger.wait(lambda message: message.get("event") == "initialized")
        debugger.request(
            "setBreakpoints",
            {"source": {"path": str(source)}, "breakpoints": [{"line": line}]},
        )
        debugger.request("configurationDone", {}, timeout=60)
        stopped = debugger.wait(
            lambda message: message.get("event") == "stopped"
            and message.get("body", {}).get("reason") == "breakpoint",
            timeout=60,
        )
        thread_id = stopped["body"]["threadId"]
        frames = debugger.request("stackTrace", {"threadId": thread_id})["body"]["stackFrames"]
        assert frames[0]["source"]["path"] == str(source), frames[0]
        assert frames[0]["line"] == line, frames[0]
        debugger.request("continue", {"threadId": thread_id})
        exited = debugger.wait(lambda message: message.get("event") == "exited", timeout=60)
        assert exited["body"]["exitCode"] == 0, exited
        debugger.wait(lambda message: message.get("event") == "terminated", timeout=10)
        print(f"PASS: launch, breakpoint Program.cs:{line}, continue, exit 0")
    finally:
        debugger.close()
        stderr = debugger.process.stderr.read().decode(errors="replace")
        if stderr:
            print(stderr, end="")


if __name__ == "__main__":
    main()
