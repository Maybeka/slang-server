"""Prove mini-only parent parsing on the separate counted binary; never time it.

Use the plain large-package fixture so current full diagnostics run in the
isolated worker. Its snapshot capture may parse sources in the parent. Queued
observations separate foreground queries from idle diagnostics; child events
are excluded by PID. This makes no zero-full-parse claim about diagnostics.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import tempfile

from body_queries import fixture, position, sequence
from fixture import HEADER
from protocol import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    evidence = []
    with tempfile.TemporaryDirectory(prefix="body-count-", dir=args.output.parent) as temp:
        root = Path(temp)
        fixture(root, 5000, False)
        for enabled in (False, True):
            (root / ".slang/server.json").write_text(json.dumps({
                "flags": "--single-unit --threads 1", "indexingThreads": 1,
                "compilationFilelist": "design.f", "experimentalMethodBodyQueries": enabled}))
            log = root / ("on.csv" if enabled else "off.csv")
            c = Client(args.binary.resolve(), root, {"SLANG_SERVER_PARSE_COUNT_LOG": str(log)})
            uri = (root / "item.svh").as_uri()
            def query(text):
                return {"textDocument": {"uri": uri}, "position": position(
                    text, text.index("return (local_") + len("return (loc"))}
            def counts(limit):
                parent = [kind for pid, kind, _ in (
                    line.split(",") for line in log.read_text().splitlines())
                    if int(pid) == c.process.pid]
                assert len(parent) >= limit, (len(parent), limit)
                return Counter(parent[:limit])
            try:
                c.request("textDocument/completion", query(HEADER), changes=[{
                    "method": "textDocument/didOpen", "params": {"textDocument": {
                        "uri": uri, "version": 1, "languageId": "systemverilog", "text": HEADER}}}])
                assert c.state()["parserUnits"] is not None, "use slang_server_counted"
                c.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics" and
                       m["params"]["uri"] == uri and m["params"].get("version") == 1)
                rows = []
                for version, text in enumerate(sequence(), 2):
                    before = counts(c.state()["parserUnits"])
                    # Queue the observation with the query so the server records
                    # its counter before entering the idle diagnostic pass. A
                    # native diagnostic snapshot may then parse current sources
                    # in the parent; those events are reported separately.
                    requests = []
                    for method, params in (("textDocument/completion", query(text)),
                                           ("textDocument/definition", query(text)),
                                           ("workspace/executeCommand", {
                                               "command": "slang.methodScopeStatus",
                                               "arguments": ["state"]})):
                        c.serial += 1
                        requests.append({"id": c.serial, "method": method, "params": params})
                    c.send([{"method": "textDocument/didChange", "params": {
                        "textDocument": {"uri": uri, "version": version},
                        "contentChanges": [{"text": text}]}}] + requests)
                    marker, _ = c.wait(lambda m: m.get("id") == requests[-1]["id"])
                    assert "error" not in marker, marker
                    for request in requests[:-1]:
                        reply, _ = c.wait(lambda m, identity=request["id"]: m.get("id") == identity)
                        assert "error" not in reply and reply.get("result"), reply
                    after = counts(marker["result"]["parserUnits"])
                    delta = after-before
                    assert delta == Counter({"mini-shape" if enabled else "other": 1}), delta
                    c.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics" and
                           m["params"]["uri"] == uri and m["params"].get("version") == version)
                    background = counts(c.state()["parserUnits"])-after
                    rows.append({"version": version, "foregroundParentParserDelta": dict(delta),
                                 "diagnosticParentParserDelta": dict(background)})
                status = c.state()
                assert status["active"] == enabled
                evidence.append({"enabled": enabled, "edits": rows})
            finally:
                c.close()
    args.output.write_text(json.dumps({"passed": True, "rows": evidence,
        "scope": "Parser observation only; no timings from the counted binary."}, indent=2)+"\n")
    print("PASS: each accepted edit parses only one mini-shaped source in the parent")


if __name__ == "__main__":
    main()
