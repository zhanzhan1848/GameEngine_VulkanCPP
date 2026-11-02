#include "InstanceNormalCache.h"
#include "GIGatherModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Engine/Common/CoreLogStream.h"
#include <fstream>
#include <cstring>
#include <limits>
namespace primal::graphics {
using namespace rhi;
bool InstanceNormalCache::Initialize(RHIDeviceBase* d,const DescriptorSetLayoutBinding* bindings,u32 count) try {
    if(!d || d->GetPlatform()!=RHIPlatform::Vulkan)return false;
    device_=d;
    const char* paths[]={"DDGIInstanceNormalProduce.comp.spv","DDGIInstanceNormalVerify.comp.spv","DDGIGIGather_instance_cache.comp.spv"};
    for(u32 i=0;i<3;++i){
        std::ifstream f(utils::ShaderRegistry::ResolveShaderPath(std::string("Engine/Graphics/Vulkan/shaders/Lumen/")+paths[i]),std::ios::binary|std::ios::ate);
        if(!f || f.tellg()<=0)return false;const auto n=size_t(f.tellg());f.seekg(0);std::vector<u8> bytes(n);
        if(!f.read(reinterpret_cast<char*>(bytes.data()),n))return false;
        u64 hash=14695981039346656037ull;for(auto b:bytes){hash^=b;hash*=1099511628211ull;}shader_hash_[i]=hash;
        shaders_[i]=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,i==2?"ddgi_gi_gather":"main");
        if(shaders_[i]==handles::INVALID_SHADER)return false;
    }
    std::vector<DescriptorSetLayoutBinding> gb(bindings,bindings+count);
    for(u32 i=28;i<=30;++i)gb.push_back({i,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr});
    gather_set_layout_=d->CreateDescriptorSetLayout({u32(gb.size()),gb.data()});
    gather_layout_=d->CreatePipelineLayout({1,&gather_set_layout_});
    DescriptorSetLayoutBinding pb[5]{};u32 numbers[]={25,26,28,29,30};
    for(u32 i=0;i<5;++i){pb[i]={numbers[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};pb[i].readonly=i<3;}
    produce_set_layout_=d->CreateDescriptorSetLayout({5,pb});produce_layout_=d->CreatePipelineLayout({1,&produce_set_layout_});
    if(gather_set_layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT||produce_set_layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT||
       gather_layout_==handles::INVALID_PIPELINE_LAYOUT||produce_layout_==handles::INVALID_PIPELINE_LAYOUT)return false;
    ComputePipelineDesc pd{};pd.layout=produce_layout_;pd.threadGroupSize={64,1,1};pd.computeShader=shaders_[0];produce_pipeline_=d->CreateComputePipeline(pd);
    pd.computeShader=shaders_[1];verify_pipeline_=d->CreateComputePipeline(pd);
    pd.layout=gather_layout_;pd.computeShader=shaders_[2];pd.threadGroupSize={8,8,1};gather_pipeline_=d->CreateComputePipeline(pd);
    return produce_pipeline_!=handles::INVALID_PIPELINE && verify_pipeline_!=handles::INVALID_PIPELINE && gather_pipeline_!=handles::INVALID_PIPELINE;
} catch(const std::bad_alloc&) { return false; }
void InstanceNormalCache::ClearSlot(u32 index){
    auto& s=slots_[index];
    for(auto b:s.buffers)if(b!=handles::INVALID_RESOURCE)device_->DestroyBuffer(b);
    if(s.produce_set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s.produce_set);
    if(s.gather_set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s.gather_set);
    s=Slot{};
}
void InstanceNormalCache::Shutdown(){
    if(!device_)return;
    // Teardown only, never the steady frame path. Caller must quiesce GPU before
    // shutting down modules; WaitIdle also covers partial initialization.
    device_->WaitIdle();for(u32 i=0;i<3;++i)ClearSlot(i);
    for(auto p:{produce_pipeline_,verify_pipeline_,gather_pipeline_})if(p!=handles::INVALID_PIPELINE)device_->DestroyPipeline(p);
    for(auto p:{produce_layout_,gather_layout_})if(p!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(p);
    for(auto p:{produce_set_layout_,gather_set_layout_})if(p!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(p);
    for(auto p:shaders_)if(p!=handles::INVALID_SHADER)device_->DestroyShader(p);
    *this=InstanceNormalCache{};
}
bool InstanceNormalCache::Matches(const GIGatherInputs& in)const{
    const auto& s=slots_[in.current_buffer_index%3];const auto& c=in.geometry_normal_owners;
    if(!s.prepared||!c||!c->Valid()||!in.geometry_ready||s.epoch!=in.geometry_epoch||!s.digest||s.digest!=in.geometry_source_digest||
       s.owner_hash!=c->owner_hash||s.payload_hash!=c->source_payload_hash||s.count!=c->owner.size()||
       !in.geometry_still_current||!in.geometry_still_current())return false;
    for(u32 i=0;i<4;++i)if(s.sources[i]!=in.geometry[i]||s.sizes[i]!=in.geometry_sizes[i])return false;
    return true;
}
bool InstanceNormalCache::Prepare(const GIGatherInputs& in) try {
    if(!device_||gather_pipeline_==handles::INVALID_PIPELINE||in.frame_index==GIGatherInputs::kFrameIndexUnset)return false;
    const u32 slot=in.current_buffer_index%3;auto& s=slots_[slot];
    if(!device_->FrameSlotCompletions().CanReuse(slot,0))return false;
    if(Matches(in))return !s.recorded || s.record_frame==in.frame_index ||
        device_->FrameSlotCompletions().CanReuse(slot,s.record_serial);
    // No overwrite or destruction of a slot until its recorded submission has
    // really completed. A failed submit never advances this serial.
    if(s.recorded&&!device_->FrameSlotCompletions().CanReuse(slot,s.record_serial))return false;
    ClearSlot(slot);
    const auto& c=in.geometry_normal_owners;
    if(!c||!c->Valid()||c->owner.empty()||!in.geometry_ready||!in.geometry_still_current||!in.geometry_still_current())return false;
    // Vulkan guarantees >=128MiB storage ranges and >=65535 groups on X.
    // This conservative cap is below those device limits, not an inferred cap.
    const u64 n=c->owner.size();if(n>65535ull*64||n*24*3>64ull*1024*1024||c->normal_bytes!=n*16||c->owner_bytes!=n*4)return false;
    if(in.geometry_sizes[2]!=n*sizeof(ray_geometry::Triangle)||!in.geometry_sizes[3])return false;
    for(u32 i=0;i<4;++i)if(in.geometry[i]==handles::INVALID_RESOURCE||!in.geometry_sizes[i])return false;
    // NaN sentinel distinguishes missing producer writes; flags start invalid.
    std::vector<u32> normals(n*4,0x7fc00001u),flags(n,0);
    const void* data[]={c->owner.data(),normals.data(),flags.data()};const u64 sizes[]={n*4,n*16,n*4};
    for(u32 i=0;i<3;++i){BufferDesc bd{};bd.name="DDGI instance normal cache";bd.type=BufferType::Structured;bd.memoryUsage=GPUMemoryUsage::Dynamic;bd.size=sizes[i];
        s.buffers[i]=device_->CreateBuffer(bd);if(s.buffers[i]==handles::INVALID_RESOURCE)return false;
        if(!device_->UpdateBufferData(s.buffers[i],data[i],sizes[i],0))return false;}
    s.produce_set=device_->CreateDescriptorSet({produce_set_layout_});s.gather_set=device_->CreateDescriptorSet({gather_set_layout_});
    if(s.produce_set==handles::INVALID_DESCRIPTOR_SET||s.gather_set==handles::INVALID_DESCRIPTOR_SET)return false;
    DescriptorBufferInfo info[5]={{in.geometry[2],0,in.geometry_sizes[2]},{in.geometry[3],0,in.geometry_sizes[3]},
        {s.buffers[0],0,sizes[0]},{s.buffers[1],0,sizes[1]},{s.buffers[2],0,sizes[2]}};
    WriteDescriptorSet writes[5]{};u32 numbers[]={25,26,28,29,30};
    for(u32 i=0;i<5;++i){writes[i].dstSet=s.produce_set;writes[i].dstBinding=numbers[i];writes[i].descriptorCount=1;writes[i].descriptorType=DescriptorType::StorageBuffer;writes[i].bufferInfo=&info[i];}
    device_->UpdateDescriptorSets(5,writes);
    for(u32 i=2;i<5;++i)writes[i].dstSet=s.gather_set;device_->UpdateDescriptorSets(3,writes+2);
    for(u32 i=0;i<4;++i){s.sources[i]=in.geometry[i];s.sizes[i]=in.geometry_sizes[i];}
    s.epoch=in.geometry_epoch;s.digest=in.geometry_source_digest;s.owner_hash=c->owner_hash;s.payload_hash=c->source_payload_hash;s.count=u32(n);s.prepared=true;
    return true;
} catch(const std::bad_alloc&) { return false; }
bool InstanceNormalCache::Available(const GIGatherInputs& in)const{
    if(!Matches(in))return false;const u32 slot=in.current_buffer_index%3;const auto& s=slots_[slot];
    if(!s.recorded)return false;
    return s.record_frame==in.frame_index || device_->FrameSlotCompletions().CanReuse(slot,s.record_serial);
}
void InstanceNormalCache::Record(RHICommandBuffer* cmd,const GIGatherInputs& in){
    if(!Matches(in))return;auto& s=slots_[in.current_buffer_index%3];
    if(s.recorded&&device_->FrameSlotCompletions().CanReuse(in.current_buffer_index%3,s.record_serial))return;
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
    cmd->BindComputePipeline(produce_pipeline_);cmd->BindDescriptorSets(PipelineBindPoint::Compute,produce_layout_,0,1,&s.produce_set,0,nullptr);cmd->Dispatch((s.count+63)/64,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
    cmd->BindComputePipeline(verify_pipeline_);cmd->Dispatch((s.count+63)/64,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
    s.recorded=true;s.record_frame=in.frame_index;s.record_serial=device_->FrameSlotCompletions().Serial(in.current_buffer_index%3);
}
std::array<rendergraph::RGResourceHandle,3> InstanceNormalCache::AddPasses(rendergraph::RenderGraph& graph,const GIGatherInputs& in,const rendergraph::RGResourceHandle* geometry){
    std::array<rendergraph::RGResourceHandle,3> out{};if(!Prepare(in))return out;
    auto& s=slots_[in.current_buffer_index%3];for(u32 i=0;i<3;++i){BufferDesc bd{};bd.type=BufferType::Structured;bd.size=u64(s.count)*(i==1?16:4);out[i]=graph.ImportBuffer("DDGIInstanceNormal"+std::to_string(i),s.buffers[i],bd);}
    if(s.recorded&&device_->FrameSlotCompletions().CanReuse(in.current_buffer_index%3,s.record_serial))return out;
    struct Data{};
    graph.AddPass<Data>("DDGIInstanceNormalProduceVerify",rendergraph::RGPassType::Compute,rendergraph::RGPassCategory::Lighting,
        [out,tri=geometry[2],inst=geometry[3]](Data&,rendergraph::RenderGraphBuilder& b){b.Read(tri,ResourceState::ShaderResource);b.Read(inst,ResourceState::ShaderResource);b.Read(out[0],ResourceState::ShaderResource);b.Write(out[1],ResourceState::UnorderedAccess);b.Write(out[2],ResourceState::UnorderedAccess);},
        [this,in](const Data&,rendergraph::RenderGraphContext& c){Record(c.cmdBuffer,in);});
    return out;
}
void InstanceNormalCache::Receipt(const GIGatherInputs& in){auto& s=slots_[in.current_buffer_index%3];if(s.receipt){const char* audit=std::getenv("DDGI_INSTANCE_CACHE_AUDIT");if(!audit||std::strcmp(audit,"1")!=0||(in.frame_index!=119&&in.frame_index!=239))return;}s.receipt=true;
    PRIMAL_LOG_STREAM(Info,"Render")<<"[DDGI_INSTANCE_NORMAL_CACHE] actual_pipeline=cache native_frame="<<in.frame_index<<" slot="<<in.current_buffer_index%3<<" epoch="<<s.epoch<<" digest="<<s.digest<<" source_payload="<<s.payload_hash<<" owner_hash="<<s.owner_hash
        <<" triangles="<<s.count<<" bytes="<<u64(s.count)*24<<" producer_fnv="<<shader_hash_[0]<<" verifier_fnv="<<shader_hash_[1]<<" gather_fnv="<<shader_hash_[2]
        <<" source_handles="<<s.sources[0]<<","<<s.sources[1]<<","<<s.sources[2]<<","<<s.sources[3]<<" source_sizes="<<s.sizes[0]<<","<<s.sizes[1]<<","<<s.sizes[2]<<","<<s.sizes[3]
        <<" sidecars="<<s.buffers[0]<<","<<s.buffers[1]<<","<<s.buffers[2]<<" recorded_serial="<<s.record_serial<<" current_completed_serial="<<device_->FrameSlotCompletions().Serial(in.current_buffer_index%3)
        <<" entry_gate=gpu-bit-verify+finite actual_cached_entries=not-observed"<<std::endl;
}
}
