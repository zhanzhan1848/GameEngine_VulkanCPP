#include "EnvironmentVisibilityModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/LumenCompareCapture.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include "Graphics/Utilities/ExperimentalSAHMesh.h"
#include "Engine/Common/CoreLogStream.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <fstream>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <string>
#include <bit>
#include <cmath>

namespace primal::graphics {
using namespace rhi;
using namespace rendergraph;
namespace {
struct VisibilityCB {
    math::m4x4 inverse_view_projection;
    math::v4 camera;
    u32 dimensions[4];
    u32 policy[4];
    math::v4 parameters;
};
static_assert(sizeof(VisibilityCB)==128);
static_assert(offsetof(VisibilityCB, camera)==64);
static_assert(offsetof(VisibilityCB, policy)==96);
TextureDesc VisibilityTexture(u32 w,u32 h) {
    TextureDesc d{};d.type=TextureType::Texture2D;d.size={w,h,1};
    d.format=DataFormat::RGBA16_Float;
    d.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;
    return d;
}
}
EnvironmentVisibilityModule::EnvironmentVisibilityModule() {
    for(u32 i=0;i<3;++i) {
        half_[i]=full_[i]=handles::INVALID_RESOURCE;
        for(u32 p=0;p<2;++p) { cb_[p][i]=handles::INVALID_RESOURCE;sets_[p][i]=handles::INVALID_DESCRIPTOR_SET; }
        for(auto& b:own_geometry_[i].buffers)b=handles::INVALID_RESOURCE;
    }
}
bool EnvironmentVisibilityModule::SetGeometryScene(std::span<const ray_geometry::InstanceInput> inputs) {
    experimental_source_digest_=0;experimental_scene_epoch_=0;
    experimental_failure_.Reset();
    ++geometry_epoch_;bool loaded=false,persisted=false;
    const bool ok=ray_geometry::LoadOrBuildSceneBVH(inputs,ray_geometry::GeometryCacheDirectory(),geometry_scene_,&loaded,&persisted);
    if(!ok)geometry_scene_={};
    PRIMAL_LOG_STREAM(Info,"Render") << "[EnvironmentVisibility] complete_scene=" << ok
        << " instances=" << geometry_scene_.instances.size() << " cached=" << loaded;
    return ok;
}
bool EnvironmentVisibilityModule::SetExperimentalGeometryScene(std::span<const ray_geometry::InstanceInput> inputs,
                                                              u64 source_digest,u64 scene_epoch) {
    if(experimental_failure_.Blocks(source_digest,scene_epoch))return false;
    ++geometry_epoch_;experimental_source_digest_=0;experimental_scene_epoch_=0;geometry_scene_={};
    const char* failure="source digest mismatch";u64 failedMesh=0;
    const bool ok=source_digest && ray_geometry::SceneSourceDigest(inputs)==source_digest &&
                  ray_geometry::BuildExperimentalSAHScene(inputs,geometry_scene_,&failure,&failedMesh);
    if(ok){experimental_source_digest_=source_digest;experimental_scene_epoch_=scene_epoch;experimental_failure_.Reset();}
    else {geometry_scene_={};experimental_failure_.Record(source_digest,scene_epoch);}
    PRIMAL_LOG_STREAM(Info,"Render") << "[EnvironmentVisibility] experimental_sah=" << ok
        << " source_digest=" << source_digest << " scene_epoch=" << scene_epoch
        << " instances=" << geometry_scene_.instances.size() << " failure=" << (ok?"none":failure)
        << " failed_mesh_digest=" << failedMesh;
    return ok;
}
bool EnvironmentVisibilityModule::HasExperimentalGeometryScene(u64 source_digest,u64 scene_epoch) const {
    return source_digest && experimental_source_digest_==source_digest && experimental_scene_epoch_==scene_epoch &&
           !geometry_scene_.top.empty();
}
bool EnvironmentVisibilityModule::UploadOwnGeometry(u32 frame) {
    if(geometry_scene_.top.empty())return false;
    auto& s=own_geometry_[frame];if(s.epoch==geometry_epoch_)return s.ready;
    const void* data[4]={geometry_scene_.top.data(),geometry_scene_.bottom.data(),geometry_scene_.triangles.data(),geometry_scene_.instances.data()};
    const u64 sizes[4]={geometry_scene_.top.size()*sizeof(ray_geometry::Node),geometry_scene_.bottom.size()*sizeof(ray_geometry::Node),geometry_scene_.triangles.size()*sizeof(ray_geometry::Triangle),geometry_scene_.instances.size()*sizeof(ray_geometry::Instance)};
    s.ready=false;bool ok=!geometry_scene_.top.empty();const u8 zero[64]{};
    // Caller supplies the fence-completed frame slot. Never touch other slots.
    for(u32 i=0;i<4;++i) {
        const auto bytes=std::max<u64>(64,sizes[i]);
        if(s.sizes[i]!=bytes||s.buffers[i]==handles::INVALID_RESOURCE) {
            if(s.buffers[i]!=handles::INVALID_RESOURCE)device_->DestroyBuffer(s.buffers[i]);
            BufferDesc d{};d.size=bytes;d.type=BufferType::Structured;
            d.usage=d.memoryUsage=GPUMemoryUsage::Dynamic;d.bindFlags=u32(BufferUsageFlags::Storage);
            s.buffers[i]=device_->CreateBuffer(d);s.sizes[i]=bytes;
        }
        void* dst=s.buffers[i]!=handles::INVALID_RESOURCE?device_->MapBuffer(s.buffers[i]):nullptr;
        if(!dst){ok=false;continue;}
        if(sizes[i])std::memcpy(dst,data[i],sizes[i]);else std::memcpy(dst,zero,64);
        device_->UnmapBuffer(s.buffers[i]);
    }
    s.epoch=ok?geometry_epoch_:~u64(0);s.ready=ok;
    if(experimental_source_digest_)PRIMAL_LOG_STREAM(Info,"Render") << "[EnvironmentVisibility] experimental_sah_upload slot=" << frame
        << " ready=" << ok << " geometry_epoch=" << geometry_epoch_ << " scene_epoch=" << experimental_scene_epoch_
        << " source_digest=" << experimental_source_digest_ << " bytes=" << sizes[0] << "," << sizes[1] << "," << sizes[2] << "," << sizes[3];
    return ok;
}
bool EnvironmentVisibilityModule::Initialize(RHIDeviceBase* device,ShaderHandle trace,ShaderHandle resolve,u32 w,u32 h) {
    if(!device||device->GetPlatform()!=RHIPlatform::Vulkan||!w||!h||trace==handles::INVALID_SHADER||resolve==handles::INVALID_SHADER)return false;
    if(device_)Shutdown();
    device_=device;width_=w;height_=h;
    const char* frameAudit=std::getenv("PRIMAL_PAIR_FRAME_AUDIT");frame_audit_enabled_=frameAudit && std::strcmp(frameAudit,"1")==0;
    for(auto& serial:experimental_consumed_serials_)serial=0;
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
    set_layout_=device_->CreateDescriptorSetLayout({10,bindings});
    if(set_layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT){Shutdown();return false;}
    layout_=device_->CreatePipelineLayout({1,&set_layout_});
    if(layout_==handles::INVALID_PIPELINE_LAYOUT){Shutdown();return false;}
    for(u32 p=0;p<2;++p) {
        ComputePipelineDesc d{};d.computeShader=p?resolve:trace;d.layout=layout_;d.threadGroupSize={8,8,1};
        pipelines_[p]=device_->CreateComputePipeline(d);
        for(u32 i=0;i<3;++i) {
            sets_[p][i]=device_->CreateDescriptorSet({set_layout_});
            BufferDesc b{};b.size=sizeof(VisibilityCB);b.type=BufferType::Constant;
            b.usage=b.memoryUsage=GPUMemoryUsage::Dynamic;b.bindFlags=u32(BufferUsageFlags::Uniform);
            cb_[p][i]=device_->CreateBuffer(b);
            if(sets_[p][i]==handles::INVALID_DESCRIPTOR_SET||cb_[p][i]==handles::INVALID_RESOURCE){Shutdown();return false;}
        }
    }
    if(const char* e=std::getenv("PRIMAL_ENV_SDF_HYBRID");e && std::strcmp(e,"1")==0){
        hybrid_env_requested_=true;auto exact=[](const char* n){const char* v=std::getenv(n);return v&&std::strcmp(v,"1")==0;};
        const char* wg=std::getenv("PRIMAL_ENV_TRACE_WORKGROUP");const bool compatible=(!wg||std::strcmp(wg,"0")==0)&&!exact("PRIMAL_ENV_CROSS_RAY_PAIR")&&!exact("PRIMAL_EXPERIMENT_ENV_SAH");
        if(compatible){std::ifstream f(environment_hybrid::kShaderPath,std::ios::binary|std::ios::ate);if(f&&f.tellg()>0){const auto n=static_cast<size_t>(f.tellg());std::vector<u8> bytes(n);f.seekg(0);if(f.read(reinterpret_cast<char*>(bytes.data()),n) && environment_hybrid::Hash(bytes)==environment_hybrid::kShaderFNV){hybrid_shader_hash_=environment_hybrid::kShaderFNV;hybrid_shader_=device_->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");}}
            DescriptorSetLayoutBinding hbindings[11]{};for(u32 i=0;i<10;++i)hbindings[i]=bindings[i];hbindings[10]={17,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};hbindings[10].readonly=true;
            if(hybrid_shader_!=handles::INVALID_SHADER)hybrid_set_layout_=device_->CreateDescriptorSetLayout({11,hbindings});
            if(hybrid_set_layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)hybrid_layout_=device_->CreatePipelineLayout({1,&hybrid_set_layout_});
            if(hybrid_layout_!=handles::INVALID_PIPELINE_LAYOUT){ComputePipelineDesc d{};d.computeShader=hybrid_shader_;d.layout=hybrid_layout_;d.threadGroupSize={8,8,1};hybrid_pipeline_=device_->CreateComputePipeline(d);}
            if(hybrid_pipeline_!=handles::INVALID_PIPELINE){hybrid_requested_=true;for(u32 i=0;i<3;++i){hybrid_sets_[i]=device_->CreateDescriptorSet({hybrid_set_layout_});hybrid_requested_ &= hybrid_sets_[i]!=handles::INVALID_DESCRIPTOR_SET;}}
        }
        PRIMAL_LOG_STREAM(Info,"Render") << "[ENV_FIELD] startup=1 compatible=" << compatible << " pipeline_ready=" << hybrid_requested_;
    }
    if(const char* e=std::getenv("PRIMAL_ENV_CROSS_RAY_PAIR");e && std::strcmp(e,"1")==0){
        pair_env_requested_=true;auto exact=[](const char* n){const char* v=std::getenv(n);return v&&std::strcmp(v,"1")==0;};
        const char* wg=std::getenv("PRIMAL_ENV_TRACE_WORKGROUP");
        const bool compatible=(!wg||std::strcmp(wg,"0")==0) && !exact("PRIMAL_EXPERIMENT_ENV_SAH") &&
            !exact("PRIMAL_DDGI_GEOMETRY_EQUAL_BOX") && !exact("PRIMAL_DDGI_GEOMETRY_UPPER_BVH") && !exact("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS") &&
            !exact("PRIMAL_DDGI_GEOMETRY_BOUNDS_FIRST") && !exact("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE") && !exact("PRIMAL_DDGI_INSTANCE_PAYLOAD_CACHE");
        if(compatible){const auto path=utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/EnvironmentVisibility_trace_pair.comp.spv");
            std::ifstream f(path,std::ios::binary|std::ios::ate);if(f&&f.tellg()>0){auto count=static_cast<size_t>(f.tellg());f.seekg(0);std::vector<u8> bytes(count);
                if(f.read(reinterpret_cast<char*>(bytes.data()),count)){pair_shader_hash_=14695981039346656037ull;for(u8 byte:bytes){pair_shader_hash_^=byte;pair_shader_hash_*=1099511628211ull;}pair_shader_=device_->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");}}
            if(pair_shader_!=handles::INVALID_SHADER){ComputePipelineDesc pd{};pd.computeShader=pair_shader_;pd.layout=layout_;pd.threadGroupSize={8,8,1};pair_pipeline_=device_->CreateComputePipeline(pd);}pair_requested_=pair_pipeline_!=handles::INVALID_PIPELINE;
        }
        PRIMAL_LOG_STREAM(Info,"Render") << "[ENV_CROSS_RAY_PAIR] startup=1 compatible=" << compatible << " pipeline_ready=" << pair_requested_ << std::endl;
    }
    const char* workgroupEnv=std::getenv("PRIMAL_ENV_TRACE_WORKGROUP");
    const bool workgroupValid=workgroup_experiment::Parse(workgroupEnv,false,workgroup_requested_);
    if(workgroupEnv) {
        if(workgroupValid && workgroup_requested_==32) {
            const auto path=utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/EnvironmentVisibility_trace_wg32.comp.spv");
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(file && file.tellg()>0) {const auto count=static_cast<size_t>(file.tellg());file.seekg(0);std::vector<u8> bytes(count);
                if(file.read(reinterpret_cast<char*>(bytes.data()),count)){if(frame_audit_enabled_){frame_audit_workgroup_hash_=14695981039346656037ull;for(u8 byte:bytes){frame_audit_workgroup_hash_^=byte;frame_audit_workgroup_hash_*=1099511628211ull;}}workgroup_shader_=device_->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");}}
            if(workgroup_shader_!=handles::INVALID_SHADER) {ComputePipelineDesc d{};d.computeShader=workgroup_shader_;d.layout=layout_;d.threadGroupSize={8,4,1};workgroup_pipeline_=device_->CreateComputePipeline(d);}
        }
        PRIMAL_LOG_STREAM(Info,"Render") << "[ENV_TRACE_WORKGROUP] startup=1 requested=" << workgroup_requested_ << " valid=" << workgroupValid << " pipeline_ready=" << (workgroup_pipeline_!=handles::INVALID_PIPELINE) << std::endl;
    }
    for(u32 i=0;i<3;++i) {
        half_[i]=device_->CreateTexture(VisibilityTexture((w+1)/2,(h+1)/2));
        full_[i]=device_->CreateTexture(VisibilityTexture(w,h));
        if(half_[i]==handles::INVALID_RESOURCE||full_[i]==handles::INVALID_RESOURCE){Shutdown();return false;}
    }
    BufferDesc dummy{};dummy.size=64;dummy.type=BufferType::Structured;
    dummy.usage=dummy.memoryUsage=GPUMemoryUsage::Dynamic;dummy.bindFlags=u32(BufferUsageFlags::Storage);
    dummy_geometry_=device_->CreateBuffer(dummy);
    auto* zero=dummy_geometry_!=handles::INVALID_RESOURCE?device_->MapBuffer(dummy_geometry_):nullptr;
    if(!zero){Shutdown();return false;}std::memset(zero,0,64);device_->UnmapBuffer(dummy_geometry_);
    // Immutable fail-closed parameters let a failed per-frame MapBuffer
    // still dispatch black output instead of retaining a prior white frame.
    BufferDesc failed{};failed.size=sizeof(VisibilityCB);failed.type=BufferType::Constant;
    failed.usage=failed.memoryUsage=GPUMemoryUsage::Dynamic;failed.bindFlags=u32(BufferUsageFlags::Uniform);
    unsupported_cb_=device_->CreateBuffer(failed);
    auto* failedMap=unsupported_cb_!=handles::INVALID_RESOURCE?device_->MapBuffer(unsupported_cb_):nullptr;
    if(!failedMap){Shutdown();return false;}
    VisibilityCB unsupported{};unsupported.dimensions[0]=width_;unsupported.dimensions[1]=height_;
    unsupported.dimensions[2]=(width_+1)/2;unsupported.dimensions[3]=(height_+1)/2;
    unsupported.policy[1]=0x80000000u;
    std::memcpy(failedMap,&unsupported,sizeof(unsupported));device_->UnmapBuffer(unsupported_cb_);
    if(!IsInitialized()){Shutdown();return false;}return true;
}
void EnvironmentVisibilityModule::Shutdown() {
    if(!device_)return;
    for(u32 i=0;i<3;++i){if(hybrid_sets_[i]!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(hybrid_sets_[i]);if(hybrid_buffers_[i]!=handles::INVALID_RESOURCE)device_->DestroyBuffer(hybrid_buffers_[i]);hybrid_sets_[i]=handles::INVALID_DESCRIPTOR_SET;hybrid_buffers_[i]=handles::INVALID_RESOURCE;hybrid_serial_[i]=0;}
    if(hybrid_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(hybrid_pipeline_);if(hybrid_shader_!=handles::INVALID_SHADER)device_->DestroyShader(hybrid_shader_);if(hybrid_layout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(hybrid_layout_);if(hybrid_set_layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(hybrid_set_layout_);
    hybrid_pipeline_=handles::INVALID_PIPELINE;hybrid_shader_=handles::INVALID_SHADER;hybrid_layout_=handles::INVALID_PIPELINE_LAYOUT;hybrid_set_layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;hybrid_requested_=hybrid_env_requested_=false;hybrid_shader_hash_=0;hybrid_receipt_={};
    if(pair_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pair_pipeline_);
    if(pair_shader_!=handles::INVALID_SHADER)device_->DestroyShader(pair_shader_);
    pair_pipeline_=handles::INVALID_PIPELINE;pair_shader_=handles::INVALID_SHADER;pair_requested_=false;pair_env_requested_=false;pair_shader_hash_=0;pair_receipt_={};frame_audit_enabled_=false;frame_audit_trace_hash_=frame_audit_workgroup_hash_=0;pair_policy0_=pair_policy1_=0;
    if(workgroup_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(workgroup_pipeline_);
    if(workgroup_shader_!=handles::INVALID_SHADER)device_->DestroyShader(workgroup_shader_);
    workgroup_pipeline_=handles::INVALID_PIPELINE;workgroup_shader_=handles::INVALID_SHADER;workgroup_requested_=0;workgroup_receipt_={};
    for(u32 p=0;p<2;++p) {
        for(u32 i=0;i<3;++i) {
            if(sets_[p][i]!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(sets_[p][i]);
            if(cb_[p][i]!=handles::INVALID_RESOURCE)device_->DestroyBuffer(cb_[p][i]);
            sets_[p][i]=handles::INVALID_DESCRIPTOR_SET;cb_[p][i]=handles::INVALID_RESOURCE;
        }
        if(pipelines_[p]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[p]);
        pipelines_[p]=handles::INVALID_PIPELINE;
    }
    for(u32 i=0;i<3;++i) {
        if(half_[i]!=handles::INVALID_RESOURCE)device_->DestroyTexture(half_[i]);
        if(full_[i]!=handles::INVALID_RESOURCE)device_->DestroyTexture(full_[i]);
        half_[i]=full_[i]=handles::INVALID_RESOURCE;
        for(auto& b:own_geometry_[i].buffers){if(b!=handles::INVALID_RESOURCE)device_->DestroyBuffer(b);b=handles::INVALID_RESOURCE;}
        own_geometry_[i].epoch=~u64(0);own_geometry_[i].ready=false;
    }
    if(dummy_geometry_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(dummy_geometry_);
    if(unsupported_cb_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(unsupported_cb_);
    unsupported_cb_=handles::INVALID_RESOURCE;
    if(layout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(layout_);
    if(set_layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(set_layout_);
    dummy_geometry_=handles::INVALID_RESOURCE;layout_=handles::INVALID_PIPELINE_LAYOUT;set_layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;device_=nullptr;
    // Legacy own-scene fallback persists across resource recreation: its
    // producer caches the scene digest and may not submit it again on resize.
    // Experimental scenes are separately epoch-certified and must rebuild.
    if(experimental_source_digest_)geometry_scene_={};
    experimental_source_digest_=0;experimental_scene_epoch_=0;experimental_failure_.Reset();
    for(auto& serial:experimental_consumed_serials_)serial=0;
}
EnvironmentVisibilityOutputs EnvironmentVisibilityModule::AddPasses(RenderGraph& graph,const EnvironmentVisibilityInputs& original) {
    EnvironmentVisibilityOutputs out{};
    pair_receipt_={};hybrid_receipt_={};hybrid_receipt_.requested=hybrid_env_requested_;pair_policy0_=pair_policy1_=0;pair_receipt_.requested=pair_env_requested_?1u:0u;
    workgroup_receipt_={};workgroup_receipt_.requested=workgroup_requested_;
    if(!IsInitialized()||original.depth==handles::INVALID_RESOURCE||original.normal==handles::INVALID_RESOURCE||original.orm==handles::INVALID_RESOURCE||
       !original.depth_rg.IsValid()||!original.normal_rg.IsValid()||!original.orm_rg.IsValid())return out;
    const u32 slot=original.current_buffer_index%3;
    if(original.geometry_still_current &&
       !experimental_sah::TryConsumeSlot(device_->FrameSlotCompletions(),slot,experimental_consumed_serials_)) {
        PRIMAL_LOG_STREAM(Error,"Render") << "[EnvironmentVisibility] experimental_sah_slot_rejected slot=" << slot;
        return out;
    }
    auto inputs=original;
    bool anyExternal=false;for(auto b:inputs.geometry)anyExternal|=b!=handles::INVALID_RESOURCE;
    if(!anyExternal) {
        inputs.geometry_ready=UploadOwnGeometry(slot);
        for(u32 i=0;i<4;++i)inputs.geometry[i]=own_geometry_[slot].buffers[i];
    }
    bool ready=inputs.geometry_ready;for(auto b:inputs.geometry)ready&=b!=handles::INVALID_RESOURCE;
    for(u32 i=0;i<4;++i)if(!inputs.geometry_rg[i].IsValid()&&inputs.geometry[i]!=handles::INVALID_RESOURCE) {
        // Imported external capacity may be unknown here; descriptor range is
        // still the real allocation. The graph needs the read dependency.
        BufferDesc desc{};desc.type=BufferType::Structured;
        if(!anyExternal)desc.size=own_geometry_[slot].sizes[i];
        inputs.geometry_rg[i]=graph.ImportBuffer("EnvironmentVisibility_Geometry"+std::to_string(i),inputs.geometry[i],desc);
    }
    VisibilityCB constants{};constants.inverse_view_projection=inputs.inverse_view_projection;
    constants.camera={inputs.camera_position.x,inputs.camera_position.y,inputs.camera_position.z,0};
    constants.dimensions[0]=width_;constants.dimensions[1]=height_;constants.dimensions[2]=(width_+1)/2;constants.dimensions[3]=(height_+1)/2;
    constants.policy[0]=ready?1u:0u;constants.policy[1]=inputs.legacy_unoccluded?1u:0u;constants.policy[3]=inputs.frame_index;
    constants.parameters={.002f,.02f,.95f,.05f};
    bool hybridReady=false;
    if(hybrid_requested_ && inputs.geometry_ready && inputs.hybrid_binding.ready && inputs.hybrid_still_current && inputs.hybrid_still_current() && inputs.hybrid_field.size()==environment_hybrid::kFieldBytes && device_->FrameSlotCompletions().CanReuse(slot,hybrid_serial_[slot])) {
        const u64 completed=device_->FrameSlotCompletions().Serial(slot);
        if(hybrid_buffers_[slot]==handles::INVALID_RESOURCE){BufferDesc d{};d.size=environment_hybrid::kFieldBytes;d.type=BufferType::Structured;d.usage=d.memoryUsage=GPUMemoryUsage::Dynamic;hybrid_buffers_[slot]=device_->CreateBuffer(d);if(hybrid_buffers_[slot]!=handles::INVALID_RESOURCE){void* dst=device_->MapBuffer(hybrid_buffers_[slot]);if(dst){std::memcpy(dst,inputs.hybrid_field.data(),inputs.hybrid_field.size());device_->UnmapBuffer(hybrid_buffers_[slot]);}else{device_->DestroyBuffer(hybrid_buffers_[slot]);hybrid_buffers_[slot]=handles::INVALID_RESOURCE;}}}
        if(hybrid_buffers_[slot]!=handles::INVALID_RESOURCE){hybrid_serial_[slot]=completed;hybridReady=true;}
    }
    const u64 hybridFrameSerial=hybridReady?hybrid_serial_[slot]:0;
    rendergraph::RGResourceHandle hybridRG{};if(hybridReady){BufferDesc d{};d.type=BufferType::Structured;d.size=environment_hybrid::kFieldBytes;hybridRG=graph.ImportBuffer("EnvironmentVisibility_Field",hybrid_buffers_[slot],d);}
    const auto half=graph.ImportTexture("EnvironmentVisibility_Half",half_[slot],VisibilityTexture((width_+1)/2,(height_+1)/2));
    const auto full=graph.ImportTexture("EnvironmentVisibility_Full",full_[slot],VisibilityTexture(width_,height_));
    struct PassData { RGResourceHandle output; };
    for(u32 phase=0;phase<2;++phase) {
        const auto& pass=graph.AddPass<PassData>(phase?"EnvironmentVisibility.Resolve":"EnvironmentVisibility.Trace",RGPassType::Compute,RGPassCategory::Lighting,
            [inputs,half,full,phase,hybridRG](PassData& data,RenderGraphBuilder& b) {
                for(auto h:{inputs.depth_rg,inputs.normal_rg,inputs.orm_rg})if(h.IsValid())b.Read(h);
                for(auto h:inputs.geometry_rg)if(h.IsValid())b.Read(h);
                if(phase)b.Read(half);
                if(!phase && hybridRG.IsValid())b.Read(hybridRG);
                data.output=b.Write(phase?full:half,ResourceState::UnorderedAccess);
            },
            [this,inputs,slot,phase,constants,hybridReady,hybridFrameSerial](const PassData&,RenderGraphContext& ctx) {
                auto actual=constants;math::v2 jitter=inputs.raster_jitter_ndc;
                if(inputs.geometry_still_current && !inputs.geometry_still_current())
                    experimental_sah::InvalidateProvenance(actual.policy);
                bool jitterValid=inputs.raster_jitter_valid;
                if(inputs.raster_jitter_provider)jitterValid=inputs.raster_jitter_provider(jitter);
                jitterValid=jitterValid&&std::isfinite(jitter.x)&&std::isfinite(jitter.y);
                actual.camera.w=jitterValid?jitter.x:0.0f;
                actual.policy[2]=std::bit_cast<u32>(jitterValid?jitter.y:0.0f);
                if(!jitterValid){actual.policy[1]|=0x80000000u;actual.policy[0]=0u;}
                auto parameterBuffer=unsupported_cb_;
                if(void* mapped=device_->MapBuffer(cb_[phase][slot])) {
                    std::memcpy(mapped,&actual,sizeof(actual));device_->UnmapBuffer(cb_[phase][slot]);
                    parameterBuffer=cb_[phase][slot];
                }
                if(inputs.geometry_still_current) {
                    const int ready=actual.policy[0]==1u && parameterBuffer==cb_[phase][slot];
                    if(experimental_reported_epochs_[phase][slot]!=geometry_epoch_ || experimental_reported_states_[phase][slot]!=ready) {
                        experimental_reported_epochs_[phase][slot]=geometry_epoch_;experimental_reported_states_[phase][slot]=ready;
                        PRIMAL_LOG_STREAM(Info,"Render") << "[EnvironmentVisibility] experimental_sah_dispatch phase=" << phase
                            << " slot=" << slot << " ready=" << ready << " geometry_epoch=" << geometry_epoch_
                            << " scene_epoch=" << experimental_scene_epoch_ << " source_digest=" << experimental_source_digest_;
                    }
                }
                const bool useWorkgroup=!hybrid_env_requested_ && !pair_env_requested_ && phase==0 && workgroup_pipeline_!=handles::INVALID_PIPELINE && actual.policy[0]==1u && !(actual.policy[1]&0x80000000u) && parameterBuffer==cb_[phase][slot] && !inputs.legacy_unoccluded;
                const bool usePair=!hybrid_env_requested_ && phase==0 && pair_requested_ && !useWorkgroup && actual.policy[0]==1u && !(actual.policy[1]&0x80000000u) && parameterBuffer==cb_[phase][slot] && !inputs.legacy_unoccluded;
                const bool useHybrid=phase==0 && hybridReady && hybridFrameSerial && hybrid_serial_[slot]==hybridFrameSerial && device_->FrameSlotCompletions().CanReuse(slot,0) && device_->FrameSlotCompletions().Serial(slot)==hybridFrameSerial && inputs.hybrid_still_current && inputs.hybrid_still_current() && actual.policy[0]==1u && !(actual.policy[1]&0x80000000u) && parameterBuffer==cb_[phase][slot] && !inputs.legacy_unoccluded;
                const auto selectedSet=useHybrid?hybrid_sets_[slot]:sets_[phase][slot];
                DescriptorImageInfo images[5]{};DescriptorBufferInfo buffers[6]{};WriteDescriptorSet writes[11]{};
                const ResourceHandle tex[5]={inputs.depth,inputs.normal,inputs.orm,phase?full_[slot]:half_[slot],phase?half_[slot]:inputs.orm};
                const u32 bindings[5]={0,1,2,3,5};
                for(u32 i=0;i<5;++i) {
                    images[i].imageView=tex[i];images[i].sampler=handles::INVALID_SAMPLER;
                    images[i].imageLayout=i==3?ResourceState::UnorderedAccess:ResourceState::ShaderResource;
                    writes[i].dstSet=selectedSet;writes[i].dstBinding=bindings[i];writes[i].descriptorCount=1;
                    writes[i].descriptorType=i==3?DescriptorType::StorageImage:(i==0?DescriptorType::SampledDepthImage:DescriptorType::SampledImage);writes[i].imageInfo=&images[i];
                }
                buffers[0].buffer=parameterBuffer;buffers[0].range=sizeof(VisibilityCB);
                writes[5].dstSet=selectedSet;writes[5].dstBinding=4;writes[5].descriptorCount=1;writes[5].descriptorType=DescriptorType::UniformBuffer;writes[5].bufferInfo=&buffers[0];
                for(u32 i=0;i<4;++i) {
                    buffers[i+1].buffer=inputs.geometry[i]!=handles::INVALID_RESOURCE?inputs.geometry[i]:dummy_geometry_;buffers[i+1].range=~0ull;
                    writes[i+6].dstSet=selectedSet;writes[i+6].dstBinding=13+i;writes[i+6].descriptorCount=1;writes[i+6].descriptorType=DescriptorType::StorageBuffer;writes[i+6].bufferInfo=&buffers[i+1];
                }
                if(useHybrid){buffers[5]={hybrid_buffers_[slot],0,environment_hybrid::kFieldBytes};writes[10].dstSet=selectedSet;writes[10].dstBinding=17;writes[10].descriptorCount=1;writes[10].descriptorType=DescriptorType::StorageBuffer;writes[10].bufferInfo=&buffers[5];}
                device_->UpdateDescriptorSets(useHybrid?11:10,writes);auto* cmd=ctx.cmdBuffer;
                // Explicit publication for both the mapped 128B CB and the
                // current borrowed/owned host-uploaded BVH. Buffer-only
                // InsertBarrier is not a portable host-write dependency.
                cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,
                                   AccessFlag::HostWrite,AccessFlag::ShaderRead);
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
                // Same-frame opt-in evidence of the actual scalar Trace
                // descriptors. Whole-size descriptors are captured through
                // their physical end, not inferred from another pass.
                if (!phase && device_->GetPlatform()==RHIPlatform::Vulkan &&
                    lumencompare::enabled && lumencompare::Selected("EnvironmentVisibility.Trace.Inputs")) {
                    bool current=!useHybrid && !usePair && !useWorkgroup &&
                        actual.policy[0]==1u && actual.policy[1]==0u &&
                        parameterBuffer==cb_[phase][slot] && inputs.geometry_ready &&
                        (!inputs.geometry_still_current || inputs.geometry_still_current());
                    u64 physical[4]{},visible[4]{};
                    // F0-11: GetBufferByteSize 已提升为 RHIDeviceBase 虚接口——
                    // 此处不再 static_cast<VulkanDevice*>（旧代码依赖
                    // LumenCompareCapture.h 传递 include 的 VulkanDevice.h，属
                    // 隐藏传递依赖；Vulkan override 行为不变）。
                    for (u32 i=0;i<4;++i) {
                        current=current && buffers[i+1].buffer==inputs.geometry[i] &&
                            buffers[i+1].buffer!=handles::INVALID_RESOURCE &&
                            buffers[i+1].range==~u64(0);
                        const bool sized=device_->GetBufferByteSize(buffers[i+1].buffer,physical[i]);
                        current=current && sized && buffers[i+1].offset<physical[i];
                        if (sized && buffers[i+1].offset<physical[i])
                            visible[i]=physical[i]-buffers[i+1].offset;
                    }
                    const auto directory=std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) /
                        ("frame-"+std::to_string(lumencompare::ordinal));
                    std::filesystem::create_directories(directory);
                    std::ofstream bridge(directory/"bridge-environment-geometry.json");
                    bridge << "{\"schema\":1,\"native_frame\":" << inputs.frame_index
                           << ",\"ordinal\":" << lumencompare::ordinal << ",\"binding_slot\":" << slot
                           << ",\"command\":" << cmd->GetHandle()
                           << ",\"selected_set\":" << selectedSet
                           << ",\"snapshot_qualified\":" << (current?"true":"false")
                           << ",\"provenance_callback_present\":" << (inputs.geometry_still_current?"true":"false")
                           << ",\"scalar_selected\":" << ((!useHybrid&&!usePair&&!useWorkgroup)?"true":"false")
                           << ",\"policy0\":" << actual.policy[0] << ",\"policy1\":" << actual.policy[1]
                           << ",\"buffers\":[";
                    for(u32 i=0;i<4;++i){
                        if(i)bridge << ',';
                        bridge << "{\"binding\":" << (13+i) << ",\"handle\":" << buffers[i+1].buffer
                               << ",\"offset\":" << buffers[i+1].offset << ",\"range\":\"whole_size\",\"physical_bytes\":"
                               << physical[i] << ",\"visible_bytes\":" << visible[i] << '}';
                    }
                    bridge << "]}\n";
                    if(current){
                        cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader,PipelineStage::Transfer,
                            AccessFlag::HostWrite|AccessFlag::ShaderWrite,AccessFlag::TransferRead);
                        const char* names[4]={"geometry-binding-13","geometry-binding-14","geometry-binding-15","geometry-binding-16"};
                        for(u32 i=0;i<4;++i)lumencompare::BufferSlice(device_,cmd,"EnvironmentVisibility.Trace.Inputs",names[i],
                            buffers[i+1].buffer,buffers[i+1].offset,visible[i]);
                    }
                }
#endif
                cmd->BindComputePipeline(useHybrid?hybrid_pipeline_:(usePair?pair_pipeline_:(useWorkgroup?workgroup_pipeline_:pipelines_[phase])));
                const DescriptorSetHandle sets[]={selectedSet};cmd->BindDescriptorSets(PipelineBindPoint::Compute,useHybrid?hybrid_layout_:layout_,0,1,sets,0,nullptr);
                const u32 w=phase?width_:(width_+1)/2,h=phase?height_:(height_+1)/2;
                const u32 localY=useWorkgroup?4u:8u,gx=workgroup_experiment::Groups(w,8),gy=workgroup_experiment::Groups(h,localY);
                cmd->Dispatch(gx,gy,1);
                if(!phase){hybrid_receipt_.requested=hybrid_env_requested_;hybrid_receipt_.qualified=useHybrid;hybrid_receipt_.actual=useHybrid;hybrid_receipt_.dispatched=true;hybrid_receipt_.slot=slot;hybrid_receipt_.shader=useHybrid?hybrid_shader_hash_:(usePair?pair_shader_hash_:(useWorkgroup?frame_audit_workgroup_hash_:frame_audit_trace_hash_));if(useHybrid){hybrid_receipt_.field_hash=environment_hybrid::kFieldFNV;hybrid_receipt_.field_buffer=hybrid_buffers_[slot];hybrid_receipt_.frame_completion=hybridFrameSerial;hybrid_receipt_.binding=inputs.hybrid_binding;}}
                if(!phase)workgroup_receipt_={workgroup_requested_,useWorkgroup?32u:0u,8,localY,gx,gy,w,h,slot,0,0,true};
                if(!phase){pair_receipt_={pair_env_requested_?1u:0u,usePair?1u:0u,8,localY,gx,gy,w,h,slot,0,0,true};pair_receipt_.shader_fnv1a64=usePair?pair_shader_hash_:0;for(u32 i=0;i<4;++i)pair_receipt_.buffers[i]=buffers[i+1].buffer;if(usePair)workgroup_receipt_.shader_fnv1a64=pair_shader_hash_;if(frame_audit_enabled_){const bool cbValid=parameterBuffer==cb_[phase][slot];pair_policy0_=cbValid?actual.policy[0]:0u;pair_policy1_=cbValid?actual.policy[1]:0x80000000u;pair_receipt_.source_qualified=cbValid && pair_policy0_==1u && !(pair_policy1_&0x80000000u) && !inputs.legacy_unoccluded;pair_receipt_.shader_fnv1a64=useHybrid?hybrid_shader_hash_:(usePair?pair_shader_hash_:(useWorkgroup?frame_audit_workgroup_hash_:frame_audit_trace_hash_));}}
                if(!phase)for(u32 i=0;i<4;++i)workgroup_receipt_.buffers[i]=buffers[i+1].buffer;
                if(lumencompare::enabled) {
                    const char* stage=phase?"EnvironmentVisibility.Resolve":"EnvironmentVisibility.Trace";
                    lumencompare::Output(device_,cmd,stage,"Visibility",phase?full_[slot]:half_[slot],false);
                    lumencompare::Output(device_,cmd,stage,"Parameters",parameterBuffer,true,ResourceState::ShaderResource);
                }
            });
        if(phase)out.visibility_rg=pass.output;else out.half_visibility_rg=pass.output;
    }
    out.visibility_tex=full_[slot];out.half_visibility_tex=half_[slot];return out;
}
} // namespace primal::graphics
