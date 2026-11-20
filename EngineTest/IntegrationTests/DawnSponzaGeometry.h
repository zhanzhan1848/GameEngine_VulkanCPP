#pragma once
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/Utilities/GeometryBVHStore.h"
#include "Engine/Common/CoreLogStream.h"
#include "DawnSponzaRelocation.h"
#include <array>
#include <fstream>
#include <iterator>
#include <span>
#include <cmath>
#include <cstring>
#include <bit>
#include <limits>

namespace primal::tests::dawn_sponza {
using namespace graphics::rhi;
namespace geometry=graphics::ray_geometry;
struct MeasuredRay { float origin[4],direction[4]; };
struct MeasuredHit { float distanceNormal[4];u32 identity[4];float triangle[12],measured[4]; };
static_assert(sizeof(MeasuredRay)==32 && sizeof(MeasuredHit)==96);

// Owns a complete immutable scene upload and triple-buffered query workspaces.
// No CPU mapping of GPU-written storage: diagnostics use explicit readback.
class MeasuredGeometry {
public:
    ~MeasuredGeometry(){Shutdown();}
    MeasuredGeometry()=default;
    MeasuredGeometry(const MeasuredGeometry&)=delete;
    MeasuredGeometry& operator=(const MeasuredGeometry&)=delete;
    bool Initialize(DawnDevice& device,std::span<const geometry::InstanceInput> instances,
                    std::span<const std::array<float,4>> origins,u32 budget,u32 raysPerProbe,float farDistance,const ProbeRelocation* relocation=nullptr){
        if(device_ || instances.empty() || origins.empty() || !budget || budget>origins.size() ||
           raysPerProbe<64 || raysPerProbe%64 || !std::isfinite(farDistance) || farDistance<=0)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;
        if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success ||
           u64(budget)*raysPerProbe>0xffffffffu || u64(budget)*raysPerProbe*96>limits.maxStorageBufferBindingSize ||
           (u64(budget)*raysPerProbe+63)/64>limits.maxComputeWorkgroupsPerDimension ||
           limits.maxStorageBuffersPerShaderStage<6)return false;
        if(relocation && (!relocation->Ready() || relocation->Points().size()!=origins.size()))return false;
        relocation_=relocation;originGeneration_=relocation?relocation->Generation():0;
        device_=&device;budget_=budget;raysPerProbe_=raysPerProbe;far_=farDistance;
        sourceDigest_=geometry::SceneSourceDigest(instances);
        if(!sourceDigest_ || !scene_.Build(instances))return false;
        for(u64 bytes:{u64(scene_.top.size())*48,u64(scene_.bottom.size())*48,u64(scene_.triangles.size())*48,
                       u64(scene_.instances.size())*64,u64(origins.size())*16})
            if(!bytes || bytes>limits.maxStorageBufferBindingSize)return false;
        inputs_.assign(instances.begin(),instances.end());
        if(relocation)origins_=relocation->Points();else origins_.assign(origins.begin(),origins.end());
        for(const auto& o:origins_)for(float v:o)if(!std::isfinite(v))return false;
        auto storage=[&](u64 bytes,const void* data){
            BufferDesc b{};b.size=bytes;b.type=BufferType::Raw;b.memoryUsage=GPUMemoryUsage::Static;
            b.bindFlags=static_cast<u32>(BufferUsageFlags::Storage);
            auto h=device.CreateBuffer(b);if(h!=handles::INVALID_RESOURCE && data && !device.UpdateBufferData(h,data,bytes)){
                device.DestroyBuffer(h);return handles::INVALID_RESOURCE;
            }return h;
        };
        geometry_[0]=storage(scene_.top.size()*sizeof(geometry::Node),scene_.top.data());
        geometry_[1]=storage(scene_.bottom.size()*sizeof(geometry::Node),scene_.bottom.data());
        geometry_[2]=storage(scene_.triangles.size()*sizeof(geometry::Triangle),scene_.triangles.data());
        geometry_[3]=storage(scene_.instances.size()*sizeof(geometry::Instance),scene_.instances.data());
        originBuffer_=relocation?relocation->Origins():storage(origins_.size()*16,origins_.data());
        for(auto h:geometry_)if(h==handles::INVALID_RESOURCE)return false;
        if(originBuffer_==handles::INVALID_RESOURCE)return false;
        const char* paths[]{"Engine/Graphics/Dawn/shaders/Lumen/DDGIMeasuredRays.wgsl","Engine/Graphics/Dawn/shaders/Lumen/GeometryFirstHit.wgsl"};
        const char* entries[]{"ddgi_measured_rays","geometry_first_hit"};
        for(u32 stage=0;stage<2;++stage){
            std::ifstream in(paths[stage]);std::string source((std::istreambuf_iterator<char>(in)),{});
            if(source.empty())return false;
            shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,entries[stage]);
            if(shaders_[stage]==handles::INVALID_SHADER)return false;
            std::array<DescriptorSetLayoutBinding,7> bindings{};u32 count=stage?7:4;
            for(u32 i=0;i<count;++i){
                bindings[i]={i,i==count-1?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
                bindings[i].readonly=i<(stage?5u:2u);
            }
            layouts_[stage]=device.CreateDescriptorSetLayout({count,bindings.data()});
            if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
            PipelineLayoutDesc pd{};pd.setLayoutCount=1;pd.setLayouts=&layouts_[stage];
            pipelineLayouts_[stage]=device.CreatePipelineLayout(pd);
            if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;
            ComputePipelineDesc cp{};cp.computeShader=shaders_[stage];cp.layout=pipelineLayouts_[stage];
            pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
        }
        for(auto& slot:slots_){
            slot.rays=storage(u64(RayCount())*32,nullptr);slot.hits=storage(u64(RayCount())*96,nullptr);
            slot.indices=storage(u64(budget_)*4,nullptr);
            if(slot.rays==handles::INVALID_RESOURCE || slot.hits==handles::INVALID_RESOURCE || slot.indices==handles::INVALID_RESOURCE)return false;
            for(u32 stage=0;stage<2;++stage){
                BufferDesc b{};b.size=32;b.type=BufferType::Constant;b.memoryUsage=GPUMemoryUsage::Dynamic;
                slot.params[stage]=device.CreateBuffer(b);if(slot.params[stage]==handles::INVALID_RESOURCE)return false;
                slot.sets[stage]=device.CreateDescriptorSet({layouts_[stage]});
                if(slot.sets[stage]==handles::INVALID_DESCRIPTOR_SET)return false;
                ResourceHandle handles[]{stage?geometry_[0]:originBuffer_,stage?geometry_[1]:slot.indices,stage?geometry_[2]:slot.rays,
                    stage?geometry_[3]:slot.params[0],slot.rays,slot.hits,slot.params[1]};
                u64 sizes[]{stage?scene_.top.size()*48:origins_.size()*16,stage?scene_.bottom.size()*48:u64(budget_)*4,
                    stage?scene_.triangles.size()*48:u64(RayCount())*32,stage?scene_.instances.size()*64:32,u64(RayCount())*32,u64(RayCount())*96,32};
                const u32 count=stage?7:4;DescriptorBufferInfo infos[7]{};WriteDescriptorSet writes[7]{};
                for(u32 i=0;i<count;++i){
                    infos[i]={handles[i],0,sizes[i]};writes[i].dstSet=slot.sets[stage];writes[i].dstBinding=i;
                    writes[i].descriptorCount=1;writes[i].descriptorType=i==count-1?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;writes[i].bufferInfo=&infos[i];
                }device.UpdateDescriptorSets(count,writes);
            }
        }
        ready_=true;
        PRIMAL_LOG_STREAM(Info,"DawnGeometry") << "complete scene: top=" << scene_.top.size() << " bottom=" << scene_.bottom.size()
            << " triangles=" << scene_.triangles.size() << " instances=" << scene_.instances.size()
            << " probes=" << origins_.size() << " rays/update=" << RayCount() << " source=" << sourceDigest_;
        return true;
    }
    // Opt-in, once per immutable scene: replay exactly the CPU bytes uploaded
    // above. Rays/hits are captured independently after GPU completion.
    bool SaveSceneFixture(const char* path)const {
        if(!ready_ || !path || !*path)return false;
        std::ofstream out(path,std::ios::binary);
        const char magic[8]{'P','G','E','O','S','C','0','1'};
        const u32 counts[]{u32(scene_.top.size()),u32(scene_.bottom.size()),u32(scene_.triangles.size()),u32(scene_.instances.size())};
        out.write(magic,8);out.write(reinterpret_cast<const char*>(counts),sizeof(counts));
        out.write(reinterpret_cast<const char*>(&sourceDigest_),sizeof(sourceDigest_));
        auto write=[&](const auto& values){out.write(reinterpret_cast<const char*>(values.data()),values.size()*sizeof(values[0]));};
        write(scene_.top);write(scene_.bottom);write(scene_.triangles);write(scene_.instances);out.close();return out.good();
    }
    // Legacy fixture scheduling. The scene supplies the qualified host plan.
    bool Record(DawnCommandBuffer& cmd,u32 frame){
        std::vector<u32> updates(budget_);
        for(u32 i=0;i<budget_;++i)updates[i]=(u64(frame)*budget_+i)%origins_.size();
        return RecordSelected(cmd,frame,updates);
    }
    bool RecordSelected(DawnCommandBuffer& cmd,u32 frame,std::span<const u32> updates){
        if(!ready_ || !OriginsCurrent() || updates.size()>budget_)return false;
        std::vector<bool> seen(origins_.size());
        for(u32 i:updates){if(i>=origins_.size()||seen[i])return false;seen[i]=true;}
        auto& s=slots_[frame%3];s.frame=~0u;s.updates.assign(updates.begin(),updates.end());
        const u32 count=u32(updates.size()),rays=count*raysPerProbe_;
        struct RayParams {u32 counts[4];float distance[4];} rp{{count,raysPerProbe_,frame,1},{far_,0,0,0}};
        u32 qp[]{u32(scene_.top.size()),u32(scene_.bottom.size()),u32(scene_.triangles.size()),u32(scene_.instances.size()),rays,1,0,0};
        if((count&&!device_->UpdateBufferData(s.indices,s.updates.data(),u64(count)*4)) ||
           !device_->UpdateBufferData(s.params[0],&rp,sizeof(rp)) || !device_->UpdateBufferData(s.params[1],qp,sizeof(qp)))return false;
        cmd.MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead);
        if(rays)for(u32 stage=0;stage<2;++stage){
            cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&s.sets[stage],0,nullptr);
            cmd.Dispatch((rays+63)/64,1,1);
            cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
        }s.frame=frame;return true;
    }
    struct RecordedOutput {
        ResourceHandle rays=handles::INVALID_RESOURCE,hits=handles::INVALID_RESOURCE;
        u32 rayCount=0,frame=~0u;u64 sceneDigest=0;
        // True only while the completed upstream SDF relocation is current.
        bool relocatedOriginsQualified=false;
        ResourceHandle updateIndices=handles::INVALID_RESOURCE,origins=handles::INVALID_RESOURCE;
        u32 raysPerProbe=0,probeCount=0;u64 originsGeneration=0;
    };
    RecordedOutput GetRecordedOutput(u32 frame)const{
        const auto& s=slots_[frame%3];
        if(!ready_ || s.frame!=frame || !OriginsCurrent())return {};
        return {s.rays,s.hits,u32(s.updates.size())*raysPerProbe_,frame,sourceDigest_,relocation_!=nullptr,s.indices,originBuffer_,raysPerProbe_,u32(origins_.size()),originGeneration_};
    }
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(!ready_ || !OriginsCurrent() || slots_[frame%3].frame!=frame || readback_!=handles::INVALID_RESOURCE)return false;
        captureRays_=u32(slots_[frame%3].updates.size())*raysPerProbe_;
        BufferDesc b{};b.size=std::max<u64>(4,u64(captureRays_)*128);b.memoryUsage=GPUMemoryUsage::Readback;
        readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
        captureFrame_=frame;
        const auto& s=slots_[frame%3];
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        if(captureRays_)cmd.CopyBuffer(s.rays,readback_,0,0,u64(captureRays_)*32);
        if(captureRays_)cmd.CopyBuffer(s.hits,readback_,0,u64(captureRays_)*32,u64(captureRays_)*96);
        cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        return true;
    }
    // Caller must first wait for the submitted command that contains Capture.
    bool VerifyAndSave(const std::string& prefix,u32 frame){
        if(readback_==handles::INVALID_RESOURCE || captureFrame_!=frame || slots_[frame%3].frame!=frame)return false;
        const u64 bytes=u64(captureRays_)*128;const auto* data=static_cast<const u8*>(device_->MapBuffer(readback_,0,std::max<u64>(4,bytes)));
        if(!data)return false;
        const auto* rays=reinterpret_cast<const MeasuredRay*>(data);
        const auto* hits=reinterpret_cast<const MeasuredHit*>(data+u64(captureRays_)*32);
        u32 mismatches=0,hitCount=0,missCount=0,unknown=0,checked=0,identityAmbiguities=0;
        float maxError=0;
        for(u32 i=0;i<captureRays_;++i){
            const auto& r=rays[i];const auto& h=hits[i];
            if(h.identity[0]==1)++hitCount;else if(h.identity[0]==0)++missCount;else ++unknown;
            const auto& origin=origins_[slots_[frame%3].updates[i/raysPerProbe_]];
            const auto direction=ReferenceDirection(i%raysPerProbe_,raysPerProbe_,frame);
            for(u32 k=0;k<3;++k)if(r.origin[k]!=origin[k] || !std::isfinite(r.direction[k]) || std::abs(r.direction[k]-direction[k])>5e-6f)++mismatches;
            if(r.origin[3]!=0 || r.direction[3]!=far_)++mismatches;
            // CPU oracle uses actual GPU directions, independent double-triangle
            // traversal implementation; all rays checked, not a few screenshots.
            geometry::Hit expected{};const int result=geometry::Trace(scene_,r.origin,r.direction,0,far_,expected);++checked;
            if(h.identity[0]!=(result<0?~0u:u32(result))){
                if(mismatches<16)PRIMAL_LOG_STREAM(Error,"DawnGeometry") << "ray=" << i << " outcome=" << h.identity[0] << " expected=" << result;
                ++mismatches;continue;
            }
            if(result==0){
                double low=far_,high=std::numeric_limits<float>::max();bool ahead=true;
                const auto& root=scene_.top[0];
                for(u32 k=0;k<3;++k){
                    if(r.direction[k]==0){if(r.origin[k]<root.lo[k]||r.origin[k]>root.hi[k])ahead=false;}
                    else{const double a=(double(root.lo[k])-r.origin[k])/r.direction[k],b=(double(root.hi[k])-r.origin[k])/r.direction[k];low=std::max(low,std::min(a,b));high=std::min(high,std::max(a,b));}
                }
                ahead=ahead&&low<=high;
                if(h.identity[3]!=0 || h.measured[0]!=(ahead?-3.f:-2.f))++mismatches;
            }
            if(result==1){
                const bool qualified=h.distanceNormal[0]>2e-5f && h.distanceNormal[0]<far_-2e-5f && h.identity[1]<=65535u;
                if(h.identity[3]!=(qualified?h.identity[1]+2u:0u) || h.measured[0]!=(qualified?h.distanceNormal[0]:-1.f))++mismatches;
                if(qualified){float length=0,dot=0;for(u32 k=0;k<3;++k){length+=h.measured[k+1]*h.measured[k+1];dot+=h.measured[k+1]*r.direction[k];}
                    if(!std::isfinite(length)||std::abs(length-1.f)>2e-4f||dot>1e-5f)++mismatches;}
                const float error=std::abs(expected.t-h.distanceNormal[0]);maxError=std::max(maxError,error);
                const bool sameIdentity=expected.instance_id==h.identity[1] && expected.triangle_id==h.identity[2];
                // Ray/triangle subtraction occurs in world-scale coordinates;
                // cancellation error depends on the origin as well as hit t.
                const float coordinateScale=std::max({1.f,std::abs(expected.t),std::abs(r.origin[0]),std::abs(r.origin[1]),std::abs(r.origin[2])});
                const float tolerance=16.f*std::numeric_limits<float>::epsilon()*coordinateScale;
                const bool ambiguous=!sameIdentity && error<=tolerance && IsNearestCandidate(r,h,expected.t,tolerance);
                if(ambiguous)++identityAmbiguities;
                if(!std::isfinite(error) || error>tolerance || (!sameIdentity && !ambiguous)){
                    if(mismatches<16)PRIMAL_LOG_STREAM(Error,"DawnGeometry") << "ray=" << i << " t=" << h.distanceNormal[0]
                        << " expected_t=" << expected.t << " instance=" << h.identity[1] << " expected_instance=" << expected.instance_id
                        << " triangle=" << h.identity[2] << " expected_triangle=" << expected.triangle_id;
                    ++mismatches;
                }
            }
        }
        std::ofstream raw(prefix+".bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(data),bytes);raw.close();
        std::ofstream meta(prefix+".json");meta << "{\"frame\":" << frame << ",\"rays\":" << captureRays_ << ",\"cpu_checked\":" << checked
            << ",\"mismatches\":" << mismatches << ",\"identity_ambiguities\":" << identityAmbiguities << ",\"identity_exact\":" << (identityAmbiguities?"false":"true") << ",\"hits\":" << hitCount << ",\"misses\":" << missCount << ",\"unknown\":" << unknown
            << ",\"max_distance_error\":" << maxError << ",\"source_digest\":\"" << sourceDigest_
            << "\",\"completion_checked\":true,\"origin_scope\":\"" << (relocation_?"completed SDF relocation":"pre-relocation grid bring-up") << "\",\"radiance_connected\":false,\"ray_stride\":32,\"hit_stride\":96}\n";
        meta.close();const bool ok=raw.good()&&meta.good()&&mismatches==0;
        device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        PRIMAL_LOG_STREAM(Info,"DawnGeometry") << "frame=" << frame << " CPU checked=" << checked << " mismatches=" << mismatches
            << " identity_ambiguities=" << identityAmbiguities << " hits=" << hitCount << " misses=" << missCount << " unknown=" << unknown << " max_error=" << maxError;
        return ok;
    }
    static std::array<float,3> ReferenceDirection(u32 ray,u32 count,u32 frame){
        auto hash=[](u32 x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);};
        const u32 cell=ray/(count/64),seed=hash(frame^0x9e3779b9u)^hash(ray^0x85ebca6bu);
        const float jx=(float(hash(seed^0x68bc21ebu)>>14)+.5f)/262144.f;
        const float jy=(float(hash(seed^0x02e5be93u)>>14)+.5f)/262144.f;
        float x=(float(cell%8)+jx)/8.f*2.f-1.f,y=(float(cell/8)+jy)/8.f*2.f-1.f;
        const float z=1.f-std::abs(x)-std::abs(y);
        if(z<0){const float oldX=x;x=(1.f-std::abs(y))*(x>=0?1.f:-1.f);y=(1.f-std::abs(oldX))*(y>=0?1.f:-1.f);}
        const float length=std::sqrt(x*x+y*y+z*z);return {x/length,y/length,z/length};
    }
    struct SceneView {
        std::array<ResourceHandle,4> buffers{};
        std::array<u32,4> counts{};
        u64 digest=0;
    };
    // Immutable borrowed buffers; the query owner must outlive screen consumers.
    SceneView GetSceneView()const {
        if(!ready_ || !OriginsCurrent())return {};
        return {geometry_,{u32(scene_.top.size()),u32(scene_.bottom.size()),
            u32(scene_.triangles.size()),u32(scene_.instances.size())},sourceDigest_};
    }
    u64 SceneDigest()const{return ready_?sourceDigest_:0;}
    u32 RayCount()const{return budget_*raysPerProbe_;}
    void Shutdown(){
        if(!device_)return;
        for(auto& s:slots_){for(auto h:s.sets)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);
            for(auto h:{s.rays,s.hits,s.indices,s.params[0],s.params[1]})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}
        for(auto h:geometry_)if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);geometry_.fill(handles::INVALID_RESOURCE);
        if(!relocation_ && originBuffer_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(originBuffer_);
        if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);
        originBuffer_=readback_=handles::INVALID_RESOURCE;
        for(u32 i=0;i<2;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);
            if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);
            if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);
            if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);}
        shaders_.fill(handles::INVALID_SHADER);pipelines_.fill(handles::INVALID_PIPELINE);
        pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);
        scene_={};inputs_.clear();origins_.clear();sourceDigest_=0;captureFrame_=~0u;
        device_=nullptr;ready_=false;relocation_=nullptr;originGeneration_=0;
    }
private:
    bool OriginsCurrent()const{return !relocation_ || (relocation_->Ready() && relocation_->Generation()==originGeneration_ && relocation_->Origins()==originBuffer_);}
    const ProbeRelocation* relocation_=nullptr;u64 originGeneration_=0;
    bool IsNearestCandidate(const MeasuredRay& ray,const MeasuredHit& hit,float nearest,float tolerance)const{
        // A double-precision nearest-hit oracle cannot impose one ID on
        // coplanar duplicates whose float t differs by a few ULPs. Require the
        // GPU-selected original triangle to independently intersect at the
        // same nearest distance; retain the ambiguity instead of claiming ID parity.
        const geometry::InstanceInput* input=nullptr;
        const geometry::Instance* transform=nullptr;
        for(const auto& v:inputs_)if(v.instance_id==hit.identity[1]){input=&v;break;}
        for(const auto& v:scene_.instances)if(v.instance_id==hit.identity[1]){transform=&v;break;}
        if(!input || !transform)return false;
        const geometry::Triangle* triangle=nullptr;
        for(const auto& t:input->mesh->triangles)if(std::bit_cast<u32>(t.a[3])==hit.identity[2]){triangle=&t;break;}
        if(!triangle || std::memcmp(triangle,hit.triangle,48)!=0)return false;
        double o[3]{},d[3]{},v[3],p[3],q[3];
        for(u32 k=0;k<3;++k){o[k]=transform->inverse_rows[k][3];for(u32 a=0;a<3;++a){o[k]+=double(transform->inverse_rows[k][a])*ray.origin[a];d[k]+=double(transform->inverse_rows[k][a])*ray.direction[a];}v[k]=o[k]-triangle->a[k];}
        for(u32 k=0;k<3;++k){const u32 a=(k+1)%3,b=(k+2)%3;p[k]=d[a]*triangle->e2[b]-d[b]*triangle->e2[a];q[k]=v[a]*triangle->e1[b]-v[b]*triangle->e1[a];}
        double determinant=0,u=0,bary=0,t=0;
        for(u32 k=0;k<3;++k){determinant+=triangle->e1[k]*p[k];u+=v[k]*p[k];bary+=d[k]*q[k];t+=triangle->e2[k]*q[k];}
        if(determinant==0)return false;u/=determinant;bary/=determinant;t/=determinant;
        return std::isfinite(t)&&u>=0&&bary>=0&&u+bary<=1&&t>=0&&t<=far_&&std::abs(t-nearest)<=tolerance;
    }
    struct Slot {ResourceHandle rays=handles::INVALID_RESOURCE,hits=handles::INVALID_RESOURCE,indices=handles::INVALID_RESOURCE;
        std::array<ResourceHandle,2> params{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
        std::array<DescriptorSetHandle,2> sets{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};
        std::vector<u32> updates;u32 frame=~0u;};
    DawnDevice* device_=nullptr;bool ready_=false;u32 budget_=0,raysPerProbe_=0,captureFrame_=~0u,captureRays_=0;float far_=0;u64 sourceDigest_=0;
    std::vector<geometry::InstanceInput> inputs_;geometry::SceneBVH scene_;std::vector<std::array<float,4>> origins_;std::array<Slot,3> slots_{};
    std::array<ResourceHandle,4> geometry_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
    ResourceHandle originBuffer_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;
    std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};
    std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};
};
}
