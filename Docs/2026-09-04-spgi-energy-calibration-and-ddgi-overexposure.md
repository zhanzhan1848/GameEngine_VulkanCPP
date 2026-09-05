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
