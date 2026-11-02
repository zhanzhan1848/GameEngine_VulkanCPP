// Direct production geometryBox finite-input containment tests.
#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace primal;
using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace {
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
struct GPU {
    RHIDeviceBase* base=nullptr;
    VulkanDevice* vk=nullptr;
    std::vector<ResourceHandle> buffers;
    ShaderHandle shader=handles::INVALID_SHADER;
    DescriptorSetLayoutHandle layout=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;
    DescriptorSetHandle ds=handles::INVALID_DESCRIPTOR_SET;
    PipelineHandle pipe=handles::INVALID_PIPELINE;
    GPU() {
        DeviceDesc d{}; d.platform=RHIPlatform::Vulkan; d.enableValidation=true; d.enableDebug=true;
        base=CreateRHIDevice(d); check(base!=nullptr,"Vulkan device init"); vk=static_cast<VulkanDevice*>(base);
    }
    ~GPU() {
        if(!base) return;
        if(ds!=handles::INVALID_DESCRIPTOR_SET) base->DestroyDescriptorSet(ds);
        if(pipe!=handles::INVALID_PIPELINE) base->DestroyPipeline(pipe);
        if(pl!=handles::INVALID_PIPELINE_LAYOUT) base->DestroyPipelineLayout(pl);
        if(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT) base->DestroyDescriptorSetLayout(layout);
        if(shader!=handles::INVALID_SHADER) base->DestroyShader(shader);
        for(auto b:buffers) base->DestroyBuffer(b);
        base->Shutdown();
    }
    ResourceHandle buffer(size_t bytes,BufferType type,GPUMemoryUsage usage,const void* data=nullptr) {
        BufferDesc b{}; b.size=bytes;b.type=type;b.memoryUsage=usage;b.name="MeasuredVisibilityTest";
        auto h=base->CreateBuffer(b);check(h!=handles::INVALID_RESOURCE,"CreateBuffer");buffers.push_back(h);
        if(data) check(base->UpdateBufferData(h,data,bytes,0),"Upload test buffer");
        return h;
    }
    void pipeline(const char* path,const char* entry,const std::vector<DescriptorType>& types,
                  const std::vector<DescriptorBufferInfo>& infos) {
        std::ifstream f(path,std::ios::binary|std::ios::ate);check(bool(f),"Open private test SPV");
        const auto size=f.tellg();check(size>0,"Nonempty SPV");std::vector<u8> bytes(static_cast<size_t>(size));
        f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),size);check(bool(f),"Read complete SPV");
        shader=base->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,entry);
        check(shader!=handles::INVALID_SHADER,"CreateShader");
        std::vector<DescriptorSetLayoutBinding> bindings(types.size());
        for(u32 i=0;i<types.size();++i) bindings[i]={i,types[i],1,ShaderStage::Compute,nullptr};
        DescriptorSetLayoutDesc ld{};ld.bindingCount=bindings.size();ld.bindings=bindings.data();
        layout=base->CreateDescriptorSetLayout(ld);check(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"Create layout");
        PipelineLayoutDesc pd{};pd.setLayoutCount=1;pd.setLayouts=&layout;
        pl=base->CreatePipelineLayout(pd);check(pl!=handles::INVALID_PIPELINE_LAYOUT,"Create pipeline layout");
        DescriptorSetDesc sd{};sd.layout=layout;ds=base->CreateDescriptorSet(sd);
        check(ds!=handles::INVALID_DESCRIPTOR_SET,"Create descriptor set");
        std::vector<WriteDescriptorSet> writes(types.size());
        for(u32 i=0;i<types.size();++i) writes[i]={ds,i,0,1,types[i],nullptr,&infos[i]};
        base->UpdateDescriptorSets(writes.size(),writes.data());
        ComputePipelineDesc cd{};cd.computeShader=shader;cd.layout=pl;
        pipe=base->CreateComputePipeline(cd);check(pipe!=handles::INVALID_PIPELINE,"Create compute pipeline");
    }
    std::vector<u8> run(ResourceHandle output,size_t bytes,u32 groupsX,u32 groupsY=1) {
        auto rb=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Readback);
        auto cmd=base->CreateCommandBuffer(CommandQueueType::Compute);
        auto* c=vk->GetCommandBuffer(cmd);check(c!=nullptr,"Get command buffer");
        check(c->Reset()&&c->Begin(),"Begin command buffer");
        c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,
                         AccessFlag::ShaderRead|AccessFlag::UniformRead);
        c->BindComputePipeline(pipe);c->BindDescriptorSets(PipelineBindPoint::Compute,pl,0,1,&ds,0,nullptr);
        c->Dispatch(groupsX,groupsY,1);
        c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        c->CopyBuffer(output,rb,0,0,bytes);
        c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        check(c->End()&&c->Submit(0)&&c->WaitForCompletion(),"Complete GPU dispatch");base->DestroyCommandBuffer(cmd);
        auto* data=static_cast<const u8*>(base->MapBuffer(rb,0,bytes));check(data!=nullptr,"Map readback");
        std::vector<u8> result(data,data+bytes);base->UnmapBuffer(rb);return result;
    }
};


struct alignas(16) Case {float parentLo[4],parentHi[4],childLo[4],childHi[4],originNear[4],directionFar[4];};
struct alignas(16) Answer {u32 parent,child,violation,index;};
static_assert(sizeof(Case)==96&&sizeof(Answer)==16);
void add(std::vector<Case>& cases,std::array<float,3> lo,std::array<float,3> hi,std::array<float,3> origin,std::array<float,3> direction,float nearT,float farT){
    Case c{};for(int k=0;k<3;++k){check(std::isfinite(lo[k])&&std::isfinite(hi[k])&&lo[k]<=hi[k],"finite ordered child");c.childLo[k]=lo[k];c.childHi[k]=hi[k];c.parentLo[k]=std::nextafter(lo[k],-std::numeric_limits<float>::infinity());c.parentHi[k]=std::nextafter(hi[k],std::numeric_limits<float>::infinity());if(!std::isfinite(c.parentLo[k]))c.parentLo[k]=lo[k];if(!std::isfinite(c.parentHi[k]))c.parentHi[k]=hi[k];c.originNear[k]=origin[k];c.directionFar[k]=direction[k];check(std::isfinite(origin[k])&&std::isfinite(direction[k])&&c.parentLo[k]<=lo[k]&&hi[k]<=c.parentHi[k],"finite contained parent");}c.originNear[3]=nearT;c.directionFar[3]=farT;cases.push_back(c);
}
TestResult RunContainment(){
    const float fmax=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    std::vector<Case> cases;
    // GPU-known positive and negative controls, not host-derived geometry results.
    add(cases,{-1,-1,-1},{1,1,1},{0,0,0},{1,0,0},0,100);
    add(cases,{-1,-1,-1},{1,1,1},{0,2,0},{1,0,0},0,100);
    const std::array<float,13> dirs={0.f,-0.f,tiny,-tiny,1e-37f,-1e-37f,1e-10f,-1e-10f,1.f,-1.f,1e20f,-1e20f,fmax};
    const std::array<float,10> anchors={0.f,-0.f,tiny,-tiny,1.f,-1.f,1024.f,-1024.f,fmax*.5f,-fmax*.5f};
    for(float center:anchors)for(float d:dirs)for(int axis=0;axis<3;++axis)for(int kind=0;kind<8;++kind){
        std::array<float,3> lo{-1,-1,-1},hi{1,1,1},o{0,0,0},v{.5f,.25f,1.f};
        const float prev=std::nextafter(center,-std::numeric_limits<float>::infinity()),next=std::nextafter(center,std::numeric_limits<float>::infinity());
        lo[axis]=prev;hi[axis]=next;v[axis]=d;
        switch(kind){case 0:o[axis]=center;break;case 1:o[axis]=prev;break;case 2:o[axis]=next;break;case 3:o[axis]=std::nextafter(prev,-fmax);break;case 4:o[axis]=std::nextafter(next,fmax);break;case 5:o[axis]=0;break;case 6:o[axis]=center>0?-fmax:fmax;break;default:o[axis]=center;v[(axis+1)%3]=-0.f;}
        add(cases,lo,hi,o,v,0,fmax);
    }
    GPU g;const size_t bytes=cases.size()*sizeof(Case),outbytes=cases.size()*sizeof(Answer);
    auto input=g.buffer(bytes,BufferType::Raw,GPUMemoryUsage::Dynamic,cases.data());
    auto output=g.buffer(outbytes,BufferType::Raw,GPUMemoryUsage::Static);std::array<u32,16> empty{};
    auto dummy=g.buffer(sizeof(empty),BufferType::Raw,GPUMemoryUsage::Dynamic,empty.data());
    std::vector<DescriptorType> types(17,DescriptorType::StorageBuffer);
    std::vector<DescriptorBufferInfo> infos(17,{dummy,0,sizeof(empty)});infos[0]={input,0,bytes};infos[1]={output,0,outbytes};
    g.pipeline(GEOMETRY_BOX_TEST_SPV,"geometry_box_test",types,infos);
    auto raw=g.run(output,outbytes,(u32(cases.size())+63)/64);std::vector<Answer> result(cases.size());std::memcpy(result.data(),raw.data(),outbytes);
    check(result[0].parent==1&&result[0].child==1,"GPU hit control");check(result[1].parent==0&&result[1].child==0,"GPU miss control");
    unsigned violations=0,childHits=0,parentOnly=0;
    for(u32 i=0;i<result.size();++i){auto a=result[i];check(a.index==i&&a.parent<=1&&a.child<=1&&a.violation==u32(a.child&&!a.parent),"complete coherent output");childHits+=a.child;parentOnly+=a.parent&&!a.child;if(a.child&&!a.parent){++violations;if(violations<=32)std::cerr<<"[GEOMETRY_BOX] containment_violation case="<<i<<" parent="<<a.parent<<" child="<<a.child<<" parentLo="<<std::hexfloat<<cases[i].parentLo[0]<<","<<cases[i].parentLo[1]<<","<<cases[i].parentLo[2]<<" parentHi="<<cases[i].parentHi[0]<<","<<cases[i].parentHi[1]<<","<<cases[i].parentHi[2]<<" childLo="<<cases[i].childLo[0]<<","<<cases[i].childLo[1]<<","<<cases[i].childLo[2]<<" childHi="<<cases[i].childHi[0]<<","<<cases[i].childHi[1]<<","<<cases[i].childHi[2]<<" origin="<<cases[i].originNear[0]<<","<<cases[i].originNear[1]<<","<<cases[i].originNear[2]<<" direction="<<cases[i].directionFar[0]<<","<<cases[i].directionFar[1]<<","<<cases[i].directionFar[2]<<std::defaultfloat<<"\n";}}
    std::cout<<"[GEOMETRY_BOX] cases="<<cases.size()<<" child_hits="<<childHits<<" parent_only="<<parentOnly<<" violations="<<violations<<"\n";
    check(childHits>1,"positive inputs beyond control");check(violations==0,"production box child accepted but containing parent rejected");return TestResult::Passed;
}
}
int main(){auto s=std::make_shared<TestSuite>("VulkanGeometryBoxContainment");s->AddTestCase(TestCase("production_geometryBox_containment",RunContainment));TestRunner::RegisterTestSuite(s);auto n=TestRunner::RunAllSuites();return n.totalTests==1&&n.passedTests==1&&n.failedTests==0&&n.skippedTests==0?0:1;}
