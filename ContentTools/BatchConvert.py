#!/usr/bin/env python3
"""
BatchConvert.py - Batch convert FBX assets to .engine_mesh format.

Pipeline (per file):
  <input>.fbx
    -> ContentToolsCLI (C++)      -> <temp>.bin
    -> pack_geometry.py (Python)  -> <output>.engine_mesh

Layout:
  assets/Raw/<pack_name>/*.fbx          (input)
  assets/Processed/<pack_name>/*.engine_mesh  (output)

Usage:
  # Convert everything under assets/Raw/
  python3 ContentTools/BatchConvert.py

  # Convert a single pack
  python3 ContentTools/BatchConvert.py --pack kenney_dungeon_tiles

  # Custom input/output roots
  python3 ContentTools/BatchConvert.py --input /path/to/raw --output /path/to/out

  # Dry-run (list what would be converted)
  python3 ContentTools/BatchConvert.py --dry-run
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import Optional

# Project root is the parent of ContentTools/
PROJECT_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_INPUT_ROOT = PROJECT_ROOT / "EngineTest" / "assets" / "Raw"
DEFAULT_OUTPUT_ROOT = PROJECT_ROOT / "EngineTest" / "assets" / "Processed"
PACK_GEOMETRY = PROJECT_ROOT / "ContentTools" / "pack_geometry.py"

# Search order for the ContentToolsCLI binary (Release preferred).
CLI_CANDIDATES = [
    PROJECT_ROOT / "Darwin" / "Release" / "ContentToolsCLI",
    PROJECT_ROOT / "Darwin" / "Debug" / "ContentToolsCLI",
    PROJECT_ROOT / "build" / "ContentTools" / "ContentToolsCLI",
]


def find_cli() -> Path:
    """Locate the ContentToolsCLI executable."""
    for candidate in CLI_CANDIDATES:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate
    searched = "\n  ".join(str(c) for c in CLI_CANDIDATES)
    raise FileNotFoundError(f"ContentToolsCLI not found. Searched:\n  {searched}")


def find_fbx_files(input_root: Path) -> list[Path]:
    """All .fbx files (case-insensitive) under input_root, sorted."""
    return sorted(
        p for p in input_root.rglob("*")
        if p.is_file() and p.suffix.lower() == ".fbx"
    )


def convert_one(cli: Path, fbx: Path, out_engine_mesh: Path,
                keep_bin: bool = False, verbose: bool = False) -> bool:
    """Run the 2-stage conversion for a single FBX. Returns True on success."""
    out_bin = out_engine_mesh.with_suffix(".bin")

    # Stage 1: FBX -> .bin
    if verbose:
        print(f"  [1/2] ContentToolsCLI {fbx.name} -> {out_bin.name}")
    r1 = subprocess.run(
        [str(cli), str(fbx), str(out_bin)],
        capture_output=True, text=True, check=False,
    )
    if r1.returncode != 0 or not out_bin.exists():
        print(f"  FAIL stage 1 (ContentToolsCLI)")
        if r1.stdout.strip():
            print(f"       stdout: {r1.stdout.strip()[:400]}")
        if r1.stderr.strip():
            print(f"       stderr: {r1.stderr.strip()[:400]}")
        return False

    # Stage 2: .bin -> .engine_mesh
    if verbose:
        print(f"  [2/2] pack_geometry.py {out_bin.name} -> {out_engine_mesh.name}")
    r2 = subprocess.run(
        [sys.executable, str(PACK_GEOMETRY), str(out_bin), str(out_engine_mesh)],
        capture_output=True, text=True, check=False,
    )
    if r2.returncode != 0 or not out_engine_mesh.exists():
        print(f"  FAIL stage 2 (pack_geometry.py)")
        if r2.stdout.strip():
            print(f"       stdout: {r2.stdout.strip()[:400]}")
        if r2.stderr.strip():
            print(f"       stderr: {r2.stderr.strip()[:400]}")
        return False

    if not keep_bin:
        try:
            out_bin.unlink()
        except OSError:
            pass

    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", type=Path, default=DEFAULT_INPUT_ROOT,
                    help=f"Input root (default: {DEFAULT_INPUT_ROOT})")
    ap.add_argument("--output", type=Path, default=DEFAULT_OUTPUT_ROOT,
                    help=f"Output root (default: {DEFAULT_OUTPUT_ROOT})")
    ap.add_argument("--pack", type=str, default=None,
                    help="Restrict to a single pack subdir under input/output root")
    ap.add_argument("--keep-bin", action="store_true",
                    help="Keep intermediate .bin file (for debugging)")
    ap.add_argument("--dry-run", action="store_true",
                    help="List candidates without converting")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="Echo each subprocess command")
    args = ap.parse_args()

    # Locate binaries/scripts
    try:
        cli = find_cli()
    except FileNotFoundError as e:
        print(f"ERROR: {e}")
        return 1
    if not PACK_GEOMETRY.exists():
        print(f"ERROR: pack_geometry.py not found at {PACK_GEOMETRY}")
        return 1
    print(f"CLI:        {cli}")
    print(f"pack_geom:  {PACK_GEOMETRY}")

    # Resolve I/O roots
    in_root = args.input.resolve()
    out_root = args.output.resolve()
    if args.pack:
        in_root = in_root / args.pack
        out_root = out_root / args.pack

    if not in_root.exists():
        print(f"ERROR: input directory does not exist: {in_root}")
        print(f"       Create it and place .fbx files inside, or pass --input.")
        return 1

    fbxs = find_fbx_files(in_root)
    if not fbxs:
        print(f"No .fbx files under {in_root}")
        return 0

    print(f"\nFound {len(fbxs)} FBX file(s) under {in_root}")
    if args.dry_run:
        for f in fbxs:
            rel = f.relative_to(in_root)
            out = out_root / rel.with_suffix(".engine_mesh")
            print(f"  {rel}  ->  {out}")
        return 0

    # Convert
    ok = 0
    fail = 0
    failed_files: list[Path] = []
    for i, fbx in enumerate(fbxs, 1):
        rel = fbx.relative_to(in_root)
        out_path = out_root / rel.with_suffix(".engine_mesh")
        out_path.parent.mkdir(parents=True, exist_ok=True)

        print(f"\n[{i}/{len(fbxs)}] {rel}")
        if convert_one(cli, fbx, out_path, keep_bin=args.keep_bin, verbose=args.verbose):
            try:
                display = out_path.relative_to(PROJECT_ROOT)
            except ValueError:
                display = out_path
            print(f"  OK  -> {display}")
            ok += 1
        else:
            fail += 1
            failed_files.append(rel)

    # Summary
    print("\n" + "=" * 60)
    print(f"Done: {ok} ok, {fail} failed, {len(fbxs)} total")
    if failed_files:
        print("Failed files:")
        for f in failed_files:
            print(f"  - {f}")
    return 0 if fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
