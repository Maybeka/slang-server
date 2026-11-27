// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

using namespace slang;

TEST_CASE("Nested assignment pattern keys resolve against contextual element types") {
    ServerHarness server;
    auto doc = server.openFile("nested_pattern_navigation.sv", R"(
typedef struct packed { int code; bit enabled; } entry_t;
typedef struct { entry_t entries[1]; } batch_t;
module sink #(parameter type T = int)(input T value); endmodule
module top;
    int code = 7;
    entry_t array_value[2] = '{'{code: code, enabled: 1}, '{code: code, enabled: 1}};
    entry_t replicated[2] = '{2{'{code: code, enabled: 1}}};
    function automatic batch_t make(int code);
        return '{entries: '{'{code: code, enabled: 1}}};
    endfunction
    sink #(entry_t) named(.value('{code: code, enabled: 1}));
    sink #(entry_t) ordered('{code: code, enabled: 1});
endmodule
)");
    for (auto prefix : {"array_value[2] = '{'{", "replicated[2] = '{2{'{", "return '{entries: '{'{",
                        "named(.value('{", "ordered('{"}) {
        CAPTURE(prefix);
        auto key = doc.after(prefix);
        auto links = key.getDefinitions();
        REQUIRE(links.size() == 1);
        CHECK(links.front().targetSelectionRange.start == doc.before("code;").getPosition());
        auto hover = doc.getHoverAt(key.m_offset);
        REQUIRE(hover);
        CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("**Field** `code`") !=
              std::string::npos);
        auto items = key.getCompletions();
        CHECK(std::ranges::any_of(items, [](const auto& item) {
            return item.m_item.label == "code" &&
                   item.m_item.kind == lsp::CompletionItemKind::Field;
        }));
        auto value = doc.after(std::string(prefix) + "code: ").getDefinitions();
        REQUIRE(value.size() == 1);
        auto declaration = std::string_view(prefix).starts_with("return") ? "code);" : "code =";
        CHECK(value.front().targetSelectionRange.start == doc.before(declaration).getPosition());
    }
}

TEST_CASE("Unfinished return assignment pattern offers the returned structure fields") {
    ServerHarness server;
    auto doc = server.openFile("unfinished_pattern_return.sv", R"(
package p;
    typedef struct { int code; bit enabled; } entry_t;
    function automatic entry_t make();
        return '{)");
    auto items = doc.after("return '{").getCompletions("{");
    REQUIRE(items.size() == 1);
    CHECK(items.front().m_item.label == "'{code, enabled}");
    CHECK(items.front().m_item.insertText == "\n\tcode: $1,\n\tenabled: $2\n\\};");
}

TEST_CASE("Incomplete nested array patterns complete only unused keys") {
    ServerHarness server;
    auto doc = server.openFile("unfinished_array_pattern.sv", R"(
package p;
    typedef struct { int code; bit enabled; } entry_t;
    entry_t empty[1] = '{'{}};
    entry_t partial[1] = '{'{co}};
    entry_t next[1] = '{'{code: 7, }};
    entry_t existing[1] = '{'{code: 7, en: 1}};
endpackage
)");
    auto all = doc.after("empty[1] = '{'{").getCompletions("{");
    REQUIRE(all.size() == 1);
    CHECK(all.front().m_item.label == "'{code, enabled}");
    for (std::string_view closing : {"", "}"}) {
        auto draft = server.openFile("unfinished_nested_elements.sv", std::string(R"(
package draft;
    typedef struct { int code; bit enabled; } entry_t;
    entry_t entries[2] = '{'{)") + std::string(closing));
        auto items = draft.after("entries[2] = '{'{").getCompletions("{");
        REQUIRE(items.size() == 1);
        const auto& snippet = items.front().m_item;
        const std::string expected = "\n\tcode: $1,\n\tenabled: $2\n";
        if (closing.empty()) {
            CHECK(snippet.insertText == expected + "\\},");
        }
        else {
            CHECK(snippet.insertText == expected);
            REQUIRE(snippet.additionalTextEdits);
            REQUIRE(snippet.additionalTextEdits->size() == 1);
            CHECK(snippet.additionalTextEdits->front().newText == ",");
        }
        draft.close();
    }
    for (auto [prefix, expected, edit] : {
             std::tuple{"partial[1] = '{'{co", "code", "code: $1"},
             std::tuple{"next[1] = '{'{code: 7, ", "enabled", "enabled: $1"},
             std::tuple{"existing[1] = '{'{code: 7, en", "enabled", "enabled"},
         }) {
        CAPTURE(prefix);
        auto items = doc.after(prefix).getCompletions();
        auto field = std::ranges::find_if(items, [&](const auto& item) {
            return item.m_item.label == expected;
        });
        REQUIRE(field != items.end());
        REQUIRE(field->m_item.textEdit);
        CHECK(rfl::get<lsp::TextEdit>(*field->m_item.textEdit).newText == edit);
        if (std::string_view(expected) == "enabled")
            CHECK(std::ranges::none_of(items, [](const auto& item) {
                return item.m_item.label == "code";
            }));
    }
}

TEST_CASE("Generate declarations remain queryable when the loop cannot elaborate") {
    ServerHarness server;
    auto broken = server.openFile("broken_generate.sv", R"(
module sample;
    for ( = 0; i < 2; i++) begin : items
        int field;
        initial field = 0;
    end
endmodule
)");
    auto use = broken.after("initial ");
    auto links = use.getDefinitions();
    REQUIRE(links.size() == 1);
    CHECK(links.front().targetSelectionRange.start == broken.before("field;").getPosition());
    CHECK(broken.getHoverAt(use.m_offset));
    CHECK_FALSE(broken.getDiagnostics().empty());
    CHECK(std::ranges::any_of(use.getCompletions(),
                              [](const auto& item) { return item.m_item.label == "field"; }));

    auto nested = server.openFile("nested_generate_scope.sv", R"(
module top #(parameter int COUNT, parameter type T);
    for (genvar i = 0; i < $bits(T); i++) begin : outer
        logic [COUNT-1:0] values;
        for (genvar j = 0; j < COUNT; j++) begin : inner
            always_comb values[j] = i;
        end
    end
endmodule
)");
    for (auto [prefix, label, declaration] : {
             std::tuple{"j = 0; ", "j", "j = 0"},
             std::tuple{"always_comb ", "values", "values;"},
             std::tuple{"values[", "j", "j = 0"},
             std::tuple{"values[j] = ", "i", "i = 0"},
         }) {
        CAPTURE(prefix);
        auto cursor = nested.after(prefix);
        auto defs = cursor.getDefinitions();
        REQUIRE(defs.size() == 1);
        CHECK(defs.front().targetSelectionRange.start == nested.before(declaration).getPosition());
        CHECK(nested.getHoverAt(cursor.m_offset));
        CHECK(std::ranges::any_of(cursor.getCompletions(),
                                  [&](const auto& item) { return item.m_item.label == label; }));
    }
}

TEST_CASE("Foreach index and array with iterators keep their own types and declaration targets") {
    ServerHarness server;
    auto doc = server.openFile("iterator_navigation.sv", R"(
module top;
    int values[3];
    int entries[string];
    string index, item;
    int total;
    initial begin
        foreach (values[index]) values[index] = index;
        foreach (entries[key]) entries[key] = key.len();
        index = "outer";
        total = values.sum() with (item);
        total = values.sum(element) with (element);
        item = "outer";
    end
endmodule
)");
    for (auto [usage, declaration, type] : {
             std::tuple{"index] = ", "index])", "int"},
             std::tuple{"key] = ", "key])", "string"},
             std::tuple{"with (", "", "int"},
             std::tuple{"with (element", "element) with", "int"},
         }) {
        CAPTURE(usage);
        auto cursor = std::string_view(usage).starts_with("with")
                          ? (*declaration ? doc.before("element);") : doc.after("with ("))
                          : doc.before(usage);
        auto hover = doc.getHoverAt(cursor.m_offset);
        REQUIRE(hover);
        CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find(type) != std::string::npos);
        auto defs = cursor.getDefinitions();
        if (*declaration) {
            REQUIRE(defs.size() == 1);
            CHECK(defs.front().targetSelectionRange.start == doc.before(declaration).getPosition());
        }
        else {
            CHECK(defs.empty());
        }
    }
    auto outer = doc.before("item =");
    auto outerDefs = outer.getDefinitions();
    REQUIRE(outerDefs.size() == 1);
    CHECK(outerDefs.front().targetSelectionRange.start == doc.before("item;").getPosition());
    auto items = doc.after("with (element").getCompletions();
    CHECK(std::ranges::any_of(items,
                              [](const auto& item) { return item.m_item.label == "element"; }));
}

TEST_CASE("Covergroup options and methods use the covergroup body") {
    ServerHarness server;
    auto doc = server.openFile("covergroup_queries.sv", R"(
module top;
    covergroup sample_group with function sample(int value);
        option.per_instance = 1;
        type_option.merge_instances = 1;
        point_a: coverpoint value;
    endgroup
    sample_group group_h = new;
    initial begin
        group_h.set_inst_name("group_h");
        group_h.sample(1);
    end
endmodule
)");
    CHECK(doc.getDiagnostics().empty());
    for (auto [prefix, name] : {
             std::pair{"        ", "option"},
             std::pair{"option.", "per_instance"},
             std::pair{"        ", "type_option"},
             std::pair{"type_option.", "merge_instances"},
             std::pair{"group_h.", "set_inst_name"},
             std::pair{"group_h.", "sample"},
         }) {
        CAPTURE(name);
        auto cursor = doc.before(std::string(prefix) + name);
        cursor.m_offset += std::string_view(prefix).size();
        auto hover = doc.getHoverAt(cursor.m_offset);
        REQUIRE(hover);
        CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find(name) != std::string::npos);
        CHECK(cursor.getDefinitions().empty());
        auto items = std::string_view(prefix).ends_with('.') ? cursor.getCompletions(".")
                                                             : cursor.getCompletions();
        CHECK(std::ranges::any_of(items,
                                  [&](const auto& item) { return item.m_item.label == name; }));
    }
}

TEST_CASE("Nested array with clauses preserve the enclosing iterator and constraint context") {
    ServerHarness server;
    auto doc = server.openFile("nested_iterator_queries.sv", R"(
class Limits;
    rand int weights[3];
    constraint bounded { weights.sum() with (int'(item > 0)) == 1; }
endclass
module top;
    int values[2][3];
    int total;
    initial total = values.sum(row) with (row.sum() with (item));
endmodule
)");
    for (auto [use, name] : {
             std::pair{"item >", "item"},
             std::pair{"row.sum", "row"},
             std::pair{"item));", "item"},
         }) {
        CAPTURE(use);
        auto cursor = doc.before(use);
        auto hover = doc.getHoverAt(cursor.m_offset);
        REQUIRE(hover);
        CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("int") != std::string::npos);
        CHECK(std::ranges::any_of(cursor.getCompletions(),
                                  [&](const auto& item) { return item.m_item.label == name; }));
    }
    auto links = doc.before("row.sum").getDefinitions();
    REQUIRE(links.size() == 1);
    CHECK(links.front().targetSelectionRange.start == doc.before("row) with").getPosition());
}

TEST_CASE("Virtual interface type tokens resolve in their own declaration context") {
    ServerHarness server("virtual_interface");
    auto iface = server.openFile("bus_if.sv");
    auto doc = server.openFile("virtual_type_navigation.sv", R"(
class base #(type T);
    T link;
endclass
class client extends base #(.T(virtual interface bus_if #(.INITIAL(7)).monitor));
    function int read(); return int'(link.value); endfunction
endclass
class broken extends base #(.T(virtual missing_bus));
endclass
)");

    for (auto [use, declaration] : {
             std::pair{"virtual interface", "bus_if #"},
             std::pair{"interface bus_if", "bus_if #"},
             std::pair{"bus_if #", "bus_if #"},
             std::pair{"INITIAL(7)", "INITIAL ="},
             std::pair{"monitor));", "monitor("},
         }) {
        CAPTURE(use);
        auto cursor = doc.before(use);
        auto links = cursor.getDefinitions();
        REQUIRE(links.size() == 1);
        CHECK(links.front().targetUri == iface.m_uri);
        CHECK(links.front().targetSelectionRange.start == iface.before(declaration).getPosition());
        CHECK(doc.getHoverAt(cursor.m_offset));
    }
    auto missing = doc.before("missing_bus");
    CHECK(missing.getDefinitions().empty());
    CHECK_FALSE(doc.getHoverAt(missing.m_offset));
    CHECK(doc.after("link.").getDefinitions().size() == 2);
}

TEST_CASE("Array members remain queryable with unknown declaration bounds") {
    for (std::string_view params : {"parameter int COUNT, parameter int WIDTH = 3",
                                    "parameter int COUNT = 2, parameter int WIDTH",
                                    "parameter int COUNT = 2, parameter int WIDTH = 3"}) {
        CAPTURE(params);
        ServerHarness server;
        auto doc = server.openFile("array_bounds_navigation.sv",
                                   std::string("module top #(") + std::string(params) + R"();
    typedef struct packed { logic [WIDTH-1:0] payload; int field; } word_t;
    word_t words[COUNT][COUNT];
    word_t [COUNT-1:0] packed_words;
    typedef struct packed { word_t [COUNT-1:0] items; } group_t;
    group_t group;
    int result, index;
    always_comb begin
        result = words[index][0].field;
        result = packed_words[index].field;
        result = group.items[index].field;
    end
endmodule
)");
        for (auto prefix : {"words[index][0].", "packed_words[index].", "group.items[index]."}) {
            CAPTURE(prefix);
            auto cursor = doc.after(prefix);
            auto links = cursor.getDefinitions();
            REQUIRE(links.size() == 1);
            CHECK(links.front().targetSelectionRange.start == doc.before("field;").getPosition());
            CHECK(doc.getHoverAt(cursor.m_offset));
            auto completions = cursor.getCompletions(".");
            CHECK(std::ranges::any_of(completions, [](const auto& completion) {
                return completion.m_item.label == "field";
            }));
        }
    }
}

TEST_CASE("Function return names navigate to the enclosing function") {
    for (bool inClass : {false, true}) {
        CAPTURE(inClass);
        ServerHarness server;
        auto text = std::string(inClass ? "class worker;" : "module top;") + R"(
    typedef struct packed { int field; } result_t;
    function automatic int compute(int value);
        begin : body
            compute = value ? compute(value - 1) : 0;
        end
    endfunction
    function automatic result_t assemble(int value);
        assemble.field = value;
    endfunction
)" + (inClass ? "endclass" : "endmodule");
        auto doc = server.openFile("return_names.sv", text);
        for (auto [use, declaration] : {
                 std::pair{"compute =", "compute(int"},
                 std::pair{"compute(value", "compute(int"},
                 std::pair{"assemble.field", "assemble(int"},
                 std::pair{"field = value", "field;"},
             }) {
            CAPTURE(use);
            auto cursor = doc.before(use);
            auto links = cursor.getDefinitions();
            REQUIRE(links.size() == 1);
            CHECK(links.front().targetSelectionRange.start ==
                  doc.before(declaration).getPosition());
            CHECK(doc.getHoverAt(cursor.m_offset));
        }
    }
}

TEST_CASE("Interface array access recovers known element members across unknown bounds") {
    ServerHarness server;
    auto doc = server.openFile("interface_array_navigation.sv", R"(
typedef struct packed { bit present; } payload_t;
interface channel #(parameter type T = logic);
    T payload;
    bit ready, private_signal;
    modport sink(input ready, payload);
endinterface
module top #(parameter int COUNT)(channel.sink inputs[COUNT]);
    $static_assert(type(inputs[0].T) == type(payload_t));
    channel #(payload_t) channels[2][COUNT]();
    localparam int i = 0;
    wire a = channels[1][i].payload.present;
    wire b = inputs[i].ready;
    wire from_port = inputs[i].payload.present;
    wire c = channels[2][i].ready;
    wire d = channels[i].ready;
    wire e = channels[1][i][i].ready;
    wire f = channels[1][i:i].ready;
endmodule
)");
    for (auto [prefix, declaration] : {
             std::pair{"channels[1][i].payload.", "present;"},
             std::pair{"inputs[i].", "ready, private_signal"},
             std::pair{"inputs[i].payload.", "present;"},
         }) {
        CAPTURE(prefix);
        auto cursor = doc.after(prefix);
        auto links = cursor.getDefinitions();
        CHECK(std::ranges::any_of(links, [&](const auto& link) {
            return link.targetSelectionRange.start == doc.before(declaration).getPosition();
        }));
        CHECK(doc.getHoverAt(cursor.m_offset));
        auto items = cursor.getCompletions(".");
        const auto label = std::string_view(prefix) == "inputs[i]." ? "ready" : "present";
        CHECK(std::ranges::any_of(items,
                                  [&](const auto& item) { return item.m_item.label == label; }));
        CHECK(std::ranges::none_of(items, [](const auto& item) {
            return item.m_item.label == "private_signal";
        }));
    }
    for (auto prefix :
         {"channels[2][i].", "channels[i].", "channels[1][i][i].", "channels[1][i:i]."}) {
        CAPTURE(prefix);
        auto cursor = doc.after(prefix);
        CHECK(cursor.getDefinitions().empty());
        CHECK(std::ranges::none_of(cursor.getCompletions("."),
                                   [](const auto& item) { return item.m_item.label == "ready"; }));
    }
}

TEST_CASE("FindSyntax") {
    /// Find the syntax at each location in the file
    ServerHarness server("");
    auto hdl = server.openFile("all.sv");

    SyntaxScanner scanner;
    scanner.scanDocument(hdl);
}

TEST_CASE("FindSymbolRef") {
    /// Find the referenced symbol at each location in the file, if any.
    ServerHarness server("");
    auto hdl = server.openFile("all.sv");

    SymbolRefScanner scanner;
    scanner.scanDocument(hdl);
}

TEST_CASE("FindSymbolRefHdl") {
    /// Find the referenced symbol at each location in the comms test file.
    ServerHarness server("");
    auto hdl = server.openFile("hdl_test.sv");

    SymbolRefScanner scanner;
    scanner.scanDocument(hdl);
}

TEST_CASE("FindSymbolRefMacro") {
    /// Find the referenced symbol at each location in the macro test file.
    ServerHarness server("macro_test");
    auto hdl = server.openFile("macros.svh");

    SymbolRefScanner scanner;
    scanner.scanDocument(hdl);
}

TEST_CASE("FindMultiSymbolRef") {
    ServerHarness server("multi_symbol");
    auto first = server.openFile("first.sv");
    first.save();
    auto second = server.openFile("second.sv");
    second.save();
    auto hdl = server.openFile("use.sv");

    SymbolRefScanner scanner(
        {hdl.after("import features::").m_offset, hdl.after("export features::").m_offset,
         hdl.after("modport endpoint(output ").m_offset, hdl.after("import task ").m_offset,
         hdl.after("module top;\n    import ").m_offset, hdl.after("leaf u(.").m_offset,
         hdl.before("duplicate d").m_offset, hdl.after("genvar ").m_offset,
         hdl.before("VALUE =").m_offset, hdl.after("VALUE = ").m_offset,
         hdl.after("`ifdef ").m_offset});
    scanner.scanDocument(hdl);
}

TEST_CASE("HoverAndGotoHierarchicalInterfaceTypeParameter") {
    ServerHarness server;

    auto doc = server.openFile("test.sv", R"(
typedef logic [7:0] payload_t;

interface stream_if #(
    parameter type data_type = logic
);
endinterface

module top;
    stream_if #(.data_type(payload_t)) decoded_event_fifo();
    localparam type resolved_t = type(decoded_event_fifo.data_type);
endmodule
)");

    auto declaration = doc.before("data_type = logic");
    auto use = doc.after("decoded_event_fifo.");
    auto definitions = use.getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetSelectionRange.start == declaration.getPosition());

    auto hover = doc.getHoverAt(use.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("Value: [`payload_t (aka logic[7:0])`](<file:") != std::string::npos);
}

TEST_CASE("GotoDefinition_UndefDirective") {
    ServerHarness server;

    auto doc = server.openFile("test.sv", R"(
`define MY_MACRO 42
module top;
    localparam int x = `MY_MACRO;
endmodule
`undef MY_MACRO
)");

    // Goto definition on MY_MACRO in the `undef should go to the `define
    auto cursor = doc.after("`undef ");
    auto defs = cursor.getDefinitions();
    REQUIRE(!defs.empty());

    // Should point to the `define line
    CHECK(defs[0].targetRange.start.line == 1);
}

TEST_CASE("GotoDefinition_IfdefDirective") {
    ServerHarness server;

    auto doc = server.openFile("test.sv", R"(
`define MY_MACRO 42
module top;
    localparam int x = `MY_MACRO;
endmodule
`ifdef MY_MACRO
`endif
)");

    // Goto definition on MY_MACRO in the `ifdef should go to the `define
    auto cursor = doc.after("`ifdef ");
    auto defs = cursor.getDefinitions();
    REQUIRE(!defs.empty());

    // Should point to the `define line
    CHECK(defs[0].targetRange.start.line == 1);
}

TEST_CASE("GotoDefinition_IfndefDirective") {
    ServerHarness server;

    auto doc = server.openFile("test.sv", R"(
`define GUARD 1
`ifndef GUARD
`endif
)");

    auto cursor = doc.after("`ifndef ");
    auto defs = cursor.getDefinitions();
    REQUIRE(!defs.empty());

    CHECK(defs[0].targetRange.start.line == 1);
}

TEST_CASE("GotoDefinition_IfdefUndefinedMacro") {
    ServerHarness server;

    auto doc = server.openFile("test.sv", R"(
`ifdef NONEXISTENT
`endif
)");

    auto cursor = doc.after("`ifdef ");
    auto defs = cursor.getDefinitions();
    CHECK(defs.empty());
}

TEST_CASE("GotoDefinition_HonorsLinkSupport") {
    auto checkResultType = [](std::optional<bool> linkSupport) {
        lsp::InitializeParams params;
        params.capabilities.textDocument.emplace();
        params.capabilities.textDocument->definition = lsp::DefinitionClientCapabilities{
            .linkSupport = linkSupport};
        ServerHarness server(std::move(params));

        auto doc = server.openFile("test.sv", R"(
module top;
    int value;
    initial value = value;
endmodule
)");
        auto cursor = doc.after("initial value = ");
        auto result = server.getDocDefinition(lsp::DefinitionParams{
            .textDocument = {.uri = doc.m_uri}, .position = cursor.getPosition()});

        if (linkSupport.value_or(false)) {
            CHECK(rfl::holds_alternative<std::vector<lsp::DefinitionLink>>(result));
        }
        else {
            CHECK(rfl::holds_alternative<lsp::Definition>(result));
            const auto& definition = rfl::get<lsp::Definition>(result);
            CHECK(rfl::holds_alternative<std::vector<lsp::Location>>(definition));
        }
    };

    checkResultType(std::nullopt);
    checkResultType(true);
}

TEST_CASE("GotoDefinition_AllIndexedModuleDefinitions") {
    ServerHarness server;

    auto first = server.openFile("first.sv", R"(
module duplicate #(parameter int FIRST_VALUE = 1);
endmodule
)");
    first.save();
    auto second = server.openFile("second.sv", R"(
module duplicate #(parameter int SECOND_VALUE = 2);
endmodule
)");
    second.save();

    auto use = server.openFile("use.sv", R"(
module top;
    duplicate u();
endmodule
)");
    auto cursor = use.before("duplicate u");

    auto defs = cursor.getDefinitions();
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].targetUri != defs[1].targetUri);

    auto hover = use.getHoverAt(cursor.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("FIRST_VALUE") != std::string::npos);
    CHECK(content.find("SECOND_VALUE") != std::string::npos);
}

TEST_CASE("GotoDefinition_SemanticDefinitionExcludesWorkspaceNamesake") {
    ServerHarness server;

    auto indexed = server.openFile("indexed.sv", R"(
interface chosen_if #(parameter int INDEXED_VALUE = 1);
endinterface
)");
    indexed.save();

    auto use = server.openFile("use.sv", R"(
interface chosen_if #(parameter int LOCAL_VALUE = 2);
endinterface

module top(chosen_if port);
endmodule
)");
    auto cursor = use.after("module top(");

    auto defs = cursor.getDefinitions();
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].targetUri == use.m_uri);

    auto hover = use.getHoverAt(cursor.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("LOCAL_VALUE") != std::string::npos);
    CHECK(content.find("INDEXED_VALUE") == std::string::npos);
}

TEST_CASE("GotoDefinition_MacroGeneratedModuleDefinition") {
    ServerHarness server;

    auto generated = server.openFile("generated.sv", R"(
`define DECLARE_DUPLICATE module duplicate #(parameter int MACRO_VALUE = 1); endmodule
`DECLARE_DUPLICATE
)");
    generated.save();
    auto declared = server.openFile("declared.sv", R"(
module duplicate #(parameter int DECLARED_VALUE = 2);
endmodule
)");
    declared.save();

    auto use = server.openFile("use.sv", R"(
module top;
    duplicate u();
endmodule
)");
    auto cursor = use.before("duplicate u");

    auto defs = cursor.getDefinitions();
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].targetUri != defs[1].targetUri);

    auto hover = use.getHoverAt(cursor.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("MACRO_VALUE") != std::string::npos);
    CHECK(content.find("DECLARED_VALUE") != std::string::npos);
}

TEST_CASE("GotoDefinition_AllIndexedPackageDefinitions") {
    ServerHarness server;

    auto first = server.openFile("first_pkg.sv", R"(
package automatic duplicate_pkg;
    localparam int FIRST_VALUE = 1;
endpackage
)");
    first.save();
    auto second = server.openFile("second_pkg.sv", R"(
package static duplicate_pkg;
    localparam int SECOND_VALUE = 2;
endpackage
)");
    second.save();

    auto use = server.openFile("use_pkg.sv", R"(
module top;
    import duplicate_pkg::*;
endmodule
)");
    auto cursor = use.before("duplicate_pkg");

    auto defs = cursor.getDefinitions();
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].targetUri != defs[1].targetUri);

    auto hover = use.getHoverAt(cursor.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("package automatic duplicate_pkg") != std::string::npos);
    CHECK(content.find("package static duplicate_pkg") != std::string::npos);
}

TEST_CASE("GotoDefinition_AllIndexedMacroDefinitions") {
    ServerHarness server;

    auto first = server.openFile("first_macro.sv", R"(
`define DUPLICATE_MACRO FIRST_VALUE
)");
    first.save();
    auto second = server.openFile("second_macro.sv", R"(
`define DUPLICATE_MACRO SECOND_VALUE
)");
    second.save();

    auto use = server.openFile("use_macro.sv", R"(
`ifdef DUPLICATE_MACRO
`endif
)");
    auto cursor = use.after("`ifdef ");

    auto defs = cursor.getDefinitions();
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].targetUri != defs[1].targetUri);

    auto hover = use.getHoverAt(cursor.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("FIRST_VALUE") != std::string::npos);
    CHECK(content.find("SECOND_VALUE") != std::string::npos);
}

TEST_CASE("LoadTransitivePackages") {
    /// Find the referenced symbol at each location in files with circular package dependencies.
    /// Tests the queue-based cycle detection while loading dependent trees.
    ServerHarness server("repo1"); // Use repo1 as workspace folder
    auto hdl = server.openFile("cycle_test.sv");

    SymbolRefScanner scanner;
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesAllTokens_all.sv") {
    /// Find references at each location in all.sv
    ServerHarness server("");
    auto hdl = server.openFile("all.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesAllTokens_references_test.sv") {
    /// Find references at each location in references_test.sv
    ServerHarness server("indexer_test");
    auto hdl = server.openFile("references_test.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesAllTokens_modules.sv") {
    /// Find references at each location in modules.sv
    ServerHarness server("indexer_test");
    auto hdl = server.openFile("modules.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesAllTokens_classes.sv") {
    /// Find references at each location in classes.sv
    ServerHarness server("indexer_test");
    auto hdl = server.openFile("classes.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesModuleCrossfile.sv") {
    /// Find references at each location in classes.sv
    ServerHarness server("indexer_test");
    auto hdl = server.openFile("macro_crossfile.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesAllTokens_struct_enum_refs.sv") {
    /// Find references at each location in struct_enum_refs.sv
    ServerHarness server("indexer_test");
    auto hdl = server.openFile("struct_enum_refs.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesAllTokens_crossfile_pkg.sv") {
    /// Find references at each location in crossfile_pkg.sv
    ServerHarness server("indexer_test");
    auto hdl = server.openFile("crossfile_pkg.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}

TEST_CASE("FindReferencesAllTokens_crossfile_module.sv") {
    /// Find references at each location in crossfile_module.sv
    ServerHarness server("indexer_test");
    auto hdl = server.openFile("crossfile_module.sv");

    ReferencesScanner scanner(server);
    scanner.scanDocument(hdl);
}
