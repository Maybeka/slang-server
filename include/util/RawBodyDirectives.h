// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>

namespace server {
struct RawBodyDirectiveStatus {
    bool changesState = false;
    bool hasConditionals = false;
    bool changesEnclosingConditional = false;
    std::size_t openConditionals = 0;
};

// Deliberately inspect raw text, including comments and strings, just as the
// prior conservative regex checks did. The word boundary is ASCII [A-Za-z0-9_].
inline RawBodyDirectiveStatus inspectRawBodyDirectives(std::string_view text) {
    static constexpr std::string_view stateDirectives[] = {"define",
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
                                                           "uselib"};
    auto word = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               ch == '_';
    };
    RawBodyDirectiveStatus result;
    for (auto at = text.find('`'); at != std::string_view::npos;) {
        const auto begin = at + 1;
        auto end = begin;
        while (end < text.size() && word(static_cast<unsigned char>(text[end])))
            ++end;
        const auto name = text.substr(begin, end - begin);
        for (auto directive : stateDirectives)
            if (name == directive) {
                result.changesState = true;
                break;
            }
        if (name == "ifdef" || name == "ifndef") {
            result.hasConditionals = true;
            ++result.openConditionals;
        }
        else if (name == "elsif" || name == "else" || name == "endif") {
            result.hasConditionals = true;
            if (result.openConditionals == 0)
                result.changesEnclosingConditional = true;
            else if (name == "endif")
                --result.openConditionals;
        }
        at = text.find('`', end);
    }
    return result;
}
} // namespace server
