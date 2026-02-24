#pragma once
#include "Common/CommonHeaders.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {

class RenderView;
namespace rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}
namespace rendergraph {
    class RenderGraph;
}

enum class GeometryDebugMode : u32 {
    None = 0,
    Meshlet = 1,
    Primitive = 2,
    Wireframe = 3,
    SDF = 4,
    Voxel = 5,
    VectorField = 6
};

enum class MeshletColorMode : u32 {
    Solid = 0,
    Colored = 1,
    LOD = 2,
    TriangleCount = 3
};

struct GeometryDebugSettings {
    bool enable = false;
    GeometryDebugMode mode = GeometryDebugMode::None;
    MeshletColorMode meshletColorMode = MeshletColorMode::Solid;
    bool wireframe = false;
    bool visualize_meshlets = true;
    bool visualize_sdf = false;
    bool visualize_voxels = false;
    bool visualize_vector_field = false;
    float slice_depth = 0.5f;
};

struct GeometryDebugData {
    rendergraph::RGResourceHandle target;
    rendergraph::RGResourceHandle depth;
    
    GeometryDebugSettings settings;

    // Camera
    float viewProjection[16];
};

/**
 * @brief Adds a pass to visualize geometry data (Meshlets, SDF, Voxels)
 * @details This pass should be added after the main rendering passes but before UI/PostProcess
 */
const GeometryDebugData& AddGeometryDebugPass(
    rendergraph::RenderGraph& graph, 
    rendergraph::RGResourceHandle target,
    rendergraph::RGResourceHandle depth,
    const RenderView& view,
    const GeometryDebugSettings* settings  // Pass by pointer!
);

/**
 * @brief Immediate mode rendering of geometry debug info
 */
void RenderGeometryDebug(
    rhi::RHIDeviceBase& device,
    rhi::RHICommandBuffer* cmdBuffer,
    const RenderView& view,
    rhi::ResourceHandle renderTarget,
    rhi::ResourceHandle depthStencil,
    rhi::DataFormat colorFormat,
    rhi::DataFormat depthFormat,
    const GeometryDebugSettings& settings
);

}
