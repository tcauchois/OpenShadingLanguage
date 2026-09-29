# Copyright Contributors to the Open Shading Language project.
# SPDX-License-Identifier: BSD-3-Clause
# https://github.com/AcademySoftwareFoundation/OpenShadingLanguage
#!/usr/bin/env python 

command += oiiotool("-pattern fill:topleft=0,0,0:topright=1,0,0:bottomleft=0,1,0:bottomright=1,1,1 256x128 3 -d uint8 -oenv ramp.env")
command += testshade("-g 256 128 -od uint8 -o Cout out.tif test")
outputs = [ "out.tif" ]

# Vulkan: The GPU doesn't filter across the whole row at the poles, as OIIO does.
if os.environ.get("TESTSHADE_VULKAN") :
    failthresh = 0.02
    failpercent = 2
    hardfail = 0.8
