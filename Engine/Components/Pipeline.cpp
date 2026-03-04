// 文件说明: Pipeline 组件实现。管理实体到 RHI Pipeline 的绑定。
// 性能策略: 索引数组按实体索引存储。

#include "Pipeline.h"
#include "EngineAPI/GameEntity.h"
#include <assert.h>

namespace primal::pipeline {
    
    namespace {
        utl::vector<graphics::rhi::PipelineHandle>    pipeline_handles;
        utl::vector<graphics::rhi::PipelineBindPoint> pipeline_types;
        utl::vector<u8>                               exists_flags;
    }

    component create(const info& initial_info, game_entity::entity entity) {
        assert(entity.is_valid());
        const id::id_type eindex{ id::index(entity.get_id()) };

        if (pipeline_handles.size() <= eindex) {
            pipeline_handles.resize(eindex + 1, graphics::rhi::handles::INVALID_PIPELINE);
            pipeline_types.resize(eindex + 1, graphics::rhi::PipelineBindPoint::Graphics);
            exists_flags.resize(eindex + 1, 0);
        }
        
        pipeline_handles[eindex] = initial_info.handle;
        pipeline_types[eindex] = initial_info.type;
        exists_flags[eindex] = 1;
        
        return component{ pipeline_id{ entity.get_id() } };
    }

    void remove(component c) {
        assert(c.is_valid());
        const id::id_type eindex{ id::index(c.get_id()) };
        if (eindex < exists_flags.size()) {
            exists_flags[eindex] = 0;
            pipeline_handles[eindex] = graphics::rhi::handles::INVALID_PIPELINE;
        }
    }

    bool is_valid(component c) {
        if (!c.is_valid()) return false;
        const id::id_type eindex{ id::index(c.get_id()) };
        return eindex < exists_flags.size() && exists_flags[eindex];
    }
    
    graphics::rhi::PipelineHandle get_pipeline_handle(component c) {
        assert(is_valid(c));
        return pipeline_handles[id::index(c.get_id())];
    }

    void set_pipeline_handle(component c, graphics::rhi::PipelineHandle handle) {
        assert(is_valid(c));
        pipeline_handles[id::index(c.get_id())] = handle;
    }
    
    graphics::rhi::PipelineBindPoint get_pipeline_type(component c) {
        assert(is_valid(c));
        return pipeline_types[id::index(c.get_id())];
    }
}
