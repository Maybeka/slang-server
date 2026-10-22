"""Framed stdio LSP client. Timing ends at body receipt, before JSON decoding."""

import json
import os
import queue
import subprocess
import threading
import time


class RpcError(AssertionError):
    def __init__(self, message):
        super().__init__(message)
        self.code = message["error"].get("code")


class Client:
    def __init__(self, binary, root, extra_env=None):
        self.stderr = open(root / "server.stderr", "wb")
        self.process = subprocess.Popen(
            [str(binary)],
            cwd=root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr,
            env={**os.environ, "SLANG_SERVER_TESTS": "YES", **(extra_env or {})},
        )
        self.messages = queue.Queue()
        self.pending = []
        self.serial = 0
        threading.Thread(target=self._read, daemon=True).start()
        self.request(
            "initialize",
            {
                "processId": os.getpid(),
                "rootUri": root.as_uri(),
                "workspaceFolders": [{"uri": root.as_uri(), "name": "method-test"}],
                "capabilities": {
                    "textDocument": {
                        "completion": {
                            "completionItem": {"documentationFormat": ["markdown"]}
                        },
                        "definition": {"linkSupport": True},
                        "publishDiagnostics": {"versionSupport": True},
                    }
                },
            },
        )
        self.notify("initialized", {})

    def _read(self):
        try:
            while True:
                length = None
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise EOFError("server stdout closed")
                    if line == b"\r\n":
                        break
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1])
                if length is None:
                    raise ValueError("missing Content-Length")
                body = self.process.stdout.read(length)
                received = time.perf_counter_ns()
                if len(body) != length:
                    raise EOFError("short LSP body")
                self.messages.put((json.loads(body), received))
        except BaseException as error:
            self.messages.put((error, time.perf_counter_ns()))

    @staticmethod
    def frame(message):
        data = json.dumps({"jsonrpc": "2.0", **message}, separators=(",", ":")).encode()
        return b"Content-Length: " + str(len(data)).encode() + b"\r\n\r\n" + data

    def send(self, messages):
        encoded = b"".join(self.frame(m) for m in messages)
        started = time.perf_counter_ns()
        self.process.stdin.write(encoded)
        self.process.stdin.flush()
        return started

    def notify(self, method, params):
        return self.send([{"method": method, "params": params}])

    def wait(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while True:
            for index, (message, timestamp) in enumerate(self.pending):
                if predicate(message):
                    self.pending.pop(index)
                    return message, timestamp
            message, timestamp = self.messages.get(
                timeout=max(0.001, deadline - time.monotonic())
            )
            if isinstance(message, BaseException):
                raise message
            if "id" in message and "method" in message:
                # Dynamic registration / configuration / refresh requests.
                result = [] if message["method"] == "workspace/configuration" else None
                self.send([{"id": message["id"], "result": result}])
                continue
            self.pending.append((message, timestamp))

    def request(self, method, params, changes=()):
        self.serial += 1
        request_id = self.serial
        start = self.send(
            [*changes, {"id": request_id, "method": method, "params": params}]
        )
        message, end = self.wait(lambda m: m.get("id") == request_id)
        if "error" in message:
            raise RpcError(message)
        return message.get("result"), (end - start) / 1e6

    def state(self, compact=False):
        return self.request(
            "workspace/executeCommand",
            {"command": "slang.methodScopeStatus", "arguments": ["compact" if compact else "state"]},
        )[0]

    def close(self):
        try:
            self.request("shutdown", {})
            self.notify("exit", {})
            self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.stderr.close()
