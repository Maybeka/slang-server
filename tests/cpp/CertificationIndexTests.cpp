// SPDX-License-Identifier: MIT
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include "slang/ast/ASTDiagMap.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/AttributeSymbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"

using namespace slang;
using namespace slang::ast;

namespace {
struct AdmissionProvider {
    SourceManager sources;
    std::shared_ptr<Compilation> compilation;
    std::shared_ptr<const SourceManager::ImmutablePrefix> prefix;

    explicit AdmissionProvider(std::string_view text) {
        CompilationOptions options;
        options.flags = CompilationFlags::PackageAnalysis;
        compilation = std::make_shared<Compilation>(options);
        auto tree = syntax::SyntaxTree::fromText(text, sources);
        compilation->addSyntaxTree(tree);
        REQUIRE(compilation->getAllDiagnostics().empty());
        prefix = sources.captureImmutablePrefix(tree->getSourceBufferIds());
    }
};
} // namespace

TEST_CASE("Body admission visits only selected metadata across repeated RTL occurrences",
          "[scope-semantic][certification-index]") {
    const int count = GENERATE(1, 16, 64, 256);
    AdmissionProvider provider(R"(
module leaf;
  (* tag = 10 *) logic [3:0] value = 32'hfedcba98;
endmodule
module tile;
  leaf u();
  initial u.value = 1;
endmodule
)");
    SourceManager sources;
    sources.inheritImmutablePrefix(provider.prefix);
    std::string text = "module top;\n";
    for (int i = 0; i < count; ++i)
        text += fmt::format("tile u{}();\n", i);
    text += "endmodule\n";
    CompilationOptions options;
    options.topModules = {"top"};
    options.errorLimit = 0;
    Compilation compilation(options);
    compilation.enableInstanceBodyReferenceTracking(true);
    compilation.importDefinition(provider.compilation, "leaf", sources);
    compilation.importDefinition(provider.compilation, "tile", sources);
    compilation.addSyntaxTree(syntax::SyntaxTree::fromText(text, sources));
    const auto before = DiagnosticEngine::reportAll(sources, compilation.getAllDiagnostics());
    REQUIRE_FALSE(compilation.hasIssuedErrors());
    REQUIRE_FALSE(before.empty());
    const auto top = compilation.getRoot().topInstances.front();
    // First admit one tile while increasing unrelated sibling metadata.
    REQUIRE(compilation.prepareInstanceBodyReuse(top->body.find("u0")->as<InstanceSymbol>().body));
    const auto first = compilation.getBodyReuseIndexStats();
    CHECK(first.attributesVisited == 1);
    CHECK(first.effectsVisited == 1);
    CHECK(first.diagnosticsVisited >= 1);
    CHECK(first.diagnosticsVisited <= 3);
    for (int i = 1; i < count; ++i)
        REQUIRE(compilation.prepareInstanceBodyReuse(
            top->body.find(fmt::format("u{}", i))->as<InstanceSymbol>().body));
    const auto all = compilation.getBodyReuseIndexStats();
    CHECK(all.attributesIndexed == size_t(count));
    CHECK(all.attributesVisited == size_t(count));
    CHECK(all.effectsIndexed == size_t(count));
    CHECK(all.effectsVisited == size_t(count));
    CHECK(all.diagnosticsIndexed >= size_t(count));
    CHECK(all.diagnosticsVisited <= size_t(3 * count));
    CHECK(DiagnosticEngine::reportAll(sources, compilation.getAllDiagnostics()) == before);
    fmt::print("CERT_INDEX_WORK count={} attributesIndexed={} attributesVisited={} "
               "effectsIndexed={} effectsVisited={} diagnosticsIndexed={} diagnosticsVisited={} "
               "firstAttributesVisited={} firstEffectsVisited={} firstDiagnosticsVisited={}\n",
               count, all.attributesIndexed, all.attributesVisited, all.effectsIndexed,
               all.effectsVisited, all.diagnosticsIndexed, all.diagnosticsVisited,
               first.attributesVisited, first.effectsVisited, first.diagnosticsVisited);
}

TEST_CASE("Admission indexes observe replaced attributes and appended raw occurrences",
          "[scope-semantic][certification-index]") {
    AdmissionProvider provider("module child; (* tag = 1 *) logic value; endmodule\n");
    SourceManager sources;
    sources.inheritImmutablePrefix(provider.prefix);
    CompilationOptions options;
    options.topModules = {"top"};
    options.errorLimit = 0;
    Compilation compilation(options);
    compilation.enableInstanceBodyReferenceTracking(true);
    compilation.importDefinition(provider.compilation, "child", sources);
    compilation.addSyntaxTree(syntax::SyntaxTree::fromText(R"(
module top;
  child a(); child b(); child c();
endmodule
)",
                                                           sources));
    REQUIRE(compilation.getAllDiagnostics().empty());
    const auto& top = compilation.getRoot().topInstances.front()->body;
    const auto& a = top.find("a")->as<InstanceSymbol>().body;
    const auto& b = top.find("b")->as<InstanceSymbol>().body;
    const auto& c = top.find("c")->as<InstanceSymbol>().body;
    REQUIRE(compilation.prepareInstanceBodyReuse(a));
    // A lazy attachment can replace an earlier list while indexes are alive.
    // Its dependency is mutable and must decline only this candidate.
    auto expression = syntax::SyntaxTree::fromText("top.a.value", sources);
    auto attribute = compilation.emplace<AttributeSymbol>(
        "late", b.location, *b.find("value"), expression->root().as<syntax::ExpressionSyntax>());
    std::array<const AttributeSymbol*, 1> attributes{attribute};
    compilation.setAttributes(*b.find("value"),
                              compilation.copyFrom<const AttributeSymbol*>(attributes));
    REQUIRE_FALSE(compilation.prepareInstanceBodyReuse(b));
    // Append to an existing source group while admission indexes are alive.
    for (int i = 0; i < 128; ++i) {
        c.addDiag(diag::UndeclaredIdentifier, c.location) << std::string_view("late");
    }
    std::string_view reason;
    CHECK_FALSE(compilation.prepareInstanceBodyReuse(c, &reason));
    CHECK(reason == "mutable-diagnostic-context");
    CHECK(compilation.prepareInstanceBodyReuse(a));
}

TEST_CASE("Immutable origin lookup scales with projected entries and retains exact provenance",
          "[source-namespace][certification-index]") {
    const size_t count = GENERATE(16u, 256u, 2048u);
    SourceManager original;
    const auto definition = original.assignText("origin-definition.svh", "payload");
    const std::string text(count + 1, 'x');
    const auto invocation = original.assignText("origin-invocation.sv", text);
    const SourceLocation def(definition.id, 0), site(invocation.id, 0);
    std::vector<SourceLocation> expected;
    for (size_t i = 0; i < count; ++i)
        expected.push_back(original.createExpansionLoc(def, {site + i, site + i + 1}, "PAYLOAD"));
    const auto prefix = original.captureImmutablePrefix(std::vector{definition.id, invocation.id});
    SourceManager current;
    current.inheritImmutablePrefix(prefix);
    const auto newDefinition = current.assignQueryText("origin-definition.svh", "payload");
    const auto newInvocation = current.assignQueryText("origin-invocation.sv", text);
    const SourceLocation newDef(newDefinition.id, 0), newSite(newInvocation.id, 0);
    std::vector<SourceLocation> locations;
    for (size_t i = 0; i < count; ++i)
        locations.push_back(
            current.createExpansionLoc(newDef, {newSite + i, newSite + i + 1}, "PAYLOAD"));
    CHECK(current.getOriginalLocationIndexStats().builds == 0);
    CHECK(current.projectImmutableOriginalLocations(locations) == expected);
    const auto first = current.getOriginalLocationIndexStats();
    CHECK(first.builds == 1);
    CHECK(first.entries == count + 2);
    CHECK(first.candidates == count + 2);
    CHECK(current.projectImmutableOriginalLocations(locations) == expected);
    const auto second = current.getOriginalLocationIndexStats();
    CHECK(second.builds == 1);
    CHECK(second.candidates == 2 * first.candidates);
    CHECK_FALSE(current.projectImmutableOriginalLocation(
        current.createExpansionLoc(newDef, {newSite, newSite + 1}, "OTHER")));
    fmt::print("ORIGIN_INDEX_WORK count={} builds={} entries={} candidates={}\n", count,
               first.builds, first.entries, first.candidates);
}

TEST_CASE("Raw diagnostic handles survive growth and expose coalescing reorder revisions",
          "[certification-index]") {
    SourceManager sources;
    const auto input = sources.assignText("index.sv", "module m; endmodule");
    ASTDiagMap map;
    ASTDiagMap::RawReference first;
    bool added;
    map.add(Diagnostic(diag::Redefinition, {input.id, 0}), added, &first)
        .args.emplace_back("first");
    for (size_t i = 0; i < 1024; ++i) {
        auto& diagnostic = map.add(Diagnostic(diag::Redefinition, {input.id, i % 17}), added);
        diagnostic.args.emplace_back(std::to_string(i));
    }
    CHECK(std::get<std::string>(map.resolveRaw(first).args.front()) == "first");
    const auto revision = map.rawOrderRevision();
    (void)map.coalesce(&sources);
    CHECK(map.rawOrderRevision() != revision);
    size_t occurrences = 0;
    map.visitRawReferences([&](const auto& reference, const Diagnostic& diagnostic) {
        CHECK(&map.resolveRaw(reference) == &diagnostic);
        ++occurrences;
    });
    CHECK(occurrences == 1025);
}
