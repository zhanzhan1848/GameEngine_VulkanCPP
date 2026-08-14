#include "BVH.hpp"
#include <algorithm>
#include <cmath>

namespace primal::graphics::utl
{
    void BVH::Build(const primal::utl::vector<math::v3>& vertices, const primal::utl::vector<u32>& indices)
    {
        m_nodes.clear();
        m_triangles.clear();
        m_primitive_indices.clear();

        if (indices.empty()) return;

        u32 triangle_count = (u32)indices.size() / 3;
        m_triangles.reserve(triangle_count);
        m_primitive_indices.resize(triangle_count);

        for (u32 i = 0; i < triangle_count; ++i)
        {
            Triangle tri;
            tri.v0 = vertices[indices[i * 3 + 0]];
            tri.v1 = vertices[indices[i * 3 + 1]];
            tri.v2 = vertices[indices[i * 3 + 2]];
            
            // Calculate center
            tri.center.x = (tri.v0.x + tri.v1.x + tri.v2.x) * (1.0f / 3.0f);
            tri.center.y = (tri.v0.y + tri.v1.y + tri.v2.y) * (1.0f / 3.0f);
            tri.center.z = (tri.v0.z + tri.v1.z + tri.v2.z) * (1.0f / 3.0f);
            
            tri.original_index = i;
            m_triangles.push_back(tri);
            m_primitive_indices[i] = i;
        }

        BVHNode root;
        root.first_primitive = 0;
        root.primitive_count = triangle_count;
        m_nodes.push_back(root);

        UpdateNodeBounds(0);
        Subdivide(0);
    }

    void BVH::UpdateNodeBounds(u32 node_index)
    {
        BVHNode& node = m_nodes[node_index];
        // Reset to infinity inverse
        node.bbox.min = { INF_FLOAT, INF_FLOAT, INF_FLOAT };
        node.bbox.max = { -INF_FLOAT, -INF_FLOAT, -INF_FLOAT };

        for (u32 i = 0; i < node.primitive_count; ++i)
        {
            u32 tri_idx = m_primitive_indices[node.first_primitive + i];
            const Triangle& tri = m_triangles[tri_idx];
            
            math::v3 tri_min, tri_max;
            tri_min.x = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
            tri_min.y = std::min({tri.v0.y, tri.v1.y, tri.v2.y});
            tri_min.z = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
            
            tri_max.x = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
            tri_max.y = std::max({tri.v0.y, tri.v1.y, tri.v2.y});
            tri_max.z = std::max({tri.v0.z, tri.v1.z, tri.v2.z});

            // Add epsilon
            float eps = 1e-5f;
            tri_min.x -= eps; tri_min.y -= eps; tri_min.z -= eps;
            tri_max.x += eps; tri_max.y += eps; tri_max.z += eps;

            AABB tri_aabb(tri_min, tri_max);
            
            if (i == 0) node.bbox = tri_aabb;
            else node.bbox = node.bbox.Union(tri_aabb);
        }
    }

    void BVH::Subdivide(u32 node_index)
    {
        u32 first_prim = m_nodes[node_index].first_primitive;
        u32 count = m_nodes[node_index].primitive_count;
        const AABB& bbox = m_nodes[node_index].bbox;

        if (count <= 2) return;

        math::v3 extent;
        extent.x = bbox.max.x - bbox.min.x;
        extent.y = bbox.max.y - bbox.min.y;
        extent.z = bbox.max.z - bbox.min.z;

        // Try axes in decreasing-extent order. The previous code picked only
        // the single longest axis; for clustered geometry (e.g. Sponza's
        // statuary, where many triangle centers land on one side of the bbox
        // midpoint) the median partition can fail and the function would bail,
        // leaving one giant leaf. Observed on Sponza: max_leaf=129865 with
        // only 2085 nodes — every ray through that leaf did ~130k triangle
        // tests, which presented as a frozen application.
        int axes[3];
        if (extent.x >= extent.y && extent.x >= extent.z) {
            axes[0] = 0;
            axes[1] = (extent.y >= extent.z) ? 1 : 2;
            axes[2] = (extent.y >= extent.z) ? 2 : 1;
        } else if (extent.y >= extent.z) {
            axes[0] = 1;
            axes[1] = (extent.x >= extent.z) ? 0 : 2;
            axes[2] = (extent.x >= extent.z) ? 2 : 0;
        } else {
            axes[0] = 2;
            axes[1] = (extent.x >= extent.y) ? 0 : 1;
            axes[2] = (extent.x >= extent.y) ? 1 : 0;
        }

        int i = (int)first_prim;
        int j = (int)first_prim + (int)count - 1;
        int chosen_axis = axes[0];
        bool split_ok = false;

        for (int a = 0; a < 3; ++a)
        {
            int axis = axes[a];
            float split_pos = (axis == 0) ? (bbox.min.x + bbox.max.x) * 0.5f :
                              (axis == 1) ? (bbox.min.y + bbox.max.y) * 0.5f :
                                            (bbox.min.z + bbox.max.z) * 0.5f;

            // Reset cursors for each attempt.
            i = (int)first_prim;
            j = (int)first_prim + (int)count - 1;

            while (i <= j)
            {
                u32 tri_idx = m_primitive_indices[i];
                float pos = (axis == 0) ? m_triangles[tri_idx].center.x :
                            (axis == 1) ? m_triangles[tri_idx].center.y :
                                          m_triangles[tri_idx].center.z;

                if (pos < split_pos)
                {
                    i++;
                }
                else
                {
                    std::swap(m_primitive_indices[i], m_primitive_indices[j]);
                    j--;
                }
            }

            u32 left_count = (u32)(i - (int)first_prim);
            if (left_count != 0 && left_count != count)
            {
                chosen_axis = axis;
                split_ok = true;
                break;
            }
        }

        if (!split_ok)
        {
            // All 3 axes failed median partition (triangle centers cluster on
            // one side of every midpoint). Force a balanced split by sorting
            // along the longest axis and cutting at the middle index. This
            // guarantees progress: left_count is always in (0, count).
            std::sort(m_primitive_indices.begin() + first_prim,
                      m_primitive_indices.begin() + first_prim + count,
                      [this, axes](u32 a, u32 b) {
                          float pa = (axes[0] == 0) ? m_triangles[a].center.x :
                                     (axes[0] == 1) ? m_triangles[a].center.y :
                                                       m_triangles[a].center.z;
                          float pb = (axes[0] == 0) ? m_triangles[b].center.x :
                                     (axes[0] == 1) ? m_triangles[b].center.y :
                                                       m_triangles[b].center.z;
                          return pa < pb;
                      });
            i = (int)first_prim + (int)count / 2;
            chosen_axis = axes[0];
        }

        u32 left_count = (u32)(i - (int)first_prim);
        // Defensive: should never trigger after the fallback above, but keep
        // the guard to prevent infinite recursion if the sort ever regresses.
        if (left_count == 0 || left_count == count) return;

        u32 left_idx = (u32)m_nodes.size();
        m_nodes.emplace_back();
        u32 right_idx = (u32)m_nodes.size();
        m_nodes.emplace_back();

        m_nodes[left_idx].first_primitive = first_prim;
        m_nodes[left_idx].primitive_count = left_count;

        m_nodes[right_idx].first_primitive = (u32)i;
        m_nodes[right_idx].primitive_count = count - left_count;

        m_nodes[node_index].left = left_idx;
        m_nodes[node_index].right = right_idx;
        m_nodes[node_index].primitive_count = 0;

        (void)chosen_axis;

        UpdateNodeBounds(left_idx);
        Subdivide(left_idx);
        UpdateNodeBounds(right_idx);
        Subdivide(right_idx);
    }

    bool BVH::Intersect(const Ray& ray, HitInfo& out_hit) const
    {
        if (m_nodes.empty()) return false;

        float t_root = 0.0f;
        if (!IntersectAABB(ray, m_nodes[0].bbox, t_root) || t_root > out_hit.t)
            return false;

        // Stackless traversal or explicit stack
        // Using explicit stack for simplicity
        u32 stack[64];
        u32 stack_ptr = 0;
        stack[stack_ptr++] = 0;

        bool hit_any = false;

        while (stack_ptr > 0)
        {
            u32 node_idx = stack[--stack_ptr];
            const BVHNode& node = m_nodes[node_idx];

            if (node.IsLeaf())
            {
                for (u32 i = 0; i < node.primitive_count; ++i)
                {
                    u32 tri_idx = m_primitive_indices[node.first_primitive + i];
                    if (IntersectTriangle(ray, m_triangles[tri_idx], out_hit))
                    {
                        hit_any = true;
                    }
                }
            }
            else
            {
                u32 left = node.left;
                u32 right = node.right;
                float t_left, t_right;
                bool hit_left = IntersectAABB(ray, m_nodes[left].bbox, t_left);
                bool hit_right = IntersectAABB(ray, m_nodes[right].bbox, t_right);

                // Optimization: visit closer child first
                if (hit_left && hit_right)
                {
                    if (t_left < t_right)
                    {
                        if (t_right < out_hit.t) stack[stack_ptr++] = right;
                        if (t_left < out_hit.t) stack[stack_ptr++] = left;
                    }
                    else
                    {
                        if (t_left < out_hit.t) stack[stack_ptr++] = left;
                        if (t_right < out_hit.t) stack[stack_ptr++] = right;
                    }
                }
                else if (hit_left)
                {
                    if (t_left < out_hit.t) stack[stack_ptr++] = left;
                }
                else if (hit_right)
                {
                    if (t_right < out_hit.t) stack[stack_ptr++] = right;
                }
            }
        }

        return hit_any;
    }

    bool BVH::IntersectAny(const Ray& ray, float max_dist) const
    {
        if (m_nodes.empty()) return false;

        // Local hit info for this check
        HitInfo hit;
        hit.t = max_dist;

        float t_root = 0.0f;
        if (!IntersectAABB(ray, m_nodes[0].bbox, t_root) || t_root > max_dist)
            return false;

        u32 stack[64];
        u32 stack_ptr = 0;
        stack[stack_ptr++] = 0;

        while (stack_ptr > 0)
        {
            u32 node_idx = stack[--stack_ptr];
            const BVHNode& node = m_nodes[node_idx];

            if (node.IsLeaf())
            {
                for (u32 i = 0; i < node.primitive_count; ++i)
                {
                    u32 tri_idx = m_primitive_indices[node.first_primitive + i];
                    // If any intersection found within range, return true immediately
                    if (IntersectTriangle(ray, m_triangles[tri_idx], hit))
                    {
                        return true;
                    }
                }
            }
            else
            {
                u32 left = node.left;
                u32 right = node.right;
                float t_left, t_right;
                bool hit_left = IntersectAABB(ray, m_nodes[left].bbox, t_left);
                bool hit_right = IntersectAABB(ray, m_nodes[right].bbox, t_right);

                if (hit_left && t_left < hit.t) stack[stack_ptr++] = left;
                if (hit_right && t_right < hit.t) stack[stack_ptr++] = right;
            }
        }
        return false;
    }

    BVH::Stats BVH::GetStats() const
    {
        Stats s{0, 0, 0, 0, 0};
        if (m_nodes.empty()) return s;

        // Depth-first walk with an explicit stack of (node_idx, depth).
        struct StackEntry { u32 idx; u32 depth; };
        StackEntry stack[128];
        u32 stack_ptr = 0;
        stack[stack_ptr++] = {0, 1};

        while (stack_ptr > 0)
        {
            StackEntry e = stack[--stack_ptr];
            const BVHNode& n = m_nodes[e.idx];
            ++s.node_count;
            if (n.IsLeaf())
            {
                ++s.leaf_count;
                if (n.primitive_count > s.max_leaf_size) s.max_leaf_size = n.primitive_count;
                s.total_prims += n.primitive_count;
            }
            if (e.depth > s.max_depth) s.max_depth = e.depth;
            if (n.left != u32_invalid_id && n.primitive_count == 0) {
                stack[stack_ptr++] = {n.left, e.depth + 1};
            }
            if (n.right != u32_invalid_id && n.primitive_count == 0) {
                stack[stack_ptr++] = {n.right, e.depth + 1};
            }
        }
        return s;
    }

    bool BVH::IntersectAABB(const Ray& ray, const AABB& aabb, float& t_enter) const
    {
        float tx1 = (aabb.min.x - ray.origin.x) * ray.inv_direction.x;
        float tx2 = (aabb.max.x - ray.origin.x) * ray.inv_direction.x;

        float tmin = std::min(tx1, tx2);
        float tmax = std::max(tx1, tx2);

        float ty1 = (aabb.min.y - ray.origin.y) * ray.inv_direction.y;
        float ty2 = (aabb.max.y - ray.origin.y) * ray.inv_direction.y;

        tmin = std::max(tmin, std::min(ty1, ty2));
        tmax = std::min(tmax, std::max(ty1, ty2));

        float tz1 = (aabb.min.z - ray.origin.z) * ray.inv_direction.z;
        float tz2 = (aabb.max.z - ray.origin.z) * ray.inv_direction.z;

        tmin = std::max(tmin, std::min(tz1, tz2));
        tmax = std::min(tmax, std::max(tz1, tz2));

        t_enter = std::max(tmin, ray.t_min);

        return tmax >= tmin && tmax >= ray.t_min && tmin <= ray.t_max;
    }

    bool BVH::IntersectTriangle(const Ray& ray, const Triangle& tri, HitInfo& hit) const
    {
        // Möller–Trumbore intersection algorithm
        math::v3 edge1, edge2, h, s, q;
        float a, f, u, v;

        edge1.x = tri.v1.x - tri.v0.x;
        edge1.y = tri.v1.y - tri.v0.y;
        edge1.z = tri.v1.z - tri.v0.z;

        edge2.x = tri.v2.x - tri.v0.x;
        edge2.y = tri.v2.y - tri.v0.y;
        edge2.z = tri.v2.z - tri.v0.z;

        // h = ray.dir x edge2
        // Cross product logic
        h.x = ray.direction.y * edge2.z - ray.direction.z * edge2.y;
        h.y = ray.direction.z * edge2.x - ray.direction.x * edge2.z;
        h.z = ray.direction.x * edge2.y - ray.direction.y * edge2.x;

        // a = edge1 . h
        a = edge1.x * h.x + edge1.y * h.y + edge1.z * h.z;

        if (a > -1e-7f && a < 1e-7f) return false; // Parallel

        f = 1.0f / a;
        
        // s = ray.origin - v0
        s.x = ray.origin.x - tri.v0.x;
        s.y = ray.origin.y - tri.v0.y;
        s.z = ray.origin.z - tri.v0.z;

        u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
        if (u < 0.0f || u > 1.0f) return false;

        // q = s x edge1
        q.x = s.y * edge1.z - s.z * edge1.y;
        q.y = s.z * edge1.x - s.x * edge1.z;
        q.z = s.x * edge1.y - s.y * edge1.x;

        v = f * (ray.direction.x * q.x + ray.direction.y * q.y + ray.direction.z * q.z);
        if (v < 0.0f || u + v > 1.0f) return false;

        float t = f * (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z);

        if (t > ray.t_min && t < hit.t)
        {
            hit.t = t;
            hit.u = u;
            hit.v = v;
            hit.triangle_index = tri.original_index;
            hit.hit = true;
            return true;
        }
        return false;
    }
}
