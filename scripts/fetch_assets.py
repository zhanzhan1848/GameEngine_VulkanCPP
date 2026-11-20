#!/usr/bin/env python3
"""Inspect and restore content-addressed assets from a local directory, HTTPS, or S3."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = ROOT / 'asset-manifests' / 'sponza.json'


def load_manifest(path):
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('version') != 1 or not isinstance(data.get('assets'), list):
        raise ValueError('unsupported manifest')
    ids, paths = set(), set()
    for a in data['assets']:
        if not isinstance(a, dict) or not isinstance(a.get('id'), str) or not a['id'] or a['id'] in ids:
            raise ValueError('missing or duplicate asset id')
        ids.add(a['id'])
        p = a.get('path', '')
        parts = PurePosixPath(p).parts
        if not p or '\\' in p or PurePosixPath(p).is_absolute() or any(x in ('.', '..') for x in p.split('/')) or not p.startswith('EngineTest/assets/'):
            raise ValueError(f'unsafe asset path: {p!r}')
        if p in paths:
            raise ValueError(f'duplicate asset path: {p}')
        paths.add(p)
        sha = a.get('sha256', '')
        if not re.fullmatch(r'[0-9a-f]{64}', sha) or type(a.get('size')) is not int or a['size'] < 0:
            raise ValueError(f'invalid digest or size: {p}')
        if a.get('key') != f'sha256/{sha}/{parts[-1]}':
            raise ValueError(f'invalid object key: {p}')
        if not isinstance(a.get('groups'), list) or not a['groups'] or any(not isinstance(g, str) or not g for g in a['groups']):
            raise ValueError(f'invalid groups: {p}')
    return data


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def matches(path, a):
    return path.is_file() and path.stat().st_size == a['size'] and digest(path) == a['sha256']


def destination(root, a):
    path = root / a['path']
    # Reject symlink escapes, including existing parent directories.
    if not path.resolve().is_relative_to(root.resolve() / 'EngineTest' / 'assets'):
        raise ValueError(f'destination escapes asset directory: {path}')
    return path


def copy_source(source, key, out):
    if source.startswith('s3://'):
        endpoint = os.environ.get('PRIMAL_ASSET_S3_ENDPOINT')
        if not endpoint:
            raise ValueError('set PRIMAL_ASSET_S3_ENDPOINT for S3-compatible storage')
        subprocess.run(['aws', 's3', 'cp', source.rstrip('/') + '/' + key, str(out), '--endpoint-url', endpoint, '--only-show-errors'], check=True)
    elif source.startswith('https://'):
        url = source.rstrip('/') + '/' + urllib.parse.quote(key, safe='/')
        with urllib.request.urlopen(url, timeout=60) as response, out.open('wb') as target:
            while chunk := response.read(1024 * 1024):
                target.write(chunk)
    else:
        src = Path(source).resolve() / key
        with src.open('rb') as f, out.open('wb') as target:
            while chunk := f.read(1024 * 1024):
                target.write(chunk)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument('--root', type=Path, default=ROOT, help='destination checkout root')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    for name in ('verify', 'fetch'):
        p = sub.add_parser(name)
        p.add_argument('group')
        if name == 'fetch':
            p.add_argument('--source', required=True, help='local directory, HTTPS base URL, or s3://bucket/prefix')
    args = parser.parse_args()
    data = load_manifest(args.manifest)
    groups = sorted({g for a in data['assets'] for g in a['groups']})
    if args.command == 'list':
        print('publication:', data.get('publication', 'unknown'))
        for group in groups:
            assets = [a for a in data['assets'] if group in a['groups']]
            print(f'{group}: {len(assets)} files, {sum(a["size"] for a in assets)} bytes')
        return 0
    if args.group not in groups:
        raise ValueError(f'unknown group {args.group!r}; available: {", ".join(groups)}')
    if args.command == 'fetch' and args.source.startswith('s3://'):
        if not os.environ.get('PRIMAL_ASSET_S3_ENDPOINT'):
            raise ValueError('set PRIMAL_ASSET_S3_ENDPOINT for S3-compatible storage')
        if not shutil.which('aws'):
            raise ValueError('AWS CLI is required for an s3:// source')
    failures = 0
    for a in (x for x in data['assets'] if args.group in x['groups']):
        path = destination(args.root, a)
        if matches(path, a):
            print(f'OK {a["path"]}')
            continue
        if path.exists() or path.is_symlink():
            print(f'CONFLICT {a["path"]}: existing file differs; preserved', file=sys.stderr)
            failures += 1
            continue
        if args.command == 'verify':
            print(f'MISSING {a["path"]}', file=sys.stderr)
            failures += 1
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        # Check again after creating directories; never follow symlinks outside root.
        destination(args.root, a)
        fd, name = tempfile.mkstemp(prefix='.asset-', dir=path.parent)
        os.close(fd)
        tmp = Path(name)
        try:
            copy_source(args.source, a['key'], tmp)
            if not matches(tmp, a):
                raise ValueError('download size or SHA-256 mismatch')
            # Atomic creation without replacing an existing destination.
            os.link(tmp, path)
            print(f'FETCHED {a["path"]}')
        except (OSError, ValueError, subprocess.CalledProcessError) as exc:
            print(f'ERROR {a["path"]}: {exc}', file=sys.stderr)
            failures += 1
        finally:
            tmp.unlink(missing_ok=True)
    return 1 if failures else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        sys.exit(2)
