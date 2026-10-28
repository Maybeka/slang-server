// SPDX-License-Identifier: MIT
#include "util/RawBodyDirectives.h"
#include <catch2/catch_test_macros.hpp>
#include <random>
#include <regex>
#include <string>
#include <vector>

namespace {
// Preserve the exact prior production expressions as an independent oracle.
const std::regex statePattern(
    R"(`(define|undef|undefineall|include|begin_keywords|end_keywords|timescale|default_nettype|resetall|line|celldefine|endcelldefine|pragma|unconnected_drive|nounconnected_drive|default_decay_time|default_trireg_strength|delay_mode_distributed|delay_mode_path|delay_mode_unit|delay_mode_zero|uselib)\b)");
const std::regex conditionalPattern(R"(`(ifdef|ifndef|elsif|else|endif)\b)");
std::string legacyBodyResult(const std::string& text) {
    if (std::regex_search(text, statePattern))
        return "state";
    size_t depth = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), conditionalPattern), end; it != end;
         ++it) {
        const auto word = (*it)[1].str();
        if (word == "ifdef" || word == "ifndef")
            ++depth;
        else {
            if (!depth)
                return "enclosing";
            if (word == "endif")
                --depth;
        }
    }
    return depth ? "open" : "okay";
}
void compareLegacy(const std::string& text) {
    INFO(text);
    const auto result = server::inspectRawBodyDirectives(text);
    const auto body = result.changesState                  ? "state"
                      : result.changesEnclosingConditional ? "enclosing"
                      : result.openConditionals            ? "open"
                                                           : "okay";
    CHECK(body == legacyBodyResult(text));
    CHECK((!result.changesState && !result.hasConditionals) ==
          (!std::regex_search(text, statePattern) && !std::regex_search(text, conditionalPattern)));
}
} // namespace

TEST_CASE("Raw directive scanner preserves prior preprocessing proof decisions",
          "[body-certification][raw-directive-proof]") {
    const std::vector<std::string> names = {"define",
                                            "undef",
                                            "undefineall",
                                            "include",
                                            "begin_keywords",
                                            "end_keywords",
                                            "timescale",
                                            "default_nettype",
                                            "resetall",
                                            "line",
                                            "celldefine",
                                            "endcelldefine",
                                            "pragma",
                                            "unconnected_drive",
                                            "nounconnected_drive",
                                            "default_decay_time",
                                            "default_trireg_strength",
                                            "delay_mode_distributed",
                                            "delay_mode_path",
                                            "delay_mode_unit",
                                            "delay_mode_zero",
                                            "uselib",
                                            "ifdef",
                                            "ifndef",
                                            "elsif",
                                            "else",
                                            "endif",
                                            "defineMore",
                                            "ifdef_",
                                            "Line",
                                            "uvm_info",
                                            "line42"};
    const std::vector<std::string> prefixes = {"`", "// `", "\"`", "``", "/* `", "plain "};
    const std::vector<std::string> suffixes = {"",  " ",      "\n", "(", "$x", "_x",
                                               "9", "`endif", ".x", "é", "/",  "\""};
    for (const auto& name : names)
        for (const auto& prefix : prefixes)
            for (const auto& suffix : suffixes)
                compareLegacy(prefix + name + suffix);
    for (const auto& text :
         {"`ifdef A\n`else\n`endif", "`ifdef A\n`ifndef B\n`elsif C\n`endif\n`endif",
          "`else\n`define X 1", "`ifdef X\n`endif\n`endif", "`ifdef X\n`else\n`else",
          "`endif\n`ifdef X", "`uvm_info(\"`line\", value, UVM_DEBUG)"})
        compareLegacy(text);
    std::mt19937 random(20261024);
    for (int i = 0; i < 500; ++i) {
        std::string text;
        for (int j = 0; j < 12; ++j)
            text += prefixes[random() % prefixes.size()] + names[random() % names.size()] +
                    suffixes[random() % suffixes.size()];
        compareLegacy(text);
    }
}
