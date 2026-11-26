#!/usr/bin/env python3
"""Offline audit of native HLSL against the Vulkan inputs in shaders.json.

Compile both sides with the same DXC and compare shader interfaces as seen by
D3D12. Also count byte-identical programs. No renderer or GPU is required.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from build_d3d12_shaders import ROOT, catalog, compile_one, compiler, recipe, select


def reference_path(name):
    if name.startswith('EngineTest/'):
        return ROOT / name
    for directory in ('shaders', 'Shaders'):
        path = ROOT / 'Engine/Graphics/Vulkan' / directory / name
        if path.is_file():
            return path
    raise FileNotFoundError(name)


def flatten(path, base):
    # Mirror build_spv.sh's include lookup order (shader root, then includer).
    code = path.read_text(encoding='utf-8')
    def include(match):
        name = match[1]
        candidate = base / name
        return flatten(candidate if candidate.is_file() else path.parent / name, base)
    return re.sub(r'^#include\s*"([^"]+)"[^\n]*', include, code, flags=re.MULTILINE)


def build_reference(e, tmp):
    source = reference_path(e['vulkan'])
    spv, hlsl, cso = (tmp / ('reference.' + ext) for ext in ('spv', 'hlsl', 'cso'))
    cross = [os.getenv('D3D12_SPIRV_CROSS', 'spirv-cross')]
    if source.suffix == '.spv':
        cross += [str(source), '--entry', e['entry']]
    else:
        stage = source.suffix[1:]
        glsl = tmp / ('source.' + stage)
        glsl.write_bytes(flatten(source, source.parent).encode())
        defines = e.get('vulkan_defines', e.get('defines', []))
        result = subprocess.run([os.getenv('D3D12_GLSLANG', 'glslangValidator'), '--quiet',
                                 '-V', *['-D' + v for v in defines], '-S', stage,
                                 '--source-entrypoint', e['entry'], '-e', 'main', str(glsl),
                                 '-o', str(spv)], capture_output=True, text=True)
        if result.returncode:
            raise ValueError(result.stdout + result.stderr)
        cross.append(str(spv))
    if e.get('reference_temporaries'):
        # Keep the result of WGSL atomicExchange addressable even when unused.
        cross.append('--force-temporary')
    subprocess.run(cross + ['--hlsl', '--shader-model', '60', '--output', str(hlsl)], check=True)
    if e.get('reference_temporaries'):
        # Some SPIRV-Cross versions omit the declaration of a *discarded*
        # atomic result. Supply a local only if this is its sole occurrence.
        code = hlsl.read_text(encoding='utf-8')
        atomic = re.compile(r'^([ ]*)(\w+\.InterlockedExchange\([^;]+, (_\d+)\);)$', re.MULTILINE)
        def declare_unused(match):
            if len(re.findall(r'\b' + match[3] + r'\b', code)) == 1:
                return f'{match[1]}uint {match[3]};\n' + match[0]
            return match[0]
        hlsl.write_bytes(atomic.sub(declare_unused, code).encode())
    subprocess.run([compiler(), '-T', e['profile'], '-E', 'main', str(hlsl), '-Fo', str(cso)],
                   check=True)
    return cso


def interface(path):
    dump = subprocess.check_output([compiler(), '-dumpbin', str(path)], text=True)
    header, separator, _ = dump.partition('target datalayout')
    if not separator or '; Resource Bindings:' not in header or ';PSVRuntimeInfo:' not in header:
        raise ValueError(f'{path}: unrecognized DXC dump')
    # Compare the signatures including mask, component type, and interpolation.
    signatures = []
    for table in re.finditer(r'^; (Input|Output) signature:\n;\n((?:; (?!shader hash:)[^\n]+\n)*)',
                             header, re.MULTILINE):
        signatures.append((table[1], table[2].splitlines()))
    shape = re.findall(r'; NumThreads=\(([^)]*)\)', header)
    resource_table = header.split('; Resource Bindings:', 1)[1].split('\n;\n;\n', 1)[0]
    bindings = {}
    for row in resource_table.splitlines():
        cols = re.sub(r'\b(UAV|texture)(unorm_|snorm_)', r'\1 \2', row.strip('; ')).split()
        if not cols or cols[0] == 'Name' or cols[0].startswith('---'):
            continue
        if len(cols) != 7 or not re.fullmatch(r'(cb|[tus])\d+(,space\d+)?', cols[-2]):
            raise ValueError(f'unrecognized binding row: {row}')
        name, kind, fmt, dim, _, slot, count = cols
        fields = []
        if kind == 'cbuffer':
            block = re.search(r'^; cbuffer ' + re.escape(name) + r'\n(.*?)^; \}', header,
                              re.MULTILINE | re.DOTALL)
            if not block:
                raise ValueError(f'missing buffer definition for {name}')
            for declaration, offset, size in re.findall(
                    r'^;\s+([^;\n]+);\s*; Offset:\s*(\d+)(?: Size:\s*(\d+))?',
                    block[1], re.MULTILINE):
                # Field names may differ between source languages. Keep the
                # type, matrix orientation, array extent, offset and total size.
                type_shape = re.sub(r'\b\w+(?=(?:\[\d+\])*\s*$)', '', declaration).strip()
                if type_shape != '}' or size or offset != '0':
                    fields.append((type_shape, offset, size))
            if not fields:
                raise ValueError(f'empty reflected buffer definition for {name}')
        bindings[slot] = (kind, fmt, dim, count, fields)
    return dict(signatures=signatures, threads=shape, bindings=bindings)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shaders', nargs='*', help='optional output names or Vulkan path[:entry]')
    entries = select(catalog(), parser.parse_args().shaders)
    identical = 0
    failures = []
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        for e in entries:
            try:
                native = tmp / 'native.cso'
                compile_one(recipe(e), native)
                ref = build_reference(e, tmp)
                if native.read_bytes() == ref.read_bytes():
                    identical += 1
                    continue
                actual, expected = interface(native), interface(ref)
                for section in expected:
                    if actual[section] != expected[section]:
                        failures.append(f'{e["out"]}: {section}\n  HLSL {actual[section]}'
                                        f'\n  Vulkan {expected[section]}')
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                failures.append(f'{e["out"]}: {error}')
    if failures:
        sys.exit('\n'.join(failures))
    print(f'{len(entries)} shader interfaces match; {identical} CSOs byte-identical.')


if __name__ == '__main__':
    main()
