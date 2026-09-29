// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// oslspvc -- compile an OSL shader to a SPIR-V library that exports
// `eval_osl(ShaderGlobals*, Groupdata*)`. Mostly a testing aid for the
// SPIR-V backend.
//
//   oslspvc shader.osl -o shader.spv
//   oslspvc shader.oso -o shader.spv --param Kd float 0.5

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <OpenImageIO/argparse.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>

#include <OSL/oslcomp.h>
#include <OSL/oslexec.h>
#include <OSL/rendererservices.h>

#include "oslspirv.h"

using namespace OSL;


// Renderer services that ask OSL to generate SPIR-V.
class SpirvRendererServices final : public RendererServices {
public:
    int supports(string_view feature) const override
    {
        return feature == "SPIRV";
    }
};



int
main(int argc, char* argv[])
{
    std::string input, output, outputs, abi_output;
    bool validate = false;
    int optimize  = 1;

    OIIO::ArgParse ap;
    // clang-format off
    ap.intro("oslspvc -- compile an OSL shader to a SPIR-V library\n");
    ap.usage("oslspvc [options] shader.osl|shader.oso -o output.spv");
    ap.arg("filename")
      .hidden()
      .action([&](OIIO::cspan<const char*> argv){ input = argv[0]; });
    ap.arg("-o %s:FILENAME", &output)
      .help("Output SPIR-V file");
    ap.arg("--outputs %s:NAMES", &outputs)
      .help("Comma-separated renderer outputs (default: all output params)");
    ap.arg("-O %d:LEVEL", &optimize)
      .help("LLVM optimization level (default 1)");
    ap.arg("--validate", &validate)
      .help("Validate the resulting library");
    ap.arg("--abi %s:FILENAME", &abi_output)
      .help("Write Slang declarations for calling eval_osl");
    // clang-format on
    if (ap.parse_args(argc, (const char**)argv) < 0 || input.empty()
        || output.empty()) {
        ap.print_help();
        return EXIT_FAILURE;
    }

    std::string shadername = OIIO::Filesystem::replace_extension(
        OIIO::Filesystem::filename(input), "");
    std::string oso;
    if (OIIO::Strutil::ends_with(input, ".osl")) {
        OSLCompiler compiler;
        std::string source;
        if (!OIIO::Filesystem::read_text_file(input, source)
            || !compiler.compile_buffer(source, oso, {}, "", input)) {
            fprintf(stderr, "oslspvc: could not compile %s\n", input.c_str());
            return EXIT_FAILURE;
        }
    } else if (!OIIO::Filesystem::read_text_file(input, oso)) {
        fprintf(stderr, "oslspvc: could not read %s\n", input.c_str());
        return EXIT_FAILURE;
    }

    SpirvRendererServices rs;
    ShadingSystem ss(&rs);
    ss.attribute("llvm_optimize", optimize);
    if (!ss.LoadMemoryCompiledShader(shadername, oso))
        return EXIT_FAILURE;

    ShaderGroupRef group = ss.ShaderGroupBegin();
    ss.Shader(*group, "surface", shadername, "layer1");
    ss.ShaderGroupEnd(*group);

    std::vector<std::string> outnames = OIIO::Strutil::splits(outputs, ",");
    std::vector<const char*> outs;
    for (auto&& o : outnames)
        outs.push_back(o.c_str());
    if (!outs.empty())
        ss.attribute(group.get(), "renderer_outputs",
                     TypeDesc(TypeDesc::STRING, outs.size()), outs.data());

    ss.optimize_group(group.get(), nullptr);

    std::vector<uint32_t> spirv;
    ss.getattribute(group.get(), "spirv_compiled_version", TypeDesc::PTR,
                    &spirv);
    if (spirv.empty()) {
        fprintf(stderr, "oslspvc: no SPIR-V was generated\n");
        return EXIT_FAILURE;
    }

    std::string err;
    if (validate && !spirv::validate(spirv, true, err)) {
        fprintf(stderr, "oslspvc: validation failed: %s\n", err.c_str());
        spirv::write_file(output, spirv, err);
        return EXIT_FAILURE;
    }
    if (!spirv::write_file(output, spirv, err)) {
        fprintf(stderr, "oslspvc: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    if (!abi_output.empty()) {
        std::string abi;
        ss.getattribute(group.get(), "spirv_slang_abi", TypeDesc::PTR, &abi);
        if (!OIIO::Filesystem::write_text_file(abi_output, abi)) {
            fprintf(stderr, "oslspvc: could not write %s\n",
                    abi_output.c_str());
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
