#!/usr/bin/env python3
"""d3d12_shader_spike.py — D3D12 M0 F0-17 shader 编译链 spike 驱动.

主链: GLSL/WGSL --(include 展平 [+GLSL 入口重命名])--> naga --> HLSL --> dxc --> DXIL
备选链(仅主链失败的 GLSL): glslangValidator -V (复刻 build_spv.sh flags) -->
  SPIR-V --> spirv-cross --hlsl --shader-model 60 --> dxc -E main

实测口径（2026-11-03, naga 30.0.0 / dxc 1.9.0.5347 / glslang 16.3.0 /
spirv-cross vulkan-sdk-1.4.350.0）:
  - naga glsl-in 只支持名为 main 的入口（自定义入口报 "Missing entry point"），
    故 GLSL 主链在展平时把 `void <entry>(` 机械重命名为 `void main(`
    （等价 glslang -e 语义；GLSL 入口不会被名字引用）。
  - spirv-cross HLSL 后端把入口统一重命名为 main；须 --shader-model 60。

语料:
  A) glsl: Engine/Graphics/Vulkan/shaders/**/*.{vert,frag,comp}（含 Debug/）
  B) wgsl: Engine/Graphics/Dawn/shaders/**/*.wgsl + Vulkan/shaders 本地 .wgsl
     （逐 @(vertex|fragment|compute) fn 入口编译；文件通过=所有入口通过）
  C) legacy: *.vert.glsl/*.frag.glsl（仅 vcxproj 引用；单独统计）

entry 解析: build_spv.sh LUMEN_GLSL_NAMES/ENTRIES 数组（含 += 追加）+
  显式特例表 + 兜底（void main / 唯一 void 函数 / build_spv.sh -e 引用）。

include 展平: build_spv.sh inline_includes 的 Python 等价（相对源目录递归
  解析 "..."；GL_GOOGLE_include_directive 行剔除——展平后无意义且 naga 不识）。

输出: --out JSON（逐文件结果 + 失败编目）。退出码 0=分析完成。
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
DAWN_SHADERS = os.path.join(REPO, "Engine/Graphics/Dawn/shaders")
BUILD_SPV = os.path.join(VK_SHADERS, "build_spv.sh")

NAGA = os.path.expanduser("~/.cargo/bin/naga")
DXC = "/usr/local/bin/dxc"
GLSLANG = "/usr/local/bin/glslangValidator"
SPIRV_CROSS = "/usr/local/bin/spirv-cross"

STAGE_OF_EXT = {"vert": "vert", "frag": "frag", "comp": "comp"}
PROFILE_OF_STAGE = {"vert": "vs_6_0", "frag": "ps_6_0", "comp": "cs_6_0",
                    "vertex": "vs_6_0", "fragment": "ps_6_0", "compute": "cs_6_0"}

SPECIAL_ENTRIES = {
    "IBL_SpecularPrefilter.comp": "cs_main",
    "Bloom.vert": "bloom_vs",
    "Bloom.frag": "bright_pass_fs",
    "Nanite/ClusterBinning.comp": "cluster_binning_kernel",
    "Nanite/VisibilityBuffer.vert": "visibility_vertex_shader",
    "Nanite/VisibilityBuffer.frag": "visibility_fragment_shader",
    "Nanite/VisibilityBufferResolve.comp": "ComputeMain",
}

# 独立源文件其规范编译需要 -D（build_spv.sh 内同义）；
# 其余 -D 变体产物复用已单列的规范源文件，不在此列。
VARIANT_DEFINES = {
    "Lumen/DDGITraceSDF_measured_geometry.comp": ["DDGI_MEASURED_GEOMETRY=1"],
}


def run(cmd, timeout=90):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stderr or "") + (p.stdout or "")
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"
    except FileNotFoundError as e:
        return 127, f"missing tool: {e}"


def inline_includes(src_path):
    """build_spv.sh awk 版等价：相对源文件目录递归展平 #include "..."。"""
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
    text = re.sub(r"^#extension\s+GL_GOOGLE_include_directive[^\n]*\n", "", text,
                  flags=re.MULTILINE)
    return text


def rename_entry_to_main(text, entry):
    if entry == "main":
        return text
    return re.sub(rf"\bvoid\s+{re.escape(entry)}\s*\(", "void main(", text, count=1)


def wgsl_entries(text):
    """WGSL: 解析 @(vertex|fragment|compute) ... fn name() 入口表。"""
    out = []
    for m in re.finditer(r"\bfn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", text):
        head = text[max(0, m.start() - 120) : m.start()]
        for stage_kw in ("vertex", "fragment", "compute"):
            if f"@{stage_kw}" in head:
                out.append((m.group(1), stage_kw))
                break
    return out


def parse_lumen_arrays():
    with open(BUILD_SPV, "r", encoding="utf-8") as f:
        script = f.read()
    names, entries = [], []
    for m in re.finditer(
        r'LUMEN_GLSL_NAMES\+?=?\(([^)]*)\)|LUMEN_GLSL_ENTRIES\+?=?\(([^)]*)\)', script
    ):
        items = re.findall(r'"([^"]+)"', m.group(1) or m.group(2) or "")
        if m.group(1) is not None:
            names.extend(items)
        else:
            entries.extend(items)
    return dict(zip(names, entries))


def find_entry(rel, flat_text, lumen_map, script_lines):
    if rel in SPECIAL_ENTRIES:
        return SPECIAL_ENTRIES[rel]
    stem = rel.rsplit(".", 1)[0]
    if rel in lumen_map:
        return lumen_map[rel]
    if stem in lumen_map:  # LUMEN_GLSL_NAMES 键不带 stage 扩展名
        return lumen_map[stem]
    if "void main(" in flat_text or "void main (" in flat_text:
        return "main"
    voids = re.findall(r"void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", flat_text)
    if len(set(voids)) == 1:
        return voids[0]
    base = os.path.basename(rel)
    for i, line in enumerate(script_lines):
        if base in line:
            for j in range(max(0, i - 6), min(len(script_lines), i + 7)):
                m2 = re.search(r"-e\s+([A-Za-z_][A-Za-z0-9_]*)", script_lines[j])
                if m2 and m2.group(1) != "NAME":
                    return m2.group(1)
    return voids[0] if voids else "main"


def hlsl_entry_in(hlsl_text, wanted):
    """入口存在性检查（宽松）：naga HLSL 入口形如 `VertexOutput_main main(...)`，
    void 返回入口为 `name_`（尾下划线，无 wrapper）；spirv-cross 为
    `void main(SPIRV_Cross_Input)`。最终仲裁者是 dxc -E。"""
    for cand in (wanted, wanted + "_", "main"):
        if re.search(rf"^[^/\n]*\b{re.escape(cand)}\s*\(", hlsl_text, flags=re.M):
            return cand
    return None


def classify_naga_error(err):
    low = err.lower()
    if "missing entry point" in low:
        return "naga-entry-main-only"
    if "extension" in low:
        return "naga-extension-unsupported"
    if "include" in low:
        return "naga-include-unsupported"
    if any(k in low for k in ("grammar", "parse", "syntax", "expected")):
        return "naga-syntax-parse"
    if any(k in low for k in ("semantic", "unsupported", "not implemented",
                              "invalid")):
        return "naga-semantic-unsupported"
    return "naga-other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--skip-backup", action="store_true")
    ap.add_argument("--backup-all", action="store_true",
                    help="对全部 GLSL（含主链已过的）跑备选链——主链资格评估用")
    args = ap.parse_args()

    with open(BUILD_SPV, "r", encoding="utf-8") as f:
        script_lines = f.read().splitlines()
    lumen_map = parse_lumen_arrays()

    corpus = []
    for root, _dirs, files in os.walk(VK_SHADERS):
        for name in sorted(files):
            ext = name.rsplit(".", 1)[-1]
            rel = os.path.relpath(os.path.join(root, name), VK_SHADERS)
            if ext in STAGE_OF_EXT:
                corpus.append(("glsl", rel, os.path.join(root, name), ext))
            elif name.endswith(".vert.glsl") or name.endswith(".frag.glsl"):
                stage = "vert" if name.endswith(".vert.glsl") else "frag"
                corpus.append(("legacy", rel, os.path.join(root, name), stage))
            elif ext == "wgsl":
                corpus.append(("wgsl", rel, os.path.join(root, name), "wgsl"))
    if os.path.isdir(DAWN_SHADERS):
        for root, _dirs, files in os.walk(DAWN_SHADERS):
            for name in sorted(files):
                if name.endswith(".wgsl"):
                    rel = os.path.relpath(os.path.join(root, name), DAWN_SHADERS)
                    corpus.append(("wgsl", f"Dawn/{rel}", os.path.join(root, name),
                                   "wgsl"))

    results = []
    counts = {"naga_fail": 0, "dxc_fail_after_naga": 0, "main_ok": 0,
              "backup_recovered": 0, "backup_also_failed": 0,
              "wgsl_ok": 0, "wgsl_fail": 0, "legacy_main_ok": 0,
              "legacy_fail": 0}
    failure_catalog = []

    for kind, rel, path, stage_ext in corpus:
        rec = {"kind": kind, "file": rel, "stage": stage_ext}
        flat = open(path, "r", encoding="utf-8", errors="replace").read()
        if "#include" in flat:
            flat = inline_includes(path)

        if kind == "wgsl":
            targets = wgsl_entries(flat)
            if not targets:
                rec["entries"] = []
                rec["err"] = "no @(vertex|fragment|compute) fn detected"
                counts["wgsl_fail"] += 1
                failure_catalog.append({"file": rel, "stage": "entry-detect",
                                        "class": "wgsl-no-entry",
                                        "err": [rec["err"]]})
                results.append(rec)
                continue
        else:
            stage = STAGE_OF_EXT.get(stage_ext, stage_ext)
            entry = (find_entry(rel, flat, lumen_map, script_lines)
                     if kind == "glsl" else "main")
            rec["entry"] = entry
            targets = [(entry, stage)]

        all_targets_ok = True
        with tempfile.TemporaryDirectory() as td:
            for entry, stage in targets:
                tr = {"entry": entry, "stage": stage}
                hlsl = os.path.join(td, "out.hlsl")
                if kind in ("glsl", "legacy"):
                    src = os.path.join(td, "in.comp" if stage == "comp" else
                                       f"in.{stage}")
                    with open(src, "w") as f:
                        f.write(rename_entry_to_main(flat, entry))
                    ncmd = [NAGA, "--input-kind", "glsl", "--shader-stage", stage,
                            "--entry-point", "main", "--shader-model", "60",
                            src, hlsl]
                    for dv in VARIANT_DEFINES.get(rel, []):
                        ncmd += ["-D", dv]
                    wanted = "main"
                else:
                    src = os.path.join(td, "in.wgsl")
                    with open(src, "w") as f:
                        f.write(flat)
                    ncmd = [NAGA, "--input-kind", "wgsl", "--entry-point", entry,
                            "--shader-model", "60", src, hlsl]
                    wanted = entry
                rc, err = run(ncmd, args.timeout)
                tr["naga_rc"] = rc
                if rc != 0:
                    tr["naga_err_class"] = classify_naga_error(err)
                    tr["naga_err_head"] = err.strip().splitlines()[:3]
                    failure_catalog.append({
                        "file": rel, "entry": entry, "stage": "naga",
                        "class": tr["naga_err_class"], "err": tr["naga_err_head"]})
                    all_targets_ok = False
                    rec.setdefault("entries", []).append(tr)
                    continue

                hlsl_text = open(hlsl).read()
                dxc_entry = hlsl_entry_in(hlsl_text, wanted)
                tr["hlsl_entry"] = dxc_entry
                if not dxc_entry:
                    rc_d, err_d = 3, f"entry {wanted} not found in naga HLSL output"
                else:
                    dxil = os.path.join(td, "out.dxil")
                    dcmd = [DXC, "-T", PROFILE_OF_STAGE[stage], "-E", dxc_entry,
                            hlsl, "-Fo", dxil, "-Od"]
                    rc_d, err_d = run(dcmd, args.timeout)
                tr["dxc_rc"] = rc_d
                if rc_d != 0:
                    tr["dxc_err_head"] = err_d.strip().splitlines()[:3]
                    failure_catalog.append({
                        "file": rel, "entry": entry, "stage": "dxc(naga-hlsl)",
                        "class": "dxc-fail", "err": tr["dxc_err_head"]})
                    all_targets_ok = False
                rec.setdefault("entries", []).append(tr)

            # ---- 备选链（GLSL；默认仅主链失败时，--backup-all 全量）----
            if kind == "glsl" and not args.skip_backup and (
                    not all_targets_ok or args.backup_all):
                spv = os.path.join(td, "out.spv")
                src_orig = os.path.join(td, "in_orig.comp" if stage == "comp"
                                        else f"in_orig.{stage}")
                with open(src_orig, "w") as f:
                    f.write(flat)
                gcmd = [GLSLANG, "--quiet", "-V", "--source-entrypoint", "main",
                        "-e", entry, src_orig, "-o", spv]
                for dv in VARIANT_DEFINES.get(rel, []):
                    gcmd.insert(3, f"-D{dv}")
                rc_g, err_g = run(gcmd, args.timeout)
                rec["glslang_rc"] = rc_g
                if rc_g != 0:
                    rec["glslang_err_head"] = err_g.strip().splitlines()[:3]
                    failure_catalog.append({
                        "file": rel, "stage": "glslang", "class": "glslang-fail",
                        "err": rec["glslang_err_head"]})
                else:
                    hlsl2 = os.path.join(td, "out2.hlsl")
                    scmd = [SPIRV_CROSS, spv, "--hlsl", "--shader-model", "60",
                            "--output", hlsl2]
                    rc_s, err_s = run(scmd, args.timeout)
                    rec["spirv_cross_rc"] = rc_s
                    if rc_s != 0:
                        rec["spirv_cross_err_head"] = err_s.strip().splitlines()[:3]
                        failure_catalog.append({
                            "file": rel, "stage": "spirv-cross",
                            "class": "spirv-cross-fail",
                            "err": rec["spirv_cross_err_head"]})
                    else:
                        hlsl2_text = open(hlsl2).read()
                        e2 = hlsl_entry_in(hlsl2_text, "main")
                        dxil2 = os.path.join(td, "out2.dxil")
                        if e2:
                            d2cmd = [DXC, "-T", PROFILE_OF_STAGE[stage], "-E", e2,
                                     hlsl2, "-Fo", dxil2, "-Od"]
                            rc_d2, err_d2 = run(d2cmd, args.timeout)
                            rec["backup_dxc_rc"] = rc_d2
                        else:
                            rc_d2, err_d2 = 3, "no main in spirv-cross HLSL"
                            rec["backup_dxc_rc"] = 3
                        if rc_d2 == 0:
                            counts["backup_recovered"] += 1
                        else:
                            counts["backup_also_failed"] += 1
                            failure_catalog.append({
                                "file": rel, "stage": "dxc(backup)",
                                "class": "dxc-fail",
                                "err": err_d2.strip().splitlines()[:3]})

        if kind == "glsl":
            if all_targets_ok:
                counts["main_ok"] += 1
            elif rec.get("naga_rc_if_single_fail") is None:
                pass
        elif kind == "wgsl":
            counts["wgsl_ok" if all_targets_ok else "wgsl_fail"] += 1
        elif kind == "legacy":
            counts["legacy_main_ok" if all_targets_ok else "legacy_fail"] += 1
        results.append(rec)
        if len(results) % 20 == 0:
            print(f"...{len(results)}/{len(corpus)}", file=sys.stderr)

    # 主链失败归因计数（naga 段 vs dxc 段）
    for f_ in failure_catalog:
        if f_["kind"] if "kind" in f_ else False:
            pass
    counts["naga_fail"] = sum(
        1 for f_ in failure_catalog if f_["stage"] == "naga")
    counts["dxc_fail_after_naga"] = sum(
        1 for f_ in failure_catalog if f_["stage"] == "dxc(naga-hlsl)")

    out = {
        "corpus_total": len(corpus),
        "glsl_total": sum(1 for r in results if r["kind"] == "glsl"),
        "wgsl_total": sum(1 for r in results if r["kind"] == "wgsl"),
        "legacy_total": sum(1 for r in results if r["kind"] == "legacy"),
        "counts": counts,
        "failure_catalog": failure_catalog,
        "results": results,
    }
    with open(args.out, "w") as f:
        json.dump(out, f, indent=1)
    print(json.dumps({k: out[k] for k in
                      ("corpus_total", "glsl_total", "wgsl_total", "legacy_total",
                       "counts")}, indent=2))


if __name__ == "__main__":
    sys.exit(main())
