# SPGI 能量标定 → DDGI 过曝根因修复（2026-09-04）

## 结论

"SPGI 槽位能量超标"判断被数据推翻。过曝根因在 **DDGI 的 SC 注入路径**：
卡片投影权重未饱和 + L1/L2 钳位上限随未钳位 L0 膨胀，探针缓冲被写入
~1660 的 L1/L2 值（trace 尺度应为 ~1.5）。修复后 gather p50 从 80.9 回到
1.92，融合输出 mean 从 43.5（max 顶死 64 钳位）回到 0.99（max 3.49）。

## 诊断链（全部带安全前缀 METAL_DEVICE_WRAPPER_TYPE=1 MTL_SHADER_VALIDATION=1）

| 实验 | 观测 | 结论 |
|---|---|---|
| PRIMAL_IND_ASYNC 槽位采集 | DDGI 槽 38/48/128/261，SPGI 槽 0.18-0.41 | SPGI 槽不超标，反而比 DDGI 暗 |
| SPGI on/off 融合对比 | 完全一致 | 过曝与 SPGI 无关 |
| PRIMAL_NO_HEAP_POOL=1 | 数值逐位一致 | heap 池放置洗清 |
| PRIMAL_DDGI_PARITY=1 | p50=81.3 不变 | gather/update 数学洗清 |
| SC_INJECT_OFF=1 | **p50=1.02** | 锁定 SC 注入路径 |
| 逐波段 IRR_DUMP | L0≤6.4 正常，**L1≤1661** | 钳位被绕过 |

## 根因（两层叠加）

1. **CPU 卡片投影权重膨胀**（SCDDGIIntegrationModule.cpp:276）：
   `totalWeight = solidAngle × NdotL × 4π`，其中
   `solidAngle = area/(d²+1)` 在探针贴近卡片时不饱和（可超 4π），多卡
   叠加后原始投影 L0 达 ~1230（trace 工作点 ~1.2）。最差探针 7983 的
   l1Limit = 3×1230 = 3690，混入 0.45 权重后正好是实测 L1=1661。
2. **钳位上限随原始值推导**（DDGIUpdateIrradiance.metal 同病）：
   `l1Limit = |sh0|×3` 用未钳位的 sh0——prev-screen 反馈环（DDGITraceRays
   path 2，`radiance = prev + bounce + albedo×L_i_prev×0.70`）的辐射尖峰
   会同步抬高上限，钳位形同虚设。这是 497919cd"第七轮判决"遗留项。

3. **放大器**：TestModularPipeline 从 `EngineTest/shaders/Lumen/` 副本编译
   DDGI/SC 链 shader，全部 14 个副本陈旧（DDGITraceRays 差 519 行）——
   引擎侧修复从未进入测试的运行时。

## 修复

- `DDGIProbeIrradianceFromCards.metal`：L0 先过能量门
  （SC_L0_GATE=1.5，trace 工作点），l1/l2Limit 由门控后 L0 派生。
- `DDGIUpdateIrradiance.metal`：l1/l2Limit 硬封顶（min(3|sh0|,30) /
  min(2|sh0|,20)），反馈环尖峰不再穿透钳位。
- `EngineTest/shaders/Lumen/*.metal`：14 个副本同步为引擎版
  （include 改回 Lumen/ 前缀形式，补 SDFTraceCommon.metal 副本）。

## 修复后数据（tick 800+ 收敛态，SPGI_CAM="0,5,0,0,0" 锁机位）

| 数据面 | 修复前 | 修复后 |
|---|---|---|
| GIGather out p50 / max | 80.9 / 880 | 1.92 / 3.24 |
| 探针缓冲 maxAbs（全波段） | 1661（L1） | 1.95 |
| 融合输出 full-strip mean / max | 43.5 / 64(钳位) | 0.99 / 3.49 |
| DDGI 驱动光敏感度 | 无（自环饱和） | 恢复受控 |

SPGI 开启（PRIMAL_SPGI=1）全链：DDGI 槽 0.92-2.23，SPGI 槽 0.088-0.14
（置信度 0.78-0.84），融合输出与 SPGI-off 一致——T8.7/T8.8 门控正确。

## 遗留项

1. SPGI 自身亮度（~0.1）比 DDGI 尺度（~1.5）低一个量级——replaceW 门控
   下不影响融合，但 SPGI 单独替换区域偏暗。下一轮：与 Vulkan SPGI
   （TestVulkanSponzaRenderGraph）同机位对数，校 MC 分母/π 链。
2. CPU 卡片权重公式（4π 因子 + Ω 不饱和）未改——与 Vulkan 共享，改前需
   Vulkan 侧 A/B。当前由 shader 侧能量门兜底。
3. ddgi_light_color 代码默认 3.0 与注释"恢复 5.0"矛盾（TestModularPipeline
   :549-554），标定时需澄清基准档。
4. 诊断旋钮保留（默认行为不变）：PRIMAL_IND_ASYNC 位 128=SPGI 槽、
   PRIMAL_NO_HEAP_POOL、SC_MIRROR_OFF、PRIMAL_GATHER=DDGIGIGatherParity.metal。
5. "DDGI mean≈72 线性"出处为 44925912（白爆调查期的诊断读数），非健康带；
   健康带基准应以 gather_avg 0.3-0.5（Vulkan）+ 本轮 1.92（同机位）为准。

## 第二轮：跨平台对齐矩阵（2026-09-05）

Vulkan 树重建（Darwin/DebugVulkan，注意两树共 Darwin/Debug 输出目录，
切树先 rm libEngine.a），同场景同机位（SPGI_CAM="0,5,0,0,0" 锁定）对数：

| 轴 | Vulkan 参照 | Metal 现状 | 状态 |
|---|---|---|---|
| gather p50 / max | 0.52 / 12.6 | 1.96 / 3.48 | **~3.7× 亮，机制待查** |
| 融合输出 full-strip mean | 0.136 | 0.98 | 同比例差 |
| ddgi_light_color | 5.0 | 5.0（本轮已对齐，原 3.0） | ✅ 已消 |
| DDGI 探针间距 | 3.0（默认） | 2.0（014ce074 修接缝） | ⚠️ 有意分歧，未裁决 |
| DDGI 网格覆盖 | ±46.5 | ±31 | 随间距 |
| SC 制度 | 稀疏（4 卡/帧 dispatch） | 全量（374 卡/帧，budget=0） | ⚠️ 结构性分歧 |
| SPGI 探针网格 | 320×180 ds=8 | 160×90 | ⚠️ 分辨率差一倍 |
| SPGI 融合贡献（GI_ISOLATE 差分） | +4% | ~0%（槽位暗，门控） | 两平台都近隐身 |
| prev-screen/自环公式 | — | 逐行一致（path-2/3、L_i_prev 采样） | ✅ 第七轮遗留销账 |
| Vulkan SPGI 槽 readback | [SPGI_ASYNC] 全零 | 0.088-0.14 | ❌ Vulkan 侧 RG 解析待修 |

视觉验收：Metal DDGI/SPGI 截图均正常（Sponza 纹理/三旗/拱廊阴影完整，
无白爆；天井开口亮部为天空）。截图通道在 Vulkan 窗口后失效，Vulkan 画面未留档。

### 结论与下一步

1. prev-screen 反馈环 shader 级已对齐——3.7× 尺度差的嫌疑收敛到
   SC LightEval 世代（Vulkan 稀疏 vs Metal 全量）、UpdateIrradiance 投影、
   间距/覆盖。建议先统一间距（Vulkan 参照一行改 2.0 或裁决 3.0 为准）再对数。
2. SPGI 能量标定的前置：修 Vulkan 侧 SPGI 槽 readback（FusionCompositeModule
   的 mask-128 通道在 Vulkan 返回全零），否则无参照可对。
3. Metal gather p50 对 ddgi 驱动不敏感（3.0→5.0 仅 +2%）——SC-first 下
   探针场由卡片辐射主导，驱动档位只影响 SC 薄覆盖区。

## 第三轮：直射区过曝根因 + DeferredLighting 世代差（2026-09-05）

用户目视：直射光照区过曝；Vulkan 整体偏暗。裁决：间距以 Vulkan 3.0 为准
（Metal 2.0 覆盖移除），ddgi 驱动退回 3.0（5.0 加重直射区过曝，且 Vulkan
自身偏暗、数值对齐≠感知对齐）。

### 新发现：测试副本的 DeferredLighting 是独立分叉（545 行差）

- **直射过驱动**：副本的 `fragmentLighting_gpuDriven` 带
  `DIRECT_INTENSITY=3.125` 额外乘数（引擎版无）——GPU 驱动路径（测试
  Draws=0 即此路径）直射被放大 3 倍多。Metal PRIMARY 峰值 2.12 vs
  Vulkan 1.38 与此吻合。
- **引擎版缺 `fragmentBlitFXAA`**：FinalBlit FXAA（a92d7a6）只提交在测试
  副本，引擎从未收到——同步后测试启动即挂。已把 FXAA 移植进引擎版。
- **SceneData 结构错位**：引擎 shader 是 416B 新布局（字段重排 + time/
  ambientLight），`DeferredLightingModule.cpp` 的 C++ SceneData 还是 384B
  旧布局——viewPos/jitter/shadowMatrix 全部错位读取。已修 C++ 侧并对齐
  416B + static_assert。（ForwardSceneRenderer 的 SceneData 本就是新布局。）
- GBuffer 族副本是法线解码接口换代（UnpackNormal 加 normalZSign），
  需连带 CPU 侧一起核，未动。

### 回答"GI 区是否影响直射区"

合成结构（scene + indirect 相加）两平台一致且物理正确——被阳光照到的
面确实接收反弹光。问题在量：Metal 直射被 3.125 遗留乘数过度驱动、GI 又
比 Vulkan 强 ~4×，叠加后直射区爆掉。同步引擎版直射 shader（去掉乘数）
后直射区应显著回落；若读作"阴天"，则把 DIRECT_INTENSITY 旋钮以校准参数
身份加回引擎 gpuDriven 路径（一行），与 Vulkan frag 的同名旋钮对表。

### 双平台曝光现状（同机位数值）

| 数据面 | Vulkan | Metal（本轮修复后） |
|---|---|---|
| gather p50 | 0.52（用户：偏暗） | 1.96（用户：直射区过曝→直射 shader 修复后待复测） |
| PRIMARY 直射峰值 | 1.38 | 2.12（3.125 乘数所致，去掉后 ~0.7，待目视定标） |
| 探针场 L0 p50 | ~1.0 | ~1.07（**同尺度**） |
| 探针 L1/L2 | 高达 70（无钳位，SH9 带符号抵消→偏暗） | ≤2（能量门钳位） |

两平台 GI 反向不正常：Vulkan L1/L2 无界+带符号抵消→压暗；Metal 强钳位
+无符号评估→偏亮。下一轮标定的核心是把两侧 gather 的 L1 处理对到同一数学。

## 第四轮：探针数学对齐（2026-09-05 深夜）

裁决落地：间距以 Vulkan 3.0 为准（Metal 2.0 覆盖移除）；ddgi 驱动回 3.0。

### 已完成的双向修复

| 修复 | Vulkan 侧 | Metal 侧 |
|---|---|---|
| UpdateIrradiance L1/L2 上限硬封顶（30/20） | ✅ comp + spv 重生成 | ✅ 上轮 |
| 注入 shader L0 能量门（1.5） | ✅ comp + spv 重生成 | ✅ 上轮 |
| gather 可见性/法线加权（八面体深度+Chebyshev+burial 低频保底，45/55 混合） | 本来就有（route-A） | ✅ 本轮移植进 DDGIGIGather.metal（深度缓冲原已绑定未用） |

### 实测（同机位收敛态）

| | 修复前 | 修复后 |
|---|---|---|
| Vulkan gather p50 / 探针 L1 max | 0.52 / 70 | 0.34 / 2.55 |
| Metal gather p50 | 1.96 | 1.94（p10 0.37→0.23，漏光压掉） |

### 关键判定

1. **同场同数学对照**（Metal 场 + SH9 parity gather = 1.63 vs Metal 默认
   1.96）——路线差只有 1.2×，**剩余 ~5× 差在探针场的角向特征**：
   Metal 场 L1 与屏幕法线正相关，Vulkan 场 L1/L2 相互抵消。根源在
   trace/injection/temporal 的逐探针生成链，gather 层已对齐。
2. Metal 直射乘数（3.125）去除后整体亮度不变——**融合输出的亮度大头是
   GI（0.75）不是直射（0.23）**，直射链修复的收益要等 GI 标定后才能显现。
3. 下轮主任务 = DDGI 逐探针 parity bisect（PRIMAL_SPGI_PARITY 同款思路
   用于 DDGI trace→update→inject 链），辅以 Vulkan SPGI 槽 readback 修复。

## 第五轮：过曝真因 = legacy 直出无 tonemap，全后处理链转正（2026-09-05）

### 判决实验（窗口级抓帧 + 像素直方图，贴白率 = max(r,g,b)≥250 占比）

| 配置 | 贴白率 | LDR 均值 |
|---|---|---|
| Metal legacy（FinalBlit 直读 fusion HDR） | **27.5%** | 131 |
| Metal FULL_POST（TAA+Bloom+ACES） | **0.0%** | 67.4 |
| Vulkan legacy | 0.7% | 39.8 |
| Vulkan FULL_POST | 2.2% | 56.0 |

**过曝真因**：RWCB 路径默认 legacy——FinalBlit 直读 fusion 线性 HDR 显示，
无任何 tonemap。线性值 ≥1.0 全部贴白；Metal GI 场均值 ~1.0 → 27.5% 贴白。
Vulkan 场暗（均值 0.117）直出 → 用户判定"压得过暗"。两平台的过曝/偏暗
都是 raw 线性直出的显示伪像，非能量本身。

注：之前对 ToneMapping.metal 的曝光修改最初"无效"即因默认链不走该 pass
（tonemap 仅 PRIMAL_TONE_ACES=1 启用）；CAL_EXPOSURE=0.45 已同时放进
tonemap_fs 与 passthrough 两入口，当前校准组合 = FULL_POST + 0.45。

### 处置

- 全后处理链转正为默认（legacyPostRWCB 逻辑反转），PRIMAL_LEGACY_POST=1
  保留旧直出路径作逃生门。
- 2026-09-02 的"全链楔死 GPU"未再复现：绑定守卫（857db3ee）、围栏追踪、
  SceneData 416B 对齐等修复后，全链稳定运行 60s+90s×2（Frame 960+），
  零 validation 错误/断言。
- 转正后双平台曝光收敛：Metal 0.0%/67.4 vs Vulkan 2.2%/56.0（1.2×，
  原 3.3×/40×贴白差）。用户判定"Vulkan 偏暗"同步改善。

### 遗留

1. DDGI 逐探针 parity bisect（场角向特征 ~5× 差，见第四轮）。
2. Vulkan SPGI 槽 readback 全零修复。
3. CAL_EXPOSURE=0.45 为全链启用后的新基线；上游 parity bisect 完成后
   复测并考虑回归 1.0。
4. DIRECT_INTENSITY 直射定标待用户在 FULL_POST 观感下重判（此前判断
   基于无 tonemap 的 legacy 直出，参考系已变）。

## 第六轮：Vulkan SPGI denoise 深度绑定 bug 修复——SPGI 槽位跨平台对齐达成（2026-09-05）

### 排查链

1. Vulkan readback 全零嫌疑 → 加 resolved 句柄打印 → resolved=534 正确
   （非黑兜底）→ 拷贝仪器洗清。
2. readback 换 gather **raw 输出**对照 → 有真实数据（mean 0.115，置信度
   0.93）→ **filtered（denoise 输出）纹理是空的**。
3. denoise dispatch 正常（SPGI-MARK 有 denoise）→ 描述符层：SPGI pass
   本地 UpdateDescriptorSet 的类型分支**漏了 SampledDepthImage**——深度
   绑定（binding 2）不写 imageInfo → 描述符空 → 深度永远读 0 → 全像素
   走天空早退 → filtered 全零。Metal 走隐式描述符不受影响。

### 修复

- ScreenProbeGIPass.cpp UpdateDescriptorSet：补 SampledDepthImage 分支
  （对齐 SSAO/HZBPass 的正常模式：imageView=采样视图句柄）。
- 配套：ScreenProbeGIPass 增 GetFilteredOutputTexture() 访问器，
  FusionInputs 增 spgi_pass 指针——async readback 用持久物理纹理。

### SPGI 槽位跨平台对齐（同机位收敛态，双平台全后处理链）

| | Metal | Vulkan |
|---|---|---|
| SPGI 槽 mean | 0.088-0.14 | 0.116-0.126 |
| 置信度 a | 0.78-0.84 | 0.92-0.94 |

**±30% 内对齐——原始任务"SPGI 能量标定"结论：无超标，两平台本就同带。**
另：修复前 Vulkan fusion 的 SPGI 贡献实际为黑（denoise 全零），本修复让
Vulkan SPGI 首次真实进入 fusion。

### 遗留（更新）

1. DDGI 逐探针 parity bisect（场角向特征 ~5× 差，见第四轮）——主任务。
2. Metal 间歇性 fence 超时（独立问题，本轮复现 1/2 次，进程存活但部分
   dispatch 被跳过——定位仍开放）。
3. CAL_EXPOSURE=0.45 复测（上游对齐后回归 1.0）。
4. DIRECT_INTENSITY 直射定标（FULL_POST 观感下）。

## 第七轮：DIRECT_INTENSITY 双平台定标 2.5——直射阳光可见性恢复（2026-09-05）

### 用户报告与判决实验

用户：Vulkan 看不到直射光/被 SPGI 压制。SPGI on/off 像素差分 + 三帧
（Metal ACES / Vulkan ON / Vulkan OFF）目视对比判定：

- SPGI on/off 差分仅 +2.8 均值——**非 SPGI 压制**。
- **Metal 全链帧完全没有直射阳光**（纯环境光观感）：同步去掉测试副本的
  3.125 乘数后，GPU 驱动直射峰值 ~0.48 过不了 ACES 感知门槛。Vulkan
  有自己的 DIRECT_INTENSITY=2.5（shaders/Forward/DeferredLighting.frag:54）
  所以还有暖色阳光。
- legacy 直出时代"直射可见"实为贴白伪像——tonemap 转正后必须显式定标。

### 处置

Metal `DeferredLighting.metal` gpuDriven 路径加 `DIRECT_INTENSITY = 2.5f`
（与 Vulkan frag 同名旋钮同值，校准参数身份入档）。两平台同值对齐：
Metal 1.8% 贴白/73 均值，直射光斑+影子边界清晰恢复；Vulkan 2.2%/56。

### 下轮

DDGI 逐探针 parity bisect（第四轮 ~5× 场角向差）为主线；Metal fence
超时独立定位。

## 第八轮：SC 注入权重物理化归一化——"GI 平了"修复（2026-09-05）

### 用户报告

正对相机的一楼狮子与二楼近直射区墙的 GI 亮度相同——场空间对比丢失。
（连带问题：Metal GI 可见性是否考虑深度/遮挡——已移植 route-A 加权，
DEPTH_DUMP 证实深度图健康（mean 181 为天空逃逸 texel 主导），但受
注入平坦化主导，视觉不可辨。）

### 根因

注入能量平坦化两层叠加：
1. CPU 权重 ×4π + Ω 不饱和（原始 L0 膨胀 ~1000×）
2. shader 平顶门（SC_L0_GATE=1.5）为压能量削平全场 → 狮子与亮墙同值。

### 修复（物理化归一化，保留对比只约束能量）

- CPU（SCDDGIIntegrationModule，两平台共享）：辐射度投影本形
  C_j = Σ Ω·L·Y_j——去 ×4π，Ω 封顶 π，NdotL 保留。
- shader 门 1.5→4.0（保险背板），Vulkan spv 重生成。

### 效果（同机位区域亮度，ACES 后 LDR）

| 区域 | Metal | Vulkan |
|---|---|---|
| 右上亮墙（直射邻区） | 191 | 108 |
| 狮子区（一楼内部） | 121 | 64 |
| 左下拱暗区 | 21 | 73 |
| 亮墙/狮子对比 | 1.6× | 1.7× |
| 全图贴白 / 均值 | 3.5% / 92 | 5.6% / 63 |

探针场 |L1|/|L0|=0.40（物理健康），L0 动态范围恢复 9.5×。提交 b5bd33fb。

### 遗留

Metal 全图仍比 Vulkan 亮 ~1.5×、暗区更黑（21 vs 73——Vulkan 场角向
分布仍偏平）→ DDGI 逐探针 parity bisect 继续为主任务。

## 第九轮："GI 暗部移动"判决——dispatch skip 帧间丢失（2026-09-05）

### 判决链（锁机位三帧差分：52% 变化 / 26% 显著）

1. 探针场校验和逐帧 ±0.2%，无 3 帧周期 → **三缓冲假设洗清**。
2. TAA_OFF 抖动更大（57%）→ TAA 在阻尼，抖动在 TAA 上游。
3. PRIMARY 直射层逐帧序列稳定，但带周期性零帧 → **fence 超时后的
   dispatch skip**（每次超时后部分 pass 整帧丢失 = 暗部移动的感知）。
4. skip 与 FULL_POST 完全相关（legacy 1 次 vs 全链 4017 次/千帧）。

### 根因

9df09146（MemoryBarrier 总是结束 encoder，L2 可见性修复）与 857db3ee
（绑定守卫按 encoder 实例校验）组合：任何 "BindComputePipeline →
InsertBarrier → Dispatch" 模式的 pass，barrier 后的新 encoder 无绑定
记录 → Dispatch 被守卫静默跳过。全链新增大量带 barrier 的 compute pass
后，skip 从偶发变常态（~4 次/帧）。

### 修复

MetalCommandBuffer 缓存最后一次成功的 compute pipeline state 与
descriptor set 句柄；新 compute encoder 创建时自动重绑二者。守卫语义
保留（真未绑定的 Dispatch 仍跳过并报错）。

### 连带暴露与止损

- skip 曾掩盖 bloom blur 的 RG 瞬态 temp 槽位别名（解析到
  fusion_output，usage 0x05 缺 ShaderWrite → blurCS2D assert）。
  别名修复前 bloom 默认关闭，PRIMAL_BLOOM=1 显式启用。
- 修复后：skip/fence/assert 三零，锁机位残差 46.9%/24.3%（蓝通道、
  直射过渡区）= 探针场慢呼吸（prev-screen 反馈环增益 ~1）。

### 下轮

DDGI 逐探针 parity bisect：压探针场慢呼吸 + 场角向特征对齐
（Metal 全图仍亮 ~1.5×、暗区更黑 vs Vulkan 场偏平）。

## 第十轮：L1 角向特征量化 + blurCS2D 陈旧句柄取证（2026-09-05 深夜）

### 探针场角向特征（同机位，SCDDGI 探针 dump 扩展 L1mag）

| | Metal | Vulkan |
|---|---|---|
| L0lum p50 | 1.07 | 1.07 |
| L1mag p50 | （Metal 待测） | **0.0027** |
| L1mag p90 | （Metal 待测） | 0.17 |
| gather p50 | 1.94 | 0.31 |

**Vulkan 半数探针 L1≈0**（完全无方向性）——场平坦化的微观结构实证。
卡片辐射已一致（Metal 0.378 vs Vulkan 0.29-0.38），差异收敛到
trace/update 链的 L1 投影与稀疏更新制度。

### 新阻塞项：blurCS2D 陈旧句柄（取证完成，修复待下轮）

自动重绑让 blurCS2D 首次真正执行后暴露：写一张 usage=0x01（仅
ShaderResource）的全屏 1280×720 纹理；同句柄跨帧 usage 3→1 漂移
（句柄被分配器复用，blur 持有陈旧句柄）。validation 下 assert 中止，
无 validation 下 ~27 帧设备丢失（GPU 页 fault）。

已固化取证仪器：TEXBIND（slot/handle/usage/size）、BLUR_EXEC
（output 句柄）、MTLUSAGE（创建映射，已证实创建侧 usage=7 正确——
问题在绑定时的句柄生命周期）、RG_TEX（名字→句柄映射）、SHADOW_TEX。
blurCS2D 消费方排查：ShadowMap(mom=37/40 usage 7 ✓)、Bloom(已关)、
LumenSSAO(不用 BlurPass)——剩余候选在 RG 瞬态生命周期审查中定位。

## 第十二轮：blurCS2D 断言真因 = SSGI blur 写交换链（2026-09-05 收尾）

地面真值（TEXBIND/BLUR_USE 打印）：失败 dispatch 绑定 slot1 =
handle 199 = **1280×720 usage 0x01 = 交换链后备缓冲**（SetNativeTexture
注入，绕过 MetalTexture::Initialize——故此前创建侧打印全部正常）。

即：**SSGI blur（无 PRIMAL_SPGI 时 SSGI 活跃）的 RG 瞬态输出解析别名
到了 BackBuffer**，blur 直接写交换链 → validation assert / 设备丢失。
与 bloom temp 别名同类——RG 瞬态槽位复用冲突，两次暴露同一 RG 缺陷。

### 处置

- RHI 新增 QueryTextureUsage 虚接口（抽象基类默认 0，MetalDevice 实现
  MTLTextureUsage 查询；其余后端零成本）。
- BlurPass::Execute 加可写性守卫：output 缺 ShaderWrite → 跳过+告警
  一次。应用存活（曾 840 帧稳定），不再被 assert/设备丢失打断。

### 下轮

1. **RG 瞬态槽位别名根修**（Compile/分配阶段对生命周期重叠的瞬态
   纹理不复用物理槽；SSGI blur 输出与 BackBuffer 的别名是实例）。
2. 修后 SSGI blur 恢复（删 BlurPass 守卫或保留为保险）。
3. DDGI parity bisect 主线（Vulkan 半数探针 L1≈0 的稀疏更新制度差）。

## 第十一轮：bisect 中断与现状（2026-09-05 收尾）

Metal 测试当前被 blurCS2D 陈旧句柄 assert 阻塞（validation 下 frame ~20
中止；无 validation ~27 帧设备丢失）。已固化的定位证据：

- 同句柄 199 跨帧 usage 3→1 漂移（分配器复用，blur 持陈旧句柄）
- 纹理 1280×720（渲染分辨率），SR-only usage
- 创建侧 usage 正确的纹理（moments 37/40 usage 7）不受影响

**临时恢复手段**：`git checkout b5bd33fb -- . && 重建`可回到无 assert 的
状态（但保留 dispatch skip 的暗部漂移）；或下轮修复 RG 瞬态生命周期后
回到 HEAD。

### 待办（优先级序）

1. blurCS2D 陈旧句柄修复：审查 RG 瞬态纹理的分配/回收与 BlurPass
   描述符集缓存的交互（setPool_ 按 frameIndex 复用，句柄跨帧失效）。
2. 解锁后继续 DDGI parity bisect：Vulkan 半数探针 L1≈0 的成因
   （稀疏 update 制度 vs Metal 全量）与 gather L2 处理对齐。
3. CAL_EXPOSURE=0.45 复测回归；DIRECT_INTENSITY 2.5 双平台观感微调。


## 第十三轮：L1mag 仪器修正 + 跨平台角向特征精测（2026-09-05）

SCDDGI 探针 dump 的 L1mag 索引修正（b*3 → b*4，vec4 布局步进）——
此前双平台 L1mag 对比均基于错位读取，作废。

修正后同机位探针场对比：

| 量 | Metal | Vulkan |
|---|---|---|
| L0 p50 | 1.066 | 1.073（一致）|
| L1mag p50 | 0.172 | 0.164（**一致**）|
| **L1mag p90** | **1.39** | **0.33（4×）** |
| gather p50 | 2.60 | 0.28 |

结论修正：两平台探针场中位性质相同；差异集中在 p90 尾部——亮侧探针
的方向性能量（Metal 强 4×）。这正是 Metal 画面直射邻区 GI 有结构、
Vulkan 偏平的精确位置。Vulkan 亮侧探针 L1 富集 = 下轮主线
（SC 制度对齐或 trace 方向内容增强）。

待办：RG 瞬态槽位别名根修（SSGI blur 恢复）；CAL_EXPOSURE 复测；
DIRECT_INTENSITY 2.5 观感微调。


## 第十四轮：SC 制度对齐实验——负例与新靶子（2026-09-05）

Vulkan 测试对齐 Metal 的 SC 全量捕获（capture_budget=0，cdb2f671）：
LightEval 从 4 卡/帧（稀疏）变 374 卡/帧（全量）✓ 生效。

判决：探针 L1 尾部**不变**（p90 0.31 vs 0.33）——SC 稀疏/陈旧不是
Vulkan 探针 L1 匮乏的驱动，排除。

新靶子（定量）：全量捕获后 Vulkan 卡片辐射 p50 = 0.128 vs Metal
0.378——**LightEval/CardRadianceAvg 链的世代差使 Vulkan 卡片辐射暗
3×**。这同时解释 Vulkan 整体偏暗（卡片→探针→gather 全链按比例暗）。
下轮开题：SurfaceCacheLightEval.comp vs .metal 逐行 diff +
DDGICardRadianceAvg 对比，定位 3× 增益差的精确位置。


## 第十五轮：LightEval 公式一致——3× 差锁定在输入数据（2026-09-06）

SurfaceCacheLightEval.comp vs .metal 逐段对比：**核心求值公式完全一致**
（(direct + E_indirect)×albedo + 自反馈 0.70 + emissive，min 64；
SC_FEEDBACK 0.70；gatherProbeIrradiance SH9 三线性 + clamp 4.0；
NaN 净化与自反馈回退全部相同）。shader 层洗清。

3× 卡片辐射差（Vulkan 0.128 vs Metal 0.378）锁定在 **LightEval 的输入
数据**，候选按嫌疑序：
1. lights[] 上传（数量/颜色/衰减/可见性数据）
2. 探针场 L1 形状差的传导（Vulkan 0.18 vs 0.40 → E_indirect 的方向
   项弱 → 但 L0 同为 1.07，只解释部分）
3. albedo atlas 内容（平台渲染管线差异）
4. 阴影可见性（T3.3 directional visibility）

### 下轮

输入数据逐项 dump 对比（lights 数组直读、albedo 采样对比、阴影
可见性统计）→ 定位后决定对齐方向（Vulkan 抬升 or 共享公式统一）。


### 输入数据排查进展（同日续）

- lights[] 上传：两平台一致（单方向光，color=settings.lighting.light_color
  =3.0；RWCB 路径喂 ddgi_light_color——注意 Vulkan 测试该值为 5.0）。
- 阴影可见性链：shadow_visibility_buffer_ CPU 初始化 1.0（全亮），
  逐帧由 SurfaceCacheShadow（两平台各有 .metal/.comp）计算写入。
  **未验证项**：Vulkan 可见性 compute 的输出数值是否正常（若全 0 →
  Vulkan 卡片直射项归零 → 完全解释卡片暗 3×）——下轮第一项 dump。

### 下轮开题顺序

1. Dump Vulkan shadow_visibility_buffer_ 统计（均值/零值占比）vs Metal
   —— 若 Vulkan 全零 → 直射项归零实锤 → 查 SurfaceCacheShadow.comp。
2. L1 亮度尾差（0.33 vs 1.39）随直射项修复自然复测。
3. RG 瞬态槽位别名根修（SSGI blur 写交换链）。


### 同时代 A/B 终测（本轮）

| 量 | Metal | Vulkan |
|---|---|---|
| 卡片 lit / p50 | 332 / **0.344** | 266 / **0.128** |
| 探针 L0 p50 | 1.066 | 1.073 |
| 探针 L1 p50 / p90 | 0.172 / **1.45** | 0.164 / **0.33** |
| gather p50 | 2.60 | 0.28 |

卡片差 2.7× 为真（同代码时代）。已排除：shader 公式、lights、可见性、
SC 制度、探针 L0。剩余收敛点 = SC atlas 内容（E_indirect 方向形状 +
反馈平衡点 + 捕获 albedo/normal 内容）。定位需 Metal GPU capture 或
atlas 逐通道 dump，留待下轮专项。


## 第七轮补遗：UE Lumen 参考图对齐校准（2026-09-06）

用户提供 UE Lumen Sponza 参考图作为视觉标准（全图均值 42.8、深暗占比
59.4%、贴白 0.7%——暗调高对比风格）。七轮迭代后的 Metal 校准态：

| 量 | UE 参考 | Metal 校准后 |
|---|---|---|
| 全图均值 | 42.8 | **43.5 ✓** |
| 深暗占比 | 59.4% | 42.6% |
| 贴白率 | 0.7% | 0.0% |
| 阳光地面 | 70 | 95 |
| 暗拱填充 | 22 | 92 |

最终旋钮组合：CAL_EXPOSURE=0.42、DIRECT_INTENSITY=2.5（gpuDriven）、
天光底 0.085 档、AO 调制 mix(0.12,1.0,pow(ssao,1.2))、ddgiBase=0.5。
附带修复：引擎版 DeferredLighting.metal 曾被部分失败的脚本截断为
0 字节，已从 1a751b41 恢复并重放校准。

遗留：暗拱内部填充 92 vs UE 22——大尺度遮挡的 GI 填充（探针级遮挡
衰减缺失），需要探针可见性/遮挡门控的深层 DDGI 工作；非旋钮可及。