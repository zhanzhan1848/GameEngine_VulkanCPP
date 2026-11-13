/**
 * @file LumenDDGIPassIO.cpp
 * @brief LumenDDGIPass 的 shader 装载与诊断读回辅助实现
 *
 * R3：自 LumenDDGIPass.cpp 逐字搬移（MSVC C1001 预防性拆分，见
 * LumenDDGIPassIO.h 头注释）。搬移范围：SHADER_BASE_DIR / DDGI_SHADER_DIR、
 * ReadFileToString / ResolveIncludes / LoadShaderBytecode /
 * DumpRelocatedOriginStats——与类状态零耦合（仅按参数工作）。
 */
#include "LumenDDGIPassIO.h"

#include "LumenDDGIPass.h"   // DDGIRuntimeParams / DDGIVolumeData

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Engine/Common/CoreLogStream.h"
#include "Engine/Graphics/Dawn/ShaderLoader.h"  // for dawn::LoadWGSL
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace primal::graphics::lumen {

namespace {

static const std::string SHADER_BASE_DIR =
    utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Metal/shaders/") + "/";
static const std::string DDGI_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Recursively resolve #include "..." directives by inlining file content.
// Only resolves local includes (quoted), not system includes (angle brackets).
static std::string ResolveIncludes(const std::string& source, const std::string& baseDir,
                                   std::set<std::string>& included) {
    std::istringstream in(source);
    std::ostringstream out;
    std::string line;

    while (std::getline(in, line)) {
        // Check for #include "..." (local include, not <...> system include)
        std::string trimmed = line;
        size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos) trimmed = trimmed.substr(firstNonSpace);

        if (trimmed.find("#include \"") == 0) {
            size_t start = trimmed.find('"') + 1;
            size_t end = trimmed.find('"', start);
            if (start != std::string::npos && end != std::string::npos) {
                std::string includeFile = trimmed.substr(start, end - start);

                // Search in baseDir first, then SHADER_BASE_DIR
                std::string fullPath = baseDir + includeFile;
                if (std::ifstream(fullPath).good() == false) {
                    fullPath = SHADER_BASE_DIR + includeFile;
                }

                if (included.find(fullPath) == included.end()) {
                    included.insert(fullPath);
                    std::string includedContent = ReadFileToString(fullPath);
                    if (!includedContent.empty()) {
                        // Recursively resolve includes in the included file
                        std::string resolved = ResolveIncludes(includedContent,
                            fullPath.substr(0, fullPath.find_last_of('/') + 1), included);
                        out << resolved << "\n";
                    } else {
                        PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] Warning: Failed to read include: " << fullPath << std::endl;
                        out << line << "\n";
                    }
                }
                // If already included, skip (header guard / #pragma once handles it)
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

}  // namespace

std::vector<u8> LoadShaderBytecode(const char* shaderName, rhi::RHIDeviceBase* device) {
    auto platform = device ? device->GetPlatform() : rhi::RHIPlatform::Metal;

    if (platform == rhi::RHIPlatform::Dawn) {
        // WGSL path. dawn::LoadWGSL has two overloads (ShaderLoader.h):
        //   WASM:   LoadWGSL(const char* name) — kShaderMap + MEMFS fallback at
        //           /Engine/Graphics/Dawn/shaders/{name}.wgsl
        //   Native: LoadWGSL(const std::string& path) — opens filesystem path as-is
        // DDGI shaders live in Engine/Graphics/Dawn/shaders/Lumen/. On native
        // we pass the full path. On WASM, Emscripten --embed-file preserves
        // subdirs, so the shader is at /Engine/Graphics/Dawn/shaders/Lumen/ in
        // MEMFS — pass a relative name and let LoadWGSL's fallback construct
        // the right path.
        std::string src;
#ifdef __EMSCRIPTEN__
        std::string lumenName = std::string("Lumen/") + shaderName;
        src = dawn::LoadWGSL(lumenName.c_str());
#else
        std::string path = std::string("Engine/Graphics/Dawn/shaders/Lumen/") + shaderName + ".wgsl";
        src = dawn::LoadWGSL(path);
#endif
        if (src.empty()) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] Failed to load WGSL shader: " << shaderName << std::endl;
            return {};
        }
        return std::vector<u8>(src.begin(), src.end());
    }

    if (platform == rhi::RHIPlatform::Vulkan || platform == rhi::RHIPlatform::D3D12) {
        // Phase 2 Vulkan Lumen port: load precompiled SPIR-V from
        // Engine/Graphics/Vulkan/shaders/Lumen/<name>.comp.spv (hand-written
        // GLSL compiled by build_spv.sh). Uses .comp.spv to distinguish from
        // the broken naga WGSL .spv products (spvUnsafeArray MoltenVK issue).
        // D3D12: DXIL .cso 镜像（EngineTest/shaders/d3d12/Lumen/<name>.comp.cso，
        // 入口恒 "main"），与 spv 同一相对路径形态。
        const std::string suffix =
            (platform == rhi::RHIPlatform::D3D12) ? ".comp.cso" : ".comp.spv";
        const std::string relPath =
            utils::ShaderRegistry::GetShaderBaseDir(platform) + "Lumen/" + shaderName + suffix;
        const std::vector<std::string> candidates = {
            relPath,
            utils::ShaderRegistry::ResolveShaderPath(relPath),
        };
        for (const auto& path : candidates) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) continue;
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<u8> bytecode(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] Failed to read SPIR-V: " << path << std::endl;
                return {};
            }
            return bytecode;
        }
        PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] Failed to load SPIR-V: " << shaderName << std::endl;
        return {};
    }

    // Metal path (unchanged): file read + #include resolution + /tmp/ dump
    std::string shaderPath = DDGI_SHADER_DIR + shaderName + ".metal";
    std::string source = ReadFileToString(shaderPath);

    // T8.13 parity track: PRIMAL_DDGI_PARITY=1 prefers <name>Parity.metal —
    // faithful ports of the Vulkan .comp math — per stage, falling back to the
    // engine shader whenever the port doesn't exist yet. Lets the DDGI chain
    // be walked stage-by-stage onto Vulkan math without touching the engine
    // defaults.
    if (std::getenv("PRIMAL_DDGI_PARITY")) {
        const std::string parityPath = DDGI_SHADER_DIR + shaderName + "Parity.metal";
        std::string paritySource = ReadFileToString(parityPath);
        if (!paritySource.empty()) {
            shaderPath = parityPath;
            source = std::move(paritySource);
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[LumenDDGI] Parity port in effect: " << shaderName << "Parity.metal" << std::endl;
        }
    }

    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Lumen/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }

    if (source.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    // Resolve all #include "..." directives by inlining
    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, DDGI_SHADER_DIR, included);

    // DDGI_PATH_DEBUG=1：finalize 将辐射来源 id（1=SC 2=prev-screen
    // 3=analytic）编码进命中距离的低两位尾数；保留真实距离供深度矩更新，
    // 配合 DDGI_DUMP=2 的 ray-path 读回定位辐射来源。
    if (std::getenv("DDGI_PATH_DEBUG")) {
        resolved = "#define DDGI_PATH_DEBUG 1\n" + resolved;
    }

    // C 系列归因旋钮（编译期诊断变体，默认全部关闭）：逐条断电不查遮挡的
    // 能量注入路径，干预式 A/B 定位探针场能量来源（计划文档 §C3）。
    for (const char* knob : {"PRIMAL_DDGI_NO_ANALYTIC", "PRIMAL_DDGI_NO_SKY",
                             "PRIMAL_DDGI_NO_PREVSCREEN", "PRIMAL_DDGI_PROJ_NOSKY"}) {
        if (std::getenv(knob)) {
            resolved = std::string("#define ") + knob + " 1\n" + resolved;
        }
    }

    // DEBUG: Dump resolved source to file
    {
        std::string dumpPath = "/tmp/ddgi_" + std::string(shaderName) + ".resolved.metal";
        std::ofstream dumpFile(dumpPath);
        if (dumpFile.is_open()) {
            dumpFile << resolved;
            dumpFile.close();
            PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] Dumped resolved source to " << dumpPath << " (" << resolved.size() << " bytes)" << std::endl;
        }
    }

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// 2026-09-17 审计"测有效覆盖"（DDGI_ORIGIN_STATS=1 一次性）：同步读回
// relocated origins 缓冲，统计重定位分类分布（本格/借格/未 trace）与位移量
// 分位数。同步停等仅诊断路径使用（与 FusionCompositeModule::RunGiLadderDump
// 同模式——同队列提交即保序，读到的就是本帧 trace 刚写的内容）。
// 口径对应审计 §3.1 的"候选数/激活数/失活原因"缺口中的重定位维度；
// 覆盖率细分（inactive/零权重率）仍需逐探针 gather 诊断，未含。
void DumpRelocatedOriginStats(rhi::RHIDeviceBase* device,
                              rhi::ResourceHandle originsBuf,
                              const DDGIRuntimeParams& params,
                              const DDGIVolumeData& volume) {
    const u32 total = params.probe_count_x * params.probe_count_y * params.probe_count_z;
    if (total == 0 || originsBuf == handles::INVALID_RESOURCE) return;

    BufferDesc bd{};
    bd.size = (u64)total * sizeof(math::v4);
    bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
    bd.memoryUsage = GPUMemoryUsage::Readback;
    auto stage = device->CreateBuffer(bd);
    if (stage == handles::INVALID_RESOURCE) return;

    auto ch = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* c = rhi::GetCommandBuffer(ch);
    if (!c) { device->DestroyBuffer(stage); return; }
    c->Begin();
    c->CopyBuffer(originsBuf, stage, 0, 0, bd.size);
    c->End(); c->Submit(); c->WaitForCompletion();
    device->DestroyCommandBuffer(ch);

    const auto* data = static_cast<const math::v4*>(device->MapBuffer(stage, 0, bd.size));
    if (!data) { device->DestroyBuffer(stage); return; }

    u32 nUntraced = 0, nSelf = 0, nBorrowed = 0;
    std::vector<float> disp;                 // 已发布条目的 |origin − 格点|
    disp.reserve(total);
    for (u32 i = 0; i < total; ++i) {
        const float w = data[i].w;
        const u32 ix = i % params.probe_count_x;
        const u32 iy = (i / params.probe_count_x) % params.probe_count_y;
        const u32 iz = i / (params.probe_count_x * params.probe_count_y);
        const float gx = volume.ProbeOrigin.x + (float)ix * params.probe_spacing;
        const float gy = volume.ProbeOrigin.y + (float)iy * params.probe_spacing;
        const float gz = volume.ProbeOrigin.z + (float)iz * params.probe_spacing;
        const float dx = data[i].x - gx, dy = data[i].y - gy, dz = data[i].z - gz;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (w >= 0.75f) {
            ++nSelf;
            if (d > 1e-4f) disp.push_back(d);   // 自由未动的不计位移
        } else if (w >= 0.25f) {
            ++nBorrowed;
            disp.push_back(d);
        } else {
            ++nUntraced;
        }
    }
    device->UnmapBuffer(stage);
    device->DestroyBuffer(stage);

    std::sort(disp.begin(), disp.end());
    auto pick = [&](float frac) -> float {
        return disp.empty() ? 0.0f
                            : disp[std::min(disp.size() - 1, (size_t)(disp.size() * frac))];
    };
    PRIMAL_LOG_STREAM(Error, "Render")
        << "[DDGIOriginStats] total=" << total
        << " self=" << nSelf << " (" << (100.0f * nSelf / total) << "%)"
        << " borrowed=" << nBorrowed << " (" << (100.0f * nBorrowed / total) << "%)"
        << " untraced=" << nUntraced
        << " | displaced_probes=" << disp.size()
        << " disp_med=" << pick(0.5f)
        << " disp_P95=" << pick(0.95f)
        << " disp_max=" << (disp.empty() ? 0.0f : disp.back())
        << std::endl;
}

}  // namespace primal::graphics::lumen
