//------------------------------------------------------------------------------
// gen_config_main.cpp
// Standalone tool to generate JSON schema from Config.h
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "Config.h"
#include <iostream>
#include <rfl/DefaultIfMissing.hpp>
#include <rfl/json.hpp>
#include <rfl/to_generic.hpp>

namespace {
using Object = rfl::Generic::Object;

void addDefaults(Object& schema, const Object& defaults, Object& definitions) {
    auto& properties = std::get<Object>(schema.at("properties").get());
    for (auto& [name, value] : properties) {
        auto& property = std::get<Object>(value.get());
        const auto resolved = defaults.get(name);
        property["default"] = resolved ? resolved.value() : rfl::Generic{};

        // Annotate children of objects that have a runtime default value.
        // Optional objects left unset by Config{} retain their null default.
        if (!resolved || !property.count("$ref"))
            continue;
        const auto* children = std::get_if<Object>(&resolved.value().get());
        if (!children)
            continue;
        auto ref = property.at("$ref").to_string().value();
        constexpr std::string_view prefix = "#/$defs/";
        if (ref.starts_with(prefix)) {
            auto& definition = std::get<Object>(definitions.at(ref.substr(prefix.size())).get());
            addDefaults(definition, *children, definitions);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        constexpr auto flags = rfl::json::pretty | YYJSON_WRITE_PRETTY_TWO_SPACES;
        auto defaults = rfl::to_generic(Config{});
        if (argc == 2 && std::string_view(argv[1]) == "--defaults") {
            std::cout << rfl::json::write(defaults, flags) << '\n';
            return 0;
        }
        const std::string schema = rfl::json::to_schema<Config, rfl::DefaultIfMissing>(
            flags);
        auto annotated = rfl::json::read<rfl::Generic>(schema).value();
        auto& root = std::get<Object>(annotated.get());
        auto& definitions = std::get<Object>(root.at("$defs").get());
        auto& config = std::get<Object>(definitions.at("Config").get());
        addDefaults(config, std::get<Object>(defaults.get()), definitions);
        std::cout << rfl::json::write(annotated, flags) << '\n';
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error generating config schema: " << e.what() << '\n';
        return 1;
    }
}
