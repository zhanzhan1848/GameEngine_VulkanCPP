// Isolated native contracts for the opt-in Vulkan albedo array mip path.
#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#include "Graphics/Nanite/MaterialDataBuilder.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct GPU {
    RHIDeviceBase* d=nullptr;VulkanDevice* vk=nullptr;
    std::vector<ResourceHandle> textures,buffers;
    GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;d=CreateRHIDevice(desc);require(d,"Create Vulkan device");vk=static_cast<VulkanDevice*>(d);}
    ~GPU(){if(!d)return;for(auto t:textures)d->DestroyTexture(t);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
    ResourceHandle buffer(u64 size,GPUMemoryUsage memory,const void* bytes=nullptr){BufferDesc b{};b.size=size;b.type=BufferType::Raw;b.memoryUsage=b.usage=memory;b.bindFlags=u32(memory==GPUMemoryUsage::Readback?BufferUsageFlags::TransferDst:BufferUsageFlags::TransferSrc);auto h=d->CreateBuffer(b);require(h!=handles::INVALID_RESOURCE,"Create buffer");buffers.push_back(h);if(bytes)require(d->UpdateBufferData(h,bytes,size,0),"Upload source bytes");return h;}
    VulkanCommandBuffer* begin(CommandBufferHandle& h){h=d->CreateCommandBuffer(CommandQueueType::Graphics);auto* c=vk->GetCommandBuffer(h);require(c&&c->Begin(),"Begin graphics command buffer");return c;}
    void finish(CommandBufferHandle h,VulkanCommandBuffer* c){require(c->End()&&c->Submit(0)&&c->WaitForCompletion(),"Submit and complete GPU command");d->DestroyCommandBuffer(h);}
    ResourceHandle texture(u32 w,u32 h,u32 layers,u32 mips,DataFormat f){TextureDesc t{};t.type=layers>1?TextureType::Texture2DArray:TextureType::Texture2D;t.size={w,h,1};t.arraySize=layers;t.mipLevels=mips;t.format=f;t.usage=TextureUsage::ShaderResource|TextureUsage::CopySource|TextureUsage::CopyDest;auto r=d->CreateTexture(t);require(r!=handles::INVALID_RESOURCE,"Create texture");textures.push_back(r);return r;}
    ResourceHandle source(u32 w,u32 h,DataFormat f,const std::vector<u8>& bytes,bool shaderReady){auto t=texture(w,h,1,1,f);auto stage=buffer(bytes.size(),GPUMemoryUsage::Dynamic,bytes.data());CommandBufferHandle handle;auto* c=begin(handle);BufferTextureCopyRegion r{};r.imageSubresource.layerCount=1;r.imageExtent={w,h,1};c->CopyBufferToTexture(stage,t,&r,1);if(shaderReady){ResourceBarrier b{};b.resource=t;b.beforeState=ResourceState::CopyDest;b.afterState=ResourceState::ShaderResource;b.subresource=RHI_ALL_SUBRESOURCES;b.queueFamily=0xffffffffu;c->InsertBarrier(&b,1);}finish(handle,c);return t;}
    std::vector<std::vector<std::vector<u8>>> read(ResourceHandle t,bool skipPlaceholder=false){
        const auto& desc=vk->GetTexture(t)->GetTextureDesc();std::vector<BufferTextureCopyRegion> regions;u64 bytes=0;
        for(u32 m=0;m<desc.mipLevels;++m)for(u32 l=skipPlaceholder?1u:0u;l<desc.arraySize;++l){BufferTextureCopyRegion r{};r.bufferOffset=bytes;r.imageSubresource={m,l,1};r.imageExtent={std::max(1u,desc.size.x>>m),std::max(1u,desc.size.y>>m),1};bytes+=u64(r.imageExtent.width)*r.imageExtent.height*4;regions.push_back(r);}
        auto rb=buffer(bytes,GPUMemoryUsage::Readback);CommandBufferHandle handle;auto* c=begin(handle);c->CopyTextureToBuffer(t,rb,regions.data(),u32(regions.size()));c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);finish(handle,c);
        auto* p=static_cast<const u8*>(d->MapBuffer(rb,0,bytes));require(p,"Map complete mip array readback");std::vector<std::vector<std::vector<u8>>> result(desc.mipLevels,std::vector<std::vector<u8>>(desc.arraySize));
        u32 i=0;for(u32 m=0;m<desc.mipLevels;++m)for(u32 l=skipPlaceholder?1u:0u;l<desc.arraySize;++l){auto& r=regions[i++];size_t n=size_t(r.imageExtent.width)*r.imageExtent.height*4;result[m][l].assign(p+r.bufferOffset,p+r.bufferOffset+n);}
        d->UnmapBuffer(rb);return result;
    }
};
u32 mipCount(u32 w,u32 h){u32 n=1;for(u32 x=std::max(w,h);x>1;x>>=1)++n;return n;}
std::vector<u8> pattern(u32 w,u32 h,u32 layer){std::vector<u8> p(w*h*4);for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x){auto* q=&p[(y*w+x)*4];if(layer==0){q[0]=q[1]=q[2]=((x+y)&1)?255:0;}else{q[0]=u8((x+y*w+layer*31)%256);q[1]=u8(255-q[0]);q[2]=u8((x*37+y*19+layer*11)%256);}q[3]=layer==0?u8(((x+y)&1)?255:0):255;}return p;}
double decode(double v,bool srgb){return srgb?(v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4)):v;}
double encode(double v,bool srgb){return srgb?(v<=.0031308?v*12.92:1.055*std::pow(v,1./2.4)-.055):v;}
std::vector<u8> reference(const std::vector<u8>& src,u32 sw,u32 sh,u32 dw,u32 dh,bool srgb){
    std::vector<u8> dst(dw*dh*4);
    for(u32 y=0;y<dh;++y)for(u32 x=0;x<dw;++x){double sx=(x+.5)*sw/dw-.5,sy=(y+.5)*sh/dh-.5;int ix=int(std::floor(sx)),iy=int(std::floor(sy));double tx=sx-ix,ty=sy-iy;
        for(u32 channel=0;channel<4;++channel){auto sample=[&](int a,int b){a=std::clamp(a,0,int(sw)-1);b=std::clamp(b,0,int(sh)-1);return decode(src[(b*sw+a)*4+channel]/255.,srgb&&channel<3);};double a=sample(ix,iy)*(1-tx)+sample(ix+1,iy)*tx,b=sample(ix,iy+1)*(1-tx)+sample(ix+1,iy+1)*tx;dst[(y*dw+x)*4+channel]=u8(std::clamp<long>(std::lround(encode(a*(1-ty)+b*ty,srgb&&channel<3)*255),0,255));}
    }
    return dst;
}
void chainCase(u32 w,u32 h,u32 layers,DataFormat format,bool ready,bool mixed=false){
    GPU g;std::vector<ResourceHandle> sources;std::vector<std::vector<u8>> originals;
    for(u32 l=0;l<layers;++l){originals.push_back(pattern(w,h,l));sources.push_back(g.source(w,h,format,originals.back(),ready));}
    auto array=g.texture(w,h,layers,mipCount(w,h),format);CommandBufferHandle handle;auto* c=g.begin(handle);
    for(u32 l=0;l<layers;++l)require(vulkan::CopyAlbedoMip0Layer(*g.vk,*c,sources[l],array,l,false),"Copy exact layer mip0");
    if(mixed&&mipCount(w,h)>1){VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=g.vk->GetTexture(array)->GetNativeImage();b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,1,1,0,layers};vkCmdPipelineBarrier(c->GetNativeCmdBuffer(),VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);g.vk->GetTexture(array)->SetMipLayout(1,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
    require(vulkan::GenerateAlbedoMipChain(*g.vk,*c,array),"Generate checked complete chain");g.finish(handle,c);
    for(u32 m=0;m<mipCount(w,h);++m)require(g.vk->GetTexture(array)->GetMipLayout(m)==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"Every mip tracked sampled for all layers");
    auto actual=g.read(array);u32 maxError=0;
    for(u32 l=0;l<layers;++l){require(actual[0][l]==originals[l],"mip0 byte-exact across every layer");for(u32 m=1;m<actual.size();++m){auto ref=reference(actual[m-1][l],std::max(1u,w>>(m-1)),std::max(1u,h>>(m-1)),std::max(1u,w>>m),std::max(1u,h>>m),format==DataFormat::RGBA8_sRGB);for(size_t i=0;i<ref.size();++i)maxError=std::max(maxError,u32(std::abs(int(actual[m][l][i])-int(ref[i]))));}}
    require(maxError<=2,"All lower mips match linear-domain Vulkan blit reference within 2 bytes");
    if(w==4&&h==4)require(std::abs(int(actual[1][0][0])-(format==DataFormat::RGBA8_sRGB?188:128))<=1,"Checker filter uses correct source color encoding");
    if(w==4&&h==4)require(std::abs(int(actual[1][0][3])-128)<=1,"sRGB alpha stays linear, not gamma filtered");
    // Regeneration begins from all-SRO layouts and must preserve every byte.
    c=g.begin(handle);require(vulkan::GenerateAlbedoMipChain(*g.vk,*c,array),"Regenerate from SRO");g.finish(handle,c);require(g.read(array)==actual,"Repeated generation and mip0 stay stable");
    std::cout<<"[ALBEDO_MIP] "<<w<<'x'<<h<<" layers="<<layers<<" levels="<<actual.size()<<" format="<<u32(format)<<" max_error="<<maxError<<'\n';
}
void builderCase(bool enabled,bool generic=false){
    GPU g;std::vector<u8> white(4,255),real=pattern(16,16,1);auto placeholder=g.source(1,1,DataFormat::RGBA8_sRGB,white,true),source=g.source(16,16,DataFormat::RGBA8_sRGB,real,true);
    setenv("PRIMAL_VK_ALBEDO_MIPS",enabled?"1":"0",1);ResourceHandle array=handles::INVALID_RESOURCE;
    require(nanite::MaterialDataBuilder::CreateTextureArray(g.d,nullptr,{placeholder,source},"AlbedoMipGpuTest",array,generic?nanite::TextureArrayChannel::Generic:nanite::TextureArrayChannel::Albedo),"Builder array success");g.textures.push_back(array);
    require(g.vk->GetTexture(array)->GetTextureDesc().mipLevels==(enabled&&!generic?5u:1u),"Only explicit Vulkan albedo policy enables mips");auto pixels=g.read(array,!(enabled&&!generic));require(pixels[0][1]==real,"Builder real mip0 byte-exact");
    if(enabled&&!generic)for(const auto& level:pixels)require(std::all_of(level[0].begin(),level[0].end(),[](u8 v){return v==255;}),"Placeholder initialized white at every mip");
    unsetenv("PRIMAL_VK_ALBEDO_MIPS");
}
void rejectionCase(u32 which){
    GPU g;CommandBufferHandle handle;auto* c=g.begin(handle);
    if(which==0){auto t=g.texture(4,4,2,3,DataFormat::RGBA8_UNorm);require(!vulkan::GenerateAlbedoMipChain(*g.vk,*c,t),"Undefined mip0 rejected before commands");require(g.vk->GetTexture(t)->GetMipLayout(0)==VK_IMAGE_LAYOUT_UNDEFINED,"Rejected layout unchanged");}
    if(which==1){auto t=g.texture(4,4,2,3,DataFormat::R32_UInt);require(!vulkan::GenerateAlbedoMipChain(*g.vk,*c,t),"Unsupported integer format rejected");}
    if(which==2){auto src=g.source(4,4,DataFormat::RGBA8_UNorm,pattern(4,4,0),true),dst=g.texture(8,8,2,4,DataFormat::RGBA8_UNorm);require(!vulkan::CopyAlbedoMip0Layer(*g.vk,*c,src,dst,0,false),"Real-layer resizing rejected");}
    g.finish(handle,c);
    if(which==3){handle=g.d->CreateCommandBuffer(CommandQueueType::Graphics);auto* raw=g.vk->GetCommandBuffer(handle);TextureDesc desc{};desc.type=TextureType::Texture2DArray;desc.size={4,4,1};desc.arraySize=2;desc.mipLevels=3;desc.format=DataFormat::RGBA8_UNorm;desc.usage=TextureUsage::ShaderResource|TextureUsage::CopySource|TextureUsage::CopyDest;require(!vulkan::CanGenerateAlbedoMipChain(*g.vk,*raw,desc),"Non-recording command rejected");g.d->DestroyCommandBuffer(handle);}
}
void registerCases(const std::shared_ptr<TestSuite>& s){
    auto add=[&](const char* name,auto test){s->AddTestCase(TestCase(name,[=]{test();return TestResult::Passed;}));};
    add("linear_4x4_two_layers_transfer_dst_sources",[]{chainCase(4,4,2,DataFormat::RGBA8_UNorm,false);});
    add("srgb_4x4_three_layers_shader_read_sources",[]{chainCase(4,4,3,DataFormat::RGBA8_sRGB,true);});
    add("linear_npot_5x3_three_layers_mixed_layouts",[]{chainCase(5,3,3,DataFormat::RGBA8_UNorm,true,true);});
    add("srgb_one_by_eight_two_layers",[]{chainCase(1,8,2,DataFormat::RGBA8_sRGB,true);});
    add("linear_one_by_one_two_layers",[]{chainCase(1,1,2,DataFormat::RGBA8_UNorm,true);});
    add("bgra_4x4_two_layers",[]{chainCase(4,4,2,DataFormat::BGRA8_UNorm,true);});
    add("builder_albedo_optin_placeholder_and_mip0",[]{builderCase(true);});
    add("builder_albedo_flag0_legacy_single_mip",[]{builderCase(false);});
    add("builder_generic_flag1_still_single_mip",[]{builderCase(true,true);});
    add("reject_undefined_base",[]{rejectionCase(0);});add("reject_integer_format",[]{rejectionCase(1);});
    add("reject_real_layer_resize",[]{rejectionCase(2);});add("reject_nonrecording_command",[]{rejectionCase(3);});
}
}
int main(){auto suite=std::make_shared<TestSuite>("VulkanAlbedoMipmaps");registerCases(suite);TestRunner::RegisterTestSuite(suite);auto r=TestRunner::RunAllSuites();return r.totalTests==13&&r.passedTests==13&&r.failedTests==0&&r.skippedTests==0?0:1;}
