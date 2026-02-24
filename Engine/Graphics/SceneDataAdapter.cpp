// 文件说明: 实现 SceneDataAdapter 类，用于加载场景数据和网格数据。
// 关键点: 解析 ContentToEngine 生成的二进制格式，包括 LOD、Submesh、Material 等信息。
// 注意: 二进制格式必须与 MeshCPU.cpp 中的写入格式严格一致（特别是 16 字节对齐）。

#include "SceneDataAdapter.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include "Material.h"
#include "MaterialInstance.h"
#include "Content/ContentToEngine.h"
#include "Graphics/RHI/Core/RHIMeshAsset.h"

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
             
             // Read roughness and metallic (added by pack_geometry.py)
             uint32_t roughLen = reader.Read<uint32_t>();
             std::string roughness = reader.ReadString(roughLen);
             
             uint32_t metalLen = reader.Read<uint32_t>();
             std::string metallic = reader.ReadString(metalLen);
             
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

        // Track submesh loading for diagnostics
        int loadedCount = 0;
        int failedCount = 0;

        for (uint32_t i = 0; i < submeshCount; ++i) {
             if (reader.Remaining() < 4) break;
             
             // =====================================================================
             // COMPACT FORMAT DETECTION (Geometry.cpp output format)
             // Compact format header: [NameLen] [Name] [LodId] [MatIdx] [ElemSize] [ElemType] [VertCount] [IdxSize] [IdxCount] [LodThreshold]
             // =====================================================================
             
             int32_t materialIndex = -1;
             uint32_t elementSize = 0;
             uint32_t vertexCount = 0;
             uint32_t indexCount = 0;
             uint32_t indexSize = 2;  // Default
             uint32_t elementsType = 0;
             uint32_t primitiveTopology = 4; // TriangleList
             std::string meshName;
             bool isCompactFormat = false;
             
             // Read first value and try to detect format
             uint32_t firstVal = reader.Read<uint32_t>();
             
             // Check if firstVal could be a name length (Compact format)
             // Name length should be reasonable (0-200 chars)
             if (firstVal > 0 && firstVal < 200 && reader.Remaining() >= firstVal + 32) {
                 // Peek at the potential name
                 const char* namePtr = static_cast<const char*>(reader.GetCurrentPtr());
                 bool looksLikeString = true;
                 for (uint32_t c = 0; c < firstVal && c < 50; ++c) {
                     char ch = namePtr[c];
                     if (ch != 0 && (ch < 32 || ch > 126)) {
                         looksLikeString = false;
                         break;
                     }
                 }
                 
                 if (looksLikeString) {
                     // Try to read as Compact format
                     meshName = std::string(namePtr, firstVal);
                     reader.Skip(firstVal);
                     
                     uint32_t lodId = reader.Read<uint32_t>();
                     materialIndex = (int32_t)reader.Read<uint32_t>();
                     elementSize = reader.Read<uint32_t>();
                     elementsType = reader.Read<uint32_t>();
                     vertexCount = reader.Read<uint32_t>();
                     indexSize = reader.Read<uint32_t>();
                     indexCount = reader.Read<uint32_t>();
                     float lodThreshold;
                     reader.Read(&lodThreshold, sizeof(float));
                     
                     // Validate Compact format signatures
                     if (elementSize == 20 && (indexSize == 2 || indexSize == 4) &&
                         vertexCount > 0 && vertexCount < 10000000 &&
                         indexCount > 0 && indexCount < 30000000) {
                         isCompactFormat = true;
                         std::cout << "DEBUG: Detected COMPACT Format. Name='" << meshName 
                                   << "', LodId=" << lodId
                                   << ", MatIdx=" << materialIndex
                                   << ", ElemSize=" << elementSize
                                   << ", ElemType=" << elementsType
                                   << ", Verts=" << vertexCount
                                   << ", IdxSize=" << indexSize
                                   << ", Indices=" << indexCount << std::endl;
                     }
                 }
             }
             
             if (!isCompactFormat) {
                 // Fallback: Use original format detection
                 // firstVal was either materialIndex or elementSize (old format)
                 elementSize = reader.Read<uint32_t>();
                 uint32_t vertexCount_tmp, indexCount_tmp, elementsType_tmp, primitiveTopology_tmp;
                 
                 // Heuristic to detect Old Format
                 if (elementSize > 200) {
                     // Old Format: [ElementSize] [VertexCount] [IndexCount] [ElementType] [PrimitiveTopology]
                     uint32_t realElementSize = firstVal;
                     uint32_t realVertexCount = elementSize;
                     
                     vertexCount = realVertexCount;
                     elementSize = realElementSize;
                     materialIndex = -1;
                     
                     indexCount_tmp = reader.Read<uint32_t>();
                     elementsType_tmp = reader.Read<uint32_t>();
                     primitiveTopology_tmp = reader.Read<uint32_t>();
                     
                     indexCount = indexCount_tmp;
                     elementsType = elementsType_tmp;
                     primitiveTopology = primitiveTopology_tmp;
                     indexSize = (vertexCount < (1 << 16)) ? 2 : 4;
                     
                     std::cout << "DEBUG: Detected Old Format. MatIdx=" << materialIndex 
                               << ", ElemSize=" << elementSize << ", Verts=" << vertexCount << std::endl;
                 } else {
                     // Engine Format (pack_geometry): [MatIdx i32] [ElemSize u32] [VertCount u32] [IdxCount u32] [ElemType u32] [PrimTopo u32]
                     // NOTE: elementSize was already read at line 264
                     materialIndex = (int32_t)firstVal;
                     
                     if (reader.Remaining() < 16) {  // 4 remaining fields * 4 bytes (ElemSize already read)
                         std::cerr << "ERROR: Incomplete submesh header at LOD " << lod << ", submesh " << i << std::endl;
                         failedCount++;
                         continue;
                     }
                     
                     // elementSize already set above at line 264
                     vertexCount = reader.Read<uint32_t>();
                     indexCount = reader.Read<uint32_t>();
                     elementsType = reader.Read<uint32_t>();
                     primitiveTopology = reader.Read<uint32_t>();
                     indexSize = (vertexCount < (1 << 16)) ? 2 : 4;
                     
                     std::cout << "DEBUG: Detected Engine Format (pack_geometry). MatIdx=" << materialIndex 
                               << ", ElemSize=" << elementSize << ", Verts=" << vertexCount 
                               << ", IdxCount=" << indexCount << std::endl;
                 }
             }
             
             (void)elementsType;
             (void)primitiveTopology;

             
             std::cout << "Submesh " << i << " (LOD " << lod << "): MatIdx=" << materialIndex << ", Verts=" << vertexCount 
                       << ", Indices=" << indexCount << ", ElementSize=" << elementSize << std::endl;

             // Data sizes
             uint32_t positionSize = 12 * vertexCount;
             uint32_t elementBufferSize = elementSize * vertexCount;
             // indexSize already defined above (line 199)
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
             
             // =================================================================
             // Create RHIMeshAsset for meshlet/SDF debug support
             // =================================================================
             rhi::RHIMeshAsset meshAsset;
             meshAsset.lod_id = lod;
             meshAsset.material_idx = (materialIndex >= 0) ? materialIndex : 0;
             meshAsset.lod_threshold = thresholds[lod];
             meshAsset.index_size = indexSize;
             meshAsset.num_vertices = vertexCount;
             meshAsset.num_indices = indexCount;
             meshAsset.elements_type = elementsType;
             
             // Copy position buffer (12 bytes per vertex)
             meshAsset.position_buffer.resize(positionSize);
             memcpy(meshAsset.position_buffer.data(), posPtr, positionSize);
             
             // Copy element buffer
             meshAsset.element_buffer.resize(elementBufferSize);
             memcpy(meshAsset.element_buffer.data(), elemPtr, elementBufferSize);
             
             // Copy index buffer
             meshAsset.index_buffer.resize(indexBufferSize);
             memcpy(meshAsset.index_buffer.data(), idxPtr, indexBufferSize);
             
             reader.Skip(totalSize);
             
             // Parse MSHL (Meshlets) section
             // NOTE: We need to peek at the magic without advancing if it doesn't match
             if (reader.Remaining() >= 8) {
                 const uint8_t* peekPtr = static_cast<const uint8_t*>(reader.GetCurrentPtr());
                 uint32_t meshletMagic = *reinterpret_cast<const uint32_t*>(peekPtr);
                 
                 if (meshletMagic == 0x4C48534D) { // 'MSHL'
                     reader.Skip(4); // Consume the magic
                     uint32_t meshletCount = reader.Read<uint32_t>();
                     
                     if (meshletCount > 0 && meshletCount < 100000) {
                         meshAsset.meshlets.resize(meshletCount);
                         
                         // Read meshlet data (60 bytes each)
                         for (uint32_t m = 0; m < meshletCount; ++m) {
                             rhi::RHIMeshlet& ml = meshAsset.meshlets[m];
                             ml.vertex_offset = reader.Read<uint32_t>();
                             ml.triangle_offset = reader.Read<uint32_t>();
                             ml.vertex_count = reader.Read<uint32_t>();
                             ml.triangle_count = reader.Read<uint32_t>();
                             
                             // cone_apex[3]
                             reader.Read(&ml.cone_apex[0], sizeof(float) * 3);
                             // cone_axis[3]
                             reader.Read(&ml.cone_axis[0], sizeof(float) * 3);
                             // cone_cutoff
                             reader.Read(&ml.cone_cutoff, sizeof(float));
                             // center[3]
                             reader.Read(&ml.center[0], sizeof(float) * 3);
                             // radius
                             reader.Read(&ml.radius, sizeof(float));
                         }
                         
                         // Read meshlet vertices
                         if (reader.Remaining() >= 4) {
                             uint32_t meshletVertCount = reader.Read<uint32_t>();
                             if (meshletVertCount > 0 && meshletVertCount < 10000000) {
                                 meshAsset.meshlet_vertices.resize(meshletVertCount);
                                 reader.Read(meshAsset.meshlet_vertices.data(), meshletVertCount * 4);
                             }
                         }
                         
                         // Read meshlet triangles
                         if (reader.Remaining() >= 4) {
                             uint32_t meshletTriCount = reader.Read<uint32_t>();
                             if (meshletTriCount > 0 && meshletTriCount < 10000000) {
                                 meshAsset.meshlet_triangles.resize(meshletTriCount);
                                 reader.Read(meshAsset.meshlet_triangles.data(), meshletTriCount);
                             }
                         }
                         
                         std::cout << "DEBUG: Loaded " << meshletCount << " meshlets for mesh LOD " << lod << std::endl;
						 if (meshletCount > 0) {
							 const auto& firstMl = meshAsset.meshlets[0];
							 std::cout << "  First meshlet: center=(" << firstMl.center[0] << "," << firstMl.center[1] << "," << firstMl.center[2] << ") radius=" << firstMl.radius << std::endl;
						 }
                     }
                 }
                 // If not MSHL, don't consume - it might be next submesh data
             }
             
             // Parse SDF section
             if (reader.Remaining() >= 8) {
                 const uint8_t* peekPtr = static_cast<const uint8_t*>(reader.GetCurrentPtr());
                 uint32_t sdfMagic = *reinterpret_cast<const uint32_t*>(peekPtr);
                 
                 if (sdfMagic == 0x20464453) { // 'SDF '
                     reader.Skip(4); // Consume the magic
                     
                     // Read SDF header
                     reader.Read(&meshAsset.sdf.resolution[0], sizeof(uint32_t) * 3);
                     reader.Read(&meshAsset.sdf.bounds_min[0], sizeof(float) * 3);
                     reader.Read(&meshAsset.sdf.bounds_max[0], sizeof(float) * 3);
                     
                     // Read SDF data
                     if (reader.Remaining() >= 4) {
                         uint32_t sdfSize = reader.Read<uint32_t>();
                         if (sdfSize > 0 && sdfSize < 100000000) {
                             meshAsset.sdf.data.resize(sdfSize);
                             reader.Read(meshAsset.sdf.data.data(), sdfSize * 2);
                         }
                     }
                     
                     // Read voxels
                     if (reader.Remaining() >= 4) {
                         uint32_t voxelsSize = reader.Read<uint32_t>();
                         if (voxelsSize > 0 && voxelsSize < 100000000) {
                             meshAsset.sdf.voxels.resize(voxelsSize);
                             reader.Read(meshAsset.sdf.voxels.data(), voxelsSize);
                         }
                     }
                     
                     // Read vector field
                     if (reader.Remaining() >= 4) {
                         uint32_t vecFieldSize = reader.Read<uint32_t>();
                         if (vecFieldSize > 0 && vecFieldSize < 100000000) {
                             meshAsset.sdf.vector_field.resize(vecFieldSize);
                             reader.Read(meshAsset.sdf.vector_field.data(), vecFieldSize * 2);
                         }
                     }
                     
                     std::cout << "DEBUG: Loaded SDF data, resolution: " 
                               << meshAsset.sdf.resolution[0] << "x"
                               << meshAsset.sdf.resolution[1] << "x"
                               << meshAsset.sdf.resolution[2] << std::endl;
                 }
                 // If not SDF, don't consume - it might be next submesh data
             }
             
             // Register mesh asset to content system
             id::id_type meshEntityId = content::register_mesh_asset(meshAsset);
             
             // Don't create GPU mesh here - let it be created lazily when needed by GeometryDebugPass
             // This avoids potential resource conflicts during scene loading
             
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
                 info.meshEntityId = meshEntityId;
                 info.lodId = lod;
                 info.lodThreshold = thresholds[lod];
                 info.materialIndex = materialIndex;
                if (materialIndex >= 0 && (size_t)materialIndex < materialInstances.size()) {
                    info.materialInstance = materialInstances[materialIndex];
                }
                if (materialIndex >= 0 && (size_t)materialIndex < materials.size()) {
                    info.material = materials[materialIndex];
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
                
                SceneDataMeshInfo info;
                info.name = meshName;
                info.lodId = lodId;
                info.lodThreshold = lodThreshold;
                info.mesh = mesh;
                info.materialIndex = materialIndex;
                info.diffuseTexturePath = diffusePath;
                info.normalTexturePath = normalPath;
                info.material = assignedMaterial;
                info.materialInstance = assignedMatInst;
                result.push_back(info);
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
