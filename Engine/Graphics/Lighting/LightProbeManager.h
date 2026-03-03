/**
 * @file LightProbeManager.h
 * @brief Light Probe 管理器
 * @details 负责管理场景中的 Light Probes，并为动态物体提供 SH 插值
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-28
 * @version 0.1.0
 */

#pragma once

#include "Utilities/SphericalHarmonics.h"
#include "Utilities/MathTypes.h"
#include <memory>

namespace primal::graphics::lighting
{
    /**
     * @brief 单个 Light Probe 数据结构
     */
    struct LightProbe
    {
        math::v3 position;
        math::sh::SH9Color sh_coeffs;
        f32 radius; // 影响半径，用于加权或剔除

        LightProbe() : position{0.f, 0.f, 0.f}, radius(1.0f) {}
    };

    /**
     * @brief Octree 节点
     */
    struct OctreeNode
    {
        math::v3 center;
        f32 half_size; // 半边长
        utl::vector<size_t> probe_indices;
        std::unique_ptr<OctreeNode> children[8];
        bool is_leaf{ true };

        OctreeNode(const math::v3& c, f32 hs) : center(c), half_size(hs) {}
    };

    /**
     * @brief Light Probe 管理器
     */
    class LightProbeManager
    {
    public:
        LightProbeManager() = default;
        ~LightProbeManager() = default;

        /**
         * @brief 添加一个 Light Probe
         */
        void AddProbe(const LightProbe& probe)
        {
            m_probes.push_back(probe);
            m_dirty = true;
        }

        /**
         * @brief 清除所有 Probes
         */
        void ClearProbes()
        {
            m_probes.clear();
            m_root.reset();
            m_dirty = false;
        }

        /**
         * @brief 获取所有 Probes (只读)
         */
        const utl::vector<LightProbe>& GetProbes() const { return m_probes; }

        /**
         * @brief 构建 Octree 加速结构
         */
        void BuildOctree();

        /**
         * @brief 获取指定位置的插值 SH 系数
         * @param position 物体世界坐标
         * @return 插值后的 SH9Color
         * @note 使用 Octree 加速查找最近的 Probe
         */
        math::sh::SH9Color GetInterpolatedSH(const math::v3& position);

    private:
        utl::vector<LightProbe> m_probes;
        std::unique_ptr<OctreeNode> m_root;
        bool m_dirty{ false };

        void InsertProbe(OctreeNode* node, size_t probe_index);
        void QueryProbes(const OctreeNode* node, const math::v3& position, f32 radius, utl::vector<size_t>& out_indices) const;
    };
}
