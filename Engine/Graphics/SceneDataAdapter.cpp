// 文件说明: 实现 SceneDataAdapter 类，用于加载场景数据和网格数据。
// 关键点: 解析 ContentToEngine 生成的二进制格式，包括 LOD、Submesh、Material 等信息。
// 注意: 二进制格式必须与 MeshCPU.cpp 中的写入格式严格一致（特别是 16 字节对齐）。

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

    size_t Remaining() const {
        return size_ > offset_ ? size_ - offset_ : 0;
    }

    size_t GetOffset() const {
        return offset_;
    }

private:
    const uint8_t* buffer_;
    size_t size_;
    size_t offset_;
};

// Helper for alignment
// inline uint32_t align_up(uint32_t s, uint32_t a) { return (s + a - 1) & ~(a - 1); }

} // namespace

// 函数说明: 从二进制数据中加载渲染项数据（网格）。
// 参数:
//   device: RHI 设备指针
//   data: 二进制数据指针
//   size: 数据大小
// 返回: 加载的网格信息列表
std::vector<SceneDataMeshInfo> SceneDataAdapter::LoadRenderItemData(rhi::RHIDeviceBase* device, const void* data, uint32_t size) {
    std::vector<SceneDataMeshInfo> result;
    if (!data || !device) return result;

    std::cout << "SceneDataAdapter::LoadRenderItemData: Start. Size=" << size << std::endl;

    BlobReader reader(static_cast<const uint8_t*>(data), size);

    // DEBUG: Dump first 16 integers
    const uint32_t* debugPtr = reinterpret_cast<const uint32_t*>(reader.GetCurrentPtr());
    std::cout << "File Header Dump (First 16 u32):" << std::endl;
    const uint8_t* u8Data = static_cast<const uint8_t*>(data);
    for (int i = 0; i < 16; ++i) {
        if (u8Data + (i + 1) * sizeof(uint32_t) > u8Data + size) break;
        std::cout << "[" << i << "]: " << debugPtr[i] << " (0x" << std::hex << debugPtr[i] << std::dec << ")" << std::endl;
    }

    // 1. Materials Header (Added by pack_geometry.py)
    // We try to detect if this is the new format with materials header or old format
    // New format: [NumMaterials] [Mat0_NameLen] [Mat0_Name] ... [LODCount]
    // Old format: [LODCount] ...
    // Since we rebuilt the model, we assume New Format. 
    // But to be safe, we can check if the first uint32 is reasonably small and followed by string length?
    // Actually, let's just implement the New Format reading as we control the pipeline.
    
    uint32_t numMaterials = reader.Read<uint32_t>();
    std::cout << "SceneDataAdapter: Header NumMaterials=" << numMaterials << std::endl;
    
    struct TempMaterialData {
        std::string name;
        std::string diffuse;
        std::string normal;
    };
    std::vector<TempMaterialData> tempMaterials;

    std::vector<std::shared_ptr<MaterialInstance>> materialInstances;
    std::vector<std::shared_ptr<Material>> materials;
    
    // Basic sanity check: if numMaterials is huge (e.g. > 10000), it might be lodCount from old format (which is usually small, e.g. 1-5).
    // Wait, lodCount is usually small. numMaterials is e.g. 25.
    // If we read lodCount (e.g. 1) as numMaterials, we try to read 1 material.
    // Material reading involves reading string length.
    // If it was lodCount, next is thresholds (floats).
    // Float as u32 (length) might be huge.
    
    if (numMaterials < 10000) {
        materialInstances.reserve(numMaterials);
        materials.reserve(numMaterials);
        tempMaterials.reserve(numMaterials);
        
        for (uint32_t i = 0; i < numMaterials; ++i) {
             if (reader.Remaining() < 4) break;
             uint32_t nameLen = reader.Read<uint32_t>();
             if (nameLen > 1000) {
                 // Something is wrong, maybe old format?
                 std::cerr << "SceneDataAdapter: Material Name too long (" << nameLen << "), possible format mismatch." << std::endl;
                 break;
             }
             std::string name = reader.ReadString(nameLen);
             
             uint32_t diffLen = reader.Read<uint32_t>();
             std::string diffuse = reader.ReadString(diffLen);
             
             uint32_t normLen = reader.Read<uint32_t>();
             std::string normal = reader.ReadString(normLen);
             
             tempMaterials.push_back({name, diffuse, normal});

             // Create Dummy Material & Instance
             auto mat = std::make_shared<Material>();
             // We can store the name/paths in the material for debug/loading later if Material class supports it
             // For now, just create the instance
             materials.push_back(mat);
             auto inst = std::make_shared<MaterialInstance>(mat.get());
             materialInstances.push_back(inst);
             
             std::cout << "  Mat " << i << ": " << name << " (Diff: " << diffuse << ")" << std::endl;
        }
    } else {
         std::cerr << "SceneDataAdapter: numMaterials suspiciously large, assuming Old Format or Error." << std::endl;
         // Rewind? We can't rewind BlobReader easily without creating new one or hacking offset.
         // But we made BlobReader private.
         // Let's just hope we are using the new format.
    }

    // 2. lod_count
    if (reader.Remaining() < 4) return result;
    uint32_t lodCount = reader.Read<uint32_t>();
    std::cout << "lodCount: " << lodCount << std::endl;
    
    std::vector<float> thresholds(lodCount);
    if (reader.Remaining() < sizeof(float) * lodCount) return result;
    reader.Read(thresholds.data(), sizeof(float) * lodCount);

    // Skip lod_offsets
    if (reader.Remaining() < sizeof(uint32_t) * lodCount) return result;
    reader.Skip(sizeof(uint32_t) * lodCount);
    
    for (uint32_t lod = 0; lod < lodCount; ++lod) {
        if (reader.Remaining() < 8) break;
        uint32_t submeshCount = reader.Read<uint32_t>();
        uint32_t sizeOfSubmeshes = reader.Read<uint32_t>(); // Read and use variable to avoid skip confusion
        
        std::cout << "LOD " << lod << ": SubmeshCount=" << submeshCount << ", SizeOfSubmeshes=" << sizeOfSubmeshes << std::endl;

        for (uint32_t i = 0; i < submeshCount; ++i) {
             if (reader.Remaining() < 20) break;
             // Read submesh header
             int32_t materialIndex = reader.Read<int32_t>();
             uint32_t elementSize = reader.Read<uint32_t>();
             
             uint32_t vertexCount, indexCount, elementsType, primitiveTopology;

             // Heuristic to detect Old Format (shifted fields due to missing MaterialIndex)
             if (elementSize > 200) {
                 // Old Format: [ElementSize] [VertexCount] [IndexCount] [ElementType] [PrimitiveTopology]
                 // We read [ElementSize] as materialIndex, and [VertexCount] as elementSize.
                 uint32_t realElementSize = (uint32_t)materialIndex;
                 uint32_t realVertexCount = elementSize;
                 
                 vertexCount = realVertexCount;
                 elementSize = realElementSize;
                 materialIndex = -1; // Default
                 
                 indexCount = reader.Read<uint32_t>();
                 elementsType = reader.Read<uint32_t>();
                 primitiveTopology = reader.Read<uint32_t>();
                 
                 std::cout << "DEBUG: Detected Old Format. Adapted values: MatIdx=" << materialIndex 
                           << ", ElemSize=" << elementSize << ", Verts=" << vertexCount << std::endl;
             } else {
                 // New Format: [MatIdx] [ElemSize] [VertCount] [IdxCount] [ElemType] [PrimTopo]
                 if (reader.Remaining() < 16) break; // Need 4 more ints
                 
                 vertexCount = reader.Read<uint32_t>();
                 indexCount = reader.Read<uint32_t>();
                 elementsType = reader.Read<uint32_t>();
                 primitiveTopology = reader.Read<uint32_t>();
             }
             
             (void)elementsType;
             (void)primitiveTopology;

             
             std::cout << "Submesh " << i << " (LOD " << lod << "): MatIdx=" << materialIndex << ", Verts=" << vertexCount 
                       << ", Indices=" << indexCount << ", ElementSize=" << elementSize << std::endl;

             // Data sizes
             uint32_t positionSize = 12 * vertexCount;
             uint32_t elementBufferSize = elementSize * vertexCount;
             uint32_t indexSize = (vertexCount < (1 << 16)) ? 2 : 4;
             uint32_t indexBufferSize = indexSize * indexCount;
             
             // Alignment (Match MeshCPU.cpp align16 logic which aligns absolute offset)
             size_t currentOffset = reader.GetOffset();
             
             uint32_t posPadding = 0;
             if (vertexCount > 0) {
                 size_t endPos = currentOffset + positionSize;
                 size_t alignedEndPos = (endPos + 15) & ~15;
                 posPadding = (uint32_t)(alignedEndPos - endPos);
             }
             
             uint32_t elemPadding = 0;
             if (elementBufferSize > 0) {
                 size_t currentElemOffset = currentOffset + positionSize + posPadding;
                 size_t endElem = currentElemOffset + elementBufferSize;
                 size_t alignedEndElem = (endElem + 15) & ~15;
                 elemPadding = (uint32_t)(alignedEndElem - endElem);
             }
             
             // Pointers
             const uint8_t* posPtr = static_cast<const uint8_t*>(reader.GetCurrentPtr());
             const uint8_t* elemPtr = posPtr + positionSize + posPadding;
             const uint8_t* idxPtr = elemPtr + elementBufferSize + elemPadding;
             
             // Skip data in reader
             uint32_t totalSize = positionSize + posPadding + elementBufferSize + elemPadding + indexBufferSize;
             if (reader.Remaining() < totalSize) {
                 std::cerr << "Incomplete submesh data." << std::endl;
                 break;
             }
             reader.Skip(totalSize);
             
             // Create Interleaved Data
        // Force standard stride (32 bytes) to match Metal shader (12 Pos + 20 Element)
        constexpr uint32_t TARGET_ELEMENT_SIZE = 20;
        constexpr uint32_t TARGET_VERTEX_STRIDE = 12 + TARGET_ELEMENT_SIZE;
        
        // Determine source strides
        // WARNING: The binary file format for Positions MUST be 12 bytes (packed float3).
        // MeshCPU.cpp on Mac might write 16 bytes (sizeof(simd::float3)), but pack_geometry.py (Python) writes 12 bytes.
        // We assume 12 bytes to be safe and platform-independent for the file format.
        const uint32_t srcPosStride = 12; 
        const uint32_t srcElemStride = elementSize;

        std::cout << "SceneDataAdapter: Interleaving - SrcPosStride=" << srcPosStride 
                  << ", SrcElemStride=" << srcElemStride 
                  << ", TargetStride=" << TARGET_VERTEX_STRIDE << std::endl;
        
        std::vector<uint8_t> interleavedVertices(vertexCount * TARGET_VERTEX_STRIDE);
        // Zero initialize to handle padding/missing elements safely
        memset(interleavedVertices.data(), 0, interleavedVertices.size());
        
        for (uint32_t v = 0; v < vertexCount; ++v) {
            uint8_t* dstVertex = interleavedVertices.data() + v * TARGET_VERTEX_STRIDE;
            
            // 1. Copy Position (Always 12 bytes: x, y, z)
            // Skip padding if srcPosStride > 12 (though we force 12 now)
            memcpy(dstVertex, posPtr + v * srcPosStride, 12);
            
            // 2. Copy Elements
            if (srcElemStride > 0) {
                uint8_t* dstElem = dstVertex + 12;
                const uint8_t* srcElem = elemPtr + v * srcElemStride;
                
                if (srcElemStride >= 24) {
                    // Special handling for static_normal_texture on platforms with padding (e.g. Mac C++ writer)
                    // Layout: [Color+Sign+Normal+Tangent (12 bytes)] [Padding (4 bytes)] [UV (8 bytes)] ...
                    // Target: [Color+Sign+Normal+Tangent (12 bytes)] [UV (8 bytes)]
                    memcpy(dstElem, srcElem, 12);      // Copy Color, Normal, Tangent
                    // Check if we have enough space for UV
                    // UV starts at offset 16 in source
                    memcpy(dstElem + 12, srcElem + 16, 8); // Copy UV
                } else {
                    // Standard copy (clamp to target size)
                    // e.g. 20 bytes -> 20 bytes
                    uint32_t copySize = std::min(srcElemStride, TARGET_ELEMENT_SIZE);
                    memcpy(dstElem, srcElem, copySize);
                }
            }
        }

             if (vertexCount > 0) {
                 const float* p = reinterpret_cast<const float*>(interleavedVertices.data());
                 std::cout << "DEBUG: Mesh " << i << " Vertex 0 Pos: " << p[0] << ", " << p[1] << ", " << p[2] << std::endl;
             }
             
             RenderMesh* mesh = new RenderMesh();
             rhi::DataIndexType idxType = (indexSize == 2) ? rhi::DataIndexType::UInt16 : rhi::DataIndexType::UInt32;
             
             if (mesh->Create(device, primal::id::invalid_id, 
                              interleavedVertices.data(), vertexCount, TARGET_VERTEX_STRIDE,
                              idxPtr, indexCount, idxType)) {
                 SceneDataMeshInfo info;
                 info.mesh = mesh;
                 info.lodId = lod;
                 info.lodThreshold = thresholds[lod];
                 info.materialIndex = materialIndex;
                if (materialIndex >= 0 && (size_t)materialIndex < materialInstances.size()) {
                    info.materialInstance = materialInstances[materialIndex];
                }
                if (materialIndex >= 0 && (size_t)materialIndex < tempMaterials.size()) {
                    info.diffuseTexturePath = tempMaterials[materialIndex].diffuse;
                    info.normalTexturePath = tempMaterials[materialIndex].normal;
                }
                info.name = "Mesh_" + std::to_string(lod) + "_" + std::to_string(i);
                result.push_back(info);
             } else {
                 delete mesh;
             }
        }
    }
    
    return result;
}

// 函数说明: 加载完整的场景数据，包括材质、LOD 组、网格等。
// 参数:
//   device: RHI 设备指针
//   data: 二进制数据指针
//   size: 数据大小
// 返回: 加载的网格信息列表
std::vector<SceneDataMeshInfo> SceneDataAdapter::Load(rhi::RHIDeviceBase* device, const void* data, uint32_t size) {
    std::vector<SceneDataMeshInfo> result;
    if (!data || size == 0 || !device) return result;

    BlobReader reader(static_cast<const uint8_t*>(data), size);

    // 1. Scene Name
    if (reader.Remaining() < sizeof(uint32_t)) return result;
    uint32_t sceneNameSize = reader.Read<uint32_t>();
    if (reader.Remaining() < sceneNameSize) return result;
    std::string sceneName = reader.ReadString(sceneNameSize);
    std::cout << "Scene Name: " << sceneName << std::endl;

    // 2. Num Materials
    if (reader.Remaining() < sizeof(uint32_t)) return result;
    uint32_t numMaterials = reader.Read<uint32_t>();
    if (numMaterials > 100000) { // Sanity check
        std::cerr << "Invalid NumMaterials: " << numMaterials << std::endl;
        return result;
    }
    std::cout << "Num Materials: " << numMaterials << std::endl;
    std::vector<std::shared_ptr<MaterialInstance>> materialInstances;
    std::vector<std::shared_ptr<Material>> materials;
    materialInstances.reserve(numMaterials);
    materials.reserve(numMaterials);

    for (uint32_t i = 0; i < numMaterials; ++i) {
        if (reader.Remaining() < sizeof(uint32_t)) return result;
        uint32_t matSize = reader.Read<uint32_t>();
        std::cout << "Material " << i << " Size: " << matSize << std::endl;
        if (reader.Remaining() < matSize) return result;
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
    if (reader.Remaining() < sizeof(uint32_t)) return result;
    uint32_t numLodGroups = reader.Read<uint32_t>();
    if (numLodGroups > 1000) { // Sanity check
        std::cerr << "Invalid NumLodGroups: " << numLodGroups << std::endl;
        return result;
    }
    std::cout << "Num LOD Groups: " << numLodGroups << std::endl;

    for (uint32_t i = 0; i < numLodGroups; ++i) {
        // LOD Name
        if (reader.Remaining() < sizeof(uint32_t)) return result;
        uint32_t lodNameSize = reader.Read<uint32_t>();
        if (reader.Remaining() < lodNameSize) return result;
        std::string lodName = reader.ReadString(lodNameSize);
        std::cout << "LOD Group: " << lodName << std::endl;

        // Num Meshes
        if (reader.Remaining() < sizeof(uint32_t)) return result;
        uint32_t numMeshes = reader.Read<uint32_t>();
        if (numMeshes > 100000) return result; // Sanity check
        std::cout << "Num Meshes: " << numMeshes << std::endl;

        for (uint32_t j = 0; j < numMeshes; ++j) {
            // Mesh Name
            if (reader.Remaining() < sizeof(uint32_t)) return result;
            uint32_t meshNameSize = reader.Read<uint32_t>();
            if (reader.Remaining() < meshNameSize) return result;
            std::string meshName = reader.ReadString(meshNameSize);
            std::cout << "Mesh: " << meshName << std::endl;

            // LOD ID
            if (reader.Remaining() < sizeof(uint32_t) * 8) return result; // Basic check for next few fields
            uint32_t lodId = reader.Read<uint32_t>();

            // Vertex Size (Position + Elements)
            uint32_t vertexSize = reader.Read<uint32_t>();

            // Num Vertices
            uint32_t numVertices = reader.Read<uint32_t>();

            // Index Size
            uint32_t indexSize = reader.Read<uint32_t>();

            // Num Indices
            uint32_t numIndices = reader.Read<uint32_t>();

            std::cout << "  VertexSize: " << vertexSize << ", NumVertices: " << numVertices 
                      << ", IndexSize: " << indexSize << ", NumIndices: " << numIndices << std::endl;

            // LOD Threshold
            float lodThreshold = reader.Read<float>();

            // Position Buffer Data Ptr
            size_t posSize = numVertices * sizeof(float) * 3;
            if (reader.Remaining() < posSize) return result;
            const uint8_t* posData = static_cast<const uint8_t*>(reader.Skip(posSize));

            // Element Buffer Data Ptr
            uint32_t elementSize = vertexSize - sizeof(float) * 3;
            size_t elemBufSize = numVertices * elementSize;
            if (reader.Remaining() < elemBufSize) return result;
            const uint8_t* elementData = static_cast<const uint8_t*>(reader.Skip(elemBufSize));

            // Index Buffer Data Ptr
            size_t idxBufSize = numIndices * indexSize;
            if (reader.Remaining() < idxBufSize) return result;
            const void* indexData = reader.Skip(idxBufSize);

            // Material Index
            if (reader.Remaining() < sizeof(int32_t)) return result;
            int32_t materialIndex = reader.Read<int32_t>();
            std::shared_ptr<MaterialInstance> assignedMatInst = nullptr;
            std::shared_ptr<Material> assignedMaterial = nullptr;
            
            std::cout << "Mesh: " << meshName << ", MaterialIndex: " << materialIndex << ", TotalMaterials: " << materials.size() << std::endl;

            if (materialIndex >= 0 && (size_t)materialIndex < materialInstances.size()) {
                assignedMatInst = materialInstances[materialIndex];
                assignedMaterial = materials[materialIndex];
                if (assignedMaterial) {
                    std::cout << "  Assigned Material: " << materialIndex << std::endl;
                } else {
                     std::cout << "  Assigned Material is NULL at index " << materialIndex << std::endl;
                }
            } else {
                std::cout << "  Invalid Material Index or Out of Bounds!" << std::endl;
            }
            
            // Create RenderMesh
            // 由于 RenderMesh 需要 Interleaved 数据，我们需要在这里重组数据
            // 这是一个临时方案，直到 RenderMesh 支持 Bindless/Planar 布局
            std::vector<uint8_t> interleavedData(numVertices * vertexSize);
            uint8_t* destPtr = interleavedData.data();
            const uint8_t* currPos = posData;
            const uint8_t* currElem = elementData;

            for (uint32_t v = 0; v < numVertices; ++v) {
                // Debug: Print first vertex of each mesh
                if (v == 0) {
                     const float* p = reinterpret_cast<const float*>(currPos);
                     std::cout << "Mesh: " << meshName << " Vertex 0 Position: " << p[0] << ", " << p[1] << ", " << p[2] << std::endl;
                }

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
                
                std::string diffusePath;
                std::string normalPath;
                /*
                if (materialIndex >= 0 && (size_t)materialIndex < tempMaterials.size()) {
                    diffusePath = tempMaterials[materialIndex].diffuse;
                    normalPath = tempMaterials[materialIndex].normal;
                }
                */
                
                result.push_back({meshName, lodId, lodThreshold, mesh, materialIndex, diffusePath, normalPath, assignedMaterial, assignedMatInst});
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
