#pragma once

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics::rhi {
    /**
     * @brief RHI 变换组件
     * @details 存储实体的世界变换矩阵，用于渲染时的位置计算
     */
    struct RHITransformComponent {
        math::m4x4 worldMatrix{math::MatrixIdentity()};

        RHITransformComponent() = default;
        explicit RHITransformComponent(const math::m4x4& matrix) : worldMatrix(matrix) {}
    };
}
