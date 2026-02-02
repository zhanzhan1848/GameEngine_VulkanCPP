#include "SceneDataAdapter.h"
#include <cassert>
#include <cstring>

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

    // 2. Num LOD Groups
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
                
                result.push_back({meshName, lodId, lodThreshold, mesh});
            } else {
                delete mesh;
                // Log error?
            }
        }
    }

    return result;
}

} // namespace primal::graphics
