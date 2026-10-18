"""One uninstrumented metric for native and method paths; no historical samples.

Predeclare a seed and balanced ABBA/BAAB order in --plan before measuring.
Latency runs from immediately before writing the pre-encoded didChange+query
batch until the matching complete response body arrives. Completion and
binding-changing definition are separate experiments. Ninth-edit fallback is
reported separately from edits 1..8. No parser-count executable is accepted.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
import tempfile
import time

from check import edited
from fixture import change, open_document, position, prepare, query
from protocol import Client, RpcError


OUTCOMES = {"success", "timeout", "cancelled", "crash", "error"}


def validate(record):
    assert record["outcome"] in OUTCOMES
    assert isinstance(record["step"], int) and 1 <= record["step"] <= 9
    assert record["method"] in ("completion", "definition")
    assert record["arm"] in ("native", "method")
    assert (record["latencyMs"] is not None) == (record["outcome"] == "success")
    if record["outcome"] == "success":
        assert record["latencyMs"] >= 0


def run(binary, root, arm, method, plan):
    uri = prepare(root, arm == "method", noise=plan["classes"])
    assert fixture_hashes(root) == plan["fixtureSha256"]
    config_hash = hashlib.sha256((root / ".slang/server.json").read_bytes()).hexdigest()
    assert config_hash == plan["configSha256"][arm]
    cold_start = time.perf_counter_ns()
    client = Client(binary, root)
    rows = []
    try:
        open_document(client, uri)
        assert client.state().get("parserUnits") is None, (
            "counted binaries forbidden for timing"
        )
        cold = (time.perf_counter_ns() - cold_start) / 1e6
        sequence_start = time.perf_counter_ns()
        for step in range(1, 10):
            text = edited(step)
            if step == 9:
                text = text.replace("alpha * bravo", "alpha + bravo")
            row = {
                "arm": arm,
                "method": method,
                "step": step,
                "outcome": "error",
                "latencyMs": None,
            }
            try:
                result, latency = client.request(
                    "textDocument/" + method,
                    query(uri, text, ("local_0" if step % 2 else "local_1") + " +"),
                    [change(uri, step + 1, text)],
                )
                assert result, "empty query response"
                if method == "definition":
                    name = "local_0" if step % 2 else "local_1"
                    at = text.index(name + " =")
                    target = {
                        "start": position(text, at),
                        "end": position(text, at + 7),
                    }
                    assert result == [
                        {
                            "targetUri": uri,
                            "targetRange": target,
                            "targetSelectionRange": target,
                        }
                    ], result
                row.update(outcome="success", latencyMs=latency)
                # Separate idle diagnostic time from interactive response time.
                diagnostic_start = time.perf_counter_ns()
                message, received = client.wait(
                    lambda m: m.get("method") == "textDocument/publishDiagnostics"
                    and m.get("params", {}).get("uri") == uri
                    and m["params"].get("version") == step + 1
                )
                row["diagnosticWaitMs"] = max(0, (received - diagnostic_start) / 1e6)
                row["diagnostics"] = message["params"]["diagnostics"]
                assert not row["diagnostics"]
            except Exception as error:
                import queue

                row["outcome"] = (
                    "timeout"
                    if isinstance(error, queue.Empty)
                    else "cancelled"
                    if isinstance(error, RpcError) and error.code == -32800
                    else "crash"
                    if isinstance(error, (EOFError, BrokenPipeError))
                    else "error"
                )
                row["latencyMs"] = None
                row["error"] = repr(error)
                row["serverReturncode"] = client.process.poll()
                row["stderrTail"] = (root / "server.stderr").read_text(
                    errors="replace"
                )[-4096:]
            validate(row)
            rows.append(row)
            if row["outcome"] != "success":
                break
        sequence_ms = (time.perf_counter_ns() - sequence_start) / 1e6
        return {
            "coldPreparationMs": cold,
            "sequenceMs": sequence_ms,
            "interactiveSumMs": (
                sum(row["latencyMs"] for row in rows)
                if len(rows) == 9 and all(row["outcome"] == "success" for row in rows)
                else None
            ),
            "rows": rows,
        }
    finally:
        client.close()


def summarize(runs):
    summary = {}
    for method in ("completion", "definition"):
        for arm in ("native", "method"):
            selected = [
                row
                for run in runs
                for row in run["rows"]
                if row["method"] == method and row["arm"] == arm
            ]
            summary[method + "/" + arm] = {
                "outcomes": {
                    outcome: sum(row["outcome"] == outcome for row in selected)
                    for outcome in sorted(OUTCOMES)
                },
                "steps": {
                    str(step): describe(values)
                    for step in range(1, 10)
                    for values in [
                        [
                            row["latencyMs"]
                            for row in selected
                            if row["step"] == step and row["outcome"] == "success"
                        ]
                    ]
                },
                "sequences": {
                    metric: describe(
                        [
                            run[metric]
                            for run in runs
                            if run["arm"] == arm
                            and run["method"] == method
                            and run.get(metric) is not None
                        ]
                    )
                    for metric in (
                        "coldPreparationMs",
                        "sequenceMs",
                        "interactiveSumMs",
                    )
                },
            }
    return summary


def describe(values):
    return {
        "n": len(values),
        "medianMs": statistics.median(values) if values else None,
        # An empirical percentile, not a confidence bound. Small experiments
        # retain their samples without presenting a stable tail estimate.
        "empiricalP95Ms": sorted(values)[math.ceil(0.95 * len(values)) - 1]
        if len(values) >= 100
        else None,
    }


def fixture_hashes(root):
    return {
        name: hashlib.sha256((root / name).read_bytes()).hexdigest()
        for name in sorted([p.name for p in root.glob("*.sv*")] + ["design.f"])
    }


def driver_hashes():
    directory = Path(__file__).resolve().parent
    return {
        name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
        for name in ("protocol.py", "fixture.py", "check.py", "benchmark.py")
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20261017)
    parser.add_argument("--blocks", type=int, default=3)
    parser.add_argument("--classes", type=int, default=500)
    parser.add_argument("--plan-only", action="store_true")
    args = parser.parse_args()
    if args.blocks < 1 or args.classes < 1:
        parser.error("--blocks and --classes must be positive")
    binary = args.binary.resolve()
    if args.plan_only:
        if args.plan.exists():
            raise SystemExit("Refusing to overwrite a preregistered plan")
        rng = random.Random(args.seed)
        order = []
        for method in ("completion", "definition"):
            for block in range(args.blocks):
                arms = ["native", "method", "method", "native"]
                if rng.getrandbits(1):
                    arms = ["method", "native", "native", "method"]
                order.extend(
                    {"method": method, "block": block, "arm": arm} for arm in arms
                )
        with tempfile.TemporaryDirectory(prefix="slang-method-plan-") as tmp:
            root = Path(tmp).resolve()
            configs = {}
            for arm in ("native", "method"):
                prepare(root, arm == "method", noise=args.classes)
                configs[arm] = hashlib.sha256(
                    (root / ".slang/server.json").read_bytes()
                ).hexdigest()
            fixture = fixture_hashes(root)
        args.plan.write_text(
            json.dumps(
                {
                    "schema": 2,
                    "classes": args.classes,
                    "workload": f"{args.classes} classes in five headers, one nested include, target class with three methods, two packages and three ordered roots; only target open",
                    "seed": args.seed,
                    "metric": "before pre-encoded batch write to matching complete response-body receipt, before JSON decode",
                    "fixtureSha256": fixture,
                    "configSha256": configs,
                    "driverSha256": driver_hashes(),
                    "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                    "order": order,
                },
                indent=2,
            )
            + "\n"
        )
        return
    plan = json.loads(args.plan.read_text())
    assert plan["driverSha256"] == driver_hashes(), (
        "measurement driver changed after plan"
    )
    assert plan["binarySha256"] == hashlib.sha256(binary.read_bytes()).hexdigest(), (
        "binary changed after plan"
    )
    runs = []
    with tempfile.TemporaryDirectory(prefix="slang-method-timing-") as tmp:
        root = Path(tmp).resolve()
        for entry in plan["order"]:
            result = run(binary, root, entry["arm"], entry["method"], plan)
            result.update(entry)
            runs.append(result)
            args.output.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "plan": plan,
                        "runs": runs,
                        "summary": summarize(runs),
                    },
                    indent=2,
                )
                + "\n"
            )
    assert all(row["outcome"] == "success" for run in runs for row in run["rows"])
    print(json.dumps(summarize(runs), indent=2))


if __name__ == "__main__":
    main()
