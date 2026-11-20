#pragma once
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/Utils/ShaderRegistry.h"
#include "Engine/Common/CoreDiag.h"
#include <chrono>
#include <fstream>
#include <thread>

namespace primal::tests::dawn_sponza {
// Sponza albedo parity: a format-matched WGSL linear blit for each mip.
// Source/destination views select disjoint subresources; WebGPU owns barriers.
inline bool GenerateAlbedoMips(graphics::rhi::DawnDevice& device,
                              graphics::rhi::ResourceHandle texture) {
    using namespace graphics::rhi;
    auto* wrapped = device.GetTexture(texture);
    if (!wrapped || wrapped->GetTextureDesc().format != DataFormat::RGBA8_sRGB) return false;
    const auto levels = wrapped->GetTextureDesc().mipLevels;
    if (levels <= 1) return true;
    const auto path = graphics::utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Dawn/shaders/Blit.wgsl");
    std::ifstream file(path);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    if (text.empty()) return false;
    auto native = device.GetNativeDevice();
    wgpuDevicePushErrorScope(native, WGPUErrorFilter_Validation);
    struct Resources {
        WGPUShaderModule shader{}; WGPURenderPipeline pipeline{};
        WGPUBindGroupLayout layout{}; WGPUSampler sampler{};
        WGPUCommandEncoder encoder{}; WGPUCommandBuffer command{};
        ~Resources() {
            if (command) wgpuCommandBufferRelease(command);
            if (encoder) wgpuCommandEncoderRelease(encoder);
            if (sampler) wgpuSamplerRelease(sampler);
            if (layout) wgpuBindGroupLayoutRelease(layout);
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
            if (shader) wgpuShaderModuleRelease(shader);
        }
    } r;
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = {text.c_str(), text.size()};
    WGPUShaderModuleDescriptor sd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    sd.nextInChain = &source.chain;
    r.shader = wgpuDeviceCreateShaderModule(native, &sd);
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = WGPUTextureFormat_RGBA8UnormSrgb;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = r.shader; fragment.entryPoint = {"blit_fs", WGPU_STRLEN};
    fragment.targetCount = 1; fragment.targets = &target;
    WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pd.vertex.module = r.shader; pd.vertex.entryPoint = {"blit_vs", WGPU_STRLEN};
    pd.fragment = &fragment; pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.multisample.count = 1; pd.multisample.mask = ~0u;
    r.pipeline = wgpuDeviceCreateRenderPipeline(native, &pd);
    r.layout = wgpuRenderPipelineGetBindGroupLayout(r.pipeline, 0);
    WGPUSamplerDescriptor sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler.minFilter = sampler.magFilter = WGPUFilterMode_Linear;
    sampler.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler.lodMaxClamp = 0;
    r.sampler = wgpuDeviceCreateSampler(native, &sampler);
    WGPUCommandEncoderDescriptor ed = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    r.encoder = wgpuDeviceCreateCommandEncoder(native, &ed);
    const auto image = wrapped->GetNativeTexture();
    for (u32 mip = 1; mip < levels; ++mip) {
        WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        vd.format = target.format; vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = mip - 1; vd.mipLevelCount = 1; vd.arrayLayerCount = 1;
        auto src = wgpuTextureCreateView(image, &vd);
        vd.baseMipLevel = mip;
        auto dst = wgpuTextureCreateView(image, &vd);
        WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0; entries[0].textureView = src;
        entries[1].binding = 1; entries[1].sampler = r.sampler;
        WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bd.layout = r.layout; bd.entryCount = 2; bd.entries = entries;
        auto group = wgpuDeviceCreateBindGroup(native, &bd);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = dst; ca.loadOp = WGPULoadOp_Clear; ca.storeOp = WGPUStoreOp_Store;
        ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rd.colorAttachmentCount = 1; rd.colorAttachments = &ca;
        auto pass = wgpuCommandEncoderBeginRenderPass(r.encoder, &rd);
        wgpuRenderPassEncoderSetPipeline(pass, r.pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
        wgpuBindGroupRelease(group); wgpuTextureViewRelease(src); wgpuTextureViewRelease(dst);
    }
    WGPUCommandBufferDescriptor cb = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    r.command = wgpuCommandEncoderFinish(r.encoder, &cb);
    wgpuQueueSubmit(device.GetQueue(), 1, &r.command);
    struct Result { bool done = false; bool ok = false; } completion, validation;
    WGPUQueueWorkDoneCallbackInfo done = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    done.mode = WGPUCallbackMode_AllowProcessEvents; done.userdata1 = &completion;
    done.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView, void* ptr, void*) {
        auto& result = *static_cast<Result*>(ptr);
        result.ok = status == WGPUQueueWorkDoneStatus_Success; result.done = true;
    };
    wgpuQueueOnSubmittedWorkDone(device.GetQueue(), done);
    WGPUPopErrorScopeCallbackInfo checked = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    checked.mode = WGPUCallbackMode_AllowProcessEvents; checked.userdata1 = &validation;
    checked.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView, void* ptr, void*) {
        auto& result = *static_cast<Result*>(ptr);
        result.ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
        result.done = true;
    };
    wgpuDevicePopErrorScope(native, checked);
    const auto start = std::chrono::steady_clock::now();
    while (!completion.done || !validation.done) {
        wgpuInstanceProcessEvents(device.GetInstance());
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) {
            primal::diag::Emit(primal::diag::kLevelError, "DawnAlbedoMips", "Completion timeout; resource ownership unknown");
            std::_Exit(7);
        }
#ifdef __EMSCRIPTEN__
        emscripten_sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    if (!completion.ok || !validation.ok)
        primal::diag::Emit(primal::diag::kLevelError, "DawnAlbedoMips", "Mip generation or validation failed");
    return completion.ok && validation.ok;
}
} // namespace primal::tests::dawn_sponza
