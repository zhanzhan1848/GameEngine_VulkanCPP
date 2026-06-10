#pragma once

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderProxy.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/PCG/PCGGraph.h"
#include "Engine/Graphics/PCG/PCGInstanceBuilder.h"
#include "Engine/Graphics/PCG/PCGEntityFactory.h"
#include "Engine/Graphics/PCG/PCGSerializer.h"
#include "Engine/Graphics/PCG/Nodes/ReferenceFieldNode.h"
#include "Engine/Graphics/PCG/Nodes/NoiseFieldNode.h"
#include "Engine/Graphics/PCG/Nodes/FieldScatterNode.h"
#include "Engine/Graphics/PCG/Nodes/SDFConstraintNode.h"
#include "Engine/Graphics/PCG/Nodes/DensityFilterNode.h"
#include "Engine/Graphics/PCG/Nodes/TransformNode.h"
#include "Engine/Graphics/PCG/Nodes/MeshAssignNode.h"
#include "Engine/Graphics/PCG/Nodes/RasterizedFieldNode.h"
#include "Engine/Graphics/PCG/Nodes/ScatterOnGeometryNode.h"
#include "Engine/Graphics/PCG/Nodes/CurveAlignNode.h"
#include "Engine/Graphics/PCG/Nodes/SurfaceScatterNode.h"
#include "Engine/Geometry/Geometry.h"
#include "Engine/Geometry/GeometryFieldRasterizer.h"
#include "Engine/Geometry/GeometryTypes.h"
#include "Engine/Geometry/Operations/Offset.h"
#include "Engine/Geometry/Operations/Trim.h"
#include "Engine/JobSystem/JobSystem.h"

class PCGScatterTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void ExecutePCGGraph();
    void ExecuteFieldDrivenScatter();
    void CreateGeometryDemo();
    void ReScatterPCG();
    void RunPhase2UnitTests();
    void RunPhase25UnitTests();
    void HandleInput(float dt);
    void UpdateCamera();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    primal::math::v3 camera_pos_{0, 10, 25};
    float camera_yaw_{0.f};
    float camera_pitch_{-0.3f};
    float move_speed_{10.f};
    float rotate_speed_{2.f};
    time_it timer_;
    u64 frame_count_{0};
    primal::geometry::GeometryHandle curve_handle_{};

    // Procedural mesh slot offsets (registered after Sponza)
    u32 procedural_slot_base_{0};  // First procedural mesh slot index
    u32 slot_cylinder_{0};
    u32 slot_cone_{0};
    u32 slot_box_{0};
    u32 slot_torus_{0};
    u32 slot_capsule_{0};
    u32 slot_hemisphere_{0};
    u32 slot_pyramid_{0};
    u32 slot_disc_{0};
    u32 slot_plane_{0};

    // Hot-reload: persistent PCG graph for scatter
    std::unique_ptr<primal::graphics::pcg::PCGGraph> pcg_graph_;
    u32 pcg_scatter_node_id_{0};
    u32 pcg_noise_node_id_{0};
    u32 pcg_mesh_node_id_{0};
    std::vector<primal::id::id_type> pcg_entity_ids_;
    std::vector<u32> pcg_mesh_slots_;
    u32 pcg_target_count_{2000};

    // Key debounce
    bool key_r_pressed_{false};
    bool key_t_pressed_{false};
    bool key_g_pressed_{false};
    bool key_n_pressed_{false};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
