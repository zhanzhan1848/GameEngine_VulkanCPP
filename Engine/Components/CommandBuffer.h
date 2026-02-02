#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/CommandBufferComponent.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::command_buffer {

    struct info {
        graphics::rhi::CommandBufferHandle handle{ graphics::rhi::handles::INVALID_COMMAND_BUFFER };
        graphics::rhi::SyncHandle sync_handle{ graphics::rhi::handles::INVALID_SYNC };
        graphics::rhi::CommandQueueType queue_type{ graphics::rhi::CommandQueueType::Graphics };
    };

    component create(const info& initial_info, game_entity::entity entity);
    void remove(component c);
    bool is_valid(component c);
    
    graphics::rhi::CommandBufferHandle get_handle(component c);
    void set_handle(component c, graphics::rhi::CommandBufferHandle handle);
    
    graphics::rhi::SyncHandle get_sync_handle(component c);
    void set_sync_handle(component c, graphics::rhi::SyncHandle handle);
    
    graphics::rhi::CommandQueueType get_queue_type(component c);
}
