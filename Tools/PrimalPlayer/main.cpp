// =============================================================================
// PrimalPlayer — 运行时宿主（规划 §Run/Stop，2026-09-03）
//
//   PrimalPlayer --scene <active.primalrun> [--width W] [--height H] [--fixed]
//                 [--debug] [--record <dir>] [--record-frames N] [--record-every N]
//                 [--record-metrics <file.csv>]
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
#include <cfloat>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
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

std::uint32_t InitializeAssetSystem();
void          ShutdownAssetSystem();
std::uint64_t ImportMeshAsync(const char* path);
void          TryGetMeshImportResult(std::uint64_t request_id,
                                     std::uint64_t* out_mesh_content_id, std::uint32_t* out_status);
std::uint64_t PipelineImportCookedTexture(const char* path);
void          TickMainThreadJobs();
void          TickAssetSystem();

std::uint64_t PipelineRegisterMeshEntity(std::uint64_t geometry_content_id,
                                         const std::uint64_t* texture_ids,
                                         std::uint32_t texture_count);
void          PipelineUnregisterMeshEntity(std::uint64_t entity_id);
void          PipelineTick(float dt_seconds);
std::uint64_t ScriptCreateForEntity(std::uint32_t entity_id, const char* lua_path,
                                    const char** param_keys, const char** param_values,
                                    std::uint32_t param_count);
void          ScriptDestroyForEntity(std::uint64_t script_id);

std::uint64_t CreateRenderTarget(std::uint32_t width, std::uint32_t height, std::uint32_t format);
void          DestroyRenderTarget(std::uint64_t handle);
std::uint32_t DebugReadRenderTarget(std::uint64_t target_handle, void* out_buffer,
                                    std::uint64_t buffer_size);
int           SaveRenderTargetAsPng(std::uint64_t target_handle, const char* path);
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

// 引擎 PipelineRegisterMeshEntity 的纹理槽契约：[albedo, normal, orm]
// （RenderPipelineAPI "typically [albedo, normal, orm]"，最多 8 槽）。
constexpr std::uint32_t kMaxTextureSlots = 3;

struct RuntimeEntity {
    std::string name;
    float pos[3]{0.f, 0.f, 0.f};
    float rot[3]{0.f, 0.f, 0.f};
    float scl[3]{1.f, 1.f, 1.f};
    std::string mesh_path;
    std::string tex_paths[kMaxTextureSlots]; // 槽位空洞 = 空串
    std::string script_path;                 // v2：实体挂的 .lua（相对包根或绝对）
    std::vector<std::pair<std::string, std::string>> script_params; // param <k> "<v>"
    std::uint32_t engine_entity = 0xFFFFFFFFu; // 注入期回填（脚本挂载用）
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
    float pos[3]{0.f, 0.f, -8.f};
    float rot[3]{0.f, 0.f, 0.f}; // 欧拉度——飞行相机的 yaw/pitch 播种值
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
    if (header.rfind("# primal-runtime-scene v", 0) != 0) {
        std::fprintf(stderr, "[player] unsupported package header: %s\n", header.c_str());
        std::fclose(f);
        return false;
    }
    const int format_version = std::atoi(header.c_str() + header.rfind('v') + 1);
    if (format_version < 1 || format_version > 2) {
        std::fprintf(stderr, "[player] unsupported package version: %d\n", format_version);
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
        else if (key == "tex") {
            // "tex <slot> \"<path>\"" — v2 材质槽位行；槽位越界静默忽略
            // （未来扩展槽位对旧 player 无害）。
            const long slot = std::strtol(rest.c_str(), nullptr, 10);
            const auto quote = rest.find('"');
            if (slot >= 0 && slot < static_cast<long>(kMaxTextureSlots) &&
                quote != std::string::npos) {
                current.tex_paths[slot] = ParseQuoted(rest.substr(quote));
            }
        }
        else if (key == "script") {
            // v2：`script "<路径>"` — 实体挂的 .lua（scripting S1）。
            current.script_path = ParseQuoted(rest);
        }
        else if (key == "param") {
            // v2：`param <key> "<value>"` — 键为无引号 token，值为引号串。
            const auto key_end = rest.find(' ');
            if (key_end != std::string::npos) {
                current.script_params.emplace_back(
                    rest.substr(0, key_end), ParseQuoted(rest.substr(key_end + 1)));
            }
        }
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

// v2 包内路径规则：绝对路径直用；项目相对路径按包位置反推项目根
// （包位于 <proj>/.Primal/runtime/，三级上溯）。v1 包的 mesh 行本就是
// 绝对路径，同样命中"绝对直用"分支。
std::string ResolveAssetPath(const std::string& raw, const std::string& package_path)
{
    if (raw.empty() || raw.front() == '/') return raw;
    std::filesystem::path pkg(package_path);
    const auto root = pkg.parent_path().parent_path().parent_path();
    return (root / std::filesystem::path(raw)).lexically_normal().string();
}

// deg 单位契约与编辑器 SceneRuntimeSynchronizer 一致：CreateCamera 的 fov 传
// degrees/180；灯光角度原样直传（引擎 Light 组件自行换算）。
// 实体分两路：带网格的走 PipelineRegisterMeshEntity（注册即建实体），灯光/
// 相机/空实体走 CreateEntity + AddEntityLight。相机组件不在此挂——由调用方
// 对最终选中的相机实体调一次 CreateCamera 拿渲染用 camera slot。
std::uint32_t InjectScene(RuntimeScene& scene,
                          const std::string& package_path,
                          std::unordered_map<std::string, std::uint64_t>& texture_cache,
                          ChosenCamera& out_camera,
                          std::vector<std::uint64_t>& out_mesh_entities,
                          std::vector<std::uint64_t>& out_script_handles)
{
    UpdateSceneEnvironment(scene.env);

    ChosenCamera chosen{};

    // v2 tex 槽位 → 纹理 content id（路径为键缓存去重，对齐编辑器
    // SceneRuntimeSynchronizer._worldTextureIds 语义；失败槽留哨兵走白材质）。
    const auto import_texture = [&](const std::string& raw) -> std::uint64_t {
        const auto path = ResolveAssetPath(raw, package_path);
        if (const auto it = texture_cache.find(path); it != texture_cache.end())
            return it->second;
        const auto id = PipelineImportCookedTexture(path.c_str());
        if (id == 0xFFFFFFFFFFFFFFFFull)
            std::fprintf(stderr, "[player] texture import failed: %s\n", path.c_str());
        else
            std::fprintf(stderr, "[player] bound world texture '%s' (content=%llu)\n",
                         path.c_str(), static_cast<unsigned long long>(id));
        texture_cache[path] = id;
        return id;
    };

    for (auto& e : scene.entities) {
        if (e.mesh_path.empty()) continue;
        const auto mesh_path = ResolveAssetPath(e.mesh_path, package_path);
        const std::uint64_t request = ImportMeshAsync(mesh_path.c_str());
        if (request == 0) {
            std::fprintf(stderr, "[player] ImportMeshAsync rejected: %s\n", mesh_path.c_str());
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
        if (status != 1 || mesh_id == 0xFFFFFFFFFFFFFFFFull) { // 1 = Success（编辑器同值）；content id 0 是合法首槽，只认 0xFFFFFFFF 哨兵
            std::fprintf(stderr, "[player] mesh import failed (status=%u): %s\n",
                         status, mesh_path.c_str());
            continue;
        }
        // 纹理槽位数组：有任一槽位则长度 = 最高非空洞槽 +1（空洞/失败 =
        // 0xFFFFFFFF 哨兵，引擎按槽回退默认材质）；全空保持 {0} 白材质
        // （primal-engine 同款默认）。
        std::uint64_t texture_ids[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        std::uint32_t texture_count = 1;
        int last_slot = -1;
        for (std::uint32_t s = 0; s < kMaxTextureSlots; ++s)
            if (!e.tex_paths[s].empty()) last_slot = static_cast<int>(s);
        if (last_slot >= 0) {
            texture_count = static_cast<std::uint32_t>(last_slot) + 1;
            for (std::uint32_t s = 0; s < texture_count; ++s) {
                texture_ids[s] = e.tex_paths[s].empty()
                    ? 0xFFFFFFFFFFFFFFFFull
                    : import_texture(e.tex_paths[s]);
            }
        }
        const std::uint64_t entity =
            PipelineRegisterMeshEntity(mesh_id, texture_ids, texture_count);
        if (entity == 0xFFFFFFFFFFFFFFFFull) { // ECS slot 0 合法，只认 0xFFFFFFFF 哨兵（同 CreateEntity 约定）
            std::fprintf(stderr, "[player] PipelineRegisterMeshEntity failed: %s\n",
                         mesh_path.c_str());
            continue;
        }
        SetEntityTransform(static_cast<std::uint32_t>(entity), e.pos, e.rot, e.scl);
        e.engine_entity = static_cast<std::uint32_t>(entity);
        out_mesh_entities.push_back(entity);
    }

    for (auto& e : scene.entities) {
        // 灯光/相机/纯脚本实体走 CreateEntity（带网格的已在上一循环建过）。
        if (!e.has_light && !e.has_camera && e.script_path.empty()) continue;
        if (e.engine_entity != kInvalidEntityId) continue;
        const std::uint32_t entity = CreateEntity(e.pos[0], e.pos[1], e.pos[2]);
        if (entity == kInvalidEntityId) {  // 只认哨兵——slot 0 是合法分配（primal-engine 同约定）
            std::fprintf(stderr, "[player] CreateEntity failed: %s\n", e.name.c_str());
            continue;
        }
        SetEntityTransform(entity, e.pos, e.rot, e.scl);
        e.engine_entity = entity;
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
            chosen.pos[0] = e.pos[0]; chosen.pos[1] = e.pos[1]; chosen.pos[2] = e.pos[2];
            chosen.rot[0] = e.rot[0]; chosen.rot[1] = e.rot[1]; chosen.rot[2] = e.rot[2];
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
    // v2：脚本挂载（scripting S1）。mesh 实体用注册时的引擎 id，其余用上一
    // 循环回填的 id；坏脚本/缺文件由 ScriptAPI 报 stderr 并返回哨兵，宿主继续。
    for (auto& e : scene.entities) {
        if (e.script_path.empty() || e.engine_entity == kInvalidEntityId) continue;
        const auto lua_path = ResolveAssetPath(e.script_path, package_path);
        std::vector<const char*> keys, values;
        keys.reserve(e.script_params.size());
        values.reserve(e.script_params.size());
        for (const auto& kv : e.script_params) {
            keys.push_back(kv.first.c_str());
            values.push_back(kv.second.c_str());
        }
        const std::uint64_t handle = ScriptCreateForEntity(
            e.engine_entity, lua_path.c_str(),
            keys.empty() ? nullptr : keys.data(),
            values.empty() ? nullptr : values.data(),
            static_cast<std::uint32_t>(keys.size()));
        if (handle == 0xFFFFFFFFFFFFFFFFull)
            std::fprintf(stderr, "[player] script attach failed: %s (entity '%s')\n",
                         lua_path.c_str(), e.name.c_str());
        else
            std::fprintf(stderr, "[player] script attached '%s' → entity '%s' (handle=%llu)\n",
                         lua_path.c_str(), e.name.c_str(),
                         static_cast<unsigned long long>(handle));
        out_script_handles.push_back(handle);
    }

    out_camera = chosen;
    return 1;
}

// --- Run 录制（Part B，Docs/2026-09-05 规划）：渲染帧与捕获帧 1:1 ---
// 主循环每帧 PipelineRenderFrame 之后对离屏 RT 调 SaveRenderTargetAsPng，
// 一张 PNG 对应一次真实渲染、无丢帧（外部屏幕录制做不到）；读回 + PNG 编码
// 会拖慢循环，这是分析模式的可接受代价。视频编码不做在本进程——帧序列留给
// 编辑器侧用外部 ffmpeg 统一处理。

struct RecordConfig {
    bool enabled = false;
    std::string directory;     // frame_%04d.png 输出目录
    long max_frames = 0;       // 捕获帧上限（受 stride 影响计的是捕获数）；0 = 录到窗口关闭
    long stride = 1;           // 每 N 个渲染帧捕获 1 帧
    std::string metrics_path;  // 可选 CSV：frame,meanLuma,minLuma,maxLuma,captureMs
};

// IEEE 754 half → float（DebugReadRenderTarget 返回 RGBA16F）。不用
// _Float16——Linux x86 工具链支持不齐。
float HalfToFloat(std::uint16_t half)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
    const std::uint32_t exp = (half >> 10) & 0x1Fu;
    const std::uint32_t man = half & 0x03FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            // 半精度 subnormal → 规格化 float：左移找隐含位，指数同步回退。
            std::uint32_t e = 0;
            std::uint32_t m = man;
            while ((m & 0x0400u) == 0) { m <<= 1; ++e; }
            bits = sign | ((113u - e) << 23) | ((m & 0x03FFu) << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (man << 13); // inf/nan
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    float out = 0.f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Rec.709 线性亮度统计，HDR 原值不截断（对齐 Primal.CliFramework/FrameMetrics
// 与 editor.metrics 的口径，两条通道的数字可直接对比）。
struct FrameLumaStats { double mean_luma; float min_luma; float max_luma; };

FrameLumaStats ComputeLumaStats(const std::uint16_t* rgba16f, std::size_t pixel_count)
{
    if (pixel_count == 0) return {0.0, 0.f, 0.f};
    double sum = 0.0;
    float mn = FLT_MAX, mx = -FLT_MAX;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const float r = HalfToFloat(rgba16f[i * 4 + 0]);
        const float g = HalfToFloat(rgba16f[i * 4 + 1]);
        const float b = HalfToFloat(rgba16f[i * 4 + 2]);
        const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        sum += luma;
        mn = std::min(mn, luma);
        mx = std::max(mx, luma);
    }
    return {sum / static_cast<double>(pixel_count), mn, mx};
}

bool EnsureDirectory(const std::string& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        std::fprintf(stderr, "[player] cannot create record dir %s: %s\n",
                     path.c_str(), ec.message().c_str());
    }
    return !ec;
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
    bool fixed_size = false; // --fixed：窗口不可缩放（显示分辨率=渲染分辨率）
    std::uint32_t render_mode = 3; // Deferred+Shadow+AO+GI（编辑器默认档一致）
    std::uint32_t editor_mode = 0; // 0=真 runtime 路径；1=编辑器路径（--editor-mode 诊断旗标）
    RecordConfig record;

    for (int i = 1; i < argc; ++i) {
        const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (std::strcmp(argv[i], "--scene") == 0 && next()) scene_path = argv[i];
        else if (std::strcmp(argv[i], "--width") == 0 && next()) width = static_cast<std::uint32_t>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--height") == 0 && next()) height = static_cast<std::uint32_t>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--debug") == 0) debug_backend = true;
        else if (std::strcmp(argv[i], "--fixed") == 0) fixed_size = true;
        else if (std::strcmp(argv[i], "--mode") == 0 && next()) render_mode = static_cast<std::uint32_t>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--editor-mode") == 0 && next()) editor_mode = static_cast<std::uint32_t>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--record") == 0 && next()) {
            record.enabled = true;
            record.directory = argv[i];
        }
        else if (std::strcmp(argv[i], "--record-frames") == 0 && next()) record.max_frames = std::atol(argv[i]);
        else if (std::strcmp(argv[i], "--record-every") == 0 && next()) record.stride = std::atol(argv[i]);
        else if (std::strcmp(argv[i], "--record-metrics") == 0 && next()) record.metrics_path = argv[i];
        else {
            std::fprintf(stderr, "[player] unknown arg: %s\n", argv[i]);
            std::fprintf(stderr, "usage: PrimalPlayer --scene <pkg> [--width W] [--height H] [--fixed] [--debug] [--mode 0..3] [--editor-mode 0|1]\n");
            std::fprintf(stderr, "                   [--record <dir>] [--record-frames N] [--record-every N] [--record-metrics <file.csv>]\n");
            return 2;
        }
    }
    if (scene_path.empty() || width == 0 || height == 0) {
        std::fprintf(stderr, "usage: PrimalPlayer --scene <pkg> [--width W] [--height H]\n");
        return 2;
    }
    if (record.enabled) {
        if (record.directory.empty()) {
            std::fprintf(stderr, "[player] --record requires a directory\n");
            return 2;
        }
        if (record.stride < 1) record.stride = 1;
        if (record.max_frames < 0) record.max_frames = 0;
        if (!EnsureDirectory(record.directory)) return 2;
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

    // ImportMeshAsync 依赖 JobSystem/AsyncResourceLoader（LoadMeshAsync 在
    // JobSystem 未运行时直接拒绝请求）——编辑器在启动序列里调
    // InitializeAssetSystem，宿主此前漏调导致所有 mesh 实体被拒（存量问题③）。
    if (InitializeAssetSystem() == 0) {
        std::fprintf(stderr, "[player] InitializeAssetSystem failed — mesh import unavailable\n");
        return 3;
    }

    void* host_view = nullptr;
#if defined(PRIMAL_PLAYER_WINDOW)
    host_view = PlayerWindowCreate(static_cast<int>(width), static_cast<int>(height),
                                   ("Primal Player — " + scene.scene_name).c_str(),
                                   fixed_size ? 0 : 1);
    if (!host_view) { std::fprintf(stderr, "[player] window create failed\n"); return 3; }
#endif

    SetPipelineViewportSize(width, height);
    const std::uint64_t device = GetEngineDeviceHandle();
    if (device == 0 || CreateStandardRenderPipeline(device) == 0) {
        std::fprintf(stderr, "[player] CreateStandardRenderPipeline failed\n");
        return 3;
    }
    PipelineSetEditorMode(editor_mode); // 0=runtime；1=编辑器路径（诊断对照用）
    PipelineApplyModePreset(render_mode);

    ChosenCamera camera{};
    std::vector<std::uint64_t> mesh_entities;
    std::vector<std::uint64_t> script_handles;
    std::unordered_map<std::string, std::uint64_t> texture_cache;
    if (InjectScene(scene, scene_path, texture_cache, camera, mesh_entities, script_handles) == 0 ||
        camera.entity_id == kInvalidEntityId) {
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

    std::fprintf(stderr,
                 "[player] running '%s' (%u entities, %ux%u, mode %u, pipeline %s) — "
                 "WASD/QE 移动 · 拖拽/方向键视角 · 滚轮调速 · Shift 加速 · Esc 退出\n",
                 scene.scene_name.c_str(), static_cast<unsigned>(scene.entities.size()),
                 width, height, render_mode, editor_mode == 0 ? "runtime" : "editor-path");

    // 主循环：事件泵 + 渲染都在主线程（编辑器 DispatcherTimer 同款约束，
    // 规避 PipelineRenderFrame 内部互斥锁的双线程崩溃）。无窗口平台跑固定
    // 帧数即退（CI/冒烟用）。
    const bool windowed = surface_id != kInvalidSurfaceId;
    int remaining_frames = windowed ? -1 : 120;

    // 飞行相机（P3 相机桥）：从选中相机的初始 transform 播种，每帧积分
    // 输入后回写 SetEntityTransform。yaw=0 朝 +Z（默认机位约定）；欧拉
    // 写回 (pitch, yaw, 0)。单位勘误（2026-10-11）：SetEntityTransform 的
    // 旋转全链按弧度消费（simd_quaternion 吃 rad，编辑器推送无转换），
    // 灵敏度/钳位/步进全部按弧度。无窗口路径输入恒零，相机保持静止。
    struct FlyCamera {
        float pos[3]{0.f, 0.f, 0.f};
        float yaw = 0.f;   // 弧度
        float pitch = 0.f; // 弧度
        float speed = 4.f; // 单位/秒
        double last_seconds = 0.0;
        bool seeded = false;
    } fly;
    fly.pos[0] = camera.pos[0]; fly.pos[1] = camera.pos[1]; fly.pos[2] = camera.pos[2];
    fly.yaw = camera.rot[1];
    fly.pitch = camera.rot[0];

    // 帧计时（PipelineTick 的 dt 源）。
    auto last_frame_time = std::chrono::steady_clock::now();

    // 录制状态：rendered 计渲染帧，captured 计已捕获帧（第 1、1+stride、…
    // 渲染帧落盘，文件名按捕获序号）。录满 max_frames 即退出进程——编辑器侧
    // 依赖"进程退出 = 录制完成"的契约做后续编码/通知。
    long rendered_frames = 0;
    long captured_frames = 0;
    double total_capture_ms = 0.0;
    FILE* metrics_file = nullptr;
    if (record.enabled && !record.metrics_path.empty()) {
        metrics_file = std::fopen(record.metrics_path.c_str(), "w");
        if (!metrics_file) {
            std::fprintf(stderr, "[player] metrics csv open failed: %s（继续录制，仅无遥测）\n",
                         record.metrics_path.c_str());
        } else {
            std::fprintf(metrics_file, "frame,meanLuma,minLuma,maxLuma,captureMs\n");
        }
    }

    while (true) {
#if defined(PRIMAL_PLAYER_WINDOW)
        if (windowed && !PlayerWindowPump()) break;
#endif

        // 宿主帧 tick（scripting S1）：script → physics → pfx → bindings
        // （EngineDLL 顺序契约），渲染前驱动。dt 钳 0.1s 防切后台回来跳变。
        {
            const auto frame_now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(frame_now - last_frame_time).count();
            last_frame_time = frame_now;
            if (dt < 0.f) dt = 0.f;
            if (dt > 0.1f) dt = 0.1f;
            PipelineTick(dt);
        }

#if defined(PRIMAL_PLAYER_WINDOW)
        if (windowed) {
            PlayerInputState input{};
            PlayerWindowPollInput(&input);
            const auto now_ns = std::chrono::steady_clock::now().time_since_epoch();
            const double seconds = std::chrono::duration<double>(now_ns).count();
            if (!fly.seeded) { fly.last_seconds = seconds; fly.seeded = true; }
            const float dt = static_cast<float>(
                seconds - fly.last_seconds < 0.1 ? seconds - fly.last_seconds : 0.1);
            fly.last_seconds = seconds;

            // 视角：拖拽（点→弧度，≈0.15°/px 手感）+ 方向键步进；pitch 钳 ±89°。
            constexpr float kLookSensitivity = 0.15f * 3.14159265f / 180.f;
            constexpr float kArrowStepRad = 2.f * 3.14159265f / 180.f;
            constexpr float kPitchClamp = 89.f * 3.14159265f / 180.f;
            fly.yaw += input.look_dx * kLookSensitivity;
            fly.pitch += input.look_dy * kLookSensitivity;
            if (input.keys & kPlayerKeyLeftArrow) fly.yaw -= kArrowStepRad;
            if (input.keys & kPlayerKeyRightArrow) fly.yaw += kArrowStepRad;
            if (input.keys & kPlayerKeyUpArrow) fly.pitch -= kArrowStepRad;
            if (input.keys & kPlayerKeyDownArrow) fly.pitch += kArrowStepRad;
            if (fly.pitch > kPitchClamp) fly.pitch = kPitchClamp;
            if (fly.pitch < -kPitchClamp) fly.pitch = -kPitchClamp;

            // 速度：滚轮指数调节，Shift ×3。
            if (input.scroll_dy != 0.f) {
                const float scaled = fly.speed * std::exp(-input.scroll_dy * 0.05f);
                fly.speed = scaled < 0.5f ? 0.5f : (scaled > 50.f ? 50.f : scaled);
            }
            const float speed =
                fly.speed * ((input.keys & kPlayerKeyShift) ? 3.f : 1.f);

            // 相机系移动：forward=(sin yaw, 0, cos yaw)，right=(cos yaw, 0, -sin yaw)。
            const float fx = std::sin(fly.yaw), fz = std::cos(fly.yaw);
            const float rx = std::cos(fly.yaw), rz = -std::sin(fly.yaw);
            if (input.keys & kPlayerKeyW) { fly.pos[0] += fx * speed * dt; fly.pos[2] += fz * speed * dt; }
            if (input.keys & kPlayerKeyS) { fly.pos[0] -= fx * speed * dt; fly.pos[2] -= fz * speed * dt; }
            if (input.keys & kPlayerKeyD) { fly.pos[0] += rx * speed * dt; fly.pos[2] += rz * speed * dt; }
            if (input.keys & kPlayerKeyA) { fly.pos[0] -= rx * speed * dt; fly.pos[2] -= rz * speed * dt; }
            if (input.keys & kPlayerKeyE) fly.pos[1] += speed * dt;
            if (input.keys & kPlayerKeyQ) fly.pos[1] -= speed * dt;

            const float rot[3] = { fly.pitch, fly.yaw, 0.f };
            const float scl[3] = { 1.f, 1.f, 1.f };
            SetEntityTransform(camera.entity_id, fly.pos, rot, scl);
        }
#endif
        if (PipelineRenderFrame(target, camera_id, surface_id) == 0) {
            std::fprintf(stderr, "[player] PipelineRenderFrame failed\n");
            break;
        }
        if (windowed)
            BlitRenderTargetToSurface(target, surface_id);
        ++rendered_frames;

        if (record.enabled && ((rendered_frames - 1) % record.stride) == 0) {
            const auto capture_start = std::chrono::steady_clock::now();
            char frame_name[32];
            std::snprintf(frame_name, sizeof(frame_name), "frame_%04d.png",
                          static_cast<int>(captured_frames));
            const std::string png_path = record.directory + "/" + frame_name;
            if (!SaveRenderTargetAsPng(target, png_path.c_str())) {
                std::fprintf(stderr, "[player] record: SaveRenderTargetAsPng failed at rendered frame %ld\n",
                             rendered_frames);
                break;
            }
            if (metrics_file) {
                // PNG 落盘后二次读回做遥测（SaveRenderTargetAsPng 内部读回不外露
                // 数据）。逐帧 fflush：分析场景常伴随崩溃取证，不能丢尾部样本。
                std::vector<std::uint16_t> pixels(static_cast<std::size_t>(width) * height * 4);
                if (DebugReadRenderTarget(target, pixels.data(),
                                          static_cast<std::uint64_t>(pixels.size()) * 2) == 1) {
                    const auto stats = ComputeLumaStats(
                        pixels.data(), static_cast<std::size_t>(width) * height);
                    std::fprintf(metrics_file, "%ld,%.6f,%.6f,%.6f,",
                                 captured_frames, stats.mean_luma,
                                 static_cast<double>(stats.min_luma),
                                 static_cast<double>(stats.max_luma));
                } else {
                    std::fprintf(metrics_file, "%ld,,,", captured_frames);
                }
            }
            const double capture_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - capture_start).count();
            total_capture_ms += capture_ms;
            if (metrics_file) {
                std::fprintf(metrics_file, "%.2f\n", capture_ms);
                std::fflush(metrics_file);
            }
            ++captured_frames;
            if (captured_frames % 60 == 0) {
                std::fprintf(stderr, "[player] recorded %ld frames → %s\n",
                             captured_frames, record.directory.c_str());
            }
            if (record.max_frames > 0 && captured_frames >= record.max_frames) {
                std::fprintf(stderr, "[player] record complete: %ld frames → %s\n",
                             captured_frames, record.directory.c_str());
                break;
            }
        }

        if (!windowed && --remaining_frames <= 0) break;
    }

    if (metrics_file) std::fclose(metrics_file);
    if (record.enabled && captured_frames > 0) {
        std::fprintf(stderr,
                     "[player] record summary: frames=%ld rendered=%ld avgCaptureMs=%.2f dir=%s\n",
                     captured_frames, rendered_frames,
                     total_capture_ms / static_cast<double>(captured_frames),
                     record.directory.c_str());
    }

    if (surface_id != kInvalidSurfaceId)
        RemoveRenderSurface(surface_id);
    DestroyRenderTarget(target);
    DestroyStandardRenderPipeline();
    // 脚本先行拆除（destroy 钩子解绑事件订阅，防止后续 tick 派发悬空句柄）。
    for (const auto handle : script_handles)
        ScriptDestroyForEntity(handle);
    // 趁 graphics device 仍存活释放 mesh/texture content registries——
    // AssetAPI 头注释：不这样做事后静态析构会对非空 free-list abort（存量问题②）。
    ShutdownAssetSystem();
    ShutdownEngine();
#if defined(PRIMAL_PLAYER_WINDOW)
    PlayerWindowDestroy();
#endif
    std::fprintf(stderr, "[player] clean exit\n");
    return 0;
}
