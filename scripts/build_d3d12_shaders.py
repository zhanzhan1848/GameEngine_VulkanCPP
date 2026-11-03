#!/usr/bin/env python3
"""build_d3d12_shaders.py — D3D12 M1 F1-7 shader 主链编译器 + freshness gate 原型.

主链（Docs/RHI/D3D12_Shader_Spike_Report.md §5 决定，GLSL=链 B）:
  GLSL 源（与 Vulkan 生产 .spv 同一源文件）
  → glslangValidator -V -S <stage> --source-entrypoint main -e main → SPIR-V
  → spirv-cross --hlsl --shader-model 60 → HLSL（入口恒重命名为 main）
  → dxc -T <profile> -E main → DXIL .cso

profile 映射（spike §6）: .vert→vs_6_0, .frag→ps_6_0, .comp→cs_6_0。

产物布局（<out-dir> 默认 EngineTest/shaders/D3D12/，镜像源相对路径）:
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
DEFAULT_OUT = os.path.join(REPO, "EngineTest/shaders/D3D12")
MANIFEST_NAME = "manifest.json"

GLSLANG = os.environ.get("D3D12_GLSLANG", "/usr/local/bin/glslangValidator")
SPIRV_CROSS = os.environ.get("D3D12_SPIRV_CROSS", "/usr/local/bin/spirv-cross")
DXC = os.environ.get("D3D12_DXC", "/usr/local/bin/dxc")

# ---------------------------------------------------------------------------
# 编译清单：仓库相对 VK_SHADERS 的 GLSL 路径（不带 .spv）。
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
    # D3D12 M6 F6-2/F6-3: probe-grid GI characterization compute pair.
    # Bake: writeonly rgba16f image3D (typed UAV store) + BakeParams UBO.
    # Accum: readonly image3D (typed UAV load) + sampler3D history pair +
    # writeonly image3D out + flags UBO (EMA / history-zero reset).
    "Forward/M6ProbeBake.comp",
    "Forward/M6ProbeAccum.comp",
]

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


def sha256_file(path):
    h = hashlib.sha256()
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


def compile_shader(rel, out_dir, timeout=120):
    """编译单个 GLSL → .hlsl + .cso。返回 (ok, record, errors)。"""
    stage = STAGE_OF_EXT[rel.rsplit(".", 1)[-1]]
    profile = PROFILE_OF_STAGE[stage]
    src = os.path.join(VK_SHADERS, rel)
    if not os.path.isfile(src):
        return False, None, [f"source missing: {src}"]

    flat = inline_includes(src) if "#include" in open(src, encoding="utf-8",
                                                      errors="replace").read() \
        else open(src, encoding="utf-8", errors="replace").read()

    hlsl_out = os.path.join(out_dir, rel + ".hlsl")
    cso_out = os.path.join(out_dir, rel + ".cso")
    os.makedirs(os.path.dirname(hlsl_out), exist_ok=True)

    errors = []
    with tempfile.TemporaryDirectory() as td:
        spv = os.path.join(td, "stage.spv")
        src_tmp = os.path.join(td, f"in.{stage}")
        with open(src_tmp, "w", encoding="utf-8") as f:
            f.write(flat)
        # 第一级与生产 build_spv.sh 同前端同 flags（spike §5 理由②：
        # GLSL 语义单一事实源；入口=main，--source-entrypoint 语义一致）。
        rc_g, err_g = run([GLSLANG, "--quiet", "-V", "-S", stage,
                           "--source-entrypoint", "main", "-e", "main",
                           src_tmp, "-o", spv], timeout)
        if rc_g != 0:
            return False, None, [f"glslang failed: {err_g.strip()[:2000]}"]

        hlsl_tmp = os.path.join(td, "stage.hlsl")
        rc_s, err_s = run([SPIRV_CROSS, spv, "--hlsl", "--shader-model", "60",
                           "--output", hlsl_tmp], timeout)
        if rc_s != 0:
            return False, None, [f"spirv-cross failed: {err_s.strip()[:2000]}"]

        rc_d, err_d = run([DXC, "-T", profile, "-E", "main", hlsl_tmp,
                           "-Fo", cso_out], timeout)
        if rc_d != 0:
            return False, None, [f"dxc failed: {err_d.strip()[:2000]}"]
        shutil.copyfile(hlsl_tmp, hlsl_out)

    record = {
        "source": rel,
        "source_sha256": sha256_file(src),
        "chain": "glslang->spirv->spirv-cross->dxc",
        "profile": profile,
        "entry": "main",
        "hlsl": os.path.relpath(hlsl_out, out_dir),
        "hlsl_sha256": sha256_file(hlsl_out),
        "cso": os.path.relpath(cso_out, out_dir),
        "cso_sha256": sha256_file(cso_out),
    }
    return True, record, errors


def do_build(shader_list, out_dir):
    missing = check_tools()
    if missing:
        print("FATAL: missing tools: " + ", ".join(missing), file=sys.stderr)
        return 2
    os.makedirs(out_dir, exist_ok=True)
    records, failures = [], []
    for rel in shader_list:
        ok, rec, errs = compile_shader(rel, out_dir)
        if ok:
            records.append(rec)
            print(f"[build] {rel} -> {rec['cso']} ({rec['profile']})")
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
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"manifest: {mpath} ({len(records)} ok, {len(failures)} failed)")
    return 0 if not failures else 1


def do_check(shader_list, out_dir):
    """freshness gate：源/产物 SHA 必须与 manifest 一致。"""
    mpath = os.path.join(out_dir, MANIFEST_NAME)
    if not os.path.isfile(mpath):
        print(f"FRESHNESS FAIL: manifest missing: {mpath}\n"
              f"Run: python3 scripts/build_d3d12_shaders.py", file=sys.stderr)
        return 1
    with open(mpath, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    by_source = {r["source"]: r for r in manifest.get("shaders", [])}

    stale = []
    expected = set(shader_list)
    listed = set(by_source)
    for extra in sorted(listed - expected):
        stale.append(f"manifest has shader no longer in list: {extra}")
    for rel in shader_list:
        rec = by_source.get(rel)
        src = os.path.join(VK_SHADERS, rel)
        if rec is None:
            stale.append(f"{rel}: not in manifest (never built)")
            continue
        if not os.path.isfile(src):
            stale.append(f"{rel}: source missing")
            continue
        src_sha = sha256_file(src)
        if src_sha != rec["source_sha256"]:
            stale.append(f"{rel}: source changed since manifest "
                         f"(manifest {rec['source_sha256'][:12]}.., now {src_sha[:12]}..)")
            continue
        for key in ("hlsl", "cso"):
            p = os.path.join(out_dir, rec[key])
            if not os.path.isfile(p):
                stale.append(f"{rel}: product missing: {rec[key]}")
                continue
            got = sha256_file(p)
            if got != rec[key + "_sha256"]:
                stale.append(f"{rel}: {key} sha mismatch (manifest "
                             f"{rec[key + '_sha256'][:12]}.., now {got[:12]}..)")

    if stale:
        print("D3D12 shader freshness check FAILED:", file=sys.stderr)
        for s in stale:
            print(f"  STALE: {s}", file=sys.stderr)
        return 1
    print(f"D3D12 shader freshness check passed ({len(shader_list)} shaders).")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="freshness gate：重算 SHA 与 manifest 比对，失配 exit 1")
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("shaders", nargs="*",
                    help="覆盖编译清单（VK_SHADERS 相对路径）；默认用内置 M1 清单")
    args = ap.parse_args()

    shader_list = args.shaders if args.shaders else SHADER_LIST
    if args.check:
        return do_check(shader_list, args.out_dir)
    return do_build(shader_list, args.out_dir)


if __name__ == "__main__":
    sys.exit(main())
