// Private production VulkanQueryPool contract: no WAIT on opt-in timestamps.
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanQuery.h"
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using namespace primal;
using namespace primal::graphics::rhi;
namespace {
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
struct TestDevice : VulkanDevice {
 using VulkanDevice::VulkanDevice;
 using VulkanDevice::GetQueryPool;
};
struct GPU {
 RHIDeviceBase* d=nullptr;TestDevice* vk=nullptr;QueryPoolHandle query=handles::INVALID_QUERY_POOL;
 CommandBufferHandle reset=handles::INVALID_COMMAND_BUFFER,work=handles::INVALID_COMMAND_BUFFER;
 ResourceHandle src=handles::INVALID_RESOURCE,dst=handles::INVALID_RESOURCE;
 GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;vk=new TestDevice(desc);d=vk;check(vk->Initialize(),"Vulkan device");}
 ~GPU(){if(!d)return;d->WaitIdle();if(reset!=handles::INVALID_COMMAND_BUFFER)d->DestroyCommandBuffer(reset);if(work!=handles::INVALID_COMMAND_BUFFER)d->DestroyCommandBuffer(work);if(query!=handles::INVALID_QUERY_POOL)d->DestroyQueryPool(query);if(src!=handles::INVALID_RESOURCE)d->DestroyBuffer(src);if(dst!=handles::INVALID_RESOURCE)d->DestroyBuffer(dst);d->Shutdown();}
 VulkanCommandBuffer* begin(CommandBufferHandle& h){h=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(h);check(c&&c->Reset()&&c->Begin(),"begin command buffer");return c;}
};
void run(){
 check(setenv("PRIMAL_VK_QUERY_NONBLOCKING","1",1)==0,"set experiment");GPU g;
 QueryPoolDesc q{};q.type=QueryType::Timestamp;q.queryCount=2;g.query=g.d->CreateQueryPool(q);check(g.query!=handles::INVALID_QUERY_POOL,"timestamp pool");
 auto* pool=g.vk->GetQueryPool(g.query);check(pool&&pool->GetNativePool()!=VK_NULL_HANDLE,"native pool");
 // A completed GPU reset guarantees queries are unavailable, without relying
 // on hostQueryReset or an uninitialized query's undefined state.
 auto* reset=g.begin(g.reset);reset->ResetQueryPool(g.query,0,2);check(reset->End()&&reset->Submit(0)&&reset->WaitForCompletion(),"reset fence completion");
 uint64_t result[2]={0x1111111111111111ull,0x2222222222222222ull};
 check(!g.d->GetQueryPoolResults(g.query,0,2,result,sizeof(uint64_t)),"reset but unwritten timestamps report unavailable");
 struct Available {uint64_t value,available;};Available native[2]{};
 auto status=vkGetQueryPoolResults(g.vk->GetNativeDevice(),pool->GetNativePool(),0,2,sizeof(native),native,sizeof(Available),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
 check(status==VK_NOT_READY&&!native[0].available&&!native[1].available,"native oracle confirms unwritten queries unavailable");
 constexpr u64 bytes=4u*1024u*1024u;BufferDesc b{};b.size=bytes;b.type=BufferType::Raw;b.memoryUsage=GPUMemoryUsage::Dynamic;b.name="TimestampQueryFixture";g.src=g.d->CreateBuffer(b);g.dst=g.d->CreateBuffer(b);check(g.src!=handles::INVALID_RESOURCE&&g.dst!=handles::INVALID_RESOURCE,"copy buffers");
 auto* work=g.begin(g.work);work->WriteTimestamp(g.query,0);work->CopyBuffer(g.src,g.dst,0,0,bytes);work->WriteTimestamp(g.query,1);check(work->End(),"end work");
 // Recorded but not submitted: no GPU timestamp could have been produced.
 check(!g.d->GetQueryPoolResults(g.query,0,2,result,sizeof(uint64_t)),"recording does not make timestamps available");
 check(work->Submit(0),"submit timestamp work");result[0]=result[1]=0;
 bool immediate=g.d->GetQueryPoolResults(g.query,0,2,result,sizeof(uint64_t));
 if(immediate)check(result[0]>0&&result[1]>=result[0],"immediate success contains actual ordered timestamps");
 // Whether the immediate read was ready depends on scheduling. Explicit fence
 // completion, not query availability, authorizes reuse and final assertion.
 check(work->WaitForCompletion(),"work fence completion");result[0]=result[1]=0;
 check(g.d->GetQueryPoolResults(g.query,0,2,result,sizeof(uint64_t)),"completed timestamps readable");
 check(result[0]>0&&result[1]>result[0],"completed timestamp values strictly increasing");
 std::cout<<"{\"unwritten_not_ready\":true,\"recorded_unsubmitted_not_ready\":true,\"immediate_ready\":"<<(immediate?"true":"false")<<",\"completed_ticks\":["<<result[0]<<','<<result[1]<<"]}\n";
 unsetenv("PRIMAL_VK_QUERY_NONBLOCKING");result[0]=result[1]=0;
 check(g.d->GetQueryPoolResults(g.query,0,2,result,sizeof(uint64_t))&&result[0]>0&&result[1]>result[0],"default WAIT path still reads completed results");
}
}
int main(){try{run();return 0;}catch(const std::exception& e){std::cerr<<"Vulkan timestamp fixture: "<<e.what()<<'\n';return 1;}}
