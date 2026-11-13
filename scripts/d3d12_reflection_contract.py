#!/usr/bin/env python3
"""d3d12_reflection_contract.py — D3D12 M1 F1-7 绑定反射契约核对.

对主链（glslang→SPIR-V→spirv-cross→HLSL→dxc，见 scripts/build_d3d12_shaders.py）
的每个 shader 自动核对三级绑定事实，全部一致才 exit 0；任一失配 exit 1：

  1. spirv-cross --reflect（SPIR-V 权威）：set/binding/类型/块尺寸 与
     本文件内嵌的 M1 冻结契约（EXPECTED 常量）逐项相等；
  2. spirv-cross HLSL 源内 `: register(xN, spaceM)` 注解：寄存器号 == binding、
     space == set（M1 决策：GLSL set/binding 直映 HLSL space/register；
     space0 可省略不写，视为 0）；
  3. dxc -dumpbin 的 DXIL Resource Bindings 表（终产物权威）：每绑定出现、
     寄存器/space 与 1/2 一致；push constant（spirv-cross HLSL 不注解 register）
     落位 = "该 stage 显式 b 寄存器数"（下一个空闲 bN，spike §10 固化规则）。

通用断言（跨 shader）：入口恒 main（reflect entryPoints + HLSL 函数签名 +
dxc -E main 编译成功三方）；同 shader 内无两个不同 RHI 绑定映射到同一
(space, 寄存器字母, 寄存器号)。

用法: python3 scripts/d3d12_reflection_contract.py [--keep-temp]
工具路径环境变量同 build_d3d12_shaders.py（D3D12_GLSLANG/D3D12_SPIRV_CROSS/D3D12_DXC）。
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
VK_SHADERS = os.path.join(REPO, "Engine/Graphics/Vulkan/shaders")

GLSLANG = os.environ.get("D3D12_GLSLANG", "/usr/local/bin/glslangValidator")
SPIRV_CROSS = os.environ.get("D3D12_SPIRV_CROSS", "/usr/local/bin/spirv-cross")
DXC = os.environ.get("D3D12_DXC", "/usr/local/bin/dxc")

PROFILE_OF_STAGE = {"vert": "vs_6_0", "frag": "ps_6_0", "comp": "cs_6_0"}

# ---------------------------------------------------------------------------
# M1/M2 冻结反射契约（F1-7/F2-5；与 TestRHISponzaShared 宿主
# CreateDescriptorLayouts 一一对应；改绑定必须同时改这里 + 宿主 + 阶段文档，
# 三者一源）。
# 记录形式: (set, binding, kind, name, extra)
#   kind: ubo | separate_image | separate_sampler | comparison_sampler
#       | push_constant
#   ubo extra: block_size；push_constant extra: block_size
#   comparison_sampler：spirv-cross --reflect 层归 separate_samplers（反射
#   JSON 不区分比较性），HLSL 层必须注解为 SamplerComparisonState（M2 F2-5
#   比较采样器契约——错用 SamplerState 即 fail）。
# ---------------------------------------------------------------------------
EXPECTED = {
    "Forward/M1Forward.vert": [
        (0, 0, "ubo", "ViewData", 80),
        (None, None, "push_constant", "M1PushConsts", 80),
    ],
    "Forward/M1Forward.frag": [
        (0, 0, "ubo", "ViewData", 80),
        (0, 1, "ubo", "LightData", 32),
        (1, 0, "separate_image", "albedoTexture", None),
        (1, 1, "separate_sampler", "albedoSampler", None),
    ],
    # D3D12 M2 F2-5: shadow depth VS（set0 b0 light VP UBO + 64B model push）。
    "Forward/M2ShadowDepth.vert": [
        (0, 0, "ubo", "ShadowDepthData", 64),
        (None, None, "push_constant", "M2ShadowPush", 64),
    ],
    # D3D12 M2 F2-5: lit FS（M1 面不动 + set0 b2 ShadowData + set1 b2/b3
    # shadow map 采样对，b3 为比较采样器）。
    "Forward/M2ForwardLit.frag": [
        (0, 0, "ubo", "ViewData", 80),
        (0, 1, "ubo", "LightData", 32),
        (0, 2, "ubo", "ShadowData", 80),
        (1, 0, "separate_image", "albedoTexture", None),
        (1, 1, "separate_sampler", "albedoSampler", None),
        (1, 2, "separate_image", "shadowMap", None),
        (1, 3, "comparison_sampler", "shadowSampler", None),
    ],
    # D3D12 M3 F3-2: post 全屏三角形 VS——vertex-ID 驱动，零绑定零 push
    # （空契约表；DXIL 侧无 Resource Bindings 属预期，见 check_shader 的
    # 空绑定旁路）。
    "Forward/M3Post.vert": [],
    # D3D12 M3 F3-2: post FS——set0 b0 PostParams UBO(32B) + b1/b2 HDR
    # 分离采样对（宿主 post set0 一一对应）。
    "Forward/M3Post.frag": [
        (0, 0, "ubo", "PostParams", 32),
        (0, 1, "separate_image", "hdrTexture", None),
        (0, 2, "separate_sampler", "hdrSampler", None),
    ],
    # D3D12 M4 F4-2: deferred GBuffer vert——M1Forward.vert 变体（同 ViewData
    # UBO + 80B push；DXIL push 落位 b1 固化规则同 M1）。
    "Forward/M4GBuffer.vert": [
        (0, 0, "ubo", "ViewData", 80),
        (None, None, "push_constant", "M1PushConsts", 80),
    ],
    # D3D12 M4 F4-2: deferred GBuffer frag——MRT×3 写出。绑定复用 M1 material
    # set1（albedo 采样对）；MRT 输出目标核对见 EXPECTED_TARGETS（SV_TargetN）。
    "Forward/M4GBuffer.frag": [
        (1, 0, "separate_image", "albedoTexture", None),
        (1, 1, "separate_sampler", "albedoSampler", None),
    ],
    # D3D12 M4 F4-3: deferred light FS——set0 8 绑定（b0/b1 UBO + b2-b6 纹理
    # + b7 比较采样器；宿主 deferred set0 一一对应）。
    "Forward/M4DeferredLight.frag": [
        (0, 0, "ubo", "DeferredViewData", 80),
        (0, 1, "ubo", "LightShadowData", 112),
        (0, 2, "separate_image", "depthTexture", None),
        (0, 3, "separate_image", "gbufferAlbedo", None),
        (0, 4, "separate_image", "gbufferNormal", None),
        (0, 5, "separate_image", "gbufferMaterial", None),
        (0, 6, "separate_image", "shadowMap", None),
        (0, 7, "comparison_sampler", "shadowSampler", None),
        # D3D12 M6 F6-2: GI ambient consumption — probe-grid tri-linear pair
        # (Texture3D t8 + SamplerState s9; 3D kind checked at HLSL decl layer)
        # + GIParams UBO (32B: gridMinAndScale + gridInvExtent).
        (0, 8, "separate_image_3d", "giProbeTexture", None),
        (0, 9, "separate_sampler", "giProbeSampler", None),
        (0, 10, "ubo", "GIParams", 32),
    ],
    # D3D12 M5 F5-1: GPU-driven cull compute——set0 5 绑定（反射层名=块名，
    # HLSL/DXIL 层名=实例名 → 第 6 元素 hlsl_name 显式映射；M5 新 kind：
    # ssbo_read=t 寄存器 / ssbo_write=u 寄存器）。
    "Forward/M5Cull.comp": [
        (0, 0, "ssbo_read", "InstBuf", None, "instBuf"),
        (0, 1, "ubo", "CullParams", 128),
        (0, 2, "ssbo_write", "ArgsBuf", None, "argsBuf"),
        (0, 3, "ssbo_write", "CounterBuf", None, "counter"),
        (0, 4, "ubo", "CullFlags", 16),
    ],
    # D3D12 M5 F5-5: compute mipmap 工具 shader——set0 4 绑定（分离
    # texture+sampler 形态=M1 惯例：utexture2D srcTex=t0 + sampler
    # srcSamp=s1；rgba8ui 存储图像 dstMip=u2；MipParams UBO b3）。
    "Forward/M5Mipmap.comp": [
        (0, 0, "separate_image", "srcTex", None),
        (0, 1, "separate_sampler", "srcSamp", None),
        (0, 2, "storage_image", "dstMip", None),
        (0, 3, "ubo", "MipParams", 16),
    ],
    # D3D12 M6 F6-2: probe bake compute — REMOVED (M6 GI rebuild replaces the
    # analytic bake by M6ProbeInject; see below).
    # D3D12 M6 F6-2: probe accum compute — readonly image3D (typed UAV load,
    # HLSL RWTexture3D u0) + texture3D history pair (t1 + s2; texelFetch→Load
    # strips the sampler from DXIL — DXIL_OPTIONAL below) + writeonly image3D
    # out (u3) + flags UBO (16B).
    "Forward/M6ProbeAccum.comp": [
        (0, 0, "storage_image_3d", "newGrid", None),
        (0, 1, "separate_image_3d", "histTexture", None),
        (0, 2, "separate_sampler", "histSampler", None),
        (0, 3, "storage_image_3d", "outGrid", None),
        (0, 4, "ubo", "AccumParams", 16),
    ],
    # D3D12 M6 GI rebuild: direct-only deferred light FS — b0-b7 IDENTICAL to
    # M4DeferredLight.frag (names/types/sizes) so the host reuses the M4 light
    # set layout (11-binding superset; this shader declares/consumes b0-b7
    # only). MRT×2 outputs see EXPECTED_TARGETS.
    "Forward/M6DeferredDirect.frag": [
        (0, 0, "ubo", "DeferredViewData", 80),
        (0, 1, "ubo", "LightShadowData", 112),
        (0, 2, "separate_image", "depthTexture", None),
        (0, 3, "separate_image", "gbufferAlbedo", None),
        (0, 4, "separate_image", "gbufferNormal", None),
        (0, 5, "separate_image", "gbufferMaterial", None),
        (0, 6, "separate_image", "shadowMap", None),
        (0, 7, "comparison_sampler", "shadowSampler", None),
    ],
    # D3D12 M6 GI rebuild: SH-L1 probe inject compute (replaces analytic
    # M6ProbeBake) — directDiffuse/depth/normal/material texelFetch pair +
    # InjectParams UBO (std140 144B) + writeonly rgba16f image3D (typed UAV
    # store, (8,8,24) SH grid). No gl_NumWorkGroups (dims via UBO).
    "Forward/M6ProbeInject.comp": [
        (0, 0, "separate_image", "directDiffuse", None),
        (0, 1, "separate_image", "depthTexture", None),
        (0, 2, "separate_image", "gbufferNormal", None),
        (0, 3, "separate_image", "gbufferMaterial", None),
        (0, 4, "ubo", "InjectParams", 144),
        (0, 5, "storage_image_3d", "outSH", None),
    ],
    # D3D12 M6 GI rebuild: independent indirect resolve FS — GBuffer fetch
    # pair + DeferredViewData UBO (80B, same block as M4) + SH probe Texture3D
    # + sampler + ResolveParams UBO (std140 48B: gridMin.w=normalBias +
    # gridInvExtent + gridDims).
    "Forward/M6GIResolve.frag": [
        (0, 0, "ubo", "DeferredViewData", 80),
        (0, 1, "separate_image", "depthTexture", None),
        (0, 2, "separate_image", "gbufferAlbedo", None),
        (0, 3, "separate_image", "gbufferNormal", None),
        (0, 4, "separate_image", "gbufferMaterial", None),
        (0, 5, "separate_image_3d", "giProbeSH", None),
        (0, 6, "separate_sampler", "giProbeSampler", None),
        (0, 7, "ubo", "ResolveParams", 48),
    ],
    # D3D12 M6 GI rebuild: single direct+indirect composite FS — texelFetch
    # pair only, no UBO/sampler.
    "Forward/M6Composite.frag": [
        (0, 0, "separate_image", "giDirect", None),
        (0, 1, "separate_image", "giIndirect", None),
    ],
}

# M5 F5-5：DXIL 层允许缺席的绑定（shader=rel, HLSL 名）——DXC 淘汰未消费
# 的采样器（texelFetch→Load 路径）；反射/HLSL 两层仍强制核对。
DXIL_OPTIONAL = {
    ("Forward/M5Mipmap.comp", "srcSamp"),
    ("Forward/M6ProbeAccum.comp", "histSampler"),
}

# ---------------------------------------------------------------------------
# M4 F4-2 MRT 输出目标契约（ spirv-cross --reflect 不列出 fragment outputs，
# 退而在 HLSL 源层核对 SV_TargetN 注解——至少 HLSL SV_Target 注解核对，
# 见 M4 任务书 T2）。记录形式: shader → [(index, 输出变量名), ...]。
# 核对规则：HLSL `out <type> <name> : SV_Target<N>` 集合与契约逐项相等
#（多余/缺失 SV_Target 均失败）；未列出的 shader 不做 MRT 核对（M1-M3
# 单 RT 输出隐式 SV_Target0，保持既有契约面不变）。
# ---------------------------------------------------------------------------
EXPECTED_TARGETS = {
    "Forward/M4GBuffer.frag": [
        (0, "outAlbedo"),    # RGBA8_UNorm  albedo×baseColor
        (1, "outNormal"),    # RG16_Float   世界法线 xy 直存
        (2, "outMaterial"),  # RGBA8_UNorm  占位/符号打包
    ],
    "Forward/M4DeferredLight.frag": [
        (0, "outColor"),     # 单 RT（litRTFormat：RGBA16F 或 BGRA8）
    ],
    # D3D12 M6 GI rebuild MRT 输出：direct-only 直射双 RT（RT1 = GI 输入，
    # alpha 携带诊断通道）与 resolve/composite 单 RT。
    "Forward/M6DeferredDirect.frag": [
        (0, "outDirect"),        # RGBA16F rgb=direct, a=ndl（诊断）
        (1, "outDirectDiffuse"), # RGBA16F rgb=direct-diffuse (GI 输入), a=shadow
    ],
    "Forward/M6GIResolve.frag": [
        (0, "outIndirect"),      # RGBA16F rgb=albedo·E_h(N), a=1
    ],
    "Forward/M6Composite.frag": [
        (0, "outColor"),         # direct+indirect 恰一次合成（post 前最终）
    ],
}

REFLECT_KINDS = {
    "ubo": "ubos",
    "separate_image": "separate_images",
    "separate_sampler": "separate_samplers",
    "comparison_sampler": "separate_samplers",  # 反射层不可区分，见上注
    # M5 F5-1/F5-5（compute 链新增资源面）：
    #   ssbo_read  —— spirv-cross 反射 ssbos（readonly），HLSL t 寄存器
    #   ssbo_write —— 反射 ssbos（writeonly/coherent），HLSL u 寄存器
    #   storage_image —— 反射 images（UAV 图像），HLSL u 寄存器
    #   combined_texture —— 反射 textures（GLSL 组合采样器），HLSL t 寄存器
    #     + spirv-cross 自动配对 SamplerState <name>_sampler（同号 s 寄存器，
    #     契约层不单列——占用唯一性由 occupied 集合保证）
    "ssbo_read": "ssbos",
    "ssbo_write": "ssbos",
    "storage_image": "images",
    "combined_texture": "textures",
    # M6 F6-3：3D 维度 kind（反射层不可区分维度——维度核对在 HLSL 声明层
    # 以 decl_type 强制：Texture3D / RWTexture3D；寄存器字母与 2D 同）。
    "separate_image_3d": "separate_images",
    "storage_image_3d": "images",
}
REGISTER_LETTER = {
    "ubo": "b",
    "separate_image": "t",
    "separate_sampler": "s",
    "comparison_sampler": "s",
    "push_constant": "b",
    "ssbo_read": "t",
    "ssbo_write": "u",
    "storage_image": "u",
    "combined_texture": "t",
    "separate_image_3d": "t",
    "storage_image_3d": "u",
}


def run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stderr or "") + (p.stdout or "")
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"
    except FileNotFoundError as e:
        return 127, f"missing tool: {e}"


class ContractFailure(Exception):
    pass


def check_shader(rel, td):
    stage = rel.rsplit(".", 1)[-1]
    src = os.path.join(VK_SHADERS, rel)
    if not os.path.isfile(src):
        raise ContractFailure(f"source missing: {src}")
    expected = EXPECTED[rel]
    # M5：6 元组（…, hlsl_name）——SSBO 反射层名=块名 ≠ HLSL/DXIL 层名=
    # 实例名；其余 kind 默认 hlsl_name == name。
    expected = [e if len(e) == 6 else tuple(e) + (e[3],) for e in expected]

    # ---- 链前端：glslang → SPIR-V（与 build_d3d12_shaders.py 同 flags）----
    spv = os.path.join(td, rel.replace("/", "_") + ".spv")
    os.makedirs(os.path.dirname(spv), exist_ok=True)
    rc, err = run([GLSLANG, "--quiet", "-V", "-S", stage,
                   "--source-entrypoint", "main", "-e", "main", src, "-o", spv])
    if rc != 0:
        raise ContractFailure(f"glslang: {err.strip()[:500]}")

    # ---- 1) spirv-cross --reflect ----
    rc, out = run([SPIRV_CROSS, spv, "--reflect"])
    if rc != 0:
        raise ContractFailure(f"spirv-cross --reflect: {out.strip()[:500]}")
    refl = json.loads(out)

    # 入口恒 main + stage 正确（compute 反射额外携带 workgroup_size 键——
    # M5 F5-1 起含 compute 契约；名称/模式严格，附加键忽略）
    eps = refl.get("entryPoints", [])
    ok_eps = (len(eps) == 1 and eps[0].get("name") == "main"
              and eps[0].get("mode") == stage)
    if not ok_eps:
        raise ContractFailure(f"entryPoints != main/{stage}: {eps}")

    problems = []
    seen = set()
    types = refl.get("types", {})

    def block_name_of(hit):
        t = types.get(hit.get("type"), {})
        return t.get("name") or ""

    for set_, binding, kind, name, extra, _hlsl in expected:
        if kind == "push_constant":
            # spirv-cross --reflect: push_constant 条目 name=实例名（pc），
            # block 名在 types[type].name；尺寸由 HLSL packoffset 侧核对。
            pcs = refl.get("push_constants", [])
            hit = next((p for p in pcs
                        if p.get("name") == name or block_name_of(p) == name), None)
            if hit is None:
                problems.append(f"reflect: push_constant {name} missing ({pcs})")
                continue
            seen.add(("push_constant", name))
            continue
        bucket = refl.get(REFLECT_KINDS[kind], [])
        hit = next((b for b in bucket
                    if b.get("set") == set_ and b.get("binding") == binding), None)
        if hit is None:
            problems.append(f"reflect: {kind} set{set_} b{binding} missing "
                            f"(have {bucket})")
            continue
        if hit.get("name") != name:
            problems.append(f"reflect: set{set_} b{binding} name "
                            f"{hit.get('name')} != {name}")
        if kind == "ubo" and hit.get("block_size") != extra:
            problems.append(f"reflect: UBO {name} block_size "
                            f"{hit.get('block_size')} != {extra}")
        # M5 SSBO 方向性核对（readonly→t / 写侧→u；反射层可区分）
        if kind == "ssbo_read" and not hit.get("readonly"):
            problems.append(f"reflect: ssbo {name} not readonly "
                            f"(got {hit})")
        if kind == "ssbo_write" and hit.get("readonly"):
            problems.append(f"reflect: ssbo {name} unexpected readonly "
                            f"(got {hit})")
        seen.add((kind, set_, binding))
    # 反向：reflect 里不得有契约外的绑定（一个 bucket 可对应多个 kind——
    # separate_samplers 同时覆盖普通/比较采样器，反射层不可区分）。
    bucket_kinds = {}
    for k, v in REFLECT_KINDS.items():
        bucket_kinds.setdefault(v, set()).add(k)
    for bucket_key in REFLECT_KINDS.values():
        for b in refl.get(bucket_key, []):
            key = (b.get("set"), b.get("binding"))
            if not any((k,) + key in seen for k in bucket_kinds[bucket_key]):
                problems.append(f"reflect: unexpected extra binding in "
                                f"{bucket_key}: {b}")
    if problems:
        raise ContractFailure("; ".join(problems))

    # ---- 2) spirv-cross HLSL register 注解 ----
    hlsl_path = os.path.join(td, rel.replace("/", "_") + ".hlsl")
    rc, out = run([SPIRV_CROSS, spv, "--hlsl", "--shader-model", "60",
                   "--output", hlsl_path])
    if rc != 0:
        raise ContractFailure(f"spirv-cross --hlsl: {out.strip()[:500]}")
    hlsl = open(hlsl_path, encoding="utf-8").read()

    # 入口 main（HLSL 函数定义：单入口=void main；多入口拆分形=
    # SPIRV_Cross_Output main(SPIRV_Cross_Input) wrapper——两种都合法）
    if not re.search(r"\b(?:void\s+|SPIRV_Cross_Output\s+)main\s*\(", hlsl):
        raise ContractFailure("HLSL: no `main(` entry function definition")

    reg_map = {}       # (kind_marker, name) -> (letter, number, space)
    occupied = set()   # (letter, number, space)
    explicit_b_regs = set()
    pushconst_names = {e[3] for e in expected if e[2] == "push_constant"}

    for m in re.finditer(
            r"^(?:globallycoherent\s+)?((?:RW)?ByteAddressBuffer\s+(\w+)"
            r"|(?:RW)?StructuredBuffer<[\w. ]+>\s+(\w+)"
            r"|RWTexture2D<[\w.]+>\s+(\w+)"
            r"|cbuffer\s+(\w+)|Texture2D<[\w.]+>\s+(\w+)"
            r"|SamplerState\s+(\w+)|SamplerComparisonState\s+(\w+)"
            r"|Texture3D<[\w.]+>\s+(\w+)"
            r"|RWTexture3D<[\w.]+>\s+(\w+))"
            r"\s*(?::\s*register\((\w)(\d+)(?:,\s*space(\d+))?\))?",
            hlsl, flags=re.M):
        name = (m.group(2) or m.group(3) or m.group(4) or m.group(5)
                or m.group(6) or m.group(7) or m.group(8)
                or m.group(9) or m.group(10))
        letter, number, space = m.group(11), m.group(12), m.group(13)
        if name in pushconst_names and letter is None:
            continue  # push constant：spirv-cross 不注解，落位交给 DXIL 表核对
        if letter is None:
            raise ContractFailure(f"HLSL: {name} has no register annotation")
        number, space = int(number), int(space or 0)
        # 声明类型词（去掉变量名与模板参数：RWTexture2D<uint4> → RWTexture2D）
        decl_type = m.group(1).split()[0].split("<")[0]
        if decl_type in ("RWByteAddressBuffer", "RWStructuredBuffer"):
            kind = "ssbo_write"
        elif decl_type in ("ByteAddressBuffer", "StructuredBuffer"):
            kind = "ssbo_read"
        elif decl_type == "RWTexture2D":
            kind = "storage_image"
        elif decl_type == "Texture3D":
            kind = "separate_image_3d"
        elif decl_type == "RWTexture3D":
            kind = "storage_image_3d"
        else:
            kind = ("ubo" if letter == "b" else
                    "separate_image" if letter == "t" else
                    "comparison_sampler" if m.group(8) else "separate_sampler")
        reg_map[(kind, name)] = (letter, number, space)
        if (letter, number, space) in occupied:
            raise ContractFailure(f"HLSL: register {letter}{number},space{space} "
                                  f"assigned twice ({name})")
        occupied.add((letter, number, space))
        if letter == "b":
            explicit_b_regs.add(number)

    for set_, binding, kind, name, extra, hlsl_name in expected:
        if kind == "push_constant":
            # spirv-cross 不注解 register；核对 cbuffer 存在 + 尺寸布局
            # （extra=块字节数）：80B=mat4+vec4（末成员 packoffset(c4)，
            # PCGPushConsts 惯例）；64B=单 mat4（无第二成员，M2ShadowPush）。
            m_cb = re.search(rf"cbuffer\s+{re.escape(name)}\s*\{{(.*?)\}}",
                             hlsl, flags=re.S)
            if not m_cb:
                raise ContractFailure(f"HLSL: push constant cbuffer {name} "
                                      f"absent")
            body = m_cb.group(1)
            if extra == 80 and "packoffset(c4)" not in body:
                raise ContractFailure(
                    f"HLSL: push constant {name} layout != 64B+16B "
                    f"(no packoffset(c4) second member)")
            if extra == 64 and re.search(r"packoffset\(c[1-9]\)", body):
                raise ContractFailure(
                    f"HLSL: push constant {name} 64B single-mat4 layout "
                    f"unexpectedly has extra members ({body.strip()[:120]})")
            continue
        hit = reg_map.get((kind, hlsl_name))
        if hit is None and kind == "combined_texture":
            # HLSL 层组合采样器 = Texture2D + 自动 <name>_sampler 对（与
            # separate_image 同形）；按 t 寄存器映射回退，再核对自动采样器
            # 落位 s<binding>（spirv-cross 命名 <name>_sampler）。
            hit = reg_map.get(("separate_image", hlsl_name))
            # spirv-cross 自动采样器命名 = _<name>_sampler（前导下划线惯例）
            samp = (reg_map.get(("separate_sampler", f"_{hlsl_name}_sampler"))
                    or reg_map.get(("separate_sampler", f"{hlsl_name}_sampler")))
            if samp is None or samp[0] != "s" or samp[1] != binding:
                raise ContractFailure(
                    f"HLSL: combined sampler {hlsl_name}_sampler not at "
                    f"s{binding} (got {samp})")
        if hit is None:
            raise ContractFailure(f"HLSL: {kind} {hlsl_name} not annotated")
        letter, number, space = hit
        if number != binding:
            raise ContractFailure(
                f"HLSL: {hlsl_name} register {letter}{number} != binding {binding}")
        if space != set_:
            raise ContractFailure(
                f"HLSL: {hlsl_name} space{space} != set {set_}")

    # ---- 2b) MRT 输出目标契约（M4 F4-2）：HLSL SV_TargetN 注解核对 ----
    # spirv-cross --reflect 无 fragment outputs 面；SV_TargetN 是 spirv-cross
    # HLSL 层的逐目标注解（核对层级=上文任务书允许的"至少 HLSL SV_Target
    # 注解核对"）。列出契约的 shader：SV_Target 集合必须逐项相等。
    want_targets = EXPECTED_TARGETS.get(rel)
    if want_targets is not None:
        got_targets = []
        # spirv-cross HLSL 的 fragment 输出有两种形态：单入口直接 wrapper 的
        # `out <type> <name> : SV_TargetN` 形参，或多输出/显式拆分的
        # `struct SPIRV_Cross_Output { <type> <name> : SV_TargetN; ... };`
        # 成员——两种都匹配（成员形不要求行首 out 前缀）。
        for m in re.finditer(
                r"(?:out\s+)?[\w<>]+\s+(\w+)\s*:\s*SV_Target(\d+)\s*;", hlsl):
            got_targets.append((int(m.group(2)), m.group(1)))
        if sorted(got_targets) != sorted(want_targets):
            raise ContractFailure(
                f"HLSL: SV_Target set {sorted(got_targets)} != expected "
                f"{sorted(want_targets)}")

    # ---- 3) dxc 编译 + dumpbin Resource Bindings（终产物权威）----
    cso = os.path.join(td, rel.replace("/", "_") + ".cso")
    rc, out = run([DXC, "-T", PROFILE_OF_STAGE[stage], "-E", "main",
                   hlsl_path, "-Fo", cso])
    if rc != 0:
        raise ContractFailure(f"dxc: {out.strip()[:500]}")
    rc, dump = run([DXC, "-dumpbin", cso])
    if rc != 0:
        raise ContractFailure(f"dxc -dumpbin: {dump.strip()[:500]}")

    # 行样例: "; ViewData  cbuffer  NA  NA  CB1  cb0  1"
    #         "; albedoSampler  sampler  NA  NA  S0  s1,space1  1"
    dxil_binds = {}
    in_table = False
    for line in dump.splitlines():
        if "Resource Bindings:" in line:
            in_table = True
            continue
        if in_table:
            mm = re.match(
                r";\s*(\w+)\s+(cbuffer|sampler|texture|UAV\w*)\s+\S+\s+\S+\s+"
                r"\w+\s+(cb|t|s|u)(\d+)(?:,space(\d+))?\s+(\d+)", line)
            if mm:
                name, _typ, letter, number, space, _cnt = mm.groups()
                dxil_binds[name] = (letter, int(number), int(space or 0))
            elif line.strip() and not line.strip().startswith(";") :
                pass  # 表外行
            elif line.strip() == ";" and dxil_binds:
                break  # 表结束
    if not dxil_binds:
        # 零资源 shader（M3Post.vert：vertex-ID 驱动全屏三角形，契约表空）
        # 在 DXIL 里本就没有 Resource Bindings 表——dxc 编译成功即验收，
        # 表缺失不是失败（非空契约仍要求表可解析）。
        if any(k != "push_constant" for _, _, k, *_ in expected):
            raise ContractFailure("dumpbin: no Resource Bindings table parsed\n" +
                                  dump[:800])
        return {
            "source": rel,
            "entry": "main",
            "bindings": [],
            "push_constant_register": None,
        }

    def normalize_letter(ch):
        return "b" if ch == "cb" else ch  # dumpbin 常量缓冲前缀是 cb

    for set_, binding, kind, name, _extra, hlsl_name in expected:
        letter = REGISTER_LETTER[kind]
        hit = dxil_binds.get(hlsl_name)
        if hit is None and (rel, hlsl_name) in DXIL_OPTIONAL:
            # DXC 对"声明了但 shader 体内未消费"的采样器会从 DXIL 资源表
            # 淘汰（M5Mipmap：texelFetch→Texture.Load 不走采样器）——反射
            # 与 HLSL 注解两层已核对绑定存在与落位，DXIL 层显式豁免登记。
            continue
        if hit is None:
            raise ContractFailure(f"DXIL: binding {hlsl_name} absent from "
                                  f"Resource Bindings ({sorted(dxil_binds)})")
        dletter, dnumber, dspace = normalize_letter(hit[0]), hit[1], hit[2]
        if kind == "push_constant":
            # 固化规则（spike §10）：push constant 落位 = 下一个空闲 bN =
            # 该 stage 显式 b 寄存器个数（UBO 绑定号 0..n-1 稠密时即 n）。
            want = len(explicit_b_regs)
            if dletter != "b" or dnumber != want:
                raise ContractFailure(
                    f"DXIL: push constant {name} at {dletter}{dnumber} != "
                    f"expected next-free b{want}")
            if (dletter, dnumber, dspace) in occupied:
                raise ContractFailure(
                    f"DXIL: push constant {name} register collides with a "
                    f"declared binding")
            continue
        if (dletter, dnumber, dspace) != (letter, binding, set_):
            raise ContractFailure(
                f"DXIL: {name} at {dletter}{dnumber},space{dspace} != "
                f"{letter}{binding},space{set_}")

    return {
        "source": rel,
        "entry": "main",
        "bindings": [
            {"set": s, "binding": b, "kind": k, "name": n,
             "register": (None if k == "push_constant"
                          else f"{REGISTER_LETTER[k]}{b},space{s}")}
            for s, b, k, n, *_ in expected
        ],
        "push_constant_register": (
            f"b{len(explicit_b_regs)}"
            if any(k == "push_constant" for _, _, k, *_ in expected) else None),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--keep-temp", action="store_true")
    args = ap.parse_args()

    failures = []
    results = []
    td = tempfile.mkdtemp(prefix="d3d12_reflect_")
    try:
        for rel in EXPECTED:
            try:
                results.append(check_shader(rel, td))
                print(f"PASS {rel}")
            except ContractFailure as e:
                failures.append((rel, str(e)))
                print(f"FAIL {rel}: {e}", file=sys.stderr)
    finally:
        if args.keep_temp:
            print(f"[temp kept] {td}")
        else:
            import shutil
            shutil.rmtree(td, ignore_errors=True)

    print(json.dumps({"shaders": results,
                      "failed": [f[0] for f in failures]}, indent=2))
    if failures:
        print(f"reflection contract FAILED ({len(failures)})", file=sys.stderr)
        return 1
    print(f"reflection contract PASSED ({len(results)} shaders, "
          f"entry=main, bindings unique)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
