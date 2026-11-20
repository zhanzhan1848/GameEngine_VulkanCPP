#pragma once
#include "DawnSponzaGeometry.h"
#include "DawnSponzaTAA.h"
#include "DawnSponzaGBufferFormats.h"
namespace primal::tests::dawn_sponza {
// Borrows one complete immutable geometry source and the current raster GBuffer.
// Owns paired trace/resolve slots; submit in order on one queue. Save after wait.
class EnvironmentVisibility {
public:
 struct alignas(16) Params{primal::math::m4x4 inverseVP;float camera[4]{};u32 dimensions[4]{},policy[4]{};float parameters[4]{.002f,.02f,.95f,.05f};u32 target[4]{0,0,1,0};primal::math::m4x4 previousVP;};static_assert(sizeof(Params)==208);
 struct Inputs{MeasuredGeometry::SceneView scene;ResourceHandle depth=handles::INVALID_RESOURCE,normal=handles::INVALID_RESOURCE,orm=handles::INVALID_RESOURCE;u32 width=0,height=0,targetWidth=0,targetHeight=0;};
 EnvironmentVisibility()=default;EnvironmentVisibility(const EnvironmentVisibility&)=delete;EnvironmentVisibility& operator=(const EnvironmentVisibility&)=delete;~EnvironmentVisibility(){Shutdown();}
 static std::string Source(){std::ifstream f("Engine/Graphics/Dawn/shaders/Lumen/EnvironmentVisibility.wgsl");return {(std::istreambuf_iterator<char>(f)),{}};}
 bool Initialize(DawnDevice& device,const Inputs& in){
  if(device_||!in.scene.digest||!in.width||!in.height||!in.targetWidth||!in.targetHeight||in.targetWidth>in.width||in.targetHeight>in.height)return false;
  for(u32 i=0;i<4;++i)if(!in.scene.counts[i]||in.scene.buffers[i]==handles::INVALID_RESOURCE)return false;
  for(auto h:{in.depth,in.normal,in.orm}){auto* t=device.GetTexture(h);if(!t||t->GetTextureDesc().size.x!=in.width||t->GetTextureDesc().size.y!=in.height||(h==in.depth?t->GetTextureDesc().format!=DataFormat::D32_Float:!IsGBufferAttributeFormat(t->GetTextureDesc().format)))return false;}
  device_=&device;in_=in;u32 bindings[]{0,1,2,3,4,5,13,14,15,16};DescriptorSetLayoutBinding b[10]{};
  for(u32 i=0;i<10;++i){b[i].binding=bindings[i];b[i].descriptorCount=1;b[i].stageFlags=ShaderStage::Compute;b[i].descriptorType=i==0?DescriptorType::SampledDepthImage:i==3?DescriptorType::StorageImage:i==4?DescriptorType::UniformBuffer:i>=6?DescriptorType::StorageBuffer:DescriptorType::SampledImage;b[i].readonly=i>=6;}
  layout_=device.CreateDescriptorSetLayout({10,b});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;PipelineLayoutDesc pl{};pl.setLayouts=&layout_;pl.setLayoutCount=1;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
  auto source=Source();for(u32 stage=0;stage<2;++stage){shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,stage?"environment_visibility_resolve":"environment_visibility_trace");if(shaders_[stage]==handles::INVALID_SHADER)return false;ComputePipelineDesc cp{};cp.layout=pipelineLayout_;cp.computeShader=shaders_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;}
  for(auto& s:slots_){BufferDesc cb{};cb.type=BufferType::Constant;cb.size=sizeof(Params);cb.memoryUsage=GPUMemoryUsage::Static;s.params=device.CreateBuffer(cb);if(s.params==handles::INVALID_RESOURCE)return false;
   for(u32 stage=0;stage<2;++stage){TextureDesc t{};t.type=TextureType::Texture2D;t.format=DataFormat::RGBA16_Float;t.size={stage?in.targetWidth:(in.targetWidth+1)/2,stage?in.targetHeight:(in.targetHeight+1)/2,1};t.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;s.output[stage]=device.CreateTexture(t);s.sets[stage]=device.CreateDescriptorSet({layout_});if(s.output[stage]==handles::INVALID_RESOURCE||s.sets[stage]==handles::INVALID_DESCRIPTOR_SET)return false;}}
  ready_=true;return true;
 }
 bool Record(DawnCommandBuffer& cmd,u32 frame,Params p,const TemporalAA::Raster& raster,const MeasuredGeometry::SceneView& scene){
  if(!ready_||frame!=nextFrame_||!raster.uploaded||raster.frame!=frame||raster.depth!=in_.depth||raster.width!=in_.width||raster.height!=in_.height||!scene.digest||scene.digest!=in_.scene.digest||scene.buffers!=in_.scene.buffers||scene.counts!=in_.scene.counts||!std::isfinite(raster.jitter.x)||!std::isfinite(raster.jitter.y))return false;
  for(u32 c=0;c<4;++c)for(u32 r=0;r<4;++r)if(!std::isfinite(primal::math::elem(raster.baseVP,c,r)))return false;
  p.inverseVP=graphics::rhi::math::Inverse(raster.baseVP);p.previousVP=frame?previousVP_:raster.baseVP;
  for(u32 c=0;c<4;++c)for(u32 r=0;r<4;++r)if(!std::isfinite(primal::math::elem(p.inverseVP,c,r)))return false;
  for(float v:p.camera)if(!std::isfinite(v))return false;for(float v:p.parameters)if(!std::isfinite(v))return false;
  p.camera[3]=raster.jitter.x;p.policy[2]=std::bit_cast<u32>(raster.jitter.y);p.policy[3]=frame;
  p.dimensions[0]=in_.width;p.dimensions[1]=in_.height;p.dimensions[2]=(in_.targetWidth+1)/2;p.dimensions[3]=(in_.targetHeight+1)/2;p.target[0]=in_.targetWidth;p.target[1]=in_.targetHeight;
  auto& s=slots_[frame%3];const auto& prev=slots_[(frame+2)%3];if(frame&&prev.frame!=frame-1)return false;if(!device_->UpdateBufferData(s.params,&p,sizeof(p)))return false;
  for(u32 stage=0;stage<2;++stage){ResourceHandle textures[]{in_.depth,in_.normal,in_.orm,s.output[stage],handles::INVALID_RESOURCE,stage?s.output[0]:prev.output[0]};DescriptorImageInfo images[6]{};DescriptorBufferInfo buffers[5]{};WriteDescriptorSet writes[10]{};u32 binding[]{0,1,2,3,4,5,13,14,15,16};
   for(u32 i=0;i<10;++i){auto& w=writes[i];w.dstSet=s.sets[stage];w.dstBinding=binding[i];w.descriptorCount=1;w.descriptorType=i==0?DescriptorType::SampledDepthImage:i==3?DescriptorType::StorageImage:i==4?DescriptorType::UniformBuffer:i>=6?DescriptorType::StorageBuffer:DescriptorType::SampledImage;
    if(i==4||i>=6){u32 k=i==4?0:i-5;buffers[k]={i==4?s.params:scene.buffers[i-6],0,i==4?sizeof(Params):u64(scene.counts[i-6])*(i==9?64:48)};w.bufferInfo=&buffers[k];}else{images[i]={handles::INVALID_SAMPLER,textures[i],i==3?ResourceState::UnorderedAccess:ResourceState::ShaderResource};w.imageInfo=&images[i];}}
   device_->UpdateDescriptorSets(10,writes);cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&s.sets[stage],0,nullptr);cmd.Dispatch(((stage?in_.targetWidth:p.dimensions[2])+7)/8,((stage?in_.targetHeight:p.dimensions[3])+7)/8,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);
  }s.frame=frame;s.cpu=p;s.history=prev.output[0];previousVP_=raster.baseVP;++nextFrame_;return true;
 }
 ResourceHandle Output(u32 frame,u32 stage=1)const{return ready_&&nextFrame_&&frame==nextFrame_-1&&stage<2?slots_[frame%3].output[stage]:handles::INVALID_RESOURCE;}
 bool Capture(DawnCommandBuffer& cmd,u32 frame){
  if(Output(frame)==handles::INVALID_RESOURCE||readback_!=handles::INVALID_RESOURCE)return false;const auto& s=slots_[frame%3];captured_=frame;capturedParams_=s.cpu;
  ResourceHandle images[]{in_.depth,in_.normal,in_.orm,s.history,s.output[0],s.output[1]};u64 bytes=0;
  for(u32 i=0;i<6;++i){widths_[i]=i<3?in_.width:i<5?(in_.targetWidth+1)/2:in_.targetWidth;heights_[i]=i<3?in_.height:i<5?(in_.targetHeight+1)/2:in_.targetHeight;formats_[i]=device_->GetTexture(images[i])->GetTextureDesc().format;bpp_[i]=CapturePixelBytes(formats_[i]);if(!bpp_[i])return false;pitch_[i]=(widths_[i]*bpp_[i]+255)&~255u;offset_[i]=bytes;bytes+=u64(pitch_[i])*heights_[i];}
  BufferDesc d{};d.size=bytes;d.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(d);if(readback_==handles::INVALID_RESOURCE)return false;
  for(u32 i=0;i<6;++i){BufferTextureCopyRegion r{};r.bufferOffset=offset_[i];r.bufferRowLength=pitch_[i]/bpp_[i];r.bufferImageHeight=heights_[i];r.imageExtent={widths_[i],heights_[i],1};cmd.CopyTextureToBuffer(images[i],readback_,&r,1);}return true;
 }
 bool Save(const std::string& prefix){
  if(readback_==handles::INVALID_RESOURCE)return false;const auto* bytes=static_cast<const char*>(device_->MapBuffer(readback_,0,offset_[5]+u64(pitch_[5])*heights_[5]));if(!bytes)return false;
  std::ofstream f(prefix+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&capturedParams_),sizeof(Params));u32 invalid=0;
  for(u32 i=0;i<6;++i)for(u32 y=0;y<heights_[i];++y){const auto* row=bytes+offset_[i]+u64(pitch_[i])*y;f.write(row,widths_[i]*bpp_[i]);if(i>=4){const auto* values=reinterpret_cast<const _Float16*>(row);for(u32 x=0;x<widths_[i]*4;++x)invalid+=!std::isfinite(float(values[x]));}}f.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
  std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<captured_<<",\"source_digest\":"<<in_.scene.digest<<",\"invalid\":"<<invalid<<",\"normal_format\":\""<<CaptureFormatName(formats_[1])<<"\",\"orm_format\":\""<<CaptureFormatName(formats_[2])<<"\",\"layout\":\"Params208;source D32,normal/orm as formats;previous trace,current trace,resolve RGBA16F tight rows\"}\n";meta.close();return f.good()&&meta.good()&&!invalid;
 }
 void Shutdown(){if(!device_)return;for(auto& s:slots_){for(auto h:s.sets)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);for(auto h:s.output)if(h!=handles::INVALID_RESOURCE)device_->DestroyTexture(h);if(s.params!=handles::INVALID_RESOURCE)device_->DestroyBuffer(s.params);s=Slot{};}for(u32 i=0;i<2;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);}if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);device_=nullptr;ready_=false;nextFrame_=0;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;shaders_.fill(handles::INVALID_SHADER);pipelines_.fill(handles::INVALID_PIPELINE);readback_=handles::INVALID_RESOURCE;}
private:
 struct Slot{ResourceHandle params=handles::INVALID_RESOURCE,history=handles::INVALID_RESOURCE;std::array<ResourceHandle,2> output{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};std::array<DescriptorSetHandle,2> sets{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};u32 frame=~0u;Params cpu{};};
 DawnDevice* device_=nullptr;Inputs in_{};bool ready_=false;u32 nextFrame_=0,captured_=~0u;std::array<Slot,3> slots_{};primal::math::m4x4 previousVP_;Params capturedParams_{};DataFormat formats_[6]{};u32 bpp_[6]{};u32 widths_[6]{},heights_[6]{},pitch_[6]{};u64 offset_[6]{};ResourceHandle readback_=handles::INVALID_RESOURCE;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
};
}
