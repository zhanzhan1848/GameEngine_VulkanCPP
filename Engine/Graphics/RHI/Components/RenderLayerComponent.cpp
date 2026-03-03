/**
 * @file RenderLayerComponent.cpp
 * @brief 渲染层级与视口配置组件实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-30
 * @version 0.1.0
 */

#include "RenderLayerComponent.h"

namespace primal::graphics::rhi {

RenderLayerComponent::RenderLayerComponent()
    : layerMask(1) // 默认 Layer 0 (Bit 0)
    , priority(0)
    , useCustomViewport(false)
    , useCustomScissor(false)
{
}

void RenderLayerComponent::SetLayer(u8 layerIndex) {
    if (layerIndex >= 32) return;
    layerMask = (1u << layerIndex);
}

void RenderLayerComponent::EnableLayer(u8 layerIndex) {
    if (layerIndex >= 32) return;
    layerMask |= (1u << layerIndex);
}

void RenderLayerComponent::DisableLayer(u8 layerIndex) {
    if (layerIndex >= 32) return;
    layerMask &= ~(1u << layerIndex);
}

bool RenderLayerComponent::HasLayer(u8 layerIndex) const {
    if (layerIndex >= 32) return false;
    return (layerMask & (1u << layerIndex)) != 0;
}

bool RenderLayerComponent::Match(u32 mask) const {
    return (layerMask & mask) != 0;
}

} // namespace primal::graphics::rhi
