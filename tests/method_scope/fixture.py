import json
from pathlib import Path

HEADER = """class Accumulator;
  int marker;
  function int earlier(input int first, input int other);
    int early_0 = first + other;
    int early_1 = early_0 * other;
    return (early_1 + other);
  endfunction
  function int combine(input int alpha, input int bravo, input int charl);
    int local_0 = alpha + bravo;
    int local_1 = local_0 * charl;
    return (local_1 + charl);
  endfunction
  int tail_field;
  function int later(input int omega, input int extra);
    int later_0 = omega + extra;
    int later_1 = later_0 * extra;
    return (later_1 + extra);
  endfunction
endclass
"""


def prepare(root: Path, enabled: bool, noise=500, header=HEADER):
    root.mkdir(parents=True, exist_ok=True)
    (root / ".slang").mkdir(exist_ok=True)
    (root / ".slang/server.json").write_text(
        json.dumps(
            {
                "indexingThreads": 1,
                "flags": "--single-unit --threads 1",
                "compilationFilelist": "design.f",
                "experimentalMethodScopeOverlay": enabled,
            }
        )
    )
    (root / "design.f").write_text("+incdir+.\nprelude.sv\nowner.sv\ntail.sv\n")
    (root / "prelude.sv").write_text(
        "package prelude; class Prelude; int value; endclass endpackage\n"
    )
    # Five independent class headers plus one nested include exercise a complete
    # multi-header package, rather than inflating a package with scalar fields.
    for chunk in range(5):
        (root / f"classes_{chunk}.svh").write_text(
            "".join(
                f"class Noise_{i}; int field; function int value(input int arg); "
                "return arg; endfunction endclass\n"
                for i in range(chunk, noise, 5)
            )
        )
    (root / "nested.svh").write_text('`include "classes_2.svh"\n')
    (root / "owner.sv").write_text(
        'package owner;\n`include "classes_0.svh"\n`include "classes_1.svh"\n'
        '`include "nested.svh"\n`include "item.svh"\n'
        '`include "classes_3.svh"\n`include "classes_4.svh"\nendpackage\n'
    )
    (root / "tail.sv").write_text("module tail; int value; endmodule\n")
    (root / "item.svh").write_text(header)
    return (root / "item.svh").as_uri()


def position(text, offset):
    return {
        "line": text.count("\n", 0, offset),
        "character": offset - text.rfind("\n", 0, offset) - 1,
    }


def query(uri, text, needle="local_1 +", delta=2):
    return {
        "textDocument": {"uri": uri},
        "position": position(text, text.index(needle) + delta),
    }


def change(uri, version, text):
    return {
        "method": "textDocument/didChange",
        "params": {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"text": text}],
        },
    }


def open_document(client, uri, text=HEADER):
    client.notify(
        "textDocument/didOpen",
        {
            "textDocument": {
                "uri": uri,
                "languageId": "systemverilog",
                "version": 1,
                "text": text,
            }
        },
    )
    client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    client.request("textDocument/completion", query(uri, text))
    client.request("textDocument/definition", query(uri, text))
    client.wait(
        lambda m: m.get("method") == "textDocument/publishDiagnostics"
        and m.get("params", {}).get("uri") == uri
        and m["params"].get("version") == 1
    )
