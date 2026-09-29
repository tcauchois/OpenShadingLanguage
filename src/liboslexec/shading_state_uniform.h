// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once
#include <OSL/shaderglobals.h>
#include "opcolor.h"

OSL_NAMESPACE_BEGIN

namespace pvt {

// Intent is for a device to export or serialize the ShadingStateUniform from the
// ShadingSystem into a device side buffer that can be passed into
// kernel launches.

// Requires data to be self-contained and flat in memory to be device friendly.


struct ShadingStateUniform {
    ColorSystem m_colorsystem;          ///< Data for current colorspace
    ustringhash m_commonspace_synonym;  ///< Synonym for "common" space
    bool m_unknown_coordsys_error;      ///< Error to use unknown xform name?
    int m_max_warnings_per_thread;  ///< How many warnings to display per thread before giving up?
};

#ifdef OSL_SPIRV_SHADEOPS
// SPIR-V (logical addressing) can't follow the ShaderGlobals'
// shadingStateUniform pointer. Instead, the shading system gives each
// compiled group a constant copy of the ShadingStateUniform in this global.
extern "C" const ShadingStateUniform osl_spirv_shading_state_uniform;

inline const ShadingStateUniform&
get_shading_state_uniform(const OpaqueExecContextPtr)
{
    return osl_spirv_shading_state_uniform;
}
#else
inline const ShadingStateUniform&
get_shading_state_uniform(const OpaqueExecContextPtr oec)
{
    auto ec = pvt::get_ec(oec);
    return *(const ShadingStateUniform*)(ec->shadingStateUniform);
}
#endif

inline bool
get_unknown_coordsys_error(const OpaqueExecContextPtr oec)
{
    return get_shading_state_uniform(oec).m_unknown_coordsys_error;
}

inline ustringhash
get_commonspace_synonym(const OpaqueExecContextPtr oec)
{
    return get_shading_state_uniform(oec).m_commonspace_synonym;
}

}  // namespace pvt

OSL_NAMESPACE_END
