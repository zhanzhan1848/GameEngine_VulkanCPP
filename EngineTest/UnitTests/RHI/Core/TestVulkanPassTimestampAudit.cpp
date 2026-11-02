// Diagnostic availability fixture. Never interprets an uncompleted query.
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanQuery.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
using namespace primal::graphics::rhi;
namespace {
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
struct GPU {
 VulkanDevice* d=nullptr;VulkanQueryPool* q=nullptr;CommandBufferHandle cmds[4]{handles::INVALID_COMMAND_BUFFER,handles::INVALID_COMMAND_BUFFER,handles::INVALID_COMMAND_BUFFER,handles::INVALID_COMMAND_BUFFER};
 GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;d=new VulkanDevice(desc);check(d->Initialize(),"initialize device");q=new VulkanQueryPool(d->GetNativeDevice(),QueryType::Timestamp,2);check(q->GetNativePool()!=VK_NULL_HANDLE,"query pool");}
 ~GPU(){if(!d)return;d->WaitIdle();if(d->GetLastWaitIdleResult()!=VK_SUCCESS)std::_Exit(7);for(auto c:cmds)if(c!=handles::INVALID_COMMAND_BUFFER)d->DestroyCommandBuffer(c);delete q;d->Shutdown();delete d;}
 VulkanCommandBuffer* begin(int n){cmds[n]=d->CreateCommandBuffer(CommandQueueType::Graphics);auto* c=static_cast<VulkanCommandBuffer*>(primal::graphics::rhi::GetCommandBuffer(cmds[n]));check(c&&c->Reset()&&c->Begin()&&c->GetNativeCmdBuffer(),"begin graphics");return c;}
 void complete(VulkanCommandBuffer* c){check(c->End(),"end");check(c->Submit(0),"submit");if(!c->WaitForCompletion())std::_Exit(7);}
};
struct Query {uint64_t tick=0,available=0;};
VkResult read(GPU& g,Query (&q)[2]){q[0]={};q[1]={};return vkGetQueryPoolResults(g.d->GetNativeDevice(),g.q->GetNativePool(),0,2,sizeof(q),q,sizeof(Query),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);}
void run(){GPU g;Query q[2]{};
 uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(g.d->GetNativePhysicalDevice(),&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(g.d->GetNativePhysicalDevice(),&count,families.data());check(g.d->GetGraphicsQueueFamily()<count,"family");if(!families[g.d->GetGraphicsQueueFamily()].timestampValidBits){throw Unsupported("timestamp unsupported by graphics queue");}
 auto* c0=g.begin(0);vkCmdResetQueryPool(c0->GetNativeCmdBuffer(),g.q->GetNativePool(),0,2);g.complete(c0);check(read(g,q)==VK_NOT_READY&&!q[0].available&&!q[1].available,"reset unwritten");
 auto* c1=g.begin(1);vkCmdWriteTimestamp(c1->GetNativeCmdBuffer(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,g.q->GetNativePool(),0);check(read(g,q)==VK_NOT_READY&&!q[0].available&&!q[1].available,"only recorded, not submitted");g.complete(c1);check(read(g,q)==VK_NOT_READY&&q[0].available&&!q[1].available,"partial availability");
 auto* c2=g.begin(2);vkCmdResetQueryPool(c2->GetNativeCmdBuffer(),g.q->GetNativePool(),0,2);vkCmdWriteTimestamp(c2->GetNativeCmdBuffer(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,g.q->GetNativePool(),0);vkCmdWriteTimestamp(c2->GetNativeCmdBuffer(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,g.q->GetNativePool(),1);
 check(c2->End()&&c2->Submit(0),"submit complete pair");
 // No host read or reuse is authorized merely because Submit returned true.
 if(!c2->WaitForCompletion())std::_Exit(7);
 check(read(g,q)==VK_SUCCESS&&q[0].available&&q[1].available,"complete pair after fence");
 auto* c3=g.begin(3);vkCmdResetQueryPool(c3->GetNativeCmdBuffer(),g.q->GetNativePool(),0,2);g.complete(c3);check(read(g,q)==VK_NOT_READY&&!q[0].available&&!q[1].available,"generation reuse reset");
 std::puts("timestamp availability fixture passed");
}
}
int main(){try{run();return 0;}catch(const Unsupported& e){std::fprintf(stderr,"%s\n",e.what());return 2;}catch(const std::exception& e){std::fprintf(stderr,"timestamp fixture failed: %s\n",e.what());return 1;}}
