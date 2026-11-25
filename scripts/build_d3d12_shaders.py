#!/usr/bin/env python3
"""Compile the renderer's HLSL sources to DXIL with DXC.

The catalog keeps the output names used by the RHI, including one CSO per
stage/entry of multi-entry Vulkan modules. Vulkan references are for offline
comparison only. Use --check to verify an existing build without a compiler. Run compare_d3d12_shaders.py for an optional Vulkan reference check.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'Engine/Graphics/RHI/Platforms/D3D12/Shaders'
DEFAULT_OUT = ROOT / 'EngineTest/shaders/d3d12'
MANIFEST = 'manifest.json'
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def catalog():
    entries = json.loads((SOURCES / 'shaders.json').read_text(encoding='utf-8'))
    if len({e['out'] for e in entries}) != len(entries):
        raise ValueError('duplicate CSO output name in shaders.json')
    return entries


def select(entries, names):
    if not names:
        return entries
    chosen = {}
    for name in names:
        # Preserve the old path[:source-entry] syntax as well as exact CSO names.
        key, _, entry = name.partition(':')
        matches = [e for e in entries if key in (e['out'], e['vulkan'])
                   and (not entry or e['entry'] == entry)]
        if not matches:
            raise ValueError(f'unknown shader {name}')
        chosen.update((e['out'], e) for e in matches)
    return list(chosen.values())


def recipe(entry):
    source = entry.get('source', entry['out'] + '.hlsl')
    inputs = {}

    def collect(path):
        key = path.resolve().relative_to(SOURCES).as_posix()
        if key in inputs:
            return
        code = path.read_bytes().replace(b'\r\n', b'\n')
        inputs[key] = digest(code)
        for include in INCLUDE.findall(code.decode('utf-8-sig')):
            collect(path.parent / include)

    collect(SOURCES / source)
    return dict(source=source, inputs=inputs, language='2021', profile=entry['profile'],
                entry=entry.get('hlsl_entry', 'main'), defines=entry.get('defines', []),
                cso=entry['out'] + '.cso')


def compiler():
    return os.environ.get('D3D12_DXC') or shutil.which('dxc') or 'dxc'


def compile_one(record, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler(), '-HV', record['language'], '-T', record['profile'], '-E', record['entry'],
                    *['-D' + d for d in record['defines']], str(SOURCES / record['source']),
                    '-Fo', str(target)], check=True)


def load_manifest(path):
    return {r['cso']: r for r in json.loads(path.read_text(encoding='utf-8'))['shaders']}


def compiler_version():
    return subprocess.check_output([compiler(), '--version'], text=True).strip()


def build(entries, out, partial):
    out.mkdir(parents=True, exist_ok=True)
    manifest = out / MANIFEST
    prior = load_manifest(manifest) if partial and manifest.exists() else {}
    # Publish only after all selected sources have compiled successfully.
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        stage = Path(tmp)
        for entry in entries:
            r = recipe(entry)
            target = stage / r['cso']
            compile_one(r, target)
            r['cso_sha256'] = digest(target.read_bytes())
            prior[r['cso']] = r
        data = dict(manifest_version=2, generator=Path(__file__).name, tools={'dxc': compiler_version()},
                    shaders=list(prior.values()))
        (stage / MANIFEST).write_bytes((json.dumps(data, indent=2) + '\n').encode())
        for entry in entries:
            name = entry['out'] + '.cso'
            (out / name).parent.mkdir(parents=True, exist_ok=True)
            (stage / name).replace(out / name)
        (stage / MANIFEST).replace(manifest)
    print(f'Compiled {len(entries)} native HLSL shaders to {out}')


def check(entries, out, partial):
    records = load_manifest(out / MANIFEST)
    stale = []
    for entry in entries:
        r = recipe(entry)
        cso = r['cso']
        actual = records.get(cso, {})
        path = out / cso
        if any(actual.get(k) != v for k, v in r.items()):
            stale.append(f'{cso}: source, include or compile recipe changed')
        elif not path.is_file() or actual.get('cso_sha256') != digest(path.read_bytes()):
            stale.append(f'{cso}: compiled product missing or changed')
    if not partial:
        stale.extend(f'{extra}: obsolete manifest entry' for extra in
                     sorted(set(records) - {e['out'] + '.cso' for e in entries}))
    if stale:
        raise ValueError('\n'.join(stale))
    print(f'Freshness check passed for {len(entries)} native HLSL shaders')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true')
    mode.add_argument('--if-stale', action='store_true',
                      help='reuse products matching the current compiler, sources and recipes')
    parser.add_argument('--out-dir', type=Path, default=DEFAULT_OUT)
    parser.add_argument('shaders', nargs='*', help='output name or Vulkan path[:entry]')
    args = parser.parse_args()
    entries = select(catalog(), args.shaders)
    if args.if_stale:
        try:
            prev = json.loads((args.out_dir / MANIFEST).read_text(encoding='utf-8'))
            if prev.get('tools', {}).get('dxc') == compiler_version():
                check(entries, args.out_dir, bool(args.shaders))
                return
        except (FileNotFoundError, ValueError, KeyError):
            pass  # Missing or stale products are rebuilt below.
    (check if args.check else build)(entries, args.out_dir, bool(args.shaders))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
