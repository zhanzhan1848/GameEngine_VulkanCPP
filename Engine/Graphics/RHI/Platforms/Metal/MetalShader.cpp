/**
 * @file MetalShader.cpp
 * @brief Metal着色器实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalShader.h"
#include "MetalDevice.h"
#include "Engine/Common/CoreDiag.h"
#include <iostream>
#include <cstring>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi {

MetalShader::MetalShader(MetalDevice& device, const void* data, size_t size, ShaderStage stage, const char* entryPoint)
    : device_(device), data_(data), size_(size), stage_(stage), entryPoint_(entryPoint) {}

MetalShader::~MetalShader() {
    Destroy();
}

bool MetalShader::Initialize() {
    if (!data_ || size_ == 0) return false;

    NS::Error* error = nullptr;

    // Detect binary input: a metallib starts with the "MTLB" magic at
    // offset 0. If the data is binary, the source-compile fallback
    // below would fail too (compiling metallib bytes as if they were
    // .metal source text produces a null library silently). Detect
    // binary up front and skip the source fallback to avoid the wasted
    // work + a confusing "Compiling from source..." log line.
    const bool is_binary = (size_ >= 4 && std::memcmp(data_, "MTLB", 4) == 0);

    // Create dispatch data to hold the source/binary
    dispatch_data_t dispatchData = dispatch_data_create(data_, size_, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);

    // Try to create library from data (assuming binary/metallib)
    library_ = device_.GetNativeDevice()->newLibrary(dispatchData, &error);

    if (dispatchData) {
        dispatch_release(dispatchData);
    }

    // If binary creation failed, try as source code (only for non-binary data).
    // Binary input is already a valid metallib; if newLibrary rejected it,
    // it's a different problem (corrupt, wrong version, GPU-incompatible) and
    // the source fallback would only make things worse.
    if (!library_ && !is_binary) {
        if (error) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Failed to create library from binary: "
                      << error->localizedDescription()->utf8String() << std::endl;
            // Error is autoreleased, do not release manually
            error = nullptr;
        }

        PRIMAL_LOG_STREAM(Info, "RHI") << "[MetalShader] Compiling from source... (size: " << size_ << " bytes)" << std::endl;
        std::string sourceStr(static_cast<const char*>(data_), size_);
        NS::String* source = NS::String::alloc()->init(sourceStr.c_str(), NS::UTF8StringEncoding);
        if (!source) {
             PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Failed to create NS::String from source data (UTF8). Trying ASCII..." << std::endl;
             source = NS::String::alloc()->init(sourceStr.c_str(), NS::ASCIIStringEncoding);
        }
        if (!source) {
             PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Failed to create NS::String from source data (ASCII). Trying MacOSRoman..." << std::endl;
             source = NS::String::alloc()->init(sourceStr.c_str(), NS::MacOSRomanStringEncoding);
        }
        if (!source) {
             PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Failed to create NS::String from source data (All encodings failed)." << std::endl;
             return false;
        }
        MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
        // 2026-09-04 闪烁修复：禁用 fast-math。Metal 默认 fastMathEnabled=YES
        // 会导致 VSM Chebyshev 方差计算（E[x²]-E[x]²）精度不稳定 → 阴影
        // visibility 在亮暗边界高频翻转。MoltenVK 编译 SPIR-V 时不开
        // fast-math，所以 Vulkan 路径没有此问题。
        options->setFastMathEnabled(false);
        library_ = device_.GetNativeDevice()->newLibrary(source, options, &error);
        source->release();
        options->release();
    } else if (!library_ && is_binary) {
        // Binary metallib failed to load. The source-compile fallback
        // would only waste time on binary input. Log the error and let
        // the function return false with library_ = nullptr.
        if (error) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Failed to load metallib (size=" << size_
                      << "): " << error->localizedDescription()->utf8String() << std::endl;
            // Error is autoreleased, do not release manually
            error = nullptr;
        } else {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Failed to load metallib (size=" << size_ << ")" << std::endl;
        }
    }

    if (!library_) {
        // 着色器编译失败重定向进 diag 通道（CLI 规划 §11.3 轨道 B）：
        // 源码 dump 只留 stderr（太长且低频），结构化错误进黑匣子/宿主面板。
        std::string diagMsg = std::string("[MetalShader] Library creation failed: ")
            + (error ? error->localizedDescription()->utf8String() : "unknown error (error object is null)");
        primal::diag::Emit(primal::diag::kLevelError, "RHI", diagMsg.c_str());
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Library creation failed." << std::endl;
        if (error) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Error: " 
                      << error->localizedDescription()->utf8String() << std::endl;
            
            // Print source code for debugging
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Source Code (First 200 lines):" << std::endl;
            const char* sourceStr = static_cast<const char*>(data_);
            std::string sourceString(sourceStr, std::min(size_, (size_t)10000)); // Limit output
            
            int lineNum = 1;
            size_t start = 0;
            size_t end = sourceString.find('\n');
            while (end != std::string::npos && lineNum <= 200) {
                PRIMAL_LOG_STREAM(Error, "RHI") << lineNum << ": " << sourceString.substr(start, end - start) << std::endl;
                start = end + 1;
                end = sourceString.find('\n', start);
                lineNum++;
            }
            if (lineNum <= 200) {
                PRIMAL_LOG_STREAM(Error, "RHI") << lineNum << ": " << sourceString.substr(start) << std::endl;
            }
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] End of Source Code" << std::endl;

        } else {
             PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Unknown error (error object is null)" << std::endl;
        }
        return false;
    }

    // Get entry point function
    // Short NSStrings may be tagged pointers. The autoreleased factory avoids
    // metal-cpp's derived-to-base release() upcast, which UBSan diagnoses as
    // misaligned for those valid Objective-C pointers. The caller owns the pool.
    NS::String* nsEntryPoint = NS::String::string(entryPoint_.c_str(), NS::UTF8StringEncoding);
    if (!nsEntryPoint) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Invalid UTF-8 entry point" << std::endl;
        return false;
    }
    function_ = library_->newFunction(nsEntryPoint);
    
    if (!function_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Failed to find entry point: " << entryPoint_ << std::endl;
        
        // List all function names in the library for debugging
        NS::Array* functionNames = library_->functionNames();
        if (functionNames) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalShader] Available functions: ";
            for (int i = 0; i < functionNames->count(); ++i) {
                NS::String* name = reinterpret_cast<NS::String*>(functionNames->object(i));
                PRIMAL_LOG_STREAM(Error, "RHI") << name->utf8String() << " ";
            }
            PRIMAL_LOG_STREAM(Error, "RHI") << std::endl;
        }
        return false;
    }

    return true;
}

bool MetalShader::Reload(const void* data, size_t size) {
    if (!data || size == 0) return false;
    
    // 销毁旧资源
    Destroy();
    
    // 更新数据指针
    data_ = data;
    size_ = size;
    
    // 重新初始化
    return Initialize();
}

void MetalShader::Destroy() {
    if (function_) {
        function_->release();
        function_ = nullptr;
    }
    if (library_) {
        library_->release();
        library_ = nullptr;
    }
}

} // namespace primal::graphics::rhi
