#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalTexture.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/TAAPass.h"
#include "Engine/Graphics/RenderGraph/RenderGraph.h"
#include "Engine/Common/CoreLogStream.h"
#include <array>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
static void Check(bool ok,const std::string& message) {
    if(!ok){PRIMAL_LOG_STREAM(Error,"TAARasterTest") << message;std::exit(1);}
}
int main() {
    auto* pool=NS::AutoreleasePool::alloc()->init();
    DeviceDesc dd{};dd.platform=RHIPlatform::Metal;dd.enableDebug=true;
    MetalDevice device(dd);Check(device.Initialize(),"Metal device");
    auto* queue=device.GetNativeDevice()->newCommandQueue();
    u32 graphs=0,pixels=0;
    for(auto size:{std::array<u32,2>{16,16},std::array<u32,2>{17,19}}) {
        const u32 w=size[0],h=size[1];
        auto makeColor=[&](bool velocity) {
            TextureDesc d{};d.type=TextureType::Texture2D;d.size={w,h,1};d.format=DataFormat::RGBA32_Float;
            d.memoryUsage=GPUMemoryUsage::Staging;d.usage=TextureUsage::ShaderResource;
            auto handle=device.CreateTexture(d);Check(handle!=handles::INVALID_RESOURCE,"color allocation");
            std::vector<float> data(w*h*4,0);
            for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x) {
                for(u32 c=0;c<3;++c)data[(y*w+x)*4+c]=velocity?0.f:((x+y)%2?.9f:.1f);
                data[(y*w+x)*4+3]=1;
            }
            device.GetTexture(handle)->GetNativeTexture()->replaceRegion(MTL::Region(0,0,w,h),0,data.data(),w*16);
            return handle;
        };
        auto makeDepth=[&](bool attachment,u32 width) {
            TextureDesc d{};d.type=TextureType::Texture2D;d.size={width,h,1};
            d.format=attachment?DataFormat::D32_Float:DataFormat::R32_Float;
            d.memoryUsage=attachment?GPUMemoryUsage::Static:GPUMemoryUsage::Staging;
            d.usage=TextureUsage::ShaderResource;
            if(attachment)d.usage=d.usage|TextureUsage::DepthStencil;
            auto handle=device.CreateTexture(d);Check(handle!=handles::INVALID_RESOURCE,"depth allocation");
            auto* native=device.GetTexture(handle)->GetNativeTexture();
            if(attachment) {
                auto* rp=MTL::RenderPassDescriptor::renderPassDescriptor();rp->depthAttachment()->setTexture(native);
                rp->depthAttachment()->setLoadAction(MTL::LoadActionClear);rp->depthAttachment()->setStoreAction(MTL::StoreActionStore);
                rp->depthAttachment()->setClearDepth(.5);
                auto* c=queue->commandBuffer();c->renderCommandEncoder(rp)->endEncoding();c->commit();c->waitUntilCompleted();
                Check(c->status()==MTL::CommandBufferStatusCompleted,"depth clear");
            } else {
                std::vector<float> data(width*h,.5f);native->replaceRegion(MTL::Region(0,0,width,h),0,data.data(),width*4);
            }
            return handle;
        };
        const auto current=makeColor(false),velocity=makeColor(true);
        const auto depth=makeDepth(true,w),otherDepth=makeDepth(true,w),previous=makeDepth(false,w),wrongSize=makeDepth(false,w+1);
        auto run=[&](u32 frame,int mode,math::v2 jitter) {
            rendergraph::RenderGraph graph(device);
            const auto colorRG=graph.ImportResource("color",current,ResourceState::ShaderResource);
            const auto velocityRG=graph.ImportResource("velocity",velocity,ResourceState::ShaderResource);
            PostProcess::TAADepthInputs input{};
            input.currDepth=mode==7?otherDepth:depth;
            input.prevDepth=mode==8?wrongSize:(mode==11?handles::INVALID_RESOURCE:previous);
            input.previous_depth_frame=mode==6?123u:(frame?frame-1:~0u);
            input.current_depth_rg=graph.ImportResource("depth",input.currDepth,ResourceState::ShaderResource);
            input.require_raster_jitter=true;
            const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            std::memcpy(&input.currInvViewProj,identity,sizeof(identity));std::memcpy(&input.prevViewProj,identity,sizeof(identity));
            math::v2 actual{99,99};u32 providerCalls=0;
            if(mode!=3)input.raster_jitter_provider=[&](math::v2& value){++providerCalls;value=actual;return mode!=4;};
            const auto output=PostProcess::AddTAAPass(graph,colorRG,velocityRG,w,h,frame,&input).output;
            graph.MarkAsOutput(output);graph.Compile();
            Check(providerCalls==0,"jitter must not be queried while building graph");
            actual=jitter;if(mode==5)actual.x=std::numeric_limits<float>::quiet_NaN();
            const auto command=device.CreateCommandBuffer(CommandQueueType::Graphics);
            auto* cmd=GetCommandBuffer(command);Check(cmd && cmd->Begin(),"RHI command begin");
            graph.Execute(cmd);Check(cmd->End() && cmd->Submit() && cmd->WaitForCompletion(),"TAA graph completion");
            Check(providerCalls==(mode==3?0u:1u),"jitter evaluated once at execution");
            const auto handle=graph.GetResource(output)->GetPhysicalHandle();
            auto* texture=device.GetTexture(handle)->GetNativeTexture();
            const u32 stride=((w*8+255)/256)*256;
            auto* buffer=device.GetNativeDevice()->newBuffer(stride*h,MTL::ResourceStorageModeShared);
            auto* c=queue->commandBuffer();auto* blit=c->blitCommandEncoder();
            blit->copyFromTexture(texture,0,0,MTL::Origin(0,0,0),MTL::Size(w,h,1),buffer,0,stride,stride*h);
            blit->endEncoding();c->commit();c->waitUntilCompleted();Check(c->status()==MTL::CommandBufferStatusCompleted,"readback");
            std::vector<float> result(w*h);
            for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x) {
                const auto* v=reinterpret_cast<const _Float16*>(static_cast<const u8*>(buffer->contents())+y*stride+x*8);
                Check(std::isfinite(float(v[0])) && float(v[0])==float(v[1]) && float(v[1])==float(v[2]) && float(v[3])==1,"finite grayscale RGBA");
                result[y*w+x]=float(v[0]);
            }
            buffer->release();device.DestroyCommandBuffer(command);++graphs;return result;
        };
        for(int mode=0;mode<12;++mode) {
            PostProcess::ResetTAAHistory();
            const auto seed=run(0,-1,math::v2{0,0});
            for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x)
                Check(std::abs(seed[y*w+x]-((x+y)%2?.9f:.1f))<.001f,"first frame passthrough");
            if(mode==9)PostProcess::ResetTAAHistory();
            // Jitter in pixel units; correct NDC history offset is (-x,+y).
            const float jx=mode==1?0.f:(mode==2?.35f:1.f),jy=mode==1?1.f:(mode==2?-.45f:0.f);
            const auto output=run(mode==10?4:1,mode,math::v2{2*jx/w,2*jy/h});
            for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x) {
                const float currentValue=(x+y)%2?.9f:.1f;
                float expected=currentValue;
                if(mode<3) {
                    const float px=float(x)-jx,py=float(y)+jy;
                    // UV bounds, not texel-centre bounds, govern history acceptance.
                    if(px>=-.5f && px<=w-.5f && py>=-.5f && py<=h-.5f) {
                        const int ix=int(std::floor(px)),iy=int(std::floor(py));const float fx=px-ix,fy=py-iy;
                        float history=0;
                        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
                            const u32 sx=u32(std::clamp(ix+dx,0,int(w)-1)),sy=u32(std::clamp(iy+dy,0,int(h)-1));
                            history+=seed[sy*w+sx]*(dx?fx:1-fx)*(dy?fy:1-fy);
                        }
                        expected=.9f*history+.1f*currentValue;
                    }
                }
                Check(std::abs(output[y*w+x]-expected)<.001f,"raster/history mismatch mode="+std::to_string(mode)+" at "+std::to_string(x)+","+std::to_string(y)+" got="+std::to_string(output[y*w+x])+" expected="+std::to_string(expected));
                ++pixels;
            }
        }
        for(auto handle:{current,velocity,depth,otherDepth,previous,wrongSize})device.DestroyTexture(handle);
    }
    PRIMAL_LOG_STREAM(Info,"TAARasterTest") << "PASS native Metal TAA: " << graphs << " RHI graphs, " << pixels
        << " analytic pixels; execution jitter XY/fractional, missing/invalid provider, frame/source/extent mismatch, reset/gap/resize";
    queue->release();device.Shutdown();pool->release();return 0;
}
