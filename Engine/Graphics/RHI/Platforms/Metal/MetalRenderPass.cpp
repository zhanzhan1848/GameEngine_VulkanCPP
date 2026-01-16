#include "MetalRenderPass.h"
#include "MetalDevice.h"
#include "MetalTexture.h"

namespace primal::graphics::rhi {

MetalRenderPass::MetalRenderPass(MetalDevice& device, const RenderPassDesc& desc)
    : RHIRenderPass(device, desc) {
    cachedDepthAttachment_ = {handles::INVALID_RESOURCE, nullptr};
    cachedStencilAttachment_ = {handles::INVALID_RESOURCE, nullptr};
}

MetalRenderPass::~MetalRenderPass() {
    Destroy();
}

bool MetalRenderPass::Initialize() {
    buildDescriptor();
    return mtlPassDesc_ != nullptr;
}

void MetalRenderPass::destroyImpl() {
    if (mtlPassDesc_) {
        mtlPassDesc_->release();
        mtlPassDesc_ = nullptr;
    }
    cachedColorAttachments_.clear();
    cachedDepthAttachment_ = {handles::INVALID_RESOURCE, nullptr};
    cachedStencilAttachment_ = {handles::INVALID_RESOURCE, nullptr};
}

bool MetalRenderPass::isDirty() const {
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);

    // 检查颜色附件
    if (cachedColorAttachments_.size() != desc_.colorAttachments.size()) return true;
    
    for (size_t i = 0; i < cachedColorAttachments_.size(); ++i) {
        const auto& cached = cachedColorAttachments_[i];
        if (cached.handle != handles::INVALID_RESOURCE) {
            MetalTexture* texture = metalDevice.GetTexture(cached.handle);
            if (!texture || texture->GetNativeTexture() != cached.nativeTexture) {
                return true;
            }
        }
    }

    // 检查深度附件
    if (cachedDepthAttachment_.handle != handles::INVALID_RESOURCE) {
        MetalTexture* texture = metalDevice.GetTexture(cachedDepthAttachment_.handle);
        if (!texture || texture->GetNativeTexture() != cachedDepthAttachment_.nativeTexture) {
            return true;
        }
    }

    // 检查模板附件
    if (cachedStencilAttachment_.handle != handles::INVALID_RESOURCE) {
        MetalTexture* texture = metalDevice.GetTexture(cachedStencilAttachment_.handle);
        if (!texture || texture->GetNativeTexture() != cachedStencilAttachment_.nativeTexture) {
            return true;
        }
    }

    return false;
}

void MetalRenderPass::buildDescriptor() {
    if (mtlPassDesc_) {
        mtlPassDesc_->release();
        mtlPassDesc_ = nullptr;
    }

    mtlPassDesc_ = MTL::RenderPassDescriptor::alloc()->init();
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    
    cachedColorAttachments_.clear();
    cachedColorAttachments_.reserve(desc_.colorAttachments.size());

    // 设置颜色附件
    for (size_t i = 0; i < desc_.colorAttachments.size(); ++i) {
        const auto& attachment = desc_.colorAttachments[i];
        MTL::Texture* nativeTex = nullptr;
        
        if (attachment.texture != handles::INVALID_RESOURCE) {
            MetalTexture* texture = metalDevice.GetTexture(attachment.texture);
            if (texture && texture->GetNativeTexture()) {
                nativeTex = texture->GetNativeTexture();
                
                MTL::RenderPassColorAttachmentDescriptor* ca = mtlPassDesc_->colorAttachments()->object(i);
                ca->setTexture(nativeTex);
                ca->setSlice(attachment.arrayLayer);
                ca->setLevel(attachment.mipLevel);
                
                MTL::LoadAction metalLoadAction = MTL::LoadActionDontCare;
                switch (attachment.loadOp) {
                    case LoadAction::Load: metalLoadAction = MTL::LoadActionLoad; break;
                    case LoadAction::Clear: metalLoadAction = MTL::LoadActionClear; break;
                    case LoadAction::DontCare: metalLoadAction = MTL::LoadActionDontCare; break;
                }
                ca->setLoadAction(metalLoadAction);
                
                MTL::StoreAction metalStoreAction = MTL::StoreActionDontCare;
                switch (attachment.storeOp) {
                    case StoreAction::Store: metalStoreAction = MTL::StoreActionStore; break;
                    case StoreAction::DontCare: metalStoreAction = MTL::StoreActionDontCare; break;
                }
                ca->setStoreAction(metalStoreAction);
                
                ca->setClearColor(MTL::ClearColor::Make(
                    attachment.clearValue.color.r,
                    attachment.clearValue.color.g,
                    attachment.clearValue.color.b,
                    attachment.clearValue.color.a
                ));
            }
        }
        cachedColorAttachments_.push_back({attachment.texture, nativeTex});
    }

    // 设置深度附件
    cachedDepthAttachment_ = {desc_.depthAttachment.texture, nullptr};
    if (desc_.depthAttachment.texture != handles::INVALID_RESOURCE) {
        MetalTexture* texture = metalDevice.GetTexture(desc_.depthAttachment.texture);
        if (texture && texture->GetNativeTexture()) {
            cachedDepthAttachment_.nativeTexture = texture->GetNativeTexture();
            
            MTL::RenderPassDepthAttachmentDescriptor* da = mtlPassDesc_->depthAttachment();
            da->setTexture(cachedDepthAttachment_.nativeTexture);
            da->setSlice(desc_.depthAttachment.arrayLayer);
            da->setLevel(desc_.depthAttachment.mipLevel);
            
            MTL::LoadAction metalLoadAction = MTL::LoadActionDontCare;
            switch (desc_.depthAttachment.loadOp) {
                case LoadAction::Load: metalLoadAction = MTL::LoadActionLoad; break;
                case LoadAction::Clear: metalLoadAction = MTL::LoadActionClear; break;
                case LoadAction::DontCare: metalLoadAction = MTL::LoadActionDontCare; break;
            }
            da->setLoadAction(metalLoadAction);
            da->setClearDepth(desc_.depthAttachment.clearValue.depth);
            
            MTL::StoreAction metalStoreAction = MTL::StoreActionDontCare;
            switch (desc_.depthAttachment.storeOp) {
                case StoreAction::Store: metalStoreAction = MTL::StoreActionStore; break;
                case StoreAction::DontCare: metalStoreAction = MTL::StoreActionDontCare; break;
            }
            da->setStoreAction(metalStoreAction);
        }
    }

    // 设置模板附件
    cachedStencilAttachment_ = {desc_.stencilAttachment.texture, nullptr};
    if (desc_.stencilAttachment.texture != handles::INVALID_RESOURCE) {
        MetalTexture* texture = metalDevice.GetTexture(desc_.stencilAttachment.texture);
        if (texture && texture->GetNativeTexture()) {
            cachedStencilAttachment_.nativeTexture = texture->GetNativeTexture();
            
            MTL::RenderPassStencilAttachmentDescriptor* sa = mtlPassDesc_->stencilAttachment();
            sa->setTexture(cachedStencilAttachment_.nativeTexture);
            sa->setSlice(desc_.stencilAttachment.arrayLayer);
            sa->setLevel(desc_.stencilAttachment.mipLevel);
            
            MTL::LoadAction metalLoadAction = MTL::LoadActionDontCare;
            switch (desc_.stencilAttachment.loadOp) {
                case LoadAction::Load: metalLoadAction = MTL::LoadActionLoad; break;
                case LoadAction::Clear: metalLoadAction = MTL::LoadActionClear; break;
                case LoadAction::DontCare: metalLoadAction = MTL::LoadActionDontCare; break;
            }
            sa->setLoadAction(metalLoadAction);
            sa->setClearStencil(desc_.stencilAttachment.clearValue.stencil);
            
            MTL::StoreAction metalStoreAction = MTL::StoreActionDontCare;
            switch (desc_.stencilAttachment.storeOp) {
                case StoreAction::Store: metalStoreAction = MTL::StoreActionStore; break;
                case StoreAction::DontCare: metalStoreAction = MTL::StoreActionDontCare; break;
            }
            sa->setStoreAction(metalStoreAction);
        }
    }
}

MTL::RenderPassDescriptor* MetalRenderPass::GetNativeRenderPassDescriptor() {
    if (isDirty()) {
        buildDescriptor();
    }
    return mtlPassDesc_;
}

} // namespace primal::graphics::rhi
