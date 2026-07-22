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
    BlobReader(const u8* buffer, size_t size) : buffer_(buffer), size_(size), offset_(0) {}

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

    std::string ReadString(u32 length) {
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
    const u8* buffer_;
    size_t size_;
    size_t offset_;
};

// Helper for alignment
// inline u32 align_up(u32 s, u32 a) { return (s + a - 1) & ~(a - 1); }

} // namespace

// 函数说明: 从二进制数据中加载渲染项数据（网格）。
// 参数:
//   device: RHI 设备指针
//   data: 二进制数据指针
//   size: 数据大小
// 返回: 加载的网格信息列表
utl::vector<SceneDataMeshInfo> SceneDataAdapter::LoadRenderItemData(rhi::RHIDeviceBase* device, const void* data, u32 size) {
    utl::vector<SceneDataMeshInfo> result;
    if (!data || !device) return result;

    // std::cout << "SceneDataAdapter::LoadRenderItemData: Start. Size=" << size << std::endl;

    BlobReader reader(static_cast<const u8*>(data), size);

    // DEBUG: Dump first 16 integers
    const u32* debugPtr = reinterpret_cast<const u32*>(reader.GetCurrentPtr());
    /*
    std::cout << "File Header Dump (First 16 u32, size=" << size << "):" << std::endl;
    const u8* u8Data = static_cast<const u8*>(data);
    for (int i = 0; i < 16; ++i) {
        if (u8Data + (i + 1) * sizeof(u32) > u8Data + size) break;
        std::cout << "[" << i << "]: " << debugPtr[i] << " (0x" << std::hex << debugPtr[i] << std::dec << ")" << std::endl;
    }
    */

    // 1. Materials Header (Added by pack_geometry.py)
    // We try to detect if this is the new format with materials header or old format
    // New format: [NumMaterials] [Mat0_NameLen] [Mat0_Name] ... [LODCount]
    // Old format: [LODCount] ...
    // Since we rebuilt the model, we assume New Format. 
    // But to be safe, we can check if the first uint32 is reasonably small and followed by string length?
    // Actually, let's just implement the New Format reading as we control the pipeline.
    
    u32 numMaterials = reader.Read<u32>();
    // std::cout << "SceneDataAdapter: Header NumMaterials=" << numMaterials << std::endl;
    
    struct TempMaterialData {
        std::string name;
        std::string diffuse;
        std::string normal;
        std::string roughness;
        std::string metallic;
    };
    utl::vector<TempMaterialData> tempMaterials;

    utl::vector<std::shared_ptr<MaterialInstance>> materialInstances;
    utl::vector<std::shared_ptr<Material>> materials;
    
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
        
        // Detect fields-per-material by probing the data at two candidate offsets:
        //   3-field format (name, diffuse, normal): after numMaterials*3 strings,
        //     the next u32 should be lodCount (typically 1-10).
        //   5-field format (name, diffuse, normal, roughness, metallic): after
        //     numMaterials*5 strings, the next u32 should be lodCount.
        // We scan forward from the start of material data to count how many
        // consecutive length-prefixed strings exist before hitting a non-string u32,
        // then divide by numMaterials to get the field count.
        int fieldsPerMaterial = 3; // default: older format
        {
            size_t probeOffset = reader.GetOffset();
            int stringCount = 0;
            for (int s = 0; s < (int)(numMaterials * 6); ++s) {
                if (probeOffset + 4 > size) break;
                u32 sLen = *reinterpret_cast<const u32*>(static_cast<const u8*>(data) + probeOffset);
                if (sLen > 500) break; // not a string length
                probeOffset += 4 + sLen;
                if (probeOffset > size) break;
                stringCount++;
            }
            if (stringCount == (int)(numMaterials * 5)) {
                fieldsPerMaterial = 5;
            } else if (stringCount == (int)(numMaterials * 3)) {
                fieldsPerMaterial = 3;
            }
            // else: keep default 3
        }

        for (u32 i = 0; i < numMaterials; ++i) {
             if (reader.Remaining() < 4) break;
             u32 nameLen = reader.Read<u32>();
             if (nameLen > 1000) {
                 std::cerr << "SceneDataAdapter: Material Name too long (" << nameLen << "), possible format mismatch." << std::endl;
                 break;
             }
             std::string name = reader.ReadString(nameLen);

             u32 diffLen = reader.Read<u32>();
             std::string diffuse = reader.ReadString(diffLen);

             u32 normLen = reader.Read<u32>();
             std::string normal = reader.ReadString(normLen);

             std::string roughness;
             std::string metallic;
             if (fieldsPerMaterial >= 5) {
                 u32 roughLen = reader.Read<u32>();
                 roughness = reader.ReadString(roughLen);

                 u32 metalLen = reader.Read<u32>();
                 metallic = reader.ReadString(metalLen);
             }

             tempMaterials.push_back({name, diffuse, normal, roughness, metallic});

             // Create Dummy Material & Instance
             auto mat = std::make_shared<Material>();
             // We can store the name/paths in the material for debug/loading later if Material class supports it
             // For now, just create the instance
             materials.push_back(mat);
             auto inst = std::make_shared<MaterialInstance>(mat.get());
             materialInstances.push_back(inst);
             
             // std::cout << "  Mat " << i << ": " << name << " (Diff: " << diffuse << ")" << std::endl;
        }
    } else {
         std::cerr << "SceneDataAdapter: numMaterials suspiciously large, assuming Old Format or Error." << std::endl;
         // Rewind? We can't rewind BlobReader easily without creating new one or hacking offset.
         // But we made BlobReader private.
         // Let's just hope we are using the new format.
    }

    // 2. lod_count
    if (reader.Remaining() < 4) return result;
    u32 lodCount = reader.Read<u32>();
    // std::cout << "lodCount: " << lodCount << std::endl;
    
    utl::vector<float> thresholds(lodCount);
    if (reader.Remaining() < sizeof(float) * lodCount) return result;
    reader.Read(thresholds.data(), sizeof(float) * lodCount);

    // Skip lod_offsets
    if (reader.Remaining() < sizeof(u32) * lodCount) return result;
    reader.Skip(sizeof(u32) * lodCount);
    
    for (u32 lod = 0; lod < lodCount; ++lod) {
        if (reader.Remaining() < 8) break;
        u32 submeshCount = reader.Read<u32>();
        u32 sizeOfSubmeshes = reader.Read<u32>(); // Read and use variable to avoid skip confusion
        
        // std::cout << "LOD " << lod << ": SubmeshCount=" << submeshCount << ", SizeOfSubmeshes=" << sizeOfSubmeshes << std::endl;

        // Track submesh loading for diagnostics
        int loadedCount = 0;
        int failedCount = 0;

        for (u32 i = 0; i < submeshCount; ++i) {
             if (reader.Remaining() < 4) break;
             
             // =====================================================================
             // COMPACT FORMAT DETECTION (Geometry.cpp output format)
             // Compact format header: [NameLen] [Name] [LodId] [MatIdx] [ElemSize] [ElemType] [VertCount] [IdxSize] [IdxCount] [LodThreshold]
             // =====================================================================
             
             s32 materialIndex = -1;
             u32 elementSize = 0;
             u32 vertexCount = 0;
             u32 indexCount = 0;
             u32 indexSize = 2;  // Default
             u32 elementsType = 0;
             u32 primitiveTopology = 4; // TriangleList
             std::string meshName;
             bool isCompactFormat = false;
             
             // Read first value and try to detect format
             u32 firstVal = reader.Read<u32>();
             
             // Check if firstVal could be a name length (Compact format)
             // Name length should be reasonable (0-200 chars)
             if (firstVal > 0 && firstVal < 200 && reader.Remaining() >= firstVal + 32) {
                 // Peek at the potential name
                 const char* namePtr = static_cast<const char*>(reader.GetCurrentPtr());
                 bool looksLikeString = true;
                 for (u32 c = 0; c < firstVal && c < 50; ++c) {
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
                     
                     u32 lodId = reader.Read<u32>();
                     materialIndex = (s32)reader.Read<u32>();
                     elementSize = reader.Read<u32>();
                     elementsType = reader.Read<u32>();
                     vertexCount = reader.Read<u32>();
                     indexSize = reader.Read<u32>();
                     indexCount = reader.Read<u32>();
                     float lodThreshold;
                     reader.Read(&lodThreshold, sizeof(float));
                     
                     // Validate Compact format signatures
                     if ((indexSize == 2 || indexSize == 4) &&
                         vertexCount > 0 && vertexCount < 10000000 &&
                         indexCount > 0 && indexCount < 30000000) {
                         isCompactFormat = true;
                     }
                 }
             }
             
             if (!isCompactFormat) {
                 // Fallback: Use original format detection
                 // firstVal was either materialIndex or elementSize (old format)
                 elementSize = reader.Read<u32>();
                 u32 vertexCount_tmp, indexCount_tmp, elementsType_tmp, primitiveTopology_tmp;
                 
                 // Heuristic to detect Old Format
                 if (elementSize > 200) {
                     // Old Format: [ElementSize] [VertexCount] [IndexCount] [ElementType] [PrimitiveTopology]
                     u32 realElementSize = firstVal;
                     u32 realVertexCount = elementSize;
                     
                     vertexCount = realVertexCount;
                     elementSize = realElementSize;
                     materialIndex = -1;
                     
                     indexCount_tmp = reader.Read<u32>();
                     elementsType_tmp = reader.Read<u32>();
                     primitiveTopology_tmp = reader.Read<u32>();
                     
                     indexCount = indexCount_tmp;
                     elementsType = elementsType_tmp;
                     primitiveTopology = primitiveTopology_tmp;
                     indexSize = (vertexCount < (1 << 16)) ? 2 : 4;
                     
                     // std::cout << "DEBUG: Detected Old Format. MatIdx=" << materialIndex 
                     //           << ", ElemSize=" << elementSize << ", Verts=" << vertexCount << std::endl;
                 } else {
                     // Engine Format (pack_geometry): [MatIdx i32] [ElemSize u32] [VertCount u32] [IdxCount u32] [ElemType u32] [PrimTopo u32]
                     // NOTE: elementSize was already read at line 264
                     materialIndex = (s32)firstVal;
                     
                     if (reader.Remaining() < 16) {  // 4 remaining fields * 4 bytes (ElemSize already read)
                         std::cerr << "ERROR: Incomplete submesh header at LOD " << lod << ", submesh " << i << std::endl;
                         failedCount++;
                         continue;
                     }
                     
                     // elementSize already set above at line 264
                     vertexCount = reader.Read<u32>();
                     indexCount = reader.Read<u32>();
                     elementsType = reader.Read<u32>();
                     primitiveTopology = reader.Read<u32>();
                     indexSize = (vertexCount < (1 << 16)) ? 2 : 4;
                     
                     // std::cout << "DEBUG: Detected Engine Format (pack_geometry). MatIdx=" << materialIndex
                     //           << ", ElemSize=" << elementSize << ", Verts=" << vertexCount
                     //           << ", IdxCount=" << indexCount << std::endl;
                 }
             }
             
             (void)elementsType;
             (void)primitiveTopology;

             
             // std::cout << "Submesh " << i << " (LOD " << lod << "): MatIdx=" << materialIndex << ", Verts=" << vertexCount
             //           << ", Indices=" << indexCount << ", ElementSize=" << elementSize << std::endl;

             // Data sizes
             u32 positionSize = 12 * vertexCount;
             u32 elementBufferSize = elementSize * vertexCount;
             // indexSize already defined above (line 199)
             u32 indexBufferSize = indexSize * indexCount;
             
             // Alignment (Match MeshCPU.cpp align16 logic which aligns absolute offset)
             size_t currentOffset = reader.GetOffset();
             
             u32 posPadding = 0;
             if (vertexCount > 0) {
                 size_t endPos = currentOffset + positionSize;
                 size_t alignedEndPos = (endPos + 15) & ~15;
                 posPadding = (u32)(alignedEndPos - endPos);
             }
             
             u32 elemPadding = 0;
             if (elementBufferSize > 0) {
                 size_t currentElemOffset = currentOffset + positionSize + posPadding;
                 size_t endElem = currentElemOffset + elementBufferSize;
                 size_t alignedEndElem = (endElem + 15) & ~15;
                 elemPadding = (u32)(alignedEndElem - endElem);
             }
             
             // Pointers
             const u8* posPtr = static_cast<const u8*>(reader.GetCurrentPtr());
             const u8* elemPtr = posPtr + positionSize + posPadding;
             const u8* idxPtr = elemPtr + elementBufferSize + elemPadding;
             
             // Skip data in reader
             u32 totalSize = positionSize + posPadding + elementBufferSize + elemPadding + indexBufferSize;
             if (reader.Remaining() < totalSize) {
                 std::cerr << "Incomplete submesh data. Skipping remaining LOD." << std::endl;
                 // Skip whatever remains in this LOD to avoid corrupting reader position
                 if (reader.Remaining() > 0) {
                     reader.Skip(reader.Remaining());
                 }
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
                 const u8* peekPtr = static_cast<const u8*>(reader.GetCurrentPtr());
                 u32 meshletMagic = *reinterpret_cast<const u32*>(peekPtr);
                 
                 if (meshletMagic == 0x4C48534D) { // 'MSHL'
                     reader.Skip(4); // Consume the magic
                     u32 meshletCount = reader.Read<u32>();
                     
                     if (meshletCount > 0 && meshletCount < 100000) {
                         meshAsset.meshlets.resize(meshletCount);
                         
                         // Read meshlet data (60 bytes each)
                         for (u32 m = 0; m < meshletCount; ++m) {
                             rhi::RHIMeshlet& ml = meshAsset.meshlets[m];
                             ml.vertex_offset = reader.Read<u32>();
                             ml.triangle_offset = reader.Read<u32>();
                             ml.vertex_count = reader.Read<u32>();
                             ml.triangle_count = reader.Read<u32>();
                             
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
                             u32 meshletVertCount = reader.Read<u32>();
                             if (meshletVertCount > 0 && meshletVertCount < 10000000) {
                                 meshAsset.meshlet_vertices.resize(meshletVertCount);
                                 reader.Read(meshAsset.meshlet_vertices.data(), meshletVertCount * 4);
                             }
                         }
                         
                         // Read meshlet triangles
                         if (reader.Remaining() >= 4) {
                             u32 meshletTriCount = reader.Read<u32>();
                             if (meshletTriCount > 0 && meshletTriCount < 10000000) {
                                 meshAsset.meshlet_triangles.resize(meshletTriCount);
                                 reader.Read(meshAsset.meshlet_triangles.data(), meshletTriCount);
                             }
                         }
                         
                         // std::cout << "DEBUG: Loaded " << meshletCount << " meshlets for mesh LOD " << lod << std::endl;
						 if (meshletCount > 0) {
							 const auto& firstMl = meshAsset.meshlets[0];
							 // std::cout << "  First meshlet: center=(" << firstMl.center[0] << "," << firstMl.center[1] << "," << firstMl.center[2] << ") radius=" << firstMl.radius << std::endl;
						 }
                     }
                 }
                 // If not MSHL, don't consume - it might be next submesh data
             }
             
             // Parse SDF section
             if (reader.Remaining() >= 8) {
                 const u8* peekPtr = static_cast<const u8*>(reader.GetCurrentPtr());
                 u32 sdfMagic = *reinterpret_cast<const u32*>(peekPtr);
                 
                 if (sdfMagic == 0x20464453) { // 'SDF '
                     reader.Skip(4); // Consume the magic
                     
                     // Read SDF header
                     reader.Read(&meshAsset.sdf.resolution[0], sizeof(u32) * 3);
                     reader.Read(&meshAsset.sdf.bounds_min[0], sizeof(float) * 3);
                     reader.Read(&meshAsset.sdf.bounds_max[0], sizeof(float) * 3);
                     
                     // Read SDF data
                     if (reader.Remaining() >= 4) {
                         u32 sdfSize = reader.Read<u32>();
                         if (sdfSize > 0 && sdfSize < 100000000) {
                             meshAsset.sdf.data.resize(sdfSize);
                             reader.Read(meshAsset.sdf.data.data(), sdfSize * 2);
                         }
                     }
                     
                     // Read voxels
                     if (reader.Remaining() >= 4) {
                         u32 voxelsSize = reader.Read<u32>();
                         if (voxelsSize > 0 && voxelsSize < 100000000) {
                             meshAsset.sdf.voxels.resize(voxelsSize);
                             reader.Read(meshAsset.sdf.voxels.data(), voxelsSize);
                         }
                     }
                     
                     // Read vector field
                     if (reader.Remaining() >= 4) {
                         u32 vecFieldSize = reader.Read<u32>();
                         if (vecFieldSize > 0 && vecFieldSize < 100000000) {
                             meshAsset.sdf.vector_field.resize(vecFieldSize);
                             reader.Read(meshAsset.sdf.vector_field.data(), vecFieldSize * 2);
                         }
                     }
                     
                     // std::cout << "DEBUG: Loaded SDF data, resolution: " 
                     //           << meshAsset.sdf.resolution[0] << "x"
                     //           << meshAsset.sdf.resolution[1] << "x"
                     //           << meshAsset.sdf.resolution[2] << std::endl;
                 }
                 // If not SDF, don't consume - it might be next submesh data
             }
             
             // Register mesh asset to content system
             id::id_type meshEntityId = content::register_mesh_asset(meshAsset);
             
             // Don't create GPU mesh here - let it be created lazily when needed by GeometryDebugPass
             // This avoids potential resource conflicts during scene loading
             
             // Create Interleaved Data
        // Force standard stride (32 bytes) to match Metal shader (12 Pos + 20 Element)
        constexpr u32 TARGET_ELEMENT_SIZE = 20;
        constexpr u32 TARGET_VERTEX_STRIDE = 12 + TARGET_ELEMENT_SIZE;
        
        // Determine source strides
        // WARNING: The binary file format for Positions MUST be 12 bytes (packed float3).
        // MeshCPU.cpp on Mac might write 16 bytes (sizeof(simd::float3)), but pack_geometry.py (Python) writes 12 bytes.
        // We assume 12 bytes to be safe and platform-independent for the file format.
        const u32 srcPosStride = 12; 
        const u32 srcElemStride = elementSize;


        utl::vector<u8> interleavedVertices(vertexCount * TARGET_VERTEX_STRIDE);
        // Zero initialize to handle padding/missing elements safely
        memset(interleavedVertices.data(), 0, interleavedVertices.size());
        
        for (u32 v = 0; v < vertexCount; ++v) {
            u8* dstVertex = interleavedVertices.data() + v * TARGET_VERTEX_STRIDE;
            
            // 1. Copy Position (Always 12 bytes: x, y, z)
            // Skip padding if srcPosStride > 12 (though we force 12 now)
            memcpy(dstVertex, posPtr + v * srcPosStride, 12);
            
            // 2. Copy Elements
            if (srcElemStride > 0) {
                u8* dstElem = dstVertex + 12;
                const u8* srcElem = elemPtr + v * srcElemStride;
                
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
                    u32 copySize = std::min(srcElemStride, TARGET_ELEMENT_SIZE);
                    memcpy(dstElem, srcElem, copySize);
                }
            }
        }

             // Diagnostic: print UV range for cloth meshes (matIdx 14-19) and a reference mesh
             if (vertexCount > 0 && srcElemStride >= 20 && materialIndex >= 0) {
                 static int clothDiagCount = 0;
                 static bool printedRef = false;
                 bool isCloth = (materialIndex >= 14 && materialIndex <= 19);
                 bool isCurtain = (materialIndex >= 17 && materialIndex <= 19);
                 bool printThis = false;
                 if (isCloth && clothDiagCount < 6) { printThis = true; clothDiagCount++; }
                 else if (!printedRef && materialIndex == 0) { printThis = true; printedRef = true; }
                 if (printThis) {
                     float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
                     float minY = 1e30f, maxY = -1e30f;
                     for (u32 v = 0; v < vertexCount && v < 10000; ++v) {
                         const u8* dst = interleavedVertices.data() + v * TARGET_VERTEX_STRIDE;
                         float u, vv;
                         memcpy(&u, dst + 12 + 12, 4);
                         memcpy(&vv, dst + 12 + 12 + 4, 4);
                         if (u < minU) minU = u; if (u > maxU) maxU = u;
                         if (vv < minV) minV = vv; if (vv > maxV) maxV = vv;
                         float py;
                         memcpy(&py, dst + 4, 4);
                         if (py < minY) minY = py; if (py > maxY) maxY = py;
                     }
                 }
             }
             
             RenderMesh* mesh = new RenderMesh();
             rhi::DataIndexType idxType = (indexSize == 2) ? rhi::DataIndexType::UInt16 : rhi::DataIndexType::UInt32;
             
             if (mesh->Create(device, meshEntityId,
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
                    info.roughnessTexturePath = tempMaterials[materialIndex].roughness;
                    info.metallicTexturePath = tempMaterials[materialIndex].metallic;
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
utl::vector<SceneDataMeshInfo> SceneDataAdapter::Load(rhi::RHIDeviceBase* device, const void* data, u32 size) {
    utl::vector<SceneDataMeshInfo> result;
    if (!data || size == 0 || !device) return result;

    BlobReader reader(static_cast<const u8*>(data), size);

    // 1. Scene Name
    if (reader.Remaining() < sizeof(u32)) return result;
    u32 sceneNameSize = reader.Read<u32>();
    if (reader.Remaining() < sceneNameSize) return result;
    std::string sceneName = reader.ReadString(sceneNameSize);
    // std::cout << "Scene Name: " << sceneName << std::endl;

    // 2. Num Materials
    if (reader.Remaining() < sizeof(u32)) return result;
    u32 numMaterials = reader.Read<u32>();
    if (numMaterials > 100000) { // Sanity check
        std::cerr << "Invalid NumMaterials: " << numMaterials << std::endl;
        return result;
    }
    // std::cout << "Num Materials: " << numMaterials << std::endl;
    utl::vector<std::shared_ptr<MaterialInstance>> materialInstances;
    utl::vector<std::shared_ptr<Material>> materials;
    materialInstances.reserve(numMaterials);
    materials.reserve(numMaterials);

    for (u32 i = 0; i < numMaterials; ++i) {
        if (reader.Remaining() < sizeof(u32)) return result;
        u32 matSize = reader.Read<u32>();
        // std::cout << "Material " << i << " Size: " << matSize << std::endl;
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
    if (reader.Remaining() < sizeof(u32)) return result;
    u32 numLodGroups = reader.Read<u32>();
    if (numLodGroups > 1000) { // Sanity check
        std::cerr << "Invalid NumLodGroups: " << numLodGroups << std::endl;
        return result;
    }
    // std::cout << "Num LOD Groups: " << numLodGroups << std::endl;

    for (u32 i = 0; i < numLodGroups; ++i) {
        // LOD Name
        if (reader.Remaining() < sizeof(u32)) return result;
        u32 lodNameSize = reader.Read<u32>();
        if (reader.Remaining() < lodNameSize) return result;
        std::string lodName = reader.ReadString(lodNameSize);
        // std::cout << "LOD Group: " << lodName << std::endl;

        // Num Meshes
        if (reader.Remaining() < sizeof(u32)) return result;
        u32 numMeshes = reader.Read<u32>();
        if (numMeshes > 100000) return result; // Sanity check
        // std::cout << "Num Meshes: " << numMeshes << std::endl;

        for (u32 j = 0; j < numMeshes; ++j) {
            // Mesh Name
            if (reader.Remaining() < sizeof(u32)) return result;
            u32 meshNameSize = reader.Read<u32>();
            if (reader.Remaining() < meshNameSize) return result;
            std::string meshName = reader.ReadString(meshNameSize);
            // std::cout << "Mesh: " << meshName << std::endl;

            // LOD ID
            if (reader.Remaining() < sizeof(u32) * 8) return result; // Basic check for next few fields
            u32 lodId = reader.Read<u32>();

            // Vertex Size (Position + Elements)
            u32 vertexSize = reader.Read<u32>();

            // Num Vertices
            u32 numVertices = reader.Read<u32>();

            // Index Size
            u32 indexSize = reader.Read<u32>();

            // Num Indices
            u32 numIndices = reader.Read<u32>();

            // std::cout << "  VertexSize: " << vertexSize << ", NumVertices: " << numVertices 
            //           << ", IndexSize: " << indexSize << ", NumIndices: " << numIndices << std::endl;

            // LOD Threshold
            float lodThreshold = reader.Read<float>();

            // Position Buffer Data Ptr
            size_t posSize = numVertices * sizeof(float) * 3;
            if (reader.Remaining() < posSize) return result;
            const u8* posData = static_cast<const u8*>(reader.Skip(posSize));

            // Element Buffer Data Ptr
            u32 elementSize = vertexSize - sizeof(float) * 3;
            size_t elemBufSize = numVertices * elementSize;
            if (reader.Remaining() < elemBufSize) return result;
            const u8* elementData = static_cast<const u8*>(reader.Skip(elemBufSize));

            // Index Buffer Data Ptr
            size_t idxBufSize = numIndices * indexSize;
            if (reader.Remaining() < idxBufSize) return result;
            const void* indexData = reader.Skip(idxBufSize);

            // Material Index
            if (reader.Remaining() < sizeof(s32)) return result;
            s32 materialIndex = reader.Read<s32>();
            std::shared_ptr<MaterialInstance> assignedMatInst = nullptr;
            std::shared_ptr<Material> assignedMaterial = nullptr;
            
            // std::cout << "Mesh: " << meshName << ", MaterialIndex: " << materialIndex << ", TotalMaterials: " << materials.size() << std::endl;

            if (materialIndex >= 0 && (size_t)materialIndex < materialInstances.size()) {
                assignedMatInst = materialInstances[materialIndex];
                assignedMaterial = materials[materialIndex];
                /*
                if (assignedMaterial) {
                    std::cout << "  Assigned Material: " << materialIndex << std::endl;
                } else {
                     std::cout << "  Assigned Material is NULL at index " << materialIndex << std::endl;
                }
                */
            } else {
                // std::cout << "  Invalid Material Index or Out of Bounds!" << std::endl;
            }
            
            // Create RenderMesh
            // 由于 RenderMesh 需要 Interleaved 数据，我们需要在这里重组数据
            // 这是一个临时方案，直到 RenderMesh 支持 Bindless/Planar 布局
            utl::vector<u8> interleavedData(numVertices * vertexSize);
            u8* destPtr = interleavedData.data();
            const u8* currPos = posData;
            const u8* currElem = elementData;

            for (u32 v = 0; v < numVertices; ++v) {
                // Debug: Print first vertex of each mesh
                /*
                if (v == 0) {
                     const float* p = reinterpret_cast<const float*>(currPos);
                     std::cout << "Mesh: " << meshName << " Vertex 0 Position: " << p[0] << ", " << p[1] << ", " << p[2] << std::endl;
                }
                */

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

            rhi::DataIndexType indexType = (indexSize == 2) ? rhi::DataIndexType::UInt16 : rhi::DataIndexType::UInt32;

            // Generate a unique ID for mesh registry lookup
            static primal::id::id_type fallback_mesh_id = 0x80000000;
            primal::id::id_type meshId = fallback_mesh_id++;

            if (mesh->Create(device, meshId, 
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
                info.meshEntityId = meshId;
                result.push_back(info);
            } else {
                delete mesh;
                // Log error?
            }
        }
    }

    return result;
}

std::shared_ptr<Material> SceneDataAdapter::LoadMaterial(rhi::RHIDeviceBase* device, const void* data, u32 size) {
    if (!data || size == 0 || !device) return nullptr;

    BlobReader reader(static_cast<const u8*>(data), size);

    // Read Header
    u32 magic = reader.Read<u32>();
    if (magic != 0x4C54414D) return nullptr; // 'MATL'
    u32 version = reader.Read<u32>();
    if (version != 1) return nullptr;

    u32 shaderCount = reader.Read<u32>();

    auto material = std::make_shared<Material>();

    for(u32 i=0; i<shaderCount; ++i) {
        rhi::ShaderStage stage = (rhi::ShaderStage)reader.Read<u32>();
        u32 bytecodeSize = reader.Read<u32>();
        u32 entryPointSize = reader.Read<u32>();
        
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
    rhi::PrimitiveTopology topology = (rhi::PrimitiveTopology)reader.Read<u32>();
    material->SetTopology(topology);

    // Vertex Input Attributes
    u32 numVertexAttributes = reader.Read<u32>();
    if (numVertexAttributes > 0) {
        utl::vector<rhi::VertexInputAttribute> attributes(numVertexAttributes);
        for (u32 i = 0; i < numVertexAttributes; ++i) {
            attributes[i] = reader.Read<rhi::VertexInputAttribute>();
        }
        material->SetVertexAttributes(attributes);
    }

    // Vertex Input Bindings
    u32 numVertexBindings = reader.Read<u32>();
    if (numVertexBindings > 0) {
        utl::vector<rhi::VertexInputBinding> bindings(numVertexBindings);
        for (u32 i = 0; i < numVertexBindings; ++i) {
            bindings[i] = reader.Read<rhi::VertexInputBinding>();
        }
        material->SetVertexBindings(bindings);
    }

    // Descriptor Binding Count
    u32 numDescriptorBindings = reader.Read<u32>();
    if (numDescriptorBindings > 0) {
        utl::vector<rhi::DescriptorSetLayoutBinding> bindings(numDescriptorBindings);
        for (u32 i = 0; i < numDescriptorBindings; ++i) {
            bindings[i].binding = reader.Read<u32>();
            bindings[i].descriptorType = reader.Read<rhi::DescriptorType>();
            bindings[i].descriptorCount = reader.Read<u32>();
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

// Phase 4: Parse binary scene data → register RHIMeshAssets → return content IDs
ImportedResources SceneDataAdapter::ImportResources(const void* data, u32 size) {
    ImportedResources result;
    if (!data) return result;

    BlobReader reader(static_cast<const u8*>(data), size);

    // 1. Materials header
    u32 numMaterials = reader.Read<u32>();
    struct TempMat { std::string diffuse, normal, orm; };
    utl::vector<TempMat> tempMaterials;

    if (numMaterials < 10000) {
        tempMaterials.reserve(numMaterials);
        for (u32 i = 0; i < numMaterials; ++i) {
            if (reader.Remaining() < 4) break;
            u32 nameLen = reader.Read<u32>();
            if (nameLen > 1000) break;
            reader.Skip(nameLen);

            u32 diffLen = reader.Read<u32>();
            std::string diffuse = reader.ReadString(diffLen);

            u32 normLen = reader.Read<u32>();
            std::string normal = reader.ReadString(normLen);

            u32 roughLen = reader.Read<u32>();
            std::string roughness = reader.ReadString(roughLen);

            u32 metalLen = reader.Read<u32>();
            reader.Skip(metalLen);

            tempMaterials.push_back({diffuse, normal, roughness});
        }
    }

    // 2. LODs
    if (reader.Remaining() < 4) return result;
    u32 lodCount = reader.Read<u32>();
    utl::vector<float> thresholds(lodCount);
    if (reader.Remaining() < sizeof(float) * lodCount) return result;
    reader.Read(thresholds.data(), sizeof(float) * lodCount);
    if (reader.Remaining() < sizeof(u32) * lodCount) return result;
    reader.Skip(sizeof(u32) * lodCount);

    for (u32 lod = 0; lod < lodCount; ++lod) {
        if (reader.Remaining() < 8) break;
        u32 submeshCount = reader.Read<u32>();
        reader.Skip(4); // sizeOfSubmeshes

        for (u32 i = 0; i < submeshCount; ++i) {
            if (reader.Remaining() < 4) break;

            s32 materialIndex = -1;
            u32 elementSize = 0, vertexCount = 0, indexCount = 0, indexSize = 2;
            u32 elementsType = 0;
            std::string meshName;
            bool isCompactFormat = false;

            u32 firstVal = reader.Read<u32>();
            if (firstVal > 0 && firstVal < 200 && reader.Remaining() >= firstVal + 32) {
                const char* namePtr = static_cast<const char*>(reader.GetCurrentPtr());
                bool looksLikeString = true;
                for (u32 c = 0; c < firstVal && c < 50; ++c) {
                    char ch = namePtr[c];
                    if (ch != 0 && (ch < 32 || ch > 126)) { looksLikeString = false; break; }
                }
                if (looksLikeString) {
                    meshName = std::string(namePtr, firstVal);
                    reader.Skip(firstVal);
                    u32 lodId = reader.Read<u32>();
                    materialIndex = (s32)reader.Read<u32>();
                    elementSize = reader.Read<u32>();
                    elementsType = reader.Read<u32>();
                    vertexCount = reader.Read<u32>();
                    indexSize = reader.Read<u32>();
                    indexCount = reader.Read<u32>();
                    float lodThreshold;
                    reader.Read(&lodThreshold, sizeof(float));
                    if (elementSize == 20 && (indexSize == 2 || indexSize == 4) &&
                        vertexCount > 0 && vertexCount < 10000000 && indexCount > 0 && indexCount < 30000000) {
                        isCompactFormat = true;
                    }
                }
            }

            if (!isCompactFormat) {
                elementSize = reader.Read<u32>();
                if (elementSize > 200) {
                    vertexCount = elementSize;
                    elementSize = firstVal;
                    materialIndex = -1;
                    indexCount = reader.Read<u32>();
                    elementsType = reader.Read<u32>();
                    reader.Skip(4); // primitiveTopology
                    indexSize = (vertexCount < (1 << 16)) ? 2 : 4;
                } else {
                    materialIndex = (s32)firstVal;
                    if (reader.Remaining() < 16) break;
                    vertexCount = reader.Read<u32>();
                    indexCount = reader.Read<u32>();
                    elementsType = reader.Read<u32>();
                    reader.Skip(4); // primitiveTopology
                    indexSize = (vertexCount < (1 << 16)) ? 2 : 4;
                }
            }

            (void)elementsType;

            u32 positionSize = 12 * vertexCount;
            u32 elementBufferSize = elementSize * vertexCount;
            u32 indexBufferSize = indexSize * indexCount;

            size_t currentOffset = reader.GetOffset();
            u32 posPadding = 0;
            if (vertexCount > 0) {
                size_t endPos = currentOffset + positionSize;
                posPadding = (u32)(((endPos + 15) & ~15) - endPos);
            }
            u32 elemPadding = 0;
            if (elementBufferSize > 0) {
                size_t elemOff = currentOffset + positionSize + posPadding;
                elemPadding = (u32)(((elemOff + elementBufferSize + 15) & ~15) - (elemOff + elementBufferSize));
            }

            const u8* posPtr = static_cast<const u8*>(reader.GetCurrentPtr());
            const u8* elemPtr = posPtr + positionSize + posPadding;
            const u8* idxPtr = elemPtr + elementBufferSize + elemPadding;

            u32 totalSize = positionSize + posPadding + elementBufferSize + elemPadding + indexBufferSize;
            if (reader.Remaining() < totalSize) break;

            // Create and register RHIMeshAsset
            rhi::RHIMeshAsset meshAsset;
            meshAsset.lod_id = lod;
            meshAsset.material_idx = (materialIndex >= 0) ? materialIndex : 0;
            meshAsset.lod_threshold = thresholds[lod];
            meshAsset.index_size = indexSize;
            meshAsset.num_vertices = vertexCount;
            meshAsset.num_indices = indexCount;
            meshAsset.elements_type = elementsType;
            meshAsset.position_buffer.resize(positionSize);
            memcpy(meshAsset.position_buffer.data(), posPtr, positionSize);
            meshAsset.element_buffer.resize(elementBufferSize);
            memcpy(meshAsset.element_buffer.data(), elemPtr, elementBufferSize);
            meshAsset.index_buffer.resize(indexBufferSize);
            memcpy(meshAsset.index_buffer.data(), idxPtr, indexBufferSize);

            reader.Skip(totalSize);

            // Skip meshlets + SDF (same as LoadRenderItemData)
            if (reader.Remaining() >= 8) {
                const u8* peekPtr = static_cast<const u8*>(reader.GetCurrentPtr());
                u32 meshletMagic = *reinterpret_cast<const u32*>(peekPtr);
                if (meshletMagic == 0x4C48534D) {
                    reader.Skip(4);
                    u32 meshletCount = reader.Read<u32>();
                    if (meshletCount > 0 && meshletCount < 100000) {
                        meshAsset.meshlets.resize(meshletCount);
                        for (u32 m = 0; m < meshletCount; ++m) {
                            reader.Skip(60); // RHIMeshlet is 60 bytes
                        }
                        u32 mvCount = reader.Read<u32>();
                        if (mvCount > 0) { meshAsset.meshlet_vertices.resize(mvCount); reader.Read(meshAsset.meshlet_vertices.data(), mvCount * 4); }
                        u32 mtCount = reader.Read<u32>();
                        if (mtCount > 0) { meshAsset.meshlet_triangles.resize(mtCount); reader.Read(meshAsset.meshlet_triangles.data(), mtCount); }
                    }
                }
            }
            if (reader.Remaining() >= 8) {
                const u8* peekPtr = static_cast<const u8*>(reader.GetCurrentPtr());
                u32 sdfMagic = *reinterpret_cast<const u32*>(peekPtr);
                if (sdfMagic == 0x20464453) {
                    reader.Skip(4);
                    reader.Read(meshAsset.sdf.resolution, 12);
                    reader.Read(meshAsset.sdf.bounds_min, 12);
                    reader.Read(meshAsset.sdf.bounds_max, 12);
                    u32 sdfSize = reader.Read<u32>();
                    if (sdfSize > 0 && sdfSize < 100000000) {
                        meshAsset.sdf.data.resize(sdfSize);
                        reader.Read(meshAsset.sdf.data.data(), sdfSize * 2);
                    }
                    if (reader.Remaining() >= 4) {
                        u32 voxSize = reader.Read<u32>();
                        if (voxSize > 0 && voxSize < 100000000) {
                            meshAsset.sdf.voxels.resize(voxSize);
                            reader.Read(meshAsset.sdf.voxels.data(), voxSize);
                        }
                    }
                    if (reader.Remaining() >= 4) {
                        u32 vfSize = reader.Read<u32>();
                        if (vfSize > 0 && vfSize < 100000000) {
                            meshAsset.sdf.vector_field.resize(vfSize);
                            reader.Read(meshAsset.sdf.vector_field.data(), vfSize * 2);
                        }
                    }
                }
            }

            id::id_type meshId = content::register_mesh_asset(meshAsset);

            ImportedResources::MeshEntry entry;
            entry.mesh_content_id = meshId;
            entry.material_index = materialIndex;
            if (materialIndex >= 0 && (size_t)materialIndex < tempMaterials.size()) {
                entry.diffuse_path = tempMaterials[materialIndex].diffuse;
                entry.normal_path = tempMaterials[materialIndex].normal;
                entry.orm_path = tempMaterials[materialIndex].orm;
            }
            result.meshes.push_back(entry);
        }
    }

    std::cout << "[SceneDataAdapter::ImportResources] Imported " << result.meshes.size()
              << " mesh resources" << std::endl;
    return result;
}

} // namespace primal::graphics
