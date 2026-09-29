# Copyright Contributors to the Open Shading Language project.
# SPDX-License-Identifier: BSD-3-Clause
# https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

# Helpers for linking OSL-generated SPIR-V with renderer shaders.

add_library (oslspirv STATIC oslspirv.cpp)
target_include_directories (oslspirv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries (oslspirv
    PUBLIC
        Imath::Imath   # via OSL/oslconfig.h
    PRIVATE
        SPIRV-Headers::SPIRV-Headers
        SPIRV-Tools-link SPIRV-Tools-opt SPIRV-Tools-static)
set_target_properties (oslspirv PROPERTIES
                       POSITION_INDEPENDENT_CODE ON
                       FOLDER "Libraries")

# Vulkan compute runner for testshade and osltoy
add_library (oslvkcompute STATIC vkcompute.cpp)
target_include_directories (oslvkcompute PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries (oslvkcompute
    PUBLIC
        oslspirv
    PRIVATE
        Vulkan::Vulkan slang::slang)
set_target_properties (oslvkcompute PROPERTIES
                       POSITION_INDEPENDENT_CODE ON
                       FOLDER "Libraries")

add_executable (oslspvfix oslspvfix.cpp)
target_include_directories (oslspvfix BEFORE PRIVATE ${OpenImageIO_INCLUDES})
target_link_libraries (oslspvfix PRIVATE oslspirv OpenImageIO::OpenImageIO_Util)
set_target_properties (oslspvfix PROPERTIES FOLDER "Tools")
install_targets (oslspvfix)

# Runs LLVM's SPIR-V backend in its own process, for each shader group (see
# oslspvemit.cpp). The shading system finds it next to the library or in
# the bin directory.
add_executable (oslspvemit oslspvemit.cpp)
target_link_libraries (oslspvemit PRIVATE
                       ${LLVM_LIBRARIES} ${LLVM_LDFLAGS} ${LLVM_SYSTEM_LIBRARIES})
set_target_properties (oslspvemit PROPERTIES FOLDER "Tools")
install_targets (oslspvemit)

add_executable (oslspvc oslspvc.cpp)
target_include_directories (oslspvc BEFORE PRIVATE ${OpenImageIO_INCLUDES})
target_link_libraries (oslspvc PRIVATE oslspirv oslexec oslcomp)
set_target_properties (oslspvc PROPERTIES FOLDER "Tools")
install_targets (oslspvc)


if (OSL_BUILD_TESTS AND BUILD_TESTING AND SLANGC_EXECUTABLE)
    # Build the test fixtures: a Slang compute shader that calls an
    # [__extern] eval_osl, and an LLVM IR library that exports it.
    find_program (LLVM_LLC_TOOL NAMES "llc"
                  PATHS "${LLVM_DIRECTORY}/bin" NO_DEFAULT_PATH)
    set (_testdata ${CMAKE_CURRENT_SOURCE_DIR}/testdata)
    set (_host_spv ${CMAKE_CURRENT_BINARY_DIR}/test_host.spv)
    set (_lib_spv ${CMAKE_CURRENT_BINARY_DIR}/test_lib.spv)
    add_custom_command (OUTPUT ${_host_spv}
        COMMAND ${SLANGC_EXECUTABLE} ${_testdata}/host.slang -target spirv
                -profile spirv_1_6 -incomplete-library -o ${_host_spv}
        DEPENDS ${_testdata}/host.slang)
    add_custom_command (OUTPUT ${_lib_spv}
        COMMAND ${LLVM_LLC_TOOL} -mtriple=spirv1.6-unknown-vulkan1.3-compute
                -filetype=obj ${_testdata}/lib.ll -o ${_lib_spv}
        DEPENDS ${_testdata}/lib.ll)
    add_custom_target (oslspirv_testdata ALL DEPENDS ${_host_spv} ${_lib_spv})

    add_executable (oslspirv_test oslspirv_test.cpp)
    target_link_libraries (oslspirv_test PRIVATE oslspirv OpenImageIO::OpenImageIO_Util)
    target_include_directories (oslspirv_test BEFORE PRIVATE ${OpenImageIO_INCLUDES})
    set_target_properties (oslspirv_test PROPERTIES FOLDER "Unit Tests")
    add_dependencies (oslspirv_test oslspirv_testdata)
    add_test (unit_oslspirv ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/oslspirv_test
              ${_host_spv} ${_lib_spv})
endif ()
