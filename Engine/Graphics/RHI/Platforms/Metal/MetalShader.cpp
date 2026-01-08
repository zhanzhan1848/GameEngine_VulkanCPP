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
            error->release();
            error = nullptr;
        }

        NS::String* source = NS::String::string(static_cast<const char*>(data_), NS::UTF8StringEncoding);
        MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
        library_ = device_.GetNativeDevice()->newLibrary(source, options, &error);
        options->release();
    }

    if (!library_) {
        if (error) {
            std::cerr << "[MetalShader] Failed to create library: " 
                      << error->localizedDescription()->utf8String() << std::endl;
            error->release();
        }
        return false;
    }

    // Get entry point function
    NS::String* nsEntryPoint = NS::String::string(entryPoint_.c_str(), NS::UTF8StringEncoding);
    function_ = library_->newFunction(nsEntryPoint);
    
    if (!function_) {
        std::cerr << "[MetalShader] Failed to find entry point: " << entryPoint_ << std::endl;
        return false;
    }

    return true;
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
