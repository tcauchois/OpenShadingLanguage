# Copyright Contributors to the Open Shading Language project.
# SPDX-License-Identifier: BSD-3-Clause
# https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

# osltoy-vk: osltoy with a Vulkan 3D viewport, running the shader in the
# fragment shader. (Adapted from osltoy, without sharing its code.)

set (CMAKE_AUTOMOC ON)
if (Qt6_POSITION_INDEPENDENT_CODE)
    set (CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
add_executable (osltoy-vk main.cpp toyapp.cpp toyrenderer.cpp vkview.cpp
                codeeditor.cpp)
target_include_directories (osltoy-vk BEFORE PRIVATE ${OpenImageIO_INCLUDES})
set_target_properties (osltoy-vk PROPERTIES FOLDER "Tools")
# (So that it can find stdosl.h when run from the build tree.)
target_compile_definitions (osltoy-vk PRIVATE OSL_QT_MAJOR=6
    OSLTOYVK_SOURCE_SHADERS="${CMAKE_SOURCE_DIR}/src/shaders")
target_link_libraries (osltoy-vk
    PRIVATE
        oslexec oslcomp oslspirv oslvkcompute
        Qt6::Core Qt6::Gui Qt6::Widgets
        Vulkan::Vulkan
        ${CMAKE_DL_LIBS})
install_targets (osltoy-vk)
