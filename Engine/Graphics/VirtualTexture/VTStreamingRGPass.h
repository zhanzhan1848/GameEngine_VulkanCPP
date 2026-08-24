/**
 * @file VTStreamingRGPass.h
 * @brief VT M2.2 — Virtual Texture 流送 pass 的 RenderGraph 声明
 * @details 把 VT 系统的每帧工作(页表 UAV 更新 + 流送上传 + 淘汰)注册进
 *          RenderGraph:setup 阶段导入页表并声明 写(UAV)→读(SRV) 转换,
 *          execute 阶段调用 virtual_texture_system::update(frame)。
 *          feedback 回读拷贝由渲染方在材质 pass 的命令缓冲上经
 *          record_feedback/AttachVTFeedback 记录(本 pass 不重复)。
 *
 * 依赖声明:
 *   - 页表:写(UnorderedAccess)后 读(ShaderResource)— RG 自动生成
 *     UAV→SRV barrier,后续材质 pass 采样可见(替代系统内部的手动
 *     barrier 序列,与管线其余 pass 的同步语义统一)。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-23
 */

#pragma once

#include "VirtualTextureSystem.h"
#include "../RenderGraph/RenderGraph.h"
#include "../RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics::vt {

struct VTStreamingPassInputs {
    u32 frame_index = 0;
};

struct VTStreamingPassData {
    rendergraph::RGResourceHandle page_table;
};

inline void AddVTStreamingPass(rendergraph::RenderGraph& graph,
                               const VTStreamingPassInputs& inputs) {
    auto& sys = virtual_texture_system::instance();
    if (!sys.is_initialized()) return;

    auto ptRG = graph.ImportResource("VT_PageTable", sys.page_table_texture(),
                                      ResourceState::ShaderResource);
    graph.AddPass<VTStreamingPassData>("VTStreaming",
        rendergraph::RGPassType::Compute,
        rendergraph::RGPassCategory::None,
        [ptRG](VTStreamingPassData& data, rendergraph::RenderGraphBuilder& builder) {
            // 页表:导入(SRV)→ 本 pass 写(UAV);pass 结束 RG 生成
            // UAV→SRV 转换(结束状态在 Write 时指定),后续材质 pass 采样可见。
            // 流送上传的 atlas 拷贝与 barrier 由系统 update() 内部闭合。
            data.page_table = builder.Write(ptRG, ResourceState::UnorderedAccess);
            builder.Read(data.page_table, ResourceState::ShaderResource);
        },
        [inputs](const VTStreamingPassData&, rendergraph::RenderGraphContext&) {
            virtual_texture_system::instance().update(inputs.frame_index);
        });
}

} // namespace primal::graphics::vt
