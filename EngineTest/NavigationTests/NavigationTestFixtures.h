#pragma once

// NavigationTestFixtures.h — ai-navigation 单测共用的程序化几何夹具。

#include "Navigation/NavGrid.h"

#include <vector>

namespace primal::navigation::test {

struct mesh_builder {
    std::vector<f32> positions;
    std::vector<u32> indices;

    u32 vert(f32 x, f32 y, f32 z)
    {
        positions.push_back(x);
        positions.push_back(y);
        positions.push_back(z);
        return (u32)(positions.size() / 3 - 1);
    }
    void tri(u32 a, u32 b, u32 c)
    {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }
    // 水平四边形（+Y 朝向：烘焙的绕向实体化要求地板/顶面朝上）
    void quad_y(f32 y, f32 x0, f32 z0, f32 x1, f32 z1)
    {
        const u32 a = vert(x0, y, z0), b = vert(x0, y, z1), c = vert(x1, y, z1), d = vert(x1, y, z0);
        tri(a, b, c);
        tri(a, c, d);
    }
    // 斜坡：x0 边高 y0 → x1 边高 y1（沿 X 爬升，法线朝上）
    void quad_slope(f32 x0, f32 z0, f32 x1, f32 z1, f32 y0, f32 y1)
    {
        const u32 a = vert(x0, y0, z0), b = vert(x0, y0, z1), c = vert(x1, y1, z1), d = vert(x1, y1, z0);
        tri(a, b, c);
        tri(a, c, d);
    }
    // 轴对齐闭合盒（顶面 +Y、底面 -Y；侧面竖直不产生列中心穿越，绕序任意）
    void box(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1)
    {
        quad_y(y1, x0, z0, x1, z1);                       // 顶面（上朝向）
        {
            // 底面（下朝向，反向绕序）
            const u32 a = vert(x0, y0, z0), b = vert(x1, y0, z0), c = vert(x1, y0, z1), d = vert(x0, y0, z1);
            tri(a, b, c);
            tri(a, c, d);
        }
        u32 a = vert(x0, y0, z0), b = vert(x1, y0, z0), c = vert(x1, y1, z0), d = vert(x0, y1, z0);
        tri(a, b, c); tri(a, c, d);
        a = vert(x1, y0, z1); b = vert(x0, y0, z1); c = vert(x0, y1, z1); d = vert(x1, y1, z1);
        tri(a, b, c); tri(a, c, d);
        a = vert(x0, y0, z1); b = vert(x0, y0, z0); c = vert(x0, y1, z0); d = vert(x0, y1, z1);
        tri(a, b, c); tri(a, c, d);
        a = vert(x1, y0, z0); b = vert(x1, y0, z1); c = vert(x1, y1, z1); d = vert(x1, y1, z0);
        tri(a, b, c); tri(a, c, d);
    }
    bake_input input(f32 cell = 0.25f) const
    {
        return bake_input{ positions.data(), (u32)(positions.size() / 3),
                           indices.data(), (u32)indices.size(), cell, 0.05f, 0.5f };
    }
};

inline bool near3(const math::v3& a, const math::v3& b, f32 tol)
{
    return fabsf(a.x - b.x) <= tol && fabsf(a.y - b.y) <= tol && fabsf(a.z - b.z) <= tol;
}

} // namespace primal::navigation::test
