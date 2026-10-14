// TestNavigationSerialize.cpp — ai-navigation S1 序列化单测（无 GPU 依赖）。
//
// 对应合同：保存→重开→已验结果不变（A03）、坏数据/版本差异给类型化错误（A02）。

#include "NavigationTestFixtures.h"
#include "Navigation/NavigationService.h"

#include <cstdio>
#include <iostream>
#include <string>

using namespace primal::navigation;
namespace math = primal::math;
namespace ntest = primal::navigation::test;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestNavigationSerialize] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestNavigationSerialize] PASS: " << name << std::endl;
    }
}

navigation_asset make_scene()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 10.f, 10.f);
    mb.box(4.f, 0.f, 4.f, 6.f, 2.f, 6.f);   // 中央墙 → 两块连通域被分割成绕行
    navigation_asset a;
    asset_bake(a, mb.input(), agent_profile{});
    return a;
}

void test_round_trip()
{
    const navigation_asset a = make_scene();

    std::string text;
    check(serialize(a.grid, text) == nav_error::ok, "roundtrip: serialize ok");

    nav_grid loaded;
    check(deserialize(text.c_str(), text.size(), loaded) == nav_error::ok, "roundtrip: deserialize ok");
    check(loaded.width == a.grid.width && loaded.depth == a.grid.depth, "roundtrip: dims match");
    check(loaded.region_count == a.grid.region_count, "roundtrip: regions match");
    check(loaded.walkable_span_count == a.grid.walkable_span_count, "roundtrip: walkable count match");
    check(loaded.source_hash == a.grid.source_hash, "roundtrip: source hash preserved");
    check(ntest::near3(loaded.bmin, a.grid.bmin, 1e-5f) && ntest::near3(loaded.bmax, a.grid.bmax, 1e-5f),
          "roundtrip: bounds match");

    const math::v3 from{ 1.f, 0.f, 1.f }, to{ 9.f, 0.f, 9.f };
    const path_result r1 = find_path(a.grid, from, to, 1.f);
    const path_result r2 = find_path(loaded, from, to, 1.f);
    check(r1.error == nav_error::ok && r2.error == nav_error::ok, "roundtrip: paths ok");
    bool same = r1.waypoints.size() == r2.waypoints.size();
    if (same) {
        for (size_t i = 0; i < r1.waypoints.size(); ++i)
            if (!ntest::near3(r1.waypoints[i], r2.waypoints[i], 1e-4f)) {
                same = false;
                break;
            }
    }
    check(same, "roundtrip: same waypoints");

    // 不可达语义保留：墙两侧... 本场景墙不可翻越（2m > step），角到角可达已验；
    // 断开域场景验证：
    ntest::mesh_builder isl;
    isl.quad_y(0.f, 0.f, 0.f, 2.f, 2.f);
    isl.quad_y(0.f, 5.f, 0.f, 7.f, 2.f);
    nav_grid gi, go;
    bake(isl.input(), agent_profile{}, gi);
    std::string ti;
    serialize(gi, ti);
    deserialize(ti.c_str(), ti.size(), go);
    check(find_path(go, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 6.f, 0.f, 1.f }, 1.f).error
              == nav_error::unreachable, "roundtrip: unreachable preserved");
}

void test_corrupted_inputs()
{
    const navigation_asset a = make_scene();
    std::string text;
    serialize(a.grid, text);

    nav_grid g;
    check(deserialize("", 0, g) == nav_error::bad_format, "corrupt: empty");
    check(deserialize("GARBAGE 1", 9, g) == nav_error::bad_format, "corrupt: bad magic");

    const std::string truncated = text.substr(0, text.size() / 2);
    check(deserialize(truncated.c_str(), truncated.size(), g) == nav_error::bad_format,
          "corrupt: truncated");

    std::string v99 = text;
    const size_t pos = v99.find("PRIMALNAV 1");
    v99.replace(pos, strlen("PRIMALNAV 1"), "PRIMALNAV 99");
    check(deserialize(v99.c_str(), v99.size(), g) == nav_error::unsupported_version,
          "corrupt: version mismatch typed error");

    // 篡改列数 → 结构不一致
    std::string badcols = text;
    const size_t cpos = badcols.find("columns ");
    badcols.replace(cpos + strlen("columns "), std::string::npos, "999999\n");
    check(deserialize(badcols.c_str(), badcols.size(), g) == nav_error::bad_format,
          "corrupt: column count mismatch");
}

void test_asset_file_round_trip()
{
    const char* path = "/tmp/primal_nav_ut.navtext";

    const navigation_asset a = make_scene();
    check(asset_save(a, path) == nav_error::ok, "file: save ok");

    navigation_asset b;
    check(asset_load(b, path) == nav_error::ok, "file: load ok");
    check(b.generation == 1, "file: generation assigned");
    check(b.grid.width == a.grid.width && b.grid.walkable_span_count == a.grid.walkable_span_count,
          "file: grid matches");

    const math::v3 from{ 1.f, 0.f, 1.f }, to{ 9.f, 0.f, 9.f };
    const path_result rb = find_path(b.grid, from, to, 1.f);
    check(rb.error == nav_error::ok && !rb.waypoints.empty(), "file: query after reload");

    check(asset_load(b, "/tmp/primal_nav_ut_does_not_exist.navtext") == nav_error::bad_format,
          "file: missing file typed error");
    check(asset_save(a, "") == nav_error::null_input, "file: empty path typed error");

    std::remove(path);   // 磁盘纪律：测试临时文件即清
}

} // anonymous namespace

int main()
{
    std::cout << "[TestNavigationSerialize] === begin ===" << std::endl;
    test_round_trip();
    test_corrupted_inputs();
    test_asset_file_round_trip();
    std::cout << "[TestNavigationSerialize] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
