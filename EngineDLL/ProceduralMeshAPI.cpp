// ProceduralMeshAPI.cpp - procedural mesh creation C ABI for UI layer.
//
// Phase 5.1: Bridges primal::content::create_*_mesh inline generators to C ABI,
// letting the UI create procedural geometry (sphere/cylinder/cone/box/torus/etc.)
// and register them with the content system to obtain geometry_content_id.
//
// UI-mediated workflow:
//   1. ProceduralMeshCreate(kSphere, [1.0, 16, 12]) → geometry_content_id
//   2. PipelineRegisterMeshEntity(content_id, tex_ids, 3) → entity_id
//   3. Use entity_id in AddRenderItem / SetPCGEntities / etc.
//
// Mesh type enum:
//   0=Sphere(r,segs,rings) 1=Cylinder(r,h,segs) 2=Cone(r,h,segs)
//   3=Box(sx,sy,sz)        4=Torus(oR,iR,segs,sides) 5=Capsule(r,h,segs,hemiRings)
//   6=Disc(r,segs)         7=Hemisphere(r,segs,rings) 8=Pyramid(base,h)
//   9=Plane(w,d,wSegs,dSegs) 10=QuadXY(w,h) 11=Teapot(size,tess)
//
// Lifetime: ProceduralMeshDestroy returns the asset to the content system.
// Caller must PipelineUnregisterMeshEntity BEFORE destroy to avoid dangling refs.

#include "Common.h"
#include "CommonHeaders.h"
#include "Content/ProceduralMesh.h"
#include "Content/ContentToEngine.h"

using namespace primal;

namespace {

// Mesh type enum (mirrors ProceduralMesh.h generator list)
constexpr u32 kMeshTypeCount = 12;

// Minimum param count per mesh type
constexpr u32 kMinParams[kMeshTypeCount] = {
    3,  // Sphere
    3,  // Cylinder
    3,  // Cone
    3,  // Box
    4,  // Torus
    4,  // Capsule
    2,  // Disc
    3,  // Hemisphere
    2,  // Pyramid
    4,  // Plane
    2,  // QuadXY
    2,  // Teapot
};

} // anonymous namespace

extern "C" {

EDITOR_INTERFACE u64 ProceduralMeshCreate(u32 mesh_type, const f32* params, u32 param_count) {
    if (mesh_type >= kMeshTypeCount) return 0;
    if (!params && param_count > 0) return 0;
    if (param_count < kMinParams[mesh_type]) return 0;

    id::id_type content_id = id::invalid_id;
    switch (mesh_type) {
        case 0:  // Sphere
            content_id = content::create_sphere_mesh(params[0], (u32)params[1], (u32)params[2]);
            break;
        case 1:  // Cylinder
            content_id = content::create_cylinder_mesh(params[0], params[1], (u32)params[2]);
            break;
        case 2:  // Cone
            content_id = content::create_cone_mesh(params[0], params[1], (u32)params[2]);
            break;
        case 3:  // Box
            content_id = content::create_box_mesh(params[0], params[1], params[2]);
            break;
        case 4:  // Torus
            content_id = content::create_torus_mesh(params[0], params[1], (u32)params[2], (u32)params[3]);
            break;
        case 5:  // Capsule
            content_id = content::create_capsule_mesh(params[0], params[1], (u32)params[2], (u32)params[3]);
            break;
        case 6:  // Disc
            content_id = content::create_disc_mesh(params[0], (u32)params[1]);
            break;
        case 7:  // Hemisphere
            content_id = content::create_hemisphere_mesh(params[0], (u32)params[1], (u32)params[2]);
            break;
        case 8:  // Pyramid
            content_id = content::create_pyramid_mesh(params[0], params[1]);
            break;
        case 9:  // Plane
            content_id = content::create_plane_mesh(params[0], params[1], (u32)params[2], (u32)params[3]);
            break;
        case 10: // QuadXY
            content_id = content::create_quad_xy_mesh(params[0], params[1]);
            break;
        case 11: // Teapot
            content_id = content::create_teapot_mesh(params[0], (u32)params[1]);
            break;
        default:
            return 0;
    }
    return static_cast<u64>(content_id);
}

EDITOR_INTERFACE void ProceduralMeshDestroy(u64 content_id) {
    if (content_id == 0) return;
    primal::content::destroy_resource(static_cast<id::id_type>(content_id),
                                       primal::content::asset_type::mesh);
}

// Release all engine-side content-system resources (meshes, shaders, textures).
// Call BEFORE ShutdownEngine in test programs that registered procedural meshes
// or pipeline mesh entities. Without this, the engine's static destructors trip
// a !_size assertion in ~free_list because resources created via the dylib's
// PipelineRegisterMeshEntity path are not released by ShutdownEngine alone.
// Idempotent.
EDITOR_INTERFACE void ShutdownContentSystem() {
    primal::content::shutdown();
}

} // extern "C"
