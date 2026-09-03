# Metal↔Vulkan 视觉对齐——伪影定性与下一步（2026-09-02）

## 已修复（本轮提交 01925cc6..77511788）

1. **白爆**：DDGI gather 的 compute 写纹理对后续采样不可见 → Metal 默认
   ddgi 全幅 blit flush（FusionCompositeModule）。注：部分区域拷贝/仅声明
   RG 边均无效，必须全幅 blit 读。
2. **后处理链从未执行**：FinalBlit 在 AddPass 时预解析瞬态纹理物理句柄
   （分配发生在 Compile）恒 INVALID → 永久回退 fusion 原始帧，TAA/Bloom/
   ToneMapping 被整体裁剪（两平台同病）。修复：FinalBlit exec 时解析。
3. **GI 暖色温差**：Metal ddgi_trace_finalize 为 SC-first，但 SC LightEval
   每帧仅 ~8 张可见卡（393 注册），atlas 大部分黑 → 探针退化为解析白+蓝天。
   修复：对齐 Vulkan 层级 prev_frame_color 主导 + 能量门 isfinite 防御
  （屏幕颜色 NaN 使 dot 门恒 false，prev 路径静默旁路——路径可视化实证）。
4. **直射过曝**：fragmentLighting_gpuDriven 扔掉 ORM（roughness 硬编码
   0.5/metallic 0）。改为读取真实值；light 3.0（直射峰值 ~1.4 对齐 Vulkan
   实测 1.38）。
5. GI gather Metal 升全分辨率（半分辨率边缘深度/法线错位=块状锯齿）。
6. Vulkan 黑屏/崩溃：新启用的 Vulkan 后处理链存量 bug（Bloom 管线
   INVALID、ToneMapping render pass 附件畸形→validation
   AttachmentTracker 崩溃）——Vulkan 默认回退 legacy，基准观感已恢复。

## 遗留伪影定性（用户标注，A/B 排除记录）

1. **楔形加性亮斑**（左下暗墙，恒定位置）：三角形楔状、楼梯边缘。
   已排除：SSGI（SSGI_SILENCE=1 仍在）、阴影泄漏（翻转光照方向仍在）、
   SC atlas（已降为 fallback 仍在）。剩余候选：SSAO 半分辨率滤波、
   shadow visibility 半分辨率上采样、DDGI 探针穿墙泄漏（gather 无
   depth/normal 权重，两平台同式但内容对比度不同）。定位法：逐项加
   静音开关二分（SSAO 的 mute、shadow vis 直连、DDGI 场归零）。
2. **阴影边界楼梯**（中央竖条）：shadow map texel 台阶。对比 Vulkan
   同机位边界更柔——查两侧 ShadowMapModule 配置差（分辨率/PCF 核/
   cascade 覆盖）。
3. **几何边缘锯齿**：legacy 无 TAA。阻塞于后处理链启用即楔死 GPU 驱动
   （validation 已默认开，PRIMAL_FULL_POST=1 复现抓报告）。

## 工具入库

- PRIMAL_IND_ASYNC=<掩码>：无同步污染的多路条带统计（RGB 分量），
  兼任白爆 flush 的生产路径（Metal 默认 mask=2）。
- PRIMAL_LIGHT_DIR="x,y,z"：GI 光照响应 A/B（实测 GI 场随光翻转迁移）。
- SPGI_CAM + SPGI_CAM_LOCK：两平台同机位对比（标定前提）。
- [BLOOM/TONE-EXEC] abort stderr、RG 分配失败日志、BeginRenderPass
  附件缺失日志。

## 下一轮主线

1. 驱动楔死根因：PRIMAL_FULL_POST=1 + validation 报告 → 修非法用法 →
   全链安全启用 → TAA 消锯齿（伪影 3）→ 曝光标定。
2. Vulkan 后处理链逐环（Bloom 管线、ToneMapping 附件）。
3. 伪影 1/2 的二分定位（各加静音开关）。
4. GI 弹射幅度档位（FusionIndirect ddgiBase 0.7 与 Vulkan 7.5 直射的
   配比差异——待同机位定量）。

## 全链楔死审计（2026-09-02 第二轮，静态分析）

复现一次（PRIMAL_FULL_POST=1，validation 开）仍楔死 WindowServer——
说明非法用法通过 Metal API validation，是语义/资源竞争级问题，不是
API 误用。之后禁止再直接跑全链，先修后分环验证。

审计结论（pass 本体、编码器状态机、着色器循环均有界）：

1. **TAA dispatch→blit 零屏障（已修）**：compute storage 写 s_OutputTex
   后同 command buffer 内 BlitTexture 读，中间无任何屏障。Vulkan 是
   sync violation；Metal 靠隐式 hazard tracking 兜底——blit encoder
   切换时驱动介入，正是 validation 静默、驱动级 wedge 的形态。修复：
   dispatch 后插 outHandle UnorderedAccess→CopySource 显式屏障。
2. **ToneMapping render pass loadOp=Clear 无 clearValue（已修）**：
   Vulkan 附件畸形/AttachmentTracker 崩溃的确定嫌疑；Metal 未定义清屏。
3. **RG 屏障体系在 Metal 上形同虚设**（结构问题，未动）：RG Execute
   在 BeginRenderPass 前发屏障，此时 Metal currentEncoder_ 为空直接
   return；Render encoder 分支也无操作。同步完全依赖 encoder 边界隐式
   顺序 + 自动 hazard tracking。
4. RG 瞬态池判据严格（尺寸+格式+usage 全等、生命周期区间不重叠才复用、
   Imported 纹理已隔离），后处理链各 pass 的 Read/Write 声明齐全，
   静态构造不出同 pass 反馈环。
5. TAA currDepth（final_depth_texture_）usage 带 ShaderResource，合法；
   jitter 只改常量；各 MSL 循环有界。

新增诊断开关：TONE_OFF=1（同 TAA_OFF/BLOOM_OFF，FinalBlit 回退直读
TAA 解析帧）。分环验证顺序（每环短窗口）：TAA_OFF+BLOOM_OFF（仅
ToneMapping）→ +BLOOM → 全链。

## 全链修复落地（2026-09-02 第三轮，提交 bf468210 + 798bc172）

**Metal 全链已可用**（TAA/Bloom/ToneMapping 稳定运行，观感对齐 legacy，
TAA 深度重投影 active）。三个真根因：

1. **进程 assert（Texture Descriptor Validation, 0 尺寸/pixelFormat 0）**：
   FusionOutput/DeferredOutput 用 ImportResource 注册（Unknown 类型），
   ToneMapping/Bloom 的输出 desc 类型守卫失败 → 空 desc 创建瞬态。
   修复：ImportTexture 携带真实 desc + desc 解析失败回退 1x1 并打日志。
   **Vulkan Bloom 管线 INVALID 同根因**（brightDesc 空 → 管线 Unknown
   格式创建失败），修复后 [BLOOM-EXEC] ok。
2. **GPU 楔死（Bloom blur）**：同 compute encoder 内
   memoryBarrier(Textures) 在本代 AGX 不保证跨 dispatch texture 写-读
   排序（spindump：主线程 dispatch_semaphore 永久等待）。修复：
   InsertBarrier 遇 afterState==UnorderedAccess 在 compute encoder 上
   endCurrentEncoder（编码器边界自动 hazard tracking，严格更强）。
   BLUR_OFF=1 开关入库辅助定位。
3. **画面洗白**：Metal 上游 fragmentLighting/FusionComposite 已内联
   exposure+ACES+gamma（LDR），ToneMappingPass 再做一次 = 双重 tonemap。
   新增 tonemap_fs_passthrough（Metal 自动选用，仅 bloom 合成）；
   Vulkan/Dawn 保持 ACES（上游线性 HDR）。注意测试用的是
   EngineTest/shaders/DeferredLighting.metal 独立副本
   （fragmentBlitNoTonemap 透传），与 Engine/Graphics/Metal/shaders/Forward/
   的 fragmentBlit（带 tonemap）不是同一文件。

## Vulkan 全链现状（下一轮主线）

PRIMAL_FULL_POST=1 不再崩溃（此前 AttachmentTracker 崩溃/黑屏），
Bloom/TAA/Tone 全部执行，但**画面全黑**。validation 报告（40s 采样）：

- 大量 vkQueueSubmit layout 期望错位：期望 SHADER_READ_ONLY_OPTIMAL，
  实际 UNDEFINED / TRANSFER_DST / TRANSFER_SRC——导入纹理初始状态与
  RG InsertBarriers 的状态追踪不一致（RG resourceStates 每帧从
  Unknown 起、跨帧池化复用时初始转换在 Metal 是 no-op 掩盖了问题）。
- vkCmdPipelineBarrier oldLayout 与实际不符（GENERAL vs SHADER_READ_ONLY）。
- descriptor set 在 pending CB 上被改写（无 UPDATE_AFTER_BIND）——
  TAA/ToneMapping/Bloom 的 MAX_SETS=2/帧 × 3 帧池，set 三帧轮转但
  CB 可能仍 in-flight，需加大池或按 CB 绑定。
- SampledImage 写入 pImageInfo NULL（某处 INVALID 句柄直写）。
- descriptorType 错配（STORAGE_BUFFER 写到 SAMPLED_IMAGE binding 13）。
- push constant offset 非对齐（vkCreatePipelineLayout offset=2）。

优先序：layout 追踪（黑屏根因）→ descriptor set 池 → 杂项 VUID。

## Vulkan 黑屏修复（2026-09-02 第四轮，提交 497ef4a7）

**PRIMAL_FULL_POST=1 全链 90s 稳定，画面完整正确**（TAA/Bloom/ToneMapping
全链，[BLOOM-EXEC] ok、TAA 深度重投影 active）。真正的黑屏根因不在
layout 追踪（那些 VUID 与 legacy 同量级、预存于 GI pass），而是：

**tonemap_fs 的旧版全合成语义**：`color *= ao.r`（binding 3 AO）+
`color += ssgi * 0.4`（binding 4）。C++ 传的 1x1 AO dummy 只在
ENABLE_WEBGPU 分支填充白色——Vulkan 上从未写入，采样 0 → 整帧乘黑。
ToneMapping.spv 由 Dawn/shaders/ToneMapping.wgsl 经 naga 编译（多入口），
Metal MSL 无此合成所以 Metal 从未复现。

定位方法（可复用）：窗口级截图（Quartz CGWindowList + screencapture -l，
不抢前台）+ TONE_SOLID 红屏探针（红屏可达=写入/采样/Blit 路径通；真
draw 黑=fragment 内部乘黑）+ TONE_OFF 对照（FinalBlit 直读 TAA 正常）。

修复：WGSL 去除 AO/SSGI 合成（对齐 Metal；FusionIndirect 负责 AO/GI），
naga 重编 spv。注意 binding 3/4/5 的 DSL 与描述符写入保留——写未用绑定
无害，但未来若启用非 fusion 的 AO/SSGI 合成路径，dummy 必须全平台上传。

**遗留平台差异（下一轮标定项）**：Vulkan 上游 fusion 输出为线性 HDR，
全链 ToneMapping 做 ACES+gamma（T4.6.5 设计：ToneMapping 是唯一 HDR→LDR）；
Metal 上游已 LDR，全链走 passthrough。两平台全链的色调映射路径不同，
亮度观感差异需 SPGI_CAM+SPGI_CAM_LOCK 同机位定量后统一。

validation 预存问题（不阻塞全链，逐项清单见上轮）：GI pass push constant
无 range/offset 非对齐、descriptor set pending 改写（03047）、
STORAGE_BUFFER 误写 binding 13、CopyImageToBuffer 缺 TRANSFER_SRC usage、
depthTexture imageView 0x0（某 compute pass 未绑定）。

## 色调管线统一 + 伪影1二分开局（2026-09-02 第五轮，8be5a3e7 + 4a160e0d）

**同机位对比方法**：SPGI_CAM="0,5,0,0,0" + SPGI_CAM_LOCK=1 锁相机，
窗口级截图（Quartz CGWindowList 找窗口 id + screencapture -l<id>，
不抢前台、不受其他窗口遮挡影响；熄屏状态下截屏返回全黑——本方向发展
出的教训是尽量用进程内回读做数值对比）。

**关键事实厘清**：两平台 fusion 上游都输出线性 HDR
（fragmentLighting_gpuDriven 注释明示 "RGB = direct lighting (HDR
linear)"，fragmentFusion 无内联 tonemap）。legacy 基准观感 = 线性直通。
据此统一：ToneMapping 默认走透传（仅 bloom 合成），ACES+gamma 降级为
PRIMAL_TONE_ACES=1 可选路径。统一后两平台观感一致（直射阳光/暗部对比
恢复），提交 8be5a3e7。

**同机位剩余差异清单**（Metal vs Vulkan，全链、锁相机）：
1. 直射阳光覆盖不同：Metal 顶部大片阳光斜切区，Vulkan 直射区域明显更小
   ——疑两侧 ShadowMapModule cascade 覆盖/阴影过滤差异（与伪影2 同源）。
2. 伪影1 楔形亮斑：Metal 左中暗墙白色楔形条纹，Vulkan 无——Metal 特有。
3. Vulkan 整体略偏暖黄，Metal 偏中性——GI/IBL 色温差（待标定）。
4. TAA 柔和度接近，几何锯齿两侧均消除——伪影3 随 TAA 上线解决 ✓。

**伪影1 二分进展**：
- SSAO_SILENCE=1：亮斑仍在 → **SSAO 排除**。注意静音同时压暗整个 GI
  合成（fusion 把 ssao 当 GI 调制系数而非纯遮蔽），此开关只用于来源排除。
- DDGI_SILENCE=1：亮斑仍在 → **DDGI 排除**（场景整体变暗属预期，DDGI
  是主要间接光）。
- SPGI_SILENCE=1：亮斑仍在 → **SPGI 排除**。
- 加上此前已排除的 SSGI/阴影泄漏（光向翻转）/SC atlas fallback，
  GI 侧候选全部排除。**收敛：亮斑源自直接光照路径，归档里最后一个
  候选 shadow visibility 半分辨率上采样（ShadowFilter，half-res compute，
  ShadowMapModule.cpp:421）成为头号嫌疑**。
- 下一步实验：加 PRIMAL_SHADOW_WHITE=1（强制 shadow vis=1，无阴影）
  ——亮斑消失则坐实 shadow vis 路径，再做 vis 直连/上采样修正；
  亮斑仍在则直接查 DeferredLighting 的直射合成。

**回归教训（同日修复 581a25f0）**：加二分开关时把 RWCB 的
fusionIn.ddgi_tex 误置 INVALID（照抄 SSGI 模式），DDGI 的 tex/rg 双路径
都是活的——GI 整体退化为解析 sky fallback（用户实拍暗部无 GI）。
修复：DDGI_SILENCE 才同时置空 rg+tex，非静音恢复 giOut.gi_output_tex /
giOut.gi_specular_tex。教训：fusion 输入的 tex 字段不是死的回退槽，
置空前必须核对上游是否真实产出。

## 伪影1 坐实：VSM light bleeding（2026-09-02 第六轮，8d46109a）

阴影有双路径：`sceneData.shadowParams.x > 0.5` → **VSM moments+Chebyshev
（激活路径）**；否则半分辨率 R8 vis（GetShadowVisibility，注释明示
"half-res, bilinear upsample"）。

探针两连：
1. PRIMAL_SHADOW_WHITE=1（mode-0 vis 全白）→ **画面零变化**（差分
   0.64≈噪声）——mode-0 路径根本没在跑，第一刀打空。
2. EngineTest/shaders/DeferredLighting.metal 的 DEBUG_SHADOW_WHITE=1
   （VSM 分支强制 shadow=1）→ 阴影全消、**楔形亮斑同步消失**——
   伪影1 坐实 VSM moments 链：Chebyshev light bleeding（暗区渗光）
   + moments blur/上采样（楔形的楼梯状边缘）。

**同源推论**：伪影2（阴影边界楼梯）与两平台"直射阳光覆盖差异"
（Metal 渗光更多→阴影区偏亮）都是 VSM 链的平台差异，一次对齐解决三个。

**下一刀**：对比两侧 VSM 参数与实现——moments blit（D32→moments）的
半分辨率、BlurPass 的 radius/sigma（Metal=blurCS2D）、Chebyshev 的
minVariance/bleeding 校正系数。修复方向候选：降低 blur sigma、
edge-aware moments、提高 Chebyshev 衰减截断。

## 伪影1 根因修复（2026-09-02 第七轮，225314b0）

探针四连最终坐实（每步同机位截图差分）：
1. PRIMAL_SHADOW_WHITE（mode-0 R8 vis 全白）→ 零变化，该路径未激活；
2. DEBUG_SHADOW_VFLIP=1（V 翻转采样）→ 阴影图案大改但偏离 Vulkan
   基准 → moments 方向假设否掉（原始方向正确）；
3. blurCS2D 对齐 Vulkan 5-tap（σ1.0 真高斯 → 0.227/0.316/0.070，含
   WGSL textureLoad 越界零语义）→ 变化甚微，blur 非主因（但作为
   跨平台一致性修复保留）；
4. **PRIMAL_SHADOW_CULLNONE=1（moments raster CullMode::Back→None）→
   楔形亮斑全部消失，阴影本体不变——根因坐实**。

**根因**：moments raster（GPU-culled meshlet indirect draw）用
CullMode::Back；meshlet 缠绕方向不一致（光投影 det=-1，代码库 3728/
4073 已有同类先例注释）时剔错面 → 被剔三角形区域保持 moments
ClearValue (1,1) = "全亮 z=1" → Chebyshev 的 `sc.z <= moments.x` 直接
判 lit → 暗区三角形楔状亮斑。修复：moments pipeline cullMode 默认 None
（VSM moments 捕获双面是标准做法）。

**修复后两平台观感**（SPGI_CAM="0,5,0,0,0" 锁相机）：楔形亮斑消失、
阴影结构一致。残余差异（标定项，量级小）：Metal 左上阳光覆盖略大、
Vulkan 整体略偏暖（GI/IBL 色温）。

**诊断开关全表**：PRIMAL_SHADOW_WHITE / DEBUG_SHADOW_WHITE（VSM 强制
shadow=1）/ DEBUG_SHADOW_VFLIP / PRIMAL_SHADOW_CULLNONE（已转正为默认）/
TONE_OFF / TAA_OFF / BLOOM_OFF / BLUR_OFF / SSGI·SSAO·DDGI·SPGI_SILENCE /
PRIMAL_TONE_ACES / SHADOW_BLUR。

## "逐渐变暗"定量：GI 探针收敛，非失控（2026-09-03）

用户观察 cull 修复后场景逐渐变暗。锁相机时间序列定量（窗口内容区均值）：

- 密集前段：t=2-4s 暗（37/33/38，SC/探针未就绪）→ **t≈7s 亮峰**
  （78/47/122，GI 探针以"解析白+蓝天"fallback 初始化）→ t=12s 起回落
  （67/39/112）→ ~30s 进入平台。
- 长程（第二组）：t=10/60/120/200s = 80.2/80.9/80.2/80.3——**完全平坦，
  无持续衰减**。
- 平台水平对照：修复前基线同时间戳 ≈71，修复后 ≈78-80（5-tap blur 阴影
  边缘略软）；Vulkan 基准 ≈51（Metal 仍整体偏亮，属残余标定项）。

**结论**：变暗 = GI 探针从亮 fallback 初始化向"正确阴影下的真实解"收敛
（楔子曾给场景注入假光，修复后平衡点真实但更显眼）。非 bug、非失控。
可选改善（若收敛闪光碍眼）：探针初始化改暗中性色（从暗往亮收敛比从亮
往暗观感好），或前 N 帧提高探针混合速率加速收敛。

## GI 冻结真相（2026-09-03，3e97d0ea）：探针在更新，但辐照≈0

用户观察"GI 区和第一秒没区别（等 100s）"。GI_VERIFY_FRAME 环境变量化 +
探针 dump 升级为相机邻域 2×2×2（原 dump 读网格角落 probe 0..7，在按距离
优先级的调度下被永久饿死——ageScore 封顶 0.3 竞争不过近距 ~1.0，读数
恒=种子 (1.05,1.05,1.23)，误导了两轮排查）。

相机邻域实测（frame 120，gc=16,9,16）：
- 半数探针 L0=(0.03,0.03,0.035)——在更新但辐照近黑；
- 半数 L0=(0,0,1)、L1=0——射线全 miss，纯蓝天。

两个挖掘点（下一轮）：
1. **prev-screen 重投影采样返回暗**：finalize 的 prev 主导路径按 hitPos
   重投影到上一帧合成图取色，Metal 上疑似 UV/矩阵约定错位（采样到
   黑/边界），radiance≈0.03；
2. **室内射线全 miss 率过高**：封闭 Sponza 内部探针的射线应大量命中
   墙体，(0,0,1) 纯蓝天占比 ~50% 说明 SDF trace 穿墙（GlobalSDF 体素
   内容/级联覆盖可疑）。
工具：GI_VERIFY_FRAME=<帧>（dump 时机）+ 相机邻域 L0/L1 dump 已入库；
DDGI_DUMP=2（DDGI_PATH_DEBUG 编译的 finalize，ray 路径聚合）在
TestVulkanSponzaRenderGraph，Metal 侧可移植。

## SC-first 回迁 + 冻结挖掘点定位到 update pass（2026-09-03 续，3ca98663）

用户方向确认："逻辑应参考 Vulkan，条件都已具备"。逐行核对
DDGITraceFinalize.comp 后：

1. **Vulkan 真实层级 = SC-first**（能量门 → prev+材质弹射 → 解析项；
   miss 探针中心搜卡带门 + 暗灰天光 (0.3,0.3,0.35)）。0c2e34d9 把 Metal
   改成 prev-first 的理由（"Vulkan 以 prev 为主"）与参考实现不符。
2. **SC 覆盖差异实证**：Vulkan LightEval 全量 374 卡/帧；Metal 因 T1.4
   页预算 + governor FPS 缩放长期只有 2-8 卡/帧 → atlas 长期欠点亮 →
   当年 SC-first 采到黑 → 被误判为"SC 路径坏"。
   新增 PRIMAL_SC_BUDGET=<页>（0=全量），实测 Metal 374 卡/帧达成。
3. Engine 副本 finalize 回迁 SC-first；EngineTest 副本（旧版 378 行纯 SC
   无门）整体替换为完整层级版并补齐 4 个缺失绑定。

**未决（下一轮第一件事）**：全量预算 + SC-first 下，相机邻域探针 L0 仍
逐位恒为 (0.03,0.03,0.035)=DDGI_SKY_COLOR×0.1 与 (0,0,1)——说明写探针
的 `ddgi_update_irradiance`（DDGIUpdateIrradiance.metal）没有消费新
finalize 的 ray_buffer，或其自身有 (0,0,1) 硬编码 sky/绑定缺失。审计点：
update pass 的 Metal 描述符绑定 vs 派发参数、ray_buffer 槽位索引、
miss/sky 分支的常量。

## GI 冻结根因坐实：SDF trace miss 率 97%（2026-09-03，05cd5db3）

DDGI_PATH_DEBUG 移植到 Metal（宏注入 + finalize 路径标签 + GIFusionVerify
ray_buffer 直方图读回）。相机邻域 8 探针全部射线实测：

  **miss=995 SC=10 prev=0 analytic=19（miss 率 97%）**

GI 冻结的完整因果链：SDF trace 穿墙（室内射线不命中）→ 97% 射线按 miss
填充（暗灰天光×距离权重）→ 探针场 = 死灰平光 + 无方向细节 → gather 插值
在探针格边界产生棋盘格亮条 → 用户观察的"GI 无变化 + 棋盘亮条"。
（update kernel 索引经核对为绝对探针号，与 finalize 一致，排除。）

**下一轮主线：GlobalSDF 在 Metal 上的体素化内容/绑定/覆盖**——
线索：日志 `IsVoxelizationReady: vox_pipe=true provider=null
provider_ready=no`（provider 为空可疑）；对比 Vulkan DDGITraceSDF.comp
与其 Metal 对应实现的 trace 步长/级联选择/SDF 纹理绑定。
诊断工具已入库：DDGI_PATH_DEBUG=1 + GI_VERIFY_FRAME + ray-path 直方图。
