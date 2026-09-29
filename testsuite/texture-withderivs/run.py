#!/usr/bin/env python

# Copyright Contributors to the Open Shading Language project.
# SPDX-License-Identifier: BSD-3-Clause
# https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

command += testshade("-g 256 256 --center -od uint8 -o Cout out.tif test")
outputs = [ "out.txt", "out.tif" ]

# Vulkan: The GPU's anisotropic filtering differs from OIIO's.
if os.environ.get("TESTSHADE_VULKAN") :
    failthresh = 0.08
    failpercent = 1
    hardfail = 0.25
