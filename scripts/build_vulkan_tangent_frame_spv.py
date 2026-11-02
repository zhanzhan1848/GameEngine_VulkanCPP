#!/usr/bin/env python3
"""Rebuild the opt-in Vulkan tangent-frame variants; never modify defaults.

Requires naga and spirv-val. Refuses to write if current source does not
reproduce the shipped baseline or if output differs from the tested witness.
"""
import hashlib
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'Engine/Graphics/Vulkan/shaders/Nanite'
CASES = (
    (ROOT / 'Engine/Graphics/Dawn/shaders/Nanite/GPUDrivenDraw.wgsl', 'GPUDrivenDraw.spv',
     'GPUDrivenDrawTangentFrame.wgsl', 'GPUDrivenDrawTangentFrame.spv',
     '8e4c1f2bc6939d7cd62c6aad7c5a44cf65a06f60657946e5ab1de647454a826d'),
    (DEST / 'GPUDrivenDrawAlbedoMips.wgsl', 'GPUDrivenDrawAlbedoMips.frag.spv',
     'GPUDrivenDrawTangentFrameAlbedoMips.wgsl', 'GPUDrivenDrawTangentFrameAlbedoMips.frag.spv',
     '5c65100d6819f63c297710e62d28f39151547e9f8ca8cc7b96ecb2d60588d1af'),
)
HELPER = '''// Qualified generated unit tangents can quantize just outside the XY disk.
// Preserve their in-plane direction instead of inventing an unrelated +Z axis.
fn unpack_tangent(packed: u32, color_t_sign: u32) -> vec3<f32> {
    let f = vec2<f32>(f32(packed >> 16u), f32(packed & 65535u)) * (2.0 / 65535.0) - vec2<f32>(1.0);
    if (dot(f, f) > 1.0) { return normalize(vec3<f32>(f, 0.0)); }
    return unpack_normal(packed, color_t_sign >> 1u);
}

'''
def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError(f'Expected exactly one occurrence: {old!r}')
    return source.replace(old, new)

def transform(source):
    source = replace_once(source, 'let tangent = unpack_normal(element.tangent, element.color_t_sign);',
                          'let tangent = unpack_tangent(element.tangent, element.color_t_sign);')
    source = replace_once(source, '(element.color_t_sign & 0xFF000000u) != 0u);',
                          '(element.color_t_sign & 0x01000000u) != 0u);')
    source = replace_once(source, '    let B = normalize(cross(N, T));',
                          '''    // Preserve the authored-UV frame handedness when orienting N to the visible side.
    let frame_sign = select(-1.0, 1.0, dot(cross(normalize(in.normal), T), in.bitangent) >= 0.0);
    let B = normalize(cross(N, T)) * frame_sign;''')
    token = '// Vertex shader output. Interpolated across the triangle.'
    return replace_once(source, token, HELPER + token)

def compile_shader(source, name, folder):
    src = folder / (name + '.wgsl')
    spv = folder / (name + '.spv')
    src.write_text(source)
    subprocess.run(['naga', str(src), str(spv)], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(spv)], check=True)
    return spv.read_bytes()

def main():
    products = []
    with tempfile.TemporaryDirectory(prefix='primal-tangent-frame-') as td:
        folder = Path(td)
        for number, (source_path, baseline_name, output_source, output_spv, expected_sha) in enumerate(CASES):
            source = source_path.read_text()
            baseline = compile_shader(source, f'baseline{number}', folder)
            if baseline != (DEST / baseline_name).read_bytes():
                raise RuntimeError(f'{source_path} does not reproduce {baseline_name}; no files written')
            variant = transform(source)
            binary = compile_shader(variant, f'variant{number}', folder)
            digest = hashlib.sha256(binary).hexdigest()
            if digest != expected_sha:
                raise RuntimeError(f'{output_spv}: SHA {digest} != tested {expected_sha}; no files written')
            products.append((output_source, output_spv, variant, binary, digest))
    for wgsl, spv, source, binary, digest in products:
        (DEST / wgsl).write_text(source)
        (DEST / spv).write_bytes(binary)
        print(f'{spv}: {digest}; production baseline byte-identical')

if __name__ == '__main__':
    main()
