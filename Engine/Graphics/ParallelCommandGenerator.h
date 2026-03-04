#pragma once

#include "CommonHeaders.h"
#include "../JobSystem/JobSystem.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "RenderProxy.h"
#include "Utilities/Vector.h"

namespace primal::graphics {

struct DrawCall
{
    id::id_type proxy_id;
    id::id_type mesh_id;
    id::id_type material_id;
    math::v3 position;
    f32 distance_to_camera;
};

class ParallelCommandGenerator
{
public:
    static ParallelCommandGenerator* Get();
    
    static bool Initialize();
    static void Shutdown();
    
    // Perform frustum culling in parallel
    // proxies: input array of render proxies
    // visibility: output array of boolean visibility flags
    // view_matrix: camera view matrix
    // projection_matrix: camera projection matrix
    jobsystem::JobHandle FrustumCullParallel(
    jobsystem::JobHandle FrustumCullParallel(
        const utl::vector<RenderProxy>& proxies,
        utl::vector<bool>& visibility,
    
    // Generate draw commands in parallel
    // proxies: input array of visible render proxies
    // camera_position: camera world position for sorting
    utl::vector<DrawCall> GenerateDrawCallsParallel(
        const utl::vector<RenderProxy>& proxies,
        const utl::vector<bool>& visibility,
    
    // Sort draw calls in parallel (for transparent objects)
    // back_to_front: true for back-to-front, false for front-to-back
    void SortDrawCallsParallel(
        utl::vector<DrawCall>& draw_calls,
        bool back_to_front = true);
    
    // Combined pipeline: cull -> generate -> sort
    void GenerateCommands(
    void GenerateCommands(
        const utl::vector<RenderProxy>& proxies,
        utl::vector<DrawCall>& out_draw_calls,
    
private:
    ParallelCommandGenerator() = default;
    ~ParallelCommandGenerator() = default;
    
    DISABLE_COPY(ParallelCommandGenerator);
    DISABLE_MOVE(ParallelCommandGenerator);
    
    static ParallelCommandGenerator* s_instance;
    
    bool FrustumCullSingle(
        const RenderProxy& proxy,
        const math::m4x4& view_projection) const;
};

#define g_ParallelCommandGenerator primal::graphics::ParallelCommandGenerator::Get()

} // namespace primal::graphics
