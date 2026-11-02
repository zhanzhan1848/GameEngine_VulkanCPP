#pragma once
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/PassTimestampTicket.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Engine/Common/CoreDiag.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace primal::graphics::rhi::pass_timestamp {
// Sponza diagnostic only. Native query pools never enter production RG stats.
class Audit {
    static constexpr uint32_t MaxPairs=48, QueryCount=MaxPairs*2;
    struct Pair {char name[80]{};uint32_t graph=0,occurrence=0,first=0;bool start=false,end=false,graphics=false;};
    struct Slot {VkQueryPool pool=VK_NULL_HANDLE;std::array<Pair,MaxPairs> pairs{};uint32_t count=0,graphs=0;bool reset=false,overflow=false;std::chrono::steady_clock::time_point begin{};};
    BoundTickets owner_;std::array<Slot,3> slots_{};VulkanDevice* device_=nullptr;VulkanCommandBuffer* cmd_=nullptr;
    uint32_t active_=~0u,graph_=0,validBits_=0;uint64_t nextTicket_=0;double period_=0;
    static bool Enabled(){static const bool v=[] {const char* p=std::getenv("PRIMAL_VK_PASS_TIMESTAMP_AUDIT");return p&&std::strcmp(p,"1")==0;}();return v;}
    static bool Selected(uint64_t f){return f>=128 && f<=240 && (f-128)%16==0;}
    static bool Requested(const char* s){for(const char* x:{"DDGIGIGather","LumenDDGI","LumenDDGI_Far","ScreenProbeGI","EnvironmentVisibility.Trace","EnvironmentVisibility.Resolve","LumenSurfaceCache","DeferredLighting","FinalBlit"})if(std::strcmp(x,s)==0)return true;return false;}
    static void Log(const char* s){primal::diag::Emit(primal::diag::kLevelInfo,"Render",s);}
    static void Reject(uint64_t frame,uint32_t slot,const char* reason){char out[256];std::snprintf(out,sizeof(out),"[VK_PASS_TIMESTAMP_AUDIT] source_frame=%llu slot=%u status=%s",(unsigned long long)frame,slot,reason);Log(out);}
    bool Pools(VulkanDevice* v){
        if(device_)return device_==v;device_=v;
        auto pd=v->GetNativePhysicalDevice();auto dev=v->GetNativeDevice();if(!pd||!dev)return false;
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(pd,&props);period_=props.limits.timestampPeriod;
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(pd,&count,nullptr);std::vector<VkQueueFamilyProperties> q(count);vkGetPhysicalDeviceQueueFamilyProperties(pd,&count,q.data());
        if(v->GetGraphicsQueueFamily()>=count)return false;validBits_=q[v->GetGraphicsQueueFamily()].timestampValidBits;
        if(!validBits_||validBits_>64||!FiniteDouble(period_)||period_<=0)return false;
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryType=VK_QUERY_TYPE_TIMESTAMP;ci.queryCount=QueryCount;
        for(auto& s:slots_)if(vkCreateQueryPool(dev,&ci,nullptr,&s.pool)!=VK_SUCCESS){for(auto& x:slots_)if(x.pool){vkDestroyQueryPool(dev,x.pool,nullptr);x.pool=VK_NULL_HANDLE;}return false;}
        return true;
    }
    void Read(uint32_t slot,uint64_t readFrame,uint64_t serial){
        auto& data=slots_[slot];auto* ticket=owner_.tickets.Get(slot);if(!ticket||ticket->status!=Tickets::Status::Ready)return;
        const auto elapsed=std::chrono::duration<long double,std::nano>(std::chrono::steady_clock::now()-data.begin).count();
        bool seen[4][9]{};const char* names[]={"DDGIGIGather","LumenDDGI","LumenDDGI_Far","ScreenProbeGI","EnvironmentVisibility.Trace","EnvironmentVisibility.Resolve","LumenSurfaceCache","DeferredLighting","FinalBlit"};
        for(uint32_t i=0;i<data.count;++i){const auto& p=data.pairs[i];for(int j=0;j<9;++j)if(p.graph>=1 && p.graph<=4 && std::strcmp(p.name,names[j])==0)seen[p.graph-1][j]=true;
            struct Q{uint64_t tick,available;};Q q[2]{};VkResult result=VK_ERROR_UNKNOWN;bool called=false;const char* status="unsupported_graphics";long double ns=0;
            if(!p.graphics){status="incomplete_recording";if(p.start&&p.end){called=true;result=vkGetQueryPoolResults(device_->GetNativeDevice(),data.pool,p.first,2,sizeof(q),q,sizeof(Q),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                const auto evaluated=Evaluate(int32_t(result),q[0].available,q[1].available,q[0].tick,q[1].tick,validBits_,period_,elapsed);
                status=evaluated.reason;ns=evaluated.nanoseconds;}}
            char duration[80];if(std::strcmp(status,"accepted")==0)std::snprintf(duration,sizeof(duration),"%.3Lf",ns);else std::snprintf(duration,sizeof(duration),"unavailable");
            char out[1024];std::snprintf(out,sizeof(out),"[VK_PASS_TIMESTAMP_AUDIT] source_frame=%llu read_frame=%llu slot=%u graph=%u pass=%s occurrence=%u begin_index=%u end_index=%u pool=%llu generation=%llu cmd=%llu fence=%llu ticket=%llu recorded_serial=%llu completion_serial=%llu device_generation=%llu host_bound_ns=%.21Lg vk_called=%u vk_result=%d start_tick=%llu end_tick=%llu start_available=%llu end_available=%llu valid_bits=%u period_ns=%.9g elapsed_ns=%s status=%s",
                (unsigned long long)ticket->frame,(unsigned long long)readFrame,slot,p.graph,p.name,p.occurrence,p.first,p.first+1,(unsigned long long)(uintptr_t)data.pool,(unsigned long long)ticket->generation,(unsigned long long)ticket->command,(unsigned long long)ticket->fence,(unsigned long long)ticket->ticket,(unsigned long long)ticket->serial,(unsigned long long)serial,(unsigned long long)owner_.binding.Generation(),elapsed,unsigned(called),int(result),(unsigned long long)q[0].tick,(unsigned long long)q[1].tick,(unsigned long long)q[0].available,(unsigned long long)q[1].available,validBits_,period_,duration,status);Log(out);
        }
        if(!data.graphs)Reject(ticket->frame,slot,"no_executed_graph");
        for(uint32_t g=0;g<data.graphs && g<4;++g)for(int j=0;j<9;++j)if(!seen[g][j]){char out[256];std::snprintf(out,sizeof(out),"[VK_PASS_TIMESTAMP_AUDIT] source_frame=%llu read_frame=%llu slot=%u graph=%u pass=%s status=missing",(unsigned long long)ticket->frame,(unsigned long long)readFrame,slot,g+1,names[j]);Log(out);}
        if(data.overflow)Log("[VK_PASS_TIMESTAMP_AUDIT] status=capacity_overflow");owner_.tickets.Retire(slot,ticket->generation);
    }
public:
    uint64_t Begin(VulkanDevice* dev,VulkanCommandBuffer* cmd,uint64_t command,uint32_t slot,uint64_t fence,uint64_t frame){
        active_=~0u;cmd_=nullptr;graph_=0;if(!Enabled()||!dev||!cmd||slot>=3)return 0;
        const char* legacy=std::getenv("PRIMAL_RG_TIMESTAMPS");if(!legacy||std::strcmp(legacy,"0")!=0){if(Selected(frame))Reject(frame,slot,"requires_rg_timestamps_off");return 0;}
        // Bind before consulting completion or touching any pool/device from a
        // previous lifetime. Generation defeats pointer/native handle reuse.
        if(!owner_.binding.BindOrMatch(reinterpret_cast<uintptr_t>(dev),dev->GetLifecycleGeneration())) {
            if(Selected(frame))Reject(frame,slot,"device_lifecycle_mismatch");return 0;
        }
        if(rhi::GetCommandBuffer(command)!=cmd||cmd->GetType()!=CommandQueueType::Graphics||!cmd->IsRecordingOutsideRenderPass()||cmd->GetNativeQueueFamilyIndex()!=dev->GetGraphicsQueueFamily()||!cmd->GetNativeCmdBuffer())return 0;
        if(!owner_.tickets.OnFence(dev->FrameSlotCompletions(),slot,fence)){if(Selected(frame))Reject(frame,slot,"no_completed_host_slot");return 0;}
        if(device_)Read(slot,frame,owner_.tickets.Serial(slot));if(!Selected(frame))return 0;
        if(!Pools(dev) || !slots_[slot].pool || nextTicket_==~uint64_t(0)-1){Reject(frame,slot,"pool_or_queue_unsupported");return 0;}
        const auto ticket=++nextTicket_;if(!owner_.tickets.Start(slot,command,fence,frame,ticket)){Reject(frame,slot,"slot_not_reusable");return 0;}
        auto& s=slots_[slot];s.count=0;s.graphs=0;s.reset=true;s.overflow=false;s.begin=std::chrono::steady_clock::now();
        vkCmdResetQueryPool(cmd->GetNativeCmdBuffer(),s.pool,0,QueryCount);active_=slot;cmd_=cmd;return ticket;
    }
    void Graph(VulkanCommandBuffer* cmd){if(active_!=~0u&&cmd==cmd_){++graph_;slots_[active_].graphs=graph_;if(graph_>4)slots_[active_].overflow=true;}}
    int BeginPass(VulkanCommandBuffer* cmd,const char* name,bool graphics){
        if(active_==~0u||cmd!=cmd_||!Requested(name)||graph_==0||graph_>4)return -1;auto& s=slots_[active_];
        if(s.count>=MaxPairs){s.overflow=true;return -1;}const uint32_t i=s.count++;auto& p=s.pairs[i];p={};std::snprintf(p.name,sizeof(p.name),"%s",name);p.graph=graph_;p.first=i*2;p.graphics=graphics;
        for(uint32_t j=0;j<i;++j)if(s.pairs[j].graph==graph_&&std::strcmp(s.pairs[j].name,name)==0)++p.occurrence;
        if(!graphics && s.reset && s.pool && cmd->GetNativeCmdBuffer() && cmd->IsRecordingOutsideRenderPass()) {vkCmdWriteTimestamp(cmd->GetNativeCmdBuffer(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.pool,p.first);p.start=true;}
        return int(i);
    }
    void EndPass(VulkanCommandBuffer* cmd,int index){if(active_==~0u||cmd!=cmd_||index<0)return;auto& s=slots_[active_];auto& p=s.pairs[uint32_t(index)];if(p.start && cmd->GetNativeCmdBuffer() && cmd->IsRecordingOutsideRenderPass()){vkCmdWriteTimestamp(cmd->GetNativeCmdBuffer(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.pool,p.first+1);p.end=true;}}
    void Commit(uint32_t slot,uint64_t cmd,uint64_t fence,uint64_t frame,uint64_t ticket,bool ended,bool submitted){if(!ticket)return;if(!owner_.tickets.Commit(slot,cmd,fence,frame,ticket,ended,submitted))Reject(frame,slot,"submission_not_certified");active_=~0u;cmd_=nullptr;}
    // Call only after the existing device idle operation reported VK_SUCCESS.
    void ReleaseAfterIdle(VulkanDevice* dev){
        if(!dev || dev->GetLastWaitIdleResult()!=VK_SUCCESS ||
           !owner_.Release(reinterpret_cast<uintptr_t>(dev),dev->GetLifecycleGeneration(),true))return;
        for(auto& s:slots_)if(s.pool)vkDestroyQueryPool(dev->GetNativeDevice(),s.pool,nullptr);
        slots_={};device_=nullptr;cmd_=nullptr;active_=~0u;graph_=0;validBits_=0;nextTicket_=0;period_=0;
    }
};
inline thread_local Audit instance;
}
#endif
