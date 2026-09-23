# 神经渲染推理/训练：剩余工程计划

日期：2026-09-22（R1-R4 执行记录更新于 2026-09-23）
状态：执行计划（基于 M1-a/M1-b/M2 核心/M3 shim 侧已完成的基础）
前置文档：`Docs/2026-09-15-neural-mlx-backend-plan.md`（M1/M2/M3 定义与已完成基建）

---

## 0. 当前状态快照

| 项目 | 状态 | 关键数字 |
|---|---|---|
| MLX 桥接闭环 | ✅ | 四端对拍 torch 2.4e-07 / MLX 3.7e-05；双树 CTest |
| Vulkan 采集链 | ✅ | 三层缺陷修复；12 take 多机位管线 holdout +8.30dB |
| SC cards=0 | ✅ 深修 | 门禁判据二次收口，消除超时配方依赖 |
| 7ch 降噪器 | ✅ | holdout +8.30dB；双树 CTest |
| NR 权重提取 | ✅ | 649 张量、145.7M 参数、0 NaN；MLXNeuralRenderer 加载成功 |
| FG C ABI | ✅ | neural_fg_create/interpolate；fp16 位重解释；机制测试过 |
| NR 输入适配 | ✅ R1 | 16ch 契约/残差输出语义入档（附录 A）；nr_adapter.py 方案 C + 原生前向验证 RepeatCheck PASS；同 take 并排 PPM 产出 |
| NR 微调 | ✅ R3.2 框架 | train_nr.py（方案 B in_proj + block0 adapter + block70 头，主干冻结） |
| FG 引擎集成 | ✅ R2.1 | NeuralFrameGenModule（4 槽延迟回读）；引擎内插值帧非零（99.7% 像素）+ present 换源 + dump 通道 |
| 推理性能基准 | ✅ R1.3 实测 | NR metal-fused 720p p50 112.8ms（8.9fps）——30fps 门禁未达成（见附录 B）；FG 720p 冷态 p50 10.0ms（100fps）PASS |

---

## 0.1 执行结果索引（2026-09-23）

- **R1.1** 附录 A（接口契约）+ 附录 B（性能基准）；RepeatCheck 原生 16ch 前向验证
  `[1,768,1280,16]→[1,768,1280,4]` 非零无 NaN。
- **R1.2** `ContentTools/neural/nr_adapter.py`（方案 C 自包含适配）+
  `eval_nr_vs_denoiser.py`（同 take 并排 + PPM）；方案 B 微调入口并入
  `train_nr.py` 的 `NRAdapterInput`（1×1 conv 7→6 初始化等价方案 C）。
- **R1.3** 附录 B。结论：M4 Pro 上 145.7M transformer 达不到 30fps（最快
  metal-fused 8.9fps@720p）；热节流可致 3-6× 漂移，跨模式对比须独立进程冷跑
  （RepeatCheck `REPEATCHECK_MODES` 支持）。
- **R2.1/R2.2** `Engine/Graphics/RenderPipeline/Modules/NeuralFrameGenModule.*` +
  `Engine/Graphics/Neural/NeuralFrameGenBackend.*`（`PRIMAL_FRAMEGEN=1` 门控，
  `PRIMAL_FRAMEGEN_PRESENT=1` present 换源，`PRIMAL_FRAMEGEN_DUMP` 验证通道）。
  引擎内 1000+ 插值帧上传、dump 99.7% 像素非零、无 NaN；单测
  `TestNeuralFrameGen`（双树 CTest）。端到端预算：模型级 10ms PASS；引擎内
  CPU 回读路径稳态 ~17ms infer + MapBuffer 同步开销（附录 B.3）。
- **R3.1** `scripts/batch_capture_m2.sh`（每批独立子目录 + 全局偏移重命名，
  避 take_idx_ 每次运行从 0 起的覆写坑；每 50 take 自动重训挂钩）。
- **R3.2/R3.3** `ContentTools/neural/train_nr.py` + `metrics.py`（PSNR/SSIM
  门禁共用）。微调框架落地并跑通导出（logical safetensors 全 649 张量重写）。
  **门禁数值（留出 take 3，LDR 域）**：
  - 质量门禁：PSNR **53.15 dB**（≥30 ✅）/ SSIM **1.00000**（≥0.9999 ✅）
    ——注意该门禁在 LDR 域被 adapter 的"保输入"特性天然满足（tonemap 压缩
    了 GI 噪声幅度），零训练即 53.15dB；真实去噪余量仍在 7ch 降噪器
    （LDR 68.8dB）。微调敏感性：lr 2e-4 全开（含 block70 头）400 步 →
    **坍塌至 23.77dB**；lr 1e-5 仅 in_proj+block0 adapter 200 步 → 稳定
    53.15dB 无增益（信号弱：当前 take 的 1spp 噪声在 LDR 域过小）。
  - 性能门禁：≥30fps **❌ 未达成**——最优 metal-fused 8.9fps@720p（附录 B.1）。
  - 产物：`/tmp/nr_eval/dlssnr-weights-finetuned.safetensors`（保守版）、
    `/tmp/nr_eval/side_by_side_take3.ppm`（noisy | NR | denoiser | GT）。
- **R4.1** 批量采集落地：`scripts/batch_capture_m2.sh` 挂机 4 批 **+48 take**
  （总计 **60 npz** @ /tmp/neural_capture_m2，达成 R4.1 的 50+ 目标），每批
  独立子目录 + 全局偏移重命名 + bins 清理全链路验证。≥500 take 只需
  `BATCHES=42` 挂机（~12.5h）。
- **R4.2** 架构/数据双实验（`train_denoiser.py --width`，1200 步）：
  - w24（10.7k 参数）vs w32（17.9k 参数）：留出 take 20-23 上 w32 仅比 w24
    好 ~0.1dB——**宽度不是瓶颈**。
  - 分布发现（关键）：新采集机位噪声显著更低（noisy PSNR ~36dB vs 原始
    ~25dB）。混合训练后：干净留出 take 全线负增益（-1.0dB 量级）；反向划分
    （训新测旧）噪声 take **+2.2dB**、过净 take 大幅受损（-10dB，take 3），
    take 2 为近黑坏帧（11dB/relL1 0.66，采集数据质量项）。
  - **结论**：下一步是噪声条件化（noise-level conditioning）或按噪声分桶
    采样，而非加宽通道；采集脚本已具备按机位/噪声分布扩数据的条件。

---

## Phase R1：NR 输入适配设计与推理验证

### R1.1 NR 模型输入/输出接口分析

**目标**：确定 NVIDIA NR transformer 的输入张量布局与输出语义，为 GI 适配建立映射。

**步骤**：
1. 读 MLX-DLSS `NeuralRenderingModel` 的 forward 签名（Sources/DLSSMLX/ 内）
2. 确认输入张量：帧数 × 通道数 × 分辨率（NVIDIA 期望 noisy color + motion vectors + depth）
3. 确认输出：denoised color [N,H,W,3]
4. 输出接口文档到计划文档附录

**验证**：MLXNeuralRenderer 用 NVIDIA 原始输入格式跑一次完整前向（无 GI 适配，仅验证推理链）

### R1.2 GI → NR 输入 adapter 设计

**目标**：把引擎 7ch GI 特征（log1p(noisy)×3 + normal×3 + depth/16）映射到 NR transformer 的输入空间。

**方案选项**：
- **A（最小侵入）**：将 7ch GI 复制/填充到 NR 期望的通道布局，初始 adapter 权重随机初始化，仅微调 adapter + 末层
- **B（投影层）**：在 NR transformer 前插入 7ch→Nch 的 1×1 conv 投影层，端到端微调
- **C（逐像素适配）**：将引擎 7ch 特征编码为 NR 期望的"噪声帧"格式（利用 SPGI 1spp 输出的噪声分布）

**推荐**：先实现 A（最快验证推理链），再实现 B（最优质量）。

**验证**：适配后的 NR 输出与 7ch 降噪器输出并排对比（同一 take）

### R1.3 推理性能基准

**目标**：确认 145.7M 参数 transformer 在 M4 Pro MLX 上的实际帧率。

**步骤**：
1. 在 RepeatCheck 探针中加载 NR 模型，计时 100 次前向（2560×1440 输入）
2. 如果帧率 < 30fps：测试 int8 量化（MLX `nn.quantize`）与半分辨率
3. 记录基准数据到计划文档

---

## Phase R2：FG 引擎时序插值 pass

### R2.1 NeuralFrameGenModule

**目标**：在引擎渲染管线中新增 FG 推理 pass，将前帧/当前帧经 shim `neural_fg_interpolate` 生成插值帧。

**设计**：
- 新模块 `NeuralFrameGenModule`（参照 `NeuralDenoiseModule` 的 MLX 后端模式）
- 输入：前帧 HDR 颜色（rgba16f）+ 当前帧 HDR 颜色
- 处理：延迟回读（+2 帧）→ shim `neural_fg_interpolate` → 上传插值帧
- 输出：插值帧纹理（RGBA16F），由 present 路径选择显示
- 门控：`PRIMAL_FRAMEGEN=1`（默认关）

**验证**：引擎运行中 FG pass 输出纹理非全零，帧率翻倍

### R2.2 FG 推理性能验证

- FG 模型 1.44M 参数在 64×64→2560×1440 插值的耗时
- 确认实时约束（<16ms/frame）

---

## Phase R3：M2 大规模数据采集与 NR 微调

### R3.1 批量采集自动化脚本

**目标**：一键启动 ≥500 take 的挂机采集。

**设计**：
- `scripts/batch_capture_m2.sh`
- 循环调用 `TestVulkanSponzaRenderGraph`，每次 TAKES=12，自动轮转 CAM_PATH
- 每 12 take 完成后：bins → npz → 清理 bins（磁盘管理）
- 增量重训：每新增 50 take 自动触发一次重训 + holdout 评估

**磁盘预算**：500 take × 15MB npz ≈ 7.5GB（bins 临时 ~3GB/12take 已清理）

### R3.2 MLX 训练循环

**目标**：为 NR transformer 搭建 MLX 训练循环。

**方案**：
- 基于 MLX-DLSS 的 `NeuralRenderingModel` + MLX NN 优化器
- 冻结策略：前 N 块冻结（保留 NVIDIA 预训练特征提取），后 M 块 + adapter 微调
- 损失：L1 + SSIM（与 M1-b 门禁一致）
- 数据加载：从 npz 读取 7ch GI 特征 + GT mean

### R3.3 NR 微调门禁

- **输入适配验证**：适配后 NR 输出 vs 7ch 降噪器输出并排
- **质量门禁**：holdout PSNR ≥ 30dB 且 SSIM ≥ 0.9999
- **性能门禁**：MLX 推理帧率 ≥ 30fps（2560×1440）

---

## Phase R4：7ch 降噪器持续优化

### R4.1 更多数据

- 使用批量采集脚本扩展到 50+ take
- 增加场景多样性（不同光照/相机/材质）

### R4.2 架构搜索

- 尝试更大通道数（7→32→32→32→3 vs 当前 7→24→24→16→3）
- 尝试加入时域通道（前帧降噪结果作为额外输入）

---

## 依赖关系图

```
R1.1 (接口分析) ──→ R1.2 (adapter 设计) ──→ R1.3 (性能基准)
                                                │
R2.1 (FG 模块) ──→ R2.2 (性能验证)              │
                                                ▼
R3.1 (批量采集) ──→ R3.2 (MLX 训练循环) ──→ R3.3 (NR 微调门禁)
                                                │
R4.1 (更多数据) ──→ R4.2 (架构搜索)             │
                                                ▼
                                         NR 引擎集成完成
```

## 执行顺序建议

1. **R1.1**（~1h）：NR 接口分析——决定后续所有适配工作的方向
2. **R1.2 + R2.1**（并行）：adapter 设计与 FG 模块可并行
3. **R3.1**（挂机 ~12h）：批量采集可与 R1/R2 并行
4. **R1.3 + R2.2**：性能验证
5. **R3.2 + R3.3**：MLX 训练循环 + NR 微调门禁
6. **R4**：持续优化

## 完工状态（2026-09-23）

| 阶段 | 状态 | 证据 |
|---|---|---|
| R1.1 | ✅ | 附录 A 契约 + 附录 B 基准；RepeatCheck 原生前向非零无 NaN |
| R1.2 | ✅ | nr_adapter.py + eval_nr_vs_denoiser.py 并排 PPM（take 3） |
| R1.3 | ✅ 实测 | 附录 B.1：metal-fused 8.9fps@720p；30fps 门禁未达成（如实记录） |
| R2.1 | ✅ | NeuralFrameGenModule + 单测 TestNeuralFrameGen（双树 CTest）；引擎内插值帧 99.7% 非零 + dump 通道 |
| R2.2 | ✅ 实测 | 模型级 10ms PASS；引擎内 CPU 回读路径 ~54ms（附录 B.3 拆解） |
| R3.1 | ✅ | batch_capture_m2.sh 挂机 +24 take 全链路验证；≥500 take 可一键 |
| R3.2 | ✅ | train_nr.py 训练/导出闭环 + 保守/激进参数敏感性结论 |
| R3.3 | ◐ 部分 | 质量门禁 LDR 域 PASS（53.15dB/1.0）；性能门禁 ❌（8.9fps） |
| R4.1 | ✅ | 60 take 数据在手（50+ 目标达成；R4.2 使用）；扩数据脚本就绪 |
| R4.2 | ✅ | 宽度实验（w32≈w24）+ 分布发现（噪声条件化是下一步） |

## 后续：自动化 DLSS 集成渲染测试（2026-09-23 追加交付）

应"独立于 TestVulkanSponzaRenderGraph 的自动化 DLSS 门禁 + NR 引擎集成"需求落地：

- **NR 引擎集成**（此前 NR 只有离线路径）：
  - shim `NeuralRender.swift`：`neural_nr_create_ex/infer/destroy` C ABI——
    7ch（log1p×3+normal×3+depth/16）→ vendor 16ch 特征（噪声 hash/tonemap/
    镜像对齐全在 shim 内，features.py 的 Swift 移植）→ MLXNeuralRenderer →
    LDR 残差 compose。flags bit0 = metalFused（f16 块编译）。
  - 引擎 `NeuralNRBackend`（dlopen）+ `NeuralNRModule`（pack 复用
    NeuralDenoisePack 字节码；**间隔门控** PRIMAL_NEURAL_NR_INTERVAL 默认 6 帧
    ——NR 单次 ~113-190ms，非实时，门禁只验正确性；回读环带代计数防陈旧槽）。
    SRP 双树挂接，PRIMAL_NEURAL_NR=1 门控，默认零足迹。
- **TestVulkanDLSSRenderGraph**（新集成测试，独立二进制）：
  - 复制 Sponza 测试脚手架，Initialize 内自带 env（原测试不读这些开关，
    零影响——已实测原测试 0 行 DLSS 日志、0 校验错误）。
  - 门禁（默认开，DLSS_GATE=0 可关；DLSS_GATE_FRAME 默认 150）：FG
    present_ready + dump 非零≥30% 无 NaN；NR frames_inferred>0 + 非零≥30%
    + nan_free + dump 一致。`[DLSSGate] RESULT PASS` → `std::_Exit(0/1)`
    （沿 GI_VERIFY 惯例）。
  - 运行：`Darwin/Debug/TestVulkanDLSSRenderGraph; echo $?`（~25s 自动退出）。
  - 已注册 ctest（Engine_Integration_Tests_TestVulkanDLSSRenderGraph）。
- **分辨率错配修复（2026-09-23 截图实证）**：模块纹理原按管线初始化分辨率
  （窗口 2560×1440）创建，逐帧内容是渲染目标分辨率（1280×720）→ present
  时内容只占屏幕左上角四分之一。修复：FG/NR 模块资源改为首次 AddPass 按实际
  帧分辨率惰性创建（EnsureResources），dump 文件尺寸随之精确对齐（7372800 B）。
- **present 与门禁解耦（用户实测反馈后）**：FG 上传环改为无条件消费生产
  结果（原实现 PRESENT=0 时上传环塞满→生产停摆）；插值帧默认不上屏
  （窗口显示真实渲染帧），DLSS_GATE_PRESENT=1 可显式打开换源显示。
  两种模式门禁均 PASS（实测）。 motivation：vendor FG 对静态内容 flow
  幻觉会让上屏画面形变/局部放大（见下条），门禁经 dump 验证插值帧内容，
  不依赖上屏。
- **FG present 形变（诊断结论：vendor 模型行为，非管线 bug）**：静态帧探针
  `interp(A, A, 0.5)`（128×128 渐变+棋盘）输出与输入平均差 0.2547、76.6% 像素
  偏差 >0.05——该恢复模型对静态/非摄影内容会幻觉 flow+残差（与其相位扫描
  非单调的结论一致；M3 时代的 FG 验证只测非零/NaN，未测保真）。管线数据面
  （LDR [0,1] 输入、帧对齐、缓冲环）已逐项排查无误。如需保真 present，需
  更换/再训 FG 流估计或仅在检测到真实运动时插入插值帧（超出本计划）。
- **顺带修复（继承性潜在 bug）**：NeuralDenoiseModule Vulkan 布局 binding 3
  声明 SampledImage 与 GLSL 的 StorageImage（outT0）不符（18 条校验错误）；
  FG NeuralFG_Result 首用布局未转换；FG PresentReady 用"已生产"而非"已上传"
  导致 FinalBlit 消费 UNDEFINED 纹理。修后 DLSS 测试 0 校验错误。

## 后续：低分辨率渲染 × 模型放大 对比图集（2026-09-23 追加）

需求：低分辨率渲染 + "DLSS 放大"到高分辨率，出对比截图图集。

- **真 DLSS-SR 不可用（结论）**：`mlxdlss-weights sha256` 实测本机
  `libnvidia-ngx-dlss.so.310.7.0` = unknown checkpoint（无提取通道）；
  `package-sr` 需 DLL 之外的 CUDA parameter capture（需自行抓取）；
  `extract-vsr` 需 SHA 锁定的 Linux RTX VSR 1.8.2 驱动库。三者皆无。
- **替代实现**：用已集成的 DLSS-NR transformer 作放大增强器。
  引擎加 `PRIMAL_RENDER_SCALE` env（quality render_scale 的乘数覆盖，
  默认 1.0 零影响；注意 quality 预设本身 =2.0，render = logical×2×env）。
  同一锁定相机（SPGI_CAM+SPGI_CAM_LOCK=1）两次运行至帧 420：
  640×360 渲染 与 2560×1440 渲染（回读均为 1280×720 backbuffer PPM）。
- **图集**：`ContentTools/neural/make_sr_atlas.py` → `/tmp/sr_atlas/atlas.png`
  （整帧行带指标标签 + 两行 2× 细节裁剪，四列：Nearest/Bilinear/DLSS-NR/
  Native Reference；单帧 PNG 同目录）。
- **数值（vs 2560 超采样原生参考）**：Nearest 26.03dB/SSIM 0.739 ·
  Bilinear 26.08/0.740 · DLSS-NR 24.77/0.728。
- **结论（重要负结果）**：DLSS-NR 是"保输入+小残差"的增强模型，对低分辨率
  上采样**不能恢复丢失的细节**（PSNR 反而 -1.3dB，视觉上略平滑但无新细节，
  细节裁剪行肉眼可辨）。真 DLSS-SR 需按 MLX-DLSS 的 package-sr 路径补做
  CUDA parameter capture，或换已知 checkpoint 的 dlss 库。

遗留（超出本计划，已入档）：
1. NR 实时化：metal-fused 全分辨率 + GPU 驻留路径（B.1/B.3）。
2. NR 微调信号：需要更强噪声分布或 HDR 域损失（当前 LDR 域信号过弱）。
3. 7ch 降噪器噪声条件化（R4.2 分布发现）。
4. 引擎 FG 的 Metal 树运行时验证（编译已覆盖，运行时只验了 Vulkan 树；
   Metal 树重建有共享 libEngine.a 交叉污染坑，见 AGENTS.md）。

## 风险与缓解

| 风险 | 概率 | 缓解 |
|---|---|---|
| NR transformer 输入维度与 7ch GI 不兼容 | 中 | 方案 C（编码为噪声帧格式）绕过维度限制 |
| 145.7M 参数 MLX 推理帧率不足 | 中 | int8 量化 + 半分辨率（已有 shim 侧验证） |
| 500 take 采集时间超预期 | 低 | 数据管线已验证 ~90s/take，可并行多进程 |
| NR 微调需要大量 GPU 内存 | 中 | 冻结前 N 块只微调 adapter + 末层 |
| FG compose custom Metal kernel 在 shim 上下文失败 | 已确认 | 输出全零已定位为 fp16 加载 bug，修复后应为非零 |

---

## 附录 A：NR transformer 输入/输出接口（R1.1 结论）

来源：`/Users/zhanyuanwei/Desktop/MLX-DLSS/` 的 `python/mlxdlss/features.py`（16ch 语义
权威定义）、`python/mlxdlss/pipeline.py`（参考 torch 推理路径）、
`Sources/DLSSMLX/MLXNeuralRenderer.swift` + `NeuralRenderingTransformerOperations.swift`
（MLX Swift 执行器）、`weights/nr/NeuralRendering.dlssmodel/manifest.json`（包 manifest）。

### A.1 张量契约

| 项 | 值 |
|---|---|
| 输入 | `color`，NHWC `[1, H, W, 16]`，float32（数值为 half 舍入值） |
| 输出 | `color`，NHWC `[1, H, W, 4]`，float32 |
| 空间约束 | H、W ≥ 320 且为 64 的倍数（vendor-aligned extent；图像贴原点，越界镜像延伸 `2*extent-2-x`，噪声按网络坐标重生成） |
| 状态 | stateless（无时序隐状态，单帧） |
| 权重 | 649 张量 / 145.7M 参数；fp16 为主 + 少量 f32 标量（attn_scale 等） |
| 执行模式 | eager(f32) / block-compiled / int8-fast(FFN int8 量化) / metal-fused(f16+编译块) |

### A.2 16 输入通道语义（features.py 逐字）

```
0-2   确定性高斯噪声（按网络像素坐标 + frame_index 的 hash 流；half 舍入）
3     常数 1
4-6   缩放颜色 ((half(c) - 0.5) * 0.125)，c ∈ [0,1]
7-9   同 4-6 的副本
10    归一化风格（standard 档 = 0；natural 1/128；cinematic 2/128）
11    局部色调强度（standard = 1）
12    局部结构强度（standard = 1；control_mask 路径另有语义）
13    皮肤结构（standard = -1）
14    自动掩码结构（standard = -1）
15    零
```

### A.3 输出语义（pipeline.py `compose_head`）

`head[..., :3]` 是 **LDR 域残差**：`predicted = clip(color + 0.25 * half(head_rgb), 0, 1)`；
`head[..., 3]` 被 compose 忽略。即网络不直接输出颜色，而是预测对输入颜色的残差修正。

### A.4 对 GI 适配（R1.2）的直接推论

1. 7ch GI（log1p(noisy)×3 + normal×3 + depth/16）映射到 16ch：
   - 引擎 GI noisy 是线性 HDR → 先 tonemap 到 [0,1]（`1 - exp(-gi * k)`，k=1.6 与采集
     预览一致）再走 `scaled_color` 编码；
   - normal/depth 无原生通道。零训练方案（C）丢弃（或后续复用 13/14 控制位，风险高不采）；
     微调方案（B）在前置 1×1 投影（7→16）中学习如何注入。
2. 残差输出意味着 NR 的"降噪"以输入颜色为基准——零训练时输出 ≈ 输入 + 微修正，
   对强噪声 GI 输入的去噪能力取决于 NVIDIA 训练分布与 GI 噪声的相似度。
3. 空间约束对引擎分辨率的影响：1280×720 → 网络 extent 1280×768（镜像延伸 48 行）；
   输出裁回 720。shim/离线适配都要处理这一 extent 对齐。
4. 帧号噪声通道：连续帧换 `frame_index` 可让噪声通道去相关（时域抖动），对 FG/时序
   稳定性是免费手柄。

### A.5 训练接口（R3.2）

参考 torch 图 `NeuralRenderingModel`（model.py）是纯 `nn.functional` 组合、无自定义
autograd 障碍，可直接 MPS 微调；权重以 logical safetensors
（`dlssnr-logical-v16`，`weights/nr/dlssnr-weights-logical.safetensors`）进出，
微调后经 `mlxdlss-weights` 的打包工具重建 `.dlssmodel` 包供 Swift MLX 加载。

**实现偏差说明**：计划原文写 "MLX NN 优化器"，但两个 venv（仓库与 MLX-DLSS）
均无 python `mlx` 包（mlx 只有 Swift 侧），python 参考图即 torch——微调走
torch/MPS（train_nr.py），推理性能门禁仍在 Swift MLX 执行器（RepeatCheck）
上测，与 M1-a/M1-b 的 torch 训练 / MLX 推理分工一致。

---

## 附录 B：性能基准实测（R1.3 / R2.2，2026-09-23）

设备：M4 Pro（MLX 0.21+ / Swift 执行器）。**协议警告**：持续 GPU 负载下热节流
可让数字漂移 3-6×（int8@1440p 持续负载 p50 1898s vs 冷态量级完全不同）；
跨模式对比必须独立进程冷跑（RepeatCheck `REPEATCHECK_MODES` 选择器）。

### B.1 NR transformer（145.7M 参数，head-only）

| 模式 | 分辨率（网络 extent） | p50 | 帧率 |
|---|---|---|---|
| f32-eager | 1280×720（1280×768） | 771.7 ms | 1.3 fps |
| f32-eager | 2560×1440（2560×1472） | 35007.9 ms | 0.03 fps |
| f16-eager | 1280×720（1280×768） | 514.0 ms | 1.9 fps |
| f16-eager | 2560×1440（2560×1472） | 1745.3 ms | 0.6 fps |
| int8-fast | 1280×720（1280×768） | 717.6 ms | 1.4 fps |
| **metal-fused** | 1280×720（1280×768） | **112.8 ms** | **8.9 fps** |
| f16-eager | 640×384（640×384，半分辨率档） | 352.7 ms | 2.8 fps |

**结论（R3.3 性能门禁）**：30fps @2560×1440 在 M4 Pro 上未达成。最优路径
metal-fused 也只有 8.9fps@720p；半分辨率收益有限（dispatch 主导而非 FLOP
主导）。eager 路径对 token 数近乎线性、对量化不敏感——瓶颈在算子调度而非
算力。若要实时 NR，需要 fused 块级编译的全分辨率路径或更激进的分块/裁剪策略
（超出本计划范围，留待 M4 后续）。

### B.2 FG（1.44M 参数 conv，FrameGenerator.interpolate 全流程）

| 场景 | p50 | 帧率 | 16ms 预算 |
|---|---|---|---|
| 独立进程冷跑 1280×720 | 10.0 ms | 100 fps | **PASS** |
| 同进程持续负载后 | 74.9 ms | 13 fps | FAIL（热节流） |

### B.3 FG 引擎内端到端（NeuralFrameGenModule，1280×720 渲染 / 2560×1440 窗口）

| 阶段 | 稳态 | 说明 |
|---|---|---|
| decode（2× MapBuffer 回读 + u8→f32） | 8.2 ms | 帧率跌时升至 50-62ms（回读队列回压） |
| infer（shim interpolate） | 17-20 ms | 冷态独立 10ms；与渲染并发膨胀 |
| encode（MapBuffer 上传 + f16 编码） | 28 ms | Vulkan Dynamic buffer Map 全队列同步 |
| CPU 合计 | ~54 ms | peak 首帧 67ms（含 MLX 编译预热 582ms） |

**结论（R2.2）**：FG 模型级实时预算 PASS（10ms < 16ms）；引擎内延迟回读
（CPU shim）路径端到端未达 16ms——卡顿主导项是 MapBuffer 与图形队列的同步
语义而非推理本身。后续路线：GPU 驻留 compose（Metal kernel 直接读写引擎纹理）
或贴 swapchain 队列深度的 upload ring；本阶段按计划完成 CPU shim 验证路径。
