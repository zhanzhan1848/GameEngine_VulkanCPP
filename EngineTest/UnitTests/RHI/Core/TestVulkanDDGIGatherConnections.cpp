// Actual production DDGIGIGather geometry-connection contracts; private fixtures only.
#include "../../TestFramework.h"
#include "EngineTest/Utils/TestEnv.h"   // R3: portable setenv/unsetenv (MSVC lacks POSIX ones)
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/Utilities/GeometryFirstHit.h"
#include "Graphics/RenderPipeline/Modules/GIGatherModule.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <algorithm>
#include "DDGIGatherConnectionWitness.h"
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace {

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool finite(float x){return (std::bit_cast<u32>(x)&0x7f800000u)!=0x7f800000u;}
float half(u16 h){
    const u32 sign=u32(h&0x8000)<<16;u32 exp=(h>>10)&31,mant=h&1023;
    if(exp==31)return std::bit_cast<float>(sign|0x7f800000u|(mant<<13));
    if(exp==0){if(!mant)return std::bit_cast<float>(sign);int e=-14;while(!(mant&1024)){mant<<=1;--e;}return std::bit_cast<float>(sign|(u32(e+127)<<23)|((mant&1023)<<13));}
    return std::bit_cast<float>(sign|((exp+112)<<23)|(mant<<13));
}
struct Pixel {float r,g,b,a;};
struct GPU {
    RHIDeviceBase* d=nullptr;VulkanDevice* vk=nullptr;
    std::vector<ResourceHandle> buffers,textures;
    ShaderHandle shaders[2]{handles::INVALID_SHADER,handles::INVALID_SHADER};
    DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;
    PipelineHandle pipes[2]{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    DescriptorSetHandle sets[2]{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};
    GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;d=CreateRHIDevice(desc);require(d!=nullptr,"Create Vulkan device");vk=static_cast<VulkanDevice*>(d);}
    ~GPU(){if(!d)return;for(auto s:sets)if(s!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(s);for(auto p:pipes)if(p!=handles::INVALID_PIPELINE)d->DestroyPipeline(p);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);for(auto s:shaders)if(s!=handles::INVALID_SHADER)d->DestroyShader(s);for(auto t:textures)d->DestroyTexture(t);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
    ResourceHandle buffer(u64 bytes,BufferType type,GPUMemoryUsage mem,const void* data=nullptr){
        BufferDesc bd{};bd.size=bytes;bd.type=type;bd.memoryUsage=mem;bd.name="EnvironmentVisibilityTest";
        auto h=d->CreateBuffer(bd);require(h!=handles::INVALID_RESOURCE,"Create buffer");buffers.push_back(h);if(data)require(d->UpdateBufferData(h,data,bytes,0),"Upload buffer");return h;
    }
    VulkanCommandBuffer* begin(CommandBufferHandle& handle){handle=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(handle);require(c&&c->Reset()&&c->Begin(),"Begin commands");return c;}
    void finish(CommandBufferHandle handle,VulkanCommandBuffer* c){require(c->End()&&c->Submit(0)&&c->WaitForCompletion(),"Submit and complete");d->DestroyCommandBuffer(handle);}
    void barrier(VulkanCommandBuffer* c,ResourceHandle h,ResourceState before,ResourceState after){ResourceBarrier b{};b.resource=h;b.beforeState=before;b.afterState=after;b.subresource=0xffffffffu;b.queueFamily=0xffffffffu;c->InsertBarrier(&b,1);}
    ResourceHandle texture(u32 w,u32 h,DataFormat format,const void* data,u64 bytes){
        TextureDesc td{};td.type=TextureType::Texture2D;td.size={w,h,1};td.format=format;td.mipLevels=1;
        td.usage=TextureUsage::ShaderResource|TextureUsage::CopyDest|TextureUsage::CopySource;
        if(!data)td.usage=td.usage|TextureUsage::UnorderedAccess;
        auto t=d->CreateTexture(td);require(t!=handles::INVALID_RESOURCE,"Create typed texture");textures.push_back(t);
        CommandBufferHandle handle;auto* c=begin(handle);
        if(data){auto staging=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Dynamic,data);BufferTextureCopyRegion region{};region.imageSubresource.layerCount=1;region.imageExtent={w,h,1};c->CopyBufferToTexture(staging,t,&region,1);barrier(c,t,ResourceState::CopyDest,ResourceState::ShaderResource);}
        else barrier(c,t,ResourceState::Unknown,ResourceState::UnorderedAccess);
        finish(handle,c);return t;
    }
    void initializePipelines(){
        const u32 numbers[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,17,18,19,20,23,24,25,26,27};
        std::vector<DescriptorSetLayoutBinding> bindings;
        for(auto n:numbers){DescriptorType t=DescriptorType::StorageBuffer;
            if(n==0)t=DescriptorType::SampledDepthImage;
            else if(n==1||n==3||n==13)t=DescriptorType::SampledImage;
            else if(n==2||n==14)t=DescriptorType::StorageImage;
            else if(n==4||n==5||n==6||n==9||n==18||n==27)t=DescriptorType::UniformBuffer;
            DescriptorSetLayoutBinding b{n,t,1,ShaderStage::Compute,nullptr};if(t==DescriptorType::StorageBuffer)b.readonly=true;bindings.push_back(b);
        }
        sl=d->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"Create actual GG descriptor layout");
        pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"Create GG pipeline layout");
        const char* paths[]={GG_CONNECTION_BASE_SPV,GG_CONNECTION_GEOMETRY_SPV};
        for(u32 p=0;p<2;++p){std::ifstream f(paths[p],std::ios::binary|std::ios::ate);require(bool(f),"Open actual production GG module");auto n=f.tellg();require(n>0,"Nonempty GG SPV");std::vector<u8> bytes(static_cast<size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),n);require(bool(f),"Read GG SPV");shaders[p]=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");require(shaders[p]!=handles::INVALID_SHADER,"Create GG shader");ComputePipelineDesc cd{};cd.layout=pl;cd.computeShader=shaders[p];pipes[p]=d->CreateComputePipeline(cd);require(pipes[p]!=handles::INVALID_PIPELINE,"Create GG pipeline");sets[p]=d->CreateDescriptorSet({sl});require(sets[p]!=handles::INVALID_DESCRIPTOR_SET,"Create GG set");}
    }
    std::vector<Pixel> read(ResourceHandle t,u32 w,u32 h){
        auto rb=buffer(u64(w)*h*8,BufferType::Raw,GPUMemoryUsage::Readback);CommandBufferHandle handle;auto* c=begin(handle);
        barrier(c,t,ResourceState::ShaderResource,ResourceState::CopySource);
        c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        BufferTextureCopyRegion r{};r.imageSubresource.layerCount=1;r.imageExtent={w,h,1};c->CopyTextureToBuffer(t,rb,&r,1);
        c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);finish(handle,c);
        auto* p=static_cast<const u16*>(d->MapBuffer(rb,0,u64(w)*h*8));require(p!=nullptr,"Map visibility readback");std::vector<Pixel> result(w*h);for(u32 i=0;i<w*h;++i)result[i]={half(p[i*4]),half(p[i*4+1]),half(p[i*4+2]),half(p[i*4+3])};d->UnmapBuffer(rb);return result;
    }
};

enum class Geometry {Clear,Closed,Target,Malformed,Cycle,Cloth0,Cloth1,ScanTarget,CenterSheet};
struct Config {
    Geometry geometry=Geometry::Clear;
    bool ready=true,allBright=true,weakSource=false,scan=false;
    float strength=1,mean=100,variance=.25f,originState=1;
    u32 mode=7;
    float jitterX=0,jitterY=0;
    u32 invalidSurface=0;
};
struct Pair {std::vector<Pixel> diffuse,specular;};
struct Result {Pair baseline,candidate;};
struct ViewCB {float invVP[16],origin[4],counts[4],camera[4],debug[4];};
struct VolumeCB {float origin[4][4],counts[4][4],meta[4];};
static_assert(sizeof(ViewCB)==128&&offsetof(ViewCB,counts)==80&&offsetof(ViewCB,camera)==96);
static_assert(sizeof(VolumeCB)==144);

ray_geometry::SceneBVH buildGeometry(const Config& cfg){
    RHIMeshAsset mesh{};std::vector<float> v;std::vector<u32> indices;
    auto triangle=[&](std::array<float,3>a,std::array<float,3>b,std::array<float,3>c){u32 first=v.size()/3;for(auto p:{a,b,c})v.insert(v.end(),p.begin(),p.end());indices.insert(indices.end(),{first,first+1,first+2});};
    auto quad=[&](std::array<float,3>a,std::array<float,3>b,std::array<float,3>c,std::array<float,3>d){triangle(a,b,c);triangle(a,c,d);};
    if(cfg.geometry==Geometry::Cloth0||cfg.geometry==Geometry::Cloth1){
        auto* t=GatherWitness::Triangle;triangle({t[0][0],t[0][1],t[0][2]},{t[1][0],t[1][1],t[1][2]},{t[2][0],t[2][1],t[2][2]});
    }else if(cfg.geometry==Geometry::Closed||cfg.geometry==Geometry::Malformed||cfg.geometry==Geometry::Cycle){
        constexpr float a=-.25f,b=.25f,z=.35f,t=.85f;
        quad({a,a,z},{b,a,z},{b,b,z},{a,b,z});quad({a,a,t},{a,b,t},{b,b,t},{b,a,t});
        quad({a,a,z},{a,b,z},{a,b,t},{a,a,t});quad({b,a,z},{b,a,t},{b,b,t},{b,b,z});
        quad({a,a,z},{a,a,t},{b,a,t},{b,a,z});quad({a,b,z},{b,b,z},{b,b,t},{a,b,t});
    }else if(cfg.geometry==Geometry::Target){quad({.35f,.35f,.8f},{.65f,.35f,.8f},{.65f,.65f,.8f},{.35f,.65f,.8f});}
    else if(cfg.geometry==Geometry::CenterSheet){quad({.01f,-10,-10},{.01f,10,-10},{.01f,10,10},{.01f,-10,10});}
    else if(cfg.geometry==Geometry::ScanTarget){quad({-.1f,.35f,.75f},{.25f,.35f,.75f},{.25f,.65f,.75f},{-.1f,.65f,.75f});}
    else quad({-10,-10,-4},{10,-10,-4},{10,10,-4},{-10,10,-4});
    mesh.num_vertices=v.size()/3;mesh.num_indices=indices.size();mesh.index_size=4;mesh.position_buffer.resize(v.size()*4);mesh.index_buffer.resize(indices.size()*4);
    std::memcpy(mesh.position_buffer.data(),v.data(),v.size()*4);std::memcpy(mesh.index_buffer.data(),indices.data(),indices.size()*4);
    auto blas=ray_geometry::BuildMeshBLAS(mesh);require(bool(blas),"Build real GG fixture BLAS");math::m4x4 m{};m.columns[0]={1,0,0,0};m.columns[1]={0,1,0,0};m.columns[2]={0,0,1,0};m.columns[3]={0,0,0,1};
    std::array<ray_geometry::InstanceInput,1> input{{{blas,m,324}}};ray_geometry::SceneBVH out;require(out.Build(input),"Build real GG fixture TLAS");
    if(cfg.geometry==Geometry::Malformed)out.top[0].links[2]=0xffffffffu;
    if(cfg.geometry==Geometry::Cycle){out.top[0].links[0]=out.top[0].links[1]=0;out.top[0].links[3]=0;}
    return out;
}
Result dispatch(const Config& cfg){
    GPU g;g.initializePipelines();const u32 width=cfg.scan?5:1,height=1;
    const bool cloth=cfg.geometry==Geometry::Cloth0||cfg.geometry==Geometry::Cloth1;const u32 witness=cfg.geometry==Geometry::Cloth1?1:0;
    float p[3]={0,0,.5f},n[3]={0,0,1};float spacing=2;
    if(cfg.scan){p[0]=-.9466666667f;n[0]=1;n[2]=0;}
    if(cloth){for(u32 k=0;k<3;++k){p[k]=GatherWitness::SurfaceReceivers[witness][k];n[k]=GatherWitness::Normals[witness][k];}spacing=3;}
    ViewCB view{};view.invVP[0]=cfg.scan?.0026666667f:.04f;view.invVP[5]=.04f;view.invVP[10]=view.invVP[15]=1;
    view.invVP[12]=p[0];view.invVP[13]=p[1];view.invVP[14]=p[2]-.5f;
    if(cloth){
        // Map this tiny plane to the archived real surface P, with depth Ng +Y.
        // Full-res selected texel(1,1) has NDC(.5,-.5). Legacy interpolation
        // remains at output UV center, intentionally a different point.
        std::fill(std::begin(view.invVP),std::end(view.invVP),0.f);
        view.invVP[2]=.02f;view.invVP[4]=.02f;view.invVP[9]=1;view.invVP[15]=1;
        view.invVP[12]=p[0]+.01f;view.invVP[13]=p[1]-.5f;view.invVP[14]=p[2]-.01f;
    }
    if(cfg.invalidSurface==3)view.invVP[5]=0;
    for(u32 k=0;k<3;++k){view.origin[k]=cloth?p[k]-spacing*.5f:-1;view.counts[k]=2;view.camera[k]=p[k]+n[k]*5;}
    view.origin[3]=spacing;view.camera[3]=float(cfg.mode);
    VolumeCB volume{};for(u32 k=0;k<4;++k)volume.origin[0][k]=view.origin[k];volume.counts[0][0]=volume.counts[0][1]=volume.counts[0][2]=2;volume.meta[0]=1;
    std::array<float,8*9*4> sh{};std::array<float,8*128> moments{};std::array<float,8*4> origins{},confidence{};std::array<float,16> open{};
    for(u32 i=0;i<8;++i){
        for(u32 k=0;k<3;++k)sh[i*36+k]=cfg.allBright?cfg.strength:0;
        for(u32 c=0;c<64;++c){moments[i*128+c]=cfg.mean;moments[i*128+64+c]=cfg.variance;}
        origins[i*4]=view.origin[0]+float(i&1u)*spacing;origins[i*4+1]=view.origin[1]+float((i>>1u)&1u)*spacing;origins[i*4+2]=view.origin[2]+float((i>>2u)&1u)*spacing;origins[i*4+3]=cfg.originState;
        confidence[i*4+2]=confidence[i*4+3]=1;
    }
    if(!cfg.allBright)for(u32 k=0;k<3;++k)sh[7*36+k]=cfg.strength;
    if(cfg.weakSource)for(u32 k=0;k<3;++k)sh[k]=1;
    if(cloth){for(u32 k=0;k<3;++k)origins[7*4+k]=GatherWitness::Anchor[k];for(u32 j=0;j<9;++j)for(u32 k=0;k<3;++k)sh[7*36+j*4+k]=GatherWitness::SH[j][k];}
    auto geometry=buildGeometry(cfg);
    if(cloth||cfg.geometry==Geometry::Target||cfg.geometry==Geometry::Closed||cfg.geometry==Geometry::Clear){
        float origin[3],dir[3];float dist=0;for(u32 k=0;k<3;++k){origin[k]=(cloth?GatherWitness::Receivers[witness][k]:p[k])+n[k]*spacing*.05f;dir[k]=origins[7*4+k]-origin[k];dist+=dir[k]*dir[k];}dist=std::sqrt(dist);for(float& v:dir)v/=dist;ray_geometry::Hit hit;
        require(ray_geometry::Trace(geometry,origin,dir,.001f,dist-.001f,hit)==(cfg.geometry==Geometry::Clear?0:1),"Historical Ns-biased diagnostic segment oracle");
        if(cloth){float d2=0;for(u32 k=0;k<3;++k){origin[k]=p[k]+(k==1?.002f:0.f);dir[k]=origins[7*4+k]-origin[k];d2+=dir[k]*dir[k];}d2=std::sqrt(d2);for(float& v:dir)v/=d2;require(ray_geometry::Trace(geometry,origin,dir,0.f,d2,hit)==0,"Actual archived surface+depthNg2mm must be clear, unlike historical Ns15cm");}
    }
    std::vector<float> depths(width*2*height*2,.5f);std::vector<std::array<float,4>> normals(depths.size(),{n[0]*.5f+.5f,n[1]*.5f+.5f,n[2]*.5f+.5f,1}),orm(depths.size(),{1,1,0,1});
    if(cfg.invalidSurface==1)depths[width*2]=std::numeric_limits<float>::quiet_NaN(); // Only available X neighbor.
    if(cfg.invalidSurface==2){depths[width*2]=.9f;depths[1]=.9f;}
    const std::array<u16,4> black{};
    auto depth=g.texture(width*2,height*2,DataFormat::D32_Float,depths.data(),depths.size()*4),normal=g.texture(width*2,height*2,DataFormat::RGBA32_Float,normals.data(),normals.size()*16),material=g.texture(width*2,height*2,DataFormat::RGBA32_Float,orm.data(),orm.size()*16),history=g.texture(1,1,DataFormat::RGBA16_Float,black.data(),8);
    ResourceHandle diffuse[2],spec[2];for(u32 i=0;i<2;++i){diffuse[i]=g.texture(width,height,DataFormat::RGBA16_Float,nullptr,0);spec[i]=g.texture(width,height,DataFormat::RGBA16_Float,nullptr,0);}
    // R11/VUID-00327: bindings 5/6 sub-allocate one 128B constant block at
    // legacy offsets 64/80. minUniformBufferOffsetAlignment is device
    // dependent (NVIDIA desktop 256 — GTX1060 十测实测 0x100；MoltenVK 16)，
    // so pack the three slices at runtime-aligned offsets instead of the
    // fixed 64/80 layout that only survives align<=16 devices.
    VkPhysicalDeviceProperties devProps{};vkGetPhysicalDeviceProperties(g.vk->GetNativePhysicalDevice(),&devProps);
    const u64 uboAlign=std::max<u64>(devProps.limits.minUniformBufferOffsetAlignment,1);
    auto alignUbo=[uboAlign](u64 v){return (v+uboAlign-1)/uboAlign*uboAlign;};
    const u64 originOff=alignUbo(64),tailOff=alignUbo(originOff+16);
    std::vector<u8> viewPadded(size_t(tailOff+48),0);
    std::memcpy(viewPadded.data(),&view.invVP,sizeof(view.invVP));
    std::memcpy(viewPadded.data()+originOff,&view.origin,sizeof(view.origin));
    std::memcpy(viewPadded.data()+tailOff,&view.counts,sizeof(view.counts)+sizeof(view.camera)+sizeof(view.debug));
    auto viewBuffer=g.buffer(viewPadded.size(),BufferType::Constant,GPUMemoryUsage::Dynamic,viewPadded.data()),volumeBuffer=g.buffer(sizeof(volume),BufferType::Constant,GPUMemoryUsage::Dynamic,&volume);
    std::array<u8,176> staticProbe{};std::array<float,36> staticSH{};std::array<float,8> staticFactor{};
    auto staticCB=g.buffer(sizeof(staticProbe),BufferType::Constant,GPUMemoryUsage::Dynamic,staticProbe.data()),sky=g.buffer(sizeof(staticSH),BufferType::Raw,GPUMemoryUsage::Dynamic,staticSH.data()),skyFactor=g.buffer(sizeof(staticFactor),BufferType::Raw,GPUMemoryUsage::Dynamic,staticFactor.data());
    auto nearSH=g.buffer(sizeof(sh),BufferType::Raw,GPUMemoryUsage::Dynamic,sh.data()),depthBuffer=g.buffer(sizeof(moments),BufferType::Raw,GPUMemoryUsage::Dynamic,moments.data()),originBuffer=g.buffer(sizeof(origins),BufferType::Raw,GPUMemoryUsage::Dynamic,origins.data()),conf=g.buffer(sizeof(confidence),BufferType::Raw,GPUMemoryUsage::Dynamic,confidence.data());
    std::vector<u8> flat(256+sizeof(sh),0xcd),flatOpen(256+sizeof(open),0xcd);std::memcpy(flat.data()+256,sh.data(),sizeof(sh));std::memcpy(flatOpen.data()+256,open.data(),sizeof(open));
    auto flatBuffer=g.buffer(flat.size(),BufferType::Raw,GPUMemoryUsage::Dynamic,flat.data()),openBuffer=g.buffer(flatOpen.size(),BufferType::Raw,GPUMemoryUsage::Dynamic,flatOpen.data());
    struct Policy {u32 values[4];float jitter[4];};static_assert(sizeof(Policy)==32);
    const Policy policy{{cfg.ready?1u:0u,13,1,0},{cfg.jitterX,cfg.jitterY,0,0}};auto policyBuffer=g.buffer(sizeof(policy),BufferType::Constant,GPUMemoryUsage::Dynamic,&policy);
    ResourceHandle geometryBuffers[]={g.buffer(geometry.top.size()*sizeof(ray_geometry::Node),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.top.data()),g.buffer(geometry.bottom.size()*sizeof(ray_geometry::Node),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.bottom.data()),g.buffer(geometry.triangles.size()*sizeof(ray_geometry::Triangle),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.triangles.data()),g.buffer(geometry.instances.size()*sizeof(ray_geometry::Instance),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.instances.data())};
    for(u32 phase=0;phase<2;++phase){
        DescriptorImageInfo images[6]={{handles::INVALID_SAMPLER,depth,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,normal,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,diffuse[phase],ResourceState::UnorderedAccess},{handles::INVALID_SAMPLER,history,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,material,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,spec[phase],ResourceState::UnorderedAccess}};
        const u32 imageBindings[]={0,1,2,3,13,14};std::vector<WriteDescriptorSet> writes;
        for(u32 i=0;i<6;++i)writes.push_back({g.sets[phase],imageBindings[i],0,1,i==0?DescriptorType::SampledDepthImage:(i==2||i==5?DescriptorType::StorageImage:DescriptorType::SampledImage),&images[i],nullptr});
        const u32 bindings[]={4,5,6,7,8,9,10,11,12,17,18,19,20,23,24,25,26,27};
        DescriptorBufferInfo buffers[]={{viewBuffer,0,64},{viewBuffer,originOff,16},{viewBuffer,tailOff,48},{sky,0,sizeof(staticSH)},{skyFactor,0,sizeof(staticFactor)},{staticCB,0,sizeof(staticProbe)},{conf,0,sizeof(confidence)},{nearSH,0,sizeof(sh)},{depthBuffer,0,sizeof(moments)},{flatBuffer,256,sizeof(sh)},{volumeBuffer,0,sizeof(volume)},{originBuffer,0,sizeof(origins)},{openBuffer,256,sizeof(open)},
            {geometryBuffers[0],0,geometry.top.size()*sizeof(ray_geometry::Node)},{geometryBuffers[1],0,geometry.bottom.size()*sizeof(ray_geometry::Node)},{geometryBuffers[2],0,geometry.triangles.size()*sizeof(ray_geometry::Triangle)},{geometryBuffers[3],0,geometry.instances.size()*sizeof(ray_geometry::Instance)},{policyBuffer,0,sizeof(policy)}};
        for(u32 i=0;i<18;++i){u32 n=bindings[i];auto type=n==4||n==5||n==6||n==9||n==18||n==27?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;writes.push_back({g.sets[phase],n,0,1,type,nullptr,&buffers[i]});}
        g.d->UpdateDescriptorSets(writes.size(),writes.data());CommandBufferHandle handle;auto* c=g.begin(handle);
        c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);c->BindComputePipeline(g.pipes[phase]);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[phase],0,nullptr);c->Dispatch((width+7)/8,(height+7)/8,1);
        g.barrier(c,diffuse[phase],ResourceState::UnorderedAccess,ResourceState::ShaderResource);g.barrier(c,spec[phase],ResourceState::UnorderedAccess,ResourceState::ShaderResource);g.finish(handle,c);
    }
    Result out;out.baseline={g.read(diffuse[0],width,height),g.read(spec[0],width,height)};out.candidate={g.read(diffuse[1],width,height),g.read(spec[1],width,height)};
    for(const auto* pair:{&out.baseline,&out.candidate})for(const auto* data:{&pair->diffuse,&pair->specular})for(auto p:*data)for(float x:{p.r,p.g,p.b,p.a})require(finite(x),"Actual GG diffuse/glossy RGBA finite");
    return out;
}
void zero(const Pair& x,bool unsupported){for(const auto* image:{&x.diffuse,&x.specular})for(auto p:*image){require(p.r==0&&p.g==0&&p.b==0,"Blocked/unknown must have exact zero diffuse/glossy");if(unsupported)require(p.a==0,"No eligible support: diffuse/glossy alpha zero");}}
void identical(const Result& x){for(auto pair:{std::pair{&x.baseline.diffuse,&x.candidate.diffuse},std::pair{&x.baseline.specular,&x.candidate.specular}})require(pair.first->size()==pair.second->size()&&std::memcmp(pair.first->data(),pair.second->data(),pair.first->size()*sizeof(Pixel))==0,"Clear or legacy actual GPU outputs remain bit-identical");}
struct ConnectionEnvironment {
    bool present=false;std::string original;
    ConnectionEnvironment(){if(const char* p=std::getenv("PRIMAL_DDGI_GEOMETRY_CONNECTIONS")){present=true;original=p;}primaltest::SetEnv("PRIMAL_DDGI_GEOMETRY_CONNECTIONS","0");}
    ~ConnectionEnvironment(){if(present)primaltest::SetEnv("PRIMAL_DDGI_GEOMETRY_CONNECTIONS",original.c_str());else primaltest::UnsetEnv("PRIMAL_DDGI_GEOMETRY_CONNECTIONS");}
};
struct OwnedGather {GIGatherModule value;~OwnedGather(){value.Shutdown();}};
void registerLifecycleCases(const std::shared_ptr<TestSuite>& suite){
    auto add=[&](const char* name,int scenario){suite->AddTestCase(TestCase(name,[=]{
        ConnectionEnvironment env;GPU g;
        std::ifstream f(GG_CONNECTION_BASE_SPV,std::ios::binary|std::ios::ate);require(bool(f),"Open lifecycle baseline SPV");auto n=f.tellg();require(n>0,"Nonempty lifecycle shader");std::vector<u8> bytes(static_cast<size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),n);require(bool(f),"Read lifecycle shader");
        g.shaders[0]=g.d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");require(g.shaders[0]!=handles::INVALID_SHADER,"Create lifecycle baseline shader");
        OwnedGather owner;auto& m=owner.value;
        require(m.Initialize(g.d,g.shaders[0],16,16),"Baseline module Initialize succeeds");require(m.IsInitialized()&&!m.RequiresGeometryConnections(),"Baseline module live without candidate");
        if(scenario==1){
            primaltest::SetEnv("PRIMAL_DDGI_GEOMETRY_CONNECTIONS","1");
            require(m.Initialize(g.d,g.shaders[0],16,16),"Reinitialize candidate from production geometry SPV succeeds");
            require(m.IsInitialized()&&m.RequiresGeometryConnections(),"Exact-1 candidate initialized after baseline cleanup");
        }else if(scenario==2){
            require(!m.Initialize(g.d,handles::INVALID_SHADER,16,16),"Invalid shader reinitialize must fail");
            require(!m.IsInitialized(),"Failed reinitialize cannot retain old pipeline");
        }
        m.Shutdown();require(!m.IsInitialized()&&!m.RequiresGeometryConnections(),"Shutdown clears initialized and candidate state");
        m.Shutdown();require(!m.IsInitialized(),"Repeated Shutdown remains uninitialized");
        std::cout<<"{\"host_lifecycle\":\""<<name<<"\",\"passed\":true}\n";
        return TestResult::Passed;
    }));};
    add("host_initialize_shutdown_twice",0);
    add("host_baseline_reinitialize_exact1_candidate",1);
    add("host_invalid_shader_reinitialize_discards_old_pipeline",2);
}
void registerCases(const std::shared_ptr<TestSuite>& suite){
    auto add=[&](const std::string& name,Config cfg,auto validate){suite->AddTestCase(TestCase(name,[=]{auto r=dispatch(cfg);std::cout<<"{\"case\":\""<<name<<"\",\"baseline_r\":"<<r.baseline.diffuse[0].r<<",\"candidate_r\":"<<r.candidate.diffuse[0].r<<"}\n";validate(r);return TestResult::Passed;}));};
    Config c;add("clear_preserves_moments",c,[](const Result&r){identical(r);require(r.candidate.diffuse[0].r>0,"Known clear retains measured light");});
    c.originState=.5f;add("clear_borrowed_half_weight_preserved",c,[](const Result&r){identical(r);});
    c={};c.mean=0;c.variance=0;add("clear_does_not_override_blocking_moment",c,[](const Result&r){identical(r);zero(r.candidate,true);});
    c.mean=-1;add("clear_does_not_override_unknown_moment",c,[](const Result&r){identical(r);zero(r.candidate,true);});
    c.mean=std::numeric_limits<float>::quiet_NaN();add("clear_nonfinite_moment_unknown",c,[](const Result&r){identical(r);zero(r.candidate,true);});
    c={};c.geometry=Geometry::Closed;add("opaque_all_connections_blocked",c,[](const Result&r){require(r.baseline.diffuse[0].r>0,"Old moment path is a positive control");zero(r.candidate,true);});
    c.geometry=Geometry::Target;c.allBright=false;c.strength=100;add("strong_blocked_source_cannot_leak_through_valid_black_neighbors",c,[](const Result&r){require(r.baseline.diffuse[0].r>0,"Strong-source positive control");zero(r.candidate,false);require(r.candidate.diffuse[0].a==1,"Clear black neighbors remain valid support");});
    c.weakSource=true;add("blocked_weight_not_erased_from_confidence_denominator",c,[](const Result&r){constexpr float expected=.28209479f*3.14159265f*.025f;require(std::abs(r.candidate.diffuse[0].r-expected)<.000031f,"Analytic L0 clear contribution includes original coverage denominator");});
    c={};c.ready=false;add("geometry_not_ready_is_unknown",c,[](const Result&r){require(r.baseline.diffuse[0].r>0,"Positive moment baseline");zero(r.candidate,true);});
    c={};c.geometry=Geometry::Malformed;add("malformed_geometry_cannot_be_clear",c,[](const Result&r){zero(r.candidate,true);});
    c.geometry=Geometry::Cycle;add("cyclic_geometry_cannot_be_clear",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.originState=0;add("invalid_anchor_no_bright_fallback",c,[](const Result&r){zero(r.candidate,true);});
    for(u32 i=0;i<2;++i){c={};c.geometry=i?Geometry::Cloth1:Geometry::Cloth0;c.allBright=false;add(i?"surface_cloth_1018_to_410_clear":"surface_cloth_1017_to_410_clear",c,[](const Result&r){require(r.baseline.diffuse[0].r>0,"Witness SH source positive under favorable moments");identical(r);require(r.candidate.diffuse[0].r>0,"True surface clear preserves favorable moment light");});}
    c={};c.scan=true;c.allBright=false;c.strength=100;c.geometry=Geometry::ScanTarget;add("blocked_connection_zero_across_removed_tw_gate",c,[](const Result&r){require(r.baseline.diffuse.back().r>.5f,"Above-cutoff positive control");zero(r.candidate,false);});
    c.geometry=Geometry::Clear;add("clear_positive_tw_continuous_across_old_cutoff",c,[](const Result&r){
        require(r.baseline.diffuse.front().r==0&&r.baseline.diffuse.back().r>.5f,"Old cutoff remains the negative control");
        for(u32 i=0;i<5;++i){const float ndc=(float(i)+.5f)/5.f*2.f-1.f;const float x=-.9466666667f+.0026666667f*ndc;const float tw=(x+1.f)*.5f*.5f*.75f;const float expected=.28209479f*3.14159265f*100.f*tw;require(std::abs(r.candidate.diffuse[i].r-expected)<.001f,"All positive clear tw retains independent L0 trilinear value within two half ULP");if(i)require(r.candidate.diffuse[i].r>r.candidate.diffuse[i-1].r,"Clear positive trilinear sweep must remain continuous and monotonic");}
    });
    c={};c.geometry=Geometry::CenterSheet;c.allBright=false;add("physical_fullres_texel_center_clear_not_output_uv",c,[](const Result&r){identical(r);require(r.candidate.diffuse[0].r>0,"Selected full-res texel center must be on clear side of thin sheet");});
    c.jitterX=.75f;c.jitterY=.25f;add("actual_nonzero_XY_jitter_moves_physical_ray_to_blocked_side",c,[](const Result&r){require(r.baseline.diffuse[0].r>0,"Legacy interpolation remains positive under physical jitter diagnostic");zero(r.candidate,false);});
    c={};c.jitterX=std::numeric_limits<float>::quiet_NaN();add("nonfinite_actual_jitter_is_unknown",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.invalidSurface=1;add("missing_valid_depth_neighbor_axis_is_unknown",c,[](const Result&r){zero(r.candidate,true);});
    c.invalidSurface=2;add("depth_neighborhood_large_jump_is_unknown",c,[](const Result&r){zero(r.candidate,true);});
    c.invalidSurface=3;add("degenerate_depth_Ng_is_unknown",c,[](const Result&r){zero(r.candidate,true);});
    for(u32 mode=0;mode<7;++mode){c={};c.mode=mode;c.ready=false;c.geometry=Geometry::Closed;add("legacy_mode_"+std::to_string(mode)+"_unchanged",c,[](const Result&r){identical(r);});}
}
}
int main(){auto s=std::make_shared<TestSuite>("VulkanDDGIGatherConnections");registerCases(s);registerLifecycleCases(s);TestRunner::RegisterTestSuite(s);const auto stats=TestRunner::RunAllSuites();return stats.totalTests==32&&stats.passedTests==32&&stats.failedTests==0&&stats.skippedTests==0?0:1;}
