# SPDX-License-Identifier: MIT
"""Serial, version-matched editor latency and kernel memory qualification.

Run only after independent correctness/build jobs end for accepted timing.
--readiness labels all times non-qualifying and permits harness checks meanwhile.
Original project files/configurations are never written. Changes exist only in LSP.
"""

from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import statistics
import sys
import time
import traceback

task = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(task / "tests/method_scope"))
from protocol import Client  # noqa: E402 — shared test client lives in the sibling directory
from helpers import position, range_change, sample  # noqa: E402

TRACE_KEYS = (
    "SLANG_SCOPE_BODY_TRACE",
    "SLANG_UVM_PHASE_TRACE",
    "SLANG_BODY_PHASE_TRACE",
    "SLANG_METHOD_SCOPE_TRACE",
    "SLANG_SERVER_PHASE_TRACE",
)
for key in TRACE_KEYS:
    os.environ.pop(key, None)


def digest(value):
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


class TimedClient(Client):
    def __init__(self, binary, root):
        self.root = root
        self.launchNs = time.perf_counter_ns()
        super().__init__(binary, root)
        self.initializationMs = (time.perf_counter_ns() - self.launchNs) / 1e6

    def request(self, method, params, changes=()):
        if hasattr(self, "process"):
            marker = {
                "pid": self.process.pid,
                "parentPid": os.getpid(),
                "phase": method,
                "params": params,
                "memory": sample(self.process.pid),
            }
            path = self.root / "live-server.json"
            tmp = path.with_suffix(".json.tmp")
            tmp.write_text(json.dumps(marker) + "\n")
            tmp.replace(path)
        return super().request(method, params, changes)

    def send(self, messages):
        stamp = super().send(messages)
        if any(m.get("method") == "textDocument/hover" for m in messages):
            self.editQueryStartNs = stamp
        return stamp

    def wait(self, predicate, timeout=900):
        return super().wait(
            predicate, 30 if getattr(self, "closing", False) else timeout
        )

    def command(self, name, *args):
        return self.request(
            "workspace/executeCommand", {"command": name, "arguments": list(args)}
        )[0]

    def close(self):
        self.closing = True
        try:
            self.request("shutdown", {})
            self.notify("exit", {})
            self.process.wait(timeout=30)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.stderr.close()
            (self.root / "live-server.json").unlink(missing_ok=True)


def prepare(case, scenario, root):
    root.mkdir(parents=True, exist_ok=True)
    (root / ".slang").mkdir(exist_ok=True)
    if scenario == "background":
        text = (
            "module freeze_audit_consumer;\nfunction automatic int audit_value(input int x);\n"
            "return x + 1;\nendfunction\nendmodule\n"
        )
        target = root / "mutable_consumer.sv"
        target.write_text(text)
        recipe = {
            "anchor": "return x + 1;",
            "alternate": "return x + 2;",
            "query": "x",
            "hoverAnchor": "input int x",
            "freeze": {"instances": [case["dut"]]},
        }
        if case["id"] == "opentitan-aes-uvm":
            recipe["freeze"]["modules"] = ["fi_ghash_if"]
        filelist = (
            "-F " + str(Path(case["root"]) / "design.f") + "\n" + str(target) + "\n"
        )
    else:
        recipe = json.loads(json.dumps(case))
        target = Path(recipe["target"])
        text = target.read_text()
        filelist = "-F " + str(Path(case["root"]) / "design.f") + "\n"
    if case.get("config", {}).get("uvm") or case["id"] in (
        "spi-block",
        "spi-pss",
        "official-ubus",
    ):
        recipe["freeze"].setdefault("packages", [])
        if "uvm_pkg" not in recipe["freeze"]["packages"]:
            recipe["freeze"]["packages"].append("uvm_pkg")
    recipe["freeze"].pop("maxMiB", None)
    (root / "design.f").write_text(filelist)
    assert recipe["anchor"] in text
    return target, text, recipe


def sequence(text, recipe, body_edits=6):
    before, after = recipe["anchor"], recipe["alternate"]
    changed = text.replace(before, after, 1)
    newline = text.replace(before, "\n" + after, 1)
    number = re.search(r"\b\d+'[sS]?[bodhBODH][0-9a-fA-F_xXzZ?]+|\b\d+\b", before)
    if number:
        bad = before[: number.start()] + "scope_perf_missing" + before[number.end() :]
    elif " = " in before:
        bad = before.replace(" = ", " = scope_perf_missing + ", 1)
    else:
        word = "m_cfg" if "m_cfg" in before else "nbits"
        assert word in before
        bad = before.replace(word, "scope_perf_missing", 1)
    error = text.replace(before, bad, 1)
    end = recipe.get("structuralEnd")
    index = (
        text.rfind(end)
        if end
        else max(text.rfind("endmodule"), text.rfind("endpackage"))
    )
    assert index >= 0
    structural = (
        text[:index] + "localparam integer scope_perf_extra = 1;\n" + text[index:]
    )
    states = [("initial", text)]
    states += [
        ("body-" + str(i), changed if i % 2 else text) for i in range(1, body_edits + 1)
    ]
    states += [
        ("newline", newline),
        ("error", error),
        ("repair", text),
        ("structural", structural),
        ("structural-repair", text),
        ("close-reopen", text),
        ("manual-rebuild", text),
        ("post-rebuild", changed),
    ]
    return states


def config_for(case, root, recipe, scenario, arm):
    cfg = {
        **case["config"],
        "compilationFilelist": str(root / "design.f"),
        "indexingThreads": 1,
        "semanticCacheUnlimited": True,
        "frozenSources": recipe["freeze"],
        "experimentalCooperativeAnalysis": True,
        "experimentalScopeSemanticCache": arm != "native",
        "experimentalUvmSemanticCache": arm != "native",
        "experimentalSyntaxPrefixCache": arm != "native",
        "experimentalParallelDiagnostics": False,
        "experimentalFrozenSources": False,
        "experimentalMethodScopeOverlay": False,
        "experimentalDiagnosticDeltaPublishing": False,
        "experimentalQueryDiagnosticTransactions": False,
    }
    if scenario == "background":
        cfg["flags"] += " --top freeze_audit_consumer"
    for k in (
        "experimentalMethodBodyQueries",
        "experimentalMethodBodyDiagnostics",
        "experimentalMethodBodyCertificateRenewal",
        "experimentalMethodBodyCertification",
        "experimentalMethodBodyMacroProof",
    ):
        cfg[k] = arm == "combined"
    return cfg


ap = argparse.ArgumentParser()
ap.add_argument("--case", required=True)
ap.add_argument("--manifest", type=Path, required=True)
ap.add_argument("--output-root", type=Path, required=True)
ap.add_argument("--scenario", choices=["background", "real-edit"], required=True)
ap.add_argument("--label", required=True)
ap.add_argument("--binary", type=Path, required=True)
ap.add_argument("--repeats", type=int, default=3)
ap.add_argument("--arms", default="native,freeze,combined")
ap.add_argument("--readiness", action="store_true")
ap.add_argument(
    "--allow-native-fallback",
    action="store_true",
    help="Check exact public results for unsupported freeze inputs; report fallback as fallback, never reuse",
)
ap.add_argument("--state-limit", type=int, default=0)
ap.add_argument(
    "--long-combined-edits",
    type=int,
    default=0,
    help="Memory/lifetime run: extend only combined edits; timing is not formal performance qualification",
)
args = ap.parse_args()
stage = args.output_root.resolve()
stage.mkdir(parents=True, exist_ok=True)
assert Path(args.label).name == args.label and args.label not in ("", ".", "..")
assert args.repeats > 0
assert args.long_combined_edits == 0 or (
    args.long_combined_edits > 6 and args.repeats == 1
)
assert not args.long_combined_edits or not args.state_limit
assert (args.state_limit == 0 or args.state_limit >= 2) and (
    args.state_limit == 0 or args.readiness
)
assert args.readiness or not os.environ.get("SLANG_METHOD_INPUT_TRACE")
arms = args.arms.split(",")
assert arms[0] == "native" and set(arms) <= {"native", "freeze", "combined"}
assert not args.long_combined_edits or "combined" in arms
case = next(c for c in json.loads(args.manifest.read_text()) if c["id"] == args.case)
root = stage / ("performance-" + args.label) / args.scenario / args.case
outpath = stage / (args.case + "-" + args.scenario + "-" + args.label + ".json")
assert not outpath.exists(), outpath
target, original, recipe = prepare(case, args.scenario, root)
uri = target.as_uri()
binary = args.binary.resolve()
report = {
    "case": args.case,
    "scenario": args.scenario,
    "target": str(target),
    "recipe": recipe,
    "sourceSha256": hashlib.sha256(original.encode()).hexdigest(),
    "sourceFileSha256": hashlib.sha256(target.read_bytes()).hexdigest(),
    "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "performanceQualified": False,
    "status": "running",
    "rows": [],
    "allowsNativeFallback": args.allow_native_fallback,
    "measurement": {
        "queryMs": "Batched didChange/hover dispatch to full hover response receipt before JSON decoding",
        "diagnosticMs": "Same dispatch to receipt of matching edited-document version diagnostics; not global completion",
        "processCold": "New process; OS file cache/project UVM parse artifacts may already be warm",
        "memory": "Kernel boundary RSS/current physical footprint/lifetime maximum footprint, not peak RSS",
        "cadence": "Serial awaited edits; no burst/tail-frequency claim",
        "comparison": "Identical cooperative scheduling and index. freeze includes scope/UVM semantic and syntax prefix; combined adds body query/diagnostic certificates.",
        "readiness": args.readiness,
    },
    "allSemanticBudgetsUnlimited": True,
}
report["inputManifests"] = {
    str(path): {
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "contents": path.read_text(),
    }
    for path in (root / "design.f", Path(case["root"]) / "design.f")
}
report["measurement"]["longCombinedEdits"] = args.long_combined_edits
report["memoryStressQualified"] = False
reference = {}
referenceValues = {}
client = None
try:
    for repeat in range(args.repeats):
        order = arms if repeat % 2 == 0 else list(reversed(arms))
        for arm in order:
            cfg = config_for(case, root, recipe, args.scenario, arm)
            (root / ".slang/server.json").write_text(json.dumps(cfg) + "\n")
            row = {"repeat": repeat, "arm": arm, "configuration": cfg, "states": []}
            report["rows"].append(row)
            started = time.perf_counter_ns()
            client = TimedClient(binary, root)
            row["initializationMs"] = client.initializationMs
            _, row["projectBuildMs"] = client.request(
                "workspace/executeCommand",
                {
                    "command": "slang.setBuildFile",
                    "arguments": [str(root / "design.f")],
                },
            )
            selected, row["activeInstanceMs"] = client.request(
                "workspace/executeCommand",
                {"command": "slang.setActiveInstance", "arguments": [case["top"]]},
            )
            assert selected
            row["initBuildSelectMs"] = (time.perf_counter_ns() - started) / 1e6
            latest = {}
            last = original
            previousLocalUpdates = 0
            extended = args.long_combined_edits if arm == "combined" else 0
            states = sequence(original, recipe, extended or 6)
            row["scheduledStates"] = len(states)
            row["stressSamples"] = []
            if args.state_limit:
                states = states[: args.state_limit]
            for version, (name, text) in enumerate(states, 1):
                if name == "close-reopen":
                    client.notify(
                        "textDocument/didClose", {"textDocument": {"uri": uri}}
                    )
                rebuildMs = None
                rebuildStartNs = None
                if name == "manual-rebuild":
                    tick = time.perf_counter_ns()
                    rebuildStartNs = tick
                    assert client.command("slang.rebuildScopeLibrary")
                    rebuildMs = (time.perf_counter_ns() - tick) / 1e6
                    assert client.command("slang.setActiveInstance", case["top"])
                change = (
                    {
                        "method": "textDocument/didOpen",
                        "params": {
                            "textDocument": {
                                "uri": uri,
                                "languageId": "systemverilog",
                                "version": version,
                                "text": text,
                            }
                        },
                    }
                    if version == 1 or name == "close-reopen"
                    else range_change(uri, version, last, text, "utf-16")
                )
                marker = recipe.get("hoverAnchor", recipe["anchor"])
                offset = (
                    text.index(marker)
                    + marker.index(recipe["query"])
                    + max(0, len(recipe["query"]) // 2)
                )
                hover, queryMs = client.request(
                    "textDocument/hover",
                    {"textDocument": {"uri": uri}, "position": position(text, offset)},
                    changes=[change],
                )
                notice, stamp = client.wait(
                    lambda m: m.get("method") == "textDocument/publishDiagnostics"
                    and m["params"]["uri"] == uri
                    and m["params"].get("version") == version
                )
                if name == "initial":
                    row["processLaunchThroughInitialResponseMs"] = (
                        client.editQueryStartNs - client.launchNs
                    ) / 1e6 + queryMs
                memory = sample(client.process.pid)
                assert memory["available"]
                # The priority edited-document notification can precede sibling
                # publications in the same complete batch. Measure it immediately,
                # then use an ordered response as the correctness collection barrier.
                scope = client.command("slang.scopeLibraryStatus")
                method = client.state(compact=True)
                report["lastObservedState"] = {
                    "name": name,
                    "arm": arm,
                    "repeat": repeat,
                    "scope": scope,
                }
                mechanismStorage = (
                    client.command("slang.sourceStorageStatus")
                    if args.readiness and os.environ.get("SLANG_METHOD_INPUT_TRACE")
                    else None
                )
                for message, _ in client.pending:
                    if message.get("method") == "textDocument/publishDiagnostics":
                        p = message["params"]
                        latest[p["uri"]] = p["diagnostics"]
                client.pending = []
                latest[uri] = notice["params"]["diagnostics"]
                diagnostics = {
                    u: sorted(ds, key=lambda d: json.dumps(d, sort_keys=True))
                    for u, ds in sorted(latest.items())
                    if ds
                }
                value = {"hover": hover, "diagnostics": diagnostics}
                sha = digest(value)
                key = name
                stress = bool(
                    extended and name.startswith("body-") and int(name[5:]) > 6
                )
                if stress:
                    # The exact same two texts have each been independently
                    # analyzed three times in the Native arm. Require repeatable
                    # full public results before using their oracle repeatedly.
                    assert (
                        reference["body-1"]
                        == reference["body-3"]
                        == reference["body-5"]
                    )
                    assert (
                        reference["body-2"]
                        == reference["body-4"]
                        == reference["body-6"]
                    )
                    key = "body-5" if int(name[5:]) % 2 else "body-6"
                if arm == "native" and key not in reference:
                    reference[key] = sha
                    referenceValues[key] = value
                assert hover, (name, "empty hover")
                if sha != reference[key]:
                    import collections

                    expected = referenceValues[key]
                    difference = {
                        "state": name,
                        "arm": arm,
                        "hoverEqual": hover == expected["hover"],
                        "diagnostics": {},
                    }
                    if hover != expected["hover"]:
                        difference["hover"] = {
                            "expected": expected["hover"],
                            "actual": hover,
                        }
                    for path in sorted(set(diagnostics) | set(expected["diagnostics"])):
                        left = collections.Counter(
                            json.dumps(d, sort_keys=True)
                            for d in expected["diagnostics"].get(path, [])
                        )
                        right = collections.Counter(
                            json.dumps(d, sort_keys=True)
                            for d in diagnostics.get(path, [])
                        )
                        if left != right:
                            difference["diagnostics"][path] = {
                                "missing": [
                                    json.loads(t) for t in (left - right).elements()
                                ],
                                "unexpected": [
                                    json.loads(t) for t in (right - left).elements()
                                ],
                            }
                    report["publicMismatch"] = difference
                assert sha == reference[key], (
                    name,
                    arm,
                    "public hover/diagnostic mismatch",
                )
                missing = any(
                    "scope_perf_missing" in d.get("message", "")
                    and d.get("severity") == 1
                    for ds in diagnostics.values()
                    for d in ds
                )
                assert missing == (name == "error"), (name, "error/repair not observed")
                fixedReuseEvidence = "native" if arm == "native" else "prepared"
                if arm != "native":
                    bodies = scope["bodies"]
                    assert scope["budgetUnlimited"]
                    bodyActive = bool(
                        bodies["retainedRoots"]
                        or bodies["owners"]
                        or bodies["publications"]
                    )
                    assert not bodyActive or bodies["budgetUnlimited"]
                    assert bodies["budgetBytes"] == 0 and bodies["capacitySkips"] == 0
                    if not scope["prepared"]:
                        assert args.allow_native_fallback, (
                            name,
                            "fixed input preparation failed",
                            scope["reason"],
                        )
                        fixedReuseEvidence = "native-fallback:preparation-declined"
                    elif name not in ("initial", "manual-rebuild"):
                        # Prove actual import or a new certified transaction on
                        # the retained original graph, not mere cache preparation.
                        originalLocalReuse = (
                            arm == "combined"
                            and method["localDiagnosticUpdates"] > previousLocalUpdates
                            and bodies["retainedRoots"]
                            and bodies["publications"] > 0
                        )
                        imported = bool(
                            scope["lastModelSharedRoots"]
                            or (
                                scope["packages"]
                                and scope["lastModelLibraryImport"]["status"]
                                == "imported"
                            )
                        )
                        if not (originalLocalReuse or imported):
                            assert args.allow_native_fallback, (
                                name,
                                "no actual fixed semantic reuse",
                                scope,
                            )
                            fixedReuseEvidence = "native-fallback:" + (
                                scope["lastModelLibraryImport"].get("reason")
                                or scope["lastModelLibraryImport"].get(
                                    "failureCategory"
                                )
                                or scope["lastModelLibraryImport"]["status"]
                            )
                        else:
                            fixedReuseEvidence = (
                                "body-import"
                                if scope["lastModelSharedRoots"]
                                else "original-model-local-update"
                                if originalLocalReuse
                                else "package-import"
                            )
                previousLocalUpdates = method["localDiagnosticUpdates"]
                record = {
                    "name": name,
                    "version": version,
                    "queryMs": queryMs,
                    "diagnosticMs": (stamp - client.editQueryStartNs) / 1e6,
                    "memory": memory,
                    "equalNative": True,
                    "publicSha256": sha,
                    "diagnosticFiles": len(diagnostics),
                    "fixedReuseEvidence": fixedReuseEvidence,
                    "diagnosticSummary": {
                        "errors": sum(
                            d.get("severity") == 1
                            for ds in diagnostics.values()
                            for d in ds
                        ),
                        "warnings": sum(
                            d.get("severity") == 2
                            for ds in diagnostics.values()
                            for d in ds
                        ),
                    },
                    "scope": {
                        "prepared": scope["prepared"],
                        "generation": scope["generation"],
                        "packages": scope["packages"],
                        "actualRoots": scope["lastModelSharedRoots"],
                        "lastModelCurrent": scope["lastModelCurrent"],
                        "libraryImport": scope["lastModelLibraryImport"],
                        "bodies": scope["bodies"],
                    },
                    "local": method,
                    "mechanismSourceStorage": mechanismStorage,
                    "manualRebuildMs": rebuildMs,
                }
                if rebuildStartNs is not None:
                    record["manualRebuildThroughResponseMs"] = (
                        client.editQueryStartNs - rebuildStartNs
                    ) / 1e6 + queryMs
                    record["manualRebuildThroughEditedDiagnosticMs"] = (
                        stamp - rebuildStartNs
                    ) / 1e6
                if stress:
                    row["stressSamples"].append(
                        {
                            k: record[k]
                            for k in (
                                "name",
                                "version",
                                "queryMs",
                                "diagnosticMs",
                                "memory",
                                "equalNative",
                                "publicSha256",
                            )
                        }
                        | {
                            "scopeGeneration": scope["generation"],
                            "actualRootCount": len(scope["lastModelSharedRoots"]),
                            "scopeBudgetUnlimited": scope["budgetUnlimited"],
                            "scopeCapacitySkips": scope["bodies"]["capacitySkips"],
                            "sourceMemory": method["sourceMemory"],
                            "retainedHandles": method["retainedHandles"],
                            "localDiagnosticUpdates": method["localDiagnosticUpdates"],
                            "localDiagnosticDecline": method["localDiagnosticDecline"],
                        }
                    )
                else:
                    row["states"].append(record)
                last = text
                if not stress or len(row["stressSamples"]) % 64 == 0:
                    outpath.write_text(json.dumps(report, indent=2) + "\n")
            row["sourceStorage"] = client.command("slang.sourceStorageStatus")
            row["uvm"] = client.command("slang.uvmLibraryStatus")
            client.close()
            row["exitCode"] = client.process.returncode
            assert row["exitCode"] == 0
            client = None
            raw = (root / "server.stderr").read_bytes()
            row["stderr"] = {
                "sha256": hashlib.sha256(raw).hexdigest(),
                "bytes": len(raw),
            }
            assert not any(
                token in raw
                for token in (
                    b"ERROR: AddressSanitizer",
                    b"AddressSanitizer:DEADLYSIGNAL",
                )
            )
            (root / "server.stderr").unlink()
            for metric in ("queryMs", "diagnosticMs"):
                row[metric + "WarmMedian"] = statistics.median(
                    t[metric] for t in row["states"] if t["name"].startswith("body-")
                )
            if extended:
                assert len(row["stressSamples"]) == extended - 6
                samples = row["stressSamples"]
                row["stressSummary"] = {
                    "edits": extended,
                    "extendedSamples": len(samples),
                    "allEqualNative": all(s["equalNative"] for s in samples),
                    "scopeGenerations": sorted({s["scopeGeneration"] for s in samples}),
                    "retainedHandlesRange": [
                        min(s["retainedHandles"] for s in samples),
                        max(s["retainedHandles"] for s in samples),
                    ],
                }
                for metric in (
                    "rssBytes",
                    "physicalFootprintBytes",
                    "lifetimePeakPhysicalFootprintBytes",
                ):
                    values = [s["memory"][metric] for s in samples]
                    row["stressSummary"][metric] = {
                        "first": values[0],
                        "last": values[-1],
                        "min": min(values),
                        "max": max(values),
                        "first64Median": statistics.median(values[:64]),
                        "last64Median": statistics.median(values[-64:]),
                    }
            print(
                args.case,
                args.scenario,
                repeat,
                arm,
                "warm",
                round(row["queryMsWarmMedian"], 2),
                round(row["diagnosticMsWarmMedian"], 2),
                flush=True,
            )
    report.pop("lastObservedState", None)
    report["status"] = "completed"
    report["performanceQualified"] = not args.readiness and not args.long_combined_edits
    report["memoryStressQualified"] = not args.readiness and bool(
        args.long_combined_edits
    )
except BaseException as exc:
    report.update(status="failed", failure=repr(exc), traceback=traceback.format_exc())
    report["performanceQualified"] = False
finally:
    if client:
        try:
            client.close()
        except BaseException as exc:
            report["closeFailure"] = repr(exc)
        report["failureExitCode"] = client.process.returncode
    log = root / "server.stderr"
    if log.exists():
        raw = log.read_bytes()
        errors = [
            line
            for line in raw.decode(errors="replace").splitlines()
            if any(
                token in line
                for token in (
                    "ERROR: AddressSanitizer",
                    "SUMMARY: AddressSanitizer",
                    "AddressSanitizer:DEADLYSIGNAL",
                )
            )
        ]
        frames = (
            [
                line.strip()
                for line in raw.decode(errors="replace").splitlines()
                if line.lstrip().startswith("#")
            ][:32]
            if errors
            else []
        )
        report["terminalStderr"] = {
            "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "sanitizerErrors": errors[:8],
            "sanitizerFrames": frames,
        }
        log.unlink()
    assert target.read_text() == original
    outpath.write_text(json.dumps(report, indent=2) + "\n")
if report["status"] != "completed":
    print(report["failure"], flush=True)
    raise SystemExit(1)
