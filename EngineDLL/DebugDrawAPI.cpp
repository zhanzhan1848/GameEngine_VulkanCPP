// DebugDrawAPI.cpp - C ABI entry points for Editor-side debug line drawing.
// Phase 5: forwards to Engine's debug_draw::queue(), which ForwardSceneRenderer
// drains in Pass 6 every frame. All buffering and threading lives in Engine;
// this file is a thin C ABI shim.
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/DebugDraw/DebugDrawQueue.h"

#include <cmath>

// MSVC 的 <cmath> 不定义 M_PI（POSIX 扩展，clang/gcc 有）——补回退定义
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace primal;

namespace {

void push_line(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, u32 rgb) {
    graphics::debug_draw::add_line(x0, y0, z0, x1, y1, z1, rgb);
}

} // anonymous namespace

EDITOR_INTERFACE void DrawDebugLine(f32 x0, f32 y0, f32 z0,
                                    f32 x1, f32 y1, f32 z1, u32 rgb)
{
    push_line(x0, y0, z0, x1, y1, z1, rgb);
}

EDITOR_INTERFACE void DrawDebugBoxWireframe(f32 cx, f32 cy, f32 cz,
                                            f32 ex, f32 ey, f32 ez, u32 rgb)
{
    // 8 corners
    f32 corners[8][3] = {
        {cx - ex, cy - ey, cz - ez}, // 0
        {cx + ex, cy - ey, cz - ez}, // 1
        {cx + ex, cy + ey, cz - ez}, // 2
        {cx - ex, cy + ey, cz - ez}, // 3
        {cx - ex, cy - ey, cz + ez}, // 4
        {cx + ex, cy - ey, cz + ez}, // 5
        {cx + ex, cy + ey, cz + ez}, // 6
        {cx - ex, cy + ey, cz + ez}, // 7
    };
    // 12 edges
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, // bottom
        {4,5},{5,6},{6,7},{7,4}, // top
        {0,4},{1,5},{2,6},{3,7}, // verticals
    };
    for (const auto& e : edges) {
        push_line(
            corners[e[0]][0], corners[e[0]][1], corners[e[0]][2],
            corners[e[1]][0], corners[e[1]][1], corners[e[1]][2],
            rgb);
    }
}

EDITOR_INTERFACE void DrawDebugSphereWireframe(f32 cx, f32 cy, f32 cz,
                                               f32 radius, u32 segments, u32 rgb)
{
    if (segments == 0) segments = 12;
    if (segments > 128) segments = 128;

    // Latitude rings
    for (u32 i = 1; i < segments; ++i) {
        f32 phi = (f32)M_PI * (f32)i / (f32)segments; // 0..pi
        f32 y = radius * cosf(phi);
        f32 r = radius * sinf(phi);
        for (u32 j = 0; j < segments; ++j) {
            f32 theta0 = 2.f * (f32)M_PI * (f32)j / (f32)segments;
            f32 theta1 = 2.f * (f32)M_PI * (f32)(j + 1) / (f32)segments;
            push_line(
                cx + r * cosf(theta0), cy + y, cz + r * sinf(theta0),
                cx + r * cosf(theta1), cy + y, cz + r * sinf(theta1),
                rgb);
        }
    }

    // Longitude lines
    for (u32 j = 0; j < segments; ++j) {
        f32 theta = 2.f * (f32)M_PI * (f32)j / (f32)segments;
        f32 ct = cosf(theta), st = sinf(theta);
        for (u32 i = 0; i < segments; ++i) {
            f32 phi0 = (f32)M_PI * (f32)i / (f32)segments;
            f32 phi1 = (f32)M_PI * (f32)(i + 1) / (f32)segments;
            push_line(
                cx + radius * sinf(phi0) * ct, cy + radius * cosf(phi0), cz + radius * sinf(phi0) * st,
                cx + radius * sinf(phi1) * ct, cy + radius * cosf(phi1), cz + radius * sinf(phi1) * st,
                rgb);
        }
    }
}

EDITOR_INTERFACE void DrawDebugFrustum(const f32* view_proj_4x4_row_major, u32 rgb)
{
    if (!view_proj_4x4_row_major) return;

    // Compute inverse of VP matrix
    const f32* m = view_proj_4x4_row_major;
    // 4x4 matrix inverse
    f32 inv[16];
    f32 det = 0;

    // Cofactor-based inverse
    // Using the standard 4x4 adjugate / determinant method
    f32 s[6]; // Determinants of 2x2 submatrices
    f32 c[6]; // Additional determinants

    // This is the MESA implementation of 4x4 inverse
    s[0] = m[0]*m[5] - m[4]*m[1];  c[0] = m[10]*m[15] - m[14]*m[11];
    s[1] = m[0]*m[9] - m[8]*m[1];  c[1] = m[6]*m[15] - m[14]*m[7];
    s[2] = m[0]*m[13]- m[12]*m[1]; c[2] = m[6]*m[11] - m[10]*m[7];
    s[3] = m[4]*m[9] - m[8]*m[5];  c[3] = m[2]*m[15] - m[14]*m[3];
    s[4] = m[4]*m[13]- m[12]*m[5]; c[4] = m[2]*m[11] - m[10]*m[3];
    s[5] = m[8]*m[13]- m[12]*m[9]; c[5] = m[2]*m[7]  - m[6]*m[3];

    det = s[0]*c[0] - s[1]*c[1] + s[2]*c[2] + s[3]*c[3] - s[4]*c[4] + s[5]*c[5];
    if (fabsf(det) < 1e-12f) return;
    f32 inv_det = 1.f / det;

    inv[0]  = ( m[5]*c[0] - m[9]*c[1] + m[13]*c[2]) * inv_det;
    inv[1]  = (-m[1]*c[0] + m[9]*c[3] - m[13]*c[4]) * inv_det;
    inv[2]  = ( m[1]*c[1] - m[5]*c[3] + m[13]*c[5]) * inv_det;
    inv[3]  = (-m[1]*c[2] + m[5]*c[4] - m[9]*c[5]) * inv_det;
    inv[4]  = (-m[4]*c[0] + m[8]*c[1] - m[12]*c[2]) * inv_det;
    inv[5]  = ( m[0]*c[0] - m[8]*c[3] + m[12]*c[4]) * inv_det;
    inv[6]  = (-m[0]*c[1] + m[4]*c[3] - m[12]*c[5]) * inv_det;
    inv[7]  = ( m[0]*c[2] - m[4]*c[4] + m[8]*c[5]) * inv_det;
    inv[8]  = ( m[7]*s[5] - m[11]*s[4] + m[15]*s[3]) * inv_det;
    inv[9]  = (-m[3]*s[5] + m[11]*s[2] - m[15]*s[1]) * inv_det;
    inv[10] = ( m[3]*s[4] - m[7]*s[2] + m[15]*s[0]) * inv_det;
    inv[11] = (-m[3]*s[3] + m[7]*s[1] - m[11]*s[0]) * inv_det;
    inv[12] = (-m[6]*s[5] + m[10]*s[4] - m[14]*s[3]) * inv_det;
    inv[13] = ( m[2]*s[5] - m[10]*s[2] + m[14]*s[1]) * inv_det;
    inv[14] = (-m[2]*s[4] + m[6]*s[2] - m[14]*s[0]) * inv_det;
    inv[15] = ( m[2]*s[3] - m[6]*s[1] + m[10]*s[0]) * inv_det;

    // NDC 8 corners of the frustum cube
    f32 ndc[8][4] = {
        {-1,-1,-1,1}, {1,-1,-1,1}, {1,1,-1,1}, {-1,1,-1,1},
        {-1,-1, 1,1}, {1,-1, 1,1}, {1,1, 1,1}, {-1,1, 1,1},
    };

    f32 corners[8][3];
    for (int i = 0; i < 8; ++i) {
        f32 x = inv[0]*ndc[i][0] + inv[4]*ndc[i][1] + inv[8]*ndc[i][2]  + inv[12]*ndc[i][3];
        f32 y = inv[1]*ndc[i][0] + inv[5]*ndc[i][1] + inv[9]*ndc[i][2]  + inv[13]*ndc[i][3];
        f32 z = inv[2]*ndc[i][0] + inv[6]*ndc[i][1] + inv[10]*ndc[i][2] + inv[14]*ndc[i][3];
        f32 w = inv[3]*ndc[i][0] + inv[7]*ndc[i][1] + inv[11]*ndc[i][2] + inv[15]*ndc[i][3];
        if (fabsf(w) < 1e-12f) w = 1e-12f;
        corners[i][0] = x / w;
        corners[i][1] = y / w;
        corners[i][2] = z / w;
    }

    // 12 edges
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, // near
        {4,5},{5,6},{6,7},{7,4}, // far
        {0,4},{1,5},{2,6},{3,7}, // connectors
    };
    for (const auto& e : edges) {
        push_line(
            corners[e[0]][0], corners[e[0]][1], corners[e[0]][2],
            corners[e[1]][0], corners[e[1]][1], corners[e[1]][2],
            rgb);
    }
}

// --- Queue inspection / management ---

// Returns the number of queued line segments.
EDITOR_INTERFACE u32 GetDebugLineCount()
{
    return graphics::debug_draw::line_count();
}

// Drain queued lines into caller-provided arrays. Returns count copied.
// Note: drain semantics — calling this clears the engine-side queue.
EDITOR_INTERFACE u32 GetQueuedDebugLines(f32* out_vertices, u32* out_rgb, u32 max_lines)
{
    std::vector<graphics::debug_draw::DebugLine> snapshot;
    const u32 drained = graphics::debug_draw::drain_into(snapshot);
    const u32 count = (drained < max_lines) ? drained : max_lines;
    for (u32 i = 0; i < count; ++i) {
        const auto& l = snapshot[i];
        if (out_vertices) {
            out_vertices[i * 6 + 0] = l.a.x;
            out_vertices[i * 6 + 1] = l.a.y;
            out_vertices[i * 6 + 2] = l.a.z;
            out_vertices[i * 6 + 3] = l.b.x;
            out_vertices[i * 6 + 4] = l.b.y;
            out_vertices[i * 6 + 5] = l.b.z;
        }
        if (out_rgb) {
            out_rgb[i] = l.rgb;
        }
    }
    return count;
}

EDITOR_INTERFACE void ClearDebugLines()
{
    graphics::debug_draw::clear();
}

// No-op now: draining happens inside ForwardSceneRenderer::Render automatically.
// Kept for ABI compatibility; safe to call.
EDITOR_INTERFACE void FlushDebugDraw()
{
}

