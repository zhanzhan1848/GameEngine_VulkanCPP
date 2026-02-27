#include "ParallelCommandGenerator.h"
#include <algorithm>

namespace primal::graphics {

ParallelCommandGenerator* ParallelCommandGenerator::s_instance = nullptr;

ParallelCommandGenerator* ParallelCommandGenerator::Get()
{
    return s_instance;
}

bool ParallelCommandGenerator::Initialize()
{
    if (s_instance)
    {
        return true;
    }
    
    s_instance = new ParallelCommandGenerator();
    return true;
}

void ParallelCommandGenerator::Shutdown()
{
    if (s_instance)
    {
        delete s_instance;
        s_instance = nullptr;
    }
}

bool ParallelCommandGenerator::FrustumCullSingle(
    const RenderProxy& proxy,
    const math::m4x4& view_projection) const
{
    const rhi::AABB& aabb = proxy.worldAABB;
    
    math::v3 corners[8] = {
        math::v3{ aabb.min.x, aabb.min.y, aabb.min.z },
        math::v3{ aabb.max.x, aabb.min.y, aabb.min.z },
        math::v3{ aabb.min.x, aabb.max.y, aabb.min.z },
        math::v3{ aabb.max.x, aabb.max.y, aabb.min.z },
        math::v3{ aabb.min.x, aabb.min.y, aabb.max.z },
        math::v3{ aabb.max.x, aabb.min.y, aabb.max.z },
        math::v3{ aabb.min.x, aabb.max.y, aabb.max.z },
        math::v3{ aabb.max.x, aabb.max.y, aabb.max.z }
    };
    
    bool all_outside_left = true;
    bool all_outside_right = true;
    bool all_outside_top = true;
    bool all_outside_bottom = true;
    bool all_outside_near = true;
    bool all_outside_far = true;
    
    for (int i = 0; i < 8; ++i)
    {
        math::v4 clip_pos = view_projection * math::v4{ corners[i].x, corners[i].y, corners[i].z, 1.0f };
        
        if (clip_pos.w > 0.001f)
        {
            f32 x = clip_pos.x / clip_pos.w;
            f32 y = clip_pos.y / clip_pos.w;
            f32 z = clip_pos.z / clip_pos.w;
            
            if (x > -1.0f) all_outside_left = false;
            if (x < 1.0f) all_outside_right = false;
            if (y > -1.0f) all_outside_bottom = false;
            if (y < 1.0f) all_outside_top = false;
            if (z > 0.0f) all_outside_near = false;
            if (z < 1.0f) all_outside_far = false;
        }
    }
    
    return !(all_outside_left || all_outside_right || all_outside_top || 
             all_outside_bottom || all_outside_near || all_outside_far);
}

jobsystem::JobHandle ParallelCommandGenerator::FrustumCullParallel(
    const std::vector<RenderProxy>& proxies,
    std::vector<bool>& visibility,
    const math::m4x4& view_matrix,
    const math::m4x4& projection_matrix)
{
    visibility.resize(proxies.size());
    
    if (proxies.empty())
    {
        auto tracker = std::make_shared<jobsystem::JobStateTracker>(0);
        return jobsystem::JobHandle(tracker);
    }
    
    math::m4x4 view_projection = projection_matrix * view_matrix;
    
    u32 count = static_cast<u32>(proxies.size());
    
    return jobsystem::JobSystem::ParallelFor(count,
        [this, &proxies, &visibility, view_projection](u32 index, u32 thread)
        {
            visibility[index] = FrustumCullSingle(proxies[index], view_projection);
        });
}

std::vector<DrawCall> ParallelCommandGenerator::GenerateDrawCallsParallel(
    const std::vector<RenderProxy>& proxies,
    const std::vector<bool>& visibility,
    const math::v3& camera_position)
{
    std::vector<DrawCall> draw_calls;
    
    if (proxies.size() != visibility.size())
    {
        return draw_calls;
    }
    
    u32 visible_count = 0;
    for (bool v : visibility)
    {
        if (v) ++visible_count;
    }
    
    draw_calls.reserve(visible_count);
    
    for (size_t i = 0; i < proxies.size(); ++i)
    {
        if (visibility[i])
        {
            const RenderProxy& proxy = proxies[i];
            DrawCall dc;
            dc.proxy_id = static_cast<id::id_type>(i);
            dc.mesh_id = proxy.meshId;
            dc.material_id = proxy.materialId;
            dc.position = math::v3{
                proxy.transform.r[3].x,
                proxy.transform.r[3].y,
                proxy.transform.r[3].z
            };
            math::v3 diff = dc.position - camera_position;
            dc.distance_to_camera = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
            draw_calls.push_back(dc);
        }
    }
    
    return draw_calls;
}

void ParallelCommandGenerator::SortDrawCallsParallel(
    std::vector<DrawCall>& draw_calls,
    bool back_to_front)
{
    if (draw_calls.size() < 1000)
    {
        if (back_to_front)
        {
            std::sort(draw_calls.begin(), draw_calls.end(),
                [](const DrawCall& a, const DrawCall& b)
                {
                    return a.distance_to_camera > b.distance_to_camera;
                });
        }
        else
        {
            std::sort(draw_calls.begin(), draw_calls.end(),
                [](const DrawCall& a, const DrawCall& b)
                {
                    return a.distance_to_camera < b.distance_to_camera;
                });
        }
        return;
    }
    
    size_t size = draw_calls.size();
    size_t mid = size / 2;
    
    auto left_begin = draw_calls.begin();
    auto left_end = draw_calls.begin() + mid;
    auto right_begin = draw_calls.begin() + mid;
    auto right_end = draw_calls.end();
    
    auto handle = jobsystem::JobSystem::ParallelFor(2,
        [&draw_calls, left_begin, left_end, right_begin, right_end, back_to_front, mid](u32 index, u32)
        {
            if (index == 0)
            {
                if (back_to_front)
                {
                    std::sort(left_begin, left_end,
                        [](const DrawCall& a, const DrawCall& b)
                        {
                            return a.distance_to_camera > b.distance_to_camera;
                        });
                }
                else
                {
                    std::sort(left_begin, left_end,
                        [](const DrawCall& a, const DrawCall& b)
                        {
                            return a.distance_to_camera < b.distance_to_camera;
                        });
                }
            }
            else
            {
                if (back_to_front)
                {
                    std::sort(right_begin, right_end,
                        [](const DrawCall& a, const DrawCall& b)
                        {
                            return a.distance_to_camera > b.distance_to_camera;
                        });
                }
                else
                {
                    std::sort(right_begin, right_end,
                        [](const DrawCall& a, const DrawCall& b)
                        {
                            return a.distance_to_camera < b.distance_to_camera;
                        });
                }
            }
        });
    
    handle.Wait();
    
    if (back_to_front)
    {
        std::inplace_merge(draw_calls.begin(), draw_calls.begin() + mid, draw_calls.end(),
            [](const DrawCall& a, const DrawCall& b)
            {
                return a.distance_to_camera > b.distance_to_camera;
            });
    }
    else
    {
        std::inplace_merge(draw_calls.begin(), draw_calls.begin() + mid, draw_calls.end(),
            [](const DrawCall& a, const DrawCall& b)
            {
                return a.distance_to_camera < b.distance_to_camera;
            });
    }
}

void ParallelCommandGenerator::GenerateCommands(
    const std::vector<RenderProxy>& proxies,
    std::vector<DrawCall>& out_draw_calls,
    const math::m4x4& view_matrix,
    const math::m4x4& projection_matrix,
    const math::v3& camera_position,
    bool sort_transparent)
{
    std::vector<bool> visibility;
    
    auto cull_handle = FrustumCullParallel(proxies, visibility, view_matrix, projection_matrix);
    cull_handle.Wait();
    
    out_draw_calls = GenerateDrawCallsParallel(proxies, visibility, camera_position);
    
    if (sort_transparent && !out_draw_calls.empty())
    {
        SortDrawCallsParallel(out_draw_calls, true);
    }
}

} // namespace primal::graphics
