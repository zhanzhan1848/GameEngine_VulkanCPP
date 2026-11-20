#include "DawnSponzaShadows.h"
#include "Engine/Common/CoreDiag.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace primal::graphics::rhi;
using namespace primal::tests::dawn_sponza;
static void Require(bool ok,const char* text){if(!ok){primal::diag::Emit(primal::diag::kLevelError,"DawnShadows",text);std::exit(1);}}
static void Near(double actual,double expected,double tolerance,const char* label){
    if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance){
        char message[256];std::snprintf(message,sizeof(message),"%s actual=%.9g expected=%.9g",label,actual,expected);Require(false,message);
    }
}
int main(){
    DeviceDesc dd{};dd.platform=RHIPlatform::Dawn;dd.enableValidation=true;DawnDevice device(dd);
    Require(device.Initialize(),"device initialization failed");wgpuDevicePushErrorScope(device.GetNativeDevice(),WGPUErrorFilter_Validation);
    constexpr u32 mapSize=256;
    Shadows shadows;Require(shadows.Initialize(device,2,2,mapSize),"production shadows init failed");
    TextureDesc td{};td.type=TextureType::Texture2D;td.size={2,2,1};td.format=DataFormat::D32_Float;
    td.usage=TextureUsage::DepthStencil|TextureUsage::ShaderResource;auto depth=device.CreateTexture(td);
    td.format=DataFormat::RGBA16_Float;td.usage=TextureUsage::CopyDest|TextureUsage::ShaderResource;
    auto normal=device.CreateTexture(td);const uint16_t normals[16]{0x3800,0x3800,0x3c00,0x3c00,0x3800,0x3800,0x3c00,0x3c00,
        0x3800,0x3800,0x3c00,0x3c00,0x3800,0x3800,0x3c00,0x3c00};
    device.UpdateTextureData(normal,normals,0,0,0,2,2,1,16);
    BufferDesc bd{};bd.size=4;bd.memoryUsage=GPUMemoryUsage::Readback;auto readback=device.CreateBuffer(bd);
    Require(depth!=handles::INVALID_RESOURCE && normal!=handles::INVALID_RESOURCE && readback!=handles::INVALID_RESOURCE,"fixture resources failed");
    auto clear=[&](DawnCommandBuffer& cmd,ResourceHandle tex,float value){
        RenderPassDesc pass{};pass.depthAttachment.texture=tex;pass.depthAttachment.format=DataFormat::D32_Float;
        pass.depthAttachment.loadOp=LoadAction::Clear;pass.depthAttachment.storeOp=StoreAction::Store;pass.depthAttachment.clearValue.depth=value;
        cmd.BeginRenderPass(pass);cmd.EndRenderPass();
    };
    u32 cases=0;
    auto run=[&](int selected,u32 quality,bool sky,bool blocked,float expected=-1.f,float sun=0.f){
        ShadowParameters p{};p.inverseViewProjection=smath::MatrixIdentity();p.lightDirection={0,0,1,0};p.width=p.height=2;
        p.quality=quality;p.texelSize={1.f/mapSize,1.f/mapSize};p.depthTexelSize={.5f,.5f};p.sunTanHalfAngle=sun;
        for(u32 c=0;c<4;++c){p.lightVP[c]=smath::MatrixIdentity();if(int(c)!=selected)primal::math::set_elem(p.lightVP[c],3,0,4);
            p.cascade[c]={.001f,1,1,0};}
        auto command=device.CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=device.GetCommandBuffer(command);Require(cmd&&cmd->Begin(),"begin failed");
        clear(*cmd,depth,sky?1.f:.5f);
        for(u32 c=0;c<4;++c)clear(*cmd,shadows.Map(c),int(c)==selected&&blocked?.25f:.75f);
        Require(shadows.RecordFilter(*cmd,depth,normal,p,cases),"filter record failed");
        BufferTextureCopyRegion region{};region.imageExtent={1,1,1};cmd->CopyTextureToBuffer(shadows.Visibility(),readback,&region,1);
        Require(cmd->End(),"end failed");QueueSubmitInfo submit{};submit.cmdBuffer=command;Require(device.Submit(submit)&&cmd->WaitForCompletion(),"completion failed");
        const auto* bytes=static_cast<const unsigned char*>(device.MapBuffer(readback,0,4));Require(bytes,"readback failed");
        const bool lit=sky||quality==255u||selected<0||!blocked;
        const double vis=expected>=0?expected:lit?1:0;
        const double transmission=expected>=0||lit?0:std::exp(-.2505*1.5);
        std::fprintf(stderr,"fixture=%u cascade=%d quality=%u vis=%u transmission=%u\n",cases,selected,quality,bytes[0],bytes[1]);
        Near(bytes[0],std::round(vis*255),1,"visibility");Near(bytes[1],std::round(transmission*255),1,"transmission");
        Require(bytes[2]==0&&bytes[3]==255,"output packing mismatch");
        device.UnmapBuffer(readback);device.DestroyCommandBuffer(command);++cases;
    };
    for(u32 q=0;q<3;++q)for(int c=0;c<4;++c)run(c,q,false,true);
    run(-1,0,false,true);run(0,0,true,true);run(0,255,false,true);run(0,0,false,false);
    for(auto [q,v]:{std::pair<u32,float>{3,.25f},{4,.5f},{5,0},{6,1},{7,0}})run(0,q,false,true,v);
    run(0,2,false,true,-1,.0046749f);run(3,2,false,false,-1,.0046749f);

    // Exercise the actual indexed-layout raster producer and distinct UBOs.
    // A plane at z=.25 fills the map; cascade-specific Z translations remain
    // visible after ONE queue submission (no per-cascade upload overwrite).
    float vertices[6][8]{{-1,-1,.25f},{1,-1,.25f},{1,1,.25f},{-1,-1,.25f},{1,1,.25f},{-1,1,.25f}};
    BufferDesc vb{};vb.size=sizeof(vertices);vb.type=BufferType::Vertex;vb.memoryUsage=GPUMemoryUsage::Dynamic;
    auto vertex=device.CreateBuffer(vb);Require(device.UpdateBufferData(vertex,vertices,sizeof(vertices)),"vertex upload failed");
    ShadowParameters raster{};for(u32 c=0;c<4;++c){raster.lightVP[c]=smath::MatrixIdentity();primal::math::set_elem(raster.lightVP[c],3,2,.1f*c);}
    std::array<ResourceHandle,4> mapReadbacks;
    bd.size=mapSize*mapSize*4;for(auto& h:mapReadbacks)h=device.CreateBuffer(bd);
    auto command=device.CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=device.GetCommandBuffer(command);Require(cmd&&cmd->Begin(),"raster begin failed");
    Require(shadows.RecordMaps(*cmd,raster,0,[&](DawnCommandBuffer& c){ResourceHandle buffers[]{vertex};u64 offsets[]{0};c.BindVertexBuffers(0,1,buffers,offsets);c.Draw(6,0,1,0);}),"raster record failed");
    for(u32 c=0;c<4;++c){BufferTextureCopyRegion r{};r.imageExtent={mapSize,mapSize,1};cmd->CopyTextureToBuffer(shadows.Map(c),mapReadbacks[c],&r,1);}
    Require(cmd->End(),"raster end failed");QueueSubmitInfo submit{};submit.cmdBuffer=command;Require(device.Submit(submit)&&cmd->WaitForCompletion(),"raster completion failed");
    for(u32 c=0;c<4;++c){const auto* value=static_cast<const float*>(device.MapBuffer(mapReadbacks[c],0,mapSize*mapSize*4));Require(value,"raster map failed");Near(value[128*mapSize+128],.251+.1*c,1e-6,"caster depth + cascade UBO");device.UnmapBuffer(mapReadbacks[c]);device.DestroyBuffer(mapReadbacks[c]);}
    device.DestroyCommandBuffer(command);

    // Rasterize a half-plane: D32=.251 to the left, 1 to the right.
    // Golden counts come from analytic bilinear interpolation of this step
    // and the frozen Poisson coordinates, at screen pixel (0,0), rotation 0.
    // They distinguish a point load from bilinear depth, and PCF from PCSS.
    const float edgeVertices[6][8]{{-1,-1,.25f},{0,-1,.25f},{0,1,.25f},{-1,-1,.25f},{0,1,.25f},{-1,1,.25f}};
    Require(device.UpdateBufferData(vertex,edgeVertices,sizeof(edgeVertices)),"edge upload failed");
    struct Edge { u32 quality; float shift, sun, visibility; };
    const Edge edges[]{
        {0,-.001f,0,0},{0,0,0,1},
        {1,-.004f,0,.5f},{1,-.001f,0,.5f},{1,0,0,.5f},{1,.001f,0,.625f},{1,.004f,0,.75f},
        {2,-.004f,0,.375f},{2,-.001f,0,.5f},{2,0,0,.5f},{2,.001f,0,.625f},{2,.004f,0,.75f},
        {2,-.004f,.0046749f,.375f},{2,-.001f,.0046749f,.5f},{2,0,.0046749f,.5f},
        {2,.001f,.0046749f,.625f},{2,.004f,.0046749f,1}};
    for(const auto& edge:edges){
        ShadowParameters p{};p.inverseViewProjection=smath::MatrixIdentity();
        primal::math::set_elem(p.inverseViewProjection,3,0,edge.shift*2);
        p.lightDirection={0,0,1,0};p.width=p.height=2;p.quality=edge.quality;
        p.texelSize={1.f/mapSize,1.f/mapSize};p.depthTexelSize={.5f,.5f};p.sunTanHalfAngle=edge.sun;
        for(u32 c=0;c<4;++c){p.lightVP[c]=smath::MatrixIdentity();p.cascade[c]={.001f,1,1,0};}
        command=device.CreateCommandBuffer(CommandQueueType::Graphics);cmd=device.GetCommandBuffer(command);
        Require(cmd&&cmd->Begin(),"edge begin failed");clear(*cmd,depth,.5f);
        Require(shadows.RecordMaps(*cmd,p,cases,[&](DawnCommandBuffer& c){ResourceHandle buffers[]{vertex};u64 offsets[]{0};c.BindVertexBuffers(0,1,buffers,offsets);c.Draw(6,0,1,0);}),"edge raster failed");
        Require(shadows.RecordFilter(*cmd,depth,normal,p,cases),"edge filter failed");
        BufferTextureCopyRegion r{};r.imageExtent={1,1,1};cmd->CopyTextureToBuffer(shadows.Visibility(),readback,&r,1);
        Require(cmd->End(),"edge end failed");submit.cmdBuffer=command;
        Require(device.Submit(submit)&&cmd->WaitForCompletion(),"edge completion failed");
        const auto* bytes=static_cast<const unsigned char*>(device.MapBuffer(readback,0,4));Require(bytes,"edge map failed");
        Near(bytes[0],std::round(edge.visibility*255),1,"penumbra visibility");
        const double centerDepth=.251+.749*std::clamp((.5+double(edge.shift))*mapSize-127.5,0.,1.);
        const double transmission=std::exp(-std::max(.5005-centerDepth,0.)*1.5)*(1-edge.visibility);
        Near(bytes[1],std::round(transmission*255),1,"penumbra transmission");
        device.UnmapBuffer(readback);device.DestroyCommandBuffer(command);++cases;
    }
    device.DestroyBuffer(vertex);

    bool checked=false;WGPUPopErrorScopeCallbackInfo cb=WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    cb.mode=WGPUCallbackMode_AllowProcessEvents;cb.userdata1=&checked;
    cb.callback=[](WGPUPopErrorScopeStatus status,WGPUErrorType type,WGPUStringView,void* data,void*){
        Require(status==WGPUPopErrorScopeStatus_Success&&type==WGPUErrorType_NoError,"Dawn validation scope failed");*static_cast<bool*>(data)=true;};
    wgpuDevicePopErrorScope(device.GetNativeDevice(),cb);const auto start=std::chrono::steady_clock::now();
    while(!checked){wgpuInstanceProcessEvents(device.GetInstance());Require(std::chrono::steady_clock::now()-start<std::chrono::seconds(30),"callback timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    shadows.Shutdown();device.DestroyTexture(depth);device.DestroyTexture(normal);device.DestroyBuffer(readback);device.Shutdown();
    std::printf("{\"passed\":true,\"filter_cases\":%u,\"raster_cascades\":4,\"language\":\"WGSL\"}\n",cases);
}
