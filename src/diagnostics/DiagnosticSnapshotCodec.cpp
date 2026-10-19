// SPDX-License-Identifier: MIT
#include "diagnostics/DiagnosticSnapshotCodec.h"

#include <algorithm>
#include <array>
#include <climits>
#include <istream>
#include <limits>
#include <memory>
#include <ostream>
#include <rfl/NoExtraFields.hpp>
#include <rfl/json.hpp>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include "slang/analysis/AnalysisOptions.h"
#include "slang/ast/Compilation.h"
#include "slang/driver/SourceLoader.h"
#include "slang/parsing/Lexer.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"

namespace server::diagnostic_snapshot {
namespace {

using namespace slang;
constexpr std::string_view Magic = "SLANGDG1";
constexpr uint32_t CodecVersion = 1;

[[noreturn]] void fail(std::string_view message) {
    throw CodecError("diagnostic snapshot: " + std::string(message));
}

void bounded(uint64_t value, uint64_t limit, std::string_view what) {
    if (value > limit)
        fail(what);
}

// JSON metadata must be UTF-8. Source bytes never pass through this function.
bool validUtf8(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80)
            continue;
        unsigned count;
        uint32_t code;
        uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            code = c & 0x1f;
            minimum = 0x80;
        }
        else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            code = c & 0x0f;
            minimum = 0x800;
        }
        else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            code = c & 0x07;
            minimum = 0x10000;
        }
        else {
            return false;
        }
        if (text.size() - i < count)
            return false;
        for (unsigned n = 0; n < count; ++n) {
            auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}

void stringOk(std::string_view value, const Limits& limits, bool path = false) {
    bounded(value.size(), limits.maxStringBytes, "metadata string limit exceeded");
    if (!validUtf8(value))
        fail("non-UTF-8 metadata is unsupported");
    if (path && value.find('\0') != std::string_view::npos)
        fail("NUL in a metadata path is unsupported");
}

template<typename Range>
void countOk(const Range& range, const Limits& limits) {
    bounded(range.size(), limits.maxEntries, "metadata entry limit exceeded");
}

void stringsOk(const std::vector<std::string>& strings, const Limits& limits, bool path = false,
               bool unique = false) {
    countOk(strings, limits);
    std::unordered_set<std::string_view> seen;
    for (const auto& value : strings) {
        stringOk(value, limits, path);
        if (unique && !seen.insert(value).second)
            fail("duplicate entry in a set");
    }
}

void languageOk(uint32_t language) {
    if (language > static_cast<uint32_t>(LanguageVersion::v1800_2023))
        fail("unsupported language version");
}

void validateOptions(const OwnedOptions& options, const Limits& limits) {
    if (options.lexer) {
        const auto& value = *options.lexer;
        languageOk(value.languageVersion);
        countOk(value.commentHandlers, limits);
        std::unordered_set<std::string_view> prefixes;
        uint64_t total = 0;
        for (const auto& group : value.commentHandlers) {
            stringOk(group.prefix, limits);
            if (!prefixes.insert(group.prefix).second)
                fail("duplicate comment handler prefix");
            countOk(group.handlers, limits);
            total += group.handlers.size();
            bounded(total, limits.maxEntries, "comment handler limit exceeded");
            std::unordered_set<std::string_view> directives;
            for (const auto& entry : group.handlers) {
                stringOk(entry.directive, limits);
                stringOk(entry.endRegion, limits);
                if (entry.kind > static_cast<uint32_t>(parsing::CommentHandler::LintRestore))
                    fail("unsupported comment handler kind");
                if (!directives.insert(entry.directive).second)
                    fail("duplicate comment handler directive");
            }
        }
    }
    if (options.preprocessor) {
        const auto& value = *options.preprocessor;
        languageOk(value.languageVersion);
        stringOk(value.predefineSource, limits);
        stringOk(value.uvmPrefixRoot, limits);
        stringsOk(value.predefines, limits);
        stringsOk(value.undefines, limits);
        stringsOk(value.additionalIncludePaths, limits, true);
        stringsOk(value.ignoreDirectives, limits, false, true);
        countOk(value.keywordMapping, limits);
        for (const auto& entry : value.keywordMapping) {
            stringOk(entry.pattern, limits, true);
            if (entry.version > static_cast<uint32_t>(parsing::KeywordVersion::v1800_2023))
                fail("unsupported keyword version");
        }
    }
    if (options.parser)
        languageOk(options.parser->languageVersion);
    if (options.compilation) {
        const auto& value = *options.compilation;
        languageOk(value.languageVersion);
        if (value.flags & ~bitmask<ast::CompilationFlags>::mask_value)
            fail("unsupported compilation flags");
        if (value.minTypMax > static_cast<uint32_t>(ast::MinTypMax::Max))
            fail("unsupported min/typ/max choice");
        if (value.defaultTimeScale) {
            const auto& time = *value.defaultTimeScale;
            auto magnitudeOk = [](uint32_t magnitude) {
                return magnitude == 1 || magnitude == 10 || magnitude == 100;
            };
            if (time.baseUnit > static_cast<uint32_t>(TimeUnit::Femtoseconds) ||
                time.precisionUnit > static_cast<uint32_t>(TimeUnit::Femtoseconds) ||
                !magnitudeOk(time.baseMagnitude) || !magnitudeOk(time.precisionMagnitude))
                fail("unsupported time scale");
        }
        stringsOk(value.topModules, limits, false, true);
        stringsOk(value.paramOverrides, limits);
        stringsOk(value.defaultLiblist, limits);
    }
    if (options.analysis &&
        (options.analysis->flags & ~bitmask<analysis::AnalysisFlags>::mask_value))
        fail("unsupported analysis flags");
    // SourceOptions contains only bools and an optional uint32_t. JSON primitive
    // types and numThreads bounds are checked before reconstruction. Zero and
    // absence have distinct meanings and must both remain representable.
}

// The exact enum's subsystem and code are encoded, never the std::hash or
// object representation (padding, byte order, RTTI, and addresses are absent).
uint32_t encodeCode(DiagCode code) {
    return (static_cast<uint32_t>(code.getSubsystem()) << 16) | code.getCode();
}
DiagCode decodeCode(uint32_t code) {
    static const auto knownCodes = [] {
        std::unordered_set<uint32_t> known;
        for (auto value : DiagCode::KnownCodes)
            known.insert(encodeCode(value));
        return known;
    }();
    auto result = DiagCode(static_cast<DiagSubsystem>(code >> 16),
                           static_cast<uint16_t>(code & 0xffff));
    if (!knownCodes.contains(code))
        fail("unsupported diagnostic code");
    return result;
}

uint64_t diagnosticSchema() {
    // An additional mismatch guard, not a security identity or substitute for
    // launching the same binary. Explicit byte order makes it platform-stable.
    uint64_t result = 14695981039346656037ull;
    auto add = [&](unsigned char byte) { result = (result ^ byte) * 1099511628211ull; };
    for (auto code : DiagCode::KnownCodes) {
        auto number = encodeCode(code);
        for (unsigned n = 0; n < 4; ++n)
            add(static_cast<unsigned char>(number >> (8 * n)));
        for (auto ch : toString(code))
            add(static_cast<unsigned char>(ch));
        add(0);
    }
    return result;
}

struct SeverityEntry {
    uint32_t code;
    uint32_t severity;
};
struct MessageEntry {
    uint32_t code;
    std::string message;
};
struct WireSettings {
    std::vector<SeverityEntry> severityTable;
    std::vector<SeverityEntry> baselineSeverityTable;
    std::vector<MessageEntry> messageTable;
    std::vector<std::string> ignoreWarnPatterns;
    std::vector<std::string> ignoreMacroWarnPatterns;
    int errorLimit;
    bool ignoreAllWarnings;
    bool ignoreAllNotes;
    bool warningsAsErrors;
    bool errorsAsFatal;
    bool fatalsAsErrors;
    bool useBuiltinTypeFormatter;
    bool useBuiltinSymbolPath;
};
struct FileMetadata {
    std::string fullPath, directory, displayName;
    bool generated;
    uint64_t byteLength;
};
struct WireSources {
    uint32_t schemaVersion;
    std::string cwd;
    bool disableProximatePaths, disableLocalIncludes, incDirFirst;
    std::vector<std::string> systemDirectories, userDirectories;
    std::vector<FileMetadata> files;
    std::vector<SourceManager::SourceSnapshot::Lookup> lookups;
    std::vector<std::string> headerRequests;
    std::vector<uint32_t> graphFileIds;
};
struct Metadata {
    uint32_t codecVersion;
    uint64_t diagnosticSchema;
    WireSources sources;
    OwnedOptions options;
    std::vector<OwnedOptions> treeOptions;
    WireSettings settings;
};

template<typename Map>
std::vector<SeverityEntry> captureSeverities(const Map& map) {
    std::vector<SeverityEntry> result;
    result.reserve(map.size());
    for (const auto& [code, severity] : map)
        result.push_back({encodeCode(code), static_cast<uint32_t>(severity)});
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.code < b.code; });
    return result;
}

WireSettings captureSettings(const DiagnosticEngine::SettingsSnapshot& input) {
    WireSettings result;
    result.severityTable = captureSeverities(input.severityTable);
    result.baselineSeverityTable = captureSeverities(input.baselineSeverityTable);
    for (const auto& [code, message] : input.messageTable)
        result.messageTable.push_back({encodeCode(code), message});
    std::sort(result.messageTable.begin(), result.messageTable.end(),
              [](const auto& a, const auto& b) { return a.code < b.code; });
    for (const auto& path : input.ignoreWarnPatterns)
        result.ignoreWarnPatterns.push_back(path.string());
    for (const auto& path : input.ignoreMacroWarnPatterns)
        result.ignoreMacroWarnPatterns.push_back(path.string());
#define COPY_SETTING(member) result.member = input.member
    COPY_SETTING(errorLimit);
    COPY_SETTING(ignoreAllWarnings);
    COPY_SETTING(ignoreAllNotes);
    COPY_SETTING(warningsAsErrors);
    COPY_SETTING(errorsAsFatal);
    COPY_SETTING(fatalsAsErrors);
    COPY_SETTING(useBuiltinTypeFormatter);
    COPY_SETTING(useBuiltinSymbolPath);
#undef COPY_SETTING
    return result;
}

DiagnosticEngine::SettingsSnapshot reconstructSettings(const WireSettings& input,
                                                       const Limits& limits) {
    DiagnosticEngine::SettingsSnapshot result;
    auto addSeverities = [&](const auto& entries, auto& map) {
        countOk(entries, limits);
        for (const auto& entry : entries) {
            auto code = decodeCode(entry.code);
            if (entry.severity > static_cast<uint32_t>(DiagnosticSeverity::Fatal))
                fail("invalid diagnostic severity");
            if (!map.emplace(code, static_cast<DiagnosticSeverity>(entry.severity)).second)
                fail("duplicate diagnostic severity entry");
        }
    };
    addSeverities(input.severityTable, result.severityTable);
    addSeverities(input.baselineSeverityTable, result.baselineSeverityTable);
    countOk(input.messageTable, limits);
    for (const auto& entry : input.messageTable) {
        stringOk(entry.message, limits);
        if (!result.messageTable.emplace(decodeCode(entry.code), entry.message).second)
            fail("duplicate diagnostic message entry");
    }
    stringsOk(input.ignoreWarnPatterns, limits, true);
    stringsOk(input.ignoreMacroWarnPatterns, limits, true);
    for (const auto& path : input.ignoreWarnPatterns)
        result.ignoreWarnPatterns.emplace_back(path);
    for (const auto& path : input.ignoreMacroWarnPatterns)
        result.ignoreMacroWarnPatterns.emplace_back(path);
#define COPY_SETTING(member) result.member = input.member
    COPY_SETTING(errorLimit);
    COPY_SETTING(ignoreAllWarnings);
    COPY_SETTING(ignoreAllNotes);
    COPY_SETTING(warningsAsErrors);
    COPY_SETTING(errorsAsFatal);
    COPY_SETTING(fatalsAsErrors);
    COPY_SETTING(useBuiltinTypeFormatter);
    COPY_SETTING(useBuiltinSymbolPath);
#undef COPY_SETTING
    return result;
}

void validateSources(const WireSources& source, const Limits& limits) {
    if (source.schemaVersion != SourceManager::SourceSnapshot::SchemaVersion)
        fail("unsupported source schema");
    bounded(source.files.size(), limits.maxFiles, "source file count limit exceeded");
    stringOk(source.cwd, limits, true);
    stringsOk(source.systemDirectories, limits, true);
    stringsOk(source.userDirectories, limits, true);
    stringsOk(source.headerRequests, limits, true);
    countOk(source.lookups, limits);
    countOk(source.graphFileIds, limits);
    uint64_t total = 0;
    std::unordered_set<std::string_view> paths;
    for (const auto& file : source.files) {
        stringOk(file.fullPath, limits, true);
        stringOk(file.directory, limits, true);
        stringOk(file.displayName, limits, true);
        if (!paths.insert(file.fullPath).second)
            fail("conflicting source file paths");
        if (!file.byteLength)
            fail("source file is missing its terminating NUL");
        bounded(file.byteLength, limits.maxFileBytes, "source file byte limit exceeded");
        if (file.byteLength > limits.maxTotalFileBytes - total)
            fail("total source byte limit exceeded");
        total += file.byteLength;
    }
    paths.clear();
    for (const auto& lookup : source.lookups) {
        stringOk(lookup.path, limits, true);
        if (!paths.insert(lookup.path).second || lookup.fileId > source.files.size())
            fail("conflicting source lookup");
        if (lookup.fileId) {
            if (lookup.errorValue || !lookup.errorCategory.empty())
                fail("source lookup contains both file and error");
        }
        else if (!lookup.errorValue ||
                 (lookup.errorCategory != "generic" && lookup.errorCategory != "system")) {
            fail("unsupported source lookup error");
        }
    }
    for (auto id : source.graphFileIds) {
        if (id > source.files.size())
            fail("source graph file ID out of bounds");
    }
}

Metadata captureMetadata(const Input& input) {
    const auto& source = input.sources;
    WireSources wire{source.schemaVersion,
                     source.cwd,
                     source.disableProximatePaths,
                     source.disableLocalIncludes,
                     source.incDirFirst,
                     source.systemDirectories,
                     source.userDirectories,
                     {},
                     source.lookups,
                     source.headerRequests,
                     source.graphFileIds};
    wire.files.reserve(source.files.size());
    for (const auto& file : source.files) {
        wire.files.push_back(
            {file.fullPath, file.directory, file.displayName, file.generated, file.bytes.size()});
    }
    return {CodecVersion,  diagnosticSchema(), std::move(wire),
            input.options, input.treeOptions,  captureSettings(input.settings)};
}

void validateMetadata(const Metadata& metadata, const Limits& limits) {
    if (metadata.codecVersion != CodecVersion || metadata.diagnosticSchema != diagnosticSchema())
        fail("incompatible diagnostic snapshot schema");
    validateSources(metadata.sources, limits);
    validateOptions(metadata.options, limits);
    countOk(metadata.treeOptions, limits);
    for (const auto& options : metadata.treeOptions)
        validateOptions(options, limits);
}

void readExact(std::istream& stream, char* buffer, uint64_t size) {
    if (size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        fail("frame length cannot be represented on this platform");
    if (!stream.read(buffer, static_cast<std::streamsize>(size)))
        fail("truncated input");
}
void writeExact(std::ostream& stream, const char* buffer, uint64_t size) {
    if (size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        fail("frame length cannot be represented on this platform");
    if (!stream.write(buffer, static_cast<std::streamsize>(size)))
        fail("output write failed");
}
void writeU64(std::ostream& stream, uint64_t value) {
    std::array<char, 8> bytes{};
    for (unsigned n = 0; n < 8; ++n)
        bytes[n] = static_cast<char>(value >> (8 * n));
    writeExact(stream, bytes.data(), bytes.size());
}
uint64_t readU64(std::istream& stream) {
    std::array<char, 8> bytes{};
    readExact(stream, bytes.data(), bytes.size());
    uint64_t result = 0;
    for (unsigned n = 0; n < 8; ++n)
        result |= uint64_t(static_cast<unsigned char>(bytes[n])) << (8 * n);
    return result;
}

// Bound nesting before asking the JSON parser to allocate its syntax tree.
void checkJsonDepth(std::string_view json, const Limits& limits) {
    uint64_t depth = 0;
    bool quoted = false, escape = false;
    for (char ch : json) {
        if (quoted) {
            if (escape)
                escape = false;
            else if (ch == '\\')
                escape = true;
            else if (ch == '"')
                quoted = false;
        }
        else if (ch == '"')
            quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > limits.maxJsonDepth)
                fail("JSON nesting limit exceeded");
        }
        else if (ch == '}' || ch == ']') {
            if (!depth)
                fail("unbalanced JSON metadata");
            --depth;
        }
    }
    if (quoted || depth)
        fail("unterminated JSON metadata");
}

void validateJsonTree(yyjson_val* value, const Limits& limits, std::string_view name = {},
                      std::string_view parentName = {}) {
    if (yyjson_is_str(value)) {
        stringOk({yyjson_get_str(value), yyjson_get_len(value)}, limits);
    }
    else if (yyjson_is_arr(value)) {
        bounded(yyjson_arr_size(value), limits.maxEntries, "JSON array limit exceeded");
        size_t index, max;
        yyjson_val* child;
        yyjson_arr_foreach(value, index, max, child) {
            validateJsonTree(child, limits, {}, name);
        }
    }
    else if (yyjson_is_obj(value)) {
        bounded(yyjson_obj_size(value), limits.maxEntries, "JSON object limit exceeded");
        std::unordered_set<std::string_view> keys;
        size_t index, max;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, index, max, key, child) {
            std::string_view keyName(yyjson_get_str(key), yyjson_get_len(key));
            stringOk(keyName, limits);
            if (!keys.insert(keyName).second)
                fail("duplicate JSON object key");
            validateJsonTree(child, limits, keyName, name);
        }
    }
    else if (yyjson_is_num(value)) {
        // reflect-cpp's JSON Reader uses static_cast for integral fields. Check
        // ranges before reflection so malicious integers cannot wrap to a valid
        // ID, enum, length, or option. No floating-point numbers occur here.
        const bool signedField = name == "errorValue" ||
                                 (name == "errorLimit" && parentName == "settings");
        if (signedField) {
            if (!yyjson_is_int(value) ||
                (yyjson_is_uint(value) && yyjson_get_uint(value) > uint64_t(INT_MAX)) ||
                (!yyjson_is_uint(value) && yyjson_get_sint(value) < INT_MIN))
                fail("signed metadata integer out of range");
        }
        else {
            if (!yyjson_is_uint(value))
                fail("expected unsigned metadata integer");
            const bool wideField = name == "diagnosticSchema" || name == "maxConstantSize" ||
                                   name == "byteLength";
            if (!wideField && yyjson_get_uint(value) > UINT32_MAX)
                fail("metadata integer out of range");
        }
    }
}

Metadata parseMetadata(std::string_view text, const Limits& limits) {
    checkJsonDepth(text, limits);
    yyjson_read_err error{};
    auto* raw = yyjson_read_opts(const_cast<char*>(text.data()), text.size(), 0, nullptr, &error);
    if (!raw)
        fail("invalid JSON metadata");
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(raw, &yyjson_doc_free);
    auto* root = yyjson_doc_get_root(doc.get());
    validateJsonTree(root, limits);
    auto parsed = rfl::json::read<Metadata, rfl::NoExtraFields>(rfl::json::InputVarType(root));
    if (!parsed)
        fail("JSON metadata does not match the snapshot schema");
    return std::move(*parsed);
}

} // namespace

OwnedOptions captureOptions(const slang::Bag& options, const Limits& limits) {
    OwnedOptions result;
    size_t count = 0;
    if (auto* input = options.get<slang::parsing::LexerOptions>()) {
        ++count;
        auto& out = result.lexer.emplace();
        out.maxErrors = input->maxErrors;
        out.languageVersion = static_cast<uint32_t>(input->languageVersion);
        out.enableLegacyProtect = input->enableLegacyProtect;
        out.allowMacroTrailingSpace = input->allowMacroTrailingSpace;
        for (const auto& [prefix, handlers] : input->commentHandlers) {
            OwnedCommentHandlers group{std::string(prefix), {}};
            for (const auto& [directive, handler] : handlers)
                group.handlers.push_back({std::string(directive),
                                          static_cast<uint32_t>(handler.kind),
                                          std::string(handler.endRegion)});
            std::sort(group.handlers.begin(), group.handlers.end(),
                      [](const auto& a, const auto& b) { return a.directive < b.directive; });
            out.commentHandlers.push_back(std::move(group));
        }
        std::sort(out.commentHandlers.begin(), out.commentHandlers.end(),
                  [](const auto& a, const auto& b) { return a.prefix < b.prefix; });
    }
    if (auto* input = options.get<slang::parsing::PreprocessorOptions>()) {
        ++count;
        if (input->bufferChangeCB)
            fail("live preprocessor buffer callback is unsupported");
        auto& out = result.preprocessor.emplace();
        out.maxIncludeDepth = input->maxIncludeDepth;
        out.languageVersion = static_cast<uint32_t>(input->languageVersion);
        out.predefineSource = input->predefineSource;
        out.predefines = input->predefines;
        out.undefines = input->undefines;
        for (const auto& path : input->additionalIncludePaths)
            out.additionalIncludePaths.push_back(path.string());
        for (auto directive : input->ignoreDirectives)
            out.ignoreDirectives.emplace_back(directive);
        std::sort(out.ignoreDirectives.begin(), out.ignoreDirectives.end());
        for (const auto& [pattern, version] : input->keywordMapping)
            out.keywordMapping.push_back({pattern, static_cast<uint32_t>(version)});
        out.allowMissingProtectedScopeEnd = input->allowMissingProtectedScopeEnd;
        out.captureUvmPrefix = input->captureUvmPrefix;
        out.uvmPrefixRoot = input->uvmPrefixRoot;
    }
    if (auto* input = options.get<slang::parsing::ParserOptions>()) {
        ++count;
        result.parser = OwnedParserOptions{input->maxRecursionDepth,
                                           static_cast<uint32_t>(input->languageVersion)};
    }
    if (auto* input = options.get<slang::ast::CompilationOptions>()) {
        ++count;
        auto& out = result.compilation.emplace();
        out.flags = input->flags.bits();
#define COPY_OPTION(member) out.member = input->member
        COPY_OPTION(maxInstanceDepth);
        COPY_OPTION(maxCheckerInstanceDepth);
        COPY_OPTION(maxGenerateSteps);
        COPY_OPTION(maxConstexprDepth);
        COPY_OPTION(maxConstexprSteps);
        COPY_OPTION(maxConstexprBacktrace);
        COPY_OPTION(maxConstantSize);
        COPY_OPTION(maxDefParamSteps);
        COPY_OPTION(maxDefParamBlocks);
        COPY_OPTION(maxInstanceArray);
        COPY_OPTION(maxEnumValues);
        COPY_OPTION(maxRecursiveClassSpecialization);
        COPY_OPTION(maxUDPCoverageNotes);
        COPY_OPTION(errorLimit);
        COPY_OPTION(typoCorrectionLimit);
        COPY_OPTION(paramOverrides);
        COPY_OPTION(defaultLiblist);
#undef COPY_OPTION
        out.minTypMax = static_cast<uint32_t>(input->minTypMax);
        out.languageVersion = static_cast<uint32_t>(input->languageVersion);
        if (input->defaultTimeScale) {
            const auto& time = *input->defaultTimeScale;
            out.defaultTimeScale = OwnedTimeScale{static_cast<uint32_t>(time.base.unit),
                                                  static_cast<uint32_t>(time.base.magnitude),
                                                  static_cast<uint32_t>(time.precision.unit),
                                                  static_cast<uint32_t>(time.precision.magnitude)};
        }
        for (auto module : input->topModules)
            out.topModules.emplace_back(module);
        std::sort(out.topModules.begin(), out.topModules.end());
    }
    if (auto* input = options.get<slang::analysis::AnalysisOptions>()) {
        ++count;
        result.analysis = OwnedAnalysisOptions{static_cast<uint32_t>(input->flags.bits()),
                                               input->maxCaseAnalysisSteps,
                                               input->maxLoopAnalysisSteps};
    }
    if (auto* input = options.get<slang::driver::SourceOptions>()) {
        ++count;
        result.source = OwnedSourceOptions{input->numThreads, input->singleUnit, input->onlyLint,
                                           input->librariesInheritMacros};
    }
    if (count != options.size())
        fail("unrecognized Bag option type");
    validateOptions(result, limits);
    return result;
}

ReconstructedOptions::ReconstructedOptions(OwnedOptions options, const Limits& limits) :
    owned_(std::move(options)) {
    validateOptions(owned_, limits);
    if (owned_.lexer) {
        const auto& input = *owned_.lexer;
        slang::parsing::LexerOptions out;
        out.maxErrors = input.maxErrors;
        out.languageVersion = static_cast<slang::LanguageVersion>(input.languageVersion);
        out.enableLegacyProtect = input.enableLegacyProtect;
        out.allowMacroTrailingSpace = input.allowMacroTrailingSpace;
        for (const auto& group : input.commentHandlers) {
            auto& handlers = out.commentHandlers[group.prefix];
            for (const auto& entry : group.handlers)
                handlers.emplace(entry.directive,
                                 slang::parsing::CommentHandler(
                                     static_cast<slang::parsing::CommentHandler::Kind>(entry.kind),
                                     entry.endRegion));
        }
        bag_.set(std::move(out));
    }
    if (owned_.preprocessor) {
        const auto& input = *owned_.preprocessor;
        slang::parsing::PreprocessorOptions out;
        out.maxIncludeDepth = input.maxIncludeDepth;
        out.languageVersion = static_cast<slang::LanguageVersion>(input.languageVersion);
        out.predefineSource = input.predefineSource;
        out.predefines = input.predefines;
        out.undefines = input.undefines;
        for (const auto& path : input.additionalIncludePaths)
            out.additionalIncludePaths.emplace_back(path);
        for (const auto& directive : input.ignoreDirectives)
            out.ignoreDirectives.emplace(directive);
        for (const auto& entry : input.keywordMapping)
            out.keywordMapping.emplace_back(
                entry.pattern, static_cast<slang::parsing::KeywordVersion>(entry.version));
        out.allowMissingProtectedScopeEnd = input.allowMissingProtectedScopeEnd;
        out.captureUvmPrefix = input.captureUvmPrefix;
        out.uvmPrefixRoot = input.uvmPrefixRoot;
        bag_.set(std::move(out));
    }
    if (owned_.parser) {
        const auto& input = *owned_.parser;
        slang::parsing::ParserOptions out;
        out.maxRecursionDepth = input.maxRecursionDepth;
        out.languageVersion = static_cast<slang::LanguageVersion>(input.languageVersion);
        bag_.set(std::move(out));
    }
    if (owned_.compilation) {
        const auto& input = *owned_.compilation;
        slang::ast::CompilationOptions out;
        out.flags = static_cast<slang::ast::CompilationFlags>(input.flags);
#define COPY_OPTION(member) out.member = input.member
        COPY_OPTION(maxInstanceDepth);
        COPY_OPTION(maxCheckerInstanceDepth);
        COPY_OPTION(maxGenerateSteps);
        COPY_OPTION(maxConstexprDepth);
        COPY_OPTION(maxConstexprSteps);
        COPY_OPTION(maxConstexprBacktrace);
        COPY_OPTION(maxConstantSize);
        COPY_OPTION(maxDefParamSteps);
        COPY_OPTION(maxDefParamBlocks);
        COPY_OPTION(maxInstanceArray);
        COPY_OPTION(maxEnumValues);
        COPY_OPTION(maxRecursiveClassSpecialization);
        COPY_OPTION(maxUDPCoverageNotes);
        COPY_OPTION(errorLimit);
        COPY_OPTION(typoCorrectionLimit);
        COPY_OPTION(paramOverrides);
        COPY_OPTION(defaultLiblist);
#undef COPY_OPTION
        out.minTypMax = static_cast<slang::ast::MinTypMax>(input.minTypMax);
        out.languageVersion = static_cast<slang::LanguageVersion>(input.languageVersion);
        if (input.defaultTimeScale) {
            const auto& time = *input.defaultTimeScale;
            out.defaultTimeScale =
                slang::TimeScale{{static_cast<slang::TimeUnit>(time.baseUnit),
                                  static_cast<slang::TimeScaleMagnitude>(time.baseMagnitude)},
                                 {static_cast<slang::TimeUnit>(time.precisionUnit),
                                  static_cast<slang::TimeScaleMagnitude>(time.precisionMagnitude)}};
        }
        for (const auto& module : input.topModules)
            out.topModules.emplace(module);
        bag_.set(std::move(out));
    }
    if (owned_.analysis) {
        const auto& input = *owned_.analysis;
        slang::analysis::AnalysisOptions out;
        out.flags = static_cast<slang::analysis::AnalysisFlags>(input.flags);
        out.maxCaseAnalysisSteps = input.maxCaseAnalysisSteps;
        out.maxLoopAnalysisSteps = input.maxLoopAnalysisSteps;
        bag_.set(std::move(out));
    }
    if (owned_.source) {
        const auto& input = *owned_.source;
        slang::driver::SourceOptions out{};
        out.numThreads = input.numThreads;
        out.singleUnit = input.singleUnit;
        out.onlyLint = input.onlyLint;
        out.librariesInheritMacros = input.librariesInheritMacros;
        bag_.set(std::move(out));
    }
}

void writeInput(std::ostream& stream, const Input& input, const Limits& limits) {
    auto metadata = captureMetadata(input);
    validateMetadata(metadata, limits);
    (void)reconstructSettings(metadata.settings, limits);
    for (const auto& file : input.sources.files) {
        if (file.bytes.empty() || file.bytes.back() != '\0')
            fail("source file is missing its terminating NUL");
    }
    auto json = rfl::json::write(metadata);
    bounded(json.size(), limits.maxMetadataBytes, "metadata byte limit exceeded");
    checkJsonDepth(json, limits);
    writeExact(stream, Magic.data(), Magic.size());
    writeU64(stream, json.size());
    writeExact(stream, json.data(), json.size());
    for (const auto& file : input.sources.files) {
        writeU64(stream, file.bytes.size());
        writeExact(stream, file.bytes.data(), file.bytes.size());
    }
}

Input readInput(std::istream& stream, const Limits& limits) {
    std::array<char, 8> magic{};
    readExact(stream, magic.data(), magic.size());
    if (std::string_view(magic.data(), magic.size()) != Magic)
        fail("invalid snapshot magic");
    auto metadataLength = readU64(stream);
    bounded(metadataLength, limits.maxMetadataBytes, "metadata byte limit exceeded");
    bounded(metadataLength, std::numeric_limits<size_t>::max(), "metadata byte length overflow");
    std::string json(static_cast<size_t>(metadataLength), '\0');
    readExact(stream, json.data(), metadataLength);
    auto metadata = parseMetadata(json, limits);
    validateMetadata(metadata, limits);
    Input result;
    result.settings = reconstructSettings(metadata.settings, limits);
    result.options = std::move(metadata.options);
    result.treeOptions = std::move(metadata.treeOptions);
    auto& source = metadata.sources;
    auto& out = result.sources;
    out.schemaVersion = source.schemaVersion;
    out.cwd = std::move(source.cwd);
    out.disableProximatePaths = source.disableProximatePaths;
    out.disableLocalIncludes = source.disableLocalIncludes;
    out.incDirFirst = source.incDirFirst;
    out.systemDirectories = std::move(source.systemDirectories);
    out.userDirectories = std::move(source.userDirectories);
    out.lookups = std::move(source.lookups);
    out.headerRequests = std::move(source.headerRequests);
    out.graphFileIds = std::move(source.graphFileIds);
    out.files.reserve(source.files.size());
    for (auto& file : source.files) {
        auto size = readU64(stream);
        if (size != file.byteLength)
            fail("source frame length does not match metadata");
        bounded(size, std::numeric_limits<size_t>::max(), "source byte length overflow");
        std::string bytes(static_cast<size_t>(size), '\0');
        readExact(stream, bytes.data(), size);
        if (bytes.back() != '\0')
            fail("source file is missing its terminating NUL");
        out.files.push_back({std::move(file.fullPath), std::move(file.directory),
                             std::move(file.displayName), std::move(bytes), file.generated});
    }
    return result;
}

} // namespace server::diagnostic_snapshot
