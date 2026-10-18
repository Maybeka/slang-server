"""Measure an externally prepared real project without generating design sources.

The scenario pins every .sv/.svh/.f input by relative path and SHA-256. Both arms
use the same workspace and ordinary binary. Completion / definition use fresh
processes; their first queries include didOpen. Edits alternate one exact string
replacement, with versioned diagnostic waits outside request latency. This does
not assume the method overlay accepts the project. --oracle separately compares
ordered completion, all resolves, definition and diagnostics using source-backed
buffer normalization; those runs are never latency evidence.
"""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import time

from benchmark import describe
from check import normalize
from fixture import change, position
from protocol import Client


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalize_project(value, state, uri, text):
    # The test command exposes physical source buffers. Macro expansion handles
    # are deliberately kept exact, never collapsed into a guessed identity.
    # This checks their deterministic native equality, not macro freshness.
    known = {source["buffer"] for source in state["sources"]}

    def visit(item):
        if isinstance(item, dict):
            if {"bufferId", "documentUri", "symbolName"} <= item.keys():
                if item["bufferId"] in known:
                    return normalize(item, state, uri, text)
                return copy.deepcopy(item)
            return {key: visit(child) for key, child in item.items()}
        if isinstance(item, list):
            return [visit(child) for child in item]
        return item

    return visit(value)


def run(binary, root, scenario, enabled, method, oracle=False):
    config_path = root / ".slang/server.json"
    config = json.loads(config_path.read_text())
    config["experimentalMethodScopeOverlay"] = enabled
    config_path.write_text(json.dumps(config))
    target = root / scenario["target"]
    original = target.read_text()
    assert original.count(scenario["before"]) == 1
    uri = target.as_uri()
    start = time.perf_counter_ns()
    client = Client(binary, root)
    rows = []
    try:
        sequence_start = time.perf_counter_ns()
        for step in range(10):
            text = (
                original.replace(scenario["before"], scenario["after"])
                if step % 2
                else original
            )
            params = {
                "textDocument": {"uri": uri},
                "position": position(
                    text, text.index(scenario["query"]) + scenario["queryDelta"]
                ),
            }
            notifications = (
                [
                    {
                        "method": "textDocument/didOpen",
                        "params": {
                            "textDocument": {
                                "uri": uri,
                                "version": 1,
                                "languageId": "systemverilog",
                                "text": text,
                            }
                        },
                    }
                ]
                if step == 0
                else [change(uri, step + 1, text)]
            )
            result, elapsed = client.request(
                "textDocument/" + method, params, notifications
            )
            assert result, (method, step, "empty response")
            first_ready = (time.perf_counter_ns() - start) / 1e6
            message, _ = client.wait(
                lambda m: m.get("method") == "textDocument/publishDiagnostics"
                and m["params"].get("uri") == uri
                and m["params"].get("version") == step + 1
            )
            row = {
                "step": step,
                "latencyMs": elapsed,
                "diagnostics": message["params"]["diagnostics"],
            }
            if step == 0:
                row["startupThroughFirstResponseMs"] = first_ready
            if oracle:
                state = client.state()
                row["result"] = normalize_project(result, state, uri, text)
                if method == "completion":
                    row["resolved"] = [
                        normalize_project(
                            client.request("completionItem/resolve", item)[0],
                            state,
                            uri,
                            text,
                        )
                        for item in result
                    ]
            rows.append(row)
        sequence_ms = (time.perf_counter_ns() - sequence_start) / 1e6
        state = client.state()
        assert state.get("parserUnits") is None, "use an ordinary binary"
        return {
            "enabled": enabled,
            "method": method,
            "sequenceWithDiagnosticsMs": sequence_ms,
            "editResponseSumMs": sum(row["latencyMs"] for row in rows[1:]),
            "status": {key: state[key] for key in ("active", "accepted", "decline")},
            "rows": rows,
        }
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--scenario", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--blocks", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20261019)
    parser.add_argument("--oracle", action="store_true")
    args = parser.parse_args()
    assert args.blocks > 0
    assert not args.output.exists(), "preserve previous evidence"
    root = args.workspace.resolve()
    scenario = json.loads(args.scenario.read_text())
    for name, expected in scenario["files"].items():
        assert digest(root / name) == expected, name
    config_path = root / ".slang/server.json"
    config_bytes = config_path.read_bytes()
    randomizer = random.Random(args.seed)
    schedule = []
    for method in ("completion", "definition"):
        for block in range(args.blocks):
            order = [False, True, True, False]
            if randomizer.randrange(2):
                order = [True, False, False, True]
            schedule.extend((method, block, enabled) for enabled in order)
    report = {
        "schema": 1,
        "binarySha256": digest(args.binary.resolve()),
        "driverSha256": digest(Path(__file__)),
        "helperSha256": {
            name: digest(Path(__file__).parent / name)
            for name in ("benchmark.py", "check.py", "fixture.py", "protocol.py")
        },
        "scenario": scenario,
        "config": json.loads(config_bytes),
        "seed": args.seed,
        "oracle": args.oracle,
        "schedule": schedule,
        "runs": [],
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    try:
        for method, block, enabled in schedule:
            for name, expected in scenario["files"].items():
                assert digest(root / name) == expected, name
            result = run(
                args.binary.resolve(), root, scenario, enabled, method, args.oracle
            )
            result["block"] = block
            report["runs"].append(result)
            args.output.write_text(json.dumps(report, indent=2) + "\n")
        if args.oracle:
            for method in ("completion", "definition"):
                values = []
                for run_result in report["runs"]:
                    if run_result["method"] == method:
                        values.append(
                            [
                                {
                                    k: row[k]
                                    for k in ("result", "diagnostics", "resolved")
                                    if k in row
                                }
                                for row in run_result["rows"]
                            ]
                        )
                assert all(value == values[0] for value in values), method
            report["oracleEqual"] = True
        else:
            report["summary"] = {
                f"{method}/{enabled}": {
                    "steps": {
                        str(step): describe(
                            [r["rows"][step]["latencyMs"] for r in selected]
                        )
                        for step in range(10)
                    },
                    **{
                        metric: describe([r[metric] for r in selected])
                        for metric in ("sequenceWithDiagnosticsMs", "editResponseSumMs")
                    },
                }
                for method in ("completion", "definition")
                for enabled in (False, True)
                for selected in [
                    [
                        r
                        for r in report["runs"]
                        if r["method"] == method and r["enabled"] == enabled
                    ]
                ]
            }
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    finally:
        config_path.write_bytes(config_bytes)


if __name__ == "__main__":
    main()
