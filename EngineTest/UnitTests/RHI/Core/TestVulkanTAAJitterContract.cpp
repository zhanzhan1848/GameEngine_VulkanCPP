// Real AddTAAPass execution, uploaded CB and logical history contract.
#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/TAAPass.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>


#include "Graphics/RenderGraph/LumenCompareCapture.h"
#include "Graphics/Utils/HaltonSequence.h"
#include <array>
#include <map>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <chrono>
#include <limits>

using namespace primal::graphics::rhi;
using namespace primal::graphics;
using namespace primal::math;
using namespace Engine::Test;

namespace {

constexpr u32 kW = 32;
constexpr u32 kH = 32;

// ============================= Half-float helpers =============================
u16 float_to_half(float f) {
    u32 bits;
    std::memcpy(&bits, &f, 4);
    u32 sign = (bits >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    u32 mant = bits & 0x7FFFFF;
    if (exp <= 0) {
        if (exp < -10) return static_cast<u16>(sign);
        mant |= 0x800000;
        u32 shift = static_cast<u32>(14 - exp);
        return static_cast<u16>(sign | (mant >> shift));
    } else if (exp == 0xFF - (127 - 15)) {
        return static_cast<u16>(sign | 0x7C00 | (mant >> 13));
    } else if (exp > 31) {
        return static_cast<u16>(sign | 0x7C00);
    }
    return static_cast<u16>(sign | (exp << 10) | (mant >> 13));
}

float half_to_float(u16 h) {
    u32 sign = (h >> 15) & 0x1;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;
        } else {
            // Subnormal half → normalized float
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            bits = (sign << 31) | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

struct DeviceFixture {
    RHIDeviceBase* base{nullptr};
    VulkanDevice* vk{nullptr};
    DeviceDesc desc{};
    bool Init() {
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = true;
        desc.enableDebug = true;
        base = CreateRHIDevice(desc);
        if (!base) return false;
        vk = static_cast<VulkanDevice*>(base);
        return true;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

// Create a 2D texture, upload pixels via staging, transition to ShaderResource.
ResourceHandle CreateAndUploadTexture(DeviceFixture& fx,
                                      const TextureDesc& desc,
                                      const std::vector<u8>& pixels,
                                      const char* name) {
    TextureDesc d = desc;
    d.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource;
    d.memoryUsage = GPUMemoryUsage::Static;
    d.name = name;
    ResourceHandle tex = fx.base->CreateTexture(d);
    if (tex == handles::INVALID_RESOURCE) return tex;

    BufferDesc stagingDesc{};
    stagingDesc.size = pixels.size();
    stagingDesc.type = BufferType::Raw;
    stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    stagingDesc.name = "Staging";
    ResourceHandle staging = fx.base->CreateBuffer(stagingDesc);
    if (staging == handles::INVALID_RESOURCE ||
        !fx.base->UpdateBufferData(staging, pixels.data(), pixels.size(), 0)) {
        fx.base->DestroyTexture(tex);
        if (staging != handles::INVALID_RESOURCE) fx.base->DestroyBuffer(staging);
        return handles::INVALID_RESOURCE;
    }

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { d.size.x, d.size.y, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);
    ResourceBarrier b{};
    b.resource = tex;
    b.beforeState = ResourceState::CopyDest;
    b.afterState = ResourceState::ShaderResource;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&b, 1);
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(staging);
    return tex;
}

} // anonymous namespace
namespace {
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
DeviceFixture& gpu(){static DeviceFixture f;static bool once=f.Init();check(once,"Native Vulkan device");return f;}
std::filesystem::path rootOut(){static auto p=[](){const char* e=std::getenv("PRIMAL_TAA_JITTER_TEST_DIR");auto out=e?std::filesystem::path(e):std::filesystem::temp_directory_path()/("primal-taa-jitter-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(out);std::cout<<"[TAA_FIXTURE_OUTPUT] "<<out<<std::endl;return out;}();return p;}
uint64_t integer(const std::string& s,const char* k){auto at=s.find(std::string("\"")+k+"\":");check(at!=std::string::npos,"Actual execution sidecar field exists");return std::stoull(s.substr(s.find(':',at)+1));}
struct Capture {std::map<std::string,std::vector<u8>> data;std::string meta;};
struct Scenario {
 DeviceFixture& f=gpu();std::vector<ResourceHandle> textures;ResourceHandle color,velocity,current,previous;u32 w=32,h=32;bool yRamp=false;
 Scenario(bool y=false,u32 width=32):w(width),yRamp(y){
  color=texture(DataFormat::RGBA16_Float,w,h,[&](u32 x,u32 y,u8* p){for(u32 c=0;c<4;++c){u16 v=float_to_half(c==3?1.f:.25f+.25f*float(yRamp?y:x));std::memcpy(p+c*2,&v,2);}},8);
  velocity=texture(DataFormat::RG16_Float,w,h,[](u32,u32,u8* p){std::memset(p,0,4);},4);
  current=depth(DataFormat::D32_Float,w,h);previous=depth(DataFormat::R32_Float,w,h);
 }
 ~Scenario(){for(auto t:textures)f.base->DestroyTexture(t);}
 ResourceHandle texture(DataFormat format,u32 width,u32 height,std::function<void(u32,u32,u8*)> fill,u32 bpp){TextureDesc d{};d.size={width,height,1};d.format=format;d.type=TextureType::Texture2D;d.mipLevels=d.arraySize=1;std::vector<u8> bytes(u64(width)*height*bpp);for(u32 y=0;y<height;++y)for(u32 x=0;x<width;++x)fill(x,y,bytes.data()+(u64(y)*width+x)*bpp);auto t=CreateAndUploadTexture(f,d,bytes,"TAAJitterFixture");check(t!=handles::INVALID_RESOURCE,"Upload actual fixture texture");textures.push_back(t);return t;}
 ResourceHandle depth(DataFormat format,u32 width,u32 height){return texture(format,width,height,[](u32,u32,u8* p){float z=.5f;std::memcpy(p,&z,4);},4);}
 Capture frame(const std::string& label,u32 n,int fault=0,bool buildOnly=false){
  auto dir=rootOut()/label;std::filesystem::create_directories(dir);setenv("LUMEN_COMPARE_DIR",dir.c_str(),1);auto ord=std::to_string(n+1);setenv("LUMEN_COMPARE_FRAMES",ord.c_str(),1);setenv("LUMEN_COMPARE_PASSES","TAAPass,Test.Output",1);setenv("LUMEN_COMPARE_STAGING_MIB","64",1);primal::graphics::lumencompare::Begin(n+1);
  RenderGraph graph(*f.base);auto in=graph.ImportResource("TAAFixtureColor",color,ResourceState::ShaderResource),vel=graph.ImportResource("TAAFixtureVelocity",velocity,ResourceState::ShaderResource);auto dep=graph.ImportResource("TAAFixtureDepth",current,ResourceState::ShaderResource);
  bool published=false;u32 calls=0;auto jitter=primal::graphics::utils::GetJitterOffset(n,w,h);
  struct Producer {RGResourceHandle depth;};
  auto producer=graph.AddPass<Producer>("ControlledRasterPublication",RGPassType::Compute,RGPassCategory::Main,[&](Producer& d,RenderGraphBuilder& b){d.depth=b.Write(dep,ResourceState::ShaderResource);b.SideEffect();},[&](const Producer&,RenderGraphContext&){published=true;});
  PostProcess::TAADepthInputs depthInputs{};depthInputs.require_raster_jitter=true;depthInputs.currDepth=current;depthInputs.prevDepth=n==118?handles::INVALID_RESOURCE:previous;depthInputs.previous_depth_frame=fault==3?n-2:n-1;depthInputs.current_depth_rg=producer.depth;
  for(u32 i=0;i<4;++i){depthInputs.currInvViewProj.columns[i]={0,0,0,0};depthInputs.prevViewProj.columns[i]={0,0,0,0};}depthInputs.currInvViewProj.columns[0].x=depthInputs.currInvViewProj.columns[1].y=depthInputs.currInvViewProj.columns[2].z=depthInputs.currInvViewProj.columns[3].w=1;depthInputs.prevViewProj=depthInputs.currInvViewProj;
  if(fault!=1)depthInputs.raster_jitter_provider=[&](primal::math::v2& out){++calls;if(!published)return false;out=jitter;if(fault==2)out.x=std::numeric_limits<float>::quiet_NaN();return true;};
  auto out=PostProcess::AddTAAPass(graph,in,vel,w,h,n,fault==4?nullptr:&depthInputs).output;graph.MarkAsOutput(out);check(calls==0,"Raster provider must not run during graph construction");if(buildOnly)return {};
  auto command=f.base->CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=f.vk->GetCommandBuffer(command);check(cmd&&cmd->Reset()&&cmd->Begin(),"Begin RG command");graph.Compile();graph.Execute(cmd);auto* resource=graph.GetResource(out);check(resource!=nullptr,"Actual TAA output RG");primal::graphics::lumencompare::Output(f.base,cmd,"Test.Output","Output",resource->GetPhysicalHandle(),false,ResourceState::CopySource);check(cmd->End()&&cmd->Submit()&&cmd->WaitForCompletion(),"Submit actual TAA RG");f.base->DestroyCommandBuffer(command);check(fault==1||fault==4?calls==0:calls==1,"Exec-only raster provider call count");
  Capture result;for(auto& item:primal::graphics::lumencompare::items){check(item.staging!=handles::INVALID_RESOURCE,"Every selected actual diagnostic resource staged");auto* p=f.base->MapBuffer(item.staging,0,item.bytes);check(p!=nullptr,"Map actual pre-dispatch observer copy");result.data[item.name]=std::vector<u8>(static_cast<u8*>(p),static_cast<u8*>(p)+item.bytes);f.base->UnmapBuffer(item.staging);}
  check(primal::graphics::lumencompare::Drain(f.base,n,n+1),"Actual producer diagnostic Drain");std::ifstream mf(dir/("frame-"+ord)/"taa-inputs.json");check(bool(mf),"Actual execution identity sidecar");result.meta=std::string(std::istreambuf_iterator<char>(mf),{});return result;
 }
};
float channel(const Capture& c,const char* key,u32 x,u32 y,u32 width){const auto& d=c.data.at(key);u16 h;std::memcpy(&h,d.data()+(u64(y)*width+x)*8,2);float v=half_to_float(h);check(std::isfinite(v),"Every checked output/history sample finite");return v;}
void fresh(const Capture& c){check(c.data.at("Parameters").size()>=160,"Actual binding4 CB >=160B");float invalid;u32 depth;std::memcpy(&invalid,c.data.at("Parameters").data()+16,4);std::memcpy(&depth,c.data.at("Parameters").data()+20,4);check(invalid==1&&depth==0,"Invalid identity must disable actual history and depth CB");check(integer(c.meta,"history_valid")==0&&integer(c.meta,"depth_valid")==0,"Actual sidecar invalid");check(integer(c.meta,"history_color_handle")==integer(c.meta,"current_hdr_handle"),"Invalid history actually binds current HDR");check(c.data.at("Output")==c.data.at("CurrentHDR"),"Fresh output exactly equals actual current HDR");}
void valid(const Capture& c,u32 n,u32 w,u32 h,bool yRamp){
 float params[40];check(c.data.at("Parameters").size()>=sizeof(params),"Read actual uploaded160B");std::memcpy(params,c.data.at("Parameters").data(),sizeof(params));u32 depth;std::memcpy(&depth,c.data.at("Parameters").data()+20,4);check(params[4]==0&&depth==1,"Actual depth+history enabled");auto j=primal::graphics::utils::GetJitterOffset(n,w,h),p=primal::graphics::utils::GetJitterOffset(n-1,w,h);
 for(u32 i=0;i<16;++i){float current=i%5==0?1.f:0.f,prev=current;if(i==12){current=-j.x;prev=p.x;}if(i==13){current=-j.y;prev=p.y;}check(std::abs(params[8+i]-current)<1e-7f&&std::abs(params[24+i]-prev)<1e-7f,"GPU-read CB is actual jittered current inverse and recorded previous VP");}
 check(integer(c.meta,"engine_frame")==n&&integer(c.meta,"history_written_frame")==n-1&&integer(c.meta,"previous_depth_frame")==n-1,"Actual current/color/depth logical frame identities match");check(integer(c.meta,"history_read_slot")==((n+2)%3)&&integer(c.meta,"history_write_slot")==n%3,"Actual distinct history ring slots");check(integer(c.meta,"current_depth_source")==integer(c.meta,"previous_history_depth_source"),"Actual previous color depth source identity");check(integer(c.meta,"history_color_handle")!=integer(c.meta,"current_hdr_handle"),"Real prior output texture bound as history");
 const float dx=(p.x-j.x)*w*.5f,dy=-(p.y-j.y)*h*.5f;float worst=0;for(u32 y=3;y<h-3;++y)for(u32 x=3;x<w-3;++x){float sx=x+dx,sy=y+dy;u32 ix=u32(std::floor(sx)),iy=u32(std::floor(sy));float fx=sx-ix,fy=sy-iy;float prior=(1-fy)*((1-fx)*channel(c,"HistoryColor",ix,iy,w)+fx*channel(c,"HistoryColor",ix+1,iy,w))+fy*((1-fx)*channel(c,"HistoryColor",ix,iy+1,w)+fx*channel(c,"HistoryColor",ix+1,iy+1,w));float current=channel(c,"CurrentHDR",x,y,w);float expected=.9f*prior+.1f*current,got=channel(c,"Output",x,y,w);float bound=std::max(.0001f,std::abs(expected)*.0011f);check(std::abs(got-expected)<bound,"Real shader output follows correct fractional historyUV and existing EMA");worst=std::max(worst,std::abs(got-expected));}
 std::cout<<"{\"frame\":"<<n<<",\"history_offset_pixels\":["<<dx<<','<<dy<<"],\"max_abs_output_error\":"<<worst<<",\"axis\":\""<<(yRamp?'Y':'X')<<"\"}\n";
}
void registerTests(){auto suite=std::make_shared<TestSuite>("VulkanTAAJitterContract");const char* names[]={"ActualDepthCBAndHistoryUV_X","ActualDepthCBAndHistoryUV_Y","MissingRasterProvider","NonfiniteRasterProvider","WrongPreviousDepthFrame","ChangedCurrentDepthSource","PreviousDepthSizeMismatch","ResetInvalidatesHistory","GraphBuildWithoutExecution","ThreeSlotWrapIdentity","ResizeInvalidatesHistory","LegacyVelocityOnlyAccumulation"};for(int mode=0;mode<12;++mode)suite->AddTestCase(TestCase(names[mode],[=]{PostProcess::ResetTAAHistory();Scenario s(mode==1||mode==11);std::string label=names[mode];fresh(s.frame(label,118,mode==11?4:0));if(mode==5)s.current=s.depth(DataFormat::D32_Float,32,32);if(mode==6)s.previous=s.depth(DataFormat::R32_Float,16,32);if(mode==7)PostProcess::ResetTAAHistory();if(mode==11){
 s.color=s.texture(DataFormat::RGBA16_Float,32,32,[](u32,u32 y,u8* p){for(u32 k=0;k<4;++k){u16 v=float_to_half(k==3?1.f:.375f+.25f*y);std::memcpy(p+2*k,&v,2);}},8);
 s.velocity=s.texture(DataFormat::RG16_Float,32,32,[](u32,u32,u8* p){u16 v[2]={float_to_half(0),float_to_half(2.f/32)};std::memcpy(p,v,4);},4);
 auto c=s.frame(label,119,4);float invalid;u32 depth;std::memcpy(&invalid,c.data.at("Parameters").data()+16,4);std::memcpy(&depth,c.data.at("Parameters").data()+20,4);check(invalid==0&&depth==0,"Legacy velocity-only retains actual history accumulation");check(integer(c.meta,"history_valid")==1&&integer(c.meta,"history_written_frame")==118,"Legacy actual history identity");for(u32 y=3;y<29;++y)for(u32 x=3;x<29;++x){float expected=.9f*channel(c,"HistoryColor",x,y+1,32)+.1f*channel(c,"CurrentHDR",x,y,32);check(std::abs(channel(c,"Output",x,y,32)-expected)<std::abs(expected)*.0011f,"Legacy actual NDC velocity reprojects and blends prior output");}
 }else if(mode==8){s.frame(label,119,0,true);fresh(s.frame(label,120));}else if(mode==10){Scenario resized(false,17);fresh(resized.frame(label,119));}else{auto c=s.frame(label,119,mode==2?1:mode==3?2:mode==4?3:0);if(mode>=2&&mode<=7)fresh(c);else valid(c,119,s.w,s.h,s.yRamp);if(mode==9){valid(s.frame(label,120),120,s.w,s.h,s.yRamp);valid(s.frame(label,121),121,s.w,s.h,s.yRamp);}}return TestResult::Passed;}));TestRunner::RegisterTestSuite(suite);}
}
int main(){registerTests();const auto s=TestRunner::RunAllSuites();auto& f=gpu();if(f.base){f.base->Shutdown();f.base=nullptr;f.vk=nullptr;}return s.totalTests==12&&s.passedTests==12&&s.failedTests==0&&s.skippedTests==0?0:1;}
