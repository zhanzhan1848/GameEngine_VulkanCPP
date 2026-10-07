#!/usr/bin/env python3
"""Prepare original SC trace rays and compare SDF/card hits to GPU+float64 triangle BVH.
Diagnostics only; opaque, two-sided geometry, no alpha/transmission certification.
"""
import argparse
import json
import struct
from pathlib import Path
import numpy as np


def prepare(source, target):
    data = Path(source).read_bytes()
    count, stride = struct.unpack_from('<II', data)
    if stride != 16 or len(data) != 8 + count * 64:
        raise ValueError('Invalid SC sample layout')
    words = np.frombuffer(data, '<u4', offset=8).reshape(count, 16)
    values = words.view('<f4')
    valid = np.isfinite(values[:, :10]).all(axis=1)
    valid &= abs((values[:, 4:7] ** 2).sum(axis=1) - 1) <= 2e-5
    valid &= (values[:, 3] >= 0) & (values[:, 7] > values[:, 3])
    words = words[valid]
    if not len(words):
        raise ValueError('No finite rays')
    Path(target + '.samples.bin').write_bytes(struct.pack('<II', len(words), 16) + words.tobytes())
    Path(target + '.rays.bin').write_bytes(b'PGRAYS01' + struct.pack('<II', len(words), 0) + words[:, :8].tobytes())
    meta = dict(input_count=count, finite_rays=len(words), invalid_rays=int((~valid).sum()),
                ray_ids=words[:, 10].tolist(), frame_stamps=sorted(set(words[:, 12].tolist())))
    Path(target + '.prepare.json').write_text(json.dumps(meta, indent=2))
    print(json.dumps(meta | {'ray_ids': 'see prepare.json'}))


def cpu_first(ray, nodes, links, triangles):
    o, d, minimum, maximum = ray[:3], ray[4:7], ray[3], ray[7]
    best, found = maximum, False
    stack = [0]
    while stack:
        index = stack.pop()
        lo, hi = minimum, best
        bounds = nodes[index]
        for k in range(3):
            if abs(d[k]) < 1e-15:
                if o[k] < bounds[0, k] or o[k] > bounds[1, k]:
                    lo, hi = 1., 0.
                    break
            else:
                a = (bounds[0, k] - o[k]) / d[k]
                b = (bounds[1, k] - o[k]) / d[k]
                lo, hi = max(lo, min(a, b)), min(hi, max(a, b))
        if lo > hi:
            continue
        left, right, start, count = links[index]
        if not count:
            stack.extend((int(left), int(right)))
            continue
        t = triangles[start:start + count]
        p = np.cross(d, t[:, 2, :3])
        det = np.einsum('ij,ij->i', t[:, 1, :3], p)
        mask = abs(det) > 1e-12
        inv = np.divide(1., det, out=np.zeros_like(det), where=mask)
        delta = o - t[:, 0, :3]
        u = np.einsum('ij,ij->i', delta, p) * inv
        q = np.cross(delta, t[:, 1, :3])
        v = q @ d * inv
        distance = np.einsum('ij,ij->i', t[:, 2, :3], q) * inv
        mask &= (u >= 0) & (v >= 0) & (u + v <= 1) & (distance >= minimum) & (distance <= best)
        if mask.any():
            best, found = min(best, distance[mask].min()), True
    return float(best) if found else -1.


def evaluate(sample_prefix, proof_prefix, trace_source="sdf", identity_rejections=False):
    data = Path(sample_prefix + '.samples.bin').read_bytes()
    n, stride = struct.unpack_from('<II', data)
    words = np.frombuffer(data, '<u4', offset=8).reshape(n, stride)
    values = words.view('<f4')
    raw = np.fromfile(proof_prefix + '.raw', '<f4').reshape(-1, 12)
    if len(raw) != n:
        raise ValueError('Oracle count mismatch')
    query_data = Path(proof_prefix + '.queries.bin').read_bytes()
    resident_rays = np.frombuffer(query_data, '<f4', offset=16).reshape(n, 8)
    if not np.array_equal(resident_rays, values[:, :8]):
        raise ValueError('Oracle rays differ from actual trace samples')
    node_data = np.fromfile(proof_prefix + '.nodes.bin', '<f4').reshape(-1, 3, 4)
    links = node_data[:, 2].view('<u4')
    tris = np.fromfile(proof_prefix + '.triangles.bin', '<f4').reshape(-1, 3, 4).astype('float64')
    cpu_nodes = node_data.astype('float64')
    truth = np.array([cpu_first(q.astype('float64'), cpu_nodes, links, tris) for q in resident_rays])
    gpu_hit = raw[:, 8] == 1
    gpu_valid = raw[:, 11] == 0
    gpu_agree = gpu_valid & (gpu_hit == (truth >= 0))
    gpu_agree &= ~gpu_hit | (abs(raw[:, 3] - truth) <= 2e-5)
    sdf_hit = words[:, 11] < 2
    card_hit = words[:, 13] == 1
    real_hit = truth >= 0
    sdf_delta = abs(values[:, 8] - truth)
    card_delta = abs(values[:, 9] - truth)
    def counts(hit, delta):
        both = hit & real_hit
        return dict(found=int(hit.sum()), missed_surface=int((~hit & real_hit).sum()),
                    invented_surface=int((hit & ~real_hit).sum()),
                    within_005=int((both & (delta <= .05)).sum()),
                    within_045=int((both & (delta <= .45)).sum()),
                    wrong_first_surface=int((both & (delta > .45)).sum()))
    false_sky = (words[:, 11] == 2) & real_hit
    report = dict(samples=n, triangle_hits=int(real_hit.sum()), triangle_misses=int((~real_hit).sum()),
                  gpu_cpu_agree=int(gpu_agree.sum()), oracle_valid=bool(gpu_agree.all()),
                  sdf=counts(sdf_hit, sdf_delta), card=counts(card_hit, card_delta),
                  sdf_false_sky=int(false_sky.sum()),
                  card_recovers_false_sky=int((false_sky & card_hit & (card_delta <= .45)).sum()),
                  divergence_gt045=int((sdf_hit & card_hit & (abs(values[:, 8]-values[:, 9]) > .45)).sum()),
                  sdf_good_card_wrong=int((sdf_hit & card_hit & real_hit & (sdf_delta <= .45) & (card_delta > .45)).sum()),
                  card_good_sdf_wrong=int((sdf_hit & card_hit & real_hit & (card_delta <= .45) & (sdf_delta > .45)).sum()),
                  frame_stamps=sorted(set(words[:, 12].tolist())),
                  scope='sampled original rays from latest refreshed tiles; opaque two-sided triangle geometry')
    report['trace_source'] = trace_source
    agreement = (sdf_hit == real_hit) & (~real_hit | (sdf_delta <= 2e-5))
    report['trace_oracle_agree_within_2e5'] = int(agreement.sum())
    report['trace_max_hit_distance_error'] = float(sdf_delta[sdf_hit & real_hit].max(initial=0))
    if trace_source == 'geometry':
        report['geometry'] = report.pop('sdf')
        if identity_rejections:
            missing = real_hit & ~card_hit
            report['no_cache_rejection_bits'] = {name: int((missing & ((words[:,14] & bit)!=0)).sum())
                for bit,name in [(1,'no_allocated_owner_card'),(2,'surface_candidate_rejected'),
                                 (4,'triangle_identity_rejected'),(8,'point_distance_rejected')]}
            report['rejection_scope'] = 'v4 identity resolver: overlapping candidate rejection bits; older samples lack this contract'
        report['geometry_false_sky'] = report.pop('sdf_false_sky')
        report['geometry_good_card_wrong'] = report.pop('sdf_good_card_wrong')
        report['card_good_geometry_wrong'] = report.pop('card_good_sdf_wrong')
    Path(sample_prefix + '.comparison.json').write_text(json.dumps(report, indent=2))
    header = 'ray_id,frame_stamp,sdf_type,sdf_t,card_t,triangle_t,oracle_agree\n'
    with open(sample_prefix + '.comparison.csv', 'w') as f:
        f.write(header)
        for i in range(n):
            f.write(f'{words[i,10]},{words[i,12]},{words[i,11]},{values[i,8]},{values[i,9]},{truth[i]},{int(gpu_agree[i])}\n')
    print(json.dumps(report, indent=2))
    if not report['oracle_valid']:
        raise ValueError('GPU oracle failed float64 cross-check; classification is not certified')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mode', choices=['prepare', 'evaluate'])
    ap.add_argument('--samples', required=True, help='source .samples.bin for prepare; prefix for evaluate')
    ap.add_argument('--prefix', required=True, help='output prefix for prepare; GPU proof prefix for evaluate')
    ap.add_argument('--trace-source', choices=['sdf', 'geometry'], default='sdf')
    ap.add_argument('--identity-rejections', action='store_true', help='interpret word14 as v4 triangle resolver rejection bits; only for v4 identity samples')
    args = ap.parse_args()
    if args.mode == 'prepare': prepare(args.samples, args.prefix)
    else: evaluate(args.samples, args.prefix, args.trace_source, args.identity_rejections)

if __name__ == '__main__': main()
