#include "MaterialComponent.h"
#include "Graphics/MaterialInstance.h"

namespace primal::graphics::rhi {

    MaterialComponent::MaterialComponent(std::shared_ptr<MaterialInstance> instance)
        : materialInstance(std::move(instance)) {
    }

    bool MaterialComponent::Create(RHIDeviceBase* device, Material* material) {
        if (!device || !material) {
            return false;
        }

        materialInstance = std::make_shared<MaterialInstance>(material);
        return materialInstance->Initialize(device);
    }

    void MaterialComponent::SetTexture(u32 binding, ResourceHandle texture) {
        if (materialInstance) {
            materialInstance->SetTexture(binding, texture);
        }
    }

    void MaterialComponent::SetBuffer(u32 binding, ResourceHandle buffer, u32 size, u32 offset) {
        if (materialInstance) {
            materialInstance->SetBuffer(binding, buffer, size, offset);
        }
    }

    void MaterialComponent::SetSampler(u32 binding, SamplerHandle sampler) {
        if (materialInstance) {
            materialInstance->SetSampler(binding, sampler);
        }
    }

    void MaterialComponent::SetUniformData(u32 offset, const void* data, u32 size) {
        if (materialInstance) {
            materialInstance->SetUniformData(offset, data, size);
        }
    }

    void MaterialComponent::Update(RHIDeviceBase* device, u32 frameIndex) {
        if (materialInstance) {
            materialInstance->SetCurrentFrame(frameIndex);
            materialInstance->Update(device);
        }
    }

    DescriptorSetHandle MaterialComponent::GetDescriptorSet() const {
        if (materialInstance) {
            return materialInstance->GetDescriptorSet();
        }
        return handles::INVALID_RESOURCE;
    }

    bool MaterialComponent::IsValid() const {
        return materialInstance != nullptr;
    }

    Material* MaterialComponent::GetMaterial() const {
        if (materialInstance) {
            return materialInstance->GetMaterial();
        }
        return nullptr;
    }

} // namespace primal::graphics::rhi
