#!/usr/bin/env python3
"""Browse native pass textures and buffers; previews never replace raw data."""
import argparse
import hashlib
import html
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from report import raw_bytes


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--gbuffer-url', default='gbuffer/index.html', help='relative URL of the GBuffer report')
    a = p.parse_args()
    run, out = a.run.resolve(), a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    images = out / 'images'
    images.mkdir(exist_ok=True)
    formats = {1: ('u1', 1), 3: ('u1', 1), 7: ('<u2', 1), 9: ('<f2', 1),
               10: ('u1', 2), 20: ('<u4', 1), 22: ('<f4', 1), 27: ('<f2', 2),
               32: ('u1', 4), 36: ('u1', 4), 40: ('u1', 4), 45: ('<f4', 2),
               50: ('<f4', 3), 55: ('<f2', 4), 60: ('<f4', 4), 78: ('<f4', 1)}
    frames = json.loads((run / 'run.json').read_text())['frames']
    records, sheets = [], {}
    frame_label = ' / '.join(map(str, frames))
    body = [f'<!doctype html><meta charset="utf-8"><title>Primal frames {frame_label} — captured passes</title>',
            '<style>body{background:#121923;color:#e5ecf5;font:15px system-ui;margin:24px}a{color:#9cd1ff}img{max-width:100%}table{border-collapse:collapse;width:100%}td,th{padding:10px;border-bottom:1px solid #435063}td img{width:360px}code{overflow-wrap:anywhere}</style>',
            f'<h1>本引擎 Frame {frame_label}：各 pass 输出</h1>',
            '<p>未预热。首帧跳过无效 HZB 遮挡历史，GI 正常执行。捕获实际执行的 RenderGraph 输出及已接入的内部 dispatch 输出；不等于所有私有资源。未执行的 pass 不伪造输出。</p>',
            '<p>纹理保存 PNG；浮点纹理以 x/(1+x) 和 gamma 1/2.2 显示正值，深度使用单独映射。buffer 图为前 64 KiB 的原始字节灰度图，仅供检查布局，不能视作物理渲染结果。数值以归档 raw 和 capture.json 格式/尺寸为准。</p>',
            ('<p>'+ (f'<a href="{html.escape(a.gbuffer_url, quote=True)}">GBuffer / culling 专页</a> · ' if a.gbuffer_url else '') + '<a href="manifest.json">逐输出 SHA / 归档位置</a></p>'),
            '<p>'+ ' · '.join(f'<a href="#frame-{f}">Frame {f}</a>' for f in frames)+'</p>']
    archive = json.loads((run / 'raw-manifest.json').read_text()) if (run / 'raw-manifest.json').exists() else None
    archived = {r['path']: r for r in archive['files']} if archive else {}
    for frame in frames:
        directory = run / f'frame-{frame}'
        meta = json.loads((directory / 'capture.json').read_text())
        body += [f'<h2 id="frame-{frame}">Frame {frame}</h2>', '<table><tr><th>Pass / resource</th><th>截图 / 数据预览</th><th>原始数据</th></tr>']
        for index, r in enumerate(meta['resources']):
            row = dict(r, frame=frame)
            caption = html.escape(r['pass']+' / '+r['name'])
            preview = ''
            detail = html.escape(r['status'])
            if r['status'] == 'captured':
                data = raw_bytes(run, directory / r['file'])
                assert len(data) == r['bytes']
                row['sha256'] = hashlib.sha256(data).hexdigest()
                relative = f'frame-{frame}/'+r['file']
                row['raw_path'] = relative
                row['archive'] = archived.get(relative)
                if r['width'] and r['format'] in formats:
                    dtype, channels = formats[r['format']]
                    pixels = np.frombuffer(data, dtype=dtype).reshape(max(1,r['depth']),r['height'],r['width'],channels)
                    pixels = pixels[len(pixels)//2].astype('f4')
                    if dtype == 'u1': pixels /= 255
                    elif r['format'] in (7,20): pixels /= max(1,float(np.max(pixels)))
                    elif r['format'] == 78: pixels = np.clip(1-np.log(np.maximum(.1/(1-pixels+1e-7),.1)/.1)/np.log(10000),0,1)
                    else:
                        pixels = np.maximum(np.nan_to_num(pixels,nan=0,posinf=1e6,neginf=0),0)
                        pixels = np.power(pixels/(1+pixels),1/2.2)
                    if channels == 1: pixels = np.repeat(pixels,3,axis=2)
                    elif channels == 2: pixels = np.concatenate([pixels,np.zeros_like(pixels[:,:,:1])],axis=2)
                    else: pixels = pixels[:,:,:3]
                    if r['format'] == 32: pixels = pixels[:,:,::-1]
                    im = Image.fromarray((np.clip(pixels,0,1)*255).astype('u1'))
                    row['preview_kind'] = 'texture (3D middle slice)' if r['depth']>1 else 'texture'
                else:
                    values = np.zeros(65536,dtype='u1')
                    count = min(len(data),65536)
                    values[:count] = np.frombuffer(data[:count],dtype='u1')
                    im = Image.fromarray(values.reshape(256,256)).convert('RGB')
                    row['preview_kind'] = 'buffer byte grid, first 64 KiB'
                name = f'frame-{frame}-output-{index}.png'
                im.save(images/name)
                row['preview'] = 'images/'+name
                preview = f'<a href="images/{name}"><img loading="lazy" src="images/{name}"></a>'
                key = (r['pass'],r['name'])
                thumb = im.copy(); thumb.thumbnail((480,270))
                sheets.setdefault(key,{})[frame] = thumb
                obj = row['archive']
                link = Path(__import__('os').path.relpath(run / obj['object'],out)) if obj else Path(__import__('os').path.relpath(directory/r['file'],out))
                detail = f'{r["bytes"]:,} bytes · {r["width"]}×{r["height"]}×{r["depth"]} · format {r["format"]}<br><a href="{link}">raw gzip</a><br><code>{row["sha256"]}</code><br>{row["preview_kind"]}'
            records.append(row)
            body.append(f'<tr><td>{caption}</td><td>{preview}</td><td>{detail}</td></tr>')
        body.append('</table>')
    contacts=[]
    for index,(key, thumbnails) in enumerate(sheets.items()):
        sheet=Image.new('RGB',(1440,300*((len(frames)+2)//3)),(18,25,35)); draw=ImageDraw.Draw(sheet)
        for i,frame in enumerate(frames):
            x,y=(i%3)*480,(i//3)*300
            draw.text((x+8,y+5),f'Frame {frame}',fill='white')
            if frame in thumbnails: sheet.paste(thumbnails[frame],(x,y+28))
            else: draw.text((x+8,y+60),'No executed output',fill='gray')
        name=f'contact-{index}.png'; sheet.save(out/name)
        contacts.append({'pass':key[0],'name':key[1],'image':name})
    body.insert(6,'<details><summary>各 pass 逐帧对照</summary>'+''.join(f'<h3>{html.escape(r["pass"]+" / "+r["name"])}</h3><img loading="lazy" src="{r["image"]}">' for r in contacts)+'</details>')
    (out/'manifest.json').write_text(json.dumps({'run':str(run),'frames':frames,'outputs':records,'contacts':contacts},indent=2))
    (out/'index.html').write_text('\n'.join(body))
    print(out/'index.html')


if __name__ == '__main__': main()
