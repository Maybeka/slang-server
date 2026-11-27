//------------------------------------------------------------------------------
// Config.cpp
// Configuration management for the language server
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "Config.h"

#include "rfl/Result.hpp"
#include "rfl/from_generic.hpp"
#include "util/Logging.h"
#include "slang/util/OS.h"
#include <cstdlib>
#include <fstream>
#include <rfl/DefaultIfMissing.hpp>
#include <rfl/json.hpp>
#include <rfl/to_generic.hpp>

static int CONFIG_READ_FLAGS = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS;

namespace fs = std::filesystem;

namespace {
rfl::Result<rfl::Nothing> expandProjectPaths(Config& config) {
    std::optional<std::string> missing;
    auto resolve = [&](const std::string& name) {
        auto value = config.getEnvironmentVariable(name);
        if (!value && !missing)
            missing = name;
        return value.value_or("");
    };
    auto expand = [&](std::string& path) {
        if (path.find('$') == std::string::npos)
            return;
        std::string expanded;
        const char* cursor = path.data();
        const char* end = cursor + path.size();
        while (cursor != end) {
            char ch = *cursor++;
            if (ch != '$' || cursor == end)
                expanded += ch;
            else if (*cursor == '$') {
                ++cursor;
                expanded += '$';
            }
            else
                expanded += slang::OS::parseEnvVar(cursor, end, resolve);
        }
        path = std::move(expanded);
    };
    auto expandOptional = [&](std::optional<std::string>& path) {
        if (path)
            expand(*path);
    };
    auto expandList = [&](std::vector<std::string>& list) {
        for (auto& path : list)
            expand(path);
    };
    expandOptional(config.build.value());
    expandOptional(config.compilationFilelist.value());
    expandOptional(config.buildPattern.value());
    expandList(config.incdirs.value());
    expandList(config.explorePreincludeFiles.value());
    expandList(config.indexGlobs.value());
    expandList(config.excludeDirs.value());
    for (auto& entry : config.index.value()) {
        expandList(entry.dirs.value());
        if (entry.excludeDirs.value())
            expandList(*entry.excludeDirs.value());
    }
    for (auto& entry : config.builds.value())
        expand(entry.glob.value());
    if (auto& uvm = config.uvm.value()) {
        expand(uvm->sourceDirectory.value());
        expandOptional(uvm->entryFile.value());
    }
    if (missing)
        return rfl::error(fmt::format("Unknown environment variable '{}' in project configuration",
                                      *missing));
    return rfl::Nothing{};
}
} // namespace

std::optional<std::string> Config::getEnvironmentVariable(const std::string& name) const {
    auto it = env.value().find(name);
    if (it != env.value().end())
        return it->second;
    if (const char* value = std::getenv(name.c_str()))
        return std::string(value);
    return {};
}

rfl::Result<Config> Config::fromFiles(const std::optional<std::string>& workspaceConf,
                                      const std::optional<std::string>& userConf,
                                      const std::optional<std::string>& localConf) {
    rfl::Generic::Object config = *rfl::to_generic(Config()).to_object();
    std::optional<std::string> readError;
    std::vector<Config::InputSource> inputs;

    // Layer a single config file onto the merged config object
    auto layerFile = [&](const std::string& confPath) -> std::optional<rfl::Generic::Object> {
        if (!fs::exists(confPath)) {
            inputs.push_back({confPath, std::nullopt});
            WARN("Config file {} does not exist, skipping", confPath);
            return std::nullopt;
        }

        INFO("Layering config from {}", confPath);
        auto file = std::ifstream(confPath);
        auto jsonstr = std::string(std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>());
        inputs.push_back({confPath, jsonstr});

        // Validate against Config schema first
        auto fileConfig = rfl::json::read<Config, rfl::DefaultIfMissing>(jsonstr,
                                                                         CONFIG_READ_FLAGS);
        if (!fileConfig) {
            readError = fmt::format("Failed to read config from {}: {}", confPath,
                                    fileConfig.error().what());
            return std::nullopt;
        }

        auto generic = rfl::json::read<rfl::Generic>(jsonstr, CONFIG_READ_FLAGS);
        if (!generic) {
            readError = fmt::format("Failed to read generic config from {}: {}", confPath,
                                    generic.error().what());
            return std::nullopt;
        }

        auto object = generic->to_object();
        if (!object) {
            readError = fmt::format("Failed to convert config from {} to object: {}", confPath,
                                    object.error().what());
            return std::nullopt;
        }

        for (const auto& [k, v] : *object) {
            if (k == "env") {
                auto values = config[k].to_object().value();
                const auto layer = v.to_object().value();
                for (const auto& [name, value] : layer)
                    values[name] = value;
                config[k] = std::move(values);
            }
            else if (auto existingArray = config[k].to_array()) {
                auto arr = existingArray.value();
                auto newArr = v.to_array().value();
                arr.reserve(arr.size() + newArr.size());
                arr.insert(arr.end(), newArr.begin(), newArr.end());
                config[k] = arr;
            }
            else {
                config[k] = v;
            }
        }
        return *object;
    };

    // Layer configs: workspace and user override each other, local is additive (for flags)
    std::optional<rfl::Generic::Object> workspaceObj, userObj, localObj;
    if (workspaceConf)
        workspaceObj = layerFile(*workspaceConf);
    if (userConf)
        userObj = layerFile(*userConf);
    if (localConf)
        localObj = layerFile(*localConf);
    if (readError)
        return rfl::error(*readError);

    auto finalConfig = rfl::from_generic<Config, rfl::DefaultIfMissing>(config);
    if (!finalConfig) {
        return rfl::error(
            fmt::format("Failed to convert final config: {}", finalConfig.error().what()));
    }

    if (!finalConfig->sourceHistoryMaxMiB.value() ||
        finalConfig->sourceHistoryMaxMiB.value() > 4096)
        return rfl::error("sourceHistoryMaxMiB must be between 1 and 4096");

    auto hasField = [](const std::optional<rfl::Generic::Object>& obj,
                       std::string_view fieldName) -> bool {
        return obj && obj->count(std::string(fieldName)) > 0;
    };

    const bool buildPatternExplicit = hasField(workspaceObj, "buildPattern") ||
                                      hasField(userObj, "buildPattern") ||
                                      hasField(localObj, "buildPattern");
    if (!buildPatternExplicit && finalConfig->builds.value().empty()) {
        finalConfig->buildPattern = "**/*.f";
    }

    // Build flagsByFile: workspace overrides user (last non-local wins), local appends
    auto extractFlags = [](std::optional<rfl::Generic::Object>& obj) -> std::string {
        if (!obj)
            return {};
        auto flags = (*obj)["flags"].to_string();
        return flags.value_or("");
    };

    std::vector<FlagSource> flagsByFile;
    // Base flags: workspace wins over user
    auto wsFlags = extractFlags(workspaceObj);
    auto userFlags = extractFlags(userObj);
    if (!wsFlags.empty())
        flagsByFile.push_back({*workspaceConf, wsFlags});
    else if (!userFlags.empty())
        flagsByFile.push_back({*userConf, userFlags});
    // Local always appends
    auto lcFlags = extractFlags(localObj);
    if (!lcFlags.empty())
        flagsByFile.push_back({*localConf, lcFlags});

    finalConfig->flagsByFile = std::move(flagsByFile);
    finalConfig->inputSources = std::move(inputs);

    // Reconstruct merged flags for client config
    std::string merged;
    for (auto& src : finalConfig->flagsByFile.value()) {
        if (!merged.empty())
            merged += " ";
        merged += src.flags;
    }
    finalConfig->flags = merged;

    if (auto paths = expandProjectPaths(*finalConfig); !paths)
        return rfl::error(paths.error().what());
    return *finalConfig;
}
