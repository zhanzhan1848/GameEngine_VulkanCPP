// Actual production irradiance old/new GPU history contracts; no test-side sampler or SH kernel.
#include "../../TestFramework.h"
#include "Graphics/Lumen/DDGI/FarUpdatePolicy.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include <algorithm>
#include <array>
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
constexpr unsigned Rays=128, Frames=64, Cells=64;
constexpr double Pi=3.14159265358979323846;
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
bool finite(float x) { u32 bits; std::memcpy(&bits,&x,4); return (bits&0x7f800000u)!=0x7f800000u; }
struct V { double x,y,z; };
V sub(V a,V b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
double length(V a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
V cross(V a,V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
V decode(double u,double v) {
    V q{2*u-1,2*v-1,1-std::abs(2*u-1)-std::abs(2*v-1)};
    if(q.z<0) { const double x=q.x; q.x=(1-std::abs(q.y))*(x>=0?1:-1); q.y=(1-std::abs(x))*(q.y>=0?1:-1); }
    const double n=length(q); return {q.x/n,q.y/n,q.z/n};
}
std::array<double,2> encode(V d) {
    const double l1=std::abs(d.x)+std::abs(d.y)+std::abs(d.z);
    double x=d.x/l1,y=d.y/l1;
    if(d.z<0) {const double old=x; x=(1-std::abs(y))*(old>=0?1:-1); y=(1-std::abs(old))*(y>=0?1:-1);}
    return {(x+1)*.5,(y+1)*.5};
}
double pdf(V d) { const double l1=std::abs(d.x)+std::abs(d.y)+std::abs(d.z); return .25/(l1*l1*l1); }
std::array<double,9> sh(V d) {
    return {.28209479177387814,.4886025119029199*d.x,.4886025119029199*d.y,.4886025119029199*d.z,
        1.0925484305920792*d.x*d.y,1.0925484305920792*d.y*d.z,
        .31539156525252005*(3*d.z*d.z-1),1.0925484305920792*d.x*d.z,
        .5462742152960396*(d.x*d.x-d.y*d.y)};
}
struct Sample { float d[4]; u32 identity[4]; };
static_assert(sizeof(Sample)==32);

// Small private harness: all input uploads precede dispatch; shader writes are
// published to transfer, then host. Map only follows a successful completion.
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
        if(ds!=handles::INVALID_DESCRIPTOR_SET) {base->DestroyDescriptorSet(ds);ds=handles::INVALID_DESCRIPTOR_SET;}
        if(pipe!=handles::INVALID_PIPELINE) {base->DestroyPipeline(pipe);pipe=handles::INVALID_PIPELINE;}
        if(pl!=handles::INVALID_PIPELINE_LAYOUT) {base->DestroyPipelineLayout(pl);pl=handles::INVALID_PIPELINE_LAYOUT;}
        if(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT) {base->DestroyDescriptorSetLayout(layout);layout=handles::INVALID_DESCRIPTOR_SET_LAYOUT;}
        if(shader!=handles::INVALID_SHADER) {base->DestroyShader(shader);shader=handles::INVALID_SHADER;}
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
    std::vector<u8> run(ResourceHandle output,size_t bytes,u32 groupsX,u32 groupsY=1, ResourceHandle inherit=handles::INVALID_RESOURCE) {
        auto rb=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Readback);
        auto cmd=base->CreateCommandBuffer(CommandQueueType::Compute);
        auto* c=vk->GetCommandBuffer(cmd);check(c!=nullptr,"Get command buffer");
        check(c->Reset()&&c->Begin(),"Begin command buffer");
        c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,
                         AccessFlag::ShaderRead|AccessFlag::UniformRead);
        if(inherit!=handles::INVALID_RESOURCE) {
            c->MemoryBarrier(PipelineStage::AllCommands|PipelineStage::Host,PipelineStage::Transfer,
                AccessFlag::MemoryRead|AccessFlag::MemoryWrite|AccessFlag::HostWrite,AccessFlag::TransferRead|AccessFlag::TransferWrite);
            c->CopyBuffer(inherit,output,0,0,bytes);
            c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,
                AccessFlag::TransferWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
        }
        c->BindComputePipeline(pipe);c->BindDescriptorSets(PipelineBindPoint::Compute,pl,0,1,&ds,0,nullptr);
        c->Dispatch(groupsX,groupsY,1);
        c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        c->CopyBuffer(output,rb,0,0,bytes);
        c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        check(c->End()&&c->Submit(0)&&c->WaitForCompletion(),"Complete GPU dispatch");base->DestroyCommandBuffer(cmd);
        auto* data=static_cast<const u8*>(base->MapBuffer(rb,0,bytes));check(data!=nullptr,"Map readback");
        std::vector<u8> result(data,data+bytes);base->UnmapBuffer(rb);return result;
    }
    std::vector<u8> read(ResourceHandle output,size_t bytes) {
        auto rb=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Readback);
        auto handle=base->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(handle);
        check(c&&c->Reset()&&c->Begin(),"Begin readback");
        c->MemoryBarrier(PipelineStage::AllCommands,PipelineStage::Transfer,AccessFlag::MemoryWrite,AccessFlag::TransferRead);
        c->CopyBuffer(output,rb,0,0,bytes);
        c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        check(c->End()&&c->Submit(0)&&c->WaitForCompletion(),"Complete readback");base->DestroyCommandBuffer(handle);
        auto* data=static_cast<const u8*>(base->MapBuffer(rb,0,bytes));check(data,"Map readback");
        std::vector<u8> result(data,data+bytes);base->UnmapBuffer(rb);return result;
    }
};

#include "DDGIFarSparseHistoryFixture.h"

struct Batch {
    std::vector<float> values;
    std::vector<float> conv;
};
Batch runHistory(GPU& gpu,const char* path,unsigned mode) {
    // One controlled state per probe, fixed frame/directions/radiance/history.
    // The first 21 entries are consecutive pre-update confidence states;
    // additional entries bracket the old .5 transition exactly.
    std::vector<float> conv;
    float age=0.f;
    for(unsigned i=0;i<21;++i){conv.push_back(age);age=std::min(age+.0333f,1.f);}
    conv.insert(conv.end(),{.4999f,.5f,.5001f,1.f});
    const unsigned count=conv.size();
    std::vector<float> rays(count*Rays*4),hist(count*9*4),confidence(count*4),opensky(count*2);
    std::vector<u32> list(count);
    for(unsigned p=0;p<count;++p){
        list[p]=p;confidence[p*4+3]=conv[p];
        for(unsigned r=0;r<Rays;++r){auto at=(p*Rays+r)*4;rays[at]=.17f;rays[at+1]=.31f;rays[at+2]=.47f;rays[at+3]=1.f;}
        for(unsigned c=0;c<9;++c)for(unsigned channel=0;channel<3;++channel)
            hist[(p*9+c)*4+channel]=c==0?float(channel+2):.002f;
    }
    std::array<u32,84> volume{};std::array<u32,128> globals{};
    auto vf=[&](unsigned at,float f){std::memcpy(reinterpret_cast<u8*>(volume.data())+at,&f,4);};
    volume[12]=Rays;volume[13]=count;volume[17]=31;vf(72,50);vf(80,.05f);
    volume[21]=count;volume[27]=mode;vf(160,.01f);
    auto g=gpu.buffer(sizeof(globals),BufferType::Constant,GPUMemoryUsage::Dynamic,globals.data());
    auto v=gpu.buffer(sizeof(volume),BufferType::Constant,GPUMemoryUsage::Dynamic,volume.data());
    auto r=gpu.buffer(rays.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,rays.data());
    auto h=gpu.buffer(hist.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,hist.data());
    auto o=gpu.buffer(hist.size()*4,BufferType::Structured,GPUMemoryUsage::Static);
    auto l=gpu.buffer(list.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,list.data());
    auto c=gpu.buffer(confidence.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,confidence.data());
    auto sky=gpu.buffer(opensky.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,opensky.data());
    gpu.pipeline(path,"ddgi_update_irradiance",
       {DescriptorType::UniformBuffer,DescriptorType::UniformBuffer,DescriptorType::StorageBuffer,
        DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,
        DescriptorType::StorageBuffer,DescriptorType::StorageBuffer},
       {{g,0,sizeof(globals)},{v,0,sizeof(volume)},{r,0,rays.size()*4},{h,0,hist.size()*4},
        {o,0,hist.size()*4},{l,0,list.size()*4},{c,0,confidence.size()*4},{sky,0,opensky.size()*4}});
    auto raw=gpu.run(o,hist.size()*4,1);Batch result;result.conv=conv;result.values.resize(hist.size());
    std::memcpy(result.values.data(),raw.data(),raw.size());return result;
}
TestResult TestProductionHistoryGPU() {
    GPU gpu;unsigned legacyCases=0,mode7Cases=0;double maxError=0;float oldSecondAlpha=0,newSecondAlpha=0;
    for(unsigned mode=0;mode<=7;++mode){
        const auto old=runHistory(gpu,DDGI_HISTORY_OLD_SPV,mode);
        const auto now=runHistory(gpu,DDGI_HISTORY_NEW_SPV,mode);
        check(old.values.size()==now.values.size(),"old/new allocation identity");
        if(mode!=7){
            check(std::memcmp(old.values.data(),now.values.data(),now.values.size()*4)==0,"legacy modes bit exact actual GPU output");
            legacyCases+=now.conv.size();continue;
        }
        for(unsigned p=0;p<now.conv.size();++p){
            const float conv=now.conv[p];
            const float alpha=conv<=0?1.f:(.35f*(1-std::min(conv*2,1.f))+.05f*std::min(conv*2,1.f));
            for(unsigned coeff=0;coeff<9;++coeff)for(unsigned channel=0;channel<3;++channel){
                const float current=now.values[coeff*4+channel]; // actual GPU cold production result
                const float history=coeff==0?float(channel+2):.002f;
                const float expected=history*(1-alpha)+current*alpha;
                const float actual=now.values[(p*9+coeff)*4+channel];
                const double error=std::abs(actual-expected);maxError=std::max(maxError,error);
                check(finite(actual)&&error<2e-5,"actual GPU mode7 EMA matches fixed GPU-current/history contract");
            }
            const float current=now.values[0],history=2.f;
            const float inferred=(now.values[p*36]-history)/(current-history);
            if(p==0)check(std::abs(inferred-1.f)<1e-6,"cold first publishes directly");
            if(p==1){newSecondAlpha=inferred;oldSecondAlpha=(old.values[p*36]-history)/(old.values[0]-history);
                check(std::abs(oldSecondAlpha-1.f)<1e-6,"old GPU negative control overwrote the second observation");
                check(std::abs(inferred-alpha)<2e-5&&inferred<.35f,"new second observation EMA not overwrite");
                check(std::abs(oldSecondAlpha-alpha)>.5f,"old production GPU violates new second-update contract");}
            if(conv>=.5f)check(std::abs(inferred-.05f)<2e-5,"settles at configured alpha after young ramp");
            ++mode7Cases;
        }
    }
    std::cout<<"{\"test\":\"irradiance_history_actual_gpu\",\"mode7_states\":"<<mode7Cases
        <<",\"legacy_mode0_to6_states_byteexact\":"<<legacyCases<<",\"old_second_alpha\":"<<oldSecondAlpha
        <<",\"new_second_alpha\":"<<newSecondAlpha<<",\"max_abs_error\":"<<maxError<<"}\n";
    return TestResult::Passed;
}
}
int main(){
    auto suite=std::make_shared<TestSuite>("VulkanDDGIIrradianceHistory");
    suite->AddTestCase(TestCase("ProductionOldNewHistory_GPU",TestProductionHistoryGPU));
    suite->AddTestCase(TestCase("FarSparseThreeSlotHistory_GPU",TestFarSparseHistoryGPU));
    TestRunner::RegisterTestSuite(suite);const auto stats=TestRunner::RunAllSuites();
    return stats.totalTests==2&&stats.passedTests==2&&stats.failedTests==0&&stats.skippedTests==0?0:1;
}
