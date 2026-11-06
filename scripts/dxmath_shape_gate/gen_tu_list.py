#!/usr/bin/env python3
# ============================================================================
# DirectXMath 形状门禁 —— Windows 会编译的 C++ 编译单元清单生成器。
#
# 规则来源（人工核对 CMakeLists 后复刻）：
#   1. Engine/CMakeLists.txt 在 WIN32 + ENABLE_VULKAN + ENABLE_D3D12 下：
#      - PLATFORM_UTILS(.cpp) + WIN32 PLATFORM_SOURCES(.cpp)
#      - GRAPHICS_UTILS（大清单 glob，全部目录）
#      - GRAPHICS_SOURCES：legacy Direct3D12/Vulkan 排除（该配置下为空），
#        改为 RHI/Platforms/{Vulkan,D3D12}/* + third_party/D3D12MemAlloc
#      - COMMON_SOURCES（Common/Components/Scripting/Audio/Core/EngineAPI/
#        Utilities/Content/Geometry(+Algorithms)/JobSystem/Simulation/
#        Particles/ParticleFX/Navigation/Gameplay），排除 *_test.cpp
#      - Compiler/MSVC/*（实测目录不存在——Engine/Compiler 下只有 Mac，
#        glob 结果为空，与 Windows MSVC 构建一致）
#      - 排除 Metal 目录与 *.mm（Windows 不编）
#   2. EngineDLL/CMakeLists.txt 固定 SOURCE_FILES（.cpp，去 .mm 与已并入
#      Engine 清单的重复项）
#   3. ContentTools/CMakeLists.txt PROJECTION_SOURCES + common/* + PfxJson.cpp
#   4. EngineTest/UnitTests/CMakeLists.txt：按 if/elseif/else/endif 门控
#      追踪（APPLE/NOT WIN32/NOT MSVC/EMSCRIPTEN → 排除；MSVC/WIN32/
#      ENABLE_VULKAN/ENABLE_D3D12 → 包含；不可静态判定 → 包含并标注）
#   5. EngineTest/IntegrationTests：TestSponzaVT（无条件）+ 
#      TestRHISponzaShared（ENABLE_VULKAN，已移出 APPLE 门）；
#      WIN32 分支的 legacy TestRenderer 已于 R13-C 移除（无 exe）
#
# 依赖 D3D12/DirectXTex/FBX SDK 等垫片满足不了的 TU 不在此排除——
# 一律进清单，由 run_gate.sh 实跑后按"平台/垫片噪声"分类；结构性排除
# （缺第三方头）由 EXCLUDES 表显式给出并写明原因。
# 用法：python3 gen_tu_list.py > tu_list.txt
# ============================================================================
import os
import re
import sys
import glob

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ENGINE = os.path.join(ROOT, "Engine")


def rel(p):
    return os.path.relpath(p, ROOT)


def find(patterns, base=ENGINE):
    out = []
    for pat in patterns:
        p = os.path.join(base, pat)
        if os.path.isdir(p):
            # 目录 → 递归收集 *.cpp（对应 CMake GLOB_RECURSE 目录规则）
            out.extend(glob.glob(os.path.join(p, "**", "*.cpp"), recursive=True))
        else:
            out.extend(glob.glob(p))
    return out


tus = {}  # relpath -> 组标签


def add(paths, group):
    for p in paths:
        if not p.endswith(".cpp"):
            continue
        r = rel(p)
        if r.endswith("_test.cpp"):
            continue
        tus.setdefault(r, group)


# ---------------------------------------------------------------- Engine ---
add(find(["Platform/Window.cpp", "Input/Input.cpp"]), "Engine/platform")
add(find(["Platform/PlatformWin32.cpp", "Input/InputWin32.cpp"]), "Engine/platform-win32")

graphics_utils_dirs = [
    "Graphics/GraphicsPlatform.cpp", "Graphics/Renderer.cpp", "Graphics/RenderMesh.cpp",
    "Graphics/RenderTexture.cpp", "Graphics/RenderScene.cpp", "Graphics/RenderView.cpp",
    "Graphics/Camera/RenderViewBuilder.cpp", "Graphics/RenderProxy.cpp", "Graphics/Material.cpp",
    "Graphics/MaterialInstance.cpp", "Graphics/ParticleMaterial.cpp", "Graphics/SceneDataAdapter.cpp",
    "Graphics/ForwardRenderer.cpp",
    "Graphics/RHI/Core", "Graphics/VirtualTexture", "Graphics/RHI/Systems",
    "Graphics/RHI/Utils", "Graphics/RHI/Components", "Graphics/Passes", "Graphics/Scene",
    "Graphics/DebugDraw", "Graphics/Nanite", "Graphics/Nanite/Providers", "Graphics/Lumen",
    "Graphics/Volume", "Graphics/Field", "Graphics/Fluid", "Graphics/PCG", "Graphics/PCG/Nodes",
    "Graphics/PCG/Noise", "Graphics/PCG/GPU", "Graphics/WFC", "Graphics/WFC/Nodes",
    "Graphics/WFC/Styles", "Graphics/MaterialGraph", "Graphics/MaterialGraph/Nodes",
    "Graphics/ShaderIR", "Graphics/MaterialPreview", "Graphics/RenderGraph",
    "Graphics/RenderPipeline", "Graphics/RenderPipeline/Modules",
    "Graphics/RenderPipeline/RenderPasses/Debug",
    "Graphics/RenderPipeline/RenderPasses/PostProcess",
    "Graphics/RenderPipeline/RenderPasses/Visibility",
    "Graphics/Lighting", "Graphics/Neural", "Graphics/Baking", "Graphics/Utils",
    "Graphics/Utilities",
]
add(find(graphics_utils_dirs), "Engine/graphics")

add(find(["Graphics/RHI/Platforms/Vulkan"]), "Engine/rhi-vulkan")
add(find(["Graphics/RHI/Platforms/D3D12"]), "Engine/rhi-d3d12")
add([os.path.join(ROOT, "third_party/D3D12MemAlloc/src/D3D12MemAlloc.cpp")], "Engine/rhi-d3d12")

common_dirs = [
    "Common", "Components", "Scripting", "Audio", "Core", "EngineAPI", "Utilities",
    "Content", "Geometry", "Geometry/Algorithms", "JobSystem", "Simulation",
    "Particles", "ParticleFX", "Navigation", "Gameplay",
]
add(find(common_dirs), "Engine/common")

# ---------------------------------------------------------------- EngineDLL
enginedll = [
    "DllMain.cpp", "EngineAPI.cpp", "EngineAPI_win32.cpp", "DiagnosticsAPI.cpp",
    "EngineEventsAPI.cpp", "CrashHandler.cpp", "EntityAPI.cpp", "AssetAPI.cpp",
    "RenderPipelineAPI.cpp", "LightmapAPI.cpp", "PCGAPI.cpp", "GeometryAPI.cpp",
    "MaterialAPI.cpp", "MaterialPreviewAPI.cpp", "MaterialGraphAPI.cpp",
    "MaterialGraphReflectionAPI.cpp", "ParticleMaterialAPI.cpp",
    "ParticleEmitterAPI.cpp", "ScriptAPI.cpp", "AudioAPI.cpp", "ParticleFXAPI.cpp",
    "ShaderCompilerAPI.cpp", "FrameAPI.cpp", "ResourceAPI.cpp", "DebugDrawAPI.cpp",
    "CaptureAPI.cpp", "RenderTargetAPI.cpp", "LightAPI.cpp", "CameraAPI.cpp",
    "PhysicsAPI.cpp", "NavigationAPI.cpp", "GameplayAPI.cpp", "PrintAPI.cpp",
    "ProceduralMeshAPI.cpp", "HeadlessRender.cpp",
    # EditorShaderManager.cpp 同时在 Engine GRAPHICS_UTILS 与 EngineDLL 清单中，
    # 去重（同一文件同一语法面）
]
add([os.path.join(ROOT, "EngineDLL", f) for f in enginedll], "EngineDLL")

# ------------------------------------------------------------- ContentTools
contenttools_files = [
    "ContentTools.cpp", "Geometry.cpp", "GltfImporter.cpp",
    "NormalMapIdentification.cpp", "ObjImporter.cpp", "PrimitiveMesh.cpp",
    "common/SDF.cpp", "common/CardBake.cpp", "common/CardSurfaceBake.cpp",
    "common/ProcessableMesh.cpp", "common/MeshConverters.cpp",
]
ct = [os.path.join(ROOT, "ContentTools", f) for f in contenttools_files]
ct.append(os.path.join(ROOT, "Engine/ParticleFX/core/PfxJson.cpp"))
add(ct, "ContentTools")

# --------------------------------------------------- IntegrationTests（固定）
add([os.path.join(ROOT, "EngineTest/IntegrationTests/TestSponzaVT.cpp"),
     os.path.join(ROOT, "ContentTools/common/VTBaker.cpp")], "EngineTest/integration")
add([os.path.join(ROOT, "EngineTest/IntegrationTests/TestRHISponzaShared.cpp")],
    "EngineTest/integration")

# ------------------------------------------------- UnitTests（门控解析）----
def eval_cond(cond, stack_all_active):
    """返回 (visible, decidable, note)。仅静态判 APPLE/WIN32/MSVC/EMSCRIPTEN 族。"""
    c = cond.strip()
    if re.search(r'\bAPPLE\b', c) and not re.search(r'WIN32|MSVC', c):
        return (False, True, "APPLE gate")
    if re.search(r'NOT\s+WIN32', c):
        return (False, True, "NOT WIN32")
    if re.search(r'NOT\s+MSVC', c):
        return (False, True, "NOT MSVC")
    if re.search(r'\bEMSCRIPTEN\b', c):
        return (False, True, "EMSCRIPTEN")
    return (True, False, "")  # 默认可见（ENABLE_*/TARGET/EXISTS/MSVC/WIN32…）


def parse_unittests(path):
    """粗粒度 CMake 门控追踪：if/elseif/else/endif 堆栈 + add_executable 收集。"""
    stack = []  # 每帧: dict(active=当前分支是否生效, base=if 条件可见性, undecidable=set)
    exes = []   # (name, sources, note)
    pending = None  # dict(name, sources, note)
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            def active():
                return all(fr["active"] for fr in stack)
            m = re.match(r'^if\s*\((.*)\)\s*$', line)
            if m and pending is None:
                vis, dec, note = eval_cond(m.group(1), None)
                stack.append({"active": vis, "base": vis, "note": note,
                              "any_taken": vis, "undec": not dec})
                continue
            m = re.match(r'^elseif\s*\((.*)\)\s*$', line)
            if m and stack:
                fr = stack[-1]
                vis, dec, note = eval_cond(m.group(1), None)
                # 简化：若 if 分支已可见，后续分支不激活；否则按本条件
                fr["active"] = vis and not fr["any_taken"]
                fr["any_taken"] = fr["any_taken"] or vis
                if not dec:
                    fr["undec"] = True
                continue
            if line == "else()" and stack:
                fr = stack[-1]
                fr["active"] = not fr["any_taken"]
                fr["any_taken"] = True
                continue
            if line.startswith("endif") and stack:
                stack.pop()
                continue
            m = re.match(r'^add_executable\s*\(\s*(\w+)\s*(.*?)\s*$', line)
            if m and active():
                name, rest = m.group(1), m.group(2)
                pending = {"name": name, "sources": [], "undec": any(fr["undec"] for fr in stack)}
                rest = rest.strip()
                if rest.endswith(")"):
                    # 单行块：闭括号已在行内
                    for tok in rest[:-1].split():
                        if tok.startswith("#") or tok.startswith("$"):
                            break
                        pending["sources"].append(tok)
                    exes.append(pending)
                    pending = None
                elif rest:
                    for tok in rest.split():
                        if tok.startswith("#") or tok.startswith("$"):
                            break
                        pending["sources"].append(tok)
                continue
            if pending is not None:
                # 多行源列表：取行内标识（保留 ${CMAKE_SOURCE_DIR}/ 前缀，跳过
                # 其他 ${VAR} 与注释）。闭括号既可能独占一行，也可能跟在最后
                # 一个源文件后面（如 `${CMAKE_SOURCE_DIR}/x.cpp)`）——旧版只认
                # 独占一行的 ")"，后者会让 pending 吞掉后续所有 add_executable。
                body = line.split("#", 1)[0].strip()
                closes = body.endswith(")")
                if closes:
                    body = body[:-1]
                for tok in body.split():
                    if tok.startswith("${CMAKE_SOURCE_DIR}/"):
                        pending["sources"].append(tok)
                    elif tok.startswith("$"):
                        continue
                    else:
                        pending["sources"].append(tok)
                if closes:
                    exes.append(pending)
                    pending = None
    return exes


UT = os.path.join(ROOT, "EngineTest/UnitTests")
exes = parse_unittests(os.path.join(UT, "CMakeLists.txt"))
ut_added, ut_skipped = 0, 0
for e in exes:
    srcs = []
    for s in e["sources"]:
        s = s.strip('"')
        if not s or not s.endswith(".cpp"):
            continue
        if "${CMAKE_SOURCE_DIR}/" in s:
            p = os.path.join(ROOT, s.split("${CMAKE_SOURCE_DIR}/", 1)[1])
        else:
            p = os.path.join(UT, s)
        if not os.path.exists(p):
            continue
        srcs.append(p)
    if not srcs:
        ut_skipped += 1
        continue
    note = " undecidable-gate" if e["undec"] else ""
    add(srcs, "EngineTest/unittests" + note)
    ut_added += 1
print(f"# UnitTests executables parsed: {len(exes)} (added {ut_added}, skipped-no-src {ut_skipped})",
      file=sys.stderr)

# ---------------------------------------------------------------- 排除表 ---
# 结构性排除：垫片无法满足的第三方依赖（文件存在但必失败，写明原因）
EXCLUDES = {
    "ContentTools/FbxImporter.cpp": "FBX SDK（fbxsdk.h）：第三方闭源头，Windows 真机验证",
    "ContentTools/TextureImporter.cpp": "DirectXTex.h + d3d11.h + WIC：DirectXTex 未 vendored",
    "ContentTools/NormalMapIdentification.cpp": "DirectXTex.h（DirectX::Image）：DirectXTex 未 vendored",
    "EngineDLL/CrashHandler.cpp": "dbghelp.h/Win32 SEH 深度面：交 Windows 真机",
}
for ex in EXCLUDES:
    tus.pop(ex, None)

# ---------------------------------------------------------------- 输出 -----
missing = [t for t in tus if not os.path.exists(os.path.join(ROOT, t))]
for t in missing:
    print(f"# MISSING: {t}", file=sys.stderr)
    tus.pop(t)
for t in sorted(tus):
    print(f"{t}\t{tus[t]}")
print(f"# total TUs: {len(tus)}  (excluded: {len(EXCLUDES)})", file=sys.stderr)
