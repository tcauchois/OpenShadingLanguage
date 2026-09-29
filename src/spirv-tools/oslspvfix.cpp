// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// oslspvfix -- patch and link SPIR-V modules produced by OSL and by
// renderer shader compilers (e.g. Slang).
//
//   oslspvfix --import eval_osl host.spv -o host_fixed.spv
//   oslspvfix --link host_fixed.spv osl_group.spv -o linked.spv

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <OpenImageIO/argparse.h>
#include <OpenImageIO/strutil.h>

#include "oslspirv.h"

using namespace OSL;


int
main(int argc, char* argv[])
{
    std::vector<std::string> inputs;
    std::vector<std::string> imports;
    std::string output;
    bool do_link  = false;
    bool validate = false;

    OIIO::ArgParse ap;
    // clang-format off
    ap.intro("oslspvfix -- patch and link SPIR-V modules for use with OSL\n");
    ap.usage("oslspvfix [options] input.spv [input2.spv ...] -o output.spv");
    ap.arg("filename")
      .hidden()
      .action([&](OIIO::cspan<const char*> argv){ inputs.emplace_back(argv[0]); });
    ap.arg("--import %s:NAME")
      .help("Turn the bodiless function NAME into an imported declaration (may be repeated)")
      .action([&](OIIO::cspan<const char*> argv){ imports.emplace_back(argv[1]); });
    ap.arg("--link", &do_link)
      .help("Link all inputs into one module");
    ap.arg("--validate", &validate)
      .help("Validate the output (Vulkan 1.3 rules if fully linked)");
    ap.arg("-o %s:FILENAME", &output)
      .help("Output file");
    // clang-format on

    if (ap.parse_args(argc, (const char**)argv) < 0 || inputs.empty()
        || output.empty()) {
        ap.print_help();
        return EXIT_FAILURE;
    }
    if (inputs.size() > 1 && !do_link) {
        fprintf(stderr, "oslspvfix: multiple inputs require --link\n");
        return EXIT_FAILURE;
    }

    std::string err;
    std::vector<std::vector<uint32_t>> modules(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (!spirv::read_file(inputs[i], modules[i], err)) {
            fprintf(stderr, "oslspvfix: %s\n", err.c_str());
            return EXIT_FAILURE;
        }
    }

    // Imports apply to the first input (the renderer's shader module).
    if (!imports.empty() && !spirv::make_imports(modules[0], imports, err)) {
        fprintf(stderr, "oslspvfix: %s: %s\n", inputs[0].c_str(), err.c_str());
        return EXIT_FAILURE;
    }

    std::vector<uint32_t> result;
    if (do_link) {
        if (!spirv::link(modules, result, err)) {
            fprintf(stderr, "oslspvfix: link failed: %s\n", err.c_str());
            return EXIT_FAILURE;
        }
    } else {
        result = std::move(modules[0]);
    }

    if (validate
        && !spirv::validate(result, spirv::has_linkage(result), err)) {
        fprintf(stderr, "oslspvfix: validation failed: %s\n", err.c_str());
        return EXIT_FAILURE;
    }

    if (!spirv::write_file(output, result, err)) {
        fprintf(stderr, "oslspvfix: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
