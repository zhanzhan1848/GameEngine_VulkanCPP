#pragma once

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Common/Id.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include <unordered_map>
#include "Engine/Utilities/Vector.h"

namespace primal::graphics {
    class RenderScene;
    class RenderSystem;

    namespace rhi {
        class RHIEntityManager;
        class RHIDeviceBase;

        /**
         * @brief RHI 场景同步器
         * @details 负责将 GamePlay 层的 RenderScene 数据同步到 RHI 层的 ECS 系统
         */
        class RHISceneSynchronizer {
        public:
            RHISceneSynchronizer();
            ~RHISceneSynchronizer();

            /**
             * @brief 初始化同步器
             * @param entityManager RHI实体管理器
             * @param device RHI设备
             * @param renderSystem 渲染系统（用于查找资源）
             */
            void Initialize(RHIEntityManager* entityManager, RHIDeviceBase* device, RenderSystem* renderSystem);

            /**
             * @brief 执行同步
             * @details 遍历 RenderScene 中的代理，更新 RHI 实体
             * @param renderScene 渲染场景
             */
            void Synchronize(const RenderScene& renderScene);

            /**
             * @brief 清理所有同步的实体
             */
            void Clear();

        private:
            RHIEntityManager* entityManager_{nullptr};
            RHIDeviceBase* device_{nullptr};
            RenderSystem* renderSystem_{nullptr};

            // GamePlay Entity ID -> RHI Entity ID 映射
            std::unordered_map<id::id_type, RHIEntityID> entityMap_;
        };
    }
}
