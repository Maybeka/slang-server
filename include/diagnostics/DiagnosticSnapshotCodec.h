// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

namespace server::diagnostic_snapshot {

class CodecError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Limits {
    uint64_t maxMetadataBytes = 64ull * 1024 * 1024;
    uint64_t maxFileBytes = 256ull * 1024 * 1024;
    uint64_t maxTotalFileBytes = 1024ull * 1024 * 1024;
    uint64_t maxStringBytes = 4ull * 1024 * 1024;
    uint64_t maxFiles = 100000;
    uint64_t maxEntries = 500000;
    uint32_t maxJsonDepth = 32;
};

struct OwnedCommentHandler {
    std::string directive;
    uint32_t kind = 0;
    std::string endRegion;
};
struct OwnedCommentHandlers {
    std::string prefix;
    std::vector<OwnedCommentHandler> handlers;
};
struct OwnedLexerOptions {
    std::vector<OwnedCommentHandlers> commentHandlers;
    uint32_t maxErrors = 16;
    uint32_t languageVersion = 1;
    bool enableLegacyProtect = false;
    bool allowMacroTrailingSpace = false;
};
struct OwnedKeywordMapping {
    std::string pattern;
    uint32_t version = 0;
};
struct OwnedPreprocessorOptions {
    uint32_t maxIncludeDepth = 1024;
    uint32_t languageVersion = 1;
    std::string predefineSource = "<api>";
    std::vector<std::string> predefines;
    std::vector<std::string> undefines;
    std::vector<std::string> additionalIncludePaths;
    std::vector<std::string> ignoreDirectives;
    std::vector<OwnedKeywordMapping> keywordMapping;
    bool allowMissingProtectedScopeEnd = false;
    bool captureUvmPrefix = false;
    std::string uvmPrefixRoot;
};
struct OwnedParserOptions {
    uint32_t maxRecursionDepth = 1024;
    uint32_t languageVersion = 1;
};
struct OwnedTimeScale {
    uint32_t baseUnit = 3;
    uint32_t baseMagnitude = 1;
    uint32_t precisionUnit = 3;
    uint32_t precisionMagnitude = 1;
};
struct OwnedCompilationOptions {
    uint32_t flags = 1u << 3;
    uint32_t maxInstanceDepth = 128;
    uint32_t maxCheckerInstanceDepth = 64;
    uint32_t maxGenerateSteps = 131072;
    uint32_t maxConstexprDepth = 128;
    uint32_t maxConstexprSteps = 1000000;
    uint32_t maxConstexprBacktrace = 10;
    uint64_t maxConstantSize = 8ull * 1024 * 1024;
    uint32_t maxDefParamSteps = 128;
    uint32_t maxDefParamBlocks = UINT32_MAX;
    uint32_t maxInstanceArray = 65535;
    uint32_t maxEnumValues = 65535;
    uint32_t maxRecursiveClassSpecialization = 8;
    uint32_t maxUDPCoverageNotes = 8;
    uint32_t errorLimit = 64;
    uint32_t typoCorrectionLimit = 32;
    uint32_t minTypMax = 1;
    uint32_t languageVersion = 1;
    std::optional<OwnedTimeScale> defaultTimeScale;
    std::vector<std::string> topModules;
    std::vector<std::string> paramOverrides;
    std::vector<std::string> defaultLiblist;
};
struct OwnedAnalysisOptions {
    uint32_t flags = 0;
    uint32_t maxCaseAnalysisSteps = 65535;
    uint32_t maxLoopAnalysisSteps = 65535;
};
struct OwnedSourceOptions {
    std::optional<uint32_t> numThreads;
    bool singleUnit = false;
    bool onlyLint = false;
    bool librariesInheritMacros = false;
};

// Every string is owned. Absence of a Bag entry remains absence after replay.
struct OwnedOptions {
    std::optional<OwnedLexerOptions> lexer;
    std::optional<OwnedPreprocessorOptions> preprocessor;
    std::optional<OwnedParserOptions> parser;
    std::optional<OwnedCompilationOptions> compilation;
    std::optional<OwnedAnalysisOptions> analysis;
    std::optional<OwnedSourceOptions> source;
};

// Throws for unknown Bag entries and any live preprocessor callback.
OwnedOptions captureOptions(const slang::Bag& options, const Limits& limits = {});

// Keep this object alive until every consumer of bag() (including SyntaxTrees
// and compilations that copy options) has been destroyed. It cannot move: a
// move of short strings could invalidate the reconstructed string_views.
class ReconstructedOptions {
public:
    explicit ReconstructedOptions(OwnedOptions options, const Limits& limits = {});
    ReconstructedOptions(const ReconstructedOptions&) = delete;
    ReconstructedOptions& operator=(const ReconstructedOptions&) = delete;
    ReconstructedOptions(ReconstructedOptions&&) = delete;
    ReconstructedOptions& operator=(ReconstructedOptions&&) = delete;
    const slang::Bag& bag() const { return bag_; }

private:
    const OwnedOptions owned_;
    slang::Bag bag_;
};

struct Input {
    slang::SourceManager::SourceSnapshot sources;
    OwnedOptions options;
    std::vector<OwnedOptions> treeOptions;
    slang::DiagnosticEngine::SettingsSnapshot settings;
};

// Private IPC protocol, for a child running the exact same server binary.
// Magic + little-endian u64 JSON length + strict UTF-8 JSON metadata + one
// little-endian u64 length/raw-byte frame per source file. File bytes include
// slang's terminating NUL and may contain any other bytes, including NUL.
// These functions neither consult the filesystem nor apply engine settings.
// readInput consumes one frame only; the caller owns EOF / enclosing framing.
void writeInput(std::ostream& stream, const Input& input, const Limits& limits = {});
Input readInput(std::istream& stream, const Limits& limits = {});

} // namespace server::diagnostic_snapshot
