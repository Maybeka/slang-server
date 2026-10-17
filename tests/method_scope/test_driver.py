"""Guard the benchmark's response correlation and receipt timestamp contract."""

import io
import json
import queue
from types import SimpleNamespace
from unittest.mock import patch

from protocol import Client


def test_batch_matches_id_and_timestamps_before_decode():
    client = object.__new__(Client)
    client.serial = 0
    client.pending = []
    client.messages = queue.Queue()
    messages = [
        {"id": 999, "result": "old response"},
        {"method": "textDocument/publishDiagnostics", "params": {"version": 1}},
        {"id": 1, "result": ["current response"]},
    ]
    writes = []
    stdout = io.BytesIO(b"".join(Client.frame(m) for m in messages))
    client.process = SimpleNamespace(
        stdout=stdout,
        stdin=SimpleNamespace(
            write=lambda data: writes.append(data), flush=client._read
        ),
    )
    decoder = json.loads
    decode_clock_calls = []
    with patch(
        "protocol.time.perf_counter_ns",
        side_effect=[10_000_000, 11_000_000, 12_000_000, 13_000_000, 14_000_000],
    ) as clock:

        def decode(data):
            decode_clock_calls.append(clock.call_count)
            return decoder(data)

        with patch("protocol.json.loads", side_effect=decode):
            result, elapsed = client.request(
                "textDocument/definition",
                {},
                changes=[
                    {"method": "textDocument/didChange", "params": {"version": 2}}
                ],
            )
    assert result == ["current response"]
    assert elapsed == 3
    assert decode_clock_calls == [2, 3, 4]
    assert len(writes) == 1
    assert writes[0].index(b"textDocument/didChange") < writes[0].index(
        b"textDocument/definition"
    )


def test_short_response_body_is_a_crash_not_a_success():
    client = object.__new__(Client)
    client.messages = queue.Queue()
    client.process = SimpleNamespace(
        stdout=io.BytesIO(b"Content-Length: 100\r\n\r\n{}")
    )
    client._read()
    error, received = client.messages.get_nowait()
    assert isinstance(error, EOFError)
    assert "short LSP body" in str(error)
    assert received > 0
