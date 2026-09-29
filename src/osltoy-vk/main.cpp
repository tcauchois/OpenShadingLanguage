// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// osltoy-vk: osltoy, but with the shader running in the fragment shader of
// a Vulkan rasterizer that draws a lit sphere.

#include <iostream>

#include <QApplication>
#include <QVulkanInstance>

#undef foreach

#include <OpenImageIO/argparse.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/sysutil.h>

#include "toyapp.h"
#include "toyrenderer.h"

using namespace OSL;


int
main(int argc, char* argv[])
{
#ifdef OIIO_HAS_STACKTRACE
    OIIO::Sysutil::setup_crash_stacktrace("stdout");
#endif
    OIIO::Filesystem::convert_native_arguments(argc, (const char**)argv);

    int xres = 640, yres = 480;
    bool check = false;
    std::string snapshot;
    std::vector<std::string> diddle;
    int recompiles = 0;
    std::string script;
    std::vector<std::string> filenames, include_paths;
    OIIO::ArgParse ap;
    // clang-format off
    ap.intro("osltoy-vk -- interactive OSL plaything, on Vulkan\n" OSL_INTRO_STRING);
    ap.usage("osltoy-vk [options] [filename...]");
    ap.arg("filename")
      .hidden()
      .action([&](cspan<const char*> argv){ filenames.emplace_back(argv[0]); });
    ap.arg("--res %d:XRES %d:YRES", &xres, &yres)
      .help("Set the viewport resolution");
    ap.arg("--check", &check)
      .help("Compile the shaders, print build statistics, and exit");
    ap.arg("--snapshot %s:FILE", &snapshot)
      .help("Render, save the viewport to FILE, print statistics, and exit");
    ap.arg("--diddle %L:PARAM", &diddle)
      .help("Diddle (adjust interactively) PARAM (for testing)");
    ap.arg("--recompile %d:N", &recompiles)
      .help("Recompile N more times (for testing)");
    ap.arg("--script %s:ACTIONS", &script)
      .help("Run test actions (see ToyMainWindow::run_script)");
    ap.arg("-I DIRPATH")
      .action([&](cspan<const char*> argv){ include_paths.emplace_back(argv[1]); })
      .help("Add DIRPATH to the list of header search paths");
    // clang-format on
    ap.parse_args(argc, (const char**)argv);

    QApplication app(argc, argv);

    QVulkanInstance inst;
    inst.setApiVersion(QVersionNumber(1, 3));
    if (!inst.create()) {
        std::cerr << "osltoy-vk: could not create a Vulkan instance (error "
                  << inst.errorCode() << ")\n";
        return EXIT_FAILURE;
    }

    ToyRenderer* rend = new ToyRenderer;
    ToyMainWindow mainwin(rend, &inst, xres, yres);
    mainwin.set_include_search_paths(include_paths);
    for (auto&& filename : filenames)
        mainwin.open_file(filename);
    mainwin.recompile_shaders();
    for (auto&& d : diddle)
        mainwin.diddle(d);
    for (int i = 0; i < recompiles; ++i)
        mainwin.recompile_shaders();
    if (script.size())
        mainwin.run_script(script);
    if (check) {
        std::cout << mainwin.build_report() << "\n";
        return mainwin.build_ok() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    mainwin.show();
    if (snapshot.size())
        mainwin.snapshot_and_quit(snapshot);
    return app.exec();
}
