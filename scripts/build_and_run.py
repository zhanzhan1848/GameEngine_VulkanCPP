#!/usr/bin/env python3
"""
build_and_run.py — Cross-platform smart build + run for the Primal editor.

  1. Compute MD5 of all source files under Engine/, ContentTools/, EngineDLL/.
  2. Compare with the last stored baseline (.build_state/source.md5).
  3. If sources changed (or baseline missing, or any expected build artifact
     missing) → invoke CMake to rebuild Engine, ContentTools, EngineDLL.
  4. Build and launch the Avalonia editor (through LaunchServices on macOS).

Pure Python 3.8+; only requires `cmake` and `dotnet` on PATH.

Usage:
  python3 scripts/build_and_run.py                # default config
  python3 scripts/build_and_run.py --config ReleaseEditor
  python3 scripts/build_and_run.py --force-rebuild
  python3 scripts/build_and_run.py --skip-run     # only build
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent

SOURCE_ROOTS = ["Engine", "ContentTools", "EngineDLL"]

SOURCE_EXTS = {
    ".cpp", ".c", ".cc", ".cxx",
    ".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp",
    ".m", ".mm",
    ".metal",
    ".glsl", ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
}

EXCLUDE_DIRS = {
    "build-wt", "_deps", ".git", ".vs", ".vscode", ".idea",
    "obj", "bin", "node_modules",
    "Darwin",
    "third_party", "pmp-library",
}

# 独立构建树（Ninja + DebugEditor）：编辑器原生库需要 DebugEditor 配置
# （csproj 从 Darwin/DebugEditor/ 取 libEngineDLL.dylib 等产物），单配置生成器
# 下与测试用的 Darwin/Debug、Darwin/DebugVulkan 无法共用目录。
BUILD_DIR = REPO_ROOT / "build-wt"
BUILD_TARGETS = ["Engine", "ContentTools", "EngineDLL"]

# Native libs end up at Darwin/<Project>/<Config>/<artifact>{.dylib|.so|.dll}
ARTIFACT_PROJECT = "PrimalEditor_Avalonia"
ARTIFACTS = ["libPrimalEngine", "libContentTools", "libEngineDLL", "PrimalEngine"]
ARTIFACT_SUFFIXES = [".dylib", ".so", ".dll"]

DOTNET_PROJECT = REPO_ROOT / "PrimalEditor_Avalonia" / "PrimalEditor_Avalonia.csproj"

STATE_DIR = REPO_ROOT / ".build_state"
SOURCE_HASH_FILE = STATE_DIR / "source.md5"


def log(tag: str, msg: str) -> None:
    print(f"[{tag}] {msg}", flush=True)


def md5_of_file(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def is_excluded_dir(rel: Path) -> bool:
    return bool(set(rel.parts) & EXCLUDE_DIRS)


def collect_source_files() -> list[Path]:
    files: list[Path] = []
    for root_name in SOURCE_ROOTS:
        root = REPO_ROOT / root_name
        if not root.exists():
            log("warn", f"source root missing: {root_name}")
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() not in SOURCE_EXTS:
                continue
            if is_excluded_dir(path.relative_to(REPO_ROOT)):
                continue
            files.append(path)
    files.sort()
    return files


def compute_source_hash() -> tuple[str, int]:
    """MD5 over (relative path, per-file MD5) so renames also invalidate."""
    files = collect_source_files()
    if not files:
        log("warn", "no source files found — is this the right repo root?")
    combined = hashlib.md5()
    for f in files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        combined.update(rel.encode("utf-8"))
        try:
            combined.update(md5_of_file(f).encode("ascii"))
        except OSError as e:
            log("warn", f"could not read {rel}: {e}")
    return combined.hexdigest(), len(files)


def load_baseline_hash() -> str | None:
    if not SOURCE_HASH_FILE.exists():
        return None
    # File format: "<md5> <count>" — first token is the hash, count is informational.
    text = SOURCE_HASH_FILE.read_text(encoding="utf-8").strip()
    return text.split()[0] if text else None


def save_baseline_hash(h: str, count: int) -> None:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    SOURCE_HASH_FILE.write_text(f"{h} {count}\n", encoding="utf-8")


def expected_artifacts(config: str) -> list[Path]:
    base = REPO_ROOT / "Darwin" / ARTIFACT_PROJECT / config
    return [base / f"{stem}{suf}" for stem in ARTIFACTS for suf in ARTIFACT_SUFFIXES]


def artifacts_present(config: str) -> tuple[bool, list[Path]]:
    paths = expected_artifacts(config)
    primary_stems = ["libPrimalEngine", "libContentTools", "libEngineDLL"]
    missing = [
        stem for stem in primary_stems
        if not any(p.name.startswith(stem) and p.exists() for p in paths)
    ]
    return (not missing, [p for p in paths if not p.exists()])


def run_cmake_build(config: str) -> None:
    # 无条件 configure（2026-10-12）：Engine 的源文件用 file(GLOB_RECURSE)
    # 收集，GLOB 结果在 configure 期固化——只新增源文件、不动 CMakeLists 时，
    # 生成器内置的"CMakeLists 变更自动重跑"规则不会触发，新文件静默漏编。
    # 每次先跑一遍 configure 幂等且便宜（~1s）：未变化的构建脚本 → Ninja 按
    # 时间戳/命令行比对，零重编译。注意参数必须每次完全一致（同 -G、同
    # CMAKE_BUILD_TYPE），否则会全量重编。
    cfg_cmd = [
        "cmake", "-S", str(REPO_ROOT), "-B", str(BUILD_DIR),
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=" + config,
    ]
    log("build", f"$ {' '.join(cfg_cmd)}")
    rc = subprocess.call(cfg_cmd, cwd=str(REPO_ROOT))
    if rc != 0:
        log("error", f"CMake configure failed with exit code {rc}")
        sys.exit(rc)
    # `--config` is ignored on single-config generators (Makefile/Ninja) and
    # honored by multi-config generators (MSBuild/Xcode) — safe to always pass.
    cmd = [
        "cmake", "--build", str(BUILD_DIR),
        "--target", *BUILD_TARGETS,
        "--config", config,
        "--",
        "-j", str(os.cpu_count() or 4),
    ]
    log("build", f"$ {' '.join(cmd)}")
    rc = subprocess.call(cmd, cwd=str(REPO_ROOT))
    if rc != 0:
        log("error", f"CMake build failed with exit code {rc}")
        sys.exit(rc)


def run_editor(config: str) -> None:
    if not DOTNET_PROJECT.exists():
        log("error", f"project not found: {DOTNET_PROJECT}")
        sys.exit(1)

    # Avalonia.Native needs a foreground Aqua application session on macOS.
    # Launching the apphost via `dotnet run` leaves it as a Background process;
    # CoreVideo can then fail to create Avalonia's RenderTimer (-6661), which
    # surfaces as a managed exception followed by SIGABRT in libcoreclr.
    # The build target creates PrimalEditor.app, so hand that bundle to
    # LaunchServices instead.
    if sys.platform == "darwin":
        build_cmd = ["dotnet", "build", str(DOTNET_PROJECT), "-c", config]
        log("run", f"$ {' '.join(build_cmd)}")
        rc = subprocess.call(build_cmd, cwd=str(REPO_ROOT))
        if rc != 0:
            log("error", f"`dotnet build` exited with {rc}")
            sys.exit(rc)

        app_bundle = (
            REPO_ROOT / "Darwin" / "PrimalEditor_Avalonia" /
            config / "PrimalEditor.app"
        )
        if not app_bundle.exists():
            log("error", f"app bundle not found after build: {app_bundle}")
            sys.exit(1)

        cmd = ["open", str(app_bundle)]
        log("run", f"$ {' '.join(cmd)}")
        rc = subprocess.call(cmd, cwd=str(REPO_ROOT))
        if rc != 0:
            log("error", f"`open` exited with {rc}")
            sys.exit(rc)
        log("done", "editor launch requested through macOS LaunchServices")
        return

    cmd = ["dotnet", "run", "--project", str(DOTNET_PROJECT), "-c", config]
    log("run", f"$ {' '.join(cmd)}")
    rc = subprocess.call(cmd, cwd=str(REPO_ROOT))
    if rc != 0:
        log("error", f"`dotnet run` exited with {rc}")
        sys.exit(rc)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--config", default="DebugEditor",
                        help="Avalonia build configuration (default: DebugEditor)")
    parser.add_argument("--force-rebuild", action="store_true",
                        help="always rebuild, ignore the source-hash baseline")
    parser.add_argument("--skip-build", action="store_true",
                        help="skip CMake build, only build/launch the editor")
    parser.add_argument("--skip-run", action="store_true",
                        help="only build, do not launch the editor")
    args = parser.parse_args()

    log("init", f"repo root: {REPO_ROOT}")
    log("init", f"config:    {args.config}")

    needs_build = args.force_rebuild

    if args.skip_build:
        log("init", "skip-build requested; trusting existing artifacts")
    else:
        current_hash, file_count = compute_source_hash()
        log("init", f"scanned {file_count} source files under {', '.join(SOURCE_ROOTS)}")
        log("init", f"current source MD5: {current_hash}")

        baseline = load_baseline_hash()
        if baseline is None:
            log("init", "no baseline found — first run, will build")
            needs_build = True
        elif baseline == current_hash:
            log("init", "source hash matches baseline — sources unchanged")
        else:
            log("init", f"source hash changed (was {baseline}) — rebuild required")
            needs_build = True

        if not needs_build:
            ok, missing = artifacts_present(args.config)
            if not ok:
                log("init", f"build artifacts missing for stems: "
                            f"{[p.stem for p in missing]}")
                needs_build = True
            else:
                log("init", "all expected build artifacts present")

    if needs_build:
        run_cmake_build(args.config)
        if not args.skip_build:
            current_hash, file_count = compute_source_hash()
            save_baseline_hash(current_hash, file_count)
            log("init", f"saved source baseline ({current_hash})")
    else:
        log("init", "no build needed")

    if args.skip_run:
        log("done", "build phase complete (skip-run)")
        return 0

    run_editor(args.config)
    return 0


if __name__ == "__main__":
    sys.exit(main())
