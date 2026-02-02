#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/PipelineComponent.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"

namespace primal::pipeline {

    struct info {
        graphics::rhi::PipelineHandle handle{ graphics::rhi::handles::INVALID_PIPELINE };
        graphics::rhi::PipelineBindPoint type{ graphics::rhi::PipelineBindPoint::Graphics };
    };

    component create(const info& initial_info, game_entity::entity entity);
    void remove(component c);
    bool is_valid(component c);
    
    graphics::rhi::PipelineHandle get_pipeline_handle(component c);
    void set_pipeline_handle(component c, graphics::rhi::PipelineHandle handle);
    
    graphics::rhi::PipelineBindPoint get_pipeline_type(component c);
}
