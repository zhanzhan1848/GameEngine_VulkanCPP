#!/usr/bin/env python3
"""Run isolated fixed/moving Sponza timing trials, serially; no captures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / 'Darwin/Debug/TestVulkanSponzaRenderGraph'
REPORT = ROOT / 'Docs/reports/primal-stutter-darkpatch-20261028'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', choices=('fixed', 'moving', 'both'), default='both')
    p.add_argument('--frames', type=int, default=480)
    p.add_argument('--timeout', type=float, default=300)
    p.add_argument('--nonblocking-query', action='store_true', help='explicitly enable the experimental Vulkan nonblocking query')
    p.add_argument('--sah', action='store_true', help='explicitly enable the experimental SAH environment BVH')
    args = p.parse_args()
    if args.frames < 421 or args.timeout <= 0:
        p.error('frames must be at least 421 and timeout positive')
    if not BINARY.is_file() or not os.access(BINARY, os.X_OK):
        p.error(f'missing executable: {BINARY}')
    REPORT.mkdir(parents=True, exist_ok=True)
    run = REPORT / ('timing-' + time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:10])
    run.mkdir(exist_ok=False)
    # Do not inherit caller diagnostic, capture, quality or experimental flags.
    system_keys = ('PATH', 'HOME', 'TMPDIR', 'USER', 'LOGNAME', 'SHELL', 'LANG', 'LC_ALL',
                   'DISPLAY', 'VK_ICD_FILENAMES', 'VK_DRIVER_FILES', 'VK_LAYER_PATH',
                   'DYLD_FALLBACK_LIBRARY_PATH')
    base = {k: os.environ[k] for k in system_keys if k in os.environ}
    base.update(PRIMAL_FRAME_TIMING='1', SPONZA_TIMING_ONLY='1', SPONZA_MAX_FRAMES=str(args.frames),
                SPONZA_REFERENCE='0', SPGI_CAM_LOCK='1', SPGI_CAM='-12.5,1.7,-0.7,14,2',
                SPGI_FOV='60', DDGI_RAY_SAMPLE='7', DDGI_MEASURED_GEOMETRY='1',
                PRIMAL_SC_OWNER_CARD_DIRECTORY='1', SPGI_DDGI_VISIBILITY='2',
                PRIMAL_VK_ALBEDO_MIPS='1', PRIMAL_IBL_VISIBILITY='1',
                PRIMAL_IBL_SPECULAR_SCALE='.5', PRIMAL_TONE_ADAPT='0', PRIMAL_TONE_EV='0')
    if args.nonblocking_query: base['PRIMAL_VK_QUERY_NONBLOCKING'] = '1'
    if args.sah: base['PRIMAL_EXPERIMENT_ENV_SAH'] = '1'
    modes = ('fixed', 'moving') if args.mode == 'both' else (args.mode,)
    receipt = {'binary': str(BINARY), 'binary_sha256': digest(BINARY), 'frames': args.frames,
               'timeout_seconds': args.timeout, 'runs': [],
               'nonblocking_query_requested': args.nonblocking_query, 'sah_requested': args.sah,
               'scope': 'Serial native fixed/moving CPU segments and delayed GPU query recording timestamps; no screenshot or readback.'}
    (run / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'Timing output: {run}', flush=True)
    ok = True
    for mode in modes:
        if digest(BINARY) != receipt['binary_sha256']:
            print('Executable changed between timing trials; stopping.', file=sys.stderr)
            ok = False
            break
        env = dict(base)
        if mode == 'moving': env['SPONZA_CAM_SCRIPT'] = '1'
        output = run / (mode + '.log')
        started = time.monotonic()
        with output.open('xb') as stream:
            try:
                result = subprocess.run([str(BINARY)], cwd=ROOT, env=env, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=args.timeout)
                code, timed_out = result.returncode, False
            except subprocess.TimeoutExpired:
                code, timed_out = None, True
        receipt['runs'].append({'mode': mode, 'environment': {k: env[k] for k in sorted(env) if k not in system_keys},
                                'log': str(output), 'elapsed_seconds': time.monotonic()-started,
                                'exit_code': code, 'timed_out': timed_out, 'log_sha256': digest(output),
                                'binary_sha256_after': digest(BINARY)})
        (run / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(f'{mode}: exit={code} timeout={timed_out} log={output}', flush=True)
        if code != 0 or timed_out or receipt['runs'][-1]['binary_sha256_after'] != receipt['binary_sha256']:
            ok = False
            break
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
