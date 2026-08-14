// Engine/Graphics/WFC/ProceduralRoomPack.cpp
#include "ProceduralRoomPack.h"

#include "../../Content/ProceduralMesh.h"
#include <cassert>

namespace primal::graphics::wfc {

namespace {

// 12 tiles: 3 footprint sizes × 4 door configurations.
//
// door_mask bit layout (matches test convention in TestProceduralRoomPack):
//   bit 0 (+X)  bit 1 (-X)  bit 2 (+Z)  bit 3 (-Z)
//
// The four door configurations used here:
//   0x1 = +X only            (single door)
//   0x3 = +X + -X            (opposite pair, X axis)
//   0xC = +Z + -Z            (opposite pair, Z axis)
//   0xF = all four doors     (4-way intersection)
constexpr ProceduralRoomPack::RoomTileDef kTiles[ProceduralRoomPack::kTileCount] = {
    // 3×3 footprint
    { "proc_room_3x3_door_n",  3, 0x1, WFCCategory::Primitive },
    { "proc_room_3x3_door_ns", 3, 0x3, WFCCategory::Primitive },
    { "proc_room_3x3_door_ew", 3, 0xC, WFCCategory::Primitive },
    { "proc_room_3x3_door_4",  3, 0xF, WFCCategory::Primitive },
    // 5×5 footprint
    { "proc_room_5x5_door_n",  5, 0x1, WFCCategory::Primitive },
    { "proc_room_5x5_door_ns", 5, 0x3, WFCCategory::Primitive },
    { "proc_room_5x5_door_ew", 5, 0xC, WFCCategory::Primitive },
    { "proc_room_5x5_door_4",  5, 0xF, WFCCategory::Primitive },
    // 7×7 footprint
    { "proc_room_7x7_door_n",  7, 0x1, WFCCategory::Primitive },
    { "proc_room_7x7_door_ns", 7, 0x3, WFCCategory::Primitive },
    { "proc_room_7x7_door_ew", 7, 0xC, WFCCategory::Primitive },
    { "proc_room_7x7_door_4",  7, 0xF, WFCCategory::Primitive },
};

// Tile geometry lives in a unit cube (extent ±0.5) so it fits the WFC wave
// grid (1 m × 1 m × 1 m cells) and matches the classifier's face basis.
constexpr f32 kHalfExtent = 0.5f;       // hx = hy = hz
constexpr f32 kDoorHalfWidth = 0.25f;   // door spans [-0.25, +0.25] (half of cell width)
constexpr f32 kDoorTop = 0.1f;          // door extends y=[-0.5, +0.1] (60% of wall height)

// Append a quad (4 verts + 6 indices) to `out`. Verts are emitted in CCW-from-
// outside order (P0, P1, P2, P3), so the triangles (P0,P1,P2) and (P0,P2,P3)
// have outward-facing normals under back-face culling. `vi`/`ii` are in/out
// cursors into the position/element/index buffers.
inline void EmitQuad(graphics::rhi::RHIMeshAsset& out, u32& vi, u32& ii,
                     const f32 p0[3], const f32 p1[3], const f32 p2[3], const f32 p3[3],
                     f32 nx, f32 ny, f32 nz) {
    u8* pos  = out.position_buffer.data();
    u8* elem = out.element_buffer.data();
    u32* idx = reinterpret_cast<u32*>(out.index_buffer.data());

    content::WriteVertex(pos + vi * 12, elem + vi * content::PROC_ELEM_STRIDE,
                         p0[0], p0[1], p0[2], nx, ny, nz, 0.f, 0.f);
    ++vi;
    content::WriteVertex(pos + vi * 12, elem + vi * content::PROC_ELEM_STRIDE,
                         p1[0], p1[1], p1[2], nx, ny, nz, 1.f, 0.f);
    ++vi;
    content::WriteVertex(pos + vi * 12, elem + vi * content::PROC_ELEM_STRIDE,
                         p2[0], p2[1], p2[2], nx, ny, nz, 1.f, 1.f);
    ++vi;
    content::WriteVertex(pos + vi * 12, elem + vi * content::PROC_ELEM_STRIDE,
                         p3[0], p3[1], p3[2], nx, ny, nz, 0.f, 1.f);
    ++vi;

    idx[ii++] = vi - 4; idx[ii++] = vi - 3; idx[ii++] = vi - 2;
    idx[ii++] = vi - 4; idx[ii++] = vi - 2; idx[ii++] = vi - 1;
}

// Emit a solid wall (one quad) for the wall index 0..3 = +X, -X, +Z, -Z.
// Wall vert order matches emit_box_geometry (CCW from outside).
inline void EmitSolidWall(graphics::rhi::RHIMeshAsset& out, u32& vi, u32& ii, u32 wall) {
    const f32 H = kHalfExtent;
    switch (wall) {
        case 0: { // +X
            const f32 p0[3] = {+H, -H, +H}, p1[3] = {+H, -H, -H}, p2[3] = {+H, +H, -H}, p3[3] = {+H, +H, +H};
            EmitQuad(out, vi, ii, p0, p1, p2, p3, 1.f, 0.f, 0.f);
            break;
        }
        case 1: { // -X
            const f32 p0[3] = {-H, -H, -H}, p1[3] = {-H, -H, +H}, p2[3] = {-H, +H, +H}, p3[3] = {-H, +H, -H};
            EmitQuad(out, vi, ii, p0, p1, p2, p3, -1.f, 0.f, 0.f);
            break;
        }
        case 2: { // +Z
            const f32 p0[3] = {-H, -H, +H}, p1[3] = {+H, -H, +H}, p2[3] = {+H, +H, +H}, p3[3] = {-H, +H, +H};
            EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, 1.f);
            break;
        }
        case 3: { // -Z
            const f32 p0[3] = {+H, -H, -H}, p1[3] = {-H, -H, -H}, p2[3] = {-H, +H, -H}, p3[3] = {+H, +H, -H};
            EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, -1.f);
            break;
        }
        default: assert(0 && "wall index out of range");
    }
}

// Emit a wall with a door cutout (three quads: top strip + two side strips).
// Door geometry: y=[-H, kDoorTop], axis_perp=[-kDoorHalfWidth, +kDoorHalfWidth]
// where axis_perp is the within-wall horizontal axis (z for X-walls, x for Z-walls).
inline void EmitDoorWall(graphics::rhi::RHIMeshAsset& out, u32& vi, u32& ii, u32 wall) {
    const f32 H = kHalfExtent;
    const f32 DW = kDoorHalfWidth;
    const f32 DT = kDoorTop;
    switch (wall) {
        case 0: { // +X — door in (y, z). From +X viewer: +Z is LEFT.
            // Top strip: y=[DT, +H], z=[-H, +H]
            { const f32 p0[3] = {+H, DT, +H}, p1[3] = {+H, DT, -H}, p2[3] = {+H, +H, -H}, p3[3] = {+H, +H, +H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 1.f, 0.f, 0.f); }
            // Left side strip z=[+DW, +H]: BL=(−H_y, +H_z), BR=(−H_y, +DW_z)
            { const f32 p0[3] = {+H, -H, +H}, p1[3] = {+H, -H, +DW}, p2[3] = {+H, DT, +DW}, p3[3] = {+H, DT, +H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 1.f, 0.f, 0.f); }
            // Right side strip z=[-H, -DW]: BL=(−H_y, -DW_z), BR=(−H_y, -H_z)
            { const f32 p0[3] = {+H, -H, -DW}, p1[3] = {+H, -H, -H}, p2[3] = {+H, DT, -H}, p3[3] = {+H, DT, -DW};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 1.f, 0.f, 0.f); }
            break;
        }
        case 1: { // -X — symmetric to +X
            { const f32 p0[3] = {-H, DT, -H}, p1[3] = {-H, DT, +H}, p2[3] = {-H, +H, +H}, p3[3] = {-H, +H, -H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, -1.f, 0.f, 0.f); }
            { const f32 p0[3] = {-H, -H, -H}, p1[3] = {-H, -H, -DW}, p2[3] = {-H, DT, -DW}, p3[3] = {-H, DT, -H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, -1.f, 0.f, 0.f); }
            { const f32 p0[3] = {-H, -H, +DW}, p1[3] = {-H, -H, +H}, p2[3] = {-H, DT, +H}, p3[3] = {-H, DT, +DW};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, -1.f, 0.f, 0.f); }
            break;
        }
        case 2: { // +Z — door in (y, x)
            { const f32 p0[3] = {-H, DT, +H}, p1[3] = {+H, DT, +H}, p2[3] = {+H, +H, +H}, p3[3] = {-H, +H, +H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, 1.f); }
            // Side strip x=[-H, -DW]
            { const f32 p0[3] = {-H, -H, +H}, p1[3] = {-DW, -H, +H}, p2[3] = {-DW, DT, +H}, p3[3] = {-H, DT, +H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, 1.f); }
            // Side strip x=[+DW, +H]
            { const f32 p0[3] = {+DW, -H, +H}, p1[3] = {+H, -H, +H}, p2[3] = {+H, DT, +H}, p3[3] = {+DW, DT, +H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, 1.f); }
            break;
        }
        case 3: { // -Z — symmetric to +Z
            { const f32 p0[3] = {+H, DT, -H}, p1[3] = {-H, DT, -H}, p2[3] = {-H, +H, -H}, p3[3] = {+H, +H, -H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, -1.f); }
            { const f32 p0[3] = {+H, -H, -H}, p1[3] = {+DW, -H, -H}, p2[3] = {+DW, DT, -H}, p3[3] = {+H, DT, -H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, -1.f); }
            { const f32 p0[3] = {-DW, -H, -H}, p1[3] = {-H, -H, -H}, p2[3] = {-H, DT, -H}, p3[3] = {-DW, DT, -H};
              EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, 0.f, -1.f); }
            break;
        }
        default: assert(0 && "wall index out of range");
    }
}

} // namespace

const ProceduralRoomPack::RoomTileDef (&ProceduralRoomPack::TileDefs())[kTileCount] {
    return kTiles;
}

void ProceduralRoomPack::GenerateTileMesh(u32 tile_index, graphics::rhi::RHIMeshAsset& out) {
    assert(tile_index < kTileCount);
    const u32 door_mask = kTiles[tile_index].door_mask;

    // Count walls with doors: each door wall contributes 12 verts / 18 indices
    // (3 sub-quads), each solid wall contributes 4 verts / 6 indices.
    // Floor and ceiling add 4 verts / 6 indices each.
    u32 door_count = 0;
    for (u32 bit = 0; bit < 4; ++bit) {
        if (door_mask & (1u << bit)) ++door_count;
    }
    const u32 solid_count = 4u - door_count;
    const u32 vert_count  = 4u + 4u + door_count * 12u + solid_count * 4u;
    const u32 idx_count   = 6u + 6u + door_count * 18u + solid_count * 6u;

    out.num_vertices = vert_count;
    out.num_indices  = idx_count;
    out.index_size   = 4;
    out.position_buffer.resize(vert_count * 12u);
    out.element_buffer.resize(vert_count * content::PROC_ELEM_STRIDE);
    out.index_buffer.resize(idx_count * 4u);

    u32 vi = 0, ii = 0;

    // Floor (-Y face). CCW from -Y viewer (matches emit_box_geometry v20..v23).
    {
        const f32 H = kHalfExtent;
        const f32 p0[3] = {-H, -H, -H}, p1[3] = {+H, -H, -H}, p2[3] = {+H, -H, +H}, p3[3] = {-H, -H, +H};
        EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, -1.f, 0.f);
    }
    // Ceiling (+Y face).
    {
        const f32 H = kHalfExtent;
        const f32 p0[3] = {-H, +H, +H}, p1[3] = {+H, +H, +H}, p2[3] = {+H, +H, -H}, p3[3] = {-H, +H, -H};
        EmitQuad(out, vi, ii, p0, p1, p2, p3, 0.f, +1.f, 0.f);
    }

    // Walls: bit 0 = +X, bit 1 = -X, bit 2 = +Z, bit 3 = -Z.
    for (u32 wall = 0; wall < 4; ++wall) {
        if (door_mask & (1u << wall)) {
            EmitDoorWall(out, vi, ii, wall);
        } else {
            EmitSolidWall(out, vi, ii, wall);
        }
    }

    assert(vi == vert_count);
    assert(ii == idx_count);
}

} // namespace primal::graphics::wfc
