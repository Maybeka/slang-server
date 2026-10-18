"""Verify parser scope on the separate counted executable; never time this run."""

import argparse
from collections import Counter
import json
from pathlib import Path
import tempfile

from check import edited
from fixture import change, open_document, prepare, query
from protocol import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="slang-method-count-") as tmp:
        root = Path(tmp)
        uri = prepare(root, True)
        log = root / "parses.csv"
        client = Client(
            args.binary.resolve(), root, {"SLANG_SERVER_PARSE_COUNT_LOG": str(log)}
        )

        def counts():
            rows = (
                [line.split(",") for line in log.read_text().splitlines()]
                if log.exists()
                else []
            )
            return Counter(
                kind for pid, kind, members in rows if int(pid) == client.process.pid
            )

        evidence = []
        try:
            open_document(client, uri)
            initial = client.state()
            assert initial["parserUnits"] is not None
            before = counts()
            for step in range(1, 10):
                text = edited(step)
                if step == 9:
                    text = text.replace("alpha * bravo", "alpha + bravo")
                params = query(uri, text, ("local_0" if step % 2 else "local_1") + " +")
                client.request(
                    "textDocument/definition", params, [change(uri, step + 1, text)]
                )
                after_change = counts()
                client.request("textDocument/completion", params)
                after_query = counts()
                state = client.state()
                delta = after_change - before
                assert after_query == after_change, (
                    step,
                    before,
                    after_change,
                    after_query,
                )
                if step <= 8:
                    assert state["active"] and state["accepted"] == step, state
                    assert delta == Counter({"mini-shape": 1}), (step, delta)
                else:
                    assert not state["active"] and delta["other"] >= 1, (state, delta)
                evidence.append(
                    {
                        "step": step,
                        "parentDelta": dict(delta),
                        "additionalCompletionParses": dict(after_query - after_change),
                        "active": state["active"],
                        "accepted": state["accepted"],
                        "totalParentUnits": state["parserUnits"],
                    }
                )
                before = after_query
        finally:
            client.close()
        fallbacks = []
        for reason in ("dependency-bytes", "repeated-target"):
            uri = prepare(root, True)
            if reason == "repeated-target":
                owner = root / "owner.sv"
                owner.write_text(
                    owner.read_text()
                    + '\npackage another;\n`include "item.svh"\nendpackage\n'
                )
            log.unlink(missing_ok=True)
            client = Client(
                args.binary.resolve(), root, {"SLANG_SERVER_PARSE_COUNT_LOG": str(log)}
            )
            try:
                open_document(client, uri)
                if reason == "dependency-bytes":
                    client.request(
                        "textDocument/definition",
                        query(uri, edited(1), "local_0 +"),
                        [change(uri, 2, edited(1))],
                    )
                    assert client.state()["active"]
                    sibling = root / "classes_0.svh"
                    sibling.write_text(
                        sibling.read_text() + "\n// external edit, no watcher\n"
                    )
                before = counts()
                client.request(
                    "textDocument/definition",
                    query(uri, edited(2), "local_1 +"),
                    [change(uri, 3, edited(2))],
                )
                delta = counts() - before
                status = client.state()
                assert not status["active"] and delta["other"] >= 1, (
                    reason,
                    delta,
                    status["decline"],
                )
                fallbacks.append(
                    {
                        "reason": reason,
                        "parentDelta": dict(delta),
                        "active": status["active"],
                        "decline": status["decline"],
                    }
                )
            finally:
                client.close()
        args.output.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "observations": evidence,
                    "fallbacks": fallbacks,
                    "note": "mini-shape is the exact single-package/single-class/single-method fixture shape; "
                    "the native fixture has three source roots, 500 sibling classes and seven include edges. "
                    "Private diagnostic children are excluded by PID.",
                },
                indent=2,
            )
            + "\n"
        )
    print(
        "PASS: eight mini parses, no full parent parse on accepted queries, ninth native fallback"
    )


if __name__ == "__main__":
    main()
