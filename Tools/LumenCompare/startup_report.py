#!/usr/bin/env python3
"""Inspect completed native-startup GBuffer/culling captures (no GI inference)."""
import argparse
from collections import Counter
import csv
import hashlib
import html
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw
from report import raw_bytes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    run, out = a.run.resolve(), a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'previews').mkdir(exist_ok=True)
    frames = json.loads((run / 'run.json').read_text())['frames']
    records, sets, masks, visibility = [], [], {}, []
    for frame in frames:
        directory = run / f'frame-{frame}'
        meta = json.loads((directory / 'capture.json').read_text())
        native = json.loads((directory / 'culling.json').read_text())
        assert meta['complete'] and meta['completion_checked']
        assert meta['ordinal'] == frame and meta['engine_frame'] == frame - 1
        assert not native['force_pass']
        cull = {r['name']: r for r in meta['resources'] if r['pass'] == 'Culling.Result'}
        def load(name):
            return np.frombuffer(raw_bytes(run, directory / cull[name]['file']), dtype='<u4')
        count = int(load('visible-counter')[0])
        expanded = int(load('cluster-counter')[0])
        args = load('indirect-args')[:4]
        ids = load('visible-cluster-list')[:count]
        cv = load('cluster-visibility')[:expanded * 12].reshape(expanded, 12)
        iv = load('instance-visibility')[:native['instances'] * 4].reshape(-1, 4)
        instances = load('input-instances')[:native['instances'] * 48].reshape(-1, 48)
        assert int(args[1]) == count == int(np.count_nonzero(cv[:, 0]))
        assert len(set(map(int, ids))) == count
        assert set(map(int, ids)) == set(map(int, cv[cv[:, 0] != 0, 1]))
        assert np.all(cv[:, 1] < native['cluster_refs']) and np.all(cv[:, 2] < native['instances'])
        cluster_rows = []
        for row in cv:
            instance = int(row[2])
            cluster_rows.append({'cluster_id': int(row[1]), 'instance_id': instance,
                'geometry_id': int(instances[instance, 32]), 'material_id': int(instances[instance, 33]),
                'visible': bool(row[0]), 'slot_frame': int(row[4])})
        with (out / f'culling-frame-{frame}.csv').open('w') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(cluster_rows[0]))
            writer.writeheader(); writer.writerows(cluster_rows)
        visibility.append(cluster_rows)
        sets.append(set(map(int, ids)))
        record = {'frame': frame, 'engine_frame': meta['engine_frame'], 'slot': native['slot'],
            'visible_instances': int(np.count_nonzero(iv[:, 0])), 'expanded_clusters': expanded,
            'visible_clusters': count, 'rejected_clusters': expanded - count,
            'indirect_args': list(map(int, args)), 'meshlets_before_draw': native['meshlets_before_draw'],
            'complete': meta['complete'], 'gbuffer': {}}
        for kind in ['albedo', 'normal', 'orm', 'depth']:
            r = next(r for r in meta['resources'] if r['pass'] == 'GI.GBuffer' and r['name'] == kind)
            data = raw_bytes(run, directory / r['file'])
            w, h = r['width'], r['height']
            if kind == 'depth':
                depth = np.frombuffer(data, dtype='<f4').reshape(h, w)
                distance = .1 / np.maximum(1. - depth, 1e-7)
                gray = np.clip(1. - np.log(np.maximum(distance, .1) / .1) / np.log(10000.), 0., 1.)
                rgb = np.repeat((gray * 255).astype('u1')[:, :, None], 3, axis=2)
            else:
                rgba = np.frombuffer(data, dtype='u1').reshape(h, w, 4)
                rgb = rgba[:, :, [2, 1, 0]] if r['format'] == 32 else rgba[:, :, :3]
            Image.fromarray(rgb).save(out / 'previews' / f'{kind}-{frame}.png')
            record['gbuffer'][kind] = {'sha256': hashlib.sha256(data).hexdigest(), 'format': r['format'], 'width': w, 'height': h, 'file': r['file']}
            if kind == 'albedo':
                f = rgb.astype('f4')
                green = (f[:, :, 1] > f[:, :, 0] * 1.4) & (f[:, :, 1] > f[:, :, 2] * 1.4) & (f[:, :, 1] > 50)
                record['green_material_pixels'] = int(np.count_nonzero(green))
        records.append(record)
    reference = sets[-1]
    affected = []
    for frame, visible, rows, record in zip(frames, sets, visibility, records):
        missing = reference - visible
        record['missing_vs_last_frame'] = len(missing)
        record['extra_vs_last_frame'] = len(visible - reference)
        count = Counter(r['geometry_id'] for r in rows if r['cluster_id'] in missing)
        affected.append({'frame': frame, 'missing_cluster_ids': sorted(missing), 'affected_geometry_cluster_counts': dict(count)})
    summary = {'scope': 'Primal native startup; no raster preparation, geometry prepublication, force-pass or GI reset',
        'frames': records, 'culling_frame2_to_6_sets_identical': all(s == sets[1] for s in sets[1:]),
        'qualification': 'Known startup geometry-loss regression; reference is last captured frame, not a proof of all scene triangles.',
        'limits': ['Depth preview is logarithmic (near=.1); raw depth is preserved.',
                   'Cluster visibility is captured after Stage7, before draw; no individual rejection stage is certified.',
                   'Readback introduces synchronization; this is not a performance measurement.']}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    (out / 'missing-clusters.json').write_text(json.dumps(affected, indent=2) + '\n')
    for kind in ['albedo', 'normal', 'orm', 'depth']:
        sheet = Image.new('RGB', (1920, 776), (16, 21, 29)); draw = ImageDraw.Draw(sheet)
        for index, frame in enumerate(frames):
            x, y = (index % 3) * 640, (index // 3) * 388
            im = Image.open(out / 'previews' / f'{kind}-{frame}.png')
            sheet.paste(im.resize((640, 360)), (x, y + 28))
            draw.text((x + 12, y + 8), f'Frame {frame} | GBuffer {kind}', fill=(255, 125, 105) if sets[index] != reference else (225, 235, 245))
        sheet.save(out / f'gbuffer-{kind}-1-6.png')
    defects = [r['frame'] for r in records if r['missing_vs_last_frame'] or r['extra_vs_last_frame']]
    finding = (f'相对 Frame {frames[-1]}，Frame {", ".join(map(str, defects))} 存在可见 cluster 集合缺失/变化。' if defects else '采样帧的最终可见 cluster 集合一致。')
    if summary['culling_frame2_to_6_sets_identical']:
        finding += 'Frame 2–6 的可见 cluster ID 集合完全一致。'
    body = ['<!doctype html><meta charset="utf-8"><title>Primal first six frames — GBuffer / culling</title>',
        '<style>body{font:16px system-ui;background:#10151d;color:#e3ebf5;margin:24px}a{color:#93c9ff}img{max-width:100%}table{border-collapse:collapse}td,th{padding:10px 16px;border-bottom:1px solid #405065}pre{white-space:pre-wrap}.bad{color:#ff967b}</style>',
        '<h1>本引擎原生启动前 6 帧：GBuffer / culling</h1>',
        '<p class="bad">'+html.escape(finding)+'</p>',
        '<p>未预热、未提前发布几何、未强制放行剔除、未重置 GI。以下均为真实 GBuffer，不含 GI 或后处理亮度。</p>',
        '<table><tr><th>Frame</th><th>可见实例</th><th>展开 clusters</th><th>最终可见</th><th>被拒绝</th><th>DrawIndirect instanceCount</th><th>绘制前总 meshlets</th></tr>']
    for r in records:
        body.append('<tr>'+''.join(f'<td>{x}</td>' for x in [r['frame'], r['visible_instances'], r['expanded_clusters'], r['visible_clusters'], r['rejected_clusters'], r['indirect_args'][1], r['meshlets_before_draw']])+'</tr>')
    body += [f'</table><p>Frame {frames[0]} 比参考帧少 {records[0]["missing_vs_last_frame"]} 个 cluster；变化已在实际剔除输出及间接绘制参数中体现。GBuffer 不含 GI；尚未据此断言某个具体剔除阶段或 HZB 的根因。</p>',
             '<p><a href="summary.json">机器摘要与 SHA</a> · <a href="missing-clusters.json">缺失 cluster ID / geometry ID</a></p>']
    if (out / 'cause-analysis.json').exists():
        body += ['<h2>后续单变量实验：已定位首帧 HZB 遮挡误剔除</h2>',
                 '<p>同一个 binary，只改变 enable_occlusion_culling。Stage4 两组均为 2249；ON 的 Stage5 降为 1353，OFF 保持 2249。第一帧尚无上一帧构建的 HZB，但 IsReady 只判断纹理已创建，因而启用了无效的遮挡历史。</p>',
                 '<p>GPU 常量仅 byte 220 不同，实例/cluster-ref 输入逐位相同；带阶段快照的 ON 运行与原始六帧运行的首帧四种 GBuffer 也全部逐位相同。<a href="cause-analysis.json">实验及 SHA 证据</a></p>',
                 '<img src="frame1-occlusion-on-off.png">']
    for kind in ['albedo', 'normal', 'depth', 'orm']:
        body.append(f'<h2>{kind}</h2><img src="gbuffer-{kind}-1-6.png">')
    body.append('<h2>逐帧原图及剔除明细</h2>')
    for frame in frames:
        body.append(f'<p>Frame {frame}: '+ ' · '.join(f'<a href="previews/{kind}-{frame}.png">{kind}</a>' for kind in ['albedo','normal','depth','orm']) + f' · <a href="culling-frame-{frame}.csv">逐 cluster 剔除结果 CSV</a></p>')
    (out / 'index.html').write_text('\n'.join(body))
    print(out / 'index.html')


if __name__ == '__main__': main()
