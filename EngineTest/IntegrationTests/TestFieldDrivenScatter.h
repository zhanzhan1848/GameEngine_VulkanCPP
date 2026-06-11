#pragma once

#include "Test.h"
#include "Engine/Geometry/Geometry.h"
#include "Engine/Geometry/GeometryFieldRasterizer.h"
#include "Engine/Geometry/GeometryTypes.h"
#include "Engine/Graphics/PCG/PCGGraph.h"
#include "Engine/Graphics/PCG/PCGSerializer.h"
#include "Engine/Graphics/PCG/PCGTypes.h"
#include "Engine/Graphics/PCG/Nodes/RasterizedFieldNode.h"
#include "Engine/Graphics/PCG/Nodes/NoiseFieldNode.h"
#include "Engine/Graphics/PCG/Nodes/FieldScatterNode.h"
#include "Engine/Graphics/PCG/Nodes/TransformNode.h"
#include "Engine/Graphics/PCG/Nodes/MeshAssignNode.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#include <iostream>
#include <cassert>
#include <cmath>
#include <ctime>
#include <algorithm>

class Engine_Test : public Test
#ifdef __APPLE__
    , public NS::ApplicationDelegate
#endif
{
public:
    Engine_Test() = default;

    bool initialize() override {
        srand((u32)time(nullptr));
        primal::geometry::init();
        std::cout << "=== Field-Driven Scatter Integration Test ===" << std::endl;
        run_all_tests();
        return true;
    }

    void run() override {}
    void shutdown() override {}

#ifdef __APPLE__
    void applicationDidFinishLaunching(NS::Notification* notification) override {
        NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
        pApp->activateIgnoringOtherApps(true);
        initialize();
        pApp->terminate(nullptr);
    }

    void applicationWillFinishLaunching(NS::Notification* notification) override {
        NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
        pApp->setActivationPolicy(NS::ActivationPolicy::ActivationPolicyRegular);
    }

    bool applicationShouldTerminateAfterLastWindowClosed(NS::Application*) override {
        return true;
    }
#endif

private:
    static constexpr f32 EPS = 1e-3f;

    void check(bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "FAIL: " << msg << std::endl;
            assert(false);
        }
    }

    void approx(f32 a, f32 b, const char* msg) {
        check(std::abs(a - b) < EPS, msg);
    }

    void run_all_tests() {
        test_rasterized_field_node();
        test_field_driven_scatter();
        test_serializer_roundtrip();
        std::cout << "\n=== All Field-Driven Scatter tests passed ===" << std::endl;
    }

    // Verify RasterizedFieldNode produces a non-empty field from geometry
    void test_rasterized_field_node() {
        std::cout << "[RasterizedFieldNode] ";

        // Create line geometry (0,0,0)→(10,0,0)
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0, 0, 0});
        pts.push_back(primal::math::v3{10, 0, 0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);
        check(h.is_valid(), "line handle valid");

        // Build RasterizedFieldNode
        primal::graphics::pcg::RasterizedFieldNode node;
        node.geometry_handles.push_back(h);
        node.bounds_min = primal::math::v3{-2, -2, -2};
        node.bounds_max = primal::math::v3{12, 2, 2};
        node.resolution_x = 16;
        node.resolution_y = 8;
        node.resolution_z = 8;
        node.band_width = 3.0f;
        node.union_mode = false;

        node.Execute();

        auto* field = node.outputs[0].AsField();
        check(field != nullptr, "RasterizedFieldNode output is a Field");

        // Verify the underlying PCGRasterizedField has data
        auto* rasterField = static_cast<primal::graphics::pcg::PCGRasterizedField*>(field);
        check(!rasterField->data.empty(), "Rasterized field data is non-empty");
        check(rasterField->dim[0] == 16, "dim[0] == 16");
        check(rasterField->dim[1] == 8, "dim[1] == 8");
        check(rasterField->dim[2] == 8, "dim[2] == 8");

        // Near the line midpoint (5,0,0), value should be most negative
        f32 d_mid = rasterField->SampleFloat(primal::math::v3{5, 0, 0});
        // Far from line but inside volume: should be less negative or positive
        f32 d_far = rasterField->SampleFloat(primal::math::v3{5, 1.9f, 1.9f});
        // Convention: union_mode=false → near surface = negative, far = positive
        check(d_mid < 0.0f, "Near line value is negative");
        check(d_mid < d_far, "Near line value < far value (signed ordering)");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    // Full end-to-end: Geometry → RasterizedField → Scatter → verify clustering
    void test_field_driven_scatter() {
        std::cout << "[FieldDrivenScatter] ";

        using namespace primal::graphics::pcg;

        // Create line geometry (0,0,0)→(10,0,0)
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0, 0, 0});
        pts.push_back(primal::math::v3{10, 0, 0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);
        check(h.is_valid(), "line handle valid for e2e test");

        // Build PCG graph: RasterizedField → FieldScatter
        PCGGraph graph;

        auto rasterNode = std::make_unique<RasterizedFieldNode>();
        rasterNode->geometry_handles.push_back(h);
        rasterNode->bounds_min = primal::math::v3{-2, -2, -2};
        rasterNode->bounds_max = primal::math::v3{12, 2, 2};
        rasterNode->resolution_x = 32;
        rasterNode->resolution_y = 16;
        rasterNode->resolution_z = 16;
        rasterNode->band_width = 2.0f;
        rasterNode->union_mode = false;
        u32 rid = graph.AddNode(std::move(rasterNode));

        auto scatterNode = std::make_unique<FieldScatterNode>();
        scatterNode->target_count = 2000;
        scatterNode->bounds_min = primal::math::v3{-1, -1, -1};
        scatterNode->bounds_max = primal::math::v3{11, 1, 1};
        scatterNode->seed = 42;
        scatterNode->points_per_unit_area = 1.0f;
        u32 sid = graph.AddNode(std::move(scatterNode));

        graph.Connect(rid, 0, sid, 0);

        // Execute
        graph.Execute();

        auto* points = graph.GetOutputPoints(sid);
        check(points != nullptr, "Graph produced output");
        check(points->count > 0, "Graph produced > 0 points");

        // Verify clustering: >60% of points within 1.5 units of line
        u32 near_count = 0;
        for (u32 i = 0; i < points->count; ++i) {
            auto& p = points->positions[i];
            // Distance to line segment (0,0,0)→(10,0,0) in XZ-like plane
            // Project onto line axis
            f32 t = std::clamp(p.x, 0.0f, 10.0f);
            f32 dx = p.x - t;
            f32 dy = p.y;
            f32 dz = p.z;
            f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < 1.5f) near_count++;
        }
        f32 near_ratio = static_cast<f32>(near_count) / static_cast<f32>(points->count);
        check(near_ratio > 0.6f, ">60% of points cluster near the line");

        // Verify near points have higher density than far points
        f32 avg_density_near = 0, avg_density_far = 0;
        u32 cnt_near = 0, cnt_far = 0;
        for (u32 i = 0; i < points->count; ++i) {
            auto& p = points->positions[i];
            f32 t = std::clamp(p.x, 0.0f, 10.0f);
            f32 dx = p.x - t;
            f32 dy = p.y;
            f32 dz = p.z;
            f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            f32 density = points->GetAttr(i, primal::graphics::pcg::PCGAttr::Density);
            if (dist < 1.5f) { avg_density_near += density; cnt_near++; }
            else { avg_density_far += density; cnt_far++; }
        }
        if (cnt_near > 0) avg_density_near /= cnt_near;
        if (cnt_far > 0) avg_density_far /= cnt_far;
        check(avg_density_near > avg_density_far, "Near points have higher density than far points");

        std::cout << "OK (points=" << points->count
                  << ", near=" << (near_ratio * 100) << "%"
                  << ", density_near=" << avg_density_near
                  << ", density_far=" << avg_density_far << ")" << std::endl;

        primal::geometry::destroy(h);
    }

    // Verify RasterizedField serializes/deserializes correctly
    void test_serializer_roundtrip() {
        std::cout << "[SerializerRoundtrip] ";

        using namespace primal::graphics::pcg;

        // Create node via factory
        auto node = PCGSerializer::CreateNode("RasterizedField");
        check(node != nullptr, "CreateNode('RasterizedField') returns non-null");
        check(std::strcmp(node->TypeName(), "RasterizedField") == 0, "TypeName = RasterizedField");

        // Set scalar params via SetParamByName
        check(node->SetParamByName("resolution_x", 64.0f), "Set resolution_x");
        check(node->SetParamByName("band_width", 3.5f), "Set band_width");

        auto* rn = static_cast<RasterizedFieldNode*>(node.get());
        check(rn->resolution_x == 64, "resolution_x == 64");
        check(std::abs(rn->band_width - 3.5f) < 0.01f, "band_width == 3.5");

        // Set vec3 params
        check(node->SetParamByName("bounds_min", primal::math::v3{-5, -1, -5}), "Set bounds_min");
        check(node->SetParamByName("bounds_max", primal::math::v3{15, 3, 15}), "Set bounds_max");
        approx(rn->bounds_min.x, -5.0f, "bounds_min.x == -5");
        approx(rn->bounds_max.z, 15.0f, "bounds_max.z == 15");

        // Verify pin descriptors
        u32 pin_count;
        auto* pins = node->GetPinDescriptors(pin_count);
        check(pin_count == 1, "RasterizedFieldNode has 1 pin");
        check(!pins[0].is_input, "Pin is output");
        check(pins[0].data_type == PCGDataType::Field, "Pin is Field type");

        // Verify param descriptors
        u32 param_count;
        auto* descs = node->GetParamDescriptors(param_count);
        check(param_count == 7, "RasterizedFieldNode has 7 params");

        // Verify registry
        check(PCGSerializer::GetRegisteredNodeTypeCount() == 11, "11 registered node types");
        check(std::strcmp(PCGSerializer::GetRegisteredNodeTypeName(10), "SurfaceScatter") == 0,
              "Type 10 = SurfaceScatter");
        check(std::strcmp(PCGSerializer::GetRegisteredNodeTypeName(7), "RasterizedField") == 0,
              "Type 7 = RasterizedField");

        // Serialize single node and verify key content
        std::string serialized = PCGSerializer::SerializeNode(*node);
        check(serialized.find("RasterizedField") != std::string::npos, "Serialized contains type name");
        check(serialized.find("band_width") != std::string::npos, "Serialized contains band_width");
        check(serialized.find("bounds_min") != std::string::npos, "Serialized contains bounds_min");

        std::cout << "OK" << std::endl;
    }
};
