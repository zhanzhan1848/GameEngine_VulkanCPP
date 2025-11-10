#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
文件级注释：
cpp_review.py

用途：
- 对 Engine/ 目录下的 C++ 源码进行基础静态审查：
  1) 检查文件级注释是否存在；
  2) 粗略识别函数定义并检查其上方是否存在函数级注释；
  3) 扫描 TODO / FIXME 等待办标记；
  4) 结合编译日志，统计 warning / error 数量并输出概要。

设计约束：
- 使用 Python 标准库，不引入第三方库；
- 尽量遵循项目的代码风格与约束；
- 结果以 Markdown 报告形式输出到 Docs/CodeReview/。
"""

import argparse
import os
import re
import sys
from typing import List, Tuple


CPP_SUFFIXES = (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx")


def list_source_files(root: str) -> List[str]:
    """
    函数级注释：
    列出指定根目录下的所有 C/C++ 源文件路径。

    参数：
    - root: 根目录路径

    返回：
    - 文件路径列表
    """
    files: List[str] = []
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            if name.endswith(CPP_SUFFIXES):
                files.append(os.path.join(dirpath, name))
    return sorted(files)


def read_text(path: str) -> List[str]:
    """
    函数级注释：
    以行列表的形式读取文本文件内容，失败返回空列表。

    参数：
    - path: 文件路径

    返回：
    - 行列表
    """
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read().splitlines()
    except Exception:
        return []


def has_file_header(lines: List[str]) -> bool:
    """
    函数级注释：
    判断文件前若干行是否包含文件级注释（以 // 或 /* 作为近似规则）。

    参数：
    - lines: 文件的行列表

    返回：
    - True / False
    """
    head = "\n".join(lines[:20]).strip()
    return head.startswith("//") or head.startswith("/*")


def find_functions(lines: List[str]) -> List[Tuple[int, str]]:
    """
    函数级注释：
    粗略匹配函数定义的行号与签名（近似规则，可能有误报/漏报）。

    参数：
    - lines: 文件内容的行列表

    返回：
    - (行号, 签名文本) 列表
    """
    results: List[Tuple[int, str]] = []
    # 一个非常保守的近似正则：匹配形如 `ret name(args) {` 的行
    func_re = re.compile(r"^[\w:\*&\s<>]+\w\s*\([^\)]*\)\s*\{\s*$")
    for i, line in enumerate(lines):
        if func_re.match(line.strip()):
            signature = line.strip()
            results.append((i, signature))
    return results


def has_comment_above(lines: List[str], line_index: int, window: int = 3) -> bool:
    """
    函数级注释：
    判断某行的上方若干行内是否存在注释（// 或 /**）。

    参数：
    - lines: 文件的行列表
    - line_index: 目标行号
    - window: 向上检查的行数窗口大小

    返回：
    - True / False
    """
    start = max(0, line_index - window)
    segment = "\n".join(lines[start:line_index])
    return ("//" in segment) or ("/*" in segment)


def scan_todos(lines: List[str]) -> int:
    """
    函数级注释：
    统计文件中 TODO / FIXME 的出现次数。

    参数：
    - lines: 文件的行列表

    返回：
    - 计数值
    """
    text = "\n".join(lines).lower()
    return text.count("todo") + text.count("fixme")


def parse_build_log(path: str) -> Tuple[int, int]:
    """
    函数级注释：
    从编译日志中粗略统计 warning 与 error 数量。

    参数：
    - path: 编译日志路径

    返回：
    - (warnings, errors)
    """
    warnings = 0
    errors = 0
    lines = read_text(path)
    for ln in lines:
        low = ln.lower()
        if "warning:" in low:
            warnings += 1
        if "error:" in low:
            errors += 1
    return warnings, errors


def generate_markdown(summary: dict, details: List[dict]) -> str:
    """
    函数级注释：
    将审查汇总与详细条目生成 Markdown 文本。

    参数：
    - summary: 汇总信息字典
    - details: 每个文件的审查结果列表

    返回：
    - Markdown 文本
    """
    md = []
    md.append("# C++ 代码审查报告\n")
    md.append(f"- 审查目录: {summary.get('source_dir','')}\n")
    md.append(f"- 文件总数: {summary.get('files',0)}\n")
    md.append(f"- 缺少文件级注释文件数: {summary.get('missing_file_header',0)}\n")
    md.append(f"- 缺少函数级注释函数数: {summary.get('missing_func_comments',0)}\n")
    md.append(f"- TODO/FIXME 总计: {summary.get('todos',0)}\n")
    md.append(f"- 编译警告统计: {summary.get('warnings',0)}\n")
    md.append(f"- 编译错误统计: {summary.get('errors',0)}\n\n")

    md.append("## 详细结果\n\n")
    for item in details:
        md.append(f"### {item['file']}\n")
        md.append(f"- 文件级注释: {'有' if item['has_file_header'] else '缺失'}\n")
        md.append(f"- TODO/FIXME: {item['todo_count']}\n")
        if item['functions']:
            md.append("- 函数注释检查:\n")
            for f in item['functions']:
                md.append(f"  - 行 {f['line']}: {'有注释' if f['has_comment'] else '缺少注释'} | {f['signature']}\n")
        md.append("\n")
    return "".join(md)


def run_review(source_dir: str, build_log: str) -> str:
    """
    函数级注释：
    执行 C++ 基础审查流程并返回 Markdown 报告文本。

    参数：
    - source_dir: 需要审查的源码根目录
    - build_log: 编译日志路径，用于统计警告与错误

    返回：
    - Markdown 文本
    """
    files = list_source_files(source_dir)
    details = []
    missing_file_header = 0
    missing_func_comments = 0
    total_todos = 0

    for fp in files:
        lines = read_text(fp)
        has_header = has_file_header(lines)
        if not has_header:
            missing_file_header += 1
        todos = scan_todos(lines)
        total_todos += todos
        funcs = []
        for idx, sig in find_functions(lines):
            has_cmt = has_comment_above(lines, idx, window=4)
            if not has_cmt:
                missing_func_comments += 1
            funcs.append({"line": idx + 1, "signature": sig, "has_comment": has_cmt})
        details.append({
            "file": fp,
            "has_file_header": has_header,
            "todo_count": todos,
            "functions": funcs,
        })

    warnings, errors = parse_build_log(build_log) if build_log else (0, 0)
    summary = {
        "source_dir": source_dir,
        "files": len(files),
        "missing_file_header": missing_file_header,
        "missing_func_comments": missing_func_comments,
        "todos": total_todos,
        "warnings": warnings,
        "errors": errors,
    }
    return generate_markdown(summary, details)


def parse_args(argv) -> argparse.Namespace:
    """
    函数级注释：
    解析命令行参数。

    参数：
    - argv: 命令行参数列表

    返回：
    - 参数对象
    """
    p = argparse.ArgumentParser(description="C++ 代码基础审查")
    p.add_argument("--source-dir", required=True, help="源码根目录，如 Engine")
    p.add_argument("--build-log", required=False, default="", help="编译日志路径")
    p.add_argument("--out", required=True, help="输出 Markdown 报告路径")
    return p.parse_args(argv)


def main() -> int:
    """
    函数级注释：
    主入口，执行审查并写出报告。

    返回：
    - 进程退出码
    """
    args = parse_args(sys.argv[1:])
    md = run_review(args.source_dir, args.build_log)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(md)
    print(f"C++审查报告已生成: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())