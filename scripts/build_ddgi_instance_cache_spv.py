#!/usr/bin/env python3
"""Compile and validate the three instance normal cache shaders."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

shaders = Path(__file__).resolve().parents[1] / "Engine/Graphics/Vulkan/shaders"
lumen = shaders / "Lumen"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path, default=lumen,
                    help="output directory (defaults to the runtime shader directory)")
output = parser.parse_args().output_dir
output.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="instance-normal-", dir=output) as tmp:
    modules = []
    for name in ("DDGIInstanceNormalProduce", "DDGIInstanceNormalVerify", "DDGIGIGather_instance_cache"):
        module = Path(tmp) / (name + ".comp.spv")
        source = lumen / (name + ".comp")
        flags = []
        if name == "DDGIGIGather_instance_cache":
            source = lumen / "DDGIGIGather.comp"
            flags = ["-DDDGI_GEOMETRY_CONNECTIONS", "-DDDGI_GEOMETRY_REPLACE",
                     "-DDDGI_GEOMETRY_ANY_HIT", "-DDDGI_GEOMETRY_QUERY_KERNEL",
                     "-DDDGI_INSTANCE_NORMAL_CACHE", "-Dddgi_gi_gather=main",
                     "--source-entrypoint", "main", "-e", "ddgi_gi_gather"]
        subprocess.run(["glslangValidator", "--quiet", "-V", "-I" + str(shaders),
                        *flags, str(source), "-o", str(module)], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(module)], check=True)
        modules.append(module)

    for module in modules:
        digest = hashlib.sha256(module.read_bytes()).hexdigest()
        module.replace(output / module.name)
        print(module.name, digest)
