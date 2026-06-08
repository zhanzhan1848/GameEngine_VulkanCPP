#include "DawnDevice.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "DawnBuffer.h"
#include "DawnTexture.h"
#include "DawnCommandBuffer.h"
#include "DawnSync.h"
#include "DawnQuery.h"
#include "DawnShader.h"
#include "DawnPipeline.h"
#include "DawnPipelineLayout.h"
#include "DawnDescriptorSetLayout.h"
#include "DawnDescriptorSet.h"
#include "DawnRenderPass.h"
#include "DawnSampler.h"
#include "DawnSwapChain.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <chrono>

namespace primal::graphics::rhi {

// === Helper structures for async callbacks ===

struct AdapterUserData {
    WGPUAdapter adapter{nullptr};
    bool done{false};
    bool success{false};
};

struct DeviceUserData {
    WGPUDevice device{nullptr};
    bool done{false};
    bool success{false};
};

// === Constructor / Destructor ===

DawnDevice::DawnDevice(const DeviceDesc& desc)
    : RHIDevice<DawnDevice>(desc) {
    std::cout << "[DawnDevice] Creating Dawn WebGPU device" << std::endl;
}

DawnDevice::~DawnDevice() {
    if (IsValid()) {
        Shutdown();
    }
}

// === Core lifecycle ===

bool DawnDevice::initializeImpl() {
    std::cout << "[DawnDevice] Initializing..." << std::endl;

    // 1. Create WGPU Instance
    WGPUInstanceDescriptor instanceDesc{};
    instanceDesc.nextInChain = nullptr;
    instanceDesc.requiredFeatureCount = 0;
    instanceDesc.requiredFeatures = nullptr;
    instanceDesc.requiredLimits = nullptr;
    wgpuInstance_ = wgpuCreateInstance(&instanceDesc);
    if (!wgpuInstance_) {
        std::cerr << "[DawnDevice] Failed to create WGPU instance" << std::endl;
        return false;
    }
    std::cout << "[DawnDevice] WGPU instance created" << std::endl;

    // 2. Request Adapter (async with polling)
    WGPURequestAdapterOptions adapterOptions{};
    adapterOptions.nextInChain = nullptr;
    adapterOptions.featureLevel = WGPUFeatureLevel_Undefined;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;
    adapterOptions.forceFallbackAdapter = WGPU_FALSE;
    adapterOptions.backendType = WGPUBackendType_Undefined;
    adapterOptions.compatibleSurface = nullptr;

    AdapterUserData adapterData{};
    wgpuInstanceRequestAdapter(
        wgpuInstance_,
        &adapterOptions,
        WGPURequestAdapterCallbackInfo{
            .nextInChain = nullptr,
            .mode = WGPUCallbackMode_AllowProcessEvents,
            .callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2) {
                auto* data = static_cast<AdapterUserData*>(userdata1);
                (void)userdata2;
                if (status == WGPURequestAdapterStatus_Success) {
                    data->adapter = adapter;
                    data->success = true;
                } else {
                    std::cerr << "[DawnDevice] Adapter request failed: "
                              << (message.data ? std::string(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length) : "unknown error") << std::endl;
                }
                data->done = true;
            },
            .userdata1 = &adapterData,
            .userdata2 = nullptr,
        });

    // Pump callbacks until adapter request completes.
    // On Emscripten the WebGPU adapter callback is dispatched via JS microtasks,
    // so a tight WASM loop blocks the event loop. We yield with emscripten_sleep(0)
    // to let pending microtasks (and the actual adapter request) make progress.
    while (!adapterData.done) {
        wgpuInstanceProcessEvents(wgpuInstance_);
#ifdef __EMSCRIPTEN__
        emscripten_sleep(0);
#endif
    }

    if (!adapterData.success || !adapterData.adapter) {
        std::cerr << "[DawnDevice] Failed to get adapter" << std::endl;
        return false;
    }
    wgpuAdapter_ = adapterData.adapter;
    std::cout << "[DawnDevice] Adapter acquired" << std::endl;

    // 3. Request Device (async with polling)
    WGPULimits requiredLimits{};
    requiredLimits.nextInChain = nullptr;
    requiredLimits.maxBufferSize = 256 * 1024 * 1024;               // 256 MB
    requiredLimits.maxStorageBufferBindingSize = 128 * 1024 * 1024;  // 128 MB
    requiredLimits.maxUniformBufferBindingSize = 64 * 1024;          // 64 KB

    WGPULimits adapterLimits{};
    adapterLimits.nextInChain = nullptr;
    if (wgpuAdapterGetLimits(wgpuAdapter_, &adapterLimits) == WGPUStatus_Success) {
        // Start from adapter-supported limits, then override our requirements
        requiredLimits = adapterLimits;
        requiredLimits.maxBufferSize = (std::max)(requiredLimits.maxBufferSize, 256ull * 1024ull * 1024ull);
        requiredLimits.maxStorageBufferBindingSize = (std::max)(requiredLimits.maxStorageBufferBindingSize, 128ull * 1024ull * 1024ull);
        requiredLimits.maxUniformBufferBindingSize = (std::max)(requiredLimits.maxUniformBufferBindingSize, 64ull * 1024ull);
    }

    WGPUDeviceDescriptor deviceDesc{};
    deviceDesc.nextInChain = nullptr;
    deviceDesc.label = ToWGPUStringView("Primal Dawn Device");
    deviceDesc.requiredFeatureCount = 0;
    deviceDesc.requiredFeatures = nullptr;
    deviceDesc.requiredLimits = &requiredLimits;
    deviceDesc.defaultQueue.nextInChain = nullptr;
    deviceDesc.defaultQueue.label = ToWGPUStringView("Default Queue");
    deviceDesc.deviceLostCallbackInfo = WGPUDeviceLostCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = [](WGPUDevice const* device, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void* userdata2) {
            (void)device;
            (void)userdata1;
            (void)userdata2;
            const char* reasonStr = "Unknown";
            switch (reason) {
                case WGPUDeviceLostReason_Unknown:            reasonStr = "Unknown"; break;
                case WGPUDeviceLostReason_Destroyed:          reasonStr = "Destroyed"; break;
                case WGPUDeviceLostReason_CallbackCancelled:  reasonStr = "CallbackCancelled"; break;
                case WGPUDeviceLostReason_FailedCreation:     reasonStr = "FailedCreation"; break;
                default: break;
            }
            std::cout << "[DawnDevice] Device lost (" << reasonStr << "): "
                      << (message.data ? std::string(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length) : "no message") << std::endl;
        },
        .userdata1 = nullptr,
        .userdata2 = nullptr,
    };
    deviceDesc.uncapturedErrorCallbackInfo = WGPUUncapturedErrorCallbackInfo{
        .nextInChain = nullptr,
        .callback = [](WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2) {
            (void)device;
            (void)userdata1;
            (void)userdata2;
            const char* typeStr = "Unknown";
            switch (type) {
                case WGPUErrorType_NoError:     typeStr = "NoError"; break;
                case WGPUErrorType_Validation:  typeStr = "Validation"; break;
                case WGPUErrorType_OutOfMemory: typeStr = "OutOfMemory"; break;
                case WGPUErrorType_Internal:    typeStr = "Internal"; break;
                case WGPUErrorType_Unknown:     typeStr = "Unknown"; break;
                default: break;
            }
            std::cout << "[DawnDevice] Device error (" << typeStr << "): "
                      << (message.data ? std::string(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length) : "no message") << std::endl;
        },
        .userdata1 = nullptr,
        .userdata2 = nullptr,
    };

    DeviceUserData deviceData{};
    wgpuAdapterRequestDevice(
        wgpuAdapter_,
        &deviceDesc,
        WGPURequestDeviceCallbackInfo{
            .nextInChain = nullptr,
            .mode = WGPUCallbackMode_AllowProcessEvents,
            .callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2) {
                auto* data = static_cast<DeviceUserData*>(userdata1);
                (void)userdata2;
                if (status == WGPURequestDeviceStatus_Success) {
                    data->device = device;
                    data->success = true;
                } else {
                    std::cerr << "[DawnDevice] Device request failed: "
                              << (message.data ? std::string(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length) : "unknown error") << std::endl;
                }
                data->done = true;
            },
            .userdata1 = &deviceData,
            .userdata2 = nullptr,
        });

    // Pump callbacks until device request completes.
    while (!deviceData.done) {
        wgpuInstanceProcessEvents(wgpuInstance_);
#ifdef __EMSCRIPTEN__
        emscripten_sleep(0);
#endif
    }

    if (!deviceData.success || !deviceData.device) {
        std::cerr << "[DawnDevice] Failed to get device" << std::endl;
        return false;
    }
    wgpuDevice_ = deviceData.device;
    std::cout << "[DawnDevice] Device acquired (uncaptured error callback active)" << std::endl;

    // 4. Get Queue
    wgpuQueue_ = wgpuDeviceGetQueue(wgpuDevice_);
    if (!wgpuQueue_) {
        std::cerr << "[DawnDevice] Failed to get queue" << std::endl;
        return false;
    }
    std::cout << "[DawnDevice] Queue acquired" << std::endl;

    // 5. Create push constant emulation ring buffer (256 KB)
    WGPUBufferDescriptor pushConstantDesc{};
    pushConstantDesc.nextInChain = nullptr;
    pushConstantDesc.label = ToWGPUStringView("Push Constant Ring Buffer");
    pushConstantDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    pushConstantDesc.size = PUSH_CONSTANT_RING_SIZE;
    pushConstantDesc.mappedAtCreation = WGPU_FALSE;

    pushConstantBuffer_ = wgpuDeviceCreateBuffer(wgpuDevice_, &pushConstantDesc);
    if (!pushConstantBuffer_) {
        std::cerr << "[DawnDevice] Failed to create push constant ring buffer" << std::endl;
        return false;
    }
    std::cout << "[DawnDevice] Push constant ring buffer created ("
              << PUSH_CONSTANT_RING_SIZE << " bytes)" << std::endl;

    // 6. Reserve allocators
    bufferAllocator_.Reserve(1024);
    textureAllocator_.Reserve(512);
    commandBufferAllocator_.Reserve(64);
    syncAllocator_.Reserve(64);
    queryPoolAllocator_.Reserve(16);
    shaderAllocator_.Reserve(256);
    pipelineAllocator_.Reserve(256);
    samplerAllocator_.Reserve(64);
    descriptorSetLayoutAllocator_.Reserve(64);
    pipelineLayoutAllocator_.Reserve(64);
    descriptorSetAllocator_.Reserve(256);
    renderPassAllocator_.Reserve(128);

    std::cout << "[DawnDevice] Initialization complete" << std::endl;
    return true;
}

WGPUTextureView DawnDevice::GetDummyTextureView() {
    if (dummyTextureView_) return dummyTextureView_;

    // Create a 1x1 magenta texture as a visible error indicator
    WGPUTextureDescriptor texDesc{};
    texDesc.nextInChain = nullptr;
    texDesc.label = ToWGPUStringView("Dummy 1x1 Texture");
    texDesc.dimension = WGPUTextureDimension_2D;
    texDesc.size = {1, 1, 1};
    texDesc.format = WGPUTextureFormat_RGBA8Unorm;
    texDesc.mipLevelCount = 1;
    texDesc.sampleCount = 1;
    texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;

    dummyTexture_ = wgpuDeviceCreateTexture(wgpuDevice_, &texDesc);
    if (!dummyTexture_) return nullptr;

    unsigned char magenta[4] = {255, 0, 255, 255};
    WGPUTexelCopyTextureInfo dest{};
    dest.texture = dummyTexture_;
    dest.mipLevel = 0;
    dest.origin = {0, 0, 0};
    dest.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout{};
    layout.offset = 0;
    layout.bytesPerRow = 256;
    layout.rowsPerImage = 1;
    WGPUExtent3D writeSize{1, 1, 1};
    unsigned char padded[256] = {};
    memcpy(padded, magenta, 4);
    wgpuQueueWriteTexture(wgpuQueue_, &dest, padded, 256, &layout, &writeSize);

    dummyTextureView_ = wgpuTextureCreateView(dummyTexture_, nullptr);
    return dummyTextureView_;
}

void DawnDevice::shutdownImpl() {
    std::cout << "[DawnDevice] Shutting down..." << std::endl;

    // Release dummy texture
    if (dummyTextureView_) { wgpuTextureViewRelease(dummyTextureView_); dummyTextureView_ = nullptr; }
    if (dummyTexture_) { wgpuTextureRelease(dummyTexture_); dummyTexture_ = nullptr; }

    // Release blit pipeline
    DestroyBlitPipeline();

    // Shutdown allocators
    bufferAllocator_.Shutdown();
    textureAllocator_.Shutdown();
    commandBufferAllocator_.Shutdown();
    syncAllocator_.Shutdown();
    queryPoolAllocator_.Shutdown();
    shaderAllocator_.Shutdown();
    pipelineAllocator_.Shutdown();
    samplerAllocator_.Shutdown();
    descriptorSetLayoutAllocator_.Shutdown();
    pipelineLayoutAllocator_.Shutdown();
    descriptorSetAllocator_.Shutdown();
    renderPassAllocator_.Shutdown();

    // Release push constant buffer
    if (pushConstantBuffer_) {
        wgpuBufferRelease(pushConstantBuffer_);
        pushConstantBuffer_ = nullptr;
    }

    // Release queue
    if (wgpuQueue_) {
        wgpuQueueRelease(wgpuQueue_);
        wgpuQueue_ = nullptr;
    }

    // Release device
    if (wgpuDevice_) {
        wgpuDeviceRelease(wgpuDevice_);
        wgpuDevice_ = nullptr;
    }

    // Release adapter
    if (wgpuAdapter_) {
        wgpuAdapterRelease(wgpuAdapter_);
        wgpuAdapter_ = nullptr;
    }

    // Release instance
    if (wgpuInstance_) {
        wgpuInstanceRelease(wgpuInstance_);
        wgpuInstance_ = nullptr;
    }

    std::cout << "[DawnDevice] Shutdown complete" << std::endl;
}

void DawnDevice::waitIdleImpl() const {
    if (!wgpuDevice_) return;

    // Use wgpuInstanceProcessEvents to process pending work.
    // The new webgpu.h API does not have wgpuDevicePoll; we process
    // events on the instance to flush outstanding work.
    auto startTime = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(5);

    while (true) {
        wgpuInstanceProcessEvents(wgpuInstance_);

        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > timeout) {
            std::cerr << "[DawnDevice] waitIdle timed out after 5 seconds" << std::endl;
            break;
        }

        // Process events a few times to ensure work completes.
        // In practice, for a simple idle wait we rely on event processing
        // to drain the queue. A more robust solution would use a
        // wgpuQueueOnSubmittedWorkDone future.
        break;
    }
}

void DawnDevice::beginFrameImpl() {
    ResetPushConstantOffset();
    currentFrameIndex_ = currentFrameIndex_.load() + 1;
}

void DawnDevice::endFrameImpl() {
    // No-op: frame boundary is handled by the base class GC
}

void DawnDevice::presentImpl() {
    // No-op: present is handled by DawnSwapChain
}

void DawnDevice::queryDeviceInfo(DeviceInfo& info) {
    info.platform = RHIPlatform::Dawn;

    // Query adapter info for device name
    if (wgpuAdapter_) {
        WGPUAdapterInfo adapterInfo{};
        adapterInfo.nextInChain = nullptr;
        if (wgpuAdapterGetInfo(wgpuAdapter_, &adapterInfo) == WGPUStatus_Success) {
            // Build device name from adapter info
            std::string name;
            if (adapterInfo.vendor.data) {
                name += std::string(adapterInfo.vendor.data, adapterInfo.vendor.length == WGPU_STRLEN ? std::strlen(adapterInfo.vendor.data) : adapterInfo.vendor.length);
                name += " ";
            }
            if (adapterInfo.architecture.data) {
                name += std::string(adapterInfo.architecture.data, adapterInfo.architecture.length == WGPU_STRLEN ? std::strlen(adapterInfo.architecture.data) : adapterInfo.architecture.length);
                name += " ";
            }
            if (adapterInfo.description.data) {
                name += std::string(adapterInfo.description.data, adapterInfo.description.length == WGPU_STRLEN ? std::strlen(adapterInfo.description.data) : adapterInfo.description.length);
            }

            if (!name.empty()) {
                strncpy(info.deviceName, name.c_str(), sizeof(info.deviceName) - 1);
                info.deviceName[sizeof(info.deviceName) - 1] = '\0';
            }

            // Driver version from adapter info description
            if (adapterInfo.description.data) {
                std::string desc(adapterInfo.description.data, adapterInfo.description.length == WGPU_STRLEN ? std::strlen(adapterInfo.description.data) : adapterInfo.description.length);
                strncpy(info.driverVersion, desc.c_str(), sizeof(info.driverVersion) - 1);
                info.driverVersion[sizeof(info.driverVersion) - 1] = '\0';
            }

            wgpuAdapterInfoFreeMembers(adapterInfo);
        }
    }

    // Reasonable default limits for WebGPU
    info.maxTexture1DSize = 16384;
    info.maxTexture2DSize = 16384;
    info.maxTexture3DSize = 4096;
    info.maxTextureCubeSize = 16384;
    info.maxRenderTargets = 8;
    info.maxVertexAttributes = 16;
    info.maxSamplerStates = 16;
    info.maxConstantBufferSize = 64 * 1024; // 64 KB

    // Capability flags
    info.supportsRayTracing = false;
    info.supportsMeshShaders = false;
    info.supportsVariableRateShading = false;
}

u32 DawnDevice::getCurrentFrameIndexImpl() const {
    return currentFrameIndex_.load();
}

// === Resource creation ===

ResourceHandle DawnDevice::createBufferImpl(const BufferDesc& desc) {
    u32 id = bufferAllocator_.Allocate(*this, desc);
    auto* buf = bufferAllocator_.Get(id);
    if (!buf) {
        std::cerr << "[DawnDevice] Buffer allocation failed" << std::endl;
        bufferAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    if (!buf->Initialize()) {
        std::cerr << "[DawnDevice] Buffer Initialize failed for id: " << id << std::endl;
        bufferAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    buf->SetHandle(static_cast<ResourceHandle>(id));
    ResourceManager::Instance().RegisterResource(buf);
    return static_cast<ResourceHandle>(id);
}

ResourceHandle DawnDevice::createTextureImpl(const TextureDesc& desc) {
    u32 id = textureAllocator_.Allocate(*this, desc);
    auto* tex = textureAllocator_.Get(id);
    if (!tex) {
        std::cerr << "[DawnDevice] Texture allocation failed" << std::endl;
        textureAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    if (!tex->Initialize()) {
        std::cerr << "[DawnDevice] Texture Initialize failed for id: " << id << std::endl;
        textureAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    tex->SetHandle(static_cast<ResourceHandle>(id));
    ResourceManager::Instance().RegisterResource(tex);
    return static_cast<ResourceHandle>(id);
}

ResourceHandle DawnDevice::createTextureViewImpl(const TextureViewDesc& desc) {
    // In WebGPU, texture views are lightweight objects created from textures.
    // We create a new DawnTexture entry that wraps a WGPUTextureView.
    auto srcId = static_cast<u32>(desc.texture);
    auto* srcTex = textureAllocator_.Get(srcId);
    if (!srcTex) {
        std::cerr << "[DawnDevice] Invalid source texture for view creation" << std::endl;
        return handles::INVALID_RESOURCE;
    }

    // Create the view on the source texture
    WGPUTextureView view = srcTex->CreateView(desc);
    if (!view) {
        std::cerr << "[DawnDevice] Failed to create WGPUTextureView" << std::endl;
        return handles::INVALID_RESOURCE;
    }

    // Allocate a new DawnTexture slot — do NOT call Initialize() since we
    // don't want a new WGPUTexture. Just set the view directly.
    TextureDesc viewDesc;
    viewDesc.name = "TextureView";
    viewDesc.type = desc.viewType;
    viewDesc.format = desc.format;
    viewDesc.size = srcTex->GetTextureDesc().size;
    viewDesc.mipLevels = desc.mipCount;
    viewDesc.arraySize = desc.arraySize;
    viewDesc.usage = srcTex->GetTextureDesc().usage;
    viewDesc.memoryUsage = srcTex->GetTextureDesc().memoryUsage;

    u32 id = textureAllocator_.Allocate(*this, viewDesc);
    auto* viewTex = textureAllocator_.Get(id);
    if (!viewTex) {
        std::cerr << "[DawnDevice] Texture view allocation failed" << std::endl;
        wgpuTextureViewRelease(view);
        textureAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }

    // Override with the created view — no new GPU texture allocation
    viewTex->OverrideNativeTexture(nullptr, view);

    return static_cast<ResourceHandle>(id);
}

ShaderHandle DawnDevice::createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint) {
    u32 id = shaderAllocator_.Allocate(*this);
    auto* shader = shaderAllocator_.Get(id);
    if (!shader) {
        std::cerr << "[DawnDevice] Shader allocation failed" << std::endl;
        shaderAllocator_.Free(id);
        return handles::INVALID_SHADER;
    }
    if (!shader->Initialize(data, size, stage, entryPoint)) {
        std::cerr << "[DawnDevice] Shader Initialize failed for id: " << id << std::endl;
        shaderAllocator_.Free(id);
        return handles::INVALID_SHADER;
    }
    return static_cast<ShaderHandle>(id);
}

PipelineHandle DawnDevice::createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc) {
    u32 id = pipelineAllocator_.Allocate(*this);
    auto* pipeline = pipelineAllocator_.Get(id);
    if (!pipeline) {
        std::cerr << "[DawnDevice] Pipeline allocation failed" << std::endl;
        pipelineAllocator_.Free(id);
        return handles::INVALID_PIPELINE;
    }
    if (!pipeline->InitializeGraphics(desc)) {
        std::cerr << "[DawnDevice] Graphics Pipeline InitializeGraphics failed for id: " << id << std::endl;
        pipelineAllocator_.Free(id);
        return handles::INVALID_PIPELINE;
    }
    return static_cast<PipelineHandle>(id);
}

PipelineHandle DawnDevice::createComputePipelineImpl(const ComputePipelineDesc& desc) {
    u32 id = pipelineAllocator_.Allocate(*this);
    auto* pipeline = pipelineAllocator_.Get(id);
    if (!pipeline) {
        std::cerr << "[DawnDevice] Pipeline allocation failed" << std::endl;
        pipelineAllocator_.Free(id);
        return handles::INVALID_PIPELINE;
    }
    if (!pipeline->InitializeCompute(desc)) {
        std::cerr << "[DawnDevice] Compute Pipeline InitializeCompute failed for id: " << id << std::endl;
        pipelineAllocator_.Free(id);
        return handles::INVALID_PIPELINE;
    }
    return static_cast<PipelineHandle>(id);
}

CommandBufferHandle DawnDevice::createCommandBufferImpl(CommandQueueType type) {
    u32 id = commandBufferAllocator_.Allocate(*this, type);
    auto* cmdBuf = commandBufferAllocator_.Get(id);
    if (!cmdBuf) {
        std::cerr << "[DawnDevice] CommandBuffer allocation failed" << std::endl;
        commandBufferAllocator_.Free(id);
        return handles::INVALID_COMMAND_BUFFER;
    }
    if (!cmdBuf->Initialize()) {
        std::cerr << "[DawnDevice] CommandBuffer Initialize failed for id: " << id << std::endl;
        commandBufferAllocator_.Free(id);
        return handles::INVALID_COMMAND_BUFFER;
    }
    return static_cast<CommandBufferHandle>(id);
}

SamplerHandle DawnDevice::createSamplerImpl(const SamplerDesc& desc) {
    u32 id = samplerAllocator_.Allocate(*this);
    auto* sampler = samplerAllocator_.Get(id);
    if (!sampler) {
        std::cerr << "[DawnDevice] Sampler allocation failed" << std::endl;
        samplerAllocator_.Free(id);
        return handles::INVALID_SAMPLER;
    }
    if (!sampler->Initialize(desc)) {
        std::cerr << "[DawnDevice] Sampler Initialize failed for id: " << id << std::endl;
        samplerAllocator_.Free(id);
        return handles::INVALID_SAMPLER;
    }
    return static_cast<SamplerHandle>(id);
}

DescriptorSetLayoutHandle DawnDevice::createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc) {
    u32 id = descriptorSetLayoutAllocator_.Allocate(*this);
    auto* layout = descriptorSetLayoutAllocator_.Get(id);
    if (!layout) {
        std::cerr << "[DawnDevice] DescriptorSetLayout allocation failed" << std::endl;
        descriptorSetLayoutAllocator_.Free(id);
        return static_cast<DescriptorSetLayoutHandle>(handles::INVALID_RESOURCE);
    }
    if (!layout->Initialize(desc)) {
        std::cerr << "[DawnDevice] DescriptorSetLayout Initialize failed for id: " << id << std::endl;
        descriptorSetLayoutAllocator_.Free(id);
        return static_cast<DescriptorSetLayoutHandle>(handles::INVALID_RESOURCE);
    }
    return static_cast<DescriptorSetLayoutHandle>(id);
}

PipelineLayoutHandle DawnDevice::createPipelineLayoutImpl(const PipelineLayoutDesc& desc) {
    u32 id = pipelineLayoutAllocator_.Allocate(*this);
    auto* layout = pipelineLayoutAllocator_.Get(id);
    if (!layout) {
        std::cerr << "[DawnDevice] PipelineLayout allocation failed" << std::endl;
        pipelineLayoutAllocator_.Free(id);
        return handles::INVALID_PIPELINE_LAYOUT;
    }
    if (!layout->Initialize(desc)) {
        std::cerr << "[DawnDevice] PipelineLayout Initialize failed for id: " << id << std::endl;
        pipelineLayoutAllocator_.Free(id);
        return handles::INVALID_PIPELINE_LAYOUT;
    }
    return static_cast<PipelineLayoutHandle>(id);
}

DescriptorSetHandle DawnDevice::createDescriptorSetImpl(const DescriptorSetDesc& desc) {
    u32 id = descriptorSetAllocator_.Allocate(*this);
    auto* set = descriptorSetAllocator_.Get(id);
    if (!set) {
        std::cerr << "[DawnDevice] DescriptorSet allocation failed" << std::endl;
        descriptorSetAllocator_.Free(id);
        return static_cast<DescriptorSetHandle>(handles::INVALID_RESOURCE);
    }
    if (!set->Initialize(desc)) {
        std::cerr << "[DawnDevice] DescriptorSet Initialize failed for id: " << id << std::endl;
        descriptorSetAllocator_.Free(id);
        return static_cast<DescriptorSetHandle>(handles::INVALID_RESOURCE);
    }
    return static_cast<DescriptorSetHandle>(id);
}

void DawnDevice::updateDescriptorSetsImpl(u32 writeCount, const WriteDescriptorSet* writes) {
    for (u32 w = 0; w < writeCount; ++w) {
        const auto& write = writes[w];
        if (write.dstSet == static_cast<DescriptorSetHandle>(handles::INVALID_RESOURCE)) continue;

        auto* ds = GetDescriptorSet(write.dstSet);
        if (!ds) continue;

        for (u32 d = 0; d < write.descriptorCount; ++d) {
            u32 binding = write.dstBinding + d;

            switch (write.descriptorType) {
            case DescriptorType::UniformBuffer:
            case DescriptorType::UniformBufferDynamic:
            case DescriptorType::StorageBuffer:
            case DescriptorType::StorageBufferDynamic:
            case DescriptorType::UniformTexelBuffer:
            case DescriptorType::StorageTexelBuffer: {
                if (!write.bufferInfo) continue;
                const auto& bufInfo = write.bufferInfo[d];

                DawnPendingBinding* pb = ds->GetOrCreatePending(binding, write.descriptorType);
                if (!pb) continue;

                pb->type = write.descriptorType;
                pb->engineBinding = binding;

                if (bufInfo.buffer != handles::INVALID_RESOURCE) {
                    DawnBuffer* buf = GetBuffer(bufInfo.buffer);
                    if (buf) {
                        pb->buffer = buf->GetNativeBuffer();
                        pb->offset = bufInfo.offset;
                        pb->size = bufInfo.range > 0 ? bufInfo.range : WGPU_WHOLE_SIZE;
                        pb->populated = true;
                    }
                }
                break;
            }
            case DescriptorType::SampledImage:
            case DescriptorType::SampledDepthImage:
            case DescriptorType::StorageImage:
            case DescriptorType::InputAttachment: {
                if (!write.imageInfo) continue;
                const auto& imgInfo = write.imageInfo[d];

                DawnPendingBinding* pb = ds->GetOrCreatePending(binding, write.descriptorType);
                if (!pb) continue;

                pb->type = write.descriptorType;
                pb->engineBinding = binding;

                if (imgInfo.imageView != handles::INVALID_RESOURCE) {
                    DawnTexture* tex = GetTexture(imgInfo.imageView);
                    if (tex && tex->GetDefaultView()) {
                        pb->textureView = tex->GetDefaultView();
                        pb->populated = true;
                    }
                }
                break;
            }
            case DescriptorType::Sampler: {
                if (!write.imageInfo) continue;
                const auto& imgInfo = write.imageInfo[d];

                DawnPendingBinding* pb = ds->GetOrCreatePending(binding, DescriptorType::Sampler);
                if (!pb) continue;

                pb->type = DescriptorType::Sampler;
                pb->engineBinding = binding;

                if (imgInfo.sampler != handles::INVALID_SAMPLER) {
                    DawnSampler* samp = GetSampler(imgInfo.sampler);
                    if (samp) {
                        pb->sampler = samp->GetNativeSampler();
                        pb->populated = true;
                    }
                }
                break;
            }
            case DescriptorType::CombinedImageSampler: {
                if (!write.imageInfo) continue;
                const auto& imgInfo = write.imageInfo[d];

                // Texture part
                DawnPendingBinding* texPB = ds->GetOrCreatePending(binding, DescriptorType::SampledImage);
                if (texPB) {
                    texPB->type = DescriptorType::SampledImage;
                    texPB->engineBinding = binding;
                    if (imgInfo.imageView != handles::INVALID_RESOURCE) {
                        DawnTexture* tex = GetTexture(imgInfo.imageView);
                        if (tex && tex->GetDefaultView()) {
                            texPB->textureView = tex->GetDefaultView();
                            texPB->populated = true;
                        }
                    }
                }

                // Sampler part
                DawnPendingBinding* sampPB = ds->GetOrCreatePending(binding, DescriptorType::Sampler);
                if (sampPB) {
                    sampPB->type = DescriptorType::Sampler;
                    sampPB->engineBinding = binding;
                    if (imgInfo.sampler != handles::INVALID_SAMPLER) {
                        DawnSampler* samp = GetSampler(imgInfo.sampler);
                        if (samp) {
                            sampPB->sampler = samp->GetNativeSampler();
                            sampPB->populated = true;
                        }
                    }
                }
                break;
            }
            default:
                break;
            }
        }

        ds->MarkDirty();
    }
}

RenderPassHandle DawnDevice::createRenderPassImpl(const RenderPassDesc& desc) {
    u32 id = renderPassAllocator_.Allocate(*this);
    auto* pass = renderPassAllocator_.Get(id);
    if (!pass) {
        std::cerr << "[DawnDevice] RenderPass allocation failed" << std::endl;
        renderPassAllocator_.Free(id);
        return static_cast<RenderPassHandle>(handles::INVALID_RESOURCE);
    }
    if (!pass->Initialize(desc)) {
        std::cerr << "[DawnDevice] RenderPass Initialize failed for id: " << id << std::endl;
        renderPassAllocator_.Free(id);
        return static_cast<RenderPassHandle>(handles::INVALID_RESOURCE);
    }
    return static_cast<RenderPassHandle>(id);
}

RHISwapChain* DawnDevice::createSwapChainImpl(const SwapChainDesc& desc) {
    auto* swapChain = new DawnSwapChain(*this, desc);
    if (!swapChain) {
        std::cerr << "[DawnDevice] SwapChain allocation failed" << std::endl;
        return nullptr;
    }
    if (!swapChain->Initialize()) {
        std::cerr << "[DawnDevice] SwapChain Initialize failed" << std::endl;
        delete swapChain;
        return nullptr;
    }
    return swapChain;
}

void DawnDevice::destroySwapChainImpl(RHISwapChain* swapChain) {
    if (swapChain) {
        swapChain->Destroy();
        delete swapChain;
    }
}

SyncHandle DawnDevice::createSyncImpl() {
    u32 id = syncAllocator_.Allocate(*this);
    auto* sync = syncAllocator_.Get(id);
    if (!sync) {
        std::cerr << "[DawnDevice] Sync allocation failed" << std::endl;
        syncAllocator_.Free(id);
        return handles::INVALID_SYNC;
    }
    return static_cast<SyncHandle>(id);
}

bool DawnDevice::waitForSyncImpl(SyncHandle handle, u32 timeoutMs) {
    auto* sync = GetSync(handle);
    if (!sync) {
        std::cerr << "[DawnDevice] waitForSync: invalid handle " << handle << std::endl;
        return false;
    }
    // submitImpl signals synchronously (Signal is called before Submit returns),
    // so the target value is already set. Wait for that value to be confirmed.
    u64 targetValue = sync->GetValue();
    return sync->Wait(targetValue, timeoutMs);
}

void DawnDevice::destroySyncImpl(SyncHandle handle) {
    if (handle == handles::INVALID_SYNC) return;
    syncAllocator_.Free(static_cast<u32>(handle));
}

QueryPoolHandle DawnDevice::createQueryPoolImpl(const QueryPoolDesc& desc) {
    u32 id = queryPoolAllocator_.Allocate(*this);
    auto* pool = queryPoolAllocator_.Get(id);
    if (!pool) {
        std::cerr << "[DawnDevice] QueryPool allocation failed" << std::endl;
        queryPoolAllocator_.Free(id);
        return static_cast<QueryPoolHandle>(handles::INVALID_RESOURCE);
    }
    if (!pool->Initialize(desc)) {
        std::cerr << "[DawnDevice] QueryPool Initialize failed for id: " << id << std::endl;
        queryPoolAllocator_.Free(id);
        return static_cast<QueryPoolHandle>(handles::INVALID_RESOURCE);
    }
    return static_cast<QueryPoolHandle>(id);
}

void DawnDevice::destroyQueryPoolImpl(QueryPoolHandle handle) {
    if (handle == static_cast<QueryPoolHandle>(handles::INVALID_RESOURCE)) return;
    auto* pool = GetQueryPool(handle);
    if (pool) {
        pool->Destroy();
    }
    queryPoolAllocator_.Free(static_cast<u32>(handle));
}

bool DawnDevice::getQueryPoolResultsImpl(QueryPoolHandle handle, u32 firstQuery, u32 queryCount, void* data, size_t stride) {
    auto* pool = GetQueryPool(handle);
    if (!pool) {
        std::cerr << "[DawnDevice] getQueryPoolResults: invalid handle " << handle << std::endl;
        return false;
    }

    // WebGPU query results must be resolved via wgpuCommandEncoderResolveQuerySet into a buffer,
    // then read back via mapAsync. This synchronous API cannot directly retrieve results.
    // For now, return zeros. A full implementation would require an async resolve-and-readback pipeline.
    static bool loggedOnce = false;
    if (!loggedOnce) {
        std::cerr << "[DawnDevice] getQueryPoolResults: synchronous query readback not fully supported in WebGPU" << std::endl;
        loggedOnce = true;
    }

    // Zero out the output data as a safe default
    u8* dst = static_cast<u8*>(data);
    for (u32 i = 0; i < queryCount; ++i) {
        std::memset(dst + i * stride, 0, stride);
    }
    (void)firstQuery;
    return false;
}

double DawnDevice::getTimestampPeriodImpl() const {
    // WebGPU timestamp queries are in nanoseconds; return 1.0 as the period
    return 1.0;
}

// === Resource destruction ===

void DawnDevice::destroyBufferImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    auto* buf = GetBuffer(handle);
    if (buf) {
        ResourceManager::Instance().UnregisterResource(handle);
        buf->destroyImpl();
    }
    bufferAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyTextureImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    auto* tex = GetTexture(handle);
    if (tex) {
        ResourceManager::Instance().UnregisterResource(handle);
        tex->destroyImpl();
    }
    textureAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyShaderImpl(ShaderHandle handle) {
    if (handle == handles::INVALID_SHADER) return;
    auto* shader = GetShader(handle);
    if (shader) {
        shader->Destroy();
    }
    shaderAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyPipelineImpl(PipelineHandle handle) {
    if (handle == handles::INVALID_PIPELINE) return;
    auto* pipeline = GetPipeline(handle);
    if (pipeline) {
        pipeline->Destroy();
    }
    pipelineAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyCommandBufferImpl(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER) return;
    auto* cmdBuf = GetCommandBuffer(handle);
    if (cmdBuf) {
        cmdBuf->destroyImpl();
    }
    commandBufferAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroySamplerImpl(SamplerHandle handle) {
    if (handle == handles::INVALID_SAMPLER) return;
    auto* sampler = GetSampler(handle);
    if (sampler) {
        sampler->Destroy();
    }
    samplerAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle) {
    if (handle == static_cast<DescriptorSetLayoutHandle>(handles::INVALID_RESOURCE)) return;
    auto* layout = GetDescriptorSetLayout(handle);
    if (layout) {
        layout->Destroy();
    }
    descriptorSetLayoutAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyPipelineLayoutImpl(PipelineLayoutHandle handle) {
    if (handle == handles::INVALID_PIPELINE_LAYOUT) return;
    auto* layout = GetPipelineLayout(handle);
    if (layout) {
        layout->Destroy();
    }
    pipelineLayoutAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyDescriptorSetImpl(DescriptorSetHandle handle) {
    if (handle == static_cast<DescriptorSetHandle>(handles::INVALID_RESOURCE)) return;
    auto* set = GetDescriptorSet(handle);
    if (set) {
        set->Destroy();
    }
    descriptorSetAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::destroyRenderPassImpl(RenderPassHandle handle) {
    if (handle == static_cast<RenderPassHandle>(handles::INVALID_RESOURCE)) return;
    auto* pass = GetRenderPass(handle);
    if (pass) {
        pass->Destroy();
    }
    renderPassAllocator_.Free(static_cast<u32>(handle));
}

// === Command submission ===

void DawnDevice::FlushStagingBuffers() {
    bufferAllocator_.ForEach([](DawnBuffer& buf) {
        if (buf.HasStaging()) {
            buf.FlushStaging();
        }
    });
}

bool DawnDevice::submitImpl(const QueueSubmitInfo& info) {
    auto* cmdBuf = GetCommandBuffer(info.cmdBuffer);
    if (!cmdBuf) {
        std::cerr << "[DawnDevice] submitImpl: invalid command buffer handle " << info.cmdBuffer << std::endl;
        return false;
    }

    // Handle command buffer lifecycle: auto-begin/end if the caller didn't.
    auto state = cmdBuf->GetState();

    if (state == CommandBufferState::Recording) {
        // Currently recording — end it.
        if (!cmdBuf->End()) {
            std::cerr << "[DawnDevice] submitImpl: failed to end command buffer" << std::endl;
            return false;
        }
    } else if (state == CommandBufferState::Reset || state == CommandBufferState::Executed) {
        // Never begun — auto-begin and end for empty submissions.
        cmdBuf->Begin();
        cmdBuf->End();
    }
    // else RecordingEnded — already ready to submit.

    WGPUCommandBuffer wgpuCmdBuf = cmdBuf->GetNativeCommandBuffer();
    if (!wgpuCmdBuf) {
        // Empty command buffer (no operations recorded) — not an error.
        static bool loggedNullCb = false;
        if (!loggedNullCb) {
            std::cerr << "[DawnDevice] submitImpl: WGPU command buffer is NULL (empty?) state="
                      << static_cast<int>(state) << std::endl;
            loggedNullCb = true;
        }
        // Signal fence if requested.
        if (info.signalFence != handles::INVALID_SYNC) {
            auto* sync = GetSync(info.signalFence);
            if (sync) {
                sync->Signal(sync->GetValue() + 1);
            }
        }
        return true;
    }

    // Flush any persistently mapped staging buffers before submit.
    // ForwardRenderer maps constant buffers once at init and writes every frame.
    // The staging data must be uploaded via wgpuQueueWriteBuffer before the
    // GPU executes the commands that read those buffers.
    bufferAllocator_.ForEach([](DawnBuffer& buf) {
        if (buf.HasStaging()) {
            buf.FlushStaging();
        }
    });

    // Submit to the device queue
    wgpuQueueSubmit(wgpuQueue_, 1, &wgpuCmdBuf);

    // Wait for GPU work to complete before proceeding.
    // wgpuInstanceProcessEvents only processes completed callbacks;
    // wgpuQueueOnSubmittedWorkDone + polling ensures the GPU actually
    // finishes rendering before we present the surface texture.
    {
        struct WorkDoneData { bool done{false}; };
        WorkDoneData wd;
        WGPUQueueWorkDoneCallbackInfo cbInfo{};
        cbInfo.nextInChain = nullptr;
        cbInfo.mode = WGPUCallbackMode_AllowProcessEvents;
        cbInfo.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView message, void* userdata1, void* userdata2) {
            (void)status; (void)message; (void)userdata2;
            static_cast<WorkDoneData*>(userdata1)->done = true;
        };
        cbInfo.userdata1 = &wd;
        cbInfo.userdata2 = nullptr;
        wgpuQueueOnSubmittedWorkDone(wgpuQueue_, cbInfo);
        int poll = 0;
        while (!wd.done && poll < 10000) {
            wgpuInstanceProcessEvents(wgpuInstance_);
            ++poll;
        }
    }

    // Note: do NOT release wgpuCmdBuf here. The DawnCommandBuffer owns it
    // and will release it in destroyImpl/resetImpl.

    // Signal fence if requested
    if (info.signalFence != handles::INVALID_SYNC) {
        auto* sync = GetSync(info.signalFence);
        if (sync) {
            sync->Signal(sync->GetValue() + 1);
        }
    }

    return true;
}

// === Memory operations ===

void* DawnDevice::mapBufferImpl(ResourceHandle handle, u64 offset, u64 size) {
    auto* buf = GetBuffer(handle);
    if (!buf) {
        std::cerr << "[DawnDevice] mapBuffer: invalid handle " << handle << std::endl;
        return nullptr;
    }
    return buf->mapImpl(offset, size);
}

void DawnDevice::unmapBufferImpl(ResourceHandle handle) {
    auto* buf = GetBuffer(handle);
    if (!buf) {
        std::cerr << "[DawnDevice] unmapBuffer: invalid handle " << handle << std::endl;
        return;
    }
    buf->unmapImpl();
}

void DawnDevice::setBufferDirtySizeImpl(ResourceHandle handle, u64 size) {
    auto* buf = GetBuffer(handle);
    if (!buf) return;
    buf->SetDirtySize(size);
}

void DawnDevice::UpdateTextureData(ResourceHandle handle, const void* data,
    u32 x, u32 y, u32 z, u32 width, u32 height, u32 depth, u32 rowPitch,
    u32 mipLevel) {
    DawnTexture* tex = GetTexture(handle);
    if (!tex || !data) return;

    WGPUTexture nativeTex = tex->GetNativeTexture();
    if (!nativeTex) return;

    WGPUTexelCopyTextureInfo dest{};
    dest.texture = nativeTex;
    dest.mipLevel = mipLevel;
    dest.origin = {x, y, z};
    dest.aspect = WGPUTextureAspect_All;

    // WebGPU requires bytesPerRow to be a multiple of 256.
    constexpr u32 kAlignment = 256;
    u32 alignedPitch = (rowPitch + kAlignment - 1) & ~(kAlignment - 1);

    WGPUTexelCopyBufferLayout layout{};
    layout.offset = 0;
    layout.bytesPerRow = alignedPitch;
    layout.rowsPerImage = height;

    WGPUExtent3D writeSize{width, height, depth};

    if (alignedPitch == rowPitch) {
        size_t dataSize = (size_t)rowPitch * height * depth;
        wgpuQueueWriteTexture(wgpuQueue_, &dest, data, dataSize, &layout, &writeSize);
    } else {
        size_t dataSize = (size_t)alignedPitch * height * depth;
        std::vector<u8> padded(dataSize, 0);
        for (u32 row = 0; row < height * depth; ++row) {
            memcpy(padded.data() + row * alignedPitch,
                   static_cast<const u8*>(data) + row * rowPitch,
                   rowPitch);
        }
        wgpuQueueWriteTexture(wgpuQueue_, &dest, padded.data(), dataSize, &layout, &writeSize);
    }
}

// === Blit pipeline for mipmap generation ===

bool DawnDevice::EnsureBlitPipeline() {
    if (blitPipelineInitialized_) return blitPipeline_ != nullptr;
    blitPipelineInitialized_ = true;

    // Load blit WGSL shader
    std::string shaderPath = "Engine/Graphics/Dawn/shaders/Blit.wgsl";
    std::ifstream file(shaderPath);
    if (!file.is_open()) {
#ifndef __EMSCRIPTEN__
        file.open("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + shaderPath);
#endif
    }
    if (!file.is_open()) {
        std::cerr << "[DawnDevice] Blit shader not found: " << shaderPath << std::endl;
        return false;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    WGPUShaderSourceWGSL wgslDesc{};
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWGPUStringView(source.c_str());

    WGPUShaderModuleDescriptor moduleDesc{};
    moduleDesc.nextInChain = &wgslDesc.chain;
    moduleDesc.label = ToWGPUStringView("BlitShader");

    WGPUShaderModule module = wgpuDeviceCreateShaderModule(wgpuDevice_, &moduleDesc);
    if (!module) {
        std::cerr << "[DawnDevice] Failed to compile blit shader" << std::endl;
        return false;
    }

    // Bind group layout: src texture + sampler
    WGPUBindGroupLayoutEntry bgEntries[2]{};
    bgEntries[0].nextInChain = nullptr;
    bgEntries[0].binding = 0;
    bgEntries[0].visibility = WGPUShaderStage_Fragment;
    bgEntries[0].texture.sampleType = WGPUTextureSampleType_Float;
    bgEntries[0].texture.viewDimension = WGPUTextureViewDimension_2D;

    bgEntries[1].nextInChain = nullptr;
    bgEntries[1].binding = 1;
    bgEntries[1].visibility = WGPUShaderStage_Fragment;
    bgEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    WGPUBindGroupLayoutDescriptor bglDesc{};
    bglDesc.nextInChain = nullptr;
    bglDesc.entryCount = 2;
    bglDesc.entries = bgEntries;
    blitBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(wgpuDevice_, &bglDesc);

    WGPUPipelineLayoutDescriptor plDesc{};
    plDesc.nextInChain = nullptr;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &blitBindGroupLayout_;
    WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(wgpuDevice_, &plDesc);

    // Vertex state
    WGPUVertexState vertexState{};
    vertexState.nextInChain = nullptr;
    vertexState.module = module;
    vertexState.entryPoint = ToWGPUStringView("blit_vs");
    vertexState.bufferCount = 0;

    // Fragment state
    WGPUColorTargetState colorTarget{};
    colorTarget.nextInChain = nullptr;
    colorTarget.format = WGPUTextureFormat_RGBA8Unorm; // Will be overridden per-texture
    colorTarget.blend = nullptr;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState{};
    fragmentState.nextInChain = nullptr;
    fragmentState.module = module;
    fragmentState.entryPoint = ToWGPUStringView("blit_fs");
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPURenderPipelineDescriptor rpDesc{};
    rpDesc.nextInChain = nullptr;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex = vertexState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.fragment = &fragmentState;
    rpDesc.multisample.count = 1;

    blitPipeline_ = wgpuDeviceCreateRenderPipeline(wgpuDevice_, &rpDesc);

    wgpuPipelineLayoutRelease(pipelineLayout);
    wgpuShaderModuleRelease(module);

    // Create linear sampler
    WGPUSamplerDescriptor samplerDesc{};
    samplerDesc.nextInChain = nullptr;
    samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    samplerDesc.magFilter = WGPUFilterMode_Linear;
    samplerDesc.minFilter = WGPUFilterMode_Linear;
    samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    blitSampler_ = wgpuDeviceCreateSampler(wgpuDevice_, &samplerDesc);

    return blitPipeline_ != nullptr;
}

void DawnDevice::DestroyBlitPipeline() {
    if (blitSampler_) { wgpuSamplerRelease(blitSampler_); blitSampler_ = nullptr; }
    if (blitPipeline_) { wgpuRenderPipelineRelease(blitPipeline_); blitPipeline_ = nullptr; }
    if (blitBindGroupLayout_) { wgpuBindGroupLayoutRelease(blitBindGroupLayout_); blitBindGroupLayout_ = nullptr; }
    blitPipelineInitialized_ = false;
}

void DawnCommandBuffer::GenerateMipmaps(ResourceHandle texture) {
    DawnTexture* tex = device_.GetTexture(texture);
    if (!tex || !tex->GetNativeTexture()) return;

    const TextureDesc& desc = tex->GetTextureDesc();
    if (desc.mipLevels <= 1) return;

    if (!device_.EnsureBlitPipeline()) {
        std::cerr << "[DawnCommandBuffer] GenerateMipmaps: blit pipeline not available" << std::endl;
        return;
    }

    EnsureCommandEncoder();
    if (!wgpuEncoder_) return;

    WGPUTexture wgpuTex = tex->GetNativeTexture();
    WGPUTextureFormat texFormat = ToWGPUTextureFormat(desc.format);

    u32 width = desc.size.x;
    u32 height = desc.size.y;

    for (u32 mip = 0; mip < desc.mipLevels - 1; ++mip) {
        u32 nextWidth = std::max(1u, width / 2);
        u32 nextHeight = std::max(1u, height / 2);

        // Create source texture view for mip level N
        WGPUTextureViewDescriptor srcViewDesc{};
        srcViewDesc.nextInChain = nullptr;
        srcViewDesc.format = texFormat;
        srcViewDesc.dimension = WGPUTextureViewDimension_2D;
        srcViewDesc.baseMipLevel = mip;
        srcViewDesc.mipLevelCount = 1;
        srcViewDesc.baseArrayLayer = 0;
        srcViewDesc.arrayLayerCount = 1;
        srcViewDesc.aspect = WGPUTextureAspect_All;
        WGPUTextureView srcView = wgpuTextureCreateView(wgpuTex, &srcViewDesc);

        // Create destination texture view for mip level N+1
        WGPUTextureViewDescriptor dstViewDesc{};
        dstViewDesc.format = texFormat;
        dstViewDesc.dimension = WGPUTextureViewDimension_2D;
        dstViewDesc.baseMipLevel = mip + 1;
        dstViewDesc.mipLevelCount = 1;
        dstViewDesc.baseArrayLayer = 0;
        dstViewDesc.arrayLayerCount = 1;
        dstViewDesc.aspect = WGPUTextureAspect_All;
        WGPUTextureView dstView = wgpuTextureCreateView(wgpuTex, &dstViewDesc);

        // Create bind group: src texture + sampler
        WGPUBindGroupEntry bgEntries[2]{};
        bgEntries[0].nextInChain = nullptr;
        bgEntries[0].binding = 0;
        bgEntries[0].textureView = srcView;
        bgEntries[1].nextInChain = nullptr;
        bgEntries[1].binding = 1;
        bgEntries[1].sampler = device_.blitSampler_;

        WGPUBindGroupDescriptor bgDesc{};
        bgDesc.nextInChain = nullptr;
        bgDesc.layout = device_.blitBindGroupLayout_;
        bgDesc.entryCount = 2;
        bgDesc.entries = bgEntries;
        WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(device_.GetNativeDevice(), &bgDesc);

        // End any current encoder to start render pass
        EndCurrentEncoder();

        // Render pass: render to mip N+1
        WGPURenderPassColorAttachment colorAttachment{};
        colorAttachment.nextInChain = nullptr;
        colorAttachment.view = dstView;
        colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        colorAttachment.resolveTarget = nullptr;
        colorAttachment.loadOp = WGPULoadOp_Clear;
        colorAttachment.storeOp = WGPUStoreOp_Store;

        WGPURenderPassDescriptor rpDesc{};
        rpDesc.nextInChain = nullptr;
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAttachment;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(wgpuEncoder_, &rpDesc);
        wgpuRenderPassEncoderSetPipeline(pass, device_.blitPipeline_);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);

        wgpuBindGroupRelease(bindGroup);
        wgpuTextureViewRelease(srcView);
        wgpuTextureViewRelease(dstView);

        width = nextWidth;
        height = nextHeight;
    }

    UpdateStats(CommandType::GenerateMipmaps);
}

// === SwapChain surface texture wrapping ===

ResourceHandle DawnDevice::WrapSurfaceTexture(WGPUTexture texture, u32 width, u32 height, DataFormat format) {
    if (!texture) return handles::INVALID_RESOURCE;

    TextureDesc desc{};
    desc.type = TextureType::Texture2D;
    desc.format = format;
    desc.size = {width, height, 1};
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.usage = TextureUsage::RenderTarget;
    desc.name = "SwapChainSurface";

    u32 id = textureAllocator_.Allocate(*this, desc);
    auto* tex = textureAllocator_.Get(id);
    if (!tex) {
        textureAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }

    // Override the DawnTexture's native handles with the surface texture.
    // We skip normal Initialize() and manually set the state + native objects.
    tex->SetState(ResourceState::Ready);

    // Create a view for the surface texture
    WGPUTextureViewDescriptor viewDesc{};
    viewDesc.nextInChain = nullptr;
    viewDesc.label = ToWGPUStringView("SwapChainView");
    viewDesc.format = ToWGPUTextureFormat(format);
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;

    // We need to set the native texture on the DawnTexture.
    // Since DawnTexture's wgpuTexture_ is private, we add a setter.
    tex->OverrideNativeTexture(texture, wgpuTextureCreateView(texture, &viewDesc));

    return static_cast<ResourceHandle>(id);
}

void DawnDevice::ReleaseSurfaceTexture(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    // Release the texture view we created, but not the surface-owned texture.
    auto* tex = textureAllocator_.Get(static_cast<u32>(handle));
    if (tex) {
        tex->ReleaseWrappedHandles();
    }
    textureAllocator_.Free(static_cast<u32>(handle));
}

void DawnDevice::UpdateSurfaceTexture(ResourceHandle handle, WGPUTexture texture, u32 width, u32 height, DataFormat format) {
    if (handle == handles::INVALID_RESOURCE || !texture) return;
    auto* tex = textureAllocator_.Get(static_cast<u32>(handle));
    if (!tex) return;

    // Release the old texture view (not the surface-owned texture itself)
    tex->ReleaseWrappedHandles();

    // Create a new view for the new surface texture
    WGPUTextureViewDescriptor viewDesc{};
    viewDesc.nextInChain = nullptr;
    viewDesc.label = ToWGPUStringView("SwapChainView");
    viewDesc.format = ToWGPUTextureFormat(format);
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;

    tex->OverrideNativeTexture(texture, wgpuTextureCreateView(texture, &viewDesc));
    tex->SetState(ResourceState::Ready);
    (void)width;
    (void)height;
}

// === Push constant ring buffer ===

u32 DawnDevice::AllocatePushConstantSlot() {
    u32 oldOffset = pushConstantOffset_.load();
    u32 newOffset;
    do {
        newOffset = oldOffset + PUSH_CONSTANT_ALIGNMENT;
        // Wrap around if exceeding ring buffer size
        if (newOffset >= PUSH_CONSTANT_RING_SIZE) {
            newOffset = 0;
        }
        // Avoid allocating slot 0 if it would collide with a wrapped allocation
    } while (!pushConstantOffset_.compare_exchange_weak(oldOffset, newOffset));

    return oldOffset;
}

void DawnDevice::ResetPushConstantOffset() {
    pushConstantOffset_.store(0);
}

// === Resource accessors ===

DawnBuffer* DawnDevice::GetBuffer(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return nullptr;
    return bufferAllocator_.Get(static_cast<u32>(handle));
}

DawnTexture* DawnDevice::GetTexture(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return nullptr;
    return textureAllocator_.Get(static_cast<u32>(handle));
}

DawnCommandBuffer* DawnDevice::GetCommandBuffer(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER) return nullptr;
    return commandBufferAllocator_.Get(static_cast<u32>(handle));
}

DawnSync* DawnDevice::GetSync(SyncHandle handle) {
    if (handle == handles::INVALID_SYNC) return nullptr;
    return syncAllocator_.Get(static_cast<u32>(handle));
}

DawnQueryPool* DawnDevice::GetQueryPool(QueryPoolHandle handle) {
    if (handle == static_cast<QueryPoolHandle>(handles::INVALID_RESOURCE)) return nullptr;
    return queryPoolAllocator_.Get(static_cast<u32>(handle));
}

DawnShader* DawnDevice::GetShader(ShaderHandle handle) {
    if (handle == handles::INVALID_SHADER) return nullptr;
    return shaderAllocator_.Get(static_cast<u32>(handle));
}

DawnPipeline* DawnDevice::GetPipeline(PipelineHandle handle) {
    if (handle == handles::INVALID_PIPELINE) return nullptr;
    return pipelineAllocator_.Get(static_cast<u32>(handle));
}

DawnSampler* DawnDevice::GetSampler(SamplerHandle handle) {
    if (handle == handles::INVALID_SAMPLER) return nullptr;
    return samplerAllocator_.Get(static_cast<u32>(handle));
}

DawnDescriptorSetLayout* DawnDevice::GetDescriptorSetLayout(DescriptorSetLayoutHandle handle) {
    if (handle == static_cast<DescriptorSetLayoutHandle>(handles::INVALID_RESOURCE)) return nullptr;
    return descriptorSetLayoutAllocator_.Get(static_cast<u32>(handle));
}

DawnPipelineLayout* DawnDevice::GetPipelineLayout(PipelineLayoutHandle handle) {
    if (handle == handles::INVALID_PIPELINE_LAYOUT) return nullptr;
    return pipelineLayoutAllocator_.Get(static_cast<u32>(handle));
}

DawnDescriptorSet* DawnDevice::GetDescriptorSet(DescriptorSetHandle handle) {
    if (handle == static_cast<DescriptorSetHandle>(handles::INVALID_RESOURCE)) return nullptr;
    return descriptorSetAllocator_.Get(static_cast<u32>(handle));
}

DawnRenderPass* DawnDevice::GetRenderPass(RenderPassHandle handle) {
    if (handle == static_cast<RenderPassHandle>(handles::INVALID_RESOURCE)) return nullptr;
    return renderPassAllocator_.Get(static_cast<u32>(handle));
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
