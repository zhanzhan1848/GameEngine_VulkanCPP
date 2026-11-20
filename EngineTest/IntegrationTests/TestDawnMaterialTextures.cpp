// Real Sponza upload helper -> encoded mip readback -> native WGSL sampling.
#include "DawnSponzaTextures.h"
#include "Engine/Common/CoreDiag.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace primal::graphics::rhi;
static void Require(bool ok, const char* message) {
    if (!ok) {
        primal::diag::Emit(primal::diag::kLevelError, "DawnMaterialTextures", message);
        std::exit(1);
    }
}
static void Pump(WGPUInstance instance, const bool& done) {
    const auto start = std::chrono::steady_clock::now();
    while (!done) {
        wgpuInstanceProcessEvents(instance);
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(30), "Dawn callback timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main() {
    DeviceDesc dd{}; dd.platform = RHIPlatform::Dawn; dd.enableValidation = true;
    DawnDevice device(dd);
    Require(device.Initialize(), "Dawn initialization failed");
    auto native = device.GetNativeDevice();
    wgpuDevicePushErrorScope(native, WGPUErrorFilter_Validation);
    const char* code = R"(
@group(0) @binding(0) var tex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<storage, read_write> results: array<vec4<f32>>;
@compute @workgroup_size(1)
fn probe(@builtin(global_invocation_id) id: vec3<u32>) {
    let mip = id.x;
    results[2u * mip] = textureLoad(tex, vec2<i32>(0), i32(mip));
    let uv = vec2<f32>(0.5) / vec2<f32>(textureDimensions(tex, mip));
    results[2u * mip + 1u] = textureSampleLevel(tex, samp, uv, f32(mip));
})";
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = {code, WGPU_STRLEN};
    WGPUShaderModuleDescriptor sd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    sd.nextInChain = &source.chain;
    auto shader = wgpuDeviceCreateShaderModule(native, &sd);
    WGPUComputePipelineDescriptor pd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    pd.compute.module = shader; pd.compute.entryPoint = {"probe", WGPU_STRLEN};
    auto pipeline = wgpuDeviceCreateComputePipeline(native, &pd);
    auto layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    WGPUSamplerDescriptor samplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    samplerDesc.lodMaxClamp = 32;
    auto sampler = wgpuDeviceCreateSampler(native, &samplerDesc);
    size_t encodedChecks = 0, sampleChecks = 0, cases = 0;
    enum Pattern { Checker, Gray, LastColumn, LastRow };
    struct Case { int w, h; bool srgb; Pattern pattern; };
    for (const auto& test : {
        Case{8,8,true,Checker}, Case{8,8,false,Checker},
        Case{8,8,true,Gray}, Case{8,8,false,Gray},
        Case{3,1,true,LastColumn}, Case{1,3,true,LastRow},
        Case{3,1,false,LastColumn}, Case{1,3,false,LastRow},
        Case{1,8,true,Gray}, Case{1,1,true,Gray}}) {
        std::vector<unsigned char> pixels(size_t(test.w) * test.h * 4);
        for (int y = 0; y < test.h; ++y) for (int x = 0; x < test.w; ++x) {
            const bool white = test.pattern == Checker ? (x + y) % 2 != 0 :
                test.pattern == LastColumn ? x == test.w - 1 : y == test.h - 1;
            for (int c = 0; c < 4; ++c)
                pixels[(size_t(y) * test.w + x) * 4 + c] = test.pattern == Gray
                    ? (c == 3 ? 64 : 128) : (white ? 255 : 0);
        }
        const auto format = test.srgb ? DataFormat::RGBA8_sRGB : DataFormat::RGBA8_UNorm;
        auto texture = primal::tests::dawn_sponza::CreateTexture(device, test.w, test.h, pixels.data(), format);
        Require(texture != handles::INVALID_RESOURCE, "Sponza upload failed");
        auto* wrapped = device.GetTexture(texture);
        auto actual = wrapped->GetNativeTexture();
        Require(wgpuTextureGetFormat(actual) == (test.srgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm), "wrong native format");
        const u32 mipCount = wrapped->GetTextureDesc().mipLevels;
        u32 expectedMipCount = 1;
        for (int dim = std::max(test.w, test.h); dim > 1; dim /= 2) ++expectedMipCount;
        Require(mipCount == expectedMipCount && wgpuTextureGetMipLevelCount(actual) == mipCount, "wrong mip count");
        std::vector<double> expectedSamples;
        int w = test.w, h = test.h;
        for (u32 mip = 0; mip < mipCount; ++mip) {
            BufferDesc bd{}; bd.size = size_t(w) * h * 4; bd.memoryUsage = GPUMemoryUsage::Readback;
            auto readback = device.CreateBuffer(bd);
            auto command = device.CreateCommandBuffer(CommandQueueType::Graphics);
            auto* cmd = device.GetCommandBuffer(command);
            Require(cmd && cmd->Begin(), "copy command begin failed");
            BufferTextureCopyRegion region{};
            region.imageSubresource.mipLevel = mip; region.imageExtent = {u32(w), u32(h), 1};
            cmd->CopyTextureToBuffer(texture, readback, &region, 1);
            Require(cmd->End(), "copy command end failed");
            QueueSubmitInfo submit{}; submit.cmdBuffer = command;
            Require(device.Submit(submit) && cmd->WaitForCompletion(), "copy completion failed");
            const auto* bytes = static_cast<const unsigned char*>(device.MapBuffer(readback, 0, bd.size));
            Require(bytes, "mip readback failed");
            for (size_t i = 0; i < bd.size; ++i) {
                // Independent analytic fixtures: 50% black/white -> sRGB 188;
                // Frozen sRGB 3->1 linear blit samples the middle texel: black.
                // Linear-data textures retain the independent CPU area filter.
                const int expected = mip == 0 ? pixels[i] : test.pattern == Gray ? (i % 4 == 3 ? 64 : 128) :
                    test.pattern == Checker ? (i % 4 == 3 || !test.srgb ? 128 : 188) : (test.srgb ? 0 : 85);
                Require(bytes[i] == expected, "encoded mip mismatch (including mip0)");
                ++encodedChecks;
            }
            for (int c = 0; c < 4; ++c) {
                // Compare GPU decode against physical linear fixture values,
                // allowing only 8-bit encoding/quantization error in lower mips.
                double expected = test.pattern == Gray ? (c == 3 ? 64.0 / 255.0 : test.srgb ? 0.2158605001 : 128.0 / 255.0) :
                    mip == 0 ? 0.0 : test.pattern == Checker ? 0.5 : test.srgb ? 0.0 : 1.0 / 3.0;
                expectedSamples.push_back(expected);
            }
            device.UnmapBuffer(readback);
            device.DestroyCommandBuffer(command); device.DestroyBuffer(readback);
            w = std::max(1, w / 2); h = std::max(1, h / 2);
        }
        const size_t size = mipCount * 2 * 4 * sizeof(float);
        auto makeBuffer = [&](WGPUBufferUsage usage) {
            WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
            desc.size = size; desc.usage = usage;
            return wgpuDeviceCreateBuffer(native, &desc);
        };
        auto output = makeBuffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        auto readback = makeBuffer(WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0; entries[0].textureView = wrapped->GetDefaultView();
        entries[1].binding = 1; entries[1].sampler = sampler;
        entries[2].binding = 2; entries[2].buffer = output; entries[2].size = size;
        WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        gd.layout = layout; gd.entryCount = 3; gd.entries = entries;
        auto group = wgpuDeviceCreateBindGroup(native, &gd);
        auto encoder = wgpuDeviceCreateCommandEncoder(native, nullptr);
        auto pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
        wgpuComputePassEncoderSetPipeline(pass, pipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, mipCount, 1, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuCommandEncoderCopyBufferToBuffer(encoder, output, 0, readback, 0, size);
        auto command = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(device.GetQueue(), 1, &command);
        bool mapped = false;
        WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_AllowProcessEvents; callback.userdata1 = &mapped;
        callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* data, void*) {
            Require(status == WGPUMapAsyncStatus_Success, "WGSL readback mapping failed");
            *static_cast<bool*>(data) = true;
        };
        wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, size, callback);
        Pump(device.GetInstance(), mapped);
        const auto* values = static_cast<const float*>(wgpuBufferGetConstMappedRange(readback, 0, size));
        Require(values, "WGSL readback null");
        for (u32 mip = 0; mip < mipCount; ++mip) for (int mode = 0; mode < 2; ++mode) for (int c = 0; c < 4; ++c) {
            const auto actualValue = values[mip * 8 + mode * 4 + c];
            Require(std::isfinite(actualValue) && std::abs(actualValue - expectedSamples[mip * 4 + c]) < 0.004, "WGSL linear sample mismatch");
            ++sampleChecks;
        }
        wgpuBufferUnmap(readback);
        wgpuCommandBufferRelease(command); wgpuComputePassEncoderRelease(pass); wgpuCommandEncoderRelease(encoder);
        wgpuBindGroupRelease(group); wgpuBufferRelease(output); wgpuBufferRelease(readback);
        device.DestroyTexture(texture);
        ++cases;
    }
    const unsigned char pixel[4]{};
    Require(primal::tests::dawn_sponza::CreateTexture(device, 0, 1, pixel, DataFormat::RGBA8_sRGB) == handles::INVALID_RESOURCE, "zero dimension accepted");
    Require(primal::tests::dawn_sponza::CreateTexture(device, -1, 1, pixel, DataFormat::RGBA8_sRGB) == handles::INVALID_RESOURCE, "negative dimension accepted");
    Require(primal::tests::dawn_sponza::CreateTexture(device, 1, 1, nullptr, DataFormat::RGBA8_sRGB) == handles::INVALID_RESOURCE, "null pixels accepted");
    Require(primal::tests::dawn_sponza::CreateTexture(device, 1, 1, pixel, DataFormat::RGBA16_Float) == handles::INVALID_RESOURCE, "unsupported format accepted");
    bool checked = false;
    WGPUPopErrorScopeCallbackInfo errorCallback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    errorCallback.mode = WGPUCallbackMode_AllowProcessEvents; errorCallback.userdata1 = &checked;
    errorCallback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView, void* data, void*) {
        Require(status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError, "Dawn validation scope failed");
        *static_cast<bool*>(data) = true;
    };
    wgpuDevicePopErrorScope(native, errorCallback);
    Pump(device.GetInstance(), checked);
    wgpuSamplerRelease(sampler); wgpuBindGroupLayoutRelease(layout);
    wgpuComputePipelineRelease(pipeline); wgpuShaderModuleRelease(shader);
    device.Shutdown();
    std::printf("{\"passed\":true,\"textures\":%zu,\"encoded_components\":%zu,\"wgsl_linear_components\":%zu,\"rejected_inputs\":4}\n", cases, encodedChecks, sampleChecks);
}
