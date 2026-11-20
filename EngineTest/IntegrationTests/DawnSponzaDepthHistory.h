#pragma once
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "DawnSponzaProbeUpdate.h"
#include "DawnSponzaProbeSchedule.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnBuffer.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace primal::tests::dawn_sponza {
// Copy -> successful submission -> MapAsync success -> consume N-2 -> unmap.
// Each slot remains unavailable for reuse until its exact copy has completed.
class DepthHistory {
public:
    ~DepthHistory() { Shutdown(); }
    DepthHistory() = default;
    DepthHistory(const DepthHistory&) = delete;
    DepthHistory& operator=(const DepthHistory&) = delete;
    bool Initialize(DawnDevice& device, u32 probes, u64 digest, u64 generation) {
        if(device_ || !probes || !digest || !generation)return false;
        device_=&device;probes_=probes;digest_=digest;generation_=generation;
        for(auto& slot:slots_){
            BufferDesc b{};b.size=u64(probes)*512;b.memoryUsage=GPUMemoryUsage::Readback;
            slot.buffer=device.CreateBuffer(b);if(slot.buffer==handles::INVALID_RESOURCE)return false;
        }
        values_.resize(u64(probes)*128);return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const ProbeUpdate::Output& output) {
        if(!device_ || frame!=nextRecord_ || output.frame!=frame || output.probes!=probes_ ||
           output.sceneDigest!=digest_ || output.originsGeneration!=generation_ ||
           output.depth==handles::INVALID_RESOURCE)return false;
        auto& slot=slots_[frame%3];if(slot.frame!=~0u || slot.map)return false;
        cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::Transfer,AccessFlag::MemoryWrite,AccessFlag::TransferRead);
        cmd.CopyBuffer(output.depth,slot.buffer,0,0,u64(probes_)*512);
        slot.frame=frame;slot.source=output.depth;++nextRecord_;return true;
    }
    // Call only after device.Submit for the command containing Record succeeds.
    bool Submitted(u32 frame) {
        if(!device_ || frame!=nextSubmit_)return false;
        auto& slot=slots_[frame%3];if(slot.frame!=frame || slot.map)return false;
        auto* buffer=device_->GetBuffer(slot.buffer);if(!buffer)return false;
        slot.map=std::make_shared<Mapping>();
        WGPUBufferMapCallbackInfo callback{};callback.mode=WGPUCallbackMode_AllowProcessEvents;
        callback.userdata1=new std::shared_ptr<Mapping>(slot.map);
        callback.callback=[](WGPUMapAsyncStatus status,WGPUStringView,void* data,void*){
            std::unique_ptr<std::shared_ptr<Mapping>> owner(static_cast<std::shared_ptr<Mapping>*>(data));
            (*owner)->success=status==WGPUMapAsyncStatus_Success;
            (*owner)->done.store(true,std::memory_order_release);
        };
        wgpuBufferMapAsync(buffer->GetNativeBuffer(),WGPUMapMode_Read,0,u64(probes_)*512,callback);
        ++nextSubmit_;return true;
    }
    bool Consume(u32 frame,ProbeSchedule::DepthHistory& result) {
        result={};if(!device_ || frame!=nextConsume_)return false;
        if(frame<2){++nextConsume_;return true;}
        auto& slot=slots_[(frame-2)%3];
        if(slot.frame!=frame-2 || !slot.map || slot.source==handles::INVALID_RESOURCE)return false;
        const auto start=std::chrono::steady_clock::now();
        while(!slot.map->done.load(std::memory_order_acquire)){
            wgpuInstanceProcessEvents(device_->GetInstance());
            if(std::chrono::steady_clock::now()-start>std::chrono::seconds(10))return false;
#ifdef __EMSCRIPTEN__
            if (!slot.map->done.load(std::memory_order_acquire)) emscripten_sleep(1);
#else
            if (!slot.map->done.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
        }
        if(!slot.map->success)return false;
        auto* buffer=device_->GetBuffer(slot.buffer);if(!buffer)return false;
        const void* mapped=wgpuBufferGetConstMappedRange(buffer->GetNativeBuffer(),0,u64(probes_)*512);
        if(!mapped)return false;
        std::memcpy(values_.data(),mapped,u64(probes_)*512);
        wgpuBufferUnmap(buffer->GetNativeBuffer());
        // Frozen variance readback starts only after three recorded copies.
        // Frame2 still drains copy0 so that slot0 can be reused at frame3.
        if(frame>=3)result={values_,slot.frame,true};
        slot.frame=~0u;slot.source=handles::INVALID_RESOURCE;slot.map.reset();++nextConsume_;return true;
    }
    void Shutdown() {
        if(!device_)return;
        for(auto& slot:slots_){
            if(auto* buffer=device_->GetBuffer(slot.buffer)) {
                if(slot.map)wgpuBufferUnmap(buffer->GetNativeBuffer());
                device_->DestroyBuffer(slot.buffer);
            }
            slot={};
        }
        device_=nullptr;values_.clear();nextRecord_=nextSubmit_=nextConsume_=0;
    }
private:
    struct Mapping {std::atomic<bool> done{false};bool success=false;};
    struct Slot {ResourceHandle buffer=handles::INVALID_RESOURCE,source=handles::INVALID_RESOURCE;u32 frame=~0u;std::shared_ptr<Mapping> map;};
    DawnDevice* device_=nullptr;u32 probes_=0,nextRecord_=0,nextSubmit_=0,nextConsume_=0;
    u64 digest_=0,generation_=0;std::array<Slot,3> slots_{};std::vector<float> values_;
};
}
