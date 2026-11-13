#!/usr/bin/env python3
# ============================================================================
# DirectXMath 形状门禁 —— 诊断分类与汇总。
# 输入：<report_dir>/per_tu/*.log（run_gate.sh 的原始 clang 输出）
# 输出：<report_dir>/summary.txt
# 分类：
#   MATH      数学相关（DirectXMath/XMFLOAT 形状、MathAccess/MathDirectXOps、
#             columns/下标/构造/运算符不匹配且涉数学类型）
#   PLATFORM  平台/垫片相关（其余）
# ============================================================================
import os
import re
import sys
from collections import defaultdict

report_dir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/dxmath_gate_report"
per_tu = os.path.join(report_dir, "per_tu")

DIAG_START = re.compile(r'^(.+?):(\d+):(\d+): (?:fatal error|error|warning): (.*)$')
NOTE_LINE = re.compile(r'^(?:.+?):(\d+):(\d+): (?:note|warning): ')

MATH_TOKENS = [
    "XMFLOAT", "XMVECTOR", "XMMATRIX", "XMUINT", "XMINT", "DirectX::",
    "XMQuaternion", "XMVector", "XMMatrix", "XMScalar", "XMLoad", "XMStore",
    "XMConvert", "XM_CALLCONV", "MathDirectXOps", "MathAccess", "MathTypes.h",
    "RHIMath.h", "primal::math", "math::v2", "math::v3", "math::v4",
    "math::m3x3", "math::m4x4",
]
STUB_PATH_MARK = "dxmath_shape_gate/include/DirectXMath.h"
ALWAYS_MATH_MSG = ["no member named 'columns'"]
MAYBE_MATH_MSG = ["does not provide a subscript operator",
                  "no matching constructor",
                  "invalid operands to binary expression",
                  "no viable conversion",
                  "cannot convert"]


def classify(block_lines):
    """返回 'MATH' / 'PLATFORM' / None(非错误块)。"""
    if not block_lines:
        return None
    m = DIAG_START.match(block_lines[0])
    if not m or "warning" in block_lines[0].split(": ", 1)[0]:
        # 汇总只统计 error 块
        first = block_lines[0]
        if "error" not in first:
            return None
    joined = "\n".join(block_lines)
    if STUB_PATH_MARK in joined:
        return "MATH"
    if any(tok in joined for tok in MATH_TOKENS):
        return "MATH"
    msg = block_lines[0]
    if any(pat in msg for pat in ALWAYS_MATH_MSG):
        return "MATH"
    if any(pat in msg for pat in MAYBE_MATH_MSG):
        # 仅当 note 里出现数学别名/类型时才算数学（避免误吞其他类型错误）
        notes = "\n".join(block_lines[1:])
        if any(tok in notes for tok in MATH_TOKENS):
            return "MATH"
    return "PLATFORM"


def top_dir(path):
    p = path.strip()
    if p.startswith("/Users/") and "/EngineTest/" in p:
        return "EngineTest"
    for d in ("EngineDLL", "ContentTools", "third_party", "Engine"):
        if p.startswith(d + "/") or ("/" + d + "/") in p:
            return d
    return "other"


def main():
    blocks_math = defaultdict(list)     # top_dir -> [(file:line, msg), ...]
    blocks_platform = defaultdict(int)
    per_tu_state = {}                   # slug -> (math_count, platform_count, special)
    n_tus = 0
    for fn in sorted(os.listdir(per_tu)):
        if not fn.endswith(".log"):
            continue
        n_tus += 1
        slug = fn[:-4]
        special = None
        first_line = ""
        with open(os.path.join(per_tu, fn), encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
        if lines:
            first_line = lines[0]
        if first_line.startswith("MISSING_SOURCE") or first_line.startswith("TIMEOUT"):
            special = first_line.split()[0]
            per_tu_state[slug] = (0, 0, special)
            continue
        # 切诊断块
        blocks = []
        cur = []
        for ln in lines:
            if DIAG_START.match(ln):
                if cur:
                    blocks.append(cur)
                cur = [ln]
            elif cur is not None:
                if (ln.startswith(" ") or ln.startswith("\t") or ln.startswith("~")
                        or NOTE_LINE.match(ln) or ln.startswith("In file included from")
                        or ln.startswith("^") or not ln):
                    cur.append(ln)
                else:
                    # clang 输出杂项行（如 "N warnings generated"）—— 终止当前块
                    if cur:
                        blocks.append(cur)
                    cur = []
            else:
                if DIAG_START.match(ln):
                    cur = [ln]
        if cur:
            blocks.append(cur)
        math_c = plat_c = 0
        for b in blocks:
            if "error" not in b[0]:
                continue
            cls = classify(b)
            if cls == "MATH":
                math_c += 1
                m = DIAG_START.match(b[0])
                loc, msg = m.group(1), m.group(4)
                blocks_math[top_dir(loc)].append((f"{loc}:{m.group(2)}", msg))
            elif cls == "PLATFORM":
                plat_c += 1
                m = DIAG_START.match(b[0])
                blocks_platform[top_dir(m.group(1))] += 1
        per_tu_state[slug] = (math_c, plat_c, None)

    total_math = sum(len(v) for v in blocks_math.values())
    total_plat = sum(blocks_platform.values())
    clean = sum(1 for v in per_tu_state.values() if v[0] == 0 and v[1] == 0 and v[2] is None)
    math_only = sum(1 for v in per_tu_state.values() if v[0] > 0 and v[2] is None)
    out = []
    out.append("==== DirectXMath 形状门禁汇总 ====")
    out.append(f"编译单元总数: {n_tus}")
    out.append(f"零错误 TU: {clean}    含 MATH 错误的 TU: {math_only}")
    out.append(f"MATH 诊断块总数: {total_math}    PLATFORM 诊断块总数: {total_plat}")
    specials = defaultdict(list)
    for slug, v in per_tu_state.items():
        if v[2]:
            specials[v[2]].append(slug)
    for k, v in specials.items():
        out.append(f"{k}: {len(v)}")
    out.append("")
    out.append("-- 按顶层目录的 MATH 诊断数 --")
    for d in sorted(blocks_math, key=lambda d: -len(blocks_math[d])):
        out.append(f"{d}: {len(blocks_math[d])}")
    out.append("-- 按顶层目录的 PLATFORM 诊断数 --")
    for d in sorted(blocks_platform, key=lambda d: -blocks_platform[d]):
        out.append(f"{d}: {blocks_platform[d]}")
    out.append("")
    out.append("-- MATH 诊断样例（前 40）--")
    shown = 0
    for d in sorted(blocks_math, key=lambda d: -len(blocks_math[d])):
        for loc, msg in blocks_math[d]:
            out.append(f"{loc}: {msg}")
            shown += 1
            if shown >= 40:
                break
        if shown >= 40:
            break

    # 按唯一位置（file:line）去重 —— 同一头文件错误会在每个包含它的 TU
    # 重复出现；去重视图才是第 2 阶段的真实工作量清单
    out.append("")
    out.append("-- MATH 唯一位置（file:line 去重，按块数排序，前 60）--")
    uniq = {}
    for d, items in blocks_math.items():
        for loc, msg in items:
            key = loc
            if key not in uniq:
                uniq[key] = [0, set()]
            uniq[key][0] += 1
            uniq[key][1].add(msg[:90])
    for loc, (cnt, msgs) in sorted(uniq.items(), key=lambda kv: -kv[1][0])[:60]:
        out.append(f"{cnt:4d}x {loc}  {sorted(msgs)[0]}")
    out.append(f"MATH 唯一位置总数: {len(uniq)}")
    with open(os.path.join(report_dir, "summary.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
