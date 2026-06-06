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

    CompilationUserData compData{};
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
    callbackInfo.userdata1 = &compData;
    callbackInfo.userdata2 = nullptr;

    wgpuShaderModuleGetCompilationInfo(wgpuModule_, callbackInfo);

#ifdef __EMSCRIPTEN__
    // On Emscripten + JSPI, wgpuInstanceProcessEvents may never deliver the
    // compilation-info callback. Skip the polling loop — errors will surface
    // later as pipeline-creation failures or device errors via the uncaptured
    // error callback.
    compData.done = true;
#else
    // Pump instance events to ensure compilation info callback is processed
    while (!compData.done) {
        wgpuInstanceProcessEvents(device_.GetInstance());
    }
#endif

    if (compData.hasErrors) {
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
