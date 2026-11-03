// Actual production DDGIGIGather geometry-connection contracts; private fixtures only.
#include "../../TestFramework.h"
#include "EngineTest/Utils/TestEnv.h"   // R3: portable setenv/unsetenv (MSVC lacks POSIX ones)
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/Utilities/GeometryFirstHit.h"
#if defined(GG_CONNECTION_COMPACT_TEST) || defined(GG_CONNECTION_UPPER_TEST) || defined(GG_INSTANCE_CACHE_TEST) || defined(GG_EQUAL_BOX_TEST) || defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
#include "Graphics/Utilities/GeometryBVHStore.h"
#endif
#include "Graphics/RenderPipeline/Modules/GIGatherModule.h"
#ifdef GG_CONNECTION_TRAVERSAL_TEST
#include "Graphics/RenderPipeline/Modules/DDGITraversalAudit.h"
#endif
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <algorithm>
#include "DDGIGatherConnectionWitness.h"
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
struct Pixel {float r,g,b,a;u16 raw[4];};
struct GPU {
    RHIDeviceBase* d=nullptr;VulkanDevice* vk=nullptr;
    std::vector<ResourceHandle> buffers,textures;
    ShaderHandle shaders[2]{handles::INVALID_SHADER,handles::INVALID_SHADER};
    DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;
    PipelineHandle pipes[2]{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    DescriptorSetHandle sets[2]{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};
#ifdef GG_INSTANCE_CACHE_TEST
    InstanceNormalCache cache;
#endif
    GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;d=CreateRHIDevice(desc);require(d!=nullptr,"Create Vulkan device");vk=static_cast<VulkanDevice*>(d);}
    ~GPU(){if(!d)return;
#ifdef GG_INSTANCE_CACHE_TEST
    cache.Shutdown();
#endif
for(auto s:sets)if(s!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(s);for(auto p:pipes)if(p!=handles::INVALID_PIPELINE)d->DestroyPipeline(p);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);for(auto s:shaders)if(s!=handles::INVALID_SHADER)d->DestroyShader(s);for(auto t:textures)d->DestroyTexture(t);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
    ResourceHandle buffer(u64 bytes,BufferType type,GPUMemoryUsage mem,const void* data=nullptr){
        BufferDesc bd{};bd.size=bytes;bd.type=type;bd.memoryUsage=mem;bd.name="EnvironmentVisibilityTest";
        auto h=d->CreateBuffer(bd);require(h!=handles::INVALID_RESOURCE,"Create buffer");buffers.push_back(h);if(data)require(d->UpdateBufferData(h,data,bytes,0),"Upload buffer");return h;
    }
    VulkanCommandBuffer* begin(CommandBufferHandle& handle){handle=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(handle);require(c&&c->Reset()&&c->Begin(),"Begin commands");return c;}
    void finish(CommandBufferHandle handle,VulkanCommandBuffer* c){
        const auto fail=[](const char* operation){std::fprintf(stderr,"[DDGI_GATHER_FIXTURE] FAIL %s; no cleanup of possibly pending GPU work\n",operation);std::fflush(stderr);std::_Exit(7);};
        if(!c->End())fail("end before submit");
        if(!c->Submit(0))fail("submit outcome");
        if(!c->WaitForCompletion())fail("completion after submit");
        d->DestroyCommandBuffer(handle);
    }
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
    void initializePipelines(){
        const u32 numbers[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,17,18,19,20,23,24,25,26,27
#ifdef GG_CONNECTION_TRAVERSAL_TEST
            ,28,29
#endif
#ifdef GG_EQUAL_BOX_TEST
            ,28
#endif
#ifdef GG_INSTANCE_CACHE_TEST
            ,28,29,30
#endif
        };
        std::vector<DescriptorSetLayoutBinding> bindings;
        for(auto n:numbers){DescriptorType t=DescriptorType::StorageBuffer;
            if(n==0)t=DescriptorType::SampledDepthImage;
            else if(n==1||n==3||n==13)t=DescriptorType::SampledImage;
            else if(n==2||n==14)t=DescriptorType::StorageImage;
            else if(n==4||n==5||n==6||n==9||n==18||n==27||n==28)t=DescriptorType::UniformBuffer;
#if defined(GG_INSTANCE_CACHE_TEST) || defined(GG_EQUAL_BOX_TEST)
            if(n>=28)t=DescriptorType::StorageBuffer;
#endif
            DescriptorSetLayoutBinding b{n,t,1,ShaderStage::Compute,nullptr};if(t==DescriptorType::StorageBuffer)b.readonly=n!=29;bindings.push_back(b);
        }
#ifdef GG_INSTANCE_CACHE_TEST
        require(cache.Initialize(d,bindings.data(),u32(bindings.size())-3),"Initialize actual host cache pipelines");
#endif
        sl=d->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"Create actual GG descriptor layout");
        pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"Create GG pipeline layout");
        const char* paths[]={GG_CONNECTION_BASE_SPV,GG_CONNECTION_GEOMETRY_SPV};
        for(u32 p=0;p<2;++p){std::ifstream f(paths[p],std::ios::binary|std::ios::ate);require(bool(f),"Open actual production GG module");auto n=f.tellg();require(n>0,"Nonempty GG SPV");std::vector<u8> bytes(static_cast<size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),n);require(bool(f),"Read GG SPV");
#if defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
            u64 hash=14695981039346656037ull;for(u8 byte:bytes){hash^=byte;hash*=1099511628211ull;}
            static u64 referenceHash=0;if(p==0)referenceHash=hash;else require(referenceHash && referenceHash!=hash,"payload reference and candidate SPVs differ");
            std::fprintf(stderr,"[DDGI_GATHER_VARIANT_FIXTURE] phase=%s shader_fnv1a64=%llu path=%s\n",p?"candidate":"reference",static_cast<unsigned long long>(hash),paths[p]);
#endif
            shaders[p]=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");require(shaders[p]!=handles::INVALID_SHADER,"Create GG shader");ComputePipelineDesc cd{};cd.layout=pl;cd.computeShader=shaders[p];pipes[p]=d->CreateComputePipeline(cd);require(pipes[p]!=handles::INVALID_PIPELINE,"Create GG pipeline");sets[p]=d->CreateDescriptorSet({sl});require(sets[p]!=handles::INVALID_DESCRIPTOR_SET,"Create GG set");}
    }
    std::vector<Pixel> read(ResourceHandle t,u32 w,u32 h){
        auto rb=buffer(u64(w)*h*8,BufferType::Raw,GPUMemoryUsage::Readback);CommandBufferHandle handle;auto* c=begin(handle);
        barrier(c,t,ResourceState::ShaderResource,ResourceState::CopySource);
        c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        BufferTextureCopyRegion r{};r.imageSubresource.layerCount=1;r.imageExtent={w,h,1};c->CopyTextureToBuffer(t,rb,&r,1);
        c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);finish(handle,c);
        auto* p=static_cast<const u16*>(d->MapBuffer(rb,0,u64(w)*h*8));require(p!=nullptr,"Map visibility readback");std::vector<Pixel> result(w*h);for(u32 i=0;i<w*h;++i)result[i]={half(p[i*4]),half(p[i*4+1]),half(p[i*4+2]),half(p[i*4+3]),{p[i*4],p[i*4+1],p[i*4+2],p[i*4+3]}};d->UnmapBuffer(rb);return result;
    }
};

enum class Geometry {Clear,Closed,Target,Malformed,Cycle,Cloth0,Cloth1,ScanTarget,CenterSheet,Endpoint,Sheet,ReverseSheet,EndpointPlane,Parallel,ParallelFar,HitThenInvalid,InvalidThenHit,DeepTLAS,DeepBLAS,UpperReordered,UpperRejected,UpperBlocking,UpperParallel,UpperMultiple,UpperDeep,UpperDeepRejected,UpperMixed,UpperCentroidOpen,UpperCentroidBlocked,QueryUReject,QueryBaryReject,QueryDegenerate,QuerySkinny,BoundsTopMissBad,BoundsBottomMissBad,BoundsTopHitBad,BoundsBottomHitBad};
struct Config {
    Geometry geometry=Geometry::Clear;
    bool ready=true,allBright=true,weakSource=false,scan=false;
#if defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
    u32 payloadInstanceCount=0,activeProbes=8,payloadSignedZero=0,payloadDeepCount=63;bool payloadMirror=false;
#endif
#ifdef GG_EQUAL_BOX_TEST
    bool equalBoxes=false,corruptFlags=false;u32 equalMask=3,deepCount=63,zeroBounds=0;
#endif
#ifdef GG_CACHELESS_TEST
    bool oddGrid=false;
#endif
#ifdef GG_CONNECTION_TRAVERSAL_TEST
    bool background=false,outsideVolume=false;
#endif
    float strength=1,mean=100,variance=.25f,originState=1;
    u32 mode=7;
#ifdef GG_INSTANCE_CACHE_TEST
    bool skipProducer=false,wrongOwner=false,sharedRoot=false,mirror=false;
#endif
    float jitterX=0,jitterY=0;
    u32 invalidSurface=0; bool invalidOriginXYZ=false,nonneutralNormal=false,scaled=false,large=false,shortSegment=false; float endOffset=0.f,parallelTilt=0.f,largeX=4096.f;
};
struct Pair {std::vector<Pixel> diffuse,specular;};
struct Result {Pair baseline,candidate;
#ifdef GG_CONNECTION_TRAVERSAL_TEST
    std::vector<u32> audit;
#endif
};
struct ViewCB {float invVP[16],origin[4],counts[4],camera[4],debug[4];};
struct VolumeCB {float origin[4][4],counts[4][4],meta[4];};
static_assert(sizeof(ViewCB)==128&&offsetof(ViewCB,counts)==80&&offsetof(ViewCB,camera)==96);
static_assert(sizeof(VolumeCB)==144);

ray_geometry::SceneBVH buildGeometry(const Config& cfg){
    RHIMeshAsset mesh{};std::vector<float> v;std::vector<u32> indices;
    auto triangle=[&](std::array<float,3>a,std::array<float,3>b,std::array<float,3>c){u32 first=v.size()/3;for(auto p:{a,b,c})v.insert(v.end(),p.begin(),p.end());indices.insert(indices.end(),{first,first+1,first+2});};
    auto quad=[&](std::array<float,3>a,std::array<float,3>b,std::array<float,3>c,std::array<float,3>d){triangle(a,b,c);triangle(a,c,d);};
    if(cfg.geometry==Geometry::QueryUReject) triangle({0,0,.75f},{-1,0,.75f},{0,1,.75f});
    else if(cfg.geometry==Geometry::QueryBaryReject) triangle({0,0,.75f},{.75f,0,.75f},{0,.75f,.75f});
    else if(cfg.geometry==Geometry::QueryDegenerate) triangle({0,0,.75f},{1,0,.75f},{0,1,.75f});
    else if(cfg.geometry==Geometry::QuerySkinny) triangle({0,0,.75f},{1,1,.75f},{1,1.00001f,.75f});
    else if(cfg.geometry==Geometry::UpperCentroidOpen||cfg.geometry==Geometry::UpperCentroidBlocked){
        float extent=cfg.geometry==Geometry::UpperCentroidOpen?4.f:6.f;
        triangle({-2.f,-2.f,.75f},{-2.f+extent,-2.f,.75f},{-2.f,-2.f+extent,.75f});
    }
    else if(cfg.geometry==Geometry::EndpointPlane){float z=1.f+cfg.endOffset*.331f;quad({-5,-5,z},{5,-5,z},{5,5,z},{-5,5,z});}
    else if(cfg.geometry==Geometry::Parallel||cfg.geometry==Geometry::ParallelFar||cfg.geometry==Geometry::UpperParallel||cfg.geometry==Geometry::UpperMixed){float y=cfg.geometry==Geometry::ParallelFar?100.f:0.f;triangle({-.176f,-.224f+y,.4024f},{1.196f,1.204f+y,1.0996f+cfg.parallelTilt},{-.176f,.776f+y,.4024f});}
    else if(cfg.geometry==Geometry::Endpoint){quad({-5,-5,.999995f},{5,-5,.999995f},{5,5,.999995f},{-5,5,.999995f});}
    else if(cfg.geometry==Geometry::ReverseSheet){quad({-5,5,.75f},{5,5,.75f},{5,-5,.75f},{-5,-5,.75f});}
    else if(cfg.geometry==Geometry::Sheet||cfg.geometry==Geometry::UpperBlocking){quad({-5,-5,.75f},{5,-5,.75f},{5,5,.75f},{-5,5,.75f});}
    else if(cfg.geometry==Geometry::Cloth0||cfg.geometry==Geometry::Cloth1){
        auto* t=GatherWitness::Triangle;triangle({t[0][0],t[0][1],t[0][2]},{t[1][0],t[1][1],t[1][2]},{t[2][0],t[2][1],t[2][2]});
    }else if(cfg.geometry==Geometry::Closed||cfg.geometry==Geometry::Malformed||cfg.geometry==Geometry::Cycle||cfg.geometry==Geometry::HitThenInvalid||cfg.geometry==Geometry::InvalidThenHit){
        constexpr float a=-.25f,b=.25f,z=.35f,t=.85f;
        quad({a,a,z},{b,a,z},{b,b,z},{a,b,z});quad({a,a,t},{a,b,t},{b,b,t},{b,a,t});
        quad({a,a,z},{a,b,z},{a,b,t},{a,a,t});quad({b,a,z},{b,a,t},{b,b,t},{b,b,z});
        quad({a,a,z},{a,a,t},{b,a,t},{b,a,z});quad({a,b,z},{b,b,z},{b,b,t},{a,b,t});
    }else if(cfg.geometry==Geometry::Target){quad({.35f,.35f,.8f},{.65f,.35f,.8f},{.65f,.65f,.8f},{.35f,.65f,.8f});}
    else if(cfg.geometry==Geometry::CenterSheet){quad({.01f,-10,-10},{.01f,10,-10},{.01f,10,10},{.01f,-10,10});}
    else if(cfg.geometry==Geometry::ScanTarget){quad({-.1f,.35f,.75f},{.25f,.35f,.75f},{.25f,.65f,.75f},{-.1f,.65f,.75f});}
    else quad({-10,-10,-4},{10,-10,-4},{10,10,-4},{-10,10,-4});
    if(cfg.scaled)for(size_t i=0;i<v.size();i+=3){v[i]/=2.f;v[i+1]/=.5f;v[i+2]/=3.f;}
    mesh.num_vertices=v.size()/3;mesh.num_indices=indices.size();mesh.index_size=4;mesh.position_buffer.resize(v.size()*4);mesh.index_buffer.resize(indices.size()*4);
    std::memcpy(mesh.position_buffer.data(),v.data(),v.size()*4);std::memcpy(mesh.index_buffer.data(),indices.data(),indices.size()*4);
    auto blas=ray_geometry::BuildMeshBLAS(mesh);require(bool(blas),"Build real GG fixture BLAS");math::m4x4 m{};m.columns[0]={1,0,0,0};m.columns[1]={0,1,0,0};m.columns[2]={0,0,1,0};m.columns[3]={0,0,0,1};if(cfg.scaled){m.columns[0].x=2.f;m.columns[1].y=.5f;m.columns[2].z=3.f;}
    std::array<ray_geometry::InstanceInput,1> input{{{blas,m,324}}};ray_geometry::SceneBVH out;require(out.Build(input),"Build real GG fixture TLAS");
#ifdef GG_CONNECTION_EQUIVALENCE
    if(cfg.geometry==Geometry::HitThenInvalid||cfg.geometry==Geometry::InvalidThenHit){
        // Traversal pushes x then y, so y is visited first. Node 1 is a real
        // blocker leaf; the out-of-range sibling is bounded UNKNOWN.
        const auto blocker=out.top[0];auto root=blocker;
        root.links[3]=0;root.links[2]=0;
        const bool hitFirst=cfg.geometry==Geometry::HitThenInvalid;
        root.links[0]=hitFirst?999u:1u;root.links[1]=hitFirst?1u:999u;
        out.top.clear();out.top.push_back(root);out.top.push_back(blocker);
    }
#endif
#if defined(GG_CONNECTION_COMPACT_TEST) || defined(GG_EQUAL_BOX_TEST) || defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
    if(cfg.geometry==Geometry::DeepTLAS||cfg.geometry==Geometry::DeepBLAS){
        // A valid push-both comb. Every box contains the test ray; y descends
        // first while x siblings stay pending, reaching the exact capacity.
        const bool top=cfg.geometry==Geometry::DeepTLAS;
#ifdef GG_EQUAL_BOX_TEST
        const u32 count=cfg.deepCount;
#elif defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
        const u32 count=cfg.payloadDeepCount;
#else
        const u32 count=top?16u:32u;
#endif
        const auto originalTop=out.top[0],originalBottom=out.bottom[0];
        const auto originalInstance=out.instances[0];
        std::vector<ray_geometry::Triangle> source(out.triangles.begin(),out.triangles.end());
        if(top){out.top.clear();out.instances.clear();}
        else {out.bottom.clear();out.triangles.clear();}
        auto& nodes=top?out.top:out.bottom;
        auto make=[&](auto&& self,u32 left)->u32 {
            u32 at=u32(nodes.size());ray_geometry::Node node=top?originalTop:originalBottom;
            for(u32 k=0;k<3;++k){node.lo[k]=-10.f;node.hi[k]=10.f;}
            nodes.push_back(node);
            if(left==1){
                if(top){auto instance=originalInstance;instance.instance_id=u32(out.instances.size());nodes[at].links[2]=instance.instance_id;nodes[at].links[3]=1;out.instances.push_back(instance);}
                else {nodes[at].links[2]=u32(out.triangles.size());nodes[at].links[3]=u32(source.size());for(auto t:source)out.triangles.push_back(t);}
            }else{
                const u32 child=self(self,left-1),sibling=self(self,1);
                nodes[at].links[0]=sibling;nodes[at].links[1]=child;nodes[at].links[2]=nodes[at].links[3]=0;
            }
            return at;
        };make(make,count);
        // Both levels must be traversable. The original clear fixture's box
        // sits behind the ray, so expand the other level without moving tris.
        if(top)for(auto& n:out.bottom)for(u32 k=0;k<3;++k){n.lo[k]=-10.f;n.hi[k]=10.f;}
        else for(auto& n:out.top)for(u32 k=0;k<3;++k){n.lo[k]=-10.f;n.hi[k]=10.f;}
        const auto cert=ray_geometry::CertifyCompactTraversal(out);
#if defined(GG_EQUAL_BOX_TEST) || defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
        require(count==63 ? (cert.valid && (top?cert.top_max_pending==63:cert.bottom_max_pending==63)) : !cert.valid,"63 pending admitted; 64-deep source rejected and falls back");
#else
        require(cert.Fits()&&(top?cert.top_max_pending==16:cert.bottom_max_pending==32),"valid exact-capacity comb certificate");
#endif
        std::cout<<"{\"compact_certificate\":true,\"top_pending\":"<<cert.top_max_pending<<",\"bottom_pending\":"<<cert.bottom_max_pending<<"}\n";
    }
#endif
#ifdef GG_CONNECTION_UPPER_TEST
    if(cfg.geometry==Geometry::UpperReordered||cfg.geometry==Geometry::UpperRejected||cfg.geometry==Geometry::UpperBlocking||cfg.geometry==Geometry::UpperParallel||cfg.geometry==Geometry::UpperMultiple||cfg.geometry==Geometry::UpperDeep||cfg.geometry==Geometry::UpperDeepRejected||cfg.geometry==Geometry::UpperMixed||cfg.geometry==Geometry::UpperCentroidOpen||cfg.geometry==Geometry::UpperCentroidBlocked){
        // Eight atomic leaves in deliberately poor spatial order, preserving
        // every triangle byte after the source has been assembled.
        const auto original=out.bottom[0];std::vector<ray_geometry::Triangle> source(out.triangles.begin(),out.triangles.end());
        out.bottom.clear();out.triangles.clear();
        const u32 countLeaves=cfg.geometry==Geometry::UpperDeep?63u:cfg.geometry==Geometry::UpperDeepRejected?64u:8u;
        auto make=[&](auto&& self,u32 count,u32 first)->u32 {
            u32 at=u32(out.bottom.size());out.bottom.push_back(original);
            if(count==1){auto& n=out.bottom[at];n.links[2]=u32(out.triangles.size());n.links[3]=u32(source.size());
                for(u32 k=0;k<3;++k){n.lo[k]=original.lo[k];n.hi[k]=original.hi[k];}const float offset=(cfg.geometry==Geometry::UpperCentroidOpen||cfg.geometry==Geometry::UpperCentroidBlocked)?(first%2?100.f:0.f):float(first/2)+(first%2?100.f:0.f);n.lo[0]+=offset;n.hi[0]+=offset;
                for(auto t:source){t.a[0]+=offset;
                    if(cfg.geometry==Geometry::UpperMixed&&first==1){t.a[0]=-5;t.a[1]=-5;t.a[2]=.75f;t.e1[0]=15;t.e1[1]=0;t.e1[2]=0;t.e2[0]=0;t.e2[1]=15;t.e2[2]=0;for(u32 k=0;k<3;++k){n.lo[k]=t.a[k];n.hi[k]=std::max({t.a[k],t.a[k]+t.e1[k],t.a[k]+t.e2[k]});}}
                    out.triangles.push_back(t);}}
            else{u32 a=self(self,1,first),b=self(self,count-1,first+1);auto& n=out.bottom[at];n.links[0]=a;n.links[1]=b;n.links[2]=n.links[3]=0;
                for(u32 k=0;k<3;++k){n.lo[k]=std::min(out.bottom[a].lo[k],out.bottom[b].lo[k]);n.hi[k]=std::max(out.bottom[a].hi[k],out.bottom[b].hi[k]);}}
            return at;};make(make,countLeaves,0u);
        for(u32 k=0;k<3;++k){out.top[0].lo[k]=out.bottom[0].lo[k];out.top[0].hi[k]=out.bottom[0].hi[k];}
        if(cfg.geometry==Geometry::UpperMultiple){
            const u32 nb=u32(out.bottom.size()),nt=u32(out.triangles.size());
            for(u32 i=0;i<nb;++i){auto node=out.bottom[i];if(node.links[3])node.links[2]+=nt;else{node.links[0]+=nb;node.links[1]+=nb;}out.bottom.push_back(node);}
            for(u32 i=0;i<nt;++i)out.triangles.push_back(out.triangles[i]);
            auto instance=out.instances[0];instance.root=nb;instance.instance_id++;instance.inverse_rows[0][3]=-.125f;out.instances.push_back(instance);
            auto first=out.top[0],second=first,root=first;first.links[2]=0;second.links[2]=1;root.links[0]=1;root.links[1]=2;root.links[2]=root.links[3]=0;root.hi[0]+=.125f;second.lo[0]+=.125f;second.hi[0]+=.125f;out.top={root,first,second};
        }
        if(cfg.geometry==Geometry::UpperRejected){
            // The original triangle plane is z=-4, behind these receiver rays.
            // Expand only the root bounds to enclose receiver and all probes so
            // traversal pushes and eventually visits the invalid x child.
            for(u32 k=0;k<3;++k){out.bottom[0].lo[k]=std::min(out.bottom[0].lo[k],-10.f);out.bottom[0].hi[k]=std::max(out.bottom[0].hi[k],10.f);out.top[0].lo[k]=out.bottom[0].lo[k];out.top[0].hi[k]=out.bottom[0].hi[k];}
            out.bottom[0].links[0]=u32(out.bottom.size());
        }
    }
#endif
    if(cfg.geometry==Geometry::QueryUReject||cfg.geometry==Geometry::QueryBaryReject||cfg.geometry==Geometry::QueryDegenerate||cfg.geometry==Geometry::QuerySkinny) {
        // Force the actual ray through both leaf boxes so the triangle guards execute.
        for(auto* nodes:{&out.top,&out.bottom}) for(auto& node:*nodes) for(u32 k=0;k<3;++k) {node.lo[k]=-10.f;node.hi[k]=10.f;}
        if(cfg.geometry==Geometry::QueryDegenerate) for(u32 k=0;k<3;++k)out.triangles[0].e2[k]=0.f;
    }
    if(cfg.geometry==Geometry::BoundsTopMissBad || cfg.geometry==Geometry::BoundsTopHitBad) {
        if(cfg.geometry==Geometry::BoundsTopHitBad)for(u32 k=0;k<3;++k){out.top[0].lo[k]=-10.f;out.top[0].hi[k]=10.f;}
        out.top[0].links[2]=0xffffffffu;
    }
    if(cfg.geometry==Geometry::BoundsBottomMissBad || cfg.geometry==Geometry::BoundsBottomHitBad) {
        for(u32 k=0;k<3;++k){out.top[0].lo[k]=-10.f;out.top[0].hi[k]=10.f;}
        if(cfg.geometry==Geometry::BoundsBottomHitBad)for(u32 k=0;k<3;++k){out.bottom[0].lo[k]=-10.f;out.bottom[0].hi[k]=10.f;}
        out.bottom[0].links[2]=0xffffffffu;
    }
    if(cfg.geometry==Geometry::Malformed)out.top[0].links[2]=0xffffffffu;
    if(cfg.geometry==Geometry::Cycle){out.top[0].links[0]=out.top[0].links[1]=0;out.top[0].links[3]=0;}
    return out;
}
Result dispatch(const Config& cfg){
    GPU g;g.initializePipelines();
#ifdef GG_CACHELESS_TEST
    const u32 width=cfg.oddGrid?17:(cfg.scan?5:1),height=cfg.oddGrid?9:1;
#else
    const u32 width=cfg.scan?5:1,height=1;
#endif
    const bool cloth=cfg.geometry==Geometry::Cloth0||cfg.geometry==Geometry::Cloth1;const u32 witness=cfg.geometry==Geometry::Cloth1?1:0;
    float p[3]={0,0,.5f},n[3]={0,0,1};float spacing=2;
    if(cfg.large)p[0]=cfg.largeX;
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
    for(u32 k=0;k<3;++k){view.origin[k]=(cloth||cfg.large)?p[k]-spacing*.5f:-1;view.counts[k]=2;view.camera[k]=p[k]+n[k]*5;}
    view.origin[3]=spacing;view.camera[3]=float(cfg.mode);
#ifdef GG_CONNECTION_TRAVERSAL_TEST
    if(cfg.outsideVolume)for(u32 k=0;k<3;++k)view.origin[k]+=100.f;
#endif
    VolumeCB volume{};for(u32 k=0;k<4;++k)volume.origin[0][k]=view.origin[k];volume.counts[0][0]=volume.counts[0][1]=volume.counts[0][2]=2;volume.meta[0]=1;
    std::array<float,8*9*4> sh{};std::array<float,8*128> moments{};std::array<float,8*4> origins{},confidence{};std::array<float,16> open{};
    for(u32 i=0;i<8;++i){
        for(u32 k=0;k<3;++k)sh[i*36+k]=cfg.allBright?cfg.strength:0;
        for(u32 c=0;c<64;++c){moments[i*128+c]=cfg.mean;moments[i*128+64+c]=cfg.variance;}
        origins[i*4]=view.origin[0]+float(i&1u)*spacing;origins[i*4+1]=view.origin[1]+float((i>>1u)&1u)*spacing;origins[i*4+2]=view.origin[2]+float((i>>2u)&1u)*spacing;origins[i*4+3]=cfg.originState;
#if defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
        if(i>=cfg.activeProbes)origins[i*4+3]=0.f;
#endif
        confidence[i*4+2]=confidence[i*4+3]=1;
    }
    if(cfg.shortSegment)for(u32 i=0;i<8;++i){origins[i*4]=.02001f;origins[i*4+1]=-.02f;origins[i*4+2]=.502f;}
    if(cfg.large)for(u32 i=0;i<8;++i){origins[i*4]=cfg.largeX+1.f;origins[i*4+1]=1.f;origins[i*4+2]=1.5f;}
    if(cfg.invalidOriginXYZ) for(u32 i=0;i<8;++i) origins[i*4]=std::numeric_limits<float>::quiet_NaN();
    if(!cfg.allBright)for(u32 k=0;k<3;++k)sh[7*36+k]=cfg.strength;
    if(cfg.weakSource)for(u32 k=0;k<3;++k)sh[k]=1;
    if(cloth){for(u32 k=0;k<3;++k)origins[7*4+k]=GatherWitness::Anchor[k];for(u32 j=0;j<9;++j)for(u32 k=0;k<3;++k)sh[7*36+j*4+k]=GatherWitness::SH[j][k];}
    auto geometry=buildGeometry(cfg);
#if defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
    if(cfg.payloadInstanceCount) {
        const auto leaf=geometry.top[0];const auto instance=geometry.instances[0];geometry.top.clear();geometry.instances.clear();
        auto build=[&](auto&& self,u32 count)->u32 {
            const u32 at=u32(geometry.top.size());auto node=leaf;for(u32 k=0;k<3;++k){node.lo[k]=-10.f;node.hi[k]=10.f;}geometry.top.push_back(node);
            if(count==1){auto inst=instance;inst.instance_id=u32(geometry.instances.size());geometry.top[at].links[2]=inst.instance_id;geometry.top[at].links[3]=1;geometry.instances.push_back(inst);}
            else {const u32 left=self(self,count/2),right=self(self,count-count/2);geometry.top[at].links[0]=left;geometry.top[at].links[1]=right;geometry.top[at].links[2]=geometry.top[at].links[3]=0;}
            return at;
        };build(build,cfg.payloadInstanceCount);
        if(cfg.payloadMirror){require(geometry.instances.size()>1,"mirror requires two instances");geometry.instances[1].inverse_rows[0][0]*=-1.f;}
        if(cfg.payloadSignedZero)for(auto& node:geometry.top)node.hi[2]=cfg.payloadSignedZero==1?0.f:-0.f;
    }
#endif
#ifdef GG_EQUAL_BOX_TEST
    if(cfg.equalBoxes) {
        require(geometry.bottom.size()==1 && geometry.bottom[0].links[3]==2,"two-triangle synthetic BLAS leaf");
        auto parent=geometry.bottom[0],left=parent,right=parent;left.links[3]=1;right.links[2]+=1;right.links[3]=1;
        parent.links[0]=1;parent.links[1]=2;parent.links[3]=0;geometry.bottom={parent,left,right};
        // Ensure the real receiver-to-probe ray enters every equal box; the
        // original distant triangle remains behind the ray and must be clear.
        for(auto& n:geometry.bottom)for(u32 k=0;k<3;++k){n.lo[k]=-10.f;n.hi[k]=10.f;}
        if(cfg.zeroBounds)for(auto& n:geometry.bottom)n.hi[2]=cfg.zeroBounds==1?0.f:-0.f;
        if(cfg.equalMask!=3) {geometry.bottom[0].hi[0]=11.f;if(cfg.equalMask&1)geometry.bottom[1].hi[0]=11.f;if(cfg.equalMask&2)geometry.bottom[2].hi[0]=11.f;}
        for(auto& n:geometry.top)for(u32 k=0;k<3;++k){n.lo[k]=-10.f;n.hi[k]=10.f;}
    }
#endif
    if(cfg.geometry==Geometry::QuerySkinny) {
        // The isolated probe-7 segment crosses the deliberately expanded TLAS
        // and BLAS boxes and the triangle plane. Independently check it lies
        // below this triangle's y>=x region, with a nonzero local normal.
        const auto& t=geometry.triangles[0];
        const float crossZ=t.e1[0]*t.e2[1]-t.e1[1]*t.e2[0];
        const float f=(.75f-.502f)/(1.f-.502f);
        const float x=.02f+(.98f*f), y=-.02f+(1.02f*f);
        require(geometry.top.size()==1 && geometry.bottom.size()==1 && crossZ>0.f &&
                x>0.f && x<1.f && y<x && geometry.top[0].lo[0]<x && geometry.top[0].hi[0]>x &&
                geometry.bottom[0].lo[1]<y && geometry.bottom[0].hi[1]>y,
                "skinny triangle: nondegenerate and isolated segment enters both broad leaves but misses triangle");
    }
    if(cfg.geometry==Geometry::Parallel&&!cfg.large&&!cfg.shortSegment){
        // Actual physical full-res receiver is (0.02,-0.02,0.502) and isolated
        // probe 7 is (1,1,1). Verify its segment intersects both TLAS and BLAS
        // leaf bounds in their respective coordinate systems, before GPU use.
        const float p0[3]={.02f,-.02f,.502f};const float p1[3]={1.f,1.f,1.f};
        auto insideBox=[&](const ray_geometry::Node& box,bool local){float lo=0,hi=1;for(int k=0;k<3;++k){float scale=cfg.scaled?(k==0?2.f:(k==1?.5f:3.f)):1.f;float a=p0[k]/(local?scale:1.f),b=p1[k]/(local?scale:1.f);float delta=b-a;if(std::abs(delta)<1e-12f){if(a<box.lo[k]||a>box.hi[k])return false;continue;}float l=(box.lo[k]-a)/delta,h=(box.hi[k]-a)/delta;if(l>h)std::swap(l,h);lo=std::max(lo,l);hi=std::min(hi,h);}return lo<=hi;};
        require(geometry.top.size()==1&&geometry.bottom.size()==1&&insideBox(geometry.top[0],false)&&insideBox(geometry.bottom[0],true),"actual ray must enter TLAS and BLAS leaf");
        const auto& t=geometry.triangles[0];float nrm[3]={t.e1[1]*t.e2[2]-t.e1[2]*t.e2[1],t.e1[2]*t.e2[0]-t.e1[0]*t.e2[2],t.e1[0]*t.e2[1]-t.e1[1]*t.e2[0]};float d[3]={.98f,1.02f,.498f};float len=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);float dot=0,worldN2=0;for(int k=0;k<3;++k){float scale=cfg.scaled?(k==0?2.f:(k==1?.5f:3.f)):1.f;dot+=d[k]/len/scale*nrm[k];worldN2+=nrm[k]*nrm[k]/(scale*scale);}require(std::abs(dot)<=1e-6f*std::sqrt(worldN2),"near-parallel world-metre threshold");std::cout<<"{\"leaf_certificate\":true,\"scaled\":"<<cfg.scaled<<",\"abs_dot\":"<<std::abs(dot)<<",\"threshold\":"<<1e-6f*std::sqrt(worldN2)<<"}\n";
    }
    if(!cfg.invalidOriginXYZ&&(cloth||cfg.geometry==Geometry::Target||cfg.geometry==Geometry::Closed||cfg.geometry==Geometry::Clear)){
        float origin[3],dir[3];float dist=0;for(u32 k=0;k<3;++k){origin[k]=(cloth?GatherWitness::Receivers[witness][k]:p[k])+n[k]*spacing*.05f;dir[k]=origins[7*4+k]-origin[k];dist+=dir[k]*dir[k];}dist=std::sqrt(dist);for(float& v:dir)v/=dist;ray_geometry::Hit hit;
        require(ray_geometry::Trace(geometry,origin,dir,.001f,dist-.001f,hit)==(cfg.geometry==Geometry::Clear?0:1),"Historical Ns-biased diagnostic segment oracle");
        if(cloth){float d2=0;for(u32 k=0;k<3;++k){origin[k]=p[k]+(k==1?.002f:0.f);dir[k]=origins[7*4+k]-origin[k];d2+=dir[k]*dir[k];}d2=std::sqrt(d2);for(float& v:dir)v/=d2;require(ray_geometry::Trace(geometry,origin,dir,0.f,d2,hit)==0,"Actual archived surface+depthNg2mm must be clear, unlike historical Ns15cm");}
    }
#ifdef GG_CONNECTION_UPPER_TEST
    ray_geometry::SceneBVH upper;ray_geometry::UpperBVHStats upperStats{};
    const bool upperBuilt=ray_geometry::BuildUpperSAH16(geometry,upper,upperStats);
    if(cfg.geometry==Geometry::UpperReordered||cfg.geometry==Geometry::UpperBlocking||cfg.geometry==Geometry::UpperParallel||cfg.geometry==Geometry::UpperMultiple||cfg.geometry==Geometry::UpperDeep||cfg.geometry==Geometry::UpperMixed||cfg.geometry==Geometry::UpperCentroidOpen||cfg.geometry==Geometry::UpperCentroidBlocked)require(upperBuilt && upperStats.roots_changed>0 && upperStats.original_payload_hash!=upperStats.candidate_payload_hash,"expected real internal-node rearrangement");
    if(cfg.geometry==Geometry::UpperRejected||cfg.geometry==Geometry::UpperDeepRejected)require(!upperBuilt && upper.bottom.empty(),"invalid candidate must reject and fall back");
    if(upperBuilt){
        require(upper.top.size()==geometry.top.size()&&upper.bottom.size()==geometry.bottom.size()&&upper.triangles.size()==geometry.triangles.size()&&upper.instances.size()==geometry.instances.size(),"candidate sizes");
        require(std::memcmp(upper.top.data(),geometry.top.data(),upper.top.size()*sizeof(ray_geometry::Node))==0&&std::memcmp(upper.triangles.data(),geometry.triangles.data(),upper.triangles.size()*sizeof(ray_geometry::Triangle))==0&&std::memcmp(upper.instances.data(),geometry.instances.data(),upper.instances.size()*sizeof(ray_geometry::Instance))==0,"TLAS/triangle/instance bytes preserved");
        for(size_t i=0;i<geometry.bottom.size();++i)if(geometry.bottom[i].links[3])require(std::memcmp(&geometry.bottom[i],&upper.bottom[i],sizeof(ray_geometry::Node))==0,"leaf index and bytes preserved");
    }
    if(cfg.geometry==Geometry::UpperCentroidOpen||cfg.geometry==Geometry::UpperCentroidBlocked){
        // The full texel receiver and probe 7, reconstructed by the existing
        // view/geometry path. Slab certificates are auxiliary only: GPU still
        // performs its own real traversal and full-output equality test.
        const double from[3]={.02,-.02,.502},to[3]={1.,1.,1.};
        auto intersects=[&](const ray_geometry::Node& n){double lo=0,hi=1;for(int k=0;k<3;++k){double d=to[k]-from[k];if(d==0){if(from[k]<n.lo[k]||from[k]>n.hi[k])return false;}else{double a=(n.lo[k]-from[k])/d,b=(n.hi[k]-from[k])/d;if(a>b)std::swap(a,b);lo=std::max(lo,a);hi=std::min(hi,b);if(lo>hi)return false;}}return true;};
        // BVH::UpdateNodeBounds expands triangle bounds by 1e-5; obtain
        // the first near leaf from the constructed topology rather than
        // assuming its minimum equals the unexpanded vertex coordinate -2.
        const auto& reference=geometry.bottom[geometry.bottom[geometry.instances[0].root].links[0]];
        require(reference.links[3] && intersects(reference),"first near leaf intersects selected segment");
        auto sameBounds=[&](const ray_geometry::Node& n){for(u32 k=0;k<3;++k)if(n.lo[k]!=reference.lo[k]||n.hi[k]!=reference.hi[k])return false;return true;};
        auto count=[&](const ray_geometry::SceneBVH& scene){require(intersects(scene.top[0]),"centroid ray enters TLAS");u32 internal=0,nearLeaves=0;std::vector<u32> stack{scene.instances[0].root};while(!stack.empty()){u32 i=stack.back();stack.pop_back();const auto& n=scene.bottom[i];if(!intersects(n))continue;if(!n.links[3]){++internal;stack.push_back(n.links[0]);stack.push_back(n.links[1]);}else if(sameBounds(n))++nearLeaves;}require(internal>0&&nearLeaves==4,"ray enters BLAS internal and all four coincident leaves");return internal;};
        u32 coincident=0;for(const auto& n:geometry.bottom)if(n.links[3]&&sameBounds(n))++coincident;require(coincident==4,"four coincident leaf centroids");
        auto before=count(geometry),after=count(upper);std::cout<<"{\"centroid_certificate\":true,\"leaves\":"<<coincident<<",\"original_internal\":"<<before<<",\"candidate_internal\":"<<after<<"}\n";
    }
    std::cout<<"{\"upper_built\":"<<upperBuilt<<",\"roots_changed\":"<<upperStats.roots_changed<<",\"original_pending\":"<<upperStats.original_max_pending<<",\"candidate_pending\":"<<upperStats.candidate_max_pending<<"}\n";
#endif
#ifdef GG_INSTANCE_CACHE_TEST
    if(cfg.mirror)geometry.instances[0].inverse_rows[0][0]*=-1.f;
    if(cfg.sharedRoot){geometry.instances.push_back(geometry.instances[0]);auto leaf=geometry.top[0];leaf.links[2]=1;geometry.top.push_back(leaf);geometry.top.push_back(geometry.top[0]);geometry.top[0].links[0]=1;geometry.top[0].links[1]=2;geometry.top[0].links[3]=0;}
#endif
    std::vector<float> depths(width*2*height*2,.5f);std::vector<std::array<float,4>> normals(depths.size(),{n[0]*.5f+.5f,n[1]*.5f+.5f,n[2]*.5f+.5f,1}),orm(depths.size(),{1,1,0,1});
#ifdef GG_CONNECTION_TRAVERSAL_TEST
    if(cfg.background)std::fill(depths.begin(),depths.end(),1.f);
#endif
    if(cfg.nonneutralNormal)for(auto& pixel:normals)pixel={.6f,.5f,.98989795f,1};
    if(cfg.invalidSurface==1)depths[width*2]=std::numeric_limits<float>::quiet_NaN(); // Only available X neighbor.
    if(cfg.invalidSurface==2){depths[width*2]=.9f;depths[1]=.9f;}
    const std::array<u16,4> black{};
    auto depth=g.texture(width*2,height*2,DataFormat::D32_Float,depths.data(),depths.size()*4),normal=g.texture(width*2,height*2,DataFormat::RGBA32_Float,normals.data(),normals.size()*16),material=g.texture(width*2,height*2,DataFormat::RGBA32_Float,orm.data(),orm.size()*16),history=g.texture(1,1,DataFormat::RGBA16_Float,black.data(),8);
    ResourceHandle diffuse[2],spec[2];for(u32 i=0;i<2;++i){
#ifdef GG_CACHELESS_TEST
        // Distinct quiet-NaN bit patterns cannot be mistaken for any legal finite result.
        std::vector<u16> diffuseSentinel(width*height*4,u16(0x7e01u+i));
        std::vector<u16> specSentinel(width*height*4,u16(0x7e11u+i));
        diffuse[i]=g.texture(width,height,DataFormat::RGBA16_Float,diffuseSentinel.data(),diffuseSentinel.size()*sizeof(u16),true);
        spec[i]=g.texture(width,height,DataFormat::RGBA16_Float,specSentinel.data(),specSentinel.size()*sizeof(u16),true);
#else
        diffuse[i]=g.texture(width,height,DataFormat::RGBA16_Float,nullptr,0);spec[i]=g.texture(width,height,DataFormat::RGBA16_Float,nullptr,0);
#endif
    }
    auto viewBuffer=g.buffer(sizeof(view),BufferType::Constant,GPUMemoryUsage::Dynamic,&view),volumeBuffer=g.buffer(sizeof(volume),BufferType::Constant,GPUMemoryUsage::Dynamic,&volume);
    std::array<u8,176> staticProbe{};std::array<float,36> staticSH{};std::array<float,8> staticFactor{};
    auto staticCB=g.buffer(sizeof(staticProbe),BufferType::Constant,GPUMemoryUsage::Dynamic,staticProbe.data()),sky=g.buffer(sizeof(staticSH),BufferType::Raw,GPUMemoryUsage::Dynamic,staticSH.data()),skyFactor=g.buffer(sizeof(staticFactor),BufferType::Raw,GPUMemoryUsage::Dynamic,staticFactor.data());
    auto nearSH=g.buffer(sizeof(sh),BufferType::Raw,GPUMemoryUsage::Dynamic,sh.data()),depthBuffer=g.buffer(sizeof(moments),BufferType::Raw,GPUMemoryUsage::Dynamic,moments.data()),originBuffer=g.buffer(sizeof(origins),BufferType::Raw,GPUMemoryUsage::Dynamic,origins.data()),conf=g.buffer(sizeof(confidence),BufferType::Raw,GPUMemoryUsage::Dynamic,confidence.data());
    std::vector<u8> flat(256+sizeof(sh),0xcd),flatOpen(256+sizeof(open),0xcd);std::memcpy(flat.data()+256,sh.data(),sizeof(sh));std::memcpy(flatOpen.data()+256,open.data(),sizeof(open));
    auto flatBuffer=g.buffer(flat.size(),BufferType::Raw,GPUMemoryUsage::Dynamic,flat.data()),openBuffer=g.buffer(flatOpen.size(),BufferType::Raw,GPUMemoryUsage::Dynamic,flatOpen.data());
    struct Policy {u32 values[4];float jitter[4];};static_assert(sizeof(Policy)==32);
    const Policy policy{{cfg.ready?1u:0u,13,1,0},{cfg.jitterX,cfg.jitterY,0,0}};auto policyBuffer=g.buffer(sizeof(policy),BufferType::Constant,GPUMemoryUsage::Dynamic,&policy);
    ResourceHandle geometryBuffers[]={g.buffer(geometry.top.size()*sizeof(ray_geometry::Node),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.top.data()),g.buffer(geometry.bottom.size()*sizeof(ray_geometry::Node),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.bottom.data()),g.buffer(geometry.triangles.size()*sizeof(ray_geometry::Triangle),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.triangles.data()),g.buffer(geometry.instances.size()*sizeof(ray_geometry::Instance),BufferType::Raw,GPUMemoryUsage::Dynamic,geometry.instances.data())};
#ifdef GG_EQUAL_BOX_TEST
    auto equalCert=ray_geometry::TryBuildEqualBoxSidecar(geometry,16ull*1024*1024);
    if(cfg.corruptFlags && equalCert){auto damaged=std::make_shared<ray_geometry::EqualBoxSidecar>(*equalCert);damaged->flags[0]^=1;equalCert=damaged;}
    const bool equalReady=equalCert && ray_geometry::EqualBoxSidecarIntact(*equalCert);
    if(cfg.corruptFlags)require(!equalReady,"tampered pre-upload flags select original shader");
    if(cfg.equalBoxes && !cfg.corruptFlags)require(equalReady && equalCert->flags[equalCert->top_count]==cfg.equalMask,"equal-box edge bits certified");
    const u32 dummyFlags[4]{};
    auto equalFlags=g.buffer(equalReady?equalCert->flags.size()*sizeof(u32):sizeof(dummyFlags),BufferType::Raw,GPUMemoryUsage::Dynamic,equalReady?static_cast<const void*>(equalCert->flags.data()):static_cast<const void*>(dummyFlags));
    if(!equalReady)std::cout << "[DDGI_EQUAL_BOX_FIXTURE] fallback=invalid_source\n";
#endif
#ifdef GG_CONNECTION_UPPER_TEST
    ResourceHandle candidateBottom=upperBuilt?g.buffer(upper.bottom.size()*sizeof(ray_geometry::Node),BufferType::Raw,GPUMemoryUsage::Dynamic,upper.bottom.data()):geometryBuffers[1];
#endif
#ifdef GG_CONNECTION_TRAVERSAL_TEST
    traversal_audit::Selection selection{};std::vector<traversal_audit::Pixel> pixels,roi;
    for(u32 x=0;x<width;++x)roi.push_back({x,0});
    require(traversal_audit::BuildTraversalAuditSelection(width,height,0,0,roi,13,1,selection,pixels),"ROI-only selection");
    const u64 auditBytes=pixels.size()*traversal_audit::RecordWords*sizeof(u32);
    std::vector<u32> sentinel(auditBytes/4,0xffffffffu);
    auto auditCB=g.buffer(sizeof(selection),BufferType::Constant,GPUMemoryUsage::Dynamic,&selection);
    auto auditBuffer=g.buffer(auditBytes,BufferType::Raw,GPUMemoryUsage::Dynamic,sentinel.data());
    auto auditReadback=g.buffer(auditBytes,BufferType::Raw,GPUMemoryUsage::Readback);
#endif
#ifdef GG_INSTANCE_CACHE_TEST
    GIGatherInputs cacheInputs{};cacheInputs.current_buffer_index=0;cacheInputs.frame_index=13;cacheInputs.geometry_ready=true;
    cacheInputs.geometry_epoch=1;cacheInputs.geometry_source_digest=123;cacheInputs.geometry_still_current=[] {return true;};
    const u64 geoSizes[]={geometry.top.size()*sizeof(ray_geometry::Node),geometry.bottom.size()*sizeof(ray_geometry::Node),geometry.triangles.size()*sizeof(ray_geometry::Triangle),geometry.instances.size()*sizeof(ray_geometry::Instance)};
    for(u32 i=0;i<4;++i){cacheInputs.geometry[i]=geometryBuffers[i];cacheInputs.geometry_sizes[i]=geoSizes[i];}
    cacheInputs.geometry_normal_owners=std::make_shared<ray_geometry::InstanceNormalOwnerCertificate>(ray_geometry::CertifyInstanceNormalOwners(geometry,128ull*1024*1024,53ull*1024*1024));
    // All uploads above have actually completed, including their device fence.
    require(g.d->FrameSlotCompletions().Publish(0),"Publish actual completed fixture slot");
    const bool prepared=g.cache.Prepare(cacheInputs);
    if(cfg.sharedRoot)require(!prepared,"shared root falls back to original pipeline");
    if(prepared&&!cfg.skipProducer){CommandBufferHandle h;auto* c=g.begin(h);g.cache.Record(c,cacheInputs);g.finish(h,c);require(g.d->FrameSlotCompletions().Publish(0),"Publish actual cache submission completion");require(g.cache.Available(cacheInputs),"Host cache available after real producer/verifier");}
    // Read real flags only in this fixture, after completion. No production readback.
    if(prepared&&!cfg.skipProducer){const u64 bytes=geometry.triangles.size()*4;auto rb=g.buffer(bytes,BufferType::Raw,GPUMemoryUsage::Readback);CommandBufferHandle h;auto* c=g.begin(h);
        c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);c->CopyBuffer(g.cache.Buffer(0,2),rb,0,0,bytes);c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);g.finish(h,c);
        auto* flags=static_cast<const u32*>(g.d->MapBuffer(rb,0,bytes));require(flags,"Read actual verification flags");u32 verified=0;for(u32 i=0;i<geometry.triangles.size();++i){require(flags[i]<=1,"every flag written or safely invalid");verified+=flags[i];}g.d->UnmapBuffer(rb);
        if(cfg.geometry!=Geometry::QueryDegenerate)require(verified==geometry.triangles.size(),"all finite fixture normals verified bitwise");
        std::cout<<"{\"cache_prepared\":true,\"gpu_verified\":"<<verified<<",\"triangles\":"<<geometry.triangles.size()<<"}\n";
    }
    ResourceHandle cacheBuffers[3]{};
    if(prepared)for(u32 i=0;i<3;++i)cacheBuffers[i]=g.cache.Buffer(0,i);
    else {const u32 zero[4]={};for(auto& b:cacheBuffers)b=g.buffer(16,BufferType::Raw,GPUMemoryUsage::Dynamic,zero);}
    if(prepared&&cfg.wrongOwner){std::vector<u32> invalid(geometry.triangles.size(),0xffffffffu);require(g.d->UpdateBufferData(cacheBuffers[0],invalid.data(),invalid.size()*4,0),"fixture deliberately invalidates owner after completion");}
#endif
    for(u32 phase=0;phase<2;++phase){
        DescriptorImageInfo images[6]={{handles::INVALID_SAMPLER,depth,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,normal,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,diffuse[phase],ResourceState::UnorderedAccess},{handles::INVALID_SAMPLER,history,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,material,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,spec[phase],ResourceState::UnorderedAccess}};
        const u32 imageBindings[]={0,1,2,3,13,14};std::vector<WriteDescriptorSet> writes;
        for(u32 i=0;i<6;++i)writes.push_back({g.sets[phase],imageBindings[i],0,1,i==0?DescriptorType::SampledDepthImage:(i==2||i==5?DescriptorType::StorageImage:DescriptorType::SampledImage),&images[i],nullptr});
        const u32 bindings[]={4,5,6,7,8,9,10,11,12,17,18,19,20,23,24,25,26,27};
        DescriptorBufferInfo buffers[]={{viewBuffer,0,64},{viewBuffer,64,16},{viewBuffer,80,48},{sky,0,sizeof(staticSH)},{skyFactor,0,sizeof(staticFactor)},{staticCB,0,sizeof(staticProbe)},{conf,0,sizeof(confidence)},{nearSH,0,sizeof(sh)},{depthBuffer,0,sizeof(moments)},{flatBuffer,256,sizeof(sh)},{volumeBuffer,0,sizeof(volume)},{originBuffer,0,sizeof(origins)},{openBuffer,256,sizeof(open)},
            {geometryBuffers[0],0,geometry.top.size()*sizeof(ray_geometry::Node)},{geometryBuffers[1],0,geometry.bottom.size()*sizeof(ray_geometry::Node)},{geometryBuffers[2],0,geometry.triangles.size()*sizeof(ray_geometry::Triangle)},{geometryBuffers[3],0,geometry.instances.size()*sizeof(ray_geometry::Instance)},{policyBuffer,0,sizeof(policy)}};
#ifdef GG_CONNECTION_UPPER_TEST
#if defined(GG_CONNECTION_TRAVERSAL_TEST) || defined(GG_CONNECTION_QUERY_KERNEL_TEST)
        // Both dispatches use the same actual private BLAS selection.
        buffers[14].buffer=candidateBottom;
#else
        if(phase==1)buffers[14].buffer=candidateBottom;
#endif
#endif
        for(u32 i=0;i<18;++i){u32 n=bindings[i];auto type=n==4||n==5||n==6||n==9||n==18||n==27?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;writes.push_back({g.sets[phase],n,0,1,type,nullptr,&buffers[i]});}
#ifdef GG_CONNECTION_TRAVERSAL_TEST
        DescriptorBufferInfo auditInfos[]={{auditCB,0,sizeof(selection)},{auditBuffer,0,auditBytes}};
        writes.push_back({g.sets[phase],28,0,1,DescriptorType::UniformBuffer,nullptr,&auditInfos[0]});
        writes.push_back({g.sets[phase],29,0,1,DescriptorType::StorageBuffer,nullptr,&auditInfos[1]});
#endif
#ifdef GG_EQUAL_BOX_TEST
        DescriptorBufferInfo equalInfo{equalFlags,0,equalReady?equalCert->flags.size()*sizeof(u32):sizeof(dummyFlags)};
        writes.push_back({g.sets[phase],28,0,1,DescriptorType::StorageBuffer,nullptr,&equalInfo});
#endif
#ifdef GG_INSTANCE_CACHE_TEST
        DescriptorBufferInfo cacheInfos[3]{};for(u32 i=0;i<3;++i){cacheInfos[i]={cacheBuffers[i],0,prepared?u64(geometry.triangles.size())*(i==1?16:4):16};writes.push_back({g.sets[phase],28+i,0,1,DescriptorType::StorageBuffer,nullptr,&cacheInfos[i]});}
#endif
        g.d->UpdateDescriptorSets(writes.size(),writes.data());CommandBufferHandle handle;auto* c=g.begin(handle);
        c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);c->BindComputePipeline(g.pipes[
#ifdef GG_INSTANCE_CACHE_TEST
            phase==1&&prepared?1:0
#elif defined(GG_EQUAL_BOX_TEST)
            phase==1&&equalReady?1:0
#else
            phase
#endif
        ]);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[phase],0,nullptr);c->Dispatch((width+7)/8,
#ifdef GG_CACHELESS_TEST
        phase==1?(height+GG_CACHELESS_LOCAL_Y-1)/GG_CACHELESS_LOCAL_Y:
#endif
        (height+7)/8,1);
        g.barrier(c,diffuse[phase],ResourceState::UnorderedAccess,ResourceState::ShaderResource);g.barrier(c,spec[phase],ResourceState::UnorderedAccess,ResourceState::ShaderResource);
#ifdef GG_CONNECTION_TRAVERSAL_TEST
        if(phase==1){
            c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
            c->CopyBuffer(auditBuffer,auditReadback,0,0,auditBytes);
            c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        }
#endif
        g.finish(handle,c);
    }
    Result out;out.baseline={g.read(diffuse[0],width,height),g.read(spec[0],width,height)};out.candidate={g.read(diffuse[1],width,height),g.read(spec[1],width,height)};
#ifdef GG_CACHELESS_TEST
    for(u32 phase=0;phase<2;++phase)for(u32 output=0;output<2;++output) {
        const auto& image=phase?(output?out.candidate.specular:out.candidate.diffuse):(output?out.baseline.specular:out.baseline.diffuse);
        const u16 sentinel=u16((output?0x7e11u:0x7e01u)+phase);
        for(const auto& pixel:image)for(u16 channel:pixel.raw)require(channel!=sentinel,"each output channel overwrites its own sentinel");
    }
#endif
    for(const auto* pair:{&out.baseline,&out.candidate})for(const auto* data:{&pair->diffuse,&pair->specular})for(auto p:*data)for(float x:{p.r,p.g,p.b,p.a})require(finite(x),"Actual GG diffuse/glossy RGBA finite");
#ifdef GG_CONNECTION_TRAVERSAL_TEST
    const auto* mapped=static_cast<const u32*>(g.d->MapBuffer(auditReadback,0,auditBytes));require(mapped,"Map traversal records");
    out.audit.assign(mapped,mapped+auditBytes/4);g.d->UnmapBuffer(auditReadback);
    for(size_t i=0;i<pixels.size();++i){
        const auto* r=out.audit.data()+i*traversal_audit::RecordWords;
        require(r[0]==traversal_audit::Magic&&r[1]==13&&r[2]==1&&r[3]==i&&r[4]==pixels[i].x&&r[5]==pixels[i].y,"Every selected lane stamped once with exact frame/slot/pixel");
        require(r[6]==(cfg.ready?1u:2u)&&r[7]<=8,"Status and query bound");
        if(cfg.background)require(r[8]==0&&r[9]==0xffffffffu&&r[7]==0,"Background classified without calls");
        else if(cfg.mode!=7)require(r[8]==1&&r[7]==0,"Legacy mode classified without geometry calls");
        else if(cfg.outsideVolume)require(r[8]==2&&r[9]==0xffffffffu&&r[7]==0,"Outside volume classified without calls");
        else require(r[8]==4&&r[9]==0,"Real near-volume invocation classified");
        u32 calls=0;
        for(u32 q=0;q<8;++q){const auto* v=r+16+q*16;
            if(v[1]==0xffffffffu)continue;
            ++calls;require(v[0]<8&&v[1]<=1&&v[2]>=3&&v[2]<=5,"Recorded result/reason/probe valid");
            require(!(v[7]&1u),"No saturated GPU counter");
            require((v[1]==1)==(v[2]==4),"OPEN mapping exact");
            for(u32 k=8;k<14;++k)require(finite(std::bit_cast<float>(v[k])),"Exact ray endpoint bits finite");
            if(!cfg.ready)require(v[3]==0&&v[4]==0&&v[5]==0&&v[6]==0,"Unready policy cannot traverse");
        }
        require(calls==r[7],"Query count equals initialized slots");
        if(cfg.mode!=7||cfg.originState==0)require(calls==0,"No-query receiver still stamped");
        if(cfg.mode==7&&cfg.geometry==Geometry::Clear&&cfg.ready&&!cfg.scan&&!cfg.invalidSurface&&!cfg.invalidOriginXYZ&&cfg.originState==1&&!cfg.background&&!cfg.outsideVolume&&cfg.mean>=0&&cfg.variance>=0&&finite(cfg.jitterX)){
            require(calls==8,"Eight qualified original calls preserved");
            for(u32 q=0;q<8;++q){const auto* v=r+16+q*16;require(v[1]==1&&v[3]==1&&v[4]==0&&v[5]==0&&v[6]==0,"Known OPEN miss performs exactly one TLAS box and no other work");}
        }
        if(cfg.mode==7&&cfg.geometry==Geometry::Malformed)for(u32 q=0;q<8;++q){const auto* v=r+16+q*16;require(v[2]==3&&(v[7]&2u),"Invalid index UNKNOWN flag");}
        if(cfg.mode==7&&cfg.geometry==Geometry::Cycle)for(u32 q=0;q<8;++q){const auto* v=r+16+q*16;require(v[2]==3&&(v[7]&4u),"Stack overflow UNKNOWN flag");}
    }
#endif
    return out;
}
void zero(const Pair& x,bool unsupported){for(const auto* image:{&x.diffuse,&x.specular})for(auto p:*image){require(p.r==0&&p.g==0&&p.b==0,"Blocked/unknown must have exact zero diffuse/glossy");if(unsupported)require(p.a==0,"No eligible support: diffuse/glossy alpha zero");}}
void identical(const Result& x){for(auto pair:{std::pair{&x.baseline.diffuse,&x.candidate.diffuse},std::pair{&x.baseline.specular,&x.candidate.specular}}){require(pair.first->size()==pair.second->size(),"GPU output dimensions match");for(size_t i=0;i<pair.first->size();++i)require(std::memcmp((*pair.first)[i].raw,(*pair.second)[i].raw,sizeof(Pixel::raw))==0,"raw RGBA16F outputs remain bit-identical");}}
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
    auto add=[&](const std::string& name,Config c,auto validate){suite->AddTestCase(TestCase(name,[=]{auto r=dispatch(c);std::cout<<"{\"case\":\""<<name<<"\",\"gate_r\":"<<r.baseline.diffuse[0].r<<",\"replace_r\":"<<r.candidate.diffuse[0].r<<"}\n";
#ifdef GG_CONNECTION_EQUIVALENCE
        identical(r);
#endif
        validate(r);return TestResult::Passed;}));};
#ifdef GG_CACHELESS_TEST
    Config odd;odd.oddGrid=true;add("odd_grid_full_output",odd,[](const Result&r){require(r.candidate.diffuse.size()==17*9,"all odd pixels read");for(const auto& p:r.candidate.diffuse)require(p.r>0,"all odd-grid pixels visible");});
#endif
#ifdef GG_EQUAL_BOX_TEST
    Config equal;equal.equalBoxes=true;add("both_equal_children_certified",equal,[](const Result&r){require(r.candidate.diffuse[0].r>0,"equal children complete OPEN output");});
    equal.equalMask=1;add("left_equal_child_certified",equal,[](const Result&r){require(r.candidate.diffuse[0].r>0,"left equal child OPEN");});
    equal.equalMask=2;add("right_equal_child_certified",equal,[](const Result&r){require(r.candidate.diffuse[0].r>0,"right equal child OPEN");});
    equal.equalMask=0;add("non_equal_children_certified",equal,[](const Result&r){require(r.candidate.diffuse[0].r>0,"non equal children OPEN");});
    equal.corruptFlags=true;add("equal_flags_tampered_fallback",equal,[](const Result&r){require(r.candidate.diffuse[0].r>0,"tampered flags use original OPEN");});
    equal={};equal.equalBoxes=true;equal.zeroBounds=1;add("equal_positive_zero_bounds",equal,[](const Result&r){require(r.candidate.diffuse[0].r>0,"positive-zero bound case OPEN");});
    equal.zeroBounds=2;add("equal_negative_zero_bounds",equal,[](const Result&r){require(r.candidate.diffuse[0].r>0,"negative-zero bound case OPEN");});
    Config deep;deep.geometry=Geometry::DeepTLAS;deep.deepCount=63;add("equal_tlas_pending63",deep,[](const Result&r){require(r.candidate.diffuse[0].r>0,"63-pending TLAS remains open");});
    deep.geometry=Geometry::DeepBLAS;add("equal_blas_pending63",deep,[](const Result&r){require(r.candidate.diffuse[0].r>0,"63-pending BLAS remains open");});
    deep.deepCount=64;deep.geometry=Geometry::DeepTLAS;add("equal_tlas_depth64_fallback",deep,[](const Result&r){require(r.candidate.diffuse[0].r>0,"64-depth TLAS falls back to original");});
    deep.geometry=Geometry::DeepBLAS;add("equal_blas_depth64_fallback",deep,[](const Result&r){require(r.candidate.diffuse[0].r>0,"64-depth BLAS falls back to original");});
#endif
#if defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
    for(u32 count:{1u,2u,4u,5u}){Config c;c.payloadInstanceCount=count;add("payload_instance_count_"+std::to_string(count),c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"payload fixture shared BLAS stays open");});}
    {Config c;c.payloadInstanceCount=2;c.payloadMirror=true;add("payload_shared_blas_mirrored_instance",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"mirrored same-BLAS instance remains open");});}
    for(u32 sign:{1u,2u}){Config c;c.payloadInstanceCount=4;c.payloadSignedZero=sign;add("payload_bounds_signed_zero_"+std::to_string(sign),c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"signed zero bounds preserve open output");});}
    for(u32 depth:{63u,64u})for(auto tree:{Geometry::DeepTLAS,Geometry::DeepBLAS}){Config c;c.geometry=tree;c.payloadDeepCount=depth;add(std::string("payload_stack_")+(tree==Geometry::DeepTLAS?"top_":"bottom_")+std::to_string(depth),c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"deep traversal output remains open");});}
    for(u32 active=0;active<=8;++active){Config c;c.payloadInstanceCount=4;c.activeProbes=active;add("payload_active_probes_"+std::to_string(active),c,[=](const Result&r){if(active)require(r.candidate.diffuse[0].r>0,"active probes retain visible output");else zero(r.candidate,true);});}
#endif
    Config c;c.mean=.1f;c.variance=0;add("open_low_finite_moment_recovers",c,[](const Result&r){
#ifdef GG_CONNECTION_EQUIVALENCE
        require(r.candidate.diffuse[0].r>0,"OPEN low moment remains positive");
#else
        require(r.candidate.diffuse[0].r>r.baseline.diffuse[0].r+1e-5f,"OPEN must recover low finite moment");
#endif
    });
    c={};c.geometry=Geometry::Closed;add("blocked_favorable_moments",c,[](const Result&r){zero(r.candidate,true);});
    c.geometry=Geometry::Sheet;c.allBright=false;c.strength=100;add("floor_sheet_blocks",c,[](const Result&r){zero(r.candidate,false);});
    c.geometry=Geometry::ReverseSheet;add("cloth_reversed_winding_blocks",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::Target;c.allBright=false;c.strength=100;add("thin_double_sided_blocker",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::Endpoint;c.allBright=false;add("endpoint_not_open",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.ready=false;add("not_ready_unknown",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.geometry=Geometry::Malformed;add("malformed_unknown",c,[](const Result&r){zero(r.candidate,true);});
    c.geometry=Geometry::Cycle;add("cycle_unknown",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.originState=0;add("invalid_origin",c,[](const Result&r){zero(r.candidate,true);});
    c.originState=std::numeric_limits<float>::quiet_NaN();add("nan_origin",c,[](const Result&r){zero(r.candidate,true);});
    c.originState=std::numeric_limits<float>::infinity();add("inf_origin_state",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.invalidOriginXYZ=true;add("nan_origin_xyz",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.mean=-1;add("unknown_moments",c,[](const Result&r){zero(r.candidate,true);});
    c.mean=std::numeric_limits<float>::quiet_NaN();add("nan_mean",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.variance=-1;add("negative_variance",c,[](const Result&r){zero(r.candidate,true);});
    c.variance=std::numeric_limits<float>::quiet_NaN();add("nan_variance",c,[](const Result&r){zero(r.candidate,true);});
    for(u32 i=1;i<=3;++i){c={};c.invalidSurface=i;add("invalid_receiver_"+std::to_string(i),c,[](const Result&r){zero(r.candidate,true);});}
    c={};c.jitterX=std::numeric_limits<float>::quiet_NaN();add("invalid_jitter",c,[](const Result&r){zero(r.candidate,true);});
    c={};c.originState=.5f;add("borrowed_favorable_moments",c,[](const Result&r){identical(r);Config local;auto l=dispatch(local);require(std::abs(r.candidate.diffuse[0].r*2-l.candidate.diffuse[0].r)<.001f,"borrowed source carries half local weight");});
    c={};c.mean=.1f;c.variance=0;c.nonneutralNormal=true;add("nonneutral_normal_open_low_moment",c,[](const Result&r){
#ifdef GG_CONNECTION_EQUIVALENCE
        require(r.candidate.diffuse[0].r>0,"nonneutral normal texture remains positive");
#else
        require(r.candidate.diffuse[0].r>r.baseline.diffuse[0].r+1e-5f,"nonneutral normal texture stays on actual path");
#endif
    });
    c={};c.scan=true;c.allBright=false;c.strength=100;add("positive_tw_below_point01",c,[](const Result&r){for(auto p:r.candidate.diffuse)require(p.r>0,"positive tw must not be clipped at .01");for(u32 i=1;i<5;++i)require(r.candidate.diffuse[i].r>r.candidate.diffuse[i-1].r,"positive tw continuous");});
    c={};c.geometry=Geometry::EndpointPlane;c.allBright=false;c.strength=100;
    add("endpoint_exact",c,[](const Result&r){zero(r.candidate,false);});
    c.endOffset=-.00001f;add("endpoint_inner_10um_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c.endOffset=.00001f;add("endpoint_outer_10um_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c.endOffset=-.00003f;add("endpoint_inner_30um_blocked",c,[](const Result&r){zero(r.candidate,false);});
    c.endOffset=.00003f;add("endpoint_outer_30um_clear",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"outside extension must be open");});
    c={};c.geometry=Geometry::Parallel;c.allBright=false;c.strength=100;add("coplanar_visited_leaf_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c.parallelTilt=.0000002f;add("near_parallel_visited_leaf_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c.scaled=true;add("near_parallel_nonuniform_scale_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::ParallelFar;c.allBright=false;c.strength=100;add("parallel_outside_visited_leaf_open",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"unvisited leaf remains open");});
    c={};c.geometry=Geometry::ParallelFar;c.large=true;c.allBright=true;c.strength=100;add("large_coordinate_unrepresentable_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::ParallelFar;c.large=true;c.largeX=30.9f;c.allBright=true;c.strength=100;add("ulp_inside_31_9_open",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"representable below-32m source must remain open");});
    c.largeX=31.1f;add("ulp_outside_32_1_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c.largeX=-4096.f;add("ulp_negative_large_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::ParallelFar;c.shortSegment=true;c.allBright=true;c.strength=100;add("short_segment_unknown",c,[](const Result&r){zero(r.candidate,false);});
#ifdef GG_CONNECTION_EQUIVALENCE
    c={};c.geometry=Geometry::HitThenInvalid;add("hit_then_invalid_sibling",c,[](const Result&r){zero(r.candidate,true);});
    c.geometry=Geometry::InvalidThenHit;add("invalid_then_hit_sibling",c,[](const Result&r){zero(r.candidate,true);});
#endif
#ifdef GG_BOUNDS_FIRST_TEST
    c={};c.geometry=Geometry::BoundsTopMissBad;c.allBright=false;c.strength=100;
    add("bounds_top_miss_bad_link_open",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"unvisited invalid top leaf stays open");});
    c.geometry=Geometry::BoundsBottomMissBad;
    add("bounds_bottom_miss_bad_link_open",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"unvisited invalid bottom leaf stays open");});
    c.geometry=Geometry::BoundsTopHitBad;
    add("bounds_top_hit_bad_link_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c.geometry=Geometry::BoundsBottomHitBad;
    add("bounds_bottom_hit_bad_link_unknown",c,[](const Result&r){zero(r.candidate,false);});
#endif
#ifdef GG_CONNECTION_QUERY_KERNEL_TEST
    c={};c.geometry=Geometry::QueryUReject;c.allBright=false;c.strength=100;
    add("query_u_reject_broad_leaf",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"u-rejected triangle must leave the isolated connection open");});
    c.geometry=Geometry::QueryBaryReject;
    add("query_bary_reject_broad_leaf",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"barycentric-rejected triangle must leave the isolated connection open");});
    c.geometry=Geometry::QueryDegenerate;
    add("query_degenerate_broad_leaf_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c.geometry=Geometry::QuerySkinny;
    add("query_skinny_broad_leaf",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"skinny triangle miss must keep isolated connection open");});
#endif
#ifdef GG_INSTANCE_CACHE_TEST
    c={};c.geometry=Geometry::Sheet;c.allBright=false;c.skipProducer=true;add("cache_missing_producer_entries_fall_back",c,[](const Result&r){zero(r.candidate,false);});
    c.skipProducer=false;c.wrongOwner=true;add("cache_wrong_owner_entries_fall_back",c,[](const Result&r){zero(r.candidate,false);});
    c.wrongOwner=false;c.sharedRoot=true;add("cache_shared_root_original_pipeline",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.mirror=true;c.geometry=Geometry::Sheet;c.allBright=false;add("cache_mirrored_inverse",c,[](const Result&r){zero(r.candidate,false);});
#endif
#ifdef GG_CONNECTION_COMPACT_TEST
    c={};c.geometry=Geometry::DeepTLAS;add("compact_tlas_capacity_16_clear",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"exact TLAS capacity must stay OPEN");});
    c.geometry=Geometry::DeepBLAS;add("compact_blas_capacity_32_clear",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"exact BLAS capacity must stay OPEN");});
#endif
#ifdef GG_CONNECTION_UPPER_TEST
    c={};c.geometry=Geometry::UpperCentroidOpen;c.allBright=false;c.strength=100;add("upper_coincident_centroid_open",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"coincident-leaf OPEN receiver remains lit");});
    c={};c.geometry=Geometry::UpperCentroidBlocked;c.allBright=false;c.strength=100;add("upper_coincident_centroid_blocked",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::UpperReordered;add("upper_real_reorder_clear",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"reordered clear scene remains OPEN");});
    c={};c.geometry=Geometry::UpperBlocking;c.allBright=false;c.strength=100;add("upper_reordered_thin_blocker",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::UpperParallel;c.allBright=false;c.strength=100;add("upper_reordered_parallel_unknown",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::UpperMixed;c.allBright=false;c.strength=100;add("upper_reordered_hit_and_parallel_leaves",c,[](const Result&r){zero(r.candidate,false);});
    c={};c.geometry=Geometry::UpperDeep;add("upper_original_pending_63",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"deep clear remains OPEN");});
    c={};c.geometry=Geometry::UpperDeepRejected;add("upper_depth_limit_rejected_fallback",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"rejected depth falls back to original OPEN");});
    c={};c.geometry=Geometry::UpperMultiple;add("upper_multiple_roots_transformed",c,[](const Result&r){require(r.candidate.diffuse[0].r>0,"multiple roots clear remains OPEN");});
    c={};    c.geometry=Geometry::UpperRejected;add("upper_rejected_original_fallback",c,[](const Result&r){zero(r.candidate,true);});
#endif
#ifdef GG_CONNECTION_TRAVERSAL_TEST
    c={};c.background=true;add("audit_background_stamped_no_calls",c,[](const Result&r){identical(r);});
    c={};c.outsideVolume=true;add("audit_outside_volume_stamped_no_calls",c,[](const Result&r){identical(r);});
#endif
    for(u32 mode=0;mode<7;++mode){c={};c.mode=mode;c.ready=false;c.geometry=Geometry::Closed;add("legacy_"+std::to_string(mode),c,[](const Result&r){identical(r);});}
}
}
#ifdef GG_CONNECTION_TRAVERSAL_TEST
void registerSelectionCases(const std::shared_ptr<TestSuite>& suite){
    suite->AddTestCase(TestCase("audit_selection_boundaries",[]{
        using namespace traversal_audit;Selection s{};std::vector<traversal_audit::Pixel> p;
        const std::array<traversal_audit::Pixel,3> roi{{{0,0},{0,0},{99,59}}};
        require(BuildTraversalAuditSelection(100,60,0,0,roi,119,2,s,p)&&p.size()==2&&s.config[2]==2,"ROI duplicates dedup");
        require(BuildTraversalAuditSelection(100,60,10,6,roi,119,2,s,p)&&p.size()==62,"Grid plus distinct ROI");
        require(BuildTraversalAuditSelection(64,64,32,32,{},119,2,s,p)&&p.size()==1024,"Exact max capacity");
        const std::array<traversal_audit::Pixel,1> extra{{{0,0}}};
        require(!BuildTraversalAuditSelection(64,64,32,32,extra,119,2,s,p),"Capacity plus one rejected");
        require(!BuildTraversalAuditSelection(0,60,0,0,roi,119,2,s,p),"Zero dimension rejected");
        require(!BuildTraversalAuditSelection(100,60,0,1,roi,119,2,s,p),"Partial grid rejected");
        require(!BuildTraversalAuditSelection(100,60,0,0,roi,119,3,s,p),"Invalid slot rejected");
        require(!BuildTraversalAuditSelection(99,59,0,0,roi,119,2,s,p),"Out-of-bounds ROI rejected");
        return TestResult::Passed;
    }));
}
#endif
#ifdef GG_INSTANCE_CACHE_TEST
#include "DDGIInstanceNormalNumericFixture.h"
#endif
int main(){auto s=std::make_shared<TestSuite>("VulkanDDGIGatherGeometryReplace");registerCases(s);
#ifdef GG_INSTANCE_CACHE_TEST
registerInstanceNormalNumericCases(s);
registerInstanceNormalLifecycleCases(s);
#endif
#ifdef GG_CONNECTION_TRAVERSAL_TEST
registerSelectionCases(s);
#endif
TestRunner::RegisterTestSuite(s);const auto n=TestRunner::RunAllSuites();
#ifdef GG_INSTANCE_CACHE_TEST
    constexpr int expected=57;
#elif defined(GG_CONNECTION_TRAVERSAL_TEST)
    constexpr int expected=60;
#elif defined(GG_CONNECTION_UPPER_TEST) && defined(GG_CONNECTION_QUERY_KERNEL_TEST)
    constexpr int expected=61;
#elif defined(GG_BOUNDS_FIRST_TEST)
    constexpr int expected=55;
#elif defined(GG_EQUAL_BOX_TEST)
    constexpr int expected=62;
#elif defined(GG_CACHELESS_TEST)
    constexpr int expected=52;
#elif defined(GG_INSTANCE_PAYLOAD_TEST) || defined(GG_PAIR_TEST)
    constexpr int expected=71;
#elif defined(GG_CONNECTION_QUERY_KERNEL_TEST)
    constexpr int expected=51;
#elif defined(GG_CONNECTION_UPPER_TEST)
    constexpr int expected=57;
#elif defined(GG_CONNECTION_COMPACT_TEST)
    constexpr int expected=49;
#elif defined(GG_CONNECTION_EQUIVALENCE)
    constexpr int expected=47;
#else
    constexpr int expected=45;
#endif
    return n.totalTests==expected&&n.passedTests==expected&&n.failedTests==0&&n.skippedTests==0?0:1;}
