# Lightmap 烘焙与运行时渲染——实现完工记录

- **日期**：2026-10-19
- **分支**：`feat/lightmap`（worktree `.worktrees/lightmap`，基线 7755b13e）
- **状态**：M0-M6 全部落地；视觉终验收的最后一步（UVLM 项目资产出图）遗留为手册步骤，见 §6
- **规划文档**：`Docs/2026-10-19-lightmap-baking-and-rendering-architecture-plan.md`（原始方案）

## 1. 交付总览（提交链）

| 提交 | 里程碑 | 内容 |
| :--- | :--- | :--- |
| cb3524f5 | M0 | 旁路骨架：LightmapModule + fusion 插针 + 渐变 debug 页（默认零影响） |
| d3359a26 | M1 | 契约冻结：LMAP 容器（104B 条目）+ MaxRects + ABI 结构（280B static_assert）+ 单测 |
| d6215e64/efee6f6a | M2 | 烘焙核心：CPU BVH radiosity 积分器（解析解单测）+ 场景收集器 |
| 6fb53442 | M3 | Vulkan 运行时消费链：UV pre-pass + LMAP 加载 + 实体自动匹配 + composite v2 + 零影响证明 |
| 87252b40/11634ef9/6f668657/9cbc9f60 | M4 | EngineDLL LightmapAPI + `scene.bake_lightmaps` CLI + 场景级持久化 + Bake 菜单 |
| 2f89048a | M5 | Metal 移植：MSL 双 shader + 模块 Metal 分支（编辑器视口验证） |
| afa26443 | M6a | `primal-engine bake-lightmaps` 离线 verb |

## 2. 架构落地形态（与方案的偏离记录）

1. **消费端 = 纯旁路 pass，Lumen shader 零改动**（方案 §4.5 原计划改 FusionIndirect 分支；
   实际为独立 `LightmapComposite` pass 插在 FusionIndirect→FusionComposite 之间，FusionComposite
   仅切换绑定源 ~22 行）。Vulkan 端 Lumen/DDGI/SPGI/SC/DeferredLighting shader 文件全部未动。
2. **参数走 push 常量而非 UBO**（8B gain+debugMode）：规避描述符池/在飞重写整类时序问题。
3. **逐像素 UV 经 UV pre-pass RT 携带**（RGBA16F：xy=atlas UV 已含实体 scale/bias，z=有效位），
   composite 全屏 pass 零逐实体数据。
4. **场景级持久化（`Scene.LightmapPage`）替代逐实体 LightmapBaked 组件**：引擎按收集器命名
   约定 `mesh_<contentId>_e<entityId>` 自动匹配 LMAP 清单，逐实体数据冗余。
5. **烘培器 CPU BVH**（utl::BVH + JobSystem ParallelFor，StaticProbeBaker 同模式）；GPU 化留作
   后续优化，不碰任何契约。
6. **Dawn 不实现**（Parity Matrix 登记降级边界，与 Lumen fusion 链未移植 Dawn 现状一致）。
7. **产物 = LMAP 自有容器**（RGBA16F 直传显存；BC6H 无编码器可用，format 枚举已预留）。

## 3. 验证证据

- **单测**（`TestLightmapBaking`，Vulkan/Metal 两树均绿）：
  - 装箱：无重叠/界内/确定性/顺序无关/利用率一致性；
  - LMAP：往返逐字段一致/坏 magic/截断/尺寸不符拒绝；
  - 积分器解析解：4×4 发光天花板@h=1 点源视因子 F=0.83112 → 地板中心 0.41558±0.02
    （1024spp；视因子闭式 + 径向积分下限 2.513 交叉验证）；
  - 确定性：同场景双烘焙逐位一致；dilation 扩张环；trace-only 遮挡（无 UV 网格只进 BVH）。
- **运行时端到端**（TestVulkanSponzaRenderGraph，规范 Sponza 门禁资产）：
  - 渐变 debug 档全屏替换可见（61% 像素变化，`/tmp/lm_m3/off↔on.ppm` 族）；
  - 真烘焙链（PRIMAL_LIGHTMAP_BAKE=1，UVLM 资产）：收集→烘焙→LMAP 落盘→加载→实体重建
    （`entities rebuilt: 1`）全链日志。
- **零影响证明**（PRIMAL_LIGHTMAP 默认开、场景未烘焙）：与 M0 期基线捕获对比
  **mean=0.761 / p99=6 / 0% 像素>8**（SPGI 运行间方差协议阈值 ~15；`/tmp/lm_m3/equiv.ppm`）。
- **Metal**：模块在编辑器视口初始化成功（`[Lightmap] module initialized (Metal, 812x390)`）；
  TestModularPipeline 25s 无 fault；TestLightmapBaking 1/1。
- **CLI/工具**：`primal-engine bake-lightmaps` 实跑（SponzaUVLM 1024²/32spp/2bounce，全核
  6min → `{ok:true, entries:1, bakeable:1, trace_only:5}`）；`scene.bake_lightmaps` 编辑器内
  提交/进度/终态闭环（task.status）；schema 快照门禁 64/64。
- **引擎回归**：ctest Unit 14/14（Metal 树）。

## 4. 关键坑位记录（跨会话复用）

- **编辑器原生库链**：`Darwin/DebugEditor/libEngineDLL.dylib`（→ 部署目录 libPrimalEngine.dylib）
  是编辑器实际加载的权威副本——改 Engine/EngineDLL 后必须刷新，否则编辑器跑旧代码
  （"过期 dylib 误诊"前科重现）。
- **RHI 内存**：`GPUMemoryUsage::Static` 不可 Map（MoltenVK 拒绝 PRIVATE 映射）——CPU 填充
  缓冲一律 `Dynamic`（DebugProbeSphere 同注）。
- **描述符**：StorageBuffer 写必须走 bufferInfo（range=0 = WHOLE_SIZE）；单份描述符集每帧
  重写触发 in-flight 失效——三槽轮转（帧号%3，与 fusion cbIdx 恒等）。
- **顶点拉取顶点阶段 SSBO** 合法（Particle.vert 先例）；索引缓冲需 `BufferUsageFlags::Index`。
- **渲染图深度只读**：`builder.Read(rg, ResourceState::DepthStencilReadOnly)` + 附件
  `depthStencil.loadOp=Load/storeOp=DontCare`（ProbeSphere 同款）。
- **管线的两个渲染图站点**（BuildRenderGraph + RenderWithCommandBuffer）必须镜像挂接
  （Vulkan 测试走后者——M3 首次挂接只插了站点一，全链静默无效果，排查耗时段）。
- **CLIP 测试钩子时序**：子系统在首个渲染帧才初始化——注册/烘焙统一延迟到帧 20。
- **单巨网格资产**（138MB cm 级 SponzaUVLM）在测试中不消费 fusion 呈现链——e2e 用规范
  Sponza.model 验证；UVLM 只做烘焙/收集链载体。
- **C++ 字符串拼接**：表驱动 JSON 行的收尾逗号必须在字面量内（`"},\n` 而非 `}",\n`）——
  schema 表接入时踩了一小时。

## 5. env 旋钮

| 变量 | 语义 |
| :--- | :--- |
| `PRIMAL_LIGHTMAP=0` | 整体关闭（零影响 A/B 总开关） |
| `PRIMAL_LIGHTMAP_DEBUG=1` | composite 全屏采样测试页（诊断） |
| `PRIMAL_LIGHTMAP_DEBUG=2` | composite 全屏纯洋红（消费链诊断档） |
| `PRIMAL_LIGHTMAP_BAKE=1` | Sponza 测试内帧 20 烘焙（密度自适应） |

## 5.5 v2 架构轮（2026-10-20 用户定调：独立渲染 + 路径追踪 64spp）

**需求变更**：lightmap 渲染与 Lumen 动态 GI 解耦（不再做 fusion 间接信号
置换）；烘焙必须是离线路径追踪且达 64spp。

**烘焙（LightmapBaker v2）**：积分核改逐纹素路径追踪——顶点 NEE（太阳/
点/聚光含阴影射线）+ 自发光命中 + 天空 miss + 余弦半球 BSDF 采样，
depth≥2 俄罗斯轮盘；产物 = 表面完整出射辐亮度（直射+多弹间接）；
samples_per_texel 默认 64。新增 TestBakerSkyDirect 天空直照回归。

**运行时独立消费**：FusionCompositeModule 钩子全撤（文件还原基线，Lumen
侧零改动、fluid 冲突面消除）；独立 composite 移至 fusion 最终输出之后、
后处理之前（两渲染图站点镜像），lightmap 覆盖像素整像素替换；
FrameInputs.indirect_tex → scene_color_tex。

**本轮新坑（黑图第三根因）**：演示地板 OBJ 绕序反 → cooker 法线朝下 →
半球射线全朝地下、天空/太阳零贡献。判别手段 = PRIMAL_LIGHTMAP_TRACE_TEXEL
单纹素分解（E0/miss 统计/sky 值）+ 样本逐分解。另：Vulkan deferred 实际
支持点光（旧记忆"仅单太阳"过时）。

**e2e 证据**（SPGI_CAM=0,6.5,-3,-42,0，30m 白地板+中庭点光，64spp）：
LMAP 均值 0.34/点光池峰 3.9；off/on 66.2% 像素显著差（max=520）；ON=
暖光池+径向衰减+柱基遮挡阴影（analyze_image 复核通过）；
/tmp/mini_lightmap/final2_*.png。单测 13 项全绿。

**v2 已知边界**：独立替换会覆盖 lightmap 像素的镜面/SSGI（漫反射表面
无感；后续可加 specular 透传项）；Dawn 无 fusion 链维持降级。

## 6. 遗留与后续轮新增证据（2026-10-19 深夜轮）

**§6.1 三根因修复轮（3f98ee5c）——真实视觉差异达成**：前轮"79% 变化"实为
debug 渐变分支，纯数据替换当时不可见。本轮逐实验定位并修复：
1. **烘焙产物全黑**：trace-only（无 UVLM）遮挡物 gather 命中按黑体吸收，
   Sponza 墙体全部 trace-only → 可烘焙面间接数学上恒 0。改为命中时一阶直射
   着色（太阳可见性×albedo/π + 天空 π·L(n) 近似）；新增单测
   TestBakerBlockerBounce（解析几何 F=0.83112，期望≈0.105 实测过）。
2. **UV pre-pass 采样恒指纹素 (0,0)**（该纹素恰为黑 → 替换不可见）：
   GLSL vec4 scale_bias 被 std430 对齐推到偏移 80，与 C++ float[4]@72 错位
   8B；拆 vec2 scale/bias（72/80），Metal 同步修。
3. **composite 诊断阶梯 z>0.5 分支在前 → debug 档死代码**；重排诊断优先，
   Metal 补档 3。
判别实验手段（复用价值）：DEBUG=1 屏幕采样页证上传/采样；UV RT z 清 1 证
pass 执行+RT 读回；深度临时 Always 证画出区域；通道优势法（g>r+25）检
fusion 消费后的暗绿（饱和绿阈值会漏检——绿经 FusionComposite 变暗）。
**最终证据**（SPGI_CAM=0,6.5,-3,-42,0 俯视 30m 白地板 frame 360）：
图集均值 0.35/0.45/0.64；生产 off/on 显著差 7.9%（72,926px，max=248，
带 gain=3 放大对比）；debug3 绿覆盖带与差异带重合；视觉复核=地板间接光由
暖调 DDGI 反弹变为更亮冷调天空主导。/tmp/mini_lightmap/final_*.png。

**真实数据链视觉证实已达成（b3c5e242）**：默认 Sponza + 旁路注册的 UVLM 地板，
全链零崩溃（烘焙→LMAP→直登记→entities rebuilt）；叠 debug 档对比 79% 画面变化，
地板像素 (17,14,11)→(21,22,21) 为孤立平面天空反弹的真实烘焙值（与 debug 渐变
色调可分辨），其余像素走 debug 分支——UV RT 逐像素选择性与真实数据替换双证实。
证据：/tmp/mini_lightmap/{off,e2e7,e2e8}.ppm。
（§6.1 勘误：本段"79% 变化"的主因是 debug 渐变分支，见上。）

**本轮发现并修复的两个生产 bug**：
1. 收集器/模块对 proxy.meshId 单步解析在真实场景全数失败（hierarchy id 需
   get_rhi_mesh_id 两步解析）——编辑器 "no proxies resolved" 的根因；
2. 测试注入实体号触发 transform 组件表越界（需真实 game-entity 号）。

**仍遗留（合并前）**：
1. 编辑器内 UVLM 资产导入后的完整 UI 终验收（无 CLI 导入命令，需手工导入；
   两步解析修复后编辑器烘焙收集对真实体已可用）；
2. rebase 后重跑零影响证明（fluid 线合入后）；
3. Metal 端视觉验收；
4. **上游 bug 移交**：ContentTools 多网格 OBJ 导入把 mesh[1..] 变全场景三角汤
   （Sponza cook 1/6 UVLM 的根因）——影响 fluid 线资产，独立立项修复；
5. 测试旁路 API（AddLightmapBakeMeshOverride/RegisterEntityDirect）仅供终验，
   生产路径仍是场景同步；
6. 测试 hook 的资产绝对路径指向主树 EngineTest/assets（worktree 内改动不生效，
   本轮踩过：12m 旧地板被反复加载）。

## 6.3 互斥轮（2026-10-20 用户追加需求）：五路实现全被挡，取证入档

**需求**："如果开启 lightmap，Lumen GI 就完全关闭"。五条实现路线与结果：
1. **跳过 fusion pass**（真抑制/质量开关）：fusion 缺席的图布局下 lightmap
   UV pre-pass 内容失效（exec 在跑、CPU 侧 scale_bias/UV 数据全对、句柄
   互异、验证层无错，但 composite 读到的 UV RT = 场景色样值——片元常量
   探针 (0.25,0.75) 完全不可见=本 pass 写入被丢弃/覆盖）；
2. **PRIMAL_GI_INDIRECT_GAIN=0**：触发 SC↔probe 回路分叉（ToneMappingPass
   注释官方警告），直射/点光一并变黑；
3. **SSGI/DDGI/SPGI_SILENCE 静音族**：fusionIn 空 rg → 融合内部 pass 集
   变化 → 同 1 的图布局失效；且 DDGI 空 tex 有"解析 sky fallback"自愈
   （黑≠关）；
4. **fusion 内插"间接清零"级**（v1 钩子位+独立 zero_tex_）：清零输出同样
   触发 sky fallback（地板变蓝非黑），且 fusion 内多一个 pass 后消费级
   替换同样失效；
5. **UV pre-pass 提出图外直录**：未实施（RHI 无现成 ad-hoc render pass 面）。
**对照实验铁律**：同一二进制下，图形状=v2-COEXIST 原样（UV 在 fusion 前+
composite 在 fusion 后+fusion pass 集不变）→ 替换稳定工作（19k 暖池像素）；
任何对 fusion 区域 pass 集的增删 → 替换消失。指向 RG/RHI 层对 pass 布局
敏感的资源失效（怀疑瞬态纹理内存别名或描述符集时序），需 GPU 调试器+
RenderDoc 下一轮攻坚。
**已交付**：互斥代码（钩子+清零级+zero_tex_）保留但 PRIMAL_LIGHTMAP_MUTEX=1
门控（默认关=稳定并存形态）；诊断旋钮 PRIMAL_LIGHTMAP_COEXIST=1。
**当前语义**（默认）：lightmap 覆盖表面整像素替换为烘焙完整光照（含该表面
的直射+间接），Lumen 继续服务其余表面——"逐像素互斥、同帧并存"。

## 6.4 全隔离互斥轮（2026-10-20 v3）：铁律破除一半，binding-2 采样之谜入档

**实现**（用户授权"全新通道、不复用 fusion 链"的 A+B 全隔离形态）：两渲染
站点（BuildRenderGraph Step 4 后 / RenderWithCommandBuffer deferred 后）以
`lmMutex = IsActive() && PRIMAL_LIGHTMAP_MUTEX` 门控整个 Lumen GI 区——
volume/SC/DDGI/WRC/SC-DDGI/SSGI/SSR/SPGI/GIGather/fusion（含 refl_specular）
十一个块全部让路，composite 以 deferred 直射输出为底图，UpdateFrame 提前
（幂等）。FusionCompositeModule 钩子实验代码还原基线（流体线 rebase 友好）；
LightmapModule 清 zero_tex_/zero_indirect；新增 PRIMAL_LIGHTMAP_UVDUMP（UV RT
一次性读回：有效像素数+bbox+三定点+页值，帧 360 读 ≥3 帧前已完成内容）与
PRIMAL_LIGHTMAP_SWAPUV（b1/b2 纹理交换判别）两个诊断旋钮。

**e2e 四路对照**（SPGI_CAM=0,6.5,-3,-42,0，frame 360，1280x720 判据同 §6.3）：
| 配置 | 暖池 | 池心 (431,416) | 墙 (200,300) | 结论 |
|---|---|---|---|---|
| HEAD + v2 shader（并存） | 170,344 | (148,131,99) | (106,83,46) | 基线复现 ✓ |
| 本轮 C++ + v2 shader（并存） | 170,292 | (148,131,99) | (106,83,46) | C++ 重构零影响 ✓ |
| 本轮 C++ + v2 shader（MUTEX） | 552 | (8,8,7) | (14,14,14) | 墙=直射-only ✓ 池黑 ✗ |
| OFF（本轮 / HEAD 逐值同） | 120,640 | (161,142,108) | (22,17,13) | 零影响 ✓（暖池=日光地斑） |

注：并存 ON 的墙暖增量来自烘焙演示点光（TestVulkanSponzaRenderGraph L2380
AddLight），非 Lumen 差异。

**§6.3 两项旧结论被本轮推翻/收窄**：
1. **"UV pre-pass 写入被丢弃"不成立**：MUTEX 形态下 UVDUMP 读回 uv_rt_ 含
   289,593 个 z=1 marker（与并存形态逐字节同分布）——写入存活，两形态
   uv_rt_ 内容一致；
2. **失效点精确到 composite 的 binding 2 采样**：DEBUG=3 全红（marker z≤0.5
   恒假）+ SWAPUV 换绑烘焙页（a=1 处处）到 b2 仍全红 ⇒ binding 2 槽位采样
   恒零，与绑哪张纹理无关；同 pass 内 binding 0（deferred 底图）采样正常
   （墙区直射-only 透传证明）。DS 追踪（PRIMAL_DS_TRACE）证实 b1=烘焙页
   0x44b7…/b2=uv_rt_ 0x3c7… 写入正确的原生图像；RGTRACE 证实 uv_rt_
   COLOR→SRO 屏障逐帧正常发出；除烘焙页遗留 TRANSFER_DST（并存形态同样
   存在、MoltenVK 下无害）外验证层零错；MVK_CONFIG_USE_METAL_ARGUMENT_
   BUFFERS=0 无改善。
3. **新发现（渲染资产生命周期纪律）**：重建 LightmapComposite.frag.spv（仅
   删除 mode-5 死分支、glslang 同版本）后并存形态也全红——提交在库的
   Lightmap 系列 spv 是 load-bearing，任何"无害"重建都需全量 e2e 复验后
   才能入库。本轮最终交付保留 v2 原版 spv 未动。

**当前语义**：默认=并存（v2，170k 暖池回归）；PRIMAL_LIGHTMAP_MUTEX=1 启用
全隔离互斥（Lumen 关闭 ✓、烘焙替换被 binding-2 之谜挡住，池黑）。下轮攻坚
方向：GPU 捕获（RenderDoc/Xcode GPU）对比 MUTEX/并存两形态 composite draw
的 descriptor 状态；候选根因：MoltenVK 稀疏描述符更新下的编码器状态泄漏。

## 6.5 互斥攻坚 II（2026-10-20 v3.1）：binding-2 之谜定罪为 MoltenVK
render→render 编码器边界缺落盘；16B 哑拷贝 flush 修复，互斥形态全绿

**逐块二分**（PRIMAL_LM_KEEP 单块放开旋钮，已随轮移除）：fusion 两 pass
放开仍死；SPGI 放开（位于 UV pass 之前）仍死；**refl_specular（Compute
pass，恰在 UV pass 与 composite 之间）单块放开即恢复**（DEBUG=3 绿带
55,464px）——magic 不是某个 Lumen 块本身，而是"UV pass(render 写附件) 与
composite(render 采样) 之间是否存在非 render 编码器"。

**交叉矩阵修正**（v2 spv 全程）：b1 绑烘焙页在互斥态正常（DEBUG=1 显页
内容——上轮"b1+b2 全死"是误读，页大部分为黑）；uv_rt_ 经 b1/b2 均读零
（纹理级失效而非槽位级）。CPU 帧末读回 marker 完好 vs 帧中采样为零 ⇒
写入存在但**对紧随的 render pass 采样不可见**：COLOR→SRO 屏障已发出仍
失效（RGTRACE 证实）。并存态之所以工作：refl_specular(Compute) 恰好位于
两 pass 之间。定罪：**MoltenVK(tiler) render→render 直连缺编码器边界落盘，
任何 compute/blit 编码器切入即强制收尾落盘**。

**修复**：LightmapModule::AddUVPrePass 互斥态（FrameInputs::encoder_flush）
在 UV pass 后追加 16B buffer→buffer 哑拷贝 pass（RGPassType::Copy）——
最小代价编码器切换；并存态不挂（v2 图形状零影响）。修复后与 keep=refl
参照逐字节同（带均值 (72,80,82)、样本一致）。

**指标修正（重要）**：地板烘焙带的真实屏幕位置=右上带（UVLM 地板远端
投影，DEBUG=3 绿带=marker 区，互斥/并存两态逐网格一致）；显示坐标
(431,416) 从未被 marker 覆盖——历轮"暖池"指标实测的是日光地斑+烘焙演示
点光（OFF 基线即 120k 暖像素）。本轮验收判据改用：①右上带 (x[640,1200]
y[0,380]) 与 OFF 的逐像素差（互斥 50,688/53,200px 重写=烘焙内容在显）；
②墙 (200,300)=(14,14,14) 直射-only（Lumen 关）。

**验收**（SPGI_CAM=0,6.5,-3,-42,0，frame 360）：
| 配置 | 烘焙带 | 墙区 | 暖池回归 | 判定 |
|---|---|---|---|---|
| MUTEX（交付形态） | diff 50,688/53,200，均值 (72,80,82) | (14,14,14) | — | ①②✓ |
| OFF（新二进制） | — | (22,18,13) | 121,472≈HEAD 120,640 | 零影响 ✓ |
| 并存（默认） | — | (104,82,45) | 167,520≈基线 170,344 | 回归 ✓ |
| 单测 | TestLightmapBaking ALL PASSED（14 项） | | | ✓ |

**遗留**：①编码器切换 flush 是驱动级 workaround，Vulkan 语义上 COLOR→SRO
屏障应已足够——建议 MoltenVK 侧提交 issue（macOS/Metal, tiler, render→
render 直连附件写入不可见）；②Metal 端（非 MoltenVK）未复验互斥形态；
③历轮以 (431,416) 暖池作指标的记录需按本节修正读法；④哑拷贝 pass 依赖
RGPassType::Copy 产生真实 blit 编码器（16B CopyBuffer），若未来 RHI 把
小拷贝优化为 no-op 需复验。

## 6.6 互斥转正 + 相机默认位姿 + 烘焙页可视化（2026-10-20 v4）

**需求 1：互斥转正默认**（用户定调"按正常逻辑，Lumen 不能和 lightmap
共存"）：lmMutex = IsActive() && !PRIMAL_LIGHTMAP_COEXIST（两站点）。
MUTEX env 门控撤销；COEXIST=1 保留作 A/B 诊断。零影响不变式保持：
无烘焙 ⇒ IsActive=false ⇒ lmMutex=false ⇒ Lumen 照常。
验收（frame 360）：默认 ON（裸跑）与 OFF 全帧 82% 差异=Lumen 关闭效应，
墙区直射-only；COEXIST=1 回归 168,860 暖池≈参照 170,260、池心
(148,131,99) 逐值同；OFF（同位姿）121,488≈HEAD 基线 120,640、墙
(22,18,13)≈(22,17,13)（帧间噪声内）。

**需求 2：相机默认 (0,5,0)/(0,0,0) + 可改变/可操控**：裸跑默认即此
（TestVulkanSponzaRenderGraph L2056，sponza_reference 为 opt-in env、
SPGI_CAM 仅覆盖初始化不锁操控）。操控链走查：WASD+鼠标 =
camera_.Update(dt)（L2252）、方向键 yaw/pitch（L2076-2097）、R 重置
(0,5,0)+(0,0,0)+TAA 历史清空（L2115-2124）——均仅受 SPGI_CAM_LOCK
（测量锁定）门控，默认开。SPGI_CAM 程序化换位姿验证："0,5,-4,-35,10"
marker 覆盖 86,554px、DEBUG=3 绿带 15,384px 经 composite 可见。
**如实报告**：默认位姿 (0,5,0) 水平朝 +X 下 UVLM 地板近乎侧对（远场），
marker 仅 6,917px（地平线细条 y[0,10]），烘焙带可见性弱；最近可行
位姿数据 = 0,5,-4,-35,10（覆盖 ×12.5）。**e2e 配方自本轮起**：验收
采集用默认位姿（无 SPGI_CAM）；需要烘焙带入画的判据采集用
SPGI_CAM="0,5,-4,-35,10"（替代旧 0,6.5,-3,-42,0）。

**需求 3：烘焙页可视化 PNG**：BakeLightmaps 落盘 LMAP 后同步导出
output_dir/lightmap_page.png（新文件 Engine/Graphics/Baking/
LightmapPagePng.{h,cpp}）：HDR→LDR = Reinhard（c/(1+c)）+ gamma 2.2，
per-mesh packer rect 叠 1px 白框；stb_image_write 以
STB_IMAGE_WRITE_STATIC 引入（内部链接——EngineTest 多 TU 自带 stb
实现且链接 libEngine，外部链接会重复定义）。导出失败仅打日志不阻断。
编辑器 CLI scene.bake_lightmaps 把 PNG 一并 TaskRegistry.AddArtifact
（kind=lightmap-page-png），Description 同步（schema 快照已重生成，
64 项 Cli 测试全绿）。
PIL 验收：PNG 1024×1024=图集尺寸；白框完整（顶行/左列逐像素白）；
矩形内均值 (87.2,85.9,83.2)、max 230 非全黑（整页均值≈0 系本场景
1 mesh、覆盖率 0.2%——非缺陷）。

**验收汇总**：单测 TestLightmapBaking ALL PASSED（14 项）；
/tmp/lm_agent/v4_*.ppm 为本轮全部证据。

## 7. 集成协议

与 fluid 线（主树未提交改动）在 `FusionCompositeModule.cpp`、`StandardRenderPipeline.cpp` 相遇：
谁先合 dev，另一边 rebase 解冲突；rebase 后重跑零影响证明 + §6 验收，然后
`git worktree remove .worktrees/lightmap`。

## 8. 全场景定义轮（2026-10-20，3a25a55b + 732451db）

**上游 ContentTools 修复（3a25a55b，小样本 3 网格/3 材质 OBJ 复现→修复→验证）**：
1. ObjImporter::load_obj_file MTL 搜索目录反斜杠 rfind——POSIX 下 npos→整路径当
   目录，.mtl 从不加载、材质全丢（macOS 多网格 OBJ 必现）。改 find_last_of("/\\")。
2. ObjImporter::get_mesh_data 每网格只 push 1 条 material_indices，而
   Geometry.cpp:820 split 按逐三角形读——合并后越界读堆，"mesh[1..] 全场景
   三角汤"的机制根因。改逐多边形填充（FbxImporter 同契约）；GltfImporter
   同型隐患一并修。修后小样本 18/18 网格 UVLM（修前 1/6），逐材质 bb 与
   源形状精确一致、三角形坐标逐点核对；ContentTools 11 套测试 108 项全绿。
3. AssetPipeline Phase 1.5：既有 LOD≥1 级补跑 uvatlas（OBJ/GLTF legacy LOD
   链此前只有 LOD0 拿 UVLM）。

**Sponza 重 cook**：Sponza.fbx 经 ContentToolsCLI --pipeline --no-lod
--no-remesh --uvatlas-lightmap → SponzaLM.model（主树 EngineTest/assets/，
12.5MB）：393 网格全带 UVLM（与旧 Sponza.model 同 393 网格布局）、206,596
顶点、总面积 12,669 m²、米制尺度（旧记录"厘米级"不适用于当前 FBX 链）。
注意：cook 工具是 worktree Darwin/Debug/{primal-engine,ContentToolsCLI}
（主树 DebugEditor 二进制为 9/17 dev 分支产物，无 UVLM 能力）。

**e2e 实测（SPGI_CAM=0,5,-4,-35,10）**：density 16 溢出→12 OK（2048 页，
393 entries，烘焙 ~14 分钟，frame 20 阻塞）。lightmap_page.png：覆盖 33.7%
（烘焙值 32.1%+chart 框 1.56%），全亮度带分布（26.2% 暗~4.6% 满亮），
页内最大 chart 978²、地板 chart 265² 均值 46.6/255。ON/OFF 98.90% 像素
显著差（max=151）；ON 上半屏（墙体/拱）为烘焙天空反弹值 (108-120,
115-127, 128-141) 冷调，OFF 为 Lumen 暖调。OFF 零影响复证：含 hook 改动
的新二进制（无 env）vs HEAD 旧二进制 = 0 像素差>8、六探针逐一相等。
SSBO 体量：3.3MB pos + 1.65MB uv + 3.1MB idx（无上传失败）。

**新遗留（独立立项）**：UV pre-pass 场景级消费在屏幕下半（近场地板）无效
——UVDUMP valid=50%（墙探针有效、地板探针无效；两个机位复现），而页内
地板 chart 健康、393 实体缓冲已建、单实体演示地板曾工作。疑 GBuffer
（meshlet 绘制）与 UV pre-pass 重绘的深度等值性近场失配。取证工具：
PRIMAL_LIGHTMAP_LOAD=<lmap> 跳烘加载 + PRIMAL_LIGHTMAP_UVDUMP。

## 9. 近场 UV 覆盖攻坚轮（2026-10-20 深夜，5cbeefd0）

**判别实验链（逐步定罪）**：
1. DEPTHALWAYS=1（跳深度比较）→ 覆盖 100%（3686400/3686400）：几何绘制
   完整，阻塞 = LessEqual 深度等值性，排除"实体未建/变换全错"；
2. TAA jitter 假设死路：GPUDrivenDraw 绘制链（PipelineDraw.vert /
   VisibilityBuffer.vert）均未应用 jitter（常量上传但 shader 不消费，
   spv 与源同步）；
3. Vulkan depthBias 死路：D32_Float 下 bias 按 ~1ULP 缩放，0.05/0.2 档
   覆盖逐位不变（1841967→1834960 为噪声差）；
4. VP z 行 (1-eps) CPU 收缩：eps=1e-3 → +59k px（屏幕中心地板恢复），
   eps=1e-2 → 59.5% 仍不饱和——**结论：两条变换链存在真实几何偏移**
   （GBuffer 实例矩阵三段式 GPU 链 vs UV pre-pass CPU 烘焙位置 +
   CPU 段拼 VP），非纯浮点舍入，epsilon 无法安全闭合。

**交付**：LightmapUvPassViewProjection（双站点，默认 eps=1e-3，
PRIMAL_LIGHTMAP_ZEPS 可调；PRIMAL_LIGHTMAP_DEPTHALWAYS 取证旋钮）。
OFF 零影响 0 像素差>8（max 5）；TestLightmapBaking 全绿。

**下一轮建议（数据级根因定位路线）**：
- UV FS 写 gl_FragCoord.z 到 RT .w 通道 + DumpUvRT 同帧读回
  GBufferDepthSampleable → 逐像素打印两链深度差的空间分布（按
  实体/按距离分桶），直接定位偏移来源与量级；
- 嫌疑排序：RenderScene 代理 transform 与 GPUDrivenDraw 实例
  world_matrix 的数据分叉（上传时机/来源组件不同）；其次 CPU
  transformPoint 与 GPU 三段乘的系统性差（若分布均匀随距离增长）。

## 10. §9 差分取证执行轮（2026-10-20 深夜 II，9a545b0b）

**插桩**（永久保留）：UV FS `.w=gl_FragCoord.z`（GLSL+MSL+spv，composite 不读）；
DumpUvRT 同帧读回 GBuffer 深度做逐像素 rel-delta 直方图（7 幅度桶 × 3 纵带）
+ 探针绝对深度；`PRIMAL_LIGHTMAP_UVONLY=<实体序号>` 单实体取证旋钮。

**判决数据**（SPGI_CAM=0,5,-4,-35,10，frame 360）：
- mesh 118 地板：可见处 **78859/78859 像素 delta≤1e-7（位级相等）**；
- mesh 17 二层廊道地板：**1554082/1554082 像素位级相等**；
- ⇒ **两条变换链无任何分叉**（world matrix/矩阵约定/VP 拼接全部排除）。
  §9 的"实例矩阵 vs 代理变换分叉"假设与上轮"6.8× 真实偏移"推断一并
  撤销——后者系 DEPTHALWAYS 全实体绘制被后绘远面覆盖污染直方图所致
  （UVONLY 旋钮即为此引入）。
- z 行收缩 eps=1e-3 工作如设计（通过像素 rel ≈ -1.1e-3，向相机侧）。

**剩余谜题（精确框定）**：屏幕中/下带存在大面积 GBuffer 深度 ~1.3-1.5m
的表面，**无一实体重绘能匹配**（帘幕 mesh 330 在视内、重绘几何距离正确
（8.9m 射线落点 y≈0 ✓），但其像素处 GBuffer 全为 1.3-1.5m 近面，delta
全 >1e-2）。同时 (200,300) 墙探针在 1.72m 处正常匹配——近面是真实场景
几何而非叠加层。候选：拱顶内侧 flank/近柱列/未识别的近场面。定位手段
（下一轮）：VisibilityBufferResolve 的 per-pixel mesh_id 读回直接点名。
布料仿真假设已排除（无仿真代码；静态双面）。

**重定性**：UV pre-pass 对全部已核静态面（地板/廊道/墙）工作正确；
"下半屏黑"= 该近面实体的 lightmap 替换缺席（互斥态透传直射-only）。

## 11. 收尾轮（2026-10-20 III）：bot 带点名收窄 + 验收欠账补齐

**点名进展**（UVONLY "a:b" 区间二分 + bot1/2/3 探针）：
- bot 带探针 gb 深度 0.85-1.23m；按相机几何反解世界 y≈4.16 ——**恰为
  二层廊道地板面高**（mesh#17 平面 y=4.1553，16 顶点完全共面）；
- [0,196) 与 [196,393) 两半实体在 bot 探针处**均零通过**——该近面不属于
  393 实体重绘集合（或其重绘在这些像素处系统性偏离）；
- 已排除：布料仿真（无代码）、probe 球叠加（画 backbuffer 不写深度）、
  实体序错位（chart rect 反证 UVONLY=17 确绘 mesh_17）；
- **未解矛盾（全部数字在案，供下轮直攻）**：UVONLY=17+ALWAYS 下 bot1
  读数 0.9443（d=1.79m→y=3.78，比平面低 38cm），而平面几何+eps 收缩
  应得 0.9187（=gb，恰可通过）；同一 mesh 远场 1.55M 像素位级精确。
  平面不可能有两个深度——指向 ALWAYS 读数污染源未明（或 bot 探针
  dispIdx 映射与我手算像素不一致）。
- 覆盖现状：判据位姿 51.4%（1893869/3686400）、默认位姿 63.9%
  （2354960/3686400）。**≈100% 目标未达**，根因锁定在上述单一矛盾。

**验收欠账补齐（最终二进制，含 .w 插桩 spv）**：
- ON/OFF 复采（判据位姿）：98.82% 像素差>8（max 151），与上轮 98.78%
  同构——远场不回归成立；ON 黑带（floor-center (9,9,9)）仍在（bot 带
  未解）；
- OFF 零影响：vs 上轮 HEAD 基线 0.00% 像素差>8（max 8=运行间噪声）；
- 单测：TestLightmapBaking ALL PASSED、TestAssetPipeline 12/12、
  TestUvAtlas 6/6；
- 提交：UVONLY 区间+探针（本轮）、文档本轮。

## 12. 两步对账轮（2026-10-20 IV）：映射排除、VP 复刻自证、矛盾锐化为"共面双深度"

**步 1（探针映射）**：DumpUvRT 打印整数像素坐标——bot1=(1280,892)
与手算一致，display→texture 换算无误。✓排除。

**步 2（CPU 投影复刻）**：精确复刻 CreateLookAtMatrix/CreatePerspectiveMatrix
（列主序）+ z 收缩 eps，mesh 17 的 16 顶点前向投影 + 屏幕重心插值：
- **center (1280,720)：CPU 0.931258 ↔ GPU 0.931152——f16 量化内一致**。
  整条相机/VP/像素映射复刻链自证正确；
- **bot1 (1280,892)：CPU 0.917895（=gb×(1-eps)，恰可通过）vs GPU 读数
  0.944336**——后者恰为同一平面 ~564 行处的值（行差 ~328）；
- mesh 17 全部 16 顶点 y=4.1553 完全共面 ⇒ 平面上任一三角形在同一像素
  深度唯一 ⇒ **RT(1280,892) 的 0.9443 不可能来自 mesh 17 在该像素的
  光栅化** ⇒ UV pass 之后存在部分性写入/内存别名（行 720 内容正确、
  行 892 为他源内容；UVONLY=118 的 LessEqual 帧 clear 值全帧存活 ⇒
  覆写似与 ALWAYS 大覆盖形态相关，或依赖当帧 RG 布局）。

**下一步最优路线（RenderDoc/Xcode GPU 帧抓取预案）**：
1. MoltenVK 层开 layer（MVK_CONFIG_TRACE_PATH 或 RenderDoc macOS
   Vulkan 支持）抓 UV pass 前后 uv_rt_ 内存快照与 RG 瞬态池分配表——
   直接看谁在 UV pass 后写了该区域（候选：RG alias pool 部分复用、
   composite/后处理某 pass 越界写）；
2. 或最小复现：构造两 pass 图（A 写纹理下半→B 读），在 MoltenVK
   上验证 tiler 部分区域可见性（§6.5 曾有编码器边界先例——本轮怀疑
   同族但形态不同：不是"读不到"而是"读到旧/他内容"）；
3. 复验时保留三件取证旋钮（DEPTHALWAYS/UVONLY/UVDUMP+坐标打印）。

## 13. 间接光色渗透轮（2026-10-20 V，93baba0a）

**验收发现**：最终渲染间接光黑白无彩（红帷幕反弹不红）。根因坐实：
收集器 v1 用 proxy.base_color 作 BakeMesh.albedo（LightmapSceneCollector.h:13
自注），而 RenderProxy 默认 {1,1,1} 白（RenderProxy.h:20）、测试从未赋值、
Sponza 材质色在 diffuse 贴图（Kd 近白）→ 全场景白 albedo 路径追踪。

**修复（方案 A）**：LoadSponzaScene 把每 mesh 已解码 diffuse（decAlbedo，
sRGB 8bit）逐像素转线性（pow 2.2）strided 平均（≤64k 样本/mesh）写入
proxy.base_color[0..2]；收集器零改动。收集侧新增前 10 mesh albedo 日志。
数据链注：编辑器生产路径将来应从材质系统同源注入（本轮为测试链）。

**实测**（SPGI_CAM=0,5,-4,-35,10 + SponzaLM + BAKE，density 12 重烘）：
1. 收集日志：per-mesh 彩色（mesh_0 叶簇 (0.053,0.046,0.029) 偏暖绿暗、
   mesh_1 植物土盆黄主导、mesh_2 花瓶红棕——注入链可见）；
2. 页差（pre/post 同 packing）：显著变化纹素 **红主导 82%**
   （832835:10852:1281）——红帷幕反弹红定量坐实；均值亮度 105→52
   （白→真实 albedo 的物理量级修正，旧白 albedo 烘焙偏亮 ~2×）；
3. ON 上屏（GAIN=5 曝光匹配消费侧既有旋钮——实时链 50× 直射放大
   [C2.1] vs 烘焙物理单位的尺度差，均值 54.2≈修前 53.1）：阴影区
   R-B=+8、地板带 +11~+13；修复前间接蓝调（back-arch B>R+21）——
   色相反转即彩色反弹；
4. OFF 零影响 0.00%（max 5）；TestLightmapBaking/TestAssetPipeline 全绿。

**遗留**：方案 B（per-texel 贴图采样，贴图花纹参与反弹——UV0 在
RHIMeshAsset，成本=收集期解码+双线性）；金属度未参与（链/旗杆反弹
按漫反射处理）；GAIN=5 为显示尺度匹配而非物理修正（正解=bake 输出
对齐实时单位制，另行立项）。

## 14. 方案 B——per-texel 贴图采样 albedo（2026-10-20 VI，84caf704）

**实现**（纯 C++，零 spv）：BakeMesh 契约扩展（albedo_texture sRGB u8 +
w/h + material_uvs + source_mesh_id，缺一回退方案 A）；收集器从 element
static_normal_texture(0x3) 24B 布局提取材质 UV0（uv=每顶点末 8B f32 对，
与 PipelineDraw 顶点拉取同源同偏移）；管道 SetLightmapBakeAlbedoTexture
注册（照 AddLightmapBakeMeshOverride 模式）；测试侧 stbi 释放前 ≤256²
盒滤波降采样 u8 副本注册（392/393——Material__300 无 diffuse 回退 A）；
baker 栅格化重心插值 UV0 → 双线性 → 256 项 LUT 线性化。**顺修 §13 UB**：
平均色原在 stbi_image_free 后读已释放内存（堆运气下碰巧正确），移到释放前。

**实测**：
1. 单测 TestBakerTexelAlbedoVariance（条纹贴图→左红/右蓝主导+方差）通过；
   全套 15 项 ALL PASSED；
2. 烘焙耗时 **958s vs 方案 A ~13min（+~25%）**——逐 texel 双线性无额外
   射线，纯采样开销；
3. 页差（A vs B 同 packing）：**365k/989k 纹素显著变化（37%）**——
   per-texel 真值 vs 全网格平均；蓝主导 179445:红 84599；均值亮度
   53.7→40.9（average-of-linear 比 linear-of-average 暗——Jensen 间隙，
   物理正确方向）；
4. 上屏（GAIN=5 同口径）：B vs A **9.56% 像素差>8**（上半阴影区
   15049 : 下半 7008——远面/掠射角花纹细节进入反弹）；vs OFF 92.68%
   差异；floor-center (117,109,94) 与 A (114,111,103) 同量级；
5. OFF 零影响 0.00%（max 4）。

**遗留**：金属度仍未参与（链/旗杆按漫反射反弹）；UV0 插值在 lightmap-UV
参数空间线性（v1 近似，与 pos/normal 同级）；编辑器生产路径需把贴图
albedo 从材质系统同源注入（当前为测试链注册）。
