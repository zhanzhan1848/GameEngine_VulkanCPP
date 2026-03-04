#include "RHIEntityManager.h"

namespace primal::graphics::rhi {

RHIEntityID RHIEntityManager::CreateEntity() {
    u32 index;
    if (!freeIndices.empty()) {
        index = freeIndices.front();
        freeIndices.pop();
    } else {
        generations.push_back(0);
        alive.push_back(false); // Will be set to true
        index = static_cast<u32>(generations.size() - 1);
        EnsureCapacity(index);
    }
    
    alive[index] = true;
    activeEntityCount++;
    return index;
}

void RHIEntityManager::DestroyEntity(RHIEntityID entity) {
    u32 index = entity;
    if (index >= alive.size() || !alive[index]) return;
    
    // Invalidate components
    // We don't necessarily need to clear the component data, just the 'has' flag
    if (index < hasGPUBuffer.size()) hasGPUBuffer[index] = false;
    if (index < hasTexture.size()) hasTexture[index] = false;
    if (index < hasMaterial.size()) {
        hasMaterial[index] = false;
        materials[index] = MaterialComponent{}; // Release shared_ptr if any
    }
    if (index < hasRenderLayer.size()) hasRenderLayer[index] = false;

    alive[index] = false;
    generations[index]++; // Increment generation for future robust ID support
    freeIndices.push(index);
    activeEntityCount--;
}

bool RHIEntityManager::IsAlive(RHIEntityID entity) const {
    u32 index = entity;
    if (index >= alive.size()) return false;
    return alive[index];
}

void RHIEntityManager::Clear() {
    generations.clear();
    alive.clear();
    while(!freeIndices.empty()) freeIndices.pop();
    activeEntityCount = 0;

    gpuBuffers.clear();
    hasGPUBuffer.clear();
    textures.clear();
    hasTexture.clear();
    materials.clear();
    hasMaterial.clear();
    renderLayers.clear();
    hasRenderLayer.clear();
}

void RHIEntityManager::EnsureCapacity(u32 index) {
    if (index >= gpuBuffers.size()) {
        // Grow by 1.5x or similar strategy could be better, but direct resize is fine for now
        // Assuming EnsureCapacity is called when adding new entity at 'index'
        u32 newSize = index + 1;
        if (newSize < gpuBuffers.size() * 2) {
            newSize = gpuBuffers.size() * 2;
        }
        if (newSize < 256) newSize = 256; // Initial capacity

        gpuBuffers.resize(newSize);
        hasGPUBuffer.resize(newSize, false);
        textures.resize(newSize);
        hasTexture.resize(newSize, false);
        materials.resize(newSize);
        hasMaterial.resize(newSize, false);
        renderLayers.resize(newSize);
        hasRenderLayer.resize(newSize, false);
        transforms.resize(newSize);
        hasTransform.resize(newSize, false);
        
        // Ensure 'alive' and 'generations' are synced if resized directly (CreateEntity handles them)
    }
}

} // namespace primal::graphics::rhi
