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
#include "Engine/Graphics/PCG/Nodes/MarchingCubesNode.h"
#include "Engine/Graphics/PCG/Nodes/GlobalSDFMeshNode.h"
#include "Engine/EngineAPI/GameEntity_impl.h"
#include "Engine/Components/Material.h"
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
    void ExecuteMarchingCubesDemo();
    void ReExecuteMarchingCubes(f32 new_iso);
    void ReScatterPCG();
    void RunPhase2UnitTests();
    void RunPhase25UnitTests();
    void HandleInput(float dt);
    void UpdateCamera();
    void UpdatePCGMaterialParams();

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

    // MarchingCubes demo (Phase 9.1): separate persistent graph so re-execute
    // doesn't re-run the main scatter graph (and vice versa).
    std::unique_ptr<primal::graphics::pcg::PCGGraph> mc_graph_;
    u32 mc_node_id_{0};
    primal::id::id_type mc_entity_id_{primal::id::invalid_id};
    f32 mc_iso_value_{0.0f};
    u32  mc_algorithm_{0};  // 0=SurfaceNets_CPU, 1=SurfaceNets_GPU (Phase 9.3a)

    // GlobalSDFMeshNode demo (Phase 9.3b): GPU-resident streaming terrain.
    // Held outside any PCGGraph because Execute() uses RenderPipeline::Get()
    // singleton + GPUMesher — no upstream nodes required. B toggles visibility.
    std::unique_ptr<primal::graphics::pcg::GlobalSDFMeshNode> global_sdf_mesh_node_;
    bool global_sdf_mesh_visible_{false};

    // True after ForwardSceneRenderer::SetLightColor has dimmed the default
    // HDR (20,20,20) light down to (3,3,3) so N·L variation survives tone map.
    bool light_dimmed_{false};

    // Key debounce
    bool key_r_pressed_{false};
    bool key_t_pressed_{false};
    bool key_g_pressed_{false};
    bool key_n_pressed_{false};
    bool key_m_pressed_{false};
    bool key_b_pressed_{false};

    // Material parameter controls
    f32 mat_roughness_{0.5f};
    f32 mat_metallic_{0.0f};
    f32 mat_base_color_[4]{1.f, 1.f, 1.f, 1.f};
    u32 mat_color_index_{0};
    u32 mat_technique_index_{0}; // 0=Opaque, 1=AlphaClip, 2=Unlit
    bool key_1_pressed_{false};
    bool key_2_pressed_{false};
    bool key_3_pressed_{false};
    bool key_4_pressed_{false};
    bool key_5_pressed_{false};
    bool key_6_pressed_{false};
    bool key_7_pressed_{false};
    bool key_8_pressed_{false};
    bool key_9_pressed_{false};
    bool key_0_pressed_{false};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
