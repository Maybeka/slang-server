// SPDX-License-Identifier: MIT
#include "document/BodyDiagnosticCache.h"

#include "ast/ServerCompilation.h"
#include "ast/TrustedUvmLibrary.h"
#include "document/NativeDiagnosticSnapshot.h"
#include "util/Converters.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <set>

#include "slang/analysis/ValueDriver.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/diagnostics/AnalysisDiags.h"
#include "slang/diagnostics/GeneralDiags.h"
#include "slang/diagnostics/LexerDiags.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/ScopeGuard.h"

namespace server {
using namespace slang;
namespace {
void require(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
bool contained(const ast::Symbol* symbol, const ast::SubroutineSymbol* fn) {
    for (; symbol;) {
        if (symbol == fn)
            return true;
        auto parent = symbol->getParentScope();
        symbol = parent ? &parent->asSymbol() : nullptr;
    }
    return false;
}
bool outerMethodCheck(const Diagnostic& d, const ast::SubroutineSymbol* fn) {
    if (d.symbol != fn)
        return false;
    return d.code == diag::UnusedClassMethod || d.code == diag::UnusedLocalClassMethod ||
           d.code == diag::UnusedConstructor || d.code == diag::ShadowHierarchy ||
           d.code == diag::ShadowValue;
}
void bindUris(ServerDiagClient& output, ServerDriver& driver) {
    output.setUriResolver([&driver](const URI& physical) {
        if (driver.isDocumentOpen(physical))
            return physical;
        for (const auto& [key, doc] : driver.docs)
            if (driver.isDocumentOpen(key) &&
                driver.sm.getFullPath(doc->getBuffer()).generic_string() == physical.getPath())
                return key;
        return physical;
    });
}
// Capture the actual publication URI selected by the diagnostic engine, including
// macro provenance and editor aliases. The scratch client never publishes.
struct DiagnosticBuckets : ServerDiagClient {
    size_t phase = 0;
    std::array<std::unordered_map<URI, Diagnostics>, 3> buckets;
    DiagnosticBuckets(ServerDriver& d) : ServerDiagClient(d.sm, d.client) {
        disableDestructorPublication();
        bindUris(*this, d);
    }
    void report(const ReportedDiagnostic& diagnostic) override {
        clear();
        ServerDiagClient::report(diagnostic);
        for (const auto& [key, values] : renderedDiagnostics())
            if (!values.empty())
                buckets[phase][key].push_back(diagnostic.originalDiagnostic);
    }
};
struct InitializerCalls : ast::ASTVisitor<InitializerCalls, ast::VisitFlags::AllGood> {
    void handle(const ast::CallExpression& call) {
        if (!call.isSystemCall() && !call.thisClass()) {
            auto fn = std::get<const ast::SubroutineSymbol*>(call.subroutine);
            require(fn->getParentScope()->asSymbol().kind == ast::SymbolKind::ClassType &&
                        !fn->flags.has(ast::MethodFlags::Static),
                    "external initializer calls require driver dependency analysis");
        }
        visitDefault(call);
    }
};
struct LocalBoundaries : ast::ASTVisitor<LocalBoundaries, ast::VisitFlags::AllGood> {
    void handle(const ast::VariableSymbol& variable) {
        if (auto initializer = variable.getInitializer()) {
            InitializerCalls calls;
            initializer->visit(calls);
        }
        visitDefault(variable);
    }
    void handle(const ast::HierarchicalValueExpression&) {
        require(false, "hierarchical method references require full diagnostics");
    }
};
std::vector<std::string> rendered(const Diagnostics& diagnostics, SourceManager& sm,
                                  SlangLspClient& client,
                                  const DiagnosticEngine::SettingsSnapshot& settings) {
    auto output = std::make_shared<ServerDiagClient>(sm, client);
    output->disableDestructorPublication();
    DiagnosticEngine engine(sm);
    engine.applySettings(settings);
    engine.setMappingsFromPragmas();
    engine.addClient(output);
    for (const auto& d : diagnostics)
        engine.issue(d);
    std::vector<std::string> result;
    for (const auto& [uri, values] : output->renderedDiagnostics())
        for (const auto& value : values)
            result.push_back(uri.str() + rfl::json::write(value));
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace

std::shared_ptr<const BodyDiagnosticBaseline> BodyDiagnosticCache::capture(
    ServerDriver& d, const URI& uri, size_t start, size_t stop, const lsp::RequestContext& ctx) {
    ctx.checkpoint("before method diagnostic capture");
    auto baseline = std::make_shared<BodyDiagnosticBaseline>(uri);
    baseline->driver = &d;
    baseline->context = d.m_snapshotOwner;
    baseline->begin = start;
    baseline->end = stop;
    baseline->bufferLimit = uint32_t(d.sm.getSourceLocationEntryCount());
    baseline->settings = d.diagEngine.captureSettings();
    baseline->analysisOptions = d.options.getOrDefault<analysis::AnalysisOptions>();
    require(d.comp && d.m_fullDiagnosticGeneration &&
                d.m_fullDiagnosticGeneration == d.m_diagnosticGeneration &&
                d.comp->containsCurrentSource(d.sm.getFullPath(d.getDocument(uri)->getBuffer())),
            "local diagnostics require a completed current full-design baseline");
    for (auto& key : d.m_openDocs)
        require(!d.hasExplicitContextError(key) && d.comp->containsCurrentSource(d.sm.getFullPath(
                                                       d.getDocument(key)->getBuffer())),
                "open document outside full-design diagnostic coverage");
    require(baseline->settings.errorLimit == 0, "bounded diagnostic streams require full analysis");
    auto design = d.comp->createActiveDesignContext({});
    auto owner = design.retainCompilation();
    baseline->owner = owner;
    require(!owner->hasFatalErrors(), "fatal baseline diagnostics require native analysis");
    auto& manager = design.getAnalysis().getAnalysisManager(ctx);
    auto path = d.sm.getFullPath(d.getDocument(uri)->getBuffer());
    for (auto fn : manager.getAnalyzedSubroutines()) {
        auto syntax = fn->getSyntax();
        if (!syntax)
            continue;
        auto first = syntax->getFirstToken().location();
        if (!d.sm.isFileLoc(first) || first.offset() != start ||
            d.sm.getFullPath(first.buffer()) != path)
            continue;
        require(fn->getParentScope()->asSymbol().kind == ast::SymbolKind::ClassType &&
                    !fn->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor),
                "local diagnostics currently require nonstatic class methods");
        require(fn->getCompilation().hasFlag(
                    ast::CompilationFlags::TrackSubroutineBodyDependencies) &&
                    !fn->bodyHasExternalDependents.load(std::memory_order_relaxed),
                "method has external elaboration dependencies");
        baseline->originals.push_back(fn);
    }
    require(!baseline->originals.empty(), "method absent from full-design analysis");
    require(baseline->originals.size() == 1,
            "multiple full-design method contexts require full diagnostics");

    // replaceBuffer clears the editor BufferID's FileInfo even while old FileData
    // is pinned. Keep one target-file value, not a chain of edited source owners.
    baseline->text = std::string(d.getDocument(uri)->getText());
    auto publication = d.m_nativeDiagnosticSnapshot;
    UvmPhaseTrace capturePhases;
    capturePhases.mark(!publication ? "body-native-publication-missing"
                       : publication->generation != d.m_fullDiagnosticGeneration
                           ? "body-native-publication-generation-changed"
                       : publication->context.lock() != d.m_snapshotOwner
                           ? "body-native-publication-context-changed"
                       : publication->owner.lock() != owner
                           ? "body-native-publication-owner-changed"
                       : publication->config != rfl::json::write(d.m_config)
                           ? "body-native-publication-config-changed"
                           : "body-native-publication-matched");
    if (publication && publication->generation == d.m_fullDiagnosticGeneration &&
        publication->context.lock() == d.m_snapshotOwner && publication->owner.lock() == owner &&
        publication->config == rfl::json::write(d.m_config)) {
        baseline->publication = std::move(publication);
        ctx.checkpoint("native diagnostic routes reused");
    }

    if (!baseline->publication) {
        for (const auto& tree : owner->getSyntaxTrees())
            baseline->diagnostics[0].append_range(tree->diagnostics());
        baseline->diagnostics[1].append_range(owner->getSemanticDiagnostics());
        baseline->diagnostics[2].append_range(manager.getDiagnostics());
    }
    ctx.checkpoint("after method diagnostic capture");
    // Transfer the raw publication into this one pending certificate. Once it
    // becomes retained buckets or is declined, no second project-sized copy stays.
    if (baseline->publication)
        d.m_nativeDiagnosticSnapshot.reset();
    return baseline;
}

BodyDiagnosticCache::BodyDiagnosticCache(ServerDriver& d, const URI& target, size_t start,
                                         size_t stop,
                                         const std::shared_ptr<syntax::SyntaxTree>& tree,
                                         bool sensitive, const lsp::RequestContext& ctx,
                                         std::shared_ptr<const BodyDiagnosticBaseline> saved,
                                         const std::shared_ptr<syntax::SyntaxTree>& current) :
    driver(d), uri(target), begin(start), end(stop), lineSensitive(sensitive),
    baselineLine(d.sm.getRawLineNumber(SourceLocation(tree->getSourceBufferIds().front(), stop))) {
    UvmPhaseTrace phases;
    auto baseline = saved ? std::move(saved) : capture(d, target, start, stop, ctx);
    require(baseline->driver == &d && baseline->context.lock() == d.m_snapshotOwner &&
                baseline->uri == uri && baseline->begin == start && baseline->end == stop,
            "original diagnostic baseline belongs to another context");
    bufferLimit = baseline->bufferLimit;
    settings = baseline->settings;
    analysisOptions = baseline->analysisOptions;
    owner = baseline->owner;
    originals = baseline->originals;
    auto path = d.sm.getFullPath(d.getDocument(uri)->getBuffer());
    // Macro locations frequently revisit the same physical buffers. Resolve each
    // buffer's path once during this construction, including synthetic buffers.
    std::unordered_map<BufferID, bool> targetBuffers;
    auto targetLocation = [&](SourceLocation loc) {
        if (!loc)
            return false;
        auto [it, inserted] = targetBuffers.try_emplace(loc.buffer(), false);
        if (inserted)
            it->second = d.sm.getFullPath(loc.buffer()) == path;
        return it->second;
    };
    auto local = [&](const Diagnostic& diagnostic) {
        auto loc = d.sm.getFullyExpandedLoc(diagnostic.location);
        if (targetLocation(loc) && begin <= loc.offset() && loc.offset() < end &&
            !std::ranges::any_of(originals,
                                 [&](auto fn) { return outerMethodCheck(diagnostic, fn); }))
            return true;
        for (auto fn : originals)
            if (contained(diagnostic.symbol, fn) && !outerMethodCheck(diagnostic, fn))
                return true;
        return false;
    };
    Diagnostics baselineDiagnostics;
    std::array<Diagnostics, 3> retainedDiagnostics;
    const auto& raw = baseline->publication ? baseline->publication->diagnostics
                                            : baseline->diagnostics;
    for (size_t phase = 0; phase < raw.size(); ++phase)
        for (size_t index = 0; index < raw[phase].size(); ++index) {
            ctx.checkpoint("local diagnostic publication certificate");
            const auto& diagnostic = raw[phase][index];
            auto loc = d.sm.getFullyExpandedLoc(diagnostic.location);
            bool belongs = phase == 0
                               ? targetLocation(loc) && begin <= loc.offset() && loc.offset() < end
                               : local(diagnostic);
            if (belongs)
                baselineDiagnostics.push_back(diagnostic);
            else if (baseline->publication) {
                for (const auto& route : baseline->publication->routes[phase][index])
                    retained[phase][d.diagClient->resolvePublicationUri(route)].push_back(
                        diagnostic);
            }
            else
                retainedDiagnostics[phase].push_back(diagnostic);
        }
    phases.mark("body-diagnostic-partition");
    auto initial = analyze(tree, ctx);
    auto expected = rendered(baselineDiagnostics, d.sm, d.client, settings);
    Diagnostics combined;
    for (auto& phase : initial.diagnostics)
        combined.append_range(phase);
    auto actual = rendered(combined, d.sm, d.client, settings);
    require(expected == actual, "detached diagnostic baseline differs from full design");
    phases.mark("body-diagnostic-compare");
    baselineBinding = initial.compilation;
    specializations = std::move(initial.specializations);
    references = std::move(initial.references);
    calls = std::move(initial.calls);
    effectSensitiveCalls = std::move(initial.effectSensitiveCalls);
    // Test current dependency / reference eligibility before formatting all retained
    // diagnostics. Unsupported first edits should not pay for a baseline we discard.
    if (current)
        validateSlice(analyze(current, ctx));
    phases.mark("body-diagnostic-eligibility");
    if (!baseline->publication) {
        auto collector = std::make_shared<DiagnosticBuckets>(d);
        DiagnosticEngine engine(d.sm);
        engine.applySettings(settings);
        engine.setMappingsFromPragmas();
        engine.addClient(collector);
        for (size_t phase = 0; phase < retainedDiagnostics.size(); ++phase) {
            collector->phase = phase;
            for (const auto& diagnostic : retainedDiagnostics[phase]) {
                ctx.checkpoint("local diagnostic bucket formatting");
                engine.issue(diagnostic);
            }
        }
        retained = std::move(collector->buckets);
    }
    else
        ctx.checkpoint("native diagnostic buckets reused");
    phases.mark(baseline->publication ? "body-diagnostic-native-routes"
                                      : "body-diagnostic-formatted-routes");
    affectedUris.insert(uri);
    const auto bodyStart = originals.front()
                               ->getSyntax()
                               ->as<syntax::FunctionDeclarationSyntax>()
                               .semi.range()
                               .end()
                               .offset();
    for (const auto& phase : retained)
        for (const auto& [key, diagnostics] : phase)
            for (const auto& diagnostic : diagnostics) {
                bool affected = false;
                auto location = [&](SourceLocation loc) {
                    for (auto actual :
                         {d.sm.getFullyExpandedLoc(loc), d.sm.getFullyOriginalLoc(loc)})
                        if (targetLocation(actual)) {
                            affected = true;
                            require(actual.offset() <= bodyStart || actual.offset() >= end,
                                    "outside diagnostic references the replaced body");
                        }
                };
                auto inspect = [&](auto&& self, const Diagnostic& value) -> void {
                    location(value.location);
                    for (auto range : value.ranges) {
                        location(range.start());
                        location(range.end());
                    }
                    for (auto& note : value.notes)
                        self(self, note);
                };
                inspect(inspect, diagnostic);
                if (affected)
                    affectedUris.insert(key);
            }
    phases.mark("body-diagnostic-affected");
    ctx.checkpoint("after local diagnostic baseline");
}

BodyDiagnosticCache::Slice BodyDiagnosticCache::analyze(
    const std::shared_ptr<syntax::SyntaxTree>& tree, const lsp::RequestContext& ctx) {
    Slice result;
    result.compilation = std::make_shared<ast::Compilation>(Bag{owner->getOptions()});
    auto& compilation = *result.compilation;
    compilation.setBodyQueryContext(owner, tree);
    ast::BodyQueryReferenceGuard referenceGuard(compilation);
    const syntax::FunctionDeclarationSyntax* syntax = nullptr;
    auto find = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        if (auto fn = node.as_if<syntax::FunctionDeclarationSyntax>()) {
            require(!syntax, "multiple detached diagnostic methods");
            syntax = fn;
            return;
        }
        for (uint32_t i = 0; i < node.getChildCount(); ++i)
            if (auto child = node.childNode(i))
                self(self, *child);
    };
    find(find, tree->root());
    require(syntax, "missing detached diagnostic method");
    ast::BodyQueryEvaluationGuard guard(originals);
    std::vector<const ast::SubroutineSymbol*> roots;
    for (auto original : originals) {
        require(!original->bodyHasExternalDependents.load(std::memory_order_relaxed),
                "method acquired external elaboration dependencies");
        auto& fn = ast::SubroutineSymbol::createBodyQuery(compilation, *syntax, *original);
        compilation.forceElaborate(fn);
        LocalBoundaries boundaries;
        fn.visit(boundaries);
        auto callEffects = ast::makeVisitor([&](auto& visitor, const ast::CallExpression& call) {
            // A safe outer call must not hide a package/static/task call in its
            // receiver or actual arguments, including system-call arguments.
            visitor.visitDefault(call);
            if (call.isSystemCall())
                return;
            const auto target = std::get<const ast::SubroutineSymbol*>(call.subroutine);
            const auto parent = target->getParentScope();
            if (parent && parent->asSymbol().kind == ast::SymbolKind::ClassType &&
                !target->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor) &&
                target->subroutineKind == ast::SubroutineKind::Function)
                return;
            if (const auto node = target->getSyntax())
                result.effectSensitiveCalls.insert(node);
            if (const auto prototype = target->getPrototype())
                if (const auto node = prototype->getSyntax())
                    result.effectSensitiveCalls.insert(node);
        });
        fn.visit(callEffects);
        roots.push_back(&fn);
        ctx.checkpoint("after local diagnostic binding");
    }
    // Do not permit new generic instantiations, imports, local classes or checker
    // contexts to change global specialization / elaboration state in this slice.
    std::vector<std::string> specialSyntax;
    std::unordered_set<std::string> localNames, parameterNames;
    auto parameterTokens = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        for (uint32_t i = 0; i < node.getChildCount(); ++i) {
            if (auto child = node.childNode(i))
                self(self, *child);
            else if (auto token = node.childToken(i);
                     token && token.kind == parsing::TokenKind::Identifier)
                parameterNames.emplace(token.valueText());
        }
    };
    auto inspect = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        require(node.kind != syntax::SyntaxKind::ClassDeclaration &&
                    node.kind != syntax::SyntaxKind::PackageImportDeclaration &&
                    node.kind != syntax::SyntaxKind::CheckerDeclaration &&
                    node.kind != syntax::SyntaxKind::CheckerInstanceStatement &&
                    node.kind != syntax::SyntaxKind::CheckerInstantiation &&
                    node.kind != syntax::SyntaxKind::ConcurrentAssertionMember &&
                    node.kind != syntax::SyntaxKind::SequenceDeclaration &&
                    node.kind != syntax::SyntaxKind::PropertyDeclaration &&
                    node.kind != syntax::SyntaxKind::LetDeclaration &&
                    node.kind != syntax::SyntaxKind::TypedefDeclaration &&
                    node.kind != syntax::SyntaxKind::EnumType &&
                    node.kind != syntax::SyntaxKind::StructType &&
                    node.kind != syntax::SyntaxKind::UnionType,
                "local nominal declarations or imports require full diagnostics");
        if (auto declaration = node.as_if<syntax::DeclaratorSyntax>())
            localNames.emplace(declaration->name.valueText());
        if (node.kind == syntax::SyntaxKind::ParameterValueAssignment) {
            specialSyntax.push_back(node.toString());
            parameterTokens(parameterTokens, node);
        }
        for (uint32_t i = 0; i < node.getChildCount(); ++i)
            if (auto child = node.childNode(i))
                self(self, *child);
    };
    inspect(inspect, *syntax);
    for (const auto& name : parameterNames)
        require(!localNames.contains(name),
                "generic arguments depend on method-local declarations");
    result.calls = std::move(specialSyntax);
    std::sort(result.calls.begin(), result.calls.end());
    auto local = [&](const Diagnostic& diagnostic) {
        auto loc = driver.sm.getFullyExpandedLoc(diagnostic.location);
        if (loc && loc.buffer() == tree->getSourceBufferIds().front() && begin <= loc.offset())
            return true;
        return std::ranges::any_of(roots,
                                   [&](auto fn) { return contained(diagnostic.symbol, fn); });
    };
    for (auto& diagnostic : tree->diagnostics())
        if (diagnostic.code != diag::NewlineEOF)
            result.diagnostics[0].push_back(diagnostic);
    for (auto& diagnostic : compilation.getBodyQueryDiagnostics()) {
        require(local(diagnostic), "detached body generates a nonlocal semantic diagnostic");
        result.diagnostics[1].push_back(diagnostic);
    }
    for (auto type : compilation.getImportedClassSpecializations()) {
        auto cls = type->as_if<ast::ClassType>();
        require(cls && cls->genericClass, "unsupported detached specialization");
        for (auto parameter : cls->genericParameters) {
            if (auto value = parameter->as_if<ast::ParameterSymbol>())
                require(!value->getValue().bad() && !value->getType().isError(),
                        "invalid generic values require full diagnostics");
            else
                require(!parameter->as<ast::TypeParameterSymbol>().targetType.getType().isError(),
                        "invalid generic types require full diagnostics");
        }
        result.specializations.push_back(cls);
    }
    require(!compilation.hasFatalErrors(), "fatal body diagnostics require full analysis");
    compilation.freeze();
    auto unfreeze = ScopeGuard([&] { compilation.unfreeze(); });
    for (auto fn : roots) {
        analysis::AnalysisManager manager(analysisOptions);
        manager.addListener([&](const analysis::AnalyzedProcedure&) {
            ctx.checkpoint("after local diagnostic procedure");
        });
        manager.analyzeBody(compilation, *fn);
        auto procedure = manager.getAnalyzedSubroutine(*fn);
        require(procedure, "missing local diagnostic procedure");
        for (auto driver : procedure->getDrivers())
            require(contained(&driver->getSymbol(), fn),
                    "nonlocal persistent drivers require full diagnostics");
        if (fn->subroutineKind == ast::SubroutineKind::Task) {
            auto proc = manager.getAnalyzedSubroutine(*fn);
            require(proc && proc->getTimingControls().empty(),
                    "task timing changes require caller analysis");
            for (auto call : proc->getCallExpressions()) {
                require(call->getSubroutineKind() != ast::SubroutineKind::Task,
                        "task calls require transitive timing analysis");
            }
        }
        for (auto& diagnostic : manager.getDiagnostics())
            if (local(diagnostic))
                result.diagnostics[2].push_back(diagnostic);
        ctx.checkpoint("after local diagnostic dataflow");
    }
    auto internalSyntax = [&](const syntax::SyntaxNode* node) {
        for (; node; node = node->parent) {
            if (node == syntax)
                return true;
            for (auto fn : originals)
                if (node == fn->getSyntax())
                    return true;
        }
        return false;
    };
    for (auto& [node, usage] : compilation.getBodyQueryReferences())
        if (!internalSyntax(node))
            result.references.emplace(node, usage);
    return result;
}

void BodyDiagnosticCache::validateSlice(const Slice& slice) const {
    if (slice.references != references) {
        // A fixed provider's exported use exists independently of this editable
        // caller. Changing a bit already set by that provider cannot change the
        // full-design read/write status. Do not use isReferenced here: later
        // navigation can add uses to a provider's separate mutable overlay.
        auto equivalent = [&](const syntax::SyntaxNode* node, std::pair<bool, bool> before,
                              std::pair<bool, bool> after) {
            if (before == after)
                return true;
            require(!effectSensitiveCalls.contains(node) &&
                        !slice.effectSensitiveCalls.contains(node),
                    "changed package/static/task calls require full effect analysis");
            std::pair<bool, bool> fixed{};
            for (const auto& library : {driver.m_trustedUvm, driver.m_trustedScopes})
                if (library && library->compilation) {
                    auto usage = library->compilation->getExportedReferenceUsage(*node);
                    fixed.first |= usage.first;
                    fixed.second |= usage.second;
                }
            return (before.first == after.first || fixed.first) &&
                   (before.second == after.second || fixed.second);
        };
        for (const auto& [node, before] : references) {
            auto found = slice.references.find(node);
            require(equivalent(node, before,
                               found == slice.references.end() ? std::pair<bool, bool>{}
                                                               : found->second),
                    "method changes external reference usage");
        }
        for (const auto& [node, after] : slice.references)
            if (!references.contains(node))
                require(equivalent(node, {}, after), "method changes external reference usage");
    }
    require(slice.calls == calls, "method changes specialization or declaration dependencies");
    require(
        slice.specializations.size() == specializations.size() &&
            std::ranges::all_of(
                slice.specializations,
                [&](auto current) {
                    return std::ranges::any_of(specializations, [&](auto previous) {
                        return current->genericClass == previous->genericClass &&
                               ast::ParameterSymbolBase::allMatching(current->genericParameters,
                                                                     previous->genericParameters) &&
                               std::ranges::equal(
                                   current->genericParameters, previous->genericParameters,
                                   [](auto a, auto b) {
                                       if (auto value = a->template as_if<ast::ParameterSymbol>()) {
                                           auto& other = b->template as<ast::ParameterSymbol>();
                                           const auto& left = value->getValue();
                                           const auto& right = other.getValue();
                                           if (left.isInteger() && right.isInteger() &&
                                               (left.integer().getBitWidth() !=
                                                    right.integer().getBitWidth() ||
                                                left.integer().isSigned() !=
                                                    right.integer().isSigned()))
                                               return false;
                                           return value->getType().isMatching(other.getType());
                                       }
                                       return true;
                                   });
                    });
                }),
        "method changes evaluated specialization parameters");
    const auto errorLimit = owner->getOptions().errorLimit;
    require(!errorLimit || owner->getIssuedErrorCount() + slice.compilation->getIssuedErrorCount() <
                               errorLimit,
            "method may exceed full-design semantic error limit");
    for (auto& phase : slice.diagnostics)
        for (auto& diagnostic : phase)
            require(driver.diagEngine.getSeverity(diagnostic.code, diagnostic.location) !=
                        DiagnosticSeverity::Fatal,
                    "fatal diagnostics require full analysis");
}

void BodyDiagnosticCache::publish(const std::shared_ptr<syntax::SyntaxTree>& tree, size_t newEnd,
                                  const lsp::RequestContext& ctx,
                                  const std::function<void()>& validate) {
    require(!lineSensitive || baselineLine == driver.sm.getRawLineNumber(SourceLocation(
                                                  tree->getSourceBufferIds().front(), newEnd)),
            "line shift affects diagnostics in later macro bodies");
    auto slice = analyze(tree, ctx);
    validateSlice(slice);
    auto document = driver.getDocument(uri);
    BodySourceProjection projection(driver.sm, document->getBuffer(),
                                    tree->getSourceBufferIds().front(), end, newEnd, bufferLimit);
    auto cwd = driver.sm.beginFastCwdPhase();
    auto output = std::make_shared<ServerDiagClient>(driver.sm, driver.client);
    output->disableDestructorPublication();
    bindUris(*output, driver);
    DiagnosticEngine engine(driver.sm);
    engine.applySettings(settings);
    engine.setMappingsFromPragmas();
    engine.addClient(output);
    // Discover any newly emitted macro diagnostic URI before selecting its
    // retained siblings. Only these publication buckets need to be reformatted.
    for (const auto& phase : slice.diagnostics)
        for (const auto& diagnostic : phase)
            engine.issue(diagnostic);
    auto changed = affectedUris;
    for (const auto& [key, _] : output->renderedDiagnostics())
        changed.insert(key);
    output->clear();
    for (size_t phase = 0; phase < retained.size(); ++phase) {
        Diagnostics diagnostics = slice.diagnostics[phase];
        for (const auto& key : changed)
            if (auto found = retained[phase].find(key); found != retained[phase].end())
                diagnostics.append_range(found->second);
        std::ranges::stable_sort(diagnostics, [&](const auto& a, const auto& b) {
            auto key = [&](const auto& d) {
                auto loc = driver.sm.getFullyExpandedLoc(d.location);
                auto pos = toPosition(loc, driver.sm);
                return std::tuple(driver.sm.getFullPath(loc.buffer()).string(), pos.line,
                                  pos.character, d.code);
            };
            return key(a) < key(b);
        });
        for (auto& diagnostic : diagnostics) {
            ctx.checkpoint("retained local diagnostic publication");
            engine.issue(diagnostic);
        }
    }
    validate();
    ctx.checkpoint("before local diagnostic publication");
    cwd.finish();
    auto update = driver.diagClient->beginUpdate();
    for (const auto& key : changed) {
        auto found = output->renderedDiagnostics().find(key);
        driver.diagClient->replaceRendered(key, found == output->renderedDiagnostics().end()
                                                    ? std::vector<lsp::Diagnostic>{}
                                                    : found->second);
    }
    for (const auto& [key, doc] : driver.docs)
        driver.diagClient->setVersion(key, doc->getVersion());
    update.commit();
    driver.diagClient->pushDiags(uri);
    affectedUris = std::move(changed);
    driver.m_fullDiagnosticGeneration = driver.m_diagnosticGeneration;
    driver.m_pendingIncludedDiagnostics.clear();
    ++updates;
}
} // namespace server
