#pragma once
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Common/CoreLogStream.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace primal::tests::dawn_sponza {
using namespace graphics::rhi;
using ProbePoint=std::array<float,4>;
struct RelocationParameters {float origin[4]{},extent[4]{},policy[4]{};u32 counts[4]{};};
static_assert(sizeof(RelocationParameters)==64);
struct SdfCacheHeader {char magic[8];u32 resolution;float origin[3],extent[3],voxel;u32 flags,pad;u64 count;};
static_assert(sizeof(SdfCacheHeader)==56);

// Static offline field and grid, immutable for this Sponza scene lifetime.
// Upload, relocation and the small origin readback complete before publication.
// A field/scene change must destroy and rebuild this producer.
class ProbeRelocation {
public:
    ProbeRelocation()=default;ProbeRelocation(const ProbeRelocation&)=delete;
    ProbeRelocation& operator=(const ProbeRelocation&)=delete;~ProbeRelocation(){Shutdown();}
    bool Load(DawnDevice& device,const char* cache,std::span<const ProbePoint> origins,float spacing,const char* capture=nullptr){
        std::ifstream in(cache,std::ios::binary);SdfCacheHeader h{};in.read(reinterpret_cast<char*>(&h),sizeof(h));
        if(!in || std::memcmp(h.magic,"PGSDFC02",8)!=0 || h.flags!=1 || h.resolution!=384 || h.count!=u64(384)*384*384)return false;
        RelocationParameters p{};for(u32 a=0;a<3;++a){p.origin[a]=h.origin[a];p.extent[a]=h.extent[a];}
        p.counts[0]=h.resolution;p.policy[0]=spacing;p.policy[1]=std::max({h.extent[0],h.extent[1],h.extent[2]})/float(h.resolution);
        std::vector<u16> field(h.count);in.read(reinterpret_cast<char*>(field.data()),field.size()*2);
        if(!in || in.peek()!=std::char_traits<char>::eof())return false;
        return Initialize(device,p,field,origins,capture);
    }
    bool Initialize(DawnDevice& device,RelocationParameters p,std::span<const u16> field,std::span<const ProbePoint> origins,const char* capture=nullptr){
        if(device_ || origins.empty() || !p.counts[0] || p.counts[0]>2048 || field.size()!=u64(p.counts[0])*p.counts[0]*p.counts[0] ||
           !std::isfinite(p.policy[0]) || p.policy[0]<=0 || !std::isfinite(p.policy[1]) || p.policy[1]<=0)return false;
        for(u32 a=0;a<3;++a)if(!std::isfinite(p.origin[a]) || !std::isfinite(p.extent[a]) || p.extent[a]<=0)return false;
        for(auto v:field)if((v&0x7c00u)==0x7c00u)return false;
        for(const auto& v:origins)for(float f:v)if(!std::isfinite(f))return false;
        WGPULimits limits=WGPU_LIMITS_INIT;
        if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success || p.counts[0]>limits.maxTextureDimension3D ||
           field.size()*2>limits.maxBufferSize || origins.size()*16>limits.maxStorageBufferBindingSize || (origins.size()+63)/64>limits.maxComputeWorkgroupsPerDimension)return false;
        device_=&device;++generation_;params_=p;params_.counts[1]=u32(origins.size());params_.counts[2]=1;
        TextureDesc td{};td.type=TextureType::Texture3D;td.size={p.counts[0],p.counts[0],p.counts[0]};td.format=DataFormat::R16_Float;
        td.usage=TextureUsage::ShaderResource|TextureUsage::CopyDest;field_=device.CreateTexture(td);if(field_==handles::INVALID_RESOURCE)return false;
        auto buffer=[&](u64 size,GPUMemoryUsage memory,BufferType type,const void* data){
            BufferDesc b{};b.size=size;b.memoryUsage=memory;b.type=type;
            auto h=device.CreateBuffer(b);if(h!=handles::INVALID_RESOURCE && data && !device.UpdateBufferData(h,data,size)){device.DestroyBuffer(h);return handles::INVALID_RESOURCE;}return h;
        };
        base_=buffer(origins.size()*16,GPUMemoryUsage::Static,BufferType::Raw,origins.data());
        output_=buffer(origins.size()*16,GPUMemoryUsage::Static,BufferType::Raw,nullptr);
        uniform_=buffer(sizeof(params_),GPUMemoryUsage::Dynamic,BufferType::Constant,&params_);
        readback_=buffer(origins.size()*16,GPUMemoryUsage::Readback,BufferType::Unknown,nullptr);
        const u32 n=p.counts[0],row=(n*2+255)&~255u;
        std::vector<u8> padded(u64(row)*n*n);
        for(u32 z=0;z<n;++z)for(u32 y=0;y<n;++y)std::memcpy(padded.data()+(u64(z)*n+y)*row,field.data()+(u64(z)*n+y)*n,n*2);
        staging_=buffer(padded.size(),GPUMemoryUsage::Static,BufferType::Raw,padded.data());
        for(auto h:{base_,output_,uniform_,readback_,staging_})if(h==handles::INVALID_RESOURCE)return false;
        std::ifstream source("Engine/Graphics/Dawn/shaders/Lumen/DDGIProbeRelocation.wgsl");std::string code((std::istreambuf_iterator<char>(source)),{});
        shader_=device.CreateShader(code.data(),code.size(),ShaderStage::Compute,"ddgi_relocate_probes");if(shader_==handles::INVALID_SHADER)return false;
        DescriptorSetLayoutBinding b[4]{{0,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},{1,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {2,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},{3,DescriptorType::UniformBuffer,1,ShaderStage::Compute,nullptr}};
        b[0].readonly=true;b[2].is3D=true;
        layout_=device.CreateDescriptorSetLayout({4,b});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layout_;pipelineLayout_=device.CreatePipelineLayout(pl);
        ComputePipelineDesc cp{};cp.computeShader=shader_;cp.layout=pipelineLayout_;pipeline_=device.CreateComputePipeline(cp);
        if(pipeline_==handles::INVALID_PIPELINE)return false;set_=device.CreateDescriptorSet({layout_});if(set_==handles::INVALID_DESCRIPTOR_SET)return false;
        DescriptorBufferInfo infos[3]{{base_,0,origins.size()*16},{output_,0,origins.size()*16},{uniform_,0,sizeof(params_)}};
        DescriptorImageInfo image{handles::INVALID_SAMPLER,field_,ResourceState::ShaderResource};WriteDescriptorSet writes[4]{};
        for(u32 i=0;i<4;++i){writes[i].dstSet=set_;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=b[i].descriptorType;
            if(i==2)writes[i].imageInfo=&image;else writes[i].bufferInfo=&infos[i==3?2:i];}device.UpdateDescriptorSets(4,writes);
        auto command=device.CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=device.GetCommandBuffer(command);
        if(!cmd || !cmd->Begin()){device.DestroyCommandBuffer(command);return false;}
        BufferTextureCopyRegion region{};region.bufferRowLength=row/2;region.bufferImageHeight=n;region.imageExtent={n,n,n};region.imageSubresource={0,0,1};
        cmd->CopyBufferToTexture(staging_,field_,&region,1);
        ResourceBarrier barrier{};barrier.resource=field_;barrier.beforeState=ResourceState::CopyDest;barrier.afterState=ResourceState::ShaderResource;barrier.subresource=~0u;
        cmd->InsertBarrier(&barrier,1);cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead);
        cmd->BindComputePipeline(pipeline_);cmd->BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&set_,0,nullptr);cmd->Dispatch((u32(origins.size())+63)/64,1,1);
        cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        cmd->CopyBuffer(output_,readback_,0,0,origins.size()*16);
        cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        QueueSubmitInfo submit{};submit.cmdBuffer=command;
        const bool completed=cmd->End()&&device.Submit(submit)&&cmd->WaitForCompletion();device.DestroyCommandBuffer(command);
        if(!completed)return false;
        const auto* results=static_cast<const ProbePoint*>(device.MapBuffer(readback_,0,origins.size()*16));if(!results)return false;
        points_.assign(results,results+origins.size());device.UnmapBuffer(readback_);
        u32 mismatches=0,borrowed=0,moved=0;float maxError=0;
        for(u32 i=0;i<origins.size();++i){const auto expected=Reference(origins[i],params_,field);float distance=0;
            for(u32 a=0;a<3;++a){const float delta=points_[i][a]-expected[a];distance+=delta*delta;}
            distance=std::sqrt(distance);maxError=std::max(maxError,distance);
            if(!std::isfinite(distance)||distance>1e-4f||points_[i][3]!=expected[3]){if(mismatches<12)PRIMAL_LOG_STREAM(Error,"DawnRelocation") << "probe=" << i << " got=" << points_[i][0] << "," << points_[i][1] << "," << points_[i][2] << "," << points_[i][3] << " expected=" << expected[0] << "," << expected[1] << "," << expected[2] << "," << expected[3];++mismatches;}
            borrowed+=points_[i][3]==.5f;moved+=points_[i]!=origins[i];
        }
        if(capture){std::ofstream raw(std::string(capture)+".bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(points_.data()),points_.size()*16);raw.close();
            std::ofstream meta(std::string(capture)+".json");meta << "{\"probes\":" << points_.size() << ",\"cpu_mismatches\":" << mismatches << ",\"max_cpu_distance\":" << maxError << ",\"borrowed\":" << borrowed << ",\"moved\":" << moved << ",\"completion_checked\":true,\"resolution\":" << n << "}\n";meta.close();if(!raw.good()||!meta.good())return false;}
        PRIMAL_LOG_STREAM(Info,"DawnRelocation") << "probes=" << points_.size() << " CPU mismatches=" << mismatches << " max_error=" << maxError << " borrowed=" << borrowed << " moved=" << moved;
        device.DestroyBuffer(staging_);staging_=handles::INVALID_RESOURCE;device.DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        ready_=mismatches==0;return ready_;
    }
    ResourceHandle Origins()const{return ready_?output_:handles::INVALID_RESOURCE;}
    ResourceHandle Field()const{return ready_?field_:handles::INVALID_RESOURCE;}
    const RelocationParameters& Parameters()const{return params_;}
    const std::vector<ProbePoint>& Points()const{return points_;}
    bool Ready()const{return ready_;}
    u64 Generation()const{return generation_;}
    static float Sample(const ProbePoint& point,const RelocationParameters& p,std::span<const u16> data){
        const u32 n=p.counts[0];int base[3];float f[3];
        for(u32 a=0;a<3;++a){const float local=point[a]-p.origin[a];if(local<0||local>=p.extent[a])return 1e10f;
            const float q=local/p.extent[a]*float(n)-.5f;base[a]=int(std::floor(q));f[a]=q-float(base[a]);}
        auto at=[&](int x,int y,int z){u16 bits=data[(u64(std::clamp(z,0,int(n)-1))*n+std::clamp(y,0,int(n)-1))*n+std::clamp(x,0,int(n)-1)];_Float16 value;std::memcpy(&value,&bits,2);return float(value);};
        auto mix=[](float a,float b,float t){return a*(1.f-t)+b*t;};
        const float z0=mix(mix(at(base[0],base[1],base[2]),at(base[0]+1,base[1],base[2]),f[0]),mix(at(base[0],base[1]+1,base[2]),at(base[0]+1,base[1]+1,base[2]),f[0]),f[1]);
        const float z1=mix(mix(at(base[0],base[1],base[2]+1),at(base[0]+1,base[1],base[2]+1),f[0]),mix(at(base[0],base[1]+1,base[2]+1),at(base[0]+1,base[1]+1,base[2]+1),f[0]),f[1]);return mix(z0,z1,f[2]);
    }
    static ProbePoint Reference(const ProbePoint& origin,const RelocationParameters& p,std::span<const u16> data){
        const float spacing=p.policy[0],voxel=p.policy[1],skin=std::max(voxel*1.5f,spacing*.05f),push=spacing*.95f,step=std::max(voxel,spacing*.1f);
        const float initial=Sample(origin,p,data);auto best=origin;best[3]=1;float bestD=initial;if(initial>=1e9f||initial>skin)return best;
        constexpr float d=.5773502691896258f;const float dirs[14][3]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{d,d,d},{-d,d,d},{d,-d,d},{-d,-d,d},{d,d,-d},{-d,d,-d},{d,-d,-d},{-d,-d,-d}};
        for(u32 round=0;round<3;++round){auto roundBest=best;float roundD=bestD;
            for(const auto& direction:dirs){auto point=best;for(u32 i=0;i<10;++i){float distance=0;for(u32 a=0;a<3;++a){point[a]=std::fma(direction[a],step,point[a]);const float delta=point[a]-origin[a];distance+=delta*delta;}if(distance>push*push)break;
                const float value=Sample(point,p,data);if(value<1e9f&&value>roundD){roundD=value;roundBest=point;}if(value>skin)break;}}
            if(roundD<=bestD)break;best=roundBest;bestD=roundD;if(bestD>skin)break;
        }
        if(bestD<=skin){auto freeBest=best;float freeD=bestD;for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){if(!x&&!y&&!z)continue;
            ProbePoint point{origin[0]+x*spacing,origin[1]+y*spacing,origin[2]+z*spacing,1};const float value=Sample(point,p,data);if(value<1e9f&&value>freeD){freeD=value;freeBest=point;}}
            if(freeD>skin){best=freeBest;best[3]=.5f;}}
        return best;
    }
    void Shutdown(){if(!device_)return;
        if(set_!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(set_);
        if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);
        if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);
        if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);
        if(shader_!=handles::INVALID_SHADER)device_->DestroyShader(shader_);
        for(auto h:{base_,output_,uniform_,readback_,staging_})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);
        if(field_!=handles::INVALID_RESOURCE)device_->DestroyTexture(field_);
        set_=handles::INVALID_DESCRIPTOR_SET;pipeline_=handles::INVALID_PIPELINE;pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;
        layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;shader_=handles::INVALID_SHADER;
        field_=base_=output_=uniform_=readback_=staging_=handles::INVALID_RESOURCE;
        device_=nullptr;ready_=false;points_.clear();
    }
private:
    DawnDevice* device_=nullptr;bool ready_=false;u64 generation_=0;RelocationParameters params_{};std::vector<ProbePoint> points_;
    ResourceHandle field_=handles::INVALID_RESOURCE,base_=handles::INVALID_RESOURCE,output_=handles::INVALID_RESOURCE,uniform_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE,staging_=handles::INVALID_RESOURCE;
    ShaderHandle shader_=handles::INVALID_SHADER;PipelineHandle pipeline_=handles::INVALID_PIPELINE;
    PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;DescriptorSetHandle set_=handles::INVALID_DESCRIPTOR_SET;
};
}
