"""Hold old private diagnostics while the method overlay serves newer edits."""

import argparse
import json
from pathlib import Path
import tempfile
import time

from check import edited
from fixture import HEADER, change, prepare, query
from protocol import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        required=True,
        help="diagnostic_test_server, never the timing executable",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="slang-method-held-") as tmp:
        root = Path(tmp).resolve()
        uri = prepare(root, True)
        gate = root / "diagnostic-gate"
        client = Client(
            args.binary.resolve(), root, {"SLANG_DIAGNOSTIC_TEST_GATE": str(gate)}
        )
        try:
            client.notify(
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": HEADER,
                    }
                },
            )
            client.request(
                "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
            )
            initial = client.request("textDocument/completion", query(uri, HEADER))[0]
            old = next(item for item in initial if item["label"] == "local_1")
            deadline = time.monotonic() + 10
            while not Path(str(gate) + ".ready").exists():
                assert time.monotonic() < deadline, (
                    "old private diagnostic job did not reach gate"
                )
                time.sleep(0.005)
            observations = []
            for step in (1, 2):
                text = edited(step)
                params = query(uri, text, ("local_0" if step % 2 else "local_1") + " +")
                links = client.request(
                    "textDocument/definition", params, [change(uri, step + 1, text)]
                )[0]
                assert links and links[0]["targetUri"] == uri
                state = client.state()
                assert (
                    state["active"]
                    and state["accepted"] == step
                    and state["version"] == step + 1
                ), state
                observations.append(
                    {"version": step + 1, "active": True, "definition": links}
                )
            stale = client.request("completionItem/resolve", old)[0]
            assert "documentation" not in stale
            Path(str(gate) + ".release").touch()
            message, received = client.wait(
                lambda m: m.get("method") == "textDocument/publishDiagnostics"
                and m.get("params", {}).get("uri") == uri
                and m["params"].get("version") == 3
            )
            assert message["params"]["diagnostics"] == []
            older = [
                m
                for m, stamp in client.pending
                if m.get("method") == "textDocument/publishDiagnostics"
                and m.get("params", {}).get("uri") == uri
                and m["params"].get("version") in (1, 2)
            ]
            assert not older, older
            args.output.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "heldGeneration": 1,
                        "foreground": observations,
                        "publishedVersion": 3,
                        "staleAutomaticHandleUnresolved": True,
                        "obsoletePublications": older,
                    },
                    indent=2,
                )
                + "\n"
            )
        finally:
            Path(str(gate) + ".release").touch()
            client.close()
    print(
        "PASS: held old private job, two active overlay edits, latest diagnostics only, old handle rejected"
    )


if __name__ == "__main__":
    main()
