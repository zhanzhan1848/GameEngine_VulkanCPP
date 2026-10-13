#!/usr/bin/env python3
"""PrimalPlayer E2E 冒烟（release S1 验收，2026-10-14）。

手工构造最小 v2 运行时包，覆盖三件事再实跑 Player 录制：
  1. builtin 行 —— 无资产内置几何（ProceduralMeshCreate 现场生成）；
  2. 包内相对路径 —— 脚本以 `_assets/…` 形式引用（portable 解析顺序：
     包内相对 → 项目根），_assets 不在项目根下，命中包内分支即证明解析正确；
  3. 录制闭环 —— --record N 帧后进程自退，PNG 数 + 亮度 CSV 落盘。

用法：python3 scripts/player_e2e_smoke.py [player 二进制] [--frames N]
默认 player = Darwin/Debug/PrimalPlayer，默认 60 帧。退出码 0=通过，
非 0=失败（stdout 打印检查项清单）。
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def build_package(pkg_dir: Path) -> Path:
    assets = pkg_dir / "_assets"
    assets.mkdir(parents=True, exist_ok=True)
    # 旋转脚本：update 里连续转，管线 tick 生效即有日志可查（frame 计数落
    # Scripts/Log 沙箱，失败也会在 stderr 报 attach/执行错误）。
    (assets / "spin.lua").write_text(
        "local M = {}\n"
        "local r = 0\n"
        "function M:begin_play() r = 0 end\n"
        "function M:update(dt) r = r + (params.speed or 1.0) * dt end\n"
        "return M\n",
        encoding="utf-8",
    )
    pkg = pkg_dir / "e2e.primalrun"
    pkg.write_text(
        "# primal-runtime-scene v2\n"
        'scene "E2E"\n'
        "env 0.05 0.05 0.06 1.0 0 0 0 10 100 0 0 0.4 0.6 0.9 1 0 1.0 0 0 0 0 0 0 0\n"
        "entity\n"
        'name "sun"\n'
        "pos 0 10 0\nrot 0 0 0\nscl 1 1 1\n"
        "light 0 3.0 1 0.95 0.9 0 45 5 1\n"
        "end\n"
        "entity\n"
        'name "ground"\n'
        "pos 0 -1 0\nrot 0 0 0\nscl 1 1 1\n"
        "builtin plane\n"
        "end\n"
        "entity\n"
        'name "spinner"\n'
        "pos 0 0 0\nrot 0 0 0\nscl 1 1 1\n"
        "builtin box\n"
        'script "_assets/spin.lua"\n'
        'param speed "2.0"\n'
        "end\n"
        "entity\n"
        'name "cam"\n'
        "pos 0 1.5 -6\nrot 0 0 0\nscl 1 1 1\n"
        "camera 0 60 0.1 500 1\n"
        "end\n",
        encoding="utf-8",
    )
    return pkg


def main() -> int:
    player = Path(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("--") \
        else REPO / "Darwin" / "Debug" / "PrimalPlayer"
    frames = 60
    if "--frames" in sys.argv:
        frames = int(sys.argv[sys.argv.index("--frames") + 1])
    if not player.exists():
        print(f"FAIL: player 不存在: {player}")
        return 1

    with tempfile.TemporaryDirectory(prefix="primal_e2e_") as tmp:
        pkg = build_package(Path(tmp))
        record = Path(tmp) / "record"
        record.mkdir()
        # 注意：包必须在 .Primal/runtime 两级下？不需要——包内相对优先分支
        # 不依赖项目根；即使三级上溯失败，_assets 命中即通过。
        proc = subprocess.run(
            [str(player), "--scene", str(pkg),
             "--record", str(record), "--record-frames", str(frames),
             "--record-metrics", str(record / "luma.csv")],
            capture_output=True, text=True, timeout=180,
            cwd=str(player.parent))
        log = proc.stderr
        # 已知引擎侧移交项（Docs/completed/2026-10-09-runtime-chain-*.md）：
        # RHIAllocator MetalRenderPass 泄漏 → 退出 SIGABRT(134)，但帧序列完整
        # （RuntimeLauncher 同一豁免契约）。真失败 = 非零退出且无该签名。
        known_leak_abort = proc.returncode in (-6, 134) and "RHIAllocator" in log
        checks = {
            f"player 退出(0 或已知 RHI 泄漏 abort, rc={proc.returncode})":
                proc.returncode == 0 or known_leak_abort,
            "builtin 几何生成日志": "builtin geometry 'plane' created" in log
                                  and "builtin geometry 'box' created" in log,
            "脚本挂载日志": "script attached" in log,
            "录制 PNG 数量": len(list(record.glob("frame_*.png"))) == frames,
            "亮度 CSV 存在": (record / "luma.csv").exists()
                            and sum(1 for _ in open(record / "luma.csv")) >= frames,
            "无 mesh/纹理失败": "ImportMeshAsync rejected" not in log
                              and "texture import failed" not in log,
            "无脚本执行错误": "script error" not in log.lower()
                            and "script attach failed" not in log,
        }
        ok = True
        for name, passed in checks.items():
            print(f"{'PASS' if passed else 'FAIL'}: {name}")
            ok = ok and passed
        if not ok:
            print("--- stderr tail ---")
            print("\n".join(log.splitlines()[-40:]))
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
