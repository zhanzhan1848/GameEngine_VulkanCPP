/**
 * @file MetalShader.cpp
 * @brief Metal着色器实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalShader.h"
#include "MetalDevice.h"
#include <iostream>

namespace primal::graphics::rhi {

MetalShader::MetalShader(MetalDevice& device, const void* data, size_t size, ShaderStage stage, const char* entryPoint)
    : device_(device), data_(data), size_(size), stage_(stage), entryPoint_(entryPoint) {}

MetalShader::~MetalShader() {
    Destroy();
}

bool MetalShader::Initialize() {
    if (!data_ || size_ == 0) return false;

    NS::Error* error = nullptr;
    
    // Create dispatch data to hold the source/binary
    dispatch_data_t dispatchData = dispatch_data_create(data_, size_, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    
    // Try to create library from data (assuming binary/metallib)
    library_ = device_.GetNativeDevice()->newLibrary(dispatchData, &error);

    if (dispatchData) {
        dispatch_release(dispatchData);
    }

    // If binary creation failed, try as source code
    if (!library_) {
        if (error) {
            // std::cerr << "[MetalShader] Failed to create library from binary: " 
            //           << error->localizedDescription()->utf8String() << std::endl;
            // Error is autoreleased, do not release manually
            error = nullptr;
        }

        // std::cout << "[MetalShader] Compiling from source..." << std::endl;
        NS::String* source = NS::String::alloc()->init(static_cast<const char*>(data_), NS::UTF8StringEncoding);
        if (!source) {
             std::cerr << "[MetalShader] Failed to create NS::String from source data (UTF8). Trying ASCII..." << std::endl;
             source = NS::String::alloc()->init(static_cast<const char*>(data_), NS::ASCIIStringEncoding);
        }
        if (!source) {
             std::cerr << "[MetalShader] Failed to create NS::String from source data (ASCII). Trying MacOSRoman..." << std::endl;
             source = NS::String::alloc()->init(static_cast<const char*>(data_), NS::MacOSRomanStringEncoding);
        }
        if (!source) {
             std::cerr << "[MetalShader] Failed to create NS::String from source data (All encodings failed)." << std::endl;
             return false;
        }
        MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
        library_ = device_.GetNativeDevice()->newLibrary(source, options, &error);
        source->release();
        options->release();
    }

    if (!library_) {
        std::cerr << "[MetalShader] Library creation failed." << std::endl;
        if (error) {
            std::cerr << "[MetalShader] Error: " 
                      << error->localizedDescription()->utf8String() << std::endl;
        } else {
             std::cerr << "[MetalShader] Unknown error (error object is null)" << std::endl;
        }
        return false;
    }

    // Get entry point function
    NS::String* nsEntryPoint = NS::String::alloc()->init(entryPoint_.c_str(), NS::UTF8StringEncoding);
    function_ = library_->newFunction(nsEntryPoint);
    nsEntryPoint->release();
    
    if (!function_) {
        std::cerr << "[MetalShader] Failed to find entry point: " << entryPoint_ << std::endl;
        
        // List all function names in the library for debugging
        NS::Array* functionNames = library_->functionNames();
        if (functionNames) {
            std::cerr << "[MetalShader] Available functions: ";
            for (int i = 0; i < functionNames->count(); ++i) {
                NS::String* name = reinterpret_cast<NS::String*>(functionNames->object(i));
                std::cerr << name->utf8String() << " ";
            }
            std::cerr << std::endl;
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
