#!/usr/bin/env python3
"""Render paired linear irradiance captures; uses only Python 3.11+ stdlib."""
import argparse
import base64
import hashlib
import html
import json
import math
import pathlib
import statistics
import struct
import zlib


def png(width, height, rgb):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data)))
    rows = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def display_rgb(pixels):
    # Fixed exposure=1 and the same Reinhard+gamma curve for both captures.
    # This is a diagnostic view, not production tone mapping or color grading.
    return bytes(round(255 * (max(0., channel) / (1 + max(0., channel))) ** (1 / 2.2))
                 for pixel in pixels for channel in pixel[:3])


def finite_number(value, name, minimum=None):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f'Invalid numeric field {name}')
    if minimum is not None and value < minimum:
        raise ValueError(f'Out-of-range numeric field {name}')
    return value


def render(directory):
    directory = pathlib.Path(directory)
    rows = [json.loads(line) for line in (directory / 'samples.jsonl').read_text().splitlines()
            if line.strip()]
    if not rows:
        raise ValueError('No completed GPU samples')
    seen = set()
    panels, hashes, coverage, measured = [], {}, [], []
    dimensions = None
    for row in rows:
        frame, width, height = (row[key] for key in ('frame', 'width', 'height'))
        if any(isinstance(value, bool) or not isinstance(value, int)
               for value in (frame, width, height)) or frame < 0 or width <= 0 or height <= 0:
            raise ValueError('Invalid frame or dimensions')
        if frame in seen:
            raise ValueError('Duplicate frame capture')
        seen.add(frame)
        if dimensions is not None and dimensions != (width, height):
            raise ValueError('Capture dimensions changed')
        dimensions = (width, height)
        finite_number(row['rc_gpu_ms'], 'rc_gpu_ms', 0)
        for key in ('baseline_vma_allocated_bytes', 'with_rc_vma_allocated_bytes',
                    'rc_requested_bytes'):
            finite_number(row[key], key, 0)
        prefix = directory / f'frame-{frame}'
        captures = []
        for suffix, fmt, bpp in [('-gi.rgba16f', '<4e', 8), ('-rc.rgba32f', '<4f', 16)]:
            path = pathlib.Path(str(prefix) + suffix)
            data = path.read_bytes()
            if len(data) != width * height * bpp:
                raise ValueError(f'Incomplete capture {path}')
            hashes[path.name] = hashlib.sha256(data).hexdigest()
            pixels = list(struct.iter_unpack(fmt, data))
            if any(not math.isfinite(value) for pixel in pixels for value in pixel):
                raise ValueError('Nonfinite capture')
            captures.append(pixels)
        baseline, rc = captures
        if any(pixel[3] < 0 or pixel[3] > 1.00001 for pixel in rc):
            raise ValueError('RC coverage outside [0,1]')
        raw_coverage = statistics.mean(pixel[3] for pixel in rc)
        reported_coverage = finite_number(row['coverage'], 'coverage', 0)
        if abs(raw_coverage - reported_coverage) > 1e-5:
            raise ValueError('Reported coverage does not match captured GPU pixels')
        fully_known = sum(pixel[3] > .99 for pixel in rc)
        if fully_known != row['fully_known_pixels']:
            raise ValueError('Reported known-pixel count does not match GPU capture')
        # Baseline alpha rejects sky/invalid receivers. Comparison uses the same
        # fully known receiver mask on both images, not every black RC pixel.
        joint = [(old, new) for old, new in zip(baseline, rc)
                 if old[3] > .5 and new[3] > .99]
        squared = sum((old[k] - new[k]) ** 2 for old, new in joint for k in range(3))
        rmse = math.sqrt(squared / (len(joint) * 3)) if joint else None
        valid_baseline = sum(pixel[3] > .5 for pixel in baseline)
        measured.append({'frame': frame, 'jointly_valid_pixels': len(joint),
                         'valid_baseline_pixels': valid_baseline,
                         'jointly_valid_fraction': len(joint) / max(valid_baseline, 1),
                         'irradiance_difference_rmse_joint': rmse})
        images = [png(width, height, display_rgb(pixels)) for pixels in captures]
        mask = bytes(round(255 * min(1., max(0., pixel[3]))) for pixel in rc for _ in range(3))
        images.append(png(width, height, mask))
        for label, data in zip(('Existing GI irradiance', 'RC irradiance',
                                'RC known-direction coverage'), images):
            panels.append(f'<figure><figcaption>Frame {frame}: {label}</figcaption>'
                          f'<img src="data:image/png;base64,{base64.b64encode(data).decode()}"></figure>')
        coverage.append(raw_coverage)
    frames = [row['frame'] for row in rows]
    complete = len(rows) == 8 and frames == list(range(frames[0], frames[0] + 8))
    warm = rows[1:]
    baseline = rows[0]['baseline_vma_allocated_bytes']
    current = rows[-1]['with_rc_vma_allocated_bytes']
    summary = {
        'samples': len(rows), 'expected_samples': 8, 'eight_consecutive_samples': complete,
        'capture_acceptance': 'PASS' if complete else 'INCOMPLETE: need eight consecutive completed frames',
        'rc_gpu_ms_all': [row['rc_gpu_ms'] for row in rows],
        'rc_gpu_ms_median_warm': statistics.median(row['rc_gpu_ms'] for row in warm) if warm else None,
        'warmup_scope': 'First sample omitted from median; driver/shader warmup sufficiency is unverified',
        'coverage_mean': statistics.mean(coverage),
        'fully_known_pixels_last': rows[-1]['fully_known_pixels'],
        'coverage_acceptance': ('INSUFFICIENT: less than 99% of valid baseline receivers have full RC coverage'
                                if any(value['jointly_valid_fraction'] < .99 for value in measured)
                                else 'Coverage threshold reached; visual quality still unverified'),
        'paired_metrics': measured,
        'baseline_vma_allocated_bytes': baseline, 'with_rc_vma_allocated_bytes': current,
        'vma_allocated_delta_bytes': current - baseline,
        'rc_requested_bytes': rows[-1]['rc_requested_bytes'],
        'quality_acceptance': 'UNVERIFIED: current GI is not ground truth; incomplete coverage cannot establish quality',
        'performance_acceptance': 'UNVERIFIED: RC runs after current GI, shared SC excluded; selected baseline pass sum is not equivalent cost or replacement FPS',
        'memory_scope': 'VMA allocation/block bytes on this device; unified memory is not dedicated VRAM residency. Delta can include other live allocations.',
        'source_scope': rows[0]['source_scope'], 'sha256': hashes,
    }
    for key in ('baseline_vma_reserved_bytes', 'with_rc_vma_reserved_bytes'):
        if key in rows[-1]:
            summary[key] = rows[0][key] if key.startswith('baseline') else rows[-1][key]
    if 'baseline_vma_reserved_bytes' in summary and 'with_rc_vma_reserved_bytes' in summary:
        summary['vma_reserved_delta_bytes'] = summary['with_rc_vma_reserved_bytes'] - summary['baseline_vma_reserved_bytes']
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n')
    body = '''<!doctype html><meta charset="utf-8"><title>Sponza RC experiment</title>
<style>body{background:#171b21;color:#e1e7ee;font:15px system-ui;margin:24px}main{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}figure{margin:0}img{width:100%}pre{white-space:pre-wrap}figcaption{padding:8px}</style>
<h1>Sponza RC / current GI</h1><p>Paired linear diffuse irradiance, common exposure=1 with diagnostic Reinhard/gamma. Coverage black means missing data, not physical darkness. This compares transport using current Surface Cache supply; it does not establish an independent RC GI solution or visual superiority. RMSE is a difference metric, not accuracy against ground truth.</p>'''
    (directory / 'comparison.html').write_text(body + '<pre>' + html.escape(json.dumps(summary, indent=2)) +
                                               '</pre><main>' + ''.join(panels) + '</main>')
    print(json.dumps(summary, indent=2, allow_nan=False))
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=pathlib.Path)
    render(parser.parse_args().directory)
