// TestRHISponzaShared.cpp — D3D12 M1 F1-8 / M2 F2-6 共享测试宿主（双后端参数化）。
//
// 同一份调用代码，仅以 env RHI_TEST_PLATFORM=vulkan|d3d12 切换后端：
//   CreateRHIDevice → Sponza 场景加载（顶点/索引/逐 mesh albedo 纹理）
//   → M1Forward 管线（vulkan=SPIR-V / d3d12=DXIL .cso，按 platform 装载）
//   → 轨迹驱动 N 帧（EngineTest/trajectories/m1_sponza_forward.json，
//      线性插值相机位姿 + lookAt + fov）
//   → 每帧离屏 RT（BGRA8 UNorm）渲染 → readback → 逐帧非黑校验
//   → 帧子集 P6 PPM 落 $RHI_CAPTURE_DIR
//   → resize 段（中途换一次 RT 尺寸重走创建，若干帧后换回）
//   → 全量销毁 + 泄漏计数 sanity + 关闭。
//
// F2 Y 契约（2026-11-05，d3d12-blackfix 批）：主相机投影在
// CreatePerspectiveMatrix 后对 y 行整行取反（Y-down clip，与生产 naga 对
// gl_Position.y 取反同效）→ 两端视口下世界 +Y 落 RT 小行号、帧正立
//（§7.2-1 旧叙述"两端原始读回同向"仍成立，但方向由倒立改正立）。投影
// 取反使帧缓冲空间绕序变号，主相机光栅化管线 cull Back→Front 对换
//（M1 lit / M2 lit / M4 GBuffer / M5 复用 M1；depth 与全屏 pass Cull
// None 不动）。两端 frontFace 均硬编码 CW、不可配置，故对换在管线侧。
//
// M2 F2-6 阴影段（RHI_TEST_SHADOWS=1 启用，默认 0 = M1 行为不变，
// M1 回归门禁照跑）：每帧先录单级联 shadow depth pass（离屏 D32 2048²，
// M2ShadowDepth.vert，depth-only/Cull None/Less——生产 ExecuteShadowRaster
// 同构），InsertBarrier DepthStencil→ShaderResource 后走 M2ForwardLit.frag
// lit pass（M1 RT 上采样阴影：比较采样器 LessEqual + PCF 3×3 + 生产 B17.2
// bias/normal-offset 参数——单源 GLSL 双链，反射契约锁定绑定）。
// RHI_SHADOW_LIT_ALL=1 消融档：比较采样器改传 Always（RHI 默认坑值本身
// 成为消融开关）→ 采样恒 1 → 无阴影对照帧，供阴影可见性 A/B 统计。
//
// M3 F3-2/F3-3 post 段（RHI_TEST_POST=1 启用，默认 0 = M1/M2 行为不变；
// 可与 RHI_TEST_SHADOWS=1 组合）：lit pass 改写 RGBA16F HDR 中间 RT（管线
// RT 格式数组变更）→ post 全屏三角形 pass（M3Post.vert+frag：固定 EV 曝光
// ×生产同源 ACES 拟合 2.51/0.03/2.43/0.59/0.14）→ BGRA8 UNorm 直写（唯一
// 编码点：无 sRGB 视图、无 shader pow——M1 §7.2-3 契约；生产 pow(1/2.2)
// 为其交换链侧编码，共同链编码点推迟 M7，偏差登记于 M3 交付报告）。
// 曝光 = 轨迹 render.post.exposure（默认 1.0）× 2^RHI_POST_EV；全固定
// 常数、无 adapt 时变项（确定性纪律沿 M1/M2）。
// RHI_HDR_DUMP=1：首帧读回 HDR 中间（F3-5 中间输出可定位；线性域 [0,1]
// 预览 PPM hdr_0.ppm，>1 值贴 255——诊断预览，非编码输出）。
//
// M4 deferred 段（RHI_TEST_DEFERRED=1 启用，默认 0 = M1/M2/M3 行为不变；
// 需 RHI_TEST_SHADOWS=1 组合——延迟光照 shader 绑定 shadow map/比较采样器，
// 无阴影档为配置错误 exit 2）：每帧 lit pass 换为两段——
//   ① GBuffer pass（M4GBuffer.vert+frag，MRT×3 + D32：RT0 albedo RGBA8×
//      材质色 / RT1 世界法线 RG16F xy 直存 / RT2 材质占位+N.z 符号 RGBA8；
//      set0/set1/push 复用 M1 布局——零新描述符集）
//   ② 延迟光照 pass（M3Post.vert 全屏三角 + M4DeferredLight.frag：texelFetch
//      1:1 读 GBuffer+深度，invViewProjection 从 NDC 深度重建世界位（生产
//      DeferredLighting.frag:274-297 同源公式），光照/阴影表达式与
//      M2ForwardLit.frag 逐字同源——同轨迹同帧 DEFERRED=0 vs =1 差异只剩
//      GBuffer 量化（M4 核心等价证据，F4-4））。
// RHI_GBUFFER_DUMP=1：首帧读回 GBuffer 三 RT + 深度（F4-6 中间输出定位：
// gbuffer_albedo_0.ppm / gbuffer_normal_0.ppm（f16 解码，v*0.5+0.5 预览）/
// gbuffer_material_0.ppm / gbuffer_depth_0.pgm）。
// 光照关键帧（F4-7 光照变化段）：轨迹 render.light.keyframes=[{time,
// intensity?,dir_to_light?,color?}...] 步进求值（取 time<=t 的最后一条），
// 光参数由轨迹 config 驱动按帧切换——确定性纪律（无时变项）；缺省无
// 关键帧 = 静态光（M1-M3 行为逐位不变）。
//
// M6 GI 段（RHI_TEST_GI=1 启用，需 RHI_TEST_DEFERRED=1；默认 0 = M1-M5 与
// GI-off 延迟路径逐位不变）：单次反弹、屏幕空间直射光注入的 SH-L1 探针
// 网格。不是完整物理场景 GI——传输无可见性项（注入不测探针→发射体遮挡）、
// 发射体只有相机可见像素（屏外/被挡表面不发光）、无自发光/天空/IBL。
// 每帧链（GBuffer 之后，GI 模式替换 M4 延迟光照 pass）：
//   P2 直射 pass（M3Post.vert + M6DeferredDirect.frag，MRT×2 RGBA16F）：
//      giDirect = 光色×(漫反射+高光)×阴影（a=ndl 诊断）；giDirectDiffuse =
//      光色×漫反射×阴影（a=shadow 诊断，GI 唯一输入）。完全阴影面两者恰为
//      0——无 ambient 常数、无旧 GI、无 IBL（线性 HDR，预曝光）。
//   P3 注入（M6ProbeInject.comp，一 workgroup 64 线程 = 一探针，dispatch
//      8×8×8）：屏幕 8px 步长读 giDirectDiffuse+深度+法线，像素当 Lambert
//      发射体累积 SH-L1 入射辐射 → 注入输出纹理 M6ProbeInjectOut（RGBA16F
//      3D (8,8,24)，slab=R/G/B）。
//   P4 累积（M6ProbeAccum.comp 原样）：grid = hist*0.9 + injectOut*0.1，光事件/
//      首帧 reset ⇒ history 清零——全链唯一时间递归（GI 输入不含间接/前帧
//      输出 ⇒ 无光反馈回路）。
//   P5 解析（M3Post.vert + M6GIResolve.frag）：giIndirect = albedo×E_h(N)
//      ——独立间接缓冲（无强度旋钮）。
//   P6 合成（M3Post.vert + M6Composite.frag）：direct + indirect 恰一次
//      → post on 写 hdr / off 写 color；P7 post 原样。
// 探针盒 = 全部 mesh 局部 AABB 并集各向外扩 0.5m（model=identity，同 M5
// 包围球源）。[GI_MEAN] 阶梯 = 512 探针 0.282095·c00 逐通道均值（各向同性
// 平均入射辐射）。RHI_GI_EXPORT_FRAMES=<csv>（+RHI_GI_EXPORT_DIR）：DEFERRED=1
// 时（GI on/off 均可）在指定帧原始浮点导出 giexp_f<N>_<name>.bin（紧密行
// 落盘——GPU 读回行距 256 对齐，填充字节不写出，逐位可复现）+ giexp_f<N>.json
// 清单（数值验证用，见 M6 spec §6.5）。
// GI 模式黑帧守卫（F1-10）改测几何覆盖率（逐帧深度读回，深度<1 占比 ≥2%）：
// 完全阴影几何按契约恰为黑，"非黑比例=几何覆盖"前提在 GI 模式失效；单帧
// 颜色暗帧合法（数量如实输出于 [GI] black-frame guard 行），但全部帧颜色暗
// = 颜色链（direct/indirect/composite）无出光证据 → 判失败。
//
// 纪律（D3D12 M0 F0-12 静态依赖门禁）：本文件零后端分支、零后端
// include/static_cast——全部经 RHI Core 中立接口（命令录制走
// rhi::GetCommandBuffer 中立自由函数）。d3d12 分支在 macOS 上由工厂
// 返回 nullptr → 显式 SKIP 输出 + exit 42（Windows 集成会话复用同一
// 二进制，见 M1 PhaseDoc §5 验收④）。
//
// 退出码：0=通过；1=断言失败；2=配置错误；42=SKIP（平台在本机不可用）。
// dt：固定 = 轨迹 timing.timestep（M0 §3.5 契约；每帧 stderr 一行
// [FIXED_DT] frame N dt=<v> 供 manifest 生成器核验）。
// 冒烟：RHI_TEST_FRAMES=3（resize 段默认关闭，帧数 ≤8 视为冒烟）。

#include "Engine/Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/ParticleFX/core/PfxJson.h"

#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
namespace rhimath = primal::graphics::rhi::math;
namespace pfxjson = primal::pfx::json;

namespace {

// ======================= 通用小工具 =======================

int g_failures = 0;

void Fail(const std::string& msg) {
    std::cerr << "[RHI_SHARED] FAIL: " << msg << std::endl;
    ++g_failures;
}

void Require(bool cond, const std::string& msg) {
    if (!cond) Fail(msg);
}

std::string EnvOr(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}

std::vector<u32> ParseCsvU32(const std::string& s) {
    std::vector<u32> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(u32(std::strtoul(tok.c_str(), nullptr, 10)));
    }
    return out;
}

// 相对路径解析：cwd 直接命中，否则逐级向上走（最多 8 级），使二进制在
// Darwin/DebugVulkan/ 下运行时能找到仓库相对资产（沿 LoadSpvBytes 惯例）。
// FindFileRel 额外返回命中路径本身（供派生兄弟路径，如纹理目录）——
// ctest 的工作目录与手工运行不同，纹理目录必须锚定在命中的资产前缀上。
std::string FindFileRel(const std::string& rel) {
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec) &&
               std::filesystem::is_regular_file(p, ec);
    };
    if (exists(rel)) return rel;
    for (int i = 1; i <= 8; ++i) {
        std::string prefix;
        for (int j = 0; j < i; ++j) prefix += "../";
        if (exists(prefix + rel)) return prefix + rel;
    }
    return "";
}

std::vector<u8> ReadFileRel(const std::string& rel) {
    auto tryPath = [](const std::string& p) -> std::vector<u8> {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<u8> bytes;
        bytes.resize(static_cast<size_t>(sz));
        if (sz > 0) f.read(reinterpret_cast<char*>(bytes.data()), sz);
        return bytes;
    };
    std::vector<u8> bytes = tryPath(rel);
    for (int i = 1; i <= 8 && bytes.empty(); ++i) {
        std::string prefix;
        for (int j = 0; j < i; ++j) prefix += "../";
        bytes = tryPath(prefix + rel);
    }
    return bytes;
}

// ======================= D3D12 cso SHA 门禁（错位护盾，2026-11-08） =======
// 背景：M6 D3D12 shader 组（cso/hlsl + manifest）是未提交工作产物，
// Windows 树曾拿到中间迭代 cso → M6Composite 管线创建 E_INVALIDARG
// (0x80070057) 且无任何先兆。本门禁在 CreateShader 前对 cso 字节做
// SHA-256 校验，期望值取自 EngineTest/shaders/D3D12/manifest.json 的
// cso_sha256（生成链单一事实源）。仅 wantD3D12 路径调用——Vulkan 分支
// 零新读盘、零行为变化。路径前缀照抄 loadShader 调用点拼写
//（"EngineTest/shaders/D3D12/"——磁盘目录大小写以两端文件系统实际
// 解析为准，macOS/Windows 均大小写不敏感，已实测解析）。
namespace sha_gate {

// 纯函数 SHA-256（FIPS 180-4；仓库内无现成可复用实现——Engine/ 仅有
// CMake 侧 file(SHA256) 与注释字符串，故 harness 内自带最小实现，
// 无新依赖）。正确性已与 shasum -a 256 对真实 cso 文件核对（同码文本
// 独立编译验证 + 本机树 cso/manifest 自洽实测）。
struct Sha256Digest {
    u32 h[8];
};

inline u32 Rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

inline void Sha256Block(const u32* const k, Sha256Digest& st, const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (u32(p[i * 4]) << 24) | (u32(p[i * 4 + 1]) << 16) |
               (u32(p[i * 4 + 2]) << 8) | u32(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        const u32 s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = st.h[0], b = st.h[1], c = st.h[2], d = st.h[3];
    u32 e = st.h[4], f = st.h[5], g = st.h[6], h = st.h[7];
    for (int i = 0; i < 64; ++i) {
        const u32 S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + S1 + ch + k[i] + w[i];
        const u32 S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st.h[0] += a; st.h[1] += b; st.h[2] += c; st.h[3] += d;
    st.h[4] += e; st.h[5] += f; st.h[6] += g; st.h[7] += h;
}

inline Sha256Digest Sha256(const u8* data, size_t len) {
    static const u32 k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    Sha256Digest st{{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}};
    const size_t total = len;
    size_t off = 0;
    while (off + 64 <= total) {
        Sha256Block(k, st, data + off);
        off += 64;
    }
    // 尾块 + padding（0x80 + 0 填充 + 8 字节大端 bit 长度）。
    u8 tail[128];
    const size_t rem = total - off;
    for (size_t i = 0; i < rem; ++i) tail[i] = data[off + i];
    tail[rem] = 0x80;
    size_t padded = (rem + 1 + 8 <= 64) ? 64 : 128;
    for (size_t i = rem + 1; i < padded; ++i) tail[i] = 0;
    const u64 bits = u64(total) * 8;
    for (int i = 0; i < 8; ++i)
        tail[padded - 1 - i] = u8((bits >> (8 * i)) & 0xFF);
    Sha256Block(k, st, tail);
    if (padded == 128) Sha256Block(k, st, tail + 64);
    return st;
}

inline std::string ToHex32(const u32* h) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (int i = 0; i < 8; ++i)
        for (int shift = 28; shift >= 0; shift -= 4)
            s += hex[(h[i] >> shift) & 0xF];
    return s;
}

// ---- manifest 解析（固定形状最小扫描，非通用 JSON）----
// manifest.shaders[] 每条目：{... "cso": "Forward/X.cso",
// "cso_sha256": "<hex64>" ...}。按花括号对象配对两键，键序无关；
// 仅扫描 "shaders" 数组区间。条目数与配对数一致才接受。
struct Manifest {
    bool ok{false};
    std::string error;
    std::unordered_map<std::string, std::string> csoToSha;
};

inline bool ExtractJsonString(const std::string& text, size_t from,
                              std::string& out, size_t& after) {
    if (from >= text.size() || text[from] != '"') return false;
    const size_t start = from + 1;
    size_t i = start;
    while (i < text.size() && text[i] != '"') {
        if (text[i] == '\\') ++i;   // 跳过转义（本 manifest 无转义，防御）
        ++i;
    }
    if (i >= text.size()) return false;
    out = text.substr(start, i - start);
    after = i + 1;
    return true;
}

// 在 [from,to) 内找 "key": "value"（key 为精确引号串），返回 value。
inline bool FindKeyValue(const std::string& text, size_t from, size_t to,
                         const std::string& key, std::string& value,
                         size_t& after) {
    const std::string pat = "\"" + key + "\"";
    size_t pos = from;
    while (true) {
        const size_t hit = text.find(pat, pos);
        if (hit == std::string::npos || hit + pat.size() > to) return false;
        size_t i = hit + pat.size();
        while (i < to && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                          text[i] == '\r'))
            ++i;
        if (i < to && text[i] == ':') ++i;
        else { pos = hit + 1; continue; }
        while (i < to && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                          text[i] == '\r'))
            ++i;
        if (ExtractJsonString(text, i, value, after)) return true;
        pos = hit + 1;
    }
}

inline Manifest ParseManifest(const std::vector<u8>& bytes) {
    Manifest m;
    const std::string text(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
    // "shaders" 数组区间：键 → 下一个 '[' → 匹配的 ']'（不嵌套数组）。
    const size_t shadersKey = text.find("\"shaders\"");
    if (shadersKey == std::string::npos) {
        m.error = "no \"shaders\" array";
        return m;
    }
    const size_t arrStart = text.find('[', shadersKey);
    if (arrStart == std::string::npos) {
        m.error = "shaders array '[' not found";
        return m;
    }
    size_t depth = 0;
    size_t arrEnd = std::string::npos;
    for (size_t i = arrStart; i < text.size(); ++i) {
        if (text[i] == '[') ++depth;
        else if (text[i] == ']') {
            --depth;
            if (depth == 0) { arrEnd = i; break; }
        }
    }
    if (arrEnd == std::string::npos) {
        m.error = "shaders array ']' not found";
        return m;
    }
    // 逐对象 {..} 扫描（花括号配对），每对象提取 cso / cso_sha256。
    size_t i = arrStart + 1;
    size_t objCount = 0, pairCount = 0;
    while (i < arrEnd) {
        if (text[i] != '{') { ++i; continue; }
        size_t objDepth = 0;
        size_t objEnd = std::string::npos;
        for (size_t j = i; j < arrEnd; ++j) {
            if (text[j] == '{') ++objDepth;
            else if (text[j] == '}') {
                --objDepth;
                if (objDepth == 0) { objEnd = j; break; }
            }
        }
        if (objEnd == std::string::npos) {
            m.error = "unbalanced object braces in shaders[]";
            return m;
        }
        ++objCount;
        std::string cso, sha;
        size_t dummy = 0;
        const bool gotCso =
            FindKeyValue(text, i, objEnd, "cso", cso, dummy);
        const bool gotSha =
            FindKeyValue(text, i, objEnd, "cso_sha256", sha, dummy);
        if (gotCso && gotSha && sha.size() == 64) {
            m.csoToSha[cso] = sha;
            ++pairCount;
        }
        i = objEnd + 1;
    }
    if (objCount == 0) {
        m.error = "shaders array has no entries";
        return m;
    }
    if (pairCount != objCount) {
        m.error = "incomplete entries: " + std::to_string(pairCount) + "/" +
                  std::to_string(objCount) + " objects carry cso+cso_sha256";
        return m;
    }
    m.ok = true;
    return m;
}

// 门禁主体：校验单次 cso 装载。失败时 Fail 一次并带齐修复信息；
// 成功时 stdout 一行 [SHADER_GATE]（机器输出走 stdout，日志纪律）。
// 返回 false = 门禁未过（调用方应中止装载）。
inline const char* kGatePrefix = "EngineTest/shaders/D3D12/";

inline bool GateD3D12Cso(const char* rel, const std::vector<u8>& bytes,
                         const char* tag) {
    // manifest 惰性加载 + 进程内缓存（一次解析，静态局部）。
    static const Manifest manifest = [] {
        const std::vector<u8> mb =
            ReadFileRel("EngineTest/shaders/D3D12/manifest.json");
        if (mb.empty()) {
            Manifest bad;
            bad.error = "manifest.json unreadable (missing or empty)";
            return bad;
        }
        return ParseManifest(mb);
    }();
    const std::string path(rel);
    if (path.rfind(kGatePrefix, 0) != 0) {
        Fail(std::string("[SHADER_GATE] ") + tag +
             " unexpected path form (expected prefix " + kGatePrefix +
             "): " + path + " — gate cannot key into manifest");
        return false;
    }
    const std::string key = path.substr(std::strlen(kGatePrefix));
    if (!manifest.ok) {
        Fail(std::string("[SHADER_GATE] ") + tag +
             " manifest unusable: " + manifest.error +
             " (manifest.json is the single source of truth for cso sha256; "
             "sync the whole tree or rerun python3 scripts/build_d3d12_shaders.py "
             "and ship manifest+cso together)");
        return false;
    }
    const auto it = manifest.csoToSha.find(key);
    if (it == manifest.csoToSha.end()) {
        Fail(std::string("[SHADER_GATE] ") + tag + " manifest has no entry for " +
             key + " (loaded " + path +
             ") — stale harness vs newer manifest, or renamed shader; sync "
             "tree or rerun python3 scripts/build_d3d12_shaders.py");
        return false;
    }
    const std::string& want = it->second;
    if (bytes.empty()) {
        Fail(std::string("[SHADER_GATE] ") + tag + " cso missing on disk: " +
             path + " expected sha256(manifest)=" + want +
             " actual=missing — sync the whole tree or rerun python3 "
             "scripts/build_d3d12_shaders.py and ship manifest+cso together");
        return false;
    }
    const Sha256Digest d = Sha256(bytes.data(), bytes.size());
    const std::string got = ToHex32(d.h);
    if (got != want) {
        Fail(std::string("[SHADER_GATE] ") + tag + " cso sha256 MISMATCH: " +
             path + " expected(manifest)=" + want + " actual=" + got +
             " — stale/intermediate cso reached this tree (typical symptom: "
             "CreateGraphicsPipeline E_INVALIDARG 0x80070057 with no prior "
             "hint); sync the whole tree or rerun python3 "
             "scripts/build_d3d12_shaders.py, then ship manifest+cso together");
        return false;
    }
    std::cout << "[SHADER_GATE] " << path << " cso sha256 ok" << std::endl;
    return true;
}

}   // namespace sha_gate

// ======================= 泄漏计数账本（F1-12 sanity） =======================
// 只登记宿主自建/自毁的 RHI 资源；RenderMesh 内部顶点/索引缓冲经
// mesh->Destroy 释放（device 无句柄计数查询接口，记录在案为已知边界）。
struct ResourceLedger {
    std::unordered_set<ResourceHandle> buffers, textures, samplers;
    std::unordered_set<ShaderHandle> shaders;
    std::unordered_set<PipelineHandle> pipelines;
    std::unordered_set<PipelineLayoutHandle> layouts;
    std::unordered_set<DescriptorSetLayoutHandle> setLayouts;
    std::unordered_set<DescriptorSetHandle> sets;
    std::unordered_set<CommandBufferHandle> cmds;
    u32 meshCount{0};

    void Print(const char* tag) const {
        std::cout << "[RHI_SHARED] ledger(" << tag << ")"
                  << " buffers=" << buffers.size()
                  << " textures=" << textures.size()
                  << " samplers=" << samplers.size()
                  << " shaders=" << shaders.size()
                  << " pipelines=" << pipelines.size()
                  << " layouts=" << layouts.size()
                  << " setLayouts=" << setLayouts.size()
                  << " sets=" << sets.size()
                  << " cmds=" << cmds.size()
                  << " meshes=" << meshCount << std::endl;
    }
    bool AllEmpty() const {
        return buffers.empty() && textures.empty() && samplers.empty() &&
               shaders.empty() && pipelines.empty() && layouts.empty() &&
               setLayouts.empty() && sets.empty() && cmds.empty();
    }
} g_ledger;

// ======================= 轨迹数据（schema v1） =======================

struct Keyframe {
    double time{0.0};
    math::v3 position{0, 0, 0};
    math::v3 lookAt{0, 0, 0};
    double fovDeg{60.0};
};

// M4 F4-7 光照变化段：光照关键帧（步进求值——取 time<=t 的最后一条，
// 未覆盖字段继承上一条/基值；确定性：光参数仅由轨迹 config 驱动按帧切换）。
struct LightKeyframe {
    double time{0.0};
    bool hasIntensity{false};
    float intensity{0.0f};
    bool hasDir{false};
    math::v3 dirToLight{0, 0, 0};
    bool hasColor{false};
    math::v3 color{0, 0, 0};
};

struct Trajectory {
    u32 version{0};
    std::string name;
    std::string assetPath;
    double dt{1.0 / 60.0};
    u32 totalFrames{240};
    u32 warmupFrames{30};
    u32 width{1280}, height{720};
    std::vector<Keyframe> keyframes;
    bool staticCam{false};
    // 光照（M1 单方向光；缺省与轨迹文件一致）
    math::v3 dirToLight{0.25f, 0.9f, 0.35f};
    float lightIntensity{1.35f};
    math::v3 lightColor{1.0f, 0.98f, 0.92f};
    // M3：post 段曝光基值（render.post.exposure，默认 1.0；固定 EV——
    // 确定性纪律：无 adapt 时变项）
    float postExposure{1.0f};
    // M4 F4-7：光照关键帧（render.light.keyframes；缺省空 = 静态光，
    // M1-M3 行为不变）
    std::vector<LightKeyframe> lightKeys;
};

bool ParseTrajectory(const pfxjson::value& root, Trajectory& t, std::string& err) {
    const pfxjson::value* v = root.find("version");
    if (!v || !v->is_number() || u32(v->as_number()) != 1) {
        err = "trajectory version missing or != 1";
        return false;
    }
    t.version = 1;
    if (auto* n = root.find("name")) t.name = n->as_string();

    const pfxjson::value* scene = root.find("scene");
    if (!scene || !scene->is_object()) { err = "scene missing"; return false; }
    if (auto* a = scene->find("asset")) t.assetPath = a->as_string();
    if (t.assetPath.empty()) { err = "scene.asset empty"; return false; }

    const pfxjson::value* timing = root.find("timing");
    if (!timing || !timing->is_object()) { err = "timing missing"; return false; }
    const pfxjson::value* ts = timing->find("timestep");
    if (!ts || !ts->is_number() || ts->as_number() <= 0.0) {
        err = "timing.timestep missing or <= 0"; return false;
    }
    t.dt = ts->as_number();
    if (auto* f = timing->find("total_frames")) t.totalFrames = f->as_u32();
    if (auto* f = timing->find("warmup_frames")) t.warmupFrames = f->as_u32();
    if (t.totalFrames == 0) { err = "timing.total_frames == 0"; return false; }

    const pfxjson::value* render = root.find("render");
    if (render && render->is_object()) {
        if (auto* res = render->find("resolution")) {
            if (res->is_array() && res->array_size() == 2) {
                t.width = res->array_at(0)->as_u32();
                t.height = res->array_at(1)->as_u32();
            }
        }
        if (auto* light = render->find("light")) {
            if (auto* d = light->find("dir_to_light"); d && d->array_size() == 3) {
                t.dirToLight = math::v3{d->array_at(0)->as_f32(),
                                        d->array_at(1)->as_f32(),
                                        d->array_at(2)->as_f32()};
            }
            if (auto* i = light->find("intensity")) t.lightIntensity = i->as_f32();
            if (auto* c = light->find("color"); c && c->array_size() == 3) {
                t.lightColor = math::v3{c->array_at(0)->as_f32(),
                                        c->array_at(1)->as_f32(),
                                        c->array_at(2)->as_f32()};
            }
            // M4 F4-7：光照关键帧（步进档；字段缺省=继承，见 EvalLight）。
            if (auto* kfs = light->find("keyframes");
                kfs && kfs->is_array() && kfs->array_size() > 0) {
                double lastTime = -1e30;
                for (size_t i = 0; i < kfs->array_size(); ++i) {
                    const pfxjson::value* k = kfs->array_at(i);
                    if (!k || !k->is_object()) {
                        err = "light.keyframes entry not an object";
                        return false;
                    }
                    LightKeyframe lk;
                    if (auto* tt = k->find("time")) lk.time = tt->as_number();
                    if (lk.time < lastTime) {
                        err = "light.keyframes not sorted by time";
                        return false;
                    }
                    lastTime = lk.time;
                    if (auto* v = k->find("intensity")) {
                        lk.hasIntensity = true; lk.intensity = v->as_f32();
                    }
                    if (auto* d = k->find("dir_to_light");
                        d && d->array_size() == 3) {
                        lk.hasDir = true;
                        lk.dirToLight = math::v3{d->array_at(0)->as_f32(),
                                                 d->array_at(1)->as_f32(),
                                                 d->array_at(2)->as_f32()};
                    }
                    if (auto* c2 = k->find("color");
                        c2 && c2->array_size() == 3) {
                        lk.hasColor = true;
                        lk.color = math::v3{c2->array_at(0)->as_f32(),
                                            c2->array_at(1)->as_f32(),
                                            c2->array_at(2)->as_f32()};
                    }
                    t.lightKeys.push_back(lk);
                }
            }
        }
        // M3：post 曝光基值（render.post.exposure；缺省 1.0）
        if (auto* post = render->find("post")) {
            if (auto* e = post->find("exposure")) t.postExposure = e->as_f32();
        }
    }

    const pfxjson::value* cam = root.find("camera");
    if (!cam || !cam->is_object()) { err = "camera missing"; return false; }
    std::string interp = "linear";
    if (auto* ip = cam->find("interpolation")) interp = ip->as_string();
    t.staticCam = (interp == "static");
    const pfxjson::value* kfs = cam->find("keyframes");
    if (!kfs || !kfs->is_array() || kfs->array_size() == 0) {
        err = "camera.keyframes empty";
        return false;
    }
    double lastTime = -1e30;
    for (size_t i = 0; i < kfs->array_size(); ++i) {
        const pfxjson::value* k = kfs->array_at(i);
        Keyframe kf;
        if (auto* tt = k->find("time")) kf.time = tt->as_number();
        if (kf.time < lastTime) { err = "keyframes not sorted by time"; return false; }
        lastTime = kf.time;
        if (auto* p = k->find("position"); p && p->array_size() == 3) {
            kf.position = math::v3{p->array_at(0)->as_f32(),
                                   p->array_at(1)->as_f32(),
                                   p->array_at(2)->as_f32()};
        }
        if (auto* p = k->find("lookAt"); p && p->array_size() == 3) {
            kf.lookAt = math::v3{p->array_at(0)->as_f32(),
                                 p->array_at(1)->as_f32(),
                                 p->array_at(2)->as_f32()};
        }
        if (auto* f = k->find("fov_deg")) kf.fovDeg = f->as_number();
        t.keyframes.push_back(kf);
    }
    return true;
}

// 最小相机求值：linear = 位置/lookAt/fov 线性插值（schema v1 §2.1）；
// static = 恒 keyframes[0]；t 超出首尾关键帧时刻时钳制到端点。
Keyframe EvalCamera(const Trajectory& t, double time) {
    const auto& kfs = t.keyframes;
    if (t.staticCam || kfs.size() == 1 || time <= kfs.front().time)
        return kfs.front();
    if (time >= kfs.back().time) return kfs.back();
    for (size_t i = 0; i + 1 < kfs.size(); ++i) {
        if (time >= kfs[i].time && time <= kfs[i + 1].time) {
            const double span = kfs[i + 1].time - kfs[i].time;
            const double u = span > 0.0 ? (time - kfs[i].time) / span : 0.0;
            Keyframe r;
            r.time = time;
            r.position = kfs[i].position +
                         (kfs[i + 1].position - kfs[i].position) * float(u);
            r.lookAt = kfs[i].lookAt +
                       (kfs[i + 1].lookAt - kfs[i].lookAt) * float(u);
            r.fovDeg = kfs[i].fovDeg + (kfs[i + 1].fovDeg - kfs[i].fovDeg) * u;
            return r;
        }
    }
    return kfs.back();
}

// M4 F4-7：光照步进求值——基值（render.light 静态字段）起步，依次套用
// time<=t 的关键帧（字段级覆盖，未覆盖字段继承）；t 早于首条=基值。
// 步进语义（非插值）：两档切换即帧间跳变，确定性可逐 SHA 复验。
struct LightState {
    math::v3 dirToLight;
    float intensity;
    math::v3 color;
};
LightState EvalLight(const Trajectory& t, double time) {
    LightState s{t.dirToLight, t.lightIntensity, t.lightColor};
    for (const auto& k : t.lightKeys) {
        if (k.time > time) break;
        if (k.hasDir) s.dirToLight = k.dirToLight;
        if (k.hasIntensity) s.intensity = k.intensity;
        if (k.hasColor) s.color = k.color;
    }
    return s;
}

// ======================= GPU 数据布局（与 M1Forward.* 一源） =======================

struct ViewDataGPU {
    math::m4x4 viewProjection;   // set0 b0 @0
    math::v4 cameraPos;          // @64
};
static_assert(sizeof(ViewDataGPU) == 80, "ViewData std140 = 64 + 16");

struct LightDataGPU {
    math::v4 dirAndIntensity;    // set0 b1 @0（xyz=表面指向光源，w=强度）
    math::v4 color;              // @16
};
static_assert(sizeof(LightDataGPU) == 32, "LightData std140 = 16 + 16");

struct M1PushConsts {
    math::m4x4 model;            // @0（PCGPushConsts 80B 惯例）
    math::v4 baseColor;          // @64
};
static_assert(sizeof(M1PushConsts) == 80, "push constant = 64 + 16");

// ---- M2 F2-5/F2-6 阴影段 GPU 布局（与 M2ShadowDepth/M2ForwardLit 一源）----
struct ShadowDataGPU {           // set0 b2（lit）/ b0（depth pass）
    math::m4x4 lightViewProjection;  // @0
    math::v4 shadowParams;           // @64：x=texelWorld y=depthSpan z=texelUV w=size
};
static_assert(sizeof(ShadowDataGPU) == 80, "ShadowData std140 = 64 + 16");

struct M2ShadowPush {            // depth pass push constant（仅 model）
    math::m4x4 model;            // @0
};
static_assert(sizeof(M2ShadowPush) == 64, "M2 depth push = 64");

// ---- M3 F3-2/F3-3 post 段 GPU 布局（与 M3Post.frag 一源）----
// 曲线系数从生产盘点逐字抄录（ToneMapping.wgsl ACESFilm 常数）：
// a=2.51 b=0.03 c=2.43 d=0.59 e=0.14（生产 tonemap_fs 同值经 UBO 参数化）。
// exposure = 轨迹 render.post.exposure（默认 1.0）× 2^RHI_POST_EV——
// 生产 ToneExposureEV 同式（基值 × 2^EV）；无 adapt 项（眼适应=历史
// 依赖，不选入共同最小链，报告登记处置）。
struct PostParamsGPU {           // set0 b0（post pass）
    float exposure;              // @0
    float acesA;                 // @4
    float acesB;                 // @8
    float acesC;                 // @12
    float acesD;                 // @16
    float acesE;                 // @20
    float pad0;                  // @24
    float pad1;                  // @28
};
static_assert(sizeof(PostParamsGPU) == 32, "PostParams std140 = 32");

// ---- M4 F4-3/F4-4 deferred 段 GPU 布局（与 M4DeferredLight.frag 一源）----
// DeferredViewData：延迟光照 set0 b0（深度重建用逆矩阵 + 相机位）。
struct DeferredViewGPU {
    math::m4x4 invViewProjection;  // @0（主相机 VP 之逆，宿主逐帧算）
    math::v4 cameraPos;            // @64
};
static_assert(sizeof(DeferredViewGPU) == 80, "DeferredView std140 = 64 + 16");

// LightShadowData：延迟光照 set0 b1 合并 UBO = LightData(32B) + ShadowData(80B)
// 逐字段拼接（M4DeferredLight.frag LightShadowData 同构；std140 偏移：
// dirAndIntensity@0 / color@16 / lightViewProjection@32 / shadowParams@96）。
struct LightShadowDataGPU {
    math::v4 dirAndIntensity;       // @0（LightData 前半，同 M1）
    math::v4 color;                 // @16
    math::m4x4 lightViewProjection; // @32（ShadowData 后半，同 M2）
    math::v4 shadowParams;          // @96
};
static_assert(sizeof(LightShadowDataGPU) == 112, "LightShadowData = 32 + 80");

// M2 单级联 shadow ortho 参数（T1 盘点对齐记录见交付报告）：
// 覆盖 Sponza 场景包围盒的保守盒（中心/半宽/光照侧退距/近平面/远平面）。
constexpr float kShadowCenter[3] = {0.0f, 5.0f, 10.0f};
constexpr float kShadowHalfExtent = 40.0f;    // 光空间 XY 半宽（世界单位）
constexpr float kShadowEyeDistance = 80.0f;   // 沿 +L（光源侧）退到 eye
constexpr float kShadowNear = 20.0f, kShadowFar = 150.0f;
constexpr u32 kShadowMapSize = 2048;

// ======================= 逐 mesh 资源 =======================

struct MeshEntry {
    RenderMesh* mesh{nullptr};
    ResourceHandle albedoTex{handles::INVALID_RESOURCE};
    DescriptorSetHandle materialSet{handles::INVALID_DESCRIPTOR_SET};
    DescriptorSetHandle litMaterialSet{handles::INVALID_DESCRIPTOR_SET};  // M2
};

// 纹理路径解析（沿 TestVulkanSponzaRenderGraph::ResolveTexturePath 语义）。
std::string ResolveTexturePath(const std::string& base, const std::string& filename) {
    if (filename.empty()) return "";
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    };
    std::string path = base + filename;
    if (exists(path)) return path;
    path = base + "models/Sponza/" + filename;
    if (exists(path)) return path;
    path = base + "textures/" + filename;
    if (exists(path)) return path;
    return "";
}

// stbi 解码 → staging 上传 → CopyBufferToTexture（+mips）→ ShaderResource。
// 沿 CreateTextureFromData 模式。
ResourceHandle CreateTextureFromPixels(RHIDeviceBase* device, RHICommandBuffer* cmd,
                                       const unsigned char* data, u32 w, u32 h) {
    u32 mipLevels = 1;
    { u32 maxDim = std::max(w, h); while (maxDim > 1) { mipLevels++; maxDim /= 2; } }
    TextureDesc desc{};
    desc.size = {w, h, 1};
    desc.format = DataFormat::RGBA8_UNorm;
    desc.type = TextureType::Texture2D;
    desc.mipLevels = mipLevels;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    desc.memoryUsage = GPUMemoryUsage::Static;
    desc.name = "M1Albedo";
    ResourceHandle tex = device->CreateTexture(desc);
    if (tex == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;
    g_ledger.textures.insert(tex);

    const u64 kBytes = u64(w) * h * 4;
    BufferDesc sdesc{};
    sdesc.size = kBytes;
    sdesc.type = BufferType::Raw;
    sdesc.memoryUsage = GPUMemoryUsage::Dynamic;
    sdesc.name = "M1TexStaging";
    ResourceHandle staging = device->CreateBuffer(sdesc);
    if (staging == handles::INVALID_RESOURCE ||
        !device->UpdateBufferData(staging, data, kBytes, 0) ||
        !cmd->Begin()) {
        if (staging != handles::INVALID_RESOURCE) {
            device->DestroyBuffer(staging);
            g_ledger.buffers.erase(staging);
        }
        return handles::INVALID_RESOURCE;
    }
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    cmd->CopyBufferToTexture(staging, tex, &region, 1);
    if (mipLevels > 1) cmd->GenerateMipmaps(tex);
    ResourceBarrier toShaderRead{};
    toShaderRead.resource = tex;
    toShaderRead.beforeState = ResourceState::CopyDest;
    toShaderRead.afterState = ResourceState::ShaderResource;
    toShaderRead.subresource = 0xFFFFFFFF;
    toShaderRead.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&toShaderRead, 1);
    cmd->End();
    cmd->Submit(0);
    cmd->WaitForCompletion();
    device->DestroyBuffer(staging);
    g_ledger.buffers.erase(staging);
    return tex;
}

// PPM 写出：BGRA8 → P6 RGB，无 Y 翻转（F2 后 RT 本身正立：投影 Y-down
// clip + 两端视口 NDC y=-1→RT 第 0 行；读回链路两端保序）。
bool WritePPM(const std::string& path, const u8* bgra, u32 w, u32 h) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << " " << h << "\n255\n";
    for (u32 i = 0; i < w * h; ++i) {
        const u8* p = bgra + size_t(i) * 4;
        f.write(reinterpret_cast<const char*>(&p[2]), 1);
        f.write(reinterpret_cast<const char*>(&p[1]), 1);
        f.write(reinterpret_cast<const char*>(&p[0]), 1);
    }
    return bool(f);
}

// ======================= 帧目标（离屏 RT + 深度 + readback） =======================

struct FrameTargets {
    ResourceHandle color{handles::INVALID_RESOURCE};
    ResourceHandle depth{handles::INVALID_RESOURCE};
    ResourceHandle readback{handles::INVALID_RESOURCE};
    ResourceHandle hdr{handles::INVALID_RESOURCE};   // M3：RGBA16F 中间（post on）
    // M4：GBuffer 三 RT（deferred on）；depth 复用 ft.depth（deferred 时
    // 额外带 ShaderResource usage 供延迟光照采样）。
    ResourceHandle gAlbedo{handles::INVALID_RESOURCE};
    ResourceHandle gNormal{handles::INVALID_RESOURCE};
    ResourceHandle gMaterial{handles::INVALID_RESOURCE};
    // M6（GI on 才建）：P2 直射 MRT 两 RT + P5 独立间接 RT（RGBA16F 线性
    // HDR；RT 写 → SRV 采样 → 导出拷贝源）。GI off 时保持无效句柄——
    // 默认路径资源集不变。
    ResourceHandle giDirect{handles::INVALID_RESOURCE};
    ResourceHandle giDirectDiffuse{handles::INVALID_RESOURCE};
    ResourceHandle giIndirect{handles::INVALID_RESOURCE};
    // M6（GI on）：逐帧深度读回 → 几何覆盖率（深度<1 像素占比）。GI 模式黑帧
    // 守卫用：直射-only 语义下完全阴影几何恰为黑，"清屏黑→非黑比例即几何
    // 覆盖"前提失效，守卫改测几何覆盖（颜色非黑比例照常统计输出）。
    ResourceHandle giCoverageReadback{handles::INVALID_RESOURCE};
    u32 giCoveragePitch{0};   // 字节行距（align(W*4, 256)）
    u32 w{0}, h{0};
};

bool CreateFrameTargets(RHIDeviceBase* device, FrameTargets& ft, u32 w, u32 h,
                        bool wantPost, bool wantDeferred, bool wantGI) {
    ft.w = w; ft.h = h;
    TextureDesc colorDesc{};
    colorDesc.size = {w, h, 1};
    colorDesc.format = DataFormat::BGRA8_UNorm;   // M1 §7.2-3：离屏直读直写
    colorDesc.type = TextureType::Texture2D;
    colorDesc.mipLevels = 1;
    colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    colorDesc.memoryUsage = GPUMemoryUsage::Static;
    colorDesc.name = "M1Color";
    ft.color = device->CreateTexture(colorDesc);
    if (ft.color == handles::INVALID_RESOURCE) return false;
    g_ledger.textures.insert(ft.color);

    // M3 F3-2：HDR 中间（RGBA16F，无 sRGB 语义——half float 线性域）。
    // lit pass RT + post pass 采样源；resize 重建同路。
    if (wantPost) {
        TextureDesc hdrDesc = colorDesc;
        hdrDesc.format = DataFormat::RGBA16_Float;
        hdrDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        hdrDesc.name = "M3Hdr";
        ft.hdr = device->CreateTexture(hdrDesc);
        if (ft.hdr == handles::INVALID_RESOURCE) return false;
        g_ledger.textures.insert(ft.hdr);
    }

    // M4 F4-2：GBuffer 三 RT（MRT；延迟光照采样源，resize 重建同路）。
    // RT1 RG16F（世界法线 xy 直存）= RG16_Float；RT0/RT2 RGBA8（注意不可
    // 沿用 colorDesc 的 BGRA8——初版即栽在此：格式与管线/desc 声明不符，
    // validation 00880 抓获）。
    if (wantDeferred) {
        TextureDesc gDesc = colorDesc;
        gDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        gDesc.format = DataFormat::RGBA8_UNorm;
        gDesc.name = "M4GAlbedo";
        ft.gAlbedo = device->CreateTexture(gDesc);
        gDesc.format = DataFormat::RG16_Float;
        gDesc.name = "M4GNormal";
        ft.gNormal = device->CreateTexture(gDesc);
        gDesc.format = DataFormat::RGBA8_UNorm;
        gDesc.name = "M4GMaterial";
        ft.gMaterial = device->CreateTexture(gDesc);
        if (ft.gAlbedo == handles::INVALID_RESOURCE ||
            ft.gNormal == handles::INVALID_RESOURCE ||
            ft.gMaterial == handles::INVALID_RESOURCE) return false;
        g_ledger.textures.insert(ft.gAlbedo);
        g_ledger.textures.insert(ft.gNormal);
        g_ledger.textures.insert(ft.gMaterial);
    }

    // M6 GI（spec §6.1）：直射/直射漫反射/独立间接三 RT（RGBA16F；resize 同路
    // 重建）。仅 GI on 创建——GI off 路径资源形状不变纪律（同 M4 depth usage）。
    if (wantGI) {
        TextureDesc giDesc = colorDesc;
        giDesc.format = DataFormat::RGBA16_Float;
        giDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource |
                       TextureUsage::CopySource;
        giDesc.name = "M6GIDirect";
        ft.giDirect = device->CreateTexture(giDesc);
        giDesc.name = "M6GIDirectDiffuse";
        ft.giDirectDiffuse = device->CreateTexture(giDesc);
        giDesc.name = "M6GIIndirect";
        ft.giIndirect = device->CreateTexture(giDesc);
        if (ft.giDirect == handles::INVALID_RESOURCE ||
            ft.giDirectDiffuse == handles::INVALID_RESOURCE ||
            ft.giIndirect == handles::INVALID_RESOURCE) return false;
        g_ledger.textures.insert(ft.giDirect);
        g_ledger.textures.insert(ft.giDirectDiffuse);
        g_ledger.textures.insert(ft.giIndirect);
        // 几何覆盖率读回缓冲（D32 4B/px，行距 256 对齐——两端同布局）
        BufferDesc cbd{};
        ft.giCoveragePitch = (w * 4u + 255u) & ~255u;
        cbd.size = u64(ft.giCoveragePitch) * h;
        cbd.type = BufferType::Raw;
        // R19-1：同 M1Readback——CopyTextureToBuffer 的 GPU 写目标须 Readback 位。
        cbd.usage = GPUMemoryUsage::Readback;
        cbd.memoryUsage = GPUMemoryUsage::Readback;
        cbd.name = "M6GICoverageReadback";
        ft.giCoverageReadback = device->CreateBuffer(cbd);
        if (ft.giCoverageReadback == handles::INVALID_RESOURCE) return false;
        g_ledger.buffers.insert(ft.giCoverageReadback);
    }

    TextureDesc depthDesc = colorDesc;
    depthDesc.format = DataFormat::D32_Float;     // M1 §7.2-2：[0,1] 正向 Z
    depthDesc.usage = TextureUsage::DepthStencil;
    // M4：延迟光照采深度重建世界位 → 深度纹理需可采样（同 M2 shadow map
    // 用法；仅 deferred 开启时加 usage——默认路径资源形状不变纪律）。
    if (wantDeferred)
        depthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    depthDesc.name = "M1Depth";
    ft.depth = device->CreateTexture(depthDesc);
    if (ft.depth == handles::INVALID_RESOURCE) return false;
    g_ledger.textures.insert(ft.depth);

    BufferDesc rbDesc{};
    rbDesc.size = u64(w) * h * 4;
    rbDesc.type = BufferType::Raw;
    // R19-1：读回意图用 Readback 位（GPU 写 → CPU 读）。原 Staging 位在
    // D3D12 落 UPLOAD 堆（CPU 写/GPU 读），CopyTextureRegion 的 GPU 写目标
    // 必须在 READBACK 堆——十八测 M1/M2 退出 #849 根因。Vulkan/Metal 侧
    // Readback 与 Staging 同落 host-visible，行为等价（零回归见 runbook §28-1）。
    rbDesc.usage = GPUMemoryUsage::Readback;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "M1Readback";
    ft.readback = device->CreateBuffer(rbDesc);
    if (ft.readback == handles::INVALID_RESOURCE) return false;
    g_ledger.buffers.insert(ft.readback);
    return true;
}

void DestroyFrameTargets(RHIDeviceBase* device, FrameTargets& ft) {
    if (ft.readback != handles::INVALID_RESOURCE) {
        device->DestroyBuffer(ft.readback); g_ledger.buffers.erase(ft.readback);
    }
    // M6 GI 三 RT + 覆盖率读回（GI off 时为无效句柄，跳过）
    if (ft.giCoverageReadback != handles::INVALID_RESOURCE) {
        device->DestroyBuffer(ft.giCoverageReadback);
        g_ledger.buffers.erase(ft.giCoverageReadback);
    }
    for (ResourceHandle h : {ft.giIndirect, ft.giDirectDiffuse, ft.giDirect}) {
        if (h != handles::INVALID_RESOURCE) {
            device->DestroyTexture(h); g_ledger.textures.erase(h);
        }
    }
    if (ft.gMaterial != handles::INVALID_RESOURCE) {
        device->DestroyTexture(ft.gMaterial); g_ledger.textures.erase(ft.gMaterial);
    }
    if (ft.gNormal != handles::INVALID_RESOURCE) {
        device->DestroyTexture(ft.gNormal); g_ledger.textures.erase(ft.gNormal);
    }
    if (ft.gAlbedo != handles::INVALID_RESOURCE) {
        device->DestroyTexture(ft.gAlbedo); g_ledger.textures.erase(ft.gAlbedo);
    }
    if (ft.hdr != handles::INVALID_RESOURCE) {
        device->DestroyTexture(ft.hdr); g_ledger.textures.erase(ft.hdr);
    }
    if (ft.depth != handles::INVALID_RESOURCE) {
        device->DestroyTexture(ft.depth); g_ledger.textures.erase(ft.depth);
    }
    if (ft.color != handles::INVALID_RESOURCE) {
        device->DestroyTexture(ft.color); g_ledger.textures.erase(ft.color);
    }
    ft = FrameTargets{};
}

// ======================= M2 阴影段资源（F2-2/F2-3/F2-6） =======================

// 单级联 shadow map + 比较采样器 + depth-only/lit 两管线与布局。
// 比较采样器（M2ShadowSampler）：SamplerDesc::comparisonFunc 的 RHI 默认值
// 是 ComparisonFunc::Always（RHITypes.h:1058）——普通采样用途下是坑值
// （Vulkan 侧 != Never 即 compareEnable=TRUE）。本宿主显式传 LessEqual
// （生产比较语义 sc.z-bias <= d → lit）；RHI_SHADOW_LIT_ALL=1 消融档则
// 故意传 Always → 采样恒 1（无阴影对照帧，供阴影可见性 A/B）。
struct M2ShadowState {
    bool ablationLitAll{false};
    // 资源
    ResourceHandle shadowMap{handles::INVALID_RESOURCE};
    ResourceHandle shadowUbo{handles::INVALID_RESOURCE};
    SamplerHandle shadowSampler{handles::INVALID_SAMPLER};
    // depth pass
    ShaderHandle depthVs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle depthSetLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle depthLayout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle depthPipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle depthSet{handles::INVALID_DESCRIPTOR_SET};
    // lit pass（3 UBO 全局 + 4 绑定材质）
    ShaderHandle litFs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle litGlobalLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    DescriptorSetLayoutHandle litMaterialLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle litLayout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle litPipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle litGlobalSet{handles::INVALID_DESCRIPTOR_SET};
    // 每帧常数
    math::m4x4 lightVP{};
    ShadowDataGPU shadowData{};
};

// 全局光照方向（M2ShadowInit 帧常数计算用；main 里装载轨迹后写入）。
math::v3 g_trajDirToLight{0.25f, 0.9f, 0.35f};

bool M2ShadowInit(RHIDeviceBase* device, M2ShadowState& s, bool ablationLitAll,
                  std::string& err) {
    s.ablationLitAll = ablationLitAll;
    // ---- shadow map 纹理（D32 2048²，可采样）----
    TextureDesc smDesc{};
    smDesc.size = {kShadowMapSize, kShadowMapSize, 1};
    smDesc.format = DataFormat::D32_Float;
    smDesc.type = TextureType::Texture2D;
    smDesc.mipLevels = 1;
    smDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    smDesc.memoryUsage = GPUMemoryUsage::Static;
    smDesc.name = "M2ShadowMap";
    s.shadowMap = device->CreateTexture(smDesc);
    if (s.shadowMap == handles::INVALID_RESOURCE) { err = "shadow map texture"; return false; }
    g_ledger.textures.insert(s.shadowMap);

    // ---- 光照 VP / shadowData（静态光 + 静态场景 → 帧常数）----
    // 近似生产 ComputeCascadeVP 的结构（中心对齐 + 从光源侧退距观察），
    // 单级联取保守盒覆盖场景（生产第 4 级 extent=270 的角色）。
    // 注意方向约定：生产 lightDir=光行进方向故 lightEye=-lightDir*dist；
    // 此处 L 是 dir_to_light（表面指向光源，与 LightData 同一语义），
    // 光相机必须在 +L 侧（=中心+L*dist）朝 -L 看，Less 深度测试保留的才是
    // 离光最近的遮挡面。旧写法 eye=center-L*dist 把相机放在反光侧，
    // 深度图记录的是离光最远的背面（Sponza 外壳盒），接收端 ref 恒大于
    // stored → SampleCmp 恒 0 → M2 只有 ambient（2026-11-07 五探针定位）。
    math::v3 L = rhimath::Normalize(math::v3{g_trajDirToLight.x, g_trajDirToLight.y, g_trajDirToLight.z});
    math::v3 center{kShadowCenter[0], kShadowCenter[1], kShadowCenter[2]};
    math::v3 eye = center + L * kShadowEyeDistance;
    math::v3 up = (std::abs(L.y) < 0.99f) ? math::v3{0.0f, 1.0f, 0.0f}
                                          : math::v3{1.0f, 0.0f, 0.0f};
    math::m4x4 lightView = rhimath::CreateLookAtMatrix(eye, center, up);
    math::m4x4 lightProj = rhimath::CreateOrthographicMatrix(
        -kShadowHalfExtent, kShadowHalfExtent, -kShadowHalfExtent, kShadowHalfExtent,
        kShadowNear, kShadowFar);
    s.lightVP = lightProj * lightView;
    s.shadowData.lightViewProjection = s.lightVP;
    s.shadowData.shadowParams = math::v4{
        (kShadowHalfExtent * 2.0f) / float(kShadowMapSize),   // x = texelWorld
        kShadowFar - kShadowNear,                              // y = depthSpan
        1.0f / float(kShadowMapSize),                          // z = texelUV
        float(kShadowMapSize)};                                // w = size（未用）

    BufferDesc sbd{};
    sbd.size = sizeof(ShadowDataGPU);
    sbd.type = BufferType::Constant;
    sbd.memoryUsage = GPUMemoryUsage::Dynamic;
    sbd.name = "M2ShadowData";
    s.shadowUbo = device->CreateBuffer(sbd);
    if (s.shadowUbo == handles::INVALID_RESOURCE) { err = "shadow UBO"; return false; }
    g_ledger.buffers.insert(s.shadowUbo);
    if (!device->UpdateBufferData(s.shadowUbo, &s.shadowData, sizeof(s.shadowData), 0)) {
        err = "shadow UBO upload"; return false;
    }

    // ---- M2ShadowSampler：比较采样器（LessEqual；消融档 Always）----
    SamplerDesc ssd{};
    ssd.minFilter = FilterMode::Linear;
    ssd.magFilter = FilterMode::Linear;
    ssd.mipFilter = FilterMode::Linear;
    ssd.addressU = TextureAddressMode::Clamp;   // 生产 shadow 采样 Clamp
    ssd.addressV = TextureAddressMode::Clamp;
    ssd.addressW = TextureAddressMode::Clamp;
    ssd.maxAnisotropy = 1;
    ssd.comparisonFunc = ablationLitAll ? ComparisonFunc::Always    // 消融：恒 lit
                                        : ComparisonFunc::LessEqual; // 生产比较语义
    s.shadowSampler = device->CreateSampler(ssd);
    if (s.shadowSampler == handles::INVALID_SAMPLER) { err = "shadow comparison sampler"; return false; }
    g_ledger.samplers.insert(s.shadowSampler);

    // ---- 比较采样器创建事实回显（F2-3 证据：comparisonFunc 显式传递）----
    std::cout << "[RHI_SHARED] M2ShadowSampler: comparisonFunc="
              << (ablationLitAll ? "Always(ablation)" : "LessEqual")
              << " (RHI default would be Always — explicit override required)"
              << std::endl;
    return true;
}

void M2ShadowDestroy(RHIDeviceBase* device, M2ShadowState& s) {
    if (s.litGlobalSet != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.litGlobalSet); g_ledger.sets.erase(s.litGlobalSet); }
    if (s.litPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.litPipeline); g_ledger.pipelines.erase(s.litPipeline); }
    if (s.litLayout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.litLayout); g_ledger.layouts.erase(s.litLayout); }
    if (s.litMaterialLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.litMaterialLayout); g_ledger.setLayouts.erase(s.litMaterialLayout); }
    if (s.litGlobalLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.litGlobalLayout); g_ledger.setLayouts.erase(s.litGlobalLayout); }
    if (s.litFs != handles::INVALID_SHADER) { device->DestroyShader(s.litFs); g_ledger.shaders.erase(s.litFs); }
    if (s.depthSet != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.depthSet); g_ledger.sets.erase(s.depthSet); }
    if (s.depthPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.depthPipeline); g_ledger.pipelines.erase(s.depthPipeline); }
    if (s.depthLayout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.depthLayout); g_ledger.layouts.erase(s.depthLayout); }
    if (s.depthSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.depthSetLayout); g_ledger.setLayouts.erase(s.depthSetLayout); }
    if (s.depthVs != handles::INVALID_SHADER) { device->DestroyShader(s.depthVs); g_ledger.shaders.erase(s.depthVs); }
    if (s.shadowSampler != handles::INVALID_SAMPLER) { device->DestroySampler(s.shadowSampler); g_ledger.samplers.erase(s.shadowSampler); }
    if (s.shadowUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.shadowUbo); g_ledger.buffers.erase(s.shadowUbo); }
    if (s.shadowMap != handles::INVALID_RESOURCE) { device->DestroyTexture(s.shadowMap); g_ledger.textures.erase(s.shadowMap); }
    s = M2ShadowState{};
}

// ======================= M3 post 段资源（F3-2/F3-3） =======================

// 共同最小 post 链（RHI_TEST_POST=1 门控，默认 0 = M1/M2 行为不变）：
//   lit pass → RGBA16F HDR 中间 RT（管线 RT 格式数组改 RGBA16_Float）
//   → post 全屏三角形 pass（M3Post.vert+frag：固定 EV 曝光 × 生产同源
//   ACES 拟合）→ BGRA8 UNorm 直写（唯一编码点；无 sRGB 视图、无 shader
//   pow——M1 §7.2-3 契约，生产 pow(1/2.2) 为其交换链侧编码点，共同链
//   编码推迟 M7，选型偏差登记于 M3 交付报告）。
// 全屏 pass = 三角形 VS+PS 路径（Blit/GenerateMipmaps NOT-IMPLEMENTED，
// post 链不依赖）；PSO 无深度、无 cull、无顶点输入（vertex-ID 驱动）。
struct M3PostState {
    ShaderHandle vs{handles::INVALID_SHADER};
    ShaderHandle fs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle setLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle layout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle pipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle set{handles::INVALID_DESCRIPTOR_SET};
    SamplerHandle sampler{handles::INVALID_SAMPLER};
    ResourceHandle paramsUbo{handles::INVALID_RESOURCE};
};

bool M3PostInit(RHIDeviceBase* device, M3PostState& s, bool wantD3D12,
                float baseExposure, float evOverride, std::string& err) {
    // shader 装载（platform 决定字节码；与 M1/M2 同装载惯例）
    const char* vertRel = wantD3D12
        ? "EngineTest/shaders/D3D12/Forward/M3Post.vert.cso"
        : "Engine/Graphics/Vulkan/shaders/Forward/M3Post.vert.spv";
    const char* fragRel = wantD3D12
        ? "EngineTest/shaders/D3D12/Forward/M3Post.frag.cso"
        : "Engine/Graphics/Vulkan/shaders/Forward/M3Post.frag.spv";
    std::vector<u8> vb = ReadFileRel(vertRel), fb = ReadFileRel(fragRel);
    if (vb.empty() || fb.empty()) {
        err = std::string("M3Post shader bytes missing (") + vertRel + " / " +
              fragRel + ")";
        return false;
    }
    s.vs = device->CreateShader(vb.data(), vb.size(), ShaderStage::Vertex, "main");
    s.fs = device->CreateShader(fb.data(), fb.size(), ShaderStage::Pixel, "main");
    if (s.vs == handles::INVALID_SHADER || s.fs == handles::INVALID_SHADER) {
        err = "M3Post CreateShader";
        return false;
    }
    g_ledger.shaders.insert(s.vs);
    g_ledger.shaders.insert(s.fs);

    // 布局：set0 b0 PostParams UBO + b1 HDR image + b2 sampler（反射契约
    // 一源；无 push constant）
    DescriptorSetLayoutBinding pb[3];
    pb[0] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};
    pb[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
    pb[2] = {2, DescriptorType::Sampler, 1, ShaderStage::Pixel};
    DescriptorSetLayoutDesc pld{};
    pld.bindingCount = 3;
    pld.bindings = pb;
    s.setLayout = device->CreateDescriptorSetLayout(pld);
    PipelineLayoutDesc plpd{};
    plpd.setLayoutCount = 1;
    plpd.setLayouts = &s.setLayout;
    s.layout = device->CreatePipelineLayout(plpd);
    if (s.setLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        s.layout == handles::INVALID_PIPELINE_LAYOUT) {
        err = "M3Post layouts";
        return false;
    }
    g_ledger.setLayouts.insert(s.setLayout);
    g_ledger.layouts.insert(s.layout);

    // 采样器：Linear/Clamp（生产 ToneMapping 采样档）；MoltenVK 移植性
    // comparisonFunc 必须 Never（宿主惯例注释）。
    SamplerDesc psd{};
    psd.minFilter = FilterMode::Linear;
    psd.magFilter = FilterMode::Linear;
    psd.mipFilter = FilterMode::Linear;
    psd.addressU = TextureAddressMode::Clamp;
    psd.addressV = TextureAddressMode::Clamp;
    psd.addressW = TextureAddressMode::Clamp;
    psd.maxAnisotropy = 1;
    psd.comparisonFunc = ComparisonFunc::Never;
    s.sampler = device->CreateSampler(psd);
    if (s.sampler == handles::INVALID_SAMPLER) { err = "M3Post sampler"; return false; }
    g_ledger.samplers.insert(s.sampler);

    // PostParams UBO（帧常数一次性上传；确定性——无 adapt/无时变项）
    BufferDesc ubd{};
    ubd.size = sizeof(PostParamsGPU);
    ubd.type = BufferType::Constant;
    ubd.memoryUsage = GPUMemoryUsage::Dynamic;
    ubd.name = "M3PostParams";
    s.paramsUbo = device->CreateBuffer(ubd);
    if (s.paramsUbo == handles::INVALID_RESOURCE) { err = "M3Post UBO"; return false; }
    g_ledger.buffers.insert(s.paramsUbo);
    PostParamsGPU pp{};
    pp.exposure = baseExposure * std::pow(2.0f, evOverride);  // 生产 base×2^EV 同式
    pp.acesA = 2.51f;   // 生产 ToneMapping.wgsl ACESFilm 常数（盘点抄录）
    pp.acesB = 0.03f;
    pp.acesC = 2.43f;
    pp.acesD = 0.59f;
    pp.acesE = 0.14f;
    pp.pad0 = 0.0f;
    pp.pad1 = 0.0f;
    if (!device->UpdateBufferData(s.paramsUbo, &pp, sizeof(pp), 0)) {
        err = "M3Post UBO upload";
        return false;
    }

    // post 管线：全屏三角形（无顶点输入/无深度/无 cull），RT=BGRA8 UNorm
    //（唯一编码点；读回可比）。
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = s.vs;
    gpd.pixelShader = s.fs;
    gpd.layout = s.layout;
    gpd.topology = PrimitiveTopology::TriangleList;  // RHI 拓扑直译
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;                   // 全屏三角形无需剔除
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    s.pipeline = device->CreateGraphicsPipeline(gpd);
    if (s.pipeline == handles::INVALID_PIPELINE) { err = "M3Post pipeline"; return false; }
    g_ledger.pipelines.insert(s.pipeline);

    // 描述符集（HDR 纹理句柄在帧目标创建后回填——见 BindM3PostSet）
    DescriptorSetDesc dsd{};
    dsd.layout = s.setLayout;
    s.set = device->CreateDescriptorSet(dsd);
    if (s.set == handles::INVALID_DESCRIPTOR_SET) { err = "M3Post set"; return false; }
    g_ledger.sets.insert(s.set);
    return true;
}

// HDR 中间纹理创建后回填描述符（resize 重建后须重绑）。
void BindM3PostSet(RHIDeviceBase* device, const M3PostState& s,
                   ResourceHandle hdrTex) {
    if (s.set == handles::INVALID_DESCRIPTOR_SET ||
        hdrTex == handles::INVALID_RESOURCE) return;
    DescriptorBufferInfo pbi{};
    pbi.buffer = s.paramsUbo; pbi.offset = 0; pbi.range = sizeof(PostParamsGPU);
    DescriptorImageInfo imgInfo{}, sampInfo{};
    imgInfo.imageView = hdrTex;
    imgInfo.imageLayout = ResourceState::ShaderResource;
    sampInfo.sampler = s.sampler;
    WriteDescriptorSet pw[3]{};
    pw[0].dstSet = s.set; pw[0].dstBinding = 0; pw[0].dstArrayElement = 0;
    pw[0].descriptorCount = 1; pw[0].descriptorType = DescriptorType::UniformBuffer;
    pw[0].bufferInfo = &pbi;
    pw[1].dstSet = s.set; pw[1].dstBinding = 1; pw[1].dstArrayElement = 0;
    pw[1].descriptorCount = 1; pw[1].descriptorType = DescriptorType::SampledImage;
    pw[1].imageInfo = &imgInfo;
    pw[2].dstSet = s.set; pw[2].dstBinding = 2; pw[2].dstArrayElement = 0;
    pw[2].descriptorCount = 1; pw[2].descriptorType = DescriptorType::Sampler;
    pw[2].imageInfo = &sampInfo;
    device->UpdateDescriptorSets(3, pw);
}

void M3PostDestroy(RHIDeviceBase* device, M3PostState& s) {
    if (s.set != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.set); g_ledger.sets.erase(s.set); }
    if (s.pipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.pipeline); g_ledger.pipelines.erase(s.pipeline); }
    if (s.paramsUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.paramsUbo); g_ledger.buffers.erase(s.paramsUbo); }
    if (s.sampler != handles::INVALID_SAMPLER) { device->DestroySampler(s.sampler); g_ledger.samplers.erase(s.sampler); }
    if (s.layout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.layout); g_ledger.layouts.erase(s.layout); }
    if (s.setLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.setLayout); g_ledger.setLayouts.erase(s.setLayout); }
    if (s.fs != handles::INVALID_SHADER) { device->DestroyShader(s.fs); g_ledger.shaders.erase(s.fs); }
    if (s.vs != handles::INVALID_SHADER) { device->DestroyShader(s.vs); g_ledger.shaders.erase(s.vs); }
    s = M3PostState{};
}

// ======================= M4 deferred 段资源（F4-2/F4-3/F4-4/F4-6）======

// 共同最小 deferred 链（RHI_TEST_DEFERRED=1 门控，默认 0 = M1/M2/M3 不变；
// 组合前置：RHI_TEST_SHADOWS=1——延迟光照绑定 shadow map/比较采样器）：
//   GBuffer pass：M4GBuffer.vert+frag，MRT×3（RGBA8/RG16F/RGBA8）+ D32，
//     set0/set1/push 全复用 M1 布局（宿主零新 GBuffer 描述符集）。
//   延迟光照 pass：M3Post.vert（全屏三角）+ M4DeferredLight.frag，set0
//     8 绑定（b0 DeferredView / b1 LightShadow 合并 / b2-b6 纹理 / b7 比较
//     采样器——反射契约冻结），无深度写、单 RT=litRTFormat（post on→
//     RGBA16F，off→BGRA8，与 forward lit 同目标同格式）。
struct M4DeferredState {
    // GBuffer pass（布局复用 RunHost 的 pipelineLayout/globalSet/materialSet）
    ShaderHandle gbVs{handles::INVALID_SHADER};
    ShaderHandle gbFs{handles::INVALID_SHADER};
    PipelineHandle gbPipeline{handles::INVALID_PIPELINE};
    // 延迟光照 pass
    ShaderHandle lightVs{handles::INVALID_SHADER};   // M3Post.vert 字节复用
    ShaderHandle lightFs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle lightSetLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle lightLayout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle lightPipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle lightSet{handles::INVALID_DESCRIPTOR_SET};
    ResourceHandle viewUbo{handles::INVALID_RESOURCE};       // 80B 逐帧
    ResourceHandle lightShadowUbo{handles::INVALID_RESOURCE}; // 112B 逐帧
};

constexpr u32 kM6GridDim = 8;                 // 8x8x8 探针网格（Nx=Ny=Nz）
constexpr u32 kM6ShSlabs = 3;                 // SH 纹理 z 向三 slab = R/G/B 通道
// GI-off 占位 GIParams（延迟光照 b10）的世界盒常数：仅 GI=0 的 M4 路径上传
//（giScale=0 不进入数值路径；常数保留 = 上传字节不变）。GI 模式探针盒由
// mesh AABB 并集派生（M6ComputeGridBox），不用此常数。
constexpr float kM6GridMin[3] = {-40.0f, -10.0f, -30.0f};
constexpr float kM6GridMax[3] = {40.0f, 30.0f, 50.0f};
constexpr u32 kM6ReadbackRowTexels = 32;      // 32 texel * 8B = 256B 行距
constexpr u32 kM6InjectStride = 8;            // 注入屏幕采样步长（像素，spec §5.2）
constexpr float kM6BoxPad = 0.5f;             // 探针盒各向外扩（米，spec §4）
constexpr double kM6ShY00 = 0.282095;         // SH 实基 Y00（各向同性均值换算）

// 与 M6ProbeAccum.comp AccumParams 一源（16B：flags.x bit0=reset）。
struct M6AccumParamsGPU {
    math::v4 flags;             // x=reset
};
static_assert(sizeof(M6AccumParamsGPU) == 16, "M6AccumParams std140 = 16");

// 与 M4DeferredLight.frag GIParams 一源（32B）——仅 GI-off 占位路径消费
//（GI 模式不跑 M4 light pass，b10 只保持指向存活资源）。
struct M6GIParamsGPU {
    math::v4 gridMinAndScale;   // xyz=网格世界盒 min w=giScale（0/1）
    math::v4 gridInvExtent;     // xyz=1/(max-min)
};
static_assert(sizeof(M6GIParamsGPU) == 32, "M6GIParams std140 = 32");

// 与 M6ProbeInject.comp InjectParams 一源（std140 144B，逐帧上传）。
struct M6InjectParamsGPU {
    math::m4x4 invViewProjection;  // @0   主相机 VP 之逆（= M4 同一 Inverse(proj*view)）
    math::v4 cameraPos;            // @64  xyz = 相机世界坐标
    math::v4 gridMin;              // @80  xyz = 探针盒 min
    math::v4 cellSize;             // @96  xyz = (max-min)/Nx
    math::v4 gridDims;             // @112 xyz = (Nx,Ny,Nz)（精确整数的浮点）
    math::v4 screen;               // @128 x=W y=H z=stride（精确整数的浮点）
};
static_assert(sizeof(M6InjectParamsGPU) == 144, "M6InjectParams std140 = 64 + 5*16");
static_assert(offsetof(M6InjectParamsGPU, cameraPos) == 64, "InjectParams cameraPos @64");
static_assert(offsetof(M6InjectParamsGPU, screen) == 128, "InjectParams screen @128");

// 与 M6GIResolve.frag ResolveParams 一源（std140 48B，探针盒确定后一次上传）。
struct M6ResolveParamsGPU {
    math::v4 gridMin;              // @0  xyz = 探针盒 min，w = normalBias（米）
    math::v4 gridInvExtent;        // @16 xyz = 1/(max-min)
    math::v4 gridDims;             // @32 xyz = (Nx,Ny,Nz)
};
static_assert(sizeof(M6ResolveParamsGPU) == 48, "M6ResolveParams std140 = 3*16");
static_assert(offsetof(M6ResolveParamsGPU, gridDims) == 32, "ResolveParams gridDims @32");

struct M6GIState {
    // 全屏三角 VS（M3Post.vert 字节；direct/resolve/composite 三管线共享）
    ShaderHandle fsVs{handles::INVALID_SHADER};
    // P2 直射 pass（M6DeferredDirect.frag；布局/集复用 M4 lightLayout/lightSet）
    ShaderHandle directFs{handles::INVALID_SHADER};
    PipelineHandle directPipeline{handles::INVALID_PIPELINE};
    // P3 注入链（b0-b3 SampledImage + b4 InjectParams UBO + b5 writeonly image3D）
    ShaderHandle injectCs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle injectSetLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle injectLayout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle injectPipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle injectSet{handles::INVALID_DESCRIPTOR_SET};
    ResourceHandle injectUbo{handles::INVALID_RESOURCE};      // 144B 逐帧
    // P4 accum 链（readonly image3D + sampler3D hist 对 + writeonly image3D +
    // AccumParams UBO）
    ShaderHandle accumCs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle accumSetLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle accumLayout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle accumPipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle accumSet{handles::INVALID_DESCRIPTOR_SET};
    ResourceHandle accumUbo{handles::INVALID_RESOURCE};       // 16B 逐帧
    // P5 解析链（b0 UBO + b1-b4 SampledImage + b5 texture3D + b6 sampler + b7 UBO）
    ShaderHandle resolveFs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle resolveSetLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle resolveLayout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle resolvePipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle resolveSet{handles::INVALID_DESCRIPTOR_SET};
    ResourceHandle resolveUbo{handles::INVALID_RESOURCE};     // 48B 一次
    // P6 合成链（b0/b1 SampledImage）
    ShaderHandle compFs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle compSetLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle compLayout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle compPipeline{handles::INVALID_PIPELINE};
    DescriptorSetHandle compSet{handles::INVALID_DESCRIPTOR_SET};
    // 3D 纹理面：grid[2] SH ping-pong + 注入输出 + 黑占位（GI=0）
    ResourceHandle grid[2]{handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
    // P3 注入输出（资源名 M6ProbeInjectOut；= P4 accum 的 newGrid 输入）。
    // 成员名 baked 沿用旧 bake 链命名（不改名以免牵动 accum 绑定代码），
    // 内容是本帧屏幕空间直射注入的 SH-L1 系数，不是解析烘焙值。
    ResourceHandle baked{handles::INVALID_RESOURCE};
    ResourceHandle blackTex{handles::INVALID_RESOURCE};       // GI=0 时 b8 占位
    SamplerHandle histSampler{handles::INVALID_SAMPLER};      // Nearest/Clamp（texelFetch）
    SamplerHandle probeSampler{handles::INVALID_SAMPLER};     // Linear/Clamp（tri-linear）
    // 延迟光照 b10 GIParams UBO（常量一次上传；GI-off 占位路径消费）
    ResourceHandle giParamsUbo{handles::INVALID_RESOURCE};
    ResourceHandle readbackBuf{handles::INVALID_RESOURCE};    // 48KB（24*8*256）
    // 探针盒（GI on：mesh AABB 并集外扩 kM6BoxPad；M6ComputeGridBox 填）
    math::v4 boxMin{0, 0, 0, 0}, boxMax{0, 0, 0, 0}, cell{0, 0, 0, 0};
    float normalBias{0.0f};
    u32 cur{0};                       // ping-pong：grid[cur]=history grid[1-cur]=out
    bool havePrevLight{false};        // 重置检测（帧 0 恒 reset——history 未定义）
    LightState prevLight{};
    bool readbackPending{false};      // 本帧已录读回拷贝（提交后 M6VerifyGI 消费）
    bool lastReset{false};            // 本帧是否 reset（导出清单 reset_this_frame）
    u32 framesSinceReset{0};          // 本帧距最近 reset 的帧数（reset 帧 = 0）
    u32 resetCount{0};
    u32 readbackFrames{0};
    u32 dumpCount{0};
    u32 exportFrames{0};              // RHI_GI_EXPORT_FRAMES 实际导出帧数
};

void M6GIDestroy(RHIDeviceBase* device, M6GIState& s) {
    if (s.readbackBuf != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.readbackBuf); g_ledger.buffers.erase(s.readbackBuf); }
    if (s.giParamsUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.giParamsUbo); g_ledger.buffers.erase(s.giParamsUbo); }
    if (s.probeSampler != handles::INVALID_SAMPLER) { device->DestroySampler(s.probeSampler); g_ledger.samplers.erase(s.probeSampler); }
    if (s.histSampler != handles::INVALID_SAMPLER) { device->DestroySampler(s.histSampler); g_ledger.samplers.erase(s.histSampler); }
    if (s.blackTex != handles::INVALID_RESOURCE) { device->DestroyTexture(s.blackTex); g_ledger.textures.erase(s.blackTex); }
    if (s.baked != handles::INVALID_RESOURCE) { device->DestroyTexture(s.baked); g_ledger.textures.erase(s.baked); }
    for (int i = 0; i < 2; ++i)
        if (s.grid[i] != handles::INVALID_RESOURCE) { device->DestroyTexture(s.grid[i]); g_ledger.textures.erase(s.grid[i]); }
    // P6 合成链
    if (s.compSet != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.compSet); g_ledger.sets.erase(s.compSet); }
    if (s.compPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.compPipeline); g_ledger.pipelines.erase(s.compPipeline); }
    if (s.compLayout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.compLayout); g_ledger.layouts.erase(s.compLayout); }
    if (s.compSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.compSetLayout); g_ledger.setLayouts.erase(s.compSetLayout); }
    if (s.compFs != handles::INVALID_SHADER) { device->DestroyShader(s.compFs); g_ledger.shaders.erase(s.compFs); }
    // P5 解析链
    if (s.resolveUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.resolveUbo); g_ledger.buffers.erase(s.resolveUbo); }
    if (s.resolveSet != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.resolveSet); g_ledger.sets.erase(s.resolveSet); }
    if (s.resolvePipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.resolvePipeline); g_ledger.pipelines.erase(s.resolvePipeline); }
    if (s.resolveLayout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.resolveLayout); g_ledger.layouts.erase(s.resolveLayout); }
    if (s.resolveSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.resolveSetLayout); g_ledger.setLayouts.erase(s.resolveSetLayout); }
    if (s.resolveFs != handles::INVALID_SHADER) { device->DestroyShader(s.resolveFs); g_ledger.shaders.erase(s.resolveFs); }
    // P4 accum 链
    if (s.accumUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.accumUbo); g_ledger.buffers.erase(s.accumUbo); }
    if (s.accumSet != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.accumSet); g_ledger.sets.erase(s.accumSet); }
    if (s.accumPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.accumPipeline); g_ledger.pipelines.erase(s.accumPipeline); }
    if (s.accumLayout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.accumLayout); g_ledger.layouts.erase(s.accumLayout); }
    if (s.accumSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.accumSetLayout); g_ledger.setLayouts.erase(s.accumSetLayout); }
    if (s.accumCs != handles::INVALID_SHADER) { device->DestroyShader(s.accumCs); g_ledger.shaders.erase(s.accumCs); }
    // P3 注入链
    if (s.injectUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.injectUbo); g_ledger.buffers.erase(s.injectUbo); }
    if (s.injectSet != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.injectSet); g_ledger.sets.erase(s.injectSet); }
    if (s.injectPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.injectPipeline); g_ledger.pipelines.erase(s.injectPipeline); }
    if (s.injectLayout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.injectLayout); g_ledger.layouts.erase(s.injectLayout); }
    if (s.injectSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.injectSetLayout); g_ledger.setLayouts.erase(s.injectSetLayout); }
    if (s.injectCs != handles::INVALID_SHADER) { device->DestroyShader(s.injectCs); g_ledger.shaders.erase(s.injectCs); }
    // P2 直射（布局/集属 M4，此处只毁自有管线与 shader）
    if (s.directPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.directPipeline); g_ledger.pipelines.erase(s.directPipeline); }
    if (s.directFs != handles::INVALID_SHADER) { device->DestroyShader(s.directFs); g_ledger.shaders.erase(s.directFs); }
    if (s.fsVs != handles::INVALID_SHADER) { device->DestroyShader(s.fsVs); g_ledger.shaders.erase(s.fsVs); }
    s = M6GIState{};
}

// 延迟光照描述符集回填（帧目标/shadow map 句柄就绪后；resize 重建后重绑）。
// M6：b8-b10 GI 面一并回填（GI=0 时 gi.grid 无效 → 走 black 占位；GI=1 时
// b8 恒绑 grid[0]——GI 模式 M4 light pass 不运行，P2 直射 pass 复用本集但
// 只消费 b0-b7；b8-b10 仅保持指向存活资源，从不留悬空描述符）。
void BindM4DeferredSet(RHIDeviceBase* device, const M4DeferredState& s,
                       const FrameTargets& ft, const M2ShadowState& shadow,
                       const M6GIState& gi) {
    if (s.lightSet == handles::INVALID_DESCRIPTOR_SET) return;
    DescriptorBufferInfo vbi{}, lbi{}, gibi{};
    vbi.buffer = s.viewUbo; vbi.offset = 0; vbi.range = sizeof(DeferredViewGPU);
    lbi.buffer = s.lightShadowUbo; lbi.offset = 0;
    lbi.range = sizeof(LightShadowDataGPU);
    gibi.buffer = gi.giParamsUbo; gibi.offset = 0; gibi.range = sizeof(M6GIParamsGPU);
    DescriptorImageInfo depth{}, albedo{}, normal{}, material{}, smap{}, ssamp{};
    depth.imageView = ft.depth;      depth.imageLayout = ResourceState::ShaderResource;
    albedo.imageView = ft.gAlbedo;   albedo.imageLayout = ResourceState::ShaderResource;
    normal.imageView = ft.gNormal;   normal.imageLayout = ResourceState::ShaderResource;
    material.imageView = ft.gMaterial; material.imageLayout = ResourceState::ShaderResource;
    smap.imageView = shadow.shadowMap; smap.imageLayout = ResourceState::ShaderResource;
    ssamp.sampler = shadow.shadowSampler;
    DescriptorImageInfo giProbe{}, giSamp{};
    giProbe.imageView = gi.grid[0] != handles::INVALID_RESOURCE
        ? gi.grid[0] : gi.blackTex;
    giProbe.imageLayout = ResourceState::ShaderResource;
    giSamp.sampler = gi.probeSampler;
    struct W { u32 binding; DescriptorType type; const DescriptorBufferInfo* b;
               const DescriptorImageInfo* i; };
    const W writes[11] = {
        {0, DescriptorType::UniformBuffer, &vbi, nullptr},
        {1, DescriptorType::UniformBuffer, &lbi, nullptr},
        {2, DescriptorType::SampledImage, nullptr, &depth},
        {3, DescriptorType::SampledImage, nullptr, &albedo},
        {4, DescriptorType::SampledImage, nullptr, &normal},
        {5, DescriptorType::SampledImage, nullptr, &material},
        {6, DescriptorType::SampledImage, nullptr, &smap},
        {7, DescriptorType::Sampler, nullptr, &ssamp},
        {8, DescriptorType::SampledImage, nullptr, &giProbe},
        {9, DescriptorType::Sampler, nullptr, &giSamp},
        {10, DescriptorType::UniformBuffer, &gibi, nullptr},
    };
    WriteDescriptorSet wds[11]{};
    for (u32 i = 0; i < 11; ++i) {
        wds[i].dstSet = s.lightSet;
        wds[i].dstBinding = writes[i].binding;
        wds[i].dstArrayElement = 0;
        wds[i].descriptorCount = 1;
        wds[i].descriptorType = writes[i].type;
        wds[i].bufferInfo = writes[i].b;
        wds[i].imageInfo = writes[i].i;
    }
    device->UpdateDescriptorSets(11, wds);
}

void M4DeferredDestroy(RHIDeviceBase* device, M4DeferredState& s) {
    if (s.lightSet != handles::INVALID_DESCRIPTOR_SET) { device->DestroyDescriptorSet(s.lightSet); g_ledger.sets.erase(s.lightSet); }
    if (s.lightShadowUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.lightShadowUbo); g_ledger.buffers.erase(s.lightShadowUbo); }
    if (s.viewUbo != handles::INVALID_RESOURCE) { device->DestroyBuffer(s.viewUbo); g_ledger.buffers.erase(s.viewUbo); }
    if (s.lightPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.lightPipeline); g_ledger.pipelines.erase(s.lightPipeline); }
    if (s.lightLayout != handles::INVALID_PIPELINE_LAYOUT) { device->DestroyPipelineLayout(s.lightLayout); g_ledger.layouts.erase(s.lightLayout); }
    if (s.lightSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device->DestroyDescriptorSetLayout(s.lightSetLayout); g_ledger.setLayouts.erase(s.lightSetLayout); }
    if (s.lightFs != handles::INVALID_SHADER) { device->DestroyShader(s.lightFs); g_ledger.shaders.erase(s.lightFs); }
    if (s.lightVs != handles::INVALID_SHADER) { device->DestroyShader(s.lightVs); g_ledger.shaders.erase(s.lightVs); }
    if (s.gbPipeline != handles::INVALID_PIPELINE) { device->DestroyPipeline(s.gbPipeline); g_ledger.pipelines.erase(s.gbPipeline); }
    if (s.gbFs != handles::INVALID_SHADER) { device->DestroyShader(s.gbFs); g_ledger.shaders.erase(s.gbFs); }
    if (s.gbVs != handles::INVALID_SHADER) { device->DestroyShader(s.gbVs); g_ledger.shaders.erase(s.gbVs); }
    s = M4DeferredState{};
}

// ======================= M6 GI 段（F6-2/F6-3/F6-5/F6-6）=====================

// 共同最小 GI 切片（RHI_TEST_GI=1 门控，默认 0 = M1-M5 行为不变；组合前置
// RHI_TEST_DEFERRED=1）。真实输入版（M6 direct-input spec，替换旧解析
// sky+flood 烘焙桩）：单次反弹、屏幕空间直射光注入 SH-L1 探针网格——
// 非完整物理场景 GI（传输无可见性、发射体限相机可见像素，见文件头）。
// 链（每帧，GBuffer 及其 →SRV 屏障之后；文件头 P2-P6 同序）：
//   P2 直射 pass（M6DeferredDirect.frag，MRT×2 RGBA16F，复用 M4 light
//     set/layout b0-b7）：giDirect（直射全项）/ giDirectDiffuse（GI 输入）
//     → RT→SRV
//   P3 M6ProbeInject.comp：读 giDirectDiffuse+深度+法线（SRV），每探针
//     一 workgroup（64 线程，dispatch 8×8×8）累积 SH-L1 → 注入输出
//     M6ProbeInjectOut（成员 baked；typed UAV store，(8,8,24) 三 slab=R/G/B）
//   compute→compute 显式 MemoryBarrier（inject store → accum load）
//   P4 M6ProbeAccum.comp（shader 原样）：EMA（hist*0.9+new*0.1；reset 帧
//     history 清零——GIHistoryValidity/SC tile 清零先例，parity matrix #8）；
//     dispatch (Nx/4,Ny/4,3Nz/4)；out UAV→SRV
//   P5 M6GIResolve.frag：giIndirect = albedo×E_h(N)（三 slab trilinear）→ SRV
//   P6 M6Composite.frag：direct+indirect 恰一次 → hdr（post on）/ color
//   ping-pong：grid[2] (8,8,24) RGBA16F 3D 纹理（D3D12 TEXTURE3D / Vulkan
//   VK_IMAGE_TYPE_3D）。
// 结构等价目标（阶段文档 §3）：D3D12 同 binding 布局/同 dispatch 拓扑/
// 同资源类型序列；数值目标=Vulkan 双跑逐位。
// 收敛读回：ladder 帧 CopyTextureToBuffer 落 [GI_MEAN] 阶梯行（512 探针
// 0.282095·c00 逐通道均值；bufferRowLength=32 texel=256B、imageHeight=8——
// 两端行距一致：D3D12 footprint RowPitch 256 对齐，Vulkan rowLength 以
// texel 计；跨端字节布局等价）。
// RHI_GI_DUMP=1：中央 slice(z=4) 各向同性值落 PPM（F6-6 中间输出定位）。

// M6 初始化。giFull=false（deferred=1 但 RHI_TEST_GI=0）：只建延迟光照
// b8/b9/b10 占位面（黑 1x1x1 3D 纹理 + Linear/Clamp 采样器 + scale=0
// GIParams）——扩展 shader 的 GI 项=+0.0，默认路径逐位不变（文件头契约）。
// giFull=true：加建 P2 直射管线（layout = M4 directLayout）、P3 注入、P4
// accum、P5 解析、P6 合成（RT 格式 = litRTFormat）链与 SH 3D 纹理面。
bool M6GIInit(RHIDeviceBase* device, M6GIState& s, bool wantD3D12, bool giFull,
              PipelineLayoutHandle directLayout, DataFormat litRTFormat,
              std::string& err) {
    // ---- 延迟光照 b10 GIParams UBO（giFull 决定 giScale 1/0）----
    {
        BufferDesc bd{};
        bd.size = sizeof(M6GIParamsGPU);
        bd.type = BufferType::Constant;
        bd.memoryUsage = GPUMemoryUsage::Dynamic;
        bd.name = "M6GIParams";
        s.giParamsUbo = device->CreateBuffer(bd);
        if (s.giParamsUbo == handles::INVALID_RESOURCE) { err = "M6 GIParams UBO"; return false; }
        g_ledger.buffers.insert(s.giParamsUbo);
        M6GIParamsGPU gp{};
        gp.gridMinAndScale = math::v4{kM6GridMin[0], kM6GridMin[1], kM6GridMin[2],
                                       giFull ? 1.0f : 0.0f};
        gp.gridInvExtent = math::v4{
            1.0f / (kM6GridMax[0] - kM6GridMin[0]),
            1.0f / (kM6GridMax[1] - kM6GridMin[1]),
            1.0f / (kM6GridMax[2] - kM6GridMin[2]), 0.0f};
        if (!device->UpdateBufferData(s.giParamsUbo, &gp, sizeof(gp), 0)) {
            err = "M6 GIParams upload";
            return false;
        }
    }
    // ---- b9 probeSampler（Linear/Clamp tri-linear；MoltenVK 移植性 Never）----
    {
        SamplerDesc sd{};
        sd.minFilter = FilterMode::Linear;
        sd.magFilter = FilterMode::Linear;
        sd.mipFilter = FilterMode::Linear;
        sd.addressU = TextureAddressMode::Clamp;
        sd.addressV = TextureAddressMode::Clamp;
        sd.addressW = TextureAddressMode::Clamp;
        sd.maxAnisotropy = 1;
        sd.comparisonFunc = ComparisonFunc::Never;
        s.probeSampler = device->CreateSampler(sd);
        if (s.probeSampler == handles::INVALID_SAMPLER) { err = "M6 probe sampler"; return false; }
        g_ledger.samplers.insert(s.probeSampler);
    }

    if (!giFull) {
        // 黑占位 3D 纹理（1x1x1 RGBA16F，SR-only；首帧 SampledImage 绑定）。
        // 内容必须显式清零：新建纹理内容未定义（可为 NaN/Inf 位模式）——
        // shader 侧 giScale=0 的乘法中和不了 NaN（0*NaN=NaN），默认路径
        // 逐位不变契约依赖占位纹理为确定的有限零值（M6 实测教训：未清零
        // 时 ~59 帧起 NaN 漂移进入 ACES/post 输出）。
        TextureDesc td{};
        td.size = {1, 1, 1};
        td.format = DataFormat::RGBA16_Float;
        td.type = TextureType::Texture3D;
        td.mipLevels = 1;
        td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
        td.memoryUsage = GPUMemoryUsage::Static;
        td.name = "M6GIBlack1";
        s.blackTex = device->CreateTexture(td);
        if (s.blackTex == handles::INVALID_RESOURCE) { err = "M6 black 3D texture"; return false; }
        g_ledger.textures.insert(s.blackTex);
        const u16 black[4] = {0, 0, 0, 0};   // RGBA16F 零位模式
        TextureRegionUpdate region{};
        region.mipLevel = 0;
        region.baseArrayLayer = 0;
        region.offsetX = 0;
        region.offsetY = 0;
        region.offsetZ = 0;
        region.width = 1;
        region.height = 1;
        region.depth = 1;
        if (!device->UpdateTextureRegion(s.blackTex, black, sizeof(black), region)) {
            err = "M6 black 3D texture zero-init";
            return false;
        }
        device->PrimeTextureLayout(s.blackTex);   // P0-B 首帧 SR 绑定 priming
        return true;                              // compute 链不建（GI=0）
    }

    // shader 装载（platform 决定字节码；同 M5/M4 装载惯例）。
    auto loadShader = [&](const char* d3d12Rel, const char* vkRel, ShaderStage stage,
                          ShaderHandle& out, const char* tag) -> bool {
        const char* rel = wantD3D12 ? d3d12Rel : vkRel;
        std::vector<u8> bytes = ReadFileRel(rel);
        // D3D12 分支 SHA 门禁（错位 cso → E_INVALIDARG 护盾；空字节也走，
        // 以便 missing 场景带期望 SHA 报错。Vulkan 分支不进此块——零影响）。
        if (wantD3D12 && !sha_gate::GateD3D12Cso(rel, bytes, tag)) {
            err = std::string(tag) + " cso sha256 gate";
            return false;
        }
        if (bytes.empty()) { err = std::string(tag) + " bytes missing (" + rel + ")"; return false; }
        out = device->CreateShader(bytes.data(), bytes.size(), stage, "main");
        if (out == handles::INVALID_SHADER) { err = std::string(tag) + " CreateShader"; return false; }
        g_ledger.shaders.insert(out);
        return true;
    };
    // 全屏三角形管线公共形态（无顶点输入/无深度/无 cull——同 M3Post/M4 light）。
    auto fullscreenDesc = [&](ShaderHandle fsShader, PipelineLayoutHandle layout) {
        GraphicsPipelineDesc gpd{};
        gpd.vertexShader = s.fsVs;
        gpd.pixelShader = fsShader;
        gpd.layout = layout;
        gpd.topology = PrimitiveTopology::TriangleList;
        gpd.fillMode = FillMode::Solid;
        gpd.cullMode = CullMode::None;
        gpd.enableDepthTest = false;
        gpd.enableDepthWrite = false;
        return gpd;
    };

    // ---- 全屏三角 VS（M3Post.vert 字节；P2/P5/P6 三管线共享一个句柄）----
    if (!loadShader("EngineTest/shaders/D3D12/Forward/M3Post.vert.cso",
                    "Engine/Graphics/Vulkan/shaders/Forward/M3Post.vert.spv",
                    ShaderStage::Vertex, s.fsVs, "M6 fullscreen VS (M3Post.vert)"))
        return false;

    // ---- P2 直射 pass：M6DeferredDirect.frag，MRT×2 RGBA16F（giDirect /
    //      giDirectDiffuse）。布局 = M4 lightLayout（11 绑定超集，shader 只
    //      声明 b0-b7——spec §6.2 复用 M4 light set layout/pipeline layout/set）----
    {
        if (!loadShader("EngineTest/shaders/D3D12/Forward/M6DeferredDirect.frag.cso",
                        "Engine/Graphics/Vulkan/shaders/Forward/M6DeferredDirect.frag.spv",
                        ShaderStage::Pixel, s.directFs, "M6DeferredDirect"))
            return false;
        GraphicsPipelineDesc gpd = fullscreenDesc(s.directFs, directLayout);
        gpd.renderTargetCount = 2;
        gpd.renderTargetFormats[0] = DataFormat::RGBA16_Float;   // giDirect
        gpd.renderTargetFormats[1] = DataFormat::RGBA16_Float;   // giDirectDiffuse
        s.directPipeline = device->CreateGraphicsPipeline(gpd);
        if (s.directPipeline == handles::INVALID_PIPELINE) { err = "M6DeferredDirect pipeline"; return false; }
        g_ledger.pipelines.insert(s.directPipeline);
    }

    // ---- P3 注入链：M6ProbeInject.comp（local 64x1x1，一 workgroup = 一探针）----
    {
        if (!loadShader("EngineTest/shaders/D3D12/Forward/M6ProbeInject.comp.cso",
                        "Engine/Graphics/Vulkan/shaders/Forward/M6ProbeInject.comp.spv",
                        ShaderStage::Compute, s.injectCs, "M6ProbeInject"))
            return false;
        DescriptorSetLayoutBinding b[6];
        b[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};   // directDiffuse
        b[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute};   // depthTexture
        b[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Compute};   // gbufferNormal
        b[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Compute};   // gbufferMaterial
        b[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};  // InjectParams
        b[5] = {5, DescriptorType::StorageImage, 1, ShaderStage::Compute};   // outSH（writeonly）
        DescriptorSetLayoutDesc dld{};
        dld.bindingCount = 6;
        dld.bindings = b;
        s.injectSetLayout = device->CreateDescriptorSetLayout(dld);
        PipelineLayoutDesc pld{};
        pld.setLayoutCount = 1;
        pld.setLayouts = &s.injectSetLayout;
        s.injectLayout = device->CreatePipelineLayout(pld);
        ComputePipelineDesc cpd{};
        cpd.computeShader = s.injectCs;
        cpd.layout = s.injectLayout;
        cpd.threadGroupSize = {64, 1, 1};
        s.injectPipeline = device->CreateComputePipeline(cpd);
        if (s.injectSetLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
            s.injectLayout == handles::INVALID_PIPELINE_LAYOUT ||
            s.injectPipeline == handles::INVALID_PIPELINE) {
            err = "M6ProbeInject pipeline";
            return false;
        }
        g_ledger.setLayouts.insert(s.injectSetLayout);
        g_ledger.layouts.insert(s.injectLayout);
        g_ledger.pipelines.insert(s.injectPipeline);
        BufferDesc bd{};
        bd.size = sizeof(M6InjectParamsGPU);
        bd.type = BufferType::Constant;
        bd.memoryUsage = GPUMemoryUsage::Dynamic;
        bd.name = "M6InjectParams";
        s.injectUbo = device->CreateBuffer(bd);
        if (s.injectUbo == handles::INVALID_RESOURCE) { err = "M6 inject UBO"; return false; }
        g_ledger.buffers.insert(s.injectUbo);
        DescriptorSetDesc dsd{};
        dsd.layout = s.injectSetLayout;
        s.injectSet = device->CreateDescriptorSet(dsd);
        if (s.injectSet == handles::INVALID_DESCRIPTOR_SET) { err = "M6 inject set"; return false; }
        g_ledger.sets.insert(s.injectSet);
    }

    // ---- P5 解析链：M6GIResolve.frag → giIndirect（1 RT RGBA16F）----
    {
        if (!loadShader("EngineTest/shaders/D3D12/Forward/M6GIResolve.frag.cso",
                        "Engine/Graphics/Vulkan/shaders/Forward/M6GIResolve.frag.spv",
                        ShaderStage::Pixel, s.resolveFs, "M6GIResolve"))
            return false;
        DescriptorSetLayoutBinding b[8];
        b[0] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};   // DeferredViewData
        b[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel};    // depthTexture
        b[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel};    // gbufferAlbedo
        b[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel};    // gbufferNormal
        b[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel};    // gbufferMaterial
        b[5] = {5, DescriptorType::SampledImage, 1, ShaderStage::Pixel};    // giProbeSH（texture3D）
        b[6] = {6, DescriptorType::Sampler, 1, ShaderStage::Pixel};         // giProbeSampler
        b[7] = {7, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};   // ResolveParams
        DescriptorSetLayoutDesc dld{};
        dld.bindingCount = 8;
        dld.bindings = b;
        s.resolveSetLayout = device->CreateDescriptorSetLayout(dld);
        PipelineLayoutDesc pld{};
        pld.setLayoutCount = 1;
        pld.setLayouts = &s.resolveSetLayout;
        s.resolveLayout = device->CreatePipelineLayout(pld);
        if (s.resolveSetLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
            s.resolveLayout == handles::INVALID_PIPELINE_LAYOUT) {
            err = "M6GIResolve layouts";
            return false;
        }
        g_ledger.setLayouts.insert(s.resolveSetLayout);
        g_ledger.layouts.insert(s.resolveLayout);
        GraphicsPipelineDesc gpd = fullscreenDesc(s.resolveFs, s.resolveLayout);
        gpd.renderTargetCount = 1;
        gpd.renderTargetFormats[0] = DataFormat::RGBA16_Float;   // giIndirect
        s.resolvePipeline = device->CreateGraphicsPipeline(gpd);
        if (s.resolvePipeline == handles::INVALID_PIPELINE) { err = "M6GIResolve pipeline"; return false; }
        g_ledger.pipelines.insert(s.resolvePipeline);
        BufferDesc bd{};
        bd.size = sizeof(M6ResolveParamsGPU);
        bd.type = BufferType::Constant;
        bd.memoryUsage = GPUMemoryUsage::Dynamic;
        bd.name = "M6ResolveParams";
        s.resolveUbo = device->CreateBuffer(bd);
        if (s.resolveUbo == handles::INVALID_RESOURCE) { err = "M6 resolve UBO"; return false; }
        g_ledger.buffers.insert(s.resolveUbo);
        DescriptorSetDesc dsd{};
        dsd.layout = s.resolveSetLayout;
        s.resolveSet = device->CreateDescriptorSet(dsd);
        if (s.resolveSet == handles::INVALID_DESCRIPTOR_SET) { err = "M6 resolve set"; return false; }
        g_ledger.sets.insert(s.resolveSet);
    }

    // ---- P6 合成链：M6Composite.frag → litRTFormat（post on→hdr / off→color）----
    {
        if (!loadShader("EngineTest/shaders/D3D12/Forward/M6Composite.frag.cso",
                        "Engine/Graphics/Vulkan/shaders/Forward/M6Composite.frag.spv",
                        ShaderStage::Pixel, s.compFs, "M6Composite"))
            return false;
        DescriptorSetLayoutBinding b[2];
        b[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel};    // giDirect
        b[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel};    // giIndirect
        DescriptorSetLayoutDesc dld{};
        dld.bindingCount = 2;
        dld.bindings = b;
        s.compSetLayout = device->CreateDescriptorSetLayout(dld);
        PipelineLayoutDesc pld{};
        pld.setLayoutCount = 1;
        pld.setLayouts = &s.compSetLayout;
        s.compLayout = device->CreatePipelineLayout(pld);
        if (s.compSetLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
            s.compLayout == handles::INVALID_PIPELINE_LAYOUT) {
            err = "M6Composite layouts";
            return false;
        }
        g_ledger.setLayouts.insert(s.compSetLayout);
        g_ledger.layouts.insert(s.compLayout);
        GraphicsPipelineDesc gpd = fullscreenDesc(s.compFs, s.compLayout);
        gpd.renderTargetCount = 1;
        gpd.renderTargetFormats[0] = litRTFormat;   // 与 M4 light pass 同目标同格式
        s.compPipeline = device->CreateGraphicsPipeline(gpd);
        if (s.compPipeline == handles::INVALID_PIPELINE) { err = "M6Composite pipeline"; return false; }
        g_ledger.pipelines.insert(s.compPipeline);
        DescriptorSetDesc dsd{};
        dsd.layout = s.compSetLayout;
        s.compSet = device->CreateDescriptorSet(dsd);
        if (s.compSet == handles::INVALID_DESCRIPTOR_SET) { err = "M6 composite set"; return false; }
        g_ledger.sets.insert(s.compSet);
    }

    // ---- accum 链 ----
    {
        const char* rel = wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M6ProbeAccum.comp.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M6ProbeAccum.comp.spv";
        std::vector<u8> cb = ReadFileRel(rel);
        // 同 loadShader 的 D3D12 SHA 门禁（accum 链不走 lambda，独立装载）。
        if (wantD3D12 && !sha_gate::GateD3D12Cso(rel, cb, "M6ProbeAccum")) {
            err = "M6ProbeAccum cso sha256 gate";
            return false;
        }
        if (cb.empty()) { err = std::string("M6ProbeAccum bytes missing (") + rel + ")"; return false; }
        s.accumCs = device->CreateShader(cb.data(), cb.size(), ShaderStage::Compute, "main");
        if (s.accumCs == handles::INVALID_SHADER) { err = "M6ProbeAccum CreateShader"; return false; }
        g_ledger.shaders.insert(s.accumCs);
        DescriptorSetLayoutBinding b[5];
        b[0] = {0, DescriptorType::StorageImage, 1, ShaderStage::Compute};   // readonly load
        b[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute};   // texture3D hist
        b[2] = {2, DescriptorType::Sampler, 1, ShaderStage::Compute};        // texelFetch 对
        b[3] = {3, DescriptorType::StorageImage, 1, ShaderStage::Compute};   // writeonly out
        b[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};  // flags
        DescriptorSetLayoutDesc dld{};
        dld.bindingCount = 5;
        dld.bindings = b;
        s.accumSetLayout = device->CreateDescriptorSetLayout(dld);
        PipelineLayoutDesc pld{};
        pld.setLayoutCount = 1;
        pld.setLayouts = &s.accumSetLayout;
        s.accumLayout = device->CreatePipelineLayout(pld);
        ComputePipelineDesc cpd{};
        cpd.computeShader = s.accumCs;
        cpd.layout = s.accumLayout;
        cpd.threadGroupSize = {4, 4, 4};
        s.accumPipeline = device->CreateComputePipeline(cpd);
        if (s.accumSetLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
            s.accumLayout == handles::INVALID_PIPELINE_LAYOUT ||
            s.accumPipeline == handles::INVALID_PIPELINE) {
            err = "M6ProbeAccum pipeline";
            return false;
        }
        g_ledger.setLayouts.insert(s.accumSetLayout);
        g_ledger.layouts.insert(s.accumLayout);
        g_ledger.pipelines.insert(s.accumPipeline);
        BufferDesc bd{};
        bd.size = sizeof(M6AccumParamsGPU);
        bd.type = BufferType::Constant;
        bd.memoryUsage = GPUMemoryUsage::Dynamic;
        bd.name = "M6AccumParams";
        s.accumUbo = device->CreateBuffer(bd);
        if (s.accumUbo == handles::INVALID_RESOURCE) { err = "M6 accum UBO"; return false; }
        g_ledger.buffers.insert(s.accumUbo);
        DescriptorSetDesc dsd{};
        dsd.layout = s.accumSetLayout;
        s.accumSet = device->CreateDescriptorSet(dsd);
        if (s.accumSet == handles::INVALID_DESCRIPTOR_SET) { err = "M6 accum set"; return false; }
        g_ledger.sets.insert(s.accumSet);
        // histSampler：Nearest/Clamp（texelFetch 1:1；过滤档不进入数值路径）
        SamplerDesc sd{};
        sd.minFilter = FilterMode::Nearest;
        sd.magFilter = FilterMode::Nearest;
        sd.mipFilter = FilterMode::Nearest;
        sd.addressU = TextureAddressMode::Clamp;
        sd.addressV = TextureAddressMode::Clamp;
        sd.addressW = TextureAddressMode::Clamp;
        sd.comparisonFunc = ComparisonFunc::Never;
        s.histSampler = device->CreateSampler(sd);
        if (s.histSampler == handles::INVALID_SAMPLER) { err = "M6 hist sampler"; return false; }
        g_ledger.samplers.insert(s.histSampler);
    }

    // ---- 3D 纹理面（注入输出 + grid ping-pong）：SH-L1 (Nx,Ny,3·Nz)=(8,8,24)，
    //      slab s∈{0,1,2}=通道 R/G/B，texel(i,j,s·8+k)=(c00,c1-1,c10,c11)----
    {
        TextureDesc td{};
        td.size = {kM6GridDim, kM6GridDim, kM6ShSlabs * kM6GridDim};
        td.format = DataFormat::RGBA16_Float;
        td.type = TextureType::Texture3D;       // 首次入共享宿主的 3D 资源面
        td.mipLevels = 1;
        td.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                   TextureUsage::CopySource;
        td.memoryUsage = GPUMemoryUsage::Static;
        td.name = "M6ProbeGrid";
        for (int i = 0; i < 2; ++i) {
            td.name = i == 0 ? "M6ProbeGridA" : "M6ProbeGridB";
            s.grid[i] = device->CreateTexture(td);
            if (s.grid[i] == handles::INVALID_RESOURCE) { err = "M6 grid texture"; return false; }
            g_ledger.textures.insert(s.grid[i]);
        }
        td.name = "M6ProbeInjectOut";
        td.usage = TextureUsage::UnorderedAccess;   // inject 目标（image store）/ accum typed load
        s.baked = device->CreateTexture(td);
        if (s.baked == handles::INVALID_RESOURCE) { err = "M6 inject output texture"; return false; }
        g_ledger.textures.insert(s.baked);

        BufferDesc rb{};
        rb.type = BufferType::Raw;
        // R19-1：同 M1Readback——CopyTextureToBuffer 的 GPU 写目标须 Readback 位。
        rb.usage = GPUMemoryUsage::Readback;
        rb.memoryUsage = GPUMemoryUsage::Readback;
        rb.size = u64(kM6ShSlabs * kM6GridDim) * kM6GridDim * kM6ReadbackRowTexels * 8u;
        // = 24 slice * 8 行 * 256B = 49152B（rowLength 32 texel，imageHeight 8）
        rb.name = "M6GIReadback";
        s.readbackBuf = device->CreateBuffer(rb);
        if (s.readbackBuf == handles::INVALID_RESOURCE) { err = "M6 readback buffer"; return false; }
        g_ledger.buffers.insert(s.readbackBuf);
    }
    return true;
}

// 注入集逐帧回填（帧目标句柄随 resize 重建而变——每帧重绑，零陈旧句柄；
// 每帧 submit+wait，更新时无在途使用）。
void M6BindInjectSet(RHIDeviceBase* device, const M6GIState& s, const FrameTargets& ft) {
    if (s.injectSet == handles::INVALID_DESCRIPTOR_SET) return;
    DescriptorImageInfo dd{}, depth{}, normal{}, material{}, outImg{};
    dd.imageView = ft.giDirectDiffuse;   dd.imageLayout = ResourceState::ShaderResource;
    depth.imageView = ft.depth;          depth.imageLayout = ResourceState::ShaderResource;
    normal.imageView = ft.gNormal;       normal.imageLayout = ResourceState::ShaderResource;
    material.imageView = ft.gMaterial;   material.imageLayout = ResourceState::ShaderResource;
    outImg.imageView = s.baked;          // 注入输出 M6ProbeInjectOut：UAV image3D store
    outImg.imageLayout = ResourceState::UnorderedAccess;
    DescriptorBufferInfo bi{};
    bi.buffer = s.injectUbo; bi.offset = 0; bi.range = sizeof(M6InjectParamsGPU);
    struct W { u32 b; DescriptorType t; const DescriptorBufferInfo* ub;
               const DescriptorImageInfo* ib; };
    const W ws[6] = {
        {0, DescriptorType::SampledImage, nullptr, &dd},
        {1, DescriptorType::SampledImage, nullptr, &depth},
        {2, DescriptorType::SampledImage, nullptr, &normal},
        {3, DescriptorType::SampledImage, nullptr, &material},
        {4, DescriptorType::UniformBuffer, &bi, nullptr},
        {5, DescriptorType::StorageImage, nullptr, &outImg},
    };
    WriteDescriptorSet w[6]{};
    for (u32 i = 0; i < 6; ++i) {
        w[i].dstSet = s.injectSet;
        w[i].dstBinding = ws[i].b;
        w[i].dstArrayElement = 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = ws[i].t;
        w[i].bufferInfo = ws[i].ub;
        w[i].imageInfo = ws[i].ib;
    }
    device->UpdateDescriptorSets(6, w);
}

// accum 集回填（hist/out 由 cur 决定；逐帧重绑——ping-pong 目标切换）。
void M6BindAccumSet(RHIDeviceBase* device, const M6GIState& s) {
    if (s.accumSet == handles::INVALID_DESCRIPTOR_SET) return;
    DescriptorImageInfo newImg{}, histImg{}, histSamp{}, outImg{};
    newImg.imageView = s.baked;
    newImg.imageLayout = ResourceState::UnorderedAccess;      // typed UAV load
    histImg.imageView = s.grid[s.cur];
    histImg.imageLayout = ResourceState::ShaderResource;      // sampler3D SRV
    histSamp.sampler = s.histSampler;
    outImg.imageView = s.grid[1u - s.cur];
    outImg.imageLayout = ResourceState::UnorderedAccess;      // UAV store
    DescriptorBufferInfo bi{};
    bi.buffer = s.accumUbo; bi.offset = 0; bi.range = sizeof(M6AccumParamsGPU);
    struct W { u32 b; DescriptorType t; const DescriptorBufferInfo* ub;
               const DescriptorImageInfo* ib; };
    const W ws[5] = {
        {0, DescriptorType::StorageImage, nullptr, &newImg},
        {1, DescriptorType::SampledImage, nullptr, &histImg},
        {2, DescriptorType::Sampler, nullptr, &histSamp},
        {3, DescriptorType::StorageImage, nullptr, &outImg},
        {4, DescriptorType::UniformBuffer, &bi, nullptr},
    };
    WriteDescriptorSet w[5]{};
    for (u32 i = 0; i < 5; ++i) {
        w[i].dstSet = s.accumSet;
        w[i].dstBinding = ws[i].b;
        w[i].dstArrayElement = 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = ws[i].t;
        w[i].bufferInfo = ws[i].ub;
        w[i].imageInfo = ws[i].ib;
    }
    device->UpdateDescriptorSets(5, w);
}

// 解析集逐帧回填（帧目标句柄随 resize 变 + ping-pong）：b5 = 本帧 accum 输出
//（M6RecordGI 已翻转 cur：out = grid[翻转前 1-cur] = grid[翻转后 cur]——
// 旧 M6BindDeferredProbe 的 off-by-one 教训同此，必须在翻转后取 grid[cur]）。
void M6BindResolveSet(RHIDeviceBase* device, const M6GIState& s, const FrameTargets& ft,
                      ResourceHandle viewUbo) {
    if (s.resolveSet == handles::INVALID_DESCRIPTOR_SET) return;
    DescriptorBufferInfo vbi{}, rbi{};
    vbi.buffer = viewUbo; vbi.offset = 0; vbi.range = sizeof(DeferredViewGPU);
    rbi.buffer = s.resolveUbo; rbi.offset = 0; rbi.range = sizeof(M6ResolveParamsGPU);
    DescriptorImageInfo depth{}, albedo{}, normal{}, material{}, sh{}, samp{};
    depth.imageView = ft.depth;        depth.imageLayout = ResourceState::ShaderResource;
    albedo.imageView = ft.gAlbedo;     albedo.imageLayout = ResourceState::ShaderResource;
    normal.imageView = ft.gNormal;     normal.imageLayout = ResourceState::ShaderResource;
    material.imageView = ft.gMaterial; material.imageLayout = ResourceState::ShaderResource;
    sh.imageView = s.grid[s.cur];      sh.imageLayout = ResourceState::ShaderResource;
    samp.sampler = s.probeSampler;     // Linear/Clamp（三 slab 各自 trilinear）
    struct W { u32 b; DescriptorType t; const DescriptorBufferInfo* ub;
               const DescriptorImageInfo* ib; };
    const W ws[8] = {
        {0, DescriptorType::UniformBuffer, &vbi, nullptr},
        {1, DescriptorType::SampledImage, nullptr, &depth},
        {2, DescriptorType::SampledImage, nullptr, &albedo},
        {3, DescriptorType::SampledImage, nullptr, &normal},
        {4, DescriptorType::SampledImage, nullptr, &material},
        {5, DescriptorType::SampledImage, nullptr, &sh},
        {6, DescriptorType::Sampler, nullptr, &samp},
        {7, DescriptorType::UniformBuffer, &rbi, nullptr},
    };
    WriteDescriptorSet w[8]{};
    for (u32 i = 0; i < 8; ++i) {
        w[i].dstSet = s.resolveSet;
        w[i].dstBinding = ws[i].b;
        w[i].dstArrayElement = 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = ws[i].t;
        w[i].bufferInfo = ws[i].ub;
        w[i].imageInfo = ws[i].ib;
    }
    device->UpdateDescriptorSets(8, w);
}

// 合成集逐帧回填（giDirect / giIndirect 随 resize 重建）。
void M6BindCompositeSet(RHIDeviceBase* device, const M6GIState& s, const FrameTargets& ft) {
    if (s.compSet == handles::INVALID_DESCRIPTOR_SET) return;
    DescriptorImageInfo dir{}, ind{};
    dir.imageView = ft.giDirect;   dir.imageLayout = ResourceState::ShaderResource;
    ind.imageView = ft.giIndirect; ind.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet w[2]{};
    w[0].dstSet = s.compSet; w[0].dstBinding = 0; w[0].dstArrayElement = 0;
    w[0].descriptorCount = 1; w[0].descriptorType = DescriptorType::SampledImage;
    w[0].imageInfo = &dir;
    w[1].dstSet = s.compSet; w[1].dstBinding = 1; w[1].dstArrayElement = 0;
    w[1].descriptorCount = 1; w[1].descriptorType = DescriptorType::SampledImage;
    w[1].imageInfo = &ind;
    device->UpdateDescriptorSets(2, w);
}

// 探针盒（spec §4）：全部可绘 mesh 局部 AABB 并集（model=identity——同 M5
// 包围球源 mesh->GetLocalAABB()）各向外扩 kM6BoxPad；cell=(max-min)/Nx；
// normalBias=0.25·min(cell)。写入 s 并一次上传 ResolveParams（盒为场景
// 常量；InjectParams 逐帧上传时读 s.boxMin/s.cell）。
bool M6ComputeGridBox(RHIDeviceBase* device, M6GIState& s,
                      const std::vector<MeshEntry>& meshes, std::string& err) {
    if (meshes.empty()) { err = "probe box: no meshes"; return false; }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const MeshEntry& e : meshes) {
        const rhi::AABB& bb = e.mesh->GetLocalAABB();
        mn[0] = std::min(mn[0], bb.min.x); mx[0] = std::max(mx[0], bb.max.x);
        mn[1] = std::min(mn[1], bb.min.y); mx[1] = std::max(mx[1], bb.max.y);
        mn[2] = std::min(mn[2], bb.min.z); mx[2] = std::max(mx[2], bb.max.z);
    }
    for (int a = 0; a < 3; ++a) {
        mn[a] -= kM6BoxPad;
        mx[a] += kM6BoxPad;
        if (!(mx[a] > mn[a])) { err = "probe box: degenerate mesh AABB union"; return false; }
    }
    const float n = float(kM6GridDim);
    s.boxMin = math::v4{mn[0], mn[1], mn[2], 0.0f};
    s.boxMax = math::v4{mx[0], mx[1], mx[2], 0.0f};
    s.cell = math::v4{(mx[0] - mn[0]) / n, (mx[1] - mn[1]) / n, (mx[2] - mn[2]) / n, 0.0f};
    s.normalBias = 0.25f * std::min(s.cell.x, std::min(s.cell.y, s.cell.z));
    M6ResolveParamsGPU rp{};
    rp.gridMin = math::v4{mn[0], mn[1], mn[2], s.normalBias};
    rp.gridInvExtent = math::v4{1.0f / (mx[0] - mn[0]), 1.0f / (mx[1] - mn[1]),
                                1.0f / (mx[2] - mn[2]), 0.0f};
    rp.gridDims = math::v4{n, n, n, 0.0f};
    if (!device->UpdateBufferData(s.resolveUbo, &rp, sizeof(rp), 0)) {
        err = "M6 resolve UBO upload";
        return false;
    }
    std::cout << "[GI] grid box min=(" << mn[0] << "," << mn[1] << "," << mn[2]
              << ") max=(" << mx[0] << "," << mx[1] << "," << mx[2]
              << ") cell=(" << s.cell.x << "," << s.cell.y << "," << s.cell.z
              << ") dims=" << kM6GridDim << "x" << kM6GridDim << "x" << kM6GridDim
              << " sh=L1(" << kM6GridDim << "x" << kM6GridDim << "x"
              << kM6ShSlabs * kM6GridDim << ") normalBias=" << s.normalBias
              << std::endl;
    return true;
}

// 帧内 GI compute 段录制（P3 注入 → P4 累积 → ladder 读回 → ping-pong 翻转）。
// 前置（调用方保证）：本帧 P2 直射 pass 已录，giDirectDiffuse/深度/GBuffer
// 已转 SRV。返回 true=本帧触发历史重置（光变事件检测——GIHistoryValidity
// 表征：intensity/color/dir 任一变化或首帧）。
bool M6RecordGI(RHIDeviceBase* device, RHICommandBuffer* cmd, M6GIState& s,
                const FrameTargets& ft, const math::m4x4& invViewProj,
                const math::v4& cameraPos, const LightState& light, u32 frame,
                bool wantLadder, u32 totalFrames) {
    // 重置检测（步进语义：光 keyframe 档位切换帧）。
    const bool reset = !s.havePrevLight ||
        light.intensity != s.prevLight.intensity ||
        light.color.x != s.prevLight.color.x ||
        light.color.y != s.prevLight.color.y ||
        light.color.z != s.prevLight.color.z ||
        light.dirToLight.x != s.prevLight.dirToLight.x ||
        light.dirToLight.y != s.prevLight.dirToLight.y ||
        light.dirToLight.z != s.prevLight.dirToLight.z;
    s.prevLight = light;
    s.havePrevLight = true;
    s.lastReset = reset;
    s.framesSinceReset = reset ? 0u : s.framesSinceReset + 1u;
    if (reset) {
        ++s.resetCount;
        std::cerr << "[GI] frame " << frame << " RESET (light event; history zeroed)"
                  << std::endl;
    }

    // UBO 逐帧上传（提交前 host 写；确定性——相机/盒/屏幕尺寸 + 帧级 reset
    // 标志唯一输入；光参数只经 giDirectDiffuse 间接进入）。
    M6InjectParamsGPU ip{};
    ip.invViewProjection = invViewProj;    // = M4 DeferredView 同一 Inverse(proj*view)
    ip.cameraPos = cameraPos;
    ip.gridMin = s.boxMin;
    ip.cellSize = s.cell;
    ip.gridDims = math::v4{float(kM6GridDim), float(kM6GridDim), float(kM6GridDim), 0.0f};
    ip.screen = math::v4{float(ft.w), float(ft.h), float(kM6InjectStride), 0.0f};
    Require(device->UpdateBufferData(s.injectUbo, &ip, sizeof(ip), 0),
            "M6 inject UBO upload");
    M6AccumParamsGPU ap{};
    ap.flags = math::v4{reset ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    Require(device->UpdateBufferData(s.accumUbo, &ap, sizeof(ap), 0),
            "M6 accum UBO upload");

    const ResourceHandle hist = s.grid[s.cur];
    const ResourceHandle out = s.grid[1u - s.cur];
    M6BindInjectSet(device, s, ft);  // 本帧帧目标句柄（resize 安全）
    M6BindAccumSet(device, s);       // cur 语义：hist=grid[cur] out=grid[1-cur]

    auto barrier = [&](ResourceHandle tex, ResourceState after) {
        ResourceBarrier rb{};
        rb.resource = tex;
        rb.beforeState = ResourceState::General;   // 参考（truth-based tracked）
        rb.afterState = after;
        rb.subresource = 0xFFFFFFFF;
        rb.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&rb, 1);
    };

    // ① 注入：注入输出（M6ProbeInjectOut）→UAV，dispatch（一 workgroup = 一探针，
    //   8×8×8 组；每组 invocation 0 写三 slab → (8,8,24) 纹素全覆盖）
    barrier(s.baked, ResourceState::UnorderedAccess);
    cmd->BindComputePipeline(s.injectPipeline);
    const DescriptorSetHandle injectSets[] = {s.injectSet};
    cmd->BindDescriptorSets(PipelineBindPoint::Compute, s.injectLayout, 0, 1,
                            injectSets, 0, nullptr);
    cmd->Dispatch(kM6GridDim, kM6GridDim, kM6GridDim);
    // ② compute→compute 显式依赖（inject store → accum typed load；§1 约定①）
    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                       AccessFlag::ShaderWrite,
                       AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
    // ③ accum：hist→SRV（sampler3D）、out→UAV（store），dispatch（EMA；
    //   local 4x4x4 → (Nx/4, Ny/4, 3Nz/4) = (2,2,6)）
    barrier(hist, ResourceState::ShaderResource);
    barrier(out, ResourceState::UnorderedAccess);
    cmd->BindComputePipeline(s.accumPipeline);
    const DescriptorSetHandle accumSets[] = {s.accumSet};
    cmd->BindDescriptorSets(PipelineBindPoint::Compute, s.accumLayout, 0, 1,
                            accumSets, 0, nullptr);
    cmd->Dispatch(kM6GridDim / 4, kM6GridDim / 4, kM6ShSlabs * kM6GridDim / 4);
    // ④ out UAV→SRV（compute store → P5 解析 trilinear 采样）
    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::FragmentShader,
                       AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
    barrier(out, ResourceState::ShaderResource);

    // 收敛读回（ladder 帧：interval 命中/重置帧/末帧）。CopyTextureToBuffer
    // 内部 SRV→CopySource→SRV 自洽（两端同契约）。
    const bool ladderFrame = wantLadder && (reset || frame == totalFrames - 1 ||
                                            frame % 5u == 0u);
    if (ladderFrame) {
        BufferTextureCopyRegion cr{};
        cr.bufferOffset = 0;
        cr.bufferRowLength = kM6ReadbackRowTexels;   // 256B 行距（两端一致）
        cr.bufferImageHeight = kM6GridDim;
        cr.imageSubresource = {0, 0, 1};
        cr.imageOffset = {0, 0, 0};
        cr.imageExtent = {kM6GridDim, kM6GridDim, kM6ShSlabs * kM6GridDim};
        cmd->CopyTextureToBuffer(out, s.readbackBuf, &cr, 1);
        s.readbackPending = true;
    }
    s.cur = 1u - s.cur;   // ping-pong 翻转（out 成为下一帧 history；本帧 P5 读 grid[cur]）
    return reset;
}

// IEEE binary16 → binary32 完整解码（含次正规/±0/Inf/NaN）。旧 M6 ladder
// 把次正规数冲成 0；SH 系数在暗区/近 reset 帧可落入次正规段（< 2^-14），
// 均值换算需逐位精确。
float M6HalfToFloat(u16 h) {
    const u32 sign = (u32(h) >> 15) & 1u;
    const u32 ex = (u32(h) >> 10) & 0x1Fu;
    u32 mant = u32(h) & 0x3FFu;
    u32 bits;
    if (ex == 0u) {
        if (mant == 0u) {
            bits = sign << 31;                                  // ±0
        } else {                                                // 次正规：规格化
            u32 e = 0;
            while ((mant & 0x400u) == 0u) { mant <<= 1; ++e; }
            mant &= 0x3FFu;
            bits = (sign << 31) | ((113u - e) << 23) | (mant << 13);
        }
    } else if (ex == 31u) {
        bits = (sign << 31) | 0x7F800000u | (mant << 13);       // Inf/NaN
    } else {
        bits = (sign << 31) | ((ex + 112u) << 23) | (mant << 13);
    }
    float fv;
    std::memcpy(&fv, &bits, 4);
    return fv;
}

// 提交后读回验证：[GI_MEAN] 阶梯行 = 512 探针各向同性平均入射辐射
// 0.282095·c00 的逐通道均值（slab s = 通道 s，取 .x=c00；f16 完整解码）
// + 中央 slice(z=4) 同量 PPM（RHI_GI_DUMP=1）。reset 跳变/单调收敛证据 =
// 逐帧 [GI_MEAN] 序列。
void M6VerifyGI(RHIDeviceBase* device, M6GIState& s,
                const std::string& captureDir, u32 frame) {
    const u16* raw = static_cast<const u16*>(device->MapBuffer(s.readbackBuf));
    if (!raw) { Fail("M6 GI readback map"); return; }
    const u32 slicePitch = kM6GridDim * kM6ReadbackRowTexels;   // texels/z-slice
    // 纹素 (x,y,z) 的 c00（.x 分量）在读回缓冲中的 u16 下标
    auto c00At = [&](u32 x, u32 y, u32 z) {
        return size_t(z) * slicePitch * 4 + size_t(y) * kM6ReadbackRowTexels * 4 +
               size_t(x) * 4;
    };
    double sum[3] = {0.0, 0.0, 0.0};
    for (u32 ch = 0; ch < kM6ShSlabs; ++ch)
        for (u32 z = 0; z < kM6GridDim; ++z)
            for (u32 y = 0; y < kM6GridDim; ++y)
                for (u32 x = 0; x < kM6GridDim; ++x)
                    sum[ch] += M6HalfToFloat(raw[c00At(x, y, ch * kM6GridDim + z)]);
    const double n = double(kM6GridDim) * kM6GridDim * kM6GridDim;
    std::cerr << "[GI_MEAN] frame " << frame << " r=" << kM6ShY00 * sum[0] / n
              << " g=" << kM6ShY00 * sum[1] / n << " b=" << kM6ShY00 * sum[2] / n
              << std::endl;
    ++s.readbackFrames;

    // 中央 slice(z=4) PPM：各向同性值 0.282095·c00（HDR 贴 255 预览，同
    // RHI_HDR_DUMP 惯例；F6-6 中间输出定位）
    static const bool s_dump = [] {
        const char* d = std::getenv("RHI_GI_DUMP");
        return d && d[0] == '1';
    }();
    if (s_dump) {
        const std::string p = captureDir + "/gi_grid_" + std::to_string(frame) + ".ppm";
        std::ofstream pf(p, std::ios::binary);
        pf << "P6\n" << kM6GridDim << " " << kM6GridDim << "\n255\n";
        for (u32 y = 0; y < kM6GridDim; ++y)
            for (u32 x = 0; x < kM6GridDim; ++x) {
                u8 rgb[3];
                for (u32 ch = 0; ch < kM6ShSlabs; ++ch) {
                    const float v = float(kM6ShY00 *
                        M6HalfToFloat(raw[c00At(x, y, ch * kM6GridDim + 4u)]));
                    rgb[ch] = v <= 0.0f ? 0 : (v >= 1.0f ? 255 : u8(v * 255.0f + 0.5f));
                }
                pf.write(reinterpret_cast<const char*>(rgb), 3);
            }
        ++s.dumpCount;
    }
    device->UnmapBuffer(s.readbackBuf);
}

// ======================= M5 GPU-driven 段（F5-1/F5-2/F5-4）=======================

// M5Cull.comp 的 CPU 镜像布局（std430/std140 与 shader 声明一源）。
struct M5InstanceGPU {          // 32B std430
    math::v4 sphere;            // xyz=包围球心（model=identity→世界=局部）w=半径
    u32 indexCount;
    u32 firstIndex;
    u32 vertexOffset;
    u32 firstInstance;
};
static_assert(sizeof(M5InstanceGPU) == 32, "M5Instance std430 = 16 + 16");
static_assert(offsetof(M5InstanceGPU, indexCount) == 16, "M5Instance meta @16");

struct M5CullParamsGPU {        // 128B std140
    math::v4 planes[6];         // 归一化视锥平面（Gribb-Hartmann）
    math::v4 camMaxDist;        // xyz=相机 w=最大绘制距离
    math::v4 counts;            // x=meshCount y=resetMode
};
static_assert(sizeof(M5CullParamsGPU) == 128, "M5CullParams std140 = 96+16+16");

// M5Cull.comp 谓词 CPU 复算——与 GLSL 逐操作同序（dot 4 分量同序、平方
// 范数对比无 sqrt），供存活数交叉断言（剔除=纯几何谓词的可复核性证据）。
std::array<math::v4, 6> M5ExtractFrustumPlanes(const math::m4x4& vp) {
    // 行 i（列主序 columns[]）= (columns[0][i], columns[1][i], columns[2][i],
    // columns[3][i])；L=row3+row0 R=row3-row0 B=row3+row1 T=row3-row1
    // N=row3+row2 F=row3-row2；归一化=4 分量长度（镜像 GPUCullingPipeline
    // mkPlane 惯例）。GPU 只做点积 → 约定差异零风险。
    auto row = [&vp](int i) {
        return math::v4{math::elem(vp, 0, i), math::elem(vp, 1, i),
                        math::elem(vp, 2, i), math::elem(vp, 3, i)};
    };
    const math::v4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    auto mk = [](const math::v4& a, float sign, const math::v4& b) {
        math::v4 p{a.x + sign * b.x, a.y + sign * b.y, a.z + sign * b.z,
                   a.w + sign * b.w};
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z +
                                    p.w * p.w);
        if (len > 1e-8f) {
            p.x /= len; p.y /= len; p.z /= len; p.w /= len;
        }
        return p;
    };
    return {mk(r3, 1.f, r0), mk(r3, -1.f, r0), mk(r3, 1.f, r1),
            mk(r3, -1.f, r1), mk(r3, 1.f, r2), mk(r3, -1.f, r2)};
}

bool M5CullPredicateCPU(const M5InstanceGPU& inst, const M5CullParamsGPU& p) {
    // 与 M5Cull.comp main() 同序：任一平面 dot(plane.xyz,c)+plane.w < -r 剔除
    for (int i = 0; i < 6; ++i) {
        const float d = p.planes[i].x * inst.sphere.x +
                        p.planes[i].y * inst.sphere.y +
                        p.planes[i].z * inst.sphere.z + p.planes[i].w;
        if (d < -inst.sphere.w) return false;
    }
    // 距离：平方范数对比（两端同式，无 sqrt）
    const float dx = inst.sphere.x - p.camMaxDist.x;
    const float dy = inst.sphere.y - p.camMaxDist.y;
    const float dz = inst.sphere.z - p.camMaxDist.z;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    const float rmax = p.camMaxDist.w + inst.sphere.w;
    return dist2 <= rmax * rmax;
}

struct M5GpuDrivenState {
    ShaderHandle cs{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle setLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle layout{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle pipeline{handles::INVALID_PIPELINE};
    // 两档描述符集（共享 inst/args/counter/params，仅 flags 不同——
    // flags 恒定值 init 期一次写入，永不再改：reset/cull 两次 dispatch 的
    // UBO 内容切换零宿主竞态）。
    DescriptorSetHandle setReset{handles::INVALID_DESCRIPTOR_SET};
    DescriptorSetHandle setCull{handles::INVALID_DESCRIPTOR_SET};
    ResourceHandle instBuf{handles::INVALID_RESOURCE};      // 393×32B 只读
    ResourceHandle argsBuf{handles::INVALID_RESOURCE};      // 393×20B VkDrawIndexedIndirectCommand
    ResourceHandle counterBuf{handles::INVALID_RESOURCE};   // u32 存活计数
    ResourceHandle paramsBuf{handles::INVALID_RESOURCE};    // 128B UBO
    ResourceHandle flagsResetBuf{handles::INVALID_RESOURCE};// 16B UBO 恒 {1,0,0,0}
    ResourceHandle flagsCullBuf{handles::INVALID_RESOURCE}; // 16B UBO 恒 {0,0,0,0}
    ResourceHandle readbackBuf{handles::INVALID_RESOURCE};  // readback: args+counter
    u32 meshCount{0};
    float maxDist{250.0f};
    std::vector<M5InstanceGPU> cpuInst;   // 上传同源 CPU 镜像（谓词复算输入）
    // 验证账本（报告量化）：全通过帧数 / 有剔除帧数 / 断言失败数
    u32 framesAllPass{0}, framesCulled{0}, assertFails{0};
};

bool M5GpuDrivenInit(RHIDeviceBase* device, M5GpuDrivenState& s,
                     bool wantD3D12, const std::vector<MeshEntry>& meshes,
                     std::string& err) {
    const char* compRel = wantD3D12
        ? "EngineTest/shaders/D3D12/Forward/M5Cull.comp.cso"
        : "Engine/Graphics/Vulkan/shaders/Forward/M5Cull.comp.spv";
    std::vector<u8> cb = ReadFileRel(compRel);
    if (cb.empty()) {
        err = std::string("M5Cull shader bytes missing (") + compRel + ")";
        return false;
    }
    s.cs = device->CreateShader(cb.data(), cb.size(), ShaderStage::Compute, "main");
    if (s.cs == handles::INVALID_SHADER) { err = "M5Cull CreateShader"; return false; }
    g_ledger.shaders.insert(s.cs);

    DescriptorSetLayoutBinding b[5];
    // b0 readonly 必须显式声明：M5Cull.comp 的 inst 是 `readonly buffer`
    //（GLSL 存储限定符），spirv-cross HLSL 链产出 ByteAddressBuffer
    // t0(space0) = SRV；D3D12 ClassifyBinding 按 readonly 选 SRV/UAV range
    //——缺省 false 会生成 UAV u0，根签名零 SRV range，Windows 十八测 #882
    //（CS_ROOT_SIGNATURE_MISMATCH: SRV t0 未绑定）即此。b2/b3 是
    // writeonly/coherent 写类（u2/u3 UAV），保持缺省。Vulkan/Metal 不消费
    // readonly（仅 D3D12/Dawn），此位为后端中立修饰。
    b[0] = {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute};  // inst
    b[0].readonly = true;                                                 // readonly load → t0 SRV
    b[1] = {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};  // params
    b[2] = {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute};  // args
    b[3] = {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute};  // counter
    b[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};  // flags
    DescriptorSetLayoutDesc dld{};
    dld.bindingCount = 5;
    dld.bindings = b;
    s.setLayout = device->CreateDescriptorSetLayout(dld);
    PipelineLayoutDesc pld{};
    pld.setLayoutCount = 1;
    pld.setLayouts = &s.setLayout;
    s.layout = device->CreatePipelineLayout(pld);
    if (s.setLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        s.layout == handles::INVALID_PIPELINE_LAYOUT) {
        err = "M5Cull layouts";
        return false;
    }
    g_ledger.setLayouts.insert(s.setLayout);
    g_ledger.layouts.insert(s.layout);

    ComputePipelineDesc cpd{};
    cpd.computeShader = s.cs;
    cpd.layout = s.layout;
    cpd.threadGroupSize = {64, 1, 1};
    s.pipeline = device->CreateComputePipeline(cpd);
    if (s.pipeline == handles::INVALID_PIPELINE) { err = "M5Cull pipeline"; return false; }
    g_ledger.pipelines.insert(s.pipeline);

    // 逐 mesh 实例数据：包围球（AABB 外接球，model=identity）+ 索引绘制
    // 静态元数据。firstIndex/vertexOffset 恒 0（逐 mesh 独立 index/vertex
    // buffer，绑定后再 draw——共同切片形态；生产 merged 全局缓冲见盘点表）。
    s.meshCount = u32(meshes.size());
    std::vector<M5InstanceGPU> inst(s.meshCount);
    for (u32 m = 0; m < s.meshCount; ++m) {
        RenderMesh* mesh = meshes[m].mesh;
        if (mesh->GetIndexCount() == 0) {
            err = "M5: non-indexed mesh in GPUDRIVEN slice (m=" +
                  std::to_string(m) + ")";
            return false;
        }
        const rhi::AABB& bb = mesh->GetLocalAABB();
        math::v3 mn{bb.min.x, bb.min.y, bb.min.z}, mx{bb.max.x, bb.max.y, bb.max.z};
        math::v3 c{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
        math::v3 e{mx.x - c.x, mx.y - c.y, mx.z - c.z};
        inst[m].sphere = math::v4{c.x, c.y, c.z,
                                  std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z)};
        inst[m].indexCount = mesh->GetIndexCount();
        inst[m].firstIndex = 0;
        inst[m].vertexOffset = 0;
        inst[m].firstInstance = 0;   // =0 直绘路径 startInstance=0 逐位对齐
    }

    BufferDesc bd{};
    bd.type = BufferType::Structured;
    bd.usage = GPUMemoryUsage::Static;
    bd.memoryUsage = GPUMemoryUsage::Static;
    bd.size = u64(s.meshCount) * sizeof(M5InstanceGPU);
    bd.name = "M5Instances";
    s.instBuf = device->CreateBuffer(bd);
    bd.type = BufferType::Indirect;
    bd.usage = GPUMemoryUsage::Static;
    bd.size = u64(s.meshCount) * 20;   // VkDrawIndexedIndirectCommand×N
    // R19-1：UAV 写目标显式 Storage 位（引擎惯例=GPUDrivenDrawPipeline 的
    // Indirect|Storage）——D3D12Buffer 据此路由 ALLOW_UNORDERED_ACCESS
    // （CreateUnorderedAccessView 契约）；Vulkan 侧据此补 STORAGE_BUFFER
    // usage（原 Indirect 型缺此位 = 基线日志 4 条 pDescriptorWrites
    // STORAGE_BUFFER validation error 的根因）。
    bd.bindFlags = static_cast<u32>(BufferUsageFlags::Indirect) |
                   static_cast<u32>(BufferUsageFlags::Storage);
    bd.name = "M5DrawArgs";
    s.argsBuf = device->CreateBuffer(bd);
    bd.type = BufferType::Raw;
    bd.size = 4;
    bd.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);  // 同上（UAV 写目标）
    bd.name = "M5VisibleCounter";
    s.counterBuf = device->CreateBuffer(bd);
    bd.type = BufferType::Constant;
    bd.usage = GPUMemoryUsage::Dynamic;
    bd.memoryUsage = GPUMemoryUsage::Dynamic;
    bd.bindFlags = 0;   // UBO 无 storage 语义（清掉上位残留）
    bd.size = sizeof(M5CullParamsGPU);
    bd.name = "M5CullParams";
    s.paramsBuf = device->CreateBuffer(bd);
    bd.size = 16;
    bd.name = "M5CullFlagsReset";
    s.flagsResetBuf = device->CreateBuffer(bd);
    bd.name = "M5CullFlagsCull";
    s.flagsCullBuf = device->CreateBuffer(bd);
    bd.type = BufferType::Raw;
    // R19-1：同 M1Readback——CopyBuffer 的 GPU 写目标须 Readback 位。
    bd.usage = GPUMemoryUsage::Readback;
    bd.memoryUsage = GPUMemoryUsage::Readback;
    bd.size = u64(s.meshCount) * 20 + 16;   // args + counter（16B 对齐尾）
    bd.name = "M5Readback";
    s.readbackBuf = device->CreateBuffer(bd);
    if (s.instBuf == handles::INVALID_RESOURCE || s.argsBuf == handles::INVALID_RESOURCE ||
        s.counterBuf == handles::INVALID_RESOURCE || s.paramsBuf == handles::INVALID_RESOURCE ||
        s.flagsResetBuf == handles::INVALID_RESOURCE ||
        s.flagsCullBuf == handles::INVALID_RESOURCE ||
        s.readbackBuf == handles::INVALID_RESOURCE) {
        err = "M5 buffers";
        return false;
    }
    g_ledger.buffers.insert(s.instBuf);
    g_ledger.buffers.insert(s.argsBuf);
    g_ledger.buffers.insert(s.counterBuf);
    g_ledger.buffers.insert(s.paramsBuf);
    g_ledger.buffers.insert(s.flagsResetBuf);
    g_ledger.buffers.insert(s.flagsCullBuf);
    g_ledger.buffers.insert(s.readbackBuf);
    if (!device->UpdateBufferData(s.instBuf, inst.data(),
                                  u64(s.meshCount) * sizeof(M5InstanceGPU), 0)) {
        err = "M5 instance upload";
        return false;
    }
    s.cpuInst = std::move(inst);
    // flags 恒定值：init 期一次写入（reset 档=1 / cull 档=0），永不再改
    const u32 kResetOn[4] = {1u, 0u, 0u, 0u};
    const u32 kResetOff[4] = {0u, 0u, 0u, 0u};
    if (!device->UpdateBufferData(s.flagsResetBuf, kResetOn, 16, 0) ||
        !device->UpdateBufferData(s.flagsCullBuf, kResetOff, 16, 0)) {
        err = "M5 flags upload";
        return false;
    }

    // 两档描述符集：除 b4（flags）外全部相同
    auto writeSet = [&](DescriptorSetHandle set, ResourceHandle flagsBuf) {
        DescriptorBufferInfo bi0{}, bi1{}, bi2{}, bi3{}, bi4{};
        bi0.buffer = s.instBuf;   bi0.offset = 0; bi0.range = u64(s.meshCount) * sizeof(M5InstanceGPU);
        bi1.buffer = s.paramsBuf; bi1.offset = 0; bi1.range = sizeof(M5CullParamsGPU);
        bi2.buffer = s.argsBuf;   bi2.offset = 0; bi2.range = u64(s.meshCount) * 20;
        bi3.buffer = s.counterBuf;bi3.offset = 0; bi3.range = 4;
        bi4.buffer = flagsBuf;    bi4.offset = 0; bi4.range = 16;
        WriteDescriptorSet w[5]{};
        w[0] = {}; w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = DescriptorType::StorageBuffer; w[0].bufferInfo = &bi0;
        w[1] = {}; w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = DescriptorType::UniformBuffer; w[1].bufferInfo = &bi1;
        w[2] = {}; w[2].dstSet = set; w[2].dstBinding = 2; w[2].descriptorCount = 1;
        w[2].descriptorType = DescriptorType::StorageBuffer; w[2].bufferInfo = &bi2;
        w[3] = {}; w[3].dstSet = set; w[3].dstBinding = 3; w[3].descriptorCount = 1;
        w[3].descriptorType = DescriptorType::StorageBuffer; w[3].bufferInfo = &bi3;
        w[4] = {}; w[4].dstSet = set; w[4].dstBinding = 4; w[4].descriptorCount = 1;
        w[4].descriptorType = DescriptorType::UniformBuffer; w[4].bufferInfo = &bi4;
        device->UpdateDescriptorSets(5, w);
    };
    DescriptorSetDesc dsd{};
    dsd.layout = s.setLayout;
    s.setReset = device->CreateDescriptorSet(dsd);
    s.setCull = device->CreateDescriptorSet(dsd);
    if (s.setReset == handles::INVALID_DESCRIPTOR_SET ||
        s.setCull == handles::INVALID_DESCRIPTOR_SET) {
        err = "M5 sets";
        return false;
    }
    g_ledger.sets.insert(s.setReset);
    g_ledger.sets.insert(s.setCull);
    writeSet(s.setReset, s.flagsResetBuf);
    writeSet(s.setCull, s.flagsCullBuf);
    return true;
}

// 录帧：reset（单线程 compute 清零）→（compute→compute UAV 链，§4.4 显式
// MemoryBarrier）→ cull（单 dispatch：谓词+arg 写入+计数）→（契约 barrier）
// → InsertBarrier（arg buffer UAV→INDIRECT_ARGUMENT；Vulkan InsertBarrier
// 跳 buffer=已知债务，显式 MemoryBarrier 兜底；D3D12 侧产出官方样例转换）。
// 返回 CPU 复算预期存活数（与 GPU 同一 p 数据逐操作同序复算）。
u32 M5RecordCull(RHIDeviceBase* device, RHICommandBuffer* cmd, M5GpuDrivenState& s,
                 const math::m4x4& viewProj, const math::v3& camPos) {
    M5CullParamsGPU p{};
    {
        const std::array<math::v4, 6> planes = M5ExtractFrustumPlanes(viewProj);
        for (int i = 0; i < 6; ++i) p.planes[i] = planes[i];
    }
    p.camMaxDist = math::v4{camPos.x, camPos.y, camPos.z, s.maxDist};
    p.counts = math::v4{float(s.meshCount), 0.0f, 0.0f, 0.0f};

    // CPU 预期存活数（交叉断言基准；同 p 逐操作同序）
    u32 expected = 0;
    for (const M5InstanceGPU& i : s.cpuInst)
        if (M5CullPredicateCPU(i, p)) ++expected;
    if (std::getenv("M5_CULL_DEBUG") && expected == 0) {
        std::cerr << "[M5CULL][dbg] expected=0 cam=(" << camPos.x << ","
                  << camPos.y << "," << camPos.z << ") planes:";
        for (int i = 0; i < 6; ++i)
            std::cerr << " [" << p.planes[i].x << "," << p.planes[i].y << ","
                      << p.planes[i].z << "," << p.planes[i].w << "]";
        std::cerr << std::endl;
        for (int m = 0; m < 3 && m < int(s.cpuInst.size()); ++m) {
            const M5InstanceGPU& i = s.cpuInst[m];
            std::cerr << "[M5CULL][dbg] mesh " << m << " c=(" << i.sphere.x
                      << "," << i.sphere.y << "," << i.sphere.z << ") r="
                      << i.sphere.w << " d:";
            for (int pi = 0; pi < 6; ++pi)
                std::cerr << " " << (p.planes[pi].x * i.sphere.x +
                                     p.planes[pi].y * i.sphere.y +
                                     p.planes[pi].z * i.sphere.z + p.planes[pi].w);
            std::cerr << std::endl;
        }
    }

    // 宿主侧逐帧一次写入（params UBO；flags 两档 init 期已恒定——零竞态）
    if (!device->UpdateBufferData(s.paramsBuf, &p, sizeof(p), 0)) {
        Fail("M5 params upload");
        return expected;
    }

    // ---- stage0：单线程清零计数器（flags.resetMode=1，setReset）----
    // R19-1：args/counter 现创建带 UAV 标志（DEFAULT 堆 storage buffer，
    // CreateUnorderedAccessView 契约）——UAV 标志资源失去 simultaneous-
    // access 隐式提升，dispatch UAV 写前需显式转入 UnorderedAccess
    // （tracker 驱动：首帧 COMMON→UAV、后续帧 CopySource→UAV 均合法对；
    // Vulkan 侧 buffer barrier 跳过=零影响，§4.4 已知债务）。
    for (ResourceHandle h : {s.argsBuf, s.counterBuf}) {
        ResourceBarrier rb{};
        rb.resource = h;
        rb.beforeState = ResourceState::UnorderedAccess;  // 调用方声明（参考）
        rb.afterState = ResourceState::UnorderedAccess;
        rb.subresource = 0xFFFFFFFF;
        rb.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&rb, 1);
    }
    cmd->BindComputePipeline(s.pipeline);
    {
        const DescriptorSetHandle sets[] = {s.setReset};
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, s.layout, 0, 1, sets,
                                0, nullptr);
    }
    cmd->Dispatch(1, 1, 1);
    // compute→compute UAV 依赖（counter WAW/WAR + args 首写序）：显式
    // MemoryBarrier（rhi-barrier-semantics §1 约定①；dst 含 ShaderWrite 因
    // stage1 对 counter 是 atomic RMW=读+写）。D3D12 侧此调用转全局 UAV
    // barrier（M1 契约），与本链语义等价。
    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                       AccessFlag::ShaderWrite,
                       AccessFlag::ShaderRead | AccessFlag::ShaderWrite);

    // ---- stage1：剔除 + arg 写入 + 计数（flags=0，setCull；单 dispatch）----
    {
        const DescriptorSetHandle sets[] = {s.setCull};
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, s.layout, 0, 1, sets,
                                0, nullptr);
    }
    const u32 groups = (s.meshCount + 63) / 64;
    cmd->Dispatch(groups, 1, 1);

    // ---- 契约 barrier（阶段文档 §3 / rhi-barrier-semantics §4.4）----
    // compute 写 arg buffer/counter → DrawIndirect 消费：显式全局内存屏障
    //（ComputeShader→DrawIndirect|VertexInput，ShaderWrite→
    // IndirectCommandRead|VertexAttributeRead——镜像生产 GPUCullingPipeline
    // Execute 末尾同款屏障）。
    cmd->MemoryBarrier(PipelineStage::ComputeShader,
                       PipelineStage::DrawIndirect | PipelineStage::VertexInput,
                       AccessFlag::ShaderWrite,
                       AccessFlag::IndirectCommandRead |
                           AccessFlag::VertexAttributeRead);
    // D3D12 状态流转（arg buffer UAV→INDIRECT_ARGUMENT，官方样例模式）：
    // Vulkan InsertBarrier 对 buffer 跳过（已知债务 §4.4——上方全局屏障
    // 兜底）；D3D12 侧经 tracker 产出显式转换 op。
    {
        ResourceBarrier rb{};
        rb.resource = s.argsBuf;
        rb.beforeState = ResourceState::UnorderedAccess;
        rb.afterState = ResourceState::IndirectArgument;
        rb.subresource = 0xFFFFFFFF;
        rb.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&rb, 1);
    }
    return expected;
}

// 帧尾读回 + 交叉断言（args/counter → staging 拷贝须在 cmd End 前录制）。
void M5RecordReadback(RHICommandBuffer* cmd, M5GpuDrivenState& s) {
    // compute 写 → 复制：显式 MemoryBarrier(Compute→Transfer)（§4.4：
    // InsertBarrier 的 buffer 声明不能替代真实依赖）
    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                       AccessFlag::ShaderWrite, AccessFlag::TransferRead);
    // R21-A：读回拷贝源显式转换（args：INDIRECT_ARGUMENT→COPY_SOURCE、
    // counter：UAV→COPY_SOURCE）——D3D12 侧 tracker 权威、声明仅参考；此处
    // 声明真态以便 Vulkan/其他后端语义对照（args 前置态 = M5Execute 的
    // UAV→INDIRECT_ARGUMENT 显式转换后；counter 前置态 = compute dispatch
    // UAV 写入态）。Vulkan buffer barrier 跳过=零影响（§4.4 已知债务）。
    {
        ResourceBarrier rb{};
        rb.resource = s.argsBuf;
        rb.beforeState = ResourceState::IndirectArgument;
        rb.afterState = ResourceState::CopySource;
        rb.subresource = 0xFFFFFFFF;
        rb.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&rb, 1);
    }
    {
        ResourceBarrier rb{};
        rb.resource = s.counterBuf;
        rb.beforeState = ResourceState::UnorderedAccess;
        rb.afterState = ResourceState::CopySource;
        rb.subresource = 0xFFFFFFFF;
        rb.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&rb, 1);
    }
    cmd->CopyBuffer(s.argsBuf, s.readbackBuf, 0, 0, u64(s.meshCount) * 20);
    cmd->CopyBuffer(s.counterBuf, s.readbackBuf, 0, u64(s.meshCount) * 20, 4);
    cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                       AccessFlag::TransferWrite, AccessFlag::HostRead);
}

// 帧尾（Submit+Wait 后）断言：GPU 计数 == CPU 预期；args instanceCount 逐
// mesh 与 CPU 谓词一致（可复算性证据）。返回 GPU 存活数。
u32 M5VerifyReadback(RHIDeviceBase* device, M5GpuDrivenState& s, u32 expected,
                     const M5CullParamsGPU& p, u32 frame) {
    const u32* words = static_cast<const u32*>(device->MapBuffer(s.readbackBuf));
    if (!words) {
        Fail("M5 readback map");
        return 0;
    }
    u32 gpuCount = words[s.meshCount * 5];
    u32 argCount = 0, mismatches = 0;
    for (u32 m = 0; m < s.meshCount; ++m) {
        const bool gpuPass = words[m * 5 + 1] == 1u;
        if (gpuPass) ++argCount;
        if (gpuPass != M5CullPredicateCPU(s.cpuInst[m], p)) ++mismatches;
    }
    device->UnmapBuffer(s.readbackBuf);
    if (gpuCount != expected || argCount != expected || mismatches != 0) {
        ++s.assertFails;
        std::cerr << "[GPUDRIVEN] frame " << frame
                  << " CROSS-ASSERT FAIL: gpuCounter=" << gpuCount
                  << " argPass=" << argCount << " cpuExpected=" << expected
                  << " perMeshMismatch=" << mismatches << std::endl;
        Fail("M5 cross-assert");
    } else if (expected == s.meshCount) {
        ++s.framesAllPass;
    } else {
        ++s.framesCulled;
    }
    std::cout << "[GPUDRIVEN] frame " << frame << " gpu=" << gpuCount
              << " cpu=" << expected << "/" << s.meshCount
              << (gpuCount == expected && mismatches == 0 ? " match" : " MISMATCH")
              << std::endl;
    return gpuCount;
}

void M5GpuDrivenDestroy(RHIDeviceBase* device, M5GpuDrivenState& s) {
    for (DescriptorSetHandle h : {s.setReset, s.setCull}) {
        if (h != handles::INVALID_DESCRIPTOR_SET) {
            device->DestroyDescriptorSet(h); g_ledger.sets.erase(h);
        }
    }
    if (s.pipeline != handles::INVALID_PIPELINE) {
        device->DestroyPipeline(s.pipeline); g_ledger.pipelines.erase(s.pipeline);
    }
    if (s.layout != handles::INVALID_PIPELINE_LAYOUT) {
        device->DestroyPipelineLayout(s.layout); g_ledger.layouts.erase(s.layout);
    }
    if (s.setLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device->DestroyDescriptorSetLayout(s.setLayout);
        g_ledger.setLayouts.erase(s.setLayout);
    }
    if (s.cs != handles::INVALID_SHADER) {
        device->DestroyShader(s.cs); g_ledger.shaders.erase(s.cs);
    }
    for (ResourceHandle h : {s.instBuf, s.argsBuf, s.counterBuf, s.paramsBuf,
                             s.flagsResetBuf, s.flagsCullBuf, s.readbackBuf}) {
        if (h != handles::INVALID_RESOURCE) {
            device->DestroyBuffer(h); g_ledger.buffers.erase(h);
        }
    }
    s = M5GpuDrivenState{};
}

// ======================= M5 compute mipgen 段（F5-5，M7 F7-3 提前）=======================
// 独立小图验证（RHI_TEST_MIPGEN=1，默认 0；不并主轨迹）：8³ checker RGBA8_UINT
// → 3 级 dispatch（8→4→2→1，limit=min(mipCount,4)）→ 逐级读回断言
//（手工期望值逐字节，整数 box (sum+2)/4 公式与 M5Mipmap.comp 一源）。
bool M5RunMipgenCheck(RHIDeviceBase* device, bool wantD3D12, std::string& err) {
    // 独立小图验证（RHI_TEST_MIPGEN=1，默认 0；不并主轨迹）：8³ 列奇偶
    // checker RGBA8_UINT → 3 级 dispatch（8→4→2→1，limit=min(mipCount,4)）
    // → 逐级读回断言（手工期望值逐 word，整数 box (sum+2)/4 公式与
    // M5Mipmap.comp 一源）。
    // 共同切片形态（偏差登记）：逐级独立 1-mip 纹理对 + 全资源 barrier
    // （M1 标准流）——单纹理 per-mip 存储视图受 RHI 组合限制（SampledImage
    // 描述符 SRO 布局硬编码 + CopyTextureToBuffer 全局布局跟踪），M7 设备级
    // GenerateMipmaps 接通时统一收。
    constexpr u32 kSize = 8;                       // 8→4→2→1 = 4 级
    constexpr u32 kMipCount = 4;                   // limit=min(mipCount,4)
    const char* compRel = wantD3D12
        ? "EngineTest/shaders/D3D12/Forward/M5Mipmap.comp.cso"
        : "Engine/Graphics/Vulkan/shaders/Forward/M5Mipmap.comp.spv";
    std::vector<u8> cb = ReadFileRel(compRel);
    if (cb.empty()) {
        err = std::string("M5Mipmap shader bytes missing (") + compRel + ")";
        return false;
    }
    ShaderHandle cs = device->CreateShader(cb.data(), cb.size(),
                                           ShaderStage::Compute, "main");
    if (cs == handles::INVALID_SHADER) { err = "Mipgen CreateShader"; return false; }
    g_ledger.shaders.insert(cs);

    DescriptorSetLayoutBinding b[4];
    b[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};  // utexture2D
    b[1] = {1, DescriptorType::Sampler, 1, ShaderStage::Compute};       // 占位采样器
    b[2] = {2, DescriptorType::StorageImage, 1, ShaderStage::Compute};  // rgba8ui
    b[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute}; // srcSize
    DescriptorSetLayoutDesc dld{};
    dld.bindingCount = 4;
    dld.bindings = b;
    DescriptorSetLayoutHandle setLayout = device->CreateDescriptorSetLayout(dld);
    PipelineLayoutDesc pld{};
    pld.setLayoutCount = 1;
    pld.setLayouts = &setLayout;
    PipelineLayoutHandle layout = device->CreatePipelineLayout(pld);
    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = layout;
    cpd.threadGroupSize = {8, 8, 1};
    PipelineHandle pipeline = device->CreateComputePipeline(cpd);
    if (setLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        layout == handles::INVALID_PIPELINE_LAYOUT ||
        pipeline == handles::INVALID_PIPELINE) {
        err = "Mipgen pipeline";
        return false;
    }
    g_ledger.setLayouts.insert(setLayout);
    g_ledger.layouts.insert(layout);
    g_ledger.pipelines.insert(pipeline);

    SamplerHandle sampler = handles::INVALID_SAMPLER;
    {
        SamplerDesc sd{};
        sd.minFilter = FilterMode::Nearest;
        sd.magFilter = FilterMode::Nearest;
        sd.mipFilter = FilterMode::Nearest;
        sd.addressU = TextureAddressMode::Clamp;
        sd.addressV = TextureAddressMode::Clamp;
        sd.addressW = TextureAddressMode::Clamp;
        sd.comparisonFunc = ComparisonFunc::Never;   // MoltenVK 移植性惯例
        sampler = device->CreateSampler(sd);
        if (sampler == handles::INVALID_SAMPLER) { err = "Mipgen sampler"; return false; }
        g_ledger.samplers.insert(sampler);
    }

    // 4 张 1-mip 纹理（8/4/2/1）；[0] 上传 checker 源，[1..3] 逐级目标
    ResourceHandle texs[kMipCount]{};
    {
        TextureDesc td{};
        td.format = DataFormat::RGBA8_UInt;
        td.type = TextureType::Texture2D;
        td.mipLevels = 1;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                   TextureUsage::CopySource | TextureUsage::CopyDest;
        td.memoryUsage = GPUMemoryUsage::Static;
        td.name = "M5MipTex";
        u32 dim = kSize;
        for (u32 level = 0; level < kMipCount; ++level) {
            td.size = {dim, dim, 1};
            texs[level] = device->CreateTexture(td);
            if (texs[level] == handles::INVALID_RESOURCE) {
                err = "Mipgen texture";
                return false;
            }
            g_ledger.textures.insert(texs[level]);
            dim = (dim + 1) / 2;
        }
    }

    // R19-1 读回侧拷贝布局契约（D3D12 footprint 语义）：Offset 必须 512
    // 对齐（D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT）、RowPitch 后端按
    // align(行字节,256) 取（三后端均尊重显式 bufferRowLength：Vulkan
    // texels / Metal bytesPerRow / D3D12 RowPitch）。统一约定 = 级起点
    // 512 对齐 + 行距 256 对齐 → 三后端读回内存布局一致。
    // R21-A 注（二十测 REPORT 点名）：下方循环不递减 dim（各级均按
    // kSize=8 计行距/高度）= 既有超分配行为——实际槽位 0/2048/4096/6144、
    // 读回缓冲 6400B（递减 dim 的紧凑推演才是 0/2048/3072/3584、3840B）。
    // 读回按 mipOff 偏移比较（拷贝与下方校验段同布局同源计算），超分配
    // 不影响逐字节比较；行为保持零改动（byte-exact 读回已过门禁，不动
    // 分配逻辑）。原紧凑偏移 0,256,320,336 违 Offset 512 对齐 = 十八测
    // #849 同族。
    u64 mipOff[kMipCount]{};
    u32 mipRowPitch[kMipCount]{};
    u64 mipReadbackBytes = 0;
    {
        u32 dim = kSize;
        u64 off = 0;
        for (u32 level = 0; level < kMipCount; ++level) {
            mipRowPitch[level] = (dim * 4u + 255u) & ~255u;   // 256 对齐行距
            mipOff[level] = off;
            off += u64(mipRowPitch[level]) * dim;             // 高度 × 行距
            off = (off + 511u) & ~u64(511u);                  // 下一级 512 对齐
        }
        mipReadbackBytes = mipOff[kMipCount - 1] +
                           u64(mipRowPitch[kMipCount - 1]) * (kSize >> (kMipCount - 1));
    }
    // checker 源（列奇偶：x 偶=白 奇=黑——轴翻转不变设计，上传/读取行序
    // 约定差异不进入期望值），紧密 8×8×4。
    u8 checker[kSize * kSize * 4];
    for (u32 y = 0; y < kSize; ++y)
        for (u32 x = 0; x < kSize; ++x) {
            const u8 v = (x & 1) ? 0 : 255;
            u8* p = checker + (size_t(y) * kSize + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    // R19-1 上传侧方案 A（§28-2.6-1 转交宿主）：改走 UpdateTextureRegion
    // （紧密行数据由后端入环展开——D3D12 环侧 footprint 尺寸+512 对齐已
    // 由 R19-2 §28-2 修复合规；Vulkan/Metal 各走 updateRegion）。原
    // CommandBuffer 紧凑 staging 拷贝 = 十八测 #869 INVALIDSRCPLACEMENT
    // 宿主半边（256B 紧凑源 < required 1824B，PLACED_FOOTPRINT 无法表达
    // 32B 行距）。零后端特判；Dawn 后端该 API 默认 false → 显式报错。
    TextureRegionUpdate upRegion{};
    upRegion.width = kSize;
    upRegion.height = kSize;
    upRegion.depth = 1;
    if (!device->UpdateTextureRegion(texs[0], checker, sizeof(checker), upRegion)) {
        err = "Mipgen upload (UpdateTextureRegion)";
        return false;
    }
    ResourceHandle readback = handles::INVALID_RESOURCE;
    BufferDesc sb{};
    // R19-1：同 M1Readback——CopyTextureToBuffer 的 GPU 写目标须 Readback 位。
    sb.type = BufferType::Raw;
    sb.usage = GPUMemoryUsage::Readback;
    sb.memoryUsage = GPUMemoryUsage::Readback;
    sb.size = mipReadbackBytes;
    sb.name = "M5MipReadback";
    readback = device->CreateBuffer(sb);
    if (readback == handles::INVALID_RESOURCE) {
        err = "Mipgen readback buffer";
        return false;
    }
    g_ledger.buffers.insert(readback);

    // 每级静态专属 UBO（init 期一次写定——录制期零宿主写，杜绝同帧多
    // dispatch 的 UBO 双写竞态）：[L]={srcW,srcH,0,0}
    ResourceHandle paramsBufs[kMipCount]{};
    sb.type = BufferType::Constant;   // UBO 描述符要求（Raw=storage 用途不匹配）
    sb.usage = GPUMemoryUsage::Dynamic;
    sb.memoryUsage = GPUMemoryUsage::Dynamic;
    sb.size = 16;
    for (u32 level = 1; level < kMipCount; ++level) {
        sb.name = "M5MipParams";
        paramsBufs[level] = device->CreateBuffer(sb);
        if (paramsBufs[level] == handles::INVALID_RESOURCE) {
            err = "Mipgen params";
            return false;
        }
        g_ledger.buffers.insert(paramsBufs[level]);
        const u32 srcDim = kSize >> (level - 1);   // L1←8 L2←4 L3←2
        const u32 words[4] = {srcDim, srcDim, 0u, 0u};
        if (!device->UpdateBufferData(paramsBufs[level], words, 16, 0)) {
            err = "Mipgen params upload";
            return false;
        }
    }

    // 逐级描述符集：[L] b0=texs[L-1]（源）b2=texs[L]（目标）
    DescriptorSetHandle mipSets[kMipCount]{};
    for (u32 level = 1; level < kMipCount; ++level) {
        DescriptorSetDesc dsd{};
        dsd.layout = setLayout;
        mipSets[level] = device->CreateDescriptorSet(dsd);
        if (mipSets[level] == handles::INVALID_DESCRIPTOR_SET) {
            err = "Mipgen set";
            return false;
        }
        g_ledger.sets.insert(mipSets[level]);
        DescriptorImageInfo imgInfo{}, storeInfo{}, sampInfo{};
        imgInfo.imageView = texs[level - 1];
        imgInfo.imageLayout = ResourceState::ShaderResource;
        storeInfo.imageView = texs[level];
        storeInfo.imageLayout = ResourceState::UnorderedAccess;
        sampInfo.sampler = sampler;
        DescriptorBufferInfo bi{};
        bi.buffer = paramsBufs[level]; bi.offset = 0; bi.range = 16;
        WriteDescriptorSet w[4]{};
        w[0] = {}; w[0].dstSet = mipSets[level]; w[0].dstBinding = 0;
        w[0].descriptorCount = 1; w[0].descriptorType = DescriptorType::SampledImage;
        w[0].imageInfo = &imgInfo;
        w[1] = {}; w[1].dstSet = mipSets[level]; w[1].dstBinding = 1;
        w[1].descriptorCount = 1; w[1].descriptorType = DescriptorType::Sampler;
        w[1].imageInfo = &sampInfo;
        w[2] = {}; w[2].dstSet = mipSets[level]; w[2].dstBinding = 2;
        w[2].descriptorCount = 1; w[2].descriptorType = DescriptorType::StorageImage;
        w[2].imageInfo = &storeInfo;
        w[3] = {}; w[3].dstSet = mipSets[level]; w[3].dstBinding = 3;
        w[3].descriptorCount = 1; w[3].descriptorType = DescriptorType::UniformBuffer;
        w[3].bufferInfo = &bi;
        device->UpdateDescriptorSets(4, w);
    }

    // ---- 录制：上传源 → 3 级生成 dispatch → 4 级拷贝读回，单 cmd 一 submit ----
    CommandBufferHandle mipCmdHandle =
        device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (mipCmdHandle == handles::INVALID_COMMAND_BUFFER) { err = "Mipgen cmd"; return false; }
    g_ledger.cmds.insert(mipCmdHandle);
    RHICommandBuffer* mcmd = rhi::GetCommandBuffer(mipCmdHandle);
    auto wholeBarrier = [&](ResourceHandle tex, ResourceState after) {
        ResourceBarrier rb{};
        rb.resource = tex;
        rb.beforeState = ResourceState::ShaderResource;  // 调用方声明（参考）
        rb.afterState = after;
        rb.subresource = 0xFFFFFFFF;
        rb.queueFamily = 0xFFFFFFFF;
        mcmd->InsertBarrier(&rb, 1);
    };
    bool rec = mcmd->Begin();
    if (rec) {
        // R19-1：上传已改 init 期 UpdateTextureRegion（方案 A，见上）——
        // 录制段起点即 SR 声明（更新路径将纹理留在其 resting 布局）。
        wholeBarrier(texs[0], ResourceState::ShaderResource);   // 更新后→SR 声明
        u32 srcDim = kSize;
        for (u32 level = 1; level < kMipCount; ++level) {
            wholeBarrier(texs[level], ResourceState::UnorderedAccess);  // →UAV
            mcmd->BindComputePipeline(pipeline);
            const DescriptorSetHandle sets[] = {mipSets[level]};
            mcmd->BindDescriptorSets(PipelineBindPoint::Compute, layout, 0, 1,
                                     sets, 0, nullptr);
            const u32 dstDim = (srcDim + 1) / 2;
            mcmd->Dispatch((dstDim + 7) / 8, (dstDim + 7) / 8, 1);
            // compute→compute 依赖（§1 约定①；下一级消费本级输出）
            mcmd->MemoryBarrier(PipelineStage::ComputeShader,
                                PipelineStage::ComputeShader,
                                AccessFlag::ShaderWrite,
                                AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
            wholeBarrier(texs[level], ResourceState::ShaderResource);  // UAV→SR
            srcDim = dstDim;
        }
        // 读回 4 级（1-mip 纹理整资源拷贝：内部 SR→XFER_SRC→SR 自洽）。
        // R19-1：bufferOffset=512 对齐级起点 + 显式 bufferRowLength（texels）
        // = 256 对齐行距——满足 D3D12 footprint Offset/RowPitch 契约，且
        // Vulkan/Metal 同布局（行距 rowPitch、级间隔 mipOff）。
        u32 dim = kSize;
        for (u32 level = 0; level < kMipCount; ++level) {
            BufferTextureCopyRegion dr{};
            dr.bufferOffset = mipOff[level];
            dr.bufferRowLength = mipRowPitch[level] / 4;   // RGBA8：4B/texel
            dr.imageSubresource = {0, 0, 1};
            dr.imageExtent = {dim, dim, 1};
            mcmd->CopyTextureToBuffer(texs[level], readback, &dr, 1);
            dim = (dim + 1) / 2;
        }
        mcmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                            AccessFlag::TransferWrite, AccessFlag::HostRead);
        rec = mcmd->End();
    }
    if (!rec || !mcmd->Submit(0) || !mcmd->WaitForCompletion()) {
        err = "Mipgen submit";
        return false;
    }

    // ---- 手工期望值逐 word 断言（整数 box (sum+2)/4 与 shader 一源；
    //      拷贝读回布局 = 每 texel 4 字节紧密）----
    // L0：checker 原样（列奇偶）；L1：每 2×2 = 2 黑列 + 2 白列 → rgb
    // (510+2)/4=128，a (1020+2)/4=255；L2/3：均匀 128 输入 → (512+2)/4=128。
    const u8* got = static_cast<const u8*>(device->MapBuffer(readback));
    if (!got) { err = "Mipgen readback map"; return false; }
    u32 dim = kSize, badTexels = 0;
    for (u32 level = 0; level < kMipCount; ++level) {
        for (u32 i = 0; i < dim * dim; ++i) {
            u32 exp[4];
            if (level == 0) {
                const u32 x = i % dim;
                const u32 v = (x & 1) ? 0u : 255u;   // 列奇偶（轴翻转不变）
                exp[0] = exp[1] = exp[2] = v; exp[3] = 255u;
            } else {
                exp[0] = exp[1] = exp[2] = 128u; exp[3] = 255u;
            }
            // R19-1：按拷贝布局寻址（级起点 512 对齐 + 行距 256 对齐）：
            // texel = mipOff[level] + row*mipRowPitch[level] + col*4。
            const u64 texel = mipOff[level] +
                              (i / dim) * u64(mipRowPitch[level]) +
                              (i % dim) * 4u;
            for (u32 c = 0; c < 4; ++c)
                if (got[texel + c] != u8(exp[c])) {
                    ++badTexels;
                    if (badTexels <= 8)
                        std::cerr << "[MIPGEN][dbg] level=" << level
                                  << " texel=" << i << " ch=" << c
                                  << " got=" << int(got[texel + c])
                                  << " exp=" << exp[c] << std::endl;
                }
        }
        dim = (dim + 1) / 2;
    }
    device->UnmapBuffer(readback);

    // 清理（账本平衡）
    for (u32 level = 0; level < kMipCount; ++level) {
        if (level >= 1 && mipSets[level] != handles::INVALID_DESCRIPTOR_SET) {
            device->DestroyDescriptorSet(mipSets[level]);
            g_ledger.sets.erase(mipSets[level]);
            device->DestroyBuffer(paramsBufs[level]);
            g_ledger.buffers.erase(paramsBufs[level]);
        }
        device->DestroyTexture(texs[level]); g_ledger.textures.erase(texs[level]);
    }
    device->DestroyBuffer(readback); g_ledger.buffers.erase(readback);
    device->DestroySampler(sampler); g_ledger.samplers.erase(sampler);
    device->DestroyPipeline(pipeline); g_ledger.pipelines.erase(pipeline);
    device->DestroyPipelineLayout(layout); g_ledger.layouts.erase(layout);
    device->DestroyDescriptorSetLayout(setLayout);
    g_ledger.setLayouts.erase(setLayout);
    device->DestroyShader(cs); g_ledger.shaders.erase(cs);
    device->DestroyCommandBuffer(mipCmdHandle); g_ledger.cmds.erase(mipCmdHandle);

    if (badTexels != 0) {
        err = "Mipgen expectation mismatch: " + std::to_string(badTexels) +
              " bytes differ";
        return false;
    }
    std::cout << "[MIPGEN] PASS: 8x8 RGBA8_UINT checker -> 4-level box chain, "
                 "byte-exact (integer box (sum+2)/4)" << std::endl;
    return true;
}


// ======================= 宿主主体 =======================

struct RunConfig {
    bool wantD3D12{false};
    std::string platformName;
    std::string captureDir;
    u32 totalFrames{240};
    bool smoke{false};
    bool shadows{false};          // M2 F2-6：RHI_TEST_SHADOWS=1 启用阴影段
    bool shadowAblation{false};   // RHI_SHADOW_LIT_ALL=1：比较采样器 Always 消融
    bool post{false};             // M3 F3-2：RHI_TEST_POST=1 启用 post 段
    float postExposure{1.0f};     // 轨迹 render.post.exposure 基值
    float postEV{0.0f};           // RHI_POST_EV 覆盖（±0.5 单调性证据用）
    bool deferred{false};         // M4 F4-2：RHI_TEST_DEFERRED=1 启用 deferred 段
    bool gpudriven{false};        // M5 F5-1：RHI_TEST_GPUDRIVEN=1 启用 GPU-driven 直绘切片
    float gpudrivenMaxDist{250.0f};  // RHI_TEST_GPUDRIVEN_MAXDIST 距离剔除阈值
    bool mipgen{false};           // M5 F5-5：RHI_TEST_MIPGEN=1 独立 mipgen 验证
    bool gi{false};               // M6 F6-2：RHI_TEST_GI=1 启用 probe-grid GI 切片
    bool giLadder{true};          // 收敛读回阶梯（[GI_MEAN] 每 5 帧+重置帧+末帧）
    // M6 spec §6.5：RHI_GI_EXPORT_FRAMES=<csv> 原始浮点导出帧（DEFERRED=1 时
    // 生效，GI on/off 均可；空 = 不导出，默认路径零影响）+ 导出目录
    //（RHI_GI_EXPORT_DIR，空 = captureDir）。
    std::unordered_set<u32> giExportFrames;
    std::string giExportDir;
    std::unordered_set<u32> captureFrames;
    u32 resizeFrame{0xFFFFFFFFu}, restoreFrame{0xFFFFFFFFu};
    u32 resizeW{1600}, resizeH{900};
    Trajectory traj;
};

int RunHost(RHIDeviceBase* device, const RunConfig& cfg) {
    const Trajectory& traj = cfg.traj;
    const u32 kNoResize = 0xFFFFFFFFu;
    bool ok = true;
    // M3 F3-2：post on 时 lit pass 改写 RGBA16F HDR 中间（管线 RT 格式数组
    // 随之变更）；off 时 BGRA8 直出（M1/M2 行为不变）。
    const DataFormat litRTFormat =
        cfg.post ? DataFormat::RGBA16_Float : DataFormat::BGRA8_UNorm;

    // ---- shader 装载（platform 决定字节码；装载代码共用）----
    ShaderHandle vs = handles::INVALID_SHADER, fs = handles::INVALID_SHADER;
    {
        const char* vertRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M1Forward.vert.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M1Forward.vert.spv";
        const char* fragRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M1Forward.frag.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M1Forward.frag.spv";
        std::vector<u8> vb = ReadFileRel(vertRel), fb = ReadFileRel(fragRel);
        if (vb.empty() || fb.empty()) {
            Fail(std::string("shader bytes missing (") + vertRel + " / " + fragRel + ")");
            ok = false;
        } else {
            vs = device->CreateShader(vb.data(), vb.size(), ShaderStage::Vertex, "main");
            fs = device->CreateShader(fb.data(), fb.size(), ShaderStage::Pixel, "main");
            if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) {
                Fail("CreateShader");
                ok = false;
            } else {
                g_ledger.shaders.insert(vs);
                g_ledger.shaders.insert(fs);
            }
        }
    }

    // ---- 全局/光照 UBO ----
    ResourceHandle viewBuf = handles::INVALID_RESOURCE;
    ResourceHandle lightBuf = handles::INVALID_RESOURCE;
    if (ok) {
        BufferDesc bd{};
        bd.size = sizeof(ViewDataGPU);
        bd.type = BufferType::Constant;
        bd.memoryUsage = GPUMemoryUsage::Dynamic;
        bd.name = "M1ViewData";
        viewBuf = device->CreateBuffer(bd);
        bd.size = sizeof(LightDataGPU);
        bd.name = "M1LightData";
        lightBuf = device->CreateBuffer(bd);
        if (viewBuf == handles::INVALID_RESOURCE ||
            lightBuf == handles::INVALID_RESOURCE) {
            Fail("UBO creation");
            ok = false;
        } else {
            g_ledger.buffers.insert(viewBuf);
            g_ledger.buffers.insert(lightBuf);
            LightDataGPU ld{};
            ld.dirAndIntensity = math::v4{traj.dirToLight.x, traj.dirToLight.y,
                                          traj.dirToLight.z, traj.lightIntensity};
            ld.color = math::v4{traj.lightColor.x, traj.lightColor.y,
                                traj.lightColor.z, 1.0f};
            if (!device->UpdateBufferData(lightBuf, &ld, sizeof(ld), 0)) {
                Fail("light UBO upload");
                ok = false;
            }
        }
    }

    // ---- sampler（MoltenVK 移植性：comparisonFunc 必须 Never）----
    SamplerHandle sampler = handles::INVALID_SAMPLER;
    if (ok) {
        SamplerDesc sd{};
        sd.minFilter = FilterMode::Linear;
        sd.magFilter = FilterMode::Linear;
        sd.mipFilter = FilterMode::Linear;
        sd.addressU = TextureAddressMode::Wrap;
        sd.addressV = TextureAddressMode::Wrap;
        sd.addressW = TextureAddressMode::Wrap;
        sd.maxAnisotropy = 1;
        sd.comparisonFunc = ComparisonFunc::Never;
        sampler = device->CreateSampler(sd);
        if (sampler == handles::INVALID_SAMPLER) {
            Fail("sampler");
            ok = false;
        } else {
            g_ledger.samplers.insert(sampler);
        }
    }

    // ---- M5 F5-5 compute mipgen 独立验证（RHI_TEST_MIPGEN=1；默认 0 =
    //      行为不变。独立小图 + 自建/自毁资源，不并主轨迹）----
    if (ok && cfg.mipgen) {
        std::string mipErr;
        if (!M5RunMipgenCheck(device, cfg.wantD3D12, mipErr)) {
            Fail("M5 mipgen: " + mipErr);
            ok = false;
        }
    }

    // ---- M2 阴影段资源（F2-2/F2-3/F2-6；RHI_TEST_SHADOWS=1 时启用）----
    M2ShadowState shadow;
    if (ok && cfg.shadows) {
        // shader 装载（depth VS + lit FS；platform 决定字节码）
        const char* depthRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M2ShadowDepth.vert.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M2ShadowDepth.vert.spv";
        const char* litRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M2ForwardLit.frag.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M2ForwardLit.frag.spv";
        std::vector<u8> dvb = ReadFileRel(depthRel), lfb = ReadFileRel(litRel);
        if (dvb.empty() || lfb.empty()) {
            Fail(std::string("M2 shader bytes missing (") + depthRel + " / " + litRel + ")");
            ok = false;
        } else {
            std::string err;
            if (!M2ShadowInit(device, shadow, cfg.shadowAblation, err)) {
                Fail("M2ShadowInit: " + err);
                ok = false;
            } else {
                shadow.depthVs = device->CreateShader(dvb.data(), dvb.size(),
                                                      ShaderStage::Vertex, "main");
                shadow.litFs = device->CreateShader(lfb.data(), lfb.size(),
                                                    ShaderStage::Pixel, "main");
                if (shadow.depthVs == handles::INVALID_SHADER ||
                    shadow.litFs == handles::INVALID_SHADER) {
                    Fail("M2 CreateShader");
                    ok = false;
                } else {
                    g_ledger.shaders.insert(shadow.depthVs);
                    g_ledger.shaders.insert(shadow.litFs);
                    // depth pass 布局：set0 b0 ShadowDepthData + 64B push
                    DescriptorSetLayoutBinding db[1];
                    db[0] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex};
                    DescriptorSetLayoutDesc dld{};
                    dld.bindingCount = 1;
                    dld.bindings = db;
                    shadow.depthSetLayout = device->CreateDescriptorSetLayout(dld);
                    PushConstantRange dpush{ShaderStage::Vertex, 0, sizeof(M2ShadowPush)};
                    PipelineLayoutDesc dpld{};
                    dpld.setLayoutCount = 1;
                    dpld.setLayouts = &shadow.depthSetLayout;
                    dpld.pushConstantRangeCount = 1;
                    dpld.pushConstantRanges = &dpush;
                    shadow.depthLayout = device->CreatePipelineLayout(dpld);
                    // lit 布局：set0 3 UBO（View/Light/Shadow）+ set1 4 绑定 + 80B push
                    DescriptorSetLayoutBinding lb[3];
                    lb[0] = {0, DescriptorType::UniformBuffer, 1,
                             ShaderStage::Vertex | ShaderStage::Pixel};
                    lb[1] = {1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};
                    lb[2] = {2, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};
                    DescriptorSetLayoutDesc lgld{};
                    lgld.bindingCount = 3;
                    lgld.bindings = lb;
                    shadow.litGlobalLayout = device->CreateDescriptorSetLayout(lgld);
                    DescriptorSetLayoutBinding mb[4];
                    mb[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
                    mb[1] = {1, DescriptorType::Sampler, 1, ShaderStage::Pixel};
                    mb[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
                    mb[3] = {3, DescriptorType::Sampler, 1, ShaderStage::Pixel};
                    DescriptorSetLayoutDesc lml{};
                    lml.bindingCount = 4;
                    lml.bindings = mb;
                    shadow.litMaterialLayout = device->CreateDescriptorSetLayout(lml);
                    if (shadow.depthSetLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
                        shadow.depthLayout == handles::INVALID_PIPELINE_LAYOUT ||
                        shadow.litGlobalLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
                        shadow.litMaterialLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
                        Fail("M2 descriptor layouts");
                        ok = false;
                    } else {
                        g_ledger.setLayouts.insert(shadow.depthSetLayout);
                        g_ledger.setLayouts.insert(shadow.litGlobalLayout);
                        g_ledger.setLayouts.insert(shadow.litMaterialLayout);
                        g_ledger.layouts.insert(shadow.depthLayout);
                        // lit pipeline layout（set1 存到局部：desc 只持指针）
                        DescriptorSetLayoutHandle litSets[2] = {
                            shadow.litGlobalLayout, shadow.litMaterialLayout};
                        PushConstantRange litPush{ShaderStage::Vertex, 0,
                                                  sizeof(M1PushConsts)};
                        PipelineLayoutDesc lpld{};
                        lpld.setLayoutCount = 2;
                        lpld.setLayouts = litSets;
                        lpld.pushConstantRangeCount = 1;
                        lpld.pushConstantRanges = &litPush;
                        shadow.litLayout = device->CreatePipelineLayout(lpld);
                        if (shadow.litLayout == handles::INVALID_PIPELINE_LAYOUT) {
                            Fail("M2 lit pipeline layout");
                            ok = false;
                        } else {
                            g_ledger.layouts.insert(shadow.litLayout);
                        }
                    }
                }
            }
        }
    }

    // ---- descriptor layouts / pipeline layout（契约=M1Forward 反射契约）----
    DescriptorSetLayoutHandle globalLayout = handles::INVALID_RESOURCE;
    DescriptorSetLayoutHandle materialLayout = handles::INVALID_RESOURCE;
    PipelineLayoutHandle pipelineLayout = handles::INVALID_PIPELINE_LAYOUT;
    if (ok) {
        DescriptorSetLayoutBinding globalBindings[2];
        globalBindings[0] = {0, DescriptorType::UniformBuffer, 1,
                             ShaderStage::Vertex | ShaderStage::Pixel};
        globalBindings[1] = {1, DescriptorType::UniformBuffer, 1,
                             ShaderStage::Pixel};
        DescriptorSetLayoutDesc gld{};
        gld.bindingCount = 2;
        gld.bindings = globalBindings;
        globalLayout = device->CreateDescriptorSetLayout(gld);

        DescriptorSetLayoutBinding materialBindings[2];
        materialBindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
        materialBindings[1] = {1, DescriptorType::Sampler, 1, ShaderStage::Pixel};
        DescriptorSetLayoutDesc mld{};
        mld.bindingCount = 2;
        mld.bindings = materialBindings;
        materialLayout = device->CreateDescriptorSetLayout(mld);
        if (globalLayout == handles::INVALID_RESOURCE ||
            materialLayout == handles::INVALID_RESOURCE) {
            Fail("descriptor set layouts");
            ok = false;
        } else {
            g_ledger.setLayouts.insert(globalLayout);
            g_ledger.setLayouts.insert(materialLayout);
            DescriptorSetLayoutHandle sets[2] = {globalLayout, materialLayout};
            PushConstantRange push{ShaderStage::Vertex, 0, sizeof(M1PushConsts)};
            PipelineLayoutDesc pld{};
            pld.setLayoutCount = 2;
            pld.setLayouts = sets;
            pld.pushConstantRangeCount = 1;
            pld.pushConstantRanges = &push;
            pipelineLayout = device->CreatePipelineLayout(pld);
            if (pipelineLayout == handles::INVALID_PIPELINE_LAYOUT) {
                Fail("pipeline layout");
                ok = false;
            } else {
                g_ledger.layouts.insert(pipelineLayout);
            }
        }
    }

    // ---- M1Forward 图形管线（顶点布局=生产 32B stride）----
    PipelineHandle pipeline = handles::INVALID_PIPELINE;
    if (ok) {
        GraphicsPipelineDesc gpd{};
        gpd.vertexShader = vs;
        gpd.pixelShader = fs;
        gpd.layout = pipelineLayout;
        gpd.vertexAttributes.resize(5);
        gpd.vertexAttributes[0] = {0, 0, DataFormat::RGB32_Float, 0};   // position
        gpd.vertexAttributes[1] = {1, 0, DataFormat::R32_UInt,    12};  // colorTSign
        gpd.vertexAttributes[2] = {2, 0, DataFormat::RG16_UInt,   16};  // normal
        gpd.vertexAttributes[3] = {3, 0, DataFormat::RG16_UInt,   20};  // tangent
        gpd.vertexAttributes[4] = {4, 0, DataFormat::RG32_Float,  24};  // uv
        gpd.vertexBindings.resize(1);
        gpd.vertexBindings[0].binding = 0;
        gpd.vertexBindings[0].stride = 32;
        gpd.vertexBindings[0].perVertex = true;
        gpd.topology = PrimitiveTopology::TriangleList;
        gpd.fillMode = FillMode::Solid;
        // F2：投影 y 行取反使帧缓冲空间绕序变号，cull 对换保同一批物理面
        gpd.cullMode = CullMode::Front;
        gpd.renderTargetCount = 1;
        gpd.renderTargetFormats[0] = litRTFormat;   // M3：post on → RGBA16F HDR
        gpd.depthStencilFormat = DataFormat::D32_Float;
        gpd.enableDepthTest = true;
        gpd.enableDepthWrite = true;
        gpd.depthFunc = ComparisonFunc::Less;   // M1 §7.2-2：正向 Z 无 reverse-Z
        pipeline = device->CreateGraphicsPipeline(gpd);
        if (pipeline == handles::INVALID_PIPELINE) {
            Fail("graphics pipeline");
            ok = false;
        } else {
            g_ledger.pipelines.insert(pipeline);
        }
    }

    // ---- M2 管线：depth-only（F2-2）+ lit（F2-5 反射契约对应）----
    if (ok && cfg.shadows) {
        // depth pass：depth-only / Cull None / Less / D32 / 单 position 属性
        // （生产 ExecuteShadowRaster 同构；rasterizer bias 0/0/0——生产同值，
        // bias 全在 shader 侧 B17 参数）。
        GraphicsPipelineDesc dpd{};
        dpd.vertexShader = shadow.depthVs;
        dpd.pixelShader = handles::INVALID_SHADER;   // depth-only
        dpd.layout = shadow.depthLayout;
        dpd.vertexAttributes.resize(1);
        dpd.vertexAttributes[0] = {0, 0, DataFormat::RGB32_Float, 0};
        dpd.vertexBindings.resize(1);
        dpd.vertexBindings[0].binding = 0;
        dpd.vertexBindings[0].stride = 32;
        dpd.vertexBindings[0].perVertex = true;
        dpd.topology = PrimitiveTopology::TriangleList;
        dpd.cullMode = CullMode::None;
        dpd.renderTargetCount = 0;
        dpd.depthStencilFormat = DataFormat::D32_Float;
        dpd.enableDepthTest = true;
        dpd.enableDepthWrite = true;
        dpd.depthFunc = ComparisonFunc::Less;
        shadow.depthPipeline = device->CreateGraphicsPipeline(dpd);
        // lit pass：M1 顶点布局 + BGRA8/D32 + M2 lit FS
        GraphicsPipelineDesc lpd{};
        lpd.vertexShader = vs;
        lpd.pixelShader = shadow.litFs;
        lpd.layout = shadow.litLayout;
        lpd.vertexAttributes.resize(5);
        lpd.vertexAttributes[0] = {0, 0, DataFormat::RGB32_Float, 0};
        lpd.vertexAttributes[1] = {1, 0, DataFormat::R32_UInt, 12};
        lpd.vertexAttributes[2] = {2, 0, DataFormat::RG16_UInt, 16};
        lpd.vertexAttributes[3] = {3, 0, DataFormat::RG16_UInt, 20};
        lpd.vertexAttributes[4] = {4, 0, DataFormat::RG32_Float, 24};
        lpd.vertexBindings.resize(1);
        lpd.vertexBindings[0].binding = 0;
        lpd.vertexBindings[0].stride = 32;
        lpd.vertexBindings[0].perVertex = true;
        lpd.topology = PrimitiveTopology::TriangleList;
        // F2：投影 y 行取反使绕序变号，cull 对换（与 M1 lit 同理）
        lpd.cullMode = CullMode::Front;
        lpd.renderTargetCount = 1;
        lpd.renderTargetFormats[0] = litRTFormat;   // M3：post on → RGBA16F HDR
        lpd.depthStencilFormat = DataFormat::D32_Float;
        lpd.enableDepthTest = true;
        lpd.enableDepthWrite = true;
        lpd.depthFunc = ComparisonFunc::Less;
        shadow.litPipeline = device->CreateGraphicsPipeline(lpd);
        if (shadow.depthPipeline == handles::INVALID_PIPELINE ||
            shadow.litPipeline == handles::INVALID_PIPELINE) {
            Fail("M2 pipelines");
            ok = false;
        } else {
            g_ledger.pipelines.insert(shadow.depthPipeline);
            g_ledger.pipelines.insert(shadow.litPipeline);
            // M2 描述符集：depth set（1 UBO）+ lit global set（3 UBO）
            DescriptorSetDesc dsd{};
            dsd.layout = shadow.depthSetLayout;
            shadow.depthSet = device->CreateDescriptorSet(dsd);
            dsd.layout = shadow.litGlobalLayout;
            shadow.litGlobalSet = device->CreateDescriptorSet(dsd);
            if (shadow.depthSet == handles::INVALID_DESCRIPTOR_SET ||
                shadow.litGlobalSet == handles::INVALID_DESCRIPTOR_SET) {
                Fail("M2 descriptor sets");
                ok = false;
            } else {
                g_ledger.sets.insert(shadow.depthSet);
                g_ledger.sets.insert(shadow.litGlobalSet);
                DescriptorBufferInfo sbi{};
                sbi.buffer = shadow.shadowUbo; sbi.offset = 0;
                sbi.range = sizeof(ShadowDataGPU);
                WriteDescriptorSet sw{};
                sw.dstSet = shadow.depthSet; sw.dstBinding = 0;
                sw.dstArrayElement = 0; sw.descriptorCount = 1;
                sw.descriptorType = DescriptorType::UniformBuffer;
                sw.bufferInfo = &sbi;
                device->UpdateDescriptorSets(1, &sw);
                DescriptorBufferInfo vbi{}, lbi{}, shbi{};
                vbi.buffer = viewBuf;  vbi.offset = 0;  vbi.range = sizeof(ViewDataGPU);
                lbi.buffer = lightBuf; lbi.offset = 0; lbi.range = sizeof(LightDataGPU);
                shbi.buffer = shadow.shadowUbo; shbi.offset = 0;
                shbi.range = sizeof(ShadowDataGPU);
                WriteDescriptorSet lw[3]{};
                lw[0].dstSet = shadow.litGlobalSet; lw[0].dstBinding = 0;
                lw[0].dstArrayElement = 0; lw[0].descriptorCount = 1;
                lw[0].descriptorType = DescriptorType::UniformBuffer;
                lw[0].bufferInfo = &vbi;
                lw[1].dstSet = shadow.litGlobalSet; lw[1].dstBinding = 1;
                lw[1].dstArrayElement = 0; lw[1].descriptorCount = 1;
                lw[1].descriptorType = DescriptorType::UniformBuffer;
                lw[1].bufferInfo = &lbi;
                lw[2].dstSet = shadow.litGlobalSet; lw[2].dstBinding = 2;
                lw[2].dstArrayElement = 0; lw[2].descriptorCount = 1;
                lw[2].descriptorType = DescriptorType::UniformBuffer;
                lw[2].bufferInfo = &shbi;
                device->UpdateDescriptorSets(3, lw);
            }
        }
    }

    // ---- M3 post 段资源（F3-2/F3-3；RHI_TEST_POST=1 时启用）----
    M3PostState post;
    if (ok && cfg.post) {
        std::string err;
        if (!M3PostInit(device, post, cfg.wantD3D12, cfg.postExposure,
                        cfg.postEV, err)) {
            Fail("M3PostInit: " + err);
            ok = false;
        } else {
            std::cout << "[RHI_SHARED] M3Post: exposure="
                      << (cfg.postExposure * std::pow(2.0f, cfg.postEV))
                      << " (base " << cfg.postExposure << " × 2^" << cfg.postEV
                      << ") ACES=2.51/0.03/2.43/0.59/0.14 (production fit)"
                      << " chain=lit(RGBA16F HDR)→post(BGRA8)" << std::endl;
        }
    }

    // ---- M4 deferred 段资源（F4-2/F4-3；RHI_TEST_DEFERRED=1 且 shadows on）----
    M4DeferredState deferred4;
    if (ok && cfg.deferred) {
        // shader 装载：M4GBuffer vert+frag + M3Post.vert（全屏三角复用）+
        // M4DeferredLight.frag（platform 决定字节码）
        auto loadPair = [&](const char* vertRel, const char* fragRel,
                            ShaderHandle& vsOut, ShaderHandle& fsOut) -> bool {
            std::vector<u8> vb = ReadFileRel(vertRel), fb = ReadFileRel(fragRel);
            if (vb.empty() || fb.empty()) {
                Fail(std::string("M4 shader bytes missing (") + vertRel + " / " +
                     fragRel + ")");
                return false;
            }
            vsOut = device->CreateShader(vb.data(), vb.size(), ShaderStage::Vertex, "main");
            fsOut = device->CreateShader(fb.data(), fb.size(), ShaderStage::Pixel, "main");
            if (vsOut == handles::INVALID_SHADER || fsOut == handles::INVALID_SHADER) {
                Fail("M4 CreateShader");
                return false;
            }
            g_ledger.shaders.insert(vsOut);
            g_ledger.shaders.insert(fsOut);
            return true;
        };
        const char* gbVertRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M4GBuffer.vert.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M4GBuffer.vert.spv";
        const char* gbFragRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M4GBuffer.frag.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M4GBuffer.frag.spv";
        const char* fsVertRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M3Post.vert.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M3Post.vert.spv";
        const char* dlFragRel = cfg.wantD3D12
            ? "EngineTest/shaders/D3D12/Forward/M4DeferredLight.frag.cso"
            : "Engine/Graphics/Vulkan/shaders/Forward/M4DeferredLight.frag.spv";
        if (loadPair(gbVertRel, gbFragRel, deferred4.gbVs, deferred4.gbFs) &&
            loadPair(fsVertRel, dlFragRel, deferred4.lightVs, deferred4.lightFs)) {
            // GBuffer 管线：M1 顶点布局 + MRT×3（RGBA8/RG16F/RGBA8）+ D32
            //（布局=pipelineLayout：set0 global + set1 material + 80B push）。
            GraphicsPipelineDesc gpd{};
            gpd.vertexShader = deferred4.gbVs;
            gpd.pixelShader = deferred4.gbFs;
            gpd.layout = pipelineLayout;
            gpd.vertexAttributes.resize(5);
            gpd.vertexAttributes[0] = {0, 0, DataFormat::RGB32_Float, 0};
            gpd.vertexAttributes[1] = {1, 0, DataFormat::R32_UInt, 12};
            gpd.vertexAttributes[2] = {2, 0, DataFormat::RG16_UInt, 16};
            gpd.vertexAttributes[3] = {3, 0, DataFormat::RG16_UInt, 20};
            gpd.vertexAttributes[4] = {4, 0, DataFormat::RG32_Float, 24};
            gpd.vertexBindings.resize(1);
            gpd.vertexBindings[0].binding = 0;
            gpd.vertexBindings[0].stride = 32;
            gpd.vertexBindings[0].perVertex = true;
            gpd.topology = PrimitiveTopology::TriangleList;
            gpd.fillMode = FillMode::Solid;
            // F2：投影 y 行取反使绕序变号，cull 对换（与 M1 lit 同理）
            gpd.cullMode = CullMode::Front;
            gpd.renderTargetCount = 3;                      // F4-2 MRT
            gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
            gpd.renderTargetFormats[1] = DataFormat::RG16_Float;
            gpd.renderTargetFormats[2] = DataFormat::RGBA8_UNorm;
            gpd.depthStencilFormat = DataFormat::D32_Float;
            gpd.enableDepthTest = true;
            gpd.enableDepthWrite = true;
            gpd.depthFunc = ComparisonFunc::Less;
            deferred4.gbPipeline = device->CreateGraphicsPipeline(gpd);

            // 延迟光照布局：set0 11 绑定（M4 冻结 b0-b7 + M6 F6-2 扩展
            // b8 探针 3D 纹理/b9 tri-linear 采样器/b10 GIParams UBO——
            // 反射契约 M4DeferredLight.frag 一一对应；GI=0 时 b8 绑黑
            // 1x1x1 占位 + giScale=0，输出逐位不变）；无 push。
            // GI=1 时 M6 P2 直射 pass（M6DeferredDirect.frag）复用本布局与
            // lightSet，只消费 b0-b7（M4 light pass 不运行）。
            DescriptorSetLayoutBinding lb[11];
            lb[0] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};
            lb[1] = {1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};
            lb[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
            lb[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
            lb[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
            lb[5] = {5, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
            lb[6] = {6, DescriptorType::SampledImage, 1, ShaderStage::Pixel};
            lb[7] = {7, DescriptorType::Sampler, 1, ShaderStage::Pixel};
            lb[8] = {8, DescriptorType::SampledImage, 1, ShaderStage::Pixel};   // texture3D
            lb[9] = {9, DescriptorType::Sampler, 1, ShaderStage::Pixel};
            lb[10] = {10, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel};
            DescriptorSetLayoutDesc ld{};
            ld.bindingCount = 11;
            ld.bindings = lb;
            deferred4.lightSetLayout = device->CreateDescriptorSetLayout(ld);
            PipelineLayoutDesc pld{};
            pld.setLayoutCount = 1;
            pld.setLayouts = &deferred4.lightSetLayout;
            deferred4.lightLayout = device->CreatePipelineLayout(pld);

            // 延迟光照管线：全屏三角（无顶点输入/无深度/无 cull），单 RT=
            // litRTFormat（与 forward lit 同格式——post on→RGBA16F）。
            GraphicsPipelineDesc dpd{};
            dpd.vertexShader = deferred4.lightVs;
            dpd.pixelShader = deferred4.lightFs;
            dpd.layout = deferred4.lightLayout;
            dpd.topology = PrimitiveTopology::TriangleList;
            dpd.fillMode = FillMode::Solid;
            dpd.cullMode = CullMode::None;
            dpd.renderTargetCount = 1;
            dpd.renderTargetFormats[0] = litRTFormat;
            dpd.enableDepthTest = false;
            dpd.enableDepthWrite = false;
            deferred4.lightPipeline = device->CreateGraphicsPipeline(dpd);

            // 逐帧 UBO（DeferredView 80B + LightShadow 合并 112B）
            BufferDesc vd2{};
            vd2.size = sizeof(DeferredViewGPU);
            vd2.type = BufferType::Constant;
            vd2.memoryUsage = GPUMemoryUsage::Dynamic;
            vd2.name = "M4DeferredView";
            deferred4.viewUbo = device->CreateBuffer(vd2);
            BufferDesc ld2 = vd2;
            ld2.size = sizeof(LightShadowDataGPU);
            ld2.name = "M4LightShadow";
            deferred4.lightShadowUbo = device->CreateBuffer(ld2);

            DescriptorSetDesc dsd{};
            dsd.layout = deferred4.lightSetLayout;
            deferred4.lightSet = device->CreateDescriptorSet(dsd);
            if (deferred4.gbPipeline == handles::INVALID_PIPELINE ||
                deferred4.lightSetLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
                deferred4.lightLayout == handles::INVALID_PIPELINE_LAYOUT ||
                deferred4.lightPipeline == handles::INVALID_PIPELINE ||
                deferred4.viewUbo == handles::INVALID_RESOURCE ||
                deferred4.lightShadowUbo == handles::INVALID_RESOURCE ||
                deferred4.lightSet == handles::INVALID_DESCRIPTOR_SET) {
                Fail("M4 deferred pipelines/layouts/UBOs/sets");
                ok = false;
            } else {
                g_ledger.pipelines.insert(deferred4.gbPipeline);
                g_ledger.pipelines.insert(deferred4.lightPipeline);
                g_ledger.setLayouts.insert(deferred4.lightSetLayout);
                g_ledger.layouts.insert(deferred4.lightLayout);
                g_ledger.buffers.insert(deferred4.viewUbo);
                g_ledger.buffers.insert(deferred4.lightShadowUbo);
                g_ledger.sets.insert(deferred4.lightSet);
                std::cout << "[RHI_SHARED] M4Deferred: GBuffer(MRT3 RGBA8/RG16F/"
                             "RGBA8 + D32)→light(fullscreen, invVP depth "
                             "rebuild, Blinn-Phong+PCF3x3 same-source)"
                          << std::endl;
            }
        } else {
            ok = false;
        }
    }

    // ---- M6 GI 段资源（F6-2/F6-3；RHI_TEST_DEFERRED=1 时建 b8/b9/b10 面
    //      ——GI=0 为黑 1x1x1 占位+scale=0（默认路径逐位不变）；RHI_TEST_GI=1
    //      加 P2 直射/P3 注入/P4 accum/P5 解析/P6 合成链 + (8,8,24) RGBA16F
    //      SH 3D ping-pong 网格；探针盒在场景加载后由 mesh AABB 派生）----
    M6GIState m6gi;
    if (ok && cfg.deferred) {
        std::string m6err;
        if (!M6GIInit(device, m6gi, cfg.wantD3D12, cfg.gi, deferred4.lightLayout,
                      litRTFormat, m6err)) {
            Fail("M6 GI init: " + m6err);
            ok = false;
        } else {
            if (cfg.gi) {
                std::cout << "[GI] init: direct-only deferred input (M6DeferredDirect "
                             "MRT2 RGBA16F: direct / directDiffuse, no ambient) -> "
                             "SH-L1 inject (M6ProbeInject.comp, 1 probe/workgroup "
                             "64x1x1, screen stride " << kM6InjectStride
                          << "px, single bounce, no visibility, camera-visible "
                             "emitters only) -> EMA accum (M6ProbeAccum.comp "
                             "hist*0.9+new*0.1, reset=history-zero) -> resolve "
                             "(M6GIResolve.frag albedo*E_h(N) -> independent "
                             "indirect RGBA16F) -> composite (M6Composite.frag "
                             "direct+indirect once, before post); probe grid "
                          << kM6GridDim << "x" << kM6GridDim << "x" << kM6GridDim
                          << " SH-L1 Texture3D RGBA16F (" << kM6GridDim << "x"
                          << kM6GridDim << "x" << kM6ShSlabs * kM6GridDim
                          << ") ping-pong" << std::endl;
            }
        }
    }

    // ---- 全局描述符集 ----
    DescriptorSetHandle globalSet = handles::INVALID_RESOURCE;
    if (ok) {
        DescriptorSetDesc dsd{};
        dsd.layout = globalLayout;
        globalSet = device->CreateDescriptorSet(dsd);
        if (globalSet == handles::INVALID_RESOURCE) {
            Fail("global descriptor set");
            ok = false;
        } else {
            g_ledger.sets.insert(globalSet);
            DescriptorBufferInfo viewInfo{}, lightInfo{};
            viewInfo.buffer = viewBuf;  viewInfo.offset = 0;  viewInfo.range = sizeof(ViewDataGPU);
            lightInfo.buffer = lightBuf; lightInfo.offset = 0; lightInfo.range = sizeof(LightDataGPU);
            WriteDescriptorSet writes[2]{};
            writes[0].dstSet = globalSet; writes[0].dstBinding = 0;
            writes[0].dstArrayElement = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::UniformBuffer;
            writes[0].bufferInfo = &viewInfo;
            writes[1].dstSet = globalSet; writes[1].dstBinding = 1;
            writes[1].dstArrayElement = 0; writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::UniformBuffer;
            writes[1].bufferInfo = &lightInfo;
            device->UpdateDescriptorSets(2, writes);
        }
    }

    // ---- Sponza 场景加载（同源资产 + 逐 mesh albedo）----
    std::vector<MeshEntry> meshes;
    primal::utl::vector<SceneDataMeshInfo> sceneInfos;
    if (ok) {
        std::vector<u8> modelBytes = ReadFileRel(traj.assetPath);
        if (modelBytes.empty()) {
            Fail("asset not found: " + traj.assetPath);
            ok = false;
        } else {
            SceneDataAdapter adapter;
            sceneInfos = adapter.LoadRenderItemData(device, modelBytes.data(),
                                                    u32(modelBytes.size()));
            if (sceneInfos.empty()) {
                Fail("LoadRenderItemData returned 0 meshes");
                ok = false;
            }
        }
    }
    if (ok) {
        std::cout << "[RHI_SHARED] scene: " << sceneInfos.size()
                  << " meshes from " << traj.assetPath << std::endl;
        CommandBufferHandle loadCmdHandle =
            device->CreateCommandBuffer(CommandQueueType::Graphics);
        if (loadCmdHandle == handles::INVALID_COMMAND_BUFFER) {
            Fail("load command buffer");
            ok = false;
        } else {
            g_ledger.cmds.insert(loadCmdHandle);
            RHICommandBuffer* loadCmd = rhi::GetCommandBuffer(loadCmdHandle);
            // 资产基目录：锚定到实际命中的模型路径（ctest 与手工运行的 cwd
            // 不同，纹理目录不能按 cwd 相对解析——否则全部静默回退 1x1 白）。
            std::string modelPath = FindFileRel(traj.assetPath);
            Require(!modelPath.empty(), "asset path re-resolution");
            std::string assetBase = modelPath;
            const auto slash = assetBase.find_last_of('/');
            assetBase = slash == std::string::npos ? "" : assetBase.substr(0, slash + 1);

            static const unsigned char kWhite1x1[4] = {255, 255, 255, 255};
            u32 texLoaded = 0, texFallback = 0, meshSkipped = 0;
            for (auto& info : sceneInfos) {
                if (!info.mesh || !info.mesh->IsValid()) {
                    ++meshSkipped;
                    continue;
                }
                MeshEntry e;
                e.mesh = info.mesh;
                int tw = 0, th = 0;
                unsigned char* pixels = nullptr;
                std::string texPath =
                    ResolveTexturePath(assetBase, info.diffuseTexturePath);
                if (!texPath.empty())
                    pixels = stbi_load(texPath.c_str(), &tw, &th, nullptr, 4);
                if (pixels && tw > 0 && th > 0) {
                    e.albedoTex = CreateTextureFromPixels(device, loadCmd, pixels,
                                                          u32(tw), u32(th));
                    stbi_image_free(pixels);
                    ++texLoaded;
                } else {
                    if (pixels) stbi_image_free(pixels);
                    e.albedoTex = CreateTextureFromPixels(device, loadCmd,
                                                          kWhite1x1, 1, 1);
                    ++texFallback;
                }
                if (e.albedoTex == handles::INVALID_RESOURCE) {
                    Fail("albedo texture for mesh " + info.name);
                    ok = false;
                    break;
                }
                DescriptorSetDesc msd{};
                msd.layout = materialLayout;
                e.materialSet = device->CreateDescriptorSet(msd);
                if (e.materialSet == handles::INVALID_RESOURCE) {
                    Fail("material set for mesh " + info.name);
                    device->DestroyTexture(e.albedoTex);
                    g_ledger.textures.erase(e.albedoTex);
                    ok = false;
                    break;
                }
                g_ledger.sets.insert(e.materialSet);
                DescriptorImageInfo imgInfo{}, sampInfo{};
                imgInfo.imageView = e.albedoTex;
                imgInfo.imageLayout = ResourceState::ShaderResource;
                sampInfo.sampler = sampler;
                WriteDescriptorSet mw[2]{};
                mw[0].dstSet = e.materialSet; mw[0].dstBinding = 0;
                mw[0].dstArrayElement = 0; mw[0].descriptorCount = 1;
                mw[0].descriptorType = DescriptorType::SampledImage;
                mw[0].imageInfo = &imgInfo;
                mw[1].dstSet = e.materialSet; mw[1].dstBinding = 1;
                mw[1].dstArrayElement = 0; mw[1].descriptorCount = 1;
                mw[1].descriptorType = DescriptorType::Sampler;
                mw[1].imageInfo = &sampInfo;
                device->UpdateDescriptorSets(2, mw);
                // M2 lit 材质集：albedo 对 + shadow map 对（比较采样器）
                if (cfg.shadows) {
                    DescriptorSetDesc lmsd{};
                    lmsd.layout = shadow.litMaterialLayout;
                    e.litMaterialSet = device->CreateDescriptorSet(lmsd);
                    if (e.litMaterialSet == handles::INVALID_DESCRIPTOR_SET) {
                        Fail("lit material set for mesh " + info.name);
                        device->DestroyTexture(e.albedoTex);
                        g_ledger.textures.erase(e.albedoTex);
                        ok = false;
                        break;
                    }
                    g_ledger.sets.insert(e.litMaterialSet);
                    DescriptorImageInfo lAlbedo{}, lAlbedoS{}, lShadow{}, lShadowS{};
                    lAlbedo.imageView = e.albedoTex;
                    lAlbedo.imageLayout = ResourceState::ShaderResource;
                    lAlbedoS.sampler = sampler;
                    lShadow.imageView = shadow.shadowMap;
                    lShadow.imageLayout = ResourceState::ShaderResource;
                    lShadowS.sampler = shadow.shadowSampler;
                    WriteDescriptorSet lmw[4]{};
                    lmw[0].dstSet = e.litMaterialSet; lmw[0].dstBinding = 0;
                    lmw[0].dstArrayElement = 0; lmw[0].descriptorCount = 1;
                    lmw[0].descriptorType = DescriptorType::SampledImage;
                    lmw[0].imageInfo = &lAlbedo;
                    lmw[1].dstSet = e.litMaterialSet; lmw[1].dstBinding = 1;
                    lmw[1].dstArrayElement = 0; lmw[1].descriptorCount = 1;
                    lmw[1].descriptorType = DescriptorType::Sampler;
                    lmw[1].imageInfo = &lAlbedoS;
                    lmw[2].dstSet = e.litMaterialSet; lmw[2].dstBinding = 2;
                    lmw[2].dstArrayElement = 0; lmw[2].descriptorCount = 1;
                    lmw[2].descriptorType = DescriptorType::SampledImage;
                    lmw[2].imageInfo = &lShadow;
                    lmw[3].dstSet = e.litMaterialSet; lmw[3].dstBinding = 3;
                    lmw[3].dstArrayElement = 0; lmw[3].descriptorCount = 1;
                    lmw[3].descriptorType = DescriptorType::Sampler;
                    lmw[3].imageInfo = &lShadowS;
                    device->UpdateDescriptorSets(4, lmw);
                }
                meshes.push_back(e);
                g_ledger.meshCount++;
                info.mesh = nullptr;  // 所有权移交 meshes（避免双删）
            }
            device->DestroyCommandBuffer(loadCmdHandle);
            g_ledger.cmds.erase(loadCmdHandle);
            if (meshes.empty()) {
                Fail("no drawable meshes");
                ok = false;
            } else if (texLoaded == 0) {
                // 纹理全回退 = 路径解析断裂（曾在 ctest cwd 下发生）——按失败
                // 处理，不允许静默渲染无贴图画面混过非黑校验。
                Fail("0 textures loaded (texture path resolution broken; assetBase="
                     + assetBase + ")");
                ok = false;
            } else {
                std::cout << "[RHI_SHARED] textures: " << texLoaded
                          << " loaded, " << texFallback
                          << " fallback(1x1 white); drawable meshes="
                          << meshes.size()
                          << (meshSkipped ? " (skipped " + std::to_string(meshSkipped) + ")"
                                          : "")
                          << std::endl;
            }
        }
    }

    // ---- M5 F5-1/F5-2：GPU-driven 直绘切片资源（RHI_TEST_GPUDRIVEN=1；
    //      默认 0 = M1-M4 行为不变。剔除 compute + DrawIndexedIndirect arg
    //      buffer + 计数 + 交叉断言读回）----
    M5GpuDrivenState m5gd;
    if (ok && cfg.gpudriven) {
        m5gd.maxDist = cfg.gpudrivenMaxDist;
        std::string m5err;
        if (!M5GpuDrivenInit(device, m5gd, cfg.wantD3D12, meshes, m5err)) {
            Fail("M5 gpu-driven init: " + m5err);
            ok = false;
        } else {
            std::cout << "[GPUDRIVEN] init: meshes=" << m5gd.meshCount
                      << " maxDist=" << m5gd.maxDist
                      << " argBuffer=" << m5gd.meshCount << "x20B (VkDrawIndexed"
                         "IndirectCommand layout)"
                      << std::endl;
        }
    }

    // ---- M6 探针盒（spec §4；RHI_TEST_GI=1）：mesh AABB 并集外扩 0.5m →
    //      s.boxMin/cell/normalBias + ResolveParams 一次上传 + [GI] grid box 行 ----
    if (ok && cfg.gi) {
        std::string boxErr;
        if (!M6ComputeGridBox(device, m6gi, meshes, boxErr)) {
            Fail("M6 GI grid box: " + boxErr);
            ok = false;
        }
    }

    // ---- 渲染主循环 ----
    if (ok) {
        FrameTargets ft;
        u32 curW = traj.width, curH = traj.height;
        if (!CreateFrameTargets(device, ft, curW, curH, cfg.post, cfg.deferred,
                                cfg.gi)) {
            Fail("frame targets");
            ok = false;
        } else {
            // M3：HDR 纹理就绪后回填 post 描述符集（resize 重建后同路重绑）。
            if (cfg.post) BindM3PostSet(device, post, ft.hdr);
            // M4：GBuffer 纹理/深度句柄就绪后回填延迟光照集（同路重绑）。
            if (cfg.deferred) BindM4DeferredSet(device, deferred4, ft, shadow, m6gi);
            CommandBufferHandle cmdHandle =
                device->CreateCommandBuffer(CommandQueueType::Graphics);
            if (cmdHandle == handles::INVALID_COMMAND_BUFFER) {
                Fail("command buffer");
                ok = false;
            } else {
                g_ledger.cmds.insert(cmdHandle);
                RHICommandBuffer* cmd = rhi::GetCommandBuffer(cmdHandle);

                u32 blackFrames = 0;
                // M6（GI on）：颜色暗帧计数（post 输出非黑<2% 但几何覆盖≥2%——
                // 单帧在直射-only 语义下合法；全部帧皆暗则判失败，见汇总段）
                // + 最小几何覆盖率（GI 黑帧守卫观测量）。
                u32 giDarkFrames = 0;
                double giMinCoverage = 1.0;
                double litRatioSum = 0.0;
                double lumSum = 0.0;   // M2：平均亮度（阴影 A/B 消融对照用）
                // R20-B 黑帧分布采集（十九测硬件 M1 159/240 无分布数据——
                // 二十测裁决"resize 集中 vs 随机交错"的证据）：帧号列表 +
                // 逐帧非黑占比；RHI_SHARED_LUM_TRACE=1 时逐帧打印。
                std::vector<u32> blackFrameList;
                std::vector<double> frameLitRatio;
                std::vector<double> frameMeanLum;
                frameLitRatio.reserve(cfg.totalFrames);
                frameMeanLum.reserve(cfg.totalFrames);
                const bool lumTrace = [] {
                    const char* v = std::getenv("RHI_SHARED_LUM_TRACE");
                    return v && v[0] == '1';
                }();
                // M3：消融统计（POST=1 vs POST=0 / EV±0.5 单调性证据）——
                // Rec.709 luma 256 bin 直方图（帧均值汇总）+ 贴白像素计数
                //（>=254：POST=0 硬裁剪签名 vs POST=1 ACES 肩部滚降）。
                u64 histAll[256] = {};
                u64 clipWhiteAll = 0, pxAll = 0;
                // M3 F3-5：HDR 中间读回（RHI_HDR_DUMP=1 首帧，同 M2
                // RHI_SHADOW_DUMP 先例）——post 链中间输出可定位。
                ResourceHandle hdrDumpBuf{handles::INVALID_RESOURCE};
                bool hdrDumpDone = false;
                // RHI_SHADOW_DUMP=1 的 shadow map 读回状态
                ResourceHandle shadowDumpBuf{handles::INVALID_RESOURCE};
                bool shadowDumpDone = false;
                // M4 F4-6：GBuffer 三 RT + 深度读回（RHI_GBUFFER_DUMP=1 首帧）
                //——[0]=albedo RGBA8 [1]=normal RG16F [2]=material RGBA8
                // [3]=depth R32F。
                ResourceHandle gbDumpBufs[4] = {
                    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE,
                    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
                bool gbDumpDone = false;
                // M6 spec §6.5：RHI_GI_EXPORT_FRAMES 原始浮点导出（DEFERRED=1；
                // 逐导出帧建/毁 Readback 缓冲，账本平衡）。录制期登记，提交+
                // 等待后落盘 giexp_f<N>_<name>.bin + giexp_f<N>.json 清单。
                struct GIExportBuf {
                    std::string name;        // 文件名片段（direct / depth / ...）
                    std::string format;      // RHI 格式名（清单）
                    std::string semantics;   // 清单：通道语义
                    std::string units;       // 清单：单位
                    ResourceHandle buf{handles::INVALID_RESOURCE};
                    // rowPitch = GPU 读回行距（字节，256 对齐）；文件落盘为紧密行
                    //（每行 w*bpp 字节），清单 row_pitch = w*bpp、readback_row_pitch = rowPitch。
                    u32 bpp{0}, rowPitch{0}, w{0}, h{0}, d{0};   // d>0 = 3D
                };
                std::vector<GIExportBuf> giExports;   // 本帧待落盘
                const std::string giExportDir =
                    cfg.giExportDir.empty() ? cfg.captureDir : cfg.giExportDir;
                if (cfg.deferred && !cfg.giExportFrames.empty()) {
                    std::error_code dirEc;
                    std::filesystem::create_directories(giExportDir, dirEc);
                }
                // 录制一条导出拷贝：tex（当前处于 restState）→ CopySource → 新建
                // Readback 缓冲 → 还原 restState（同既有 dump 的 SR→Copy→SR 惯例）。
                // 2D：rowPitch = align(W*bpp, 256)，bufferRowLength = rowPitch/bpp
                //（Vulkan/D3D12 同布局）；3D（probe_sh）：rowLength 32 texel、
                // imageHeight=h。GPU 侧行距只服务拷贝契约；落盘去填充（紧密行）。
                auto recordGIExport = [&](RHICommandBuffer* rc, ResourceHandle tex,
                                          ResourceState restState, const char* name,
                                          const char* format, u32 bpp, u32 w, u32 h,
                                          u32 d, const char* semantics,
                                          const char* units) {
                    GIExportBuf e;
                    e.name = name; e.format = format; e.semantics = semantics;
                    e.units = units; e.bpp = bpp; e.w = w; e.h = h; e.d = d;
                    e.rowPitch = d == 0 ? ((w * bpp + 255u) & ~255u)
                                        : kM6ReadbackRowTexels * bpp;
                    BufferDesc bd{};
                    bd.size = u64(e.rowPitch) * h * (d == 0 ? 1u : d);
                    bd.type = BufferType::Raw;
                    // R19-1：同 M1Readback——CopyTextureToBuffer 的 GPU 写目标须 Readback 位。
                    bd.usage = GPUMemoryUsage::Readback;
                    bd.memoryUsage = GPUMemoryUsage::Readback;
                    bd.name = "M6GIExport";
                    e.buf = device->CreateBuffer(bd);
                    if (e.buf == handles::INVALID_RESOURCE) {
                        Fail(std::string("GI export buffer ") + name);
                        return;
                    }
                    g_ledger.buffers.insert(e.buf);
                    ResourceBarrier toCopy{};
                    toCopy.resource = tex;
                    toCopy.beforeState = restState;
                    toCopy.afterState = ResourceState::CopySource;
                    toCopy.subresource = 0xFFFFFFFF;
                    toCopy.queueFamily = 0xFFFFFFFF;
                    rc->InsertBarrier(&toCopy, 1);
                    BufferTextureCopyRegion cr{};
                    cr.bufferOffset = 0;
                    cr.bufferRowLength = e.rowPitch / bpp;          // texels
                    cr.bufferImageHeight = d == 0 ? 0u : h;
                    cr.imageSubresource = {0, 0, 1};
                    cr.imageOffset = {0, 0, 0};
                    cr.imageExtent = {w, h, d == 0 ? 1u : d};
                    rc->CopyTextureToBuffer(tex, e.buf, &cr, 1);
                    ResourceBarrier back{};
                    back.resource = tex;
                    back.beforeState = ResourceState::CopySource;
                    back.afterState = restState;
                    back.subresource = 0xFFFFFFFF;
                    back.queueFamily = 0xFFFFFFFF;
                    rc->InsertBarrier(&back, 1);
                    giExports.push_back(e);
                };
                M1PushConsts pc{};
                pc.model = rhimath::MatrixIdentity();
                pc.baseColor = math::v4{1.0f, 1.0f, 1.0f, 1.0f};

                // resize 时代的帧 + 回归帧 → resize tag PPM（reference 集仍按
                // captureFrames 写；两集尺寸不同故分 tag，manifest 用 --tag 过滤）。
                std::unordered_set<u32> resizeTagFrames;
                if (cfg.resizeFrame != kNoResize) {
                    resizeTagFrames.insert(cfg.resizeFrame);
                    if (cfg.restoreFrame > cfg.resizeFrame + 1)
                        resizeTagFrames.insert(cfg.restoreFrame - 1);
                    if (cfg.restoreFrame < cfg.totalFrames)
                        resizeTagFrames.insert(cfg.restoreFrame);
                }

                // M5 F5-2：逐 mesh 间接绘制（=0 直绘的逐位等价形态——同一
                // push constant/材质集/顶点索引绑定序，仅 DrawIndexed→
                // DrawIndexedIndirect(arg buffer @ m*20)；剔除 mesh 的
                // instanceCount=0 → 硬件跳过，其余与直绘逐位同路径）。
                auto DrawMeshIndirect = [&](RenderMesh* mesh, u32 m) {
                    ResourceHandle vbs[] = {mesh->GetVertexBuffer()};
                    u64 voffs[] = {0};
                    cmd->BindVertexBuffers(0, 1, vbs, voffs);
                    const DataFormat idxFormat =
                        mesh->GetIndexType() == DataIndexType::UInt32
                            ? DataFormat::R32_UInt : DataFormat::R16_UInt;
                    cmd->BindIndexBuffer(mesh->GetIndexBuffer(), idxFormat, 0);
                    cmd->DrawIndexedIndirect(m5gd.argsBuf, u64(m) * 20, 1);
                };

                const auto t0 = std::chrono::high_resolution_clock::now();
                for (u32 f = 0; f < cfg.totalFrames && ok; ++f) {
                    // ---- resize 段（F1-9）：换尺寸=销毁重建（重走创建路径）----
                    if (cfg.resizeFrame != kNoResize) {
                        if (f == cfg.resizeFrame && !(curW == cfg.resizeW && curH == cfg.resizeH)) {
                            DestroyFrameTargets(device, ft);
                            curW = cfg.resizeW; curH = cfg.resizeH;
                            Require(CreateFrameTargets(device, ft, curW, curH, cfg.post,
                                                        cfg.deferred, cfg.gi),
                                    "resize: recreate targets");
                            if (cfg.post) BindM3PostSet(device, post, ft.hdr);
                            if (cfg.deferred)
                                BindM4DeferredSet(device, deferred4, ft, shadow, m6gi);
                            std::cout << "[RHI_SHARED] resize -> " << curW << "x"
                                      << curH << " at frame " << f << std::endl;
                        } else if (f == cfg.restoreFrame &&
                                   !(curW == traj.width && curH == traj.height)) {
                            DestroyFrameTargets(device, ft);
                            curW = traj.width; curH = traj.height;
                            Require(CreateFrameTargets(device, ft, curW, curH, cfg.post,
                                                        cfg.deferred, cfg.gi),
                                    "resize: restore targets");
                            if (cfg.post) BindM3PostSet(device, post, ft.hdr);
                            if (cfg.deferred)
                                BindM4DeferredSet(device, deferred4, ft, shadow, m6gi);
                            std::cout << "[RHI_SHARED] resize -> " << curW << "x"
                                      << curH << " (restore) at frame " << f
                                      << std::endl;
                        }
                    }

                    // ---- 轨迹相机（t = timestep × 帧号）----
                    const double t = double(f) * traj.dt;
                    Keyframe cam = EvalCamera(traj, t);
                    const float aspect = float(curW) / float(curH);
                    math::m4x4 view = rhimath::CreateLookAtMatrix(
                        cam.position, cam.lookAt, math::v3{0.0f, 1.0f, 0.0f});
                    math::m4x4 proj = rhimath::CreatePerspectiveMatrix(
                        float(cam.fovDeg * 3.14159265358979 / 180.0), aspect,
                        0.1f, 200.0f);
                    // F2（Y-down clip 契约）：投影 y 行整行取反 = 左乘
                    // diag(1,-1,1,1)，与生产 naga 对 gl_Position.y 取反同效。
                    // 两端视口（Vulkan 正高 / D3D12 负高）都把 NDC y=-1 映到
                    // RT 第 0 行 → 世界 +Y 落小行号、RT 本身正立。下文
                    // vd.viewProjection（含 M5 视锥平面提取）与 M4
                    // invViewProjection 均消费此取反后的同一 proj。
                    for (int c = 0; c < 4; ++c)
                        math::set_elem(proj, c, 1, -math::elem(proj, c, 1));
                    ViewDataGPU vd{};
                    vd.viewProjection = proj * view;
                    vd.cameraPos = math::v4{cam.position.x, cam.position.y,
                                        cam.position.z, 1.0f};
                    Require(device->UpdateBufferData(viewBuf, &vd, sizeof(vd), 0),
                            "view UBO upload");

                    // ---- M4 F4-7：光照步进求值（无关键帧=常值，M1-M3
                    // 逐位不变；有则按轨迹 config 帧切换——确定性非时变）。
                    // forward/deferred 两侧同源消费（lightBuf / 合并 UBO）。
                    // 边界登记：dir_to_light 关键帧不重导 M2 阴影级联 VP
                    //（t4 光照变化段用 intensity/color 两档，方向恒定）。
                    const LightState light = EvalLight(traj, t);
                    {
                        LightDataGPU ld{};
                        ld.dirAndIntensity = math::v4{light.dirToLight.x,
                                                      light.dirToLight.y,
                                                      light.dirToLight.z,
                                                      light.intensity};
                        ld.color = math::v4{light.color.x, light.color.y,
                                            light.color.z, 1.0f};
                        Require(device->UpdateBufferData(lightBuf, &ld,
                                                         sizeof(ld), 0),
                                "light UBO upload (keyframed)");
                    }
                    // M6：dv 提升到帧作用域——P3 注入 UBO 复用同一 invVP/cameraPos
                    //（与 M4 DeferredView 上传值逐位相同，spec §6.2）。
                    DeferredViewGPU dv{};
                    if (cfg.deferred) {
                        dv.invViewProjection = rhimath::Inverse(proj * view);
                        dv.cameraPos = vd.cameraPos;
                        Require(device->UpdateBufferData(deferred4.viewUbo, &dv,
                                                         sizeof(dv), 0),
                                "deferred view UBO upload");
                        LightShadowDataGPU ls{};
                        ls.dirAndIntensity = math::v4{light.dirToLight.x,
                                                      light.dirToLight.y,
                                                      light.dirToLight.z,
                                                      light.intensity};
                        ls.color = math::v4{light.color.x, light.color.y,
                                            light.color.z, 1.0f};
                        ls.lightViewProjection = shadow.lightVP;
                        ls.shadowParams = shadow.shadowData.shadowParams;
                        Require(device->UpdateBufferData(
                                    deferred4.lightShadowUbo, &ls, sizeof(ls), 0),
                                "light+shadow merged UBO upload");
                    }

                    // ---- 录帧（F1-6 命令路径；每 mesh push constant + set1）----
                    Require(cmd->Reset(), "cmd reset");
                    Require(cmd->Begin(), "cmd begin");

                    // ---- M5 F5-1：GPU-driven 剔除链录制（reset→cull→契约
                    //      barrier）→ 存活数 CPU 复算基准 ----
                    M5CullParamsGPU m5Params{};
                    u32 m5Expected = 0;
                    if (cfg.gpudriven) {
                        const std::array<math::v4, 6> m5Planes =
                            M5ExtractFrustumPlanes(vd.viewProjection);
                        for (int i = 0; i < 6; ++i)
                            m5Params.planes[i] = m5Planes[i];
                        m5Params.camMaxDist = math::v4{cam.position.x,
                                                        cam.position.y,
                                                        cam.position.z,
                                                        m5gd.maxDist};
                        m5Params.counts = math::v4{float(m5gd.meshCount), 0.0f,
                                                   0.0f, 0.0f};
                        m5Expected = M5RecordCull(device, cmd, m5gd,
                                                  vd.viewProjection,
                                                  math::v3{cam.position.x,
                                                           cam.position.y,
                                                           cam.position.z});
                    }

                    // （M6 GI 段录制已移至 GBuffer 后、P2 直射 pass 之后——
                    //  注入消费本帧直射漫反射缓冲，见下方 deferred 分支 P2-P6。）

                    // ---- M2 阴影 depth pass（F2-2/F2-6）：先于 lit ----
                    if (cfg.shadows) {
                        RenderPassDesc srd{};
                        srd.depthAttachment.texture = shadow.shadowMap;
                        srd.depthAttachment.format = DataFormat::D32_Float;
                        srd.depthAttachment.loadOp = LoadAction::Clear;
                        srd.depthAttachment.storeOp = StoreAction::Store;
                        srd.depthAttachment.clearValue.depth = 1.0f;
                        srd.viewport.topLeft = {0.0f, 0.0f};
                        srd.viewport.size = {float(kShadowMapSize),
                                             float(kShadowMapSize)};
                        srd.viewport.minDepth = 0.0f;
                        srd.viewport.maxDepth = 1.0f;
                        srd.scissor.offset = {0, 0};
                        srd.scissor.extent = {kShadowMapSize, kShadowMapSize};
                        cmd->BeginRenderPass(srd);
                        cmd->BindGraphicsPipeline(shadow.depthPipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                shadow.depthLayout, 0, 1,
                                                &shadow.depthSet, 0, nullptr);
                        u32 shadowPassDraws = 0;
                        for (const auto& e : meshes) {
                            M2ShadowPush sp{};
                            sp.model = rhimath::MatrixIdentity();
                            cmd->PushConstants(shadow.depthLayout,
                                               ShaderStage::Vertex, 0,
                                               sizeof(sp), &sp);
                            e.mesh->Draw(cmd, 1, 0, 0);
                            ++shadowPassDraws;
                        }
                        cmd->EndRenderPass();
                        // 诊断：首帧阴影 depth pass 录制事实回显（draw 发出数/
                        // 清深值/分辨率——对照 M2ShadowSampler 回显惯例）。
                        if (f == 0) {
                            std::ostringstream cd;   // 局部流：不改 std::cout 浮点格式态
                            cd << std::fixed << std::setprecision(1)
                               << srd.depthAttachment.clearValue.depth;
                            std::cout << "[RHI_SHARED] M2ShadowPass frame=" << f
                                      << " draws=" << shadowPassDraws
                                      << " clearDepth=" << cd.str()
                                      << " size=" << kShadowMapSize << std::endl;
                        }
                        // 深度写→采样（F2-2 转换规则；D3D12 侧 StateTracker
                        // DEPTH_WRITE→SRV 单测锁定，Vulkan 侧 layout 转换
                        // 本调用即验证点——错误布局会被 validation 拦截）。
                        ResourceBarrier smToSRV{};
                        smToSRV.resource = shadow.shadowMap;
                        smToSRV.beforeState = ResourceState::DepthStencil;
                        smToSRV.afterState = ResourceState::ShaderResource;
                        smToSRV.subresource = 0xFFFFFFFF;
                        smToSRV.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&smToSRV, 1);
                        // RHI_SHADOW_DUMP=1：首帧读回 shadow map（D32→readback
                        // 缓冲→PGM 归一化深度图）——阴影贴图内容裁决证据。
                        const bool wantDump = f == 0 &&
                            std::getenv("RHI_SHADOW_DUMP") &&
                            std::getenv("RHI_SHADOW_DUMP")[0] == '1';
                        if (wantDump && shadowDumpBuf == handles::INVALID_RESOURCE) {
                            BufferDesc ddb{};
                            ddb.size = u64(kShadowMapSize) * kShadowMapSize * 4;
                            ddb.type = BufferType::Raw;
                            // R19-1：同 M1Readback——CopyTextureToBuffer 的
                            // GPU 写目标须 Readback 位（dump 四处漏改复记）。
                            ddb.usage = GPUMemoryUsage::Readback;
                            ddb.memoryUsage = GPUMemoryUsage::Readback;
                            ddb.name = "M2ShadowDump";
                            shadowDumpBuf = device->CreateBuffer(ddb);
                            g_ledger.buffers.insert(shadowDumpBuf);
                        }
                        if (wantDump && shadowDumpBuf != handles::INVALID_RESOURCE &&
                            !shadowDumpDone) {
                            ResourceBarrier smToCopy = smToSRV;
                            smToCopy.afterState = ResourceState::CopySource;
                            cmd->InsertBarrier(&smToCopy, 1);
                            BufferTextureCopyRegion dr{};
                            dr.bufferOffset = 0; dr.bufferRowLength = 0;
                            dr.bufferImageHeight = 0;
                            dr.imageSubresource = {0, 0, 1};
                            dr.imageOffset = {0, 0, 0};
                            dr.imageExtent = {kShadowMapSize, kShadowMapSize, 1};
                            cmd->CopyTextureToBuffer(shadow.shadowMap,
                                                     shadowDumpBuf, &dr, 1);
                            ResourceBarrier smBack{};
                            smBack.resource = shadow.shadowMap;
                            smBack.beforeState = ResourceState::CopySource;
                            smBack.afterState = ResourceState::ShaderResource;
                            smBack.subresource = 0xFFFFFFFF;
                            smBack.queueFamily = 0xFFFFFFFF;
                            cmd->InsertBarrier(&smBack, 1);
                            shadowDumpDone = true;
                        }
                    }

                    // M3：post on 时 lit pass 写 HDR 中间（RGBA16F 线性域，
                    // 值域不钳制）；off 时 BGRA8 直出（M1/M2 原路径）。
                    // M4：deferred on 时 lit pass 换两段——GBuffer pass（MRT×3
                    // +D32）→ 延迟光照 pass（全屏三角，无深度附件——深度纹理
                    // 此刻是采样源，不得再挂深度附件）；目标纹理/格式与
                    // forward lit 完全一致（post on→hdr，off→color）。
                    if (cfg.deferred) {
                        // ---- M4 ① GBuffer pass（F4-2 MRT）----
                        RenderPassDesc gbd{};
                        gbd.colorAttachments.resize(3);
                        gbd.colorAttachments[0].texture = ft.gAlbedo;
                        gbd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
                        gbd.colorAttachments[0].loadOp = LoadAction::Clear;
                        gbd.colorAttachments[0].storeOp = StoreAction::Store;
                        gbd.colorAttachments[0].clearValue.color = math::v4{0, 0, 0, 1};
                        gbd.colorAttachments[1].texture = ft.gNormal;
                        gbd.colorAttachments[1].format = DataFormat::RG16_Float;
                        gbd.colorAttachments[1].loadOp = LoadAction::Clear;
                        gbd.colorAttachments[1].storeOp = StoreAction::Store;
                        gbd.colorAttachments[1].clearValue.color = math::v4{0, 0, 0, 0};
                        gbd.colorAttachments[2].texture = ft.gMaterial;
                        gbd.colorAttachments[2].format = DataFormat::RGBA8_UNorm;
                        gbd.colorAttachments[2].loadOp = LoadAction::Clear;
                        gbd.colorAttachments[2].storeOp = StoreAction::Store;
                        gbd.colorAttachments[2].clearValue.color = math::v4{0, 0, 0, 1};
                        gbd.depthAttachment.texture = ft.depth;
                        gbd.depthAttachment.format = DataFormat::D32_Float;
                        gbd.depthAttachment.loadOp = LoadAction::Clear;
                        gbd.depthAttachment.storeOp = StoreAction::Store;
                        gbd.depthAttachment.clearValue.depth = 1.0f;
                        gbd.viewport.topLeft = {0.0f, 0.0f};
                        gbd.viewport.size = {float(curW), float(curH)};
                        gbd.viewport.minDepth = 0.0f;
                        gbd.viewport.maxDepth = 1.0f;
                        gbd.scissor.offset = {0, 0};
                        gbd.scissor.extent = {curW, curH};
                        cmd->BeginRenderPass(gbd);
                        // set0/set1/push 复用 M1 布局（零新 GBuffer 描述符集）。
                        cmd->BindGraphicsPipeline(deferred4.gbPipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                pipelineLayout, 0, 1,
                                                &globalSet, 0, nullptr);
                        for (u32 m = 0; m < meshes.size(); ++m) {
                            const auto& e = meshes[m];
                            cmd->PushConstants(pipelineLayout, ShaderStage::Vertex,
                                               0, sizeof(M1PushConsts), &pc);
                            cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                    pipelineLayout, 1, 1,
                                                    &e.materialSet, 0, nullptr);
                            if (cfg.gpudriven) DrawMeshIndirect(e.mesh, m);
                            else e.mesh->Draw(cmd, 1, 0, 0);
                        }
                        cmd->EndRenderPass();

                        // ---- M4 GBuffer→SRV 屏障（+ F4-6 dump 首帧绕 CopySource）
                        const bool wantGbDump = f == 0 &&
                            std::getenv("RHI_GBUFFER_DUMP") &&
                            std::getenv("RHI_GBUFFER_DUMP")[0] == '1';
                        ResourceHandle gbTargets[3] = {ft.gAlbedo, ft.gNormal,
                                                        ft.gMaterial};
                        for (int i = 0; i < 3; ++i) {
                            if (wantGbDump) {
                                ResourceBarrier toCopy{};
                                toCopy.resource = gbTargets[i];
                                toCopy.beforeState = ResourceState::RenderTarget;
                                toCopy.afterState = ResourceState::CopySource;
                                toCopy.subresource = 0xFFFFFFFF;
                                toCopy.queueFamily = 0xFFFFFFFF;
                                cmd->InsertBarrier(&toCopy, 1);
                                if (gbDumpBufs[i] == handles::INVALID_RESOURCE) {
                                    BufferDesc ddb{};
                                    ddb.size = u64(curW) * curH * 4;
                                    ddb.type = BufferType::Raw;
                                    // R19-1：同 M1Readback——CopyTextureToBuffer 的
                                    // GPU 写目标须 Readback 位（dump 四处漏改复记）。
                                    ddb.usage = GPUMemoryUsage::Readback;
                                    ddb.memoryUsage = GPUMemoryUsage::Readback;
                                    ddb.name = "M4GBufferDump";
                                    gbDumpBufs[i] = device->CreateBuffer(ddb);
                                    g_ledger.buffers.insert(gbDumpBufs[i]);
                                }
                                BufferTextureCopyRegion dr{};
                                dr.bufferOffset = 0; dr.bufferRowLength = 0;
                                dr.bufferImageHeight = 0;
                                dr.imageSubresource = {0, 0, 1};
                                dr.imageOffset = {0, 0, 0};
                                dr.imageExtent = {curW, curH, 1};
                                cmd->CopyTextureToBuffer(gbTargets[i],
                                                         gbDumpBufs[i], &dr, 1);
                                ResourceBarrier backSRV{};
                                backSRV.resource = gbTargets[i];
                                backSRV.beforeState = ResourceState::CopySource;
                                backSRV.afterState = ResourceState::ShaderResource;
                                backSRV.subresource = 0xFFFFFFFF;
                                backSRV.queueFamily = 0xFFFFFFFF;
                                cmd->InsertBarrier(&backSRV, 1);
                            } else {
                                ResourceBarrier toSRV{};
                                toSRV.resource = gbTargets[i];
                                toSRV.beforeState = ResourceState::RenderTarget;
                                toSRV.afterState = ResourceState::ShaderResource;
                                toSRV.subresource = 0xFFFFFFFF;
                                toSRV.queueFamily = 0xFFFFFFFF;
                                cmd->InsertBarrier(&toSRV, 1);
                            }
                        }
                        // 深度：DepthStencil →（dump 绕 CopySource）→ SRV。
                        {
                            ResourceBarrier dToSRV{};
                            dToSRV.resource = ft.depth;
                            dToSRV.beforeState = ResourceState::DepthStencil;
                            dToSRV.afterState = ResourceState::ShaderResource;
                            dToSRV.subresource = 0xFFFFFFFF;
                            dToSRV.queueFamily = 0xFFFFFFFF;
                            cmd->InsertBarrier(&dToSRV, 1);
                            if (wantGbDump) {
                                ResourceBarrier dToCopy = dToSRV;
                                dToCopy.afterState = ResourceState::CopySource;
                                cmd->InsertBarrier(&dToCopy, 1);
                                if (gbDumpBufs[3] == handles::INVALID_RESOURCE) {
                                    BufferDesc ddb{};
                                    ddb.size = u64(curW) * curH * 4;  // R32F
                                    ddb.type = BufferType::Raw;
                                    // R19-1：同 M1Readback——CopyTextureToBuffer 的
                                    // GPU 写目标须 Readback 位（dump 四处漏改复记）。
                                    ddb.usage = GPUMemoryUsage::Readback;
                                    ddb.memoryUsage = GPUMemoryUsage::Readback;
                                    ddb.name = "M4DepthDump";
                                    gbDumpBufs[3] = device->CreateBuffer(ddb);
                                    g_ledger.buffers.insert(gbDumpBufs[3]);
                                }
                                BufferTextureCopyRegion dr{};
                                dr.bufferOffset = 0; dr.bufferRowLength = 0;
                                dr.bufferImageHeight = 0;
                                dr.imageSubresource = {0, 0, 1};
                                dr.imageOffset = {0, 0, 0};
                                dr.imageExtent = {curW, curH, 1};
                                cmd->CopyTextureToBuffer(ft.depth, gbDumpBufs[3],
                                                         &dr, 1);
                                ResourceBarrier dBack{};
                                dBack.resource = ft.depth;
                                dBack.beforeState = ResourceState::CopySource;
                                dBack.afterState = ResourceState::ShaderResource;
                                dBack.subresource = 0xFFFFFFFF;
                                dBack.queueFamily = 0xFFFFFFFF;
                                cmd->InsertBarrier(&dBack, 1);
                            }
                        }

                        if (cfg.gi) {
                        // ---- M6 GI 模式：P2 直射 → P3 注入/P4 累积 → P5 解析
                        //      → P6 合成（替换 M4 ② 延迟光照 pass；spec §6.3）----
                        // 全屏 pass 公共视口/裁剪（同 M4 ②：当前帧目标尺寸）。
                        auto setFullscreenViewport = [&](RenderPassDesc& rp) {
                            rp.viewport.topLeft = {0.0f, 0.0f};
                            rp.viewport.size = {float(curW), float(curH)};
                            rp.viewport.minDepth = 0.0f;
                            rp.viewport.maxDepth = 1.0f;
                            rp.scissor.offset = {0, 0};
                            rp.scissor.extent = {curW, curH};
                        };
                        auto rtToSRV = [&](ResourceHandle tex) {
                            ResourceBarrier rb{};
                            rb.resource = tex;
                            rb.beforeState = ResourceState::RenderTarget;
                            rb.afterState = ResourceState::ShaderResource;
                            rb.subresource = 0xFFFFFFFF;
                            rb.queueFamily = 0xFFFFFFFF;
                            cmd->InsertBarrier(&rb, 1);
                        };

                        // ---- P2 直射 pass（M6DeferredDirect：MRT×2 RGBA16F，无深度
                        //      附件——深度是采样源；复用 M4 light set/layout b0-b7）----
                        RenderPassDesc drd{};
                        drd.colorAttachments.resize(2);
                        drd.colorAttachments[0].texture = ft.giDirect;
                        drd.colorAttachments[0].format = DataFormat::RGBA16_Float;
                        drd.colorAttachments[0].loadOp = LoadAction::Clear;
                        drd.colorAttachments[0].storeOp = StoreAction::Store;
                        drd.colorAttachments[0].clearValue.color = math::v4{0, 0, 0, 0};
                        drd.colorAttachments[1] = drd.colorAttachments[0];
                        drd.colorAttachments[1].texture = ft.giDirectDiffuse;
                        setFullscreenViewport(drd);
                        cmd->BeginRenderPass(drd);
                        cmd->BindGraphicsPipeline(m6gi.directPipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                deferred4.lightLayout, 0, 1,
                                                &deferred4.lightSet, 0, nullptr);
                        cmd->Draw(3, 0, 1, 0);
                        cmd->EndRenderPass();
                        // RT→SRV：P3 注入（compute texelFetch）+ P6 合成采样
                        rtToSRV(ft.giDirect);
                        rtToSRV(ft.giDirectDiffuse);

                        // ---- P3 注入 + P4 累积（+ ladder 读回、ping-pong 翻转）----
                        M6RecordGI(device, cmd, m6gi, ft, dv.invViewProjection,
                                   dv.cameraPos, light, f, cfg.giLadder,
                                   cfg.totalFrames);

                        // ---- P5 解析 pass → giIndirect（独立间接 RGBA16F）----
                        M6BindResolveSet(device, m6gi, ft, deferred4.viewUbo);
                        RenderPassDesc ird{};
                        ird.colorAttachments.resize(1);
                        ird.colorAttachments[0].texture = ft.giIndirect;
                        ird.colorAttachments[0].format = DataFormat::RGBA16_Float;
                        ird.colorAttachments[0].loadOp = LoadAction::Clear;
                        ird.colorAttachments[0].storeOp = StoreAction::Store;
                        ird.colorAttachments[0].clearValue.color = math::v4{0, 0, 0, 0};
                        setFullscreenViewport(ird);
                        cmd->BeginRenderPass(ird);
                        cmd->BindGraphicsPipeline(m6gi.resolvePipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                m6gi.resolveLayout, 0, 1,
                                                &m6gi.resolveSet, 0, nullptr);
                        cmd->Draw(3, 0, 1, 0);
                        cmd->EndRenderPass();
                        rtToSRV(ft.giIndirect);

                        // ---- P6 合成 pass：direct + indirect 恰一次 → hdr（post on，
                        //      P7 post 原样消费）/ color（post off）----
                        M6BindCompositeSet(device, m6gi, ft);
                        RenderPassDesc crd{};
                        crd.colorAttachments.resize(1);
                        crd.colorAttachments[0].texture = cfg.post ? ft.hdr : ft.color;
                        crd.colorAttachments[0].format = litRTFormat;
                        crd.colorAttachments[0].loadOp = LoadAction::Clear;
                        crd.colorAttachments[0].storeOp = StoreAction::Store;
                        crd.colorAttachments[0].clearValue.color = math::v4{0, 0, 0, 1};
                        setFullscreenViewport(crd);
                        cmd->BeginRenderPass(crd);
                        cmd->BindGraphicsPipeline(m6gi.compPipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                m6gi.compLayout, 0, 1,
                                                &m6gi.compSet, 0, nullptr);
                        cmd->Draw(3, 0, 1, 0);
                        cmd->EndRenderPass();
                        } else {
                        // ---- M4 ② 延迟光照 pass（F4-3；无深度附件）----
                        RenderPassDesc rpd{};
                        rpd.colorAttachments.resize(1);
                        rpd.colorAttachments[0].texture = cfg.post ? ft.hdr : ft.color;
                        rpd.colorAttachments[0].format = litRTFormat;
                        rpd.colorAttachments[0].loadOp = LoadAction::Clear;
                        rpd.colorAttachments[0].storeOp = StoreAction::Store;
                        rpd.colorAttachments[0].clearValue.color = math::v4{0, 0, 0, 1};
                        rpd.viewport.topLeft = {0.0f, 0.0f};
                        rpd.viewport.size = {float(curW), float(curH)};
                        rpd.viewport.minDepth = 0.0f;
                        rpd.viewport.maxDepth = 1.0f;
                        rpd.scissor.offset = {0, 0};
                        rpd.scissor.extent = {curW, curH};
                        cmd->BeginRenderPass(rpd);
                        cmd->BindGraphicsPipeline(deferred4.lightPipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                deferred4.lightLayout, 0, 1,
                                                &deferred4.lightSet, 0, nullptr);
                        cmd->Draw(3, 0, 1, 0);
                        cmd->EndRenderPass();
                        }  // ---- M4 ② 延迟光照（GI off 路径，原样）----
                        gbDumpDone = wantGbDump;
                    } else {
                    RenderPassDesc rpd{};
                    rpd.colorAttachments.resize(1);
                    rpd.colorAttachments[0].texture = cfg.post ? ft.hdr : ft.color;
                    rpd.colorAttachments[0].format = litRTFormat;
                    rpd.colorAttachments[0].loadOp = LoadAction::Clear;
                    rpd.colorAttachments[0].storeOp = StoreAction::Store;
                    rpd.colorAttachments[0].clearValue.color = math::v4{0, 0, 0, 1};
                    rpd.depthAttachment.texture = ft.depth;
                    rpd.depthAttachment.format = DataFormat::D32_Float;
                    rpd.depthAttachment.loadOp = LoadAction::Clear;
                    rpd.depthAttachment.storeOp = StoreAction::Store;
                    rpd.depthAttachment.clearValue.depth = 1.0f;
                    rpd.viewport.topLeft = {0.0f, 0.0f};
                    rpd.viewport.size = {float(curW), float(curH)};
                    rpd.viewport.minDepth = 0.0f;
                    rpd.viewport.maxDepth = 1.0f;
                    rpd.scissor.offset = {0, 0};
                    rpd.scissor.extent = {curW, curH};
                    cmd->BeginRenderPass(rpd);
                    if (cfg.shadows) {
                        // M2 lit pass：3 UBO 全局集 + 4 绑定材质集
                        cmd->BindGraphicsPipeline(shadow.litPipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                shadow.litLayout, 0, 1,
                                                &shadow.litGlobalSet, 0, nullptr);
                        for (u32 m = 0; m < meshes.size(); ++m) {
                            const auto& e = meshes[m];
                            cmd->PushConstants(shadow.litLayout, ShaderStage::Vertex,
                                               0, sizeof(M1PushConsts), &pc);
                            cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                    shadow.litLayout, 1, 1,
                                                    &e.litMaterialSet, 0, nullptr);
                            if (cfg.gpudriven) DrawMeshIndirect(e.mesh, m);
                            else e.mesh->Draw(cmd, 1, 0, 0);
                        }
                    } else {
                        cmd->BindGraphicsPipeline(pipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                pipelineLayout, 0, 1, &globalSet,
                                                0, nullptr);
                        for (u32 m = 0; m < meshes.size(); ++m) {
                            const auto& e = meshes[m];
                            cmd->PushConstants(pipelineLayout, ShaderStage::Vertex, 0,
                                               sizeof(M1PushConsts), &pc);
                            cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                    pipelineLayout, 1, 1,
                                                    &e.materialSet, 0, nullptr);
                            if (cfg.gpudriven) DrawMeshIndirect(e.mesh, m);
                            else e.mesh->Draw(cmd, 1, 0, 0);
                        }
                    }
                    cmd->EndRenderPass();
                    }  // ---- forward lit（非 deferred 路径，M1/M2 原样）----

                    // ---- M3 post pass（F3-2）：HDR → 曝光+ACES → BGRA8 ----
                    // 顺序/格式契约：lit(HDR RGBA16F)→post(BGRA8) 两级；
                    // 唯一编码点 = BGRA8 UNorm RT 写出（无 sRGB 视图、无
                    // shader pow）。全屏三角形 Draw(3)——无顶点缓冲、无
                    // 深度、无 cull（RHI 拓扑直译，D3D12 负 viewport 对
                    // lit/post 两 pass 对称生效 → 采样恒等保向）。
                    if (cfg.post) {
                        ResourceBarrier hdrToSRV{};
                        hdrToSRV.resource = ft.hdr;
                        hdrToSRV.beforeState = ResourceState::RenderTarget;
                        hdrToSRV.afterState = ResourceState::ShaderResource;
                        hdrToSRV.subresource = 0xFFFFFFFF;
                        hdrToSRV.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&hdrToSRV, 1);

                        RenderPassDesc ppd{};
                        ppd.colorAttachments.resize(1);
                        ppd.colorAttachments[0].texture = ft.color;
                        ppd.colorAttachments[0].format = DataFormat::BGRA8_UNorm;
                        ppd.colorAttachments[0].loadOp = LoadAction::Clear;
                        ppd.colorAttachments[0].storeOp = StoreAction::Store;
                        ppd.colorAttachments[0].clearValue.color = math::v4{0, 0, 0, 1};
                        ppd.viewport.topLeft = {0.0f, 0.0f};
                        ppd.viewport.size = {float(curW), float(curH)};
                        ppd.viewport.minDepth = 0.0f;
                        ppd.viewport.maxDepth = 1.0f;
                        ppd.scissor.offset = {0, 0};
                        ppd.scissor.extent = {curW, curH};
                        cmd->BeginRenderPass(ppd);
                        cmd->BindGraphicsPipeline(post.pipeline);
                        cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                                post.layout, 0, 1, &post.set,
                                                0, nullptr);
                        cmd->Draw(3, 0, 1, 0);
                        cmd->EndRenderPass();

                        // M3 F3-5：首帧 HDR 中间读回（SR→CopySource→SR，
                        // 布局同 CB 跟踪——同 M2 shadow dump 模式）。
                        const bool wantHdrDump = f == 0 &&
                            std::getenv("RHI_HDR_DUMP") &&
                            std::getenv("RHI_HDR_DUMP")[0] == '1';
                        if (wantHdrDump &&
                            hdrDumpBuf == handles::INVALID_RESOURCE) {
                            BufferDesc hdb{};
                            hdb.size = u64(curW) * curH * 8;   // RGBA16F=8B/px
                            hdb.type = BufferType::Raw;
                            // R19-1：同 M1Readback——CopyTextureToBuffer 的
                            // GPU 写目标须 Readback 位（dump 四处漏改复记）。
                            hdb.usage = GPUMemoryUsage::Readback;
                            hdb.memoryUsage = GPUMemoryUsage::Readback;
                            hdb.name = "M3HdrDump";
                            hdrDumpBuf = device->CreateBuffer(hdb);
                            g_ledger.buffers.insert(hdrDumpBuf);
                        }
                        if (wantHdrDump &&
                            hdrDumpBuf != handles::INVALID_RESOURCE &&
                            !hdrDumpDone) {
                            ResourceBarrier h1{};
                            h1.resource = ft.hdr;
                            h1.beforeState = ResourceState::ShaderResource;
                            h1.afterState = ResourceState::CopySource;
                            h1.subresource = 0xFFFFFFFF;
                            h1.queueFamily = 0xFFFFFFFF;
                            cmd->InsertBarrier(&h1, 1);
                            BufferTextureCopyRegion hr{};
                            hr.bufferOffset = 0; hr.bufferRowLength = 0;
                            hr.bufferImageHeight = 0;
                            hr.imageSubresource = {0, 0, 1};
                            hr.imageOffset = {0, 0, 0};
                            hr.imageExtent = {curW, curH, 1};
                            cmd->CopyTextureToBuffer(ft.hdr, hdrDumpBuf, &hr, 1);
                            ResourceBarrier h2{};
                            h2.resource = ft.hdr;
                            h2.beforeState = ResourceState::CopySource;
                            h2.afterState = ResourceState::ShaderResource;
                            h2.subresource = 0xFFFFFFFF;
                            h2.queueFamily = 0xFFFFFFFF;
                            cmd->InsertBarrier(&h2, 1);
                            hdrDumpDone = true;
                        }
                    }

                    // ---- M6 spec §6.5：原始浮点导出（RHI_GI_EXPORT_FRAMES 命中帧；
                    //      合成/post 之后录拷贝，提交+等待后落盘）。此刻状态：GI
                    //      三 RT / GBuffer / 深度 / hdr（post on）= SRV，color = RT，
                    //      grid[cur] = 本帧 accum 输出（SRV）。----
                    if (cfg.deferred && cfg.giExportFrames.count(f) != 0) {
                        const char* kHdrUnits =
                            "harness linear HDR radiance (pre-exposure, pre-ACES; "
                            "Lambert without 1/pi)";
                        if (cfg.gi) {
                            recordGIExport(cmd, ft.giDirect, ResourceState::ShaderResource,
                                "direct", "RGBA16_Float", 8, curW, curH, 0,
                                "rgb = lightColor*(diffuse+specular)*shadow (direct "
                                "only, no ambient/GI/IBL); a = ndl",
                                kHdrUnits);
                            recordGIExport(cmd, ft.giDirectDiffuse, ResourceState::ShaderResource,
                                "direct_diffuse", "RGBA16_Float", 8, curW, curH, 0,
                                "rgb = lightColor*albedo*ndl*shadow (GI injection "
                                "input L_j); a = shadow visibility (PCF 3x3)",
                                kHdrUnits);
                            recordGIExport(cmd, ft.giIndirect, ResourceState::ShaderResource,
                                "indirect", "RGBA16_Float", 8, curW, curH, 0,
                                "rgb = albedo*E_h(N) from SH-L1 probe grid (single "
                                "bounce, independent of direct); a = 1 (0 on "
                                "background)",
                                kHdrUnits);
                        }
                        if (cfg.post) {
                            recordGIExport(cmd, ft.hdr, ResourceState::ShaderResource,
                                "final_prepost", "RGBA16_Float", 8, curW, curH, 0,
                                cfg.gi ? "rgb = direct.rgb + indirect.rgb (M6Composite, "
                                         "pre-post); a = 1"
                                       : "M4DeferredLight output (GI off): albedo*0.15 "
                                         "+ lightColor*(diffuse+specular)*shadow; a = 1",
                                kHdrUnits);
                        } else {
                            recordGIExport(cmd, ft.color, ResourceState::RenderTarget,
                                "final_prepost", "BGRA8_UNorm", 4, curW, curH, 0,
                                cfg.gi ? "BGRA8 UNorm direct write of direct+indirect "
                                         "(post off, clamped)"
                                       : "BGRA8 UNorm direct write of M4DeferredLight "
                                         "output (post off, clamped)",
                                "unorm8 (no tone mapping, no sRGB)");
                        }
                        recordGIExport(cmd, ft.depth, ResourceState::ShaderResource,
                            "depth", "D32_Float", 4, curW, curH, 0,
                            "post-projection depth [0,1] (forward Z; >= 1 = "
                            "background)",
                            "ndc depth");
                        recordGIExport(cmd, ft.gAlbedo, ResourceState::ShaderResource,
                            "albedo", "RGBA8_UNorm", 4, curW, curH, 0,
                            "GBuffer RT0: rgb = albedo texture * baseColor; a = 1",
                            "unorm8 reflectance");
                        recordGIExport(cmd, ft.gNormal, ResourceState::ShaderResource,
                            "normal", "RG16_Float", 4, curW, curH, 0,
                            "GBuffer RT1: world normal xy; z = (material.a*2-1)*"
                            "sqrt(max(0,1-x^2-y^2))",
                            "unit vector components");
                        recordGIExport(cmd, ft.gMaterial, ResourceState::ShaderResource,
                            "material", "RGBA8_UNorm", 4, curW, curH, 0,
                            "GBuffer RT2: r = AO placeholder (1), g = roughness "
                            "placeholder (0.5), b = uv.x, a = N.z sign (1 = +, 0 = -)",
                            "unorm8");
                        if (cfg.gi) {
                            recordGIExport(cmd, m6gi.grid[m6gi.cur],
                                ResourceState::ShaderResource, "probe_sh",
                                "RGBA16_Float", 8, kM6GridDim, kM6GridDim,
                                kM6ShSlabs * kM6GridDim,
                                "SH-L1 probe grid after EMA accum (this frame): "
                                "texel (i,j,s*8+k) = (c00,c1-1,c10,c11) of probe "
                                "(i,j,k), slab s = channel R,G,B; file rows tight "
                                "(8 texels = 64 B per row, 8 rows per slice, 24 "
                                "slices); GPU readback used rowLength 32 texels "
                                "(256 B), imageHeight 8",
                                "SH coefficients of incident radiance (harness "
                                "radiance x steradian)");
                        }
                    }

                    // ---- M6：GI 模式几何覆盖率读回（深度 SR→Copy→SR，行距 256
                    //      对齐；提交后统计深度<1 占比，供 GI 模式黑帧守卫）----
                    if (cfg.gi) {
                        ResourceBarrier dToCopy{};
                        dToCopy.resource = ft.depth;
                        dToCopy.beforeState = ResourceState::ShaderResource;
                        dToCopy.afterState = ResourceState::CopySource;
                        dToCopy.subresource = 0xFFFFFFFF;
                        dToCopy.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&dToCopy, 1);
                        BufferTextureCopyRegion cvr{};
                        cvr.bufferOffset = 0;
                        cvr.bufferRowLength = ft.giCoveragePitch / 4u;   // texels（D32 4B）
                        cvr.bufferImageHeight = 0;
                        cvr.imageSubresource = {0, 0, 1};
                        cvr.imageOffset = {0, 0, 0};
                        cvr.imageExtent = {curW, curH, 1};
                        cmd->CopyTextureToBuffer(ft.depth, ft.giCoverageReadback, &cvr, 1);
                        ResourceBarrier dBack{};
                        dBack.resource = ft.depth;
                        dBack.beforeState = ResourceState::CopySource;
                        dBack.afterState = ResourceState::ShaderResource;
                        dBack.subresource = 0xFFFFFFFF;
                        dBack.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&dBack, 1);
                    }

                    // ---- color: RenderTarget → CopySource → 读回 → 还原 ----
                    ResourceBarrier toCopy{};
                    toCopy.resource = ft.color;
                    toCopy.beforeState = ResourceState::RenderTarget;
                    toCopy.afterState = ResourceState::CopySource;
                    toCopy.subresource = 0xFFFFFFFF;
                    toCopy.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&toCopy, 1);
                    BufferTextureCopyRegion region{};
                    region.bufferOffset = 0;
                    region.bufferRowLength = 0;   // tightly packed
                    region.bufferImageHeight = 0;
                    region.imageSubresource = {0, 0, 1};
                    region.imageOffset = {0, 0, 0};
                    region.imageExtent = {curW, curH, 1};
                    cmd->CopyTextureToBuffer(ft.color, ft.readback, &region, 1);
                    ResourceBarrier toRT{};
                    toRT.resource = ft.color;
                    toRT.beforeState = ResourceState::CopySource;
                    toRT.afterState = ResourceState::RenderTarget;
                    toRT.subresource = 0xFFFFFFFF;
                    toRT.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&toRT, 1);
                    cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                                       AccessFlag::TransferWrite,
                                       AccessFlag::HostRead);
                    // M5：args/counter 读回拷贝（同 cmd 内录制；§4.4 显式
                    // Compute→Transfer 屏障在 M5RecordReadback 内部前置）
                    if (cfg.gpudriven) M5RecordReadback(cmd, m5gd);
                    Require(cmd->End(), "cmd end");
                    Require(cmd->Submit(0), "cmd submit");
                    Require(cmd->WaitForCompletion(), "cmd wait");
                    // M5 交叉断言：GPU 计数/逐 mesh arg == CPU 复算（剔除=纯
                    // 几何谓词的可复核性证据；fail 计入 g_failures）
                    if (cfg.gpudriven)
                        M5VerifyReadback(device, m5gd, m5Expected, m5Params, f);
                    // M6：收敛阶梯读回（[GI_MEAN] + RHI_GI_DUMP 中央 slice PPM）
                    if (cfg.gi && m6gi.readbackPending) {
                        M6VerifyGI(device, m6gi, cfg.captureDir, f);
                        m6gi.readbackPending = false;
                    }
                    // M6 spec §6.5：导出落盘（紧密行：读回缓冲按 rowPitch 行距逐行取
                    // w*bpp 有效字节——行尾填充字节内容未定义，不写出，文件逐位
                    // 可复现）+ 清单 JSON；缓冲即毁。
                    if (!giExports.empty()) {
                        const std::string stem = "giexp_f" + std::to_string(f);
                        u32 written = 0;
                        for (GIExportBuf& e : giExports) {
                            // 2D：h 行；3D：d 个 slice × h 行（读回 slice 间距 = h*rowPitch）
                            const u32 rows = e.h * (e.d == 0 ? 1u : e.d);
                            const u32 tightRow = e.w * e.bpp;
                            const u8* src = static_cast<const u8*>(device->MapBuffer(e.buf));
                            if (!src) {
                                Fail("GI export map " + e.name);
                            } else {
                                const std::string p = giExportDir + "/" + stem + "_" +
                                                      e.name + ".bin";
                                std::ofstream bf(p, std::ios::binary);
                                for (u32 r = 0; r < rows; ++r)
                                    bf.write(reinterpret_cast<const char*>(
                                                 src + size_t(r) * e.rowPitch),
                                             std::streamsize(tightRow));
                                if (!bf) Fail("GI export write " + p);
                                else ++written;
                                device->UnmapBuffer(e.buf);
                            }
                            device->DestroyBuffer(e.buf);
                            g_ledger.buffers.erase(e.buf);
                            e.buf = handles::INVALID_RESOURCE;
                        }
                        // 清单（schema rhi-m6-gi-export/1；浮点 9 位有效数字 = f32 可逆）
                        auto vec3Json = [](float x, float y, float z) {
                            std::ostringstream o;
                            o << std::setprecision(9) << "[" << x << ", " << y << ", " << z << "]";
                            return o.str();
                        };
                        std::ostringstream js;
                        js << std::setprecision(9);
                        // 本帧 DeferredView/InjectParams 上传的同一 invVP（UBO 内存序 16
                        // float = GLSL mat4 列主序 [c*4+r]）——独立复算 inject/resolve 用。
                        float invVP[16];
                        static_assert(sizeof(dv.invViewProjection) == sizeof(invVP),
                                      "invViewProjection = 16 floats");
                        std::memcpy(invVP, &dv.invViewProjection, sizeof(invVP));
                        std::ostringstream ivp;
                        ivp << std::setprecision(9) << "[";
                        for (int i = 0; i < 16; ++i) ivp << (i ? ", " : "") << invVP[i];
                        ivp << "]";
                        js << "{\n  \"schema\": \"rhi-m6-gi-export/1\",\n"
                           << "  \"frame\": " << f << ",\n"
                           << "  \"platform\": \"" << cfg.platformName << "\",\n"
                           << "  \"gi\": " << (cfg.gi ? "true" : "false") << ",\n"
                           << "  \"post\": " << (cfg.post ? "true" : "false") << ",\n"
                           << "  \"exposure\": "
                           << (cfg.postExposure * std::pow(2.0f, cfg.postEV)) << ",\n"
                           << "  \"width\": " << curW << ",\n"
                           << "  \"height\": " << curH << ",\n"
                           << "  \"camera\": {\"position\": "
                           << vec3Json(cam.position.x, cam.position.y, cam.position.z)
                           << ", \"lookAt\": "
                           << vec3Json(cam.lookAt.x, cam.lookAt.y, cam.lookAt.z)
                           << ", \"fov_deg\": " << cam.fovDeg
                           << ", \"near\": 0.1, \"far\": 200, \"projection_y_row_negated\": true"
                           << ", \"inv_view_projection\": " << ivp.str()
                           << ", \"matrix_layout\": \"16 floats in UBO memory order "
                              "(GLSL mat4 column-major: index c*4+r); world = "
                              "(M*vec4(uv*2-1, depth, 1)).xyz / w, uv = (px+0.5)/(W,H)\"},\n"
                           << "  \"light\": {\"dir_to_light\": "
                           << vec3Json(light.dirToLight.x, light.dirToLight.y,
                                       light.dirToLight.z)
                           << ", \"intensity\": " << light.intensity
                           << ", \"color\": "
                           << vec3Json(light.color.x, light.color.y, light.color.z)
                           << "},\n"
                           << "  \"shadow\": {\"map_size\": " << kShadowMapSize
                           << ", \"pcf\": \"3x3\", \"compare\": \"LessEqual\""
                           << ", \"ortho_half_extent\": " << kShadowHalfExtent
                           << ", \"near\": " << kShadowNear << ", \"far\": " << kShadowFar
                           << "},\n";
                        if (cfg.gi) {
                            js << "  \"gi_grid\": {\"dims\": [" << kM6GridDim << ", "
                               << kM6GridDim << ", " << kM6GridDim << "]"
                               << ", \"box_min\": "
                               << vec3Json(m6gi.boxMin.x, m6gi.boxMin.y, m6gi.boxMin.z)
                               << ", \"box_max\": "
                               << vec3Json(m6gi.boxMax.x, m6gi.boxMax.y, m6gi.boxMax.z)
                               << ", \"cell\": "
                               << vec3Json(m6gi.cell.x, m6gi.cell.y, m6gi.cell.z)
                               << ", \"normal_bias\": " << m6gi.normalBias
                               << ", \"sh_layout\": \"Texture3D RGBA16F (8,8,24); texel "
                                  "(i,j,s*8+k) = (c00,c1-1,c10,c11) of probe (i,j,k) "
                                  "channel s (0=R,1=G,2=B); probe (i,j,k) at box_min + "
                                  "((i,j,k)+0.5)*cell; Y00=0.282095, Y1-1=0.488603*y, "
                                  "Y10=0.488603*z, Y11=0.488603*x\""
                               << ", \"ema\": \"hist*0.9+new*0.1\""
                               << ", \"reset_this_frame\": "
                               << (m6gi.lastReset ? "true" : "false")
                               << ", \"frames_since_reset\": " << m6gi.framesSinceReset
                               << "},\n";
                        }
                        js << "  \"buffers\": {";
                        for (size_t i = 0; i < giExports.size(); ++i) {
                            const GIExportBuf& e = giExports[i];
                            js << (i ? ",\n" : "\n") << "    \"" << e.name << "\": {"
                               << "\"file\": \"" << stem << "_" << e.name << ".bin\""
                               << ", \"format\": \"" << e.format << "\""
                               << ", \"bytes_per_pixel\": " << e.bpp
                               << ", \"row_pitch\": " << e.w * e.bpp          // 文件紧密行距
                               << ", \"readback_row_pitch\": " << e.rowPitch  // GPU 拷贝行距
                               << ", \"width\": " << e.w << ", \"height\": " << e.h;
                            if (e.d != 0) js << ", \"depth\": " << e.d;
                            js << ", \"semantics\": \"" << e.semantics << "\""
                               << ", \"units\": \"" << e.units << "\"}";
                        }
                        js << "\n  }\n}\n";
                        const std::string mp = giExportDir + "/" + stem + ".json";
                        std::ofstream mf(mp, std::ios::binary);
                        mf << js.str();
                        if (!mf) Fail("GI export manifest write " + mp);
                        ++m6gi.exportFrames;
                        std::cerr << "[GI_EXPORT] frame " << f << " buffers=" << written
                                  << " dir=" << giExportDir << std::endl;
                        giExports.clear();
                    }

                    // M6：GI 模式几何覆盖率（深度<1 像素占比；GI 模式黑帧守卫用）
                    double giCoverage = 1.0;
                    if (cfg.gi) {
                        const u8* cov = static_cast<const u8*>(
                            device->MapBuffer(ft.giCoverageReadback));
                        if (!cov) {
                            Fail("M6 coverage readback map");
                            giCoverage = 0.0;
                        } else {
                            u64 covered = 0;
                            for (u32 y = 0; y < curH; ++y) {
                                const float* row = reinterpret_cast<const float*>(
                                    cov + size_t(y) * ft.giCoveragePitch);
                                for (u32 x = 0; x < curW; ++x)
                                    covered += row[x] < 1.0f ? 1u : 0u;
                            }
                            giCoverage = double(covered) / (double(curW) * double(curH));
                            device->UnmapBuffer(ft.giCoverageReadback);
                        }
                        giMinCoverage = std::min(giMinCoverage, giCoverage);
                    }

                    // ---- 读回校验/落盘（F1-10 逐帧非黑）----
                    const u8* mapped =
                        static_cast<const u8*>(device->MapBuffer(ft.readback));
                    if (!mapped) {
                        Fail("MapBuffer readback");
                        break;
                    }
                    const size_t pxCount = size_t(curW) * curH;
                    u64 lit = 0;
                    double lumAcc = 0.0;
                    for (size_t i = 0; i < pxCount; ++i) {
                        const u8* p = mapped + i * 4;
                        if (p[0] || p[1] || p[2]) ++lit;
                        // Rec.709 luma（0-255）；阴影使直射面变暗 → 平均亮度下降
                        const double lum = 0.2126 * p[2] + 0.7152 * p[1] +
                                           0.0722 * p[0];
                        lumAcc += lum;
                        if (cfg.post) {   // M3 消融统计（全帧累计）
                            const u32 bin = u32(lum);
                            histAll[bin < 255 ? bin : 255]++;
                            if (p[2] >= 254 || p[1] >= 254 || p[0] >= 254)
                                ++clipWhiteAll;
                            ++pxAll;
                        }
                    }
                    const double litRatio = double(lit) / double(pxCount);
                    const double meanLum = lumAcc / double(pxCount);
                    litRatioSum += litRatio;
                    lumSum += meanLum;
                    frameLitRatio.push_back(litRatio);
                    frameMeanLum.push_back(meanLum);
                    if (lumTrace) {
                        // R20-B：逐帧非黑占比/亮度（黑帧分布的对照组——
                        // 黑帧与正常帧的占比台阶比二值黑名单更有诊断力）。
                        std::cerr << "[LUM] frame " << f
                                  << " litRatio=" << litRatio
                                  << " meanLum=" << meanLum << std::endl;
                    }
                    // d3d12-blackfix 探针：读回首 16 字节 hex + 首像素 BGRA。
                    // 区分"清屏+拷贝落地"（BGRA 首字节 00 00 00 FF，alpha=
                    // 0xFF——lit 清屏色 {0,0,0,1}）与"读回仍是 memset 0"
                    // （00 00 00 00——命令列表未执行/拷贝未写）。全平台打
                    //（Metal/Vulkan 无害），litRatio=0 区分不了这两种情形。
                    std::cerr << "[PIXEL0] frame " << f << " first16=";
                    const int probeBytes = int(pxCount * 4) < 16 ? int(pxCount * 4) : 16;
                    for (int i = 0; i < probeBytes; ++i)
                        std::cerr << std::hex << std::setw(2) << std::setfill('0')
                                  << int(mapped[i]) << ' ';
                    std::cerr << std::dec << "px0 BGRA=(" << int(mapped[0]) << ','
                              << int(mapped[1]) << ',' << int(mapped[2]) << ','
                              << int(mapped[3]) << ')' << std::endl;
                    // 轨迹相机全程朝向画廊内部几何，清屏色=黑 → 非黑比例即几何覆盖。
                    // M6：GI 模式该前提失效（完全阴影几何恰为黑——直射-only 契约，
                    // 间接仅来自相机可见发射体），守卫改测几何覆盖率（深度<1
                    // 占比）；颜色暗帧（非黑<2% 但几何覆盖正常）另计入汇总行
                    //（全部帧皆暗时汇总段判失败）。
                    const bool nearlyBlack =
                        cfg.gi ? giCoverage < 0.02 : litRatio < 0.02;
                    if (cfg.gi && litRatio < 0.02 && !nearlyBlack) ++giDarkFrames;
                    if (nearlyBlack) {
                        ++blackFrames;
                        blackFrameList.push_back(f);
                        std::cerr << "[RHI_SHARED] frame " << f
                                  << " nearly black (" << litRatio << ")";
                        if (cfg.gi) std::cerr << " coverage=" << giCoverage;
                        std::cerr << std::endl;
                    }
                    const bool isResizeTag = resizeTagFrames.count(f) != 0;
                    if (cfg.captureFrames.count(f) || isResizeTag) {
                        // resize 时代帧恒用 resize tag（尺寸与 reference 集不同，
                        // 分 tag 保证 manifest 逐 tag dims 一致）。
                        const std::string tag = isResizeTag ? "resize" : "reference";
                        const std::string path = cfg.captureDir + "/" + tag + "_" +
                                                 std::to_string(f) + ".ppm";
                        if (!WritePPM(path, mapped, curW, curH))
                            Fail("write PPM " + path);
                    }
                    device->UnmapBuffer(ft.readback);

                    // M0 §3.5 契约：固定 dt 每帧一行（manifest 证据链）。
                    std::cerr << "[FIXED_DT] frame " << f << " dt=" << traj.dt
                              << std::endl;

                    // shadow map 读回落盘（PGM：d∈[0,1] 反相显示，0=黑=
                    // 最近（遮挡物），1=白=远/空——视觉上白=能透过光）。
                    if (shadowDumpDone && shadowDumpBuf != handles::INVALID_RESOURCE) {
                        const float* sd = static_cast<const float*>(
                            device->MapBuffer(shadowDumpBuf));
                        if (sd) {
                            const std::string path =
                                cfg.captureDir + "/shadowmap_0.pgm";
                            std::ofstream pf(path, std::ios::binary);
                            pf << "P5\n" << kShadowMapSize << " "
                               << kShadowMapSize << "\n255\n";
                            for (u32 i = 0; i < kShadowMapSize * kShadowMapSize; ++i) {
                                float d = sd[i];
                                u8 v = d >= 1.0f ? 255
                                      : (d <= 0.0f ? 0 : u8(d * 255.0f));
                                pf.write(reinterpret_cast<const char*>(&v), 1);
                            }
                            std::cout << "[RHI_SHARED] shadow map dumped: "
                                      << path << std::endl;
                            device->UnmapBuffer(shadowDumpBuf);
                        }
                        device->DestroyBuffer(shadowDumpBuf);
                        g_ledger.buffers.erase(shadowDumpBuf);
                        shadowDumpBuf = handles::INVALID_RESOURCE;
                    }

                    // M3 F3-5：HDR 中间读回落盘（f16→f32 线性域 [0,1] 预览
                    // PPM；>1 的 HDR 值贴到 255——诊断用线性预览，非编码输出）。
                    if (hdrDumpDone && hdrDumpBuf != handles::INVALID_RESOURCE) {
                        const u16* hd = static_cast<const u16*>(
                            device->MapBuffer(hdrDumpBuf));
                        if (hd) {
                            auto f16 = [](u16 h) {
                                const u32 sign = (h >> 15) & 1u;
                                const u32 ex = (h >> 10) & 0x1Fu;
                                const u32 mant = h & 0x3FFu;
                                if (ex == 0u) return 0.0f;   // 暗部 subnormal
                                const u32 bits = (sign << 31) |
                                                 ((ex + 112u) << 23) |
                                                 (mant << 13);
                                float fv;
                                std::memcpy(&fv, &bits, 4);
                                return fv;
                            };
                            const std::string hp = cfg.captureDir + "/hdr_0.ppm";
                            std::ofstream hf(hp, std::ios::binary);
                            hf << "P6\n" << curW << " " << curH << "\n255\n";
                            for (u32 i = 0; i < curW * curH; ++i) {
                                const u16* px = hd + size_t(i) * 4;
                                float rgb[3] = {f16(px[0]), f16(px[1]),
                                                f16(px[2])};
                                u8 out[3];
                                for (int c = 0; c < 3; ++c) {
                                    const float v = rgb[c] < 0.0f ? 0.0f
                                                  : (rgb[c] > 1.0f ? 1.0f
                                                                   : rgb[c]);
                                    out[c] = u8(v * 255.0f + 0.5f);
                                }
                                // RGBA16F 行序=BGRx？否——RHI RGBA 通道序恒
                                // R,G,B,A；写出 P6 需 R,G,B。
                                hf.write(reinterpret_cast<const char*>(out), 3);
                            }
                            std::cout << "[RHI_SHARED] HDR intermediate dumped: "
                                      << hp << std::endl;
                            device->UnmapBuffer(hdrDumpBuf);
                        }
                        device->DestroyBuffer(hdrDumpBuf);
                        g_ledger.buffers.erase(hdrDumpBuf);
                        hdrDumpBuf = handles::INVALID_RESOURCE;
                    }

                    // M4 F4-6：GBuffer 三 RT + 深度读回落盘（首帧一次；
                    // albedo/material RGBA8 直出，normal RG16F f16 解码
                    // v*0.5+0.5 预览（z 由符号重建），深度 R32F 直出 PGM）。
                    if (gbDumpDone) {
                        auto halfToFloat = [](u16 h) {
                            const u32 sign = (h >> 15) & 1u;
                            const u32 ex = (h >> 10) & 0x1Fu;
                            const u32 mant = h & 0x3FFu;
                            if (ex == 0u) return 0.0f;
                            const u32 bits = (sign << 31) |
                                             ((ex + 112u) << 23) | (mant << 13);
                            float fv;
                            std::memcpy(&fv, &bits, 4);
                            return fv;
                        };
                        auto clampedByte = [](float v) {
                            const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                            return u8(c * 255.0f + 0.5f);
                        };
                        // [0] albedo / [2] material：RGBA8 → P6 RGB。
                        for (int i = 0; i < 3; i += 2) {
                            const u8* px = static_cast<const u8*>(
                                device->MapBuffer(gbDumpBufs[i]));
                            if (!px) continue;
                            const std::string p = cfg.captureDir + "/" +
                                (i == 0 ? std::string("gbuffer_albedo_0.ppm")
                                        : std::string("gbuffer_material_0.ppm"));
                            std::ofstream pf(p, std::ios::binary);
                            pf << "P6\n" << curW << " " << curH << "\n255\n";
                            for (u32 y = 0; y < curH; ++y)
                                for (u32 x = 0; x < curW; ++x) {
                                    const u8* q = px + (size_t(y) * curW + x) * 4;
                                    pf.write(reinterpret_cast<const char*>(q), 3);
                                }
                            std::cout << "[RHI_SHARED] GBuffer dumped: " << p
                                      << std::endl;
                            device->UnmapBuffer(gbDumpBufs[i]);
                        }
                        // [1] normal：RG16F（符号取自 material.a）→ P6 预览。
                        {
                            const u16* np = static_cast<const u16*>(
                                device->MapBuffer(gbDumpBufs[1]));
                            const u8* mp = static_cast<const u8*>(
                                device->MapBuffer(gbDumpBufs[2]));
                            if (np && mp) {
                                const std::string p =
                                    cfg.captureDir + "/gbuffer_normal_0.ppm";
                                std::ofstream pf(p, std::ios::binary);
                                pf << "P6\n" << curW << " " << curH << "\n255\n";
                                for (u32 y = 0; y < curH; ++y)
                                    for (u32 x = 0; x < curW; ++x) {
                                        const size_t idx = size_t(y) * curW + x;
                                        const float nx = halfToFloat(np[idx * 2]);
                                        const float ny = halfToFloat(np[idx * 2 + 1]);
                                        const float zSign =
                                            mp[idx * 4 + 3] >= 128 ? 1.0f : -1.0f;
                                        const float nz = zSign * std::sqrt(
                                            std::max(0.0f, 1.0f - nx * nx - ny * ny));
                                        const u8 rgb[3] = {
                                            clampedByte(nx * 0.5f + 0.5f),
                                            clampedByte(ny * 0.5f + 0.5f),
                                            clampedByte(nz * 0.5f + 0.5f)};
                                        pf.write(reinterpret_cast<const char*>(rgb), 3);
                                    }
                                std::cout << "[RHI_SHARED] GBuffer dumped: " << p
                                          << std::endl;
                            }
                            if (np) device->UnmapBuffer(gbDumpBufs[1]);
                            if (mp) device->UnmapBuffer(gbDumpBufs[2]);
                        }
                        // [3] depth：R32F → P5（直接值：白=远/背景 1.0）。
                        {
                            const float* dp = static_cast<const float*>(
                                device->MapBuffer(gbDumpBufs[3]));
                            if (dp) {
                                const std::string p =
                                    cfg.captureDir + "/gbuffer_depth_0.pgm";
                                std::ofstream pf(p, std::ios::binary);
                                pf << "P5\n" << curW << " " << curH << "\n255\n";
                                for (u32 i = 0; i < curW * curH; ++i) {
                                    const float d = dp[i];
                                    const u8 v = d >= 1.0f ? 255
                                              : (d <= 0.0f ? 0 : u8(d * 255.0f));
                                    pf.write(reinterpret_cast<const char*>(&v), 1);
                                }
                                std::cout << "[RHI_SHARED] GBuffer dumped: " << p
                                          << std::endl;
                                device->UnmapBuffer(gbDumpBufs[3]);
                            }
                        }
                        for (auto& b : gbDumpBufs) {
                            if (b != handles::INVALID_RESOURCE) {
                                device->DestroyBuffer(b);
                                g_ledger.buffers.erase(b);
                                b = handles::INVALID_RESOURCE;
                            }
                        }
                        gbDumpDone = false;  // 只在首帧 dump（bufs 已毁，防重入）
                    }
                }
                const auto t1 = std::chrono::high_resolution_clock::now();
                const double ms =
                    std::chrono::duration<double, std::milli>(t1 - t0).count();

                Require(blackFrames == 0,
                        "all frames non-black (" + std::to_string(blackFrames) +
                            " nearly-black frames)");
                // R20-B 黑帧分布汇总（十九测"159/240"无法反推分布——
                // 集中在 resize 帧后=resize 重建破坏；随机交错=时序竞争/
                // 轨迹相位相关的数据丢失；全程稀疏=局部纹理/图元缺陷）。
                if (!blackFrameList.empty()) {
                    std::cerr << "[BLACK_DIST] total=" << blackFrames << "/"
                              << cfg.totalFrames << " ("
                              << (100.0 * blackFrames / double(cfg.totalFrames))
                              << "%) first=" << blackFrameList.front()
                              << " last=" << blackFrameList.back() << std::endl;
                    // 连片 run-length：[start..end] × 长度。
                    std::cerr << "[BLACK_DIST] runs:";
                    for (size_t i = 0; i < blackFrameList.size();) {
                        size_t j = i;
                        while (j + 1 < blackFrameList.size() &&
                               blackFrameList[j + 1] == blackFrameList[j] + 1) ++j;
                        std::cerr << " [" << blackFrameList[i] << ".."
                                  << blackFrameList[j] << "]x" << (j - i + 1);
                        i = j + 1;
                    }
                    std::cerr << std::endl;
                    // resize 关联：黑帧落点 vs resize/restore 帧（判"集中在
                    // resize 后"最直接的键）。
                    if (cfg.resizeFrame != kNoResize) {
                        const u32 afterResize = static_cast<u32>(std::count_if(
                            blackFrameList.begin(), blackFrameList.end(),
                            [&](u32 fr) { return fr >= cfg.resizeFrame; }));
                        std::cerr << "[BLACK_DIST] resizeFrame=" << cfg.resizeFrame
                                  << " restoreFrame=" << cfg.restoreFrame
                                  << " black-at/after-resize=" << afterResize
                                  << "/" << blackFrames << std::endl;
                    }
                    // 黑帧占比台阶（litRatio 直方粗分档：全 0 / 近 0 / 部分亮）
                    u32 zeroPx = 0, nearZero = 0, partial = 0;
                    for (u32 fr : blackFrameList) {
                        if (fr < frameLitRatio.size()) {
                            const double r = frameLitRatio[fr];
                            if (r == 0.0) ++zeroPx;
                            else if (r < 0.005) ++nearZero;
                            else ++partial;
                        }
                    }
                    std::cerr << "[BLACK_DIST] litRatio bins: allZero=" << zeroPx
                              << " <0.5%=" << nearZero << " 0.5-2%=" << partial
                              << std::endl;
                }
                std::cout << "[RHI_SHARED] frames=" << cfg.totalFrames
                          << " final-res=" << curW << "x" << curH
                          << " avg-lit-ratio="
                          << (litRatioSum / double(cfg.totalFrames))
                          << " avg-luma=" << (lumSum / double(cfg.totalFrames))
                          << " shadows="
                          << (cfg.shadows ? (cfg.shadowAblation ? "abl(LitAll)" : "on")
                                          : "off")
                          << " post=" << (cfg.post ? "on" : "off")
                          << " deferred=" << (cfg.deferred ? "on" : "off")
                          << " wall=" << ms << " ms ("
                          << (ms / cfg.totalFrames) << " ms/frame)" << std::endl;
                // M6：GI 模式黑帧守卫观测量（几何覆盖率）+ 颜色暗帧计数——单帧
                // 暗帧不判失败（完全阴影几何恰为黑是直射-only 契约），如实输出；
                // 但全部帧颜色暗（几何覆盖正常）= 没有任何证据表明 direct/
                // indirect/composite 颜色链出过光 → 判失败。
                if (cfg.gi) {
                    std::cout << "[GI] black-frame guard: geometry coverage (depth<1) "
                                 "min=" << giMinCoverage << " (fail <0.02); dark frames "
                                 "(post litRatio<0.02 with coverage>=0.02)="
                              << giDarkFrames << "/" << cfg.totalFrames << std::endl;
                    Require(giDarkFrames != cfg.totalFrames,
                            "GI mode: all " + std::to_string(cfg.totalFrames) +
                            " frames colour-dark (coverage ok) — no evidence the "
                            "direct/indirect/composite colour chain produced light");
                }
                // M3 消融统计输出：全帧累计直方图的分位数 + 贴白率（POST
                // A/B 与 EV 单调性证据；POST=0 时无此统计——对照组统计在
                // 读回循环里同式计算，由外部脚本对两次运行输出做 diff）。
                if (cfg.post && pxAll > 0) {
                    auto pct = [&](double q) -> u32 {
                        const u64 target = u64(double(pxAll) * q);
                        u64 acc = 0;
                        for (u32 b = 0; b < 256; ++b) {
                            acc += histAll[b];
                            if (acc >= target) return b;
                        }
                        return 255;
                    };
                    std::cout << std::fixed << std::setprecision(4)
                              << "[RHI_SHARED] post-stats: pixels=" << pxAll
                              << " clipwhite=" << (double(clipWhiteAll) / double(pxAll))
                              << " p50=" << pct(0.50) << " p90=" << pct(0.90)
                              << " p99=" << pct(0.99) << " p999=" << pct(0.999)
                              << std::endl;
                    std::cout.unsetf(std::ios::fixed);
                }

                device->DestroyCommandBuffer(cmdHandle);
                g_ledger.cmds.erase(cmdHandle);
            }
            DestroyFrameTargets(device, ft);
        }
    }

    // ---- 全量销毁（F1-12：退出前显式平衡账本）----
    for (auto& e : meshes) {
        if (e.litMaterialSet != handles::INVALID_DESCRIPTOR_SET) {
            device->DestroyDescriptorSet(e.litMaterialSet);
            g_ledger.sets.erase(e.litMaterialSet);
        }
        if (e.materialSet != handles::INVALID_RESOURCE) {
            device->DestroyDescriptorSet(e.materialSet);
            g_ledger.sets.erase(e.materialSet);
        }
        if (e.albedoTex != handles::INVALID_RESOURCE) {
            device->DestroyTexture(e.albedoTex);
            g_ledger.textures.erase(e.albedoTex);
        }
        if (e.mesh) {
            e.mesh->Destroy(device);
            delete e.mesh;
            g_ledger.meshCount--;
        }
    }
    meshes.clear();
    for (auto& info : sceneInfos) {
        if (info.ownsMeshContentResource &&
            info.meshEntityId != primal::id::invalid_id)
            primal::content::destroy_resource(
                info.meshEntityId, primal::content::asset_type::mesh,
                primal::content::GraphicsAPI::RHI);
    }
    sceneInfos.clear();
    if (cfg.gpudriven) {
        // M5 验证账本汇总（报告量化：全通过帧/有剔除帧/断言失败）
        std::cout << "[GPUDRIVEN] summary: allPassFrames=" << m5gd.framesAllPass
                  << " culledFrames=" << m5gd.framesCulled
                  << " assertFails=" << m5gd.assertFails << std::endl;
        M5GpuDrivenDestroy(device, m5gd);
    }
    if (cfg.shadows) {
        M2ShadowDestroy(device, shadow);
    }
    if (cfg.deferred) {
        M4DeferredDestroy(device, deferred4);
        // M6：GI 段汇总（重置/收敛读回/dump/导出计数）+ 资源销毁（账本平衡）
        std::cout << "[GI] summary: resets=" << m6gi.resetCount
                  << " ladderFrames=" << m6gi.readbackFrames
                  << " gridDumps=" << m6gi.dumpCount
                  << " exports=" << m6gi.exportFrames << std::endl;
        M6GIDestroy(device, m6gi);
    }
    if (cfg.post) {
        M3PostDestroy(device, post);
    }
    if (globalSet != handles::INVALID_RESOURCE) {
        device->DestroyDescriptorSet(globalSet);
        g_ledger.sets.erase(globalSet);
    }
    if (pipeline != handles::INVALID_PIPELINE) {
        device->DestroyPipeline(pipeline);
        g_ledger.pipelines.erase(pipeline);
    }
    if (pipelineLayout != handles::INVALID_PIPELINE_LAYOUT) {
        device->DestroyPipelineLayout(pipelineLayout);
        g_ledger.layouts.erase(pipelineLayout);
    }
    if (materialLayout != handles::INVALID_RESOURCE) {
        device->DestroyDescriptorSetLayout(materialLayout);
        g_ledger.setLayouts.erase(materialLayout);
    }
    if (globalLayout != handles::INVALID_RESOURCE) {
        device->DestroyDescriptorSetLayout(globalLayout);
        g_ledger.setLayouts.erase(globalLayout);
    }
    if (sampler != handles::INVALID_SAMPLER) {
        device->DestroySampler(sampler);
        g_ledger.samplers.erase(sampler);
    }
    if (lightBuf != handles::INVALID_RESOURCE) {
        device->DestroyBuffer(lightBuf);
        g_ledger.buffers.erase(lightBuf);
    }
    if (viewBuf != handles::INVALID_RESOURCE) {
        device->DestroyBuffer(viewBuf);
        g_ledger.buffers.erase(viewBuf);
    }
    if (fs != handles::INVALID_SHADER) {
        device->DestroyShader(fs);
        g_ledger.shaders.erase(fs);
    }
    if (vs != handles::INVALID_SHADER) {
        device->DestroyShader(vs);
        g_ledger.shaders.erase(vs);
    }

    g_ledger.Print("before-shutdown");
    device->WaitIdle();
    device->Shutdown();
    DestroyRHIDevice(device);
    g_ledger.Print("after-shutdown");

    if (g_failures != 0) return 1;
    if (!g_ledger.AllEmpty()) {
        std::cerr << "[RHI_SHARED] FAIL: resource ledger not balanced "
                     "(see after-shutdown)"
                  << std::endl;
        return 1;
    }
    std::cout << "[RHI_SHARED] PASS: " << cfg.totalFrames
              << " frames, dt=" << traj.dt << ", ledger balanced, capture dir="
              << cfg.captureDir << std::endl;
    return 0;
}

} // anonymous namespace

// ============================= main =============================

int main() {
    const auto tBegin = std::chrono::high_resolution_clock::now();

    RunConfig cfg;
    cfg.platformName = EnvOr("RHI_TEST_PLATFORM", "vulkan");
    if (cfg.platformName != "vulkan" && cfg.platformName != "d3d12") {
        std::cerr << "[RHI_SHARED] config error: RHI_TEST_PLATFORM must be "
                     "vulkan|d3d12, got '"
                  << cfg.platformName << "'" << std::endl;
        return 2;
    }
    cfg.wantD3D12 = (cfg.platformName == "d3d12");

    // ---- 轨迹装载 ----
    const std::string trajRel =
        EnvOr("RHI_TRAJECTORY", "EngineTest/trajectories/m1_sponza_forward.json");
    std::vector<u8> trajBytes = ReadFileRel(trajRel);
    if (trajBytes.empty()) {
        std::cerr << "[RHI_SHARED] config error: trajectory not found: "
                  << trajRel << std::endl;
        return 2;
    }
    {
        auto parsed = pfxjson::parse(
            reinterpret_cast<const char*>(trajBytes.data()), trajBytes.size());
        std::string err;
        if (!parsed.ok || !parsed.root ||
            !ParseTrajectory(*parsed.root, cfg.traj, err)) {
            std::cerr << "[RHI_SHARED] config error: trajectory parse failed: "
                      << (parsed.ok ? err : parsed.error) << std::endl;
            return 2;
        }
    }
    std::cout << "[RHI_SHARED] trajectory=" << cfg.traj.name
              << " asset=" << cfg.traj.assetPath
              << " dt=" << cfg.traj.dt
              << " frames=" << cfg.traj.totalFrames
              << " warmup=" << cfg.traj.warmupFrames
              << " res=" << cfg.traj.width << "x" << cfg.traj.height
              << " interp=" << (cfg.traj.staticCam ? "static" : "linear")
              << " keyframes=" << cfg.traj.keyframes.size() << std::endl;

    // ---- 帧数 / capture / resize 参数（env 覆盖；默认取轨迹/派生值）----
    cfg.totalFrames = cfg.traj.totalFrames;
    {
        std::string v = EnvOr("RHI_TEST_FRAMES", "");
        if (!v.empty())
            cfg.totalFrames = u32(std::strtoul(v.c_str(), nullptr, 10));
    }
    if (cfg.totalFrames == 0) {
        std::cerr << "[RHI_SHARED] config error: RHI_TEST_FRAMES==0" << std::endl;
        return 2;
    }
    cfg.smoke = cfg.totalFrames <= 8;

    // M2 F2-6：阴影段开关（默认关 = M1 行为；轨迹 env 可开）。
    // RHI_SHADOW_LIT_ALL=1 消融档：比较采样器传 Always（RHI 默认坑值）
    // → 采样恒 1 → 无阴影对照帧（阴影可见性 A/B 统计用）。
    cfg.shadows = EnvOr("RHI_TEST_SHADOWS", "0") == "1";
    cfg.shadowAblation = cfg.shadows && EnvOr("RHI_SHADOW_LIT_ALL", "0") == "1";
    // M3 F3-2：post 段开关（默认关 = M1/M2 行为不变；组合验证
    // RHI_TEST_POST=1 + RHI_TEST_SHADOWS=1）。曝光 = 轨迹 render.post.exposure
    //（默认 1.0）× 2^RHI_POST_EV（EV 覆盖默认 0；±0.5 单调性档用）。
    cfg.post = EnvOr("RHI_TEST_POST", "0") == "1";
    cfg.postExposure = cfg.traj.postExposure;
    {
        const char* ev = std::getenv("RHI_POST_EV");
        cfg.postEV = ev ? float(std::atof(ev)) : 0.0f;
    }
    // M4 F4-2：deferred 段开关（默认关 = M1/M2/M3 行为不变）。组合前置
    // RHI_TEST_SHADOWS=1——延迟光照绑定 shadow map/比较采样器（无阴影档
    // 为配置错误，不给静默错误绑定）。
    cfg.deferred = EnvOr("RHI_TEST_DEFERRED", "0") == "1";
    if (cfg.deferred && !cfg.shadows) {
        std::cerr << "[RHI_SHARED] config error: RHI_TEST_DEFERRED=1 requires "
                     "RHI_TEST_SHADOWS=1 (deferred light binds shadow map)"
                  << std::endl;
        return 2;
    }
    // M5 F5-1/F5-2：GPU-driven 直绘切片（默认 0 = M1-M4 行为不变）。可与
    // SHADOWS/POST/DEFERRED 任意组合（剔除链录制于 cmd Begin 后、阴影深度
    // pass 前；lit/deferred gbuffer 的逐 mesh 绘制改间接）。距离剔除阈值
    // RHI_TEST_GPUDRIVEN_MAXDIST（默认 250；≥场景对角线时全帧 frustum-only）。
    cfg.gpudriven = EnvOr("RHI_TEST_GPUDRIVEN", "0") == "1";
    {
        const char* md = std::getenv("RHI_TEST_GPUDRIVEN_MAXDIST");
        cfg.gpudrivenMaxDist = md ? float(std::atof(md)) : 250.0f;
    }
    // M5 F5-5：compute mipgen 独立验证（默认 0；自建自毁资源，不并主轨迹）
    cfg.mipgen = EnvOr("RHI_TEST_MIPGEN", "0") == "1";
    // M6 F6-2：probe-grid GI 切片（默认 0 = M1-M5 行为不变）。组合前置
    // RHI_TEST_DEFERRED=1——GI 链建在 GBuffer + 延迟直射 pass 之上（P2 直射
    // 复用延迟光照 set；无延迟档为配置错误，不给静默错误绑定）。RHI_GI_DUMP=1
    // 时 ladder 帧中央 slice 落 PPM；[GI_MEAN] 收敛阶梯恒输出（GI=1 时）。
    cfg.gi = EnvOr("RHI_TEST_GI", "0") == "1";
    if (cfg.gi && !cfg.deferred) {
        std::cerr << "[RHI_SHARED] config error: RHI_TEST_GI=1 requires "
                     "RHI_TEST_DEFERRED=1 (GI chain consumes the deferred "
                     "GBuffer + direct-light pass)"
                  << std::endl;
        return 2;
    }
    // M6 spec §6.5：原始浮点导出（opt-in；DEFERRED=1 时生效，GI on/off 均可）。
    // RHI_GI_EXPORT_FRAMES=<csv 帧号>，RHI_GI_EXPORT_DIR 缺省 = captureDir。
    for (u32 fr : ParseCsvU32(EnvOr("RHI_GI_EXPORT_FRAMES", "")))
        cfg.giExportFrames.insert(fr);
    cfg.giExportDir = EnvOr("RHI_GI_EXPORT_DIR", "");
    if (!cfg.giExportFrames.empty() && !cfg.deferred)
        std::cerr << "[GI_EXPORT] note: RHI_GI_EXPORT_FRAMES ignored "
                     "(requires RHI_TEST_DEFERRED=1)" << std::endl;
    // 边界登记回显：dir_to_light 关键帧存在时不重导 M2 阴影级联 VP。
    if (!cfg.traj.lightKeys.empty()) {
        bool hasDir = false;
        for (const auto& k : cfg.traj.lightKeys) hasDir |= k.hasDir;
        if (hasDir)
            std::cerr << "[RHI_SHARED] note: light dir keyframes present; M2 "
                         "shadow cascade VP is NOT re-derived (documented "
                         "boundary — use intensity/color keyframes)"
                      << std::endl;
    }
    // 光照方向真值回填（M2ShadowInit 的 light VP 帧常数计算消费）
    g_trajDirToLight = cfg.traj.dirToLight;

    // resize 段：默认帧数够才启用（冒烟 ≤8 帧默认跳过）；中途改一次尺寸，
    // 若干帧后改回（reference 帧分辨率保持一致 → manifest dims 一致）。
    const u32 kNoResize = 0xFFFFFFFFu;
    cfg.resizeFrame = cfg.smoke ? kNoResize : cfg.totalFrames / 2;
    cfg.restoreFrame = kNoResize;
    {
        std::string v = EnvOr("RHI_RESIZE_FRAME", "");
        if (!v.empty())
            cfg.resizeFrame = u32(std::strtoul(v.c_str(), nullptr, 10));
        v = EnvOr("RHI_RESTORE_FRAME", "");
        if (!v.empty())
            cfg.restoreFrame = u32(std::strtoul(v.c_str(), nullptr, 10));
        else if (cfg.resizeFrame != kNoResize)
            cfg.restoreFrame = cfg.resizeFrame + 10;
        v = EnvOr("RHI_RESIZE_TO", "1600,900");
        auto dims = ParseCsvU32(v);
        if (dims.size() == 2 && dims[0] && dims[1]) {
            cfg.resizeW = dims[0];
            cfg.resizeH = dims[1];
        }
        if (cfg.resizeFrame != kNoResize &&
            cfg.resizeFrame >= cfg.totalFrames - 1)
            cfg.resizeFrame = kNoResize;
        if (cfg.resizeFrame != kNoResize &&
            cfg.restoreFrame >= cfg.totalFrames)
            cfg.restoreFrame = cfg.totalFrames - 1;
    }

    {
        // resize 时代 [resizeFrame, restoreFrame)：尺寸不同段，恒用 resize
        // tag 单独落帧；默认 capture 采样剔除该区间（显式指定则不剔除，
        // 落盘时仍按 resize tag 分流）。
        auto inResizeEra = [&](u32 f) {
            return cfg.resizeFrame != kNoResize && f >= cfg.resizeFrame &&
                   f < cfg.restoreFrame;
        };
        std::string spec = EnvOr("RHI_CAPTURE_FRAMES", "");
        if (!spec.empty()) {
            for (u32 f : ParseCsvU32(spec))
                if (f < cfg.totalFrames) cfg.captureFrames.insert(f);
        } else {
            // 默认采样：均匀 6 点 + 末帧，剔除 resize 时代。
            const u32 step = std::max<u32>(1, cfg.totalFrames / 6);
            for (u32 f = 0; f < cfg.totalFrames; f += step)
                if (!inResizeEra(f)) cfg.captureFrames.insert(f);
            if (!inResizeEra(cfg.totalFrames - 1))
                cfg.captureFrames.insert(cfg.totalFrames - 1);
        }
    }

    cfg.captureDir = EnvOr("RHI_CAPTURE_DIR",
                           "/tmp/d3d12_m1_evidence/" + cfg.platformName + "_frames");
    std::error_code ec;
    std::filesystem::create_directories(cfg.captureDir, ec);

    std::cout << "[RHI_SHARED] config: platform=" << cfg.platformName
              << " frames=" << cfg.totalFrames
              << " capture=" << cfg.captureFrames.size() << " frames"
              << " shadows=" << (cfg.shadows ? "on" : "off")
              << (cfg.shadowAblation ? "(ablation:LitAll)" : "")
              << " post=" << (cfg.post ? "on" : "off")
              << (cfg.post ? " (exposure=" + std::to_string(cfg.postExposure) +
                                "×2^" + std::to_string(cfg.postEV) + ")" : "")
              << " deferred=" << (cfg.deferred ? "on" : "off")
              << " gi=" << (cfg.gi ? "on" : (cfg.deferred ? "off(placeholder b8-b10)" : "off"))
              << (cfg.traj.lightKeys.empty()
                      ? std::string("")
                      : " lightkeyframes=" + std::to_string(cfg.traj.lightKeys.size()))
              << " resize="
              << (cfg.resizeFrame == kNoResize
                      ? std::string("off")
                      : std::to_string(cfg.resizeFrame) + "->" +
                            std::to_string(cfg.resizeW) + "x" +
                            std::to_string(cfg.resizeH) + ", restore@" +
                            std::to_string(cfg.restoreFrame))
              << " dir=" << cfg.captureDir << std::endl;

    // ---- 设备（唯一 platform 分支点：desc.platform 枚举值）----
    DeviceDesc desc{};
    desc.platform = cfg.wantD3D12 ? RHIPlatform::D3D12 : RHIPlatform::Vulkan;
    desc.enableValidation = true;
    desc.enableDebug = true;
    RHIDeviceBase* device = CreateRHIDevice(desc);
    if (!device) {
        if (cfg.wantD3D12) {
            // macOS/未启 D3D12 构建：工厂按 M0 F0-1 契约返回 nullptr。
            std::cout << "[RHI_SHARED] SKIP: platform d3d12 unavailable on this "
                         "build (factory returned nullptr) — rerun this same "
                         "binary on the Windows integration session."
                      << std::endl;
            return 42;
        }
        std::cerr << "[RHI_SHARED] FAIL: Vulkan device creation failed"
                  << std::endl;
        return 1;
    }
    std::cout << "[RHI_SHARED] device: " << device->GetDeviceInfo().deviceName
              << " platform=" << int(device->GetPlatform()) << std::endl;

    const int rc = RunHost(device, cfg);

    const auto tEnd = std::chrono::high_resolution_clock::now();
    std::cout << "[RHI_SHARED] total wall="
              << std::chrono::duration<double, std::milli>(tEnd - tBegin).count()
              << " ms exit=" << rc << std::endl;
    return rc;
}
