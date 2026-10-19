#pragma once
// Read-only, opt-in snapshots of the exact RG producer versions. No shader changes.
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/ToneMappingPass.h"
#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <chrono>
namespace primal::graphics::passcapture {
using namespace rhi;
using namespace rendergraph;
inline bool Requested(u32 frame) {
    const char* list = std::getenv("SPONZA_CAPTURE_FRAMES");
    if (!list) return false;
    std::istringstream input(list); std::string token;
    while (std::getline(input, token, ',')) if (std::strtoul(token.c_str(), nullptr, 10) == frame) return true;
    return false;
}
struct Snapshot { std::string name; ResourceHandle buffer, texture; u32 width, height, bytes; DataFormat format; };
inline std::vector<Snapshot> pending;
inline bool failed = false;
inline void Add(RenderGraph& graph, RGResourceHandle source, const char* label, u32 frame) {
    if (!Requested(frame) || !std::getenv("SPONZA_PASS_CAPTURE_DIR") || graph.GetDevice().GetPlatform()!=RHIPlatform::Vulkan) return;
    if (!source.IsValid()) { failed=true; return; }
    struct Data {};
    graph.AddPass<Data>(std::string("PassCapture_")+label, RGPassType::Compute, RGPassCategory::PostProcess,
        [source](Data&, RenderGraphBuilder& b) { b.SideEffect(); b.Read(source,ResourceState::CopySource); },
        [source,label](const Data&, RenderGraphContext& c) {
            auto* d=&c.graph->GetDevice(); auto* resource=c.graph->GetResource(source);
            if(!resource) { failed=true; return; } auto tex=resource->GetPhysicalHandle(); u32 w=0,h=0;
            const auto format=d->GetTextureFormat(tex);
            u32 bytes=format==DataFormat::RGBA16_Float?8u:
                (format==DataFormat::RGBA8_UNorm || format==DataFormat::BGRA8_UNorm)?4u:0u;
            if (!bytes || !d->GetTextureSize(tex,w,h)) { failed=true; return; }
            BufferDesc bd{}; bd.size=u64(w)*h*bytes; bd.bindFlags=u32(BufferUsageFlags::TransferDst); bd.memoryUsage=GPUMemoryUsage::Readback;
            auto buffer=d->CreateBuffer(bd); if(buffer==handles::INVALID_RESOURCE) { failed=true; return; }
            ResourceBarrier barrier{}; barrier.resource=tex; barrier.beforeState=ResourceState::Unknown; barrier.afterState=ResourceState::CopySource;
            c.cmdBuffer->InsertBarrier(&barrier,1);
            BufferTextureCopyRegion rr{}; rr.imageSubresource.layerCount=1; rr.imageExtent={w,h,1};
            c.cmdBuffer->CopyTextureToBuffer(tex,buffer,&rr,1);
            c.cmdBuffer->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
            barrier.beforeState=ResourceState::CopySource; barrier.afterState=ResourceState::ShaderResource; c.cmdBuffer->InsertBarrier(&barrier,1);
            pending.push_back({label,buffer,tex,w,h,bytes,format});
        });
}
// Called after the caller-owned render submission, on the same graphics queue.
inline bool Drain(RHIDeviceBase* device,u32 frame,double elapsed,u64 probeUpdates) {
    if(!Requested(frame) || !std::getenv("SPONZA_PASS_CAPTURE_DIR")) return true;
    auto handle=device->CreateCommandBuffer(CommandQueueType::Graphics); auto* cmd=rhi::GetCommandBuffer(handle);
    bool complete=cmd && cmd->Begin();
    if(complete) { cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead); complete=cmd->End() && cmd->Submit() && cmd->WaitForCompletion(); }
    if(handle!=handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(handle);
    complete=complete && !failed && pending.size()==5;
    std::string prefix=std::string(std::getenv("SPONZA_PASS_CAPTURE_DIR"))+"/frame-"+std::to_string(frame);
    FILE* meta=std::fopen((prefix+".json").c_str(),"w");
    if(meta) std::fprintf(meta,"{\"frame\":%u,\"elapsed_seconds\":%.9g,\"probe_updates\":%llu,\"sc_continuous_from\":120,\"sources\":[",frame,elapsed,(unsigned long long)probeUpdates);
    for(size_t i=0;i<pending.size();++i) { auto& s=pending[i]; u64 size=u64(s.width)*s.height*s.bytes;
        if(complete) { void* data=device->MapBuffer(s.buffer,0,size); FILE* out=data?std::fopen((prefix+"-"+s.name+".raw").c_str(),"wb"):nullptr;
            if(out) { complete=std::fwrite(data,1,size,out)==size; std::fclose(out); } else complete=false;
            if(data) device->UnmapBuffer(s.buffer); }
        if(meta) std::fprintf(meta,"%s{\"name\":\"%s\",\"width\":%u,\"height\":%u,\"bpp\":%u,\"format\":%u,\"resource\":%llu}",i?",":"",s.name.c_str(),s.width,s.height,s.bytes,unsigned(s.format),(unsigned long long)s.texture);
        device->DestroyBuffer(s.buffer);
    }
    if(meta) { std::fprintf(meta,"],\"complete\":%s,\"completion_scope\":\"checked_same_graphics_queue_after_render_submission\"}\n",complete?"true":"false"); std::fclose(meta); } else complete=false;
    if (FILE* provenance=std::fopen((prefix+"-tone-host.json").c_str(),"w")) {
        const auto tone=PostProcess::GetToneProvenance();
        std::fprintf(provenance,"{\"ev\":%.9g,\"base_exposure\":%.9g,\"exposure\":%.9g,\"adapt_enabled\":%s,\"contrast\":%.9g,\"saturation\":%.9g,\"vignette\":%.9g,\"bloom_intensity\":%.9g,\"scope\":\"host provenance after actual pipeline initialization; GPU UBO and descriptor memory UNKNOWN\"}\n",tone.exposure_ev,tone.base_exposure,tone.exposure,tone.adapt_enabled?"true":"false",tone.contrast,tone.saturation,tone.vignette,tone.bloom_intensity);
        std::fclose(provenance);
    } else complete=false;
    pending.clear(); failed=false; return complete;
}
}
