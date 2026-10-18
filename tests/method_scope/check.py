"""Strict native oracle for the default-off closed-method experiment.

Run with --binary. Every run creates fresh
processes over one absolute fixture directory. Buffer identity is normalized
only when the server test command proves the URI and current source generation.
"""

import argparse
import copy
import json
from pathlib import Path
import tempfile
from urllib.parse import urlparse, unquote
import hashlib
import re

from fixture import HEADER, change, open_document, position, prepare, query
from protocol import Client


def normalize(value, state, uri, expected_text=None):
    value = copy.deepcopy(value)
    sources = {d["buffer"]: d for d in state["sources"]}
    documents = {d["uri"]: d for d in state["documents"]}

    def visit(item):
        if isinstance(item, dict):
            if {"bufferId", "documentUri", "symbolName"} <= item.keys():
                assert item["documentUri"] == uri, item
                witness = sources[item["bufferId"]]
                assert witness["latest"], (item, witness)
                source_path = Path(unquote(urlparse(witness["uri"]).path))
                if witness["uri"] in documents:
                    assert witness["uri"] == uri
                    version = documents[uri]["version"]
                    expected = (
                        expected_text
                        if expected_text is not None
                        else (edited(version - 1) if version > 1 else HEADER)
                    )
                    if version == 10 and expected_text is None:
                        expected = expected.replace("alpha * bravo", "alpha + bravo")
                else:
                    expected = source_path.read_text()
                    version = "disk"
                assert witness["bytes"] == expected + "\0", (item, witness)
                item["bufferId"] = (
                    "current:"
                    + witness["uri"]
                    + ":"
                    + str(version)
                    + ":"
                    + hashlib.sha256(expected.encode()).hexdigest()
                )
            for child in item.values():
                visit(child)
        elif isinstance(item, list):
            for child in item:
                visit(child)

    visit(value)
    return value


def edited(step):
    # Eight valid same-length edits, including a same-name/range binding change.
    text = HEADER.replace("local_1 +", ("local_0" if step % 2 else "local_1") + " +")
    if step > 1:
        text = text.replace(
            "alpha + bravo", "alpha - bravo" if step % 2 == 0 else "alpha * bravo"
        )
    return text


def sequence(binary, root, enabled, resolve_all=False, steps=9):
    uri = prepare(root, enabled)
    client = Client(binary, root)
    rows = []
    try:
        open_document(client, uri)
        old_items = client.request("textDocument/completion", query(uri, HEADER))[0]
        old_local = next(i for i in old_items if i["label"] == "local_1")
        before = client.state()
        text = HEADER
        for step in range(1, steps + 1):
            text = edited(step)
            # Ninth edit is valid but must exceed this certificate's reuse bound.
            if step == 9:
                text = text.replace("alpha * bravo", "alpha + bravo")
            name = "local_0" if step % 2 else "local_1"
            params = query(uri, text, name + " +")
            links, elapsed = client.request(
                "textDocument/definition", params, [change(uri, step + 1, text)]
            )
            target_offset = text.index(name + " =")
            target = {
                "start": position(text, target_offset),
                "end": position(text, target_offset + len("local_0")),
            }
            assert links == [
                {
                    "targetUri": uri,
                    "targetRange": target,
                    "targetSelectionRange": target,
                }
            ], links
            state_before_completion = client.state()
            items = client.request("textDocument/completion", params)[0]
            state = client.state()
            assert items and any(i["label"] == "local_0" for i in items)
            for item in items:
                assert item.get("textEdit", {}).get("range") == {
                    "start": position(text, text.index(name + " +")),
                    "end": position(text, text.index(name + " +") + 7),
                }, item
            rows.append(
                {
                    "step": step,
                    "definition": links,
                    "items": normalize(items, state, uri),
                    "stateBeforeCompletion": state_before_completion,
                    "state": state,
                    "elapsedMs": elapsed,
                }
            )
            references = []
            body = text.index("int local_0 =") + len("int local_0 =")
            for match in re.finditer(r"\b(alpha|bravo|charl|local_[01])\b", text):
                if match.start() < body or text[match.end() :].lstrip().startswith("="):
                    continue
                for offset in (match.start(), match.start() + 1, match.end()):
                    at = {
                        "textDocument": {"uri": uri},
                        "position": position(text, offset),
                    }
                    definition = client.request("textDocument/definition", at)[0]
                    completion = client.request("textDocument/completion", at)[0]
                    canonical = normalize(completion, client.state(), uri)
                    references.append(
                        {
                            "offset": offset,
                            "definition": definition,
                            "completionSha256": hashlib.sha256(
                                json.dumps(canonical, sort_keys=True).encode()
                            ).hexdigest(),
                        }
                    )
            rows[-1]["references"] = references
            selected = [i for i in items if i["label"] in ("local_0", "alpha")]
            rows[-1]["resolvedLocals"] = [
                client.request("completionItem/resolve", i)[0] for i in selected
            ]
            if enabled and step <= 8:
                assert state["active"] and state["accepted"] == step, state
            if enabled and step == 9:
                assert not state["active"] and "bound" in state["decline"], state
        old = client.request("completionItem/resolve", old_local)[0]
        assert "documentation" not in old, old
        if resolve_all:
            items = client.request(
                "textDocument/completion",
                query(uri, text, ("local_0" if steps % 2 else "local_1") + " +"),
            )[0]
            resolved = [
                client.request("completionItem/resolve", item)[0] for item in items
            ]
            rows[-1]["resolved"] = normalize(resolved, client.state(), uri)
        return {"before": before, "rows": rows}
    finally:
        client.close()


def focused(binary, root, enabled, scenario):
    """Real first/middle/last method selection and declaration+all-use rename."""
    uri = prepare(root, enabled)
    client = Client(binary, root)
    try:
        open_document(client, uri)
        old_items = client.request("textDocument/completion", query(uri, HEADER))[0]
        old_locals = [i for i in old_items if i["label"] in ("local_0", "local_1")]
        text = HEADER
        if scenario == "rename":
            text = text.replace("local_0", "value_0").replace("local_1", "value_1")
            needle, names = "value_1 +", ("value_0", "value_1")
        else:
            prefix, arg = (
                ("early", "other") if scenario == "earlier" else ("later", "extra")
            )
            text = text.replace(prefix + "_1 + " + arg, prefix + "_0 + " + arg)
            needle, names = prefix + "_0 +", (prefix + "_0", prefix + "_1")
        params = query(uri, text, needle)
        links = client.request(
            "textDocument/definition", params, [change(uri, 2, text)]
        )[0]
        name = needle.split()[0]
        at = text.index(name + " =")
        target = {"start": position(text, at), "end": position(text, at + len(name))}
        assert links == [
            {"targetUri": uri, "targetRange": target, "targetSelectionRange": target}
        ]
        items = client.request("textDocument/completion", params)[0]
        current = client.state()
        if enabled:
            assert (
                current["active"]
                and current["accepted"] == 1
                and current["version"] == 2
            ), current
        canonical = normalize(items, current, uri, text)
        selected = [i for i in items if i["label"] in names]
        assert len(selected) == 2
        resolved = [client.request("completionItem/resolve", i)[0] for i in selected]
        resolved = normalize(resolved, client.state(), uri, text)
        if scenario == "rename":
            assert not any(i["label"] in ("local_0", "local_1") for i in items)
            for old in old_locals:
                stale = client.request("completionItem/resolve", old)[0]
                assert "documentation" not in stale, stale
        return {
            "scenario": scenario,
            "definition": links,
            "items": canonical,
            "resolvedLocals": resolved,
            "oldRenamedHandlesUnresolved": scenario == "rename",
        }
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix="slang-method-check-") as path:
        root = Path(path)
        native = sequence(binary, root, False)
        overlay = sequence(binary, root, True)
        for left, right in zip(native["rows"], overlay["rows"]):
            for key in ("items", "definition", "resolvedLocals", "references"):
                assert left.get(key) == right.get(key), (
                    left["step"],
                    key,
                    left.get(key),
                    right.get(key),
                )
        all_native = sequence(binary, root, False, True, 1)
        all_overlay = sequence(binary, root, True, True, 1)
        assert all_native["rows"][-1]["resolved"] == all_overlay["rows"][-1]["resolved"]
        focused_rows = []
        for scenario in ("rename", "earlier", "later"):
            left = focused(binary, root, False, scenario)
            right = focused(binary, root, True, scenario)
            assert left == right, scenario
            focused_rows.append(right)
        args.output.write_text(
            json.dumps(
                {
                    "native": native,
                    "overlay": overlay,
                    "allResolved": all_overlay,
                    "focused": focused_rows,
                },
                indent=2,
            )
            + "\n"
        )
    print(
        "PASS: nine-version definition rebind, full ordered completion JSON, resolved items, old handles"
    )


if __name__ == "__main__":
    main()
