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
#include "Engine/Graphics/PCG/Nodes/ReferenceFieldNode.h"
#include "Engine/Graphics/PCG/Nodes/NoiseFieldNode.h"
#include "Engine/Graphics/PCG/Nodes/FieldScatterNode.h"
#include "Engine/Graphics/PCG/Nodes/SDFConstraintNode.h"
#include "Engine/Graphics/PCG/Nodes/DensityFilterNode.h"
#include "Engine/Graphics/PCG/Nodes/TransformNode.h"
#include "Engine/Graphics/PCG/Nodes/MeshAssignNode.h"

class PCGScatterTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void ExecutePCGGraph();
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
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
