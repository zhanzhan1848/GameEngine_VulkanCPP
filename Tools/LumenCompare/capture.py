#!/usr/bin/env python3
"""Sponza main-view comparison: prepare UE host, run Primal, inventory and archive."""
import argparse
import configparser
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import re

FRAMES = [1, 5, 35, 75, 150, 360]
ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(data, indent=2) + '\n')
    temporary.replace(path)


def json_sequence(path):
    """UE DumpGPU writes adjacent JSON objects, not a JSON array or JSONL."""
    text = path.read_text()
    decoder = json.JSONDecoder()
    offset = 0
    result = []
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset == len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        result.append(value)
    return result


def prepare(args):
    # Dedicated project: original Content is read through a symlink; generated
    # Saved/Intermediate/Config live only under the requested new host directory.
    host = args.output.resolve()
    host.mkdir(parents=True, exist_ok=False)
    source = args.project.resolve().parent
    (host / 'Content').symlink_to(source / 'Content', target_is_directory=True)
    shutil.copytree(args.plugin, host / 'Plugins/LumenCompareCapture')
    config = configparser.ConfigParser(strict=False, interpolation=None)
    config.optionxform = str
    config.read(source / 'Config/DefaultEngine.ini')
    keep = configparser.ConfigParser(interpolation=None)
    keep.optionxform = str
    for section in ['/Script/Engine.RendererSettings', '/Script/MacTargetPlatform.MacTargetSettings', '/Script/EngineSettings.GameMapsSettings']:
        if section in config:
            keep[section] = dict(config[section])
    (host / 'Config').mkdir()
    with (host / 'Config/DefaultEngine.ini').open('w') as stream:
        keep.write(stream)
    write(host / 'LumenCompareHost.uproject', {
        'FileVersion': 3, 'EngineAssociation': '5.7',
        'Plugins': [{'Name': 'LumenCompareCapture', 'Enabled': True}],
    })
    write(host / 'host-provenance.json', {
        'original_project': str(args.project.resolve()), 'content_symlink': str(source / 'Content'),
        'frames': FRAMES, 'scope': 'standalone main game viewport; no editor viewport warmup',
        'scene_contract': str(source / 'sponza-vulkan-reference.json'),
        'model_sha256': digest(ROOT / 'EngineTest/assets/Sponza.model'),
    })
    print(host / 'LumenCompareHost.uproject')


def primal(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    frames = [int(v) for v in args.frames.split(',')] if args.frames else FRAMES
    if not frames or any(f < 1 for f in frames) or frames != sorted(set(frames)):
        raise ValueError('frames must be increasing positive integers')
    prep_views = 0 if args.raw_startup else 4
    env = dict(os.environ, LUMEN_COMPARE_DIR=str(output), LUMEN_COMPARE_FRAMES=','.join(map(str, frames)), LUMEN_COMPARE_PREP_VIEWS=str(prep_views))
    if args.raw_startup:
        env['LUMEN_COMPARE_RAW_STARTUP'] = '1'
    if args.passes:
        env['LUMEN_COMPARE_PASSES'] = args.passes
    if args.staging_mib is not None:
        if not 64 <= args.staging_mib <= 4096:
            raise ValueError('observer staging budget must be 64..4096 MiB')
        env['LUMEN_COMPARE_STAGING_MIB'] = str(args.staging_mib)
    # Existing final-frame driver provides the locked camera/reference lighting.
    command = [sys.executable, str(ROOT / 'scripts/sponza_reference.py'), 'vulkan',
               '--frame', str(frames[-1] - 1 + prep_views), '--output', str(output / 'final'), '--timeout', str(args.timeout)]
    write(output / 'run.json', {'engine': 'primal', 'frames': frames, 'raw_startup': args.raw_startup,
          'ordinal_origin': 'first GI view after raster-only preparation and GI history recreation', 'raster_preparation_views': prep_views, 'command': command,
          'binary_sha256': digest(ROOT / 'Darwin/Debug/TestVulkanSponzaRenderGraph'),
          'model_sha256': digest(ROOT / 'EngineTest/assets/Sponza.model'),
          'observer': str(ROOT / 'Engine/Graphics/RenderGraph/LumenCompareCapture.h'),
          'observer_sha256': digest(ROOT / 'Engine/Graphics/RenderGraph/LumenCompareCapture.h'),
          'environment': {k: v for k, v in env.items() if k.startswith(('PRIMAL_', 'SC_', 'DDGI_', 'SPGI_', 'SPONZA_', 'LUMEN_COMPARE_', 'GI_'))}})
    with (output / 'driver.log').open('w') as log:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    summary = inventory(output, 'primal')
    write(output / 'inventory.json', summary)
    print(json.dumps({'returncode': result.returncode, 'inventory': str(output / 'inventory.json'), 'frames_present': summary['frames_present']}))
    return result.returncode or (0 if summary['frames_present'] == frames and all(r.get('complete') for r in summary['frames']) else 2)


def unreal(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = args.engine / 'Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor'
    command = [str(binary), str(args.project.resolve()), '/Game/Sponza/Maps/Sponza_VulkanReference',
               '-game', '-windowed', '-ResX=2560', '-ResY=1440', '-ForceRes', '-unattended', '-nosplash', '-NoVSync',
               '-UseFixedTimeStep', '-FPS=62.5',  # Primal GI's existing delta_time_override is .016s.
               f'-LumenCompareOutput={output}', '-ExecCmds=r.ScreenPercentage 100,r.DynamicRes.OperationMode 0,r.Lumen.HardwareRayTracing 0',
               f'-abslog={output / "render.log"}', '-stdout']
    write(output / 'run.json', {'engine': 'unreal', 'frames': FRAMES, 'command': command,
          'scope': 'standalone main game viewport; one native dump per viewport Draw',
          'project': str(args.project.resolve()), 'fixed_delta_seconds': .016,
          'capture_source_sha256': digest(ROOT / 'Tools/LumenCompare/Unreal/LumenCompareCapture/Source/LumenCompareCapture/Private/LumenCompareCapture.cpp')})
    with (output / 'stdout.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            result = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
            result = 124
    summary = inventory(output, 'unreal')
    write(output / 'inventory.json', summary)
    # Native DumpGPU may explicitly omit unsupported resources. Successful
    # scheduling is a separate assertion from complete resource coverage.
    scheduled = summary['frames_present'] == FRAMES and all(r.get('native_status_ok') and r.get('single_view_scope_verified') for r in summary['frames'])
    print(json.dumps({'returncode': result, 'schedule_verified': scheduled, 'inventory': str(output / 'inventory.json')}))
    return result or (0 if scheduled else 2)


def inventory(directory, engine):
    run_record = directory / 'run.json'
    requested = json.loads(run_record.read_text()).get('frames', FRAMES) if run_record.exists() else FRAMES
    archive_path = directory / 'raw-manifest.json'
    archived = {r['path']: r for r in json.loads(archive_path.read_text())['files']} if archive_path.exists() else {}
    verified_objects = {}
    def available(path):
        if path.is_file():
            return True, path.stat().st_size, 'raw'
        record = archived.get(str(path.relative_to(directory)))
        if not record:
            return False, 0, 'missing'
        key = record['sha256']
        if key not in verified_objects:
            h = hashlib.sha256()
            size = 0
            with gzip.open(directory / record['object'], 'rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    h.update(block); size += len(block)
            verified_objects[key] = h.hexdigest() == key and size == record['bytes']
        return verified_objects[key], record['bytes'], 'sha_verified_archive'
    rows = []
    for frame in requested:
        if engine == 'primal':
            path = directory / f'frame-{frame}/capture.json'
            if not path.is_file():
                rows.append({'frame': frame, 'status': 'missing'}); continue
            meta = json.loads(path.read_text())
            if meta.get('ordinal') != frame:
                rows.append({'frame': frame, 'status': 'identity_mismatch'}); continue
            records = meta['resources']
            for item in records:
                if item['status'] == 'captured':
                    raw = path.parent / item['file']
                    found, size, storage = available(raw)
                    item['verified'] = found and size == item['bytes']
                    item['storage'] = storage
                    if item['verified'] and raw.exists():
                        item['sha256'] = digest(raw)
                    elif item['verified']:
                        item['sha256'] = archived[str(raw.relative_to(directory))]['sha256']
            rows.append({'frame': frame, 'status': 'present', 'metadata': str(path),
                         'complete': meta['complete'], 'resources': records})
        else:
            request = directory / f'frame-{frame}-request.json'
            if not request.is_file():
                rows.append({'frame': frame, 'status': 'missing'}); continue
            meta = json.loads(request.read_text())
            # Requests retain the original native path as provenance; resolve
            # relative to the run so an archived run can be moved intact.
            dump = directory / f'frame-{frame}' / Path(meta['dump_directory']).name
            statuses = list(dump.rglob('*Status*')) if dump.is_dir() else []
            bins = list(dump.rglob('*.bin')) if dump.is_dir() else []
            passes_path = dump / 'Base/Passes.json'
            passes = json_sequence(passes_path) if passes_path.exists() else []
            desc_path = dump / 'Base/ResourceDescs.json'
            descriptors = json_sequence(desc_path) if desc_path.exists() else []
            desc_by_id = {r['UniqueResourceName']: r for r in descriptors}
            outputs = []
            for native_pass in passes:
                for resource in native_pass['OutputResources']:
                    file = dump / 'Resources' / f"{resource}.v{native_pass['Pointer']}.bin"
                    desc = desc_by_id.get(resource.split('.')[0], {})
                    found, size, storage = available(file)
                    outputs.append({'pass_id': native_pass['Id'], 'pass': native_pass['EventName'],
                                    'scopes': native_pass['ParentEventScopes'], 'resource': resource,
                                    'name': desc.get('Name'), 'descriptor': desc,
                                    'file': str(file), 'bytes': size, 'storage': storage,
                                    'status': 'captured' if found else 'missing_binary'})
            boundary_path = directory / f'frame-{frame}-boundary.json'
            boundary = json.loads(boundary_path.read_text()) if boundary_path.exists() else None
            native_ok = any(p.read_text().strip() == 'ok' for p in statuses)
            scope_ok = bool(boundary and boundary.get('ordinal') == frame and boundary.get('main_families_in_scope') == 1 and
                            meta.get('native_frame_number', -1) - meta.get('native_frame_origin', 1) + 1 == frame)
            rows.append({'frame': frame, 'status': 'present', 'request': meta,
                         'status_files': [str(p) for p in statuses], 'binary_count': len(bins),
                         'binary_bytes': sum(p.stat().st_size for p in bins),
                         'native_status_ok': native_ok, 'single_view_scope_verified': scope_ok,
                         'boundary': boundary, 'pass_count': len(passes), 'outputs': outputs,
                         'native_frame_scopes': sorted({s for p in passes for s in p['ParentEventScopes'] if s.startswith('Frame ')}),
                         'complete': native_ok and scope_ok and bool(outputs) and all(r['status'] == 'captured' for r in outputs),
                         'verification': 'missing native texture/buffer binaries remain explicit coverage failures'})
    return {'schema': 1, 'engine': engine, 'requested': requested,
            'frames_present': [r['frame'] for r in rows if r['status'] == 'present'], 'frames': rows}


def archive(args):
    """Content-addressed gzip, verify uncompressed SHA, then remove only this run's raw/bin."""
    directory = args.output.resolve()
    store = directory / 'objects'
    store.mkdir(exist_ok=True)
    manifest = directory / 'raw-manifest.json'
    # Never replace an earlier cleanup receipt on a second invocation.
    if manifest.exists():
        raise ValueError('raw-manifest.json already exists; refusing to overwrite archive provenance')
    before = shutil.disk_usage(directory).free
    files = sorted(p for p in directory.rglob('*') if p.is_file() and p.suffix in ('.raw', '.bin') and 'objects' not in p.relative_to(directory).parts)
    records = []
    for path in files:
        sha = digest(path)
        target = store / (sha + '.gz')
        if not target.exists():
            temporary = target.with_suffix('.tmp')
            with path.open('rb') as source, temporary.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0, compresslevel=1) as dest:
                shutil.copyfileobj(source, dest)
            temporary.replace(target)
        h = hashlib.sha256()
        with gzip.open(target, 'rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                h.update(block)
        if h.hexdigest() != sha:
            raise ValueError(f'archive verification failed: {path}')
        records.append({'path': str(path.relative_to(directory)), 'sha256': sha,
                        'bytes': path.stat().st_size, 'object': str(target.relative_to(directory))})
    # All inputs have been archived and independently decompressed before deletion.
    write(manifest, {'schema': 1, 'files': records, 'verified': True, 'cleanup_requested': args.clean})
    if args.clean:
        for record in records:
            path = directory / record['path']
            if digest(path) != record['sha256']:
                raise ValueError(f'input changed before cleanup: {path}')
            path.unlink()
    write(directory / 'archive-validation.json', {'verified': True, 'removed': args.clean,
          'files': len(records), 'unique_objects': len({r['sha256'] for r in records}),
          'free_space_delta_bytes': shutil.disk_usage(directory).free - before,
          'note': 'filesystem delta may include concurrent activity; original assets/build products excluded'})


def restore(args):
    directory = args.output.resolve()
    manifest = json.loads((directory / 'raw-manifest.json').read_text())
    entries = {r['path']: r for r in manifest['files']}
    destination = args.destination.resolve()
    for relative in args.path:
        path = Path(relative)
        if path.is_absolute() or '..' in path.parts or relative not in entries:
            raise ValueError(f'not an archived relative path: {relative}')
        record = entries[relative]
        target = destination / path
        if target.exists():
            if digest(target) != record['sha256']:
                raise ValueError(f'refusing to overwrite different content: {target}')
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + '.restore-tmp')
        h = hashlib.sha256()
        with gzip.open(directory / record['object'], 'rb') as source, temporary.open('xb') as out:
            for block in iter(lambda: source.read(1024 * 1024), b''):
                h.update(block); out.write(block)
        if h.hexdigest() != record['sha256']:
            temporary.unlink()
            raise ValueError(f'restored SHA mismatch: {relative}')
        temporary.replace(target)
        print(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('prepare-ue'); p.add_argument('--project', type=Path, required=True)
    p.add_argument('--plugin', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('primal'); p.add_argument('--output', type=Path, required=True); p.add_argument('--timeout', type=int, default=240)
    p.add_argument('--frames', help='increasing comma-separated frame ordinals')
    p.add_argument('--raw-startup', action='store_true', help='observe native startup without resource preparation or history reset')
    p.add_argument('--passes', help='comma-separated producer-name fragments')
    p.add_argument('--staging-mib', type=int, help='observer readback staging budget per frame (64..4096 MiB; default 1024); does not change render/GI budgets')
    p = sub.add_parser('unreal'); p.add_argument('--engine', type=Path, required=True); p.add_argument('--project', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--timeout', type=int, default=900)
    p = sub.add_parser('inventory'); p.add_argument('--engine', choices=['primal', 'unreal'], required=True); p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('archive'); p.add_argument('--output', type=Path, required=True); p.add_argument('--clean', action='store_true')
    p = sub.add_parser('restore'); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--destination', type=Path, required=True); p.add_argument('--path', action='append', required=True)
    args = parser.parse_args()
    if args.action == 'prepare-ue': prepare(args)
    elif args.action == 'primal': return primal(args)
    elif args.action == 'unreal': return unreal(args)
    elif args.action == 'inventory': write(args.output / 'inventory.json', inventory(args.output.resolve(), args.engine))
    elif args.action == 'restore': restore(args)
    else: archive(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
