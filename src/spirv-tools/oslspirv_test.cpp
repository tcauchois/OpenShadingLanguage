// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// Usage: oslspirv_test host.spv lib.spv
//   host.spv: Slang compute shader with an [__extern] eval_osl stub
//   lib.spv:  LLVM-produced library exporting eval_osl

#include <string>
#include <vector>

#include <OpenImageIO/unittest.h>

#include "oslspirv.h"

using namespace OSL;


static void
test_link(const std::string& host_file, const std::string& lib_file)
{
    std::string err;
    std::vector<uint32_t> host, lib;
    OIIO_CHECK_ASSERT(spirv::read_file(host_file, host, err));
    OIIO_CHECK_ASSERT(spirv::read_file(lib_file, lib, err));

    // The Slang output is a complete shader with an empty stub, and the
    // LLVM output is a library.
    OIIO_CHECK_ASSERT(!spirv::has_linkage(host));
    OIIO_CHECK_ASSERT(spirv::has_linkage(lib));
    OIIO_CHECK_ASSERT(spirv::validate(lib, true, err));

    // Linking before the fixup fails: nothing resolves the stub, and the
    // exported definition duplicates it.
    std::vector<uint32_t> linked;
    err.clear();
    OIIO_CHECK_ASSERT(!spirv::link({ host, lib }, linked, err));

    // Unknown names and functions with bodies are rejected.
    std::vector<uint32_t> scratch = host;
    OIIO_CHECK_ASSERT(!spirv::make_imports(scratch, { "no_such_fn" }, err));
    OIIO_CHECK_ASSERT(!spirv::make_imports(scratch, { "main" }, err));

    // Patch the stub into an import and link.
    err.clear();
    OIIO_CHECK_ASSERT(spirv::make_imports(host, { "eval_osl" }, err));
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_ASSERT(spirv::has_linkage(host));
    OIIO_CHECK_ASSERT(spirv::validate(host, true, err));
    // A second attempt reports that the linkage is already there.
    scratch = host;
    OIIO_CHECK_ASSERT(!spirv::make_imports(scratch, { "eval_osl" }, err));

    err.clear();
    OIIO_CHECK_ASSERT(spirv::link({ host, lib }, linked, err));
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_ASSERT(!spirv::has_linkage(linked));
    err.clear();
    bool valid = spirv::validate(linked, false, err);
    OIIO_CHECK_ASSERT(valid);
    if (!valid)
        std::cout << err << "\n";
}



int
main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "usage: oslspirv_test host.spv lib.spv\n";
        return 1;
    }
    test_link(argv[1], argv[2]);
    return unit_test_failures;
}
