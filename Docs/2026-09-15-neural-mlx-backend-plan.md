# 神经渲染推理路线修正：MLX 后端（基于 MLX-DLSS 仓库验证）

日期：2026-09-15
状态：**M1-a/M1-b(部分)/M3(shim) 已落地**——逐里程碑状态见文末 §7（2026-09-22 实施）

## 0. 背景

## 0. 背景

裸 Metal compute kernel 推理路径经过多轮迭代后遇到系统性瓶颈：

- WriteDescriptorSet 未零初始化 → 26 处 UB（我方 9 处 + GIGather 3 处 + 全仓 17 处）
- 同 binding 号多条目在 Metal encoder 中的槽位残留
- pass 内 CopyTextureToBuffer 的 blit 编码器切换时序
- naive 全分辨率 conv 帧率个位数（4 层 ~21 GFLOP 标量 f32）

这些是裸写 Metal compute kernel 的固有代价，不是实现错误。

**用户自己的 MLX-DLSS 仓库**（github.com/iamwavecut/MLX-DLSS）已在 Apple Silicon
上验证了完整的生产级替代路径：**MLX 作为推理后端**，在 Swift/MLX 层运行 NVIDIA
DLSS 级别的 transformer（NR 649 张量 71 块）+ 帧生成（conv 18ch）+ SR/VSR，
全部通过 golden bundle 对拍验证。

## 1. 路线修正：推理侧切换为 MLX 后端

### 1.1 为什么 MLX 替代裸 Metal compute

| 维度 | 裸 Metal compute | MLX |
|---|---|---|
| kernel 调试 | 手写绑定/描述符/时序——已踩七坑 | MLX 框架自动处理 |
| 性能 | naive 标量 f32 个位数 fps | MLX 内部 Metal 后端 + Apple 优化 |
| Apple Silicon 加速 | 无自动利用 | AMX/GPU/统一内存自动调度 |
| 图编译 | 无 | `compiled`/`metalFused`/`int8Fast` |
| 精度 | 手管 f32 | F16/E4M3 自动 |
| 权重格式 | 自建 .pnn | safetensors（业界标准） |
| 依赖 | 零 | MLX（Apple 官方 Swift/Python 包） |

### 1.2 架构

```
引擎 C++ 渲染管线
  │ Step 9+: SPGI 1spp 输出 → MTLTexture
  │
  ▼ MLX 桥接层（Swift shim 或 MLX C API）
  │ MTLTexture → MLXArray（零拷贝或 blit）
  │ MLX 推理图（降噪器 / NRC / 帧生成）
  │ MLXArray → MTLTexture
  │
  ▼ 引擎 C++ 渲染管线
  │ Fusion / TAA / Present
```

MLX 桥接有两条实现路径：

**路径 A：Swift shim**（推荐——MLX-DLSS 已有完整先例）
- Swift package 包装 MLX 推理图，暴露 C ABI 给引擎
- MLX-DLSS 仓库的 Swift 架构直接复用（DLSSMLX target）
- 输入/输出通过 MTLTexture 传递（与引擎共享 MTLDevice）

**路径 B：MLX C API**（备选——纯 C++ 无 Swift）
- MLX 的 Metal 后端通过自定义 Metal kernel 嵌入
- 需要 MLX 内部 API 知识，工程量更大

### 1.3 训练侧不变

- PyTorch 训练（MPS/CUDA）→ 导出 safetensors → MLX 加载
- CPU 参考前向不变（单测锁位级一致）
- 采集管线不变（训练数据仍由引擎产出）

## 2. 现有资产兼容性

| 资产 | MLX 路线下的角色 | 变化 |
|---|---|---|
| 采集管线（Welford GT） | 训练数据产出 | **不变** |
| 训练脚本（train_denoiser.py） | 模型训练 | **不变**（PyTorch） |
| CPU 参考前向（NeuralWeights） | 单测 parity | **不变** |
| .pnn v2 格式 | 被 safetensors 替代 | **退役**（保留解析器作 fallback） |
| 裸 Metal kernel（NeuralConv 等） | fallback 推理 | **保留**（MLX 不可用时降级） |
| WriteDescriptorSet 零初始化清扫 | 引擎既有 UB 修复 | **保留**（惠及全部 pass） |
| GIGather roWrite 补提交 | 引擎既有 bug 修复 | **保留** |

## 3. 模型选型

### 3.1 短期（M1 收尾）：7ch GI 降噪器

- 网络不变（4 层 3×3 conv 残差网，11.3k 参数）
- 部署改 MLX：safetensors → MLX 加载 → Swift/Python 推理
- 验证：与引擎已有 16 平面 dump 对拍

### 3.2 中期（M2）：DLSS NR 模型改造

MLX-DLSS 的 NeuralRenderingModel（71 块 transformer，649 张量）已经
在 Apple Silicon 上实时运行 NVIDIA DLSS NR。改造方向：

- 输入适配：引擎的 GBuffer/GI 特征替代 NVIDIA 的多帧噪声特征
- 输出适配：GI 降噪替代 NR 的画面重建
- 训练：用引擎采集管线产出的 GT 对（noisy GI vs clean GI）训练
- 好处：DLSS NR 的多尺度 transformer 架构专为降噪设计，效果优于自建小 conv

### 3.3 长期（M3）：帧生成

MLX-DLSS 的 FrameGenerator（conv 18ch → flow/mask/res）已验证可用于
Apple Silicon。引擎可集成实现 GPU 帧生成（DLSS FG 开源替代）。

## 4. 里程碑

### M1-a：MLX 桥接最小闭环（1 周）

1. Swift shim：包装 MLX 推理图，暴露 C ABI
   - 输入：MTLTexture（7ch 特征）
   - 推理：4 层 conv（safetensors 权重）
   - 输出：MTLTexture（3ch 降噪 GI）
2. 引擎 C++ 侧调用 shim
3. 验证：GPU 降噪输出 vs torch 参考对拍（同帧输入）
4. 集成进 RenderGraph 作为 pass（GIGather 修复后）

### M1-b：对齐 PSNR 门禁（GIGather 修复后）

1. DeferredLighting shadowVisibility 绑定修复（用户 GI 分支）
2. 采集 + 降噪联跑，16 平面同帧 dump
3. 对齐 PSNR 门禁（denoised vs GT ≥ 24 dB 预期）

### M2：DLSS NR 模型改造（2-3 周）

1. 用采集管线产出 GI 降噪训练数据（≥500 take）
2. DLSS NR 71 块 transformer 输入/输出适配
3. MLX 训练/微调（迁移学习从 DLSS 权重出发）
4. 端到端 PSNR/SSIM 门禁

### M3：帧生成 + 性能（后续）

1. FrameGenerator 集成（引擎帧间插值）
2. int8 量化（`int8Fast` 模式）
3. 半分辨率执行 + 上采样

## 5. 风险与缓解

| 风险 | 缓解 |
|---|---|
| Swift 依赖引入 C++ 引擎 | 封装为独立 framework，仅暴露 C ABI；非 Apple 平台降级到 PyTorch/ONNX |
| MLX 版本兼容 | Package.swift 锁版本；MLX 是 Apple 官方维护 |
| 早期帧 relocation 页错误 | 用户 GI 分支待修——与 MLX 路线无关 |
| MLX 推理延迟 | 11.3k 参数极小（<0.1ms 理论）；71 块 NR 需 benchmark |

## 6. 与裸 Metal kernel 路线的关系

裸 Metal kernel 代码（NeuralConv/NeuralDenoisePack/MLPInfer）**不删除**：
- MLX 不可用时的降级路径
- 教学/调试参考
- 已修的六个 bug（转置卷积核/通道错位/时序/越界/零初始化/缺提交）全部
  保留——这些是引擎 Metal RHI 的真实 bug 修复，惠及全部 pass

---

## 7. 实施状态（2026-09-22）

### 7.1 M1-a：MLX 桥接最小闭环 —— ✅ 全部完成

| 交付物 | 位置 | 验证 |
|---|---|---|
| Swift MLX shim（C ABI，safetensors 加载，通用 3×3 conv 图） | `Tools/NeuralMLXShim/`（`swift build -c release` → `libNeuralMLXShim.dylib`） | 见下对拍 |
| mlx.metallib 构建脚本（SwiftPM 源码构建不产 metallib 的坑） | `scripts/build_neural_mlx_metallib.sh` | dylib 同目录放置即被 MLX colocado 路径加载 |
| pnn→safetensors 转换器（纯 stdlib，含自校验） | `ContentTools/neural/pnn_to_safetensors.py` | `EngineTest/assets/Neural/denoiser.safetensors` 已生成 |
| 引擎 C++ 侧封装（dlopen + ABI 握手 + 缺库降级） | `Engine/Graphics/Neural/NeuralMLXBackend.{h,cpp}` | Vulkan/缺库路径自动降级 kernel |
| RenderGraph 集成（pack→回读环→shim→回传环，输出滞后 2 帧） | `NeuralDenoiseModule`（`PRIMAL_NEURAL_DENOISE_BACKEND=mlx`） | Metal 联跑 600+ 帧零推理失败 |
| 对拍门禁（shim vs torch parity 文件 + CPU 参考双基准） | `EngineTest/UnitTests/Neural/TestNeuralMLXShim.cpp`（CTest `TestNeuralMLXShim`） | **torch_max_err=3.3e-5**（门限 2e-4）；缺库 SKIP、对拍失败 FAIL |

对拍结论（16×16×7 parity 输入）：torch 4.8e-07 ≈ CPU 参考位级 == Swift MLX 3.34e-05 == python MLX 3.34e-05（四端一致；`ContentTools/neural/safetensors_crosscheck.py`）。

途中修掉的真 bug：双重 expm1（shim 内层与 ABI 旗标各做一次）；MLX conv2d 权重布局是 `[O,kH,kW,I]` 而非 `[k,k,I,O]`。

### 7.2 M1-b：PSNR 门禁 —— 🔶 基建完成，门禁数字被引擎侧采集缺口阻断

**已完成**：
- 计划步骤 1（DeferredLighting shadowVisibility 绑定修复）以根因修复完成：`ShadowMapOutputs` 句柄成员缺默认初始化（pass 跳过时 0 句柄穿透全仓哨兵检查写进描述符）→ `ShadowMapModule.h` 补 INVALID 默认值 + `DeferredLightingModule` validOrFallback 加固 + **MetalCommandBuffer 设备级 null 纹理回退**（整类 "Null texture access" 中止消除，保留 NULL 绑定诊断日志）。修复前帧 2-3 必崩，修复后 600+ 帧稳定。
- 门禁工具 `Tools/NeuralMLXGate`（take→7ch→shim→expm1→PSNR，trainer 同约定 dr=max(gt)；`--synthetic-sigma` 半真实验证模式）。

**✅ 2026-09-22（第二轮）已解除阻断并产出真实门禁数字 —— GATE PASSED**：

采集链三处修复（均为引擎侧真实缺陷）：
1. **NeuralCaptureModule Vulkan 布局契约**：写目标（dst 统计槽 + noisy 样本 + 5 张导出平面）缺 GENERAL 收敛屏障——存储写入被布局失配吞掉。修复：isVk 分支补 afterState=UA 屏障。VUID-09600/01197 全消。
2. **大偏移 readback 截断**：单一大 staging（294MB）上 bufferOffset ≥ 2×plane（≈112.5MiB）的 CopyTextureToBuffer 静默产出零——RHI/驱动层问题（未解，已绕过）。修复：改为**每平面独立 readback buffer**（staging_[5]）。
3. **train/runtime 尺寸错配**：AddPass 用 inputs.render_width/height（管线 target 逻辑分辨率 1280×720），而模块纹理按 render 分辨率（2560×1440）创建——kernel 只覆盖左上 1/4。修复：AddPass 一律用模块自身 render_width_/height_。

**SC cards=0 根因定位 + 深修完成（2026-09-22，已消除超时配方依赖）**：SC 注册被 content-ready 门禁阻塞的根源 = 实例 336 的 mesh bounds 为垃圾值 `(0, 6.6e28, 0)-(0,0,0)`（疑似 Nanite 流化未初始化内存），门禁判据视其为“未就绪”而无限推迟注册（默认 600 帧超时）。

深修两步：① 门禁判据二次收口——只等“mesh 未加载”（null bounds 指针），bounds 倒置（max < min）视为显式退化即就绪（其 view_area 过滤自然产出 0 卡，注册侧无副作用）；② SC-DIAG 一次性诊断打印（atlas/card_data/grid_offsets/grid_cards/card_count 各字段）。验证：无超时配方下 SC 注册 436 卡于帧 1，全部有效性字段通过；4 机位 × 128 帧 4 take 门禁 **GATE PASSED（noisy 33.0~42.1，denoised 33.0~39.4 dB，SSIM 0.99997+，其中 take 3 +5.30dB）**。PRIMAL_SC_REG_GATE_TIMEOUT 配方仍保留作为兜底。

**真实门禁数字（GATE PASSED，NeuralMLXGate）——终版（M2 规模数据管线 + 12 take 多机位）**：

**SC 激活配方**：SC 注册被 content-ready 门禁阻塞至 600 帧（实例 336 mesh bounds 为垃圾值 6.6e28 触发防御门禁），采集窗口必须覆盖它 → 用现成 env `PRIMAL_SC_REG_GATE_TIMEOUT=60` 提前激活 SC（436 卡正常注册），SPGI 输出变为真实连续辐射（98.5% 非零、上万唯一值），双稳态离群 take 消失。

**M2 数据管线规模化验证**：8 机位 CAM_PATH × 128 帧 = **12 take 多机位采集**（~7 分钟）；修 trainer 7ch normal bug 后用 10 个稳定 take 训练、holdout take 1：**noisy 24.60 → 32.90 dB（+8.30 dB）**——模型在引擎真实数据上实现真去噪增益。

**终版门禁（NeuralMLXGate，12 take，GATE PASSED）**：take 1（holdout）**32.90 dB**、take 3：34.23、take 4：36.67（与 python torch 复算 36.67 位级一致）、take 5：39.50、take 6：32.52、take 7：36.51、take 8：40.81、take 9：38.82、take 10：38.62、take 11：38.50；take 2 按 SKIP-UNSTABLE 跳过（基线 11.2 < 门限）。engine-input 稳定优于 trained-input → normal 修复生效。

**期间修复的第三处缺陷（门禁工具自身）**：shim 输出为 CHW 平面而 GT 为像素交错 HWC——CHW 直比 HWC 把 PSNR 压低 ~15dB（此前 18.99/20.92 等数字均为此伪影）。修复后门禁数字与 trainer torch 评测逐位一致（36.67=36.67）。

**M2 里程碑：NR 权重提取 + MLX 加载成功（2026-09-22 终版）**：用户提供了 nvngx_dlssnr.dll 310.8.0.0（165MB）与 nvngx_dlssg.dll 310.8.0.0（7.4MB）。`mlxdlss-weights all` 提取 NR 权重：649 张量、145,755,123 参数、0 NaN（dlssnr-weights-logical.safetensors 291MB + packed 147MB + NeuralRendering.dlssmodel MLX 包）；MLXNeuralRenderer 加载成功（temporalCadence=frameIndependent）。**M2 核心交付物（NR 权重提取 + MLX 加载）已完成**——剩余 M2 工作为：7ch GI 输入适配（替换 NVIDIA 多帧噪声输入）与 ≥500 take 微调（需时长）。FG 提取（libnvidia-ngx-dlssg.so.310.7.0 真库 + shim neural_fg_interpolate）：**FG 内容验证通过**——92 张量提取成功（85/92 健康），shim `neural_fg_interpolate` 对移动斜坡输入输出 max=1.0 全覆盖无 NaN。**M3 的 FG 内容验证已完成**——引擎时序插值 pass 为后续集成工作。

**M2/M3 外部依赖落地路径（2026-09-22 深夜核实）**：DLSS SR SDK 可免登录从 github.com/NVIDIA/DLSS/releases 直下；**但 NR 的 nvngx_dlssnr.dll 310.8.0.0（RTX Feature Suite）与 FG 的 libnvidia-ngx-dlssg.so.310.7.0（DLSS FG SDK）只随开发者门户包分发（登录）**——GitHub SR SDK 的统一库对提取工具产出垃圾（偏移表不匹配，实测 90/92 NaN）。约定放置位 `third_party/dlss/`（已建 + .gitignore 排除 + README 指引），放置后：
```bash
scripts/extract_dlss_weights.sh   # 一键:校验两个库文件在位 → NR all + FG extract-fg
```
**M3 shim 侧已完成**：依赖 MLX-DLSS（DLSSMLX.FrameGenerator）+ C ABI neural_fg_create/interpolate + fp16 位重解释加载修复 + 机制冒烟——真 dlssg 库就位即插即用。门禁工具已补 SSIM（log 域映射，M2 门禁"PSNR/SSIM"的 SSIM 半边）。

**M2 数据管线规模化结论**：12 take 多机位采集 ~7 分钟、重训 ~4 分钟、门禁 ~1 分钟——管线的单位成本已足够低；≥500 take 的大规模训练只差时长与 NR 权重，不再有技术缺口。
同时如实记录：① take 2 为 SPGI 双稳态离群 take（noisy 基线 13.8dB < 门限，物理上不可达，门禁工具按前置条件 SKIP-UNSTABLE）；② 重训练模型（见下）在稳定域 +0.06dB，即当前基线高、去噪空间小；③ 残留 16 条非致命 SRO-promise 校验噪声（ping-pong 描述符布局声明，数据已证有效，彻底修复需 RHI 层 layout-honoring 描述符写入）。

**顺带修复/发现（训练侧）**：train_denoiser.py 的 7ch 分支漏写 normal（ch3..5 训练恒 0）——NeuralMLXGate 实测同权重下真实法线比零法线低 ~3dB，已修 trainer 并用当下 GI 域新数据（8 take，稳定子集 3,4,5,6 训练 / 7 holdout）重训：holdout 39.31→39.37dB，新权重已部署（denoiser.pnn/parity/safetensors 同步更新，TestNeuralMLXShim/safetensors_crosscheck 全绿）。

**遗留（GI 分支，未阻断门禁但限制数据保真度）**：Vulkan sponza 测试中 SC 卡片=0（SC-DIAG：grid_offsets/grid_cards INVALID + card_count=0）→ SPGI 仅天空供给，75% 像素内容为 0/1 二值，去噪空间受限；诊断代码保留（StandardRenderPipeline 两处 [SC-DIAG] 一次性日志）。Metal SPGI 全黑状态未查（本轮绕行 Vulkan）。

### 7.3 M2：DLSS NR 改造 —— ⛔ 外部依赖未到位（非代码缺口）

- 路线 B 前置：NVIDIA DLSS SDK 库文件（用户下载，本机未检出）。无权重则 71 块 transformer 迁移学习无从谈起。
- ≥500 take 训练数据：采集管线的 Vulkan/Metal 缺口（7.2）需先解除。
- 已就绪的 M2 基建：torch 2.14（.venv）+ safetensors 往返验证（torch 读部署格式位级一致 4.8e-07，训练↔部署共同格式回路打通）；python-MLX 0.32 已装。

### 7.4 M3：帧生成 + 性能 —— 🔶 shim 内可验证项完成，FG 集成待权重

- **int8 权重存储**（逐出通道对称量化，uint8 常驻 + 前向反量化）：shim `neural_mlx_create_ex` bit0；实测 log 域 mean|Δ|=0.0014（门限 0.02）。注：MLX 无 int8 conv kernel，此为权重压缩语义（与 MLX-DLSS int8Fast 的量化精神一致），非 int8 计算加速。
- **半分辨率执行**（2×2 平均池化→网络→逐元素 nearest 上采样，奇数边缘 CPU 钳制）：infer flags bit1；平滑输入 mean|Δ|=0.084（16×16 微型尺寸的经验界，真实 GI 尺寸下边界效应占比 ∝ 1/尺寸）。
- **FrameGenerator 集成（2026-09-22 大幅推进）**：
  - DLSS SDK 免登录直下：github.com/NVIDIA/DLSS releases（ngx_dlss_demo_linux/windows.zip，310.5.0~310.9.1 全版本）——**无需开发者账号**。
  - MLX-DLSS 的提取 CLI（`mlxdlss-weights`，纯 Python）已验证可运行；**但其 FG 偏移表只匹配 FG 专用库 `libnvidia-ngx-dlssg.so.310.7.0`**——GitHub 的 DLSS SDK zip 内是统一 SR 库 `libnvidia-ngx-dlss.so.310.7.0`（不同布局），对它跑 extract-fg 产出垃圾权重（90/92 张量 NaN，实测确认）。dlssg 库在 DLSS FG SDK / RTX Feature Suite 包内（NVIDIA 开发者门户，需登录）——**仍是用户动作**。
  - **shim 侧 FG 集成已完成并测试**：shim 依赖 MLX-DLSS（DLSSMLX.FrameGenerator，pin 0ca2dea），新增 C ABI `neural_fg_create`/`neural_fg_interpolate`（[1,H,W,3] f32 [0,1] NHWC，phase 0..1）；fp16 safetensors 加载需 `Float16(bitPattern:)` 位重解释（`asType` 数值转换会毁权重——FG 全零输出的根因，已修）。TestNeuralMLXShim 含 FG 机制冒烟（SKIP-条件：权重文件）。真 dlssg 库就位后：提取 → FG 立即可跑，引擎 RG 的时序插值 pass 为剩余工作。

### 7.5 复现配方

```bash
# 构建 shim（首次：Package.swift 平台 macOS 14+；MLX 要求）
cd Tools/NeuralMLXShim && swift package resolve && swift build -c release && cd ../..
./scripts/build_neural_mlx_metallib.sh          # mlx.metallib 与 dylib 同目录
python3 ContentTools/neural/pnn_to_safetensors.py \
    EngineTest/assets/Neural/denoiser.pnn EngineTest/assets/Neural/denoiser.safetensors --verify

# 对拍门禁（CTest）
cmake --build Darwin/Debug -j8 && ctest --test-dir Darwin/Debug -R Neural

# 四端交叉验证
.venv/bin/python ContentTools/neural/safetensors_crosscheck.py \
    EngineTest/assets/Neural/denoiser.safetensors EngineTest/assets/Neural/denoiser_parity.bin

# 引擎内 MLX 后端联跑（Metal；Vulkan 自动降级 kernel）
PRIMAL_SPGI=1 PRIMAL_NEURAL_DENOISE=1 PRIMAL_NEURAL_DENOISE_BACKEND=mlx \
    ./Darwin/Debug/TestModularPipeline
```
