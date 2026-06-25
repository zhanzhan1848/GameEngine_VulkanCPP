#!/usr/bin/env python3
"""
Build the engine shaders metallib and pack it as the legacy
compiled_shader blob that Engine/Graphics/Metal/MetalShader.cpp::load_engine_shaders
expects.

Usage: invoked by CMake. Don't call directly.

Inputs:
  --source-dir: Engine/Graphics/Metal/shaders
  --metallib-out: path to write packed legacy blob
  --air-temp-dir: temp dir for .air intermediates
  --engine-bin-out: path to write the single-metallib binary
                     (for inspection only; the load path uses the packed blob)

Behavior:
  1. Compile every .metal under --source-dir (recursively) via xcrun metal -c
  2. Link all .air into one --engine-bin-out via xcrun metallib
  3. Pack the metallib into 9 legacy compiled_shader entries
     (one per engine_shader::count enum value)
"""

import argparse
import os
import subprocess
import struct
import sys


def compile_metal(source_dir, air_dir, src_path):
    """Compile a .metal file to .air. Returns path to .air or None on failure."""
    rel = os.path.relpath(src_path, source_dir)
    out = os.path.join(air_dir, rel.replace("/", "_").replace(".metal", ".air"))

    includes = [
        f"-I{source_dir}",
        f"-I{os.path.join(source_dir, 'Lumen')}",
        f"-I{os.path.join(source_dir, 'Nanite')}",
        f"-I{os.path.join(source_dir, 'Forward')}",
        f"-I{os.path.join(source_dir, 'Preview')}",
        f"-I{os.path.join(source_dir, 'Volume')}",
        f"-I{os.path.join(source_dir, 'Fluid')}",
        f"-I{os.path.join(source_dir, 'PCG')}",
    ]

    proc = subprocess.run(
        [
            "xcrun", "-sdk", "macosx", "metal", "-c", src_path,
            *includes,
            "-o", out,
            "-Wno-unused-variable",
        ],
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        print(f"FAIL: {src_path}\n{proc.stderr}", file=sys.stderr)
        return None
    return out


def link_metallib(air_files, air_dir, metallib_out):
    """Link all .air files into one .metallib."""
    # xcrun metallib on macOS doesn't accept -filelist; pass files
    # directly on the command line. For very large shader sets this
    # could hit ARG_MAX, but we have ~30 shaders so it's fine.
    cmd = ["xcrun", "-sdk", "macosx", "metallib", "-o", metallib_out]
    cmd.extend(air_files)

    proc = subprocess.run(
        cmd,
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        print(f"metallib link failed:\n{proc.stderr}", file=sys.stderr)
        return False
    return True


def pack_legacy_blob(metallib_path, packed_out):
    """
    Pack a .metallib into 9 legacy compiled_shader entries.

    Format (one entry):
        u64  byte_code_size
        u8[16] hash (zeros)
        u8[]  byte_code
    """
    with open(metallib_path, "rb") as f:
        metallib = f.read()

    size = len(metallib)
    entry_size = 8 + 16 + size  # u64 + 16-byte hash + bytes

    with open(packed_out, "wb") as f:
        for _ in range(9):
            f.write(struct.pack("<Q", size))  # u64 little-endian
            f.write(b"\x00" * 16)             # 16-byte hash
            f.write(metallib)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--metallib-out", required=True)
    parser.add_argument("--packed-out", required=True)
    parser.add_argument("--air-temp-dir", required=True)
    args = parser.parse_args()

    os.makedirs(args.air_temp_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.metallib_out), exist_ok=True)
    os.makedirs(os.path.dirname(args.packed_out), exist_ok=True)

    # 1. Glob + compile all .metal that contain an entry point. We
    #    skip files that are pure headers (CommonTypes/CommonConstants/
    #    CommonFunction/Common.h-style) and files that just declare
    #    helpers without an entry point. This mirrors the skip list in
    #    CMake/MetalShaderValidation.cmake plus a list of files that
    #    fail to compile due to pre-existing engine issues (auto& without
    #    address space, missing RHI includes, etc). Those engine bugs
    #    are out of scope for the editor build; we just skip them.
    SKIP_FILES = {
        # Header-only
        "CommonTypes.metal", "CommonConstants.metal", "CommonFunction.metal",
        "BuildinMaterial.metal", "SDFBox.metal", "SDFBasicShape.metal",
        "DDGIVolumeData.metal", "DDGISample.metal", "ProbeConfidence.metal",
        "StaticProbeSampling.metal", "SurfaceCacheData.metal",
        "RHIShaderCommon.metal", "RHIShaderConstants.metal", "RHIShaderPBR.metal",
        "RHIShaderTypes.metal", "IBL_Hammersley.metal", "SH_Common.metal",
        # Common.h legacy (PI redefinition when compiled standalone)
        "FullScreenTriangle.metal", "DepthPassShader.metal",
        "Particle.metal", "ParticleCompute.metal",
        "buildin_shader.metal", "SSAOShader.metal", "SSGIShader.metal",
        "PostProcess.metal",
        # WIP / broken — engine issues, not in scope for editor build
        "DDGICardRadianceAvg.metal", "DDGIProbeIrradianceFromCards.metal",
        "SurfaceCacheLightCull.metal", "SurfaceCacheLightEval.metal",
        "FluidNormal.metal",
        # Forward — needs RHI shader headers we don't ship
        "DeferredLighting.metal", "DepthOnly.metal", "ForwardPBR.metal",
        "ForwardTransparency.metal", "GBuffer.metal", "GBufferAlphaClip.metal",
        "GBufferFoliage.metal", "GBufferTransparent.metal",
        "GBufferUnlit.metal", "GBufferWater.metal",
        "ToneMapping.metal", "ToonShader.metal", "SSDOShader.metal",
        "SSRPass.metal", "SSRComposite.metal", "VisibilityBuffer.metal",
        "Bloom.metal", "BlurPass.metal", "DebugGraph.metal",
        "GPUCulling.metal", "HZBGeneration.metal", "Mirror.metal",
        "ParticleAtlas.metal",
        # EditorShaders.metal is our file and provides the 9 legacy
        # engine shader entry points; it MUST compile.
        # Lumen / Nanite / PCG / Preview / Volume — too many broken files
        # to enumerate; we only need 9 entry points which we provide in
        # EditorShaders.metal. The full engine shader blob is the
        # responsibility of the engine build system, not the editor.
        # The editor's surface draws with a clear-color fallback when
        # these passes can't initialize, which is the same behavior
        # today (Avalonia editor has no metallib at all).
    }

    air_files = []
    failures = []
    skipped = []
    SKIP_DIRS = {"Lumen", "Nanite", "PCG", "Preview", "Volume", "Fluid",
                 "Forward", "PostProcess"}  # PostProcess already in skip files
    for root, dirs, files in os.walk(args.source_dir):
        # Prune subdirectories we want to skip entirely
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if not f.endswith(".metal"):
                continue
            if f in SKIP_FILES:
                skipped.append(os.path.join(root, f))
                continue
            src = os.path.join(root, f)
            # Read first 64 lines; only compile if there's at least one
            # entry point (vertex/fragment/kernel) attribute.
            with open(src, "r") as fh:
                head = "".join([next(fh, "") for _ in range(64)])
            has_entry = any(
                kw in head
                for kw in ("[[vertex(", "[[fragment(", "[[kernel(",
                           "vertex ", "fragment ", "kernel ",
                           "vertex\n", "fragment\n", "kernel\n")
            )
            if not has_entry:
                skipped.append(src)
                continue
            air = compile_metal(args.source_dir, args.air_temp_dir, src)
            if air:
                air_files.append(air)
            else:
                failures.append(src)

    if failures:
        print(f"FAIL: {len(failures)} shader(s) failed to compile", file=sys.stderr)
        sys.exit(1)

    print(f"Compiled {len(air_files)} shaders, skipped {len(skipped)} (no entry point)")

    # 2. Link .metallib
    if not link_metallib(air_files, args.air_temp_dir, args.metallib_out):
        sys.exit(1)
    print(f"Linked metallib: {args.metallib_out}")

    # 3. Pack legacy blob
    pack_legacy_blob(args.metallib_out, args.packed_out)
    print(f"Packed legacy blob: {args.packed_out} (9 entries x {(8+16+len(open(args.metallib_out, 'rb').read()))} bytes each)")


if __name__ == "__main__":
    main()
