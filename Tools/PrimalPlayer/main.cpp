// =============================================================================
// PrimalPlayer — 运行时宿主（规划 §Run/Stop，2026-09-03）
//
//   PrimalPlayer --scene <active.primalrun> [--width W] [--height H] [--debug]
//
// 编辑器 Run 按钮导出运行时场景包（Content/RuntimeSceneExporter.cs，行式
// "primal-runtime-scene v1"）后启动本进程。本宿主是 EngineDLL 的 C ABI 消费
// 者（与编辑器/primal-engine 平级），装配序列抄自 Tools/PrimalEngineCli 的
// render verb（InitializeEngine → CreateStandardRenderPipeline → 注入 →
// 渲染 → 逆序清理），差异有三：
//   1. 场景来自运行时包而非硬编码测试场景；
//   2. PipelineSetEditorMode(0)——真 runtime 路径，不是编辑器轻量前向；
//   3. 有窗口 + 每帧 Blit 到 surface（编辑器同款 RenderFrame→Blit 顺序）。
//
// 分图形API/分 debug/release = 启动不同构建树/配置的本宿主二进制，由编辑器
// 的 Run 下拉决定；本进程内不切换（引擎单例决定一进程一份 EngineDLL）。
// =============================================================================

#include "player_window.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#define PRIMAL_PLAYER_WINDOW 1
#else
#include <unistd.h>
#endif

// ----------------------------------------------------------------------------
// EngineDLL C ABI —— 函数声明镜像（非 POD 内存镜像，与 primal-engine 同纪律；
// 符号存在性由链接期 dyld 校验）。
// ----------------------------------------------------------------------------
extern "C" {
std::uint32_t InitializeEngine(std::uint32_t rhi_platform, std::uint32_t enable_debug);
void          ShutdownEngine();
std::uint64_t GetEngineDeviceHandle();
void          SetPipelineViewportSize(std::uint32_t width, std::uint32_t height);
std::uint64_t CreateStandardRenderPipeline(std::uint64_t device_handle);
void          DestroyStandardRenderPipeline();
void          PipelineSetEditorMode(std::uint32_t enabled);
std::uint32_t PipelineApplyModePreset(std::uint32_t mode);

std::uint32_t CreateEntity(float pos_x, float pos_y, float pos_z);
void          SetEntityTransform(std::uint32_t id, const float* position,
                                 const float* rotation, const float* scale);
std::uint32_t AddEntityLight(std::uint32_t entity_id, std::uint32_t light_type,
                             float intensity, const float* color_rgb, float range,
                             float umbra_rad, float penumbra_rad, std::uint32_t is_enabled);
std::uint32_t AddEntityCamera(std::uint32_t entity_id, std::uint32_t projection_type,
                              float field_of_view, float aspect_ratio, float near_z,
                              float far_z, const float* up_vector);
std::uint32_t CreateCamera(std::uint32_t entity_id, float fov, float aspect,
                           float near_z, float far_z);

std::uint64_t ImportMeshAsync(const char* path);
void          TryGetMeshImportResult(std::uint64_t request_id,
                                     std::uint64_t* out_mesh_content_id, std::uint32_t* out_status);
void          TickMainThreadJobs();
void          TickAssetSystem();

std::uint64_t PipelineRegisterMeshEntity(std::uint64_t geometry_content_id,
                                         const std::uint64_t* texture_ids,
                                         std::uint32_t texture_count);
void          PipelineUnregisterMeshEntity(std::uint64_t entity_id);

std::uint64_t CreateRenderTarget(std::uint32_t width, std::uint32_t height, std::uint32_t format);
void          DestroyRenderTarget(std::uint64_t handle);
std::uint32_t PipelineRenderFrame(std::uint64_t target_handle, std::uint32_t camera_id,
                                  std::uint32_t surface_id);
std::uint32_t BlitRenderTargetToSurface(std::uint64_t target_handle, std::uint32_t surface_id);
std::uint32_t CreateRenderSurface(void* host, std::int32_t width, std::int32_t height);
void          RemoveRenderSurface(std::uint32_t id);
void          UpdateSceneEnvironment(const float* params);
}

namespace {

constexpr std::uint32_t kPlatformVulkan = 2;
constexpr std::uint32_t kPlatformMetal  = 3;
constexpr std::uint32_t kRtFormatRgba16F = 1;
constexpr std::uint32_t kInvalidSurfaceId = 0xFFFFFFFFu;
constexpr std::uint32_t kInvalidEntityId  = 0xFFFFFFFFu;

enum class LightKind : std::uint32_t { Directional = 0, Point = 1, Spot = 2 };
enum class CameraProjection : std::uint32_t { Perspective = 0, Orthographic = 1 };

struct RuntimeEntity {
    std::string name;
    float pos[3]{0.f, 0.f, 0.f};
    float rot[3]{0.f, 0.f, 0.f};
    float scl[3]{1.f, 1.f, 1.f};
    std::string mesh_path;
    bool has_light = false;
    std::uint32_t light_type = 0;
    float light_intensity = 1.f;
    float light_color[3]{1.f, 1.f, 1.f};
    float light_range = 0.f;
    float light_umbra = 0.f;
    float light_penumbra = 0.f;
    bool light_enabled = true;
    bool has_camera = false;
    std::uint32_t camera_projection = 0;
    float camera_fov_deg = 60.f;
    float camera_near = 0.1f;
    float camera_far = 1000.f;
    bool camera_is_main = false;
};

// 渲染相机最终选择：主相机 > 第一个场景相机 > 默认机位。
struct ChosenCamera {
    std::uint32_t entity_id = kInvalidEntityId;
    float fov_deg = 60.f;
    float near_z = 0.1f;
    float far_z = 1000.f;
};

struct RuntimeScene {
    std::string scene_name = "Scene";
    float env[24]{};
    std::vector<RuntimeEntity> entities;
};

// --- 行式解析器（格式契约见 Content/RuntimeSceneExporter.cs 头注释） ---

std::string Trim(const std::string& s)
{
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ParseFloats(const std::string& rest, float* out, std::uint32_t count)
{
    std::size_t i = 0;
    for (std::uint32_t k = 0; k < count; ++k) {
        while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
        if (i >= rest.size()) return false;
        out[k] = std::strtof(rest.c_str() + i, nullptr);
        while (i < rest.size() && rest[i] != ' ' && rest[i] != '\t') ++i;
    }
    return true;
}

std::string ParseQuoted(const std::string& rest)
{
    auto a = rest.find('"');
    if (a == std::string::npos) return Trim(rest);
    auto b = rest.find('"', a + 1);
    if (b == std::string::npos) return rest.substr(a + 1);
    return rest.substr(a + 1, b - a - 1);
}

bool LoadRuntimeScene(const std::string& path, RuntimeScene& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[player] cannot open scene package: %s\n", path.c_str());
        return false;
    }
    std::string header;
    {
        char buf[64]{};
        if (!std::fgets(buf, sizeof(buf), f)) { std::fclose(f); return false; }
        header = Trim(buf);
    }
    if (header != "# primal-runtime-scene v1") {
        std::fprintf(stderr, "[player] unsupported package header: %s\n", header.c_str());
        std::fclose(f);
        return false;
    }

    char line[4096];
    bool in_entity = false;
    RuntimeEntity current;
    while (std::fgets(line, sizeof(line), f)) {
        std::string text = Trim(line);
        if (text.empty() || text[0] == '#') continue;
        auto space = text.find(' ');
        const std::string key = space == std::string::npos ? text : text.substr(0, space);
        const std::string rest = space == std::string::npos ? "" : Trim(text.substr(space + 1));

        if (key == "entity") {
            in_entity = true;
            current = RuntimeEntity{};
            continue;
        }
        if (key == "end") {
            if (in_entity) out.entities.push_back(current);
            in_entity = false;
            continue;
        }
        if (key == "scene") { out.scene_name = ParseQuoted(rest); continue; }
        if (key == "env") { ParseFloats(rest, out.env, 24); continue; }

        if (!in_entity) continue;
        if (key == "name") current.name = ParseQuoted(rest);
        else if (key == "pos") ParseFloats(rest, current.pos, 3);
        else if (key == "rot") ParseFloats(rest, current.rot, 3);
        else if (key == "scl") ParseFloats(rest, current.scl, 3);
        else if (key == "mesh") current.mesh_path = ParseQuoted(rest);
        else if (key == "light") {
            float v[9];
            if (ParseFloats(rest, v, 9)) {
                current.has_light = true;
                current.light_type = static_cast<std::uint32_t>(v[0]);
                current.light_intensity = v[1];
                current.light_color[0] = v[2];
                current.light_color[1] = v[3];
                current.light_color[2] = v[4];
                current.light_range = v[5];
                current.light_umbra = v[6];
                current.light_penumbra = v[7];
                current.light_enabled = v[8] != 0.f;
            }
        } else if (key == "camera") {
            float v[5];
            if (ParseFloats(rest, v, 5)) {
                current.has_camera = true;
                current.camera_projection = static_cast<std::uint32_t>(v[0]);
                current.camera_fov_deg = v[1];
                current.camera_near = v[2];
                current.camera_far = v[3];
                current.camera_is_main = v[4] != 0.f;
            }
        }
    }
    std::fclose(f);

    if (out.entities.empty()) {
        // 空场景也允许运行（用户可能想看 GI 环境光/天空盒），但留痕。
        std::fprintf(stderr, "[player] scene package has no entities: %s\n", path.c_str());
    }
    return true;
}

// deg 单位契约与编辑器 SceneRuntimeSynchronizer 一致：CreateCamera 的 fov 传
// degrees/180；灯光角度原样直传（引擎 Light 组件自行换算）。
// 实体分两路：带网格的走 PipelineRegisterMeshEntity（注册即建实体），灯光/
// 相机/空实体走 CreateEntity + AddEntityLight。相机组件不在此挂——由调用方
// 对最终选中的相机实体调一次 CreateCamera 拿渲染用 camera slot。
std::uint32_t InjectScene(const RuntimeScene& scene,
                          ChosenCamera& out_camera,
                          std::vector<std::uint64_t>& out_mesh_entities)
{
    UpdateSceneEnvironment(scene.env);

    ChosenCamera chosen{};

    for (const auto& e : scene.entities) {
        if (e.mesh_path.empty()) continue;
        const std::uint64_t request = ImportMeshAsync(e.mesh_path.c_str());
        if (request == 0) {
            std::fprintf(stderr, "[player] ImportMeshAsync rejected: %s\n", e.mesh_path.c_str());
            continue;
        }
        std::uint64_t mesh_id = 0;
        std::uint32_t status = 0; // Pending
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            TickMainThreadJobs();
            TickAssetSystem();
            TryGetMeshImportResult(request, &mesh_id, &status);
            if (status != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (status != 1 || mesh_id == 0) { // 1 = Success（编辑器 MeshImportResultStatus 同值）
            std::fprintf(stderr, "[player] mesh import failed (status=%u): %s\n",
                         status, e.mesh_path.c_str());
            continue;
        }
        const std::uint64_t texture_ids[1] = {0}; // 默认白材质（primal-engine 同款）
        const std::uint64_t entity =
            PipelineRegisterMeshEntity(mesh_id, texture_ids, 1);
        if (entity == 0 || entity == 0xFFFFFFFFFFFFFFFFull) {
            std::fprintf(stderr, "[player] PipelineRegisterMeshEntity failed: %s\n",
                         e.mesh_path.c_str());
            continue;
        }
        SetEntityTransform(static_cast<std::uint32_t>(entity), e.pos, e.rot, e.scl);
        out_mesh_entities.push_back(entity);
    }

    for (const auto& e : scene.entities) {
        if (!e.has_light && !e.has_camera) continue;
        const std::uint32_t entity = CreateEntity(e.pos[0], e.pos[1], e.pos[2]);
        if (entity == kInvalidEntityId) {  // 只认哨兵——slot 0 是合法分配（primal-engine 同约定）
            std::fprintf(stderr, "[player] CreateEntity failed: %s\n", e.name.c_str());
            continue;
        }
        SetEntityTransform(entity, e.pos, e.rot, e.scl);
        if (e.has_light) {
            AddEntityLight(entity, e.light_type, e.light_intensity, e.light_color,
                           e.light_range, e.light_umbra, e.light_penumbra,
                           e.light_enabled ? 1u : 0u);
        }
        if (e.has_camera &&
            (e.camera_is_main || chosen.entity_id == kInvalidEntityId)) {
            chosen.entity_id = entity;
            chosen.fov_deg = e.camera_fov_deg;
            chosen.near_z = e.camera_near;
            chosen.far_z = e.camera_far;
        }
    }

    // 场景没有相机：默认机位 (0,0,-8) 看 +Z。
    if (chosen.entity_id == kInvalidEntityId) {
        const std::uint32_t entity = CreateEntity(0.f, 0.f, -8.f);
        if (entity == kInvalidEntityId) {  // 只认哨兵——slot 0 是合法分配（primal-engine 同约定）
            std::fprintf(stderr, "[player] default camera CreateEntity failed\n");
            return 0;
        }
        const float pos[3]{0.f, 0.f, -8.f};
        const float rot[3]{0.f, 0.f, 0.f};
        const float scl[3]{1.f, 1.f, 1.f};
        SetEntityTransform(entity, pos, rot, scl);
        chosen.entity_id = entity;
    }
    out_camera = chosen;
    return 1;
}

std::string ExeDir()
{
    std::vector<char> buf(4096, 0);
#if defined(__APPLE__)
    std::uint32_t size = static_cast<std::uint32_t>(buf.size());
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
#else
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) return {};
#endif
    std::string exe(buf.data());
    auto slash = exe.find_last_of('/');
    return slash == std::string::npos ? std::string() : exe.substr(0, slash);
}

} // namespace

int main(int argc, char** argv)
{
    std::string scene_path;
    std::uint32_t width = 1280, height = 720;
    bool debug_backend = false;
    std::uint32_t render_mode = 3; // Deferred+Shadow+AO+GI（编辑器默认档一致）

    for (int i = 1; i < argc; ++i) {
        const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (std::strcmp(argv[i], "--scene") == 0 && next()) scene_path = argv[i];
        else if (std::strcmp(argv[i], "--width") == 0 && next()) width = static_cast<std::uint32_t>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--height") == 0 && next()) height = static_cast<std::uint32_t>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--debug") == 0) debug_backend = true;
        else if (std::strcmp(argv[i], "--mode") == 0 && next()) render_mode = static_cast<std::uint32_t>(std::atoi(argv[i]));
        else {
            std::fprintf(stderr, "[player] unknown arg: %s\n", argv[i]);
            std::fprintf(stderr, "usage: PrimalPlayer --scene <pkg> [--width W] [--height H] [--debug] [--mode 0..3]\n");
            return 2;
        }
    }
    if (scene_path.empty() || width == 0 || height == 0) {
        std::fprintf(stderr, "usage: PrimalPlayer --scene <pkg> [--width W] [--height H]\n");
        return 2;
    }

    // 引擎 shader blob 按 cwd 相对路径解析——切到 exe 目录（Darwin/<config>）。
    const std::string dir = ExeDir();
    if (!dir.empty()) {
        if (::chdir(dir.c_str()) != 0)
            std::fprintf(stderr, "[player] chdir(%s) failed — shader blob may not load\n", dir.c_str());
    }

    RuntimeScene scene;
    if (!LoadRuntimeScene(scene_path, scene)) return 2;

#if defined(PRIMAL_PLAYER_WINDOW)
    const std::uint32_t platform = kPlatformMetal;
#else
    const std::uint32_t platform = kPlatformVulkan;
#endif
    if (InitializeEngine(platform, debug_backend ? 1u : 0u) == 0) {
        std::fprintf(stderr, "[player] InitializeEngine failed (cwd=%s)\n", dir.c_str());
        return 3;
    }

    void* host_view = nullptr;
#if defined(PRIMAL_PLAYER_WINDOW)
    host_view = PlayerWindowCreate(static_cast<int>(width), static_cast<int>(height),
                                   ("Primal Player — " + scene.scene_name).c_str());
    if (!host_view) { std::fprintf(stderr, "[player] window create failed\n"); return 3; }
#endif

    SetPipelineViewportSize(width, height);
    const std::uint64_t device = GetEngineDeviceHandle();
    if (device == 0 || CreateStandardRenderPipeline(device) == 0) {
        std::fprintf(stderr, "[player] CreateStandardRenderPipeline failed\n");
        return 3;
    }
    PipelineSetEditorMode(0); // runtime 路径
    PipelineApplyModePreset(render_mode);

    ChosenCamera camera{};
    std::vector<std::uint64_t> mesh_entities;
    if (InjectScene(scene, camera, mesh_entities) == 0 || camera.entity_id == kInvalidEntityId) {
        std::fprintf(stderr, "[player] scene injection failed (no camera)\n");
        return 3;
    }
    // 相机 slot：对选中的相机实体调一次 CreateCamera（fov 契约 degrees/180）。
    const std::uint32_t camera_id =
        ::CreateCamera(camera.entity_id, camera.fov_deg / 180.f,
                       static_cast<float>(width) / static_cast<float>(height),
                       camera.near_z, camera.far_z);
    if (camera_id == kInvalidEntityId) {  // 只认哨兵——slot 0 合法
        std::fprintf(stderr, "[player] CreateCamera failed\n");
        return 3;
    }

    const std::uint64_t target = CreateRenderTarget(width, height, kRtFormatRgba16F);
    if (target == 0) { std::fprintf(stderr, "[player] CreateRenderTarget failed\n"); return 3; }

    std::uint32_t surface_id = kInvalidSurfaceId;
#if defined(PRIMAL_PLAYER_WINDOW)
    // 注意：CreateRenderSurface 返回向量索引，首个 surface 合法返回 0；
    // 只有 host 为 null（无窗口路径）它才以 0 表示失败——用 host_view 区分。
    surface_id = CreateRenderSurface(host_view, static_cast<std::int32_t>(width),
                                     static_cast<std::int32_t>(height));
    if (host_view == nullptr) {
        std::fprintf(stderr, "[player] CreateRenderSurface unavailable (null host)\n");
        return 3;
    }
#endif

    std::fprintf(stderr, "[player] running '%s' (%u entities, %ux%u, mode %u) — Esc/close to exit\n",
                 scene.scene_name.c_str(), static_cast<unsigned>(scene.entities.size()),
                 width, height, render_mode);

    // 主循环：事件泵 + 渲染都在主线程（编辑器 DispatcherTimer 同款约束，
    // 规避 PipelineRenderFrame 内部互斥锁的双线程崩溃）。无窗口平台跑固定
    // 帧数即退（CI/冒烟用）。
    const bool windowed = surface_id != kInvalidSurfaceId;
    int remaining_frames = windowed ? -1 : 120;
    while (true) {
#if defined(PRIMAL_PLAYER_WINDOW)
        if (windowed && !PlayerWindowPump()) break;
#endif
        if (PipelineRenderFrame(target, camera_id, surface_id) == 0) {
            std::fprintf(stderr, "[player] PipelineRenderFrame failed\n");
            break;
        }
        if (windowed)
            BlitRenderTargetToSurface(target, surface_id);
        if (!windowed && --remaining_frames <= 0) break;
    }

    if (surface_id != kInvalidSurfaceId)
        RemoveRenderSurface(surface_id);
    DestroyRenderTarget(target);
    DestroyStandardRenderPipeline();
    ShutdownEngine();
#if defined(PRIMAL_PLAYER_WINDOW)
    PlayerWindowDestroy();
#endif
    std::fprintf(stderr, "[player] clean exit\n");
    return 0;
}
