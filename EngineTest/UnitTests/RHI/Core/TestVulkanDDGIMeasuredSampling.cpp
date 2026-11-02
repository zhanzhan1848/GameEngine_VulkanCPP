// GPU contracts for the actual Vulkan mode-7 sampler and depth updater.
#include "../../TestFramework.h"
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

std::vector<Sample> samplerResults;
TestResult TestSamplerGPU() {
    GPU gpu;
    const size_t bytes=Rays*Frames*sizeof(Sample);
    auto output=gpu.buffer(bytes,BufferType::Raw,GPUMemoryUsage::Static);
    gpu.pipeline(DDGI_MEASURED_SAMPLER_SPV,"main",{DescriptorType::StorageBuffer},{{output,0,bytes}});
    auto raw=gpu.run(output,bytes,1,Frames);
    std::vector<Sample> samples(Rays*Frames);std::memcpy(samples.data(),raw.data(),bytes);
    std::array<std::array<double,9>,4> integral{};
    double maxNormError=0,maxPdfError=0,maxJacobianError=0;
    unsigned changed=0,jacobianChecks=0;
    for(unsigned frame=0;frame<Frames;++frame) {
        std::array<unsigned,Cells> counts{};
        for(unsigned ray=0;ray<Rays;++ray) {
            const auto& s=samples[frame*Rays+ray];
            for(float c:s.d) check(finite(c),"Sampler finite direction/PDF");
            const V d{s.d[0],s.d[1],s.d[2]};const double norm=length(d);
            maxNormError=std::max(maxNormError,std::abs(norm-1));check(std::abs(norm-1)<2e-6,"Unit direction");
            check(s.d[3]>0,"Positive PDF");
            const double expectedPdf=pdf(d);
            maxPdfError=std::max(maxPdfError,std::abs(s.d[3]-expectedPdf));
            check(std::abs(s.d[3]-expectedPdf)<3e-7,"Analytic spherical PDF from L1 direction");
            const auto uv=encode(d);
            const unsigned x=std::min(7u,unsigned(uv[0]*8)),y=std::min(7u,unsigned(uv[1]*8));
            const unsigned bin=y*8+x;++counts[bin];
            check(bin==ray/2 && s.identity[0]==bin,"Independent oct bin and stratum identity");
            check(s.identity[1]==ray&&s.identity[2]==frame&&s.identity[3]==7,"GPU sample identity");
            if(frame && std::memcmp(s.d,samples[(frame-1)*Rays+ray].d,sizeof(s.d))!=0) ++changed;
            // Independent numerical solid-angle derivative. Avoid oct seams
            // where a symmetric finite difference crosses a piecewise fold.
            const double a=2*uv[0]-1,b=2*uv[1]-1,eps=1e-6;
            if(std::abs(a)>1e-4&&std::abs(b)>1e-4&&std::abs(1-std::abs(a)-std::abs(b))>1e-4 &&
               uv[0]>eps&&uv[0]<1-eps&&uv[1]>eps&&uv[1]<1-eps) {
                const V du=sub(decode(uv[0]+eps,uv[1]),decode(uv[0]-eps,uv[1]));
                const V dv=sub(decode(uv[0],uv[1]+eps),decode(uv[0],uv[1]-eps));
                const double jac=length(cross(du,dv))/(4*eps*eps);
                const double error=std::abs(jac*s.d[3]-1);maxJacobianError=std::max(maxJacobianError,error);
                check(error<2e-5,"Numerical oct Jacobian times PDF equals one");++jacobianChecks;
            }
            const auto basis=sh(d);const std::array<double,4> fields{1,d.x,d.y,d.z};
            for(unsigned f=0;f<4;++f) for(unsigned k=0;k<9;++k)
                integral[f][k]+=fields[f]*basis[k]/(Rays*Frames*double(s.d[3]));
        }
        for(auto n:counts) check(n==2,"Exactly two samples in each of 64 bins per frame");
    }
    check(changed==Rays*(Frames-1),"Every ray changes direction between consecutive frames");
    check(jacobianChecks>8000,"Numerical Jacobian covers nearly all rays");
    double maxSHError=0;
    for(unsigned field=0;field<4;++field) for(unsigned k=0;k<9;++k) {
        double expected=0;
        if(field==0&&k==0) expected=std::sqrt(4*Pi);
        if(field>0&&k==field) expected=std::sqrt(4*Pi/3);
        const double error=std::abs(integral[field][k]-expected);maxSHError=std::max(maxSHError,error);
        if(error>=.04) std::cerr<<"SH mismatch field="<<field<<" coefficient="<<k<<" got="<<integral[field][k]<<" expected="<<expected<<'\n';
        check(error<.04,"64-frame SH9 constant/axis analytic integral abs error < .04");
    }
    samplerResults=std::move(samples);
    std::cout<<"{\"test\":\"mode7_sampler_gpu\",\"rays\":8192,\"frames\":64,\"bins_each\":64,\"samples_per_bin\":2,\"max_norm_error\":"<<maxNormError
        <<",\"max_pdf_error\":"<<maxPdfError<<",\"jacobian_checks\":"<<jacobianChecks<<",\"max_jacobian_relative_error\":"<<maxJacobianError<<",\"max_sh_error\":"<<maxSHError<<"}\n";
    return TestResult::Passed;
}

TestResult TestDepthGPU() {
    check(samplerResults.size()==Rays*Frames,"Sampler GPU must pass before depth reference");
    constexpr unsigned Probes=11;constexpr float MaxDistance=32;
    std::array<float,Probes*Rays*4> rays{};
    std::array<float,Probes*128> history{};
    for(unsigned p=0;p<Probes;++p) for(unsigned r=0;r<Rays;++r) {
        float value=-1;
        switch(p) {
            case 0:value=-2;break; // true escape is a measured max-distance observation
            case 1:value=-1;break; // unknown MUST invalidate previously open history
            case 2:value=(r&1)?std::numeric_limits<float>::infinity():std::numeric_limits<float>::quiet_NaN();break;
            case 3:value=3;break; // valid new observation must not blend unknown -1
            case 4:value=(r&1)?6:-1;break; // one unknown + one measured per cell
            case 5:value=(r&1)?-4:-std::numeric_limits<float>::infinity();break;
            case 6:value=(r&1)?6:2;break; // PDF/Jacobian weighted moments + valid EMA
            case 7:value=(r&1)?4:-2;break; // escape+hit, nonfinite history must not blend
            case 8:value=-3;break; // finite-range clear is measured, not unknown or sky
            case 9:value=-2;break; // valid history: true escape EMA control
            case 10:value=-3;break; // same history: range clear must use the same EMA
        }
        rays[(p*Rays+r)*4+3]=value;
    }
    for(unsigned p=0;p<Probes;++p) for(unsigned c=0;c<Cells;++c) {
        history[p*128+c]=(p==1||p==2||p==5)?MaxDistance:-1;
        history[p*128+64+c]=9;
        if(p==6||p==9||p==10) history[p*128+c]=10;
        if(p==7) {history[p*128+c]=std::numeric_limits<float>::quiet_NaN();history[p*128+64+c]=std::numeric_limits<float>::infinity();}
    }
    std::array<u32,84> volume{};std::array<u32,128> globals{};
    auto setFloat=[&](unsigned offset,float x){std::memcpy(reinterpret_cast<u8*>(volume.data())+offset,&x,4);};
    volume[32/4]=Probes;volume[36/4]=1;volume[40/4]=1;volume[48/4]=Rays;volume[52/4]=Probes;
    setFloat(16,1);setFloat(60,.2f);volume[68/4]=Frames-1;setFloat(72,MaxDistance);volume[84/4]=Probes;volume[108/4]=7;
    std::array<u32,Probes> updates{};for(unsigned p=0;p<Probes;++p) updates[p]=p;
    std::array<float,1> unused{};
    GPU gpu;
    auto global=gpu.buffer(sizeof(globals),BufferType::Constant,GPUMemoryUsage::Dynamic,globals.data());
    auto vol=gpu.buffer(sizeof(volume),BufferType::Constant,GPUMemoryUsage::Dynamic,volume.data());
    auto ray=gpu.buffer(sizeof(rays),BufferType::Raw,GPUMemoryUsage::Dynamic,rays.data());
    auto hist=gpu.buffer(sizeof(history),BufferType::Raw,GPUMemoryUsage::Dynamic,history.data());
    auto out=gpu.buffer(sizeof(history),BufferType::Raw,GPUMemoryUsage::Static);
    auto list=gpu.buffer(sizeof(updates),BufferType::Raw,GPUMemoryUsage::Dynamic,updates.data());
    auto extra=gpu.buffer(sizeof(unused),BufferType::Raw,GPUMemoryUsage::Dynamic,unused.data());
    gpu.pipeline(DDGI_MEASURED_DEPTH_SPV,"ddgi_update_depth",
        {DescriptorType::UniformBuffer,DescriptorType::UniformBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,
         DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer},
        {{global,0,sizeof(globals)},{vol,0,sizeof(volume)},{ray,0,sizeof(rays)},{hist,0,sizeof(history)},
         {out,0,sizeof(history)},{list,0,sizeof(updates)},{extra,0,sizeof(unused)}});
    auto bytes=gpu.run(out,sizeof(history),1);
    std::array<float,Probes*128> actual{};std::memcpy(actual.data(),bytes.data(),bytes.size());
    double maxError=0;
    for(unsigned p=0;p<Probes;++p) for(unsigned cell=0;cell<Cells;++cell) {
        double sw=0,sd=0,sd2=0;
        for(unsigned j=0;j<2;++j) {
            const unsigned r=cell*2+j;const float v=rays[(p*Rays+r)*4+3];
            if(!finite(v)||(v<0&&v!=-2&&v!=-3)) continue;
            const double distance=(v==-2||v==-3)?MaxDistance:v;
            const auto& s=samplerResults[(Frames-1)*Rays+r];
            const double w=1/pdf({s.d[0],s.d[1],s.d[2]});
            sw+=w;sd+=w*distance;sd2+=w*distance*distance;
        }
        double mean=-1,var=0;
        if(sw>0) {
            mean=sd/sw;var=std::max(0.0,sd2/sw-mean*mean);
            const float oldMean=history[p*128+cell],oldVar=history[p*128+64+cell];
            if(finite(oldMean)&&finite(oldVar)&&oldMean>=0&&oldVar>=0) {mean=oldMean*.8+mean*.2;var=oldVar*.8+var*.2;}
        }
        const float m=actual[p*128+cell],v=actual[p*128+64+cell];
        check(finite(m)&&finite(v),"Depth moments always finite");
        const double error=std::max(std::abs(m-mean),std::abs(v-var));maxError=std::max(maxError,error);
        if(error>=.002) std::cerr<<"Depth mismatch case="<<p<<" cell="<<cell<<" got="<<m<<","<<v<<" expected="<<mean<<","<<var<<'\n';
        check(error<.002,"Actual production mode7 weighted depth/status/history contract");
        if(sw==0) check(m==-1&&v==0,"Unknown exact sentinel/zero variance replaces stale open history");
        if(p==8) {
            check(m==actual[cell]&&v==actual[64+cell],"Range clear and true escape have identical depth measurements");
            check(m==MaxDistance&&std::abs(v)<.002,"Range clear is measured maxdistance, not unknown");
        }
        if(p==10) {
            check(m==actual[9*128+cell]&&v==actual[9*128+64+cell],
                  "Range clear and true escape apply identical valid-history EMA");
        }
    }
    std::cout<<"{\"test\":\"mode7_depth_gpu\",\"synthetic_probes\":11,\"cells_checked\":704,\"max_error\":"<<maxError<<"}\n";
    return TestResult::Passed;
}
}
int main() {
    auto suite=std::make_shared<TestSuite>("VulkanDDGIMeasuredSampling");
    suite->AddTestCase(TestCase("Mode7_Sampler_GPU",TestSamplerGPU));
    suite->AddTestCase(TestCase("Mode7_ProductionDepth_GPU",TestDepthGPU));
    TestRunner::RegisterTestSuite(suite);const auto stats=TestRunner::RunAllSuites();
    return stats.totalTests==2&&stats.passedTests==2&&stats.failedTests==0&&stats.skippedTests==0?0:1;
}
