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

    void MaterialComponent::SetTexture(uint32_t binding, ResourceHandle texture) {
        if (materialInstance) {
            materialInstance->SetTexture(binding, texture);
        }
    }

    void MaterialComponent::SetBuffer(uint32_t binding, ResourceHandle buffer, uint32_t size, uint32_t offset) {
        if (materialInstance) {
            materialInstance->SetBuffer(binding, buffer, size, offset);
        }
    }

    void MaterialComponent::SetSampler(uint32_t binding, SamplerHandle sampler) {
        if (materialInstance) {
            materialInstance->SetSampler(binding, sampler);
        }
    }

    void MaterialComponent::SetUniformData(uint32_t offset, const void* data, uint32_t size) {
        if (materialInstance) {
            materialInstance->SetUniformData(offset, data, size);
        }
    }

    void MaterialComponent::Update(RHIDeviceBase* device, uint32_t frameIndex) {
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
