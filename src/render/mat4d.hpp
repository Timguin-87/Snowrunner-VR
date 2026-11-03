#pragma once
#include <cmath>

// DOUBLE-PRECISION 4x4 MATRICES for the passes that unproject the game's depth
// into world space (render/dlss.cpp, render/motion_field.cpp). Row-major,
// clip = M * p, exactly as cbuffer_hook snapshots the game's camera.
//
// The world here is a few hundred metres across and a pixel's depth is a few
// centimetres of it, so the matrices are moved to the current camera position
// in double precision (relative_to) before they go to the GPU as floats --
// world coordinates in the shaders then stay small.
namespace mat4d {

struct M4 { double m[16]; };

inline M4 load(const float* f) { M4 r; for (int i = 0; i < 16; ++i) r.m[i] = f[i]; return r; }

inline void store(float* dst, const M4& M) { for (int i = 0; i < 16; ++i) dst[i] = (float)M.m[i]; }

inline M4 mul(const M4& a, const M4& b)
{
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

inline bool inverse(const M4& in, M4& out)
{
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = (j < 4) ? in.m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-300) return false;
        if (piv != c) for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[piv][j]; a[piv][j] = t; }
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f != 0.0) for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out.m[i * 4 + j] = a[i][j + 4];
    return true;
}

// M with the world moved to `o`: M_rel * p_rel == M * (p_rel + o).
inline M4 relative_to(const M4& M, const double o[3])
{
    M4 T{};
    T.m[0] = T.m[5] = T.m[10] = T.m[15] = 1.0;
    T.m[3] = o[0]; T.m[7] = o[1]; T.m[11] = o[2];
    return mul(M, T);
}

} // namespace mat4d
