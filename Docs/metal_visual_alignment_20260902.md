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
