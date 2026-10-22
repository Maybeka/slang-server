"""Offline body-query oracle and paired timing check; fixtures are always removed.

This is a restricted closed-int helper benchmark, not a real UVM workload result.
The UVM arm uses the installed library and semantic owner with inherited classes,
imports and registration macros outside the edited helper. Source handles are
owner-specific; comparisons retain all other public completion / resolve values.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import statistics
import tempfile
import time

from fixture import HEADER
from protocol import Client


def position(text, offset):
    prefix = text[:offset]
    return {"line": prefix.count("\n"), "character": len(prefix.rsplit("\n", 1)[-1])}


def public(value):
    if isinstance(value, dict):
        return {k: public(v) for k, v in value.items() if k != "data"}
    if isinstance(value, list):
        return [public(v) for v in value]
    return value


def sequence():
    result = []
    for version in range(2, 14):
        text = HEADER
        replacement = ("local_0 +\n      (charl * " + str(version * 111) + ")"
                       if version % 3 else "local_1 - charl")
        text = text.replace("local_1 + charl", replacement)
        result.append(text)
    return result


def fixture(root, classes, uvm):
    (root / ".slang").mkdir()
    (root / "design.f").write_text("+incdir+.\nowner.sv\n")
    prefix = 'package owner;\n'
    if uvm:
        prefix += 'import uvm_pkg::*;\n`include "uvm_macros.svh"\n'
    (root / "owner.sv").write_text(prefix + '`include "noise.svh"\n'
                                    '`include "item.svh"\nendpackage\n')
    (root / "noise.svh").write_text("".join(
        f"class Noise_{i}; int field; endclass\n" for i in range(classes)))
    def decorate(text):
        return (text.replace("class Accumulator;", "class Accumulator extends uvm_object;\n"
                            '  `uvm_object_utils(Accumulator)\n'
                            '  function new(string name="Accumulator"); super.new(name); endfunction')
                if uvm else text)
    (root / "item.svh").write_text(decorate(HEADER))
    return decorate


def run(binary, root, enabled, uvm, decorate):
    config = {"indexingThreads": 1, "compilationFilelist": "design.f",
              "flags": "--single-unit --threads 1", "experimentalMethodBodyQueries": enabled}
    if uvm:
        config.update(uvm={"useBundled": True}, experimentalUvmSemanticCache=True,
                      experimentalScopeSemanticCache=True,
                      frozenSources={"packages": ["uvm_pkg"]})
    (root / ".slang/server.json").write_text(json.dumps(config))
    c = Client(binary, root)
    uri = (root / "item.svh").as_uri()
    initial = decorate(HEADER)
    def query(text):
        return {"textDocument": {"uri": uri}, "position": position(
            text, text.index("return (local_") + len("return (loc"))}
    records = []
    try:
        c.request("textDocument/completion", query(initial), changes=[{
            "method": "textDocument/didOpen", "params": {"textDocument": {
                "uri": uri, "version": 1, "languageId": "systemverilog", "text": initial}}}])
        c.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics" and
               m["params"]["uri"] == uri and m["params"].get("version") == 1)
        for version, raw in enumerate(sequence(), 2):
            text = decorate(raw)
            started = time.perf_counter_ns()
            items, latency = c.request("textDocument/completion", query(text), changes=[{
                "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [{"text": text}]}}])
            assert isinstance(items, list) and items
            definitions, definition_ms = c.request("textDocument/definition", query(text))
            notice, ended = c.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                                   and m["params"]["uri"] == uri and
                                   m["params"].get("version") == version)
            resolved = []
            for item in items:
                if item["label"] in {"local_0", "local_1", "this", "tail_field", "get_name"}:
                    result, _ = c.request("completionItem/resolve", item)
                    resolved.append(public(result))
            records.append({"version": version, "queryMs": latency, "definitionMs": definition_ms,
                            "diagnosticMs": (ended-started)/1e6, "completion": public(items),
                            "definition": definitions, "resolved": resolved,
                            "diagnostics": notice["params"]["diagnostics"]})
        status = c.state()
        assert status["active"] == enabled, status.get("decline")
        if enabled:
            assert status["accepted"] == len(records), status
        return records, {k: status[k] for k in ("active", "accepted", "decline")}
    finally:
        c.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--classes", type=int, default=5000)
    parser.add_argument("--blocks", type=int, default=3)
    args = parser.parse_args()
    binary = args.binary.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {"binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "classes": args.classes, "blocks": args.blocks, "rows": [],
              "scope": "Synthetic closed scalar helper; no real-project speedup claim."}
    rng = random.Random(20261022)
    with tempfile.TemporaryDirectory(prefix="body-query-", dir=args.output.parent) as temp:
        root = Path(temp)
        for uvm in (False, True):
            project = root / ("uvm" if uvm else "plain")
            project.mkdir()
            decorate = fixture(project, args.classes, uvm)
            reference = None
            for block in range(args.blocks):
                arms = [False, True]
                rng.shuffle(arms)
                for enabled in arms:
                    records, status = run(binary, project, enabled, uvm, decorate)
                    values = [{k: v for k, v in r.items() if not k.endswith("Ms")} for r in records]
                    if reference is None:
                        reference = copy.deepcopy(values)
                    assert values == reference, (uvm, block, enabled, "native oracle mismatch")
                    report["rows"].append({"uvm": uvm, "block": block, "enabled": enabled,
                                           "status": status, "edits": [
                                               {k: v for k, v in r.items() if k.endswith("Ms") or
                                                k == "version"} for r in records]})
                    args.output.write_text(json.dumps(report, indent=2) + "\n")
    report["summary"] = []
    for uvm in (False, True):
        for enabled in (False, True):
            rows = [r for r in report["rows"] if r["uvm"] == uvm and r["enabled"] == enabled]
            report["summary"].append({"uvm": uvm, "enabled": enabled,
                "firstEditMedianMs": statistics.median(r["edits"][0]["queryMs"] for r in rows),
                "continuousMedianMs": statistics.median(
                    e["queryMs"] for r in rows for e in r["edits"][1:]),
                "diagnosticMedianMs": statistics.median(
                    e["diagnosticMs"] for r in rows for e in r["edits"])})
    report["passed"] = True
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
