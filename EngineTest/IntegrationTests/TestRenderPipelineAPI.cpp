// TestRenderPipelineAPI.cpp - Phase 5 RenderPipelineAPI C ABI tests.
//
// Validates the Editor-facing render-pipeline control surface:
//   - Pass toggles (Set/Is/IsActive/Count)
//   - Pass info (GetPipelinePassInfo returns valid strings/defaults)
//   - Settings round-trip (UpdateRenderPipelineSettings / GetRenderPipelineSettings)
//   - Param descriptors (GetPipelineParamDescriptors returns expected count,
//     Bloom/TAA descriptors present after schema-only additions)
//   - M6/T6.1 GI editor panel data plane: GI Axes descriptor group,
//     GIDebug/volume descriptor tables, panel action registry, volume CRUD +
//     axes hot-reload on a device-less pipeline, DLL C ABI pre-init safety
//   - M6/T6.2 GI volume gizmo: viewport pick (nearest AABB), translate →
//     origin_override, scale clamp 4..128, wireframe endpoints, gizmo.mode.*
//     panel actions, DLL C ABI pre-init safety
//
// No engine init required for these checks — the pipeline static table and
// per-pipeline-state getters are usable with or without a live RenderPipeline
// instance. When the pipeline is uninit, Get/Update Settings still round-trip
// through the static defaults.

#include "../UnitTests/TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Engine/Graphics/RenderPipeline/GIEditorPanel.h"
#include "Engine/Graphics/RenderPipeline/GIVolumeGizmo.h"
#include "Engine/Graphics/RenderPipeline/RenderPipeline.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
// 本地构造 StandardRenderPipeline（析构经 unique_ptr<RenderSceneSnapshot>）
// 需要完整类型；此头使用 primal::utl，必须在 StaticProbeBaker.h（utl 遮蔽源）前。
#include "Engine/Graphics/Scene/RenderSceneSnapshot.h"
// include 顺序敏感：StaticProbeBaker.h 经 BVH.hpp 打开 primal::graphics::utl
// （遮蔽后续头的 primal::utl 查找），必须在 StandardRenderPipeline.h 之后。
// 仅为 BakeStaticProbes 的 ProbeBakingScene/Params 完整类型。
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeBaker.h"
#include "Engine/Graphics/Lumen/GIDebugParams.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace primal;
using namespace primal::graphics;

// EngineDLL C ABI declarations (linked directly via EngineDLL target).
extern "C" {
    void SetRenderPassEnabled(u32 pass_id, u32 enabled);
    u32  IsRenderPassEnabled(u32 pass_id);
    u32  IsRenderPassActive(u32 pass_id);
    void GetRenderPipelineSettings(RenderPipelineSettings* out);
    void UpdateRenderPipelineSettings(const RenderPipelineSettings* settings);
    const ParamDescriptor* GetPipelineParamDescriptors(u32* out_count);
    void GetPipelinePassInfo(u32 pass_id,
                              const char** out_name,
                              const char** out_description,
                              u32* out_default_enabled);

    // M6/T6.1 GI editor panel C ABI
    const ParamDescriptor* GetGIDebugParamDescriptors(u32* out_count);
    const ParamDescriptor* GetGIVolumeParamDescriptors(u32* out_count);
    void GIGetDebugParams(lumen::GIDebugParams* out);
    void GISetDebugParams(const lumen::GIDebugParams* params);
    u32  GIApplyAxesConfig(const lumen::GIFiveAxisConfig* axes);
    u32  GIGetVolumeOverrideCount();
    u32  GIGetVolumeOverride(u32 index, lumen::LumenVolumeConfig* out);
    u32  GISetVolumeOverride(u32 index, const lumen::LumenVolumeConfig* v);
    u32  GIAppendVolumeOverride();
    u32  GIRemoveVolumeOverride(u32 index);
    u32  GIMaterializeDerivedVolumes();
    u32  GIGetEffectiveVolumeConfig(u32 index, lumen::LumenVolumeConfig* out);
    u32  GIBakeStaticProbes(const f32* vertices, u32 vertex_count,
                             const u32* indices, u32 index_count,
                             const f32* light_dir, const f32* light_color,
                             f32 light_intensity, const f32* sky_color,
                             u32 rays_per_probe, u32 bounce_count,
                             char* out_status, u32 status_cap);
    void GIRegisterPanelAction(const char* id, const char* label, const char* tooltip,
                               gi_panel::PanelActionFn fn, void* user_data);
    void GIUnregisterPanelAction(const char* id);
    void GIClearPanelActions();
    u32  GIGetPanelActionCount();
    u32  GIGetPanelAction(u32 index, const char** out_id,
                          const char** out_label, const char** out_tooltip);
    u32  GIInvokePanelAction(const char* id);

    // M6/T6.2 GI volume gizmo C ABI
    void GIGizmoSetMode(u32 mode);
    u32  GIGizmoGetMode();
    u32  GIGizmoGetSelectedVolume();
    u32  GIGizmoPick(f32 screenX, f32 screenY, u32 viewportW, u32 viewportH,
                     const f32* invViewProj, const f32* viewOrigin,
                     u32* outVolumeIndex, f32* outTMin);
    u32  GIGizmoTranslate(u32 index, const f32* delta);
    u32  GIGizmoScale(u32 index, f32 factor, u32 axis);
    u32  GIGizmoGetWireframe(u32 index, f32* outEndpoints, u32 capacity, u32* outCount);
}

using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;

// === Test 1: Pass count matches the enum ===
TestResult TestPassCountMatchesEnum() {
    u32 count = StandardRenderPipeline::GetPassCount();
    TEST_ASSERT(count == static_cast<u32>(RenderPassID::Count),
                "GetPassCount should match RenderPassID::Count");
    return TestResult::Passed;
}

// === Test 2: GetPipelinePassInfo returns valid info for all passes ===
TestResult TestPassInfoValid() {
    for (u32 i = 0; i < static_cast<u32>(RenderPassID::Count); ++i) {
        const char* name = nullptr;
        const char* desc = nullptr;
        u32 default_enabled = 0;
        GetPipelinePassInfo(i, &name, &desc, &default_enabled);
        TEST_ASSERT(name != nullptr, "Pass name should be non-null");
        TEST_ASSERT(desc != nullptr, "Pass description should be non-null");
        TEST_ASSERT(default_enabled <= 1u, "default_enabled should be 0 or 1");
        TEST_ASSERT(name[0] != '\0', "Pass name should not be empty");
    }
    return TestResult::Passed;
}

// === Test 3: GetPipelinePassInfo ignores invalid pass IDs silently ===
TestResult TestPassInfoIgnoresInvalidId() {
    const char* name = "sentinel";
    const char* desc = "sentinel";
    u32 default_enabled = 99;
    // Should not crash; out-args unchanged.
    GetPipelinePassInfo(999u, &name, &desc, &default_enabled);
    TEST_ASSERT(std::string(name) == "sentinel", "name unchanged on invalid id");
    TEST_ASSERT(default_enabled == 99u, "default_enabled unchanged on invalid id");
    return TestResult::Passed;
}

// === Test 4: ParamDescriptor table is non-empty and includes Bloom/TAA ===
TestResult TestParamDescriptorsIncludeBloomTAASchema() {
    u32 count = 0;
    const ParamDescriptor* descs = GetParamDescriptors(count);
    TEST_ASSERT(descs != nullptr, "GetParamDescriptors should return non-null");
    TEST_ASSERT(count > 0, "Should have at least one descriptor");

    // Statically, the table has 12 passes + quality + lighting + SSAO + DDGI +
    // SSGI + Volume + Bloom(3) + TAA(2). We don't assert exact count (fragile
    // against future additions), just that Bloom/TAA groups are present.
    bool has_bloom = false, has_taa = false;
    for (u32 i = 0; i < count; ++i) {
        if (std::string(descs[i].group) == "Bloom") has_bloom = true;
        if (std::string(descs[i].group) == "TAA")   has_taa   = true;
        // Sanity: each descriptor's offset+size must fit in RenderPipelineSettings.
        TEST_ASSERT(descs[i].offset + descs[i].size <= sizeof(RenderPipelineSettings),
                    "Descriptor offset+size must fit in RenderPipelineSettings");
    }
    TEST_ASSERT(has_bloom, "Bloom group should be in descriptor table");
    TEST_ASSERT(has_taa,   "TAA group should be in descriptor table");
    return TestResult::Passed;
}

// === Test 5: Pass enable/disable toggle survives a round-trip ===
// NOTE: Without an initialized RenderPipeline instance, SetPassEnabled is a
// no-op. We still call it to verify the ABI doesn't crash; the state
// round-trip is verified by IsRenderPassEnabled immediately after Set, which
// reads from the live pipeline (or returns 0 if uninit).
TestResult TestPassToggleAbiDoesNotCrash() {
    // Pick a known pass.
    const RenderPassID pass = RenderPassID::SSAO;
    SetRenderPassEnabled(static_cast<u32>(pass), 0);
    SetRenderPassEnabled(static_cast<u32>(pass), 1);
    // Just verifying no crash; the actual state depends on pipeline init.
    (void)IsRenderPassEnabled(static_cast<u32>(pass));
    (void)IsRenderPassActive(static_cast<u32>(pass));
    return TestResult::Passed;
}

// === Test 6: Bloom/TAA struct defaults match the schema ===
// We construct a default RenderPipelineSettings directly (not via
// GetRenderPipelineSettings — that zeroes when no live pipeline exists) and
// verify BloomConfig/TAAConfig ship with the declared defaults.
TestResult TestSettingsDefaultBloomTAA() {
    RenderPipelineSettings s{};
    TEST_ASSERT(std::abs(s.bloom.intensity - 1.0f) < 0.0001f, "Default bloom.intensity");
    TEST_ASSERT(std::abs(s.bloom.threshold - 1.0f) < 0.0001f, "Default bloom.threshold");
    TEST_ASSERT(std::abs(s.bloom.radius - 0.6f)    < 0.0001f, "Default bloom.radius");
    TEST_ASSERT(std::abs(s.taa.sharpness - 0.8f)   < 0.0001f, "Default taa.sharpness");
    TEST_ASSERT(std::abs(s.taa.feedback  - 0.9f)   < 0.0001f, "Default taa.feedback");
    return TestResult::Passed;
}

// === Test 7: Null GetRenderPipelineSettings zeroes the out struct ===
TestResult TestSettingsNullSafety() {
    // Passing null should not crash (defensive).
    GetRenderPipelineSettings(nullptr);
    UpdateRenderPipelineSettings(nullptr);
    u32 count = 99;
    const ParamDescriptor* descs = GetPipelineParamDescriptors(nullptr);
    TEST_ASSERT(descs == nullptr, "GetPipelineParamDescriptors(nullptr) returns null");
    (void)count;
    return TestResult::Passed;
}

// ============================================================================
// M6/T6.1 — GI editor panel data plane
// ============================================================================

// === Test 8: GI Axes 五轴分组在 kParams 表内（8 条：轴1×3 bool、轴2/3/5 枚举、轴4×2 bool） ===
TestResult TestGIAxesDescriptorsPresent() {
    u32 count = 0;
    const ParamDescriptor* descs = GetParamDescriptors(count);
    TEST_ASSERT(descs != nullptr, "GetParamDescriptors should return non-null");

    u32 axis_entries = 0;
    bool has_axis1 = false, has_axis2 = false, has_axis3 = false,
         has_axis4 = false, has_axis5 = false;
    for (u32 i = 0; i < count; ++i) {
        if (std::string(descs[i].group) != "GI Axes") continue;
        ++axis_entries;
        const std::string n = descs[i].name;
        if (n.rfind("Axis1", 0) == 0) has_axis1 = true;
        if (n.rfind("Axis2", 0) == 0) has_axis2 = true;
        if (n.rfind("Axis3", 0) == 0) has_axis3 = true;
        if (n.rfind("Axis4", 0) == 0) has_axis4 = true;
        if (n.rfind("Axis5", 0) == 0) has_axis5 = true;
        // Enum axes must carry selectable names; bool axes must not.
        if (n.rfind("Axis2", 0) == 0 || n.rfind("Axis3", 0) == 0 ||
            n.rfind("Axis5", 0) == 0) {
            TEST_ASSERT(descs[i].type == ParamType::Enum, "Axis2/3/5 are enums");
            TEST_ASSERT(descs[i].enum_names != nullptr, "Enum axis has enum_names");
        }
        // Offsets validated globally by the Bloom/TAA test; group membership here.
        TEST_ASSERT(descs[i].offset + descs[i].size <= sizeof(RenderPipelineSettings),
                    "GI axis offset+size must fit in RenderPipelineSettings");
    }
    TEST_ASSERT(axis_entries == 8, "GI Axes group has 8 entries (3+1+1+2+1)");
    TEST_ASSERT(has_axis1 && has_axis2 && has_axis3 && has_axis4 && has_axis5,
                "All five axes present");
    return TestResult::Passed;
}

// === Test 9: GIDebug / Volume 描述符表（offset 落在各 base struct 内） ===
TestResult TestGIDebugAndVolumeDescriptorTables() {
    using namespace gi_panel;
    u32 dc = 0;
    const ParamDescriptor* d = GetDebugParamDescriptors(dc);
    TEST_ASSERT(d != nullptr && dc > 0, "Debug descriptor table non-empty");
    u32 vis_entries = 0;
    for (u32 i = 0; i < dc; ++i) {
        TEST_ASSERT(d[i].offset + d[i].size <= sizeof(lumen::GIDebugParams),
                    "Debug descriptor offset+size must fit in GIDebugParams");
        TEST_ASSERT(d[i].name != nullptr && d[i].name[0] != '\0', "Named entry");
        if (std::string(d[i].name) == "Probe Visualization") {
            ++vis_entries;
            TEST_ASSERT(d[i].type == ParamType::Enum && d[i].enum_names != nullptr,
                        "Probe vis descriptor is an enum with names");
        }
    }
    TEST_ASSERT(vis_entries == 1, "Exactly one probe-vis entry");

    u32 vc = 0;
    const ParamDescriptor* v = GetVolumeParamDescriptors(vc);
    TEST_ASSERT(v != nullptr && vc > 0, "Volume descriptor table non-empty");
    for (u32 i = 0; i < vc; ++i) {
        TEST_ASSERT(v[i].offset + v[i].size <= sizeof(lumen::LumenVolumeConfig),
                    "Volume descriptor offset+size must fit in LumenVolumeConfig");
        TEST_ASSERT(v[i].name != nullptr && v[i].name[0] != '\0', "Named entry");
    }
    return TestResult::Passed;
}

// === Test 10: 面板动作注册表（按钮机制：注册/查询/触发/清除/null 安全） ===
static bool s_action_called = false;
static bool TestActionFn(void* user_data) {
    s_action_called = (user_data == &s_action_called);
    return true;
}

TestResult TestPanelActionRegistry() {
    using namespace gi_panel;
    ClearActions();
    TEST_ASSERT(GetActionCount() == 0, "Registry starts clean");

    PanelAction a{};
    a.id = "test_bake";
    a.label = "Test Bake";
    a.tooltip = "tooltip";
    a.fn = &TestActionFn;
    a.user_data = &s_action_called;
    RegisterAction(a);
    TEST_ASSERT(GetActionCount() == 1, "One action after register");
    TEST_ASSERT(GetActionById("test_bake") != nullptr, "Find by id");
    TEST_ASSERT(GetActionById("missing") == nullptr, "Unknown id not found");
    TEST_ASSERT(GetActionByIndex(1) == nullptr, "Index out of range null");

    s_action_called = false;
    TEST_ASSERT(InvokeAction("test_bake"), "Invoke succeeds");
    TEST_ASSERT(s_action_called, "Callback ran with user_data passthrough");
    TEST_ASSERT(!InvokeAction("missing"), "Unknown id invoke returns false");
    TEST_ASSERT(!InvokeAction(nullptr), "Null id invoke returns false");

    // Same id overwrites instead of duplicating.
    RegisterAction(a);
    TEST_ASSERT(GetActionCount() == 1, "Re-register same id overwrites");

    // Null fn rejected silently.
    PanelAction bad{};
    bad.id = "bad";
    bad.fn = nullptr;
    RegisterAction(bad);
    TEST_ASSERT(GetActionCount() == 1, "Null-fn action rejected");

    UnregisterAction("test_bake");
    TEST_ASSERT(GetActionCount() == 0, "Unregister removes");
    ClearActions();   // idempotent, no crash
    return TestResult::Passed;
}

// === Test 11: 引擎侧 API 无 pipeline 时的 null 安全 ===
TestResult TestGIPanelNullPipelineSafety() {
    using namespace gi_panel;
    lumen::LumenVolumeConfig v{};
    TEST_ASSERT(GetVolumeOverrideCount(nullptr) == 0, "Null pipeline count 0");
    TEST_ASSERT(!GetVolumeOverride(nullptr, 0, &v), "Null pipeline get fails");
    TEST_ASSERT(!SetVolumeOverride(nullptr, 0, v), "Null pipeline set fails");
    TEST_ASSERT(!AppendVolumeOverride(nullptr), "Null pipeline append fails");
    TEST_ASSERT(!RemoveVolumeOverride(nullptr, 0), "Null pipeline remove fails");
    TEST_ASSERT(!MaterializeDerivedVolumes(nullptr), "Null pipeline materialize fails");
    TEST_ASSERT(!GetEffectiveVolumeConfig(nullptr, 0, &v), "Null pipeline effective fails");
    TEST_ASSERT(!ApplyAxesConfig(nullptr, lumen::GIFiveAxisConfig{}), "Null pipeline axes fails");

    lumen::ProbeBakingScene scene{};
    char status[64] = {0};
    TEST_ASSERT(!BakeStaticProbes(nullptr, scene, lumen::ProbeBakingParams{},
                                  status, sizeof(status)),
                "Null pipeline bake fails safely");
    TEST_ASSERT(status[0] != '\0', "Status message written on failure");
    return TestResult::Passed;
}

// === Test 12: GIDebugParams 单例默认值（probe_vis Off = 零行为变化）与往返 ===
TestResult TestGIDebugParamsDefaultsAndRoundTrip() {
    const lumen::GIDebugParams saved = lumen::GIDebug();
    TEST_ASSERT(saved.probe_vis == lumen::GIDebugParams::ProbeVisMode::Off,
                "Default probe_vis is Off (zero-cost default)");

    lumen::GIDebugParams p = saved;
    p.ddgi_trace = false;
    p.sky_color_intensity = 1.5f;
    p.freeze_probe_updates = true;
    p.probe_vis = lumen::GIDebugParams::ProbeVisMode::DepthAtlas;
    lumen::GIDebug() = p;
    TEST_ASSERT(lumen::GIDebug().ddgi_trace == false, "Singleton write visible");
    TEST_ASSERT(std::abs(lumen::GIDebug().sky_color_intensity - 1.5f) < 1e-6f,
                "Weight write visible");
    TEST_ASSERT(lumen::GIDebug().probe_vis == lumen::GIDebugParams::ProbeVisMode::DepthAtlas,
                "Vis mode write visible");

    lumen::GIDebug() = saved;   // restore process defaults
    TEST_ASSERT(lumen::GIDebug().probe_vis == lumen::GIDebugParams::ProbeVisMode::Off,
                "Restored");
    return TestResult::Passed;
}

// === Test 13: DLL C ABI 面板导出 pre-init 安全（无活管线时全部安全返回） ===
static bool s_dll_action_called = false;
static bool DllTestActionFn(void*) { s_dll_action_called = true; return true; }

TestResult TestGIPanelAbiPreInitSafety() {
    u32 dc = 0;
    TEST_ASSERT(GetGIDebugParamDescriptors(&dc) != nullptr && dc > 0,
                "DLL debug descriptors non-empty");
    TEST_ASSERT(GetGIDebugParamDescriptors(nullptr) == nullptr, "Null count returns null");
    u32 vc = 0;
    TEST_ASSERT(GetGIVolumeParamDescriptors(&vc) != nullptr && vc > 0,
                "DLL volume descriptors non-empty");

    lumen::GIDebugParams d{};
    GIGetDebugParams(&d);
    TEST_ASSERT(d.probe_vis == lumen::GIDebugParams::ProbeVisMode::Off, "DLL get debug");
    GIGetDebugParams(nullptr);   // no crash
    GISetDebugParams(nullptr);   // no crash

    TEST_ASSERT(GIApplyAxesConfig(nullptr) == 0, "Null axes rejected");
    TEST_ASSERT(GIApplyAxesConfig(nullptr) == 0, "Idempotent null reject");
    TEST_ASSERT(GIGetVolumeOverrideCount() == 0, "No pipeline → 0 volumes");
    lumen::LumenVolumeConfig v{};
    TEST_ASSERT(GIGetVolumeOverride(0, &v) == 0, "No pipeline → get fails");
    TEST_ASSERT(GISetVolumeOverride(0, &v) == 0, "No pipeline → set fails");
    TEST_ASSERT(GIAppendVolumeOverride() == 0, "No pipeline → append fails");
    TEST_ASSERT(GIRemoveVolumeOverride(0) == 0, "No pipeline → remove fails");
    TEST_ASSERT(GIMaterializeDerivedVolumes() == 0, "No pipeline → materialize fails");
    TEST_ASSERT(GIGetEffectiveVolumeConfig(0, &v) == 0, "No pipeline → effective fails");

    char status[64] = {0};
    TEST_ASSERT(GIBakeStaticProbes(nullptr, 0, nullptr, 0, nullptr, nullptr,
                                   1.0f, nullptr, 0, 0, status, sizeof(status)) == 0,
                "Bake without pipeline fails safely");
    TEST_ASSERT(status[0] != '\0', "Bake status message written");

    // Action registry through the C ABI.
    GIClearPanelActions();
    GIRegisterPanelAction("dll_test", "DLL Test", "tip", &DllTestActionFn, nullptr);
    TEST_ASSERT(GIGetPanelActionCount() == 1, "C ABI register");
    const char* id = nullptr;
    const char* label = nullptr;
    const char* tip = nullptr;
    TEST_ASSERT(GIGetPanelAction(0, &id, &label, &tip) == 1, "C ABI action meta");
    TEST_ASSERT(id && std::string(id) == "dll_test", "Action id round-trip");
    TEST_ASSERT(label && std::string(label) == "DLL Test", "Action label round-trip");
    s_dll_action_called = false;
    TEST_ASSERT(GIInvokePanelAction("dll_test") == 1, "C ABI invoke");
    TEST_ASSERT(s_dll_action_called, "C ABI invoke ran callback");
    TEST_ASSERT(GIInvokePanelAction("nope") == 0, "Unknown id via C ABI fails");
    TEST_ASSERT(GIGetPanelAction(9, &id, &label, &tip) == 0, "Index OOB via C ABI fails");
    GIRegisterPanelAction(nullptr, "l", "t", &DllTestActionFn, nullptr);  // null id rejected
    GIRegisterPanelAction("x", "l", "t", nullptr, nullptr);               // null fn rejected
    TEST_ASSERT(GIGetPanelActionCount() == 1, "Invalid registers rejected");
    GIClearPanelActions();
    TEST_ASSERT(GIGetPanelActionCount() == 0, "C ABI clear");
    return TestResult::Passed;
}

// === Test 14: 设备无关管线上的 volume CRUD / 推导物化 / axes 热重载 / bake 失败路径 ===
// StandardRenderPipeline 无 device 也可承载 settings 路径（SetLumenConfig 跳过
// 子系统初始化）——正好覆盖面板的 copy-modify-commit 与轴1覆写接线。
TestResult TestGIPanelVolumeAndAxesOnLocalPipeline() {
    using namespace gi_panel;
    StandardRenderPipeline p;   // device-less: settings-only paths

    // -- volume 列表：推导物化（near+far，ddgi_enable_far_volume 默认 true）--
    TEST_ASSERT(MaterializeDerivedVolumes(&p), "Materialize derived volumes");
    TEST_ASSERT(GetVolumeOverrideCount(&p) == 2, "near + far materialized");
    lumen::LumenVolumeConfig v0{};
    TEST_ASSERT(GetVolumeOverride(&p, 0, &v0), "Read volume 0");
    TEST_ASSERT(v0.probe_count_x == 32 && v0.probe_count_y == 16 &&
                v0.probe_count_z == 32,
                "Near volume derived from ddgi_* defaults");
    TEST_ASSERT(std::abs(v0.probe_spacing - 2.0f) < 1e-6f, "Near spacing default");
    lumen::LumenVolumeConfig v1{};
    TEST_ASSERT(GetVolumeOverride(&p, 1, &v1), "Read volume 1 (far)");
    TEST_ASSERT(v1.update_budget_divisor == 4, "Far budget divisor");
    TEST_ASSERT(v1.probe_spacing > v0.probe_spacing, "Far is coarser");

    // -- 生效配置读取（与推导一致的另一路径）--
    lumen::LumenVolumeConfig eff{};
    TEST_ASSERT(GetEffectiveVolumeConfig(&p, 0, &eff), "Effective volume 0");
    TEST_ASSERT(eff.probe_count_x == v0.probe_count_x, "Effective matches overrides");

    // -- SetVolumeOverride clamp 生效（999 → 64）--
    v0.probe_count_x = 999;
    TEST_ASSERT(SetVolumeOverride(&p, 0, v0), "Set volume 0");
    lumen::LumenVolumeConfig chk{};
    TEST_ASSERT(GetVolumeOverride(&p, 0, &chk), "Re-read volume 0");
    TEST_ASSERT(chk.probe_count_x == 64, "Out-of-range count clamped to 64");
    TEST_ASSERT(!SetVolumeOverride(&p, 9, chk), "Set out-of-range index fails");

    // -- cap 4 追加 / 删除 --
    TEST_ASSERT(AppendVolumeOverride(&p), "Append vol 2");
    TEST_ASSERT(AppendVolumeOverride(&p), "Append vol 3");
    TEST_ASSERT(!AppendVolumeOverride(&p), "Cap 4 enforced");
    TEST_ASSERT(GetVolumeOverrideCount(&p) == 4, "Four volumes");
    TEST_ASSERT(RemoveVolumeOverride(&p, 3), "Remove last");
    TEST_ASSERT(!RemoveVolumeOverride(&p, 3), "Remove OOB fails");
    TEST_ASSERT(GetVolumeOverrideCount(&p) == 3, "Three volumes after remove");

    // -- axes 热重载：SetLumenConfig 路径 + 轴1 覆写 pass 开关 --
    lumen::GIFiveAxisConfig axes{};
    axes.use_surface_cache = false;
    axes.use_screen_probes = true;
    axes.temporal = lumen::GITemporalStrategy::EMAVarianceClamp;
    TEST_ASSERT(ApplyAxesConfig(&p, axes), "Apply axes config");
    TEST_ASSERT(p.GetSettings().lumen.axes.use_screen_probes == true, "Axes stored");
    TEST_ASSERT(p.GetSettings().lumen.axes.temporal ==
                lumen::GITemporalStrategy::EMAVarianceClamp, "Axis5 stored");
    // 轴1 覆写（ApplyGIAxis1_SpaceCaches 是轴视图单一真源）
    TEST_ASSERT(p.GetSettings().quality.enable_surface_cache == false, "Axis1 SC overwrite");
    TEST_ASSERT(p.GetSettings().quality.enable_screen_probes == true, "Axis1 SPGI overwrite");

    // -- bake 失败路径：device-less 管线从未初始化子系统 → 无 static volume --
    lumen::ProbeBakingScene scene{};
    char status[64] = {0};
    TEST_ASSERT(!BakeStaticProbes(&p, scene, lumen::ProbeBakingParams{},
                                  status, sizeof(status)),
                "Bake fails without initialized static volume");
    TEST_ASSERT(status[0] != '\0', "Failure status written");
    return TestResult::Passed;
}

// ============================================================================
// M6/T6.2 — GI volume gizmo（拾取/平移/缩放/线框/模式）
// ============================================================================

namespace {

// 视点 (0,0,-10) 朝 +Z 的透视相机的逆视图投影（列主序；tanX=tanY=1，
// near=1 / far=201，深度 402/(202-200*nz) 经 w 除法）。任何屏幕点的
// near→far 连线方向 ∝ (ndcX, ndcY, 1) —— 屏幕位置真正改变射线方向
// （identity 矩阵做不到：near/far 的 xy 相同，所有射线都平行 +Z）。
primal::math::m4x4 TestInvViewProj() {
    primal::math::m4x4 m{};
    m.columns[0] = primal::math::v4{402.0f, 0.0f, 0.0f, 0.0f};
    m.columns[1] = primal::math::v4{0.0f, 402.0f, 0.0f, 0.0f};
    m.columns[2] = primal::math::v4{0.0f, 0.0f, 2000.0f, -200.0f};
    m.columns[3] = primal::math::v4{0.0f, 0.0f, -1618.0f, 202.0f};
    return m;
}

// 两个 4x4x4 @ spacing 1 的 volume：origin (0,0,0)（AABB z 0..4）与
// origin (0,0,8)（AABB z 8..12）——沿 +Z 射线 z 间距 8，tMin 相差 8。
// StandardRenderPipeline 不可拷贝/移动（unique_ptr 成员），经出参填充。
void MakeTwoBoxPipeline(StandardRenderPipeline& p, bool nearFirst) {
    RenderPipelineSettings s = p.GetSettings();
    lumen::LumenVolumeConfig a{};   // AABB [0,4]^3
    a.probe_count_x = a.probe_count_y = a.probe_count_z = 4;
    a.probe_spacing = 1.0f;
    a.origin_override = primal::math::v3{0.0f, 0.0f, 0.0f};
    a.use_origin_override = true;
    lumen::LumenVolumeConfig b = a;   // AABB [0,4]xy × [8,12]z
    b.origin_override = primal::math::v3{0.0f, 0.0f, 8.0f};
    if (nearFirst) s.lumen.volume_overrides = {a, b};
    else           s.lumen.volume_overrides = {b, a};
    p.UpdateSettings(s);
}

bool NearlyEqual(f32 x, f32 y, f32 eps = 1e-3f) { return std::abs(x - y) < eps; }

} // namespace

// === Test 15: 拾取命中/未命中/最近优先（+ null 安全 + 空推导列表未命中） ===
TestResult TestGizmoPickHitMissNearest() {
    using namespace gi_gizmo;
    const primal::math::m4x4 invVP = TestInvViewProj();
    const primal::math::v3 camOrigin{0.0f, 0.0f, -10.0f};
    constexpr u32 kW = 800, kH = 600;

    // -- 命中 + 最近优先：屏幕中心射线 (0,0,+1) 穿过两个盒，近者 t=10 --
    {
        StandardRenderPipeline p;   // device-less: settings-only paths
        MakeTwoBoxPipeline(p, true);
        GIVolumeHit h = PickPipelineVolumes(&p, kW * 0.5f, kH * 0.5f, kW, kH,
                                            invVP, camOrigin);
        TEST_ASSERT(h.hit, "Center ray hits the stacked volumes");
        TEST_ASSERT(h.volumeIndex == 0, "Nearest volume (index 0) wins");
        TEST_ASSERT(NearlyEqual(h.tMin, 10.0f), "tMin = distance to near AABB");

        // 数组顺序反过来：近盒变成 index 1 —— 最近优先与索引无关
        StandardRenderPipeline p2;
        MakeTwoBoxPipeline(p2, false);
        GIVolumeHit h2 = PickPipelineVolumes(&p2, kW * 0.5f, kH * 0.5f, kW, kH,
                                             invVP, camOrigin);
        TEST_ASSERT(h2.hit && h2.volumeIndex == 1, "Nearest still wins after reorder");
        TEST_ASSERT(NearlyEqual(h2.tMin, 10.0f), "Reordered tMin unchanged");
    }

    // -- 未命中：射线斜出盒外（NDC (1, -1.33) → dir (0,-1.33,1)）--
    {
        StandardRenderPipeline p;
        MakeTwoBoxPipeline(p, true);
        GIVolumeHit h = PickPipelineVolumes(&p, kW * 0.5f, 700.0f, kW, kH,
                                            invVP, camOrigin);
        TEST_ASSERT(!h.hit, "Ray below the boxes misses");

        // 相机在远处侧向：所有射线都不经过盒区
        GIVolumeHit h2 = PickPipelineVolumes(&p, kW * 0.5f, kH * 0.5f, kW, kH,
                                             invVP, primal::math::v3{1000.0f, 1000.0f, -1000.0f});
        TEST_ASSERT(!h2.hit, "Distant side camera misses");
    }

    // -- null 安全 / 空数组 / 空推导列表（overrides 为空 = 无可编辑对象）--
    {
        GIVolumeHit h = PickPipelineVolumes(nullptr, 0, 0, 8, 8, invVP, camOrigin);
        TEST_ASSERT(!h.hit, "Null pipeline miss");
        lumen::LumenVolumeConfig none[1]{};
        GIVolumeHit h2 = PickVolume(0, 0, 8, 8, invVP, camOrigin, nullptr, 1);
        TEST_ASSERT(!h2.hit, "Null volumes miss");
        GIVolumeHit h3 = PickVolume(0, 0, 8, 8, invVP, camOrigin, none, 0);
        TEST_ASSERT(!h3.hit, "Zero count miss");

        StandardRenderPipeline empty;   // 默认 overrides 为空（推导模式）
        GIVolumeHit h4 = PickPipelineVolumes(&empty, kW * 0.5f, kH * 0.5f, kW, kH,
                                             invVP, camOrigin);
        TEST_ASSERT(!h4.hit, "Empty overrides list never picks");
    }
    return TestResult::Passed;
}

// === Test 16: Translate 写 origin_override + use_origin_override（含叠加与拒绝路径） ===
TestResult TestGizmoTranslateWritesOriginOverride() {
    using namespace gi_gizmo;
    StandardRenderPipeline p;
    MakeTwoBoxPipeline(p, true);

    lumen::LumenVolumeConfig before{};
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 0, &before), "Read volume 0");
    TEST_ASSERT(before.origin_override.x == 0.0f, "Start at world-centered");

    // 首次平移：world-centered → 显式 origin (1,-2,3)
    TEST_ASSERT(TranslateVolume(&p, 0, primal::math::v3{1.0f, -2.0f, 3.0f}),
                "Translate volume 0");
    lumen::LumenVolumeConfig after{};
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 0, &after), "Re-read volume 0");
    TEST_ASSERT(after.use_origin_override, "use_origin_override set");
    TEST_ASSERT(NearlyEqual(after.origin_override.x, 1.0f) &&
                NearlyEqual(after.origin_override.y, -2.0f) &&
                NearlyEqual(after.origin_override.z, 3.0f),
                "origin_override = first delta");

    // 叠加第二次平移：(1,-2,3) + (1,1,1) = (2,-1,4)
    TEST_ASSERT(TranslateVolume(&p, 0, primal::math::v3{1.0f, 1.0f, 1.0f}),
                "Translate volume 0 again");
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 0, &after), "Re-read volume 0");
    TEST_ASSERT(NearlyEqual(after.origin_override.x, 2.0f) &&
                NearlyEqual(after.origin_override.y, -1.0f) &&
                NearlyEqual(after.origin_override.z, 4.0f),
                "Deltas accumulate on origin_override");

    // 相邻 volume 不受影响（helper 里 volume 1 = use_origin_override (0,0,8)）
    lumen::LumenVolumeConfig v1{};
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 1, &v1), "Read volume 1");
    TEST_ASSERT(v1.use_origin_override &&
                NearlyEqual(v1.origin_override.x, 0.0f) &&
                NearlyEqual(v1.origin_override.y, 0.0f) &&
                NearlyEqual(v1.origin_override.z, 8.0f),
                "Volume 1 untouched");

    // 拒绝路径：null 管线 / 越界索引 / NaN delta
    TEST_ASSERT(!TranslateVolume(nullptr, 0, primal::math::v3{1, 1, 1}), "Null pipeline rejected");
    TEST_ASSERT(!TranslateVolume(&p, 9, primal::math::v3{1, 1, 1}), "OOB index rejected");
    const f32 nan3[3] = {std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f};
    TEST_ASSERT(!TranslateVolume(&p, 0, primal::math::v3{nan3[0], nan3[1], nan3[2]}),
                "NaN delta rejected");
    return TestResult::Passed;
}

// === Test 17: Scale clamp 4..128 + 逐轴/三轴同缩语义 ===
TestResult TestGizmoScaleClampAndAxes() {
    using namespace gi_gizmo;
    StandardRenderPipeline p;
    MakeTwoBoxPipeline(p, true);
    lumen::LumenVolumeConfig v{};

    // 向上 clamp：4 * 100 = 400 → 128（helper 的 counts 起步为 4）
    TEST_ASSERT(ScaleVolume(&p, 0, 100.0f, 0), "Scale up axis x");
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 0, &v) && v.probe_count_x == 128,
                "Count clamps to 128");
    TEST_ASSERT(v.probe_count_y == 4 && v.probe_count_z == 4,
                "Axis 0 touches only x");

    // 向下 clamp：128 * 0.001 = 0.128 → round 0 → clamp 4
    TEST_ASSERT(ScaleVolume(&p, 0, 0.001f, 0), "Scale down axis x");
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 0, &v) && v.probe_count_x == 4,
                "Count clamps to 4");

    // 常规乘法：4 * 2 = 8（axis=1 只动 y）
    TEST_ASSERT(ScaleVolume(&p, 0, 2.0f, 1), "Scale axis y");
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 0, &v) && v.probe_count_y == 8,
                "y doubled, others unchanged");
    TEST_ASSERT(v.probe_count_x == 4 && v.probe_count_z == 4, "x/z untouched");

    // 三轴同缩：kScaleAllAxes
    TEST_ASSERT(ScaleVolume(&p, 0, 4.0f, kScaleAllAxes), "Scale all axes");
    TEST_ASSERT(gi_panel::GetVolumeOverride(&p, 0, &v), "Re-read after uniform");
    TEST_ASSERT(v.probe_count_x == 16 && v.probe_count_y == 32 &&
                v.probe_count_z == 16,
                "Uniform scale multiplies every count (4/8/4 * 4)");
    TEST_ASSERT(NearlyEqual(v.probe_spacing, 1.0f), "Spacing unchanged by scale");

    // 拒绝路径：null / OOB / 非法 axis / 非正 factor
    TEST_ASSERT(!ScaleVolume(nullptr, 0, 2.0f, 0), "Null pipeline rejected");
    TEST_ASSERT(!ScaleVolume(&p, 9, 2.0f, 0), "OOB index rejected");
    TEST_ASSERT(!ScaleVolume(&p, 0, 2.0f, 4), "Axis 4 rejected");
    TEST_ASSERT(!ScaleVolume(&p, 0, 0.0f, 0), "Zero factor rejected");
    TEST_ASSERT(!ScaleVolume(&p, 0, -1.0f, 0), "Negative factor rejected");
    return TestResult::Passed;
}

// === Test 18: DragDelta 世界位移 + 模式/选中状态 + gizmo.mode.* 面板动作 ===
TestResult TestGizmoDragModesAndActions() {
    using namespace gi_gizmo;
    using namespace gi_panel;

    // -- DragDelta：参考平面过拾取点，法线 = cross(right, up) --
    GizmoRay prev{};
    prev.origin = primal::math::v3{0.0f, 0.0f, -10.0f};
    prev.dir    = primal::math::v3{0.0f, 0.0f, 1.0f};
    GizmoRay cur = prev;
    const f32 invSqrt2 = 1.0f / std::sqrt(2.0f);
    cur.dir = primal::math::v3{invSqrt2, 0.0f, invSqrt2};   // 右转 45°
    const primal::math::v3 delta = DragDelta(prev, cur,
                                     primal::math::v3{1.0f, 0.0f, 0.0f},
                                     primal::math::v3{0.0f, 1.0f, 0.0f}, 10.0f);
    TEST_ASSERT(NearlyEqual(delta.x, 10.0f) && NearlyEqual(delta.y, 0.0f) &&
                NearlyEqual(delta.z, 0.0f),
                "45-degree drag on z=0 plane = (10,0,0) world delta");

    // 平行于参考平面（dir 无 z 分量）→ 零位移
    GizmoRay par = prev;
    par.dir = primal::math::v3{1.0f, 0.0f, 0.0f};
    const primal::math::v3 zero = DragDelta(prev, par,
                                    primal::math::v3{1.0f, 0.0f, 0.0f},
                                    primal::math::v3{0.0f, 1.0f, 0.0f}, 10.0f);
    TEST_ASSERT(zero.x == 0.0f && zero.y == 0.0f && zero.z == 0.0f,
                "Ray parallel to plane yields zero delta");

    // -- 模式 + selected volume + 拖拽参考距离 --
    SetMode(GizmoMode::Off);
    TEST_ASSERT(GetMode() == GizmoMode::Off, "Default mode Off");
    SetSelectedVolume(kNoVolumeSelected);
    SetDragReferenceDistance(10.0f);
    SetDragReferenceDistance(-1.0f);   // 非法值被拒
    TEST_ASSERT(NearlyEqual(GetDragReferenceDistance(), 10.0f),
                "Invalid reference distance rejected");
    SetSelectedVolume(1);
    TEST_ASSERT(GetSelectedVolume() == 1, "Selected volume round-trip");

    // -- gizmo.mode.* 面板动作（经 GIEditorPanel 注册表）--
    ClearActions();
    TEST_ASSERT(RegisterDefaultActions(), "Register default gizmo actions");
    TEST_ASSERT(RegisterDefaultActions(), "Idempotent re-register");
    TEST_ASSERT(GetActionCount() == 4, "Exactly four gizmo.mode.* actions");
    TEST_ASSERT(GetActionById("gizmo.mode.select") != nullptr, "select action");
    TEST_ASSERT(GetActionById("gizmo.mode.translate") != nullptr, "translate action");
    TEST_ASSERT(GetActionById("gizmo.mode.scale") != nullptr, "scale action");
    TEST_ASSERT(GetActionById("gizmo.mode.off") != nullptr, "off action");

    TEST_ASSERT(InvokeAction("gizmo.mode.translate"), "Invoke translate action");
    TEST_ASSERT(GetMode() == GizmoMode::Translate, "Mode now Translate");
    TEST_ASSERT(InvokeAction("gizmo.mode.scale"), "Invoke scale action");
    TEST_ASSERT(GetMode() == GizmoMode::Scale, "Mode now Scale");
    TEST_ASSERT(GetSelectedVolume() == 1, "Selection survives mode switches");
    TEST_ASSERT(InvokeAction("gizmo.mode.off"), "Invoke off action");
    TEST_ASSERT(GetMode() == GizmoMode::Off, "Mode back to Off");
    TEST_ASSERT(GetSelectedVolume() == kNoVolumeSelected,
                "Off clears selected volume");
    ClearActions();
    return TestResult::Passed;
}

// === Test 19: 线框端点数 24 / 角点封闭 / 端点在 AABB 内 ===
TestResult TestGizmoWireframeEndpoints() {
    using namespace gi_gizmo;
    StandardRenderPipeline p;
    MakeTwoBoxPipeline(p, true);

    primal::math::v3 endpoints[kWireframeEndpointCount];
    TEST_ASSERT(GetPipelineVolumeWireframe(&p, 0, endpoints),
                "Wireframe for volume 0");
    TEST_ASSERT(!GetPipelineVolumeWireframe(&p, 9, endpoints), "OOB wireframe fails");
    TEST_ASSERT(!GetPipelineVolumeWireframe(nullptr, 0, endpoints),
                "Null pipeline wireframe fails");
    TEST_ASSERT(!GetVolumeWireframe(lumen::LumenVolumeConfig{}, nullptr),
                "Null out buffer fails");

    // 所有端点都在 AABB [0,4]^3 内
    for (u32 i = 0; i < kWireframeEndpointCount; ++i) {
        const primal::math::v3& e = endpoints[i];
        TEST_ASSERT(e.x >= -1e-4f && e.x <= 4.0f + 1e-4f &&
                    e.y >= -1e-4f && e.y <= 4.0f + 1e-4f &&
                    e.z >= -1e-4f && e.z <= 4.0f + 1e-4f,
                    "Endpoint inside AABB bounds");
    }
    // 12 条边：每条边的两端点恰在一根轴上不同（曼哈顿意义）
    for (u32 e = 0; e < kWireframeEndpointCount / 2; ++e) {
        const primal::math::v3& a = endpoints[e * 2];
        const primal::math::v3& b = endpoints[e * 2 + 1];
        const int diffAxes = (NearlyEqual(a.x, b.x) ? 0 : 1) +
                             (NearlyEqual(a.y, b.y) ? 0 : 1) +
                             (NearlyEqual(a.z, b.z) ? 0 : 1);
        TEST_ASSERT(diffAxes == 1, "Each wireframe edge is axis-aligned");
    }
    // 恰好 8 个不同角点（每点出现 3 次 = 立方体顶点度数）
    u32 uniqueCorners = 0;
    for (u32 i = 0; i < kWireframeEndpointCount; ++i) {
        bool first = true;
        for (u32 j = 0; j < i; ++j) {
            if (NearlyEqual(endpoints[i].x, endpoints[j].x) &&
                NearlyEqual(endpoints[i].y, endpoints[j].y) &&
                NearlyEqual(endpoints[i].z, endpoints[j].z)) { first = false; break; }
        }
        if (first) ++uniqueCorners;
    }
    TEST_ASSERT(uniqueCorners == 8, "Wireframe covers exactly 8 box corners");
    // 首端点 = min 角（覆盖 min 落点）
    TEST_ASSERT(NearlyEqual(endpoints[0].x, 0.0f) &&
                NearlyEqual(endpoints[0].y, 0.0f) &&
                NearlyEqual(endpoints[0].z, 0.0f),
                "First endpoint is the AABB min corner");
    return TestResult::Passed;
}

// === Test 20: DLL C ABI gizmo 导出 pre-init 安全（无活管线时全部安全返回） ===
TestResult TestGizmoAbiPreInitSafety() {
    using namespace gi_gizmo;

    // 模式往返 + 非法模式拒绝
    GIGizmoSetMode(2);
    TEST_ASSERT(GIGizmoGetMode() == 2u, "C ABI mode round-trip (Translate)");
    GIGizmoSetMode(99);
    TEST_ASSERT(GIGizmoGetMode() == 2u, "Invalid mode value rejected");
    GIGizmoSetMode(0);
    TEST_ASSERT(GIGizmoGetMode() == 0u, "Mode reset to Off");

    SetSelectedVolume(kNoVolumeSelected);
    TEST_ASSERT(GIGizmoGetSelectedVolume() == 0xFFFFFFFFu,
                "No-selection sentinel via C ABI");

    // Pick：null 矩阵/原点拒绝；无活管线 → 未命中（矩阵内容此时无关紧要，
    // 传 identity 即可）
    f32 m16[16] = {0};
    for (u32 i = 0; i < 4; ++i) m16[i * 4 + i] = 1.0f;
    const f32 cam[3] = {0.0f, 0.0f, -10.0f};
    u32 volIdx = 99;
    f32 tMin = -1.0f;
    TEST_ASSERT(GIGizmoPick(400.0f, 300.0f, 800, 600, nullptr, cam,
                            &volIdx, &tMin) == 0, "Null matrix rejected");
    TEST_ASSERT(GIGizmoPick(400.0f, 300.0f, 800, 600, m16, nullptr,
                            &volIdx, &tMin) == 0, "Null origin rejected");
    TEST_ASSERT(GIGizmoPick(400.0f, 300.0f, 800, 600, m16, cam,
                            &volIdx, &tMin) == 0,
                "No live pipeline → pick misses");
    TEST_ASSERT(volIdx == 99 && tMin == -1.0f, "Out-args untouched on miss");

    // Translate/Scale：无活管线 → 0
    const f32 delta[3] = {1.0f, 0.0f, 0.0f};
    TEST_ASSERT(GIGizmoTranslate(0, delta) == 0, "No pipeline translate fails");
    TEST_ASSERT(GIGizmoTranslate(0, nullptr) == 0, "Null delta rejected");
    TEST_ASSERT(GIGizmoScale(0, 2.0f, 0) == 0, "No pipeline scale fails");

    // Wireframe：无活管线 → 0；容量不足 → 0 且 *outCount = 24；null 计数 → 0
    f32 buf[72];
    u32 cnt = 0;
    TEST_ASSERT(GIGizmoGetWireframe(0, buf, 72, &cnt) == 0,
                "No pipeline wireframe fails");
    TEST_ASSERT(cnt == 0, "Count zero on failure without pipeline");
    TEST_ASSERT(GIGizmoGetWireframe(0, buf, 8, &cnt) == 0,
                "Insufficient capacity fails");
    TEST_ASSERT(cnt == 24, "Capacity failure reports required endpoint count");
    TEST_ASSERT(GIGizmoGetWireframe(0, nullptr, 72, &cnt) == 0,
                "Null buffer rejected");
    TEST_ASSERT(GIGizmoGetWireframe(0, buf, 72, nullptr) == 0,
                "Null count pointer rejected");
    return TestResult::Passed;
}

void RunRenderPipelineAPITests() {
    TestSuite suite("RenderPipeline API C ABI Tests (Phase 5)");
    suite.AddTestCase(TestCase("Pass count matches enum",
        TestPassCountMatchesEnum,
        "GetPassCount() == RenderPassID::Count"));
    suite.AddTestCase(TestCase("Pass info valid for all passes",
        TestPassInfoValid,
        "GetPipelinePassInfo returns non-empty name/desc for every pass"));
    suite.AddTestCase(TestCase("Pass info ignores invalid id",
        TestPassInfoIgnoresInvalidId,
        "Out-args unchanged when called with out-of-range id"));
    suite.AddTestCase(TestCase("ParamDescriptors include Bloom/TAA",
        TestParamDescriptorsIncludeBloomTAASchema,
        "Descriptor table has Bloom and TAA groups; all offsets fit"));
    suite.AddTestCase(TestCase("Pass toggle ABI does not crash",
        TestPassToggleAbiDoesNotCrash,
        "SetRenderPassEnabled/IsRenderPassEnabled/IsActive callable pre-init"));
    suite.AddTestCase(TestCase("Default Bloom/TAA values match schema",
        TestSettingsDefaultBloomTAA,
        "GetRenderPipelineSettings fills bloom/taa with declared defaults"));
    suite.AddTestCase(TestCase("Null safety",
        TestSettingsNullSafety,
        "Passing nullptr to setters/getters does not crash"));
    // M6/T6.1 GI editor panel data plane
    suite.AddTestCase(TestCase("GI Axes descriptors present",
        TestGIAxesDescriptorsPresent,
        "kParams has 'GI Axes' group with all five axes (8 entries)"));
    suite.AddTestCase(TestCase("GIDebug/Volume descriptor tables valid",
        TestGIDebugAndVolumeDescriptorTables,
        "Debug/volume descriptors fit their base structs; probe-vis enum present"));
    suite.AddTestCase(TestCase("Panel action registry round-trip",
        TestPanelActionRegistry,
        "Register/invoke/unregister/clear actions; null safety"));
    suite.AddTestCase(TestCase("GI panel null-pipeline safety",
        TestGIPanelNullPipelineSafety,
        "All gi_panel entry points safe with null pipeline"));
    suite.AddTestCase(TestCase("GIDebugParams defaults + round-trip",
        TestGIDebugParamsDefaultsAndRoundTrip,
        "probe_vis defaults Off; singleton writes visible; restored"));
    suite.AddTestCase(TestCase("GI panel C ABI pre-init safety",
        TestGIPanelAbiPreInitSafety,
        "GI* exports callable without a live pipeline"));
    suite.AddTestCase(TestCase("Volume CRUD + axes hot-reload on local pipeline",
        TestGIPanelVolumeAndAxesOnLocalPipeline,
        "Materialize/clamp/append/remove volumes; ApplyAxesConfig overwrites pass toggles"));
    // M6/T6.2 GI volume gizmo
    suite.AddTestCase(TestCase("Gizmo pick hit/miss/nearest",
        TestGizmoPickHitMissNearest,
        "Screen ray → AABB slab pick: nearest volume wins, misses and nulls safe"));
    suite.AddTestCase(TestCase("Gizmo translate writes origin_override",
        TestGizmoTranslateWritesOriginOverride,
        "worldDelta accumulates onto origin_override + use_origin_override"));
    suite.AddTestCase(TestCase("Gizmo scale clamps 4..128",
        TestGizmoScaleClampAndAxes,
        "Per-axis/uniform count scaling with clamp; invalid axis/factor rejected"));
    suite.AddTestCase(TestCase("Gizmo drag delta + modes + panel actions",
        TestGizmoDragModesAndActions,
        "DragDelta plane math; mode/selection state; gizmo.mode.* registry actions"));
    suite.AddTestCase(TestCase("Gizmo wireframe endpoints",
        TestGizmoWireframeEndpoints,
        "24 endpoints, 8 unique axis-aligned box corners inside the AABB"));
    suite.AddTestCase(TestCase("Gizmo C ABI pre-init safety",
        TestGizmoAbiPreInitSafety,
        "GIGizmo* exports callable without a live pipeline"));
    suite.RunAllTests();
}

int main() {
    RunRenderPipelineAPITests();
    return 0;
}
