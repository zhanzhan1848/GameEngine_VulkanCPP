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

## 7. 集成协议

与 fluid 线（主树未提交改动）在 `FusionCompositeModule.cpp`、`StandardRenderPipeline.cpp` 相遇：
谁先合 dev，另一边 rebase 解冲突；rebase 后重跑零影响证明 + §6 验收，然后
`git worktree remove .worktrees/lightmap`。
