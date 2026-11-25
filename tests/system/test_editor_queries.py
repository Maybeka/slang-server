# SPDX-License-Identifier: MIT
"""Editor-visible contracts that must hold across compiler and server upgrades."""

import json
from contextlib import contextmanager
from pathlib import Path

import pytest

from tests.method_scope.protocol import Client
from tests.method_scope.fixture import position


@pytest.fixture
def project(tmp_path, my_options):
    root = tmp_path.resolve()
    binary = Path(my_options.binary).resolve()

    @contextmanager
    def start(files, config=None):
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        if config is not None:
            (root / ".slang").mkdir()
            (root / ".slang/server.json").write_text(json.dumps(config))
        client = Client(binary, root)
        try:
            yield client, root
        finally:
            client.close()

    return start


def open_file(client, path, text):
    doc = {"uri": path.as_uri()}
    client.notify("textDocument/didOpen", {"textDocument": {
        **doc, "languageId": "systemverilog", "version": 1, "text": text,
    }})
    return doc


def at(doc, text, marker):
    return {"textDocument": doc, "position": position(text, text.index(marker) + len(marker))}


def items(client, doc, text, marker, trigger=None):
    params = at(doc, text, marker)
    if trigger:
        params["context"] = {"triggerKind": 2, "triggerCharacter": trigger}
    result, _ = client.request("textDocument/completion", params)
    return result["items"] if isinstance(result, dict) else result or []


def test_source_events_during_invalid_config(project):
    with project({"top.sv": "module top; endmodule", "design.f": "top.sv\n"},
                 {"build": "design.f", "indexingThreads": 1}) as (client, root):
        def watch(*events):
            client.notify("workspace/didChangeWatchedFiles", {"changes": [
                {"uri": p.as_uri(), "type": kind} for p, kind in events
            ]})

        def symbols(name):
            results, _ = client.request("workspace/symbol", {"query": name})
            return [s["name"] for s in results]

        assert "top" in symbols("top")
        config = root / ".slang/server.json"
        created = root / "extra.sv"
        config.write_text("{")
        created.write_text("module recent; endmodule")
        watch((config, 2), (created, 1))
        assert "recent" in symbols("recent")
        created.write_text("module renamed; endmodule")
        watch((created, 2))
        assert "renamed" in symbols("renamed")
        assert not symbols("recent")
        created.unlink()
        watch((created, 3))
        assert not symbols("renamed")
        assert "top" in symbols("top")
        # A pending reload also has to protect an unsaved editor buffer.
        path = root / "top.sv"
        overlay = "module top; int unsaved; initial unsaved = 1; endmodule"
        doc = open_file(client, path, overlay)
        path.write_text("module top; int disk_only; endmodule")
        watch((path, 2))
        links, _ = client.request("textDocument/definition", at(doc, overlay, "initial uns"))
        assert links[0]["targetSelectionRange"]["start"] == position(overlay, overlay.index("unsaved"))
        config.write_text(json.dumps({"build": "design.f", "indexingThreads": 1}))
        watch((config, 2))
        assert "top" in symbols("top")


def test_package_exports_in_lexical_and_qualified_completion(project):
    text = """package origin;
  typedef enum { OFF, ON } state_t;
  parameter int CONFIG = 3, HIDDEN = 4;
endpackage
package facade;
  import origin::*;
  import origin::HIDDEN;
  export origin::state_t, origin::OFF, origin::ON, origin::CONFIG;
endpackage
package relay;
  import facade::*;
  export *::*;
  state_t used = OFF;
endpackage
module top;
  import facade::*;
  int from_scope = facade::CONFIG;
  int from_relay = relay::OFF;
  int lexical = CONFIG;
endmodule
"""
    with project({"top.sv": text}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        for marker, trigger, required, forbidden in [
            ("from_scope = facade::", ":", {"CONFIG", "state_t", "OFF", "ON"}, {"HIDDEN"}),
            ("from_relay = relay::", ":", {"OFF", "state_t", "used"}, {"HIDDEN", "CONFIG"}),
            ("int lexical = ", None, {"CONFIG", "state_t", "OFF", "ON"}, {"HIDDEN"}),
        ]:
            labels = [i["label"] for i in items(client, doc, text, marker, trigger)]
            assert required <= set(labels), (marker, labels)
            assert forbidden.isdisjoint(labels), (marker, labels)
            assert all(labels.count(n) == 1 for n in required)


def test_navigation_touching_identifier_end(project):
    text = ("module top; logic sig; assign sig = sig;\n"
            " covergroup cg; coverpoint sig { option.weight = 2; } endgroup\n"
            "endmodule\n")
    with project({"top.sv": text}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        for method, extra in [
            ("definition", {}), ("documentHighlight", {}),
            ("references", {"context": {"includeDeclaration": True}}),
            ("rename", {"newName": "renamed"}),
        ]:
            def query(marker):
                return client.request("textDocument/" + method, {**at(doc, text, marker), **extra})[0]

            inside = query("assign si")
            assert inside and query("assign sig") == inside, method
            assert not query("assign sig "), method
        for marker in ("option.weigh", "option.weight"):
            refs, _ = client.request("textDocument/references", {
                **at(doc, text, marker), "context": {"includeDeclaration": True}
            })
            assert not refs


def test_unknown_requests_and_notifications(project):
    with project({}) as (client, _):
        client.notify("example/noSuchMethod", {})
        client.send([{"id": "unknown-id", "method": "example/noSuchMethod", "params": {}}])
        reply, _ = client.wait(lambda m: m.get("id") == "unknown-id", timeout=3)
        assert reply["error"]["code"] == -32601
        assert "result" not in reply
        client.state(compact=True)
        assert not any("error" in message for message, _ in client.pending)


def test_include_navigation_uses_active_header(project):
    header = "`ifndef PAYLOAD_SVH\n`define PAYLOAD_SVH\n`endif\n"
    text = ('`include   "payload.svh"\n`include  "payload.svh"\n'
            '`include "missing.svh"\nmodule top; endmodule\n')
    with project({"chosen/top.sv": text, "chosen/payload.svh": header,
                  "other/payload.svh": header}) as (client, root):
        doc = open_file(client, root / "chosen/top.sv", text)
        for line in (0, 1):
            for column in (3, 9, 16):
                params = {"textDocument": doc, "position": {"line": line, "character": column}}
                links, _ = client.request("textDocument/definition", params)
                assert [link["targetUri"] for link in links or []] == [
                    (root / "chosen/payload.svh").as_uri()
                ]
                hover, _ = client.request("textDocument/hover", params)
                assert "payload.svh" in hover["contents"]["value"]
                assert str(root) not in hover["contents"]["value"]
        missing, _ = client.request("textDocument/definition", at(doc, text, '"missing'))
        assert not missing


def test_mixed_named_ports_preserve_other_hint_lines(project):
    text = """module leaf(input int a, b, c, output int d); endmodule
module top;
  int a, b, c, d;
  leaf first(.a(a), .b(b),
             .c(c),
             .d(d)),
       second(.a(a),
              .b(b),
              .c(c),
              .d(d));
endmodule
"""
    with project({"top.sv": text}, {"inlayHints": {"portTypes": True}}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        hints, _ = client.request("textDocument/inlayHint", {"textDocument": doc, "range": {
            "start": {"line": 0, "character": 0}, "end": position(text, len(text))}})
        assert sorted(h["position"]["line"] for h in hints or []) == list(range(4, 10))


def test_uninstantiated_generic_forward_method(project):
    text = """class Worker #(parameter int WIDTH=4);
  function void run();
    if (WIDTH) begin int v = later(); end
    void'(later());
  endfunction
  function int later(); return WIDTH; endfunction
  function int shadow();
    int later = 1;
    return later;
  endfunction
endclass
"""
    with project({"top.sv": text}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        for marker in ("v = lat", "void'(lat"):
            links, _ = client.request("textDocument/definition", at(doc, text, marker))
            assert [link["targetSelectionRange"]["start"] for link in links or []] == [
                position(text, text.index("later(); return"))]
        links, _ = client.request("textDocument/definition", at(doc, text, "return lat"))
        assert links[0]["targetSelectionRange"]["start"] == position(text, text.index("later = 1"))


def test_typedef_type_position_and_type_expression(project):
    text = """module consumer #(parameter type T = int) (); endmodule
module top;
  typedef int existing_t;
  int source_value;
  typedef type(source_value) captured_t;
  consumer #(.T(type(source_value))) instance_name();
  type(source_value) another_value;
  typedef existing_t ordinary_t;
endmodule
"""
    with project({"top.sv": text}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        for marker in ("typedef type(", ".T(type(", "  type("):
            assert "source_value" in {i["label"] for i in items(client, doc, text, marker)}
        labels = {i["label"] for i in items(client, doc, text, "another_value;\n  typedef ")}
        assert "existing_t" in labels
        assert "source_value" not in labels


def test_macro_receiver_completion_preserves_call(project):
    text = """`define CURRENT state.entry
`define SELECT(field) state.field
module top;
  typedef struct packed {logic value;} entry_t;
  struct packed {entry_t entry; logic other;} state;
  initial begin
    $display(`CURRENT.value);
    $display(`SELECT(entry).value);
  end
endmodule
"""
    with project({"top.sv": text}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        for marker in ("`CURRENT.", "`SELECT(entry)."):
            matches = [i for i in items(client, doc, text, marker, ".") if i["label"] == "value"]
            assert len(matches) == 1
            resolved, _ = client.request("completionItem/resolve", matches[0])
            assert resolved["documentation"]
            start = text.index(marker) + len(marker)
            assert matches[0]["textEdit"]["range"] == {
                "start": position(text, start), "end": position(text, start + len("value"))
            }
        incomplete = text.replace(".value);", ".);")
        client.notify("textDocument/didChange", {"textDocument": {**doc, "version": 2},
                                                  "contentChanges": [{"text": incomplete}]})
        for marker in ("`CURRENT.", "`SELECT(entry)."):
            field = next(i for i in items(client, doc, incomplete, marker, ".") if i["label"] == "value")
            cursor = at(doc, incomplete, marker)["position"]
            assert field["textEdit"]["range"] == {"start": cursor, "end": cursor}
