#!/usr/bin/env python3
"""Create a portable six-frame evidence browser; no unverified cross-engine metric."""
import argparse
import gzip
import html
import importlib.util
import json
from pathlib import Path
import shutil
import sys

spec = importlib.util.spec_from_file_location('capture', Path(__file__).with_name('capture.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def raw_bytes(run, path):
    if path.exists():
        return path.read_bytes()
    manifest = json.loads((run / 'raw-manifest.json').read_text())
    relative = str(path.relative_to(run))
    record = next(r for r in manifest['files'] if r['path'] == relative)
    data = gzip.decompress((run / record['object']).read_bytes())
    import hashlib
    if hashlib.sha256(data).hexdigest() != record['sha256']:
        raise ValueError(f'archive SHA mismatch: {relative}')
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--primal', type=Path, required=True)
    p.add_argument('--unreal', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.primal, a.unreal, a.output = a.primal.resolve(), a.unreal.resolve(), a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=True)
    import numpy as np
    from PIL import Image
    # Inventories were verified before raw cleanup; re-render uses the SHA archive.
    primal = json.loads((a.primal / 'inventory.json').read_text())
    unreal = json.loads((a.unreal / 'inventory.json').read_text())
    stages = json.loads(Path(__file__).with_name('pass-map.json').read_text())['stages']
    readiness = {}
    for name, run in [('primal', a.primal), ('unreal', a.unreal)]:
        path = run / 'scene-readiness.json'
        readiness[name] = json.loads(path.read_text()) if path.exists() else {'verified': False, 'reason': 'no scene-readiness proof before first measured view'}
    integrity = {}
    for name, run in [('primal', a.primal), ('unreal', a.unreal)]:
        path = run / 'scene-integrity.json'
        integrity[name] = json.loads(path.read_text()) if path.exists() else {'verified': False, 'reason': 'rendered geometry/material coverage has not been checked'}
    scene_ready = all(r.get('verified') for r in [*readiness.values(), *integrity.values()])
    rows = []
    for frame in capture.FRAMES:
        pr = next(r for r in primal['frames'] if r['frame'] == frame)
        ue = next(r for r in unreal['frames'] if r['frame'] == frame)
        outputs = pr.get('resources', [])
        tone = next((r for r in outputs if r['name'] == 'ToneMapping_Output' and r['status'] == 'captured'), None)
        if tone:
            rgba = np.frombuffer(raw_bytes(a.primal, a.primal / f'frame-{frame}' / tone['file']), np.uint8).reshape(tone['height'], tone['width'], 4)
            Image.fromarray(rgba[:, :, :3]).save(a.output / f'primal-{frame}.png')
        ue_image = a.unreal / f'frame-{frame}.png'
        if ue_image.exists(): shutil.copyfile(ue_image, a.output / f'unreal-{frame}.png')
        missing = [r for r in ue.get('outputs', []) if r['status'] != 'captured']
        rows.append({'frame': frame, 'primal_complete': pr.get('complete'),
                     'primal_outputs': len(outputs), 'unreal_passes': ue.get('pass_count'),
                     'unreal_outputs': len(ue.get('outputs', [])), 'unreal_missing_outputs': len(missing),
                     'unreal_native_ok': ue.get('native_status_ok'), 'unreal_single_view': ue.get('single_view_scope_verified'),
                     'unreal_native_frame': ue.get('request', {}).get('native_frame_number'),
                     'unreal_missing_names': sorted({r.get('name') or r['resource'] for r in missing})})
    capture.write(a.output / 'summary.json', {'frames': rows, 'primal_run': str(a.primal), 'unreal_run': str(a.unreal),
        'scene_readiness': readiness, 'scene_integrity': integrity, 'gi_convergence_baseline_valid': scene_ready,
        'limits': ['Native UE DumpGPU omits some 3D and SkipTracking resource bodies; missing entries are explicit.',
                   'Primal complete means the configured RG outputs and named internal dispatch samples, not every private allocation.',
                   'No causal GI diagnosis or pixel error metric is certified until material, exposure and receiver-domain alignment.']})
    parts = ['<!doctype html><meta charset="utf-8"><title>Sponza Lumen — six frame evidence</title>',
             '<style>body{font:15px system-ui;background:#11151b;color:#e4e9f1;margin:28px}a{color:#8fc8ff}table{border-collapse:collapse;width:100%}td,th{padding:10px;border-bottom:1px solid #34404e;text-align:left}.pair{display:grid;grid-template-columns:1fr 1fr;gap:12px}img{width:100%;background:#080a0d}summary{cursor:pointer}pre{white-space:pre-wrap;overflow-wrap:anywhere}.note{color:#f7ce85}</style>',
             '<h1>Sponza · Lumen 中间过程采集</h1><p>固定主视口，2560×1440，垂直 FOV 75°。帧号指首次主视图提交起的渲染序号；UE 原生 FrameNumber 独立核对。</p>',
             ('<p class="note"><strong>不能用作 GI 收敛基线：首帧场景完整度门禁未通过。</strong>资源准备或实际几何/材质覆盖证据缺失。旧启动轮已确认 Frame 1 缺少旗帜、帘布等元素。complete 仅表示已声明输出的数据读回完整，不表示场景完整。</p>' if not scene_ready else '<p>首帧前资源准备与实际几何/材质覆盖检查均已通过，记录随采集保存。</p>'),
             '<p class="note">截图仅供观察最终显示。两边的曝光、材质求值和缓存表示仍需逐项核对；此报告不把显示亮度差直接归因为 GI。</p>',
             '<p><a href="summary.json">采集摘要</a> · <a href="#mapping">PASS 含义对应</a></p>',
             '<table><tr><th>帧</th><th>Primal 输出</th><th>UE PASS</th><th>UE 输出引用 / 缺失</th><th>单视口闭合</th></tr>']
    for r in rows:
        parts.append(f'<tr><td><a href="#f{r["frame"]}">{r["frame"]}</a></td><td>{r["primal_outputs"]} / complete={r["primal_complete"]}</td><td>{r["unreal_passes"]}</td><td>{r["unreal_outputs"]} / {r["unreal_missing_outputs"]}</td><td>{r["unreal_single_view"]}</td></tr>')
    parts.append('</table><h2 id="mapping">按数据含义比较</h2><table>')
    for s in stages:
        parts.append('<tr><th>'+html.escape(s['name'])+'</th><td>'+html.escape(', '.join(s['primal']))+'</td><td>'+html.escape(', '.join(s['unreal']))+'</td><td>'+html.escape(s['rule'])+'</td></tr>')
    parts.append('</table>')
    for r in rows:
        frame = r['frame']
        parts.append(f'<h2 id="f{frame}">Frame {frame}</h2><div class="pair"><figure><figcaption>Primal · Tone 输出</figcaption><img src="primal-{frame}.png"></figure><figure><figcaption>Unreal · 主视口</figcaption><img src="unreal-{frame}.png"></figure></div>')
        pr = next(x for x in primal['frames'] if x['frame'] == frame)
        ue = next(x for x in unreal['frames'] if x['frame'] == frame)
        parts.append('<details><summary>原始 PASS、资源身份与覆盖缺口</summary><pre>'+html.escape(json.dumps({'summary': r, 'primal': pr, 'unreal': ue}, ensure_ascii=False, indent=2))+'</pre></details>')
    (a.output / 'index.html').write_text('\n'.join(parts))
    print(a.output / 'index.html')


if __name__ == '__main__': main()
