#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
文件级注释：
generate_test_report.py

用途：
- 读取 C++ 核心层 EngineTest 的运行日志与退出码，
- 生成结构化的 Markdown 测试报告，写入到 Docs/CI/TestReports/ 目录。

设计约束：
- 仅使用 Python 标准库，不引入第三方库；
- 适配 CI 环境，输入输出路径通过命令行参数指定；
- 与项目的自研测试框架输出兼容（以文本日志为主）。
"""

import argparse
import datetime
import os
import platform
import sys


def read_file(path: str) -> str:
    """
    函数级注释：
    读取指定文件的文本内容，若文件不存在则返回空字符串。

    参数：
    - path: 文件路径

    返回：
    - 文件内容（字符串）
    """
    try:
        if not path or not os.path.exists(path):
            return ""
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
    except Exception as e:
        return f"读取文件失败: {e}"


def write_file(path: str, content: str) -> None:
    """
    函数级注释：
    将文本内容写入到指定路径，自动创建父目录。

    参数：
    - path: 目标文件路径
    - content: 写入的文本内容
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def make_markdown_report(test_output: str, exit_code_text: str, branch: str, commit: str) -> str:
    """
    函数级注释：
    构造测试报告的 Markdown 文本。

    参数：
    - test_output: EngineTest 的标准输出日志文本
    - exit_code_text: 测试退出码文本（可能为空）
    - branch: 本次CI触发所在分支名
    - commit: 本次触发的提交SHA

    返回：
    - Markdown 文本
    """
    now = datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S UTC")
    os_name = platform.system()
    exit_code = exit_code_text.strip() if exit_code_text else ""
    if not exit_code:
        exit_code = "(未知)"

    header = (
        f"# 引擎测试报告\n\n"
        f"- 生成时间: {now}\n"
        f"- 运行系统: {os_name}\n"
        f"- 分支: {branch}\n"
        f"- 提交: {commit}\n"
        f"- 退出码: {exit_code}\n\n"
    )

    summary = "## 测试摘要\n\n"
    if exit_code == "0":
        summary += "- 测试状态: 成功\n\n"
    elif exit_code == "(未知)":
        summary += "- 测试状态: 未知（未捕获退出码）\n\n"
    else:
        summary += "- 测试状态: 失败\n\n"

    log_section = "## 完整日志\n\n"
    if test_output:
        # 限制日志体积，避免文档过长
        lines = test_output.splitlines()
        max_lines = 800
        if len(lines) > max_lines:
            head = "\n".join(lines[:400])
            tail = "\n".join(lines[-400:])
            body = (
                head
                + "\n\n... (日志中间部分省略) ...\n\n"
                + tail
            )
        else:
            body = test_output
        log_section += f"```\n{body}\n```\n\n"
    else:
        log_section += "(无日志输出或未找到日志文件)\n\n"

    return header + summary + log_section


def parse_args(argv) -> argparse.Namespace:
    """
    函数级注释：
    解析命令行参数。

    参数：
    - argv: 命令行参数列表（通常为 sys.argv[1:]）

    返回：
    - 参数命名空间对象
    """
    parser = argparse.ArgumentParser(description="生成 EngineTest 测试报告")
    parser.add_argument("--input", required=True, help="测试输出日志路径")
    parser.add_argument("--exit-code-file", required=False, default="", help="测试退出码文件路径")
    parser.add_argument("--out", required=True, help="输出Markdown报告路径")
    parser.add_argument("--branch", required=False, default="", help="触发分支名")
    parser.add_argument("--commit", required=False, default="", help="触发提交SHA")
    return parser.parse_args(argv)


def main() -> int:
    """
    函数级注释：
    主函数，协调读取输入、生成报告并写入文件。

    返回：
    - 进程退出码（0 表示成功）
    """
    args = parse_args(sys.argv[1:])

    test_output = read_file(args.input)
    exit_code_text = read_file(args.exit_code_file) if args.exit_code_file else ""

    md = make_markdown_report(test_output, exit_code_text, args.branch, args.commit)
    write_file(args.out, md)
    print(f"测试报告已生成: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())