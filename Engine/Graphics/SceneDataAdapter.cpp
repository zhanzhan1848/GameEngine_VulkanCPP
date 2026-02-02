#include "SceneDataAdapter.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include "Material.h"
#include "MaterialInstance.h"

namespace primal::graphics {

namespace {

class BlobReader {
public:
    BlobReader(const uint8_t* buffer, size_t size) : buffer_(buffer), size_(size), offset_(0) {}

    template<typename T>
    T Read() {
        assert(offset_ + sizeof(T) <= size_);
        T value = *reinterpret_cast<const T*>(buffer_ + offset_);
        offset_ += sizeof(T);
        return value;
    }

    void Read(void* dest, size_t size) {
        assert(offset_ + size <= size_);
        memcpy(dest, buffer_ + offset_, size);
        offset_ += size;
    }
    
    const void* Skip(size_t size) {
         assert(offset_ + size <= size_);
         const void* ptr = buffer_ + offset_;
         offset_ += size;
         return ptr;
    }

    std::string ReadString(uint32_t length) {
        if (length == 0) return "";
        assert(offset_ + length <= size_);
        std::string s(reinterpret_cast<const char*>(buffer_ + offset_), length);
        offset_ += length;
        return s;
    }
    
    const void* GetCurrentPtr() const {
        return buffer_ + offset_;
    }

private:
    const uint8_t* buffer_;
    size_t size_;
    size_t offset_;
};

} // namespace

std::vector<SceneDataMeshInfo> SceneDataAdapter::Load(rhi::RHIDeviceBase* device, const void* data, uint32_t size) {
    std::vector<SceneDataMeshInfo> result;
    if (!data || size == 0 || !device) return result;

    BlobReader reader(static_cast<const uint8_t*>(data), size);

    // 1. Scene Name
    uint32_t sceneNameSize = reader.Read<uint32_t>();
    std::string sceneName = reader.ReadString(sceneNameSize);

    // 2. Num Materials
    uint32_t numMaterials = reader.Read<uint32_t>();
    std::vector<std::shared_ptr<MaterialInstance>> materialInstances;
    std::vector<std::shared_ptr<Material>> materials;
    materialInstances.reserve(numMaterials);
    materials.reserve(numMaterials);

    for (uint32_t i = 0; i < numMaterials; ++i) {
        uint32_t matSize = reader.Read<uint32_t>();
        const void* matData = reader.GetCurrentPtr();
        reader.Skip(matSize);
        
        std::shared_ptr<Material> mat = LoadMaterial(device, matData, matSize);
        if (mat) {
             materials.push_back(mat);
             auto matInst = std::make_shared<MaterialInstance>(mat.get());
             if (matInst->Initialize(device)) {
                 materialInstances.push_back(matInst);
             } else {
                 materialInstances.push_back(nullptr);
                 // Log warning
             }
        } else {
             materials.push_back(nullptr);
             materialInstances.push_back(nullptr);
             // Log warning
        }
    }

    // 3. Num LOD Groups
    uint32_t numLodGroups = reader.Read<uint32_t>();

    for (uint32_t i = 0; i < numLodGroups; ++i) {
        // LOD Name
        uint32_t lodNameSize = reader.Read<uint32_t>();
        std::string lodName = reader.ReadString(lodNameSize);

        // Num Meshes
        uint32_t numMeshes = reader.Read<uint32_t>();

        for (uint32_t j = 0; j < numMeshes; ++j) {
            // Mesh Name
            uint32_t meshNameSize = reader.Read<uint32_t>();
            std::string meshName = reader.ReadString(meshNameSize);

            // LOD ID
            uint32_t lodId = reader.Read<uint32_t>();

            // Vertex Size (Position + Elements)
            uint32_t vertexSize = reader.Read<uint32_t>();

            // Num Vertices
            uint32_t numVertices = reader.Read<uint32_t>();

            // Index Size
            uint32_t indexSize = reader.Read<uint32_t>();

            // Num Indices
            uint32_t numIndices = reader.Read<uint32_t>();

            // LOD Threshold
            float lodThreshold = reader.Read<float>();

            // Position Buffer Data Ptr
            const uint8_t* posData = static_cast<const uint8_t*>(reader.Skip(numVertices * sizeof(float) * 3));

            // Element Buffer Data Ptr
            uint32_t elementSize = vertexSize - sizeof(float) * 3;
            const uint8_t* elementData = static_cast<const uint8_t*>(reader.Skip(numVertices * elementSize));

            // Index Buffer Data Ptr
            const void* indexData = reader.Skip(numIndices * indexSize);

            // Material Index
            int32_t materialIndex = reader.Read<int32_t>();
            std::shared_ptr<MaterialInstance> assignedMatInst = nullptr;
            std::shared_ptr<Material> assignedMaterial = nullptr;
            
            if (materialIndex >= 0 && (size_t)materialIndex < materialInstances.size()) {
                assignedMatInst = materialInstances[materialIndex];
                assignedMaterial = materials[materialIndex];
            }
            
            // Create RenderMesh
            // 由于 RenderMesh 需要 Interleaved 数据，我们需要在这里重组数据
            // 这是一个临时方案，直到 RenderMesh 支持 Bindless/Planar 布局
            std::vector<uint8_t> interleavedData(numVertices * vertexSize);
            uint8_t* destPtr = interleavedData.data();
            const uint8_t* currPos = posData;
            const uint8_t* currElem = elementData;

            for (uint32_t v = 0; v < numVertices; ++v) {
                // Copy Position
                memcpy(destPtr, currPos, sizeof(float) * 3);
                destPtr += sizeof(float) * 3;
                currPos += sizeof(float) * 3;

                // Copy Elements
                if (elementSize > 0) {
                    memcpy(destPtr, currElem, elementSize);
                    destPtr += elementSize;
                    currElem += elementSize;
                }
            }

            RenderMesh* mesh = new RenderMesh();
            // 使用 Hash 或者其他方式生成临时的 EntityID，或者使用 invalid_id
            // 这里我们使用 invalid_id，意味着它不会注册到全局表，需要手动管理生命周期
            // SceneDataMeshInfo 会持有这个指针
            
            rhi::DataIndexType indexType = (indexSize == 2) ? rhi::DataIndexType::UInt16 : rhi::DataIndexType::UInt32;

            if (mesh->Create(device, primal::id::invalid_id, 
                             interleavedData.data(), numVertices, vertexSize,
                             indexData, numIndices, indexType)) {
                
                // Note: assignedMaterial is still null because of the loop issue.
                // I will fix this by keeping a separate vector of materials.
                result.push_back({meshName, lodId, lodThreshold, mesh, assignedMaterial, assignedMatInst});
            } else {
                delete mesh;
                // Log error?
            }
        }
    }

    return result;
}

std::shared_ptr<Material> SceneDataAdapter::LoadMaterial(rhi::RHIDeviceBase* device, const void* data, uint32_t size) {
    if (!data || size == 0 || !device) return nullptr;

    BlobReader reader(static_cast<const uint8_t*>(data), size);

    // Read Header
    uint32_t magic = reader.Read<uint32_t>();
    if (magic != 0x4C54414D) return nullptr; // 'MATL'
    uint32_t version = reader.Read<uint32_t>();
    if (version != 1) return nullptr;

    uint32_t shaderCount = reader.Read<uint32_t>();

    auto material = std::make_shared<Material>();

    for(uint32_t i=0; i<shaderCount; ++i) {
        rhi::ShaderStage stage = (rhi::ShaderStage)reader.Read<uint32_t>();
        uint32_t bytecodeSize = reader.Read<uint32_t>();
        uint32_t entryPointSize = reader.Read<uint32_t>();
        
        std::string entryPoint = reader.ReadString(entryPointSize);
        
        // Read bytecode
        // Material::SetShader copies the data, so we can just pass the pointer
        const void* bytecodePtr = reader.GetCurrentPtr();
        reader.Skip(bytecodeSize);

        material->SetShader(stage, bytecodePtr, bytecodeSize, entryPoint.c_str());
    }

    // Pipeline States
    rhi::BlendState blendState = reader.Read<rhi::BlendState>();
    material->SetBlendState(blendState);

    rhi::DepthStencilState depthStencilState = reader.Read<rhi::DepthStencilState>();
    material->SetDepthStencilState(depthStencilState);

    rhi::RasterizerState rasterizerState = reader.Read<rhi::RasterizerState>();
    material->SetRasterizerState(rasterizerState);

    // Topology is part of RasterizerState in struct, but Material has separate setter
    // We assume the data format includes it explicitly or we take it from RasterizerState
    // For now let's assume it's explicitly written after RasterizerState to be robust
    rhi::PrimitiveTopology topology = (rhi::PrimitiveTopology)reader.Read<uint32_t>();
    material->SetTopology(topology);

    // Vertex Input Attributes
    uint32_t numVertexAttributes = reader.Read<uint32_t>();
    if (numVertexAttributes > 0) {
        utl::vector<rhi::VertexInputAttribute> attributes(numVertexAttributes);
        for (uint32_t i = 0; i < numVertexAttributes; ++i) {
            attributes[i] = reader.Read<rhi::VertexInputAttribute>();
        }
        material->SetVertexAttributes(attributes);
    }

    // Vertex Input Bindings
    uint32_t numVertexBindings = reader.Read<uint32_t>();
    if (numVertexBindings > 0) {
        utl::vector<rhi::VertexInputBinding> bindings(numVertexBindings);
        for (uint32_t i = 0; i < numVertexBindings; ++i) {
            bindings[i] = reader.Read<rhi::VertexInputBinding>();
        }
        material->SetVertexBindings(bindings);
    }

    // Descriptor Binding Count
    uint32_t numDescriptorBindings = reader.Read<uint32_t>();
    if (numDescriptorBindings > 0) {
        utl::vector<rhi::DescriptorSetLayoutBinding> bindings(numDescriptorBindings);
        for (uint32_t i = 0; i < numDescriptorBindings; ++i) {
            bindings[i].binding = reader.Read<uint32_t>();
            bindings[i].descriptorType = reader.Read<rhi::DescriptorType>();
            bindings[i].descriptorCount = reader.Read<uint32_t>();
            bindings[i].stageFlags = reader.Read<rhi::ShaderStage>();
            bindings[i].flags = reader.Read<rhi::DescriptorBindingFlags>();
            bindings[i].immutableSamplers = nullptr;
        }

        rhi::DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindingCount = numDescriptorBindings;
        layoutDesc.bindings = bindings.data();

        rhi::DescriptorSetLayoutHandle layout = device->CreateDescriptorSetLayout(layoutDesc);
        if (layout != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            material->SetDescriptorSetLayout(layout);
        }
    }

    return material;
}

} // namespace primal::graphics
