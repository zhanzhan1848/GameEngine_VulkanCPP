#!/usr/bin/env python3
"""build_d3d12_shaders.py — D3D12 M1 F1-7 shader 主链编译器 + freshness gate 原型.

主链（Docs/RHI/D3D12_Shader_Spike_Report.md §5 决定，GLSL=链 B）:
  GLSL 源（与 Vulkan 生产 .spv 同一源文件）
  → glslangValidator -V -S <stage> --source-entrypoint <src-entry> -e main
    （生产 Lumen compute 的源入口是功能名 ddgi_trace_rays/taa_main 等，
    glslang 对非 main 的 --source-entrypoint 会向 stderr 打一行
    "ERROR: Source entry point must be main" 噪声但 rc=0，脚本以 rc 判成败）
  → spirv-cross --hlsl --shader-model 60 → HLSL（入口恒重命名为 main）
  → dxc -T <profile> -E main → DXIL .cso

profile 映射（spike §6）: .vert→vs_6_0, .frag→ps_6_0, .comp→cs_6_0。

SHADER_LIST 条目形态（R3 入口参数化 + R3b spv 旁路，向后兼容裸字符串）:
  "src"                          entry=main, out=src, defines=[]（GLSL 链）
  (src, entry)                   指定 GLSL 源入口（引擎 CreateShader 实参）
  (src, entry, out, [defines])   out 覆盖产物基名（对齐引擎加载的 .spv 名，
                                 同源多产物如 SurfaceCacheCapture/
                                 CaptureWrite/CaptureClear 与 EnvironmentVisibility_
                                 trace/resolve 的 -DENV_VIS_RESOLVE 变体）；
                                 defines 为 glslang -D 参数列表
  (spv, entry, out, stage, "spv")
                                 R3b spv 旁路链：naga WGSL→SPIR-V 家族产物无
                                 GLSL 源，直接以仓库内 .spv 为输入走
                                 spirv-cross --entry <entry> → dxc 两级链
                                 （不经 glslang）。stage 必填（vert/frag/comp，
                                 spv 文件名无 stage 扩展信息）。cross 把选定
                                 入口包装为 HLSL main()，故 dxc 仍 -E main；
                                 cso 命名约定：同一 spv 多入口产物用
                                 <基名>.<入口名>.cso 区分（ToneMapping 三入口、
                                 GPUCullingPipeline 8 入口、GPUDrivenDraw/
                                 ShadowCulling 双入口），单入口产物直接基名。
  (hlsl, entry, out, (stage, wgsl_src), "handwritten")
                                 R3c 手写直译链：naga spv 经 cross 后 dxc 失败
                                 的入口（AtomicExchange 类 cross 缺陷）按 WGSL
                                 源手写 HLSL 直译，源放 scripts/d3d12_handmade/。
                                 hlsl 为仓库相对路径，直过 dxc；wgsl_src 为
                                 VK_SHADERS 相对路径，仅入 manifest 作
                                 provenance（wgsl_source + wgsl_source_sha256）。
                                 绑定寄存器以同 set 已成功入口的 cross 产物
                                 为参照逐项核对（详见 agent-S 报告 §11）。

产物布局（<out-dir> 默认 EngineTest/shaders/d3d12/，镜像源相对路径；
全小写目录与 origin/d3d12-windows-test-20261103 分支布局一致——Windows
侧补丁在该目录上叠加，大小写不一致会分裂目录）:
  <relpath>.hlsl   中间件（spirv-cross 产物，供审阅/后端调试）
  <relpath>.cso    终件（DXIL 字节码，D3D12 管线装载）
  manifest.json    源 SHA → 产物 SHA 清单（freshness gate 数据源）

freshness gate（spike §9）:
  python3 scripts/build_d3d12_shaders.py --check
  重算源/产物 SHA 与 manifest 比对：源变→产物必须变（旧 manifest 失配即
  exit 1）；产物缺失/被改即 exit 1。跨平台 DXIL 字节差异预期存在，比较层
  落在本 SHA 清单，不做字节等值（spike §7）。

仅标准库；工具路径可用环境变量覆盖:
  D3D12_GLSLANG / D3D12_SPIRV_CROSS / D3D12_DXC
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
VK_SHADERS = os.path.join(REPO, "Engine/Graphics/Vulkan/shaders")
# R3: 全小写 d3d12 与 origin/d3d12-windows-test-20261103 分支既有布局一致
# （此前为 EngineTest/shaders/D3D12，macOS 大小写不敏感文件系统下同一路径，
# git 索引记录的是小写形态；Windows 侧大小写敏感，保持全小写防分裂目录）。
DEFAULT_OUT = os.path.join(REPO, "EngineTest/shaders/d3d12")
MANIFEST_NAME = "manifest.json"

GLSLANG = os.environ.get("D3D12_GLSLANG", "/usr/local/bin/glslangValidator")
SPIRV_CROSS = os.environ.get("D3D12_SPIRV_CROSS", "/usr/local/bin/spirv-cross")
DXC = os.environ.get("D3D12_DXC", "/usr/local/bin/dxc")

# ---------------------------------------------------------------------------
# 编译清单：仓库相对 VK_SHADERS 的 GLSL 路径（不带 .spv）。条目语法见模块
# docstring（裸字符串 = entry main；元组可指定源入口/产物基名/-D 宏）。
# M1 冻结清单 = M1Forward 两入口；后续阶段在此追加（如 M2 阴影、M3 后处理）。
# ---------------------------------------------------------------------------
SHADER_LIST = [
    "Forward/M1Forward.vert",
    "Forward/M1Forward.frag",
    # D3D12 M2 F2-5: shadow depth VS (single cascade, no pixel shader) and
    # lit FS (M1Forward.frag + shadow sampling; vertex stage stays M1Forward).
    "Forward/M2ShadowDepth.vert",
    "Forward/M2ForwardLit.frag",
    # D3D12 M3 F3-2: post full-screen triangle VS (vertex-ID driven, no
    # bindings) + exposure/ACES FS (PostParams UBO + HDR sample pair).
    "Forward/M3Post.vert",
    "Forward/M3Post.frag",
    # D3D12 M4 F4-2: deferred GBuffer MRT writer (vert = M1Forward variant
    # with identical layout/push; frag writes 3 RTs SV_Target0/1/2).
    "Forward/M4GBuffer.vert",
    "Forward/M4GBuffer.frag",
    # D3D12 M4 F4-3: deferred full-screen light FS (vertex stage reuses
    # M3Post.vert bytes; 8-binding set0 per reflection contract).
    "Forward/M4DeferredLight.frag",
    # D3D12 M5 F5-1/F5-5: GPU-driven cull compute (5-binding set0) and
    # compute mipmap tool shader (3-binding set0, M7 F7-3 early delivery).
    "Forward/M5Cull.comp",
    "Forward/M5Mipmap.comp",
    # D3D12 M6 F6-2/F6-3: probe-grid GI temporal accumulation compute.
    # Accum: readonly image3D (typed UAV load) + sampler3D history pair +
    # writeonly image3D out + flags UBO (EMA / history-zero reset).
    "Forward/M6ProbeAccum.comp",
    # D3D12 M6 GI rebuild: direct-only deferred light FS (MRT×2 direct +
    # direct-diffuse, b0-b7 identical to M4DeferredLight so the host reuses
    # the M4 light set; replaces M4DeferredLight in GI mode) + SH-L1 probe
    # inject compute (replaces the analytic M6ProbeBake; reads
    # directDiffuse/depth/normal/material + InjectParams UBO 144B + writes
    # rgba16f image3D (8,8,24)) + independent indirect resolve FS (SH probe
    # 3D sampling → albedo·E_h) + single direct+indirect composite FS
    # (texelFetch pair). Vertex stages reuse M3Post.vert bytes.
    "Forward/M6DeferredDirect.frag",
    "Forward/M6ProbeInject.comp",
    "Forward/M6GIResolve.frag",
    "Forward/M6Composite.frag",
    # -----------------------------------------------------------------------
    # R3 生产批（vulkan-parity-r3；清单依据 agent-A-prod-chain.md §6 默认参照
    # run 消费面，GLSL 源子集）。入口名 = 引擎 CreateShader 第四实参（与
    # build_spv.sh --source-entrypoint 一致，出处逐条见 agent-S 报告映射表）。
    # 无 GLSL 源的 .spv（naga WGSL 家族：ToneMapping.spv 三入口、IBL
    # Equirect/Irradiance/BRDF、Nanite GPUCullingPipeline/GPUDrivenDraw/
    # ShadowCulling/ShadowDepth/ShadowBlit/HZBCopy/HZBMip/GlobalSDFVoxelization/
    # GlobalSDFMipmap/MinimalTest）不在本清单——登记于报告阻塞表。
    # -----------------------------------------------------------------------
    # tvsrg SetShaderHandles 注入 13 件 + srp SelfLoad 同源：
    ("Forward/DeferredLighting.vert", "main"),
    ("Forward/DeferredLighting.frag", "main"),
    ("Forward/Blit.vert", "main"),
    ("Forward/Blit.frag", "main"),
    ("ShadowFilter.comp", "main", "ShadowFilter"),
    ("Lumen/FusionIndirect.frag", "fusion_indirect"),
    ("Lumen/FusionComposite.frag", "fusion_composite"),
    ("Lumen/DDGIGIGather.comp", "ddgi_gi_gather"),
    ("Lumen/DDGICardRadianceAvg.comp", "ddgi_card_radiance_avg"),
    ("Lumen/DDGIProbeIrradianceFromCards.comp", "ddgi_probe_irradiance_from_cards"),
    ("Debug/SDFVisualization.frag", "main"),
    ("Debug/ProbeSphere.vert", "main"),
    ("Debug/ProbeSphere.frag", "main"),
    # ForwardSceneRenderer 传统 deferred 路径（VULKAN_SHADER_DIR=
    # shaders/Forward/，load("GBuffer") → Forward/GBuffer.vert.spv+.frag.spv；
    # 顶层 GBuffer.spv 是 WGSL naga 家族，不走本链）：
    ("Forward/GBuffer.vert", "main"),
    ("Forward/GBuffer.frag", "main"),
    # Nanite 手写 GLSL 子集（ClusterBinning 出口名无 .comp infix，out 对齐
    # 引擎加载的 Nanite/ClusterBinning.spv；VisibilityBuffer 在 Vulkan 侧是
    # vert+frag spirv-link 双入口单模块，D3D12 按 stage 出双 cso）：
    ("Nanite/ClusterBinning.comp", "cluster_binning_kernel", "Nanite/ClusterBinning"),
    ("Nanite/VisibilityBuffer.vert", "visibility_vertex_shader"),
    ("Nanite/VisibilityBuffer.frag", "visibility_fragment_shader"),
    ("Nanite/VisibilityBufferResolve.comp", "ComputeMain", "Nanite/VisibilityBufferResolve"),
    # Lumen：SSAO 2 件
    ("Lumen/SSAOTrace.comp", "ssao_trace"),
    ("Lumen/SSAOFilter.comp", "ssao_filter"),
    # Lumen：DDGI 基版 5 件（运行时加载 .comp.spv；无 infix 的 .spv 是别名）
    ("Lumen/DDGITraceRays.comp", "ddgi_trace_rays"),
    ("Lumen/DDGITraceSDF.comp", "ddgi_trace_sdf"),
    ("Lumen/DDGITraceFinalize.comp", "ddgi_trace_finalize"),
    ("Lumen/DDGIUpdateIrradiance.comp", "ddgi_update_irradiance"),
    ("Lumen/DDGIUpdateDepth.comp", "ddgi_update_depth"),
    # Lumen：WRC 5 件
    ("Lumen/WRCTraceSDF.comp", "wrc_trace_sdf"),
    ("Lumen/WRCTraceFinalize.comp", "wrc_trace_finalize"),
    ("Lumen/WRCUpdateRadiance.comp", "wrc_update_radiance"),
    ("Lumen/WRCPrefilterIrradiance.comp", "wrc_prefilter_irradiance"),
    ("Lumen/WRCCardInject.comp", "wrc_card_inject"),
    # Lumen：SurfaceCache compute 系 14 件（CaptureWrite/CaptureClear 与
    # Capture 同源不同入口各出各产物；capture vert/frag 是 Metal-only 路径
    # ——SurfaceCachePass.cpp :1533，Vulkan 走 compute gather，不进本批）：
    ("Lumen/SurfaceCacheAtlasInit.comp", "surface_cache_atlas_init"),
    ("Lumen/SurfaceCacheCapture.comp", "surface_cache_capture"),
    ("Lumen/SurfaceCacheCapture.comp", "surface_cache_capture_write",
     "Lumen/SurfaceCacheCaptureWrite.comp"),
    ("Lumen/SurfaceCacheCapture.comp", "surface_cache_capture_clear",
     "Lumen/SurfaceCacheCaptureClear.comp"),
    ("Lumen/SurfaceCacheCardFill.comp", "surface_cache_card_fill"),
    ("Lumen/SurfaceCacheLightCull.comp", "surface_cache_light_cull"),
    ("Lumen/SurfaceCacheLightEval.comp", "surface_cache_light_eval"),
    ("Lumen/SurfaceCacheDilate.comp", "surface_cache_dilate"),
    ("Lumen/SurfaceCacheStaticComposite.comp", "surface_cache_static_composite"),
    ("Lumen/SurfaceCachePageClear.comp", "surface_cache_page_clear"),
    ("Lumen/SurfaceCacheIndirectTrace.comp", "surface_cache_indirect_trace"),
    ("Lumen/SurfaceCacheIndirectResolve.comp", "surface_cache_indirect_resolve"),
    ("Lumen/SurfaceCacheIndirectReconstruct.comp", "surface_cache_indirect_reconstruct"),
    ("Lumen/SurfaceCacheShadow.comp", "surface_cache_shadow"),
    # Lumen：SPGI 12 件 + RadianceCascadeUpdate（源入口 main）
    ("Lumen/ScreenProbePlace.comp", "screen_probe_place"),
    ("Lumen/ScreenProbeTraceRays.comp", "screen_probe_trace_rays"),
    ("Lumen/ScreenProbeTraceHZB.comp", "screen_probe_trace_hzb"),
    ("Lumen/ScreenProbeTraceSDF.comp", "screen_probe_trace_sdf"),
    ("Lumen/ScreenProbeTraceFinalize.comp", "screen_probe_trace_finalize"),
    ("Lumen/ScreenProbeAverage.comp", "screen_probe_average"),
    ("Lumen/ScreenProbeCompact.comp", "screen_probe_compact"),
    ("Lumen/ScreenProbeLightVar.comp", "screen_probe_light_var"),
    ("Lumen/ScreenProbeGather.comp", "screen_probe_gather"),
    ("Lumen/ScreenProbeTemporal.comp", "screen_probe_temporal"),
    ("Lumen/ScreenProbeSpatialFilter.comp", "screen_probe_spatial_filter"),
    ("Lumen/ScreenProbeDenoise.comp", "screen_probe_denoise"),
    ("Lumen/RadianceCascadeUpdate.comp", "main"),
    # Lumen：EnvironmentVisibility trace/resolve 同源宏变体（build_spv.sh:
    # trace 无宏 / resolve -DENV_VIS_RESOLVE=1，源入口 main）
    ("Lumen/EnvironmentVisibility.comp", "main", "Lumen/EnvironmentVisibility_trace.comp"),
    ("Lumen/EnvironmentVisibility.comp", "main", "Lumen/EnvironmentVisibility_resolve.comp",
     ["-DENV_VIS_RESOLVE=1"]),
    # Lumen：TAA / EyeAdaptation（EyeAdaptation 由 ToneMappingPass.cpp 加载）
    ("Lumen/TAA.comp", "taa_main"),
    ("Lumen/EyeAdaptation.comp", "eye_adaptation"),
    # IBL：GLSL 家族仅 SpecularPrefilter（引擎 IBLPrecomputer 加载 .spv、
    # 入口 cs_main；GLSL 源入口 main，D3D12 cso 入口恒 main）
    ("IBL_SpecularPrefilter.comp", "main", "IBL_SpecularPrefilter"),
    # -----------------------------------------------------------------------
    # R3b spv 旁路批：naga WGSL→SPIR-V 家族（无 GLSL 源）但属默认参照画面
    # 必经（后处理/IBL 初始化/GBuffer GPUDriven 路径与阴影剔除）。输入=
    # 仓库既有 .spv，spirv-cross --entry 按引擎 CreateShader 实参选入口。
    # 入口出处：ToneMappingPass.cpp:212/213/380；tvsrg:2004 与
    # IBLPrecomputer.cpp:271-273（cs_main）；GPUDrivenDrawPipeline.cpp:1358-
    # 1366/3456-3459；GPUCullingPipeline.cpp:323-330（stage0-7）；
    # HZBSystem.cpp:282/290；GlobalSDF.cpp:582/715。
    # -----------------------------------------------------------------------
    # ToneMapping.spv 三入口单模块（ACES FS / passthrough FS / 全屏 VS）：
    ("ToneMapping.spv", "tonemap_vs", "ToneMapping.tonemap_vs", "vert", "spv"),
    ("ToneMapping.spv", "tonemap_fs", "ToneMapping.tonemap_fs", "frag", "spv"),
    ("ToneMapping.spv", "tonemap_fs_passthrough", "ToneMapping.tonemap_fs_passthrough",
     "frag", "spv"),
    # IBL 三件（单入口 cs_main，产物直接基名）：
    ("IBL_EquirectangularToCube.spv", "cs_main", "IBL_EquirectangularToCube",
     "comp", "spv"),
    ("IBL_IrradianceConvolution.spv", "cs_main", "IBL_IrradianceConvolution",
     "comp", "spv"),
    ("IBL_BRDFIntegration.spv", "cs_main", "IBL_BRDFIntegration", "comp", "spv"),
    # Nanite 9 个 naga spv。GPUCullingPipeline 8 入口（stage0-7）逐条目。
    # R3b 三个 InterlockedExchange 类失败入口（stage0_reset_all_buffers /
    # stage4_cluster_expansion / shadow_finalize_indirect——naga
    # AtomicExchange 结果 SSA 未被 cross 物化，dxc 报未声明标识符）在 R3c
    # 以 WGSL 为语义基准手写 HLSL 直译补编（scripts/d3d12_handmade/，
    # 绑定寄存器以同 set 已成功入口的 cross 产物为参照），见 §11 批注。
    ("scripts/d3d12_handmade/GPUCullingPipeline.stage0_reset_all_buffers.hlsl",
     "stage0_reset_all_buffers",
     "Nanite/GPUCullingPipeline.stage0_reset_all_buffers",
     ("comp", "Engine/Graphics/Dawn/shaders/Nanite/GPUCullingPipeline.wgsl"), "handwritten"),
    ("scripts/d3d12_handmade/GPUCullingPipeline.stage4_cluster_expansion.hlsl",
     "stage4_cluster_expansion",
     "Nanite/GPUCullingPipeline.stage4_cluster_expansion",
     ("comp", "Engine/Graphics/Dawn/shaders/Nanite/GPUCullingPipeline.wgsl"), "handwritten"),
    ("scripts/d3d12_handmade/ShadowCulling.shadow_finalize_indirect.hlsl",
     "shadow_finalize_indirect",
     "Nanite/ShadowCulling.shadow_finalize_indirect",
     ("comp", "Engine/Graphics/Dawn/shaders/Nanite/ShadowCulling.wgsl"), "handwritten"),
    ("Nanite/GPUCullingPipeline.spv", "stage1_instance_frustum_culling",
     "Nanite/GPUCullingPipeline.stage1_instance_frustum_culling", "comp", "spv"),
    ("Nanite/GPUCullingPipeline.spv", "stage2_distance_small_object_culling",
     "Nanite/GPUCullingPipeline.stage2_distance_small_object_culling", "comp", "spv"),
    ("Nanite/GPUCullingPipeline.spv", "stage3_lod_selection",
     "Nanite/GPUCullingPipeline.stage3_lod_selection", "comp", "spv"),
    ("Nanite/GPUCullingPipeline.spv", "stage5_occlusion_culling",
     "Nanite/GPUCullingPipeline.stage5_occlusion_culling", "comp", "spv"),
    ("Nanite/GPUCullingPipeline.spv", "stage6_compact_visible_list",
     "Nanite/GPUCullingPipeline.stage6_compact_visible_list", "comp", "spv"),
    ("Nanite/GPUCullingPipeline.spv", "stage7_build_indirect_commands",
     "Nanite/GPUCullingPipeline.stage7_build_indirect_commands", "comp", "spv"),
    # GPUDrivenDraw.spv 双入口跨 stage（VS/PS 各一 cso；Vulkan 是单模块，
    # D3D12 按 stage 分件——装配层适配点）：
    ("Nanite/GPUDrivenDraw.spv", "gpu_driven_vertex_shader",
     "Nanite/GPUDrivenDraw.gpu_driven_vertex_shader", "vert", "spv"),
    ("Nanite/GPUDrivenDraw.spv", "gpu_driven_fragment_shader",
     "Nanite/GPUDrivenDraw.gpu_driven_fragment_shader", "frag", "spv"),
    # ShadowCulling.spv 双 compute 入口：cluster_culling 走 spv 旁路；
    # shadow_finalize_indirect 的 spv 旁路失败（同 InterlockedExchange 根因），
    # R3c 手写直译补编（清单前段 handwritten 条目）。
    ("Nanite/ShadowCulling.spv", "shadow_cluster_culling",
     "Nanite/ShadowCulling.shadow_cluster_culling", "comp", "spv"),
    # 单入口件（产物直接基名；HZBMip.spv 实测无 _basic 入口——
    # HZBSystem.cpp:385 的 generate_hzb_mip_level_basic 路径在 Vulkan 下
    # 本就无对应入口，属引擎 fallback，不编）：
    ("Nanite/ShadowDepth.spv", "shadow_depth_vs", "Nanite/ShadowDepth", "vert", "spv"),
    ("Nanite/ShadowBlit.spv", "shadow_depth_blit", "Nanite/ShadowBlit", "comp", "spv"),
    ("Nanite/HZBCopy.spv", "copy_depth_to_hzb_mip0", "Nanite/HZBCopy", "comp", "spv"),
    ("Nanite/HZBMip.spv", "generate_hzb_mip_level", "Nanite/HZBMip", "comp", "spv"),
    ("Nanite/GlobalSDFVoxelization.spv", "voxelize_sdf", "Nanite/GlobalSDFVoxelization",
     "comp", "spv"),
    ("Nanite/GlobalSDFMipmap.spv", "global_sdf_mipmap", "Nanite/GlobalSDFMipmap",
     "comp", "spv"),
]


def normalize_entry(item):
    """SHADER_LIST/CLI 条目 → (src, entry, out, extra, kind)。
    裸字符串=GLSL 链 entry main；extra 在 GLSL 链=defines 列表、在 spv
    旁路=stage 字符串、在 handwritten 链=(stage, wgsl_src) 元组。"""
    if isinstance(item, str):
        return item, "main", item, [], "glsl"
    kind = item[4] if len(item) > 4 and item[4] in ("spv", "handwritten") else "glsl"
    src, entry = item[0], item[1]
    out = item[2] if len(item) > 2 and item[2] else src
    extra = item[3] if len(item) > 3 and item[3] else ([] if kind == "glsl" else None)
    return src, entry, out, extra, kind


def source_path(rel, kind):
    """条目源文件绝对路径：GLSL/spv 链在 VK_SHADERS 下，handwritten 链为
    仓库相对路径（scripts/d3d12_handmade/...）。"""
    if kind == "handwritten":
        return os.path.join(REPO, rel)
    return os.path.join(VK_SHADERS, rel)

PROFILE_OF_STAGE = {"vert": "vs_6_0", "frag": "ps_6_0", "comp": "cs_6_0"}
STAGE_OF_EXT = {"vert": "vert", "frag": "frag", "comp": "comp"}


def run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stderr or "") + (p.stdout or "")
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"
    except FileNotFoundError as e:
        return 127, f"missing tool: {e}"


def tool_version(cmd):
    rc, out = run(cmd, timeout=30)
    return out.strip().splitlines()[0] if rc == 0 and out.strip() else f"unknown(rc={rc})"


def _normalize_text_bytes(data):
    """文本工件的 CRLF→LF 字节规范化（仅此一种，孤立 CR 不动）。

    Windows 在 core.autocrlf=true 检出下，文本文件（GLSL 源/.hlsl/.wgsl）
    落盘为 CRLF，与 Mac（LF）生成的 manifest 哈希不符——着色器内容没坏，
    是校验层没对齐。所有文本哈希路径统一先经此规范化再取 SHA；LF 检出上
    是无操作（Mac 行为与退出码不变）。二进制工件（.cso/.spv）git 不做
    换行转换、Windows 上已逐字节一致，绝不规范化。"""
    return data.replace(b"\r\n", b"\n")


def sha256_file(path, text=False):
    """文件 SHA256。text=True 按文本工件处理：整读后经 _normalize_text_bytes
    再哈希（着色器文本均是小文件，整读可避免 CRLF 对跨读取块边界）；
    默认逐块哈希给二进制（.cso/.spv），二进制绝不传 text=True。"""
    h = hashlib.sha256()
    if text:
        with open(path, "rb") as f:
            h.update(_normalize_text_bytes(f.read()))
        return h.hexdigest()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def inline_includes(src_path):
    """build_spv.sh awk inline_includes 的 Python 等价（spike §8）：
    相对源文件目录递归展平 #include "..."，剔除 GL_GOOGLE_include_directive。"""
    base = os.path.dirname(src_path)
    out_lines = []

    def process(path):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r'^#include\s*"([^"]+)"', line)
                if m:
                    process(os.path.join(base, m.group(1)))
                else:
                    out_lines.append(line)

    process(src_path)
    text = "".join(out_lines)
    return re.sub(r"^#extension\s+GL_GOOGLE_include_directive[^\n]*\n", "", text,
                  flags=re.MULTILINE)


def check_tools():
    missing = []
    for tool in (GLSLANG, SPIRV_CROSS, DXC):
        if not shutil.which(tool) and not os.path.isfile(tool):
            missing.append(tool)
    return missing


def compile_shader(item, out_dir, timeout=120):
    """编译单个条目 → .hlsl + .cso。GLSL 链经 glslang；spv 旁路链（R3b）
    以仓库 .spv 为输入直接 spirv-cross --entry。返回 (ok, record, errors)。"""
    rel, entry, out_rel, extra, kind = normalize_entry(item)
    src = source_path(rel, kind)
    if not os.path.isfile(src):
        return False, None, [f"source missing: {src}"]

    if kind == "spv":
        # 旁路链：spv 文件名无 stage 信息，条目第 4 位必须给 stage。
        if not extra or extra not in PROFILE_OF_STAGE:
            return False, None, [f"spv entry needs valid stage (vert/frag/"
                                 f"comp), got: {extra!r}"]
        stage = extra
        profile = PROFILE_OF_STAGE[stage]
    elif kind == "handwritten":
        # 手写直译链：extra = (stage, wgsl_src)。dxc 直编，不经 cross。
        if not (isinstance(extra, (tuple, list)) and len(extra) == 2
                and extra[0] in PROFILE_OF_STAGE):
            return False, None, [f"handwritten entry needs (stage, wgsl_src),"
                                 f" got: {extra!r}"]
        stage = extra[0]
        profile = PROFILE_OF_STAGE[stage]
    else:
        stage = STAGE_OF_EXT[rel.rsplit(".", 1)[-1]]
        profile = PROFILE_OF_STAGE[stage]

    hlsl_out = os.path.join(out_dir, out_rel + ".hlsl")
    cso_out = os.path.join(out_dir, out_rel + ".cso")
    os.makedirs(os.path.dirname(hlsl_out), exist_ok=True)

    errors = []
    with tempfile.TemporaryDirectory() as td:
        hlsl_tmp = os.path.join(td, "stage.hlsl")
        if kind == "handwritten":
            # R3c：手写 HLSL 直译，dxc 直编（源即 HLSL，无中间产物）。
            hlsl_tmp = src
        elif kind == "spv":
            # spirv-cross 以 SPIR-V 为输入，与源语言无关（naga WGSL 产物
            # 同样处理）。--entry 选定入口；cross 将其包装为 HLSL main()。
            rc_s, err_s = run([SPIRV_CROSS, src, "--entry", entry,
                               "--hlsl", "--shader-model", "60",
                               "--output", hlsl_tmp], timeout)
            if rc_s != 0:
                return False, None, [f"spirv-cross (spv passthrough) failed: "
                                     f"{err_s.strip()[:2000]}"]
        else:
            flat = inline_includes(src) if "#include" in open(
                    src, encoding="utf-8", errors="replace").read() \
                else open(src, encoding="utf-8", errors="replace").read()
            spv = os.path.join(td, "stage.spv")
            src_tmp = os.path.join(td, f"in.{stage}")
            # 文本写强制 LF（newline="\n"）：Windows 文本模式默认会把 LF
            # 译成 CRLF，防止未来在 Windows 重生成时 glslang 输入端引入
            # 行尾差异。flat 经 universal newlines 读入，本身已是 LF。
            with open(src_tmp, "w", encoding="utf-8", newline="\n") as f:
                f.write(flat)
            # 第一级与生产 build_spv.sh 同前端同 flags（spike §5 理由②：
            # GLSL 语义单一事实源）。R3 入口参数化：--source-entrypoint 取
            # 条目的源入口（生产 Lumen comp 是功能名），-e 恒 main 使
            # SPIR-V 入口与下游 spirv-cross/dxc 的 main 约定一致。
            rc_g, err_g = run([GLSLANG, "--quiet", "-V"] + list(extra) +
                              ["-S", stage, "--source-entrypoint", entry,
                               "-e", "main", src_tmp, "-o", spv], timeout)
            if rc_g != 0:
                return False, None, [f"glslang failed: {err_g.strip()[:2000]}"]

            rc_s, err_s = run([SPIRV_CROSS, spv, "--hlsl", "--shader-model",
                               "60", "--output", hlsl_tmp], timeout)
            if rc_s != 0:
                return False, None, [f"spirv-cross failed: {err_s.strip()[:2000]}"]

        rc_d, err_d = run([DXC, "-T", profile, "-E", "main", hlsl_tmp,
                           "-Fo", cso_out], timeout)
        if rc_d != 0:
            return False, None, [f"dxc failed: {err_d.strip()[:2000]}"]
        shutil.copyfile(hlsl_tmp, hlsl_out)

    # 文本源（glsl/handwritten）取规范化哈希，与 --check 同口径（未来在
    # Windows 重生成 manifest 时 source_sha256 不因 CRLF 检出漂移）；spv
    # 链源是二进制，保持逐字节哈希。
    record = {
        "source": rel,
        "source_sha256": sha256_file(src, text=kind != "spv"),
        "chain": {"spv": "spirv-cross->dxc (spv passthrough)",
                  "handwritten": "dxc (handwritten HLSL, WGSL-derived)"}.get(
                      kind, "glslang->spirv->spirv-cross->dxc"),
        "profile": profile,
        "entry": entry,
    }
    if kind in ("spv", "handwritten"):
        record["kind"] = kind
    if kind == "handwritten":
        # provenance：语义基准 WGSL 源（仓库相对路径）与其 SHA，供审计
        # 比对（手写件语义正确性的上游锚点）。
        wgsl = os.path.join(REPO, extra[1])
        record["wgsl_source"] = extra[1]
        # WGSL 源是文本，规范化哈希（provenance 锚点跨平台一致）。
        record["wgsl_source_sha256"] = (sha256_file(wgsl, text=True)
                                        if os.path.isfile(wgsl) else None)
    # 产物基名覆盖与宏变体（同源多产物）时记录 out/defines，保持 harness
    # 16 条记录字段形状与 origin/d3d12-windows-test-20261103 manifest 一致。
    if out_rel != rel:
        record["out"] = out_rel
    if kind == "glsl" and extra:
        record["defines"] = list(extra)
    record["hlsl"] = os.path.relpath(hlsl_out, out_dir)
    record["hlsl_sha256"] = sha256_file(hlsl_out, text=True)  # 文本，规范化
    record["cso"] = os.path.relpath(cso_out, out_dir)
    record["cso_sha256"] = sha256_file(cso_out)  # DXIL 二进制，绝不规范化
    return True, record, errors


def do_build(shader_list, out_dir):
    missing = check_tools()
    if missing:
        print("FATAL: missing tools: " + ", ".join(missing), file=sys.stderr)
        return 2
    os.makedirs(out_dir, exist_ok=True)
    records, failures = [], []
    for item in shader_list:
        rel, entry, out_rel, _extra, kind = normalize_entry(item)
        ok, rec, errs = compile_shader(item, out_dir)
        if ok:
            records.append(rec)
            print(f"[build:{kind}] {rel}[:{entry}] -> {rec['cso']} ({rec['profile']})")
        else:
            failures.append((rel, errs))
            for e in errs:
                print(f"FAIL {rel}: {e}", file=sys.stderr)

    manifest = {
        "manifest_version": 1,
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "generator": "scripts/build_d3d12_shaders.py",
        "chain": "glslang->SPIR-V->spirv-cross(HLSL sm60)->dxc",
        "tools": {
            "glslangValidator": tool_version([GLSLANG, "--version"]),
            "spirv-cross": tool_version([SPIRV_CROSS, "--version"]),
            "dxc": tool_version([DXC, "--version"]),
        },
        "shaders": records,
    }
    mpath = os.path.join(out_dir, MANIFEST_NAME)
    # 同上强制 LF：Windows 重生成时 manifest.json 字节与 Mac 生成一致。
    with open(mpath, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"manifest: {mpath} ({len(records)} ok, {len(failures)} failed)")
    return 0 if not failures else 1


def do_check(shader_list, out_dir):
    """freshness gate：源/产物 SHA 必须与 manifest 一致。条目键 = 产物基名
    （同源多产物如 SurfaceCacheCapture×3、EnvironmentVisibility×2 各占一键）。"""
    mpath = os.path.join(out_dir, MANIFEST_NAME)
    if not os.path.isfile(mpath):
        print(f"FRESHNESS FAIL: manifest missing: {mpath}\n"
              f"Run: python3 scripts/build_d3d12_shaders.py", file=sys.stderr)
        return 1
    with open(mpath, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    by_out = {}
    for r in manifest.get("shaders", []):
        by_out[r.get("out", r["source"])] = r

    stale = []
    expected = {normalize_entry(i)[2] for i in shader_list}
    listed = set(by_out)
    for extra in sorted(listed - expected):
        stale.append(f"manifest has shader no longer in list: {extra}")
    for item in shader_list:
        rel, entry, out_rel, _extra, kind = normalize_entry(item)
        rec = by_out.get(out_rel)
        src = source_path(rel, kind)
        if rec is None:
            stale.append(f"{out_rel}: not in manifest (never built)")
            continue
        if not os.path.isfile(src):
            stale.append(f"{out_rel}: source missing")
            continue
        # 文本源（glsl 链 .vert/.frag/.comp、handwritten 链 .hlsl）取
        # CRLF→LF 规范化哈希（Windows autocrlf=true 检出兼容）；spv 链源
        # 是二进制 .spv，git 不转换，保持逐字节哈希。
        src_sha = sha256_file(src, text=kind != "spv")
        if src_sha != rec["source_sha256"]:
            stale.append(f"{out_rel}: source changed since manifest "
                         f"(manifest {rec['source_sha256'][:12]}.., now {src_sha[:12]}..)")
            continue
        for key in ("hlsl", "cso"):
            p = os.path.join(out_dir, rec[key])
            if not os.path.isfile(p):
                stale.append(f"{out_rel}: product missing: {rec[key]}")
                continue
            # hlsl 是文本（CRLF→LF 规范化后比对）；cso 是二进制，git 不
            # 转换、Windows 上逐字节一致，绝不规范化。
            got = sha256_file(p, text=(key == "hlsl"))
            if got != rec[key + "_sha256"]:
                stale.append(f"{out_rel}: {key} sha mismatch (manifest "
                             f"{rec[key + '_sha256'][:12]}.., now {got[:12]}..)")

    if stale:
        print("D3D12 shader freshness check FAILED:", file=sys.stderr)
        for s in stale:
            print(f"  STALE: {s}", file=sys.stderr)
        return 1
    # 摘要注明文本哈希口径（ASCII 写法，避免 Windows 重定向编码问题）。
    print(f"D3D12 shader freshness check passed ({len(shader_list)} shaders; "
          f"text hashes are CRLF->LF normalized).")
    return 0


def parse_cli_shader(arg):
    """CLI 位置参数：'path'（entry main）或 'path:entry'。"""
    if ":" in arg:
        src, entry = arg.split(":", 1)
        return (src, entry)
    return arg


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="freshness gate：重算 SHA 与 manifest 比对，失配 exit 1")
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("shaders", nargs="*",
                    help="覆盖编译清单（VK_SHADERS 相对路径[.vert/.frag/.comp]，"
                         "可带 :entry 指定源入口）；默认用内置全量清单")
    args = ap.parse_args()

    shader_list = [parse_cli_shader(a) for a in args.shaders] if args.shaders \
        else SHADER_LIST
    if args.check:
        return do_check(shader_list, args.out_dir)
    return do_build(shader_list, args.out_dir)


if __name__ == "__main__":
    sys.exit(main())
