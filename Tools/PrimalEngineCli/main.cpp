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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
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
                           u32* out_lightmap_uv_meshes, u32* out_card_baked_meshes)
{
    *out_mesh_count = 0;
    *out_lightmap_uv_meshes = 0;
    *out_card_baked_meshes = 0;

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
        },
        errs);
    return true;
}

// A9 PR2: 资产管线 cook（ProcessAIAsset）。uvatlas 开启时按 ContentToolsCLI
// --pipeline 的默认组合装配 POD；lightmap 再把 uvatlas 切到 Lightmap 预设
// （purpose=Lightmap、gutter=4、strict unique pack），生成的第二套 UV 落入
// blob 的 UVLM sidecar 段而非被丢弃。
static bool CookMeshPipeline(const char* inPath, const char* outPath,
                             bool uvatlas, bool uvatlasLightmap,
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
    p.enable_card_bake = 1;
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

int VerbCookMesh(int argc, char** argv)
{
    const char* inPath = nullptr;
    const char* outPath = nullptr;
    bool uvatlas = false;
    bool uvatlasLightmap = false;
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
        else
        {
            PrintError("INVALID_PARAMS", (std::string("未知参数: ") + argv[i]).c_str(),
                       "支持 --in/--out/--uvatlas/--uvatlas-lightmap");
            return kExitUsage;
        }
    }
    if (!inPath || !outPath)
    {
        PrintError("INVALID_PARAMS", "需要 --in <源网格> 与 --out <.mesh 输出>",
                   "示例: primal-engine cook-mesh --in mesh.obj --out mesh.mesh [--uvatlas-lightmap]");
        return kExitUsage;
    }

    // cook 走 ContentTools 离线管线，不需要初始化渲染引擎。
    u32 mesh_count = 0;
    u32 lightmap_uv_meshes = 0;
    u32 card_baked_meshes = 0;
    bool cooked = false;
    if (uvatlas)
    {
        cooked = CookMeshPipeline(inPath, outPath, true, uvatlasLightmap,
                                  &mesh_count, &lightmap_uv_meshes, &card_baked_meshes);
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
    std::snprintf(data, sizeof(data),
                  "{\"input\":%s,\"path\":%s,\"bytes\":%lld,"
                  "\"meshes\":%u,\"lightmap_uv_meshes\":%u,\"card_baked_meshes\":%u}",
                  JsonEscape(inPath).c_str(), JsonEscape(outPath).c_str(), bytes,
                  mesh_count, lightmap_uv_meshes, card_baked_meshes);
    PrintOk(data);
    return kExitOk;
}

int VerbSchema()
{
    // 离线 verb 表（聚合 schema 的 offline 半张，§3.2）。手工维护：
    // 离线命令少且稳定；C++ 侧无反射，DRY 化等 C# 侧聚合器做一致性校验。
    const char* table =
        "{\"$version\":\"offline-v2\",\"protocol\":\"primal.cli.v1\",\"channel\":\"offline\",\"commands\":["
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
        "{\"name\":\"--uvatlas-lightmap\",\"type\":\"boolean\",\"required\":false}],\"returns\":\"object\"},"
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
                     "  primal-engine cook-mesh --in m.obj --out m.mesh [--uvatlas|--uvatlas-lightmap]\n"
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
    if (verb == "import-texture") return VerbImportTexture(argc - 2, argv + 2);
    if (verb == "bake-vt") return VerbBakeVt(argc - 2, argv + 2);
    if (verb == "build-shaders") return VerbBuildShaders(argc - 2, argv + 2);
    if (verb == "bake-gi") return VerbBakeGi();
    if (verb == "selftest") return VerbSelftestEvents();
    if (verb == "schema") return VerbSchema();

    PrintError("BAD_VERB", (std::string("未知命令 '") + verb + "'").c_str(),
               "支持: engine info / render / cook-mesh / import-texture / bake-vt / build-shaders / bake-gi / schema");
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
