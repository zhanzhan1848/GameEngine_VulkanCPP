#!/usr/bin/env python3
"""scan_include_in_namespace.py — 扫描 "#include 出现在 namespace 深度>0 处"的源文件。

背景（D3D12 R7，2026-11-04）：SimulationLayers.h 曾把 #include <optional> 放在
namespace primal::simulation::layers { } 内部。macOS/libc++ 因前缀头链已全局包含过
<optional>，include guard 掩蔽了错误；MSVC STL 传递闭包不同，首次包含落在 namespace
内，std 全部声明污染进 primal::simulation::layers::std → 94 条级联编译错。

判定原理：剥离注释/字符串/字符字面量后，跟踪花括号栈；每个 '{' 若由
namespace 声明引导则入 'namespace' 桶，否则入 'other' 桶；namespace 深度 =
栈中 'namespace' 桶计数。#include 行处的深度>0 即命中。

已知限制（命中后需人工核对）：
  - 不解析宏（若 namespace 由宏展开开启会漏报/误报深度）；
  - raw string literal R"(...)" 内的花括号按普通文本处理；
  - #if 0 块内的代码仍参与计数（可能产生误报，人工剔除）。
误报常见形态：注释内的 #include（已剥离）、字符串内的（已剥离）、
被 #if 0 废弃的代码（需人工剔除）。

用法：python3 scripts/scan_include_in_namespace.py [根目录]
退出码 0=无命中；1=有命中（CI 可用作门禁）。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

EXCLUDE_PARTS = {
    ".git", ".worktrees", "Darwin", "build", "third_party", "node_modules",
    "bin", "obj", "packages", ".zcode", "Docs", "scripts", "projects_test",
    ".venv", "site-packages",
}
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".inl", ".mm", ".cxx"}

NAMESPACE_HEAD = re.compile(r"(?:^|[;{}>#])\s*(namespace)\b")


def strip_comments_and_literals(text: str) -> str:
    """把注释替换为空格、字符串/字符字面量内容替换为空（保留换行，行号可追溯）。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":                      # 行注释
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":                    # 块注释
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            seg = text[i:j]
            out.append("".join(ch if ch == "\n" else " " for ch in seg))
            i = j
        elif c == '"':                                   # 字符串（含转义）
            if text.startswith('R"', i):                 # raw string：保守按普通串处理
                pass
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            out.append('" ' * (j - i - 1) + '"')
            i = j
        elif c == "'":                                   # 字符字面量
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    j += 1
                    break
                j += 1
            out.append("' ' * 1")
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scan_file(path: Path):
    """返回 [(line_no, depth, include 行原文)]，depth 为该 include 处的 namespace 深度。"""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    clean = strip_comments_and_literals(text)
    hits = []
    stack = []            # 元素: 'ns' | 'other'
    ns_pending = False    # 自上一 ';'/'{'/'}' 起是否见过裸 namespace 关键字
    line_no = 1
    i, n = 0, len(clean)
    line_start = 0
    while i < n:
        c = clean[i]
        if c == "\n":
            line_no += 1
            line_start = i + 1
            i += 1
            continue
        if c == "#":
            # 预处理指令：仅识别本行 #include
            j = clean.find("\n", i)
            j = n if j == -1 else j
            directive = clean[i:j]
            if directive.lstrip("# \t").startswith("include"):
                depth = stack.count("ns")
                if depth > 0:
                    hits.append((line_no, depth, directive.strip()))
            i = j
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (clean[j].isalnum() or clean[j] == "_"):
                j += 1
            word = clean[i:j]
            if word == "namespace":
                # 'using namespace X;' 不开作用域：回看本语句是否以 using 开头。
                # 逐词回溯到最近的 ; { } 或行首即可（简化：查本行内该词之前是否有 using）
                k = line_start
                seg = clean[k:i]
                if not re.search(r"\busing\s*$", seg):
                    ns_pending = True
            i = j
            continue
        if c == "{":
            stack.append("ns" if ns_pending else "other")
            ns_pending = False
            i += 1
            continue
        if c == "}":
            if stack:
                stack.pop()
            ns_pending = False
            i += 1
            continue
        if c in ";>":                                     # 模板/语句边界重置
            ns_pending = False
        i += 1
    return hits


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    files = []
    for p in sorted(root.rglob("*")):
        if not p.is_file() or p.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        rel = p.relative_to(root)
        if any(part in EXCLUDE_PARTS for part in rel.parts):
            continue
        files.append(p)

    total_hits = 0
    print(f"scanned {len(files)} source files under {root}")
    for p in files:
        try:
            hits = scan_file(p)
        except Exception as e:  # noqa: BLE001 — 单文件异常不中断全仓扫描
            print(f"[scanner-error] {p}: {e}")
            continue
        if not hits:
            continue
        try:
            src_lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            src_lines = []
        for line_no, depth, directive in hits:
            total_hits += 1
            raw = src_lines[line_no - 1].strip() if line_no - 1 < len(src_lines) else directive
            print(f"[hit] {p.relative_to(root)}:{line_no} ns_depth={depth} {raw}")
    print(f"total hits: {total_hits}")
    return 1 if total_hits else 0


if __name__ == "__main__":
    sys.exit(main())
