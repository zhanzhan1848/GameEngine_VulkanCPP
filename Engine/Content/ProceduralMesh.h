#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMeshAsset.h"
#include "Content/ContentToEngine.h"
#include <cmath>
#include <cstring>

namespace primal::content {

// Elements: Normal(12B) + UV(8B) = 20B per vertex, matching CreateFromAsset "else" branch.
static constexpr u32 PROC_ELEM_STRIDE = 20;
static constexpr u32 PROC_ELEMENTS_TYPE = 0x03; // static_normal_texture

// --- Helpers ---

inline void WriteVertex(u8* pos, u8* elem, f32 px, f32 py, f32 pz,
                        f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
    f32 p[3] = {px, py, pz};
    memcpy(pos, p, 12);

    f32 n[3] = {nx, ny, nz};
    memcpy(elem, n, 12);
    f32 uv[2] = {u, v};
    memcpy(elem + 12, uv, 8);
}

inline id::id_type RegisterProceduralMesh(graphics::rhi::RHIMeshAsset& asset) {
    asset.lod_id = 0;
    asset.material_idx = 0;
    asset.lod_threshold = 0.f;
    asset.index_size = 4;
    asset.elements_type = PROC_ELEMENTS_TYPE;
    return register_mesh_asset(asset);
}

// --- Generators ---

inline id::id_type create_sphere_mesh(f32 radius, u32 segments, u32 rings) {
    const u32 vertCount = (rings + 1) * (segments + 1);
    const u32 idxCount = rings * segments * 6;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    u32 vi = 0;
    for (u32 r = 0; r <= rings; ++r) {
        f32 phi = 3.14159265f * f32(r) / f32(rings);
        f32 sinP = std::sin(phi), cosP = std::cos(phi);
        for (u32 s = 0; s <= segments; ++s) {
            f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
            f32 sinT = std::sin(theta), cosT = std::cos(theta);
            f32 nx = sinP * cosT, ny = cosP, nz = sinP * sinT;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        nx * radius, ny * radius, nz * radius,
                        nx, ny, nz,
                        f32(s) / f32(segments), f32(r) / f32(rings));
            ++vi;
        }
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            u32 a = r * (segments + 1) + s;
            u32 b = a + segments + 1;
            idx[ii++] = a;   idx[ii++] = b;   idx[ii++] = a + 1;
            idx[ii++] = a+1; idx[ii++] = b;   idx[ii++] = b + 1;
        }
    }

    return RegisterProceduralMesh(asset);
}

inline id::id_type create_cylinder_mesh(f32 radius, f32 height, u32 segments) {
    // Side vertices: (segments+1) * 2 + top cap center + bottom cap center
    const u32 sideVerts = 2 * (segments + 1);
    const u32 capSegs = segments + 2; // center + ring
    const u32 vertCount = sideVerts + 2 * capSegs;
    const u32 sideIdx = segments * 6;
    const u32 capIdx = segments * 3;
    const u32 idxCount = sideIdx + 2 * capIdx;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    const f32 halfH = height * 0.5f;

    // Side ring: bottom (0..segments) then top (segments+1..2*segments+1)
    u32 vi = 0;
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 c = std::cos(theta), sn = std::sin(theta);
        f32 u = f32(s) / f32(segments);
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, -halfH, sn * radius, c, 0.f, sn, u, 0.f);
        ++vi;
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, halfH, sn * radius, c, 0.f, sn, u, 1.f);
        ++vi;
    }

    // Bottom cap: center + ring
    u32 botCenter = vi;
    WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                0.f, -halfH, 0.f, 0.f, -1.f, 0.f, 0.5f, 0.5f);
    ++vi;
    u32 botRingStart = vi;
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 c = std::cos(theta), sn = std::sin(theta);
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, -halfH, sn * radius, 0.f, -1.f, 0.f, 0.5f + c * 0.5f, 0.5f + sn * 0.5f);
        ++vi;
    }

    // Top cap: center + ring
    u32 topCenter = vi;
    WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                0.f, halfH, 0.f, 0.f, 1.f, 0.f, 0.5f, 0.5f);
    ++vi;
    u32 topRingStart = vi;
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 c = std::cos(theta), sn = std::sin(theta);
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, halfH, sn * radius, 0.f, 1.f, 0.f, 0.5f + c * 0.5f, 0.5f - sn * 0.5f);
        ++vi;
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;

    // Side faces
    for (u32 s = 0; s < segments; ++s) {
        u32 bl = s * 2, tl = s * 2 + 1;
        u32 br = bl + 2, tr = tl + 2;
        idx[ii++] = bl; idx[ii++] = tl; idx[ii++] = tr;
        idx[ii++] = bl; idx[ii++] = tr; idx[ii++] = br;
    }

    // Bottom cap (CW looking from below = CCW winding)
    for (u32 s = 0; s < segments; ++s) {
        idx[ii++] = botCenter;
        idx[ii++] = botRingStart + s + 1;
        idx[ii++] = botRingStart + s;
    }

    // Top cap
    for (u32 s = 0; s < segments; ++s) {
        idx[ii++] = topCenter;
        idx[ii++] = topRingStart + s;
        idx[ii++] = topRingStart + s + 1;
    }

    return RegisterProceduralMesh(asset);
}

inline id::id_type create_cone_mesh(f32 radius, f32 height, u32 segments) {
    // Side: (segments+1) base ring + apex
    const u32 sideVerts = (segments + 1) + 1;
    const u32 capVerts = segments + 2; // center + ring
    const u32 vertCount = sideVerts + capVerts;
    const u32 sideIdx = segments * 3;
    const u32 capIdx = segments * 3;
    const u32 idxCount = sideIdx + capIdx;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    const f32 halfH = height * 0.5f;

    // Apex at top
    u32 apexIdx = 0;
    WriteVertex(pos, elem, 0.f, halfH, 0.f, 0.f, 1.f, 0.f, 0.5f, 1.f);

    // Base ring
    u32 baseStart = 1;
    u32 vi = 1;
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 c = std::cos(theta), sn = std::sin(theta);
        // Side normal: perpendicular to slant
        f32 slant = std::sqrt(radius * radius + height * height);
        f32 nx = height * c / slant, nz = height * sn / slant;
        f32 ny = radius / slant;
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, -halfH, sn * radius,
                    nx, ny, nz,
                    f32(s) / f32(segments), 0.f);
        ++vi;
    }

    // Cap center
    u32 capCenter = vi;
    WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                0.f, -halfH, 0.f, 0.f, -1.f, 0.f, 0.5f, 0.5f);
    ++vi;

    // Cap ring
    u32 capRingStart = vi;
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 c = std::cos(theta), sn = std::sin(theta);
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, -halfH, sn * radius, 0.f, -1.f, 0.f,
                    0.5f + c * 0.5f, 0.5f + sn * 0.5f);
        ++vi;
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;

    // Side faces (apex → base ring)
    for (u32 s = 0; s < segments; ++s) {
        idx[ii++] = apexIdx;
        idx[ii++] = baseStart + s + 1;
        idx[ii++] = baseStart + s;
    }

    // Cap
    for (u32 s = 0; s < segments; ++s) {
        idx[ii++] = capCenter;
        idx[ii++] = capRingStart + s + 1;
        idx[ii++] = capRingStart + s;
    }

    return RegisterProceduralMesh(asset);
}

inline id::id_type create_box_mesh(f32 sx, f32 sy, f32 sz) {
    // 6 faces × 4 verts = 24 vertices (separate normals per face)
    const u32 vertCount = 24;
    const u32 idxCount = 36; // 6 faces × 2 triangles × 3

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    const f32 hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;

    //    pos                           normal          UV
    // +Z face
    WriteVertex(pos+ 0*12, elem+ 0*20, -hx,-hy, hz,  0, 0, 1,  0, 0);
    WriteVertex(pos+ 1*12, elem+ 1*20,  hx,-hy, hz,  0, 0, 1,  1, 0);
    WriteVertex(pos+ 2*12, elem+ 2*20,  hx, hy, hz,  0, 0, 1,  1, 1);
    WriteVertex(pos+ 3*12, elem+ 3*20, -hx, hy, hz,  0, 0, 1,  0, 1);
    // -Z face
    WriteVertex(pos+ 4*12, elem+ 4*20,  hx,-hy,-hz,  0, 0,-1,  0, 0);
    WriteVertex(pos+ 5*12, elem+ 5*20, -hx,-hy,-hz,  0, 0,-1,  1, 0);
    WriteVertex(pos+ 6*12, elem+ 6*20, -hx, hy,-hz,  0, 0,-1,  1, 1);
    WriteVertex(pos+ 7*12, elem+ 7*20,  hx, hy,-hz,  0, 0,-1,  0, 1);
    // +X face
    WriteVertex(pos+ 8*12, elem+ 8*20,  hx,-hy, hz,  1, 0, 0,  0, 0);
    WriteVertex(pos+ 9*12, elem+ 9*20,  hx,-hy,-hz,  1, 0, 0,  1, 0);
    WriteVertex(pos+10*12, elem+10*20,  hx, hy,-hz,  1, 0, 0,  1, 1);
    WriteVertex(pos+11*12, elem+11*20,  hx, hy, hz,  1, 0, 0,  0, 1);
    // -X face
    WriteVertex(pos+12*12, elem+12*20, -hx,-hy,-hz, -1, 0, 0,  0, 0);
    WriteVertex(pos+13*12, elem+13*20, -hx,-hy, hz, -1, 0, 0,  1, 0);
    WriteVertex(pos+14*12, elem+14*20, -hx, hy, hz, -1, 0, 0,  1, 1);
    WriteVertex(pos+15*12, elem+15*20, -hx, hy,-hz, -1, 0, 0,  0, 1);
    // +Y face
    WriteVertex(pos+16*12, elem+16*20, -hx, hy, hz,  0, 1, 0,  0, 0);
    WriteVertex(pos+17*12, elem+17*20,  hx, hy, hz,  0, 1, 0,  1, 0);
    WriteVertex(pos+18*12, elem+18*20,  hx, hy,-hz,  0, 1, 0,  1, 1);
    WriteVertex(pos+19*12, elem+19*20, -hx, hy,-hz,  0, 1, 0,  0, 1);
    // -Y face
    WriteVertex(pos+20*12, elem+20*20, -hx,-hy,-hz,  0,-1, 0,  0, 0);
    WriteVertex(pos+21*12, elem+21*20,  hx,-hy,-hz,  0,-1, 0,  1, 0);
    WriteVertex(pos+22*12, elem+22*20,  hx,-hy, hz,  0,-1, 0,  1, 1);
    WriteVertex(pos+23*12, elem+23*20, -hx,-hy, hz,  0,-1, 0,  0, 1);

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    for (u32 f = 0; f < 6; ++f) {
        u32 b = f * 4;
        idx[f*6+0] = b;   idx[f*6+1] = b+1; idx[f*6+2] = b+2;
        idx[f*6+3] = b;   idx[f*6+4] = b+2; idx[f*6+5] = b+3;
    }

    return RegisterProceduralMesh(asset);
}

// --- Torus ---
inline id::id_type create_torus_mesh(f32 outerRadius, f32 innerRadius, u32 segments, u32 sides) {
    const u32 vertCount = (segments + 1) * (sides + 1);
    const u32 idxCount = segments * sides * 6;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    u32 vi = 0;
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 ct = std::cos(theta), st = std::sin(theta);
        for (u32 r = 0; r <= sides; ++r) {
            f32 phi = 2.f * 3.14159265f * f32(r) / f32(sides);
            f32 cp = std::cos(phi), sp = std::sin(phi);
            f32 dist = outerRadius + innerRadius * cp;
            f32 nx = cp * ct, ny = sp, nz = cp * st;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        dist * ct, innerRadius * sp, dist * st,
                        nx, ny, nz,
                        f32(s) / f32(segments), f32(r) / f32(sides));
            ++vi;
        }
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;
    for (u32 s = 0; s < segments; ++s) {
        for (u32 r = 0; r < sides; ++r) {
            u32 a = s * (sides + 1) + r;
            u32 b = a + sides + 1;
            idx[ii++] = a;   idx[ii++] = b;   idx[ii++] = a + 1;
            idx[ii++] = a+1; idx[ii++] = b;   idx[ii++] = b + 1;
        }
    }

    return RegisterProceduralMesh(asset);
}

// --- Capsule (cylinder + hemisphere caps) ---
inline id::id_type create_capsule_mesh(f32 radius, f32 height, u32 segments, u32 hemiRings) {
    const u32 cylVerts = 2 * (segments + 1);
    const u32 hemiVerts = (hemiRings + 1) * (segments + 1);
    const u32 vertCount = cylVerts + 2 * hemiVerts;
    const u32 cylIdx = segments * 6;
    const u32 hemiIdx = hemiRings * segments * 6;
    const u32 idxCount = cylIdx + 2 * hemiIdx;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    const f32 halfH = height * 0.5f;
    u32 vi = 0;

    // Cylinder side
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 c = std::cos(theta), sn = std::sin(theta);
        f32 u = f32(s) / f32(segments);
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, -halfH, sn * radius, c, 0.f, sn, u, 0.25f);
        ++vi;
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius,  halfH, sn * radius, c, 0.f, sn, u, 0.75f);
        ++vi;
    }

    // Top hemisphere
    for (u32 r = 0; r <= hemiRings; ++r) {
        f32 phi = 1.5707963f * f32(r) / f32(hemiRings); // 0 → π/2
        f32 sinP = std::sin(phi), cosP = std::cos(phi);
        for (u32 s = 0; s <= segments; ++s) {
            f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
            f32 sinT = std::sin(theta), cosT = std::cos(theta);
            f32 nx = sinP * cosT, ny = cosP, nz = sinP * sinT;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        nx * radius, halfH + ny * radius, nz * radius,
                        nx, ny, nz,
                        f32(s) / f32(segments), 0.75f + 0.25f * f32(r) / f32(hemiRings));
            ++vi;
        }
    }

    // Bottom hemisphere
    for (u32 r = 0; r <= hemiRings; ++r) {
        f32 phi = -1.5707963f * f32(r) / f32(hemiRings); // 0 → -π/2
        f32 sinP = std::sin(phi), cosP = std::cos(phi);
        for (u32 s = 0; s <= segments; ++s) {
            f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
            f32 sinT = std::sin(theta), cosT = std::cos(theta);
            f32 nx = sinP * cosT, ny = cosP, nz = sinP * sinT;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        nx * radius, -halfH + ny * radius, nz * radius,
                        nx, ny, nz,
                        f32(s) / f32(segments), 0.25f * f32(r) / f32(hemiRings));
            ++vi;
        }
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;

    // Cylinder sides
    for (u32 s = 0; s < segments; ++s) {
        u32 bl = s * 2, tl = s * 2 + 1;
        u32 br = bl + 2, tr = tl + 2;
        idx[ii++] = bl; idx[ii++] = tl; idx[ii++] = tr;
        idx[ii++] = bl; idx[ii++] = tr; idx[ii++] = br;
    }

    // Top hemisphere
    u32 hemiStart = cylVerts;
    for (u32 r = 0; r < hemiRings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            u32 a = hemiStart + r * (segments + 1) + s;
            u32 b = a + segments + 1;
            idx[ii++] = a;   idx[ii++] = b;   idx[ii++] = a + 1;
            idx[ii++] = a+1; idx[ii++] = b;   idx[ii++] = b + 1;
        }
    }

    // Bottom hemisphere
    hemiStart = cylVerts + hemiVerts;
    for (u32 r = 0; r < hemiRings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            u32 a = hemiStart + r * (segments + 1) + s;
            u32 b = a + segments + 1;
            idx[ii++] = a;   idx[ii++] = b;   idx[ii++] = a + 1;
            idx[ii++] = a+1; idx[ii++] = b;   idx[ii++] = b + 1;
        }
    }

    return RegisterProceduralMesh(asset);
}

// --- Disc (flat circle in XZ plane, Y-up) ---
inline id::id_type create_disc_mesh(f32 radius, u32 segments) {
    const u32 vertCount = segments + 2; // center + ring + duplicate for UV seam
    const u32 idxCount = segments * 3;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();

    // Center vertex
    u32 center = 0;
    WriteVertex(pos, elem, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.5f, 0.5f);
    u32 vi = 1;
    for (u32 s = 0; s <= segments; ++s) {
        f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
        f32 c = std::cos(theta), sn = std::sin(theta);
        WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                    c * radius, 0.f, sn * radius, 0.f, 1.f, 0.f,
                    0.5f + c * 0.5f, 0.5f + sn * 0.5f);
        ++vi;
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;
    for (u32 s = 0; s < segments; ++s) {
        idx[ii++] = center;
        idx[ii++] = 1 + s;
        idx[ii++] = 1 + s + 1;
    }

    return RegisterProceduralMesh(asset);
}

// --- Hemisphere (top half of sphere, open bottom) ---
inline id::id_type create_hemisphere_mesh(f32 radius, u32 segments, u32 rings) {
    const u32 vertCount = (rings + 1) * (segments + 1);
    const u32 idxCount = rings * segments * 6;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    u32 vi = 0;
    for (u32 r = 0; r <= rings; ++r) {
        f32 phi = 1.5707963f * f32(r) / f32(rings); // 0 → π/2
        f32 sinP = std::sin(phi), cosP = std::cos(phi);
        for (u32 s = 0; s <= segments; ++s) {
            f32 theta = 2.f * 3.14159265f * f32(s) / f32(segments);
            f32 sinT = std::sin(theta), cosT = std::cos(theta);
            f32 nx = sinP * cosT, ny = cosP, nz = sinP * sinT;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        nx * radius, ny * radius, nz * radius,
                        nx, ny, nz,
                        f32(s) / f32(segments), f32(r) / f32(rings));
            ++vi;
        }
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            u32 a = r * (segments + 1) + s;
            u32 b = a + segments + 1;
            idx[ii++] = a;   idx[ii++] = b;   idx[ii++] = a + 1;
            idx[ii++] = a+1; idx[ii++] = b;   idx[ii++] = b + 1;
        }
    }

    return RegisterProceduralMesh(asset);
}

// --- Pyramid (4-sided base + apex) ---
inline id::id_type create_pyramid_mesh(f32 base, f32 height) {
    const f32 hb = base * 0.5f;
    const f32 halfH = height * 0.5f;
    // 4 side quads (2 tris each) + bottom quad = 5 faces × 4 verts = 20 verts
    const u32 vertCount = 20;
    const u32 idxCount = 30; // 5 faces × 2 tris × 3

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();

    // Slant normal helper
    auto slantNormal = [](f32 bx, f32 bz, f32 halfH, f32 h) -> void {
        // Each face normal is perpendicular to the slant edge in the face plane
        // For face pointing +Z: normal = normalize(0, base/2, h/2) projected to face plane
        // Simplified: use cross product of two edges
    };

    // 4 base corners: A(-hb,-halfH,-hb) B(+hb,-halfH,-hb) C(+hb,-halfH,+hb) D(-hb,-halfH,+hb)
    // Apex at (0, halfH, 0)
    // For each side face, compute proper flat normal

    // Front face (+Z): D C Apex
    f32 fn = std::sqrt(hb * hb + height * height);
    f32 sny = hb / fn, snz = height * 0.5f / fn;
    // +Z face
    WriteVertex(pos+ 0*12, elem+ 0*20, -hb, -halfH, hb,  0, sny, snz,  0, 0);
    WriteVertex(pos+ 1*12, elem+ 1*20,  hb, -halfH, hb,  0, sny, snz,  1, 0);
    WriteVertex(pos+ 2*12, elem+ 2*20,  hb,  halfH,  0,  0, sny, snz,  1, 1);
    WriteVertex(pos+ 3*12, elem+ 3*20, -hb,  halfH,  0,  0, sny, snz,  0, 1);
    // -Z face
    WriteVertex(pos+ 4*12, elem+ 4*20,  hb, -halfH,-hb,  0, sny,-snz,  0, 0);
    WriteVertex(pos+ 5*12, elem+ 5*20, -hb, -halfH,-hb,  0, sny,-snz,  1, 0);
    WriteVertex(pos+ 6*12, elem+ 6*20, -hb,  halfH,  0,  0, sny,-snz,  1, 1);
    WriteVertex(pos+ 7*12, elem+ 7*20,  hb,  halfH,  0,  0, sny,-snz,  0, 1);
    // +X face
    WriteVertex(pos+ 8*12, elem+ 8*20,  hb, -halfH, hb,  snz, sny, 0,  0, 0);
    WriteVertex(pos+ 9*12, elem+ 9*20,  hb, -halfH,-hb,  snz, sny, 0,  1, 0);
    WriteVertex(pos+10*12, elem+10*20,  hb,  halfH,  0,  snz, sny, 0,  1, 1);
    WriteVertex(pos+11*12, elem+11*20,  hb,  halfH,  0,  snz, sny, 0,  0, 1);
    // -X face
    WriteVertex(pos+12*12, elem+12*20, -hb, -halfH,-hb, -snz, sny, 0,  0, 0);
    WriteVertex(pos+13*12, elem+13*20, -hb, -halfH, hb, -snz, sny, 0,  1, 0);
    WriteVertex(pos+14*12, elem+14*20, -hb,  halfH,  0, -snz, sny, 0,  1, 1);
    WriteVertex(pos+15*12, elem+15*20, -hb,  halfH,  0, -snz, sny, 0,  0, 1);
    // Bottom face (-Y)
    WriteVertex(pos+16*12, elem+16*20, -hb, -halfH,-hb,  0,-1, 0,  0, 0);
    WriteVertex(pos+17*12, elem+17*20,  hb, -halfH,-hb,  0,-1, 0,  1, 0);
    WriteVertex(pos+18*12, elem+18*20,  hb, -halfH, hb,  0,-1, 0,  1, 1);
    WriteVertex(pos+19*12, elem+19*20, -hb, -halfH, hb,  0,-1, 0,  0, 1);

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    for (u32 f = 0; f < 5; ++f) {
        u32 b = f * 4;
        idx[f*6+0] = b;   idx[f*6+1] = b+1; idx[f*6+2] = b+2;
        idx[f*6+3] = b;   idx[f*6+4] = b+2; idx[f*6+5] = b+3;
    }

    return RegisterProceduralMesh(asset);
}

// --- Plane (tessellated grid in XZ plane) ---
inline id::id_type create_plane_mesh(f32 width, f32 depth, u32 wSegs, u32 dSegs) {
    const u32 vertCount = (wSegs + 1) * (dSegs + 1);
    const u32 idxCount = wSegs * dSegs * 6;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = vertCount;
    asset.num_indices = idxCount;
    asset.position_buffer.resize(vertCount * 12);
    asset.element_buffer.resize(vertCount * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(idxCount * 4);

    u8* pos = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    const f32 hw = width * 0.5f, hd = depth * 0.5f;

    u32 vi = 0;
    for (u32 dz = 0; dz <= dSegs; ++dz) {
        f32 z = -hd + depth * f32(dz) / f32(dSegs);
        for (u32 dx = 0; dx <= wSegs; ++dx) {
            f32 x = -hw + width * f32(dx) / f32(wSegs);
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        x, 0.f, z, 0.f, 1.f, 0.f,
                        f32(dx) / f32(wSegs), f32(dz) / f32(dSegs));
            ++vi;
        }
    }

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 ii = 0;
    for (u32 dz = 0; dz < dSegs; ++dz) {
        for (u32 dx = 0; dx < wSegs; ++dx) {
            u32 a = dz * (wSegs + 1) + dx;
            u32 b = a + wSegs + 1;
            idx[ii++] = a;   idx[ii++] = b;   idx[ii++] = a + 1;
            idx[ii++] = a+1; idx[ii++] = b;   idx[ii++] = b + 1;
        }
    }

    return RegisterProceduralMesh(asset);
}

} // namespace primal::content
