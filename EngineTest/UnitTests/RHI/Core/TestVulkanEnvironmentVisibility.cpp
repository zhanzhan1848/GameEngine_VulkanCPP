// Direct GPU contracts for production EnvironmentVisibility trace and resolve.
#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/Utilities/GeometryFirstHit.h"
#include "Graphics/Utilities/ExperimentalSAHMesh.h"
#include "Graphics/RenderPipeline/Modules/ExperimentalSAHState.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <cstdio>
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
struct CB {float invVP[16];float camera[4];u32 dimensions[4];u32 policy[4];float parameters[4];u32 target[4];float prevVP[16];};
static_assert(sizeof(CB)==208&&offsetof(CB,camera)==64&&offsetof(CB,dimensions)==80&&offsetof(CB,policy)==96&&offsetof(CB,parameters)==112&&offsetof(CB,target)==128&&offsetof(CB,prevVP)==144);
struct Pixel {float r,g,b,a;};
#if defined(ENV_VIS_WG32_TEST) || defined(ENV_VIS_PAIR_TEST)
struct ShaderIdentity {u64 hash=14695981039346656037ull;u32 x=0,y=0,z=0;};
ShaderIdentity shaderIdentity(const std::vector<u8>& bytes) {
    require(bytes.size()>=20 && bytes.size()%4==0,"SPIR-V header and word alignment");
    std::vector<u32> words(bytes.size()/4);std::memcpy(words.data(),bytes.data(),bytes.size());
    require(words[0]==0x07230203u,"SPIR-V magic");ShaderIdentity result;
    for(u8 b:bytes){result.hash^=b;result.hash*=1099511628211ull;}
    bool found=false;
    for(size_t i=5;i<words.size();) {
        const u32 count=words[i]>>16,op=words[i]&0xffffu;
        require(count && i+count<=words.size(),"Valid SPIR-V instruction boundary");
        if(op==16 && count==6 && words[i+2]==17) {
            require(!found,"One literal LocalSize");found=true;result.x=words[i+3];result.y=words[i+4];result.z=words[i+5];
        }
        i+=count;
    }
    require(found,"Literal SPIR-V LocalSize exists");return result;
}
#endif
bool useSAH(){const char* value=std::getenv("ENV_VIS_TEST_SAH");return value&&std::strcmp(value,"1")==0;}
void buildScene(std::span<const ray_geometry::InstanceInput> inputs,ray_geometry::SceneBVH& out){
    if(useSAH()){
        const char* failure=nullptr;u64 digest=0;
        if(!ray_geometry::BuildExperimentalSAHScene(inputs,out,&failure,&digest)){
            std::cerr<<"SAH fixture build failed: "<<(failure?failure:"unknown")<<" mesh_digest="<<digest<<'\n';
            throw std::runtime_error("Build complete experimental SAH fixture scene");
        }
    }else require(out.Build(inputs),"Build complete original fixture scene");
}
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
    void finish(CommandBufferHandle handle,VulkanCommandBuffer* c){auto fail=[](const char* op){std::fprintf(stderr,"[ENV_VIS_FIXTURE] %s failed; no cleanup of potentially pending work\n",op);std::fflush(stderr);std::_Exit(7);};if(!c->End())fail("End");if(!c->Submit(0))fail("Submit");if(!c->WaitForCompletion())fail("Wait");d->DestroyCommandBuffer(handle);}
    void barrier(VulkanCommandBuffer* c,ResourceHandle h,ResourceState before,ResourceState after){ResourceBarrier b{};b.resource=h;b.beforeState=before;b.afterState=after;b.subresource=0xffffffffu;b.queueFamily=0xffffffffu;c->InsertBarrier(&b,1);}
    ResourceHandle texture(u32 w,u32 h,DataFormat format,const void* data,u64 bytes,bool output=false){
        TextureDesc td{};td.type=TextureType::Texture2D;td.size={w,h,1};td.format=format;td.mipLevels=1;
        td.usage=TextureUsage::ShaderResource|TextureUsage::CopyDest|TextureUsage::CopySource;
        if(!data||output)td.usage=td.usage|TextureUsage::UnorderedAccess;
        auto t=d->CreateTexture(td);require(t!=handles::INVALID_RESOURCE,"Create typed texture");textures.push_back(t);
        CommandBufferHandle handle;auto* c=begin(handle);
        if(data){auto staging=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Dynamic,data);BufferTextureCopyRegion region{};region.imageSubresource.layerCount=1;region.imageExtent={w,h,1};
            if(output)c->MemoryBarrier(PipelineStage::Host,PipelineStage::Transfer,AccessFlag::HostWrite,AccessFlag::TransferRead);
            c->CopyBufferToTexture(staging,t,&region,1);
            if(output)c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,AccessFlag::TransferWrite,AccessFlag::ShaderWrite|AccessFlag::ShaderRead);
            barrier(c,t,ResourceState::CopyDest,output?ResourceState::UnorderedAccess:ResourceState::ShaderResource);}
        else barrier(c,t,ResourceState::Unknown,ResourceState::UnorderedAccess);
        finish(handle,c);return t;
    }
    void initializePipelines(bool candidate=false){
        DescriptorSetLayoutBinding bindings[10]={
            {0,DescriptorType::SampledDepthImage,1,ShaderStage::Compute,nullptr},
            {1,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},
            {2,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},
            {3,DescriptorType::StorageImage,1,ShaderStage::Compute,nullptr},
            {4,DescriptorType::UniformBuffer,1,ShaderStage::Compute,nullptr},
            {5,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},
            {13,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {14,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {15,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {16,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr}};
        for(u32 i=6;i<10;++i)bindings[i].readonly=true;
        sl=d->CreateDescriptorSetLayout({10,bindings});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"Create matching descriptor layout");
        pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"Create pipeline layout");
        const char* paths[]={ENV_VIS_TEST_TRACE_SPV,ENV_VIS_TEST_RESOLVE_SPV};
#if defined(ENV_VIS_WG32_TEST) || defined(ENV_VIS_PAIR_TEST)
#ifdef ENV_VIS_PAIR_TEST
        if(candidate)paths[0]=ENV_VIS_PAIR_SPV;
#else
        if(candidate)paths[0]=ENV_VIS_WG32_SPV;
#endif
#else
        (void)candidate;
#endif
        std::string stagePaths[2];
#if defined(ENV_VIS_WG32_TEST) || defined(ENV_VIS_PAIR_TEST)
        require(std::getenv("ENV_VIS_TEST_SHADER_DIRECTORY")==nullptr,"WG32 comparison forbids shared shader directory override");
#endif
        if(const char* dir=std::getenv("ENV_VIS_TEST_SHADER_DIRECTORY")){
            stagePaths[0]=std::string(dir)+"/EnvironmentVisibility_trace.comp.spv";
            stagePaths[1]=std::string(dir)+"/EnvironmentVisibility_resolve.comp.spv";
            paths[0]=stagePaths[0].c_str();paths[1]=stagePaths[1].c_str();
        }
        for(u32 p=0;p<2;++p){std::ifstream f(paths[p],std::ios::binary|std::ios::ate);require(bool(f),"Open private production SPV");auto n=f.tellg();require(n>0,"Nonempty SPV");std::vector<u8> bytes(static_cast<size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),n);require(bool(f),"Read SPV");
#if defined(ENV_VIS_WG32_TEST) || defined(ENV_VIS_PAIR_TEST)
            const auto id=shaderIdentity(bytes);
#ifdef ENV_VIS_PAIR_TEST
            require(id.x==8 && id.y==8 && id.z==1,"pair and reference local sizes are 8x8");
#else
            require(id.x==8 && id.y==(p==0 && candidate?4u:8u) && id.z==1,"Actual shader LocalSize matches dispatch");
#endif
            static u64 referenceTrace=0,referenceResolve=0;
            if(p==0 && !candidate) {if(referenceTrace)require(referenceTrace==id.hash,"Stable reference trace hash");else referenceTrace=id.hash;}
            if(p==0 && candidate)require(referenceTrace && referenceTrace!=id.hash,"Candidate trace differs from reference");
            if(p==1) {if(referenceResolve)require(referenceResolve==id.hash,"Same original resolve in both modes");else referenceResolve=id.hash;}
            std::cout << "[ENV_VIS_WG32_SHADER] mode=" << (candidate?"candidate":"reference") << " stage=" << (p?"resolve":"trace")
                << " path=" << paths[p] << " fnv1a64=" << id.hash << " local_x=" << id.x << " local_y=" << id.y << " local_z=" << id.z << '\n';
#endif
            shaders[p]=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");require(shaders[p]!=handles::INVALID_SHADER,"Create production shader");ComputePipelineDesc cd{};cd.layout=pl;cd.computeShader=shaders[p];pipes[p]=d->CreateComputePipeline(cd);require(pipes[p]!=handles::INVALID_PIPELINE,"Create production pipeline");sets[p]=d->CreateDescriptorSet({sl});require(sets[p]!=handles::INVALID_DESCRIPTOR_SET,"Create descriptor set");}
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

ray_geometry::SceneBVH scene(bool closed,bool biasSheet=false,u32 extra=0){
    RHIMeshAsset mesh{};std::vector<float> vertices;std::vector<u32> indices;
    auto quad=[&](std::array<float,3>a,std::array<float,3>b,std::array<float,3>c,std::array<float,3>e){const u32 base=vertices.size()/3;for(auto v:{a,b,c,e})vertices.insert(vertices.end(),v.begin(),v.end());for(auto i:{0u,1u,2u,0u,2u,3u})indices.push_back(base+i);};
    if(closed){
        const float l=-1,r=1,z=-1,t=1.5;
        quad({l,l,z},{r,l,z},{r,r,z},{l,r,z});quad({l,l,t},{l,r,t},{r,r,t},{r,l,t});
        quad({l,l,z},{l,r,z},{l,r,t},{l,l,t});quad({r,l,z},{r,l,t},{r,r,t},{r,r,z});
        quad({l,l,z},{l,l,t},{r,l,t},{r,l,z});quad({l,r,z},{r,r,z},{r,r,t},{l,r,t});
    }else{quad({-10,-10,-1},{10,-10,-1},{10,10,-1},{-10,10,-1});if(biasSheet)quad({-10,-10,.50175f},{10,-10,.50175f},{10,10,.50175f},{-10,10,.50175f});}
    if(extra==1)quad({.002f,-.004f,1},{.004f,-.004f,1},{.004f,-.002f,1},{.002f,-.002f,1});
    if(extra==2)quad({-10,-10,.502f},{10,-10,.502f},{10,10,.502f},{-10,10,.502f});
    mesh.num_vertices=vertices.size()/3;mesh.num_indices=indices.size();mesh.index_size=4;
    mesh.position_buffer.resize(vertices.size()*4);std::memcpy(mesh.position_buffer.data(),vertices.data(),vertices.size()*4);
    mesh.index_buffer.resize(indices.size()*4);std::memcpy(mesh.index_buffer.data(),indices.data(),indices.size()*4);
    auto blas=ray_geometry::BuildMeshBLAS(mesh);require(bool(blas),"Build actual mesh BLAS");math::m4x4 identity{};identity.columns[0]={1,0,0,0};identity.columns[1]={0,1,0,0};identity.columns[2]={0,0,1,0};identity.columns[3]={0,0,0,1};
    std::array<ray_geometry::InstanceInput,1> in{{{blas,identity,17}}};ray_geometry::SceneBVH out;buildScene(in,out);
    // Independent CPU geometry check establishes the synthetic geometry's role,
    // not a copied VNDF/reflection oracle.
    if(!biasSheet&&!extra){float origin[]={0,0,.502f},direction[]={0,0,1};ray_geometry::Hit hit;require(ray_geometry::Trace(out,origin,direction,0,100,hit)==(closed?1:0),"Synthetic geometry forward hit/miss");}
    return out;
}
ray_geometry::SceneBVH stackedScene(bool farFirst){
    RHIMeshAsset mesh{};const float positions[][3]={{-10,-10,0},{10,-10,0},{10,10,0},{-10,10,0}};const u32 indices[]={0,1,2,0,2,3};
    mesh.num_vertices=4;mesh.num_indices=6;mesh.index_size=4;mesh.position_buffer.resize(sizeof(positions));mesh.index_buffer.resize(sizeof(indices));
    std::memcpy(mesh.position_buffer.data(),positions,sizeof(positions));std::memcpy(mesh.index_buffer.data(),indices,sizeof(indices));
    auto blas=ray_geometry::BuildMeshBLAS(mesh);require(bool(blas),"Build stacked plane BLAS");
    math::m4x4 near{};near.columns[0]={1,0,0,0};near.columns[1]={0,1,0,0};near.columns[2]={0,0,1,0};near.columns[3]={0,0,.8f,1};auto far=near;far.columns[3].z=1.3f;
    std::array<ray_geometry::InstanceInput,2> inputs{{{blas,near,18},{blas,far,19}}};ray_geometry::SceneBVH out;buildScene(inputs,out);
    require(out.top.size()==3,"Two actual TLAS leaves");
    const u32 left=out.top[0].links[0],right=out.top[0].links[1];
    const bool rightFar=out.instances[out.top[right].links[2]].instance_id==19;
    if(rightFar!=farFirst)std::swap(out.top[0].links[0],out.top[0].links[1]);
    float origin[]={0,0,.502f},direction[]={0,0,1};ray_geometry::Hit hit;
    require(ray_geometry::Trace(out,origin,direction,0,10,hit)==1&&hit.instance_id==18,"Independent closest-hit oracle remains nearest under either leaf order");return out;
}
void snapshot(const char* name,const std::vector<Pixel>& low,const std::vector<Pixel>& high){
    const char* path=std::getenv("ENV_VIS_TEST_SNAPSHOT_PATH");if(!path)return;
    std::ofstream f(path,std::ios::app);require(bool(f),"Open GPU equivalence snapshot");f<<std::setprecision(9)<<"{\"case\":\""<<name<<"\",\"trace\":[";
    auto values=[&](const std::vector<Pixel>& pixels){for(u32 i=0;i<pixels.size();++i){if(i)f<<',';const auto p=pixels[i];f<<'['<<p.r<<','<<p.g<<','<<p.b<<','<<p.a<<']';}};
    values(low);f<<"],\"resolve\":[";values(high);f<<"]}\n";require(bool(f),"Write GPU equivalence snapshot");
}
// Special cases are in addition to the original fourteen exact checks.
enum class Variant {Normal,NotReady,NaNDepth,InvalidNormal,Fallback,UnknownFallback,BiasNormal,
    JitterPlane,JitterThinHit,ThinClearControl,InvalidProvider,NaNJitter,Malformed,Cycle,EndpointOnly,FarFirst,NearFirst,TinyUnsupported,MaterialChecker,ProvenanceLostAfterTrace};
std::pair<std::vector<Pixel>,std::vector<Pixel>> runCase(const char* name,bool closed,float roughness,Variant variant,u32 width=16,u32 height=16,bool candidate=false){
    const u32 W=width,H=height,HW=(W+1)/2,HH=(H+1)/2;
    GPU gpu;gpu.initializePipelines(candidate);auto geometry=variant==Variant::FarFirst||variant==Variant::NearFirst?stackedScene(variant==Variant::FarFirst):
        scene(closed,variant==Variant::BiasNormal,(variant==Variant::JitterThinHit||variant==Variant::ThinClearControl)?1:(variant==Variant::EndpointOnly?2:0));
    if(variant==Variant::Malformed)geometry.top[0].links[2]=0xffffffffu;
    if(variant==Variant::Cycle){geometry.top[0].links[0]=geometry.top[0].links[1]=0;geometry.top[0].links[3]=0;}
    CB cb{};cb.invVP[0]=.04f;cb.invVP[5]=.04f;cb.invVP[10]=1;cb.invVP[15]=1;cb.camera[2]=3;
    cb.dimensions[0]=W;cb.dimensions[1]=H;cb.dimensions[2]=HW;cb.dimensions[3]=HH;
    cb.target[0]=W;cb.target[1]=H;cb.target[2]=0;cb.target[3]=0;
    cb.policy[0]=(variant==Variant::NotReady||variant==Variant::UnknownFallback)?0:1;
    if(variant==Variant::JitterPlane||variant==Variant::JitterThinHit||variant==Variant::InvalidProvider){cb.camera[3]=.125f;cb.policy[2]=std::bit_cast<u32>(-.125f);}
    if(variant==Variant::InvalidProvider)cb.policy[1]=0x80000000u;
    if(variant==Variant::NaNJitter)cb.camera[3]=std::numeric_limits<float>::quiet_NaN();
    cb.parameters[0]=.002f;cb.parameters[1]=.02f;cb.parameters[2]=.95f;cb.parameters[3]=.05f;
    std::vector<float> depths(W*H,.5f);std::vector<std::array<float,4>> normals(W*H,{.5f,.5f,1,1}),orm(W*H,{1,roughness,0,1});
    if(variant==Variant::NaNDepth)for(auto& x:depths)x=std::numeric_limits<float>::quiet_NaN();
    if(variant==Variant::InvalidNormal)for(auto& n:normals)n={.5f,.5f,.5f,1};
    if(variant==Variant::Fallback||variant==Variant::UnknownFallback)normals[8*W+8]={.8f,.5f,.9f,1};
    if(variant==Variant::BiasNormal)for(auto& n:normals)n={.8f,.5f,.9f,1};
    if(variant==Variant::MaterialChecker)for(u32 y=0;y<H;++y)for(u32 x=0;x<W;++x)
        if((x/3+y/2)&1u)normals[y*W+x]={.9f,.5f,.8f,1};
    auto depth=gpu.texture(W,H,DataFormat::D32_Float,depths.data(),depths.size()*4);
    auto normal=gpu.texture(W,H,DataFormat::RGBA32_Float,normals.data(),normals.size()*16);
    auto material=gpu.texture(W,H,DataFormat::RGBA32_Float,orm.data(),orm.size()*16);
#if defined(ENV_VIS_WG32_TEST) || defined(ENV_VIS_PAIR_TEST)
    std::vector<u16> halfSentinel(HW*HH*4,u16(0x7e21u+(candidate?1u:0u)));
    std::vector<u16> fullSentinel(W*H*4,u16(0x7e31u+(candidate?1u:0u)));
    auto halfTex=gpu.texture(HW,HH,DataFormat::RGBA16_Float,halfSentinel.data(),halfSentinel.size()*sizeof(u16),true);
    auto fullTex=gpu.texture(W,H,DataFormat::RGBA16_Float,fullSentinel.data(),fullSentinel.size()*sizeof(u16),true);
#else
    auto halfTex=gpu.texture(HW,HH,DataFormat::RGBA16_Float,nullptr,0),fullTex=gpu.texture(W,H,DataFormat::RGBA16_Float,nullptr,0);
#endif
    auto constants=gpu.buffer(sizeof(cb),BufferType::Constant,GPUMemoryUsage::Dynamic,&cb);
    ResourceHandle geo[4]={gpu.buffer(geometry.top.size()*sizeof(ray_geometry::Node),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.top.data()),
        gpu.buffer(geometry.bottom.size()*sizeof(ray_geometry::Node),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.bottom.data()),
        gpu.buffer(geometry.triangles.size()*sizeof(ray_geometry::Triangle),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.triangles.data()),
        gpu.buffer(geometry.instances.size()*sizeof(ray_geometry::Instance),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.instances.data())};
    for(u32 phase=0;phase<2;++phase){
        DescriptorImageInfo images[5]{};DescriptorBufferInfo buffers[5]{};WriteDescriptorSet writes[10]{};
        const ResourceHandle tex[]={depth,normal,material,phase?fullTex:halfTex,phase?halfTex:fullTex};const u32 bindings[]={0,1,2,3,5};
        for(u32 i=0;i<5;++i){images[i]={handles::INVALID_SAMPLER,tex[i],i==3?ResourceState::UnorderedAccess:ResourceState::ShaderResource};writes[i]={gpu.sets[phase],bindings[i],0,1,i==3?DescriptorType::StorageImage:(i==0?DescriptorType::SampledDepthImage:DescriptorType::SampledImage),&images[i],nullptr};}
        buffers[0]={constants,0,sizeof(cb)};writes[5]={gpu.sets[phase],4,0,1,DescriptorType::UniformBuffer,nullptr,&buffers[0]};
        const u64 sizes[]={geometry.top.size()*sizeof(ray_geometry::Node),geometry.bottom.size()*sizeof(ray_geometry::Node),geometry.triangles.size()*sizeof(ray_geometry::Triangle),geometry.instances.size()*sizeof(ray_geometry::Instance)};
        for(u32 i=0;i<4;++i){buffers[i+1]={geo[i],0,sizes[i]};writes[i+6]={gpu.sets[phase],13+i,0,1,DescriptorType::StorageBuffer,nullptr,&buffers[i+1]};}
        gpu.d->UpdateDescriptorSets(10,writes);
    }
    CommandBufferHandle handle;auto* c=gpu.begin(handle);
    c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);
    c->BindComputePipeline(gpu.pipes[0]);c->BindDescriptorSets(PipelineBindPoint::Compute,gpu.pl,0,1,&gpu.sets[0],0,nullptr);
#ifdef ENV_VIS_PAIR_TEST
    c->Dispatch((HW+7)/8,(HH+7)/8,1);
#else
    c->Dispatch((HW+7)/8,(HH+(candidate?3:7))/(candidate?4:8),1);
#endif
    c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
    gpu.barrier(c,halfTex,ResourceState::UnorderedAccess,ResourceState::ShaderResource);
    if(variant==Variant::ProvenanceLostAfterTrace){
        // Trace must complete using a current, ready scene before Resolve sees
        // the production provenance-invalid policy. Separate submissions
        // model a change between the two RG callbacks without racing the CB.
        gpu.finish(handle,c);
        experimental_sah::InvalidateProvenance(cb.policy);
        require(cb.policy[0]==0&&(cb.policy[1]&0x80000000u),"Witness invalidation fails closed");
        require(gpu.d->UpdateBufferData(constants,&cb,sizeof(cb),0),"Upload invalidated Resolve policy");
        c=gpu.begin(handle);
        c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);
    }
    c->BindComputePipeline(gpu.pipes[1]);c->BindDescriptorSets(PipelineBindPoint::Compute,gpu.pl,0,1,&gpu.sets[1],0,nullptr);c->Dispatch((W+7)/8,(H+7)/8,1);
    gpu.barrier(c,fullTex,ResourceState::UnorderedAccess,ResourceState::ShaderResource);gpu.finish(handle,c);
    auto low=gpu.read(halfTex,HW,HH),high=gpu.read(fullTex,W,H);
    snapshot(name,low,high);
    const bool invalid=variant==Variant::NaNDepth||variant==Variant::InvalidNormal||variant==Variant::InvalidProvider||variant==Variant::NaNJitter||variant==Variant::TinyUnsupported;
    const bool unknown=variant==Variant::NotReady||variant==Variant::UnknownFallback||variant==Variant::Malformed||variant==Variant::Cycle||variant==Variant::EndpointOnly;
    const bool thin=variant==Variant::JitterThinHit||variant==Variant::ThinClearControl;
    const bool checker=variant==Variant::MaterialChecker;
    const bool provenanceLost=variant==Variant::ProvenanceLostAfterTrace;
    const bool blocked=closed||variant==Variant::FarFirst||variant==Variant::NearFirst;
    auto checkPixel=[&](Pixel p){for(float x:{p.r,p.g,p.b,p.a})require(finite(x),"Visibility RGBA finite");require(p.r>=0&&p.r<=1&&p.g>=0&&p.g<=1,"Visibility and known fraction bounded");};
    auto diagnose=[&](const char* stage,const std::vector<Pixel>& pixels){
        float minR=1,maxR=0;u32 mismatches=0;const float expected=blocked||invalid||unknown?0.f:1.f;
        for(u32 i=0;i<pixels.size();++i){const auto p=pixels[i];minR=std::min(minR,p.r);maxR=std::max(maxR,p.r);
            if(variant!=Variant::Fallback&&variant!=Variant::UnknownFallback&&!thin&&!checker&&!provenanceLost&&p.r!=expected){
                if(mismatches<4)std::cerr<<"Visibility mismatch case="<<name<<" roughness="<<roughness<<" stage="<<stage<<" pixel="<<i<<" RGBA="<<p.r<<","<<p.g<<","<<p.b<<","<<p.a<<" expectedR="<<expected<<'\n';
                ++mismatches;
            }
        }
        std::cout<<"{\"diagnostic\":\"environment_visibility\",\"case\":\""<<name<<"\",\"roughness\":"<<roughness<<",\"stage\":\""<<stage<<"\",\"min_r\":"<<minR<<",\"max_r\":"<<maxR<<",\"endpoint_mismatches\":"<<mismatches<<"}\n";
    };
    diagnose("trace",low);diagnose("resolve",high);
    for(auto p:low){checkPixel(p);if(variant!=Variant::Fallback&&variant!=Variant::UnknownFallback&&!thin&&!checker){require(p.r==(blocked||invalid||unknown?0.f:1.f),"Actual traced visibility matches synthetic open/closed/unsupported scene");if(invalid)require(p.a==0,"Invalid receiver has no support");else if(unknown)require(p.g==0,"Unknown geometry is not known visible");else require(p.g==1,"Complete valid geometry yields known visibility");}}
    for(auto p:high){checkPixel(p);if(variant!=Variant::Fallback&&variant!=Variant::UnknownFallback&&!thin&&!checker&&!provenanceLost){require(p.r==(blocked||invalid||unknown?0.f:1.f),"Actual resolved zero/unknown never turns white");if(invalid)require(p.a==0,"Resolve retains unsupported receiver");else if(unknown)require(p.g==0,"Resolve retains unknown support");}}
    if(provenanceLost){
        for(auto p:low)require(p.r==1&&p.g==1&&p.a==1,"Trace published known open half visibility before invalidation");
        for(auto p:high)require(p.r==0&&p.g==0&&p.b==1&&p.a==0,"Resolve rejects stale known half visibility as unsupported");
    }
    if(variant==Variant::Fallback||variant==Variant::UnknownFallback){const auto target=high[8*W+8];require(target.a==1,"Fallback target receiver valid");require(target.b==(unknown?3.f:2.f),"Material normal discontinuity rejects half neighbors and selects documented fallback");require(target.r==(unknown||closed?0.f:1.f),"Fallback uses actual geometry, never unknown-white");require(target.g==(unknown?0.f:1.f),"Fallback support matches actual one-ray measurement");}
    if(checker){
        auto expected=[&](u32 x,u32 y){return ((x/3+y/2)&1u)?0.f:1.f;};
        for(u32 y=0;y<HH;++y)for(u32 x=0;x<HW;++x){const auto p=low[y*HW+x];require(p.r==expected(std::min(x*2+1,W-1),std::min(y*2+1,H-1))&&p.g==1&&p.a==1,"Material checker trace has known hemisphere classification at actual mapped sample");}
        for(u32 y=0;y<H;++y)for(u32 x=0;x<W;++x){const auto p=high[y*W+x];require(p.r==expected(x,y)&&p.g==1&&p.a==1,"Material checker resolve preserves spatial identity across odd/group boundaries");}
    }
    if(thin){const float expected=variant==Variant::JitterThinHit?0.f:1.f;const auto trace=low[4*HW+4],resolved=high[9*W+9];
        require(trace.r==expected&&trace.g==1&&trace.a==1,"Actual XY jitter determines thin-sheet hit at traced receiver");
        require(resolved.r==expected&&resolved.g==1&&resolved.a==1,"Resolve keeps same-point thin-sheet classification");
        // Independent geometry-only ray oracle: 0.04*(NDC-jitter), plane z0.5,
        // camera z3 and planar mirror give this explicit reflected direction.
        const bool jitter=variant==Variant::JitterThinHit;float origin[]={jitter?.0025f:.0075f,jitter?-.0025f:-.0075f,.502f};
        float direction[]={origin[0],origin[1],2.5f};ray_geometry::Hit hit;
        require(ray_geometry::Trace(geometry,origin,direction,0,100,hit)==(jitter?1:0),"Independent thin-sheet CPU geometry oracle");
    }
    std::cout<<"{\"test\":\"environment_visibility_gpu\",\"case\":\""<<name<<"\",\"roughness\":"<<roughness<<",\"half_pixels\":"<<HW*HH<<",\"resolved_pixels\":"<<W*H<<",\"width\":"<<W<<",\"height\":"<<H<<",\"triangles\":"<<geometry.triangles.size()<<"}\n";
    return {low,high};
}
void registerCases(const std::shared_ptr<TestSuite>& suite){
    auto add=[&](const std::string& label,bool closed,float roughness,Variant variant,u32 w=16,u32 h=16){
        suite->AddTestCase(TestCase(label,[=]{auto baseline=runCase(label.c_str(),closed,roughness,variant,w,h);
#if defined(ENV_VIS_WG32_TEST) || defined(ENV_VIS_PAIR_TEST)
            auto candidate=runCase(label.c_str(),closed,roughness,variant,w,h,true);
            require(baseline.first.size()==candidate.first.size() && baseline.second.size()==candidate.second.size(),"matching output dimensions");
            require(std::memcmp(baseline.first.data(),candidate.first.data(),baseline.first.size()*sizeof(Pixel))==0 && std::memcmp(baseline.second.data(),candidate.second.data(),baseline.second.size()*sizeof(Pixel))==0,"trace and resolve complete outputs byte-identical");
#endif
            return TestResult::Passed;}));
    };
    for(float rough:{0.f,1.f}){const std::string suffix=rough==0?"_rough0":"_rough1";
        add("open_complete"+suffix,false,rough,Variant::Normal);
        add("closed_opaque"+suffix,true,rough,Variant::Normal);
        add("geometry_not_ready"+suffix,false,rough,Variant::NotReady);
        add("nan_depth"+suffix,false,rough,Variant::NaNDepth);
        add("invalid_normal"+suffix,false,rough,Variant::InvalidNormal);
    }
    add("material_normal_fallback_open",false,0,Variant::Fallback);
    add("material_normal_fallback_closed",true,0,Variant::Fallback);
    add("material_normal_fallback_unknown",false,0,Variant::UnknownFallback);
    add("geometric_normal_bias",false,0,Variant::BiasNormal);
    add("xy_jitter_open_plane_rough0",false,0,Variant::JitterPlane);
    add("xy_jitter_open_plane_rough1",false,1,Variant::JitterPlane);
    add("xy_jitter_thin_sheet_hit",false,0,Variant::JitterThinHit);
    add("zero_jitter_thin_sheet_clear_control",false,0,Variant::ThinClearControl);
    add("jitter_provider_invalid",false,0,Variant::InvalidProvider);
    add("nonfinite_jitter_failclosed",false,0,Variant::NaNJitter);
    add("malformed_tlas_unknown",true,0,Variant::Malformed);
    add("cycle_tlas_unknown",true,0,Variant::Cycle);
    add("endpoint_only_unknown",false,0,Variant::EndpointOnly);
    add("opaque_far_leaf_first",false,0,Variant::FarFirst);
    add("opaque_near_leaf_first",false,0,Variant::NearFirst);
    add("provenance_lost_after_trace",false,0,Variant::ProvenanceLostAfterTrace);
    for(auto size:{std::array<u32,2>{17,19},std::array<u32,2>{3,5}}){
        const u32 w=size[0],h=size[1];const std::string suffix="_"+std::to_string(w)+"x"+std::to_string(h);
        for(float rough:{0.f,1.f}){const std::string r=rough==0?"_rough0":"_rough1";
            add("boundary_open"+suffix+r,false,rough,Variant::Normal,w,h);
            add("boundary_closed"+suffix+r,true,rough,Variant::Normal,w,h);
            add("boundary_unknown"+suffix+r,false,rough,Variant::NotReady,w,h);
        }
        add("boundary_material_checker"+suffix,false,0,Variant::MaterialChecker,w,h);
    }
    add("single_pixel_unsupported_rough0",false,0,Variant::TinyUnsupported,1,1);
    add("single_pixel_unsupported_rough1",false,1,Variant::TinyUnsupported,1,1);
}
}
int main(){std::cout<<"[EnvironmentVisibilityTest] geometry="<<(useSAH()?"experimental_sah":"original")<<'\n';if(const char* path=std::getenv("ENV_VIS_TEST_SNAPSHOT_PATH")){std::ofstream f(path);if(!f)return 1;}
    auto s=std::make_shared<TestSuite>("VulkanEnvironmentVisibility");registerCases(s);TestRunner::RegisterTestSuite(s);const auto stats=TestRunner::RunAllSuites();
    return stats.totalTests==42&&stats.passedTests==42&&stats.failedTests==0&&stats.skippedTests==0?0:1;}
