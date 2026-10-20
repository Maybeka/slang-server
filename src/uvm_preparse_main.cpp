// SPDX-License-Identifier: MIT
#include "util/UvmParseArtifact.h"
#include <iostream>
#include <regex>

#include "slang/ast/Compilation.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/driver/Driver.h"
#include "slang/syntax/SyntaxPrinter.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4)
            throw std::runtime_error(
                "usage: uvm-preparse source-directory output "
                "[default|no-dpi|resource-converter|no-dpi-resource-converter]");
        const std::string_view profile = argc == 4 ? argv[3] : "default";
        if (profile != "default" && profile != "no-dpi" && profile != "resource-converter" &&
            profile != "no-dpi-resource-converter")
            throw std::runtime_error("unknown UVM macro profile");
        const auto root = std::filesystem::canonical(argv[1]);
        slang::SourceManager sm;
        if (auto error = sm.addUserDirectory(root))
            throw std::runtime_error(error.message());
        auto source = sm.readSource(root / "uvm_pkg.sv", nullptr, 0);
        if (!source)
            throw std::runtime_error(source.error().message());
        slang::driver::Driver defaults;
        defaults.addStandardArgs();
        auto options = defaults.createParseOptionBag();
        auto pp = options.getOrDefault<slang::parsing::PreprocessorOptions>();
        pp.captureUvmPrefix = true;
        pp.uvmPrefixRoot = (root / "uvm_pkg.sv").string();
        if (profile == "no-dpi" || profile == "no-dpi-resource-converter")
            pp.predefines.push_back("UVM_NO_DPI");
        if (profile == "resource-converter" || profile == "no-dpi-resource-converter")
            pp.predefines.push_back("UVM_USE_RESOURCE_CONVERTER");
        options.set(pp);
        auto tree = slang::syntax::SyntaxTree::fromBuffersWithCheckpoint(std::span(&*source, 1), sm,
                                                                         options);
        std::cerr << "resume=" << tree->isPrefixResumeSafe().value_or(false)
                  << " diagnostics=" << tree->diagnostics().size() << '\n';
        std::ostringstream stream(std::ios::binary);
        tree->writeParseArchive(stream, root);
        auto bytes = stream.str();
        // Round-trip the entire CST, including source metadata and macro definitions,
        // before publishing an artifact used by any server binary.
        slang::SourceManager restored;
        std::istringstream input(bytes, std::ios::binary);
        auto loaded = slang::syntax::SyntaxTree::readParseArchive(input, restored, root, options);
        auto print = [](const auto& t) {
            return slang::syntax::SyntaxPrinter(t->sourceManager())
                .setIncludeDirectives(true)
                .setExpandIncludes(true)
                .setExpandMacros(true)
                .print(*t)
                .str();
        };
        if (print(tree) != print(loaded) ||
            tree->getDefinedMacros().size() != loaded->getDefinedMacros().size())
            throw std::runtime_error("UVM parse archive round-trip mismatch");
        slang::ast::CompilationOptions co;
        co.flags |= slang::ast::CompilationFlags::PackageAnalysis;
        slang::ast::Compilation compilation(co);
        compilation.addSyntaxTree(loaded);
        for (auto& d : compilation.getAllDiagnostics())
            if (d.isError())
                throw std::runtime_error("restored UVM has semantic errors");
        std::set<std::string> dependencies;
        const std::regex conditional(R"(`(?:ifdef|ifndef|elsif)\s+([A-Za-z_][A-Za-z_0-9]*))");
        for (auto id : tree->getSourceBufferIds()) {
            const auto text = std::string(sm.getSourceText(id));
            for (std::sregex_iterator it(text.begin(), text.end(), conditional), end; it != end;
                 ++it)
                dependencies.insert((*it)[1]);
        }
        server::uvm_artifact::write(argv[2], bytes, dependencies,
                                    profile == "default" ? "" : profile);
        std::cout << "UVM preparse round-trip verified: " << bytes.size() << " bytes\n";
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
