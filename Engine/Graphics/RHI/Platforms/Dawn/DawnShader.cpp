#include "DawnShader.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include <iostream>
#include <cstring>

namespace primal::graphics::rhi {

DawnShader::DawnShader(DawnDevice& device)
    : device_(device) {
}

DawnShader::~DawnShader() {
    Destroy();
}

bool DawnShader::Initialize(const void* data, size_t size, ShaderStage stage, const char* entryPoint) {
    if (!data || size == 0 || !entryPoint) {
        std::cerr << "[DawnShader] Invalid initialization parameters" << std::endl;
        return false;
    }

    stage_ = stage;
    entryPoint_ = entryPoint;

    // Treat input data as WGSL source text (null-terminated string)
    const char* wgslSource = static_cast<const char*>(data);

    WGPUShaderSourceWGSL wgslDesc{};
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWGPUStringView(wgslSource);

    WGPUShaderModuleDescriptor moduleDesc{};
    moduleDesc.nextInChain = &wgslDesc.chain;
    moduleDesc.label = ToWGPUStringView("DawnShader");

    wgpuModule_ = wgpuDeviceCreateShaderModule(device_.GetNativeDevice(), &moduleDesc);
    if (!wgpuModule_) {
        std::cerr << "[DawnShader] Failed to create shader module" << std::endl;
        return false;
    }

    // Check for compilation errors via the compilationInfo callback
    struct CompilationUserData {
        bool hasErrors{false};
        bool done{false};
    };

    // Heap-allocate the userdata: Dawn's WASM EventManager holds the pointer
    // after wgpuShaderModuleGetCompilationInfo and may fire the callback
    // during a future wgpuInstanceProcessEvents call (e.g. endFrame's),
    // long after Initialize() has returned. Stack-local userdata would be
    // dead by then and cause the OOB we saw in EventManager::ProcessEvents.
    CompilationUserData* compData = new CompilationUserData{};

    WGPUCompilationInfoCallbackInfo callbackInfo{};
    callbackInfo.nextInChain = nullptr;
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = [](WGPUCompilationInfoRequestStatus status, struct WGPUCompilationInfo const * info, WGPU_NULLABLE void* userdata1, WGPU_NULLABLE void* userdata2) {
        auto* data = static_cast<CompilationUserData*>(userdata1);
        (void)userdata2;
        if (status == WGPUCompilationInfoRequestStatus_Success && info) {
            for (u32 i = 0; i < info->messageCount; ++i) {
                const WGPUCompilationMessage& msg = info->messages[i];
                std::string messageStr;
                if (msg.message.data) {
                    messageStr = std::string(msg.message.data, msg.message.length == WGPU_STRLEN ? std::strlen(msg.message.data) : msg.message.length);
                } else {
                    messageStr = "unknown";
                }
                if (msg.type == WGPUCompilationMessageType_Error) {
                    std::cerr << "[DawnShader] Compilation error: "
                              << messageStr
                              << " at line " << msg.lineNum << std::endl;
                    data->hasErrors = true;
                } else if (msg.type == WGPUCompilationMessageType_Warning) {
                    std::cerr << "[DawnShader] Compilation warning: "
                              << messageStr
                              << " at line " << msg.lineNum << std::endl;
                }
            }
        }
        data->done = true;
    };
    callbackInfo.userdata1 = compData;
    callbackInfo.userdata2 = nullptr;

    wgpuShaderModuleGetCompilationInfo(wgpuModule_, callbackInfo);

    // Pump instance events until the compilation-info callback fires.
    // Previously this was skipped on WASM ("may never deliver"), but that
    // caused WGSL compile errors to be silently swallowed — invalid shader
    // modules slipped through, leading to "Invalid ComputePipeline" errors
    // at dispatch time and OOB crashes in EventManager::ProcessEvents.
    // Cap iterations on WASM as a safety net if the callback somehow stalls.
#ifdef __EMSCRIPTEN__
    for (u32 iter = 0; iter < 1000 && !compData->done; ++iter) {
        wgpuInstanceProcessEvents(device_.GetInstance());
    }
#else
    while (!compData->done) {
        wgpuInstanceProcessEvents(device_.GetInstance());
    }
#endif

    bool hasErrors = compData->hasErrors;

#ifdef __EMSCRIPTEN__
    // If the callback fired during the pump loop, free normally. If it
    // didn't (timed out at 1000 iterations), intentionally leak: Dawn
    // may still fire it later and dereference the pointer.
    if (compData->done) {
        delete compData;
    }
#else
    // Native: pump loop guarantees delivery, safe to always free.
    delete compData;
#endif

    if (hasErrors) {
        std::cerr << "[DawnShader] Shader has compilation errors" << std::endl;
        wgpuShaderModuleRelease(wgpuModule_);
        wgpuModule_ = nullptr;
        return false;
    }

    return true;
}

void DawnShader::Destroy() {
    if (wgpuModule_) {
        wgpuShaderModuleRelease(wgpuModule_);
        wgpuModule_ = nullptr;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
