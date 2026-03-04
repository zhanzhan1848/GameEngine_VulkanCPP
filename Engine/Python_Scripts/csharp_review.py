#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
文件级注释：
csharp_review.py

用途：
- 对 C# UI 层（PrimalEditor 与 PrimalEditor_Avalonia）进行基础静态审查：
  1) 检查文件级注释是否存在；
  2) 检查函数/方法级文档注释（///）是否存在；
  3) 扫描 TODO/FIXME 标记；
  4) 若提供构建日志则解析编译警告与错误数量。

设计约束：
- 使用 Python 标准库，不引入第三方库；
- 识别规则为近似匹配，强调覆盖面与轻量化；
- 输出 Markdown 报告到 Docs/CodeReview/ 目录。
"""

import argparse
import os
import re
import sys
from typing import List, Tuple


CS_SUFFIXES = (".cs",)


def list_cs_files(dirs: List[str]) -> List[str]:
    """
    函数级注释：
    遍历多个目录，收集所有 C# 源文件路径。

    参数：
    - dirs: 待扫描的目录列表

    返回：
    - 文件路径列表
    """
    files: List[str] = []
    for root in dirs:
        if not os.path.isdir(root):
            continue
        for dirpath, _, filenames in os.walk(root):
            for name in filenames:
                if name.endswith(CS_SUFFIXES):
                    files.append(os.path.join(dirpath, name))
    return sorted(files)


def read_lines(path: str) -> List[str]:
    """
    函数级注释：
    读取文本文件，并返回行列表；失败时返回空列表。
    """
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read().splitlines()
    except Exception:
        return []


def has_file_header(lines: List[str]) -> bool:
    """
    函数级注释：
    判断文件前若干行是否包含文件级注释（以 // 或 /* 开始作为近似规则）。
    """
    head = "\n".join(lines[:20]).strip()
    return head.startswith("//") or head.startswith("/*")


def find_methods(lines: List[str]) -> List[Tuple[int, str]]:
    """
    函数级注释：
    粗略匹配 C# 方法定义行及签名（近似规则）。
    """
    results: List[Tuple[int, str]] = []
    # 近似匹配：修饰符 + 返回类型 + 方法名(...)
    method_re = re.compile(r"^(public|private|protected|internal|static|virtual|override|async|sealed|new|partial|extern)[\w\s<>\[\]\(\)\.,]*\([^\)]*\)\s*\{\s*$")
    for i, line in enumerate(lines):
        if method_re.match(line.strip()):
            results.append((i, line.strip()))
    return results


def has_doc_above(lines: List[str], line_index: int, window: int = 3) -> bool:
    """
    函数级注释：
    判断方法定义上方若干行内是否存在 C# 文档注释（以 /// 开始）。
    """
    start = max(0, line_index - window)
    segment = lines[start:line_index]
    return any(s.strip().startswith("///") for s in segment)


def scan_todos(lines: List[str]) -> int:
    """
    函数级注释：
    统计 TODO/FIXME 的出现次数。
    """
    text = "\n".join(lines).lower()
    return text.count("todo") + text.count("fixme")


def parse_build_log(path: str) -> Tuple[int, int]:
    """
    函数级注释：
    从构建日志中粗略统计警告与错误数量。
    """
    warnings = 0
    errors = 0
    lines = read_lines(path)
    for ln in lines:
        low = ln.lower()
        if "warning:" in low or "warning cs" in low:
            warnings += 1
        if "error:" in low or "error cs" in low:
            errors += 1
    return warnings, errors


def make_markdown(summary: dict, details: List[dict]) -> str:
    """
    函数级注释：
    生成 Markdown 报告文本。
    """
    md = []
    md.append("# C# UI 代码审查报告\n")
    md.append(f"- 审查目录: {', '.join(summary.get('source_dirs', []))}\n")
    md.append(f"- 文件总数: {summary.get('files', 0)}\n")
    md.append(f"- 缺少文件级注释文件数: {summary.get('missing_file_header', 0)}\n")
    md.append(f"- 缺少方法文档注释方法数: {summary.get('missing_method_docs', 0)}\n")
    md.append(f"- TODO/FIXME 总计: {summary.get('todos', 0)}\n")
    md.append(f"- 编译警告统计: {summary.get('warnings', 0)}\n")
    md.append(f"- 编译错误统计: {summary.get('errors', 0)}\n\n")

    md.append("## 详细结果\n\n")
    for it in details:
        md.append(f"### {it['file']}\n")
        md.append(f"- 文件级注释: {'有' if it['has_file_header'] else '缺失'}\n")
        md.append(f"- TODO/FIXME: {it['todo_count']}\n")
        if it['methods']:
            md.append("- 方法文档注释检查:\n")
            for m in it['methods']:
                md.append(f"  - 行 {m['line']}: {'有注释' if m['has_doc'] else '缺少注释'} | {m['signature']}\n")
        md.append("\n")
    return "".join(md)


def run_review(source_dirs: List[str], build_log: str) -> str:
    """
    函数级注释：
    执行 C# UI 层基础审查并返回报告文本。
    """
    files = list_cs_files(source_dirs)
    details = []
    missing_file_header = 0
    missing_method_docs = 0
    total_todos = 0
    for fp in files:
        lines = read_lines(fp)
        has_header = has_file_header(lines)
        if not has_header:
            missing_file_header += 1
        todos = scan_todos(lines)
        total_todos += todos
        methods = []
        for idx, sig in find_methods(lines):
            has_doc = has_doc_above(lines, idx, window=4)
            if not has_doc:
                missing_method_docs += 1
            methods.append({"line": idx + 1, "signature": sig, "has_doc": has_doc})
        details.append({
            "file": fp,
            "has_file_header": has_header,
            "todo_count": todos,
            "methods": methods,
        })

    warnings, errors = parse_build_log(build_log) if build_log else (0, 0)
    summary = {
        "source_dirs": source_dirs,
        "files": len(files),
        "missing_file_header": missing_file_header,
        "missing_method_docs": missing_method_docs,
        "todos": total_todos,
        "warnings": warnings,
        "errors": errors,
    }
    return make_markdown(summary, details)


def parse_args(argv) -> argparse.Namespace:
    """
    函数级注释：
    解析命令行参数。
    """
    p = argparse.ArgumentParser(description="C# UI 层代码基础审查")
    p.add_argument("--source-dirs", nargs="+", required=True, help="待审查的目录列表，如 PrimalEditor PrimalEditor_Avalonia")
    p.add_argument("--build-log", required=False, default="", help="构建日志路径")
    p.add_argument("--out", required=True, help="输出 Markdown 报告路径")
    return p.parse_args(argv)


def main() -> int:
    """
    函数级注释：
    主入口，执行审查并写出报告。
    """
    args = parse_args(sys.argv[1:])
    md = run_review(args.source_dirs, args.build_log)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(md)
    print(f"C#审查报告已生成: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())