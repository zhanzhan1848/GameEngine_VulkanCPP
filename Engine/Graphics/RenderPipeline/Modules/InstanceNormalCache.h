#pragma once
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include <array>
#include <memory>
namespace primal::graphics {
struct GIGatherInputs;
namespace rendergraph { class RenderGraph; }
// Vulkan-only, default-off sidecar. Recorded != completed: reuse is permitted
// only after the host publishes a newer successful fence completion for slot.
class InstanceNormalCache {
public:
    bool Initialize(rhi::RHIDeviceBase*,const rhi::DescriptorSetLayoutBinding*,u32);
    void Shutdown();
    bool Prepare(const GIGatherInputs&);
    std::array<rendergraph::RGResourceHandle,3> AddPasses(rendergraph::RenderGraph&,const GIGatherInputs&,
        const rendergraph::RGResourceHandle* geometry);
    bool Available(const GIGatherInputs&) const;
    void Record(rhi::RHICommandBuffer*,const GIGatherInputs&);
    rhi::DescriptorSetHandle Set(u32 slot)const{return slots_[slot%3].gather_set;}
    rhi::PipelineHandle Pipeline()const{return gather_pipeline_;}
    rhi::PipelineLayoutHandle Layout()const{return gather_layout_;}
    void Receipt(const GIGatherInputs&);
    rhi::ResourceHandle Buffer(u32 slot,u32 index)const{return slots_[slot%3].buffers[index];}
private:
    bool Matches(const GIGatherInputs&)const;
    void ClearSlot(u32);
    struct Slot {
        rhi::ResourceHandle buffers[3]{rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle sources[4]{};u64 sizes[4]{};
        rhi::DescriptorSetHandle produce_set=rhi::handles::INVALID_DESCRIPTOR_SET,gather_set=rhi::handles::INVALID_DESCRIPTOR_SET;
        u64 epoch=0,digest=0,owner_hash=0,payload_hash=0,record_serial=0;
        u32 count=0,record_frame=~u32(0);bool prepared=false,recorded=false,receipt=false;
    } slots_[3];
    rhi::RHIDeviceBase* device_=nullptr;
    rhi::ShaderHandle shaders_[3]{rhi::handles::INVALID_SHADER,rhi::handles::INVALID_SHADER,rhi::handles::INVALID_SHADER};
    u64 shader_hash_[3]{};
    rhi::DescriptorSetLayoutHandle produce_set_layout_=rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT,gather_set_layout_=rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    rhi::PipelineLayoutHandle produce_layout_=rhi::handles::INVALID_PIPELINE_LAYOUT,gather_layout_=rhi::handles::INVALID_PIPELINE_LAYOUT;
    rhi::PipelineHandle produce_pipeline_=rhi::handles::INVALID_PIPELINE,verify_pipeline_=rhi::handles::INVALID_PIPELINE,gather_pipeline_=rhi::handles::INVALID_PIPELINE;
};
}
