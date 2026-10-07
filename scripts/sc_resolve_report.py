#!/usr/bin/env python3
"""Generate a local, source-backed SC resolve experiment report. No gate changes."""
import argparse,html,json,hashlib,sys
from pathlib import Path
from PIL import Image
from sponza_region_lum import region_lums
p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--reference',required=True);p.add_argument('--stage',action='append',default=[],help='label=capture-directory');p.add_argument('--evidence',action='append',default=[]);p.add_argument('--existing-evidence',help='Reuse already embedded historical evidence; does not reopen or certify its old file paths');a=p.parse_args()
out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=True)
ref=Path(a.reference).resolve();Image.open(ref).convert('RGB').save(out/'lumen-reference.jpg');rv=region_lums(str(ref))
rows=[]
for i,item in enumerate(a.stage):
 label,directory=item.split('=',1);directory=Path(directory);cap=json.loads((directory/'capture.json').read_text());ppm=directory/'frame.ppm';assert ppm.is_file()
 Image.open(ppm).save(out/f'stage-{i}.png');v=region_lums(str(ppm));rows.append(dict(label=label,image=f'stage-{i}.png',capture=str(directory.resolve()),frame=cap.get('capture_frame',cap.get('frame')),values=v,capture_metadata=cap,ppm_sha256=hashlib.sha256(ppm.read_bytes()).hexdigest()))
evidence={}
if a.existing_evidence:
 historical=json.loads(Path(a.existing_evidence).read_text())
 evidence=dict(historical['evidence'])
for file in a.evidence:
 path=Path(file);evidence[str(path)]=json.loads(path.read_text())
data=dict(scope='fixed-exposure visual comparison; prioritize dark indirect lighting, color/texture and spatial hierarchy; transport attribution requires separate linear HDR diagnostics',reference=str(ref),reference_roi=rv,stages=rows,evidence=evidence)
(out/'evidence.json').write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n')
figures=''.join(f'<figure><img src="{r["image"]}"><figcaption>{html.escape(r["label"])} · 第 {r["frame"]} 帧</figcaption></figure>' for r in rows)
table='<tr><th>区域（显示亮度 0–255）</th><th>本次 Lumen 图片</th>'+''.join('<th>'+html.escape(r['label'])+f' · {r["frame"]}</th>' for r in rows)+'</tr>'
for key in rv:table+='<tr><td>'+key+'</td><td>'+f'{rv[key]:.2f}'+'</td>'+''.join(f'<td>{r["values"][key]:.2f} <small>Δ {r["values"][key]-rv[key]:+.2f}</small></td>' for r in rows)+'</tr>'
options=''.join(f'<option value="{r["image"]}"'+(' selected' if i==len(rows)-1 else '')+f'>{html.escape(r["label"])} · {r["frame"]}</option>' for i,r in enumerate(rows))
text='''<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SC resolve · Sponza 对照实验</title><style>body{background:#111722;color:#e2e7ef;font:16px/1.6 system-ui;margin:0 auto;padding:32px;max-width:1500px}h1{font-size:30px}p{max-width:1100px}figure{margin:0}img{width:100%;display:block}figcaption{padding:8px;color:#afbbca}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(400px,100%),1fr));gap:16px}table{border-collapse:collapse;width:100%;margin:24px 0}th,td{padding:10px;border-bottom:1px solid #354254;text-align:left}small{display:block;color:#a8bbd2}a{color:#8ac4ff}.comparison{position:relative;overflow:hidden;width:100%;aspect-ratio:16/9}.comparison img{position:absolute;height:100%;object-fit:fill}.comparison #overlay{clip-path:inset(0 50% 0 0)}input{width:100%}select{font:inherit;padding:6px}pre{white-space:pre-wrap;font-size:13px}.note{border-left:3px solid #dda558;padding:12px 20px;background:#202734}</style><h1>Vulkan Surface Cache 主 resolve 实验</h1><p>2026-10-03 · 固定曝光：adapt=0，EV=1.8546。使用真实当前帧补点 radiance、32 条辅助射线及原命中契约。此页随阶段更新，完整来源与审计见 <a href="evidence.json">evidence.json</a>。</p><p class="note">用户已确认对照为同一场景。本轮优先暗部间接光、颜色质感与空间层次；构图、材质不作为主要解释。归一化 ROI 显示亮度只作描述，另看真实间接 HDR 和来源闭合；历史六 ROI 参考常数保持原值，不以新图片替换门禁。</p><h2>阶段截图</h2><div class="grid"><figure><img src="lumen-reference.jpg"><figcaption>用户提供的 Unreal Lumen 参考图</figcaption></figure>FIGURES</div><h2>交互对照</h2><select id="stage">OPTIONS</select><div class="comparison"><img src="lumen-reference.jpg"><img id="overlay" src="FIRST"></div><input id="slider" type="range" value="50" min="0" max="100" aria-label="比较分界"><p>左侧为阶段截图，右侧为 Lumen 参考；拖动分界查看构图和明暗差异。</p><h2>差距分析</h2><p>暗部间接光：右侧廊道与近处地面仍偏亮，受光墙偏暗，亮暗分布没有接近参考。颜色与质感：本阶段未改变光传输输出，没有验证色彩或纹理层次改善。空间层次：暗廊道、地面与受光墙的层次差距仍在。新增SC命中及性能不能替代这三个视觉目标；当前先追踪实际间接来源、回退、方向与可见性，场景、材质、构图差异暂不作为主要排查方向。</p><div style="overflow-x:auto"><table>TABLE</table></div><h2>可验证证据</h2><p>各工作量计数仅覆盖该实验注明的抽样 oracle 路径，累计到捕获帧；并非完整生产工作量。GPU 时间为 BOTTOM_OF_PIPE 命令标记间隔，并非独占 kernel 时间。SC 阶段门禁和最早 P0 门禁分别报告。</p><details><summary>展开审计汇总（完整机器可读证据见 evidence.json）</summary><pre>EVIDENCE</pre></details><script>stage.onchange=()=>overlay.src=stage.value;slider.oninput=()=>overlay.style.clipPath=`inset(0 ${100-slider.value}% 0 0)`;</script></html>'''
outcomes=[]
for decision in evidence.values():
 if not isinstance(decision,dict) or 'decision' not in decision:continue
 if 'report_summary' in decision:
  outcomes.append('<p class="note">'+html.escape(decision['report_summary'])+'</p>');continue
 if 'wall_ms' not in decision or 'stages' not in decision:continue
 wall=decision['wall_ms'];st=decision['stages']
 outcomes.append(f'<p class="note">radiance采样前候选淘汰试验：<strong>丢弃，未观测到加速</strong>。关闭 / 开启 wall {wall["off"]:.2f} / {wall["on"]:.2f} ms/帧，main 标记间隔 {st["off"]["main"]:.2f} / {st["on"]["main"]:.2f} ms。旧选择、五主查询文件和十补点快照逐字节保持。临时代码与开关已全部回退；原20缺口仍恢复。最终 SC 阶段350/480 PASS、最早P0 FAIL。</p>')
text=text.replace('<h2>阶段截图</h2>',''.join(outcomes)+'<h2>阶段截图</h2>')
native=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'top_encoder_ordinals' in v),None)
if native:
 body=''.join(f'<tr><td>{row["ordinal"]}</td><td>{row["samples"]}</td><td>{row["median_active_union_ms"]:.3f}</td></tr>' for row in native['top_encoder_ordinals'][:6])
 block=f'<h2>原生驱动执行证据</h2><p>目标 pid {native["target_pid"]}，实际GPU区间 {native["target_gpu_rows"]} 条；按 command buffer/encoder ID 关联 Compute、depth=0、Active 记录，同encoder做时间区间并集，排除捕获边界帧。Shader Timeline关闭，shader采样表为空，现有encoder标签是通用名称；下面的ordinal尚未映射到命名SC pass，不能标作独占kernel时间。系统模板含系统元数据，此页仅使用目标pid过滤后的汇总。带profiler的时间不作为无profiler性能预算。</p><table><tr><th>encoder ordinal</th><th>样本数</th><th>Active区间并集中位数 ms</th></tr>{body}</table>'
 if native.get('sc_label_mapping',{}).get('mapped_encoders'):
  native_caption='该表来自非零轴分类前、完整card成员过滤与owner-ID掩码均开启的命名阶段；当前分类收益以ABBA为准，带profiler时间不作收益认证。' if native.get('measurement_stage')=='after-card-and-owner-filter' else '该表来自完整ID过滤前的历史SC命名诊断阶段；最新开关性能见ABBA证据。'
  mapping=native['sc_label_mapping'];body=''.join(f'<tr><td>{html.escape(row["label"])}</td><td>{row["ordinal"]}</td><td>{row["samples"]}</td><td>{row["median_active_union_ms"]:.3f}</td></tr>' for row in mapping['stages'])
  block=f'<h2>原生驱动执行证据</h2><p>{native_caption}显式SC_NATIVE_LABELS=1仅添加调试组。目标pid {native["target_pid"]}，{mapping["mapped_encoders"]}个SC调试组均按同pid/thread、组内唯一Compute encoder创建关联；再用command buffer/encoder ID关联GPU区间。仅统计内部帧的Compute、depth=0、Active区间并集。名称来自具体dispatch调用点，时间仍为encoder区间并集，不能宣称独占kernel时间。Shader Timeline关闭、无硬件counter；带profiler时间不作为性能预算。系统模板含其他进程元数据，本页仅发布目标pid汇总。连续消费模式已跳过基础resolve，没有两次主resolve。</p><table><tr><th>调用点调试标签</th><th>encoder ordinal</th><th>样本数</th><th>Active并集中位数 ms</th></tr>{body}</table>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
work=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'remaining_valid_percent' in v and 'current_after' in v),None)
if work:
 body=''.join(f'<tr><td>{html.escape(name)}</td><td>{row["before"]:,}</td><td>{row["after"]:,}</td><td>{row["reduction_percent"]:.2f}%</td></tr>' for name,row in work['reduced_counts'].items())
 current=work['current_after']['counters']
 remaining=''.join(f'<tr><td>{html.escape(name)}</td><td>{current[name+"_calls"]:,}</td><td>{current[name+"_valid"]:,}</td><td>{percent:.3f}%</td></tr>' for name,percent in work['remaining_valid_percent'].items())
 block=f'<h2>成员过滤后的候选工作量</h2><p>两项过滤开启后的新350帧诊断，累计{work["queries"]:,}次每37射线oracle查询，overflow=0。下表与历史过滤前诊断比较：五主查询文件及valid/光照来源数一致，但来自不同诊断binary；这是控制流计数，不能当作物理GPU访存或本次新性能收益。计数在回归前已全部移除。</p><table><tr><th>工作量</th><th>历史过滤前</th><th>当前过滤后</th><th>减少</th></tr>{body}</table><p>剩余支持候选的低valid比例引导下一步测量几何距离/范围失败；尚未给这些函数独立计时，不能仅凭次数认定GPU耗时占比。</p><table><tr><th>候选</th><th>尝试</th><th>valid</th><th>比例</th></tr>{remaining}</table>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
segment=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'bounds_rejectable_percent' in v),None)
if segment:
 c=segment['counters'];body=''.join(f'<tr><td>{html.escape(name)}</td><td>{count:,}</td></tr>' for name,count in c.items())
 block=f'<h2>线段失败分类与保守范围试验</h2><p>新350帧累计抽样{c["calls"]:,}次线段支持，{segment["distance_reject_percent"]:.3f}%因距离超限失败；非法输入、退化/非有限均0。端点范围必要测试可拒绝{segment["bounds_rejectable_percent"]:.3f}%，原valid反例0，仅限本次样本。不同于硬件指令/纹理事务或独占时间；原最终0.05m条件保持。该测试放在两世界端点构建后，仍增加判断成本。同binary/SPIR-V关闭275.142、开启275.479 ms/帧，无观测收益，已完整丢弃。诊断与试验源码保存在增量补丁，当前生产无这些开关或计数。</p><table><tr><th>分类</th><th>抽样累计</th></tr>{body}</table>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
texel=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'rejectable_percent' in v and 'total_rejectable' in v),None)
if texel:
 c=texel['counters'];body=''.join(f'<tr><td>{kind}</td><td>{c[kind+"_calls"]:,}</td><td>{c[kind+"_rejectable"]:,}</td><td>{c[kind+"_valid"]:,}</td><td>{c[kind+"_false_reject"]:,}</td></tr>' for kind in ['wide','hull','index'])
 block=f'<h2>世界端点构建前的 texel 范围试验</h2><p>新350帧抽样{texel["total_calls"]:,}对，利用原0.05m查询矩形及既有一texel guard，在坐标精度条件满足时进行整数必要条件检查。大平移/小extent/非有限计算退回原路径。本次{texel["total_rejectable"]:,}对（{texel["rejectable_percent"]:.3f}%）可拒绝，原valid反例0、overflow=0；诊断仍完整执行原helper。此处只是抽样控制流，零反例不等于全场景证明。去掉计数后同binary/SPIR-V首轮off/on无观测加速，试验已丢弃；既有成员过滤保留。</p><table><tr><th>候选路径</th><th>尝试</th><th>可拒绝</th><th>原valid</th><th>误拒绝原valid</th></tr>{body}</table>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
bvh=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'node_box_fail_percent' in v),None)
if bvh:
 c=bvh['counters'];body=''.join(f'<tr><td>{kind.upper()}</td><td>{c[kind+"_nodes"]:,}</td><td>{c[kind+"_box_pass"]:,}</td><td>{bvh["node_box_fail_percent"][kind]:.3f}%</td><td>{c[kind+"_stack_peak"]}</td></tr>' for kind in ['tlas','blas'])
 block=f'<h2>两层BVH真实遍历控制流</h2><p>新350帧当前主trace每37射线抽样{bvh["rows"]:,}条：hit {bvh["status"]["hit"]:,}、miss {bvh["status"]["miss"]:,}、unknown {bvh["status"]["unknown"]}；{bvh["main_first_hit_distance_status_bits_match"]:,}条首命中距离/状态与主resolve记录逐位一致。实例变换{c["instance_transforms"]:,}次、三角形尝试{c["triangles"]:,}次，拒绝分类与更新次数加总闭合。这是当前抽样GLSL控制流，不是物理访存/指令/独占时间，不包含补点trace；计数在性能试验前移除。</p><table><tr><th>树</th><th>节点尝试</th><th>AABB通过</th><th>未通过</th><th>栈峰值</th></tr>{body}</table><p>延迟links读取试验已因无收益丢弃；非零方向分类的保留与覆盖缺口分类见本页最新证据。不能仅凭源码读取次数声称硬件加速。</p>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
direction=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'world_direction_with_zero' in v),None)
if direction:
 block=f'<h2>方向零分量结构盘点</h2><p>对冻结{direction["sampled_rays"]:,}条主射线和PBVH完整场景的{direction["scene_counts"]["instances"]}个实例逆变换，payload checksum/文件source键核验通过。world含零{direction["world_direction_with_zero"]}，单轴置换/缩放结构的全部{direction["all_sample_instance_combinations"]:,}组合local含零{direction["local_direction_with_zero_all_combinations"]}；最小绝对分量{direction["minimum_absolute_world_direction"]:.8g}。这是CPU缓存/冻结数据结构盘点，不是新GPU动态分支计数或时间。优化仍在实际GLSL每ray/instance验证方向，零/非有限回原路径，逐轴减法/除法/near-far次序保持，没有倒数乘法或世界位置缓存。</p>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
nearest=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'nearest_work_pair' in v),None)
if nearest:
 pair=nearest['nearest_work_pair'];a=pair['original']['raw']['counters'];b=pair['expanded']['raw']['counters']
 body=''.join(f'<tr><td>{html.escape(k)}</td><td>{a[k]:,}</td><td>{b[k]:,}</td><td>{b[k]/max(1,a[k]):.3f}×</td></tr>' for k in ['calls','directory_hit','nodes','points','linear_comparisons','full_rejected','source_attempts','source_valid','fan_triangles','hull_edges'])
 block=f'<h2>混合索引检索与插值的真实抽样开销</h2><p>原SCB1/扩充SCB1用同诊断binary/SPIR-V累计主resolve每37射线oracle实际GLSL路径。两组各自六份主查询/atlas/index文件与既有对应来源逐字节一致，七项审计通过；这不是全生产查询、补点radio resolve、物理访存或独占kernel时间。诊断计数使用64位累计并在性能试验前完整移除。满nearest32后最终落选候选的线性扫描占比为{pair["original"]["full_rejected_scan_share_percent"]:.2f}%/{pair["expanded"]["full_rejected_scan_share_percent"]:.2f}%，不能把该比例当帧时间收益。125帧上传目录结构原/扩充分别只有5/19张card有组、2527/2513张为空；这是结构盘点，不是动态空card查询计数。</p><table><tr><th>工作量</th><th>原来源</th><th>扩充来源</th><th>倍率</th></tr>{body}</table><p>新增合法支持增加了source验证与凸包插值工作，本轮优先验证空card提前拒绝是否有稳定收益；完整射线、候选排序、距离和坏数据拒绝契约均保留。</p>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
coverage=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'top_miss_owners' in v and 'counts' in v),None)
if coverage:
 c=coverage['counts'];body=''.join(f'<tr><td>{html.escape(name)}</td><td>{c.get(key,0):,}</td></tr>' for name,key in [('真实几何hit','geometry_hit'),('SC接受','SC_accepted'),('SC miss','SC_miss'),('miss：owner无card','miss_owner_has_no_cards'),('miss：owner atlas无此TriangleID','miss_triangle_absent_in_owner_atlas'),('miss：ID有但无有限非负depth','miss_triangle_has_no_finite_nonnegative_depth'),('miss：ID有有限depth来源','miss_triangle_has_finite_depth_support')])
 block=f'<h2>剩余SC覆盖来源</h2><p>当前关闭组冻结主query的atlas整数/有限depth盘点，连接此前BVH owner/raw-ID记录；8190个t/sky-hit状态位核对一致。这是既有读回的CPU重分析，不冒充新GPU identity捕获。{c["geometry_hit"]:,}次几何hit中SC接受{c["SC_accepted"]:,}、仍有{c["SC_miss"]:,}次miss。ID缺席与有来源但无法形成有效查询分别需继续查烘焙覆盖及face/域/支持插值/补点；“有有限depth”不等于有效光照或合法插值。这些数不能直接归因Lumen图像能量差距，尚无匹配视角/材质的线性分量证明。</p><table><tr><th>分类</th><th>抽样query数</th></tr>{body}</table>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
source=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'primary_meshes' in v and 'counts' in v),None)
if source:
 c=source['counts'];body=''.join(f'<tr><td>{html.escape(name)}</td><td>{c.get(key,0):,}</td></tr>' for name,key in [('离线绑定页有ID，runtime atlas缺席','present_in_bound_source_page_but_absent_runtime_atlas'),('离线绑定页也缺席','absent_in_all_bound_source_pages'),('无静态绑定card','no_static_bound_cards')])
 block=f'<h2>缺席ID的生成来源分类</h2><p>2190条缺席query重新读取实际SCS1原始页，以GPU card的slot/page绑定，并核验PBVH mapping几何摘要。{source["primary_meshes"]}条SCS1记录；CPU重分析，不是新GPU source平面或first-hit身份捕获。owner16的424条缺席中403条离线源存在、21条源也缺席。该类优先查分辨率/目标中心重采样的覆盖损失；无静态绑定类不能混称重采样bug。</p><table><tr><th>来源</th><th>query</th></tr>{body}</table><figure><img style="max-width:720px" src="coverage-owner16-source-atlas.png"><figcaption>owner16前两张绑定页：左为原SCS1 ID平面，右为GPU atlas ID平面；洋红ID存在源页但缺席全部runtime card。展示离散ID和分辨率，未做屏幕/世界几何配准。</figcaption></figure>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
domain=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'counts' in v and 'eligible_cards_lack_target_ID' in v['counts']),None)
if domain:
 c=domain['counts'];body=''.join(f'<tr><td>{html.escape(name)}</td><td>{c.get(key,0):,}</td></tr>' for name,key in [('合法face/投影域的card缺此ID','eligible_cards_lack_target_ID'),('整card同ID凸包也超过0.05m','no_full_card_hull_support_within_005'),('整card凸包可能在0.05m内','coplanar_full_card_hull_within_005')])
 block=f'<h2>有深度源仍失败的几何支持分类</h2><p>2312条query用float32原hit构建、CPU float64几何重算face/投影域及整card同ID点集；仅平面残差≤1e−4纳入凸包支持。不是原GLSL浮点逐位复放、真实radiance或全场景证明；原0.05m保持。仅2条可能由更广凸包恢复，不能据此任意扩大窗口。主要缺口需要合法face的真实表面来源或边界支持。</p><table><tr><th>支持</th><th>query</th></tr>{body}</table>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
source_points=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'owners' in v and 'original_boundary_samples_byte_preserved' in v),None)
source_hits=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'all_previous3256_card_cacheT_bits_preserved' in v),None)
if source_points and source_hits:
 body=''.join(f'<tr><td>{row["owner"]}</td><td>{row["samples"]:,}</td><td>{row["target_IDs"]:,}</td><td>{row["max_point_triangle_error"]:.8g}</td><td>{source_hits["owners"].get(str(row["owner"]),0)}</td></tr>' for row in source_points['owners'])
 block=f'<h2>真实原源点补充：覆盖改善与成本</h2><p>原SCB1四记录样本逐字节保留，独立实验文件追加owner16/18/7的原页点，仅选该card的runtime缺席ID；不按8190射线挑点，不改原项目文件。CDB2布局/原始几何摘要及实际原三角形位置验证。主query 3256→3569新增313、旧3256 card/cacheT位模式全部保持，20缺口恢复；243条来自原ID缺席、70条来自原有源但支持不足。仍有4189几何hit没有SC。独立数据文件见<a href="source-support-owner16-18-7.scboundary">实验SCB1</a>，生成工具精确源在source-support-tools.patch。</p><table><tr><th>owner</th><th>追加源点</th><th>原ID</th><th>最大原三角形误差m</th><th>新增实际query</th></tr>{body}</table><p>GPU按card复制后110979条，32射线/点保持，共3551328条。原16K混合索引/缓存/radiosity预算与点打包131072上限不一致，最初试验125帧失败，未取得有效350图；已统一共享有界上限，越界仍失败清空。新增源不作为性能优化：相同binary/SPIR-V原文件260.062/扩充531.228ms（单对慢104.270%，非ABBA/Release）；原光照快照不宣称等价。151–331完整181帧BOTTOM_OF_PIPE标记中位：trace44.272→219.742ms，radio resolve13.216→93.074ms，main94.406→110.519ms；这些是命令标记间隔，不能称独占kernel时间。资源RSS是宿主观测，不能当GPU分配核算。普通帧仍当前radiance、三槽fence复用，无新增生产读回/全局等待。</p><p>GPU输入110979条与当前SCS1三摘要及SCB1引用全部匹配，native/world一致被显式验证；PCF110979/110979，直射误差1.709e−5、diagnostic radiance误差2.520e−6。本配置probe feed关闭/输出0，不当作DDGI供给认证；完整光照由当前radiosity/cache证据补齐。所有射线积分/身份/天空核验，float64首命中2048/2048抽查，非全355万首命中或全部cache radiance独立复放。Unit7/7、SC218/218、GPU112/112＋13双路径及Vulkan350/480/Metal65秒通过；SC阶段PASS/P0FAIL。保留预算一致性修复，源文件独立实验，未默认推广或宣称Lumen等价。</p><p>350帧显示ROI：右廊道原/扩充24.504/24.575，参考8.908；近地77.078/77.121，参考59.454；直射斑156.250/156.396，参考162.010。覆盖增量并未闭合主视觉差距，右廊道与近地仍偏亮。125帧近地接近参考属于不同收敛阶段，不能替代350/480验收。以上是归一ROI显示值，不是匹配材质/构图的线性GI归因。</p>'
 text=text.replace('<h2>可验证证据</h2>',block+'<h2>可验证证据</h2>')
# Keep full evidence.json intact; bound only the inline preview to avoid a huge DOM.
def inline_preview(value,depth=0):
 if depth>6:return {'preview_omitted':True,'full_source':'evidence.json'}
 if isinstance(value,list):
  preview=[inline_preview(v,depth+1) for v in value[:3]]
  return preview if len(value)<=3 else {'items':len(value),'first_three_preview':preview,'full_source':'evidence.json'}
 if isinstance(value,dict):
  result={k:inline_preview(v,depth+1) for k,v in list(value.items())[:32]}
  if len(value)>32:result['additional_keys_in_evidence_json']=len(value)-32
  return result
 if isinstance(value,str) and len(value)>1000:return value[:1000]+' [preview truncated; evidence.json retains full text]'
 return value
for k,v in [('FIGURES',figures),('OPTIONS',options),('FIRST',rows[-1]['image'] if rows else 'lumen-reference.jpg'),('TABLE',table),('EVIDENCE',html.escape(json.dumps(inline_preview(evidence),indent=2,ensure_ascii=False)))]:text=text.replace(k,v)
(out/'index.html').write_text(text)
print(out/'index.html')

radio=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'radio_bvh_work_pair' in v),None)
if radio:
 pair=radio['radio_bvh_work_pair'];a=pair['original']['aggregate'];b=pair['expanded']['aggregate']
 names=['tlas_nodes','instance_transforms','blas_nodes','triangles','u_reject','containment_reject','best_updates']
 body=''.join(f'<tr><td>{html.escape(k)}</td><td>{a["counters"][k]}</td><td>{b["counters"][k]}</td><td>{a["mean_per_sample"][k]:.3f}</td><td>{b["mean_per_sample"][k]:.3f}</td></tr>' for k in names)
 extra='<h2>辅助 trace 两层 BVH 实际工作量</h2><p>原/扩充125帧分别每37射线抽样12487/95982条，实际全量462016/3551328条射线未减少。节点闭树恒等式、三角形分支分区和全部首命中/缓存 payload 已审计。计数只描述抽样GLSL控制流，不是硬件流量或耗时；计数代码在性能实验前移除。未消费DDGI字段及零card_count bounds差异单独记录。</p><table><tr><th>计数</th><th>原累计</th><th>扩充累计</th><th>原每样本</th><th>扩充每样本</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

sourceSort=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'source_sort_work_pair' in v),None)
if sourceSort:
 pair=sourceSort['source_sort_work_pair'];a=pair['original']['source_sort'];b=pair['expanded']['source_sort']
 names=['valid','shifts','original_shifted_scalar_components','hypothetical_index_shifted_scalar_components','hypothetical_extra_order_insertions']
 body=''.join(f'<tr><td>{html.escape(k)}</td><td>{a[k]}</td><td>{b[k]}</td></tr>' for k in names)
 extra='<h2>source有效tuple排序实际移动</h2><p>每37射线main oracle累计：原/扩充每有效source平均移动6.982/3.612次。旧positions/radiance/projected/ref每次移动9个32bit分量。计数只描述GLSL赋值，不是编译后指令、物理流量或耗时。20快照契约exact，radio-resolve params只word29诊断bit65536不同；其他31字、六main文件及前15类work计数保持。性能试验前移除计数，复用已消费offsets作稳定索引排序，不新增世界坐标缓存数组。</p><table><tr><th>计数</th><th>原</th><th>扩充</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

cacheCoverage=next((v for v in evidence.values() if isinstance(v,dict) and v.get('points')==110979 and 'all_invalid_cache_exact_rejection8_and_measurements_below4' in v),None)
queryInventory=next((v for v in evidence.values() if isinstance(v,dict) and 'rejected_with_any_current_valid_cache' in v),None)
if cacheCoverage:
 body=''.join(f'<tr><td>{owner}</td><td>{row["points"]:,}</td><td>{row["valid_cache"]:,}</td><td>{row["insufficient_measurements"]:,}</td></tr>' for owner,row in cacheCoverage['by_source_owner'].items())
 extra='<h2>当前补点光照覆盖：已定位的拒绝链路</h2><p>实际125帧全量110979点×32射线：13301个cache拒绝项严格是reject8且有效光照测量少于4。所有当前base light有效，32个estimator slot也有效；这不能把几何SC缺测当作零光照。无效点的411722个几何首命中没有SC光照测量，其中目标owner16占302790。辅助resolve仍从既有atlas取得几何光照，缺测使新源无法通过有效cache门禁。未放宽>=4、分母32或任何视觉门禁，尚未实现此链路的覆盖修复。</p><table><tr><th>source owner</th><th>点数</th><th>有效cache</th><th>不足4条测量</th></tr>'+body+'</table>'
 if queryInventory:
  extra+=f'<p>同位置/身份bits连接GPU查询与所有重复源点：263个精确源位置拒绝中，{queryInventory["rejected_with_no_current_valid_cache"]}个没有有效cache、{queryInventory["rejected_with_any_current_valid_cache"]}个至少存在一个有效cache。后者还需定位card域/nearest32/有效凸包；不能把所有miss归因cache不足。这是GPU结果关联分类，不是独立重放全部插值阶段。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)


queryStages=next((v for v in evidence.values() if isinstance(v,dict) and v.get('valid_cache_rejected_with_exact_query_source')==164),None)
projection=next((v for v in evidence.values() if isinstance(v,dict) and 'collapsed_full_groups' in v),None)
if queryStages:
 body=''.join(f'<tr><td>{html.escape(k)}</td><td>{v}</td></tr>' for k,v in queryStages['classification'].items())
 extra='<h2>源位置拒绝：GPU阶段证据</h2><p>独立诊断invocation逐分支记录，全部2658原查询和16-word原结果与前捕获exact。164个源位置query都在实际nearest集合中包含位置bits完全相同、当前有效的源，因此这些样本不属于card域或nearest遗漏；旧支持构造仍拒绝。诊断helper复制自生产且生产未改，诊断/原status全部相同；不是全生产像素或独立首命中oracle。诊断当前已经移除。</p><table><tr><th>实际GPU拒绝分类</th><th>样本</th></tr>'+body+'</table>'
 if projection:
  extra+='<p>CPU实际组位置几何盘点：106个样本的card投影轴把3D支持压成近似线，其中72个完整源组不超过32点；其余使用CPU距离排序，不宣称GPUnearest逐位复放。89个other_no_interpolation均在此类，另外包括16个边域失败和1个退化边样本。该关联指向投影轴选择，尚未证明改轴即可闭合全部命中或Lumen差距。下一步验证旧查询失败后的合法表面投影支持，旧选择/0.05m/真实当前光照保持；单点和微小边另列。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)


projectionTrial=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('main_added')==17 and 'rows' in v and 'pairs' in v),None)
if projectionTrial:
 body=''.join(f'<tr><td>{html.escape(row["mode"])}</td><td>{row["wall_ms"]:.3f}</td><td>{row["markers"]["main"]:.3f}</td><td>{row["SC"]}</td></tr>' for row in projectionTrial['rows'])
 extra='<h2>正确表面投影试验：覆盖增量与成本</h2><p>旧atlas及全部旧boundary候选都失败后才重试，保持旧选择优先级。实际8190主query3569→3586新增17，旧3569全16words逐字节相同；2658独立GPU proof2144→2368新增224，107个源位置拒绝恢复。2368全部接受项按各自当前快照独立复算身份、非负权重、source/radiance/witness，新增最大世界误差0.028345m，原0.05m保持。最近32与全32射线保持，没有直接点替代或外推。</p><table><tr><th>同binary/SPIR-V</th><th>wall ms</th><th>main标记 ms</th><th>SC实际query</th></tr>'+body+'</table><p>两对wall慢1.829%/1.326%，main标记慢6.435%/6.739%，没有性能收益，完整回退。时间戳是BOTTOM_OF_PIPE命令区间，不是独占kernel。换轴路径重复nearest/source/sort并重新排序/构建hull，源码成本不能当作已测硬件占比。</p><p>新增主SC支持进入既有多反弹回路，跨开关lookup-lighting/radiosity/cache的部分radiance有变化；20份快照不宣称全部等价。几何输入、全部真实辅助ray/first-hit/rawID、source/cache身份状态、测量counts和validity保持；原GPU proof跨捕获status/witness/source/weights exact，当前光照逐份复算，同快照旧接受全部16字保持。旧选择和cacheT检查未放宽，未改光照参数、曝光或门禁。本轮实现已移除，覆盖修复仍未保留。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

binaryWork=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'binary_insert_work' in v),None)
if binaryWork:
 body=''.join(f'<tr><td>{html.escape(k)}</td><td>{v["binary_insert"]["linear_comparisons"]}</td><td>{v["binary_insert"]["comparisons"]}</td><td>{v["binary_insert"]["reduction_percent"]:.3f}%</td></tr>' for k,v in binaryWork['binary_insert_work'].items())
 extra='<h2>nearest32：插入比较的真实工作量</h2><p>新原/扩充来源诊断使用同一binary/SPIR-V。主oracle每37条射线在同一实际排序列表上计算线性与二分插入位置，每个有效点逐次校验位置相同；六主文件exact、七审计通过。整数二分保持严格distance/source谓词，等tuple排在已有tuple之后，浮点距离、KD遍历、32点数量及移动操作不改。</p><table><tr><th>来源</th><th>线性比较次数</th><th>二分比较次数</th><th>比较减少</th></tr>'+body+'</table><p>这些是抽样GLSL比较计数，不是全部生产invocation、硬件指令、物理流量或kernel时间。计数已在性能对照前移除，不能从比较减少推断帧时间收益；性能保留/回退以新开关对照为准。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

binaryTrial=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('binary_insert_trial') is True),None)
if binaryTrial:
 body=''.join(f'<tr><td>{html.escape(v["mode"])}</td><td>{v["wall_ms"]:.3f}</td><td>{v["markers"]["main"]:.3f}</td><td>{v["SC"]}</td></tr>' for v in binaryTrial['rows'])
 gains=' / '.join(f'{v["wall_improvement_percent"]:.3f}%' for v in binaryTrial['pairs'])
 main=' / '.join(f'{v["main_improvement_percent"]:.3f}%' for v in binaryTrial['pairs'])
 extra='<h2>二分插入：无计数同版本 ABBA</h2><table><tr><th>同binary/SPIR-V</th><th>wall ms</th><th>main标记 ms</th><th>SC实际query</th></tr>'+body+'</table><p>wall两对改善'+gains+'，main命令区间改善'+main+'。四组全部8190主记录16words与20份125当前快照契约文件逐字节一致，当前真实radiance及完整3551328辅助ray/hits/raw IDs保持；params仅word29开关bit不同，不能称21文件全相同。最近32、32辅助ray、原浮点运算、优先级及非负支持/0.05m不变。保留为默认关闭PRIMAL_SC_BOUNDARY_BINARY_INSERT=1；独立扩充来源收益不泛化为全场景/Release。GPU80/80两路径状态/count/source相同，原CPU与negative预期保持。</p><p>初版诊断结果缓冲仅分配40条而派发80条，初始化失败，无性能/350截图；修正尺寸后用新目录采集，不归因生产算法或驱动、不放宽预期。所有计数在ABBA前已移除。最终回归切回仓库原始SCB1，原来源覆盖及Lumen差距分别验收。BOTTOM_OF_PIPE标记不是独占kernel；光照/曝光/门禁均未改变。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

reuseWork=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('all16_duplicate_and_reused_results_bit_identical') is True and 'counts' in v),None)
if reuseWork:
 body=''.join(f'<tr><td>{html.escape(k)}</td><td>{v["nearest_calls"]}</td><td>{v["source_attempts"]}</td><td>{v["original_sort_shifts"]}</td><td>{v["alternate_sort_shifts"]}</td></tr>' for k,v in reuseWork['counts'].items())
 extra='<h2>同帧已验证源集合：复用的 GPU 证据</h2><p>2658组三条独立invocation：原查询、旧失败后重读、旧失败后复用。重读与复用的全部16word结果相同；旧2144成功逐字保持，均2368接受、新增224。全部当前source/身份/非负权重/radiance/world witness由CPU float64独立复算通过。只是125帧proof，不是所有生产query或独立首命中oracle。</p><table><tr><th>proof路径</th><th>nearest实际调用</th><th>source验证尝试</th><th>原排序移动</th><th>换轴排序移动</th></tr>'+body+'</table><p>复用实际少509次nearest、7099次source验证、5457次有效source验证和28775次原排序移动；换轴选择/新排序/凸包的逐query工作量相同。不是物理访存、全生产工作量或kernel时间，不能推断帧时间收益。诊断生产七SPIR-V完整SHA保持；计数与重读对照已在生产试验前移除。新投影候选先暂存，仅当全部旧owner候选失败才提交，防止抢占后来card的旧成功。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

reuseTrial=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('actual_main_added')==17 and 'rows' in v),None)
if reuseTrial:
 body=''.join(f'<tr><td>{html.escape(v["mode"])}</td><td>{v["wall_ms"]:.3f}</td><td>{v["markers"]["main"]:.3f}</td><td>{v["SC"]}</td></tr>' for v in reuseTrial['rows'])
 extra='<h2>复用投影：正确命中仍伴随成本，完整回退</h2><p>修正版同binary/SPIR-V首对OFF/ON，binary插入均ON、独立扩充SCB1相同：</p><table><tr><th>条件</th><th>wall ms</th><th>main标记 ms</th><th>实际8190 query SC</th></tr>'+body+'</table><p>wall慢6.913%，main命令区间慢44.808%，无性能收益完整回退；仅一对，不称稳定ABBA或独占kernel认证。实际3569→3586新增17与前合法late retry全部16字相同，旧3569全16字保持。换轴结果先暂存，在全部旧owner候选失败后才提交，未改变旧优先级、32点/32辅助ray/真实当前radiance/非负权重/0.05m。当前五文件及七生产SPIR-V恢复到之前保留的binary插入版本，本轮覆盖修复未保留。</p><p>强独立proof拒绝了初版计数移除脚本的返回契约错误：误删if控制的空语句，导致失败尾部缺少return，产生73个非法接受。OFF主路径保持且ON尚未启动，未把该版画面/时间当成功验收；修正后重建新目录，2368全部来源/权重/radiance/witness复算通过，旧成功16字保持。未放宽检查、不归因GPU驱动或原算法。新增命中进入旧多反弹回路，跨开关105个proof radiance words变化单列，不宣称全部20/21光照快照等价；几何、identity/count/validity保持。</p><p>复用已消除证明的第二次nearest/source/原排序，但在各card旧失败时先计算alternate会有额外工作；是否被后续旧成功丢弃、原hull退化/新hull有效的实际生产分布仍未测清。下一步先量化这部分，不按新增17命中推断真实工作量。Lumen/全覆盖/Release验收仍未闭合。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

hullWork=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'hull_push_pop_and_fan_edge_conservation_verified' in v),None)
if hullWork:
 body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+str(v['queries'])+'</td><td>'+str(v['with_projected_equal_world_different'])+'</td><td>'+str(v['counters']['sort_shifts'])+'</td><td>'+str(v['counters']['hull_pops'])+'</td></tr>' for k,v in hullWork['partitions'].items())
 extra='<h2>Hull 失败与实际工作量分层</h2><p>新采2658条GPU显式proof，原/计数helper全部16-word返回exact，push/pop与fan/edge次数守恒。258条失败hull仅两个顶点，其中205条含投影同坐标但不同世界位置的source；631条成功query也含这种重合，所以不可仅据重合提前换轴。card重建和UV源码均使用世界轴，没有独立旋转基底，漏用card变换的假设未获支持。生产七SPIR-V保持；只描述该显式proof控制流，不是全部主query、物理traffic或kernel耗时，也不宣称新增覆盖或视觉改善。</p><table><tr><th>分类</th><th>queries</th><th>含不同世界位置重合投影</th><th>排序移动</th><th>hull弹出</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

sourceBinaryWork=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'every_source_binary_slot_equal_to_original_linear' in v),None)
if sourceBinaryWork:
 c=sourceBinaryWork['counters'];extra='<h2>Source tuple 插入查找实际比较</h2><p>当前2658 GPU显式proof中，'+str(c['source_valid'])+'次有效source插入位置全部一致，原/计数全部16-word返回exact。线性比较'+str(c['linear_comparisons'])+'、二分dry-run比较'+str(c['binary_comparisons'])+'，减少'+format(sourceBinaryWork['comparison_reduction_percent'],'.3f')+'%。原排序移动'+str(c['sort_shifts'])+'及hull/插值不变；不是全部生产query、硬件指令/traffic或已证明的时间收益。性能前完整移除计数，以同binary/SPIR-V对照纯插入位置查找开关。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
sourceBinaryPerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'source_binary_performance' in v),None)
if sourceBinaryPerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in sourceBinaryPerf['rows'])
 extra='<h2>Source二分 无计数性能对照</h2><p>'+html.escape(sourceBinaryPerf['decision'])+'。全部8190主记录/20当前快照/2658独立GPU source插入pairs逐字保持。时间戳是BOTTOM_OF_PIPE命令区间，不能称独占kernel耗时；曝光、射线数、当前radiance和门禁不变。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

hull2Work=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'restriction_only_after_old_failure_and_hull2' in v),None)
if hull2Work:
 a=hull2Work['counts']['broad'];b=hull2Work['counts']['restricted'];names=['alternate_entered','selector_iterations','alternate_sort_shifts','alternate_fan','alternate_edges','reprojected_points','alternate_accepted']
 body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+str(a[k])+'</td><td>'+str(b[k])+'</td></tr>' for k in names)
 extra='<h2>仅失败 hull=2 才换轴：条件不足，不进入生产</h2><p>新2658 GPU三路独立proof：旧2144成功全部16-word保持，广泛换轴2368、hull2限制2286；限制节省28.926% selector构建，却丢失82个已独立source/权重/radiance/witness验证的正确修复（原hull3/4/6分别36/45/1，owner7/18/16分别61/17/4）。原最近集合/源验证/排序工作每query相同，受限接受全部16words为广泛接受的exact子集。该条件不足以恢复完整支持，因此不接入生产、不把删掉82候选当性能优化。生产七SPIR-V和主选择保持，没有生产性能试验/新覆盖或视觉收益。后续须保留完整支持再降低构建成本。计数只描述当前显式proof，不是全部main/硬件traffic/kernel时间。</p><table><tr><th>实际计数</th><th>广泛换轴</th><th>仅失败hull2</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

axisProbe=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'axis_mismatches' in v and 'GPU_selector_iterations' in v),None)
if axisProbe:
 extra='<h2>整组投影轴尚不能直接替换局部选择器</h2><p>新2658 GPU proof记录363个失败后的实际局部轴；CPU整组索引几何预选与其中328项相同、35项不同（owner7/18/16为3/5/27）。3个新增接受集中在card35/TriangleID794：整组94点接近平面，当前32近邻的索引几何近似直线，局部GPU选x、整组CPU选y。35项不一致不能直接称为丢失命中，但已否定无条件逐轴等价假设。旧2144接受全部16-word保持，广泛2368接受均独立复算；生产仍3569/原20缺口保持。诊断未进入生产，没有性能或视觉收益。CPU点积/排序及索引位置不是GLSL逐位最近集合证明，需下一轮显式GPU支持集合/轴替换结果对照。counter5合计14988包含10076次farthest/cross和4912次重投影排序外层迭代，仅显式proof控制流，不能称独占kernel时间。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

groupAxis=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'lost_local_acceptances' in v and 'hint_geometry_selector_iterations' in v),None)
if groupAxis:
 extra='<h2>实际GPU整组轴替换与恒定平面捷径</h2><p>原2144成功全部16-word保持，局部换轴/整组轴均2368接受（+224）、无丢失或新增差额。但整组轴替换改变query1143/1144/1145的来源/权重/当前radiance，最大radiance差0.0264042；因此不能作为保持局部候选优先级的等价优化。实际GPU nearest32、当前valid源位置/radiance、normal/area明确记录并独立复算；三组当前源x范围0、y范围9.537e−7m、z约1.693881m，细微y误差产生x法线，而整组平面hint选y。生产仍3569/原20缺口保持，+224只是显式proof。</p><p>新四路GPU proof中的恒定坐标平面条件：只在已有有效集合的一个坐标完全相等、原farthest有限且处于(1e−12,1e16]、所选axis前的方向分量绝对值≥1e−15时直接形成与原normal/collinear优先级相同的轴，否则原cross扫描。127个query少1123次cross循环，全部2658×16-word与局部原路径exact。未使用whole-group hint、未增加世界点数组或减少射线。附加分类比较/分支尚未计时；仅GPU proof等价，不是生产性能或视觉收益。下一步需无计数生产开关与同binary/SPIR-V性能对照。</p><p>计数口径纠正：旧counter5=14988包含10076次farthest/cross和4912次重投影排序外层迭代，受限8716包含5846+2870；本轮逐query守恒核对。整组hint的counter5=4886全部为排序外层。原JSON保留原始数值，不把该计数称作独占kernel时间。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

planePerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('constant_plane_perf_trial')),None)
if planePerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in planePerf['rows'])
 extra='<h2>恒定平面无计数ABBA：wall收益不稳定，完整回退</h2><p>两边都启用同一个局部late fallback，只切换恒定平面cross捷径，nearest/source二分均ON，扩充SCB1、曝光/射线数/当前radiance保持。同binary/七SPIR-V OFF/ON/ON/OFF，wall两对改善0.695%和−0.074%；main标记改善0.579%和0.485%，只是BOTTOM_OF_PIPE命令区间，不能当独占kernel时间。六主文件、20当前快照、2658×16word GPU pairs和跨四组返回全部exact，仅32-word参数的word29改变开关位。两边实际主均3586（旧3569保持、+17合法），新增coverage成本与捷径收益不能混用。wall没有稳定收益，整个五文件late接线/helper/开关/proof实验已回退，保留此前source/nearest二分；没有新生产覆盖、性能或视觉收益。几何捷径的proof等价结论仍有效，但减少循环不能直接证明加速，Release/全场景/Lumen目标未闭合。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

aliasProof=next((v for v in evidence.values() if isinstance(v,dict) and v.get('offset_reference_alias_proof')),None)
if aliasProof:
 extra='<h2>Offset/refs前缀存储复用：GPU等价证明</h2><p>读取nearest的offsets[i]后，有效source排序写入位置at≤count≤i，只覆盖已消费前缀0…i，未读后缀不变；显式alias helper省去独立refs[32]声明，其余源验证、浮点形成、排序、凸包与插值顺序保持。本次2658独立GPU pairs全部16word exact，2144接受及五负例保持。实际'+str(aliasProof['nearest_offset_reads'])+'次offset读取、'+str(aliasProof['reference_slot_writes_including_shifts'])+'次reference写入，没有越过已消费offset的写入；最大32有效源。六主文件仍与保留source二分基线exact，扩充主3569，七生产SPIR-V未改变。诊断计数不是硬件traffic或寄存器分配，也不证明性能收益；性能试验需移除计数并使用同binary/SPIR-V开关对照。本轮未新增coverage、调光或放宽门禁，Lumen/P0/Release目标仍未闭合。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

offsetPerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('offset_refs_perf_trial')),None)
if offsetPerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in offsetPerf['rows'])
 extra='<h2>Offsets/refs无计数生产ABBA</h2><p>'+html.escape(offsetPerf['decision'])+'</p><p>只有ON用offset前缀存储refs，其他float/source tuple/nearest/插值顺序不变，source/nearest二分均ON；同binary/七SPIR-V OFF/ON/ON/OFF。六主文件、20快照及2658×16word GPU pairs全部exact，仅resolve参数word29的262144位变化。主3569、旧20缺口保持，没有新覆盖或调光。main是BOTTOM_OF_PIPE命令标记区间，不能称独占kernel；Debug扩充资产结果不替代Release预算或Lumen视觉验收。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

sourceCard=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('source_card_share_proof')),None)
if sourceCard:
 body=''.join('<tr><td>'+label+'</td><td>'+str(sourceCard[key])+'</td></tr>' for label,key in [('source调用','source_calls'),('aux source','auxiliary_calls'),('atlas source','atlas_calls'),('原card读取','card_reads_original'),('共享card读取','card_reads_shared'),('原textureSize','textureSize_original'),('懒共享textureSize','textureSize_shared_lazy'),('valid source','valid_sources')])
 extra='<h2>Source验证实际元数据重复读取</h2><p>当前2658 query记录原source调用、atlas/aux分布、有效源及card/textureSize调用；候选共享外层已验证card，每query首次atlas才取textureSize。逐query调用数和有效性相同，card读取差=atlas调用数，textureSize从每atlas一次降至每含atlas的query一次。全部2658×16word独立GPU pairs exact、2144接受/五negative保持，六主文件仍与保留source二分exact，生产主3569/旧20缺口保持。七生产SPIR-V未改，诊断已撤除；这是显式proof控制流次数，不是硬件traffic、register或性能收益。下一步需无计数同binary/SPIR-V生产开关对照。没有新覆盖或Lumen视觉收益。</p><table><tr><th>实际控制流</th><th>次数</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

cardPerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('source_card_perf_trial')),None)
if cardPerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in cardPerf['rows'])
 extra='<h2>Source card/尺寸共享无计数ABBA</h2><p>'+html.escape(cardPerf['decision'])+'</p><p>原/共享当前card＋首次atlas textureSize，其他source/nearest/排序/浮点/插值顺序保持，同binary/七SPIR-V OFF/ON/ON/OFF；四轮六主文件、20快照与2658 GPU pairs全16word exact，仅resolve参数word29改变262144位。主3569及旧20缺口保持，没有新覆盖或调光。main为BOTTOM_OF_PIPE命令标记区间，非独占kernel。Debug扩充资产对照不能代替Release预算、全覆盖或Lumen视觉验收。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

callShape=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('call_shape_static_evidence')),None)
if callShape:
 body=''.join('<tr><td>'+tag+'</td><td>'+str(v['SPIRV_OpFunctionCall_count'])+'</td><td>'+str(v['SPIRV_OpVariable_Function_count'])+'</td></tr>' for tag,v in callShape['variants'].items())
 extra='<h2>编译结构排查：card参数复制候选线索</h2><p>重新用原build_spv参数编译保留baseline及两已丢弃候选，三SPIR-V SHA全部与真实历史采集manifest exact。离线默认spirv-cross MSL中，共享card调用前在source循环中无条件形成64-byte SurfaceCacheCard参数副本，aux也经过此调用。前轮实际proof的41488调用中37726是aux；这不是新GPU验证或native寄存器/spill证据，Metal优化器可能消除副本。</p><p>临时新候选将aux路由原helper，仅atlas分支调用sharedCard helper。spirv-val通过，离线MSL确认card参数副本进入atlas else分支。尚未GPU等价/性能认证，没有进入生产；下一步先独立GPU查询对照，再无计数ABBA。此页新增阶段沿用已验证480截图，只为展示生产未变，不当成新采集。</p><table><tr><th>静态SPIR-V</th><th>OpFunctionCall数量</th><th>Function OpVariable数量</th></tr>'+body+'</table><p>表中为模块语法数量，不是运行调用次数或硬件寄存器量。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

callRoute=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('source_card_route_proof')),None)
if callRoute:
 extra='<h2>Aux分流实际GPU三路等价</h2><p>原查询、此前全source共享、仅atlas共享三路独立invocation，2658×16word全部exact，2144接受/五negative保持。实际source'+str(callRoute['source_calls'])+'、aux'+str(callRoute['auxiliary_calls'])+'、atlas'+str(callRoute['atlas_calls'])+'、valid'+str(callRoute['valid_sources'])+'，均逐query相同；传入card参数的共享调用'+str(callRoute['card_parameter_calls_predecessor'])+'→'+str(callRoute['card_parameter_calls_route'])+'，差额正是aux调用。card读取6416→2654、textureSize3762→447保持前轮共享效果，六主文件仍与保留source二分exact、主3569/旧20缺口保持。</p><p>这是显式GPU控制流次数，不代表native编译器保留64-byte物理复制或硬件spill/register量，也不证明退化根因或性能收益。七生产SPIR-V保持、诊断已撤除，下一步移除计数后同binary/SPIR-V无计数ABBA。没有新coverage或Lumen视觉收益。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

routePerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('source_card_route_perf_trial')),None)
if routePerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in routePerf['rows'])
 extra='<h2>Aux分流当前card/尺寸无计数ABBA</h2><p>'+html.escape(routePerf['decision'])+'</p><p>aux走原source helper，atlas共享当前card＋首次atlas textureSize，其他source/nearest/排序/浮点/插值顺序保持，同binary/七SPIR-V OFF/ON/ON/OFF；四轮六主文件、20快照与2658 GPU pairs全16word exact，仅resolve参数word29改变262144位。主3569及旧20缺口保持，没有新覆盖或调光。main为BOTTOM_OF_PIPE命令标记区间，非独占kernel。Debug扩充资产对照不能代替Release预算、全覆盖或Lumen视觉验收。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

singlePerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('source_single_lookup_perf_trial')),None)
if singlePerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in singlePerf['rows'])
 if 'restored_control' in singlePerf:
  c=singlePerf['restored_control'];body+='<tr><td>恢复扩充基线（独立观测，非ABBA）</td><td>'+format(c['wall_ms'],'.3f')+'</td><td>'+format(c['markers']['main'],'.4f')+'</td></tr>'
 extra='<h2>单份Lookup内atlas共享无计数ABBA</h2><p>'+html.escape(singlePerf['decision'])+'</p><p>只有一份lookup/nearest/排序/插值函数，aux走原helper，atlas在原循环内共享当前card＋首次textureSize，其他source/nearest/排序/浮点/插值顺序保持，同binary/七SPIR-V OFF/ON/ON/OFF；四轮六主文件、20快照与2658 GPU pairs全16word exact，仅resolve参数word29改变262144位。主3569及旧20缺口保持，没有新覆盖或调光。main为BOTTOM_OF_PIPE命令标记区间，非独占kernel。Debug扩充资产对照不能代替Release预算、全覆盖或Lumen视觉验收。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

actualDriver=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('actual_moltenvk_dump')),None)
if actualDriver:
 body=''.join('<tr><td>'+html.escape(name)+'</td><td>'+str(v['bytes'])+'</td><td>'+str(v['byte_exact_to_default_offline'])+'</td></tr>' for name,v in actualDriver['functions'].items())
 extra='<h2>实际Vulkan驱动与译码证据</h2><p>'+html.escape(actualDriver['driver_line'])+'</p><p>两个ICD可暴露同名Apple M4 Pro；现已将引擎所选驱动身份写入日志和capture，而不是用GPU名称猜测。此轮实际选中MoltenVK1.4.1，dump的consumer SPIR-V与保留生产模块SHA完整匹配。实际MSL顶层使用argument buffer，默认离线MSL为平面绑定；下表三个源函数本体却逐字节一致。实际驱动生成源码仍不是Metal机器码/register/spill证明，不能将历史47→66ms变化归因于某编译机制。</p><table><tr><th>函数</th><th>字节</th><th>与默认离线本体exact</th></tr>'+body+'</table><p>新扩充350帧七审计和GPU112/13/80通过，2658pairs及对原查询全部16word exact，主3569/无新增命中保持。没有改生产SC/BVH shader、光照、射线或候选顺序；保留的是一次初始化的驱动身份日志与capture元数据完善。诊断dump观测不作为性能ABBA或独占kernel证据，P0/Lumen/全覆盖/Release目标仍未闭合。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

mainFrameWork=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('main_frame_work_repeat_proof')),None)
if mainFrameWork:
 body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+str(v['minimum'])+'</td><td>'+format(v['mean'],'.1f')+'</td><td>'+str(v['maximum'])+'</td></tr>' for k,v in mainFrameWork['metrics'].items())
 extra='<h2>主resolve 181帧实际工作量</h2><p>两个独立350帧诊断运行使用同binary/七SPIR-V、实际MoltenVK1.4.1。151–331每帧65536个有效atlas invocation，全部181×40word计数和六主文件逐字节一致；七审计、2658 GPU pairs/旧选择严格保持。临时计数采用每invocation局部累加和帧尾64位low/high原子汇总，已有350帧oracle读回一并取出；最终完整恢复生产源/七模块，普通连续刷新不增加读回或等待。</p><p>计数排除oracle helper工作；card_visits包括五类缓存阶段，point_texel_candidates是身份检查前候选数，local_bilinear_simplex_segment_tests合并三模式。表内是实际源码控制流工作量，不是物理内存访问、register/spill或独占kernel耗时。额外28份proof缓冲26份exact，DDGI opensky/SH有差异，不能宣称全场景状态完全相同。诊断A/B不能解释历史47→66ms，亦不能替代无计数生产ABBA；没有新优化收益或覆盖提升。</p><table><tr><th>工作量/帧</th><th>最小</th><th>均值</th><th>最大</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

cardStage=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('card_stage_repeat_proof')),None)
if cardStage:
 body=''.join('<tr><td>'+html.escape(v['phase'])+'</td><td>'+format(v['cards_per_frame'],'.1f')+'</td><td>'+format(v['top_rejects_per_frame'],'.1f')+'</td><td>'+format(v['past_top_checks_per_frame'],'.1f')+'</td><td>'+format(v['top_reject_percent'],'.2f')+'%</td></tr>' for v in cardStage['phases'])
 extra='<h2>五阶段card遍历与拒绝</h2><p>两个独立350帧诊断的181×72word逐字节一致，65536有效atlas invocation/帧、局部溢出0；分阶段访问之和、point候选、local测试、wide/hull pair、hull triangle、boundary lookup和geometry请求七项汇总与上一轮181帧逐帧exact。owner拒绝均0，局部三模式603.5万card访问/帧、353.2万共同前置拒绝，为本次调用内失败位图试验提供实际证据。没有将次数称作独占kernel耗时或物理访存。</p><table><tr><th>阶段</th><th>访问/帧</th><th>前置拒绝/帧</th><th>通过前置/帧</th><th>前置拒绝率</th></tr>'+body+'</table><p>前置分类包括各阶段明确列出的owner/flags/projection/face/bounds/triangle目录；local通过前置仍可能被四texel身份拒绝。临时32类计数已撤除，普通连续刷新不增加读回；capture现在明确记录SC_诊断环境。DDGI缓冲另按实际逐文件比较，不能扩大为全场景动态等价。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

localMemo=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('local_reject_memo_perf_trial') and not v.get('quiet_host_abba')),None)
if localMemo:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in localMemo['rows'])
 extra='<h2>局部三模式失败位图无计数ABBA</h2><p>'+html.escape(localMemo['decision'])+'</p><p>本次geometry请求内两个uint只记共同检查失败、最多64card，后续local模式按原升序跳过相同失败；超过64或关闭开关走原路径。失败处原来只continue、无rejectMask副作用；TriangleID拒绝继续原逻辑。本资产每owner最多12card，高32位与超过64的fallback未在此资产GPU执行，不扩大验证范围。没有缓存世界点或radiance、没有减少射线/跳帧/调光/放宽门禁。表内main为BOTTOM_OF_PIPE命令标记区间，不是独占kernel耗时；Debug扩充资产不替代Release、全覆盖或Lumen目标。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

memoWork=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('local_memo_work_proof')),None)
if memoWork:
 n=memoWork['frame_count'];skip=memoWork['totals']['local_owner_or_memo_skip']/n;remaining=memoWork['totals']['local_flags_projection_footprint_reject']/n
 extra='<h2>失败位图实际省去的检查</h2><p>独立ON诊断记录181帧主控制流，oracle排除。owner拒绝为0，因此local_owner_or_memo_skip本轮实际代表后续mode跳过的共同失败检查，平均'+format(skip,'.1f')+'次/帧；剩余共同条件拒绝'+format(remaining,'.1f')+'次/帧。两者之和与原共同拒绝数181帧逐帧exact，TriangleID拒绝和插值测试不变；七项汇总及主/2658查询严格审计保持。该诊断不混入生产ABBA，也不声称跳过源码检查等于物理访存或独占kernel节省。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


quietMemo=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('quiet_host_abba') and v.get('local_reject_memo_perf_trial')),None)
if quietMemo:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in quietMemo['rows'])
 extra='<h2>安静ABBA：最终撤回失败位图</h2><p>'+html.escape(quietMemo['decision'])+'</p><p>前探索组第一OFF采样窗口曾并行生成报告，不能据小幅结果认证收益。安静组不并行agent构建、报告生成、浏览器截图或其他GPU进程；每组四轮同binary/七SPIR-V，实际MoltenVK1.4.1，八性能轮各七审计、组六主/20快照/2658 queries/reference/results exact，params只262144位变化。两组间binary重建过，不能声称八轮同binary。实际每帧少2018579次源码检查仍没有稳定加速，生产两候选源/flag/位图完整撤除，前保留source/nearest版本及七模块恢复；保留SC_ capture环境记录。main只为BOTTOM_OF_PIPE命令标记区间。</p><table><tr><th>安静条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

projectionWork=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('projection_repeat_proof')),None)
if projectionWork:
 body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+format(v,'.2f')+'</td></tr>' for k,v in projectionWork['mean'].items() if not k.startswith('reserved'))
 extra='<h2>投影与点采样：实际拒绝工作量</h2><p>两独立诊断181×40word逐字节相同；geometry请求、point候选、geometry接受和boundary lookup与前32类计数逐帧exact。投影统计包括主SC路径中的source验证，排除oracle额外调用。每帧约1494.9万调用，182.4万负depth拒绝（12.20%），5.3万UV域拒绝；635.3万point texel候选中376.4万被TriangleID淘汰（59.25%），同ID候选有185.2万距离拒绝。没有将源码计算次数称作实际硬件除法、物理访存或独占kernel瓶颈；诊断耗时不进入性能对照。</p><table><tr><th>控制流指标</th><th>每帧平均</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
depthPerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('projection_depth_first_perf_trial')),None)
if depthPerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in depthPerf['rows'])
 extra='<h2>投影负depth早退：无计数开关对照</h2><p>'+html.escape(depthPerf['decision'])+'</p><p>只在负depth路径先返回，已接受路径的depth、u/v、pad/clamp与atlasUV算术顺序保持；候选升序和拒绝契约不变，不缓存世界点/radiance。四轮安静OFF/ON/ON/OFF同binary/七模块，原射线与静态光照条件保持。main为BOTTOM_OF_PIPE命令标记区间；Debug扩充资产不能替代Release预算或Lumen等价验收。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

pointDistance=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('pointdist_repeat_proof')),None)
if pointDistance:
 body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+format(v,'.2f')+'</td></tr>' for k,v in pointDistance['mean'].items())
 extra='<h2>点采样的分量拒绝：实际主工作量</h2><p>两轮181×40word逐字节一致，65536 invocation/帧、局部溢出0；原十四类工作量逐帧exact。每帧185.2万距离拒绝中，172.5万（93.14%）可由任一分量绝对位移大于原0.05m直接识别，所有实际被分量判定拒绝的候选也被原length门拒绝，误拒绝计数为0。诊断仍完整执行原length与候选选择，不计作生产性能收益；source验证和oracle排除范围与前轮一致。六主结果和2658查询全word保持，其他DDGI缓冲逐文件另记，不扩大为全场景动态等价。该证据覆盖当前实际Sponza候选，不声称穷尽所有浮点输入。</p><table><tr><th>控制流指标</th><th>每帧平均</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
componentPerf=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('point_component_perf_trial')),None)
if componentPerf:
 body=''.join('<tr><td>'+html.escape(v['mode'])+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in componentPerf['rows'])
 extra='<h2>分量预拒绝：无计数安静ABBA</h2><p>'+html.escape(componentPerf['decision'])+'</p><p>只在point采样形成captured后按同一0.05m做分量筛选，其余候选保留原length、radiance、cacheT和严格bestKey比较；卡顺序及拒绝位保持，无世界坐标数组或旧radiance复用，不减射线/跳帧/调光/改门禁。四轮同binary/七SPIR-V，安静宿主，不与构建/网页/截图或其他GPU任务重叠。main仍为BOTTOM_OF_PIPE标记间隔，不是独占kernel。Debug扩充资产不替代原SCB1、Release或Lumen验收。</p><table><tr><th>条件</th><th>wall ms/帧</th><th>main标记ms</th></tr>'+body+'</table>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

componentEdge=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('point_component_edge_proof')),None)
if componentEdge:
 extra='<h2>GPU数值边界：分量筛选误拒绝核对</h2><p>独立GPU执行'+str(componentEdge['cases'])+'个动态构造向量，包含0.05m前后64个float邻值、正负轴、1e−8扰动、近对角、NaN/Inf、subnormal和大数值。分量拒绝'+str(componentEdge['component_rejects'])+'个，原length拒绝'+str(componentEdge['legacy_rejects'])+'个，误拒绝'+str(componentEdge['false_positive_rejects'])+'；主六结果与原生产ON相同，2658查询全word保持。使用独立临时524288位，4word尾部仅随既有350 oracle读回，验证后已撤除，不进入生产计时。有限向量集不构成穷尽浮点域或全部资产证明；新优化默认关闭。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

hostProfile=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('host_profile_evidence')),None)
if hostProfile:
 c=hostProfile['CPU'];body=''.join('<tr><td>'+html.escape(v['function'])+'</td><td>'+format(v['ms'],'.1f')+'</td><td>'+format(v['percent_of_sampled_weight'],'.2f')+'%</td></tr>' for v in c['leaf_top'][:6])
 extra='<h2>实际宿主CPU采样与GPU trace边界</h2><p>Metal System Trace的20秒窗口结束后未完成保存，90秒超时，残留trace不能导出，未用于GPU归属。Time Profiler成功采集5秒，16127个1ms样本，跨线程权重16127ms；空闲worker的yield调用栈占主要样本，SC准备inclusive1320ms、Append714ms，主线程1646ms。源码每帧重建点并逐点hash后才判断资源reuse。该模板record-waiting-threads=0，因此不能量化阻塞GPU等待，更不能声称独占kernel或register/spill根因。两新350原生捕获的六主/2658查询严格保持；源码与七模块未改变，诊断耗时不当生产加速。</p><table><tr><th>叶函数</th><th>采样权重ms</th><th>样本占比</th></tr>'+body+'</table><p>下一项修改针对CPU几何准备，须先以源附着版本、精确card/world输入和epoch证明失效条件，每帧frame身份与radiance仍刷新。不会从digest或指针相同直接推断不可变。完整原trace含环境数据，保留在/tmp；网页只归档采样表与必要摘要。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

cpuGeometry=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('cpu_geometry_trial')),None)
if cpuGeometry:
 body=''.join('<tr><td>'+html.escape(v['metadata'].get('backend','Vulkan'))+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td></tr>' for v in cpuGeometry['rows'])
 extra='<h2>CPU几何准备复用试验：DISCARD</h2><p>源附着版本、allocation/geometry epoch及card/world/slot逐字节相同才复用不可变CPU点；逐帧frame、32射线、可见性和radiance仍更新。233项SC测试通过，帧125实际复用OFF/ON/ON/OFF为false/true/true/false；同binary/七SPIR-V四轮六主、21份proof及2658项查询全字节保持。两对wall改善0.367%/0.018%，main标记变慢0.434%/0.110%，未形成稳定收益，因此完整撤回四原文件和新helper。真实CPU重复工作不能直接换算为GPU帧加速。</p><table><tr><th>顺序OFF/ON/ON/OFF</th><th>wall ms</th><th>main标记ms</th></tr>'+body+'</table><p>恢复后重新构建和原资产回归，SC阶段与P0分别验收；参考暗廊道、地面、受光墙差距仍在，Lumen/全覆盖/Release目标未闭合。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

resolveDomain=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('resolve_dispatch_trial')),None)
if resolveDomain:
 body=''.join('<tr><td>'+html.escape(label)+'</td><td>'+format(v['wall_ms'],'.3f')+'</td><td>'+format(v['markers']['main'],'.4f')+'</td><td>'+format(v['markers']['trace'],'.3f')+'</td><td>'+format(v['markers']['radio_resolve'],'.3f')+'</td></tr>' for label,v in zip(['A OFF','A ON','B ON','B OFF'],resolveDomain['rows']))
 extra='<h2>resolve提交域实测：减少越界组，未认证稳定收益</h2><p>当前consumer编译LocalSize=8×8，宿主把256×256 tile数直接当组数提交；4194304个提交线程中65536在有效域，其余98.4375%越界返回。新开关只把组数改32×32，保持有效global IDs、每tile32射线、候选优先级、原浮点运算、params和七SPIR-V。帧125/350四轮实际记录组数OFF/ON/ON/OFF=256/32/32/256，六主、21份proof和2658查询全部exact。</p><table><tr><th>顺序</th><th>wall ms</th><th>main标记ms</th><th>未改trace标记ms</th><th>未改radio标记ms</th></tr>'+body+'</table><p>首对wall改善0.196%、main慢0.084%；反向配对wall慢6.218%、main慢5.559%，同轮未改trace/radio也变慢，不把共同退化全归因于网格。未认证稳定收益，DISCARD宿主开关与诊断日志，精确恢复后重跑回归。提交域是API/编译/source证据，不是实际指令计数或独占kernel成本；Lumen/P0/全覆盖/Release仍未闭合。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

# Current visual priorities supersede historical performance-only next steps.
extra='<h2>当前排查优先级</h2><p>后续决策围绕暗部间接光、颜色与质感、空间层次。场景、材质、构图差异暂不作为主要排查方向。先追踪实际间接光来源、来源可见性、插值范围、法线与方向、相邻像素样本和回退变化。漏光、能量累积、插值不连续均为待验证假设；SC命中数和性能收益只是辅助指标。固定曝光1.8546、光照强度和门禁保持。</p>'
bvhOrder=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('bvh_order_diagnostic')),None)
if bvhOrder:
 extra+='<h2>BVH遍历顺序诊断已收尾</h2><p>95982条实际辅助射线采样，保持原遍历及浮点顺序。TLAS/BLAS双子盒通过时原顺序远侧先访问分别11.731%/14.092%；约69–71%的双通过节点入口相等。没有执行重排，不能宣称节点节省或加速，也不证明视觉改善。首份读回长度不足，未用于计数结论；修正后计数守恒及原命中审计通过。临时诊断已撤除，生产源与七模块恢复。</p>'
giSource=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and 'right_arcade_largest_horizontal_transition' in v),None)
if giSource:
 body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+format(v['actual_indirect_Y'],'.6f')+'</td><td>'+format(v['source_share']['ddgi']*100,'.2f')+'%</td><td>'+format(v['source_share']['spgi']*100,'.2f')+'%</td><td>'+format(v['source_share']['ssgi']*100,'.2f')+'%</td></tr>' for k,v in giSource['regions'].items())
 extra+='<h2>屏幕间接光实际来源：线性HDR诊断</h2><p>实际同帧descriptor绑定及GPU完成后读回，未隔离/关闭光源，未调整亮度。CPU按照线性clamp采样与FusionIndirect公式重放，误差单独记录。份额只归属当前合成来源，不证明来源可见性、能量正确性或漏光。相邻像素最大水平变化点仅作为后续探针/法线/回退追踪位置。</p><table><tr><th>区域</th><th>实际间接光Y（线性）</th><th>DDGI份额</th><th>SPGI份额</th><th>SSGI份额</th></tr>'+body+'</table><p>可见性、世界插值范围、法线方向、射线回退及跨帧能量历史仍待验证，不能将当前来源占比当作已确认根因。</p>'
if giSource:
 giSourcePath=next(k for k,v in reversed(list(evidence.items())) if v is giSource)
 giMapFolder='ddgi-native-source/'+Path(giSourcePath).parents[1].name+'/fusion' if 'ddgi-native-source' in Path(giSourcePath).parts else 'ddgi-diffuse-history/'+Path(giSourcePath).parent.name if 'ddgi-diffuse-history' in Path(giSourcePath).parts else 'sh-scalar-349' if 'sh-scalar-349' in Path(giSourcePath).parts else 'sh-history-350' if 'sh-history-350' in Path(giSourcePath).parts else 'ddgi-borrow-350' if 'ddgi-borrow-350' in Path(giSourcePath).parts else ('gather-350' if 'gather-350' in Path(giSourcePath).parts else 'dark-source-350')
 extra+='<p>下图为统一线性HDR 0–1诊断显示，不是固定曝光成图，不作为压暗后的视觉改进。</p>'+''.join('<figure><img style="max-width:420px" src="'+giMapFolder+'/'+k+'-linear-scale-1.png"><figcaption>'+k+'：实际合成前漫反射贡献</figcaption></figure>' for k in ['ddgi','spgi','ssgi','indirect'])
text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

gatherAudit=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('spgi_gather_audit')),None)
neighborGeometry=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('gather_neighbor_geometry_evidence')),None)
if gatherAudit and neighborGeometry:
 extra='<h2>SPGI实际插值、法线与邻像素</h2><p>350帧实际gather描述符、denoise前输出及当前射线记录复制，353帧完成检查。首版遗漏BGRA8 normal被拒绝；150帧格式核查后重新采集，没有采用缺失快照。444个采样像素全部在两倍输出half ULP内重放一致，12邻像素denoise也一致。4172个接受tap中186个放置法线与接收法线点积为负；探针世界距离中位0.0356m，最大0.9623m。本次444像素全部走normal-weighted，没有触发normal-unrejected fallback；不能据此排除全场景回退或漏光。</p><p>上一轮最大的相邻变化点(native2259,776→2260,776)跨越3.8631m的接收面位置变化，法线点积0.55436；这处跳变不能单独证明插值不连续。较亮侧主要四探针当前射线无正权实测，均为借用/未激活，其中三个主要探针48条SDF命中距离全0；右廊道采样按插值权重统计的当前借用比例中位74.88%。这是当前射线记录，不能等同带历史滤波SH的能量份额或已确认漏光。下一步追踪借用来源、SC拒绝原因、DDGI实际可见性与方向。临时生产采集代码已撤除，固定曝光和门禁保持，没有保留视觉修正。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


sdfGeometry=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('spgi_sdf_geometry_audit')),None)
if sdfGeometry:
 body=''.join('<tr><td>'+str(v['probe'])+'</td><td>'+str(v['ray'])+'</td><td>0</td><td>'+format(v['geometry_cpu_distance'],'.6f')+'</td><td>'+format(v['radiance_weight'],'.5f')+'</td></tr>' for v in sdfGeometry['rows'] if v['sdf_hit_distance']==0)
 extra='<h2>SPGI起点SDF与真实几何</h2><p>实际350帧SDF体积384³与mode6 GPU方向/命中记录复制，353完成。36个诊断槽中4个探针未激活、32条实际射线；没有减少生产射线。12个零距离命中采用精确GPU起点，其他20个起点由实际输入float32重建。独立GPU真实三角形BVH与双精度CPU首命中全部一致，最大误差'+format(sdfGeometry['max_gpu_cpu_error_m'],'.8g')+'米，原2e−5米容差保持；锁定Sponza测试场景为identity变换，opaque/two-sided几何，未认证alpha/transmission。</p><p>三个主要探针起点实际SDF为0.0359/0.0389/0.0526米，均低于0.054696米门槛，marcher首次采样即返回distance0，与方向无关。12条真实几何交点则在0.0175–9.1186米，10条超过该SDF门槛、2条在门槛内；不能把所有近起点记录都等同无效命中。其中11条是负权借光，1条半球权重为0。SC候选未接受，范围与朝向拒绝并存，reason1是优先汇总代码，不证明朝向为唯一原因。确认的是局部SDF初始门槛提前停步及回退关系，不是暗部偏亮、漏光或能量累积根因。</p><table><tr><th>探针</th><th>ray</th><th>SDF距离m</th><th>真实几何距离m</th><th>当前ray权重</th></tr>'+body+'</table><p>实际hint_params.w=0，DDGI借光路径不执行深度矩/relocated-origin可见性分支；本次该分支的诊断槽为零，不能当成合法遮挡数据。下一步复制默认路径实际8邻SH/权重/方向和来源身份，再验证来源可见性；不通过打开可见性或调bias直接获取更暗图。临时采集与anchor变更已完整撤除，固定曝光和生产shader恢复；本阶段没有保留视觉或性能修正。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


borrowAudit=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_default_borrow_audit')),None)
borrowGeometry=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_grid_geometry_audit')),None)
if borrowAudit and borrowGeometry:
 extra='<h2>默认DDGI借光：实际八邻SH与网格连线</h2><p>350实际finalize描述符及输出、353完成。32条活动射线中25条实际消费DDGI八邻、7条未走该路径；200个邻项有本帧350 stamp。实际9项SH方向求值最大误差4.7684e−7，当前ray radiance归一化重放一致，三线性/方向/组合权重与实际记录一致。14个网格probe，网格起点距接收点0.896–4.346m；不会将unused relocated-origin/depth读回冒充已消费的可见性数据。</p><p>原200条线段中8条触发既有oracle端点不确定状态，明确拒绝全量首交认证。另一次原生诊断仅将query最大距离延长0.05m，原起点/方向和2e−5m GPU/CPU距离门保持，补充200条全部一致max误差3.782e−7m；交点归类仍使用原接收点距离。138项有接收点之前的opaque/two-sided交点，40项gap≤0.05m、98项更远（仅距离分箱，不是新可见性阈值）。原不确定结果/两轮成图均保留，未认证alpha/transmission。</p><p>这些项约占本次25条ray归一化邻权重71.16%、方向radiance79.77%，这是当前ray样本的算术归属，不是带历史滤波SPGI或屏幕能量份额，也不能认定漏光。网格位置只是默认插值锚点，尚未认证上一槽SH的真实测量起点；当前复制的relocated origins全部state1，不能据此证明与旧SH测量历史相同。下一步联合349帧DDGI实际SH producer与350默认借光，验证来源身份、历史起点/方向/几何可见性和能量历史。三视觉目标未改善认证，临时shader/宿主helper已撤除，生产光照、曝光、射线和门禁保持。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

shHistory=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_sh_history_identity_audit')),None)
measurementGeometry=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_measurement_origin_geometry_audit')),None)
if shHistory:
 frames=[r['latest_measurement_frame'] for r in shHistory['probes']]
 extra=f'<h2>SH历史来源身份与真实测量起点</h2><p>实际{shHistory["producer_frame"]}帧输出与{shHistory["consumer_frame"]}帧绑定SH全buffer逐字节相同，资源句柄{shHistory["resource_handle"]}对应本volume的producer编码；多volume日志按句柄连接。14个借用probe的全部128条射线Trace/Finalize起点、方向与帧/probe标记逐位一致，最近实测帧{min(frames)}–{max(frames)}。最近一次起点不能代表全部temporal混合能量来源。</p>'
 if not shHistory["GPU_scalar_summary_consistent"]:
  extra+=f'<p class="note">私有生产公式等价审计明确拒绝：{shHistory["GPU_scalar_summary_inconsistent_probes"]}项GPU误差汇总与保存的九项系数差值不一致，GPU汇总max {shHistory["GPU_scalar_summary_max"]:.6g}，直接vector差max {shHistory["coefficient_vector_max_error"]:.6g}。没有降低门禁或把vector近似相等改称完整生产公式通过；这不是已确认的渲染过亮根因。</p>'
 else:
  extra+=f'<p>本次私有GPU误差汇总已与九项发布系数差值逐位对应，max {shHistory["GPU_scalar_summary_max"]:.6g}。此前全882项不自洽记录仍保留为失败阶段；本项只认证汇总自洽，formula及物理传输范围分别报告。</p>'
 if measurementGeometry:
  m=measurementGeometry
  extra+=f'<p>真实最近测量起点→接收点的{m["queries"]}条独立opaque/two-sided GPU/CPU首命中一致，max误差{m["max_GPU_CPU_error_m"]:.6g}m，原2e−5m门保持。{m["intersections_before_endpoint"]}项接收点前有交点；本批归一邻权重{m["normalized_neighbor_weight_fraction_before_endpoint"]*100:.3f}%、未乘SH投影/重要性权重的方向radiance {m["sampled_directional_radiance_fraction_before_endpoint"]*100:.3f}%。不是屏幕/历史SH能量份额；alpha/transmission与接收面附近交点仍需分开，尚未认证漏光因果。</p>'
 extra+=('<p>未保留视觉修正，固定曝光、光照强度、射线与刷新保持。当前汇总修正后，下一步核对实际DDGI测量射线的真实三角形首命中、光照来源与更早历史来源；不直接凭变暗判断正确。</p>' if shHistory['GPU_scalar_summary_consistent'] else '<p>未保留视觉修正，固定曝光、光照强度、射线与刷新保持。下一步修复私有误差汇总证据链并验证SH历史混合及方向来源，再做具备实际可见性证据的传输对照；不直接凭变暗判断正确。</p>')
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

recordFormula=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_SH_record_formula_audit')),None)
if recordFormula:
 f=recordFormula
 extra=f'<h2>私有SH审计修正与历史混合重放</h2><p>{f["completed_records"]}项GPU scalar与发布九系数差值逐位对应，max {f["max_scalar"]:.6g}；{f["current_updates"]}项本帧实际dispatch-before history/confidence/open/rays/CB echo逐字节对应。全部记录raw ray→SH投影、temporal blend和band clamp用现有诊断容差重放通过，max channel error {f["max_CPU_record_error"]:.6g}（不同量级字段汇总，不是统一绝对阈值）；原SC命中与视觉门不变。</p><p>14个借光probe的alpha均0.02，本次系数更新保留98%历史权重；不是screen能量比例，不排除trace来源反馈或更早origin迁移影响。修正只改变私有diagnostic的误差汇总计算，生产shader SHA保持；没有保留视觉修正，物理传输/完整跨帧历史来源和漏光因果仍未认证。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

measurementRays=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_measurement_geometry_audit')),None)
measurementSources=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_measurement_SC_source_join')),None)
if measurementRays:
 m=measurementRays
 body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+str(v)+'</td></tr>' for k,v in m['bins'].items())
 extra=f'<h2>DDGI实际测量射线的真实几何与来源</h2><p>{m["queries"]}条已认证GPU测量起点/方向查询，独立原生GPU与CPU首命中全一致，max {m["max_GPU_CPU_error_m"]:.6g}m，原2e−5m门保持。独立query max延长0.05m，生产ray不改；实际最大距离内全部有opaque/two-sided首交点。1642条非负SDF记录中，{m["SDF_early_over045"]}条提前超过0.45m、{m["SDF_late_over045"]}条延后超过0.45m。0.05/0.45仅描述分箱，不是新门或漏光判据。</p><table><tr><th>几何分类</th><th>实际测量ray</th></tr>{body}</table><p>大偏差项占最近测量adjusted radiance {m["adjusted_radiance_Y_fraction_over045_or_no_triangle"]*100:.3f}%，尚未乘SH基/temporal混合，不是screen能量，也不认证alpha/transmission或漏光因果。</p>'
 if measurementSources:
  s=measurementSources
  extra+=f'<p>仅当前349帧具备新鲜SC query witness的probe {html.escape(str(s["probes"]))}：{s["queries"]}条，SC选择{s["SC_selected"]}、未选择{s["no_SC_selected"]}。SC项只占本批adjusted测量radiance {s["SC_adjusted_measurement_radiance_Y_fraction"]*100:.4f}%；未选择项的screen/analytic分支尚未认证，不能直接归因sky。全部384条实际输入HN不满足有效normal条件，消费opposite-ray法线回退；不据此认定唯一过亮根因。</p>'
 extra+='<p>本轮没有Engine/生产shader改动或视觉修正，仅独立geometry oracle和CPU来源连接。新固定曝光350/≥65秒原生运行退出0无已列fault；上一阶段完整Unit/SC/双后端回归仍单列历史结果，本轮未重复。下一步采集真正高能量的screen/analytic回退及visibility/normal来源，保持曝光、光照强度和门禁，不凭变暗判断正确。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>')
 (out/'index.html').write_text(text)

# Existing analytic cosine option: paired causal diagnostic, never promoted by darkness alone.
diffusePair=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_diffuse_history_pair')),None)
if diffusePair:
 body=''
 for mode in ['off','on']:
  v=diffusePair['modes'][mode];a=v['analytic'];f=v['right_arcade']
  body+='<tr><td>'+mode+'</td><td>'+format(a['sky_floor_raw_radiance_Y_sum'],'.6f')+'</td><td>'+format(a['history_raw_radiance_Y_sum'],'.6f')+'</td><td>'+format(a['history_fraction_of_analytic_raw_Y']*100,'.2f')+'%</td><td>'+format(f['actual_indirect_Y'],'.6f')+'</td><td>'+format(f['direct_Y'],'.6f')+'</td></tr>'
 extra='<h2>解析历史角度算子：暗部部分改善，NOT PROMOTED</h2><p>既有DDGI_ANALYTIC_IRRADIANCE选项以余弦卷积E/pi替换方向radiance在N处的求值；同binary、全部源SPIR-V，固定曝光350/480，两次新独立运行。恒定DC增益为1，独立半球积分核对三法线误差≤1.918e−8；没有调光照强度或额外压暗。直射、albedo、ORM、AO、反射输入逐字节保持；射线距离/实际起点方向stamp、SC六主文件保持，volume只变化SkyColor.w模式字。</p><p>349 fresh 3probe/384ray中329解析、50 SC、5耗尽。329项均未取有效GBuffer材质（behind-camera101/out-of-frustum23/depth mismatch205），全部384法线仍opposite-ray回退。解析历史四邻没有可见性权重；此前测量几何偏差依旧，不能据变暗认定漏光或能量累积根因。旧输入CPU卷积预估−39.814%，原生收敛后的历史场已不同，不能将其全部下降当单次角度核效果。完整SH producer/consumer byte identity、882公式重放及当前Fusion重放通过。</p><table><tr><th>模式</th><th>329天空地板原始Y和</th><th>329历史原始Y和</th><th>原始解析历史份额</th><th>右廊道实际间接Y</th><th>右廊道实际直射Y</th></tr>'+body+'</table><p>350/480右廊道约24.50/24.79→14.79/14.90，参考8.91；地面77.08/77.46→69.11/69.48，参考59.45。受光墙106.32/106.70→92.96/93.18，参考120.51；直射区域156.29/158.57→119.68/119.70，参考162.01。暗部部分接近，明暗层次整体没有通过，颜色/质感未认证改善。OFF既有SC阶段PASS，ON FAIL；最早P0两者FAIL。因此既有选项保持默认OFF、临时诊断恢复，未认证稳定性能收益。最终默认路径截图单列，不能把试验ON当保留结果。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

surfaceDirections=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_surface_direction_audit')),None)
if surfaceDirections:
 v=surfaceDirections;body=''.join('<tr><td>'+name+'</td><td>'+format(value,'.6f')+'</td><td>'+format((value/v['history_raw_Y_sum']['actual']-1)*100,'+.2f')+'%</td></tr>' for name,value in v['history_raw_Y_sum'].items())
 extra='<h2>解析表面位置与取值方向：新独立几何证据</h2><p>329条上一349实际解析射线的新GPU首交点与float64 CPU全一致，距离最大2.604e−6m、GPU barycentric/triangle witness重构最大6.001e−6m，原2e−5m门保持。几何面法线由真实三角形边计算并朝向射线；它不是已认证的平滑/材质法线，alpha/transmission亦未认证。回退方向与该面法线夹角中位44.40°，164项大于45°；真实位置改变166个四邻历史集合。</p><table><tr><th>同一历史SH/rho CPU取样</th><th>原始历史Y和</th><th>相对实际</th></tr>'+body+'</table><p>仅换真实位置174条增、155条减；仅换面方向98增、231减；两者108增、221减。位置与方向影响不一致，不能把提前命中直接当过亮原因。真实位置没有重新获取真实材质或光源，face方向没有成为生产法线；该敏感性不是screen能量、已渲染改善或漏光/累积认证。生产路径/全部源SPIR-V未改；新350帧≥65秒原生退出0、SC四完整审计/六主byte exact/GPU112/13/80通过。前阶段完整350480Metal/Unit回归另列历史结果，本轮单350不作为新视觉门禁PASS。固定曝光截图暗部/颜色质感/空间层次仍未认证改善。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

nativeTrial=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_native_source_trial')),None)
if nativeTrial:
 extra='<h2>真实首交点／原生法线：NOT PROMOTED</h2><p>同binary和全部源SPIR-V库存冻结；默认、first-surface、first-native实际使用不同既有trace模块，不能称同一个活动kernel。350/480默认右廊道24.504/24.786，first27.331/28.864，native26.021/27.509（参考8.908）；墙默认106.315/106.707，first103.653/104.944，native100.646/101.963（参考120.512）。原生法线比仅位置组稍暗，但两者都比默认偏离暗部/亮墙目标。地面native73.262/75.119低于默认77.078/77.462，不能替代整体层次和颜色质感验收。两候选SC阶段FAIL，默认PASS，最早P0全部FAIL；两个选项继续默认OFF。</p><p>第一native350截图有效但额外法线读回未完成，拒绝来源认证；采集工具原先只等待PPM，现新增repeatable readback-json完成门，须JSON新于启动且明确completed才结束，普通空列表路径不增加等待。新三组349诊断／350截图／353 Fusion完成；原生28160 fresh rays中933接受、8913条件拒绝、18314父级拒绝，实际HN与24vec witness一致。933项CPU最近首交点max2.838e−6m，packed vertex／position／geometry-aligned N／TBN max5.610e−6，原2e−5门保持。首次CPU省略geometry alignment、第二次近unit-circle分支近似均明确拒绝；补齐现行公式后重放原raw通过，无shader或容差改变。法线贴图完整内存/实际primary pixel/opacity/source visibility/完整descriptor证书仍未认证。</p><p>新的349阻塞读回改变SC刷新/ray/frame输入，因此旧main等价明确REJECTED；三新诊断组之间六main exact，不冒充旧8190证明。旧契约只由原三组350及最终默认回归六main／四完整SC／GPU112/13/80证明。native全图direct3613分量/AO1023分量变化局限左拱廊，整帧隔离检查REJECTED；其余五个ROI这两输入逐字节保持。实际右廊道间接Y default0.039847／first0.047126／native0.043542，directY均0.002434；原始解析历史总量属于不同分支/反馈场，不能当同ray法线贡献或screen能量。native初480在106帧结束无目标图，原因未认证，重新480实采退出0，旧失败保留。最终V350/480/Metal≥65秒、Unit7/SC218、原3256/20缺口保持；无稳定性能收益或视觉修正保留。下一步优先实际表面光照/历史四邻可见性与回退来源，不凭位置或法线变暗推广。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

analyticVisibility=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('analytic_visibility_audit')),None)
if analyticVisibility:
 v=analyticVisibility;body=''
 for name,group in v['groups'].items():
  c=group['counts'];s=group['signed_Y']
  body+='<tr><td>'+html.escape(name)+'</td><td>'+str(sum(c.values()))+'</td><td>'+str(c.get('before_endpoint',0))+'</td><td>'+str(c.get('endpoint_precision_band',0))+'</td><td>'+str(c.get('no_opaque_hit',0))+'</td><td>'+format(s.get('before_endpoint',0),'.6f')+'</td><td>'+format(sum(s.values()),'.6f')+'</td></tr>'
 extra='<h2>解析历史四邻连接：新几何可见性证据</h2><p>上一349实际329条解析射线的四邻历史，分别连接现行query／真实首交点与grid／绑定重定位源点。5238条本轮新GPU／float64 CPU线段全一致，max6.547e−7m，端点分类亦一致；场景三份几何raw与上一首交点证据SHA一致。实际历史重放max1.073e−6，归一化有符号线性分解逐raymax9.839e−7。四邻索引及权重保持，不是新renderer可见性权重。</p><table><tr><th>端点／源点</th><th>连接数</th><th>端点前几何</th><th>端点精度带</th><th>无opaque命中</th><th>端点前组有符号Y</th><th>全部有符号Y</th></tr>'+body+'</table><p>现行query的329条全部至少有一个grid连接遇到端点前几何，但没有全部连接被挡；真实首交点grid有305条至少一处、9条全部连接遇到端点前几何。绑定重定位快照不能定位整个temporal SH历史；其两组分别329条至少一处、0／10条全部。真实位置改变邻域及有符号份额，不保证改善。±2e−5m是既有几何数值精度带，不是新增偏移或放宽门；无opaque命中也不等于物理无遮挡。</p><p>原先漏算.001权重截断后的归一化，旧分解数值明确拒绝并归档，原查询bytes保持，修正后逐ray核对。表中Y只是同SH／rho／方向的有符号线性分解，不能当screen能量、alpha／材质可见性、全历史来源或漏光因果。本轮仅新geometry与默认350固定曝光截图，没有生产光照／曝光／法线／ray／门变化；四完整SC／六main byte exact／GPU112/13/80／原3256及20缺口保持，整库存SHA不变。前阶段Unit7／SC218／V350480／Metal全回归单列历史，不把单350称新成对视觉门PASS。暗部／颜色质感／空间层次仍未认证改善，下一步连接实际像素与这些历史来源。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

nativeScreen=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('native_screen_neighbor_stage')),None)
if nativeScreen:
 v=nativeScreen;t=v['largest_transition'];c=v['geometry_counts']
 extra='<h2>暗廊相邻像素：原生DDGI来源与跨表面上采样</h2><p>本轮新350帧九记录native来源与production／aux输出等价认证，全部921600像素SH／weight重放mean相对误差0.0352%、p99 0.1904%，原1%门保持；六ROI均通过。实际DDGI output与Fusion绑定handle及全部raw bytes相同，Fusion全图重放在原2 half ULP门内。当前右廊道最终线性间接Y0.039847，DDGI57.058%／SPGI42.942%，SSGI为0；这些是实际合成项，不是物理路径或历史能量份额。</p><p>本次最大水平跳变(2259,776)→(2260,776)：间接Y−0.098925，DDGI项−0.064363、SPGI项−0.034636；SPGI blend0.213501→0.463135。实际GBuffer重建位置相距3.863101m、normal dot0.554364，albedo也改变。该样本伴随表面切换，不能作为同一表面插值不连续的证据；这描述固定画面内部边缘，不把参考场景／材质差异作为解释。</p><p>更具体的采样问题：明侧全分辨率2259的四个DDGI半分辨率邻点中，43.75%双线性权重来自距本像素约3.858–3.863m的另一表面，暗侧2260有18.75%来自约3.865m外的明侧。native半像素实际接收点／N和现行texture-linear消费均有证据。现行Fusion直接线性采样半分辨率E；GIGather源码与实际snapshot显示Vulkan half、Metal full。跨表面取样事实已连接到这些实际像素，是否为整体暗部偏亮主因、以及全分辨率／几何上采样是否改善颜色质感和空间层次仍待实际开关渲染验证，未保留视觉修改。</p><p>12实际full像素对应12 native半像素、91个实际消费近场连接，新独立GPU／float64 CPU全部一致，max4.694e−7m；57端点前opaque几何／34无opaque命中，前交点余量1.718–2.967m。使用当前gather重定位锚点和N bias；它们不是全temporal SH emitting来源。有符号SH贡献、负normal-dot或几何遮挡不直接认证物理漏光。SPGI filtered输出和blend实际认证，内部tap／fallback／history仍UNKNOWN。</p><p>首轮请求当前恢复binary不存在的SPGI_GATHER_AUDIT，额外就绪文件永不产生；该等待由本轮进程终止，exit1／capture_succeeded=false／已完成raw保留，不用作成功截图或回归。改用实际支持的native DDGI／Fusion接口重新运行退出0；另一次91query几何运行亦退出0。两次新350截图／四SC／六main exact／GPU112/13/80／原3256及20缺口保持，binary／全部源SPIR-V冻结。没有Engine/shader修改，前阶段全Unit7／SC218／V350480／Metal与阶段PASS/P0FAIL单列历史，本轮两张350不能替代350480视觉门或performance。下一步直接验证Vulkan full-resolution gather／geometry-aware upscale，保持原射线／曝光／光源和选择契约。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

fullResTrial=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_fullres_gather_trial')),None)
if fullResTrial:
 v=fullResTrial;comparison=v['comparison'];body=''
 for state in ['off','on']:
  c=comparison[state];edge=c['edge_pixels'];region=c['regions']['R_arcade']
  body+='<tr><td>'+state+'</td><td>'+format(edge[0]['actual_indirect_Y'],'.8f')+'</td><td>'+format(edge[1]['actual_indirect_Y'],'.8f')+'</td><td>'+format(region['actual_indirect_Y'],'.8f')+'</td></tr>'
 perf=''.join('<tr><td>'+str(i+1)+' '+row['state']+'</td><td>'+format(row['wall_ms'],'.3f')+'</td></tr>' for i,row in enumerate(v['performance']))
 extra='<h2>Vulkan全分辨率DDGI：保留默认关闭的局部修正对照</h2><p>PRIMAL_DDGI_FULL_RES_GATHER=1仅改变Vulkan gather尺寸，Metal沿用原全分辨率；默认Vulkan半分辨率。生产shader和浮点公式未改，同binary／全部源SPIR-V ON/OFF/OFF/ON。实际DDGI输出与Fusion绑定handle及完整raw保持身份，两组全图合成重放通过原2 half ULP门。全分辨率的名义像素中心采样只有本像素权重1，消除上一阶段已认证的43.75%／18.75%跨表面半分辨率混样。本轮未请求九记录aux，不冒充新的native receiver／SH全图闭合认证。</p><table><tr><th>350路径</th><th>明侧2259,776 间接Y</th><th>暗侧2260,776 间接Y</th><th>暗廊平均间接Y</th></tr>'+body+'</table><p>该边界明侧+16.673%、暗侧−7.336%，局部层次方向符合消除混样；暗廊平均仅−0.162%，整体偏亮和颜色质感差距仍在。实际GBuffer／albedo／ORM／direct／AO逐字节一致，但SH历史／opensky／SPGI不同，不能把所有变化当作独立上采样因果或physical能量结论。固定曝光350暗廊显示亮度24.525→24.466，参考8.908；受光墙106.335→105.883，参考120.512，并未整体接近。</p><table><tr><th>无额外GI读回的480序列</th><th>wall cadence ms/帧</th></tr>'+perf+'</table><p>两对开启成本+'+format(v['cost_percent'][0],'.3f')+'%／+'+format(v['cost_percent'][1],'.3f')+'%；四倍输出像素及三张RGBA16F纹理逻辑payload增加66,355,200B，不是实测分配量或独占kernel时间。两路径SC阶段350/480 PASS，最早P0均FAIL；旧六main文件和3256／20缺口保持，Unit7／SC218／GPU112/13/80与新Metal≥65秒回归分别见完整审计。Metal60图明显过亮，只作运行回归，不作Lumen视觉或四API视觉对等认证。保留为默认关闭的局部边界控制，不宣称加速、全场景Lumen等价或Release预算闭合。下一步继续暗部实际来源可见性、历史与SPGI回退；局部修正不能替代大范围视觉改善。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

visibilityGrid=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('native_visibility_grid_audit')),None)
if visibilityGrid:
 v=visibilityGrid;body=''
 for name in ['R_arcade','sunlit_wall','floor_near']:
  z=v['regions'][name]
  body+='<tr><td>'+name+'</td><td>'+str(z['pixels'])+'</td><td>'+format(z['before_endpoint_signed_share']*100,'.3f')+'%</td><td>'+format(z['components'].get('before_endpoint/regular',0),'.6f')+'</td><td>'+format(z['components'].get('before_endpoint/low',0),'.6f')+'</td></tr>'
 extra='<h2>暗部实际来源：三ROI固定8×8网格与5659几何连接</h2><figure><img src="visibility-grid/native-full-reference.jpg"><figcaption>左：本轮native350固定曝光截图；右：用户Lumen参考。完整暗廊与地面均保留；本轮没有生产视觉修正。</figcaption></figure><p>本轮204个实际full像素（各ROI64加原12边界）、780个native gather接收点。取样位置独立于亮度和遮挡，不是全图或物理能量普查。新九记录认证、全输出／六ROI闭合及实际DDGI-Fusion绑定／bytes identity保持；native SH有符号所有权乘实际上采样、albedo和SPGI blend，下面只描述所取像素的DDGI合成项。三组有符号和对实际DDGI sum误差0.0337–0.0359%，不替代原全图1%／p99门。</p><table><tr><th>区域</th><th>固定像素</th><th>端点前几何连接有符号份额</th><th>常规路径有符号Y和</th><th>低频路径有符号Y和</th></tr>'+body+'</table><p>5659条新GPU／float64 CPU首交点全一致max4.694e−7m：3809端点前／1811无opaque命中／39端点后，原2e−5门不变。所有三组64像素至少有一条端点前连接，但只有受光墙／地面各1项全连接被挡。保留该贡献的既有路径包括常规权重下限和低频深度／法线拒绝前分量；不是只调低低频比例就能解释的现象。</p><p>当前native锚点按原N bias公式连接，锚点不是完整历史emitting源。raw resident三角形ID映射原始scene ordinal，与compact scene XYZ逐字节核对；暗廊遮挡份额集中mesh17／392／119／389。mesh17首交点Y≈4.155303、probeY4.74–6.24且receiver低于3.68；mesh389首点Y≈−1.264425、1523条probe在下方而receiver在上方，当前origin state全部1。这是实际几何／当前锚点关系，材质alpha、真实opacity、历史SH来源及SPGI内部仍未认证，不能宣布全场景漏光根因。</p><p>moment最近八面体bin由CPU float32公式及真实buffer重建；不是新增native bin witness。修正离线公式漏乘cheb×2后，与native regular/low×confidence标量关系max误差2.814e−7；3809端点前连接中2362的moment均值超过receiver距离、1392触发既有0.38下限，零方差越过均值0。前一次错误比例已拒绝保留，不能据零方差猜测改shader。下一步测试实际几何可见性，并认证opaque资格与历史来源，不调曝光／光强／低频总比例或门来压暗。</p><p>无Engine／shader修改，两新350／≥65秒正常退出；四SC／六main exact／GPU112/13/80／原3256及20缺口保持。13机制＋9脚本测试、默认91query bytes及91geometry机制保持。上一阶段完整Unit7／SC218／V350480／Metal、SC阶段PASS／P0FAIL单列历史；本轮两个350不是新成对门或性能收益。固定曝光截图仍与Lumen差距明显，暗部／颜色质感／空间层次改善未认证。</p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

angular=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_moment_angular_audit')),None)
if angular:
 v=angular;body=''
 for name in ['R_arcade','sunlit_wall','floor_near']:
  z=v['regions'][name]
  body+='<tr><td>'+name+'</td><td>'+str(z['pixels'])+'</td><td>'+format(z['blocked_empty_bin_signed_share']*100,'.3f')+'%</td></tr>'
 extra='<h2>深度矩方向覆盖：空格保留初始化开放历史</h2><figure><img src="moment-angular/full-reference.jpg"><figcaption>左：本轮新Vulkan350固定曝光；右：用户Lumen参考，仅调整图片尺寸。本轮仅诊断，暗部仍偏亮、颜色与空间层次未改善认证。</figcaption></figure><p>修正上一阶段下一步推断：实际RaySampleMode=0没有mode5额外depth-only射线，修改该分支不会影响当前画面。当前深度矩读主射线radiance_and_dist.w，分8×8格并混历史；负距离不参与，少于4项抬高方差。本轮重放上一认证350的实际接收点／moment，不冒充新radiance采集；从88个当前native gather锚点做11264条新GPU几何查询，独立CPU全一致max4.266µm，原20µm门和64栈保持。</p><p>CPU float32按现行mode0公式的128方向只覆盖22/64格，三角函数不是额外native方向witness。5659接收连接中3644处在空格，其实际绑定moment全部mean=50m、variance=2.25，与宿主初始化值相同。上一阶段只检查mean50/variance0而漏掉这个非零方差签名，零方差猜测仍被否定。3809端点前连接中2342在空格，1302所在格的当前几何命中全比接收点近，88混合、77全更远；空格覆盖与后续常规／低频权重需要分别排查。</p><table><tr><th>固定取样区域</th><th>实际full像素</th><th>端点前＋空格连接signed DDGI合成份额</th></tr>'+body+'</table><p>以上份额由上一实际native SH所有权、真实bilinear／albedo／SPGI blend重放，原sample 1%闭合保持；不是全ROI普查、物理漏光率、可删除能量或新screen因果认证。当前锚点也不是历史发射原点。旧first-surface资格快照与本轮完整geometry XYZ逐字节一致，3809遮挡中3248旧flags0／561非零，关键17/389/392/119均旧flags0；仍不是本轮descriptor/alpha证书，不把全部opaque proof当可修正距离。</p><p>新350≥65秒退出0，四SC／六main旧生产byte exact／GPU112/13/80／3256与20缺口保持，生产binary与所有SPIR-V不变；13机制＋9脚本和默认91几何回放通过。本轮无Engine修改、无新Unit7/SC218/480/Metal/成对视觉门或性能结论。下一步选择性验证未测量方向的几何距离来源，同时保留主radiance与原采样；先做有效距离与资格证书，不借邻格距离、压低低频比例或调整曝光强度来换暗部。</p><p><a href="moment-angular/README.md">证据、修正和重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

independentDepth=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_independent_depth_trial')),None)
if independentDepth:
 v=independentDepth;body=''
 for name in ['R_arcade','sunlit_wall','floor_near']:
  x,y=[v['states'][s]['regions'][name]['actual_indirect_Y'] for s in ['off','on']]
  body+='<tr><td>'+name+'</td><td>'+format(x,'.8f')+'</td><td>'+format(y,'.8f')+'</td><td>'+format(v['changes'][name]['actual_indirect_Y_percent'],'.3f')+'%</td></tr>'
 extra='<h2>独立深度填格实测：暗部有变化，整体候选未推广</h2><figure><img src="depth-independent/on-reference350.jpg"><figcaption>左：新mode5候选350固定曝光；右：Lumen参考，仅尺寸调整。候选SC阶段成对门FAIL，默认保持mode0。</figcaption></figure><p>同binary和全部源SPIR-V，两新350九记录／实际Fusion闭合及350480截图。既有mode5沿原128条主射线方向／PDF，再添加128条depth-only测量、不进入SH投影；没有改曝光、光强、低频比例或原门。主采样公式保持有源码机制检查，不能宣称跨反馈历史的实际ray radiance逐字节相同。</p><table><tr><th>实际间接光ROI</th><th>mode0线性Y</th><th>mode5线性Y</th><th>变化</th></tr>'+body+'</table><p>882当前native近场锚点全部相同；moment初始化开放格39067→933、38134格改变，证明深度字段实际改变，但extras的几何距离／alpha资格并未由此认证。六ROI的实际材质／ORM／direct／AO byte exact，view/depth/normal亦保持；整图26个direct像素在ROI外不同，因此整图间接-only隔离不成立。SH／openSky／SPGI反馈均改变。暗廊和墙面unit-sum平均RGB变化很小，主要是幅度变化，不是颜色质感改善认证。</p><p>350显示暗廊24.504→22.880（参考8.908）、地面77.079→71.330（参考59.454），墙106.318→104.650（参考120.512）反而更远；480保持类似趋势。默认SC阶段350480 PASS，mode5在350的direct_patch原误差恶化2.984超过原2门而FAIL、480 PASS，成对仍FAIL；最早P0两者FAIL。没有因暗部下降或开放格减少而修改验收范围。</p><p>无额外GI读回单对480 wall209.375→220.762ms/帧，约+5.439%，不是ABBA稳定收益、Release预算或独占kernel计时。候选NOT_PROMOTED，既有mode5仍实验性且默认关闭，无Engine／shader编辑。新Unit7／SC218、两350四SC／旧六main exact／GPU112/13/80、恢复默认V350及Metal≥65秒退出0分别见审计；原3256与20缺口保持。下一步核验extra测量真实首表面／material资格和消费关系，区分SDF距离与方向格聚合，不能用邻格填值或全局压暗来代替距离。</p><p><a href="depth-independent/README.md">全部来源、成对门与恢复证据</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

extraDepth=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_depth_extra_native_audit')),None)
if extraDepth:
 v=extraDepth;body=''.join('<tr><td>'+name+'</td><td>'+str(v['groups']['all'].get(name,0))+'</td><td>'+str(v['groups']['consumed'].get(name,0))+'</td><td>'+str(v['groups']['consumed_eligible'].get(name,0))+'</td></tr>' for name in ['SDF_early_over45cm','within45cm','SDF_late_over45cm','exhausted','SDF_hit_without_ray_geometry'])
 extra='<h2>额外深度射线：同帧来源、实际消费与首几何距离</h2><figure><img src="depth-extra-native/native-full-reference.jpg"><figcaption>左：本轮mode5诊断350固定曝光；右：Lumen参考，仅尺寸调整。没有保留新视觉修正，mode5仍未推广。</figcaption></figure><p>保留测试宿主可选来源导出：同帧原始三角形、当前CPU材质与raster资格、CSV独立完成标记；普通运行不增加GPU工作。没有修改Engine／shader。请求88个锚点，本帧实际更新24个，其余64个不算当前观测。3072条extras以完整native起点和CPU float32固定方向重建；方向不是新native方向SSBO证书，CSV距离只有六位小数。实际更新列表／九记录H-E-X消费计数一致、全输出及六ROI闭合、实际DDGI-Fusion身份保持。</p><p>新独立GPU／float64 CPU首交点3072/3072一致，最大距离误差2.045µm，原20µm门保持；完整262267场景三角形XYZ逐字节一致，393当前registry flags位于原48B POD的offset44。材质及raster flags0只是保守资格，不是GPU材质驻留、texture alpha或physical opacity证书。</p><table><tr><th>描述性分类</th><th>全部3072</th><th>实际消费2042</th><th>消费且保守合格</th></tr>'+body+'</table><p>45cm只是描述分桶，不是放宽距离门。实际消费1169条SDF比首几何早超过45cm，其中1119条满足当前保守资格。6条GPU barycentric到CPU float32点重构超过原20µm，包含2条消费且flags0，全部排除；不能声称原生FIRST_SURFACE接受检查已被此重放认证。620条消费SDF命中没有该射线几何首表面，保留原路径，不当作天空。统计不是全88锚点普查、屏幕radiance因果或已确认漏光率。</p><p>固定曝光仍是暗廊22.880／参考8.908、地面71.330／59.454、墙104.651／120.512；暗部仍偏亮、受光墙偏暗，颜色质感与整体空间层次改善未认证。下一步只尝试合格depth-only真实距离，保留主radiance与未知回退，再以同候选binary／活动SPIR-V运行开关、实际消费和原门判断保留或回退。</p><p>新DebugVulkan测试target构建／Unit7／SC218／机制20＋脚本9通过；两350四SC／六main旧生产exact／GPU112/13/80、默认480及Metal≥65秒退出0，原3256与20缺口保持。最终默认SC阶段350480 PASS、最早P0 FAIL分列，未认证新性能或Release预算。<a href="depth-extra-native/README.md">来源、限制、重建与全部回归</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

depthFirst=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_depth_first_candidate_trial')),None)
if depthFirst:
 v=depthFirst;body=''
 for name in ['R_arcade','sunlit_wall','floor_near']:
  x,y=[v['states'][s]['regions'][name]['actual_indirect_Y'] for s in ['off','on']]
  body+='<tr><td>'+name+'</td><td>'+format(x,'.8f')+'</td><td>'+format(y,'.8f')+'</td><td>'+format(v['changes'][name]['actual_indirect_Y_percent'],'.3f')+'%</td></tr>'
 extra='<h2>额外深度真实首几何：原生检查通过，视觉候选已回退</h2><figure><img src="depth-first-candidate/comparison350.jpg"><figcaption>左：mode5同候选管线关闭；中：开启合格extra距离替换；右：Lumen参考。EV1.8546，除标签和尺寸无色调处理；候选DISCARD。</figcaption></figure><p>独立Vulkan mode5／128变体，两侧同binary和同活动SPIR-V，均做BVH／原生审计，只有ON替换合格depth-only距离；原primary128方向／PDF／光照公式、extra128、未知−1／−2回退保持。flags0、有限端点与原20µm重构加舍入上界全部保持；没有借几何miss变天空或调曝光／光强／权重下限。使用既有flattened first-surface BVH作独立实验，不冒充新增生产per-mesh两层BVH能力。</p><table><tr><th>350实际间接光</th><th>OFF线性Y</th><th>ON线性Y</th><th>变化</th></tr>'+body+'</table><p>OFF实际请求锚点更新21／88、2688条extras消费1792，原生合格消费698；ON更新36／88、4608条消费3058，原生合格消费1379。GPU首距离与独立float64 CPU原20µm门通过，max9.184／6.853µm；scene／flags身份保持。不合格、无首几何和重构上界超限逐项保留原值。当前CPU／raster flags不是GPU材质驻留、texture alpha或physical opacity证书。上一轮CPU残差资格不能代替原生舍入上界接受条件。</p><p>两侧各220更新槽，只有52槽／6656 extras重合；其原距离、首距离、状态、TriangleID、flags和上界全部byte exact。882近场锚点以及实际view／depth／normal、整图albedo／ORM／direct／AO byte exact；原九记录全输出／六ROI门、实际Fusion2halfULP及DDGI-Fusion身份通过。SH／openSky／SPGI及更新集合改变，不能把收敛画面差全部当单次权重因果或primary实际radiance逐字节等价。开放初始化格933→299，却不能替代视觉改善。</p><p>350暗廊22.852→26.128（参考8.908）、地面71.322→73.415（59.454）更远；墙104.619→106.212（120.512）稍近，墙／暗廊显示层次比下降。480暗廊23.209→28.058、地面71.810→74.786、墙105.103→107.098，趋势继续。颜色质感改善未认证。OFF mode5 SC阶段350FAIL／480PASS；ON原SC阶段350480 PASS，却不满足当前Lumen暗部与整体层次目标，两者P0FAIL。因此渲染候选完整DISCARD，没有因辅助门PASS而推广。</p><p>无额外GI读回单对480 wall225.994→226.005ms/帧（+0.0049%），不作稳定收益／独占kernel／Release预算认证；OFF已经执行相同BVH工作，不能把此对照当新增查询相对incumbent的成本。候选全部Engine／shader／recipe和新SPIR-V已恢复，仅保留测试DUMP8的source bounds、Begin／End／Submit／completion与显式buffer依赖修正，普通运行无新增读回或GPU工作。最终完整DebugVulkan构建、Unit7／SC218／机制28＋脚本9、两候选与恢复350四SC／旧六main exact／GPU112/13/80、默认480及Metal≥65秒回归分别见审计，原3256／20缺口保持；最终默认SC阶段PASS／P0FAIL。初次管线暂存拒绝无有效截图，失败日志保留，未用旧图代替。</p><p>这组实验否定“单靠修正额外测距即可改善主要暗部”的方案；没有把漏光／能量累积确认为根因。下一步核对同输入接收连接的实际moment格、常规／低频权重与相邻像素回退变化，保留原主射线和几何契约。<a href="depth-first-candidate/README.md">完整证据、回退与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

weightPaths=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('native_weight_paths')),None)
if weightPaths:
 v=weightPaths;body=''
 for name in ['R_arcade','floor_near','sunlit_wall']:
  r=v['regions'][name];e=r['effect_Y_sum'];body+='<tr><td>'+name+'</td><td>'+format(r['actual_DDGI_delta_Y_sum'],'.6f')+'</td>'+''.join('<td>'+format(e[k],'.6f')+'</td>' for k in ['SH_field','native_weight_policy','filtered_SPGI_blend'])+'</tr>'
 extra='<h2 id="native-weight-paths">相同接收连接：SH场与实际权重的变化</h2><figure><img src="weight-paths/current-reference350.jpg"><figcaption>左：本轮新默认350；右：Lumen参考，固定EV1.8546，只调整尺寸。历史候选已回退，本轮没有新渲染修正。</figcaption></figure><p>历史OFF／ON认证原始读回重放：204全像素、780实际half接收点、5659连接射线，起点／接收点／法线／probe ID及bilinear taps全部一致。全部780沿用近层，没有体积或inactive回退切换；762常规权重及confidence、562低频权重改变，low blend保持0.30；SH场全部改变。相邻变化不能用体积切换解释，但SPGI内部样本及历史发光位置仍未知。</p><table><tr><th>每区域64取样像素</th><th>实际DDGI变化Y总和</th><th>SH场敏感度</th><th>权重策略敏感度</th><th>SPGI混合敏感度</th></tr>'+body+'</table><p>这是同连接离线交叉重放的对称敏感度分解，包含反馈历史和归一化，不是新渲染反事实图或物理能量因果。地面变化主要对应权重，暗廊更多对应SH场；不能把整图候选退步全归因于单次测距。原native1%门保持，取样最大误差0.09766%，标量权重比与CPU float32公式max3.90e−7。完整5659匹配moment、法线与常规／低频分支见证据。</p><p>当前默认新独立GPU／CPU5659首交点一致max0.470µm：3809连接有前置几何，其中2493法线dot&lt;0；2264有效系数上升，1485下降。历史moment对应的前置几何mean超接收点73→181，下限激活3269→2864；初始化开放44→8。其余1811无查询几何、39首交点在端点之后。几何使用双面不透明假设，CPU／raster flags、resident XYZ身份保持，不是材质alpha或历史SH发光位置证书，也不是漏光率。距离变准不保证权重降低。</p><figure><img src="weight-paths/current350/fusion/indirect-linear-scale-1.png"><figcaption>本轮实际同帧间接光HDR，线性scale1，未曝光或压暗。</figcaption></figure><p>新默认实际间接Y：暗廊0.039870、地面0.078230、墙0.071172；九记录全输出／六ROI原门、Fusion原2halfULP闭合及DDGI身份保持。固定曝光暗部仍偏亮、墙偏暗；颜色质感和空间层次改善未认证。本轮只新增诊断脚本与报告，binary／SPIR-V冻结；新350≥65秒四SC／旧六main byte exact／GPU112/13/80保持。前轮Unit7／SC218／350480门与Metal是历史回归，本轮未新认定成对门或性能收益。</p><p>下一步追踪实际权重增幅对应的方向格聚合与SH历史可见性，避免用全局压暗、减权下限或调曝光掩盖来源问题。<a href="weight-paths/README.md">来源、边界与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

stratified=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('ddgi_stratified_trial')),None)
if stratified:
 v=stratified;body=''
 for name in ['R_arcade','floor_near','sunlit_wall']:
  a,b=[v['states'][s]['regions'][name]['actual_indirect_Y'] for s in ['off','on']];body+='<tr><td>'+name+'</td><td>'+format(a,'.8f')+'</td><td>'+format(b,'.8f')+'</td><td>'+format(v['changes'][name]['actual_indirect_Y_percent'],'.3f')+'%</td></tr>'
 extra='<h2 id="stratified-sampler">球面采样积分：解析偏差确认，模式3视觉未推广</h2><figure><img src="stratified-sampler/comparison350.jpg"><figcaption>左：新mode0；中：已有mode3实验；右：Lumen参考。EV1.8546，两侧128条主射线，无额外depth-only射线；只调整尺寸。</figcaption></figure><p>源码默认的黄金比例u/v坐标相关：理想INV_PHI²+INV_PHI=1使fraction坐标和只有0.5或1.5，是球面的相关一维曲线。CPU float32重放128方向占22/64方向格；mode3保持azimuth、纬度分层，占62/64。假设恒定单位radiance，原SH9投影和EvalLib在8192法线重建归一漫反射mode0为0.508669–1.522875，mode3为0.998830–1.001113。实际认证native法线的恒定场CPU重放：mode3相对mode0平均暗廊−3.289%、地面−3.666%、墙+7.386%，与真实墙−80.482%方向相反。这不能解释scene幅度变化，下一步必须检查实际producer来源。解析确认积分方向偏差；这不是Sponza实际ray距离／radiance、native方向SSBO、物理漏光或完整视觉根因证明。</p><table><tr><th>350实际间接光</th><th>mode0线性Y</th><th>mode3线性Y</th><th>变化</th></tr>'+body+'</table><p>同binary／全部源SPIR-V的新0/3实际九记录和Fusion闭合，原1%及2halfULP门保持。近场初始化开放moment39067→7140；882实际origins、view/depth/normal、整图albedo/ORM/AO byte exact。direct共有1053像素变化，其中direct_patch102，暗廊/地面/墙全部相同；整图和全部六ROI间接-only隔离不成立，明确拒绝。SH/moments/openSky/SPGI反馈与测量集合随新方向改变，不把收敛变化当单次测距或相同实际primary radiance。</p><p>350显示暗廊24.535→14.237（参考8.908）、地面77.094→64.678（59.454）更近；墙106.344→92.390（120.512）更远。480暗廊24.786→14.381、地面77.461→65.024、墙106.704→92.700同趋势。部分暗部幅度改善无法认证颜色质感和完整空间层次；墙损失明显。mode0原SC阶段350480 PASS，mode3两帧FAIL；P0两者FAIL。已有mode3保持实验且默认关闭，NOT_PROMOTED，沒有为解析分布或更暗画面放宽门。</p><div class="grid"><figure><img src="stratified-sampler/off350/fusion/indirect-linear-scale-1.png"><figcaption>mode0实际同帧间接HDR，linear scale1</figcaption></figure><figure><img src="stratified-sampler/on350/fusion/indirect-linear-scale-1.png"><figcaption>mode3实际同帧间接HDR，相同scale1</figcaption></figure></div><p>没有Engine/shader/build编辑，当前默认0恢复350≥65秒，四SC/旧六main exact/GPU112/13/80保持；新Unit7含SC218、脚本9通过。同版本全部SPIR-V与binary冻结；没有新稳定性能／Release／独占kernel结论，Metal完整回归是历史。下一步追踪改变方向后实际primary hit/escape/exhausted、SC radiance来源和SH历史，定位墙面能量丢失，不使用曝光／光强／全局压暗补偿。<a href="stratified-sampler/README.md">解析与原始实测、原门和恢复证据</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

primaryRaw=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_primary_raw_pair')),None)
if primaryRaw:
 v=primaryRaw;body=''
 for name in ['hit','escape','exhausted']:
  x,y=[v['shared_groups'][s][name] for s in ['off350','on350']];body+='<tr><td>'+name+'</td><td>'+str(x['rays'])+'</td><td>'+str(y['rays'])+'</td><td>'+format(x['final_Y_mean'],'.6f')+'</td><td>'+format(y['final_Y_mean'],'.6f')+'</td></tr>'
 extra='<h2 id="primary-raw">实际主射线：耗尽减少，命中radiance仍下降</h2><figure><img src="primary-raw/comparison350.jpg"><figcaption>左：新mode0只读诊断；中：新mode3只读诊断；右：Lumen参考。固定EV1.8546，两侧128主射线；没有推广渲染修正。</figcaption></figure><p>保留测试宿主显式DUMP9只读原始快照，支持0/3，无修改Engine/shader；普通运行无新GPU资源/dispatch/readback。实际更新列表、起点、ray radiance/status、SDF hits、normals、uniform、当前SH按完整buffer extent和Begin/End/Submit/Wait及显式Transfer/Host依赖保存。CPU最近更新与actual uniform frame350逐项保持，非列表ray槽全部排除；没有按mode0 CSV伪造mode3方向。当前882×9 SH与实际gather flat payload在256B header后逐字节一致；初次忽略header的直接比较已拒绝并纠正。</p><p>两侧各220实际更新probe，但仅70共同，比较其各8960条实际主射线。共同probe不代表方向/交点等价，两种采样刻意改变方向；数量、Y总和仅是该当前集合的描述。</p><table><tr><th>同probe的实际SDF状态</th><th>mode0数量</th><th>mode3数量</th><th>mode0最终radiance平均Y</th><th>mode3平均Y</th></tr>'+body+'</table><p>命中8489→8622，逃逸125→146，耗尽346→192；总最终radiance Y8549.816→6980.804（−18.351%），命中平均Y0.837715→0.642360（−23.320%）。这批数据不支持耗尽增多导致损失；但还没有SC/previous-screen/analytic分支原生标记，也不是全SH历史或屏幕因果，不能将所有命中当SC、负distance当未知天空或合计当物理能量。</p><p>两个DUMP9捕获旧SC oracle输入等价明确REJECTED：3841记录的方向/帧等不同，原因尚未认证，不能宣称这两捕获四SC及旧选择全等；失败日志和实际旧/新字段计数保存，原门未动。九记录全输出/六ROI原1%和Fusion2halfULP闭合分别通过，只证明同帧来源消费。后续普通默认0关闭新诊断的350回归四SC/旧六main byte exact/GPU112/13/80保持，原3256与20缺口保持；新默认480补齐原SC阶段350480PASS，最早P0FAIL，单列这组新普通验证。</p><p>DebugVulkan测试target构建、Unit7含SC218、脚本9通过，全部源SPIR-V保持；首编译getter名称错误拒绝后按现行API修正。新binary只加宿主诊断，无新视觉修正、稳定性能/Release或Metal结论。暗部/墙的三个视觉目标仍未闭合；下一步获取真实Finalize分支与实际SC hit radiance，沿共同producer probe及SH历史追踪墙面丢失。<a href="primary-raw/README.md">原始数据、失败范围与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


nativeSources=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_sources_pair')),None)
if nativeSources:
 v=nativeSources;body=''
 for name in ['analytic_native','SC_or_previous_UNKNOWN','escape','exhausted']:
  a,b=[v['shared_groups'][s][name] for s in ['off350','on350']]
  body+='<tr><td>'+name+'</td><td>'+str(a['rays'])+'</td><td>'+str(b['rays'])+'</td><td>'+format(a['final_Y_mean'],'.6f')+'</td><td>'+format(b['final_Y_mean'],'.6f')+'</td></tr>'
 extra='<h2 id="native-sources">原生来源：解析回退的历史光变化</h2><figure><img src="native-sources/comparison350.jpg"><figcaption>新mode0 / 新mode3 / Lumen参考；固定EV1.8546，仅调整展示尺寸。mode3未推广。</figcaption></figure><p>复用已有source9N记录，测试宿主DUMP9导出完整allocation，无Engine/shader改动。每侧实际220个更新probe、28160条射线：Trace和Finalize起点、方向全部逐位一致；frame350/probe/ray标记与当前更新槽核对，旧槽排除。实际原生方向覆盖22/64→62/64，当前SH与gather flat payload跳过256B header后逐字节一致。</p><p>仅81共同probe、各10368条射线；共同probe不能代表相同方向、几何或历史。解析positive-hit分支有原生marker，SC与previous-screen仍UNKNOWN；逃逸和耗尽按实际SDF符号单列。</p><table><tr><th>原生来源范围</th><th>mode0数量</th><th>mode3数量</th><th>mode0最终radiance平均Y</th><th>mode3平均Y</th></tr>'+body+'</table><p>共同probe的解析分支天空分量Y和472.945→450.579（−4.729%），历史分量1995.436→406.045（−79.651%），该分支最终radiance合计2468.381→856.624（−65.296%）。UNKNOWN组反而2415.115→2953.049，耗尽398→270。因此此批解析损失主要落在历史分量；不能把它解释为完整屏幕因果、SC退化、物理能量累积或已确认漏光。组件相加的CPU误差仅记录，未替换生产算术或改变旧选择门。</p><div class="grid"><figure><img src="native-sources/off350/fusion/indirect-linear-scale-1.png"><figcaption>新mode0同帧实际间接HDR，linear scale1</figcaption></figure><figure><img src="native-sources/on350/fusion/indirect-linear-scale-1.png"><figcaption>新mode3同帧实际间接HDR，相同scale1</figcaption></figure></div><p>更暗不能代替Lumen的颜色、质感、空间层次改善；墙面仍需保留正确的间接光，参考图只作视觉对照。两来源诊断旧SC输入门REJECTED（ray/first-hit/frame改变），四SC不认证；presence/masks/lifecycle和GPU112/13/80单列通过，native gather原1%与Fusion2halfULP闭合。新普通默认0关闭来源诊断350四SC、旧六main byte exact/GPU保持，480同binary；原SC阶段350480PASS，最早P0FAIL。仅保留诊断宿主/审计，无视觉算法推广、性能收益或Release结论。Unit7含SC218、脚本9通过，231份SPIR-V与前阶段相同。<a href="native-sources/README.md">原始来源、拒绝范围与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


historyGrid=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_history_grid_replay')),None)
if historyGrid:
 v=historyGrid
 extra='<h2 id="history-grid">历史四邻：18,537条闭合，1条原门拒绝</h2><figure><img src="history-grid/comparison350.jpg"><figcaption>本轮新mode0 350 / Lumen参考，固定EV1.8546，仅尺寸调整；无视觉算法修改。</figcaption></figure><p>测试宿主DUMP9新增读取同帧previousIndex历史SH，handle243/127008B。实际18,538条解析回退全部使用−rayDir法线，未采用细节SDF梯度；是当前记录与现行分支条件核对，不是几何正确法线认证。四邻位置、身份、权重是CPU float32重建，未冒充native query witness或完整历史writer。</p><p>原2e−5+5e−5×abs(expected)历史诊断门：18,537通过，probe254/ray10失败，max abs0.000170529，整体REPLAY REJECTED。改用实际EvalLib精确常量后仍失败，未改门。CPU查询fraction y=0.0009999275，紧贴现行0.001权重跳过边界；尚未确认GPU邻居或舍入是否跨界，不能断言插值不连续是画面根因。</p><p>CPU重建9052位置在历史grid外，实际历史component全部零；7874包含自身probe。这是当前输入描述，不是能量积累或漏光认证。已有历史余弦选项仍未推广，未用更暗代替三个视觉目标。新截图暗部仍偏亮、墙面间接光仍不足，颜色质感/空间层次未闭合。</p><div class="grid"><figure><img src="history-grid/off350/fusion/indirect-linear-scale-1.png"><figcaption>本轮实际同帧间接HDR，linear scale1</figcaption></figure></div><p>只保留测试宿主历史SH导出和失败可审阅的诊断；新DebugVulkan测试target构建/Unit7通过。来源诊断旧SC输入REJECTED，native gather原1%和Fusion2halfULP通过。关闭诊断的新普通350≥65秒四SC/旧六main exact/GPU112/13/80保持；本轮没有新480成对门、Metal或性能结论。前阶段SC-stage350480PASS/P0FAIL是历史。下一步补原生history query/tetra邻居记录，优先核验0.001附近的失败项并连接实际源法线可见性。<a href="history-grid/README.md">原始数据、失败与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


nativeQuery=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_query_replay')),None)
if nativeQuery:
 v=nativeQuery;body=''
 for name,row in v['native_source_paths'].items():body+='<tr><td>'+name+'</td><td>'+str(row['rays'])+'</td><td>'+format(row['final_Y_sum'],'.6f')+'</td></tr>'
 extra='<h2 id="native-query">原生hitPos与SC赢家：本帧屏幕回退为零</h2><figure><img src="native-query/comparison350.jpg"><figcaption>新模式0 query27N诊断350 / Lumen参考，固定EV1.8546，仅尺寸调整；没有修改shader或光照算法。</figcaption></figure><p>复用已有标量模式query27N，记录实际hitPos及16项SC候选/最小key赢家。frame/probe/ray标签、候选连续性、拒绝码与原严格最小key逐项核对；结合原解析marker区分本帧正命中。负状态按实际SDF符号单列，不推为历史来源。</p><table><tr><th>本帧原生路径</th><th>射线数</th><th>最终radiance Y和</th></tr>'+body+'</table><p>SC8539、解析18538、previous-screen0；不是整个SH历史或全屏来源普查，不能排除更早屏幕反馈。全部Trace/Finalize起点方向保持，实际SH/ray/HN/hits/update/history SH与前轮9N逐字节一致，原9N前缀保持；uniform仅query word63变化。新实际DDGI输出保持，但SPGI/direct/indirect/composite部分bytes不同，整图隔离不成立。</p><p>本轮27077正命中中5387条native hitPos与CPU separate公式有bit差，max1.907µm。唯一原门失败probe254/ray10的hitPos却逐位相同；使用实际位置后仍18537通过/1失败，max.000170529，整体REPLAY REJECTED。排除该失败仅由CPU hitPos重建造成；未记录GPU grid/tetra权重，不能确认.001边界取舍或画面插值不连续为根因。原门保持，不把更暗推广为颜色质感/空间层次改善。</p><figure><img src="native-query/off350/fusion/indirect-linear-scale-1.png"><figcaption>新同帧实际间接HDR，linear scale1</figcaption></figure><p>新350≥65秒exit0、GPU112/13/80与无fault、native gather原1%/Fusion2halfULP保持；旧SC输入门REJECTED，不宣称本诊断四SC等价。无Engine/shader/build修改，同binary/231SPIR-V与前阶段相同；前轮关闭诊断普通350四SC/旧六main exact/Unit7是同binary历史证据，本轮无新480/Metal/性能结论。下一步取得实际grid/四邻ID-weight及原生源法线，连接相邻像素来源切换。<a href="native-query/README.md">身份核对、失败与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


nativeGrid=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_grid_trial')),None)
if nativeGrid:
 v=nativeGrid
 extra='<h2 id="native-grid">原生四邻权重：重放差异已解释，临时诊断已移除</h2><figure><img src="native-grid/comparison350.jpg"><figcaption>左：临时33N原生诊断350；中：恢复普通默认350；右：Lumen参考。固定EV1.8546，仅调整展示尺寸；未推广视觉修改。</figcaption></figure><p>临时扩充query诊断allocation到33N，尾部记录实际historyPos/N/grid/四邻ID-weight/使用权重和/历史值，原history算术、.001取舍未改。随后恢复全部Engine/shader源及231份SPIR-V，五个编译变体的原始/临时模块和补丁保存，只保留离线审计。33N诊断实际解析18632条，9613在grid内、9019在外；原生输入重放全部通过既有历史容差，max component abs5.722e−6。</p><table><tr><th>probe254/ray10边界</th><th>CPU重建</th><th>当前诊断GPU实际</th></tr><tr><td>grid小数y</td><td>0.0009999275</td><td>0.0010004044</td></tr><tr><td>第4邻居</td><td>因原.001规则跳过</td><td>probe366，权重.0010004044</td></tr><tr><td>使用权重和</td><td>.9990000725</td><td>1.0</td></tr></table><p>原生邻居253/254/352/366及权重应用到旧原始history SH/rho/N后，旧component [.417824388,.354802370,.265629053]逐位重现，max abs0；原CPU重放拒绝保留，未改容差。这说明该重放失配对应CPU/GPU网格与邻居取舍差异；不是暗部偏亮的完整原因、源可见性或画面插值不连续认证。</p><p>临时诊断与旧27N仅208共同更新probe，origin/HN相同，update/hits/rays/当前SH/history SH改变，18632与旧18538不是同一测量集合。原生记录只能认证当前诊断消费；旧component交叉重放不等于旧GPU四邻witness或全图反事实。旧SC输入门REJECTED，不宣称诊断四SC等价；native gather原1%/Fusion2halfULP和GPU112/13/80单列PASS。</p><div class="grid"><figure><img src="native-grid/off350/fusion/indirect-linear-scale-1.png"><figcaption>诊断版本实际同帧间接HDR，linear scale1</figcaption></figure><figure><img src="native-grid/restored350/fusion/indirect-linear-scale-1.png"><figcaption>恢复普通默认实际间接HDR，相同scale1</figcaption></figure></div><p>恢复后的完整DebugVulkan构建/Unit7、V350/V480/Metal各≥65秒exit0/no fault；普通350四SC/旧六main exact/GPU112/13/80保持，原3256及20缺口恢复。新普通350480原SC-stagePASS，最早P0FAIL。暗部、颜色质感、空间层次仍未闭合；没有新性能、Release或视觉收益认证。下一步从原生四邻对应的历史SH及源法线可见性继续追踪实际间接光，再连接相邻像素样本与回退。<a href="native-grid/README.md">原始记录、失败范围、完整恢复与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)


historySegments=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_history_segment_stage')),None)
if historySegments:
 v=historySegments;body=''
 for kind in ['grid_CPU_position','bound_relocated_snapshot']:
  row=v['groups'][kind];c=row['counts'];body+='<tr><td>'+kind+'</td><td>'+str(c.get('before_endpoint',0))+'</td><td>'+str(c.get('no_geometry_hit',0))+'</td><td>'+str(c.get('after_endpoint',0))+'</td><td>'+str(row['negative_normal_connections'])+'</td></tr>'
 extra='<h2 id="history-segments">历史四邻连接：当前几何遮挡与法线方向</h2><figure><img src="history-segments/comparison350.jpg"><figcaption>新普通默认350 / Lumen参考；固定EV1.8546，仅尺寸调整。无Engine/shader或光照算法修改。</figcaption></figure><p>沿上一阶段原生33N实际四邻数据，153个producer probe各取最前最多8条grid内解析ray，共1195条；取样先于亮度/遮挡分类。四邻实际ID-weight-N连接到逻辑格点CPU世界位置及实际bound relocated快照，4773×2连接。独立新GPU/float64 CPU9546首交点全部一致，max.459µm，原20µm/64stack保持，端点分类无分歧；完整resident查询/节点/triangle与上传及scene XYZ逐项核对。本轮882 current gather源位置与旧原生采样快照byte exact。</p><table><tr><th>连接起点范围</th><th>查询点前几何</th><th>无几何命中</th><th>查询点后</th><th>N·towardSource&lt;0</th></tr>'+body+'</table><p>grid端点前signed历史分量Y和322.398449；bound relocated为273.990348。它们是选定1195个解析样本的有符号分解，原诊断容差闭合；不是全屏/暗廊ROI或物理漏光能量。位置不是全部历史SH真实发光原点，geometry采用双面不透明假设，alpha/真实opacity未认证；不能按遮挡计数删除贡献或确认漏光/积累/不连续根因。当前normal是原生查询N，不是首几何表面正确法线的证书。</p><p>bound当前端点前1322连接首mesh主要389(378)/67(174)/391(138)/390(134)/392(104)；grid1656主要389(456)/392(321)/391(222)/390(134)。mesh是原scene ordinal，非material/owner身份。首轮标签误用了raw9统计列，已按原生triangle ID raw7更正并重验，拒绝版保存，距离和原门不变。</p><figure><img src="history-segments/geometry350/fusion/indirect-linear-scale-1.png"><figcaption>新普通350实际间接HDR，linear scale1，未压暗。</figcaption></figure><p>新350≥65秒exit0/无fault、四SC/旧六main byte exact/GPU112/13/80和native gather原1%/Fusion2halfULP保持，原3256/20缺口恢复。binary/全部SPIR-V/Sponza.model冻结，无Engine/shader/build修改；本轮无新480成对门/Metal或性能声明，前轮SC-stage350480PASS/P0FAIL单列历史。三个视觉目标仍开放，下一步优先核对这批实际producer射线的真实首表面与法线，分清查询点资格，再追踪历史SH来源与相邻屏幕样本。<a href="history-segments/README.md">选样、身份、更正与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

jointSample37=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_joint_sample37_stage')),None)
if jointSample37:
 v=jointSample37;body=''
 for name,row in v['voxel']['query_groups'].items():body+='<tr><td>'+name+'</td><td>'+str(row['queries'])+'</td><td>'+str(row['zero_only_fallback_corners'])+'</td></tr>'
 extra='<h2 id="native-sample37">实际停步采样：下界零区域与首面资格</h2><figure><img src="native-sample37/comparison350.jpg"><figcaption>原生sample37诊断 / 完整恢复350 / Lumen，EV1.8546，无视觉改善。</figcaption></figure><p>在sampleBestSDF(pos)所在loop、二分之前直接保存actual pos/t，独立37N尾部不覆盖候选/history。28160实际ray身份/距离保持、完整min-key契约通过；旧首面proof的固定1159 RO/RD/query/hit全部逐位相同。499band早停sample/receiver全部byte exact；433cross早停仅4同位，sample→receiver位移中位4.102cm、最大17.776cm。前轮receiver八角不能直接当native stop采样。</p><p>沿同一已认证ray的原20µm分类带，429cross sample仍在首面之前，另4在首面之后；新增实际sample位置8497唯一体素查询。新完整384³ R16 GPU场/cache payload/空间uniform逐字节对应，全部原merge匹配门保持。429条早采样的3432角点全0，零值候选只有支持范围外AABB下界，没有零值网格测量；原生sample scalar也全0。这确认所选跨零分支使用下界零区域，并非真实首面命中；尚未证明它造成暗部屏幕抬亮。</p><table><tr><th>实际采样／首面分类</th><th>ray</th><th>零且仅下界角点</th></tr>'+body+'</table><p>角点身份仍由CPU重建，非独立GPU fetch IDs/descriptor或物理alpha证书。所有566跨零样本八角全0；band样本仍有混合来源，CPU weighted/native最大差67.7µm仅诊断，不作为原几何20µm通过。下一步针对已定位的表面资格问题做修复开关对照，并追踪暗部屏幕贡献；不改曝光、灯光或压暗。</p><figure><img src="native-sample37/restored/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>完整恢复350实际间接HDR，Fusion2halfULP闭合，linear scale1。</figcaption></figure><p>五临时源/12变化模块/全231SPIRV byte恢复并强制重编；full DebugVulkan、Unit7、V350480/Metal≥65秒exit0/no fault，四SC/六main exact/GPU112/13/80/native全帧六ROI原1%/Fusion2halfULP保持。SCstagePASS，P0缺原基线未重验，历史FAIL分列。诊断旧SC输入拒绝与native闭合独立列；两个既有glslang入口提示保留，12模块spirv-val和原入口身份一致。无性能或视觉收益，暗部/偏灰/层次差距仍在。<a href="native-sample37/README.md">实际位置、来源与完整恢复</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

jointReceiverVoxels=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_joint_receiver_voxels_stage')),None)
if jointReceiverVoxels:
 v=jointReceiverVoxels['audit'];body=''
 for name,row in v['query_groups'].items():
  body+='<tr><td>'+name+'</td><td>'+str(row['queries'])+'</td><td>'+str(row['active_corners'])+'</td><td>'+str(row['zero_only_fallback_corners'])+'</td></tr>'
 extra='<h2 id="joint-receiver-voxels">同批接收点：粗场零值从哪里来</h2><figure><img src="joint-receiver-voxels/comparison350.jpg"><figcaption>新完整场诊断 / 关闭诊断普通350 / Lumen；EV1.8546，仅展示缩放，无光照改善。</figcaption></figure><p>新384³ R16完整GPU场与冻结cache payload逐字节一致，实际resident空间参数与stop36相同，模型/cache SHA保持。固定1159实际接收点重建八角，8431唯一voxel全部通过现有CPU merge匹配门：6859下界winner、1572网格测量winner，max缓存/重放差0.000972m在原匹配门内，不借此认证几何距离。</p><table><tr><th>原生停步／真实首面分类</th><th>query</th><th>有效角点</th><th>零值且仅下界候选</th></tr>'+body+'</table><p>433条跨零早停接收点的八角全部为0，3464有效角点全部仅AABB下界零候选，没有网格测量零值；这些候选坐标在网格支持范围外。499条band早停的3992角点中，2737下界winner、1582仅下界零值，来源混合。确认的是接收点粗场数据来源：AABB下界不是已测表面。CPU角点不是native fetch证书；跨零二分后的receiver不等于最后march采样点。还不能称为暗部漏光、能量累积或插值不连续的屏幕根因。</p><figure><img src="joint-receiver-voxels/ordinary/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>新普通350实际间接HDR，Fusion2halfULP闭合，linear scale1。</figcaption></figure><p>DUMP9未进入field分支，旧raw被mtime拒绝；DUMP2缺analytic前置记录正常拒绝退出1；补齐现有标志后新读回成功，全部失败保存。没有Engine/shader/光照修改，binary/231SPIRV/model冻结。新普通350≥65秒exit0/no fault、四SC/旧六main exact/GPU112/13/80及全帧六ROI原1%/Fusion2halfULP保持；诊断旧SC输入拒绝与native闭合分列。无新Unit/Metal/480配对门、性能或视觉收益；上一完整回归为历史证据，P0缺基线未重验。暗部仍抬亮、偏灰、空间层次不足。下一步直接核验band早停和native最后采样位置，再连接暗部屏幕贡献，避免把保守下界当表面改灯光。<a href="joint-receiver-voxels/README.md">原始来源、范围和重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

jointStop=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_joint_stop36_stage')),None)
if jointStop:
 extra='<h2 id="joint-stop36">同批射线：SDF停步与BVH首面为何不同</h2><figure><img src="joint-stop36/comparison350.jpg"><figcaption>联合停步诊断 / 完整恢复350 / Lumen参考；固定EV1.8546。诊断没有改善光照。</figcaption></figure><p>不是BVH命中而SDF未命中：本批主要是SDF已经命中，但停步距离与独立BVH真实首三角形不同。BVH在这里核验同一记录射线，尚未替换生产SDF路径。独立36N尾部保存native停步，同时保留全部16候选及history记录；28160当前ray的stamp/身份与实际hit距离逐位保持，候选min-key原契约通过。上一首面proof固定1159的RO/RD/query/hit全部逐位对应，排除0条。</p><table><tr><th>同1159真实首面分类</th><th>样本</th><th>距离带停步</th><th>有符号跨零（当前粗场值0）</th></tr><tr><td>SDF在真实首面之前</td><td>932</td><td>499</td><td>433</td></tr><tr><td>SDF在真实首面之后</td><td>76</td><td>40</td><td>36</td></tr><tr><td>50m内无BVH三角形hit</td><td>151</td><td>54</td><td>97</td></tr></table><p>50m无hit不是天空证书；粗场跨零不是triangle表面证书。早停499/433对应两个实际分支，尚未认证这些标量的具体voxel来源，也没有证明暗部屏幕偏亮由它们导致。漏光、能量累积、插值不连续仍是待验证假设。下一步同批停步位置追溯实际粗场零值／距离带来源，再连接暗部相邻像素的权重、方向和回退。</p><figure><img src="joint-stop36/restored/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>恢复350实际间接HDR，原Fusion2halfULP闭合、linear scale1；无压暗。</figcaption></figure><p>同capture history18538/inside9486及原analytic门、history/origin/完整HN与writer before逐字节保持；21帧实际SH/confidence链和原projection/temporal门通过，更早SH来源和物理可见性未认证。首诊断检查误用32步上限，按源码默认64修正并保留失败记录；未放宽渲染门。诊断旧SC输入仍REJECTED，native gather/Fusion独立通过。五源/12变化模块及全231SPIR-V已byte恢复并强制重编；full DebugVulkan、Unit7、恢复V350480/Metal≥65秒exit0/no fault，四SC/六main exact/GPU112/13/80/native闭合保持。SC阶段PASS；P0原始基线临时文件缺失，本轮未重验，上轮FAIL仅为历史记录，无性能或视觉收益。固定曝光对照中暗部仍抬亮、颜色偏灰、空间层次不足。<a href="joint-stop36/README.md">原始证据、失败和恢复记录</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

historyFirst=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_history_first_stage')),None)
if historyFirst:
 v=historyFirst
 extra='<h2 id="history-first">历史查询点资格：同批1195原生ray真实首表面</h2><figure><img src="history-first/comparison350.jpg"><figcaption>新普通350 / Lumen参考，固定EV1.8546，只调整展示尺寸。诊断没有改变生产光照。</figcaption></figure><p>沿前轮固定1195样本，直接复制原GPU Trace/Finalize RO/RD，使用实际uniform RayMaxDistance50m。新独立GPU/float64 CPU全一致，max2.616µm，原20µm/64stack保持，unknown0；resident query与triangle/scene XYZ身份核对。1042条有首几何面，153条50m内无几何命中，后者不认证天空。SDF距离早于首表面960条、晚于82条，原20µm同距离带0条。</p><p>有首面的1042条中，偏差≤.05m为16条、≤.45m为73条、&gt;.45m为969条；.05/.45仅诊断分桶，不改原门。SDF−first中位−3.813m、5%−14.991m、95%1.067m。原生查询N与迎向原射线的三角形几何N夹角中位47.578°、95%79.929°。几何点重心重建max13.109µm通过原20µm门，但平面N并非材质平滑/alpha或SDF梯度资格证书。</p><p>按probe/ray连接上一历史四邻：bound端点前1322连接中，1099来自SDF早于首面的查询、170晚于、53无首面。该分层说明必须先核验查询点位置/法线与SDF proxy对应，不能把前轮遮挡全部称为正确表面上的漏光；历史SH发射来源、物理opacity及屏幕因果仍开放。</p><figure><img src="history-first/first350/fusion/indirect-linear-scale-1.png"><figcaption>新同帧实际间接HDR，linear scale1，未压暗。</figcaption></figure><p>新350≥65秒exit0/no fault，四SC/旧六main byte exact/GPU112/13/80、原3256/20缺口保持；native gather原1%/Fusion原2halfULP通过，882源锚点与旧快照byte exact。binary/全部SPIR-V/model冻结，无Engine/shader/build或灯光修改，无新480成对门/Metal/性能结论。参考对照中暗廊仍被抬亮，颜色材质分离与远近空间层次未改善认证。下一步优先定位这些查询点对应的SDF proxy、梯度来源及真实首mesh资格，再连接相邻屏幕像素的实际样本/回退；不直接推广全局首面替换。<a href="history-first/README.md">原始记录、边界与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

sdfQuery=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_sdf_query_field_stage')),None)
if sdfQuery:
 v=sdfQuery;body=''
 for name,row in v['groups'].items():body+='<tr><td>'+name+'</td><td>'+str(row['rays'])+'</td><td>'+str(row['query_in_hit_band'])+'</td><td>'+str(row['query_nonpositive'])+'</td><td>'+str(row['native_HN_valid'])+'</td></tr>'
 extra='<h2 id="sdf-query-field">历史查询SDF资格：命中带与法线回退</h2><figure><img src="sdf-query-field/comparison350.jpg"><figcaption>左：新只读SDF诊断350；中：关闭诊断普通350；右：Lumen参考，EV1.8546，仅尺寸调整。无生产光照修改。</figcaption></figure><p>新384³ R16F scalar readback与生产cache payload逐字节一致，完整空间参数与旧native33N相同。沿旧1195原生query与前新几何测量做CPU float32八fetch三线性诊断，不冒充GPU march/gradient witness。全部query在现有.45m命中带内，580采样值为0；实际旧HN valid全0，源码保留−rayDir回退。</p><table><tr><th>真实首面相对SDF</th><th>ray</th><th>query进入现行band</th><th>query scalar≤0</th><th>实际HN有效</th></tr>'+body+'</table><p>整个场56623104 voxel，negative1179775、zero15410067（不是全部unsigned或全部zero场），min−.565918/max11.992188m。SDF早于first960条的query scalar中位.006956m、95%.391311m；153条50m无几何首面的query也都在band内。CPU raw scalar梯度对native N夹角中位51.827°，不是detail/evidence法线资格，不能据此替换法线。当前确认是字段/查询点与现有回退关系，物理alpha、完整SH发光来源、漏光/积累/屏幕不连续因果仍未认证。</p><figure><img src="sdf-query-field/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>关闭诊断新普通350实际间接HDR，linear scale1，未压暗。</figcaption></figure><p>初次capture漏设现有history要求的ANALYTIC_AUDIT，被拒绝，失败日志保存；补齐新测量未改门。只读诊断SC旧输入REJECTED（ray/first-hit/frame改变），不宣称等价；诊断native gather原1%/Fusion原2halfULP和GPU112/13/80单列通过。随后同binary普通350四SC/六main byte exact/GPU112/13/80/3256及20缺口保持，两成功capture各≥65秒exit0/no fault，binary/231SPIR-V/model冻结。无Engine/shader/build或灯光编辑，无新480/Metal/paired门/性能。暗部广泛抬亮、中性偏灰色调与远近遮蔽层次差距保留。下一步追踪局部零/近零带与具体mesh SDF来源及实际停步状态，再连接相邻屏幕样本回退，不能用额外压暗或全局首面替换换门。<a href="sdf-query-field/README.md">范围、失败与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)

nativeMarch=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_march_stage')),None)
if nativeMarch:
 v=nativeMarch;body=''
 for name,row in v['groups'].items():body+='<tr><td>'+name+'</td><td>'+str(row['rays'])+'</td><td>'+str(row['origin_crawl_rays'])+'</td><td>'+str(row['steps_quantiles'])+'</td></tr>'
 extra='<h2 id="native-march">原生SDF停步：零值带与分支实测</h2><figure><img src="native-march/comparison350.jpg"><figcaption>左：新27N原生步进诊断350；中：完整恢复普通350；右：Lumen参考。EV1.8546，仅展示尺寸调整，无生产光照修正。</figcaption></figure><p>CPU沿旧1195 native RO/RD与上一真实field重放，1163通过原20µm、32失败，max4.675865mm；整体REPLAY REJECTED，不放宽门。临时复用query27N最后3候选诊断槽保存GPU stop/step/crawl/coarseT/prevT-prevD/stopD及frame/probe/ray stamp，未改灯光、ray预算或停步算术；这些槽不消费radiance且暂不认证完整16候选审计。</p><table><tr><th>本轮实际GPU停步</th><th>ray</th><th>起点crawl</th><th>step min/median/p95/max</th></tr>'+body+'</table><p>28160实际当前ray、标签/RO-RD/extent全核对，native coarseT与最终hit buffer逐位一致，HN valid全0。13578零穿越分支stopD全部为0（不是负值），positive-band stopD中位.188758m；这认证本轮原生分支数值，不证明零值是合格物理面。旧1195中本轮976共享原点/方向，距离976逐位保持，500 band/476 zero-cross，CPU分支0分歧；其中31原CPU距离失败仍保留，219旧样本未在本轮更新。共享zero-cross中363 SDF早于first/36晚于/77无50m首面，band429/35/36；旧几何双面不透明假设仍分列，不确认漏光/能量积累/屏幕不连续。</p><p>首次binary仍分配33N而源码27N，源mtime早于旧object、增量build跳过重编；布局检查REJECTED。强制重编译现行源码后新捕获明确27N；旧33N截图/日志保存不作native证书。临时两个shader源与10个模块候选保存，全部231SPIR-V及两源最终逐字节恢复，并刷新mtime避免恢复后旧object/module被误当最新。完整DebugVulkan/Unit7、V350480及Metal≥65秒exit0/no fault通过；恢复普通350四SC/六main byte exact/GPU112/13/80与3256/20恢复保持。新SC-stage350480PASS、最早P0FAIL分列。诊断SC旧输入REJECTED；诊断及恢复native gather原1%/Fusion原2halfULP分别通过。无新性能或视觉收益认证。</p><figure><img src="native-march/restored350/fusion/indirect-linear-scale-1.png"><figcaption>恢复后新普通350实际间接HDR，linear scale1，未压暗。</figcaption></figure><p>近处拱顶/暗廊仍抬亮、中性偏灰的颜色与参考明暗分离、远近遮蔽层次差距仍在。下一步对这些原生零值停步点追踪局部scalar corners与具体mesh SDF min来源/支持资格，再沿实际邻像素样本/回退定位屏幕影响；不直接改阈值、换法线或推广全局首面替换。<a href="native-march/README.md">原生证据、失败与完整恢复重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
voxelSources=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_voxel_source_stage')),None)
if voxelSources:
 extra='''<h2 id="native-voxel-sources">原生零值停步：具体 SDF 来源与支持范围</h2><figure><img src="native-voxel-sources/comparison350.jpg"><figcaption>新只读普通350 / Lumen参考，EV1.8546。只调整展示尺寸，没有改变光照输出。</figcaption></figure><p>沿上一阶段976条已核对原生RO/RD、距离及实际query位置的共享射线，CPU重建八角点，去重7157体素。新只读AuditCacheVoxels逐mesh重放既有merge：7157全部match；cache值与上一真实R16 field逐位一致。角点索引是CPU重建，没有新增原生GPU texel fetch证书；merge沿既有1mm＋相对误差门，不替代原20µm射线门。</p><table><tr><th>原生零值停步分组</th><th>射线</th><th>角点（全零／仅下界零）</th><th>支持样本min≥.45m角点</th></tr><tr><td>SDF早于first</td><td>363</td><td>2904</td><td>2864</td></tr><tr><td>SDF晚于first</td><td>36</td><td>288</td><td>288</td></tr><tr><td>50m内无几何首面</td><td>77</td><td>616</td><td>612</td></tr></table><p>476条零值停步的3808角点全部没有mesh_sample零候选，零候选全部为aabb_lower_bound；3764角点的支持样本min仍≥.45m。.45只用于诊断分桶，没有更改停步门。SampleMeshSDF的网格支持域是local[0,res−1]，超出时返回AABB距离下界；世界点仍在AABB内部时这个保守下界为0，不是实际表面距离。全部5937零候选来自32³来源，主要超出y支持4121、z1567；所有ties保留，first winner随原merge顺序，来源名称不等于render mesh ordinal、SC owner或材质。</p><p>核对上一实际query选中card字段及analytic两项marker，476zero和500band共享射线全部走analytic/history回退，SC selected为0。此连接证明这批查询的消费路径，没有证明其历史能量是否多次累积或它对当前暗部像素的贡献。不能外推到全部13578原生zero、全场景、alpha可见性，也不能宣布漏光已确认。</p><figure><img src="native-voxel-sources/source350/fusion/indirect-linear-scale-1.png"><figcaption>本轮实际间接HDR，linear scale1，原2halfULP闭合通过，未额外压暗。</figcaption></figure><p>暗部间接光、颜色与质感、空间层次仍未认证改善；画面保留暗廊抬亮、偏灰和明暗分离不足的差距。本轮普通350四SC审计、旧六main逐字节、GPU112/13/80、DDGI全帧及六ROI原1%、Fusion原2halfULP通过，binary/231SPIR-V/三个源资产SHA保持。没有Engine或shader修改，本轮没有新Unit、Metal、480配对门或性能认证；上一阶段完整回归与SC-stagePASS/P0FAIL是历史独立范围。下一步连接这些来源不合格的查询与实际邻像素样本及回退，再决定是否需要把保守推进下界和表面命中资格分开。<a href="native-voxel-sources/README.md">证据范围与重建</a></p>'''
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
jointFirst=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_joint_first_stage')),None)
if jointFirst:
 extra='<h2 id="joint-first-surface">同批1159接收点：真实首表面资格</h2><figure><img src="joint-first-surface/comparison350.jpg"><figcaption>新普通／首表面proof350与Lumen参考，EV1.8546。本轮没有渲染算法或光照修改。</figcaption></figure><p>沿上一联合capture同1159实际RO/RD、50m上限，新GPU与独立float64 CPU首交点全部一致max2.616µm，unknown0，原20µm/64stack、query/resident scene XYZ身份保持；场景三角形与上一9262连接proof byte exact。1008有首面/151无面（不是天空证书），SDF早于首面932/晚于76/原数值带内0。首面重心重建max13.109µm通过原门；实际查询N相对迎向原射线的几何法线中位46.757°、p95 79.929°，不是平滑/alpha法线证书。</p><table><tr><th>接收点资格诊断</th><th>结果</th></tr><tr><td>SDF−first距离中位</td><td>−4.015066m</td></tr><tr><td>有首面中距差≤.05m /≤.45m /&gt;.45m</td><td>13 /64 /944</td></tr><tr><td>原1254重定位端点前遮挡：early /late /无首面</td><td>1042 /156 /56</td></tr></table><p>.05/.45仅诊断分桶，原20µm门不变。接收位置/N与上一原生history cohort逐项保持，来源分量关联保留；1254遮挡中大部分发生在早于真实首面的查询，不能统称正确表面漏光。更早混合SH来源、物理alpha、屏幕能量和因果仍开放。下一步先核验同批离面query的SDF具体来源与原生停步，再连接实际暗部邻像素样本/回退，不能直接删除能量或盲推全局首面替换。</p><figure><img src="joint-first-surface/geometry350/fusion/indirect-linear-scale-1.png"><figcaption>新350实际间接HDR，原Fusion2halfULP闭合，linear scale1。</figcaption></figure><p>新普通350≥65秒exit0/no fault，binary/231SPIR-V/model SHA冻结；四SC/旧六main exact/GPU112/13/80、DDGI全帧六ROI原1%/Fusion2halfULP保持，3256/20恢复不变。本轮无Engine/shader/build/新Unit/Metal/480配对门/性能认证；上一完整恢复及SC-stagePASS/P0FAIL明确为历史证据。暗廊/拱顶仍抬亮、偏灰和空间层次不足，三视觉目标未改善。<a href="joint-first-surface/README.md">首表面、关联证据与重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
jointHistory=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_joint_history_writer_stage')),None)
if jointHistory:
 extra='<h2 id="joint-history-writer">同捕获四邻／writer输入与新几何遮挡</h2><figure><img src="joint-history-writer/comparison350.jpg"><figcaption>新联合诊断（SC旧输入REJECTED） / 完整恢复350 / Lumen；EV1.8546。本阶段不作视觉改善。</figcaption></figure><p>原生history33N与private producer33N同捕获：18538实际analytic查询、9486 grid内；原position/N/tetra IDs/weights/value的analytic重放通过，max5.722µ SH数值。primary history SH/origin/完整HN与350 producer dispatch-before输入逐字节一致。有效邻点涉及641probe，其中540最近writer在330–350窗口内；21帧4620更新/752不同probe、重复位置变化0，全部882最近writer312–350位置保持，actual SH/confidence跨帧链和原projection/temporal门通过。更早混合SH位置/native module绑定与CopyBuffer命令资格仍UNKNOWN。</p><table><tr><th>新9262几何连接（双面不透明假设）</th><th>原格点</th><th>已连接writer当前位置</th></tr><tr><td>端点前有几何</td><td>1578</td><td>1254</td></tr><tr><td>无几何hit</td><td>3049</td><td>3369</td></tr><tr><td>端点后hit</td><td>4</td><td>8</td></tr><tr><td>负查询Ndot</td><td>2846</td><td>2814</td></tr></table><p>按每probe最先最多8条grid内analytic ray、先于亮度/遮挡选择1159样本，4631有效四邻×两端点共9262新GPU连接。独立float64 CPU首交点全部一致max.420µm、端点分类0分歧，原20µm/64stack和完整resident XYZ/query身份保持。仅几何遮挡证据，不是物理alpha、全部SH来源或暗部屏幕贡献；N仍为−RD且接收位置的真实表面资格尚未核验，不能把1254统称漏光。已准备同1159实际RO/RD的50m首表面查询，尚未取得该新集合GPU证书。</p><figure><img src="joint-history-writer/restored/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>恢复350实际间接HDR，原Fusion2halfULP闭合、linear scale1。</figcaption></figure><p>首构建被7 SPIR-V freshness拒绝，按仓库参数生成/spirv-val后重试通过；joint JSON首版numpy int序列化失败已修复，失败日志保留。诊断旧SC输入REJECTED，native gather/Fusion独立通过，不借恢复门认证诊断。全部4源/7变化模块逐字节恢复、全231SPIR-V核验并强制重编；完整DebugVulkan build/Unit7/V350480/Metal≥65秒exit0/no fault，恢复四SC/六main exact/GPU112/13/80、全帧六ROI原1%与Fusion2halfULP保持。SC-stagePASS/P0FAIL分列，无性能或视觉收益；暗部抬亮、偏灰与空间层次差距仍在。下一步同样本首表面资格，再连接实际邻像素来源与回退。<a href="joint-history-writer/README.md">联合原始输入、新几何与完整恢复</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
writerMode0=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_mode0_writer_stage')),None)
if writerMode0:
 extra='<h2 id="mode0-writer-origins">模式0跨帧writer位置：稳定性与来源边界</h2><figure><img src="mode0-writer-origins/comparison350.jpg"><figcaption>新mode0私有producer诊断（SC旧输入REJECTED） / 完整恢复350 / Lumen；固定EV1.8546，不作视觉改善。</figcaption></figure><p>330–350同一次运行21快照，实际4620更新覆盖752不同probe，重复writer位置变化0；每次实际Trace/Finalize起点与producer dispatch-before origin输入逐位一致。最终882个最近writer记录（312–350）的origin与当前缓冲逐位一致。20段actual SH/confidence→下一history输入bitexact，当前原projection/temporal analytic门通过；330以前混合SH位置 ancestry、物理可见性及本次native module/binding/CopyBuffer命令证书仍UNKNOWN。</p><p>新模式0与旧模式4同窗口21帧的origin/history/actual SH/rays四种payload逐字节一致，具体代数数据可对应；不反向补齐模块绑定资格，也不等于与33N捕获相同。新旧33N只有origin全体一致，updates/hits/rays/source/query/analytic及history不同，因此不能拼接history权重和能量。</p><figure><img src="mode0-writer-origins/restored/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>完整恢复350实际间接HDR，原Fusion2halfULP闭合，linear scale1。</figcaption></figure><p>临时入口只允许显式DUMP9私有审计和有界Flush，没有光照算术改动；已逐字节撤除并强制重编。完整DebugVulkan build/Unit7、恢复V350480/Metal各≥65秒exit0/no fault通过；恢复四SC、旧六main byte exact/GPU112/13/80、DDGI全帧六ROI原1%及Fusion2halfULP保持，SC-stagePASS/P0FAIL分列。诊断旧SC输入REJECTED，独立native gather/Fusion通过，失败保留且不放宽门。仍有暗廊与拱顶抬亮、偏灰和空间层次不足，没有性能或视觉收益认证。下一步将同capture实际history取样与位置窗口联合诊断，再验证几何遮挡，不调alpha/曝光/强度。<a href="mode0-writer-origins/README.md">原始数据、模式对应和恢复</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
originContract=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_origin_contract')),None)
if originContract:
 extra='<h2 id="native-origin-contract">当前位置契约：网格权重与测量起点分列</h2><figure><img src="native-history-current/comparison350.jpg"><figcaption>沿上一模式0原生捕获重分析；诊断 / 恢复350 / Lumen，EV1.8546。本轮没有新运行或视觉算法修改。</figcaption></figure><p>28,160条实际更新射线的Trace与Finalize起点，与同捕获origin缓冲的有效位置逐位一致。882个probe状态全部为1，没有借格状态0.5。33,970个有效history邻点中14,662个当前位置偏离原格点；当前位移min/median/p95/max为0 /0 /1.500001 /2.400003m。取样点至当前位置距离为.064273 /2.688186 /4.722053 /6.307345m。原插值仍按原网格四邻权重，源码没有在samplePrevProbeGrid内执行几何可见性或origin状态权重；这是具体机制，不是已确认漏光根因。</p><p>20,876个邻点方向与实际查询N点积为负；N是此前确认的−RD，尚非合格表面法线。负点积不能直接作为拒绝规则。current origin不是previous SH全部writer的位置证书，跨帧位置稳定性和物理可见性仍UNKNOWN。下一步取得同模式history writer位置及跨帧稳定性，再沿有效权重连接验证几何遮挡；不能用旧模式4窗口拼接。</p><p>首版离线脚本误取ProbeOrigin.w作spacing，产生不合法位移统计，已拒绝保留并修正为UBO offset16；没有改渲染或放宽门。新只读脚本原生位置契约通过，原始SHA与失败版本归档。没有新构建、Unit、Metal、配对门或性能结论；上一完整恢复回归继续明确为历史证据。三个视觉目标未改善。<a href="native-origin-contract/README.md">原始证据、范围和重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
historyCurrent=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_history_current_stage')),None)
if historyCurrent:
 extra='<h2 id="native-history-current">新原生history位置／方向／四probe权重，失败与恢复分列</h2><figure><img src="native-history-current/comparison350.jpg"><figcaption>模式0 DUMP9取样诊断（SC旧输入REJECTED） / 完整恢复350 / Lumen参考；EV1.8546。诊断不作生产视觉改善。</figcaption></figure><p>新350捕获原生17862 analytic取样，inside8518/outside9344；位置与同帧query bitexact、N全部逐位=-RD，真实gp/四probe ID/weight/used total/evaluated value与同帧history SH重放原analytic门PASS，max5.722µ SH数值（不是20µm几何门）。actual native Finalize模块、pipeline/layout/set绑定、dispatch关联通过；VK_WHOLE_SIZE由额外记录的实际allocation覆盖raw view，不能凭假定尺寸。不同日志流不认证每帧descriptor更新时序，native CopyBuffer链仍UNKNOWN。</p><table><tr><th>真实取样的几何输入统计</th><th>结果</th></tr><tr><td>网格spacing</td><td>3m</td></tr><tr><td>有效权重邻点段</td><td>33970</td></tr><tr><td>含自身probe的射线</td><td>7049</td></tr><tr><td>取样点→原网格邻点距离min/median/p95/max</td><td>.064273 /2.682730 /4.375174 /5.110979m</td></tr></table><p>网格邻点坐标由actual uniform重建，不是已认证previous SH producer的重定位位置，也不是几何可见性证书。self/距离较长只是诊断，不能直接断言漏光、累积、插值范围过大或不连续。下一步沿actual取样位置/权重及来源位置做几何遮挡核验，不能盲用其他法线替换。</p><p>首次宿主DUMP2外层筛选缺primary raw，失败日志和截图保存。第二次actual f59=4（rho control＋diagnostic模式，不是detail-SDF），超出mode0 source契约；旧SC主入口亦REJECTED。DUMP9最终原生history/绑定闭合，但诊断SC旧ray/first-hit输入仍REJECTED（3852行差异，首9float max49.94665（量纲混合），不能当距离误差）。全部失败保留，不放宽门。上一330–350窗口f59同为4，且history payload与新mode0 DUMP9不逐位一致；原窗口内部代数闭合保留，不能当默认Finalize输入证书或与本次取样直接拼接。</p><figure><img src="native-history-current/restored/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>完整恢复350实际间接HDR，linear scale1、原2halfULP通过。</figcaption></figure><p>7临时源文件及5变化SPIR-V逐字节恢复，全部231SPIR-V核对并强制重编；完整DebugVulkan build/Unit7、Vulkan350480和Metal≥65秒exit0/no fault通过。恢复350四SC/旧六main exact/GPU112/13/80、全帧六ROI原1%/Fusion2halfULP，3256与20恢复保持；新SC-stage350480PASS、最早P0FAIL分列。诊断DUMP9只认证native gather/Fusion闭合，不能借恢复门认证其SC输入。没有性能/视觉收益；近拱顶/暗廊抬亮、偏灰和近远明暗层次差距仍在。<a href="native-history-current/README.md">原生取样、失败、模式边界与完整恢复</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
windowSources=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_window_sources_stage')),None)
if windowSources:
 names=['window_boundary_history','window_analytic_history','window_analytic_sky','window_SC_selected','window_exhausted'];body=''.join('<tr><td>'+k+'</td>'+''.join('<td>'+format(v['signed_term_fraction'][name]*100,'.4f')+'%</td>' for name in names)+'</tr>' for k,v in windowSources['regions'].items())
 extra='<h2 id="native-window-sources">窗口测量的实际来源：history回退与SC分列</h2><figure><img src="native-window-sources/comparison350.jpg"><figcaption>新普通350 / Lumen参考，固定EV1.8546。来源分解使用上一已归档330–350原生窗口，不把新画面当同捕获来源证书。</figcaption></figure><p>21帧591360条actual ray逐帧核对Trace/Finalize/query frame-probe-ray标签、RO/RD、analytic两个marker及positive selected-card header；380078 analytic、174410 positive SC选中、previous-screen0、15007 escape、21865 exhausted status。全部positive HN valid为0；源码默认N=-rayDir，尚无该窗口完整原生history取样位置/法线/tetra记录。负距离status不等于其radiance来源已认证，耗尽周边SC回退或black仍需原生来源witness。</p><table><tr><th>固定屏幕样本</th><th>330前历史</th><th>窗口analytic history</th><th>窗口analytic sky</th><th>窗口positive SC选中</th><th>窗口耗尽status</th></tr>'+body+'</table><p>实际原生sky/history分量分别按真实producer distance weighting、basis和128射线顺序投影，再沿实际alpha/原clamp比例递推。每次actual调整RGB、SH投影和EMA总和、f32 gather/材质原analytic门通过；actual half舍入单列。有符号份额、略偏100%，不是物理能量或删除反事实。edge SC微小负份额来自SH代数，不能裁零制造正能量。三个ROI escape项为0仅适用于这些样本；previous-screen0仅适用于本窗口，不外推所有帧。</p><p>暗廊history回退13.1094%、analytic sky2.9424%、positive SC .6474%、耗尽status .9550%，窗口前82.3826%；近地positive SC18.5044%显著不同。结论是下一步应核验history反馈的具体取样位置、方向和可见性，不证明漏光或能量累积，不用份额压暗。完整更早history、native CopyBuffer链、SPGI内部tap/历史回退仍UNKNOWN。</p><figure><img src="native-window-sources/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>新普通350实际间接HDR，linear scale1、原2halfULP通过，无压暗。</figcaption></figure><p>新普通350≥65秒exit0/no fault；四SC/旧六main byte exact/GPU112/13/80、native全帧六ROI原1%、Fusion原2halfULP PASS，binary/231SPIR-V/三资产SHA冻结。无Engine/shader修改、新构建、Unit/Metal/480配对门或性能/视觉收益。前轮完整回归与SC-stagePASS/P0FAIL历史分列。暗廊/拱顶抬亮、偏灰和近远明暗层次差距保留。<a href="native-window-sources/README.md">实际分量、范围和重建</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
lineageStage=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_lineage_stage')),None)
if lineageStage:
 rows=lineageStage['screen']['regions'];body=''.join('<tr><td>'+k+'</td><td>'+format(v['signed_term_fraction']['window_boundary_history']*100,'.4f')+'%</td><td>'+format(v['signed_term_fraction']['window_ray_measurement']*100,'.4f')+'%</td></tr>' for k,v in rows.items())
 extra='<h2 id="native-lineage">真实跨帧历史链：330以前的输入仍占主要份额</h2><figure><img src="native-lineage/comparison350.jpg"><figcaption>连续原生诊断350 / 完整恢复普通350 / Lumen，EV1.8546。</figcaption></figure><p>临时只读诊断采集330–350连续21份快照、4620当前更新。20段相邻actual SH与confidence→下一before input逐字节一致，当前projection/temporal与previous writer标签均通过原门；第一份边界此前UNKNOWN。所有当前alpha=.02，不改混合或生产输出。</p><table><tr><th>固定屏幕样本</th><th>330以前历史项</th><th>330–350测量项</th></tr>'+body+'</table><p>窗口项沿实际alpha和每次原clamp比例递推，经当前实际gather/Fusion输入映射原analytic f32门通过；量化误差单列。这是有符号代数份额，非物理能量或删除反事实，略偏100%源于float/half舍入。暗廊82.38%仍来自窗口前输入，不能将21帧系数增长当作累积根因，也不能把窗口测量都标为350当前；完整更早history与来源可见性未闭合。近层payload exact，但native CopyBuffer链仍UNKNOWN。</p><figure><img src="native-lineage/restored/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>恢复350实际间接HDR，linear scale1，原2halfULP门通过，无压暗。</figcaption></figure><p>临时inl逐字节恢复并强制重编；完整build首次并行shader目录复制失败，串行重试通过，日志保留。Unit7/7、Vulkan350480与Metal≥65秒exit0/no fault。诊断及恢复350四SC/六main exact/GPU112/13/80、全帧及六ROI原1%/Fusion原2halfULP均PASS；新SC-stage350480PASS、最早P0FAIL分列。全部生产SPIR-V保持，无性能或视觉收益认证。固定曝光对照仍有近拱顶/暗廊抬亮、偏灰与明暗层次不足。下一步向窗口前writer追踪具体来源和消费可见性，不调曝光/alpha/光强。<a href="native-lineage/README.md">连续链、边界与恢复证据</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
confidenceStage=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_confidence_stage')),None)
if confidenceStage:
 rows=confidenceStage['temporal']['regions']
 body=''.join('<tr><td>'+str(k)+'</td><td>'+format(v['signed_term_fraction']['latest_history_input']*100,'.4f')+'%</td><td>'+format(v['signed_term_fraction']['latest_ray_measurement']*100,'.4f')+'%</td><td>'+format(v['fresh350_probe_ownership_signed_fraction']*100,'.4f')+'%</td></tr>' for k,v in rows.items())
 extra='<h2 id="native-confidence">实际 producer confidence 与最近一次历史贡献</h2><figure><img src="native-confidence/comparison350.jpg"><figcaption>新原生producer诊断350 / 新普通350 / Lumen参考；固定EV1.8546，仅缩放展示。</figcaption></figure><p>220当前更新probe的实际confidence均为1、alpha均为0.02；882最近writer记录完整核对，写入帧312–350，不能把全部射线标成本帧350。原生绑定、模块、调度、完成标签、dispatch-before输入与实际SH向量通过原analytic误差门。私有SH与生产SH最大差9.537e−7，非逐位一致；confidence/openSky逐位一致。生产近层SH与同帧packed gather payload逐位一致，但native CopyBuffer链仍UNKNOWN。新capture不反向认证旧476-ray条件证据。</p><table><tr><th>固定屏幕样本</th><th>最近历史项</th><th>最近射线测量项</th><th>350更新probe全部混合SH ownership</th></tr>'+body+'</table><p>实际EMA和原clamp比例、actual normal/confidence/weights/consumer输入映射原analytic f32门通过；实际half舍入误差单独记录。三ROI各64像素，edge12。份额为有符号代数分解，float/half舍入使份额和略偏100%，不是物理能量或删除反事实。约97.9%历史项符合0.02混合系数，不能认定能量正在增长；350更新probe的全部混合SH也不是本帧新射线测量。完整此前history来源、物理可见性和SPGI内部回退仍未闭合。</p><figure><img src="native-confidence/ordinary350/fusion/indirect-linear-scale-1.png"><figcaption>新普通350实际间接HDR，linear scale1，未额外压暗。</figcaption></figure><p>两次350≥65秒exit0/no fault；四SC、旧六main byte exact、GPU112/13/80、全帧及六ROI gather原1%、Fusion原2halfULP均PASS；binary/231SPIR-V/三源资产SHA冻结。无Engine/shader修改、新构建、Unit/Metal/480配对门或性能/视觉收益认证。历史完整回归与SC-stagePASS/P0FAIL分列。暗部抬亮、颜色偏灰和明暗层次差距保留，下一步追踪跨帧history来源及消费可见性，不通过改alpha、曝光或强度压暗。<a href="native-confidence/README.md">实际系数、误差与重建证据</a></p>'
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
voxelScreen=next((v for v in reversed(list(evidence.values())) if isinstance(v,dict) and v.get('DDGI_native_voxel_screen_stage')),None)
if voxelScreen:
 v=voxelScreen;body=''.join('<tr><td>'+html.escape(k)+'</td><td>'+str(int(g['pixels']))+'</td><td>'+f'{100*g["qualified_probe_ownership_signed_fraction"]:.4f}%'+'</td><td>'+f'{100*g["conditional_zero_measurement_signed_fraction"]:.6f}%'+'</td><td>'+f'{100*g["conditional_zero_history_component_signed_fraction"]:.6f}%'+'</td></tr>' for k,g in v['regions'].items())
 extra='''<h2 id="native-voxel-screen">零值射线与屏幕：历史SH所有权和本帧测量分列</h2><figure><img src="native-voxel-screen/comparison350.jpg"><figcaption>新普通350 / Lumen参考，固定EV1.8546。历史ray分析与新画面分列，未修改光照。</figcaption></figure><p>上一已归因476条zero连接117个probe。历史原生步进同捕获的近层producer SH与gather flat前882项payload逐字节一致；直接handle等价检查REJECTED（241 vs2953）。源码存在CopyBuffer至flat offset256打包，但本次没有native复制命令handle/offset witness，resource_chain_certified=false。不能称为同一资源或完整复制链证书。</p><p>220当前probe用实际ray radiance/原生方向重放完整128ray投影、距离权重、原SH clamp，并假定稳定alpha=.02；原analytic误差门全通过，max7.629µ、3个band clamp。缺原生producer confidence输入，所以条件分解保留为诊断，actual_confidence_source_certified=false。只将476子集的当前测量及其中history component通过不变的当前clamp比例和原gather权重映射；不是删除射线后的反事实、物理能量，不能用这个数做压暗强度。</p><table><tr><th>历史实际屏幕抽样</th><th>像素</th><th>117 probe混合SH ownership</th><th>条件本帧476测量</th><th>条件测量中history项</th></tr>'''+body+'''</table><p>每ROI固定8×8空间分层，另含原实际最大跃变邻像素，筛选不依赖亮度/遮挡。ownership包括这些probe的全部混合历史SH，与476-ray子集不同。暗廊41.56% ownership不能称476-ray能量；其条件本帧份额仅.015507%，只说明必须继续核验完整历史和全部zero来源，不能认定一次命中修正即可解决偏亮，也不能据此排除累积。</p><p>历史最大水平跃变(2259,776)→(2260,776)，actual indirect ΔY−.099035，DDGI−.064421/SPGI−.034652；新普通对应−.098793/−.064253/−.034571。两次位置相同，CPU full GBuffer位置相距3.8631m、法线点积.5544，材质采样也变；它不是连续表面干净对照，不作插值不连续根因。只观察实际过滤SPGI，内部tap/历史/回退仍UNKNOWN。新nine-record204 full/780 gather样本保存，5659几何查询仅prepare，没有新首交点证书。</p><figure><img src="native-voxel-screen/screen350/fusion/indirect-linear-scale-1.png"><figcaption>新普通350实际间接HDR，linear scale1，未额外压暗。</figcaption></figure><p>新普通350≥65秒exit0/no fault，四SC/六main byte exact/GPU112/13/80、全帧及六ROI native gather原1%、Fusion原2halfULP保持；binary/231SPIR-V/三源资产SHA冻结。没有Engine/shader修改、性能/视觉收益或新350480配对门；前一完整回归和SC-stagePASS/P0FAIL继续分列。暗部抬亮、颜色偏灰及层次差距未闭合。下一步取得同帧producer confidence、完整current zero来源及native flat复制链，再追踪跨帧history；不把推测份额变成调光参数。<a href="native-voxel-screen/README.md">拒绝范围、证据和重建</a></p>'''
 text=text.replace('<h2>可验证证据</h2>',extra+'<h2>可验证证据</h2>');(out/'index.html').write_text(text)
