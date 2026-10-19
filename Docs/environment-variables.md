# 环境变量全量清单（T1.8 / E4）

**Date**: 2026-09-08
**Scope**: Engine / EngineTest / Tools 内全部 `std::getenv` 旋钮（自动盘点 74 个，shader 侧无）。
**约定**: 【一次性】= 进程启动读取一次（static 初始化）；【每帧】= 帧循环内读取；
**优先级**: 多个机制控制同一行为时的胜负关系。
**维护纪律**: 新增 env 必须登记本表（名称 / 域 / lifetime / 语义 / 优先级）；
删除实现时同步删行。审计工具：`grep -rhoE 'std::getenv\("([A-Z0-9_]+)"\)' Engine EngineTest Tools | sort -u`。

## 1. GI / DDGI / SPGI

| 变量 | Lifetime | 语义 | 优先级/备注 |
|---|---|---|---|
| `DDGI_DUMP` | 一次性（测试） | 默认第 350 帧读回；`1`=SH 摘要，`2`=逐路径辐射 CSV（需 `DDGI_PATH_DEBUG`；仅含该帧更新且位于 SDF 内的探针），`3`=GIGather 半分辨率 PPM，`6`=深度矩 CSV，`7`=三个 ROI 的位置/法线 CSV、探针上次更新帧及全量近场深度矩/重定位原点（`/tmp/ddgi_roi_*`）；`8`=409/410/507 中该帧实际更新的探针，GPU 起点/命中距离与最近双面三角形交点、命中点到最近表面距离对照（`/tmp/ddgi_depth_triangles.csv`），模式0审计默认辐射射线、模式5审计额外深度射线 | TestVulkanSponza 只读诊断，读回按 DDGI 绝对帧号选槽；8 使用原网格几何，包含布料但不剔除 alpha-test 孔洞，CPU 审计较慢，不用于性能测量 |
| `DDGI_DUMP_FRAME` / `DDGI_DUMP_TARGET_PROBE` | 一次性（测试） | 前者覆盖默认读回帧；后者是 flat probe 索引，令诊断从指定帧起等待该 probe 实际被调度（最迟第 350 帧），用于逐射线路径归因 | 配合 `DDGI_DUMP=2/8` |
| `DDGI_DEPTH_TRI_SOURCE_PREFIX` | 一次性（Vulkan测试） | 配合`DDGI_DUMP=8`，导出同帧CPU scene48B三角形、material48B POD、mesh→material ID与保守栅格资格；恒等变换不成立时complete=false。`-meta.json`表示来源快照，`-trace-meta.json`须CSV关闭后才complete，可配合readback-json等待 | 只读诊断；不启用FIRST_SURFACE、不改primary／GPU资源／shader。flags0仅保守资格，非GPU材质驻留／alpha纹理／物理opacity认证；未指定不增加工作 |
| `SDF_POINT_PROVENANCE` | 一次性（测试） | 输入 `DDGI_DUMP=8` CSV 路径，筛选距真实表面超过 0.75m 的 SDF 命中点，CPU 重放其 8 个缓存体素的逐网格最小值；输出 `/tmp/sdf_point_provenance.csv`，区分网格采样与越界 AABB 下界并核对缓存值 | 只读诊断，不重建/替换/上传缓存；沿当前传播及合并规则重放，只有 `match=1` 才支持缓存来源归因；需当前模型与缓存 bounds/编码一致 |
| `SDF_TRACE_REPLAY` | 一次性（测试） | 配合 `SDF_POINT_PROVENANCE`，CPU 重放全部输入射线，输出 `/tmp/sdf_trace_evidence.csv`：旧 scalar 规则、独立网格支持证据门控、拒绝带两体素步长及 256 步预算四组；配合 `DDGI_DUMP=8` 导出实际加载几何 `/tmp/ddgi_oracle_triangles.bin`（每三角形 9 个 float32：a/e1/e2）供点到表面验证 | 只读固定起点实验；仅适用近场单 cascade、signed、50m、无 detail refinement 的固定模式0/5输入，必须先核对 policy0 与 GPU 距离；不改 GPU trace、缓存或默认步长。网格支持证据不等于几何正确性认证 |
| `SDF_GPU_EVIDENCE` | 一次性（Vulkan Sponza 测试） | 独立实验缓存路径；加载/生成 `PGSDFE01` RG16F 场：R 保留原 scalar 下界，G 为网格支持证据，65504 是未知；默认不启用。缓存算法版本1，需相同 pipeline model；模型变化须换缓存路径 | 不允许普通缓存路径或 SDF_NO_PROPAGATE/OFFLINE_SDF_SEAL/PRIMAL_OFFLINE_SDF_ZERO 混用。只改变 DDGI split 命中规则；SPGI/WRC/重定位继续消费 R，未完整迁移。不得替换生产缓存 |
| `DDGI_SDF_EVIDENCE_MODE` | 一次性（Vulkan Sponza 测试） | 仅 `SDF_GPU_EVIDENCE` 成功后传入 DDGI：0=RG 场但旧判定控制，1=独立证据门控＋64步保守爬行（默认），2=拒绝带两体素步进＋64步，3=同2但256步。未知插值角点不认证命中 | 2/3为步进实验，薄墙安全和 GPU 成本未经全场验证；默认渲染仍无门控。其他后端不设置此旗标 |
| `PRIMAL_SC_RADIOSITY_W` | 一次性 | Surface Cache tile radiosity 消费权重；Vulkan 默认 0.5（Sponza 参考机位 A/B），Metal/Dawn 默认 1.0；显式设 `1.0` 可回滚 Vulkan 观感，合法范围 `(0,4]` | 跨后端默认值暂时分叉，见 RHI parity matrix |
| `DDGI_PATH_DEBUG` | 一次性 | finalize kernel 换 `*_pathdbg` 变体；路径号占命中距离浮点尾数低 2 位，深度矩仍可用 | 配合 `DDGI_DUMP=2` |
| `DDGI_DEFAULT_FIRST_ORACLE_PREFIX` | 诊断触发时（Vulkan Sponza 测试） | 非空路径前缀启用独立首交几何 oracle；要求 DUMP=2、ray-source audit、scalar mode0 且 FIRST_SURFACE/FIRST_NATIVE_NORMAL 关闭。实际 GPU 源尾部与更新列表输入，最多32768条新鲜射线、12vec4/条；独立输出、BVH与生产 buffer 前后副本保存，检查提交完成后读回 | 默认关闭；不写生产数据。status5 的旧标签 TRANSMIT 仅表示非零材质 flags guard，不认证物理透射；CPU CB镜像不认证历史生产CB。源/拓扑/最近几何验证见 `autoresearch/classic-260929-0916` |
| `DDGI_ANALYTIC_NO_HISTORY` | 每帧（Vulkan UBO） | 设为任意值仅让 split-finalize 的解析 HIT 兜底不叠历史探针反弹，保留环境地板；SC、屏幕、escape/exhaust 路径不变。通过 SkyColor.w 传递 | 诊断消融，默认关闭，不是完整物理修复；Metal/Dawn 不设置旗标 |
| `DDGI_ANALYTIC_IRRADIANCE` | 每帧（Vulkan UBO） | 设为任意值将 split-finalize 解析 HIT 历史回退改为 EvalLib 余弦卷积 E/π，空间插值后非负约束，保留 ρ·E/π 反弹和环境地板；SC、屏幕、escape/exhaust 规则不变 | opt-in 候选，默认关闭；NO_HISTORY 优先；SkyColor.w=2，仅 Vulkan。法线／回退反照率未随此开关修正 |
| `DDGI_EVIDENCE_NORMAL` | 每帧（Vulkan UBO） | 非零证据模式下启用 G 通道中心差分法线，按各轴世界空间体素间距缩放，朝向入射射线；无效／未知／平坦模板保留旧法线回退，detail SDF 优先 | opt-in 实验，默认关闭；只在证据 RG 场启用，不改变命中距离或 SC 匹配规则。SdfVoxelSizes[0].z=1；Metal/Dawn 不启用 |
| `DDGI_ANALYTIC_AUDIT` | 初始化（Vulkan，分配时锁存） | 扩大 hit-normal SSBO 为五倍，保留原法线区域，每射线尾部四条 vec4 记录解析 HIT 的反照率／来源、天空 Lo、历史反弹 Lo、可选材质投影点；`DDGI_DUMP=2` 导出 `/tmp/ddgi_analytic_components.csv`。来源1=屏幕不可用，3=尺寸不符，4=已验证GBuffer，10=相机背后，11=离开视锥，12=天空/无效深度，13=重建失败，14=表面距离不符 | opt-in 只读诊断，不改变辐射；SdfVoxelSizes[0].w=1；仅 split-finalize 有记录，未启用 PATH_DEBUG 时 path 列不可解码；其他后端不启用 |
| `DDGI_MATERIAL_PROJECT` | 初始化（Vulkan，分配时锁存） | 自动启用审计尾部；仅证据 RG 模式下，沿未翻转 G 梯度做一步表面投影，要求有限非负带内距离及下降残差。投影点只用于反照率屏幕查询；失败回到原采样点 | opt-in 实验，默认关闭；SdfExtents[0].w=1。保留 .15m 深度验证、原命中距离／深度矩、SC 匹配和历史／屏幕辐射采样位置；Metal/Dawn 不启用 |
| `DDGI_RAY_SAMPLE` | 一次性 | 探针射线采样模式（0=UNIFORM，默认；1=JITTER；2=COSINE；3=固定纬度分层 A/B，128 射线时覆盖 62/64 深度方向格而非默认 22/64；4=原射线不变，仅 Vulkan 深度图未测格用最近实测格补齐；5=仅 Vulkan split SDF 补追 128 条深度射线，每 oct8x8 格两条，只更新原射线无命中的深度格，不进入辐射/SH/openSky） | 3/4/5 为诊断候选，未默认启用；5 使用同一次重定位起点及原 SDF/detail 步进。-1 耗尽保持未知历史，-2 真逃逸才计最大距离；Metal/Dawn 的 5 会回退 0 |
| `DDGI_DBG_PATH` | 一次性 | DDGI 路径调试输出（TestModularPipeline） | |
| `DDGI_RECT` | 一次性 | `DDGI_DUMP=3` 的任意统计窗口 `name:x0,y0,x1,y1;...` | |
| `DDGI_SILENCE` | 一次性 | 静默 DDGI 日志 | |
| `PRIMAL_DDGI_PARITY` | 每次编译 shader | Metal DDGI 优先加载 `*Parity.metal`（Vulkan 数学对齐版） | per-CompileShader |
| `PRIMAL_PROBE_HYSTERESIS` | 一次性（测试） | 探针滞回系数覆盖（默认 0.2） | |
| `PRIMAL_SPGI` | 一次性（测试） | Metal 上启用 SPGI 链（SetQualityOverride） | |
| `PRIMAL_SPGI_SKIP` | 一次性 | 强制走 monolith trace（A/B split 路径） | |
| `PRIMAL_SPGI_PARITY` | 一次性 | **no-op 死旋钮**（F4 在案；接线或删除待 M5） | |
| `SPGI_OFF` / `SPGI_SILENCE` | 一次性 | SPGI 关闭 / 静默日志 | SPGI_OFF 胜过 PRIMAL_SPGI |
| `SPGI_CAM` / `SPGI_CAM_LOCK` | 一次性（测试） | 相机机位覆盖（x,y,z,pitch,yaw）/锁定 | |
| `SPGI_NO_OFFLINE_SDF` | 一次性（测试） | 禁用离线 SDF（运行时体素化路径验证） | A1 验收用 |
| `SPGI_SDF_STEP_SCALE` | 每帧 | Vulkan SPGI 粗 SDF 射线远场步长倍率；默认 `1.0` 保守命中薄墙，合法范围 `[0.75,1.2]`；`1.2` 回滚旧过冲路径 | 仅 SPGI，DDGI/WRC 不变 |
| `SPGI_DDGI_VISIBILITY` | 每帧 | Vulkan split-finalize DDGI 回退可见性 A/B：`0` 默认不变；`1` 用同 SH 槽深度矩及重定位起点重选邻居；`2` 另保留可见性衰减量。遮挡软下限 0.2，借格半权；无有效邻居返回黑而非天空 | 诊断候选，尚未默认启用；Metal/Dawn 不变。配合 `SPGI_TRACE_DIAG=6 SPGI_RIGHT_RAY_SAMPLES=<frame>` 输出 `/tmp/spgi_ddgi_neighbours.csv`，包括真实消费邻居的起点/状态/距离/均值/方差/权重/辐射 |
| `SPGI_RAY_SAMPLES_FLOOR` | 每帧 | 设为任意值将 Vulkan mode-6 代表射线从右廊 3×3 格切到近景地面 `(110/160/210,160/168/176)`；诊断采样点不改变射线或光照 | 原 `spgi_right_*` CSV 文件名保留；地面模式不运行固定右廊三角审计；搭配上行可见性模式 1/2 |
| `SPGI_PROBE_SPHERES` / `SPGI_PROBE_VIS` / `SPGI_SDFVIZ` | 一次性（测试） | 探针球可视化 / 探针访存可视化 / SDF 体可视化 | |
| `SCREEN_PROBE_RAYS` | 一次性 | 每探针射线数覆盖 | |
| `GI_ALL_OFF` | 一次性（测试） | 全 GI 链关闭（消融基线） | |
| `GI_DEBUG_STRIP` | 一次性（测试） | fusion 输出按竖条切分各 GI 输入 | |
| `GI_ISOLATE` | 一次性 | FusionIndirect 消融变体选择（noSsgi/noDdgi/noSpgi） | 编译期变体 + env 选择 |
| `GI_VERIFY_FRAME` | 一次性（测试） | GIFusion 数值门禁触发帧（默认 90） | |
| `SSGI_FORCE` | 每帧 | SPGI 激活时仍保留 SSGI（A/B） | 胜过 SPGI 抑制策略 |
| `SSGI_OFF` / `SSGI_SILENCE` | 一次性 | SSGI 关闭 / 静默 | |
| `SSAO_DIRS` / `SSAO_SAMPLES` / `SSAO_SILENCE` | 一次性 | SSAO 质量 / 关闭 | |
| `PRIMAL_SPGI_SC_NOPENALTY` | 一次性 | [F2 2026-09-20] `=1` 回滚 SC 命中惩罚（[E0-B 2026-09-21] 起惩罚=firefly 软限幅，`=1` 仍可全关） | 能量不对称修复 |
| `PRIMAL_SPGI_PENALTY_KNEE` | 一次性 | [E0-B 2026-09-21] HZB/SC 亮度惩罚 firefly 膝点（默认 8.0，lum>8 才起效；`1.0`=旧语义）。膝点以下合法反弹全额通过 | probe-repr 计划 E0-B |
| `PRIMAL_SPGI_SC_NODIRECT` | 一次性 | SPGI 的 SC 二次命中默认读完整 lighting atlas（保留受直射彩色表面的合法下一跳 `Lo`）；仅显式 `=1` 读 `result−direct·ρ/π` 的 nodirect atlas，供旧行为 A/B | 彩色反弹所有权修复 |
| `PRIMAL_SC_NODIRECT` | 一次性 | [E0-B 2026-09-21] SC 场 nodirect 副本生命周期（默认 1=LightEval 写副本；`0` 不写）。**注意**：prev_light/tile 查询源必须保持混合 atlas（780 帧回路稳定性实测，见完工文档） | probe-repr 计划 E0-B |
| `SPGI_TRACE_DIAG=4/5` | 一次性 | [E0-I 2026-09-21] Debug A/B 分通路隔离：`4`=fill-only（HZB/SC 置零）、`5`=transport-only（far-cache/miss 置零）；归因统计照记 | 能量-排查 §5.5 |
| `SPGI_TRACE_DIAG_GI_FRAME` | 一次性（测试） | SPGI 分支统计/查询漏斗的读回时刻，以 GI 内容就绪后的逻辑帧计数（默认 180）；避免绝对渲染帧落在 SC 注册前而产生假结论 | 能量-排查 |
| `SPGI_TRACE_DIAG=1` | 一次性（测试） | 额外按屏幕探针真实位置投影统计六区的近原点拒绝/远拒绝/有效命中辐射，导出 `/tmp/spgi_roi_trace.csv`；是探针侧归因而非最终像素贡献 | 不改变默认着色 |
| `PRIMAL_SPGI_SC_FUNNEL` | 一次性（测试） | [E0-I 2026-09-21] 在 `SPGI_TRACE_DIAG_GI_FRAME` 打印 SC 产出率漏斗（candidates/boundsFail/orientFail/depthFail/found，从内容就绪累加）+ 下行失败计数（K-3） | 能量-排查 §6 I-2 |
| `PRIMAL_SPGI_SH2_CHECK` | 一次性（测试） | [P4 2026-09-21] frame 350 打印 SPGI SH2 重建误差 P50/P95/max（重代入 |Δ|lum 口径） | probe-repr 计划 P4 |
| `PRIMAL_PROBE_SPHERE_MODE` | 一次性 | [P0 2026-09-21] 探针球着色模式 `state`(默认)/`radiance`/`confidence`/`variance`（或 0-3）。state=生命周期诊断、radiance=能量诊断；confidence/variance 当前 WRC 有效（WRC=消费计数/64 方向 var-mean），DDGI moments 未接线回退 radiance/记录一次 | probe-repr 计划 P0 |
| `PRIMAL_WRC_PROBE_SPHERES` | 一次性 | [P0 2026-09-21] `=1` 启用 WRC 探针球 overlay（此前仅编辑器 C ABI 可达） | probe-repr 计划 P0 |
| `PRIMAL_WRC_DUMP` | 一次性 | [P4 2026-09-21] 扩展输出：`[WRC_ANGVAR]` 64 方向 var/mean 分布（各向异性健康度）+ `[WRC_PF_CHECK]` 标准化（worstProbe 坐标 + P95>15% 告警门，不再依赖 PREFILTER 开关） | probe-repr 计划 P4 |
| `PRIMAL_SPGI_HINT_SOFT` | 一次性 | [①治理 2026-09-21] `=1` 启用 G2 hint 错位**软键惩罚**（key 加罚不拒绝；found 1.87%→5.70% 三倍）。**默认关**（旧硬语义）——软罚能量流与 tile 回路双稳态相互作用，重开验证流程见 round2 完工文档 §5.3 | round2 ① |
| `PRIMAL_GI_SKY_CONF_FLOOR` | 一次性 | [②K-4 2026-09-21] 纯天空探针 confidence 地板（默认 **0.35=旧值 no-op**；目标 0.18 待重开验证）。Gather 以 sh0w 为权重——降档即天空 fill 空间分类加权 | round2 ② |
| `PRIMAL_SC_TILE_GROWTH_CAP` | 一次性 | [稳定性 2026-09-21] tile 每帧增长幅度上限 cap×prev+ε（默认 0.5；0=关）。限速率不保证增益<1——间歇 780 帧坍黑（~15-20%）的**已验证稳定规避 = `SC_INDIRECT=0`**；根治=回路增益分析（round2 完工文档 §5） | round2 ③/§5 |
| `PRIMAL_SPGI_PLACE_REPR` | 一次性 | [P0 2026-09-21] 代表像素布点（默认 1）；`0` 回退 tile center | 分布优化计划 §8 |
| `PRIMAL_SPGI_ADAPTIVE_NORMAL` | 一次性 | [P1 2026-09-21] F4 判据 normal 项（默认 1，T_n=0.85）；`0` = 仅 position | 分布优化计划 §8 |
| `PRIMAL_SPGI_TIER3` | 一次性 | [P2 2026-09-21] 1/2/4 三档 4 槽位 + 活跃槽 compaction（默认 0 = 棋盘两档；仅 Vulkan） | 分布优化计划 §8 |
| `PRIMAL_SPGI_TIER3_CAP` | 一次性 | [P2 2026-09-21] compacted radiance 容量（探针数，默认 tiles×2） | 分布优化计划 §8 |
| `PRIMAL_SPGI_LIGHTVAR_TOPK` | 一次性 | [P3 2026-09-21] 辐亮度判据总活跃 tile 上限（默认 512；`0` = 关闭辐亮度判据；需 TIER3=1） | 分布优化计划 §8 |
| `PRIMAL_SPGI_TIER_RAYS` | 一次性 | [P4 2026-09-21] 分档 ray 16/32/64（默认 0 = 全档 64；需 TIER3=1 且 rays=64） | 分布优化计划 §8 |
| `SPONZA_SPGI_TIER_STATS` | 一次性（测试） | [P2 2026-09-21] frame 350 读回 compact 统计（活跃槽 + 16/32/64 档分布 + lightVar 活跃 tile） | |
| `PRIMAL_GI_FALLBACK_CLAMP` | 一次性 | [F1 2026-09-20] SPGI 失败射线低频回退限幅 k（默认 0.3；`0` 回黑语义） | |
| `PRIMAL_DDGI_LOW_FREQ_BLEND` | 一次性 | DDGI gather 中无可见性三线性低频旁路的混合比 `[0,1]`；未设置保持后端现有默认（Vulkan 0.30 / Metal 0.55），`0` 用于隔离跨墙/跨楼层漏光 | GI 梯度 A/B |
| `PRIMAL_WRC_FALLBACK_CLAMP` | 一次性 | [F1 2026-09-20] WRC 查询失败回退限幅 k（默认 0.3；`0` 回黑语义） | |
| `PRIMAL_FUSION_PERPIXEL` | 一次性 | `=1` 使用 legacy per-pixel 融合；默认 High/Ultra 同样按 SPGI alpha 在 DDGI→SPGI 间连续替换，低置信区域不再丢失 DDGI，Medium 仍 DDGI-only | confidence fallback |
| `SPONZA_SPGI_RAY_DUMP` | 一次性（测试） | [F3-V3 2026-09-20] 指定帧逐射线 radiance readback（直方图 + /tmp/spgi_rays.bin） | |
| `SPGI_RIGHT_RAY_SAMPLES` / `SPGI_RAY_TRI_AUDIT` | 一次性（测试） | `SPGI_TRACE_DIAG=6` 时指定帧导出右廊 36 条射线与逐卡查询；后者额外对两条代表射线做原始三角形求交并写 `/tmp/spgi_right_ray_triangles.csv` | 仅 TestVulkanSponzaRenderGraph |
| `PRIMAL_PASS_DEBUG_GI_SCALE` | 一次性 | [F3-V2 2026-09-20] PASS=spgi/spgi_raw 视图显示曝光（默认 0.05；gather 输出为 HDR irradiance，ACES 直读全白） | 仅影响调试显示 |

## 2. Surface Cache / 离线 SDF

| 变量 | Lifetime | 语义 | 优先级/备注 |
|---|---|---|---|
| `PRIMAL_SC_BUDGET` | 一次性 | SC 页预算**上限**（0=全量）；与 governor 取 min（A6 仲裁） | governor 缩放值低于它时生效 |
| `SC_CARDVIEW` / `SC_CARDVIEW_BATCH` | 一次性 | 卡片视捕获启用 / 每批面数 | |
| `SC_DUMP_CARDS` / `SC_DUMP_INTERVAL` | 一次性 | 卡片转储 / 间隔 | |
| `SC_INJECT_OFF` / `SC_MIRROR_OFF` / `SC_MIN_CARD_RES` | 一次性 | 卡片注入/镜像诊断 | |
| `OFFLINE_SDF_SEAL` / `OFFLINE_SDF_VERIFY` / `PRIMAL_OFFLINE_SDF_ZERO` | 一次性 | 离线 SDF 缓存封存 / 校验 / 清零诊断 | |
| `PRIMAL_SC_TILE_SKY_OVERRIDE` | 一次性 | [F0 2026-09-20] SC tile 天空反弹色强制 `"r,g,b"`（默认= `GIDebug().sky_color_intensity×(2.75,2.88,3.0)`；旧行为传 `"1.1,1.152,1.2"`） | 能量不对称修复 |
| `PRIMAL_SC_CARD_OVERRIDE` | 一次性 | [F5 2026-09-20] 手工卡片 override `"mesh_id:x0,y0,z0,x1,y1,z1[;...]"`（世界系 AABB，6 向发卡；表空=原行为） | 能量不对称修复 |

## 3. 阴影 / 后处理

| 变量 | Lifetime | 语义 |
|---|---|---|
| `PRIMAL_SHADOW_PCSS` | 每帧 | PCSS 软阴影路径 |
| `PRIMAL_SHADOW_WHITE` | 一次性 | 阴影全 1（消融） |
| `PRIMAL_VSM_SHADOWS` | 一次性 | VSM moments 链 |
| `SHADOW_BLUR` / `SHADOW_QUALITY` | 一次性 | 模糊/质量 |
| `PRIMAL_BLOOM` / `TONE_OFF` / `PRIMAL_TONE_ACES` / `BLUR_OFF` / `TAA_OFF` | 一次性 | 各后处理开关/消融 |
| `PRIMAL_LEGACY_POST` | 一次性 | 旧后处理路径 |
| `PRIMAL_PASS_DEBUG` | 一次性 | 视口 pass 可视化启动视图（off/albedo/normal/orm/depth/velocity/visibility/meshlet/cluster/mesh/ssao/deferred/gi_indirect/spgi/sc_atlas）。强制完整渲染图路径；运行时切换用 `PipelineSetPassDebugView` / `editor.pass-view`（Docs/2026-09-20-pass-debug-view.md） |

## 4. RenderGraph / RHI（M1 新增）

| 变量 | Lifetime | 语义 | 备注 |
|---|---|---|---|
| `PRIMAL_RG_STRICT_IMPORT` | 一次性 | RG 单一物理 import 身份违例时断言（T1.1/C2） | 默认警告 |
| `PRIMAL_DEFERRED_FREE` | 一次性 | RHI 分配器 3 帧延迟槽位释放（T0.13） | **默认关**——与 MetalCommandBuffer 生命周期假设冲突（frame-1 vtable 清零 SEGV，待 M1 generation 方案完解） |

## 5. 诊断捕获 / 平台

| 变量 | Lifetime | 语义 |
|---|---|---|
| `PRIMAL_METAL_VALIDATE` | 一次性 | `0` 关闭 Metal API validation（默认开） |
| `PRIMAL_DUMP_MATRIX` | 一次性 | 矩阵真值打印（对账工具） |
| `PRIMAL_DUMP_GRAPH_MSL` | 一次性 | 转储 RG 生成的 MSL |
| `PRIMAL_GBUFFER_DEBUG` / `PRIMAL_DISABLE_GRAPH_DRAW` | 一次性 | GBuffer 调试 / 关闭图绘制 |
| `RG_PROF` | 一次性 | RG pass 计时 Top-N |
| `SPONZA_CAPTURE` / `SPONZA_CAPTURE_DIR` / `SPONZA_CAPTURE_FRAME` / `SPONZA_CAPTURE_COUNT` / `SPONZA_DIAGNOSTIC_CAPTURE` / `SPONZA_REFERENCE` | 一次性（测试） | Sponza 单帧输出路径 / 连续帧输出目录 / 起始目标帧 / 连续帧数（默认 1，1..240；`COUNT>1` 时仅设置目录即可启用并写入 `reference_<frame>.ppm`）/ 诊断捕获 / 参考模式 |
| `SPONZA_FLOOR_ALBEDO_SCALE` | 一次性（Vulkan Sponza 测试） | 地面 diffuse 在线性色彩空间的 RGB 缩放（0.1..1.5）；参考模式默认 0.5，普通运行默认 1.0 |
| `SPONZA_CAPTURE_SEQUENCE_STATS` / `SPONZA_CAPTURE_NO_PERIOD2` | 一次性（测试） | 连续参考帧抓取时输出前三帧 MAE、N↔N+2 MAE 与相邻差分相关性；后者在检测到确定性二周期（相关性 < -0.98 且 N≈N+2）时以退出码 6 失败 |
| `ENGINE_SHADER_ROOT` / `PRIMAL_SHADER_ROOT` / `PRIMAL_BUILD_DIR` / `PRIMAL_VTBAKE_BIN` | 一次性 | 路径覆盖（shader 根 / 构建目录 / VT 烘焙器） |
| `PRIMAL_NO_HEAP_POOL` | 一次性 | Metal 内存池关闭（诊断） |
| `PRIMAL_IND_ASYNC` | 一次性 | Fusion indirect 异步路径 |
| `PRIMAL_LIGHT_DIR` / `PRIMAL_DIRECT_LIGHT` | 一次性（测试） | 光方向/颜色覆盖 |
| `SPONZA_*` 见上；`PRIMAL_GATHER` | 一次性（测试） | GI gather 从指定文件编译（A/B） |

## 6. 死旋钮在案（F4，待 M5 处置）

- `PRIMAL_SPGI_PARITY`（no-op）
- `max_gi_time_ms`、`ssdo_*`（5 字段）、`shadow_cascade_split`、`axes.directional/dfg_path`、
  `fusion_glossy_strength`、`ssgi_ray_count`、
  `surface_cache_update_distance/importance_weight` —— 字段存在但无消费者（`LumenTypes.h`/`GIDebugParams.h`）
- ~~`sc_capture/sc_lighting/sc_shadow`~~ —— **[F3-V4 2026-09-20 已接线]**：
  `SurfaceCachePass` capture/CardFill/LightEval/shadow 各 dispatch 前按
  `GIDebug()` 门控（false = 跳过 dispatch，atlas 保持上帧内容）；
  `GI_ALL_OFF` 的 direct-only A/B 语义自此对 SC 生效

## 7. 诊断变体机制（统一约定）

1. **编译期变体**（同一源 + `-D` 宏 → 独立 `.spv`/独立文件名）：`GI_ISOLATE` 的
   FusionIndirect 三变体、`DDGI_PATH_DEBUG` 的 `DDGITraceFinalize_pathdbg.comp.spv`。
   Vulkan 由 `build_spv.sh` 产出；Metal 由 env 在 `LoadShaderBytecode` 处选择文件。
   新变体必须走此模式（单一源 + 变体产出），禁止手拷贝源文件。
2. **运行时分支**（shader 内 uniform 分支）：仅限单次 dispatch 级别的低成本开关
   （如 `RaySampleMode`）；高频内层循环禁止。
3. **文件替换**（`*Parity.metal` 模式）：仅用于对齐迁移期，迁移完成后删除。

| `PRIMAL_LIGHTMAP` | 每帧 | `=0` 整体关闭 lightmap 旁路（零影响 A/B 总开关；默认开但需场景烘焙页激活） | lightmap 消费链 |
| `PRIMAL_LIGHTMAP_DEBUG` | 每帧 | `=1` composite 全屏采样测试页；`=2` 纯洋红诊断档 | lightmap 消费链 |
| `PRIMAL_LIGHTMAP_BAKE` | 一次性 | `=1` Sponza 测试内第 20 帧烘焙当前场景（密度自适应） | TestVulkanSponzaRenderGraph |
