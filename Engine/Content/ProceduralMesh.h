#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMeshAsset.h"
#include "Content/ContentToEngine.h"
#include <cmath>
#include <cstring>

namespace primal::content {

// Elements: ColorTSign(4) + Normal u16[2](4) + Tangent u16[2](4) + UV f32[2](8) = 20B,
// matching engine `static_normal_texture` format consumed directly by Metal vertex
// shaders (see buildin_shader.metal `VertexElement`) and by GPUDrivenDrawPipeline
// 20-byte source branch.
static constexpr u32 PROC_ELEM_STRIDE = 20;
static constexpr u32 PROC_ELEMENTS_TYPE = 0x03; // static_normal_texture

// --- Helpers ---

// Quantize a normal/tangent XY component from [-1, 1] to u16.
// Shader decode: float(u16) * (2/65535) - 1.
inline u16 PackSignedNormalComponent(f32 c) {
    f32 q = (c + 1.f) * 32767.5f;
    if (q < 0.f) q = 0.f;
    if (q > 65535.f) q = 65535.f;
    return static_cast<u16>(q);
}

// Pack the 20-byte static_normal_texture element body (excluding position) into `elem`.
// Layout (must match Metal VertexElement in buildin_shader.metal and the GPU
// SurfaceNets kernel's pack_vertex_element):
//   elem[ 0.. 3] : u32 ColorTSign = 0x00FFFFFF | (t_sign_byte << 24)
//                  t_sign bit 1: normal Z sign (1 = +, 0 = -)
//                  t_sign bit 0: tangent Z sign (unused here, always 0)
//   elem[ 4.. 7] : u16 Normal[2]   (XY; shader reconstructs Z from t_sign bit 1)
//   elem[ 8..11] : u16 Tangent[2]  (encoded (1, 0); ForwardPBR ignores vertex tangent)
//   elem[12..19] : f32 UV[2]
// Caller must normalize (nx,ny,nz) before calling.
inline void PackVertexElement(u8* elem, f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
    const u16 n0 = PackSignedNormalComponent(nx);
    const u16 n1 = PackSignedNormalComponent(ny);
    const u16 t0 = PackSignedNormalComponent(1.f);
    const u16 t1 = PackSignedNormalComponent(0.f);

    u8 sign_byte = 0;
    if (nz >= 0.f) sign_byte |= 0x02;  // bit 1: normal Z sign

    const u32 color_tsign = 0x00FFFFFFu | (static_cast<u32>(sign_byte) << 24);

    memcpy(elem + 0,  &color_tsign, 4);
    memcpy(elem + 4,  &n0, 2);
    memcpy(elem + 6,  &n1, 2);
    memcpy(elem + 8,  &t0, 2);
    memcpy(elem + 10, &t1, 2);
    const f32 uv[2] = {u, v};
    memcpy(elem + 12, uv, 8);
}

// Writes one vertex: 12-byte position (px,py,pz) followed by a 20-byte
// static_normal_texture element body. See PackVertexElement above for the
// element body layout. Defensive normal normalization is applied before packing.
inline void WriteVertex(u8* pos, u8* elem, f32 px, f32 py, f32 pz,
                        f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
    f32 p[3] = {px, py, pz};
    memcpy(pos, p, 12);

    // Normalize defensively (callers normally pass unit normals already).
    f32 nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nlen > 1e-8f) { nx /= nlen; ny /= nlen; nz /= nlen; }
    else { nx = 0.f; ny = 1.f; nz = 0.f; }

    PackVertexElement(elem, nx, ny, nz, u, v);
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

// --- Quad (XY plane, for 2D material preview) ---
// Single quad in the XY plane facing +Z, spanning [-hw,hw]×[-hh,hh].
// Used by MaterialPreviewRenderer's SetMode2D path with orthographic projection.
inline id::id_type create_quad_xy_mesh(f32 width = 2.0f, f32 height = 2.0f) {
    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = 4;
    asset.num_indices  = 6;
    asset.position_buffer.resize(4 * 12);
    asset.element_buffer.resize(4 * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(6 * 4);

    const f32 hw = width * 0.5f, hh = height * 0.5f;
    u8* pos  = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    // Counter-clockwise when viewed from +Z. UV origin bottom-left.
    WriteVertex(pos +  0, elem +  0, -hw, -hh, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    WriteVertex(pos + 12, elem + 20,  hw, -hh, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
    WriteVertex(pos + 24, elem + 40,  hw,  hh, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    WriteVertex(pos + 36, elem + 60, -hw,  hh, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;
    return RegisterProceduralMesh(asset);
}

// --- Teapot (procedural lathe + path-swept parts) ---
// Generates a recognizable teapot silhouette using simple primitives:
//   body (lathe of profile curve) + lid (cone) + spout (curved tapered cylinder)
//   + handle (half torus). All parts share one vertex/index buffer.
// 'size' is the approximate body diameter. Produces watertight, well-formed geometry.
inline id::id_type create_teapot_mesh(f32 size = 1.0f, u32 tessellation = 16) {
    // Total vertex/index budget (computed below). Pre-allocate to avoid realloc.
    const u32 bodySegs   = tessellation;            // longitude slices
    const u32 bodyRings  = std::max<u32>(8u, tessellation / 2);  // latitude rings
    const u32 lidSegs    = tessellation;
    const u32 lidRings   = 4;
    const u32 spoutSegs  = std::max<u32>(8u, tessellation / 2);
    const u32 spoutRings = 6;
    const u32 handleSegs = std::max<u32>(8u, tessellation / 2);
    const u32 handleRings= 8;

    const u32 bodyVerts    = (bodyRings + 1) * (bodySegs + 1);
    const u32 bodyIdx      = bodyRings * bodySegs * 6;
    const u32 lidVerts     = (lidRings + 1) * (lidSegs + 1) + 1;  // +apex
    const u32 lidIdx       = lidRings * lidSegs * 6 + lidSegs * 3;  // +cap fan
    const u32 spoutVerts   = (spoutRings + 1) * (spoutSegs + 1);
    const u32 spoutIdx     = spoutRings * spoutSegs * 6;
    const u32 handleVerts  = (handleRings + 1) * (handleSegs + 1);
    const u32 handleIdx    = handleRings * handleSegs * 6;

    const u32 totalVerts = bodyVerts + lidVerts + spoutVerts + handleVerts;
    const u32 totalIdx   = bodyIdx + lidIdx + spoutIdx + handleIdx;

    graphics::rhi::RHIMeshAsset asset;
    asset.num_vertices = totalVerts;
    asset.num_indices  = totalIdx;
    asset.position_buffer.resize(totalVerts * 12);
    asset.element_buffer.resize(totalVerts * PROC_ELEM_STRIDE);
    asset.index_buffer.resize(totalIdx * 4);

    u8* pos  = asset.position_buffer.data();
    u8* elem = asset.element_buffer.data();
    u32* idx = reinterpret_cast<u32*>(asset.index_buffer.data());
    u32 vi = 0, ii = 0;

    const f32 s = size * 0.5f;   // body radius scale
    const f32 PI = 3.14159265358979323846f;

    // ---------- BODY (lathe) ----------
    // Profile: y in [-1.0, 1.0] (height ~2 units), radius varies smoothly.
    // Bottom: 0.55*s, mid: 0.95*s, top: 0.75*s. Forms a rounded pot.
    const f32 ba = 0.55f, bb = 0.95f, bc = 0.75f;  // base/mid/top radii coefficients
    auto bodyRadius = [&](f32 t) -> f32 {
        // t in [0,1], 0=bottom 1=top
        return s * (ba + (bb - ba) * std::sin(t * PI) * 0.85f + (bc - ba) * (t * t) * 0.3f);
    };
    // dR/dt (height direction). Used to slant the normal outward at the equator.
    auto bodyDRadius = [&](f32 t) -> f32 {
        return s * ((bb - ba) * std::cos(t * PI) * PI * 0.85f + (bc - ba) * 2.0f * t * 0.3f);
    };
    const u32 bodyBase = vi;
    for (u32 r = 0; r <= bodyRings; ++r) {
        const f32 t = f32(r) / f32(bodyRings);
        const f32 y = (t * 2.0f - 1.0f) * s;          // height
        const f32 rad = bodyRadius(t);
        const f32 dRdt = bodyDRadius(t);
        // dy/dt = 2*s; surface tangent in Y direction is (dR/dt, dy/dt, 0),
        // outward normal = (dy/dt, -dR/dt, 0) normalized after combining with longitude.
        for (u32 s2 = 0; s2 <= bodySegs; ++s2) {
            const f32 ang = 2.0f * PI * f32(s2) / f32(bodySegs);
            const f32 px = std::cos(ang) * rad;
            const f32 pz = std::sin(ang) * rad;
            // Normal: longitude direction (cos,0,sin) scaled by dy/dt, plus Y by -dR/dt.
            const f32 dyDt = 2.0f * s;
            const f32 nx = std::cos(ang) * dyDt;
            const f32 ny = -dRdt;
            const f32 nz = std::sin(ang) * dyDt;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        px, y, pz, nx, ny, nz, f32(s2) / f32(bodySegs), t);
            ++vi;
        }
    }
    for (u32 r = 0; r < bodyRings; ++r) {
        for (u32 s2 = 0; s2 < bodySegs; ++s2) {
            const u32 a = bodyBase + r * (bodySegs + 1) + s2;
            const u32 b = a + (bodySegs + 1);
            idx[ii++] = a;     idx[ii++] = b;     idx[ii++] = a + 1;
            idx[ii++] = a + 1; idx[ii++] = b;     idx[ii++] = b + 1;
        }
    }

    // ---------- LID (cone + small cap) ----------
    // Sits on top of body (y = +s), small cone narrowing upward to a knob.
    const f32 lidY0 = s;
    const f32 lidY1 = s * 1.4f;   // tip of cone
    const u32 lidBase = vi;
    for (u32 r = 0; r <= lidRings; ++r) {
        const f32 t = f32(r) / f32(lidRings);
        const f32 y = lidY0 + (lidY1 - lidY0) * t;
        const f32 rad = (1.0f - t) * s * 0.55f;
        for (u32 s2 = 0; s2 <= lidSegs; ++s2) {
            const f32 ang = 2.0f * PI * f32(s2) / f32(lidSegs);
            const f32 px = std::cos(ang) * rad;
            const f32 pz = std::sin(ang) * rad;
            // Slanted normal: roughly perpendicular to cone surface
            const f32 ny = 0.3f;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        px, y, pz, std::cos(ang), ny, std::sin(ang),
                        f32(s2) / f32(lidSegs), t);
            ++vi;
        }
    }
    for (u32 r = 0; r < lidRings; ++r) {
        for (u32 s2 = 0; s2 < lidSegs; ++s2) {
            const u32 a = lidBase + r * (lidSegs + 1) + s2;
            const u32 b = a + (lidSegs + 1);
            idx[ii++] = a;     idx[ii++] = b;     idx[ii++] = a + 1;
            idx[ii++] = a + 1; idx[ii++] = b;     idx[ii++] = b + 1;
        }
    }
    // Apex cap fan
    const u32 lidApex = vi;
    WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                0.0f, lidY1, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    ++vi;
    {
        const u32 lastRingBase = lidBase + lidRings * (lidSegs + 1);
        for (u32 s2 = 0; s2 < lidSegs; ++s2) {
            idx[ii++] = lidApex;
            idx[ii++] = lastRingBase + s2;
            idx[ii++] = lastRingBase + s2 + 1;
        }
    }

    // ---------- SPOUT (curved tapered cylinder) ----------
    // Path from body side (y ≈ +0.4s) curving outward and upward.
    const u32 spoutBase = vi;
    const f32 spoutAttachY = -0.3f * s;
    for (u32 r = 0; r <= spoutRings; ++r) {
        const f32 t = f32(r) / f32(spoutRings);        // 0=attach, 1=tip
        // Quadratic Bezier path: P0=(s*0.9, spoutAttachY, 0), P1=(s*1.8, s*0.6, 0), P2=(s*2.2, s*1.3, 0)
        const f32 u = 1.0f - t;
        const f32 cx = u*u*(s*0.9f) + 2*u*t*(s*1.8f) + t*t*(s*2.2f);
        const f32 cy = u*u*spoutAttachY + 2*u*t*(s*0.6f) + t*t*(s*1.3f);
        const f32 cz = 0.0f;
        // Tangent for framing
        const f32 tx = 2*u*((s*1.8f)-(s*0.9f)) + 2*t*((s*2.2f)-(s*1.8f));
        const f32 ty = 2*u*((s*0.6f)-spoutAttachY) + 2*t*((s*1.3f)-(s*0.6f));
        const f32 tz = 0.0f;
        const f32 tlen = std::sqrt(tx*tx + ty*ty + tz*tz) + 1e-8f;
        const f32 tnx = tx / tlen, tny = ty / tlen;
        // Build frame: tangent in XY plane, normal "up" = perpendicular
        const f32 ux = -tny, uy = tnx;  // perp in XY
        // For ring, use Z as one axis and (ux,uy,0) as other
        const f32 radius = (0.25f - 0.15f * t) * s;
        for (u32 s2 = 0; s2 <= spoutSegs; ++s2) {
            const f32 ang = 2.0f * PI * f32(s2) / f32(spoutSegs);
            // Ring axes: axis_a = (ux, uy, 0), axis_b = (0, 0, 1)
            const f32 ca = std::cos(ang), sa = std::sin(ang);
            const f32 px = cx + ca * ux * radius;
            const f32 py = cy + ca * uy * radius;
            const f32 pz = cz + sa * radius;
            // Normal points outward from path
            const f32 nx = ca * ux;
            const f32 ny = ca * uy;
            const f32 nz = sa;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        px, py, pz, nx, ny, nz, f32(s2) / f32(spoutSegs), t);
            ++vi;
        }
    }
    for (u32 r = 0; r < spoutRings; ++r) {
        for (u32 s2 = 0; s2 < spoutSegs; ++s2) {
            const u32 a = spoutBase + r * (spoutSegs + 1) + s2;
            const u32 b = a + (spoutSegs + 1);
            idx[ii++] = a;     idx[ii++] = b;     idx[ii++] = a + 1;
            idx[ii++] = a + 1; idx[ii++] = b;     idx[ii++] = b + 1;
        }
    }

    // ---------- HANDLE (half torus on opposite side from spout) ----------
    // Sweeps from (−s, topY) around to (−s, botY) on the X-negative side.
    const u32 handleBase = vi;
    const f32 handleCx = -s * 0.9f;
    const f32 handleMajorR = s * 0.55f;
    const f32 handleMinorR = s * 0.12f;
    for (u32 r = 0; r <= handleRings; ++r) {
        const f32 t = f32(r) / f32(handleRings);
        // Half circle in XY plane from angle PI (top) to angle 2PI (bottom), going through -X
        const f32 sweepAng = PI + t * PI;
        const f32 cx = handleCx + std::cos(sweepAng) * handleMajorR;
        const f32 cy = 0.0f + std::sin(sweepAng) * handleMajorR;
        const f32 cz = 0.0f;
        // Normal in XY plane (outward from torus center curve)
        const f32 nrmX = std::cos(sweepAng);
        const f32 nrmY = std::sin(sweepAng);
        for (u32 s2 = 0; s2 <= handleSegs; ++s2) {
            const f32 ringAng = 2.0f * PI * f32(s2) / f32(handleSegs);
            // Ring axes: in the torus tube, axis_a = (nrmX, nrmY, 0), axis_b = (0, 0, 1)
            const f32 ca = std::cos(ringAng), sa = std::sin(ringAng);
            const f32 px = cx + ca * nrmX * handleMinorR;
            const f32 py = cy + ca * nrmY * handleMinorR;
            const f32 pz = cz + sa * handleMinorR;
            WriteVertex(pos + vi * 12, elem + vi * PROC_ELEM_STRIDE,
                        px, py, pz, ca * nrmX, ca * nrmY, sa,
                        f32(s2) / f32(handleSegs), t);
            ++vi;
        }
    }
    for (u32 r = 0; r < handleRings; ++r) {
        for (u32 s2 = 0; s2 < handleSegs; ++s2) {
            const u32 a = handleBase + r * (handleSegs + 1) + s2;
            const u32 b = a + (handleSegs + 1);
            idx[ii++] = a;     idx[ii++] = b;     idx[ii++] = a + 1;
            idx[ii++] = a + 1; idx[ii++] = b;     idx[ii++] = b + 1;
        }
    }

    return RegisterProceduralMesh(asset);
}

} // namespace primal::content
