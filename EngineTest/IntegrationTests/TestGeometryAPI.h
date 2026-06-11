#pragma once

#include "Test.h"
#include "Engine/Geometry/Geometry.h"
#include "Engine/Geometry/GeometryFieldRasterizer.h"
#include "Engine/Geometry/GeometryTypes.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Geometry.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#include <iostream>
#include <cassert>
#include <cmath>
#include <ctime>

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
        std::cout << "=== Geometry API Integration Test ===" << std::endl;
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
        test_line();
        test_arc_three_point();
        test_arc_parametric();
        test_spline();
        test_polyline();
        test_control_point_modification();
        test_bounding_box();
        test_distance_to();
        test_tessellation_cache();
        test_field_rasterization();
        test_geometry_component();
        std::cout << "\n=== All Geometry API tests passed ===" << std::endl;
    }

    void test_line() {
        std::cout << "[Line] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,0,0});
        pts.push_back(primal::math::v3{1,1,0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);
        check(h.is_valid(), "line handle valid");

        auto& cp = primal::geometry::get_control_points(h);
        check(cp.size() == 3, "line 3 control points");

        auto& tess = primal::geometry::tessellate(h, 0.01f);
        check(tess.size() >= 3, "line tessellation >= 3 points");

        f32 len = primal::geometry::compute_total_length(h);
        approx(len, 2.0f, "line total length == 2.0");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_arc_three_point() {
        std::cout << "[Arc3P] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{1,0,0});
        pts.push_back(primal::math::v3{0,1,0});
        pts.push_back(primal::math::v3{-1,0,0});
        auto h = primal::geometry::create_arc_three_point(pts);
        check(h.is_valid(), "arc3p handle valid");

        auto& tess = primal::geometry::tessellate(h, 0.01f);
        check(tess.size() >= 3, "arc3p tessellation >= 3 points");

        f32 len = primal::geometry::compute_total_length(h);
        check(len > 0.1f, "arc3p length > 0");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_arc_parametric() {
        std::cout << "[ArcParam] ";
        std::vector<primal::math::v3> endpoints;
        endpoints.push_back(primal::math::v3{1,0,0});
        endpoints.push_back(primal::math::v3{0,1,0});
        auto h = primal::geometry::create_arc_parametric(endpoints, 1.0f, 0.0f, 1.5707963f);
        check(h.is_valid(), "arc param handle valid");

        auto& tess = primal::geometry::tessellate(h, 0.01f);
        check(tess.size() >= 2, "arc param tessellation >= 2 points");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_spline() {
        std::cout << "[Spline] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,1,0});
        pts.push_back(primal::math::v3{2,0,0});
        pts.push_back(primal::math::v3{3,1,0});
        auto h = primal::geometry::create_spline(pts, false);
        check(h.is_valid(), "spline handle valid");

        auto& tess = primal::geometry::tessellate(h, 0.01f);
        check(tess.size() >= 4, "spline tessellation >= 4 points");

        auto p0 = primal::geometry::compute_point_at(h, 0.0f);
        approx(p0.x, 0.0f, "spline point_at(0).x == 0");
        approx(p0.y, 0.0f, "spline point_at(0).y == 0");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_polyline() {
        std::cout << "[Polyline] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,0,0});
        pts.push_back(primal::math::v3{1,1,0});
        pts.push_back(primal::math::v3{0,1,0});
        std::vector<primal::geometry::SegmentType> segs = {
            primal::geometry::SegmentType::Line,
            primal::geometry::SegmentType::Line,
            primal::geometry::SegmentType::Line
        };
        auto h = primal::geometry::create_polyline(pts, segs);
        check(h.is_valid(), "polyline handle valid");

        auto& tess = primal::geometry::tessellate(h, 0.01f);
        check(tess.size() >= 4, "polyline tessellation >= 4 points");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_control_point_modification() {
        std::cout << "[CtrlPtMod] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,0,0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);

        primal::geometry::set_control_point(h, 0, primal::math::v3{5, 0, 0});
        auto& cp = primal::geometry::get_control_points(h);
        approx(cp[0].x, 5.0f, "set_control_point x == 5");

        std::vector<primal::math::v3> new_pts;
        new_pts.push_back(primal::math::v3{0,0,0});
        new_pts.push_back(primal::math::v3{2,0,0});
        new_pts.push_back(primal::math::v3{2,2,0});
        primal::geometry::set_control_points(h, new_pts);
        check(primal::geometry::get_control_point_count(h) == 3, "set_control_points count == 3");

        primal::geometry::append_point(h, primal::math::v3{2, 2, 2});
        check(primal::geometry::get_control_point_count(h) == 4, "append_point count == 4");

        primal::geometry::insert_point(h, 1, primal::math::v3{0.5f, 0, 0});
        check(primal::geometry::get_control_point_count(h) == 5, "insert_point count == 5");

        primal::geometry::remove_point(h, 1);
        check(primal::geometry::get_control_point_count(h) == 4, "remove_point count == 4");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_bounding_box() {
        std::cout << "[BBox] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{1,2,3});
        pts.push_back(primal::math::v3{4,5,6});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);
        auto bb = primal::geometry::compute_bounding_box(h);

        approx(bb.min.x, 1.0f, "bbox min.x");
        approx(bb.min.y, 2.0f, "bbox min.y");
        approx(bb.min.z, 3.0f, "bbox min.z");
        approx(bb.max.x, 4.0f, "bbox max.x");
        approx(bb.max.y, 5.0f, "bbox max.y");
        approx(bb.max.z, 6.0f, "bbox max.z");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_distance_to() {
        std::cout << "[DistTo] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,0,0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);

        f32 d = primal::geometry::distance_to(h, primal::math::v3{0.5f, 1.0f, 0.0f});
        approx(d, 1.0f, "distance_to midpoint perpendicular");

        d = primal::geometry::distance_to(h, primal::math::v3{0.5f, 0.0f, 0.0f});
        approx(d, 0.0f, "distance_to on line");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_tessellation_cache() {
        std::cout << "[TessCache] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,0,0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);

        auto& tess1 = primal::geometry::tessellate(h, 0.01f);
        auto& tess2 = primal::geometry::tessellate(h, 0.01f);
        check(&tess1 == &tess2, "tessellation cache returns same reference");

        primal::geometry::set_control_point(h, 0, primal::math::v3{0.5f, 0, 0});
        auto& tess3 = primal::geometry::tessellate(h, 0.01f);
        check(&tess1 != &tess3 || tess3[0].x != 0.0f, "tessellation invalidated on modify");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_field_rasterization() {
        std::cout << "[FieldRaster] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,0,0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);

        primal::geometry::FieldRasterizeParams params;
        params.bounds_min = primal::math::v3{-1, -1, -1};
        params.bounds_max = primal::math::v3{2, 1, 1};
        params.resolution_x = 4;
        params.resolution_y = 2;
        params.resolution_z = 2;
        params.band_width = 1.0f;

        std::vector<primal::geometry::GeometryHandle> handles = {h};
        auto output = primal::geometry::rasterize(handles, params);

        check(output.data.size() == 4*2*2, "field rasterize output size");
        check(!output.data.empty(), "field rasterize non-empty");

        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }

    void test_geometry_component() {
        std::cout << "[GeomComponent] ";
        std::vector<primal::math::v3> pts;
        pts.push_back(primal::math::v3{0,0,0});
        pts.push_back(primal::math::v3{1,1,0});
        auto h = primal::geometry::create(primal::geometry::GeometryType::Line, pts);

        primal::transform::init_info tf{};
        tf.position[0] = 0; tf.position[1] = 0; tf.position[2] = 0;
        tf.rotation[0] = 0; tf.rotation[1] = 0;
        tf.rotation[2] = 0; tf.rotation[3] = 1;
        tf.scale[0] = 1; tf.scale[1] = 1; tf.scale[2] = 1;

        primal::geometry::component::init_info gi{};
        gi.handle = h;

        primal::game_entity::entity_info info{};
        info.transform = &tf;
        info.geometry = &gi;

        auto entity = primal::game_entity::create(info);
        check(entity.is_valid(), "geometry entity valid");

        auto geom = primal::geometry::component::get(entity.get_id());
        check(geom.is_valid(), "geometry component valid");
        check(geom.handle().is_valid(), "geometry component handle valid");

        auto& tess = geom.tessellate();
        check(tess.size() >= 2, "geometry component tessellation");

        auto bb = geom.bounding_box();
        check(bb.min.x <= 0.0f && bb.max.x >= 1.0f, "geometry component bbox");

        primal::game_entity::remove(entity.get_id());
        primal::geometry::destroy(h);
        std::cout << "OK" << std::endl;
    }
};
