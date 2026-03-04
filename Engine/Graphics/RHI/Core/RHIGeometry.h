#pragma once
#include "RHIMath.h"
#include <cmath>

#include <limits>
#include <algorithm>

namespace primal::graphics::rhi {

/**
 * @brief 轴对齐包围盒 (Axis-Aligned Bounding Box)
 */
struct AABB {
    math::v3 min; ///< 最小点
    math::v3 max; ///< 最大点

    AABB()
        : min{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()},
          max{std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest()} {}

    AABB(const math::v3& minPoint, const math::v3& maxPoint)
        : min(minPoint), max(maxPoint) {}

    /**
     * @brief 扩展包围盒以包含一个点
     * @param point 要包含的点
     */
    void Expand(const math::v3& point) {
        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    }

    /**
     * @brief 扩展包围盒以包含另一个包围盒
     * @param other 要包含的包围盒
     */
    void Expand(const AABB& other) {
        if (!other.IsValid()) return;
        Expand(other.min);
        Expand(other.max);
    }

    /**
     * @brief 检查包围盒是否有效
     * @return 如果 min <= max 则返回 true
     */
    bool IsValid() const {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    /**
     * @brief 获取中心点
     * @return 中心点坐标
     */
    math::v3 Center() const {
        return math::v3{
            (min.x + max.x) * 0.5f,
            (min.y + max.y) * 0.5f,
            (min.z + max.z) * 0.5f
        };
    }

    /**
     * @brief 获取尺寸
     * @return 尺寸向量 (width, height, depth)
     */
    math::v3 Size() const {
        return math::v3{
            max.x - min.x,
            max.y - min.y,
            max.z - min.z
        };
    }

    /**
     * @brief 变换包围盒
     * @param transform 变换矩阵
     * @return 变换后的新包围盒
     */
    AABB Transform(const math::m4x4& transform) const {
        if (!IsValid()) return *this;

        // 获取8个角点
        math::v3 corners[8] = {
            {min.x, min.y, min.z},
            {max.x, min.y, min.z},
            {min.x, max.y, min.z},
            {max.x, max.y, min.z},
            {min.x, min.y, max.z},
            {max.x, min.y, max.z},
            {min.x, max.y, max.z},
            {max.x, max.y, max.z}
        };

        AABB newAABB;
        for (int i = 0; i < 8; ++i) {
            newAABB.Expand(math::TransformPoint(transform, corners[i]));
        }
        return newAABB;
    }
};

/**
 * @brief 视锥体
 * @details 用于视锥剔除
 */
struct Frustum {
    math::v4 planes[6];                 ///< 6个裁剪平面
    
    /**
     * @brief 从视图投影矩阵构造视锥
     * @param viewProjection 视图投影矩阵
     */
    inline void FromMatrix(const math::m4x4& viewProjection) {
        // 从视图投影矩阵提取6个裁剪平面
        // 左平面: row4 + row1
        planes[0] = math::v4{
            viewProjection.columns[3][0] + viewProjection.columns[0][0],
            viewProjection.columns[3][1] + viewProjection.columns[0][1],
            viewProjection.columns[3][2] + viewProjection.columns[0][2],
            viewProjection.columns[3][3] + viewProjection.columns[0][3]
        };
        // 右平面: row4 - row1
        planes[1] = math::v4{
            viewProjection.columns[3][0] - viewProjection.columns[0][0],
            viewProjection.columns[3][1] - viewProjection.columns[0][1],
            viewProjection.columns[3][2] - viewProjection.columns[0][2],
            viewProjection.columns[3][3] - viewProjection.columns[0][3]
        };
        // 上平面: row4 - row2
        planes[2] = math::v4{
            viewProjection.columns[3][0] - viewProjection.columns[1][0],
            viewProjection.columns[3][1] - viewProjection.columns[1][1],
            viewProjection.columns[3][2] - viewProjection.columns[1][2],
            viewProjection.columns[3][3] - viewProjection.columns[1][3]
        };
        // 下平面: row4 + row2
        planes[3] = math::v4{
            viewProjection.columns[3][0] + viewProjection.columns[1][0],
            viewProjection.columns[3][1] + viewProjection.columns[1][1],
            viewProjection.columns[3][2] + viewProjection.columns[1][2],
            viewProjection.columns[3][3] + viewProjection.columns[1][3]
        };
        // 近平面: row4 + row3
        planes[4] = math::v4{
            viewProjection.columns[3][0] + viewProjection.columns[2][0],
            viewProjection.columns[3][1] + viewProjection.columns[2][1],
            viewProjection.columns[3][2] + viewProjection.columns[2][2],
            viewProjection.columns[3][3] + viewProjection.columns[2][3]
        };
        // 远平面: row4 - row3
        planes[5] = math::v4{
            viewProjection.columns[3][0] - viewProjection.columns[2][0],
            viewProjection.columns[3][1] - viewProjection.columns[2][1],
            viewProjection.columns[3][2] - viewProjection.columns[2][2],
            viewProjection.columns[3][3] - viewProjection.columns[2][3]
        };
        
        // 归一化平面方程
        for (int i = 0; i < 6; ++i) {
            f32 length = std::sqrt(planes[i].x * planes[i].x + 
                                   planes[i].y * planes[i].y + 
                                   planes[i].z * planes[i].z);
            if (length > 0.0f) {
                planes[i] /= length;
            }
        }
    }
    
    /**
     * @brief 检查球体是否在视锥内
     * @param center 球心
     * @param radius 球半径
     * @return 是否在视锥内
     */
    inline bool IsSphereVisible(const math::v3& center, f32 radius) const {
        for (int i = 0; i < 6; ++i) {
            const math::v4& plane = planes[i];
            f32 distance = plane.x * center.x + plane.y * center.y + 
                           plane.z * center.z + plane.w;
            if (distance < -radius) {
                return false;
            }
        }
        return true;
    }
    
    /**
     * @brief 检查包围盒是否在视锥内
     * @param box 包围盒
     * @return 是否在视锥内
     */
    inline bool IsBoxVisible(const AABB& box) const {
        return IsBoxVisible(box.min, box.max);
    }

    /**
     * @brief 检查包围盒是否在视锥内
     * @param min 最小点
     * @param max 最大点
     * @return 是否在视锥内
     */
    inline bool IsBoxVisible(const math::v3& min, const math::v3& max) const {
        // 检查包围盒的8个顶点
        math::v3 corners[8] = {
            math::v3{min.x, min.y, min.z},
            math::v3{max.x, min.y, min.z},
            math::v3{min.x, max.y, min.z},
            math::v3{max.x, max.y, min.z},
            math::v3{min.x, min.y, max.z},
            math::v3{max.x, min.y, max.z},
            math::v3{min.x, max.y, max.z},
            math::v3{max.x, max.y, max.z}
        };
        
        for (int i = 0; i < 6; ++i) {
            const math::v4& plane = planes[i];
            bool allOutside = true;
            
            for (int j = 0; j < 8; ++j) {
                f32 distance = plane.x * corners[j].x + plane.y * corners[j].y + 
                               plane.z * corners[j].z + plane.w;
                if (distance >= 0.0f) {
                    allOutside = false;
                    break;
                }
            }
            
            if (allOutside) {
                return false;
            }
        }
        return true;
    }
};

} // namespace primal::graphics::rhi
