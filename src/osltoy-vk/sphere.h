// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>


namespace osltoyvk {

struct Vertex {
    float P[3];
    float N[3];
    float uv[2];
    float dPdu[3];
};

// A unit UV sphere: `rows` rings of latitude and 2 * rows segments of
// longitude, with a duplicated seam so that u runs from 0 to 1 (with the
// same parameterization as testrender's spheres: y is up, u increases
// around y, and v runs from the top pole (0) to the bottom (1)).
inline void
make_sphere(int rows, std::vector<Vertex>& vertices,
            std::vector<uint32_t>& indices)
{
    const int cols = 2 * rows;
    const float pi = 3.14159265358979f;
    vertices.clear();
    indices.clear();
    for (int y = 0; y <= rows; ++y) {
        float v = float(y) / rows;
        float z = cosf(v * pi);
        float q = sinf(v * pi);
        for (int x = 0; x <= cols; ++x) {
            float u = float(x) / cols;
            float a = 2 * pi * u;
            Vertex vtx;
            float n[3] = { q * -sinf(a), z, q * -cosf(a) };
            for (int c = 0; c < 3; ++c)
                vtx.P[c] = vtx.N[c] = n[c];
            vtx.uv[0] = u;
            vtx.uv[1] = v;
            // dP/du: around the y axis (nonzero at the poles too, where it
            // gives a consistent tangent direction).
            float s      = 2 * pi * (q > 1e-4f ? q : 1e-4f);
            vtx.dPdu[0]  = s * -cosf(a);
            vtx.dPdu[1]  = 0;
            vtx.dPdu[2]  = s * sinf(a);
            vertices.push_back(vtx);
        }
    }
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            uint32_t i00 = y * (cols + 1) + x, i10 = i00 + 1;
            uint32_t i01 = i00 + (cols + 1), i11 = i01 + 1;
            // Counter-clockwise as seen from outside.
            indices.insert(indices.end(), { i00, i01, i10, i10, i01, i11 });
        }
}

}  // namespace osltoyvk
