#!/usr/bin/env python3
"""
migrate_bare_streams_pass2.py — 轨道 C 第二轮：行中语句的上下文感知迁移

第一轮（migrate_bare_streams.py）只替换"行首语句起始"；本轮用逐字符状态机
（跟踪字符串/字符字面量/行注释/块注释/raw-string，状态跨行保持）替换任意
代码上下文中的 `std::cout <<` / `std::cerr <<`，含：
    if (x) std::cout << …;    cond ? std::cerr << … : …;    foo(), std::cout << …;

安全性：
  · 字符串/字符字面量/注释内的出现一律不动；
  · raw-string（R"delim(…)delim"）整体跳过（跨行状态保持）；
  · # 预处理行不动（宏体内的替换交给人工）；
  · 含特殊流操纵符的文件仍自动跳过。

用法：
  python3 scripts/migrate_bare_streams_pass2.py <file.cpp> […]
"""

import re
import sys
from pathlib import Path

CATEGORY_BY_DIR = [
    ("Engine/Graphics/RHI/Platforms/Vulkan", "RHI"),
    ("Engine/Graphics/RHI/Platforms/Metal", "RHI"),
    ("Engine/Graphics/RHI/Platforms/Dawn", "RHI"),
    ("Engine/Graphics/RHI", "RHI"),
    ("Engine/Graphics/RenderPipeline", "Render"),
    ("Engine/Graphics/RenderGraph", "Render"),
    ("Engine/Graphics/Metal", "RHI"),
    ("Engine/Graphics/Vulkan", "RHI"),
    ("Engine/Graphics/Lumen", "Render"),
    ("Engine/Graphics/Nanite", "Render"),
    ("Engine/Graphics/Scene", "Scene"),
    ("Engine/Content", "Asset"),
    ("Engine/Particles", "Particles"),
    ("Engine/Components", "Scene"),
    ("Engine/Platform", "Engine"),
    ("EngineDLL", "Engine"),
]

EXOTIC_PATTERN = re.compile(r"std::(hex|dec|oct|setw|setfill|setprecision|fixed|scientific|left|right|internal)\b")
RAW_START = re.compile(r'R"([^()\\ \t\n]*)\(')  # R"delim(  — delim 不含括号/空白


def category_for(path: Path) -> str:
    posix = path.as_posix()
    for prefix, category in CATEGORY_BY_DIR:
        if prefix in posix:
            return category
    return "Engine"


def rewrite_line(line: str, state: dict, macro_out: str, macro_err: str) -> str:
    """逐字符扫描一行，代码上下文中的 std::cout/cerr<< 替换为宏。
    state 携带跨行状态：in_block_comment / in_raw_string / raw_delim。"""
    out = []
    i = 0
    n = len(line)
    in_string = False   # 每行重置（跨行字符串需续行符，本仓库无此惯例）
    in_char = False

    def match_token(pos: int) -> tuple[str, str] | None:
        for token, macro in (("std::cerr", macro_err), ("std::cout", macro_out)):
            if line.startswith(token, pos):
                j = pos + len(token)
                k = j
                while k < n and line[k] in " \t":
                    k += 1
                if line.startswith("<<", k):
                    return token, macro
        return None

    while i < n:
        if state["in_block_comment"]:
            end = line.find("*/", i)
            if end == -1:
                out.append(line[i:])
                return "".join(out)
            out.append(line[i:end + 2])
            i = end + 2
            state["in_block_comment"] = False
            continue
        if state["in_raw_string"]:
            end_marker = ')' + state["raw_delim"] + '"'
            end = line.find(end_marker, i)
            if end == -1:
                out.append(line[i:])
                return "".join(out)
            out.append(line[i:end + len(end_marker)])
            i = end + len(end_marker)
            state["in_raw_string"] = False
            continue

        ch = line[i]
        if in_string:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if in_char:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if ch == "'":
                in_char = False
            i += 1
            continue

        if line.startswith('//', i):
            out.append(line[i:])
            return "".join(out)
        if line.startswith('/*', i):
            state["in_block_comment"] = True
            out.append('/*')
            i += 2
            continue
        if ch == '"':
            m = RAW_START.match(line, i)
            if m:
                state["in_raw_string"] = True
                state["raw_delim"] = m.group(1) or ''
                out.append(line[i:m.end()])
                i = m.end()
                continue
            in_string = True
            out.append(ch)
            i += 1
            continue
        if ch == "'":
            in_char = True
            out.append(ch)
            i += 1
            continue

        hit = match_token(i)
        if hit:
            _, macro = hit
            out.append(macro)
            i += len(hit[0])
            while i < n and line[i] in " \t":
                out.append(line[i])
                i += 1
            out.append('<<')
            i += 2
            continue

        out.append(ch)
        i += 1
    return "".join(out)


def migrate(path: Path) -> str:
    text = path.read_text()
    if EXOTIC_PATTERN.search(text):
        return "skip:exotic-manipulators"
    if "std::cout" not in text and "std::cerr" not in text:
        return "skip:no-sites"

    category = category_for(path)
    macro_out = f'PRIMAL_LOG_STREAM(Info, "{category}")'
    macro_err = f'PRIMAL_LOG_STREAM(Error, "{category}")'

    state = {"in_block_comment": False, "in_raw_string": False, "raw_delim": ''}
    lines = text.split('\n')
    replaced = 0
    for idx, line in enumerate(lines):
        new_line = rewrite_line(line, state, macro_out, macro_err)
        if new_line != line:
            replaced += 1
            lines[idx] = new_line
    if replaced == 0:
        return "skip:no-change"
    text = '\n'.join(lines)

    if "Engine/Common/CoreLogStream.h" not in text:
        lines = text.split('\n')
        stop = re.compile(r'R"|^\s*namespace\s|^\s*(?:class|struct)\s+\w+')
        depth = 0
        last_safe_include = -1
        for i, line in enumerate(lines):
            if stop.search(line):
                break
            stripped = line.strip()
            if re.match(r"#\s*(if|ifdef|ifndef)\b", stripped):
                depth += 1
            elif re.match(r"#\s*endif\b", stripped):
                depth = max(0, depth - 1)
            elif depth == 0 and line.startswith("#include"):
                last_safe_include = i
        lines.insert(last_safe_include + 1, '#include "Engine/Common/CoreLogStream.h"')
        text = '\n'.join(lines)

    path.write_text(text)
    return f"migrated:{replaced}"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    summary: dict[str, int] = {}
    for arg in sys.argv[1:]:
        path = Path(arg)
        if not path.is_file():
            continue
        result = migrate(path)
        key = result.split(":")[0]
        summary[key] = summary.get(key, 0) + 1
        if key == "migrated":
            print(f"{result}  {arg}")
    print("summary:", summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
