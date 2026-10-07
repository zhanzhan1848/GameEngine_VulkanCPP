# 新会话提示词（复制下方正文）

/goal 在 `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP` 继续SC／两层BVH与Lumen间接光优化，直接排查、修改、验证，按证据保留／回退，不停留在方案讨论。

先读AGENTS.md、Docs/ARCHIVE.md，然后以 `Docs/completed/2026-10-07-sc-lumen-occlusion-postprocess-convergence-handoff.md` 为本次主交接。按该文档读取旧主交接、per-mesh实现顶部/格式、RHI矩阵/barrier及UE pass分析。当前265冻结和原231SPIR-V已恢复，无活跃GPU；不要按旧文档历史LIVE记录继续任务。

优先解释左右暗廊与机位超近顶部为何比Lumen高能量，而中庭正常。遮挡/SDF离面停步、跨墙probe插值、法线/材质响应、history来源和后处理均为待验证假设；不要凭局部被挡、读取数值一致或UE流程认定根因。

先补同帧“真实间接HDR→材质合成scene HDR→TAA前后→ToneMapping最终图”对照，隔离后处理影响。同时补同一次连续运行50/150/350/480/760离散捕获，原350/480门禁保持；记录实际时间与probe更新次数，标注SC持续辅助刷新从120开始。各帧固定EV1.8546/ToneAdapt0与HDR一致尺度，不自动拉亮；760仍变化再延长。现有入口只有单帧/连续窗口，五离散帧同run尚未实现，不把五次独立重启当时间序列。

沿固定暗廊/顶部像素的实际ray、probe/tap、法线、权重、writer epoch与SC/屏幕/天空/历史来源检查遮挡资格；中庭/受光墙作为对照。使用保留旧703账本与新773实际读取的同数值cohort展开一层analytic邻点历史，先重放原算子，未知祖先/descriptor/物理opacity明确UNKNOWN。不要重复505/529/545首面SC补取或继续march、406联合位置/rho/N等失败方案，无新增证据不等价重试。

保留8190实际主ray的3256正确SC与20旧地面修复，旧card/cacheT严格保持。固定实验环境明确 `PRIMAL_SPGI_ZERO_HIT_GEOMETRY=2`，不混默认0；精确开关参考保留run-restored.py。不得调曝光、灯光、额外压暗/饱和度掩盖GI问题，不减少ray、不跳帧复用旧radiance、不改门禁/放宽opaque或UNKNOWN，不重引入discard的4×4世界线段数组。

只构建Darwin/DebugVulkan -j1，避免共享libEngine.a污染；保留全部dirty。每阶段按原适用门回归、截图/HDR对Lumen解释颜色材质空间层次，亮度降低或命中/Debug性能不是视觉成功。SCstage PASS与P0缺原baseline NOT_RUN/null历史FAIL及旧display FAIL分列。GPU BOTTOM_OF_PIPE只是命令标记间隔。

旧网页和缓存已按授权裁剪，轻量入口仍为 `Docs/reports/sc-resolve-2026-10-03/index.html`。11关键阶段raw保留逐SHA核验，其余旧阶段多数只剩文字/日志/脚本/历史manifest，先查CLEANUP标记与 `Docs/reports/sc-session-handoff-2026-10-07/deletion-manifest.json`，不存在的旧raw要重捕获，不能当新验证。持久基线sc-handoff-2026-10-05与UE原始归档保持。后续网页只展示近期有价值结果，历史失败摘要保留；每轮归档核SHA后及时清理自有重复raw和PNG。
