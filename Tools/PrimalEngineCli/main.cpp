// =============================================================================
// primal-engine — 离线通道 one-shot CLI（Docs/CLI_Agent_Interface_Plan.md §3.1/§10）
//
//   primal-engine engine info                    引擎健康检查 + 设备信息（JSON）
//   primal-engine render --out f.png [--width W] [--height H] [--frames N]
//                                                离屏渲染测试场景存 PNG
//   primal-engine schema                         离线 verb 表（聚合 schema 的另一半）
//
// 输出契约与 primal 客户端一致：stdout=JSON、日志=stderr、退出码 0/2/4。
// 无状态：跑完即退，天然可沙箱（§3.1 离线动词）。
//
// EngineDLL 交互说明：本工具是 C ABI 消费者（与编辑器/脚本宿主平级，§12.1）。
// 所需导出在 EngineDllImports 段手工声明——这是**函数声明镜像**而非 POD 内存
// 镜像（§9 风险 #2 禁的是后者）；符号存在性由链接期 dyld 校验。
// =============================================================================

#include "PipelineLightDesc.h"
#include "CoreDiag.h"

// A9 PR2: ai_asset_pipeline_params 的权威定义（§9 风险 #2 禁止 POD 手工镜像，
// 调用方一律 include 本头）+ 产物读回校验（SceneBlobReader 认 UVLM 段）。
#include "AIAssetPipelineParams.h"
#include "pipeline/SceneBlobReader.h"
// bake-sdf: per-mesh SDF 生成（mesh + generate_sdf）
#include "Geometry.h"
#include "common/SDF.h"
#include "common/CardBake.h"
// sc-bake-surface: per-card 真实表面页烘焙（SC per-mesh 计划 P1）
#include "common/CardSurfaceBake.h"
#include <stb_image.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <zlib.h>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

// EngineDLL 诊断内部接口（全限定声明——必须在匿名命名空间之外，否则会在
// anonymous:: 下创建同名 namespace 遮蔽全局 ::primal 的限定查找）。
namespace primal::engine_dll::diag {
const void* GetCrashSnapshot(std::uint32_t* outLen);
}

namespace {

// ---- PFX（可编程粒子框架）函数声明镜像（§9：非 POD 内存镜像）----
extern "C" {
using PfxDebugDumpFn = std::uint32_t (*)(const char*, std::uint32_t, float, char*, std::uint32_t, std::uint32_t*);
}
extern "C" std::uint32_t PfxSystemEnsure();
extern "C" std::uint32_t PfxDebugDump(const char* asset_path, std::uint32_t frames, float dt,
                                      char* out, std::uint32_t cap, std::uint32_t* out_len);

bool DirExists(const char* path)
{
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// --- JSON 输出小助手（离线 verb 的载荷都是扁平对象，不值得引依赖） ---

std::string JsonEscape(const char* text)
{
    std::string out = "\"";
    for (const char* p = text; *p != '\0'; ++p)
    {
        switch (*p)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += *p;
        }
    }
    out += '"';
    return out;
}

void PrintOk(const std::string& dataJson)
{
    std::printf("{\"ok\":true,\"data\":%s}\n", dataJson.c_str());
}

void PrintError(const char* code, const char* message, const char* hint)
{
    std::printf("{\"ok\":false,\"error\":{\"code\":%s,\"message\":%s,\"hint\":%s}}\n",
                JsonEscape(code).c_str(), JsonEscape(message).c_str(), JsonEscape(hint).c_str());
}

void LogToStderr(const char* message)
{
    std::fprintf(stderr, "[primal-engine] %s\n", message);
}

// §9 风险 #3：引擎按 cwd 相对解析 shader blob。one-shot CLI 必须把 cwd 锚到
// 可执行目录（复刻编辑器 C# 侧 AnchorWorkingDirectoryToExecutable 的语义），
// 否则从任意工作目录调用都会静默丢光渲染通道。
// C++ 输出目录（Darwin/DebugEditor）与 C# 输出目录（Darwin/PrimalEditor_Avalonia/
// DebugEditor）是同级布局，shader blob 只部署在 C# 侧——锚定后若 ./shaders 缺失
// 则回退到同级 C# 目录（同配置名），并保持清晰日志。
void AnchorWorkingDirectoryToExecutable()
{
#if defined(__APPLE__)
    char path[4096];
    std::uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return;
#elif defined(__linux__)
    char path[4096];
    const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return;
    path[len] = '\0';
#else
    return;
#endif
    char* lastSlash = std::strrchr(path, '/');
    if (lastSlash == nullptr) return;
    *lastSlash = '\0';
    if (chdir(path) != 0) return;

    if (DirExists("shaders"))
    {
        LogToStderr((std::string("cwd anchored to ") + path).c_str());
        return;
    }

    // 同级 C# 输出目录回退：../PrimalEditor_Avalonia/<本目录名>/shaders
    const std::string configName = [](const char* dir) {
        const std::string d = dir;
        const auto pos = d.find_last_of('/');
        return pos == std::string::npos ? d : d.substr(pos + 1);
    }(path);
    const std::string candidate =
        std::string(path) + "/../PrimalEditor_Avalonia/" + configName;
    if (DirExists((candidate + "/shaders").c_str()) && chdir(candidate.c_str()) == 0)
    {
        LogToStderr((std::string("cwd anchored to ") + candidate +
                     " (shaders live on the C# side)").c_str());
        return;
    }

    LogToStderr((std::string("cwd anchored to ") + path +
                 " — WARNING: no ./shaders found, render passes will degrade").c_str());
}

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitEngine = 4;

// --- EngineDLL C ABI 声明镜像（见文件头说明） ---

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using f32 = float;

extern "C" {
// 生命周期 / 设备
u32  InitializeEngine(u32 rhiPlatform, u32 enableDebug);
void ShutdownEngine();
void FlushGPU();
u32  IsEngineInitialized();
u64  GetEngineDeviceHandle();

// 渲染管线
void SetPipelineViewportSize(u32 width, u32 height);
u32  CreateStandardRenderPipeline(u64 deviceHandle);
void DestroyStandardRenderPipeline();
void PipelineSetEditorMode(u32 enable);

// 测试场景装配（与 C# headless-render 同一套导出）
u32  CreateEntity(float pos_x, float pos_y, float pos_z);
u32  CreateCamera(u32 entityId, float fov, float aspect, float nearZ, float farZ);
void SetEntityTransform(u32 entityId, float* position, float* rotation, float* scale);
u64  ProceduralMeshCreate(u32 meshType, float* parameters, u32 parameterCount);
void ProceduralMeshDestroy(u64 contentId);

// 物理仿真（PhysicsAPI.cpp — M2 离线门禁；POD 布局与 EngineDLL/PhysicsAPI.cpp 一致）
u32  PhysicsInitialize(const void* settingsPod);
void PhysicsShutdown(void);
u32  PhysicsSetRunning(u32 running);
void PipelineTick(f32 dtSeconds);
u64  PhysicsBodyCreate(const void* bodyDescPod);
u32  PhysicsBodyGetTransform(u64 bodyId, void* outTransformPod);
u64  PipelineRegisterMeshEntity(u64 geometryContentId, u64* textureContentIds, u32 textureCount);
void PipelineUnregisterMeshEntity(u64 entityId);
u64  PipelineRegisterLightEntity(PipelineLightDesc* desc);
void PipelineUnregisterLightEntity(u64 entityId);

// 离屏渲染
u64  CreateRenderTarget(u32 width, u32 height, u32 format);
void DestroyRenderTarget(u64 targetHandle);
u32  PipelineRenderFrame(u64 targetHandle, u32 cameraId, u32 surfaceId);
int  SaveRenderTargetAsPng(u64 targetHandle, const char* path);

// 资产（libContentTools）
u32 CookMesh(const char* inputPath, const char* outputPath);
u32 CookMeshWithPipeline(const char* inputPath, const char* outputPath,
                         ai_asset_pipeline_params* params);

// ContentTools/TextureImporter.cpp 内部类型（未公开头文件；字段序即 ABI，
// 与 ContentTools/TextureImporter.cpp:727 的 cook_result 定义逐字段对齐——
// 修改那边必须同步这里）。
struct ContentToolsCookResult
{
    u32 info_width;
    u32 info_height;
    u32 info_array_size;
    u32 info_mip_levels;
    u32 info_format;
    u32 info_flags;
    u32 import_error;
    u32 sources_len;
    u32 source_count;
    u32 settings_dimension;
    u32 settings_mip_levels;
    f32 settings_alpha_threshold;
    u32 settings_prefer_bc7;
    u32 settings_output_format;
    u32 settings_compress;
    u32 slices_size;
    u32 icon_size;
    u32 import_succeeded;
};

u32 CookTextureQuery(
    const char* input,
    u32 dimension, f32 alpha_threshold,
    u32 mip_levels_setting, u32 prefer_bc7,
    u32 output_format, u32 compress,
    ContentToolsCookResult* out_info);
u32 CookTexture(
    const char* input,
    u32 dimension, f32 alpha_threshold,
    u32 mip_levels_setting, u32 prefer_bc7,
    u32 output_format, u32 compress,
    u8* slices_buffer, u32 slices_capacity, u32* out_slices_size,
    u8* icon_buffer, u32 icon_capacity, u32* out_icon_size,
    ContentToolsCookResult* out_info);
} // extern "C"

// RHIPlatform 数值与 PrimalEditor_Avalonia/NativeEngineBridge.cs 保持一致
constexpr u32 kPlatformD3D12 = 1;
constexpr u32 kPlatformVulkan = 2;
constexpr u32 kPlatformMetal  = 3;

constexpr u32 kRenderTargetFormatRgba16F = 1;
constexpr u32 kInvalidSurfaceId = 0xFFFFFFFFu; // 离屏：无 surface（§5.1 render 行）

int VerbEngineInfo()
{
#if defined(_WIN32)
    // MSVC 分支的 EngineAPI.cpp 尚未导出 InitializeEngine（§9 风险 #5/#6：
    // 平台矩阵按可用性逐步放开）。
    PrintError("NOT_SUPPORTED", "engine info 在 Windows 上尚未可用（EngineDLL MSVC 分支未导出 InitializeEngine）",
               "使用 macOS/Linux 运行离线 verbs");
    return kExitEngine;
#else
    const u32 platform =
#if defined(__APPLE__)
        kPlatformMetal;
#else
        kPlatformVulkan;
#endif

    if (InitializeEngine(platform, 0) == 0)
    {
        PrintError("ENGINE_ERROR", "InitializeEngine 失败（常见原因：shader blob 相对 cwd 缺失）",
                   "确认从 Darwin/<CONFIG> 目录运行，或先用 build 脚本部署 shader blob");
        return kExitEngine;
    }

    char data[256];
    std::snprintf(data, sizeof(data),
                  "{\"engine\":\"primal\",\"initialized\":true,\"platform\":\"%s\",\"deviceHandle\":\"0x%llx\"}",
#if defined(__APPLE__)
                  "metal",
#else
                  "vulkan",
#endif
                  static_cast<unsigned long long>(GetEngineDeviceHandle()));
    PrintOk(data);
    ShutdownEngine();
    return kExitOk;
#endif
}

int VerbRender(int argc, char** argv)
{
#if defined(_WIN32)
    (void)argc; (void)argv;
    PrintError("NOT_SUPPORTED", "render 在 Windows 上尚未可用", "使用 macOS/Linux 运行离线 verbs");
    return kExitEngine;
#else
    const char* outPath = nullptr;
    u32 width = 1280, height = 720;
    int frames = 4;

    for (int i = 0; i < argc; ++i)
    {
        const auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };
        if (std::strcmp(argv[i], "--out") == 0)
        {
            outPath = next();
            if (!outPath) { PrintError("INVALID_PARAMS", "--out 缺少路径参数", "示例: --out /tmp/render.png"); return kExitUsage; }
        }
        else if (std::strcmp(argv[i], "--width") == 0 && next()) width = static_cast<u32>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--height") == 0 && next()) height = static_cast<u32>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--frames") == 0 && next()) frames = std::atoi(argv[i]);
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --out/--width/--height/--frames");
            return kExitUsage;
        }
    }

    if (!outPath || width == 0 || height == 0 || frames < 1 || frames > 240)
    {
        PrintError("INVALID_PARAMS", "参数无效（需要 --out；width/height > 0；frames 1..240）",
                   "示例: primal-engine render --out /tmp/render.png --width 640 --height 360 --frames 4");
        return kExitUsage;
    }

    LogToStderr("initializing engine (offline)…");
    const u32 platform =
#if defined(__APPLE__)
        kPlatformMetal;
#else
        kPlatformVulkan;
#endif
    if (InitializeEngine(platform, 0) == 0)
    {
        PrintError("ENGINE_ERROR", "InitializeEngine 失败", "确认 shader blob 与 dylib 同目录并从该目录运行");
        return kExitEngine;
    }

    auto fail = [&](const char* message) {
        PrintError("ENGINE_ERROR", message, "重跑一次；连续失败请附 stderr 日志上报");
        // 与 C# headless-render 相同的策略：失败即退出、由 OS 回收。
        // 途中资源未逐层反注册就 ShutdownEngine 会 SIGSEGV（编辑器 OnExit
        // 的注释记录过同类崩溃）。
        return kExitEngine;
    };

    // 与 C# --headless-render 相同的装配序列（§2.1"离线 render verb 蓝本"）。
    SetPipelineViewportSize(width, height);
    const u64 device = GetEngineDeviceHandle();
    if (device == 0 || CreateStandardRenderPipeline(device) == 0) return fail("CreateStandardRenderPipeline 失败");
    PipelineSetEditorMode(1);

    const u32 cameraEntity = CreateEntity(0.f, 1.f, -12.f);
    if (cameraEntity == 0xFFFFFFFFu) return fail("CreateEntity 失败");
    // slot 0 是合法分配（首个 slot）——只认 0xFFFFFFFF 哨兵（与 C# headless 一致）。
    const u32 camera = CreateCamera(cameraEntity, 1.f / 3.f /*60°*/,
                                    static_cast<float>(width) / static_cast<float>(height), 0.1f, 1000.f);
    if (camera == 0xFFFFFFFFu) return fail("CreateCamera 失败");

    float sphereParams[3] = { 0.5f, 24.f, 16.f };
    const u64 geometry = ProceduralMeshCreate(0, sphereParams, 3);
    if (geometry == 0xFFFFFFFFFFFFFFFFu) return fail("ProceduralMeshCreate 失败");

    u64 entities[4];
    const float xs[4] = { -6.f, -2.f, 2.f, 6.f };
    u64 textureIds[1] = { 0 };
    for (int i = 0; i < 4; ++i)
    {
        entities[i] = PipelineRegisterMeshEntity(geometry, textureIds, 1);
        float pos[3] = { xs[i], 0.f, 0.f };
        float rot[3] = { 0.f, 0.f, 0.f };
        float scl[3] = { 1.f, 1.f, 1.f };
        SetEntityTransform(static_cast<u32>(entities[i]), pos, rot, scl);
    }

    PipelineLightDesc light{};
    light.type = 0;                        // directional
    light.color[0] = light.color[1] = light.color[2] = 1.f;
    light.intensity = 5.f;
    light.direction[0] = -1.f; light.direction[1] = -1.f; light.direction[2] = 0.f;
    light.is_enabled = 1;
    const u64 lightEntity = PipelineRegisterLightEntity(&light);

    const u64 target = CreateRenderTarget(width, height, kRenderTargetFormatRgba16F);
    if (target == 0) return fail("CreateRenderTarget 失败");

    for (int i = 0; i < frames; ++i)
    {
        if (PipelineRenderFrame(target, camera, kInvalidSurfaceId) == 0)
            return fail("PipelineRenderFrame 失败");
    }

    const int saved = SaveRenderTargetAsPng(target, outPath);
    if (saved == 0) return fail("SaveRenderTargetAsPng 失败");

    // 清理顺序与编辑器 OnExit 的教训一致：先逐层反注册，再关引擎。
    for (int i = 3; i >= 0; --i) PipelineUnregisterMeshEntity(entities[i]);
    ProceduralMeshDestroy(geometry);
    if (lightEntity != 0) PipelineUnregisterLightEntity(lightEntity);
    DestroyRenderTarget(target);
    DestroyStandardRenderPipeline();
    ShutdownEngine();

    char data[512];
    std::snprintf(data, sizeof(data),
                  "{\"path\":%s,\"width\":%u,\"height\":%u,\"frames\":%d}",
                  JsonEscape(outPath).c_str(), width, height, frames);
    PrintOk(data);
    return kExitOk;
#endif
}

// physics-sim（M2 T3 离线门禁）：离屏物理仿真——地面 + 动态盒从 y=3 落地，
// 固定 1/60 步长驱动 PipelineTick，每帧渲染进离屏 RT，末帧写 PNG。
// 物理数值断言：终态盒 y ∈ [0.44, 0.56]（半高 0.5）。这是"接通即有测试 +
// 渲染可视化"纪律的 CLI 形态（设计文档 §8.4）。
// pfx-debug：离线驱动 PFX 发射器并 dump 统计 JSON（架构文档 §7-N2 pfx.debug）。
//   primal-engine pfx-debug --asset a.pfx_emitter [--frames 120] [--dt 0.0166667]
int VerbPfxDebug(int argc, char** argv)
{
    const char* asset = nullptr;
    std::uint32_t frames = 120;
    float dt = 1.f / 60.f;
    auto next = [&](int i) -> const char* { return (i + 1 < argc) ? argv[i + 1] : nullptr; };
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--asset") == 0) {
            asset = next(i);
            if (!asset) { PrintError("INVALID_PARAMS", "--asset 缺少路径参数", "示例: --asset /tmp/jet.pfx_emitter"); return kExitUsage; }
        } else if (std::strcmp(argv[i], "--frames") == 0 && next(i)) {
            frames = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--dt") == 0 && next(i)) {
            dt = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("pfx-debug --asset <pfx_emitter> [--frames N] [--dt SEC]\n");
            return kExitOk;
        }
    }
    if (!asset) {
        PrintError("INVALID_PARAMS", "--asset 缺少路径参数", "示例: primal-engine pfx-debug --asset /tmp/jet.pfx_emitter --frames 120");
        return kExitUsage;
    }

    // 动态缓冲：先探测长度再取数（ABI 惯例 out/cap/out_len）
    std::uint32_t len = 0;
    if (!PfxDebugDump(asset, frames, dt, nullptr, 0, &len) || len == 0) {
        PrintError("PFX_DUMP_FAILED", "发射器加载/驱动失败", "检查 --asset 路径与 JSON 合法性（stderr 有详情）");
        return kExitEngine;
    }
    std::vector<char> buf(len + 1);
    if (!PfxDebugDump(asset, frames, dt, buf.data(), len, &len)) {
        PrintError("PFX_DUMP_FAILED", "二次调用失败（非确定性问题？）", "");
        return kExitEngine;
    }
    buf[len] = '\0';
    std::fputs(buf.data(), stdout);
    std::fputc('\n', stdout);
    return kExitOk;
}

int VerbPhysicsSim(int argc, char** argv)
{
#if defined(_WIN32)
    (void)argc; (void)argv;
    PrintError("NOT_SUPPORTED", "physics-sim 在 Windows 上尚未可用", "使用 macOS/Linux 运行离线 verbs");
    return kExitEngine;
#else
    const char* outPath = nullptr;
    u32 width = 320, height = 180;
    int frames = 120;

    for (int i = 0; i < argc; ++i)
    {
        const auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };
        if (std::strcmp(argv[i], "--out") == 0)
        {
            outPath = next();
            if (!outPath) { PrintError("INVALID_PARAMS", "--out 缺少路径参数", "示例: --out /tmp/phys.png"); return kExitUsage; }
        }
        else if (std::strcmp(argv[i], "--width") == 0 && next()) width = static_cast<u32>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--height") == 0 && next()) height = static_cast<u32>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--frames") == 0 && next()) frames = std::atoi(argv[i]);
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --out/--width/--height/--frames");
            return kExitUsage;
        }
    }

    if (!outPath || width == 0 || height == 0 || frames < 30 || frames > 600)
    {
        PrintError("INVALID_PARAMS", "参数无效（需要 --out；frames 30..600）",
                   "示例: primal-engine physics-sim --out /tmp/phys.png --frames 120");
        return kExitUsage;
    }

    // POD 镜像（布局必须与 EngineDLL/PhysicsAPI.cpp 逐字段一致，static_assert 锁尺寸）
    struct PhysicsSettingsPod
    {
        float gravity[3]; unsigned max_bodies, max_worker_threads; float fixed_dt; unsigned max_sub_steps;
    };
    static_assert(sizeof(PhysicsSettingsPod) == 28, "ABI mismatch vs EngineDLL/PhysicsAPI.cpp");
    struct PhysicsBodyDescPod
    {
        unsigned shape, mode;
        float half[3], position[3], euler[3];
        float mass, restitution, friction;
        unsigned layer, debug_draw_proxy;
    };
    static_assert(sizeof(PhysicsBodyDescPod) == 64, "ABI mismatch vs EngineDLL/PhysicsAPI.cpp");
    struct PhysicsBodyTransformPod
    {
        float position[3], orientation_euler[3], linear_velocity[3]; unsigned sleeping;
    };
    static_assert(sizeof(PhysicsBodyTransformPod) == 40, "ABI mismatch vs EngineDLL/PhysicsAPI.cpp");

    LogToStderr("initializing engine (offline physics-sim)…");
    const u32 platform =
#if defined(__APPLE__)
        kPlatformMetal;
#else
        kPlatformVulkan;
#endif
    if (InitializeEngine(platform, 0) == 0)
    {
        PrintError("ENGINE_ERROR", "InitializeEngine 失败", "确认 shader blob 与 dylib 同目录并从该目录运行");
        return kExitEngine;
    }
    auto fail = [&](const char* message) {
        PrintError("ENGINE_ERROR", message, "重跑一次；连续失败请附 stderr 日志上报");
        return kExitEngine;
    };

    SetPipelineViewportSize(width, height);
    const u64 device = GetEngineDeviceHandle();
    if (device == 0 || CreateStandardRenderPipeline(device) == 0) return fail("CreateStandardRenderPipeline 失败");
    PipelineSetEditorMode(1);

    // 相机（正视坠落平面）
    const u32 cameraEntity = CreateEntity(0.f, 1.5f, -14.f);
    if (cameraEntity == 0xFFFFFFFFu) return fail("CreateEntity 失败");
    const u32 camera = CreateCamera(cameraEntity, 1.f / 3.f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 1000.f);
    if (camera == 0xFFFFFFFFu) return fail("CreateCamera 失败");

    // 网格：地面 20×1×20 + 动态盒 1×1×1（type 3 = box，参数为全尺寸）
    float groundParams[3] = { 20.f, 1.f, 20.f };
    const u64 groundGeo = ProceduralMeshCreate(3, groundParams, 3);
    float boxParams[3] = { 1.f, 1.f, 1.f };
    const u64 boxGeo = ProceduralMeshCreate(3, boxParams, 3);
    if (groundGeo == 0xFFFFFFFFFFFFFFFFull || boxGeo == 0xFFFFFFFFFFFFFFFFull) return fail("ProceduralMeshCreate 失败");

    u64 textureIds[1] = { 0 };
    const u32 groundEntity = PipelineRegisterMeshEntity(groundGeo, textureIds, 1);
    const u32 boxEntity = PipelineRegisterMeshEntity(boxGeo, textureIds, 1);
    if (groundEntity == 0xFFFFFFFFu || boxEntity == 0xFFFFFFFFu) return fail("PipelineRegisterMeshEntity 失败");
    {
        float gp[3] = { 0.f, -0.5f, 0.f }, bp[3] = { 0.f, 3.f, 0.f };
        float rot[3] = { 0.f, 0.f, 0.f }, scl[3] = { 1.f, 1.f, 1.f };
        SetEntityTransform(groundEntity, gp, rot, scl);
        SetEntityTransform(boxEntity, bp, rot, scl);
    }

    PipelineLightDesc light{};
    light.type = 0;
    light.color[0] = light.color[1] = light.color[2] = 1.f;
    light.intensity = 5.f;
    light.direction[0] = -1.f; light.direction[1] = -1.f; light.direction[2] = 0.f;
    light.is_enabled = 1;
    const u64 lightEntity = PipelineRegisterLightEntity(&light);

    // 物理：地面静态 + 动态盒 @ y=3
    PhysicsSettingsPod ps{};
    ps.gravity[0] = 0.f; ps.gravity[1] = -9.81f; ps.gravity[2] = 0.f;
    ps.max_bodies = 4096; ps.max_worker_threads = 0;   // 0 = 单线程确定性
    ps.fixed_dt = 1.f / 60.f; ps.max_sub_steps = 5;
    if (PhysicsInitialize(&ps) == 0) return fail("PhysicsInitialize 失败");

    PhysicsBodyDescPod gd{};
    gd.shape = 0; gd.mode = 0;   // static box
    gd.layer = 0;                // simulation::layers::kStatic
    gd.half[0] = 10.f; gd.half[1] = 0.5f; gd.half[2] = 10.f;
    gd.position[1] = -0.5f;
    gd.restitution = 0.1f; gd.friction = 0.5f;
    const u64 groundBody = PhysicsBodyCreate(&gd);

    PhysicsBodyDescPod bd{};
    bd.shape = 0; bd.mode = 1;   // dynamic box
    bd.layer = 1;                // simulation::layers::kDynamic——POD 零值 = kStatic，
                                 // 动态体落错 broadphase 层会直接穿地（首个实测踩坑）
    bd.half[0] = bd.half[1] = bd.half[2] = 0.5f;
    bd.position[1] = 3.f;
    bd.mass = 1.f; bd.restitution = 0.1f; bd.friction = 0.5f;
    const u64 boxBody = PhysicsBodyCreate(&bd);
    if (groundBody == 0 || boxBody == 0) return fail("PhysicsBodyCreate 失败");
    PhysicsSetRunning(1);

    const u64 target = CreateRenderTarget(width, height, kRenderTargetFormatRgba16F);
    if (target == 0) return fail("CreateRenderTarget 失败");

    for (int i = 0; i < frames; ++i)
    {
        PipelineTick(1.f / 60.f);
        PhysicsBodyTransformPod t{};
        if (PhysicsBodyGetTransform(boxBody, &t) == 1)
        {
            float pos[3] = { t.position[0], t.position[1], t.position[2] };
            float rot[3] = { 0.f, 0.f, 0.f }, scl[3] = { 1.f, 1.f, 1.f };
            SetEntityTransform(boxEntity, pos, rot, scl);
        }
        if (PipelineRenderFrame(target, camera, kInvalidSurfaceId) == 0)
            return fail("PipelineRenderFrame 失败");
    }

    PhysicsBodyTransformPod ft{};
    const int have_final = PhysicsBodyGetTransform(boxBody, &ft);
    const float final_y = have_final ? ft.position[1] : -1000.f;
    const bool physics_passed = have_final && (final_y > 0.44f && final_y < 0.56f);

    const int saved = SaveRenderTargetAsPng(target, outPath);
    if (saved == 0) return fail("SaveRenderTargetAsPng 失败");

    // JSON 先行：结果在 SaveRenderTargetAsPng 后已完全确定
    char data[640];
    std::snprintf(data, sizeof(data),
                  "{\"ok\":true,\"passed\":%s,\"final_y\":%.4f,\"expected_y\":0.5,\"frames\":%d,\"path\":%s}",
                  physics_passed ? "true" : "false", static_cast<double>(final_y), frames,
                  JsonEscape(outPath).c_str());
    PrintOk(data);

    if (!physics_passed)
    {
        PrintError("PHYSICS_FAILED", "动态盒未落在地面（y 偏离 0.5 带内）",
                   "若为预期物理行为变更，更新 --frames 或检查重力/接触参数");
    }

    // 一次性门禁进程：不做逐层拆除（teardown 与 cwd 相关的 shader 状态耦合，
    // 存在引擎侧双重释放噪声），直接带确定退出码退出，资源由 OS 回收。
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(physics_passed ? kExitOk : kExitEngine);
#endif
}

// A9 PR2: 产物读回校验——把写盘后的 blob 按 SceneBlob 布局重新走一遍解析，
// 统计 mesh 总数与带 UVLM（Lightmap UV sidecar）/ CDBK（T4.2 card bake）段的
// mesh 数。这是 "cook → 磁盘 → 引擎读回" 闭环的校验端；读取失败不否定
// cook 本身，但会体现在返回的统计里（mesh_count=0 即异常）。
static bool CountMeshStats(const char* path, u32* out_mesh_count,
                           u32* out_lightmap_uv_meshes, u32* out_card_baked_meshes,
                           u32* out_total_cards = nullptr, u32* out_max_cards = nullptr,
                           f32* out_mean_cards = nullptr)
{
    *out_mesh_count = 0;
    *out_lightmap_uv_meshes = 0;
    *out_card_baked_meshes = 0;
    if (out_total_cards) *out_total_cards = 0;
    if (out_max_cards) *out_max_cards = 0;
    if (out_mean_cards) *out_mean_cards = 0.f;

    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return false; }
    std::vector<u8> bytes(static_cast<size_t>(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return false;

    primal::tools::scene_data sd{};
    sd.buffer = bytes.data();
    sd.buffer_size = static_cast<u32>(bytes.size());
    primal::utl::vector<primal::tools::ErrorReport> errs;
    primal::tools::pipeline::WalkSceneBlob(
        sd,
        [&](u32, const primal::tools::pipeline::PackedMeshView& view) {
            ++(*out_mesh_count);
            if (view.lightmap_uv_count > 0) ++(*out_lightmap_uv_meshes);
            if (view.card_bake_card_count > 0) ++(*out_card_baked_meshes);
            // [SC 卡片 Lumen 对齐 Phase 0]：卡数分布汇总（均值/最差值）。
            if (out_total_cards) *out_total_cards += view.card_bake_card_count;
            if (out_max_cards)
                *out_max_cards = std::max(*out_max_cards, view.card_bake_card_count);
        },
        errs);
    if (out_mean_cards && *out_mesh_count > 0 && out_total_cards)
        *out_mean_cards = (f32)*out_total_cards / (f32)*out_mesh_count;
    return true;
}

// A9 PR2: 资产管线 cook（ProcessAIAsset）。uvatlas 开启时按 ContentToolsCLI
// --pipeline 的默认组合装配 POD；lightmap 再把 uvatlas 切到 Lightmap 预设
// （purpose=Lightmap、gutter=4、strict unique pack），生成的第二套 UV 落入
// blob 的 UVLM sidecar 段而非被丢弃。
static bool CookMeshPipeline(const char* inPath, const char* outPath,
                             bool uvatlas, bool uvatlasLightmap,
                             bool enable_sdf,
                             u32* out_mesh_count, u32* out_lightmap_uv_meshes,
                             u32* out_card_baked_meshes)
{
    ai_asset_pipeline_params p{};
    // 解析侧 legacy settings——与 legacy CookMesh 短格式默认一致
    // （AIAssetPipelineParams.h: "Defaults match the legacy CookMesh
    // short-form (178.0f, 1, 1, 0, 0, 0, 1)"）。
    p.legacy_smoothing_angle = 178.0f;
    p.legacy_calculate_normals = 1;
    p.legacy_calculate_tangents = 1;
    p.legacy_coalesce_meshes = 1;
    // 管线模块开关（镜像 ContentToolsCLI --pipeline 默认值）。
    p.enable_lod = 1;
    p.enable_meshlet = 1;
    p.enable_remesh = 1;
    // T4.2: cook-mesh 产出离线卡片烘焙（occupancy 来自 generate_sdf，
    // 但 SDF 大负载不持久化——输出只追加 tiny "CDBK" 段）。
    // T4.3（2026-09-16）: --sdf 时持久化 per-mesh SDF 段（detail-SDF refine
    // 消费）。generate_sdf 反正为 card bake 运行，此开关只决定是否落盘。
    p.enable_card_bake = 1;
    p.enable_sdf = enable_sdf ? 1 : 0;
    p.lod_ratio = 0.5f;
    p.auto_derive_after_subdivide = 1;
    p.subdivide_levels = 1;
    p.subdivide_scheme = 0;      // Loop
    p.derive_normal_mode = 2;    // AreaWeighted
    p.derive_tangent_mode = 2;   // MikkTSpace
    p.derive_faceted_angle_deg = 60.0f;
    if (uvatlas)
    {
        p.enable_uvatlas = 1;
        if (uvatlasLightmap)
        {
            p.uvatlas_target_purpose = 1;  // UVSetPurpose::Lightmap
            p.uvatlas_gutter = 4.0f;
            p.uvatlas_strict_unique_pack = 1;
        }
    }

    if (CookMeshWithPipeline(inPath, outPath, &p) == 0) return false;
    CountMeshStats(outPath, out_mesh_count, out_lightmap_uv_meshes, out_card_baked_meshes);
    return true;
}

// ============================================================================
// bake-sdf（2026-09-16）: 给已有 .model 的空 "SDF " 段填充 per-mesh SDF，
// 几何与其他段逐字节保持不变——场景分割/实例布局与原资产完全一致，绕开
// "原 cook 配方不可复现"的问题（重烤会改变 submesh 组成与 GI 基线）。
// 用途：为 detail-SDF refine（DDGI/SPGI，PRIMAL_DDGI_DETAIL_SDF 门控）补
// 生产资产的数据。选 N 个体积最大的 submesh 烘焙（运行时槽位只选 4 个）。
// wire 格式契约与 Engine/Content/ContentToEngine.cpp 的
// skip_mesh_in_blob/skip_lightmap_uv_section/skip_card_bake_section 逐字段
// 一致（flags 0x10 UVLM / 0x20 CDBK / 0x40 sdf_signed；card/surfel 32B）。
// ============================================================================
static int VerbBakeSdf(int argc, char** argv)
{
    const char* inPath = nullptr;
    const char* outPath = nullptr;
    u32 resolution = 48;
    u32 topN = 16;
    for (int i = 0; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) inPath = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outPath = argv[++i];
        else if (std::strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) resolution = (u32)std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) topN = (u32)std::atoi(argv[++i]);
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --in/--out/--resolution/--top");
            return kExitUsage;
        }
    }
    if (!inPath || !outPath)
    {
        PrintError("INVALID_PARAMS", "需要 --in <.model> 与 --out <.model>",
                   "示例: primal-engine bake-sdf --in Sponza.model --out SponzaSDF.model --top 16");
    PrintError("NOT_SUPPORTED",
               "bake-sdf 当前仅支持 pipeline（pack_mesh_data）格式的 .model；"
               "legacy pack_geometry 容器的 submesh 头存在双格式/对齐启发式，未复刻", 
               "生成 pipeline 格式资产：primal-engine cook-mesh --in <fbx> --out <model> --sdf");
        return kExitUsage;
    }
    if (resolution < 4) resolution = 4;
    if (resolution > 128) resolution = 128;

    FILE* in = std::fopen(inPath, "rb");
    if (!in) { PrintError("ENGINE_ERROR", "无法打开输入文件", inPath); return kExitEngine; }
    std::fseek(in, 0, SEEK_END);
    long long inSize64 = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<u8> inBuf((size_t)inSize64);
    if (std::fread(inBuf.data(), 1, (size_t)inSize64, in) != (size_t)inSize64)
    { std::fclose(in); PrintError("ENGINE_ERROR", "读取输入失败", inPath); return kExitEngine; }
    std::fclose(in);
    const u64 inSize = (u64)inSize64;

    struct Cursor {
        const u8* p;
        u64 pos = 0;
        u64 size = 0;
        bool bad = false;
        u32 u32_() {
            if (pos + 4 > size) { bad = true; pos = size; return 0; }
            u32 v; std::memcpy(&v, p + pos, 4); pos += 4; return v;
        }
        f32 f32_() {
            if (pos + 4 > size) { bad = true; pos = size; return 0.f; }
            f32 v; std::memcpy(&v, p + pos, 4); pos += 4; return v;
        }
        void skip(u64 n) {
            if (pos + n > size) { bad = true; pos = size; return; }
            pos += n;
        }
    };

    constexpr u32 kSdfMagicW = 0x20464453;   // "SDF "
    constexpr u32 kSdfSignedFlag = 0x40;

    // 与输入 walk 共用的单 submesh 解析（返回各段偏移 + 几何）
    struct Submesh {
        u64 subStart, elemTypeOff, sdfOff, sdfEnd, subEnd;
        u32 elemType;
        std::vector<f32> positions;   // raw geometry
        std::vector<u32> indices;
        f32 rankVolume = 0.0f;
        bool selected = false;
        std::vector<u8> newSdf;       // 选中时的新 SDF 段
    };

    auto walk = [&](bool collect) {
        // .model 旧格式（SceneDataAdapter 旧解析器同构，pack_geometry.py 产物）:
        // [u32 numMaterials] × (5 或 3 个 len 字符串——探测确定)
        // [u32 lodCount][f32 thresholds × lodCount][u32 lod_offsets × lodCount]
        // per LOD: [u32 submeshCount][u32 sizeOfSubmeshes] × pack_mesh_data
        Cursor cur{inBuf.data(), 0, inSize, false};
        std::vector<Submesh> subs;
        auto readStr = [&]() {
            const u32 len = cur.u32_(); cur.skip(len); };
        const u32 matCount = cur.u32_();
        // 字段数探测（与 SceneDataAdapter tryFieldCount 同法）：5 优先，3 兜底
        auto probeFields = [&](u32 fields) -> bool {
            u64 probe = cur.pos;
            for (u32 s = 0; s < matCount * fields; ++s) {
                if (probe + 4 > inSize) return false;
                u32 sLen; std::memcpy(&sLen, inBuf.data() + probe, 4);
                if (sLen > 500) return false;
                probe += 4 + sLen;
                if (probe > inSize) return false;
            }
            if (probe + 4 > inSize) return false;
            u32 maybeLod; std::memcpy(&maybeLod, inBuf.data() + probe, 4);
            return maybeLod > 0 && maybeLod < 100000;
        };
        u32 fieldsPerMaterial = probeFields(5) ? 5u : (probeFields(3) ? 3u : 5u);
        for (u32 m = 0; m < matCount; ++m)
            for (u32 f = 0; f < fieldsPerMaterial; ++f) readStr();
        const u32 lodCount = cur.u32_();
        std::fprintf(stderr, "[walk] matCount=%u fields=%u lodCount=%u\n", matCount, fieldsPerMaterial, lodCount);
        cur.skip(4ull * lodCount);            // thresholds
        // pipeline（lodCount==1 且 mesh_count 合理）无 lod_offsets 数组
        bool pipelineLayout = false;
        {
            u64 probe = cur.pos;
            if (lodCount == 1 && probe + 4 <= inSize) {
                u32 peek; std::memcpy(&peek, inBuf.data() + probe, 4);
                pipelineLayout = (peek > 50 && peek < 500000);
            }
        }
        std::fprintf(stderr, "[walk] pipelineLayout=%d\n", (int)pipelineLayout);
        if (!pipelineLayout)
            cur.skip(4ull * lodCount);        // lod_offsets（旧格式）
        for (u32 lod = 0; lod < lodCount; ++lod)
        {
            u32 idCount = cur.u32_();
            if (!pipelineLayout)
                cur.skip(4);                  // sizeOfSubmeshes（旧格式）
            for (u32 id = 0; id < idCount; ++id)
            {
                Submesh sm;
                sm.subStart = cur.pos;
                const u32 nameLen = cur.u32_(); cur.skip(nameLen);
                (void)cur.u32_(); (void)cur.u32_();
                const u32 elemSize = cur.u32_();
                sm.elemType = cur.u32_();
                sm.elemTypeOff = cur.pos - 4;
                const u32 V = cur.u32_();
                const u32 idxSize = cur.u32_();
                const u32 N = cur.u32_();
                (void)cur.f32_();
                const u64 posOff = cur.pos;
                cur.skip(12ull * V);
                cur.skip((u64)elemSize * V);
                const u64 idxOff = cur.pos;
                cur.skip((u64)idxSize * N);
                // MSHL
                (void)cur.u32_();
                const u32 mc = cur.u32_(); cur.skip(60ull * mc);
                const u32 mvc = cur.u32_(); cur.skip(4ull * mvc);
                const u32 mtc = cur.u32_(); cur.skip(mtc);
                // SDF
                sm.sdfOff = cur.pos;
                (void)cur.u32_();
                (void)cur.u32_(); (void)cur.u32_(); (void)cur.u32_();
                cur.skip(24);
                const u32 dataCount = cur.u32_(); cur.skip(2ull * dataCount);
                const u32 voxelCount = cur.u32_(); cur.skip(voxelCount);
                const u32 vfCount = cur.u32_(); cur.skip(2ull * vfCount);
                sm.sdfEnd = cur.pos;
                if (sm.elemType & 0x10) { (void)cur.u32_(); const u32 uvc = cur.u32_(); cur.skip(8ull * uvc); }
                if (sm.elemType & 0x20) {
                    // v1 "CDBK" 或 v2 "CDB2"（多一个 version u32）。
                    const u32 cdbkMagic = cur.u32_();
                    if (cdbkMagic == 0x32424443) (void)cur.u32_();  // version
                    const u32 cc = cur.u32_(); cur.skip(32ull * cc);
                    (void)cur.u32_(); const u32 sc = cur.u32_(); cur.skip(32ull * sc);
                }
                sm.subEnd = cur.pos;
                if (collect && V > 0 && N > 0)
                {
                    sm.positions.resize((size_t)V * 3);
                    std::memcpy(sm.positions.data(), inBuf.data() + posOff, 12ull * V);
                    sm.indices.resize((size_t)N);
                    if (idxSize == 4)
                        std::memcpy(sm.indices.data(), inBuf.data() + idxOff, 4ull * N);
                    else
                        for (u32 i = 0; i < N; ++i) {
                            u16 v; std::memcpy(&v, inBuf.data() + idxOff + 2ull * i, 2);
                            sm.indices[i] = v;
                        }
                    f32 mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
                    for (u32 i = 0; i < V; ++i)
                        for (u32 a = 0; a < 3; ++a) {
                            f32 v = sm.positions[i*3+a];
                            mn[a] = std::min(mn[a], v); mx[a] = std::max(mx[a], v);
                        }
                    sm.rankVolume = (mx[0]-mn[0]) * (mx[1]-mn[1]) * (mx[2]-mn[2]);
                }
                subs.push_back(std::move(sm));
            }
        }
        return subs;
    };

    std::vector<Submesh> subs = walk(true);

    // 体积降序选前 N，离线生成 SDF
    std::vector<u32> order(subs.size());
    for (u32 i = 0; i < subs.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        return subs[a].rankVolume > subs[b].rankVolume; });
    if (topN > subs.size()) topN = (u32)subs.size();
    u32 baked = 0;
    for (u32 k = 0; k < topN; ++k)
    {
        auto& sm = subs[order[k]];
        if (sm.positions.empty()) continue;
        primal::tools::mesh bridge;
        bridge.vertices.resize(sm.positions.size() / 3);
        for (size_t v = 0; v < bridge.vertices.size(); ++v)
            bridge.vertices[v].position = { sm.positions[v*3+0], sm.positions[v*3+1], sm.positions[v*3+2] };
        bridge.indices.assign(sm.indices.begin(), sm.indices.end());
        // Validate index range before SDF (same guard as AssetPipeline):
        // legacy blobs can carry indices past positions.size() after
        // compaction — closest_point_triangle would read out of bounds.
        {
            u32 w = 0;
            for (u32 r = 0; r + 2 < (u32)bridge.indices.size(); r += 3) {
                if (bridge.indices[r]     >= bridge.vertices.size()) continue;
                if (bridge.indices[r + 1] >= bridge.vertices.size()) continue;
                if (bridge.indices[r + 2] >= bridge.vertices.size()) continue;
                bridge.indices[w++] = bridge.indices[r];
                bridge.indices[w++] = bridge.indices[r + 1];
                bridge.indices[w++] = bridge.indices[r + 2];
            }
            bridge.indices.resize(w);
        }
        primal::tools::generate_sdf(bridge, resolution);
        if (bridge.sdf.data.empty()) continue;
        auto put = [&sm](const void* d, size_t n) {
            sm.newSdf.insert(sm.newSdf.end(), (const u8*)d, (const u8*)d + n); };
        put(&kSdfMagicW, 4);
        put(bridge.sdf.resolution, 12);
        put(bridge.sdf.bounds_min, 12);
        put(bridge.sdf.bounds_max, 12);
        u32 dc = (u32)bridge.sdf.data.size();
        put(&dc, 4); put(bridge.sdf.data.data(), (size_t)dc * 2);
        u32 vc = (u32)bridge.sdf.voxels.size();
        put(&vc, 4); put(bridge.sdf.voxels.data(), vc);
        u32 fc = (u32)bridge.sdf.vector_field.size();
        put(&fc, 4); put(bridge.sdf.vector_field.data(), (size_t)fc * 2);
        sm.selected = true;
        ++baked;
    }

    // ---- 顺序重放输出：容器/头部字节逐字节原样，仅选中 submesh 的
    // elem_type OR 0x40 且 SDF 段替换为新数据 ----
    std::vector<u8> outBuf;
    outBuf.reserve(inSize + (size_t)baked * resolution * resolution * resolution * 11ull / 4ull);
    u64 inPos = 0;
    for (const auto& sm : subs)
    {
        auto copy = [&](u64 a, u64 b) {
            outBuf.insert(outBuf.end(), inBuf.begin() + a, inBuf.begin() + b); };
        copy(inPos, sm.subStart);
        copy(sm.subStart, sm.elemTypeOff);
        const u32 newType = sm.selected ? (sm.elemType | kSdfSignedFlag) : sm.elemType;
        outBuf.insert(outBuf.end(), (u8*)&newType, (u8*)&newType + 4);
        copy(sm.elemTypeOff + 4, sm.sdfOff);
        if (sm.selected)
            outBuf.insert(outBuf.end(), sm.newSdf.begin(), sm.newSdf.end());
        else
            copy(sm.sdfOff, sm.sdfEnd);
        copy(sm.sdfEnd, sm.subEnd);
        inPos = sm.subEnd;
    }
    if (inPos < inSize)
        outBuf.insert(outBuf.end(), inBuf.begin() + inPos, inBuf.end());

    FILE* out = std::fopen(outPath, "wb");
    if (!out) { PrintError("ENGINE_ERROR", "无法打开输出文件", outPath); return kExitEngine; }
    size_t written = std::fwrite(outBuf.data(), 1, outBuf.size(), out);
    std::fclose(out);
    if (written != outBuf.size())
    { PrintError("ENGINE_ERROR", "写出不完整", outPath); return kExitEngine; }

    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "{\"submeshes\":%u,\"baked\":%u,\"resolution\":%u,\"bytes_in\":%lld,\"bytes_out\":%llu}",
                  (u32)subs.size(), baked, resolution, inSize64, (unsigned long long)outBuf.size());
    PrintOk(msg);
    return kExitOk;
}

// ============================================================================
// bake-cards（2026-09-19 SC 卡片 Lumen 对齐 Phase 2）：给已有 .model 补 per-
// mesh 离线卡片烘焙（v2 coverage 驱动贴面卡），几何与其他段逐字节不变——
// 与 bake-sdf 同一"容器重放"策略，绕开"重烤改变 submesh 组成与 GI 基线"
// 的问题。legacy（pack_geometry）与 pipeline（pack_mesh_data）容器都支持
// （探测同 SceneDataAdapter）。产出 v2 "CDB2" 段；已带卡段的 submesh 原段
// 被替换。默认不写 surfel（运行时无消费者，控制 blob 体积；--surfels 1 打开）。
// PRIMAL_SC_BAKE_V2=0 或 --v1 走 v1 算法（回滚路径）。
// ============================================================================
int VerbBakeCards(int argc, char** argv)
{
    const char* inPath = nullptr;
    const char* outPath = nullptr;
    u32 resolution = 32;
    bool writeSurfels = false;
    bool useV1 = false;
    for (int i = 0; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) inPath = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outPath = argv[++i];
        else if (std::strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) resolution = (u32)std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--surfels") == 0 && i + 1 < argc) writeSurfels = std::atoi(argv[++i]) != 0;
        else if (std::strcmp(argv[i], "--v1") == 0) useV1 = true;
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --in/--out/--resolution/--surfels/--v1");
            return kExitUsage;
        }
    }
    if (!inPath || !outPath)
    {
        PrintError("INVALID_PARAMS", "需要 --in <.model> 与 --out <.model>",
                   "示例: primal-engine bake-cards --in Sponza.model --out Sponza.model");
        return kExitUsage;
    }
    if (resolution < 4) resolution = 4;
    if (resolution > 128) resolution = 128;
    const char* v2Env = std::getenv("PRIMAL_SC_BAKE_V2");
    if (v2Env && v2Env[0] == '0') useV1 = true;

    FILE* in = std::fopen(inPath, "rb");
    if (!in) { PrintError("ENGINE_ERROR", "无法打开输入文件", inPath); return kExitEngine; }
    std::fseek(in, 0, SEEK_END);
    long long inSize64 = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<u8> inBuf((size_t)inSize64);
    if (std::fread(inBuf.data(), 1, (size_t)inSize64, in) != (size_t)inSize64)
    { std::fclose(in); PrintError("ENGINE_ERROR", "读取输入失败", inPath); return kExitEngine; }
    std::fclose(in);
    const u64 inSize = (u64)inSize64;

    struct Cursor {
        const u8* p;
        u64 pos = 0;
        u64 size = 0;
        bool bad = false;
        u32 u32_() {
            if (pos + 4 > size) { bad = true; pos = size; return 0; }
            u32 v; std::memcpy(&v, p + pos, 4); pos += 4; return v;
        }
        f32 f32_() {
            if (pos + 4 > size) { bad = true; pos = size; return 0.f; }
            f32 v; std::memcpy(&v, p + pos, 4); pos += 4; return v;
        }
        void skip(u64 n) {
            if (pos + n > size) { bad = true; pos = size; return; }
            pos += n;
        }
    };

    // 单 submesh 解析（返回各段偏移 + 几何）。容器探测与 bake-sdf /
    // SceneDataAdapter 同构：5/3 字段材质探测 + pipeline/legacy 布局探测。
    struct Submesh {
        u64 subStart, elemTypeOff, sdfEnd, uvlmEnd, cdbkOff, cdbkEnd, subEnd;
        u32 elemType;
        std::vector<f32> positions;
        std::vector<u32> indices;
    };

    bool walkBad = false;
    u32 total = 0;
    auto walk = [&](bool collect) {
        // 容器/头部解析同 SceneDataAdapter 的权威双路径：pipeline
        //（pack_mesh_data：带名字头部、无填充）与 legacy（pack_geometry：
        // 无名字 6 字段头、pos/elem 16 字节对齐、peek 门控的 MSHL/SDF/
        // UVLM/CDBK 段）。每一步都有边界检查——越界即截断 walk（bake-sdf
        // 的无守卫版本在 legacy 资产上会冲出缓冲 SIGSEGV）。
        Cursor cur{inBuf.data(), 0, inSize, false};
        auto peek_magic = [&](u64 at) -> u32 {
            u32 v = 0;
            if (at + 4 <= inSize) std::memcpy(&v, inBuf.data() + at, 4);
            return v;
        };
        std::vector<Submesh> subs;
        auto readStr = [&]() {
            const u32 len = cur.u32_(); cur.skip(len); };
        // Pipeline scene blobs begin with a printable scene name, then
        // materials and named LOD groups. A partial walk must never be
        // published as a successful card rebake.
        bool pipelineScene = false;
        if (inSize >= 12) {
            const u32 first = peek_magic(0);
            if (first <= 200u && 4ull + first + 4ull <= inSize) {
                pipelineScene = true;
                for (u32 i = 0; i < first; ++i) {
                    const u8 ch = inBuf[4u + i];
                    if (ch < 32u || ch > 126u) { pipelineScene = false; break; }
                }
                if (pipelineScene) {
                    const u32 materials = peek_magic(4u + first);
                    pipelineScene = materials > 0u && materials < 10000u;
                }
            }
        }
        if (pipelineScene) {
            const u32 sceneNameLength = cur.u32_();
            cur.skip(sceneNameLength);
        }
        const u32 matCount = cur.u32_();
        auto probeFields = [&](u32 fields) -> bool {
            u64 probe = cur.pos;
            for (u32 s = 0; s < matCount * fields; ++s) {
                if (probe + 4 > inSize) return false;
                u32 sLen; std::memcpy(&sLen, inBuf.data() + probe, 4);
                if (sLen > 500) return false;
                probe += 4 + sLen;
                if (probe > inSize) return false;
            }
            if (probe + 4 > inSize) return false;
            u32 maybeLod; std::memcpy(&maybeLod, inBuf.data() + probe, 4);
            return maybeLod > 0 && maybeLod < 100000;
        };
        u32 fieldsPerMaterial = pipelineScene ? 3u :
            (probeFields(5) ? 5u : (probeFields(3) ? 3u : 5u));
        for (u32 m = 0; m < matCount; ++m)
            for (u32 f = 0; f < fieldsPerMaterial; ++f) readStr();
        const u32 lodCount = cur.u32_();
        if (!pipelineScene) cur.skip(4ull * lodCount); // legacy thresholds
        bool pipelineLayout = pipelineScene;
        if (!pipelineScene) {
            u64 probe = cur.pos;
            if (lodCount == 1 && probe + 4 <= inSize) {
                u32 peek; std::memcpy(&peek, inBuf.data() + probe, 4);
                pipelineLayout = (peek > 50 && peek < 500000);
            }
        }
        if (!pipelineLayout)
            cur.skip(4ull * lodCount);        // lod_offsets（旧格式）
        for (u32 lod = 0; lod < lodCount && !cur.bad; ++lod)
        {
            if (pipelineScene) {
                const u32 lodNameLength = cur.u32_();
                if (lodNameLength > 10000u) { walkBad = true; break; }
                cur.skip(lodNameLength);
            }
            if (cur.pos + 8 > inSize) { std::fprintf(stderr, "[walk] lod %u: pos+8>inSize\n", lod); break; }
            u32 idCount = cur.u32_();
            if (!pipelineLayout)
                cur.skip(4);                  // sizeOfSubmeshes（旧格式）
            if (cur.bad || idCount > 1000000u) { std::fprintf(stderr, "[walk] lod %u: bad=%d idCount=%u\n", lod, (int)cur.bad, idCount); break; }
            for (u32 id = 0; id < idCount && !cur.bad; ++id)
            {
                if (cur.pos + 28 > inSize) { std::fprintf(stderr, "[walk] lod %u mesh %u: pos+28>inSize\n", lod, id); break; }
                Submesh sm{};
                sm.subStart = cur.pos;
                // ---- 头部双格式探测（同 SceneDataAdapter）----
                const u64 nameOff = cur.pos;
                const u32 firstVal = cur.u32_();
                bool compact = false;
                u32 elemSize = 0, elemType = 0, V = 0, N = 0, idxSize = 2;
                // 双格式探测逐字段镜像 SceneDataAdapter 的权威实现——包括
                // compact 尝试失败后**不回退游标**（legacy 分支从 compact
                // 停下的位置继续读）。回退会让偏移与引擎解读分叉。
                if (firstVal > 0 && firstVal < 200 && cur.pos + firstVal + 32 <= inSize) {
                    bool printable = true;
                    for (u32 c = 0; c < firstVal && c < 50; ++c) {
                        const u8 ch = inBuf[cur.pos + c];
                        if (ch != 0 && (ch < 32 || ch > 126)) { printable = false; break; }
                    }
                    if (printable) {
                        cur.skip(firstVal);
                        (void)cur.u32_();                   // lodId
                        (void)cur.u32_();                   // materialIdx
                        elemSize = cur.u32_();
                        elemType = cur.u32_(); sm.elemTypeOff = cur.pos - 4;
                        V = cur.u32_();
                        const u32 idxSzField = cur.u32_();
                        N = cur.u32_();
                        cur.skip(4);                        // lod threshold
                        if ((idxSzField == 2 || idxSzField == 4) &&
                            V > 0 && V < 10000000u && N > 0 && N < 30000000u) {
                            compact = true;
                            idxSize = idxSzField;
                        }
                        // 验证失败不回退——游标停在 compact 尝试终点。
                    }
                }
                if (!compact) {
                    // legacy：[MatIdx][ElemSize][V][N][ElemType][PrimTopo] 或
                    // 旧 MeshCPU：[ElemSize][V][N][ElemType][PrimTopo]（无材质）。
                    const u32 second = cur.u32_();
                    if (second > 200) {
                        elemSize = firstVal;                // 旧格式
                        V = second;
                    } else {
                        elemSize = second;                  // pack_geometry
                        V = cur.u32_();
                    }
                    N = cur.u32_();
                    elemType = cur.u32_(); sm.elemTypeOff = cur.pos - 4;
                    cur.skip(4);                            // primitiveTopology
                    idxSize = (V < (1u << 16)) ? 2u : 4u;
                }
                // ---- 缓冲（legacy: pos/elem 16 字节对齐；pipeline: 紧排）----
                const u64 posOff = cur.pos;
                {
                    const u64 endP = cur.pos + 12ull * V;
                    const u64 aligned = pipelineLayout ? endP : ((endP + 15) & ~15ull);
                    cur.skip(aligned - cur.pos);
                }
                {
                    const u64 endE = cur.pos + (u64)elemSize * V;
                    const u64 aligned = pipelineLayout ? endE : ((endE + 15) & ~15ull);
                    cur.skip(aligned - cur.pos);
                }
                const u64 idxOff = cur.pos;
                cur.skip((u64)idxSize * N);
                // ---- MSHL（peek 门控）----
                if (peek_magic(cur.pos) == 0x4C48534D) {
                    cur.skip(4);
                    const u32 mc = cur.u32_();
                    if (mc > 0 && mc < 100000u) {
                        cur.skip(60ull * mc);
                        if (cur.pos + 4 <= inSize) {
                            const u32 mvc = cur.u32_();
                            if (mvc > 0 && mvc < 10000000u) cur.skip(4ull * mvc);
                        }
                        if (cur.pos + 4 <= inSize) {
                            const u32 mtc = cur.u32_();
                            if (mtc > 0 && mtc < 10000000u) cur.skip(mtc);
                        }
                    }
                }
                // ---- SDF（peek 门控）----
                if (peek_magic(cur.pos) == 0x20464453) {
                    cur.skip(4);
                    cur.skip(12 + 24);                      // res + bounds
                    const u32 dc = cur.u32_(); cur.skip(2ull * dc);
                    const u32 vc = cur.u32_(); cur.skip(vc);
                    const u32 fc = cur.u32_(); cur.skip(2ull * fc);
                }
                sm.sdfEnd = cur.pos;
                // ---- UVLM（flag + magic 双门控）----
                sm.uvlmEnd = cur.pos;
                if ((elemType & 0x10) && peek_magic(cur.pos) == 0x4D4C5655) {
                    cur.skip(4);
                    const u32 uvc = cur.u32_(); cur.skip(8ull * uvc);
                    sm.uvlmEnd = cur.pos;
                }
                // ---- 卡片烘焙段（flag + magic 双门控；v1/v2）----
                sm.cdbkOff = cur.pos;
                sm.cdbkEnd = cur.pos;
                if ((elemType & 0x20) && cur.pos + 8 <= inSize) {
                    const u32 magic = peek_magic(cur.pos);
                    if (magic == 0x4B424443 || magic == 0x32424443) {
                        cur.skip(4);
                        if (magic == 0x32424443) cur.skip(4);   // version
                        const u32 cc = cur.u32_(); cur.skip(32ull * cc);
                        const u32 sc = cur.u32_(); cur.skip(32ull * sc);
                        sm.cdbkEnd = cur.pos;
                    }
                }
                sm.subEnd = cur.pos;
                sm.elemType = elemType;
                if (collect && V > 0 && N > 0 &&
                    posOff + 12ull * V <= inSize && idxOff + (u64)idxSize * N <= inSize)
                {
                    sm.positions.resize((size_t)V * 3);
                    std::memcpy(sm.positions.data(), inBuf.data() + posOff, 12ull * V);
                    sm.indices.resize((size_t)N);
                    if (idxSize == 4)
                        std::memcpy(sm.indices.data(), inBuf.data() + idxOff, 4ull * N);
                    else
                        for (u32 i = 0; i < N; ++i) {
                            u16 v; std::memcpy(&v, inBuf.data() + idxOff + 2ull * i, 2);
                            sm.indices[i] = v;
                        }
                }
                if (total < 6)
                            ++total;
                subs.push_back(std::move(sm));
            }
        }
        if (cur.bad || (pipelineScene && cur.pos != inSize)) walkBad = true;
        return subs;
    };
    std::vector<Submesh> subs = walk(true);
    std::fprintf(stderr, "[walk] subs=%zu walkBad=%d\n", subs.size(), (int)walkBad);
    if (subs.empty() || walkBad) {
        std::fprintf(stderr, "[bake-cards] container walk failed\n");
        PrintError("ENGINE_ERROR", "容器解析失败（非 pack_geometry/pack_mesh_data 格式？）",
                   "bake-cards 支持 legacy 与 pipeline 两种 .model 容器");
        return kExitEngine;
    }

    // 全部 submesh 离线烘卡（v2 贴面 coverage 贪心，默认）。
    u32 baked = 0, emptyShell = 0, replaced = 0;
    u32 totalCards = 0;
    f32 coverageSum = 0.f, coverageMin = 1.f;
    std::vector<std::vector<u8>> newSections(subs.size());
    for (size_t k = 0; k < subs.size(); ++k)
    {
        auto& sm = subs[k];
        if (sm.positions.empty()) continue;
        if (sm.cdbkEnd > sm.cdbkOff) ++replaced;
        primal::tools::mesh bridge;
        bridge.vertices.resize(sm.positions.size() / 3);
        for (size_t v = 0; v < bridge.vertices.size(); ++v)
            bridge.vertices[v].position = { sm.positions[v*3+0], sm.positions[v*3+1], sm.positions[v*3+2] };
        bridge.indices.assign(sm.indices.begin(), sm.indices.end());
        // Validate index range before SDF (same guard as AssetPipeline):
        // legacy blobs can carry indices past positions.size() after
        // compaction — closest_point_triangle would read out of bounds.
        {
            u32 w = 0;
            for (u32 r = 0; r + 2 < (u32)bridge.indices.size(); r += 3) {
                if (bridge.indices[r]     >= bridge.vertices.size()) continue;
                if (bridge.indices[r + 1] >= bridge.vertices.size()) continue;
                if (bridge.indices[r + 2] >= bridge.vertices.size()) continue;
                bridge.indices[w++] = bridge.indices[r];
                bridge.indices[w++] = bridge.indices[r + 1];
                bridge.indices[w++] = bridge.indices[r + 2];
            }
            bridge.indices.resize(w);
        }
        primal::tools::generate_sdf(bridge, resolution);
        if (bridge.sdf.voxels.empty() ||
            bridge.sdf.voxels.size() != (size_t)bridge.sdf.resolution[0] *
                                       bridge.sdf.resolution[1] * bridge.sdf.resolution[2])
        { ++emptyShell; continue; }

        primal::tools::card_bake::OccupancyGrid grid;
        grid.voxels = bridge.sdf.voxels.data();
        grid.resolution = bridge.sdf.resolution[0];
        grid.grid_min.x = bridge.sdf.bounds_min[0];
        grid.grid_min.y = bridge.sdf.bounds_min[1];
        grid.grid_min.z = bridge.sdf.bounds_min[2];
        grid.voxel_size.x = (bridge.sdf.bounds_max[0] - bridge.sdf.bounds_min[0]) / (f32)grid.resolution;
        grid.voxel_size.y = (bridge.sdf.bounds_max[1] - bridge.sdf.bounds_min[1]) / (f32)grid.resolution;
        grid.voxel_size.z = (bridge.sdf.bounds_max[2] - bridge.sdf.bounds_min[2]) / (f32)grid.resolution;
        primal::tools::card_bake::Params params{};
        params.generate_surfels = writeSurfels;
        primal::tools::card_bake::data bakeOut;
        if (useV1)
            primal::tools::card_bake::bake_v1(grid, params, bakeOut);
        else
            primal::tools::card_bake::bake_v2(grid, params, bakeOut);
        if (bakeOut.cards.empty()) { ++emptyShell; continue; }

        auto& sec = newSections[k];
        auto put = [&sec](const void* d, size_t n) {
            sec.insert(sec.end(), (const u8*)d, (const u8*)d + n); };
        const u32 magic = primal::tools::card_bake::kCardBakeMagicV2;
        const u32 version = primal::tools::card_bake::kCardBakeVersionV2;
        put(&magic, 4);
        put(&version, 4);
        const u32 cc = (u32)bakeOut.cards.size();
        put(&cc, 4);
        put(bakeOut.cards.data(), (size_t)cc * 32);
        const u32 sc = (u32)bakeOut.surfels.size();
        put(&sc, 4);
        if (sc) put(bakeOut.surfels.data(), (size_t)sc * 32);

        ++baked;
        totalCards += cc;
        coverageSum += bakeOut.coverage;
        coverageMin = std::min(coverageMin, bakeOut.coverage);
    }

    // ---- 顺序重放输出：头部字节逐字节原样（仅 0x20 旗标置位），尾段替换
    // / 追加新 "CDB2" 段 ----
    std::vector<u8> outBuf;
    outBuf.reserve(inSize + (size_t)totalCards * 32ull + 4096ull);
    u64 inPos = 0;
    for (size_t k = 0; k < subs.size(); ++k)
    {
        const auto& sm = subs[k];
        auto copy = [&](u64 a, u64 b) {
            outBuf.insert(outBuf.end(), inBuf.begin() + a, inBuf.begin() + b); };
        copy(inPos, sm.subStart);
        copy(sm.subStart, sm.elemTypeOff);
        const u32 newType = newSections[k].empty() ? sm.elemType : (sm.elemType | 0x20);
        outBuf.insert(outBuf.end(), (u8*)&newType, (u8*)&newType + 4);
        // elemTypeOff+4 → CDBK 起点（SDF + UVLM 原样）。
        copy(sm.elemTypeOff + 4, sm.cdbkOff);
        if (!newSections[k].empty())
            outBuf.insert(outBuf.end(), newSections[k].begin(), newSections[k].end());
        copy(sm.cdbkEnd, sm.subEnd);
        inPos = sm.subEnd;
    }
    if (inPos < inSize)
        outBuf.insert(outBuf.end(), inBuf.begin() + inPos, inBuf.end());

    FILE* out = std::fopen(outPath, "wb");
    if (!out) { PrintError("ENGINE_ERROR", "无法打开输出文件", outPath); return kExitEngine; }
    size_t written = std::fwrite(outBuf.data(), 1, outBuf.size(), out);
    std::fclose(out);
    if (written != outBuf.size())
    { PrintError("ENGINE_ERROR", "写出不完整", outPath); return kExitEngine; }

    const f32 meanCov = baked ? coverageSum / (f32)baked : 0.f;
    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "{\"submeshes\":%u,\"baked\":%u,\"replaced\":%u,\"empty_shell\":%u,"
                  "\"cards_total\":%u,\"coverage_mean\":%.3f,\"coverage_min\":%.3f,"
                  "\"resolution\":%u,\"v1\":%s,\"bytes_in\":%lld,\"bytes_out\":%llu}",
                  (u32)subs.size(), baked, replaced, emptyShell, totalCards,
                  (double)meanCov, baked ? (double)coverageMin : 0.0,
                  resolution, useV1 ? "true" : "false",
                  inSize64, (unsigned long long)outBuf.size());
    PrintOk(msg);
    return kExitOk;
}

// ============================================================================
// sc-bake-surface（2026-09-30，SC per-mesh 计划 P1）：为已有 .model 的 CDB2
// 卡片布局烘焙 per-card 真实表面页（mask/depth/normal/albedo/材质参数），
// 写 "SCS1" sidecar（不进 blob）。owner 契约：只提交 owner mesh 的几何与
// 材质。几何/布局/材质三摘要键控——资产或卡片重排变化即失效。
// 纹理解析复刻运行时约定：stbi 加载（sRGB 源）、双线性、repeat wrap、
// 顶点级 uv.y=1-uv.y 翻转；纹理变体先改 RGBA8 源字节，再计算材质摘要。
// ============================================================================
static int VerbScBakeSurface(int argc, char** argv)
{
    namespace cs = primal::tools::card_surface;
    const char* inPath = nullptr;
    const char* outPath = nullptr;
    const char* textureDir = nullptr;
    const char* boundaryOut = nullptr;
    std::string boundaryTexture;
    f32 boundarySpacing = .1f;
    bool boundarySpacingSet = false;
    f32 albedoScale = 1.0f;
    f32 texelsPerUnit = 64.0f;
    f32 tintR = 1.f, tintG = 1.f, tintB = 1.f;
    // Repeatable substring:scale pairs add records without replacing the base.
    std::vector<std::pair<std::string, f32>> textureVariants;
    u32 verbosity = 0;
    u32 maxMeshes = 0xFFFFFFFFu;
    for (int i = 0; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) inPath = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outPath = argv[++i];
        else if (std::strcmp(argv[i], "--texture-dir") == 0 && i + 1 < argc) textureDir = argv[++i];
        else if (std::strcmp(argv[i], "--boundary-out") == 0 && i + 1 < argc) boundaryOut = argv[++i];
        else if (std::strcmp(argv[i], "--boundary-texture") == 0 && i + 1 < argc) boundaryTexture = argv[++i];
        else if (std::strcmp(argv[i], "--boundary-spacing") == 0 && i + 1 < argc) {
            boundarySpacingSet=true;
            char* end=nullptr; const char* value=argv[++i]; boundarySpacing=std::strtof(value,&end);
            if (end==value || *end || !primal::graphics::rhi::card_boundary::Finite(boundarySpacing) || boundarySpacing<=0) {
                PrintError("INVALID_PARAMS","补点间距需为正有限 mesh 局部单位","--boundary-spacing 0.1"); return kExitUsage;
            }
        }
        else if (std::strcmp(argv[i], "--texels-per-unit") == 0 && i + 1 < argc) texelsPerUnit = (f32)std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--albedo-scale") == 0 && i + 1 < argc) albedoScale = (f32)std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--tint") == 0 && i + 1 < argc)
            std::sscanf(argv[++i], "%f,%f,%f", &tintR, &tintG, &tintB);
        else if (std::strcmp(argv[i], "--texture-variant") == 0 && i + 1 < argc) {
            std::string spec = argv[++i];
            const auto colon = spec.find_last_of(':');
            char* end = nullptr;
            const float scale = colon == std::string::npos ? -1.f
                : std::strtof(spec.c_str() + colon + 1, &end);
            if (colon == std::string::npos || colon == 0 || !end || *end ||
                end == spec.c_str() + colon + 1 || !std::isfinite(scale) || scale < 0.f) {
                PrintError("INVALID_PARAMS", "变体需为非空纹理路径子串:非负有限线性比例", "--texture-variant sponza_floor_diffuse:0.5");
                return kExitUsage;
            }
            std::string name = spec.substr(0, colon);
            for (auto& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            textureVariants.emplace_back(std::move(name), scale);
        }
        else if (std::strcmp(argv[i], "--verbose") == 0) verbosity = 1;
        else if (std::strcmp(argv[i], "--max-meshes") == 0 && i + 1 < argc) maxMeshes = (u32)std::atoi(argv[++i]);
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --in/--out/--texture-dir/--texture-variant/--boundary-out/--boundary-texture/--boundary-spacing/--texels-per-unit/--albedo-scale/--tint/--verbose/--max-meshes");
            return kExitUsage;
        }
    }
    if (!inPath || !outPath)
    {
        PrintError("INVALID_PARAMS", "需要 --in <.model> 与 --out <.scsurf>",
                   "示例: primal-engine sc-bake-surface --in Sponza.model --out Sponza.model.scsurf "
                   "--texture-dir EngineTest/assets");
        return kExitUsage;
    }

    if ((boundaryOut && (!*boundaryOut || boundaryTexture.empty())) || (!boundaryOut && (!boundaryTexture.empty() || boundarySpacingSet))) {
        PrintError("INVALID_PARAMS","补点输出与非空纹理子串必须一起指定","--boundary-out <.scboundary> --boundary-texture sponza_floor_diffuse"); return kExitUsage;
    }
    if (boundaryOut) {
        std::error_code ec;
        if (std::filesystem::absolute(boundaryOut).lexically_normal()==std::filesystem::absolute(outPath).lexically_normal() ||
            std::filesystem::equivalent(boundaryOut,outPath,ec) ||
            std::filesystem::absolute(boundaryOut).lexically_normal()==std::filesystem::absolute(inPath).lexically_normal() ||
            std::filesystem::equivalent(boundaryOut,inPath,ec)) {
            PrintError("INVALID_PARAMS","SCB1 路径必须与输入模型及 SCS1 输出不同","--boundary-out <独立 .scboundary>"); return kExitUsage;
        }
        for (auto& c:boundaryTexture) c=char(std::tolower(static_cast<unsigned char>(c)));
    }

    if (!std::isfinite(texelsPerUnit) || texelsPerUnit < 16.f || texelsPerUnit > 256.f ||
        !std::isfinite(albedoScale) || albedoScale < 0.f ||
        !std::isfinite(tintR) || !std::isfinite(tintG) || !std::isfinite(tintB)) {
        PrintError("INVALID_PARAMS", "密度需在 [16,256]，比例与 tint 需为有限值", "sc-bake-surface");
        return kExitUsage;
    }
    FILE* in = std::fopen(inPath, "rb");
    if (!in) { PrintError("ENGINE_ERROR", "无法打开输入文件", inPath); return kExitEngine; }
    std::fseek(in, 0, SEEK_END);
    long long inSize64 = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<u8> inBuf((size_t)inSize64);
    if (std::fread(inBuf.data(), 1, (size_t)inSize64, in) != (size_t)inSize64)
    { std::fclose(in); PrintError("ENGINE_ERROR", "读取输入失败", inPath); return kExitEngine; }
    std::fclose(in);
    const u64 inSize = (u64)inSize64;

    // 纹理搜索路径：--texture-dir、模型同目录、models/Sponza/、textures/
    //（与 TestVulkanSponzaRenderGraph::ResolveTexturePath 同序）。
    std::vector<std::string> searchDirs;
    if (textureDir && *textureDir) searchDirs.push_back(textureDir);
    {
        std::string modelDir(inPath);
        const size_t slash = modelDir.find_last_of('/');
        searchDirs.push_back(slash == std::string::npos ? std::string(".") : modelDir.substr(0, slash));
        const size_t assets = modelDir.find("/assets/");
        if (assets != std::string::npos)
        {
            const std::string base = modelDir.substr(0, assets + strlen("/assets/"));
            searchDirs.push_back(base + "models/Sponza");
            searchDirs.push_back(base + "textures");
        }
    }
    auto resolve_texture = [&](const std::string& name) -> std::string {
        if (name.empty()) return {};
        for (const std::string& dir : searchDirs)
        {
            const std::string p = dir + "/" + name;
            FILE* t = std::fopen(p.c_str(), "rb");
            if (t) { std::fclose(t); return p; }
        }
        return {};
    };

    struct Cursor {
        const u8* p;
        u64 pos = 0;
        u64 size = 0;
        bool bad = false;
        u32 u32_() {
            if (pos + 4 > size) { bad = true; pos = size; return 0; }
            u32 v; std::memcpy(&v, p + pos, 4); pos += 4; return v;
        }
        void skip(u64 n) {
            if (pos + n > size) { bad = true; pos = size; return; }
            pos += n;
        }
    };

    // 与 VerbBakeCards 同一容器探测/头部解析；额外收集 element 缓冲、
    // CDB2 卡片与材质 diffuse 引用。
    struct Submesh {
        u32 elemType = 0;
        u32 elemSize = 0;
        u32 materialIdx = 0xFFFFFFFFu;
        std::string name;
        std::vector<f32> positions;
        std::vector<u8> elements;
        std::vector<u32> indices;   // 统一展宽为 u32（视图端 index_size=4）
        // [P0 身份契约] blob 原生索引字节（digest 用，与运行时一致）。
        std::vector<u8> rawIndices;
        u32 cardCount = 0;
        std::vector<u8> cardBytes;  // CDB2 cards[] 原始 32B/张
    };

    bool walkBad = false;
    u32 meshIndex = 0;
    std::vector<std::string> materialDiffuse;
    std::vector<Submesh> subs;
    auto walk = [&]() {
        Cursor cur{inBuf.data(), 0, inSize, false};
        auto peek_magic = [&](u64 at) -> u32 {
            u32 v = 0;
            if (at + 4 <= inSize) std::memcpy(&v, inBuf.data() + at, 4);
            return v;
        };
        auto readStr = [&](std::string* out) {
            const u32 len = cur.u32_();
            if (out && !cur.bad && len < 4096 && cur.pos + len <= inSize)
                out->assign((const char*)inBuf.data() + cur.pos, len);
            cur.skip(len);
        };
        // pipeline 容器 = 可打印场景名开头。
        bool pipelineScene = false;
        if (inSize >= 12) {
            const u32 first = peek_magic(0);
            if (first <= 200u && 4ull + first + 4ull <= inSize) {
                pipelineScene = true;
                for (u32 i = 0; i < first; ++i) {
                    const u8 ch = inBuf[4u + i];
                    if (ch < 32u || ch > 126u) { pipelineScene = false; break; }
                }
                if (pipelineScene) {
                    const u32 materials = peek_magic(4u + first);
                    pipelineScene = materials > 0u && materials < 10000u;
                }
            }
        }
        std::string sceneName;
        if (pipelineScene) readStr(&sceneName);
        const u32 matCount = cur.u32_();
        if (pipelineScene) {
            // pipeline 材质 = name/diffuse/normal 三字段。
            for (u32 m = 0; m < matCount && !cur.bad; ++m) {
                std::string mn, diff, nrm;
                readStr(&mn); readStr(&diff); readStr(&nrm);
                materialDiffuse.push_back(std::move(diff));
            }
        } else {
            auto probeFields = [&](u32 fields) -> bool {
                u64 probe = cur.pos;
                for (u32 s = 0; s < matCount * fields; ++s) {
                    if (probe + 4 > inSize) return false;
                    u32 sLen; std::memcpy(&sLen, inBuf.data() + probe, 4);
                    if (sLen > 500) return false;
                    probe += 4 + sLen;
                    if (probe > inSize) return false;
                }
                if (probe + 4 > inSize) return false;
                u32 maybeLod; std::memcpy(&maybeLod, inBuf.data() + probe, 4);
                return maybeLod > 0 && maybeLod < 100000;
            };
            // 材质字段数探测：Geometry::material 的文档布局是 name/diffuse/
            // normal 三字段，3 探测优先；5 是历史变体兜底。（VerbBakeCards 先
            // 5 后 3 是因为它不消费字段内容——这里 diffuse 必须取对。）
            const u32 fieldsPerMaterial = probeFields(3) ? 3u : (probeFields(5) ? 5u : 3u);
            for (u32 m = 0; m < matCount && !cur.bad; ++m)
                for (u32 f = 0; f < fieldsPerMaterial; ++f)
                    readStr(f == 1 ? &materialDiffuse.emplace_back() : nullptr);
            if ((u32)materialDiffuse.size() > matCount) materialDiffuse.resize(matCount);
        }
        const u32 lodCount = cur.u32_();
        if (!pipelineScene) cur.skip(4ull * lodCount);
        const bool pipelineLayout = pipelineScene;
        if (!pipelineLayout) cur.skip(4ull * lodCount);
        for (u32 lod = 0; lod < lodCount && !cur.bad; ++lod)
        {
            if (pipelineScene) { const u32 ll = cur.u32_(); cur.skip(ll); }
            if (cur.pos + 8 > inSize) break;
            const u32 idCount = cur.u32_();
            if (!pipelineLayout) cur.skip(4);
            if (cur.bad || idCount > 1000000u) break;
            for (u32 id = 0; id < idCount && !cur.bad; ++id)
            {
                if (cur.pos + 28 > inSize) break;
                Submesh sm{};
                const u32 firstVal = cur.u32_();
                bool compact = false;
                u32 elemSize = 0, elemType = 0, V = 0, N = 0, idxSize = 2;
                if (firstVal > 0 && firstVal < 200 && cur.pos + firstVal + 32 <= inSize) {
                    bool printable = true;
                    for (u32 c = 0; c < firstVal && c < 50; ++c) {
                        const u8 ch = inBuf[cur.pos + c];
                        if (ch != 0 && (ch < 32 || ch > 126)) { printable = false; break; }
                    }
                    if (printable) {
                        sm.name.assign((const char*)inBuf.data() + cur.pos, firstVal);
                        cur.skip(firstVal);
                        (void)cur.u32_();   // lodId
                        sm.materialIdx = cur.u32_();
                        elemSize = cur.u32_();
                        elemType = cur.u32_();
                        V = cur.u32_();
                        const u32 idxSzField = cur.u32_();
                        N = cur.u32_();
                        cur.skip(4);
                        if ((idxSzField == 2 || idxSzField == 4) &&
                            V > 0 && V < 10000000u && N > 0 && N < 30000000u) {
                            compact = true;
                            idxSize = idxSzField;
                        }
                    }
                }
                if (!compact) {
                    const u32 second = cur.u32_();
                    if (second > 200) {
                        // Old Format：[ElemSize][V][N][ElemType][PrimTopo]（无材质）。
                        elemSize = firstVal; V = second;
                    } else {
                        // pack_geometry Engine Format：[MatIdx][ElemSize][V][N]…——
                        // firstVal = MatIdx（与 SceneDataAdapter 权威实现一致）。
                        sm.materialIdx = firstVal;
                        elemSize = second; V = cur.u32_();
                    }
                    N = cur.u32_();
                    elemType = cur.u32_();
                    cur.skip(4);
                    idxSize = (V < (1u << 16)) ? 2u : 4u;
                }
                const u64 posOff = cur.pos;
                {
                    const u64 endP = cur.pos + 12ull * V;
                    const u64 aligned = pipelineLayout ? endP : ((endP + 15) & ~15ull);
                    cur.skip(aligned - cur.pos);
                }
                const u64 elemOff = cur.pos;
                {
                    const u64 endE = cur.pos + (u64)elemSize * V;
                    const u64 aligned = pipelineLayout ? endE : ((endE + 15) & ~15ull);
                    cur.skip(aligned - cur.pos);
                }
                const u64 idxOff = cur.pos;
                cur.skip((u64)idxSize * N);
                if (peek_magic(cur.pos) == 0x4C48534D) {   // MSHL
                    cur.skip(4);
                    const u32 mc = cur.u32_();
                    if (mc > 0 && mc < 100000u) {
                        cur.skip(60ull * mc);
                        if (cur.pos + 4 <= inSize) { const u32 mvc = cur.u32_(); if (mvc < 10000000u) cur.skip(4ull * mvc); }
                        if (cur.pos + 4 <= inSize) { const u32 mtc = cur.u32_(); if (mtc < 10000000u) cur.skip(mtc); }
                    }
                }
                if (peek_magic(cur.pos) == 0x20464453) {   // SDF
                    cur.skip(4);
                    cur.skip(12 + 24);
                    const u32 dc = cur.u32_(); cur.skip(2ull * dc);
                    const u32 vc = cur.u32_(); cur.skip(vc);
                    const u32 fc = cur.u32_(); cur.skip(2ull * fc);
                }
                if ((elemType & 0x10) && peek_magic(cur.pos) == 0x4D4C5655) {   // UVLM
                    cur.skip(4);
                    const u32 uvc = cur.u32_(); cur.skip(8ull * uvc);
                }
                if ((elemType & 0x20) && cur.pos + 8 <= inSize) {   // CDBK/CDB2
                    const u32 magic = peek_magic(cur.pos);
                    if (magic == 0x4B424443 || magic == 0x32424443) {
                        cur.skip(4);
                        if (magic == 0x32424443) cur.skip(4);
                        const u32 cc = cur.u32_();
                        if (cc > 0 && cc < 100000u) {
                            sm.cardCount = cc;
                            sm.cardBytes.resize(size_t(cc) * 32u);
                            if (cur.pos + 32ull * cc <= inSize)
                                std::memcpy(sm.cardBytes.data(), inBuf.data() + cur.pos, 32ull * cc);
                            else cur.bad = true;
                            cur.skip(32ull * cc);
                        }
                        const u32 sc = cur.u32_(); cur.skip(32ull * sc);
                    }
                }
                if (V > 0 && N > 0 &&
                    posOff + 12ull * V <= inSize && idxOff + (u64)idxSize * N <= inSize &&
                    elemOff + (u64)elemSize * V <= inSize)
                {
                    sm.positions.resize((size_t)V * 3);
                    std::memcpy(sm.positions.data(), inBuf.data() + posOff, 12ull * V);
                    sm.indices.resize((size_t)N);
                    if (idxSize == 4)
                        std::memcpy(sm.indices.data(), inBuf.data() + idxOff, 4ull * N);
                    else
                        for (u32 i = 0; i < N; ++i) {
                            u16 v; std::memcpy(&v, inBuf.data() + idxOff + 2ull * i, 2);
                            sm.indices[i] = v;
                        }
                    sm.elemSize = elemSize;
                    sm.elemType = elemType;
                    if (elemSize > 0)
                    {
                        sm.elements.resize((size_t)elemSize * V);
                        std::memcpy(sm.elements.data(), inBuf.data() + elemOff, (size_t)elemSize * V);
                    }
                    sm.rawIndices.resize(size_t(idxSize) * N);
                    std::memcpy(sm.rawIndices.data(), inBuf.data() + idxOff, size_t(idxSize) * N);
                    subs.push_back(std::move(sm));
                    ++meshIndex;
                }
            }
        }
        if (cur.bad) walkBad = true;
    };
    walk();
    std::fprintf(stderr, "[sc-bake-surface] subs=%zu walkBad=%d materials=%zu\n",
                 subs.size(), (int)walkBad, materialDiffuse.size());
    if (subs.empty() || walkBad) {
        PrintError("ENGINE_ERROR", "容器解析失败（非 pack_geometry/pack_mesh_data 格式？）",
                   "sc-bake-surface 支持 legacy 与 pipeline 两种 .model 容器");
        return kExitEngine;
    }

    // ---- 逐 mesh 烘焙表面页 ----
    cs::ScsHeader header{};
    header.albedo_scale = albedoScale;
    header.texels_per_unit = texelsPerUnit;
    std::vector<cs::MeshBakeResult> results;
    std::vector<cs::BoundaryBakeResult> boundaryResults;
    u32 bakedMeshes = 0, totalCards = 0, validTexels = 0, totalTexels = 0;
    u32 texturesLoaded = 0, texturesMissing = 0, variantsBaked = 0;
    std::vector<bool> variantMatches(textureVariants.size(), false);
    for (size_t k = 0; k < subs.size() && bakedMeshes < maxMeshes; ++k)
    {
        const Submesh& sm = subs[k];
        if (sm.cardBytes.empty() || sm.positions.empty()) continue;

        primal::tools::card_bake::data cards;
        cards.cards.resize(sm.cardCount);
        std::memcpy(cards.cards.data(), sm.cardBytes.data(), sm.cardBytes.size());

        cs::MeshGeometryView geom;
        geom.positions = sm.positions.data();
        geom.vertex_count = u32(sm.positions.size() / 3);
        geom.indices = reinterpret_cast<const u8*>(sm.indices.data());
        geom.index_size = 4;
        geom.index_count = u32(sm.indices.size());
        geom.elements = sm.elements.empty() ? nullptr : sm.elements.data();
        geom.element_stride = sm.elemSize;
        geom.digest_indices = sm.rawIndices.empty() ? nullptr : sm.rawIndices.data();
        geom.digest_index_size = sm.rawIndices.empty() ? 0u
            : u32(sm.rawIndices.size() / (sm.indices.size() ? sm.indices.size() : 1));

        cs::RgbaImage tex;
        const std::string diff = (sm.materialIdx < materialDiffuse.size())
            ? materialDiffuse[sm.materialIdx] : std::string();
        const std::string texPath = resolve_texture(diff);
        if (!texPath.empty()) {
            int tw = 0, th = 0, tc = 0;
            u8* px = stbi_load(texPath.c_str(), &tw, &th, &tc, 4);
            if (px) {
                tex.width = u32(tw); tex.height = u32(th);
                tex.rgba.assign(px, px + size_t(tw) * th * 4);
                stbi_image_free(px);
                tex.valid = true;
                ++texturesLoaded;
            }
        }
        if (!tex.valid) ++texturesMissing;

        cs::BakeParams bp{};
        bp.albedo_scale = albedoScale;
        bp.texels_per_unit = texelsPerUnit;
        bp.tint[0] = tintR; bp.tint[1] = tintG; bp.tint[2] = tintB;
        bp.verbosity = verbosity;

        cs::MeshBakeResult res;
        res.name_hash = cs::fnv1a64(sm.name.data(), sm.name.size());
        if (!cs::bake_mesh(geom, cards, tex, bp, res)) continue;
        for (const auto& pg : res.pages) {
            totalCards++;
            totalTexels += pg.resolution * pg.resolution;
            validTexels += pg.valid_texels;
        }
        ++bakedMeshes;
        const u64 baseDigest = res.material_digest;
        results.push_back(std::move(res));
        std::string lowerDiff = diff;
        for (auto& ch : lowerDiff) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        const bool boundarySelected=boundaryOut && lowerDiff.find(boundaryTexture)!=std::string::npos;
        auto appendBoundary=[&](const cs::RgbaImage& source,const cs::MeshBakeResult& primary) {
            if (!boundarySelected) return true;
            std::vector<u32> ids(geom.index_count/3u);
            for (u32 i=0;i<ids.size();++i) ids[i]=i;
            cs::BoundaryBakeResult supplementary;
            if (!source.valid || !cs::bake_boundary_samples(geom,cards,source,bp,ids,boundarySpacing,
                    primal::graphics::rhi::card_boundary::MaxSamplesPerMesh,supplementary) ||
                supplementary.geometry_digest!=primary.geometry_digest || supplementary.layout_digest!=primary.layout_digest ||
                supplementary.material_digest!=primary.material_digest) return false;
            boundaryResults.push_back(std::move(supplementary)); return true;
        };
        if (!appendBoundary(tex,results.back())) {
            PrintError("ENGINE_ERROR","补点源缺失/几何无效/预算超限或身份不匹配，拒绝写出",diff.c_str()); return kExitEngine;
        }
        for (size_t vi = 0; vi < textureVariants.size(); ++vi) {
            const auto& [name, scale] = textureVariants[vi];
            if (lowerDiff.find(name) == std::string::npos) continue;
            variantMatches[vi] = true;
            if (!tex.valid) {
                PrintError("ENGINE_ERROR", "变体纹理源缺失，拒绝写出不完整 sidecar", diff.c_str());
                return kExitEngine;
            }
            cs::RgbaImage variant = tex;
            primal::graphics::rhi::card_surface_identity::ScaleTextureRgb(
                variant.rgba.data(), size_t(variant.width) * variant.height, scale);
            cs::MeshBakeResult extra;
            extra.name_hash = results.back().name_hash;
            if (!cs::bake_mesh(geom, cards, variant, bp, extra)) {
                PrintError("ENGINE_ERROR", "材质变体烘焙失败", diff.c_str());
                return kExitEngine;
            }
            // Identical rounded textures and repeated pairs must not introduce
            // ambiguous duplicate triple keys.
            if (extra.material_digest == baseDigest) continue;
            bool duplicate = false;
            for (const auto& prior : results)
                if (prior.geometry_digest == extra.geometry_digest &&
                    prior.layout_digest == extra.layout_digest &&
                    prior.material_digest == extra.material_digest) { duplicate = true; break; }
            if (duplicate) continue;
            if (!appendBoundary(variant,extra)) {
                PrintError("ENGINE_ERROR","变体补点烘焙失败，拒绝写出",diff.c_str()); return kExitEngine;
            }
            for (const auto& pg : extra.pages) {
                ++totalCards;
                totalTexels += pg.resolution * pg.resolution;
                validTexels += pg.valid_texels;
            }
            results.push_back(std::move(extra));
            ++variantsBaked;
        }
    }

    for (size_t vi = 0; vi < textureVariants.size(); ++vi) {
        if (!variantMatches[vi]) {
            PrintError("ENGINE_ERROR", "变体未匹配纹理，拒绝写出；检查子串和 --max-meshes", textureVariants[vi].first.c_str());
            return kExitEngine;
        }
    }
    if (results.empty()) {
        PrintError("ENGINE_ERROR", "无卡可烘（资产无 CDB2 段？）",
                   "先运行 bake-cards 生成卡片布局，再 sc-bake-surface");
        return kExitEngine;
    }
    if (boundaryOut) {
        std::vector<u8> encoded;
        if (!primal::graphics::rhi::card_boundary::Encode(boundaryResults,encoded)) {
            PrintError("ENGINE_ERROR","补点无匹配/身份重复或超出 64MiB 文件预算，拒绝写出",boundaryTexture.c_str()); return kExitEngine;
        }
        if (!primal::graphics::rhi::card_boundary::Write(boundaryOut,boundaryResults)) {
            PrintError("ENGINE_ERROR","SCB1 原子写出失败",boundaryOut); return kExitEngine;
        }
    }
    if (!cs::write_sidecar(outPath, header, results)) {
        PrintError("ENGINE_ERROR", "sidecar 写出失败", outPath);
        return kExitEngine;
    }

    u64 boundarySamples=0;
    for (const auto& mesh:boundaryResults) boundarySamples+=mesh.samples.size();
    char msg[768];
    std::snprintf(msg, sizeof(msg),
                  "{\"meshes_baked\":%u,\"material_variants\":%u,\"cards_total\":%u,\"valid_texels\":%u,"
                  "\"total_texels\":%u,\"coverage_mean\":%.3f,\"textures_loaded\":%u,"
                  "\"textures_missing\":%u,\"albedo_scale\":%.3f,"
                  "\"bytes_in\":%llu,\"bytes_out\":%llu,\"boundary_meshes\":%zu,\"boundary_samples\":%llu}",
                  bakedMeshes, variantsBaked, totalCards, validTexels, totalTexels,
                  totalTexels ? (double)validTexels / (double)totalTexels : 0.0,
                  texturesLoaded, texturesMissing, (double)albedoScale,
                  (unsigned long long)inSize,
                  (unsigned long long)std::filesystem::file_size(outPath),boundaryResults.size(),
                  (unsigned long long)boundarySamples);
    PrintOk(msg);
    return kExitOk;
}

int VerbCookMesh(int argc, char** argv)
{
    const char* inPath = nullptr;
    const char* outPath = nullptr;
    bool uvatlas = false;
    bool uvatlasLightmap = false;
    bool cookSdf = false;
    for (int i = 0; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) inPath = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outPath = argv[++i];
        else if (std::strcmp(argv[i], "--uvatlas") == 0) uvatlas = true;
        else if (std::strcmp(argv[i], "--uvatlas-lightmap") == 0)
        {
            uvatlas = true;
            uvatlasLightmap = true;
        }
        else if (std::strcmp(argv[i], "--sdf") == 0) cookSdf = true;
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --in/--out/--uvatlas/--uvatlas-lightmap/--sdf");
            return kExitUsage;
        }
    }
    if (!inPath || !outPath)
    {
        PrintError("INVALID_PARAMS", "需要 --in <源网格> 与 --out <.mesh 输出>",
                   "示例: primal-engine cook-mesh --in mesh.obj --out mesh.mesh [--uvatlas-lightmap] [--sdf]");
        return kExitUsage;
    }

    // cook 走 ContentTools 离线管线，不需要初始化渲染引擎。
    u32 mesh_count = 0;
    u32 lightmap_uv_meshes = 0;
    u32 card_baked_meshes = 0;
    bool cooked = false;
    if (uvatlas)
    {
        cooked = CookMeshPipeline(inPath, outPath, true, uvatlasLightmap, cookSdf,
                                  &mesh_count, &lightmap_uv_meshes, &card_baked_meshes);
    }
    else if (cookSdf)
    {
        // --sdf 需 pipeline 路径（legacy CookMesh 不产 SDF 段）
        cooked = CookMeshPipeline(inPath, outPath, false, false, true,
                                  &mesh_count, &lightmap_uv_meshes, &card_baked_meshes);
        if (cooked) CountMeshStats(outPath, &mesh_count, &lightmap_uv_meshes, &card_baked_meshes);
    }
    else
    {
        cooked = CookMesh(inPath, outPath) != 0;
        if (cooked) CountMeshStats(outPath, &mesh_count, &lightmap_uv_meshes, &card_baked_meshes);
    }
    if (!cooked)
    {
        PrintError("ENGINE_ERROR", "CookMesh 失败（源格式不支持或内容管线报错）",
                   "确认输入是受支持的网格格式；详情见 stderr 日志");
        return kExitEngine;
    }

    long long bytes = 0;
    if (FILE* f = std::fopen(outPath, "rb"))
    {
        std::fseek(f, 0, SEEK_END);
        bytes = std::ftell(f);
        std::fclose(f);
    }

    char data[1024];
    u32 total_cards = 0, max_cards = 0;
    f32 mean_cards = 0.f;
    CountMeshStats(outPath, &mesh_count, &lightmap_uv_meshes, &card_baked_meshes,
                   &total_cards, &max_cards, &mean_cards);
    std::snprintf(data, sizeof(data),
                  "{\"input\":%s,\"path\":%s,\"bytes\":%lld,"
                  "\"meshes\":%u,\"lightmap_uv_meshes\":%u,\"card_baked_meshes\":%u,"
                  "\"card_bake_cards_total\":%u,\"card_bake_cards_max\":%u,"
                  "\"card_bake_cards_mean\":%.2f}",
                  JsonEscape(inPath).c_str(), JsonEscape(outPath).c_str(), bytes,
                  mesh_count, lightmap_uv_meshes, card_baked_meshes,
                  total_cards, max_cards, (double)mean_cards);
    PrintOk(data);
    return kExitOk;
}

int VerbSchema()
{
    // 离线 verb 表（聚合 schema 的 offline 半张，§3.2）。手工维护：
    // 离线命令少且稳定；C++ 侧无反射，DRY 化等 C# 侧聚合器做一致性校验。
    const char* table =
        "{\"$version\":\"offline-v3\",\"protocol\":\"primal.cli.v1\",\"channel\":\"offline\",\"commands\":["
        "{\"verb\":\"engine info\",\"description\":\"引擎健康检查 + 设备信息\",\"params\":[],\"returns\":\"object\"},"
        "{\"verb\":\"render\",\"description\":\"离屏渲染测试场景存 PNG\",\"params\":["
        "{\"name\":\"--out\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--width\",\"type\":\"integer\",\"required\":false},"
        "{\"name\":\"--height\",\"type\":\"integer\",\"required\":false},"
        "{\"name\":\"--frames\",\"type\":\"integer\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"cook-mesh\",\"description\":\"源网格离线 cook 为引擎 .mesh\",\"params\":["
        "{\"name\":\"--in\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--out\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--uvatlas\",\"type\":\"boolean\",\"required\":false},"
        "{\"name\":\"--uvatlas-lightmap\",\"type\":\"boolean\",\"required\":false},"
        "{\"name\":\"--sdf\",\"type\":\"boolean\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"bake-sdf\",\"description\":\"为已有 .model 的空 SDF 段填充 per-mesh SDF（几何逐字节不变，detail-SDF refine 数据源）\",\"params\":["
        "{\"name\":\"--in\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--out\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--resolution\",\"type\":\"integer\",\"required\":false},"
        "{\"name\":\"--top\",\"type\":\"integer\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"bake-cards\",\"description\":\"为已有 .model 补 per-mesh 离线卡片烘焙（v2 贴面 coverage 贪心，几何逐字节不变；SC 卡片 Lumen 对齐 Phase 2）\",\"params\":["
        "{\"name\":\"--in\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--out\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--resolution\",\"type\":\"integer\",\"required\":false},"
        "{\"name\":\"--surfels\",\"type\":\"boolean\",\"required\":false},"
        "{\"name\":\"--v1\",\"type\":\"boolean\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"sc-bake-surface\",\"description\":\"为 .model 的 CDB2 卡片烘焙 per-card 真实表面页（mask/depth/normal/albedo/材质参数）到 SCS1 sidecar；owner mesh 几何与材质，几何/布局/材质三摘要键控\",\"params\":["
        "{\"name\":\"--in\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--out\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--texture-dir\",\"type\":\"string\",\"required\":false},"
        "{\"name\":\"--texture-variant\",\"type\":\"string\",\"required\":false,\"description\":\"可重复：纹理路径子串:非负线性比例；按 sRGB 解码/缩放/重新量化源 RGBA8，保留原始材质页并添加三摘要变体。需有效纹理和 CDB2；任一变体无匹配或缺源返回 ENGINE_ERROR，不写输出\"},"
        "{\"name\":\"--boundary-out\",\"type\":\"string\",\"required\":false,\"description\":\"独立 SCB1 补点文件路径，必须与 --in/--out 不同且配合非空 --boundary-texture；只保存真实 mesh 局部位置及表面属性，不生成光照或启用 GPU 查询。有效输出采用同目录临时文件 rename；SCB1/SCS1 两文件提交非联合事务\"},"
        "{\"name\":\"--boundary-texture\",\"type\":\"string\",\"required\":false,\"description\":\"大小写无关的 diffuse 纹理路径子串；为匹配 owner mesh 的全部原始 primitive 及已请求材质变体生成三摘要隔离补点。需 CDB2 和有效纹理；无匹配、退化几何、单 mesh 超过 1048576 点或文件超过 64MiB 返回 ENGINE_ERROR，验证失败不写输出\"},"
        "{\"name\":\"--boundary-spacing\",\"type\":\"number\",\"required\":false,\"description\":\"边界最大理想采样间距，正有限 mesh 局部单位（缩放前），默认 0.1；需 --boundary-out/--boundary-texture。参数非法返回 INVALID_PARAMS，预算不足不截断 primitive\"},"
        "{\"name\":\"--texels-per-unit\",\"type\":\"number\",\"required\":false},"
        "{\"name\":\"--verbose\",\"type\":\"boolean\",\"required\":false},"
        "{\"name\":\"--max-meshes\",\"type\":\"integer\",\"required\":false},"
        "{\"name\":\"--albedo-scale\",\"type\":\"number\",\"required\":false},"
        "{\"name\":\"--tint\",\"type\":\"string\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"import-texture\",\"description\":\"源纹理离线 cook 为引擎 .texture（TEXR 容器）\",\"params\":["
        "{\"name\":\"--in\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--out\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--format\",\"type\":\"string\",\"required\":false},"
        "{\"name\":\"--compress\",\"type\":\"integer\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"bake-vt\",\"description\":\"虚拟纹理烘焙（薄包装 vtbake）：PNG 源图为引擎 .vtex\",\"params\":["
        "{\"name\":\"--in\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--out\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"--kind\",\"type\":\"string\",\"required\":false},"
        "{\"name\":\"--quality\",\"type\":\"integer\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"build-shaders\",\"description\":\"重建引擎着色器 metallib（cmake --target BuildEngineShaderBlob）\",\"params\":["
        "{\"name\":\"--build-dir\",\"type\":\"string\",\"required\":false}],\"returns\":\"object\"},"
        "{\"verb\":\"bake-gi\",\"description\":\"静态 GI 探针烘焙（需活管线场景；离线持久化通道未接，当前结构化拒绝）\",\"params\":[],\"returns\":\"object\"}"
        "]}";
    PrintOk(table);
    return kExitOk;
}

// --- 子进程小助手（bake-vt 薄包装 vtbake；§2.1"独立 CLI 先例"复用而非复制） ---

// 返回子进程退出码；stdout 透传到本进程 stderr（vtbake 的进度日志不入 JSON）。
int RunChild(const std::string& exe, const std::vector<std::string>& args)
{
#if defined(_WIN32)
    (void)exe; (void)args;
    return -1;
#else
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0)
    {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::dup2(STDERR_FILENO, STDOUT_FILENO); // 子进程 stdout（进度）并入 stderr
        ::execv(exe.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// 可执行文件同目录查找（AnchorWorkingDirectoryToExecutable 已把 cwd 锚到 exe 目录）。
bool FileExists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int VerbBakeVt(int argc, char** argv)
{
    std::vector<std::string> passThrough;
    const char* inPath = nullptr;
    const char* outPath = nullptr;
    for (int i = 0; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) inPath = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outPath = argv[++i];
        else passThrough.emplace_back(argv[i]);
    }
    if (!inPath || !outPath)
    {
        PrintError("INVALID_PARAMS", "需要 --in <源 PNG> 与 --out <.vtex 输出>",
                   "示例: primal-engine bake-vt --in albedo.png --out albedo.vtex [--kind color]");
        return kExitUsage;
    }

    // vtbake 产出在 <build-tree>/Tools/vtbake，本工具在 Darwin/<cfg>/——两者目录
    // 树不重合，按构建布局逐个探测（cwd 已锚到本 exe 目录）。
    std::string vtbake;
    if (const char* env = std::getenv("PRIMAL_VTBAKE_BIN"); env && FileExists(env)) vtbake = env;
    if (vtbake.empty())
    {
        // cwd 可能在 C# 侧输出目录（shaders 回退锚定，见上），探查两类布局。
        for (const char* candidate : {"../Tools/vtbake", "Tools/vtbake",
                                      "../../build-wt/Tools/vtbake", "../../../build-wt/Tools/vtbake",
                                      "../../build/Tools/vtbake", "../../../build/Tools/vtbake"})
        {
            if (FileExists(candidate)) { vtbake = candidate; break; }
        }
    }
    if (vtbake.empty())
    {
        PrintError("NOT_FOUND", "未找到 vtbake 可执行文件",
                   "先构建: cmake --build <build-dir> --target vtbake（产物在 <build-dir>/Tools/vtbake），或设 PRIMAL_VTBAKE_BIN");
        return kExitEngine;
    }

    std::vector<std::string> args;
    args.emplace_back(inPath);
    args.emplace_back(outPath);
    for (const auto& a : passThrough) args.emplace_back(a);

    LogToStderr("bake-vt: spawning vtbake…");
    const int exitCode = RunChild(vtbake, args);
    if (exitCode != 0)
    {
        PrintError("ENGINE_ERROR", "vtbake 失败（进度与错误见 stderr）",
                   "确认输入为 PNG；--kind 从 color/normal/scalar/auto 中选择");
        return kExitEngine;
    }

    long long bytes = 0;
    if (FILE* f = std::fopen(outPath, "rb"))
    {
        std::fseek(f, 0, SEEK_END);
        bytes = std::ftell(f);
        std::fclose(f);
    }
    char data[1024];
    std::snprintf(data, sizeof(data),
                  "{\"input\":%s,\"path\":%s,\"bytes\":%lld}",
                  JsonEscape(inPath).c_str(), JsonEscape(outPath).c_str(), bytes);
    PrintOk(data);
    return kExitOk;
}

int VerbImportTexture(int argc, char** argv)
{
    // ContentTools 受管 TextureFormat 枚举（map_managed_output_format 同序）
    struct FormatName { const char* name; u32 value; };
    static constexpr FormatName kFormats[] = {
        {"rgba8", 0}, {"bc7", 1}, {"bc1", 2}, {"bc3", 3}, {"bc5", 4},
    };

    const char* inPath = nullptr;
    const char* outPath = nullptr;
    const char* format = "rgba8";
    u32 compress = 0;
    f32 alphaThreshold = 0.5f;
    u32 mipLevels = 0;      // 0 = 完整 mip 链
    u32 dimension = 2;      // 2D
    for (int i = 0; i < argc; ++i)
    {
        const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (std::strcmp(argv[i], "--in") == 0) { inPath = next(); }
        else if (std::strcmp(argv[i], "--out") == 0) { outPath = next(); }
        else if (std::strcmp(argv[i], "--format") == 0) { format = next(); }
        else if (std::strcmp(argv[i], "--compress") == 0 && next()) compress = std::atoi(argv[i]) ? 1u : 0u;
        else if (std::strcmp(argv[i], "--alpha-threshold") == 0 && next()) alphaThreshold = std::strtof(argv[i], nullptr);
        else if (std::strcmp(argv[i], "--mips") == 0 && next()) mipLevels = static_cast<u32>(std::atoi(argv[i]));
        else if (std::strcmp(argv[i], "--dimension") == 0 && next()) dimension = static_cast<u32>(std::atoi(argv[i]));
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --in/--out/--format rgba8|bc7|bc1|bc3|bc5/--compress/--alpha-threshold/--mips/--dimension");
            return kExitUsage;
        }
    }
    if (!inPath || !outPath)
    {
        PrintError("INVALID_PARAMS", "需要 --in <源图> 与 --out <.texture 输出>",
                   "示例: primal-engine import-texture --in albedo.png --out albedo.texture --format bc7");
        return kExitUsage;
    }
    u32 formatValue = 0xFFFFFFFFu;
    for (const auto& f : kFormats)
        if (std::strcmp(format, f.name) == 0) { formatValue = f.value; break; }
    if (formatValue == 0xFFFFFFFFu)
    {
        PrintError("INVALID_PARAMS", (std::string("未知 --format '") + format + "'").c_str(),
                   "有效取值: rgba8/bc7/bc1/bc3/bc5");
        return kExitUsage;
    }

    // 与 C# Import/TextureImportSettings→CookTexture 相同的两段式：Query 探参
    // （同时是设置合法性校验）→ Cook 出切片 → 本地按 TEXR 容器落盘。
    ContentToolsCookResult info{};
    if (CookTextureQuery(inPath, dimension, alphaThreshold, mipLevels, 0u,
                         formatValue, compress, &info) == 0)
    {
        PrintError("ENGINE_ERROR", "纹理 cook 探测失败（源格式不支持或参数非法）",
                   "确认输入为 PNG/TGA/HDR/DDS；压缩格式需 bcn 压缩器可用");
        return kExitEngine;
    }

    std::vector<u8> slices(info.slices_size);
    std::vector<u8> icon(info.icon_size > 0 ? info.icon_size : 1);
    u32 slicesSize = 0, iconSize = 0;
    if (CookTexture(inPath, dimension, alphaThreshold, mipLevels, 0u,
                    formatValue, compress,
                    slices.data(), static_cast<u32>(slices.size()), &slicesSize,
                    icon.data(), static_cast<u32>(icon.size()), &iconSize,
                    &info) == 0)
    {
        PrintError("ENGINE_ERROR", "纹理 cook 失败", "查看 stderr 日志；压缩格式需压缩器可用，可先试 --compress 0");
        return kExitEngine;
    }
    slices.resize(slicesSize);

    // TEXR 容器（与 PrimalEditor_Avalonia/Import/TextureTexrWriter.cs 及
    // ContentToEngine.cpp::create_texture_resource 的读取端逐字段一致）。
    std::string out;
    out.reserve(slices.size() + 256);
    out += "TEXR";
    auto putU32 = [&out](u32 v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    auto putF32 = [&out](f32 v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    putU32(1);                                        // version
    putU32(static_cast<u32>(std::strlen(inPath)));    // sources 长度
    out += inPath;                                    // sources utf8
    putU32(1);                                        // sourceCount
    putU32(info.settings_dimension);
    putU32(info.settings_mip_levels);
    putF32(info.settings_alpha_threshold);
    putU32(info.settings_prefer_bc7);
    putU32(info.settings_output_format);
    putU32(info.settings_compress);
    putU32(info.info_width);
    putU32(info.info_height);
    putU32(info.info_array_size);
    putU32(info.info_flags);
    putU32(info.info_mip_levels);
    putU32(info.info_format);

    uLongf compressedSize = compressBound(slicesSize);
    std::vector<u8> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize,
                  slices.data(), static_cast<uLong>(slicesSize), Z_BEST_SPEED) != Z_OK)
    {
        PrintError("ENGINE_ERROR", "TEXR zlib 压缩失败", "重试；连续失败请附 stderr 上报");
        return kExitEngine;
    }
    compressed.resize(compressedSize);
    putU32(static_cast<u32>(compressed.size()));
    out.append(reinterpret_cast<const char*>(compressed.data()), compressed.size());

    if (FILE* f = std::fopen(outPath, "wb"))
    {
        const auto written = std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        if (written != out.size())
        {
            PrintError("ENGINE_ERROR", "输出文件写入不完整", "检查磁盘空间与写权限");
            return kExitEngine;
        }
    }
    else
    {
        PrintError("ENGINE_ERROR", "无法创建输出文件", "检查输出目录写权限");
        return kExitEngine;
    }

    char data[768];
    std::snprintf(data, sizeof(data),
                  "{\"input\":%s,\"path\":%s,\"width\":%u,\"height\":%u,\"format\":\"%s\","
                  "\"compress\":%u,\"mipLevels\":%u,\"arraySize\":%u,\"bytes\":%llu}",
                  JsonEscape(inPath).c_str(), JsonEscape(outPath).c_str(),
                  info.info_width, info.info_height, format, compress,
                  info.info_mip_levels, info.info_array_size,
                  static_cast<unsigned long long>(out.size()));
    PrintOk(data);
    return kExitOk;
}

int VerbBuildShaders(int argc, char** argv)
{
    const char* buildDir = nullptr;
    for (int i = 0; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--build-dir") == 0 && i + 1 < argc) buildDir = argv[++i];
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --build-dir <cmake 构建目录>");
            return kExitUsage;
        }
    }

    // 探测顺序：显式指定 > PRIMAL_BUILD_DIR > 常规构建目录（含 CMakeCache 校验）。
    std::string dir;
    if (buildDir) dir = buildDir;
    else if (const char* env = std::getenv("PRIMAL_BUILD_DIR")) dir = env;
    if (dir.empty() || !DirExists(dir.c_str()))
    {
        // cwd 可能在 C# 侧输出目录（shaders 回退锚定），两类布局都探。
        for (const char* candidate : {"../Debug", "../DebugEditor", "../DebugVulkan", "../Release",
                                      "../../../Darwin/Debug", "../../../Darwin/DebugEditor",
                                      "../../../Darwin/DebugVulkan", "../../../Darwin/Release"})
        {
            const std::string probe = std::string(candidate) + "/CMakeCache.txt";
            if (FileExists(probe)) { dir = candidate; break; }
        }
    }
    if (dir.empty() || !DirExists(dir.c_str()))
    {
        PrintError("NOT_FOUND", "未找到 CMake 构建目录",
                   "用 --build-dir 指定，或先 cmake -S . -B Darwin/Debug；也可设 PRIMAL_BUILD_DIR");
        return kExitEngine;
    }

    LogToStderr((std::string("build-shaders: cmake --build ") + dir + " --target BuildEngineShaderBlob").c_str());
    const int exitCode = RunChild("/opt/homebrew/bin/cmake",
                                  {"--build", dir, "--target", "BuildEngineShaderBlob"});
    // /opt/homebrew/bin/cmake 在最小化环境可能不在 PATH，回退裸 "cmake"。
    int finalCode = exitCode;
    if (finalCode == 127)
        finalCode = RunChild("cmake", {"--build", dir, "--target", "BuildEngineShaderBlob"});
    if (finalCode != 0)
    {
        PrintError("ENGINE_ERROR", "BuildEngineShaderBlob 失败（编译器错误见 stderr）",
                   "确认 xcode-select 到完整 Xcode（需要 xcrun metal/metallib）后重试");
        return kExitEngine;
    }

    const std::string metallib = dir + "/shaders/metal/shaders.metallib";
    char data[1024];
    std::snprintf(data, sizeof(data),
                  "{\"buildDir\":%s,\"metallib\":%s,\"metallibExists\":%s}",
                  JsonEscape(dir.c_str()).c_str(),
                  JsonEscape(metallib.c_str()).c_str(),
                  FileExists(metallib) ? "true" : "false");
    PrintOk(data);
    return kExitOk;
}

int VerbBakeGi()
{
    // GIBakeStaticProbes 挂在已初始化的标准管线上（编辑器"烘焙"按钮路径），
    // 产物在管线运行态缓冲里——离线 one-shot 没有可落盘的探针导出通道。
    // 在导出侧补 SaveStaticProbes 之前，诚实结构化拒绝而不是假成功。
    PrintError("NOT_IMPLEMENTED",
               "bake-gi 离线通道未接通（探针持久化导出缺失）",
               "GI 探针烘焙暂在编辑器内进行（Lumen 面板 → Bake）；离线需求请在 PR 中提出");
    return kExitEngine;
}

// --- 宿主事件表端到端自测（CLI 规划 §12）---
// 链路：InstallEngineEvents（C ABI）→ primal::diag::Emit（核心插槽）→
// EngineDLL Dispatch → 事件表 on_log 回调。struct 为 EngineEventsAPI.h 的
// 布局镜像（static_assert 锁尺寸，§12.3 ABI 纪律）。

constexpr u32 kPrimalEventsV1 = 1u;

struct PrimalLogFields
{
    u64 frame;
    i64 threadId;
};

struct PrimalEngineEventsV1
{
    u32 version;
    u64 size;
    void* userdata;
    void (*onLog)(void* ud, u32 level, const char* category, const char* message,
                  const PrimalLogFields* fields);
    void (*onAssert)(void* ud, const char* file, int line, const char* condition);
    void (*onCrash)(void* ud, const void* info);
    void (*onProgress)(void* ud, const char* op, int done, int total);
    void (*onDeviceLost)(void* ud, const char* backend, const char* detail);
};
static_assert(sizeof(PrimalEngineEventsV1) == 64, "events v1 layout drift");

extern "C" {
u32 InstallEngineEvents(const PrimalEngineEventsV1* events);
void SetLogFile(const char* path, u32 maxBytes, u32 maxFiles);
void FlushLogQueue();
}

namespace {
struct EventsCapture
{
    char category[64];
    char message[256];
    u32 level = 0xFFFFFFFF;
    bool fieldsSeen = false;
};
EventsCapture g_capture;

void OnLogCapture(void* ud, u32 level, const char* category, const char* message,
                  const PrimalLogFields* fields)
{
    auto* capture = static_cast<EventsCapture*>(ud);
    capture->level = level;
    std::snprintf(capture->category, sizeof(capture->category), "%s", category ? category : "");
    std::snprintf(capture->message, sizeof(capture->message), "%s", message ? message : "");
    capture->fieldsSeen = fields != nullptr;
}
} // namespace

int VerbSelftestEvents()
{
    // 拒绝未知版本。
    PrimalEngineEventsV1 bad{};
    bad.version = 99;
    bad.size = sizeof(PrimalEngineEventsV1);
    if (InstallEngineEvents(&bad) == 0)
        LogToStderr("selftest: unknown-version rejection OK");
    else
    {
        PrintError("ENGINE_ERROR", "事件表接受了非法 version", "检查 InstallEngineEvents 的版本校验");
        return kExitEngine;
    }

    g_capture = EventsCapture{};
    PrimalEngineEventsV1 table{};
    table.version = kPrimalEventsV1;
    table.size = sizeof(PrimalEngineEventsV1);
    table.userdata = &g_capture;
    table.onLog = &OnLogCapture;
    if (InstallEngineEvents(&table) != 1)
    {
        PrintError("ENGINE_ERROR", "InstallEngineEvents 拒绝了合法表", "检查版本常量与 size 语义");
        return kExitEngine;
    }

    // 核心插槽（DllMain 已注入 EngineDLL 分发器）→ 事件表回调。
    // on_log 经后台 flush 线程异步送达（L0），断言前先同步排空。
    primal::diag::Emit(primal::diag::kLevelWarn, "Selftest", "events roundtrip hello");
    FlushLogQueue();

    bool ok = g_capture.level == primal::diag::kLevelWarn
        && std::strcmp(g_capture.category, "Selftest") == 0
        && std::strcmp(g_capture.message, "events roundtrip hello") == 0
        && g_capture.fieldsSeen;
    if (!ok)
    {
        PrintError("ENGINE_ERROR", "事件表 on_log 未按预期触发",
                   "检查 RegisterCoreEmitter 注入与 Dispatch 的 FireLog 转发");
        return kExitEngine;
    }

    // FileSink + 轮转参数接受性（L2）：写一条 → flush → 文件应包含该行。
    SetLogFile("/tmp/primal-selftest-log.log", 0, 0);
    primal::diag::Emit(primal::diag::kLevelInfo, "Selftest", "file sink line");
    FlushLogQueue();
    SetLogFile(nullptr, 0, 0);
    bool fileOk = false;
    if (FILE* f = std::fopen("/tmp/primal-selftest-log.log", "rb"))
    {
        char buf[512] = {0};
        const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        std::remove("/tmp/primal-selftest-log.log");
        fileOk = std::strstr(buf, "[Selftest] file sink line") != nullptr;
        (void)n;
    }
    if (!fileOk)
    {
        PrintError("ENGINE_ERROR", "FileSink 未写入日志行", "检查 SetLogFile/FlushLogQueue 链路");
        return kExitEngine;
    }

    // 崩溃快照（L2）：快照应包含最近日志（异步队列之外，发射线程同步发布）。
    u32 snapLen = 0;
    const void* snap = ::primal::engine_dll::diag::GetCrashSnapshot(&snapLen);
    bool snapOk = snap != nullptr && snapLen > 4
        && std::strstr(static_cast<const char*>(snap) + 4, "file sink line") != nullptr; // +4 跳过长度头
    if (!snapOk)
    {
        PrintError("ENGINE_ERROR", "崩溃快照未包含最近日志", "检查 PublishCrashSnapshot 链路");
        return kExitEngine;
    }

    InstallEngineEvents(nullptr); // 卸载，不污染后续 verb
    PrintOk("{\"roundtrip\":true,\"versionRejection\":true,\"uninstall\":true,"
            "\"fileSink\":true,\"crashSnapshot\":true}");
    return kExitOk;
}

int Run(int argc, char** argv)
{
    AnchorWorkingDirectoryToExecutable();

    if (argc < 2)
    {
        std::fprintf(stderr,
                     "primal-engine — Primal Engine 离线 CLI\n"
                     "用法:\n"
                     "  primal-engine engine info\n"
                     "  primal-engine render --out f.png [--width W] [--height H] [--frames N]\n"
                     "  primal-engine cook-mesh --in m.obj --out m.mesh [--uvatlas|--uvatlas-lightmap] [--sdf]\n"
                     "  primal-engine bake-sdf --in m.model --out mSDF.model [--resolution 48] [--top 16]\n"
                     "  primal-engine bake-cards --in m.model --out m2.model [--resolution 32] [--surfels 0] [--v1]\n"
                     "  primal-engine sc-bake-surface --in m.model --out m.model.scsurf [--texture-dir DIR] [--albedo-scale S]\n"
                     "  primal-engine import-texture --in t.png --out t.texture [--format bc7]\n"
                     "  primal-engine bake-vt --in t.png --out t.vtex [--kind color|normal|scalar|auto]\n"
                     "  primal-engine build-shaders [--build-dir DIR]\n"
                     "  primal-engine schema\n");
        return kExitUsage;
    }

    const std::string verb = argv[1];
    if (verb == "engine" && argc >= 3 && std::strcmp(argv[2], "info") == 0) return VerbEngineInfo();
    if (verb == "render") return VerbRender(argc - 2, argv + 2);
    if (verb == "physics-sim") return VerbPhysicsSim(argc - 2, argv + 2);
    if (verb == "pfx-debug") return VerbPfxDebug(argc - 2, argv + 2);
    if (verb == "cook-mesh") return VerbCookMesh(argc - 2, argv + 2);
    if (verb == "bake-sdf") return VerbBakeSdf(argc - 2, argv + 2);
    if (verb == "bake-cards") return VerbBakeCards(argc - 2, argv + 2);
    if (verb == "sc-bake-surface") return VerbScBakeSurface(argc - 2, argv + 2);
    if (verb == "import-texture") return VerbImportTexture(argc - 2, argv + 2);
    if (verb == "bake-vt") return VerbBakeVt(argc - 2, argv + 2);
    if (verb == "build-shaders") return VerbBuildShaders(argc - 2, argv + 2);
    if (verb == "bake-gi") return VerbBakeGi();
    if (verb == "selftest") return VerbSelftestEvents();
    if (verb == "schema") return VerbSchema();

    PrintError("BAD_VERB", (std::string("未知命令 '") + verb + "'").c_str(),
               "支持: engine info / render / cook-mesh / bake-sdf / import-texture / bake-vt / build-shaders / bake-gi / schema");
    return kExitUsage;
}

} // namespace

#if defined(_WIN32)
int wmain(int, wchar_t**)
{
    // Windows 当前没有可用 verb（EngineDLL MSVC 分支未导出 InitializeEngine，
    // 见 VerbEngineInfo/VerbRender 的说明），先给出结构化错误而非假运行。
    PrintError("NOT_SUPPORTED", "primal-engine 的离线 verbs 暂不支持 Windows",
               "使用 macOS/Linux 运行离线 verbs");
    return kExitEngine;
}
#else
int main(int argc, char** argv) { return Run(argc, argv); }
#endif
