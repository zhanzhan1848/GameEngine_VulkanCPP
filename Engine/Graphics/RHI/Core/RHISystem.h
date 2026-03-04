#pragma once
#include "RHITypes.h"

namespace primal::graphics::rhi {
    class RHIEntityManager;
    class RHIDeviceBase;
    class RHICommandBuffer;

    /**
     * @brief RHI ECS 系统基类
     * @details 所有 RHI 层的 ECS 系统都应继承此类
     */
    class RHISystem {
    public:
        RHISystem() = default;
        virtual ~RHISystem() = default;

        /**
         * @brief 初始化系统
         * @param manager 实体管理器
         * @param device RHI设备
         */
        virtual void Initialize(RHIEntityManager* manager, RHIDeviceBase* device) {
            entityManager = manager;
            rhiDevice = device;
        }

        /**
         * @brief 系统更新
         * @param deltaTime 帧间隔时间
         */
        virtual void Update(float deltaTime) = 0;

        /**
         * @brief 渲染调用
         * @details 系统在渲染阶段的入口，用于提交渲染命令
         * @param cmdBuffer 渲染命令缓冲区
         */
        virtual void Render(RHICommandBuffer* cmdBuffer) {}

    protected:
        RHIEntityManager* entityManager{nullptr};
        RHIDeviceBase* rhiDevice{nullptr};
    };
}
