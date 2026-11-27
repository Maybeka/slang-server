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


def test_system_timing_checks_are_specific_to_specify_items(project):
    text = """module delays(input logic clk, data);
  initial $display("ordinary system task");
  specify
    $setup(data, posedge clk, 1);
    $hold(posedge clk, data, 2);
    $setuphold(posedge clk, data, 1, $bits(data));
    $setu
  endspecify
endmodule
"""
    names = {
        "$setup", "$hold", "$setuphold", "$recovery", "$removal", "$recrem",
        "$skew", "$timeskew", "$fullskew", "$period", "$width", "$nochange",
    }
    with project({"delays.sv": text}) as (client, root):
        doc = open_file(client, root / "delays.sv", text)
        hover, _ = client.request("textDocument/hover", at(doc, text, "\n    $setup"))
        assert "System timing check" in hover["contents"]["value"]
        assert "$setup(data_event, reference_event, limit" in hover["contents"]["value"]
        definition, _ = client.request("textDocument/definition", at(doc, text, "\n    $setup"))
        assert not definition

        marker = "$setuphold(posedge clk, data, 1, $bits(data));\n    $"
        timing = {item["label"]: item for item in items(client, doc, text, marker + "setu")}
        assert timing.keys() == names
        assert timing["$setup"]["insertTextFormat"] == 2
        assert "data_event" in timing["$setup"]["textEdit"]["newText"]
        existing = {item["label"]: item for item in items(client, doc, text, "\n    $setup")}
        assert existing["$setup"]["textEdit"]["newText"] == r"\$setup"

        ordinary = {item["label"] for item in items(client, doc, text, "initial $dis")}
        nested = {item["label"] for item in items(client, doc, text, "1, $bi")}
        assert "$display" in ordinary and not (names & ordinary)
        assert "$bits" in nested and not (names & nested)

        bare = text.replace("$setu\n", "$\n")
        client.notify("textDocument/didChange", {
            "textDocument": {**doc, "version": 2},
            "contentChanges": [{"text": bare}],
        })
        assert {item["label"] for item in items(client, doc, bare, marker)} == names


def test_standard_class_method_hover_uses_the_receiver_specialization(project):
    text = """module top;
  mailbox #(int) integers;
  mailbox #(string) strings;
  int i;
  string s;
  initial begin
    strings = new(4);
    integers.put(i);
    strings.get(s);
  end
endmodule
"""
    with project({"mailboxes.sv": text}) as (client, root):
        doc = open_file(client, root / "mailboxes.sv", text)
        for marker, signature in (("integers.put", "task put(input int message)"),
                                  ("strings.get", "task get(ref string message)")):
            params = at(doc, text, marker)
            hover, _ = client.request("textDocument/hover", params)
            assert signature in hover["contents"]["value"]
            definition, _ = client.request("textDocument/definition", params)
            assert not definition
        constructed, _ = client.request("textDocument/hover", at(doc, text, "strings = new"))
        assert "function new(input int bound" in constructed["contents"]["value"]
        builtin_class, _ = client.request("textDocument/hover", at(doc, text, "  mailbox"))
        assert "Built-in class" in builtin_class["contents"]["value"]


def test_built_in_methods_complete_for_the_bound_receiver_type(project):
    text = """module top;
  typedef enum {idle, running} state_t;
  state_t state;
  string names[2];
  int values[2];
  bit [3:0] packed_values;
  int len;
  initial begin
    len = state.name().len();
    len = names[0].len();
    values.shuffle();
    packed_values.shuffle();
  end
endmodule
"""
    with project({"methods.sv": text}) as (client, root):
        doc = open_file(client, root / "methods.sv", text)
        for marker, method in (("state.name", "name"), ("state.name().len", "len"),
                               ("names[0].len", "len"), ("values.shuffle", "shuffle")):
            params = at(doc, text, marker)
            hover, _ = client.request("textDocument/hover", params)
            assert "Built-in method" in hover["contents"]["value"], marker
            assert method in hover["contents"]["value"]
            definition, _ = client.request("textDocument/definition", params)
            assert not definition
        for marker, name in (("state.name().", "len"), ("names[0].", "len"),
                             ("values.", "shuffle")):
            assert name in {item["label"] for item in items(client, doc, text, marker)}
        assert "shuffle" not in {item["label"] for item in items(
            client, doc, text, "packed_values."
        )}


def test_named_connections_and_arguments_suggest_unbound_formals(project):
    text = """module child #(parameter int A=1, B=2, C=3) (clk, reset_n, data);
  input clk, reset_n, data;
endmodule
module legacy (clk);
  input clk;
  parameter logic [3:0] WIDTH = 1;
  localparam int INTERNAL = 2;
endmodule
module top;
  bit clk, reset_n, data;
  child #(.A(4), .C(7), .) one(.clk(clk), .data(data), .);
  child two(clk, .);
  legacy #(.) ctrl(clk);
  class worker;
    function new(int first, string second);
    endfunction
  endclass
  worker declaration = new(.first(1), .);
  task send(int channel, int addr, int payload);
  endtask
  initial begin
    worker assigned;
    assigned = new(.second("x"), .);
    send(.channel(0), .);
  end
endmodule
"""
    with project({"named.sv": text}) as (client, root):
        doc = open_file(client, root / "named.sv", text)
        for marker, expected in (
            (".A(4), .C(7), .", {"B"}),
            (".clk(clk), .data(data), .", {"reset_n"}),
            ("two(clk, .", {"reset_n", "data"}),
            ("send(.channel(0), .", {"addr", "payload"}),
            ("declaration = new(.first(1), .", {"second"}),
            ('assigned = new(.second("x"), .', {"first"}),
        ):
            results = items(client, doc, text, marker, ".")
            assert {item["label"] for item in results} == expected
            name = results[0]["label"]
            assert results[0]["textEdit"]["newText"] == name + "(${1:" + name + "})"

        param, = items(client, doc, text, "legacy #(.", ".")
        assert param["label"] == "WIDTH"
        assert "logic [3:0]" in param["labelDetails"]["detail"]

        bound = at(doc, text, "send(.channel")
        hover, _ = client.request("textDocument/hover", bound)
        assert "channel" in hover["contents"]["value"]
        definition, _ = client.request("textDocument/definition", bound)
        assert definition[0]["targetSelectionRange"]["start"] == position(
            text, text.index("int channel") + len("int ")
        )
        constructor = at(doc, text, "declaration = new")
        rename, _ = client.request("textDocument/rename", {**constructor, "newName": "changed"})
        assert not rename


def test_project_environment_is_layered_for_paths_flags_and_nested_filelists(project):
    shared = {
        "build": "${LIST}",
        "buildRelativePaths": True,
        "flags": '-DARG_WIDTH="$WIDTH"',
        "incdirs": ["${AUX_HEADERS}"],
        "env": {
            "LIST": "lists/design.f",
            "HEADERS": "../headers $literal",
            "AUX_HEADERS": "aux",
            "WIDTH": "4",
        },
    }
    local = {"env": {"WIDTH": "7"}}
    text = """`include "config.svh"
`include "extra.svh"
module top;
    logic [`ARG_WIDTH + `EXTRA_WIDTH + `FILE_WIDTH - 1:0] count;
endmodule
"""
    files = {
        ".slang/server.json": json.dumps(shared),
        ".slang/local/server.json": json.dumps(local),
        "lists/design.f": "-F nested.f\n",
        "lists/nested.f": '-I"${HEADERS}" ../top.sv\n',
        "lists/alternate.f": "-F alternative.f\n",
        "lists/alternative.f": '-I"../other headers" ../top.sv\n',
        "headers $literal/config.svh": "`define FILE_WIDTH 2\n",
        "other headers/config.svh": "`define FILE_WIDTH 5\n",
        "aux/extra.svh": "`define EXTRA_WIDTH 3\n",
        "top.sv": text,
    }
    with project(files) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        params = at(doc, text, "] count")

        def width(value):
            result, _ = client.request("textDocument/hover", params)
            assert f"Width: `{value}`" in result["contents"]["value"]

        width(12)
        config_path = root / ".slang/local/server.json"
        raw = json.dumps({"env": {"WIDTH": "8", "LIST": "lists/alternate.f"}})
        config_path.write_text(raw)
        client.notify("workspace/didChangeWatchedFiles", {
            "changes": [{"uri": config_path.as_uri(), "type": 2}],
        })
        width(16)
        assert config_path.read_text() == raw
        assert (root / ".slang/server.json").read_text() == json.dumps(shared)
        config_path.write_text(json.dumps({"build": "${MISSING_SLANG_PROJECT_ENV_CASE}"}))
        client.notify("workspace/didChangeWatchedFiles", {
            "changes": [{"uri": config_path.as_uri(), "type": 2}],
        })
        warning, _ = client.wait(lambda m: m.get("method") == "window/showMessage"
                                 and "MISSING_SLANG_PROJECT_ENV_CASE" in m["params"]["message"])
        assert "Unknown environment variable" in warning["params"]["message"]
        width(16)


def test_default_generic_scope_queries_preserve_native_diagnostics(project):
    text = """package factories;
    class registry #(type T = int);
      static function T create();
        T item;
        return item;
      endfunction
    endclass
    class packet #(parameter int LIMIT = 8);
      typedef registry #(int) type_id;
    endclass
    class mandatory #(parameter int REQUIRED);
      localparam int NEVER_INFERRED = REQUIRED;
    endclass
endpackage
module top;
    localparam int implicit_limit = factories::packet::LIMIT;
    localparam int explicit_limit = factories::packet#(3)::LIMIT;
    int result = factories::packet::type_id::create();
    localparam int invalid = factories::mandatory::NEVER_INFERRED;
endmodule
"""
    with project({"factory.sv": text}) as (client, root):
        doc = open_file(client, root / "factory.sv", text)
        result, _ = client.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                               and m["params"]["uri"] == doc["uri"]
                               and m["params"].get("version") == 1)
        assert any("without providing parameter assignments" in d["message"]
                   for d in result["params"]["diagnostics"])
        for marker in ("factories::packet::LIMIT", "packet#(3)::LIMIT",
                       "packet::type_id", "type_id::create"):
            params = at(doc, text, marker)
            hover, _ = client.request("textDocument/hover", params)
            links, _ = client.request("textDocument/definition", params)
            assert hover and links, marker
        assert "create" in {item["label"] for item in items(client, doc, text, "type_id::")}
        invalid, _ = client.request("textDocument/hover", at(doc, text, "mandatory::NEVER_INFERRED"))
        assert not invalid
        client.notify("textDocument/didChange", {
            "textDocument": {**doc, "version": 2},
            "contentChanges": [{"text": text + "\n"}],
        })
        updated, _ = client.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                                and m["params"]["uri"] == doc["uri"]
                                and m["params"].get("version") == 2)
        assert updated["params"]["diagnostics"] == result["params"]["diagnostics"]


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


def test_missing_indexed_dependency_survives_reload(project):
    top = "module top; child instance_a(); endmodule\n"
    config = {"index": [{"dirs": ["rtl", "unused"]}], "indexingThreads": 1}
    with project({"rtl/top.sv": top, "rtl/child.sv": "module child; endmodule",
                  "unused/other.sv": "module other; endmodule"}, config) as (client, root):
        def names(query):
            return [s["name"] for s in client.request("workspace/symbol", {"query": query})[0]]

        child = root / "rtl/child.sv"
        assert "child" in names("child") and "other" in names("other")
        child.unlink()
        doc = open_file(client, root / "rtl/top.sv", top)
        assert not client.request("textDocument/definition", at(doc, top, "top; chi"))[0]
        cfg = root / ".slang/server.json"
        config["index"] = [{"dirs": ["rtl"]}]
        cfg.write_text(json.dumps(config))
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": cfg.as_uri(), "type": 2}]})
        assert not names("child") and not names("other") and "top" in names("top")
        child.write_text("module child; endmodule\n")
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": child.as_uri(), "type": 1}]})
        links, _ = client.request("textDocument/definition", at(doc, top, "top; chi"))
        assert [link["targetUri"] for link in links] == [child.as_uri()]
        overlay = "module child; int local_value; initial $display(local_value); endmodule\n"
        child_doc = open_file(client, child, overlay)
        child.unlink()
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": child.as_uri(), "type": 3}]})
        links, _ = client.request("textDocument/definition", at(child_doc, overlay, "$display(local_"))
        assert [link["targetUri"] for link in links] == [child.as_uri()]


@pytest.mark.parametrize("configured", [False, True], ids=["explore", "single-unit"])
def test_discovered_dependency_diagnostics_survive_edit(project, configured):
    top = "module top; dep u(); initial $display(missing_top); endmodule\n"
    dep = "module dep; initial $display(missing_dep); endmodule\n"
    with project({"top.sv": top, "dep.sv": dep, "design.f": "dep.sv\ntop.sv\n"},
                 {"build": "design.f", "flags": "--single-unit --top top"}
                 if configured else None) as (client, root):
        doc = open_file(client, root / "top.sv", top)
        path = root / "dep.sv"
        links, _ = client.request("textDocument/definition", at(doc, top, "top; de"))
        assert [link["targetUri"] for link in links] == [path.as_uri()]
        def published(uri, expected):
            client.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                        and m["params"]["uri"] == uri
                        and [d["message"] for d in m["params"]["diagnostics"]] == expected)
        published(doc["uri"], ["use of undeclared identifier 'missing_top'"])
        if configured:
            published(path.as_uri(), ["use of undeclared identifier 'missing_dep'"])
        other = open_file(client, path, dep)
        if not configured:
            published(path.as_uri(), ["use of undeclared identifier 'missing_dep'"])
        client.notify("textDocument/didChange", {"textDocument": {**other, "version": 2},
                      "contentChanges": [{"text": dep.replace("missing_dep", "1")}]})
        client.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                    and m["params"]["uri"] == other["uri"] and m["params"].get("version") == 2
                    and m["params"]["diagnostics"] == [])
        assert client.request("textDocument/definition", at(doc, top, "top; de"))[0][0]["targetUri"] == path.as_uri()
        assert not any(m.get("method") == "textDocument/publishDiagnostics"
                       and m["params"]["uri"] == doc["uri"] and not m["params"]["diagnostics"]
                       for m, _ in client.pending)


@pytest.mark.parametrize("configured", [False, True], ids=["explore", "build"])
@pytest.mark.parametrize("first", ["left.sv", "right.sv", "a.svh", "b.svh"])
def test_included_interfaces_keep_owner_width_and_definitions(project, configured, first):
    files = {"left.sv": '`define A 4\n`include "a.svh"\nmodule left(bus_b p); endmodule\n',
             "right.sv": '`define B 8\n`include "b.svh"\nmodule right(bus_a p); endmodule\n',
             "a.svh": "interface bus_a; logic [`A-1:0] field_a; endinterface\n",
             "b.svh": "interface bus_b; logic [`B-1:0] field_b; endinterface\n"}
    extra = {"root.sv": "module root; bus_a a(); bus_b b(); left l(b); right r(a); endmodule\n",
             "design.f": "left.sv\nright.sv\nroot.sv\n"}
    with project({**files, **extra}, {"build": "design.f", "flags": "--top root"}
                 if configured else None) as (client, root):
        docs = {first: open_file(client, root / first, files[first])}
        if first.endswith(".svh"):
            result, _ = client.request("textDocument/hover", at(docs[first], files[first], "field_"))
            assert f"Width: `{4 if first == 'a.svh' else 8}`" in result["contents"]["value"]
        else:
            target = "b.svh" if first == "left.sv" else "a.svh"
            result, _ = client.request("textDocument/definition", at(docs[first], files[first], "(bus_"))
            assert [link["targetUri"] for link in result or []] == [(root / target).as_uri()]
        docs.update({n: open_file(client, root / n, text) for n, text in files.items() if n != first})

        def check(right_width):
            for n, expected in (("left.sv", "b.svh"), ("right.sv", "a.svh")):
                result, _ = client.request("textDocument/definition", at(docs[n], files[n], "(bus_"))
                assert [link["targetUri"] for link in result or []] == [(root / expected).as_uri()]
            for n, width in (("a.svh", 4), ("b.svh", right_width)):
                result, _ = client.request("textDocument/hover", at(docs[n], files[n], "field_"))
                assert f"Width: `{width}`" in result["contents"]["value"]

        check(8)
        files["b.svh"] = files["b.svh"].replace("-1:0", ":0")
        client.notify("textDocument/didChange", {"textDocument": {**docs["b.svh"], "version": 2},
                                               "contentChanges": [{"text": files["b.svh"]}]})
        check(9)


def test_rename_encoded_paths_across_files(project):
    name = "源 %23# file.sv"
    pkg = "package defs; int sig; endpackage\n"
    top = "module top; import defs::*; initial sig=1; endmodule\n"
    with project({name: pkg, "top.sv": top, "design.f": f'"{name}"\ntop.sv\n'},
                 {"build": "design.f"}) as (client, root):
        open_file(client, root / name, pkg)
        doc = open_file(client, root / "top.sv", top)
        edits, _ = client.request("textDocument/rename", {**at(doc, top, "initial si"), "newName": "changed"})
        assert {k: len(v) for k, v in edits["changes"].items()} == {
            (root / name).as_uri(): 1, doc["uri"]: 1}
        encoded = (root / name).as_uri()
        for version, (alias, current) in enumerate(((encoded.replace("%E6", "%e6"),
                                                    pkg.replace("int sig;", "unknown_type sig;")),
                                                   (encoded, pkg)), 2):
            client.notify("textDocument/didChange", {"textDocument": {"uri": alias, "version": version},
                          "contentChanges": [{"text": current}]})
            message, _ = client.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                                     and m["params"]["uri"] == encoded and m["params"].get("version") == version)
            diagnostics = message["params"]["diagnostics"]
            if version == 2:
                assert any("unknown_type" in d["message"] for d in diagnostics)
            else:
                assert diagnostics == []


def test_macro_completion_obeys_cursor_and_includes(project):
    text = """`define PICK(first) first
module top;
int a = `PICK(1);
`undef PICK
`define PICK(second,third) second+third
int b = `PICK(2,3);
`include "extra.svh"
int c = `PICK_HEADER(1);
`undefineall
int d = `PICK_CLI;
endmodule
"""
    header = "`define PICK_HEADER(arg) arg\n" + "".join(
        f"`define PICK_OTHER_{i} {i}\n" for i in range(1500))
    with project({"top.sv": text, "extra.svh": header},
                 {"flags": "+define+PICK_CLI=7", "inlayHints": {"macroArgNames": 1}}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        def matches(prefix):
            return {item["label"]: item for item in items(client, doc, text, prefix)}
        for marker, signature in (("a = `PIC", "(first)"), ("b = `PIC", "(second,third)")):
            found = matches(marker)
            assert "`PICK_HEADER" not in found
            resolved, _ = client.request("completionItem/resolve", found["`PICK"])
            assert signature in resolved["documentation"]["value"].replace(" ", "")
        included = matches("c = `PIC")
        assert "`PICK_HEADER" in included and "`PICK_OTHER_1499" in included
        after = matches("d = `PIC")
        assert "`PICK_CLI" in after and "`PICK_HEADER" not in after and "`PICK" not in after
        hints, _ = client.request("textDocument/inlayHint", {"textDocument": doc, "range": {
            "start": {"line": 0, "character": 0}, "end": position(text, len(text))}})
        assert sorted((h["position"]["line"], h["label"]) for h in hints or []) == [
            (2, "first:"), (5, "second:"), (5, "third:"), (7, "arg:")]


def test_include_depth_recovers_on_edit(project):
    text = """`ifdef INSIDE
`ifndef DONE
`define DONE
`include "top.sv"
`include "top.sv"
`endif
`else
`define INSIDE
`include "top.sv"
`include "healthy.svh"
module top; value_t marker=1; initial $display(marker); endmodule
`endif
"""
    with project({"top.sv": text, "healthy.svh": "typedef int value_t;\n", "design.f": "top.sv\n"},
                 {"build": "design.f", "flags": "--max-include-depth=1 --top top"}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        def diagnostics(version):
            message, _ = client.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                                     and m["params"].get("version") == version
                                     and m["params"]["uri"] == doc["uri"])
            return [d["message"] for d in message["params"]["diagnostics"]]
        assert diagnostics(1) == ["exceeded max include depth"]
        fixed = "module top; int marker=1; initial $display(marker); endmodule\n"
        client.notify("textDocument/didChange", {"textDocument": {**doc, "version": 2},
                                                "contentChanges": [{"text": fixed}]})
        assert diagnostics(2) == []
        assert client.request("textDocument/definition", at(doc, fixed, "$display(mar"))[0]


def test_signal_cones_skip_absent_connections_and_internal_symbols(project):
    text = """module regular(input logic used, spare, output logic q); assign q=used; endmodule
module explicit_ports(output .q(v), input .p()); logic v; assign v=1; endmodule
module top; logic src, line, dest, other;
  regular a(.used(src), .spare(), .q());
  explicit_ports b(.q(line), .p(other));
  assign dest=line;
endmodule
"""
    with project({"top.sv": text}) as (client, root):
        path = root / "top.sv"
        open_file(client, path, text)
        def command(name, argument):
            return client.request("workspace/executeCommand", {"command": "slang." + name,
                                                              "arguments": [argument]})[0]
        command("setTopLevel", str(path))
        for direction, signal, expected in (("Loads", "top.src", [("top.a.used", "used,")]),
                                            ("Loads", "top.a.q", []),
                                            ("Loads", "top.line", [("top.dest", "dest,")]),
                                            ("Drivers", "top.line", [])):
            hits = command("get" + direction + "WithLocation", signal)
            assert [(h["path"], h["location"]["range"]["start"]) for h in hits] == [
                (name, position(text, text.index(decl))) for name, decl in expected]


def test_virtual_interface_driver_mapping_for_repeated_instances(project):
    text = """interface channel(input logic src);
  logic sampled;
  assign sampled = src;
endinterface
package env;
  class Reader;
    virtual channel bus;
    function logic read(); return bus.sampled; endfunction
  endclass
endpackage
module top(input logic a, b);
  channel left(a), right(b);
  env::Reader reader;
  initial begin reader=new; reader.bus=right; $display(reader.read()); end
endmodule
"""
    with project({"top.sv": text, "design.f": "top.sv\n"},
                 {"build": "design.f", "flags": "--top top"}) as (client, root):
        doc = open_file(client, root / "top.sv", text)
        links, _ = client.request("textDocument/definition", at(doc, text, "return bus.sam"))
        assert [x["targetSelectionRange"]["start"] for x in links] == [
            position(text, text.index("sampled;"))]
        for side, source in (("left", "a"), ("right", "b")):
            for leaf, expected in (("sampled", f"top.{side}.src"), ("src", "top." + source)):
                hits, _ = client.request("workspace/executeCommand", {
                    "command": "slang.getDriversWithLocation",
                    "arguments": [f"top.{side}.{leaf}"]})
                assert [h["path"] for h in hits] == [expected]
