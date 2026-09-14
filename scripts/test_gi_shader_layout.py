#!/usr/bin/env python3
"""Check compiled Vulkan GI UBO layouts against the C++ upload contract.

Requires glslangValidator, spirv-dis and spirv-val. Builds in a temporary
folder; it does not modify deployed shaders.
"""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "Engine/Graphics/Vulkan/shaders/Lumen"


def check(shader, entry, contracts):
    with tempfile.TemporaryDirectory(prefix="gi-layout-") as tmp:
        output = Path(tmp) / "shader.spv"
        subprocess.run(["glslangValidator", "--quiet", "-V", "--source-entrypoint",
                        "main", "-e", entry, str(SHADERS / shader), "-o", str(output)], check=True)
        subprocess.run(["spirv-val", str(output)], check=True)
        asm = subprocess.check_output(["spirv-dis", str(output)], text=True)
        for block, expected in contracts.items():
            members = dict(re.findall(r'OpMemberName %' + block + r' (\d+) "([^"\n]+)"', asm))
            offsets = dict(re.findall(r'OpMemberDecorate %' + block + r' (\d+) Offset (\d+)', asm))
            actual = {name: int(offsets[index]) for index, name in members.items()}
            assert actual == expected, (shader, block, actual, expected)
        print(f"PASS {shader}: C++/SPIR-V constant offsets")


if __name__ == "__main__":
    # GIGatherCB: binding4 at byte0, binding6 at byte80. Camera byte96
    # must therefore be member offset16 of binding6, never byte64 of binding4.
    check("DDGIGIGather.comp", "ddgi_gi_gather", {
        "ViewCB": {"viewRows": 0},
        "ProbeCounts": {"probeCounts": 0, "cameraPos": 16, "debugVis": 32},
    })
    # ScreenProbeGlobalData has C++ static_asserts for 352/384/400.
    check("ScreenProbePlace.comp", "screen_probe_place", {
        "GlobalBlock": {
            "view_projection": 0, "inv_view_projection": 64, "camera_position": 128,
            "grid_params": 144, "trace_params": 160, "sdf_origins": 176,
            "sdf_voxel_sizes": 224, "sdf_extents": 272, "sdf_resolutions": 320,
            "surface_cache_params": 336, "ddgi_fb": 352, "hzb_params": 384,
            "cache_params": 400,
        },
    })
