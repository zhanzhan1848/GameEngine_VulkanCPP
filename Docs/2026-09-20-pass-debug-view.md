# Pass Debug View — 视口实时 pass 可视化（2026-09-20）

编辑器视口内一键查看各渲染 pass 中间结果：GBuffer（albedo/normal/ORM/depth/
velocity）、几何 ID（visibility/meshlet/cluster/mesh）、SSAO、延迟光照、GI
间接光、SPGI、SC 光照 atlas；另有 WRC 探针球 overlay（world space radiance
probe）。UE Buffer Visualization 风格：选中即整屏实时显示，可继续旋转相机。

## 入口

| 入口 | 用法 |
|---|---|
| 编辑器工具栏 | VIEW 旁 **PASS** 下拉框（14 项）+ **WRC** 复选框（探针球 overlay） |
| CLI | `editor.pass-view {"view":"albedo"}`、`editor.pass-capture {"path":"/tmp/a.png","view":"meshlet"}` |
| env（headless/复现） | `PRIMAL_PASS_DEBUG=albedo`（启动旋钮，值同 CLI 名） |
| C ABI | `PipelineSetPassDebugView/GetPassDebugView/GetPassDebugViewCount/GetPassDebugViewName`、`PipelineSetWRCProbeDebug/GetWRCProbeDebug`（只增，`RenderPipelineAPI.cpp`） |

## 视图清单与解码语义

| 名 | 内容 | 解码 |
|---|---|---|
| off | 正常画面 | — |
| albedo | GBuffer albedo | 直读 + gamma |
| normal | GBuffer world normal | 直读（存储即 [0,1] 编码） |
| orm | AO/Roughness/Metallic | RGB 直读 |
| depth | 线性深度 | NDC→线性（near=0.1, far=1000），÷far 灰度 |
| velocity | 运动矢量 | RG∈[-0.5,0.5] → +0.5 |
| visibility | 可见性缓冲 triangle_id | `vis & 0xFFFFFF` 哈希伪彩色 |
| meshlet | meshlet_id | `vis >> 24` 哈希伪彩色 |
| cluster | cluster 序号 | cluster map 反查 `globalMeshletIndex==meshlet` → 条目序号哈希 |
| mesh | instance 序号 | 同上反查 → `instanceIndex` 哈希 |
| ssao | Lumen SSAO filter | R 通道灰度 |
| deferred | 延迟光照输出 | HDR → ACES tonemap |
| gi_indirect | Fusion 间接光（含 WRC/SPGI/DDGI 贡献） | HDR → ACES |
| spgi | Screen Probe GI 降噪后 | HDR irradiance → ×GI 曝光 → ACES（2026-09-20 改，直读会全白） |
| spgi_raw | SPGI 降噪前 gather 输出 | 同上；黑洞 = gather 无覆盖 |
| sc_atlas | SC 光照 atlas（Lo） | RGBA8 直读 |
| sc_albedo | SC albedo atlas | rgba8 → gamma |
| sc_normal | SC normal atlas | rg 八面体解码 → rgb |
| sc_depth | SC capture 深度（到卡平面轴向距离，主流 0-0.3m；-1=未初始化） | v<0 暗紫；v≥0 pow(v/2,0.4) 灰度（2026-09-20 修正——旧 0..16m 标尺把主流值压成近黑） |
| sc_emissive | SC emissive atlas | HDR → ×GI 曝光 → ACES（无自发光场景全黑属正常） |
| sc_shadow | SC 逐 texel 阴影可见性（buffer，binding 6） | 灰度，白=lit 黑=shadowed；Sponza 配方下 99.3% 纯 0/1（G2 PCSS 越界→SDF 硬阴影回退），PCSS 连续值仅在 shadow-map 输入有效时出现 |
| sc_indirect | SC radiosity 间接光（albedo×tile 入射） | HDR → ×GI 曝光×16 → ACES |
| sc_tile | SC tile 入射辐射（tile 域，A=validity） | 同 sc_indirect |
| sc_tile_aux | SC tile 兼容证据 | R/G=oct 编码，B=中心深度 0..16m |

GI 曝光 = `PRIMAL_PASS_DEBUG_GI_SCALE`（默认 0.05；仅影响调试显示）。
⚠️ `PassDebugView.frag.spv` 不在 build_spv.sh——改 frag 后必须手动
`glslangValidator --quiet -V -e main PassDebugView.frag -o PassDebugView.frag.spv`（Debug/ 目录），
否则旧 spv 静默生效（2026-09-20 踩坑：新视图全走旧 passthrough 分支）。

哈希函数与 Dawn `GPUDrivenDraw.wgsl::hash_id_to_color` 逐位一致（三端同色）。
cluster/mesh 视图反查未命中（map 与 vis 打包约定漂移时）退化为 meshlet 哈希
色并偏红提示。背景（深度≥1）统一深灰。

## 切换语义（重要）

- 非 off 视图**强制完整渲染图路径**：引擎内部 `SetEditorMode(false)`；视图
  激活期间 `SetEditorMode(true)` 被忽略（退出 Play 不会打回轻量前向）。
  off 恢复编辑器轻量前向。
- 目标 pass 本帧未产出（SSAO 关、GI 内容未就绪等）→ 整帧回退正常 FinalBlit，
  日志提示一次（`PassDebugView '...' target unavailable`），不报错。
- per-frame 切换，无管线重建。

## WRC 探针球 overlay

`PipelineSetWRCProbeDebug(1)` 后每帧在最终 LDR 上叠加 wrc_volumes_[0] 的
规则格探针球（GBuffer 深度遮挡、相机 4m 内隐藏、默认只画相机高度 ±2.5m
切片，`SPGI_PROBE_SLICE*` 同 DDGI）。着色 = 球面法线方向对 64 个 Fibonacci
方向 radiance 做 cos² 加权混合（`probe_sphere_wrc_fs` / `ProbeSphereWRC.frag`，
probe_data 布局见 `WRCTypes.h`）。state 位：invalid=暗灰、inGeometry=偏红。
前提：场景有 WRC volume（`PRIMAL_WRC_ENABLE` 路径），否则帧内静默跳过。

## 实现（快速导览）

- 枚举/名字表：`Engine/Graphics/RenderPipeline/PassDebugView.h`（单一事实源）
- 模块：`Engine/Graphics/RenderPipeline/Modules/PassDebugViewModule.*`
  （全屏三角 + 6 绑定：float 源 / sampler / UBO / 深度 / cluster map SSBO /
  uint 可见性缓冲 CombinedImageSampler；着色器自加载）
- 着色器：`EngineTest/shaders/PassDebugView.metal`（MSL 源直编）、
  `Engine/Graphics/Vulkan/shaders/Debug/PassDebugView.{vert,frag}.spv`
  （glslangValidator -V 重编，源与 spv 同目录提交）
- 接线：`StandardRenderPipeline::ResolvePassDebugViewSource`（ImportResource
  声明读 barrier）+ Step 11 链首分支（`BuildRenderGraph`）与 RWCB 镜像分支
- WRC overlay：`DebugProbeSphereModule` 扩展（Source=DDGI|WRC 管线族）+
  `StandardRenderPipeline` 两条路径的 `wrc_probe_debug_` 分支

## 验证记录（2026-09-20）

- 两棵树构建通过（Metal / DebugVulkan，注意两树共写 `Darwin/Debug/libEngine.a`，
  切换后先 `rm Darwin/Debug/libEngine.a`）
- `ctest -R Unit` 5/5；C# Cli 测试 57/57（schema 快照已重生成并提交）
- Vulkan 回归：`PRIMAL_PASS_DEBUG=albedo` 帧推进至 961+ 零 validation 错误；
  `cluster` 视图 1141 帧，binding 4/5（SSBO/uint 采样）零错误
- Metal RWCB 路径（TestModularPipeline）：`PRIMAL_PASS_DEBUG=albedo` 无 fault，
  **PNG/PPM 内容验证通过**（BGRA8 drawable 1280×720 与 RGBA16F 离屏 RT 均出图）
- 已知非本特性问题：基线运行即有 `AlbedoTextureArray` 层布局 validation 警告
  （材质数组管线，与本特性无关）；Sponza GI 测试在 MoltenVK 下偶发
  `kIOGPUCommandBufferCallbackErrorOutOfMemory`（重型 GI 常驻分配，间歇，重跑即过）

## ⚠️ 核心限制：编辑器场景实体未接入 RenderSceneSnapshot（既有缺口，非本功能 bug）

**现象**：编辑器视口切 pass 视图后，依赖场景几何的视图（albedo/normal/orm/
depth/velocity/visibility/meshlet/cluster/mesh）输出为空背景；deferred/
gi_indirect 仅显示天空/环境项。

**根因（日志实证）**：编辑器场景实体**从未注册进 RenderSceneSnapshot**——
`GPUDrivenDrawPipeline::UpdateGeometryData` 在编辑器 RG 路径下从未被有效调用
（快照 0 实例 → Nanite 空转）。这是编辑器同步链的既有缺口，
`EngineViewport3D.axaml.cs` 的 Run Render Surface 日志原文即承认：
"runtime 预览限制：编辑器同步链尚未接入 runtime 实体注册（RenderSceneSnapshot），
画面暂不含场景实体（仅环境/后处理）"。前向渲染路径直接读 RenderScene 不需要
该注册，所以编辑器平时看不出来。

**验证过的部分（引擎侧全部工作）**：同一 pass view 管线在测试壳
（TestModularPipeline，Sponza 显式注册场景）Metal BGRA8 drawable / RGBA16F
离屏双实验 + Vulkan 全部正确出图；编辑器内 14 视图切换不崩溃、输出链工作
（deferred/gi_indirect 渲染天空环境、ssao 全白、velocity 零速灰、depth 背景灰
——语义全部正确，只是没有场景几何可显示）。

**注册链已修（2026-09-20 深夜）**：`RegisterMeshEntity` 补挂 `cluster::create`
（快照收录门禁），`UnregisterMeshEntity` 补 `cluster::remove`。实测：快照 6 实例
全部收录（resource=ok、clusterCount 1/107/11/6/3/3、世界位置正确非 identity）、
`Building Global Buffers: Meshlets=196 / Vertices=25902 / Triangles=72582`、
**SC 卡片生成激活**（`[CardGen] cards/mesh 5-8=6`，此前 0）。Run Render Surface
预览同样受益于此修复。

**最后一环（未结，仅编辑器 Metal 进程）**：编辑器 RG 路径下 `[NaniteDraw]
draw_calls=1 clusters_rendered=0`——GBuffer 无像素（depth 停背景灰、normal 停
clear 色）。Metal validation 无断言 = 合法空输出；需 Xcode Metal GPU capture
对比编辑器 vs 测试壳的 culling dispatch 输入。**用户日常壳可用**（见下）。
**已排除**（A/B 实测）：遮挡剔除（`PRIMAL_OCCLUSION=0` 关掉仍黑）、相机矩阵
（camPos=(10.66,2.76,-4.76) 正常）、输入计数（6 实例/131 clusterRefs 完整进
dispatch）。剩余嫌疑：GPU dispatch 槽位/绑定层——注意 RWCB 路径 bufferIndex
（DLL 入口 s_rwcbFrame%3）与管线内部 frameCount_%3 的对齐。
诊断锚点已内置：`[SnapshotExtract]`（收录摘要含每实例 worldT）、`[NaniteDraw]`
（绘制统计）、`[NaniteGeo]`（快照实例数）、`[GPUCulling] inputs/camPos`（剔除
输入与相机矩阵）。 env：`PRIMAL_OCCLUSION=0` 关遮挡、`PRIMAL_CULL_FORCEPASS=1` 强制全通过（均已实测排除）。

## ✅ 2026-09-24 结案：编辑器黑屏全链路修复

**结论**：编辑器 pass 视图 GBuffer 零像素是三层问题叠加，全部修复。
normal/orm/depth/albedo 实测出实体（albedo 为默认白模）。

1. **过期 dylib 误诊（9-23 结论翻案）**：上一轮"RegisterDevice 未生效/编辑器不走工厂"
   是在 16:24 链接的旧 libEngineDLL.dylib 上测的——补丁 16:23 才进源码、17:23 只重编了
   libEngine.a 未重链 dylib（objdump 证实旧 CreateRHIDevice 无 RegisterDevice 调用）。
   重新链接后 `[GpuMeshRetry] gpu_mesh=ok`。**教训：改 Engine 源码后必须重链 EngineDLL
   并核对二进制（strings/反汇编）再下结论。**
2. **视锥约定 bug（本轮新根因）**：编辑器 MetalCamera 用 `createLookToLH`（+Z forward，
   CamDump 实测 view/proj 均为 LH 且配对自洽），而 `EngineTest/shaders/GPUCullingPipeline.metal`
   stage1 近/远平面判据写死 RH"-Z forward"→ 全部实例 viewz>0 被判相机背后全剔
   （`[CullProbe]` 实测 viewz=+8~+16）。测试壳用 `CreateLookAtMatrix`（-Z）不触发。
   **修复**：`CullingConstants.view_forward_sign`（由投影矩阵实测：视空间 (0,0,±1)
   投影深度∈[0,1] 者为前方；列主序取 P23=columns[3].z、P32=columns[2].w）+
   MSL stage1 以 `view_z = z*sign` 统一判据。注意 Metal 路径的 culling kernel 在
   `EngineTest/shaders/GPUCullingPipeline.metal`（LoadShaderBytecode 硬编码，运行时
   编译），`Engine/Graphics/Metal/shaders/GPUCulling.metal` 是另一份不走的拷贝。
3. **材质链（albedo 黑）三修**：①`ResourceAPI.cpp` 默认材质 `std::call_once` 把
   legacy gfx 就绪前的首败永久缓存（PRIMAL_OPEN_PROJECT 早于 viewport 初始化）→
   改成功才落缓存；②`GPUDrivenDrawPipeline` material data buffer 是 16B 全零 dummy
   （SetMaterialDataBuffer 全库零调用）→ PS tint 读 (0,0,0) → 改 48B"无纹理白模"
   默认记录；③cluster map 填充时 invalid materialID（`gpuMaterialId` 无写入点恒
   invalid）钳到 0。
4. **顺手修**：`SurfaceCachePass.cpp` LightEval dispatch 信息误用 Error 级每帧刷屏
   → Info + 一次性。

**新遗留**：
- `gpuMaterialId` 从未接线：真实贴图不进 Nanite 材质路径，编辑器实体以白模显示
  （pass 视图功能已可用，纹理接线另案）。
- 重 cook 的 corridor .mesh（新格式，2026-09-23 12:45 生成）被引擎 reader 拒收
  `invalid packed mesh body`（头部对比：新文件在 material 段后多写一次名字、
  少一个字段），fallback 产生矛盾几何（Meshlets=10 vs clusterRefs=13）→ 全退化。
  当前 `projects_test/TestProject_01/Content/Assets/` 已换回旧格式件
  （/tmp/mesh_backup），新格式件备份在 /tmp/mesh_newformat_backup，待对齐
  ContentTools 写出端与引擎读取端。
- 编辑器退出时偶发 `mutex lock failed: Invalid argument`（库卸载竞态，另案）。

## 2026-09-24 第二轮：ID 视图与内置 mesh（已构建部署，编辑器侧验证待用户拉起）

1. **visibility/meshlet/cluster/mesh 四视图全空根因**：GPUDrivenDrawPipeline 创建
   visibility 管线的三处 `if (isVulkan)` 门导致 Metal 上 per-frame
   visibility_cb/descriptor 恒 INVALID，Stage2 在 `visibility_cb==INVALID` 处
   **静默 return**（连 SKIP 日志都没有）→ visibility_buffer 永不写入。已拆门。
2. **VisibilityBuffer.metal（Metal 运行时编译那份）VS 假实现重写**：旧版无视
   compact_cluster_ids/cluster_map/instance_data；新 VS 镜像 GPUDrivenDraw.metal
   的 compact list → cluster_map → meshlet 顶点拉取 + 实例变换；meshlet_id 输出
   globalMeshletIndex（8bit 打包，>255 meshlet 会色块合并）。
3. **内置 mesh（procedural，无预烤 meshlet → MeshletSynthesis 运行时合成）渲染成
   细线的根因**：BuildGlobalBuffers 尺寸循环用 `get_rhi_mesh_asset(geometry_id)`
   直查（hierarchy ID 误当 rhi asset ID），填充循环却走 `get_rhi_mesh_id()` 两步
   解析——两 ID 空间不重合的几何尺寸/数据错位。两处已统一；RenderSceneSnapshot
   的 bounds fallback 同修。corridor 系纯属 ID 恰好重合才侥幸正确。
4. 环境注记：本机 GUI 会话一度拒绝新启动编辑器（Avalonia RenderTimer -6661，
   崩溃早于引擎代码），需从用户终端拉起验证。


## 2026-09-24 第三轮：plane 材质接线 + 晚注册实体缺失（已构建部署，待用户拉起验证）

1. **plane albedo 白模**：`gpuMaterialId` 遗留已接线。新增
   `GPUDrivenDrawPipeline::RegisterEditorMaterial`（1024²×16 RGBA8 编辑器数组 +
   BlitTexture 逐层拷贝 + 12-word MaterialData 记录；记录 0 = 白模默认，注册材质
   从 1 起）；`RHIDeviceBase::GetTextureSize` 非纯虚查询（Metal 覆盖）；
   `RegisterMeshEntity` 用 `content::get_rhi_texture_handle` 解析后注册并写回
   `meshInfo->gpuMaterialId`。纹理 >1024² 会被裁剪；材质图（SetEntityMaterialGraph）
   仍只影响前向渲染。
2. **cluster/mesh 视图部分 mesh 缺失**：全局几何缓冲只在首次构建，晚注册实体
   永久无 meshlet/cluster map/instance 数据。`UpdateGeometryData` 改为几何集签名
   （geometry_id+cluster_count 序列 hash）变化即重建（新增
   DestroyGlobalGeometryBuffers）。
3. 本机 GUI 会话拒绝从我侧启动（Avalonia -6661），验证需用户终端拉起。


## 2026-09-24 第四轮：ID 视图真机验证通过 + 编辑器材质接线

- ID 四视图（visibility/meshlet/cluster/mesh）真机出彩色块（127/31/31/5 unique）；
  晚注册 sphere 经几何集签名重建后正常渲染。
- 编辑器材质链落地：`RegisterEditorMaterial`（拷贝 PSO → RGBA8 层视图 +
  12-word MaterialData 记录）；`RHIDeviceBase::GetTextureSize`（Metal 覆盖）；
  `ForwardSceneRenderer::GetMeshInfoMutable`；`RegisterMeshEntity` 自动注册并
  回写 `gpuMaterialId`。Metal 三坑：材质缓冲须 Unknown(Shared)；描述符 UBO
  stageFlags 须含 Pixel；默认白模记录须并入重建用的 words 向量。
- plane albedo = 用户材质 Test01 的 Lion_Albedo（黑背景为主，视觉偏暗属正确）；
  corridor 未绑纹理 → 白模。
- 诊断文件：/tmp/primal_mat_words.txt、/tmp/primal_mat_diag.txt（引擎直写）。


## 2026-09-24 第五轮：全部视图真机验证通过

- meshlet/cluster 视图缺 triangle + 着色随相机角度变：visibility/copy PSO 默认
  `CullMode::Back` 把绕向不符的整批三角形剔掉（相机绕行时被剔集合翻转）→
  两处显式 `CullMode::None`。
- PassDebugView.metal ID 视图背景判定 `tri==0`→`packed==0`（0 号三角形不再被
  当背景丢弃）。
- 拷贝 draw 无片元：`Draw(3,1,0,0)` 参数序误用（第二参是 startVertex，
  instanceCount=0）→ `Draw(3,0,1,0)`。
- 材质层拷贝定稿：拷贝 PSO 采样源纹理渲染进 RGBA8 层视图（源拉伸铺满），
  normal/orm 缺省回退 0xFFFFFFFF。
- 终验（真机抓帧）：albedo 1140 色（plane=Lion 材质纹理、corridor=白模）、
  visibility 129 / meshlet 36 / cluster 36 / mesh 5、depth/normal/deferred 正常、
  单测 5/5。诊断文件 /tmp/primal_mat_words.txt、/tmp/primal_mat_diag.txt。

**2026-09-23 追加（gpu_mesh=null 根因链，历史记录）**：`[CullBounds]` 实锤编辑器 6 实例
全部 gpu_mesh=null → bounds 退化原点球 r=1 → 视锥剔除全拒。根因一（已修）：
`RHIDeviceManager::RegisterDevice` 全代码库零调用 → `get_rhi_gpu_mesh` 的
`GetDeviceCount()==0` 恒真短路 → RHIGpuMesh 永不创建。已在
`RHIDeviceFactory::CreateRHIDevice` 补 `RegisterDevice`。根因二（待查）：修复后
编辑器仍 gpu_mesh=null 且 `[GpuMeshDiag]` 无输出——设备创建可能不走工厂
（直构 MetalDevice 路径待查）。

## ✅ 用户日常壳接入（TestNaniteStreamingPipeline，2026-09-20 深夜验证）

`PRIMAL_PASS_DEBUG=<name>` 直接支持（TestNaniteStreamingPipeline.cpp FinalBlit
前的条件分支 + `PassDebugViewModule::DrawInline` 直绑路径）：壳内已有全部 pass
资源句柄（gbufferAlbedo/Normal、sceneDepthTexture_、ssao、primary deferred、
gi、spgi、scLighting），albedo/depth 视图截图验证通过（Sponza 石墙/挂毯 albedo、
走廊深度轮廓均正确）。壳内暂无独立 RT 的视图（velocity/orm/visibility/meshlet/
cluster/mesh）回退显示 deferred 画面——待壳内补 RT 后逐一接通。
顺带修复：`ForwardSceneRenderer::SceneData` 416→544B 对齐 P4.2（级联 2/3），
此前前向 deferred 光照 Metal validation 断言崩溃。
