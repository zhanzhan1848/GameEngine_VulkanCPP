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

## 21. §21 视角相关黑度 + 室内泛蓝/格子缓解（2026-10-22，代理中断由主会话收尾）

**问题 1：相机微移改变黑度（用户疑法线）——定论：deferred 直射侧，与 lightmap 无关**。
texel 级双机位分解（texel_shadow.py，p1/p3 微移机位 composite.bin+uvrt.bin）：
跳变暗 texel 子集（3.8%）**base（deferred 直射）0.0043→0.0755（17×）而 shadowVis 仅
0.060→0.069、marker/E 逐 texel 稳定**——驱动=阴影求值随相机视差变化（PCSS 级联
边界/半影，cascade extents 12/30/90/270m，相机距墙 ~11.6m 恰在首两级交界），
非法线（着色法线不随相机变）、非 marker 闪烁、非 E 值。两模式共用同一 deferred
直射链，OFF 同在。不修（引擎既有行为，超出 lightmap 范围）。

**问题 2a：室内泛蓝——DARKTINT 落地**。composite 内无有效间接暗像素暖化
（PRIMAL_LIGHTMAP_DARKTINT 默认 1.0，tint (1,0.94,0.74)，暗度 smoothstep(0.004,0.05)
权重，push 20B 第 5 参；间接项 Luma>3e-3 豁免；不动 Lumen/deferred 共用文件）。
实测：判据位姿暗区 B/R **1.956→0.853**（OFF 参考 0.804）；CAM_A 暗桶 p50 0.904→0.862。

**问题 2b：格感——烘焙级低通落地（诚实收益）**。baker PRIMAL_LIGHTMAP_LP（默认 1，
分离二项式核，tap 源=同 rect 内 A==1.0，A 语义不动）+ 离线同式工具 lp_lmap.py 已应用
到交付页（备份 /tmp/lm_q5/pre21_backup.lmap）。FFT 15-60px 周期段 0.218→0.193（-12%
——§20 已证该段主体为材质/几何周期，E 内容部分就这么多；8192 页仍是根治项）。

**零影响**：新二进制（含 DARKTINT/LP）OFF 双运行低频 MAD **0.02**、>8 像素 0.0%。
单测沿用 §20 全绿（本轮无 baker 契约变化）。交付页 /tmp/lm_e2e_bake/lightmap.lmap
（LP 应用版，4096/393 entries）。

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

## 15. 精度/远平面暗带/PT 复核轮（2026-10-21）

### 15.1 任务 1：atlas 4096 + 密度阶梯扩 32/48

- `DefaultLightmapBakeSettings.atlas_size` 2048→**4096**（LightmapBaker.cpp:40-42；
  单测默认值断言同步 TestLightmapBaking.cpp:209）；e2e 密度阶梯上限
  {48, 32} 前插（TestVulkanSponzaRenderGraph.cpp ~2423）。
- **阶梯实测**：density 48 与 32 在 4096 页均 atlas overflow（32 档需
  ~5461px 等效页），**16 装入**（2048 时代 16 溢出→12）。纹素密度
  12→16 = 线性 ×1.33 / 总纹素 ×1.78——页面积 ×4 未换来密度 ×2，因
  场景 chart 需求大。密度真正翻倍需 8192 页（RGBA16F 536MB）或多页
  atlas——遗留项。
- **烘焙耗时**：4096/16/64spp = **22.5 分钟**（10:08:32→10:31:00，与
  2048/12/64spp 的 958s 相比 ×1.41，与纹素 ×1.78 大体一致；采集进程
  轻度争用）。64spp 未超 40 分钟预算，**未降 spp**（denoise 仍开）。
- **验收**：
  - 图集 PNG：/tmp/lm_opt/atlas4096_skyi1/lightmap_page.png（4096²，134MB
    LMAP）vs /tmp/lm_opt/atlas2048/lightmap_page.png——地板/拱花纹更细
    （对比图 /tmp/lm_opt/atlas_cmp_floor.png 上 2048/下 4096）；
  - 上屏（同机位 0,5,-4,-35,10、同 GAIN=5、同天空）：屏幕高频能量
    Laplacian 方差 3.3→**7.5**（×2.3，含亮度贡献）；对比 crop
    /tmp/lm_opt/shadow/atlas_screen_cmp.png。远场分块感减轻但非数量级
    变化——如实入档。
- **4096 实时预览命令**：
  `SPGI_CAM="0,5,-4,-35,10" PRIMAL_TEST_MODEL=SponzaLM.model PRIMAL_LIGHTMAP_BAKE=1 PRIMAL_LIGHTMAP_LOAD=/tmp/lm_e2e_bake/lightmap.lmap PRIMAL_LIGHTMAP_GAIN=5 ./Darwin/Debug/TestVulkanSponzaRenderGraph`
  （lmap 现为 134MB/4096²；PAGEDUMP 纹素坐标换算已改用页实尺寸，
  LightmapModule page_w_/page_h_——修 1023 硬编码遗留。）

### 15.2 任务 2：远平面"黑影跟随相机"——z-eps 嫌疑排除 + 真根因（烘焙天空单位失配）

**验证 a（ZEPS=0 覆盖对照，同二进制 env A/B）**：
- 默认 eps=1e-3：UVDUMP valid(z>0.5)=**3686101**/3686400（复现 §6.6 基线）；
- PRIMAL_LIGHTMAP_ZEPS=0：覆盖塌到 **100382**（2.7%）——**z-eps 不是镜像
  时代残留，是承重的**：CPU 段拼 VP 链比 GPU 三段链系统性偏正 ~+7e-5
  相对深度（center 探针 0.932203 vs 0.932136），(1-1e-3) 收缩即为此容差。
  用户"矩阵问题"怀疑方向正确但定罪对象错误。
- eps=2e-4：覆盖 3686100（-1px）——容差可收紧但黑带不变（见下）。

**真根因定位**（DEBUG=3/4 + 顶缘深度探针）：
- 顶缘暗带行 marker 全覆盖（DEBUG=3 全绿到 row 0）；DEBUG=4 显示带内
  UV≈(0.34,0.345) 指向合法 chart（均值 46.6/255 的暗烘值）；
- 顶缘探针（新增 top1-4 @ display y 4/12/24/40，DumpUvRT）：eps=2e-4 时
  redraw-gb 相对深度 -2e-4~-6e-4 ≈ 同面容差内——**marker 是同面的、
  合法的**；cull=front（匹配 GBuffer 光栅面集，覆盖同 3686101）与
  cull=back（覆盖塌 69150，证 front 为匹配集）矩阵实验均不改变暗带；
- 结论：暗带不是覆盖/marker 错写，是**远场（atrium 深处，天空主导照明）
  烘焙值本身过暗**：烘焙天空是程序梯度（BakeSky 峰值 ~0.65 ×intensity 1），
  运行时是 HDR 环境贴图——远场 ON top16 均值 17.6 vs OFF 25.6/255。
  相机前后移动时该区域恒贴远平面 → 用户感知"黑影跟随相机"。
- **修复**（LightmapSceneCollector.cpp 末尾）：收集器天空辐射校准
  **×2.0 默认**（PRIMAL_LIGHTMAP_SKYINT 可调，1.0=旧行为）。校准依据：
  远场目标 OFF 25.6/255，×2.0 预计 ~28。太阳 NEE 走 RenderLight 单位
  不变，GAIN=5 不变。HDR env 方向采样入烘焙为后续项。
- **验收**（4096/skyI=2.0 页，3 位姿 0,5,{0,-4,-8},-35,10）：远面区
  （画面上缘）无黑影带——top16 均值/off 对照与暗行带检测见 §15.4 证据。

### 15.3 任务 3：烘焙核 = 路径追踪（复核，无码改）

LightmapBaker.cpp L470-570 积分器语义逐项核对（v2 2026-10-20 用户定调后
未再动，本轮全链走查）：
- 逐 texel 路径循环：L477-482（spp 条路径/texel，种子 HashTexel(texel,s)）；
- 顶点 NEE：L491-492 每路径顶点 DirectIrradiance（L191-230，方向光/点/
  聚光均含 IntersectAny 阴影射线），贡献 β·(alb·E)/π——Lambert f=alb/π
  与辐照度 E（内含唯一 ndotl）各一次，**无重复 cos/π**；
- 余弦半球 BSDF 采样：L108-115（pdf=cosθ/π），L502-507 权重
  f·cos/pdf = albedo 精确成立（无残余因子）；
- 天空 miss：L511-514 β·L_sky（SkyRadiance L182-188，地平线以下 0）；
- 自发光命中：L486（顶点 0 自发光）+ L520-521（命中面 β·Le）——解析灯
  与自发光材质互斥，无 NEE/命中双重计入；
- 俄罗斯轮盘：L495-500（depth≥2，q=max(albedo 分量)，pCont=clamp
  q∈[0.05,0.95]，存活 β/=pCont——无偏补偿）；
- 吞吐量 β：L484 初始化 1，L502 散射 ×alb，L499 轮盘 ÷pCont，miss/
  自发光/NEE 一律 ×β；
- 命中面属性：hitSurface L451-468 重心 (1-u-v,u,v) 与 BVH Möller–
  Trumbore（BVH.cpp:399+）约定一致；法线对射线取反（L467 双面薄板）；
  自相交防护：沿法线偏移 1e-3 + ray.t_min=1e-3（L508-509）。
**结论：语义为带 NEE 的单向路径追踪，未发现偏差，零码改。**

### 15.4 暗带验收实测（4096/skyI=2.0 页，3 位姿 0,5,{0,-4,-8},-35,10）

- **烘焙 2（skyI=2.0 默认，4096/16/64spp）**：10:32:00→10:54:07 = **22.1 分钟**
  （与 skyI=1 的 22.5min 持平——天空缩放零成本）；
- **z=0**：top16 均值 60.1→**72.5**（+20%），暗行带 无→无；
- **z=-4**：top16 均值 17.6→**22.8**（+29%，与页暗区 +30% 一致）；暗行带
  （均值<25，上缘）行 0-53 → **行 0-10**（残余为远端地板/墙交界真实
  几何暗部，OFF 同位置也暗）；
- **z=-8**：整屏纯黑——**独立预存缺陷**（昨夜 2048 基线页同样全黑，
  OFF 正常 48-66）：该位姿整屏 marker 采样集中在 U[0.05,0.27]×
  V[0.11,0.39] 一段 chart 的**未栅格化空区**（该区域页 25.4% 非黑，
  探针 UV 均 (0,0,0)，且不在 rect 边缘——空区在 rect 内部或 UVLM
  越界逃逸，待分型）。CULL=front 不改变（非背面问题）。取证链
  logs/pose-8_{after_skyi2,off,dbg3,dbg4,uvdump,cullfront}.log +
  shadow/pose-8_*.ppm。**遗留**：UVLM 值域审计（越界→marker 出 rect）
  与 chart 空区成因（unwrap 岛外浪费）分型。
- 对比图：shadow/band_strip_z{0,-4}.png（上 before/下 after 顶缘 60 行）、
  band_fix_z{0,-4}.png（整帧）。

### 15.5 证据与回归汇总

- 单测 TestLightmapBaking ALL PASSED（15 项，含 TestBakerTexelAlbedoVariance）；
- OFF 零影响：同批双运行（新二进制）低频(32×) MAD=**0.038** < 3.5 方差地板；
- 关键产物：/tmp/lm_opt/{atlas2048,atlas4096_skyi1,atlas4096_skyi2}/
  {lightmap.lmap,lightmap_page.png}（逐 SHA 已录）；对比图 atlas_cmp_floor.png、
  atlas_screen_cmp.png、shadow/ 位姿序列；日志 logs/uvdump_*.log、
  logs/mx_*.log、logs/probe_top_*.log、logs/bake4096*.log、logs/pose-8_*.log。
- skyI=2 交付页：/tmp/lm_opt/atlas4096_skyi2（lmap SHA16 2ebb4c41529dc43f，PNG f7beaa0142b0243c）；skyI=1 对照页 eda8024289b948c7/869681b86fb298a8；
- 交付配置 UVDUMP 覆盖（4096/skyI2/默认 env，z=-8 位姿）：3686318/3686400（99.98%——非 z=-4 判据位姿的数字，判据位姿矩阵已录 3686101/3686100）。

## 16. v3 架构轮（2026-10-21 下午）：烘焙改间接辐照度 + 合成改叠加

**用户四问题**（同一根因链：v2 整像素替换——烘焙存完整出射辐亮度，albedo
已乘入且被 16 tex/m 低频页拖平）：纹理无细节 / 地板光照不对 / 远平面死黑
（含 z=-8 整屏黑预存缺陷）/ 直射间接无区分。

**架构处方（经典正解）**：
- **baker 存间接辐照度 E_ind**（≥1 弹 + 天空；不含顶点 0 直射 NEE、不含
  自发射、不乘纹素自身 albedo/π）。估计器：E_ind = π·mean(L_ind)，余弦
  半球采样 pdf=cos/π；单样本贡献 = 首跳 Sky/命中 Le + ≥1 弹顶点
  alb·E_direct/π，首跳不折叠 albedo（推导入 LightmapBaker.cpp 注释）。
- **composite 改叠加**：final = deferred直射底图 + GBufferAlbedo ×
  lm.rgb / π × gain（binding 4 新增 GBuffer albedo，fusionIn 同源先例；
  Metal 槽位 = binding 直映 texture(4)）。纹理细节/直射锐影走实时链，
  间接 = 低频烘焙信号被逐像素真实材质纹理调制。
- **页 A 通道 = 有效性**（1 栅格化 / 0.5 dilation 填充 / 0 空区）；
  composite 对 a≤0.25 不加间接（退化为纯直射，绝不黑——§15.4 z=-8
  整屏黑的解）。旧全光页 A 恒 1，兼容。
- 互斥不变（lightmap 激活 ⇒ Lumen 区全关）；PRIMAL_LIGHTMAP_GAIN 语义
  = 仅间接增益，默认 1.0 物理恒等（GAIN=2 实测仅 +1.6 mean——深影区
  E≈0 无数据可救，增益不值得偏离物理，保留 env 可调）。

**验收数据**（SponzaLM 4096/64spp/3bounce 烘焙 17min@density16；
/tmp/lm_arch；同机位 v2 基线 = 旧二进制+§15 skyI2 页重采）：

| 位姿 | 版本 | mean | 近黑<8 | 拉普拉斯方差 | 暗行<25 |
|---|---|---|---|---|---|
| 默认(0,5,0) | v2 | 38.87 | 31.21% | 366.4 | 178 |
| 默认 | **v3** | **77.83** | **4.66%** | **1041.7 (2.85×)** | **0** |
| 判据 z=-4 | v2 | 25.99 | 29.55% | **2.9（全平）** | 295 |
| 判据 z=-4 | **v3** | **39.48** | **2.01%** | **206.6 (71×)** | **0** |
| z=-8 | v2 | 整屏纯黑（§15.4） | 100% | — | — |
| z=-8 | **v3** | 28.91 | 0 纯黑/42%<8 | 24.6 | 0 死黑带 |
| z=0 | v3 | 62.90 | 0.24% | 402.3 | 0 |

- ① 纹理细节：判据位姿 lapvar 2.9→206.6（71×），默认位姿 2.85×；
  视觉复核（参考截图平光感消失，石缝/雕花/地砖清晰）。
- ② 直射/间接区分：太阳直射区锐利实时阴影 + 阴影区烘焙软补光（AI 视觉
  复核确认"strong direct/indirect separation, sharp shadow boundary"）。
- ③ 4 位姿零纯黑像素（lum≤1 全 0%）、零暗行带；z=-8 从整屏黑恢复为
  带纹理光照（残余：该位姿大部分像素落 chart 空区/深廊真低间接 → 无
  间接补光、比 OFF(Lumen 62.7) 暗——§15.4 UVLM/unwrap 质量遗留，
  alpha 门控按处方降级为纯直射，绝不黑）。
- ④ 地板：E 量级合理（mesh_389 lit 区 E.g mean 0.554/p95 1.84，
  p50=0 深廊无光真值），albedo 调制后纹理可见。
- ⑤ OFF 零影响：同批双运行低频(32×) MAD=0.268 < 3.5 地板；单测
  TestLightmapBaking ALL PASSED（E 语义重算期望：解析 0.41558→2.6108、
  天空 0.40→1.57、blocker 0.105→~1.36 界(1.0,2.0)、ceiling 全光断言
  改为"只存入射间接≈0"、条纹测改锁"E 与自身 albedo 解耦"）。
- UVDUMP 覆盖（z=-8 位姿）：3686318/3686400=99.98% 不变。
- 页数据验证：A=1 21.2% / A=0.5 4.1% / A=0 74.7%（原始 u16 0x3C00/
  0x3800/0x0000 直读）。

**坑位**：①e2e LOAD 分支嵌套在 BAKE 检查内——PRIMAL_LIGHTMAP_LOAD
必须与 PRIMAL_LIGHTMAP_BAKE=1 同设（LOAD 优先跳烘），漏设 BAKE 时
LOAD 静默无效（本轮曾致一轮"假 ON"采集，靠 loaded-log 计数发现）；
②Metal 绑定槽位 = binding 序号直映（binding 4 → texture(4)，sampler
不占位）；③stash 切换重建旧基线二进制时 /tmp 副本缺 @executable_path
dylib——直接用树内二进制跑。

**产物**：/tmp/lm_e2e_bake/lightmap.lmap（4096 E_ind 页）+ 
lightmap_page.png；/tmp/lm_arch/{on_*,off_*,old_v2_*}.ppm、
cmp_default_v2_vs_v3.png、cmp_z4_v2_vs_v3.png（左 v2 右 v3）、
lm_metrics.py、logs/。v2 交付页仍在 /tmp/lm_opt/atlas4096_skyi2。

## 17. 观感轮（2026-10-21 晚）：间接区蓝纱（天空色温对齐）+ 室内渐亮（曝光适应定罪）

**用户两反馈**：① 间接光区域有一层蓝色泛光；② 进入室内间接光"逐渐变亮"。

### 17.1 问题 ① 判别链（蓝纱）

- **a 图集量化**（atlas_br.py，lmap=32B header+104B×mesh+f16 RGBA——
  .h 注释"40B"与实际 8×u32=32B 不符，按文件大小反推实证）：
  E_ind 亮区（天光主导 p90+）B/R p50=**1.465**，RGB(1.97,2.32,3.02)——
  烘焙天空色直接透出页数据。
- **a 上屏探针**：中亮度间接区（40-70 百分位）B/R ON 0.87(z4)/
  **1.94(z8)**/1.04(default) vs OFF 0.69/0.75/0.74——ON 系统性偏蓝。
- **b bloom A/B**：PRIMAL_BLOOM_INTENSITY=0 vs 默认 4.0，z8 中亮度区
  **逐位一致**（R14.3 G21.4 B28.6）——非 bloom 载体（BloomPass threshold
  =3.0 HDR，间接合成 ~0.5 不进亮通；直射 12.5 两态同进与 lightmap 无关）。
- **c 双重计入**：DeferredLighting.frag `IBL_DIFFUSE_INTENSITY=0`（编译期
  常数）——deferred 无环境漫反射，无 diffuse 重复计能；`IBL_SPECULAR_
  INTENSITY=0.35` 环境镜面两态同在，非 lightmap 引入。
- **根因**：BakeSky 旧默认 zenith(0.35,0.45,0.65)/horizon(0.55,0.60,0.70)
  （B/R 1.86/1.27，冷蓝、峰值 ~0.65）与运行时解析天空 EvaluateSkyBase
  （horizon(4.40,4.20,3.80) 暖 B/R 0.86 / zenith(5.50,5.76,6.00) B/R 1.09）
  **色温方向相反 + 数值域差 ~7 倍**（天空比太阳 NEE 5.0 暗 7 倍，反弹
  构成失真）。

### 17.2 修复（烘焙天空与运行时逐位对齐）

- `LightmapBaker.h` BakeSky 默认 = EvaluateSkyBase 逐位镜像；
  `LightmapBaker.cpp` SkyRadiance 混合曲线 pow(up,0.45) + 地平以下返回
  地平色（对齐运行时 clamp(up,0,1) 语义）；collector PRIMAL_LIGHTMAP_
  SKYINT 默认 2.0→1.0（§15 的 ×2.0 是 v2 暗天空时代的补暗校准，依据
  已废；env 保留微调，≠1 时才乘）。
- 单测 TestBakerSkyDirect 期望重算：余弦加权 ∫pow(cos,0.45)cos dω/π
  =2/2.45=0.8163 → E_g=π·(4.20+1.56×0.8163)≈**17.2**，界(15.5,19.0)；
  TestLightmapBaking ALL PASSED。
- 重烘 4096/64spp/3bounce density16 = **1250s**，SHA16
  **deff7de4b70e6c57**；旧页归档 /tmp/lm_polish/atlas_v3_oldsky
  （SHA16 b8d2ef5ebf8c6001）。

### 17.3 验收（新页 vs 旧页 vs OFF）

- 图集：亮区 B/R **1.465→0.955**（冷蓝→微暖），E_ind 亮区均值
  2.44→15.39（HDR 档对齐运行时 GI-miss 域）。
- 上屏阴影区（<p35，B/R p50）：z4 **0.889→0.750，OFF 参考 0.756——
  与 OFF 几乎一致（验收线达标）**；中亮度区 z4 0.869→0.749（OFF 0.610，
  残余=烘焙解析天空与 sunset env 固有色差）。
- 亮度：z4 mean 39.48→**70.96**（OFF 64.20——ON 与 OFF 量级首次匹配，
  旧页暗 38%）；z4 近黑 2.01%→1.53%；lapvar 206.6→233.5（纹理细节保持）。
- **z8 新旧逐位一致（mean 28.91 完全相同）**：该位姿大部分像素落 chart
  空区（§16 遗留），A≤0.25 门控纯透传 base、与页内容无关——解释性复核，
  非回归。
- OFF 零影响：同批双运行低频(32×) MAD=**0.194**<3.5 地板；mean
  64.23/64.05 vs §16 基线 64.20。
- 视觉复核：cmp_z4_old_new_off.png（AI 视觉确认旧页阴影区冷蓝纱清晰
  可见、新页暖调恢复接近 OFF、残余为绿旗材质真实反弹色）。

### 17.4 问题 ② 定罪（室内渐亮）

- **预检**：LightmapModule（UV pass/composite）零 history/accumulation
  ——逐帧静态，无时域累积嫌疑。
- **实验**：SPONZA_CAM_SCRIPT=1（f360 切镜）+ 收敛窗 340-419 逐帧
  SPONZA_CAPTURE_DIR 序列 + PRIMAL_TONE_ADAPT_LOG=1 曝光采样，三态：
  - **ON**（新页）：cut 后 29.59→29.27 **平坦**（无渐变）；log2Lum 恒
    **-6.0 = minLogLum 钳制下限**（avgLum<0.0156），曝光恒打满 2.88。
  - **OFF**：cut 后 86.59→61.81 渐变收敛 ~50 帧（τ_down=1.0s 签名）；
    log2Lum -1.996→-1.167、effExp 0.18→0.10。
  - **ON+TONE_ADAPT=0**：5.29→5.37 平坦（固定曝光画面仅 5.4/255——
    适应把 ON 拉亮 5.5 倍）。
- **结论：机制=自动曝光时域适应（EyeAdaptation，τ_up 0.3s/τ_down 1.0s，
  默认开），非 lightmap 时域 bug**——OFF 同机制有渐变（切到亮场渐暗、
  对称成立）。如实向用户解释；可选 PRIMAL_TONE_ADAPT=0 回固定曝光。
- **ON 特有放大因素**：ON 场景 HDR avgLum 贴适应下限（<0.0156 vs OFF
  0.25-0.44，差一个数量级以上）——chart 空区 74.7% 像素（§16 unwrap
  遗留）无间接照亮拉低全图均值；场景亮度在钳制临界带波动时曝光持续
  大幅调整，"渐亮"观感被放大。新页下仍贴下限——**问题①的标定修复
  不足以完全收敛适应幅度，根治=修 unwrap/chart 空区覆盖（遗留项）**；
  可选缓解=评估 minLogLum 下限上调（-6→-4 限最大曝光 0.72，但改变
  OFF 观感，需独立门禁轮评估）。

### 17.5 证据与产物

- /tmp/lm_polish/：atlas_br{,_new}.txt、screen_br.txt、bloom_ab.txt、
  shadow_new.txt、accept_out.txt、curve_{on,off,noadapt}.txt（三态
  收敛曲线）、cmp_{z4_old_new_off,z8_old_new,default_new}.png、
  on_new_{default,z4,z8}.ppm、off_z4_new_{a,b}.ppm、on_z8_bloom0.ppm、
  atlas_v3_oldsky/（旧页归档）、atlas_br.py/shadow_probe.py/accept.py/
  adapt_curve.py；seq_* 序列 ppm 已清（曲线数据留存 curve_*.txt）。
- 日志：/tmp/lm_arch/logs/{bake_skyalign,seq_on,seq_off,seq_on_noadapt,
  on_z8_bloom0,off_z4_new_a,off_z4_new_b}.log。
- 新交付页：/tmp/lm_e2e_bake/lightmap.lmap（4096 E_ind 对齐天空页，
  SHA16 deff7de4b70e6c57）+ lightmap_page.png。

## 18. chart 空区覆盖轮（2026-10-21 深夜）：bbox×精确密度装箱 + 洪泛填充

**任务**：74.7% 空区（§16/§17 遗留）→ <30%；主嫌疑"UVLM 岛屿 bbox 不满
[0,1]² 而装箱按整块分配"。先诊断后动手。

### 18.1 诊断（PRIMAL_LIGHTMAP_UV_AUDIT=1——baker 新增审计早退通道，collect+pack+raster 后打印逐 mesh UVLM 值域/覆盖率并跳过 trace）

- **主嫌疑否证**：393 mesh 岛屿 bbox ≈ 整块 [0,1]²（ΣbboxArea=391.03/393，
  sx/sy≈0.999，越界引用 0）——xatlas 类展开把每 mesh 归一化到满幅，
  "bbox 只占一部分"不成立。
- **两真构成**：
  1. 岛屿只填自身 bbox 的 ~66%（fill p25/p50/p75 = 0.57/0.65/0.75，
     min 0.055）→ rect 内 34% 空区（旧装箱实测 rect 覆盖率 0.75 =
     fill 0.66 × 1.3 假满铺系数的余量）；
  2. **页 71.7% 从未分配**（density16 pack_util 仅 0.283）——密度阶梯
     32→16 档间 4× 跳变，32 溢出直接掉回 16，页利用率腰斩是 74.7%
     空区的主项。
- raster 无"丢洞"：覆盖率与 fill 一致，缝隙全为岛屿间真实 bbox 间隙。

### 18.2 修复（纯 C++，零 shader/ABI 改动）

1. **rect = 岛屿 bbox × 精确密度**（LightmapBaker.cpp pack 段）：
   k = density·sqrt(A_world/A_island)（texel/UV 单位，两轴一致 = 保
   aspect）；w=sx·k、h=sy·k（长宽比 = bbox 长宽比）；栅格化命中数 =
   A·density² 与 unwrap 缩放解耦、精确达标。旧"世界面积×1.3 假满铺
   [0,1]²"仅留作退化 UV（零面积岛/零值域）回退。
2. **uv' = (uv-min)/size 归一 → rect**：LMAP scale/bias 写同一仿射
   （scale = rect/(size·page)，bias = rect/page − min·scale）——运行时
   UV pre-pass 读同 scale/bias，烘焙/运行自动一致；UVLM 越界逃逸也被
   bbox 归一结构性收编。
3. **洪泛填充**（dilate 后新增阶段）：rect 内残留无效纹素多源 BFS 填满
   （取已有效 8 邻均值，A=0.5 padding 语义与 dilation 一致，源/目标均限
   rect 内防跨 mesh 泄漏）——marker 落岛屿间隙时得到近旁表面扩散的
   间接估计而非纯直射透传。
4. **密度阶梯补 26/24/20 档**（TestVulkanSponzaRenderGraph.cpp）：
   48/32 均因单 mesh 溢出（mesh_391@48、mesh_18@32），落点 16→**26**
   （pack_util 0.83，rect 合计 13.9M/16.7M）。
5. **UVDUMP=2**（LightmapModule）：页 A 通道 CPU 镜像（LoadLightmap
   填充）+ marker UV 双线性采样 → markerInVoid 统计（验收②度量）。

**坑位**：①洪泛首轮把"入队无效邻居"放在有效性 continue 之后 = 死代码，
BFS 退化成只填种子环 77K 纹素（页空区 30.48% 勉强贴线、floor mesh_389
rect 内仍 13.8% 空）——先入队再跳过无效源，修正后填 2.35M、rect 内空区
0/393 mesh；②烘焙期间 relink 同树二进制 = 运行中进程 SIGBUS（-25min），
长烘改用同目录副本 .s18 启动；③macOS hang watchdog 会杀长烘进程（主线程
同步烘 = 窗口挂起 50min；spindump hang 报告取证）——caffeinate -ims
包裹后稳定跑完；④"30 分钟 audit 跑完整个阶梯"比 20 分钟单密度烘快得多，
调参轮先 audit 后 bake。

### 18.3 验收（新页 density26，烘焙 51min/64spp/3bounce，纹素 9.35M）

| 验收项 | 结果 |
|---|---|
| ① A 构成 | void 74.66%→**16.96%**（A=1 55.73%/A=0.5 27.31%）；rect 内空区 **0/393** mesh。PASS |
| ② marker 落空区 | markerInVoid：z8 **0**/3686318、z4 **0**/3686101（双线性 A≤0.25）。PASS |
| ③ z=-8 mean>45 | 28.91 **逐位不变**（vs §17 页 MAD=0.000）——marker 全有效但可见走廊 E_ind≈0（深廊无光真值；§16 ④ mesh_389 p50=0 同证）；纯黑 0.000%。**目标未达且前提被否证**：残余差距（OFF 62.7）= 烘焙解析天空 vs Lumen 天空 HDR 单位差（ON avgLum 0.020 vs OFF 0.25-0.44，12×）——独立标定项（HDR env 采样入烘），非覆盖缺陷 |
| ④ avgLum 不贴下限 | z4 log2Lum 恒 -6.0 → mean **-5.64**（贴下限 1/286 采样，avgLum 0.0156→0.0202）；z8 仍贴（E=0 同因）。minLogLum(-6→-4) 评估：封顶 effExp 2.88→0.72，ON 各位姿现靠 2.0-2.9 曝光才匹配 OFF 观感（z4 72.5 vs OFF 64.1），上调即整体回退 ~1/3 亮度——**不动**，根因是 HDR 单位差非适应参数 |
| ⑤ 回归 | z4 70.96→72.53、z0 62.90→81.81、default 77.83→83.92（lapvar 219-1168 纹理保持）；OFF 双运行低频 MAD=**0.705**<3.5 地板（mean 64.14/64.05）；TestLightmapBaking ALL PASSED（15 项） |
| ⑥ 密度上探 | 16→**26 tex/m**（阶梯自然上探，如实报告）；烘焙 21→51min（纹素 4.22M→9.35M） |

### 18.4 证据与产物

- 交付页：/tmp/lm_e2e_bake/lightmap.lmap SHA16 **cc182af3425ef7a9** +
  lightmap_page.png；§17 页备份 /tmp/lm_void/lightmap_pre18.lmap
  （deff7de4b70e6c57）。
- 对比图：cmp_atlas18.png（图集 74.7%→17.0% 空）、cmp_z4_17_18_off.png
  （z4 §17/§18/OFF 三联）、cmp_z8_17_18.png（z8 逐位一致取证）。
- 工具：/tmp/lm_void/{atlas_a_stats.py,tone_stats18.py,capture_matrix18.sh,
  atlas_cmp18.py}；日志 {audit_old,audit_new,bake18b..d}.log + logs/
  {on_*,off_z4_*,uvdump_z*,tone_z*}.log + on_*.ppm/off_z4_*.ppm。

---

## §19 用户五观感问题 + 后处理逐级消融归因（2026-10-21）

机位 CAM_A=(0,1.7,0,0,0)（走道平视）/ CAM_UP=(0,1.7,0,22,0)（仰视廊顶），
frame 400 capture（PPM 1280x720）+ composite 输出 HDR 读回
（PRIMAL_LIGHTMAP_DUMPFRAME，2560x1440 RGBA16F，预 TAA）。基线页 =
§18 交付（26 tex/m，SHA16 cc182af3425ef7a9）。曝光日志
（PRIMAL_TONE_ADAPT_LOG）：log2Lum 恒 **-6.0（贴适应态下限）**，avgLum
0.015625，ubo_exposure 0.10，**effective_exposure=2.88**。

### 19.1 消融矩阵（同位姿同帧，A0 全默认为参照）

| 级 | 配置 | 深暗桶 B/R* | 深暗桶 lum | 直射区贴白% | 廊顶带 lum | meanL |
|---|---|---|---|---|---|---|
| 数据 | composite HDR（预 TAA） | **1.055** | — | —（HDR>3 占 0.89%） | — | 0.133 |
| A0 | 默认 | 1.015 | 42.66 | **54.36** | 62.97 | 79.06 |
| A1 | TAA_OFF | 0.996 | 40.50 | 52.95 | 61.45 | 77.16 |
| A2 | PRIMAL_BLOOM=1（默认本关） | =A0 | =A0 | 55.37 | 65.18 | 79.69 |
| A3 | PRIMAL_TONE_ADAPT=0 | 1.010 | 5.60 | **0.00** | 16.94 | 13.30 |
| A4 | PRIMAL_TONE_SAT=1 | 0.998 | =A0 | 54.42 | =A0 | 79.06 |
| A6 | LM OFF（Lumen 参考） | **0.804** | 56.23 | **0.01** | 95.95 | 64.08 |
| 仰视 | A7 LM ON vs A8 OFF | darkBR 1.05/0.81 | — | **4.24 / 0.00** | 108.3/96.1 | 71.3/67.8 |

*深暗桶 = A0 composite HDR luma∈(0.0005,0.05) 的固定像素集（684k/720p，
占帧 53%）——数据侧 B/R 1.055；桶内 B/R 在后处理各级均 ≈1.0，无放大。

### 19.2 六问题归因结论

- **① 暗间接区泛蓝 = 数据导致（后处理无关已证）**。深暗桶 B/R 数据侧
  1.055（天空 zenith B/R=1.09 血统——暗区间接光由天空主导），后处理
  各级 0.996-1.015 不放大反而微降；TAA/bloom/sat 合计 <±0.02。
  2.88× 自适应曝光的"贡献"是把 0.005 级蓝偏提到可见亮度（可见性放大，
  非色偏制造）。OFF 参考 0.804。
- **② 直射过曝 = 混合：数据（单位差触发）+ 自适应曝光（执行）**。
  LM 互斥态 avgLum 贴 -6 下限（§18 12× 天空单位差的直接后果）→
  effExp 2.88 → 直射区贴白 54.36%；A3 固定曝光即归 0.00%（执行者），
  但整帧回暗 6×（meanL 79→13）不可用——真修复在数据侧增益（19.3）。
  TAA/bloom/sat 对贴白贡献 ≤1.4%。
- **③ 廊顶异常亮 = 混合：物理路径数据（主导）+ 单位差曝光（执行）**。
  仰视机位 LM ON 廊顶 clip 4.24% vs OFF 0.00%；图集直读 E max=17.17
  = π×5.5（§17 天空积分自证），top-mesh meanE 6.5 = albedo_floor×E_floor
  ≈0.35×17——**地板 bounce 全半球贡献的物理正确量级**（廊顶法线朝下，
  其半球 ray 全部命中地板）。[§20.5 勘误：仰视顶带亮 texel skyShare
  0.884——亮源是拱上方开放区的天空直照而非地板 bounce；0.35×17
  的数字巧合不构成因果。]composite 后处理各级廊顶带 61-65 不变
  （A0-A4）——与后处理无关已证。**"地平以下天空"嫌疑否证**：SKYBELOW=0
  重烘 A/B 同位姿画面 |diff|=0.0009/255（图集统计 p75 2.62→0.50 有差异
  但变化 texel 在用户机位不可见——向下逃逸 ray 仅存在于开放边缘/背面）。
  执行侧与②同链：E 1× 单位 ×2.88 曝光 → 仰视廊顶贴白。GAIN=4 后
  仰视 clip 4.24%→3.71%（F2，曝光回落部分抵消增益抬升）——廊顶物理
  亮度本身保留，观感由 gain 平衡。
- **④ 移动突变黑面：未复现**。CAM_SCRIPT 慢移（200-238）/快转
  （240-298）/横移（300-358）序列逐帧 newBlack（L<4 且前帧>20）全部
  0.000%；静止帧 UVDIFF 全帧 maxRel=0（重绘与 GBuffer 逐位同面，
  z-eps 1e-3 收缩充分）；markerInVoid 0.0016%。四项现有嫌疑中深度
  失配无证据，指向用户特定位姿/WASD 速度或 TAA history 失效瞬态——
  待用户提供复现位姿后按 UVDUMP 移动窗取证。本轮不动 eps（放宽有
  遮挡错误风险且无失败证据）。
- **⑤ 间接区一格格 = 数据分辨率（26 tex/m 装箱上限），非采样器模式**。
  composite 采样器 Linear/Linear+Clamp（无 mip 可言——纹理单层）。
  FFT：间接 ROI 检出 26.7px/27px 强周期峰（谱峰/中位 199.8）= 4cm
  texel 网格投影。[§20.4 勘误：该 36px 峰在 OFF（无 lightmap）帧同样
  存在（ratio 193）——实为材质/几何周期，非 texel 网格；格子主体
  是 E 内容 texel 尺度真实变化，采样柔化上限 7%。]A9 blur A/B（图集 3x3 高斯低通变体）：35.3% 像素
  变化（有效），但几何周期仍主导频谱——格感=密度上限（§18 装箱
  26 tex/m 封顶），主修路径=8192 页或 baker 内建低通（工具已验证：
  /tmp/lm_pp/blur_lmap.py 免重烘生成变体）。
- **⑥ GAIN=0 阴影残光 = 直射链既有行为（83%）+ TAA（17%）+ bloom（0.3%）**。
  纯阴影集（C0 HDR 最暗 5%）：LDR 残光 mean 9.98；+bloom 10.01
  （+0.3%）；+TAA_OFF 8.31（-17%）；HDR 数据底 0.0005（B/R 0.93，
  deferred 直射链暗部 fill，lmMutex 门控清单已核：froxel/volume/SC/
  DDGI/WRC/SSGI 全关）。OFF 模式同区域 61.1（Lumen GI 正常照亮）——
  GAIN=0 残光 << OFF，验收达标；非 lightmap 引入（两态共用 deferred）。

### 19.3 修复与默认值建议

- **数据侧（已实施）**：`PRIMAL_LIGHTMAP_GAIN` 默认 **1.0→4.0**
  （LightmapModule.cpp，env 可覆盖回 1.0）——机制：E 抬 4× → avgLum
  离开 -6 下限 → effEff 回落 → 直射不过曝 + 暗区提亮。本质 = §18 12×
  单位差的运行时增益补偿（E 全量级由 baker 天空 1× 单位主导，
  max=π×5.5=17.2 自证）。实测（above 页 E0）：贴白 54.36%→0.00%、
  深暗桶 lum 42.7→56.6（OFF 参考 56.2）、darkBR 1.015→0.896
  （OFF 0.804）。交付页（below0）同增益 F3/G_final：贴白 0.49%、
  darkBR 1.101、mid 49.66。PRIMAL_TONE_EV=-1 亦可消过曝但暗区偏暗
  （36.2 vs 56.6），留作备选。
- **③ baker 侧**：PRIMAL_LIGHTMAP_SKYBELOW 旋钮落地（默认保持 §17
  行为；0=物理零）。A/B 已证画面等价（19.4）——不改默认；交付页用
  below0 版（图集更物理：虚假下半球分量移除，p75 2.62→0.50）。
- **后处理侧**：不动引擎 tonemap 常量（§18 已评估 minLogLum 不动）；
  bloom 保持默认关（A2 证明开启仅 +0.2% 贴白）；TAA_OFF/SAT 等
  消融旋钮本轮已接（主路径 TAA_OFF 与 RWCB 分支同语义）。

### 19.4 ③ SKYBELOW=0 重烘 A/B

重烘（4096/64spp/3bounce，density 阶梯 48/32 溢出落 26 同 §18；副本
二进制+caffeinate -ims）：图集统计 p75 2.62→**0.50**（中低档 E 大幅下移
——below-horizon 虚假分量集中在开放边缘 texel）、p95 14.9→13.5、
max 17.17 不变（全开天空不受影响，π×5.5 自证）、E B/R median
0.42→0.26（更暖：暖地平色移除后剩余 bounce 光主导）。**同位姿画面
A/B：|diff|=0.0009/255（近零）**——below-horizon 天空对用户机位可见
表面无贡献，③ 的 baker 嫌疑否证；廊顶 E=地板 bounce 物理路径。

**事故记录（磁盘纪律）**：above 备份与输出用硬链接后，SaveLightmap
Container 的 fopen("wb") 截断写穿透 inode——§18 原页（cc182af3425ef7a9）
物理丢失。恢复路径：SKYBELOW 不设重烘（~51-110min）即逐参数复现。
本轮全部 above 页渲染数据（A0-A9/C/E 系列）均在丢失前采集完毕。

### 19.5 证据与产物

- 工具：/tmp/lm_pp/{lmap_stats.py,metrics.py,cross_metrics.py,
  blur_lmap.py,run_matrix.sh,run_extra.sh,rebake.sh}
- 采集：/tmp/lm_pp/{A0..A9,B0_move,D0_fast,C0-C3,E0-E2}（PPM+HDR bin+日志）
- 曝光：[ToneAdapt] log2Lum=-6 avgLum=0.015625 effExp=2.88（A0 log）
- 终验：G_final（交付默认=below0 页+GAIN4）clip 0.49%/darkBR 1.101/
  mid 49.66；G_off 与 A6 双 OFF 低频 MAD **3.18<3.5 地板**（零影响达标）；
  TestLightmapBaking ALL PASSED。
- 对比图：/tmp/lm_pp/cmp_{ablation,uplook,blur}.png（A0 问题画面 vs
  消融级 vs E0 修复 vs OFF 参考；仰视 LM ON/OFF；blur A/B）。
- 交付页：/tmp/lm_e2e_bake/lightmap.lmap = below0 版（cp 自
  lightmap_below0.lmap）；测试探针机位复现用 SPGI_CAM 同 §19 头。

---

## §20 用户四问 + Lumen 必答项（2026-10-22 凌晨）

基线：e53585fa（§19 后）。取证工具 = baker 天空分量插桩
（PRIMAL_LIGHTMAP_SKYSPLIT=1：trace 内并行累 sky 分量页，同 RNG 同路径
零差分噪声；与主页同 filter/dilate/flood 管线）+ 泄漏审计
（PRIMAL_LIGHTMAP_MISSDUMP=AABB：首跳 miss 率+首条方向落盘）+
UVDUMP=3（uv_rt 全帧 bin）+ PRIMAL_LIGHTMAP_SPP/OUTDIR 快烘旋钮。
快烘页 16spp/26texel-density/4096/SKYBELOW=0（与交付页同 rect 布局，
实测逐条目一致）；烘 14min（64spp 同参数 51min）。

### 20.1 任务1：暗区间接为何天空主导——真可见天，非泄漏

- **天空贡献分解**（total/sky 两页差分，/tmp/lm_q4/sky_sharemap.png）：
  全场有效亮 texel（E>0.02）**91.2% skyShare>0.9**；屏幕深暗桶
  （composite luma∈(0.0005,0.05)，§19 同定义，q1_cama uvrt+DUMPFRAME
  映射回 texel）lit 组 **88.5% skyShare≥0.7（p50 0.989）**——暗区
  间接光确实几乎全部来自天空。
- **泄漏审计**（走道 AABB 66.4 万 texel 首跳 16spp）：mean miss 率
  6.6%；高 miss（>0.3）texel 的 miss 射线 clearance 反查（KDTree 三角
  中心最近距，沿途 0.2-25m 采样）：**真开放（>0.5m）2158 vs 贴缝
  （<0.2m）4 = 泄漏嫌疑 0.2%**。miss 方向 73% 向上、方位角峰指向
  **-z 走廊远端开口**（Sponza 无门无顶的开放建筑）。BVH 相交为
  Möller–Trumbore 无背面拒绝（双面）——"单面墙背面被穿过"不成立。
- **结论（修或解释）**：无几何/BVH/trace 侧 bug 可修。暗区泛蓝 =
  **真可见天的小立体角直照**（E 0.01-0.13 → 有效天空立体角仅
  0.1-1.5% 半球——远端开口/拱上方），物理正确。
- **暗桶 B/R 复测（重新定义后）**：全桶 0.903，构成分解——**80.5%
  桶像素 texel E≈0（深廊真低间接+chart 无效），纯直射底透传，色温
  由 deferred 暗部 fill 决定（B/R 0.968）**；lit 桶 92.2% 天空主导组
  composite B/R 0.594（暗材质×天空/弹后天空染色）、反弹组 0.383。
  §19"暗桶 B/R 1.055=zenith 血统"细化修正：泛蓝第一根源是 deferred
  fill 占比，非"暗区间接被天空染色"。

### 20.2 任务2（必答）：Lumen 暗区同样天空主导

OFF 模式（PRIMAL_LIGHTMAP=0）+ GI_LADDER_FRAME=380（FusionComposite
CSV，暗区 ROI 0.02-0.42×0.25-0.95，n=64512）：

| 量 | 值 |
|---|---|
| direct(lum) | 0.004（阴影直射≈0） |
| ddgi(lum) | mean 3.48 p50 3.13（B/R **0.999**） |
| spgi(lum) | mean 8.10 p50 7.89（B/R **1.000**） |
| 构成（能量归一） | direct 0% + **DDGI 30% + SPGI 70%** |
| albedo（桶内材质） | R 0.275 / B 0.130（暖红墙） |

- **定论：Lumen 暗区间接是天空主导，无"反弹主导"可言**——探针辐照度
  B/R 严格 1.0 中性（天空 EvaluateSkyBase 积分色；若是地板/墙反弹
  主导会被 albedo 染暖 B/R≈0.6），量级横跨天空均值 5.48 的 0.7-1.9
  倍带（DDGI 31% 像素落在天空档±1.5）。
- **OFF 画面暗桶 B/R 0.804 的"暖"来自材质 albedo 调制（R/B≈2.1），
  不是反弹暖光**——与 ON 泛蓝同源同桶不同色的原因是两系统对 albedo
  的乘法位置一致而间接光色不同（Lumen 探针中性 vs 我们暗桶 80% 走
  deferred fill）。
- **架构级解释**：DDGI 探针按体积网格放置（可在几何外/开放空间），
  SPGI 屏幕探针 trace 同样 miss 取天空——暗区像素从"看得见天的探针"
  插值取光，天然绕过"表面是否几何可见天"的约束。我们的 baker 从
  纹素表面发射（起点必在表面），几何正确时深廊 E≈0。**两个系统的
  暗区都由天空主导；差别仅在于天空能量到达暗区的路径（探针插值
  平滑 vs 表面可见性硬约束）。**

### 20.3 任务3：狮子死黑——真值低（物理），非 marker/chart 缺陷

- 定位：Lion_Albedo.png = 材质 sponza:Material__301 → mesh
  sponza:sponza_377/378（右墙 x∈[12.2,13.0] / 左墙 x∈[-14.3,-13.5]，
  y∈[0.8,2.65]，z≈-1）= LMAP mesh_385..388。
- chart 诊断（交付页直读，lion_probe.py）：A1 78-85%/A0.5 15-22%/
  **A0 = 0%（chart 无空区）**；E(lum) mean 0.035-0.046、p50 0.011-
  0.018、max 0.68-0.81（对照地板 2.66、全场 p90 8.84）；B/R 0.41-0.53
  （暖=反弹，看不见天——与 20.1 一致：侧墙拱间凹龛背阴）。
- 复现（CAM 0,1.7,-0.9,0,90 正对右墙狮子）：ON 中央最暗 40px 窗
  mean 48.1/**min 12**（近黑轮廓可辨）；OFF 同窗 mean 41.5/min 26。
  ON 比 OFF 更暗但两态都不是 0。
- **判别结论：E 真值低**（背阴位置直射 NEE=0 + 凸浮雕自遮挡 + 侧墙
  拱间凹龛无天空可见 → 间接仅剩微弱反弹 0.02 档）。非 §19 ④ 深度
  失败家族（A/markers 健康）、非 chart 空区（A0=0）。合成链正常：
  albedo×0.017/π×gain4 ≈ 0.007 HDR → 2.88 曝光 ×255 ≈ 5/255 + 
  deferred fill ≈ 画面 12/255——数字对账闭合。**物理正确，不修**；
  对比图 /tmp/lm_q4/cmp_lion_crops.png。

### 20.4 任务4：TAA 亚纹素抖动 + 4-tap——已实现，量化收益 ≤7%

- 实现：LightmapComposite.frag/.metal + LightmapModule push 常量
  8B→16B（filterMode+frameIndex）。PRIMAL_LIGHTMAP_FILTER：
  0=旧行为 / 1=亚纹素时域抖动（hash(pixel,frame)±0.5texel，TAA 时域
  累积=lightmap 时域超采样）/ 2=4-tap 足迹（±0.5texel 角采样，A>0.25
  加权）/ 3=组合。**默认 3**（env 可回退）。build_spv.sh 同命令部署。
- **A/B 量化（CAM_A frame400，交付页）**：间接区阶梯指标（|dx| mean/
  p99，墙暗 ROI）f0 6.73/68.0 → w3 6.24/64.0 = **仅降 7%**；FFT 周期
  段（15-60px）能量占比四档全同 4.3%。
- **§19 ⑤ FFT 归因纠正**：36.1px 强周期峰在 **OFF（无 lightmap）帧
  同 ROI 同样存在（ratio 192.9，地板 325）——它是材质/几何周期
  （墙砖贴图重复），不是 texel 网格**。纯间接贡献（GAIN4−GAIN0 差分）
  的周期族 29-58px 为斜视角 texel 投影渐变谱 + 拱廊光照真实结构周期，
  无全局单峰。**采样域柔化对"格子"的收益上限即此（E 内容在 texel
  尺度有真实变化：chart 边界/岛屿间隙/MC 噪声残差）——根治 =
  8192 页或 baker 内建低通（§19 blur 工具先例），超出本轮"不升
  分辨率"约束，如实交付**。
- jw2/w3 变体检：±0.25 足迹（f2/f3）几乎无效（3%），加宽到 ±0.5
  后 7%；jitter 使 28.4% 像素 >2/255 微变（静态噪声柔化）。

### 20.5 任务5：廊顶亮 = 天空直照主导（§19 ③ 归因修正）

仰视 CAM_UP=(0,1.7,0,22,0) 顶部带 marker→texel（921,226 texel）：
- 构成 mesh：拱 sponza_17（Arch）、砖墙 sponza_06/36、旗帜 Fabric——
  不是 ceiling 材质 mesh（sponza_08/19 E mean 仅 0.02-0.06）。
- 交付页 E(lum) mean 1.06、p90 3.51、max 17.13（=π×5.5 天空满开口
  自证）；亮 texel（E>0.5，26.9 万个）E mean 3.376。
- **skyShare（快烘 16spp 分解）：亮 texel 0.884（p50 0.969）——廊顶
  亮度 88% 来自天空直照**（Sponza 无顶开放，拱廊上沿/檐口直接见天），
  **12% 地板 bounce**。§19 ③ "top-mesh meanE 6.5 = albedo_floor×
  E_floor 地板 bounce 物理量级"的定性被数据推翻——6.5 档 E 的 texel
  是天空直照（skyShare 0.88）非地板反弹；数字巧合（0.35×17≈6）不
 构成因果。材质侧：ceiling albedo (0.41,0.33,0.20) 中亮不是亮源；
  出射 = alb×E/π×gain4，E 主导。OFF（Lumen）仰视同位置同样天空主导
  （§20.2 架构同因），A7/A8 差异 = 单位与增益链不同。

### 20.6 验收与回归

| 项 | 结果 |
|---|---|
| OFF 零影响（双运行，新 spv） | 低频(32×) MAD **0.027** < 3.5 PASS（max 29） |
| FILTER=0 vs §19 G_final | 低频 MAD **0.002**（max 2/255）——push 常量扩展零回归 |
| TestLightmapBaking | **Passed**（3.89s，baker sky/miss 插桩无破坏） |
| 快烘 vs 交付页 rect 布局 | 逐条目一致（MaxRects 确定性自证） |
| GLSL/Metal composite | build_spv.sh 命令编译通过；Metal 同步（±0.5 足迹/抖动同式） |

### 20.7 坑位（本轮新增）

1. **sky 页保存访问已 move 的 page**：`c.pages.push_back(std::move
   (page))` 后再读 `page.texels_rgba16f[i*4+3]` → SIGSEGV（KERN_INVALID
   at 0x6，crash 报告 BakeLightmaps 帧）——A 位按 rasterized/valid
   同式重算，禁止引用被 move 对象。
2. **烘焙与渲染采集并行互踩**：两次快烘在 collect 后死灭（无 crash
   报告、无 jetsam）——全屏 Vulkan 窗口 ×2 并行时主线程 displayLink
   回调链不稳定。长烘独占跑（第三次成功），采集与烘焙不并行。
3. **GLSL 分离纹理**：textureSize(texture2D) 需 GL_EXT 扩展 → 用
   combined sampler2D 的 textureSize；sampler2D(tex,sampler) 构造必须
   point of use（不能做函数参数传）。
4. 交付页保护：快烘必须 PRIMAL_LIGHTMAP_OUTDIR 指向别处（§19.4
   fopen("wb") 截断覆盖族——本轮 env 化落地）。

### 20.8 产物与清理

- 快烘取证页（16spp sky split）分析后已清（重建：PRIMAL_LIGHTMAP_
  BAKE=1 SPP=16 SKYSPLIT=1 SKYBELOW=0 OUTDIR=... MISSDUMP=-6,0,-13,6,5,3，
  ~14min）；missrays.txt.gz（60.8 万行审计证据）+ miss_audit_sel.npy
  保留于 /tmp/lm_q4/skybake/。
- 对比图：sky_sharemap.png（天空贡献热图）、cmp_lion_crops.png
  （狮子 ON/OFF）、/tmp/lm_q4/{q1_cama,q5_up,f0..f3,w2,w3,g0,off1,
  off2}/ref.ppm（A/B 帧证据，2.7MB/帧）。
- 工具：/tmp/lm_q4/{parse_model,lion_probe,sky_share,audit_miss,
  bucket_br,task2_lumen,task5_ceiling,fft_grid}.py + capture_q4.sh +
  ab_filter.sh。
- /tmp/lm_q4 保留 91MB（工具+证据帧+审计压缩件）；uvrt/composite
  bin（29MB/个×6）已清。
