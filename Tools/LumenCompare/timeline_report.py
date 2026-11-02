#!/usr/bin/env python3
"""Add a self-contained frame slider to an archived pass_report directory."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
from PIL import Image, ImageStat


def semantic_name(name):
    if re.match(r'^(ShadowMap_C\d+|(?:Far_)?LumenDDGI_(?:Irradiance|Depth)|SurfaceCache_Lighting(?:_NoDirect)?)_[012]$', name):
        return name.rsplit('_', 1)[0] + ' [frame slot]'
    return name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = args.report.resolve()
    manifest = json.loads((report / 'manifest.json').read_text())
    run = Path(manifest['run'])
    payload = {'frames': manifest['frames'], 'outputs': [], 'metadata': {}}
    for frame in manifest['frames']:
        meta = json.loads((run / f'frame-{frame}/capture.json').read_text())
        culling_path = run / f'frame-{frame}/culling.json'
        culling = json.loads(culling_path.read_text()) if culling_path.exists() else {}
        payload['metadata'][frame] = {'native': meta['engine_frame'], 'complete': meta['complete'],
            'gi_age': meta['gi_age_after_render'], 'occlusion': culling.get('occlusion_enabled'),
            'capture': os.path.relpath(run / f'frame-{frame}/capture.json', report),
            'culling': os.path.relpath(culling_path, report) if culling_path.exists() else None}
        tone = next((r for r in manifest['outputs'] if r['frame'] == frame and r['name'] == 'ToneMapping_Output' and r.get('preview')), None)
        if tone:
            mean = ImageStat.Stat(Image.open(report / tone['preview']).convert('RGB')).mean
            payload['metadata'][frame]['display_luma'] = round(sum(a*b for a,b in zip(mean, [.2126,.7152,.0722])), 2)
    counts = Counter()
    for source in manifest['outputs']:
        row = {k: source.get(k) for k in ['frame','pass','name','status','preview','preview_kind','bytes',
                'width','height','depth','format','sha256','file','index','version','physical']}
        name = semantic_name(source['name'])
        key = (source['frame'], source['pass'], name)
        occurrence = counts[key]
        counts[key] += 1
        row['key'] = source['pass'] + ' / ' + name + (f' #{occurrence+1}' if occurrence else '')
        if source.get('archive'):
            row['download'] = os.path.relpath(run / source['archive']['object'], report)
            row['logical_path'] = source['raw_path']
        elif source['status'] == 'captured':
            row['download'] = os.path.relpath(run / f'frame-{source["frame"]}' / source['file'], report)
        payload['outputs'].append(row)
    original = report / 'index.html'
    if original.exists() and 'id="timeline-data"' not in original.read_text():
        shutil.copyfile(original, report / 'all-passes.html')
    page = TEMPLATE.replace('__PAYLOAD__', json.dumps(payload, ensure_ascii=False).replace('<', '\\u003c'))
    (report / 'index.html').write_text(page)
    print(report / 'index.html')


TEMPLATE = r'''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>本引擎前六帧 · Pass 时间轴</title>
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0c1119;color:#e3eaf4}*{box-sizing:border-box}body{margin:0}a{color:#9ecaff}button,select,input{font:inherit}button,select,input[type=search]{background:#1a2637;color:#e3eaf4;border:1px solid #40516c;border-radius:7px;padding:8px 12px}button{cursor:pointer}button:disabled{opacity:.4;cursor:default}header{padding:20px 28px 12px}h1{margin:0 0 8px;font-size:25px}p{margin:8px 0;color:#9eafc5;font-size:14px;line-height:1.6}.bar{position:sticky;top:0;z-index:3;background:#101a29f5;border-block:1px solid #30415a;padding:14px 28px;backdrop-filter:blur(12px)}.controls{display:flex;gap:12px;align-items:center}.controls strong{min-width:110px;font-size:19px;font-variant-numeric:tabular-nums}input[type=range]{width:100%;min-width:80px;accent-color:#76b4ff;cursor:pointer}.ticks{display:flex;justify-content:space-between;color:#a4b6cf;margin:8px 0 0 245px;font-size:12px}.status{margin-top:10px;font-size:13px;color:#91b3d9}main{padding:20px 28px 40px}.filters{display:flex;gap:12px;margin-bottom:16px}.filters select{flex:1;min-width:0}.inspector{border:1px solid #354b68;border-radius:10px;overflow:hidden;background:#101a29;margin-bottom:24px}.inspector h2{font-size:18px;margin:14px 18px}.hero{display:block;max-width:100%;max-height:72vh;margin:auto;object-fit:contain;background:#05080d}.details{padding:12px 18px;font-size:13px;color:#b0c1d7;overflow-wrap:anywhere;line-height:1.7}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:16px}.card{background:#121d2d;border:1px solid #293c56;border-radius:9px;overflow:hidden;cursor:pointer}.card.selected{border-color:#82baff}.card h3{font-size:14px;margin:10px 12px;overflow-wrap:anywhere}.card img{width:100%;height:180px;object-fit:contain;background:#070b12;display:block}.card .info{font-size:12px;padding:9px 12px;color:#a9bad0}.missing{padding:45px 16px;color:#dfb97d;text-align:center}.empty{display:none}code{font:12px ui-monospace,monospace}details{margin:8px 0}summary{cursor:pointer}#notice{color:#efce91}.keyhint{font-size:12px;color:#91a4bc}@media(max-width:700px){header,.bar,main{padding-inline:14px}.controls{flex-wrap:wrap}.controls input{flex-basis:100%}.ticks{margin-left:0}.filters{flex-wrap:wrap}.filters input{width:100%}}
button{white-space:nowrap;flex-shrink:0}.controls button{min-width:40px}.controls #play{min-width:64px}.controls strong{flex-shrink:0}.controls input{flex:1}.ticks{margin-left:310px;margin-right:8px}@media(max-width:700px){.ticks{margin-left:0;margin-right:0}}
</style>
<header><h1>本引擎原生前 6 帧：Pass 时间轴</h1>
<p>同一进程连续采集，未预热。拖动进度条时，所有 pass 同步切帧。<a href="all-passes.html">逐帧长页</a> · <a href="manifest.json">输出清单 / SHA</a> · <a href="gbuffer/index.html">GBuffer / culling</a></p></header>
<section class="bar"><div class="controls"><button id="prev" aria-label="上一帧">←</button><button id="play">播放</button><button id="next" aria-label="下一帧">→</button><strong id="frameLabel"></strong><input id="frameSlider" type="range" min="0" max="5" value="0" step="1" aria-label="采集帧"></div><div id="ticks" class="ticks"></div><div class="status" id="status" aria-live="polite"></div></section>
<main><div class="filters"><select id="outputSelect" aria-label="选择 pass 输出"></select><input id="search" type="search" placeholder="筛选 pass / 资源名称" aria-label="筛选 pass"></div>
<section class="inspector"><h2 id="selectedTitle"></h2><a id="heroLink" target="_blank"><img id="hero" class="hero" alt="所选 pass 截图"></a><div id="notice" class="missing empty"></div><div class="details" id="details"></div></section>
<p>以下为当前帧全部登记输出。点击卡片可在上方放大，所选 pass 会随进度条保持。<span class="keyhint">← / → 切帧；空格播放 / 暂停（输入框内除外）。</span></p>
<div id="grid" class="grid"></div>
<details><summary>截图与数据的含义</summary><p>ToneMapping_Output 是实际显示输出。HDR 纹理 PNG 使用固定 x/(1+x)、gamma 1/2.2 预览；深度单独映射，数值以原始纹理为准。3D 纹理显示中间切片。Buffer 图仅为前 64 KiB 的字节灰度预览，不能视作渲染图像。下载链接为逐 SHA 核验的 gzip 原始数据；逻辑路径与格式见 capture.json / manifest.json。</p><p>“全部”指实际执行并登记的 RenderGraph 输出和已接入的内部 dispatch；未执行的 pass 不伪造截图，未登记的私有分配不计入。帧槽后缀仅在选择器中合并，原始资源名、文件及 SHA 保留。</p></details>
</main><script id="timeline-data" type="application/json">__PAYLOAD__</script><script>
const data=JSON.parse(document.getElementById('timeline-data').textContent);
const el=id=>document.getElementById(id); const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const keys=[...new Set(data.outputs.map(r=>r.key))]; let index=0,timer=null;
let selected=keys.find(k=>k.includes('ToneMapping_Output'))||keys[0];
el('outputSelect').innerHTML=keys.map(k=>'<option>'+esc(k)+'</option>').join('');el('outputSelect').value=selected;
el('frameSlider').max=data.frames.length-1;el('ticks').innerHTML=data.frames.map(f=>'<span>'+f+'</span>').join('');
function stop(){clearInterval(timer);timer=null;el('play').textContent='播放'}
function select(key){selected=key;el('outputSelect').value=selected;render()}
function render(){const frame=data.frames[index],meta=data.metadata[frame],rows=data.outputs.filter(r=>r.frame===frame),current=rows.find(r=>r.key===selected);el('frameSlider').value=index;el('frameLabel').textContent='Frame '+frame;el('prev').disabled=index===0;el('next').disabled=index===data.frames.length-1;
 const successful=rows.filter(r=>r.status==='captured').length;
 el('status').innerHTML='原生引擎帧 '+meta.native+' · '+successful+'/'+rows.length+' 个输出已保存 · '+new Set(rows.map(r=>r.pass)).size+' 个采样 pass · HZB 遮挡 '+(meta.occlusion===null?'未记录':meta.occlusion?'开启':'跳过')+(meta.display_luma!==undefined?' · 平均显示亮度 '+meta.display_luma+'/255':'')+' · <a href="'+esc(meta.capture)+'">capture.json</a>'+(meta.culling?' · <a href="'+esc(meta.culling)+'">culling.json</a>':'');
 el('selectedTitle').textContent=selected+' · Frame '+frame;
 const visible=!!(current&&current.preview);el('heroLink').classList.toggle('empty',!visible);el('notice').classList.toggle('empty',visible);
 if(visible){el('hero').src=current.preview;el('hero').alt=current.pass+' / '+current.name+' Frame '+frame;el('heroLink').href=current.preview}else{el('hero').removeAttribute('src');el('notice').textContent=current?'该输出未保存：'+current.status:'此帧未执行 / 未登记该输出，未用其他帧填充。'}
 el('details').innerHTML=current?esc(current.name)+' · '+esc(current.preview_kind||current.status)+' · '+Number(current.bytes||0).toLocaleString()+' bytes · '+[current.width,current.height,current.depth].join(' × ')+' · format '+current.format+(current.download?' · <a href="'+esc(current.download)+'" download>下载原始数据 (.gz)</a>':'')+(current.preview?' · <a href="'+esc(current.preview)+'" target="_blank">原尺寸 PNG</a>':'')+'<br><code>'+esc(current.logical_path||current.file)+'</code><br>SHA-256: <code>'+esc(current.sha256||'—')+'</code>':'';
 const query=el('search').value.trim().toLowerCase(); const shown=rows.filter(r=>(r.pass+' '+r.name).toLowerCase().includes(query));
 el('grid').innerHTML=shown.map((r,i)=>'<article class="card '+(r.key===selected?'selected':'')+'" data-i="'+i+'" tabindex="0"><h3>'+esc(r.pass)+' / '+esc(r.name)+'</h3>'+(r.preview?'<img loading="lazy" src="'+esc(r.preview)+'" alt="'+esc(r.name)+'">':'<div class="missing">'+esc(r.status)+'</div>')+'<div class="info">'+esc(r.preview_kind||r.status)+' · '+Number(r.bytes||0).toLocaleString()+' bytes'+(r.download?' · <a href="'+esc(r.download)+'" download>raw.gz</a>':'')+'</div></article>').join('');
 el('grid').querySelectorAll('.card').forEach(card=>{const activate=e=>{if(e.target.closest('a'))return;select(shown[Number(card.dataset.i)].key);el('selectedTitle').scrollIntoView({block:'nearest',behavior:'smooth'})};card.addEventListener('click',activate);card.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();activate(e)}})});
}
function step(delta){index=Math.max(0,Math.min(data.frames.length-1,index+delta));render()}
el('frameSlider').addEventListener('input',()=>{stop();index=Number(el('frameSlider').value);render()});el('prev').onclick=()=>{stop();step(-1)};el('next').onclick=()=>{stop();step(1)};
el('play').onclick=()=>{if(timer){stop();return}if(index===data.frames.length-1)index=0;el('play').textContent='暂停';render();timer=setInterval(()=>{if(index===data.frames.length-1){stop();return}step(1)},800)};
el('outputSelect').onchange=()=>select(el('outputSelect').value);el('search').addEventListener('input',render);
document.addEventListener('keydown',e=>{if(['INPUT','SELECT','TEXTAREA','BUTTON'].includes(document.activeElement.tagName))return;if(e.key==='ArrowLeft'){e.preventDefault();stop();step(-1)}if(e.key==='ArrowRight'){e.preventDefault();stop();step(1)}if(e.code==='Space'){e.preventDefault();el('play').click()}});render();
</script></html>'''


if __name__ == '__main__': main()
