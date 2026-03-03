/**
 * @file LightProbeManager.cpp
 * @brief Light Probe 管理器实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-28
 * @version 0.1.0
 */

#include "LightProbeManager.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace primal::graphics::lighting
{
    static const size_t MAX_PROBES_PER_NODE = 8;
    static const int MAX_OCTREE_DEPTH = 6;

    // Helper: Check if point is in AABB
    static bool IsPointInAABB(const math::v3& point, const math::v3& center, f32 half_size)
    {
        return (point.x >= center.x - half_size && point.x <= center.x + half_size) &&
               (point.y >= center.y - half_size && point.y <= center.y + half_size) &&
               (point.z >= center.z - half_size && point.z <= center.z + half_size);
    }

    // Helper: Check sphere-AABB intersection
    static bool SphereIntersectsAABB(const math::v3& sphere_center, f32 sphere_radius, const math::v3& box_center, f32 box_half_size)
    {
        math::v3 closest_point;
        closest_point.x = std::max(box_center.x - box_half_size, std::min(sphere_center.x, box_center.x + box_half_size));
        closest_point.y = std::max(box_center.y - box_half_size, std::min(sphere_center.y, box_center.y + box_half_size));
        closest_point.z = std::max(box_center.z - box_half_size, std::min(sphere_center.z, box_center.z + box_half_size));

        math::v3 diff = closest_point - sphere_center;
        f32 dist_sq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
        return dist_sq <= (sphere_radius * sphere_radius);
    }

    void LightProbeManager::BuildOctree()
    {
        if (m_probes.empty()) return;

        // Calculate bounds
        math::v3 min_pt = { std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max() };
        math::v3 max_pt = { std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest() };

        for (const auto& probe : m_probes)
        {
            min_pt.x = std::min(min_pt.x, probe.position.x);
            min_pt.y = std::min(min_pt.y, probe.position.y);
            min_pt.z = std::min(min_pt.z, probe.position.z);
            max_pt.x = std::max(max_pt.x, probe.position.x);
            max_pt.y = std::max(max_pt.y, probe.position.y);
            max_pt.z = std::max(max_pt.z, probe.position.z);
        }

        // Make it a cube
        math::v3 center = (min_pt + max_pt) * 0.5f;
        math::v3 size = max_pt - min_pt;
        f32 max_dim = std::max({ size.x, size.y, size.z });
        f32 half_size = max_dim * 0.5f + 1.0f; // Add padding

        m_root = std::make_unique<OctreeNode>(center, half_size);

        for (size_t i = 0; i < m_probes.size(); ++i)
        {
            InsertProbe(m_root.get(), i);
        }

        m_dirty = false;
    }

    void LightProbeManager::InsertProbe(OctreeNode* node, size_t probe_index)
    {
        // Recursive helper
        struct RecursiveInserter {
            LightProbeManager& mgr;
            void operator()(OctreeNode* curr, size_t p_idx, int depth) {
                const auto& probe = mgr.m_probes[p_idx];
                
                if (curr->is_leaf)
                {
                    curr->probe_indices.push_back(p_idx);
                    if (curr->probe_indices.size() > MAX_PROBES_PER_NODE && depth < MAX_OCTREE_DEPTH)
                    {
                        // Split
                        curr->is_leaf = false;
                        f32 h = curr->half_size * 0.5f;
                        
                        for (int i = 0; i < 8; ++i)
                        {
                            math::v3 offset = {
                                (i & 1) ? h : -h,
                                (i & 2) ? h : -h,
                                (i & 4) ? h : -h
                            };
                            curr->children[i] = std::make_unique<OctreeNode>(curr->center + offset, h);
                        }
                        
                        // Redistribute existing
                        utl::vector<size_t> old_indices = std::move(curr->probe_indices);
                        curr->probe_indices.clear();
                        
                        for (size_t existing_idx : old_indices)
                        {
                            const auto& p = mgr.m_probes[existing_idx];
                            // Find child
                            for (int i = 0; i < 8; ++i)
                            {
                                if (IsPointInAABB(p.position, curr->children[i]->center, h))
                                {
                                    (*this)(curr->children[i].get(), existing_idx, depth + 1);
                                    break;
                                }
                            }
                        }
                    }
                }
                else
                {
                    // Push down
                    f32 h = curr->half_size * 0.5f;
                    for (int i = 0; i < 8; ++i)
                    {
                        if (IsPointInAABB(probe.position, curr->children[i]->center, h))
                        {
                            (*this)(curr->children[i].get(), p_idx, depth + 1);
                            return;
                        }
                    }
                }
            }
        };

        RecursiveInserter{*this}(node, probe_index, 0);
    }

    void LightProbeManager::QueryProbes(const OctreeNode* node, const math::v3& position, f32 radius, utl::vector<size_t>& out_indices) const
    {
        if (!node) return;

        if (!SphereIntersectsAABB(position, radius, node->center, node->half_size))
        {
            return;
        }

        if (node->is_leaf)
        {
            for (size_t idx : node->probe_indices)
            {
                out_indices.push_back(idx);
            }
        }
        else
        {
            for (int i = 0; i < 8; ++i)
            {
                QueryProbes(node->children[i].get(), position, radius, out_indices);
            }
        }
    }

    math::sh::SH9Color LightProbeManager::GetInterpolatedSH(const math::v3& position)
    {
        if (m_dirty || !m_root)
        {
            BuildOctree();
        }

        if (m_probes.empty()) return {};

        utl::vector<size_t> candidates;
        // Search radius strategy: Start with a reasonable radius.
        // If the scene is large, 20.0 might be too small or too big.
        // For now, hardcode 50.0f to be safe.
        f32 search_radius = 50.0f;
        
        if (m_root)
        {
            QueryProbes(m_root.get(), position, search_radius, candidates);
        }
        
        // Fallback if no candidates found
        if (candidates.empty())
        {
             // If few probes, just use all
             if (m_probes.size() < 100)
             {
                 for(size_t i=0; i<m_probes.size(); ++i) candidates.push_back(i);
             }
        }

        if (candidates.empty()) return {};

        math::sh::SH9Color result;
        f32 total_weight = 0.0f;
        const f32 k_epsilon = 1e-5f;

        for (size_t idx : candidates)
        {
            const auto& probe = m_probes[idx];
            math::v3 diff = position - probe.position;
            f32 dist_sq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
            f32 dist = std::sqrt(dist_sq);

            if (dist < k_epsilon)
            {
                return probe.sh_coeffs;
            }

            f32 weight = 1.0f / (dist * dist);
            
            math::sh::SH9Color weighted = probe.sh_coeffs;
            weighted *= weight;
            
            result += weighted;
            total_weight += weight;
        }

        if (total_weight > k_epsilon)
        {
            result *= (1.0f / total_weight);
        }

        return result;
    }
}
