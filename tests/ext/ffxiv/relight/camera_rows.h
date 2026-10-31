#pragma once
// The camera buffer of the contact shadow tests: g_CameraParameter laid out the way the shaders read it (ext/ffxiv/relight/shaders/
// CameraRegisters.h: the view matrix's three rows at register 0, the inverse view's at 3, the view-projection's at 6, the inverse
// view-projection's at 10, the inverse projection's at 14 and the projection's at 18), built from a 4x4 view and a 4x4 projection.
// The two WARP tests share it, so a test that shades with the wrong matrix cannot pass: the pass's one-time layout check and the reference
// of the march both read this shape.
//
// Conventions, the same as the engine's: row-major storage, a column-vector multiply (so a view-projection is P * V), view space with the
// camera at the origin looking down -z.
#include "ext/ffxiv/relight/shaders/CameraRegisters.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace contact_test
{
struct Mat4
{
    float m[4][4] {};
};

inline Mat4 Identity()
{
    Mat4 out;
    for (int i = 0; i < 4; ++i)
        out.m[i][i] = 1.0f;
    return out;
}

// a * b, the product that turns a column vector with b first and then a.
inline Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a.m[r][k] * b.m[k][c];
            out.m[r][c] = sum;
        }
    return out;
}

// The inverse of a 4x4, by Gauss-Jordan with partial pivoting. Returns false for a singular matrix (the caller builds another one).
inline bool Invert(const Mat4& in, Mat4& out)
{
    double a[4][8] {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            a[r][c] = in.m[r][c];
    for (int r = 0; r < 4; ++r)
        a[r][4 + r] = 1.0;
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12)
            return false;
        if (pivot != c)
            for (int k = 0; k < 8; ++k)
            {
                const double t = a[c][k];
                a[c][k] = a[pivot][k];
                a[pivot][k] = t;
            }
        const double scale = a[c][c];
        for (int k = 0; k < 8; ++k)
            a[c][k] /= scale;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0)
            {
                const double factor = a[r][c];
                for (int k = 0; k < 8; ++k)
                    a[r][k] -= factor * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out.m[r][c] = float(a[r][4 + c]);
    return true;
}

// The projection of a right-handed view space looking down -z: clip.w = -view.z, so a bigger depth is farther away.
inline Mat4 Projection(float nearPlane, float farPlane, float fovDegrees, float aspect)
{
    const float tanY = std::tan(fovDegrees * 0.5f * 3.14159265358979f / 180.0f);
    const float tanX = tanY * aspect;
    Mat4 p;
    p.m[0][0] = 1.0f / tanX;
    p.m[1][1] = 1.0f / tanY;
    p.m[2][2] = farPlane / (nearPlane - farPlane);
    p.m[2][3] = nearPlane * farPlane / (nearPlane - farPlane);
    p.m[3][2] = -1.0f;
    return p;
}

// The depth the projection writes for a point `metres` in front of the camera.
inline float DepthOf(const Mat4& projection, float metres)
{
    const float clipZ = -projection.m[2][2] * metres + projection.m[2][3];
    const float clipW = metres;
    return clipZ / clipW;
}

// The camera buffer the pass copies (0x120 bytes) and the one-time layout check reads (0x160).
struct CameraRows
{
    float v[CS_CAMERA_CHECK_BYTES / 16][4] {};

    void Put(uint32_t registerIndex, const Mat4& m)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                v[registerIndex + r][c] = m.m[r][c];
    }
    // Only the first three rows of a 3x4 matrix (the view and its inverse): the fourth row is not in the buffer.
    void Put3x4(uint32_t registerIndex, const Mat4& m)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                v[registerIndex + r][c] = m.m[r][c];
    }
};

// The whole buffer, from a world-to-view matrix and a projection.
inline CameraRows MakeCamera(const Mat4& view, const Mat4& projection)
{
    CameraRows rows;
    Mat4 inverseView, inverseProjection;
    Invert(view, inverseView);
    Invert(projection, inverseProjection);
    const Mat4 viewProjection = Multiply(projection, view);
    Mat4 inverseViewProjection;
    Invert(viewProjection, inverseViewProjection);
    rows.Put3x4(CS_CAMERA_VIEW_REGISTER, view);
    rows.Put3x4(CS_CAMERA_INVERSE_VIEW_REGISTER, inverseView);
    rows.Put(CS_CAMERA_VIEW_PROJECTION_REGISTER, viewProjection);
    rows.Put(CS_CAMERA_INVERSE_VIEW_PROJECTION_REGISTER, inverseViewProjection);
    rows.Put(CS_CAMERA_INVERSE_PROJECTION_REGISTER, inverseProjection);
    rows.Put(CS_CAMERA_PROJECTION_REGISTER, projection);
    return rows;
}

// A yaw quarter turn about y: view.x = world.z, view.z = -world.x (the test's "the camera turned" case).
inline Mat4 YawQuarter()
{
    Mat4 m = Identity();
    m.m[0][0] = 0.0f;
    m.m[0][2] = 1.0f;
    m.m[2][0] = -1.0f;
    m.m[2][2] = 0.0f;
    return m;
}
} // namespace contact_test
