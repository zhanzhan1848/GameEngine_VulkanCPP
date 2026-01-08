/**
 * @file MetalSampler.cpp
 * @brief Metal采样器实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalSampler.h"
#include "MetalDevice.h"

namespace primal::graphics::rhi {

namespace {
    MTL::SamplerMinMagFilter ToMTLMinMagFilter(FilterMode mode) {
        switch (mode) {
            case FilterMode::Point: return MTL::SamplerMinMagFilterNearest;
            case FilterMode::Linear: return MTL::SamplerMinMagFilterLinear;
            default: return MTL::SamplerMinMagFilterNearest;
        }
    }

    MTL::SamplerMipFilter ToMTLMipFilter(FilterMode mode) {
        switch (mode) {
            case FilterMode::Point: return MTL::SamplerMipFilterNearest;
            case FilterMode::Linear: return MTL::SamplerMipFilterLinear;
            default: return MTL::SamplerMipFilterNotMipmapped;
        }
    }

    MTL::SamplerAddressMode ToMTLAddressMode(TextureAddressMode mode) {
        switch (mode) {
            case TextureAddressMode::Wrap: return MTL::SamplerAddressModeRepeat;
            case TextureAddressMode::Mirror: return MTL::SamplerAddressModeMirrorRepeat;
            case TextureAddressMode::Clamp: return MTL::SamplerAddressModeClampToEdge;
            case TextureAddressMode::Border: return MTL::SamplerAddressModeClampToBorderColor;
            case TextureAddressMode::MirrorOnce: return MTL::SamplerAddressModeMirrorClampToEdge;
            default: return MTL::SamplerAddressModeRepeat;
        }
    }

    MTL::CompareFunction ToMTLCompareFunction(ComparisonFunc func) {
        switch (func) {
            case ComparisonFunc::Never: return MTL::CompareFunctionNever;
            case ComparisonFunc::Less: return MTL::CompareFunctionLess;
            case ComparisonFunc::Equal: return MTL::CompareFunctionEqual;
            case ComparisonFunc::LessEqual: return MTL::CompareFunctionLessEqual;
            case ComparisonFunc::Greater: return MTL::CompareFunctionGreater;
            case ComparisonFunc::NotEqual: return MTL::CompareFunctionNotEqual;
            case ComparisonFunc::GreaterEqual: return MTL::CompareFunctionGreaterEqual;
            case ComparisonFunc::Always: return MTL::CompareFunctionAlways;
            default: return MTL::CompareFunctionAlways;
        }
    }
}

MetalSampler::MetalSampler(MetalDevice& device) : device_(device) {}

MetalSampler::~MetalSampler() {
    Destroy();
}

void MetalSampler::Destroy() {
    if (samplerState_) {
        samplerState_->release();
        samplerState_ = nullptr;
    }
}

bool MetalSampler::Initialize(const SamplerDesc& desc) {
    MTL::SamplerDescriptor* samplerDesc = MTL::SamplerDescriptor::alloc()->init();

    samplerDesc->setMinFilter(ToMTLMinMagFilter(desc.minFilter));
    samplerDesc->setMagFilter(ToMTLMinMagFilter(desc.magFilter));
    samplerDesc->setMipFilter(ToMTLMipFilter(desc.mipFilter));
    
    samplerDesc->setSAddressMode(ToMTLAddressMode(desc.addressU));
    samplerDesc->setTAddressMode(ToMTLAddressMode(desc.addressV));
    samplerDesc->setRAddressMode(ToMTLAddressMode(desc.addressW));
    
    samplerDesc->setMaxAnisotropy(desc.maxAnisotropy);
    samplerDesc->setCompareFunction(ToMTLCompareFunction(desc.comparisonFunc));
    
    // Lod bias and range
    // samplerDesc->setLodMinClamp(desc.minLod); // Not directly supported in basic descriptor?
    // samplerDesc->setLodMaxClamp(desc.maxLod);

    samplerState_ = device_.GetNativeDevice()->newSamplerState(samplerDesc);
    samplerDesc->release();

    return samplerState_ != nullptr;
}

} // namespace primal::graphics::rhi
