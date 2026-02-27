#pragma once

#include "CommonHeaders.h"
#include "../JobSystem/JobSystem.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "RenderProxy.h"
#include <vector>

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
        const std::vector<RenderProxy>& proxies,
        std::vector<bool>& visibility,
        const math::m4x4& view_matrix,
        const math::m4x4& projection_matrix);
    
    // Generate draw commands in parallel
    // proxies: input array of visible render proxies
    // camera_position: camera world position for sorting
    std::vector<DrawCall> GenerateDrawCallsParallel(
        const std::vector<RenderProxy>& proxies,
        const std::vector<bool>& visibility,
        const math::v3& camera_position);
    
    // Sort draw calls in parallel (for transparent objects)
    // back_to_front: true for back-to-front, false for front-to-back
    void SortDrawCallsParallel(
        std::vector<DrawCall>& draw_calls,
        bool back_to_front = true);
    
    // Combined pipeline: cull -> generate -> sort
    void GenerateCommands(
        const std::vector<RenderProxy>& proxies,
        std::vector<DrawCall>& out_draw_calls,
        const math::m4x4& view_matrix,
        const math::m4x4& projection_matrix,
        const math::v3& camera_position,
        bool sort_transparent = true);
    
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
