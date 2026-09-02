#!/usr/bin/env python3
"""
migrate_bare_streams.py — 轨道 C 裸流批量迁移（CLI 规划 §11.3，行为零变化、按模块分批）

把目标文件里的裸流语句
    std::cerr << …
    std::cout << …
机械替换为 ostream 兼容宏（Engine/Common/CoreLogStream.h）：
    PRIMAL_LOG_STREAM(Error, "<category>") << …
    PRIMAL_LOG_STREAM(Info,  "<category>") << …

规则：
  · 只替换语句起始位置的 `std::cerr <<` / `std::cout <<`（跨行链式 `<< …`
    天然保持不动）；
  · category 按目录映射（与命令命名空间对齐），脚本参数可覆盖；
  · 含特殊流操纵符（std::hex/setw/fixed…）的文件自动跳过（需人工迁移）；
  · 已迁移文件（含 PRIMAL_LOG_STREAM）自动跳过，可安全重复执行。

用法：
  python3 scripts/migrate_bare_streams.py <file.cpp> [<file.cpp> …]
  git diff --stat   # 复核后随模块分批提交
"""

import re
import sys
from pathlib import Path

# 目录 → 类别（与命令命名空间对齐，规划 §11.2）
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
    ("EngineDLL", "Engine"),
]

# 含特殊流操纵符的文件需人工迁移（机械替换会把 std::hex 变成 shim 不认识的类型）
EXOTIC_PATTERN = re.compile(r"std::(hex|dec|oct|setw|setfill|setprecision|fixed|scientific|left|right|internal)\b")

# 语句起始：行首空白 + std::cout/cerr + 空格 + <<
STATEMENT = re.compile(r"^(\s*)(std::cerr|std::cout)\s*<<")
ALREADY = "PRIMAL_LOG_STREAM("


def category_for(path: Path) -> str:
    posix = path.as_posix()
    for prefix, category in CATEGORY_BY_DIR:
        if prefix in posix:
            return category
    return "Engine"


def migrate(path: Path) -> str:
    text = path.read_text()
    if ALREADY in text:
        return "skip:already-migrated"
    if EXOTIC_PATTERN.search(text):
        return "skip:exotic-manipulators"
    if "std::cout" not in text and "std::cerr" not in text:
        return "skip:no-sites"

    category = category_for(path)
    includes = []
    if "std::cout" in text:
        includes.append(("std::cout", f'PRIMAL_LOG_STREAM(Info, "{category}")'))
    if "std::cerr" in text:
        includes.append(("std::cerr", f'PRIMAL_LOG_STREAM(Error, "{category}")'))

    replaced = 0
    out_lines = []
    for line in text.split("\n"):
        match = STATEMENT.match(line)
        if match:
            indent, stream = match.group(1), match.group(2)
            replacement = next(macro for token, macro in includes if token == stream)
            line = line[: match.start(2)] + replacement + line[match.end(2):]
            replaced += 1
        out_lines.append(line)
    text = "\n".join(out_lines)

    # 补 include：插在头部的 include 区之后——扫描在 raw-string（R"）/首个
    # namespace/class 前停止（避免掉进 #ifdef 块或嵌入 MSL 字符串的教训）。
    if "#include" in text and "Engine/Common/CoreLogStream.h" not in text:
        include_line = '#include "Engine/Common/CoreLogStream.h"'
        lines = text.split("\n")
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
        lines.insert(last_safe_include + 1, include_line)
        text = "\n".join(lines)

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
            print(f"skip:not-a-file:{arg}")
            continue
        result = migrate(path)
        summary[result.split(":")[0]] = summary.get(result.split(":")[0], 0) + 1
        if result.startswith("migrated"):
            print(f"{result}  {arg}")
    print("summary:", summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
