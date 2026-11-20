#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalTexture.h"
#include "Engine/Graphics/Nanite/DepthHistoryManager.h"
#include "Engine/Common/CoreLogStream.h"
#include <array>
#include <cmath>
#include <cstdlib>

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;

static void Check(bool condition, const char* message) {
    if (!condition) { PRIMAL_LOG_STREAM(Error,"DepthHistoryTest") << message; std::exit(1); }
}

int main() {
    auto* pool = NS::AutoreleasePool::alloc()->init();
    DeviceDesc descriptor{}; descriptor.platform=RHIPlatform::Metal; descriptor.enableDebug=true;
    MetalDevice device(descriptor); Check(device.Initialize(),"Metal initialization");
    auto* native=device.GetNativeDevice(); auto* queue=native->newCommandQueue();
    const char* source=R"MSL(
        #include <metal_stdlib>
        using namespace metal;
        vertex float4 depth_vs(uint id [[vertex_id]]) {
            constexpr float2 positions[]={float2(-1,-1),float2(3,-1),float2(-1,3)};
            return float4(positions[id],0.5,1);
        }
        struct DepthOut { float depth [[depth(any)]]; };
        fragment DepthOut depth_fs(float4 p [[position]],constant float4& values [[buffer(0)]]) {
            return {values.z + 0.2*p.x/values.x + 0.3*p.y/values.y};
        }
    )MSL";
    NS::Error* error=nullptr;
    auto* library=native->newLibrary(NS::String::string(source,NS::UTF8StringEncoding),nullptr,&error);
    Check(library!=nullptr,"Depth pattern library");
    auto* vs=library->newFunction(NS::String::string("depth_vs",NS::UTF8StringEncoding));
    auto* fs=library->newFunction(NS::String::string("depth_fs",NS::UTF8StringEncoding));
    auto* pd=MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vs);pd->setFragmentFunction(fs);pd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    auto* pipeline=native->newRenderPipelineState(pd,&error);Check(pipeline!=nullptr,"Depth pattern pipeline");
    auto* ds=MTL::DepthStencilDescriptor::alloc()->init();ds->setDepthCompareFunction(MTL::CompareFunctionAlways);ds->setDepthWriteEnabled(true);
    auto* depthState=native->newDepthStencilState(ds);
    u64 checked=0,failed=0;double maximum=0;
    for (auto size : {std::array<u32,2>{1,1},std::array<u32,2>{17,19},std::array<u32,2>{65,33},std::array<u32,2>{2560,1440}}) {
        const auto w=size[0],h=size[1];
        TextureDesc td{};td.type=TextureType::Texture2D;td.size={w,h,1};td.format=DataFormat::D32_Float;
        td.usage=TextureUsage::DepthStencil|TextureUsage::ShaderResource;
        const auto depth=device.CreateTexture(td);Check(depth!=handles::INVALID_RESOURCE,"Depth allocation");
        nanite::DepthHistoryManager manager;nanite::DepthHistoryManager::Config config;
        config.width=w;config.height=h;Check(manager.Initialize(&device,config),"History initialization");
        const size_t stride=(size_t(w)*sizeof(float)+255)&~size_t(255);
        auto* readback=native->newBuffer(stride*h,MTL::ResourceStorageModeShared);Check(readback!=nullptr,"Readback allocation");
        // Six frames wrap every history slot and reveal stale or partial copies.
        for (u32 frame=0;frame<6;++frame) {
            const float values[]={float(w),float(h),0.1f+0.05f*float(frame),0};
            auto* rp=MTL::RenderPassDescriptor::renderPassDescriptor();
            rp->depthAttachment()->setTexture(device.GetTexture(depth)->GetNativeTexture());
            rp->depthAttachment()->setLoadAction(MTL::LoadActionClear);rp->depthAttachment()->setClearDepth(1);
            rp->depthAttachment()->setStoreAction(MTL::StoreActionStore);
            auto* render=queue->commandBuffer();auto* encoder=render->renderCommandEncoder(rp);
            encoder->setRenderPipelineState(pipeline);encoder->setDepthStencilState(depthState);
            encoder->setFragmentBytes(values,sizeof(values),0);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,NS::UInteger(0),NS::UInteger(3));
            encoder->endEncoding();render->commit();render->waitUntilCompleted();
            Check(render->status()==MTL::CommandBufferStatusCompleted,"Pattern render completion");
            const auto handle=device.CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=GetCommandBuffer(handle);
            Check(cmd && cmd->Begin(),"History command begin");
            Check(manager.StoreCurrentFrameDepth(depth,cmd,frame),"Record depth history");
            Check(cmd->End() && cmd->Submit() && cmd->WaitForCompletion(),"History copy completion");
            const auto history=manager.GetPreviousFrameDepth(frame+1);
            Check(history.is_valid && history.frame_index==frame,"History frame identity");
            auto* capture=queue->commandBuffer();auto* blit=capture->blitCommandEncoder();
            blit->copyFromTexture(device.GetTexture(history.texture)->GetNativeTexture(),0,0,MTL::Origin(0,0,0),MTL::Size(w,h,1),readback,0,stride,stride*h);
            blit->endEncoding();capture->commit();capture->waitUntilCompleted();
            Check(capture->status()==MTL::CommandBufferStatusCompleted,"History readback completion");
            for(u32 y=0;y<h;++y) {
                const auto* row=reinterpret_cast<const float*>(static_cast<const char*>(readback->contents())+y*stride);
                for(u32 x=0;x<w;++x) {
                    const double expected=double(values[2])+0.2*(double(x)+0.5)/w+0.3*(double(y)+0.5)/h;
                    const double difference=std::abs(double(row[x])-expected);
                    if (!std::isfinite(row[x]) || difference>1e-6) ++failed;
                    maximum=std::max(maximum,difference);++checked;
                }
            }
            device.DestroyCommandBuffer(handle);
        }
        readback->release();manager.Shutdown();device.DestroyTexture(depth);
    }
    PRIMAL_LOG_STREAM(Info,"DepthHistoryTest") << "pixels=" << checked << " failed=" << failed << " max_error=" << maximum;
    depthState->release();ds->release();pipeline->release();pd->release();vs->release();fs->release();library->release();queue->release();
    device.Shutdown();pool->release();return failed?2:0;
}
