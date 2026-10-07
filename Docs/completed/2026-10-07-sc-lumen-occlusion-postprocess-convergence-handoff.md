# SC／Lumen 后续交接：遮挡、后处理与时间收敛（2026-10-07）

本文件是新会话主入口。目标仍是让同一场景的间接光在暗部颜色、材质表现和空间层次上接近用户 Unreal Lumen 参考；本次只完成讨论交接与报告裁剪，没有新的渲染实验或视觉改善。不要把建议写成已完成实现。

## 当前状态与成果

- 当前生产源／二进制冻结以 `Docs/reports/sc-resolve-2026-10-03/native-primary-history-persistent-packet/current-state-freeze.json` 的265项为准；本次清理前后重新核验。原231 SPIR-V已恢复，最新阶段无活跃GPU进程。
- 每mesh SC保留原TriangleID，实例TLAS＋mesh三角BLAS、owner身份映射和持续辅助刷新已接通。主resolve 8190真实ray正确SC3236→3256，20旧地面缺口恢复，旧card/cacheT严格保持。
- 查询目录、mask、插入排序与fence三槽复用保留。历史Debug配对性能不可叠加，不代表Release预算；BOTTOM_OF_PIPE只是命令标记间隔。
- 448 `spgi-first-surface-owner` 窄修复KEEP、默认关闭。固定环境显式 `PRIMAL_SPGI_ZERO_HIT_GEOMETRY=2`：仅原SC失败且最终t=0、真实首面与材质/正面/owner目录就绪和原2mm/bary资格支持时重试；不改变旧accepted。350/480同binary对照真实间接HDR右暗廊约−16–17%、左−13–14%、受光墙约＋9%，灰亮减少、明暗区分增强，但颜色质感仍未达Lumen。不能把追加front-face前诊断命中数量当生产数量。
- 最近773–776诊断完整回退，父恢复验证包含完整serial DebugVulkan、UnitVulkan7／Debug5、ordinary Vulkan350/480、Metal≥65秒、SCstage PASS及原数值门。最新只读cohort匹配没有新GPU／构建／回归信用。
- 原P0基线缺失，本轮NOT_RUN/null，历史FAIL；旧display门FAIL单列。不能称全部门禁通过。

## 本会话用户观察与约定

用户观察：Lumen左右侧廊、机位超近顶部为非常低能量，中庭正常间接光；本引擎暗部偏灰褐、层次较平，颜色与材质表现不足。运行从低能量逐渐增加。遮挡是优先假设，不是已证明根因；低能量启动符合积累表现，但尚未证明后期正常收敛。

用户要求后续增加50、150、350、480、760帧采样。尽量同一次连续运行，不将五次独立启动伪称连续时间序列。保留350/480原门；760仍明显变化时追加更晚点。固定SC辅助刷新从120帧开始，50为启动前、150刚进入刷新，图表需标注该边界。每点记录实际frame、elapsed、probe更新次数、固定ROI亮度/RGB/邻像素变化；帧数不等于probe更新次数。

分pass输出必须沿同像素、同帧与实际历史关联，不能只看各自自动拉亮的截图。曝光固定EV1.8546、ToneAdapt0；真实HDR使用一致尺度，不自动归一化。

## 排查顺序与交付

1. **先隔离后处理影响。** 同帧保留真实间接HDR、材质合成后scene HDR、TAA前后和ToneMapping最终图；先查实际pass是否启用与真实资源链。核颜色空间、白平衡/色调映射、HDR范围、TAA历史/运动向量/细节损失。若scene HDR已有异常继续GI；若进入最终显示才丢色/层次则修对应实现。Fusion材质合成与间接光求值不能统称后处理。不得用曝光、压暗或加饱和度掩盖输入错误。
2. **补离散五点捕获入口。** 现有 `TestVulkanSponzaRenderGraph.cpp` 支持 `SPONZA_CAPTURE_FRAME` 与 `SPONZA_CAPTURE_COUNT`（连续窗口最多240），尚未实现任意五离散点同run；`scripts/sponza_reference.py` 和GI/producer审计常为单frame。先读调度／退出／readback条件再窄改捕获控制；运行实际达到760并完成fence/readback，不能仅拉长timeout却在470关闭窗口。保持渲染ray与更新策略。只导出必要pass/ROI，不默认五次完整atlas/几何/逐帧大窗口。
3. **遮挡优先，沿实际消费者证明。** 固定左右暗廊、超近顶部、拱顶、近地，受光墙与中庭作对照。读取实际像素N、位置、material、DDGI/SPGI/regular/low权重，actual probe IDs/weights、最新writer epoch与radiance来源。分两类：实际ray首面/SDF-stop/SC/屏幕/天空/analytic-history路径资格；接收点→被消费probe与analytic邻probe的空间连接是否跨墙/顶板。先算实际贡献，几何连线被挡本身不证明漏光；材质alpha/transmission/薄面/双面与未知项不能按opaque处理。
4. **历史来源展开。** `native-history-ledger-cohort-match` 已证明旧703与新773同一数值cohort：882 writer、71915 analytic查询、39epoch、1447713 loaded SH RGB逐位相同。利用保留旧 `native-temporal-writer-ledger` 实际349writer账本与新773实际tap/weights/N，一层层展开analytic邻点历史贡献。先重放原self-temporal，再拆邻点来源；seed/external/roundoff/未记录writer明确UNKNOWN。旧703module/descriptor/CopyBuffer身份不能转移给新773。当前屏幕贡献必须使用新773实际Gather，不能借旧ROI值一致来冒充证书。有符号preclamp分解不是正物理能量，也不是删除history反事实。
5. **最小修复与同binary/SPIR-V开关对照。** 针对最早异常环节修改；完整五点图＋真实HDR＋ROI曲线解释颜色/材质/空间层次变化。暗部降亮不足以KEEP，中庭与受光墙要同时检查。收益不足或原门失败自行回退；保留后做适当回归。

## 已查遮挡证据与未解决项

- 37N/同批1159真实RO/RD GPU与float64首面20µm/64stack对照：1008有50m首面、151无；932 SDF早停、76晚停，首面带内0，中位SDF−first约−4.015m。是选定cohort，不是全场景错误率。
- 433 cross早停中429 actual最后sample仍在首面前，3432角点为0、零候选只来自mesh支持域外AABB下界；native scalar也为0。不是简单“BVH命中而SDF未命中”，而可能离面先停。CPU角点不是每fetch descriptor证书，暗部屏幕因果未闭合。
- 当前原生analytic N=−RD逐位一致，不证明它是实际表面法线。读取/公式数值正确不证明支持域、遮挡、物理opacity或完整历史来源正确。
- 上述可疑机制已有若干失败修复，不能再次等价重试：505 `ddgi-first-surface-cache`真实首面SC Lo补取；529 `ddgi-zero-cross-cache`全零首面SC Lo/t/N；545 `ddgi-zero-continue`额外march。均DISCARD，前两有明显亮区/墙退化，545无视觉收益且新点仍非真实面。406联合位置/rho/N、390、266/279、分层采样、sky与多跳候选的失败文字摘要保留。不能未经新证据放宽UNKNOWN/opaque/门禁或删history。

## 固定环境、工具与验证

先读AGENTS.md、Docs/ARCHIVE.md，再按需读旧主交接 `Docs/completed/2026-10-05-sc-bvh-lumen-optimization-handoff.md` 的成果/证据表/固定环境，以及per-mesh实现顶部、数据格式、RHI四端矩阵、barrier语义、UE pass分析。旧文档开头LIVE记录是历史快照，新交接和当前freeze优先。

精确运行环境参考 `native-primary-history-persistent-packet/run-restored.py` / `retry480.py`：DDGI_RAY_SAMPLE=0、PRIMAL_SPGI_ZERO_HIT_GEOMETRY=2、PRIMAL_TONE_EV=1.8546、PRIMAL_TONE_ADAPT=0，原SC开关/静态页/boundary/continuous_from120/full_res_gather0保持。不要把默认0与固定实验2混用。

只构建Darwin/DebugVulkan、serial -j1，与Debug共享libEngine.a，不交叉污染。保留全部dirty，改Graphics前读parity/barrier。原Unit7+5、V350/480、Metal≥65、SC/producer/Gather六ROI1%/Fusion两halfULP等适用原门保持；不需要因本次纯Docs清理重新构建引擎。

## 裁剪后的报告与数据可用性

按用户授权清理旧展示和大体积诊断缓存，**不再要求网页追加全部历史图**。`Docs/reports/sc-resolve-2026-10-03/index.html`为轻量近期入口；旧111段文本页面压缩保留在 `Docs/reports/sc-session-handoff-2026-10-07/index-before-cleanup.html.gz`，其旧图片引用可能失效，不是可完整重建的旧网页。

保留11个关键阶段完整数据：最新native-primary-history-persistent-packet、native-history-ledger-cohort-match、native-primary-component-companion、native-stable-residual-isolation；后续来源展开依赖native-temporal-writer-ledger与latest-analytic-history-bank；native-sample37；有效spgi-first-surface-owner；三个失败ddgi-first-surface-cache、ddgi-zero-cross-cache、ddgi-zero-continue。保留Lumen参考及少量关键根目录stage图。

其他旧阶段保留文字/日志/脚本/raw-manifest与失败结论，旧raw、图片与诊断二进制已移除，目录中 `CLEANUP-2026-10-07.md` 明确不可用范围。原raw-manifest仅证明历史身份，不能声称所有gzip还可恢复。需要旧数据时先查 `deletion-manifest.json` 和持久副本；没有则重新捕获，不把缺失文件或历史审计当新验证。

`Docs/reports/sc-handoff-2026-10-05` 持久基线与UE16.69GiB原始归档、生产资产/构建对象均不动。保留raw用 `scripts/restore_sc_stage_raw.py --stage <保留目录> --out <本轮新目录>` 逐SHA恢复。清理范围、删除文件SHA、保留raw核验、production265清理前后身份及空间实测在 `Docs/reports/sc-session-handoff-2026-10-07/`。后续每轮归档校验后及时清理自有临时副本，不删其他会话数据。

清理完成：44845个旧文件移除，保留7213 raw引用／2458唯一SHA全部解压校验通过；statvfs可用空间增加44.23 GiB（非独占归因）。新网页6图/11阶段，链接存在检查通过，无browser QA，无新Engine/GPU回归信用。请勿运行旧 `scripts/sc_resolve_report.py` 重建缺图的累计图库；使用 `Docs/reports/sc-session-handoff-2026-10-07/render_recent.py` 或单独小型阶段页面。
