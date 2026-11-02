#!/usr/bin/env python3
"""Build the opt-in Environment Trace pair or 8x4 workgroup module.

The historical workgroup script name is a symlink to this implementation.
"""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
name = Path(sys.argv[0]).name
if name == 'build_environment_pair_spv.py':
    shaders = root / 'Engine/Graphics/Vulkan/shaders/Lumen'
    output_name = 'EnvironmentVisibility_trace_pair.comp.spv'
    define = '-DENV_VIS_PAIR=1'
    prefix = 'primal-env-pair-'
    touch_if_unchanged = True
elif name == 'build_environment_trace_workgroup_spv.py':
    shaders = root / 'Engine/Graphics/Vulkan/Shaders/Lumen'
    output_name = 'EnvironmentVisibility_trace_wg32.comp.spv'
    define = '-DENV_TRACE_WG32=1'
    prefix = 'primal-env-wg32-'
    touch_if_unchanged = False
else:
    raise SystemExit(f'Unknown environment shader profile: {name}')

with tempfile.TemporaryDirectory(prefix=prefix) as tmp:
    output = Path(tmp) / output_name
    subprocess.run(['glslangValidator', '--quiet', '-V', '-I' + str(shaders), define,
                    '--source-entrypoint', 'main', '-e', 'main',
                    str(shaders / 'EnvironmentVisibility.comp'), '-o', str(output)], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(output)], check=True)
    data = output.read_bytes()
    dest = shaders / output_name
    if touch_if_unchanged and dest.exists() and dest.read_bytes() == data:
        dest.touch()
    else:
        dest.write_bytes(data)
    print(output_name, hashlib.sha256(data).hexdigest())
