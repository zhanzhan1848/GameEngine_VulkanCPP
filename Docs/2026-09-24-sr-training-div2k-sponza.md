# 2× 超分辨率模型训练（DIV2K + Sponza 渲染帧）——完工记录

日期：2026-09-24 ｜ 状态：✅ 完成（目标 PSNR≥27dB 达成：**32.96dB，+7.91dB vs bilinear**）

## 背景与目标

M0 结论（见 [DLSS-SR capture 经验](2026-09-24-dlss-sr-capture-experience-and-blockers.md)）：
合成纹理训练的 SR 模型比 bilinear 还差 0.3dB——合成纹理与真实渲染帧域差距太大。
本轮改用 **DIV2K 真实图像预训练 + 引擎实拍 Sponza 帧微调**：

- 输入 640×360 低分辨率渲染帧 → 输出 1280×720（替代 FinalBlit 双线性）
- 目标：游戏域 PSNR ≥ 27dB（bilinear baseline ~24.4-25.5dB）
- 设备：5090 服务器训练（RTX 5090，torch 2.6 CUDA），本地 M4 Pro MPS 冒烟/兜底

## 数据

| 项 | 内容 |
|---|---|
| DIV2K | `yangtao9009/DIV2K`（hf-mirror，官方 3.53GB train.zip，800 张 HR）<br>790 张训练 / 后 10 张验证（symlink 重排）+ npy_cache 免 PNG 解码 |
| 游戏帧 | `TestVulkanSponzaRenderGraph` 两次运行配对采集：<br>`PRIMAL_RENDER_SCALE=0.5`（640×360 渲染，FinalBlit 上采样的 1280×720 回读，BOX 降回 640×360 作 LR）<br>`PRIMAL_RENDER_SCALE=1.0`（原生 1280×720 作 HR）<br>两条相机弧线（默认路径 + `SPONZA_CAM_SCRIPT=1` 切镜后视角）× 100 帧 = 200 对<br>**160 对训练 / 40 对留出验证**（每 5 帧取 1 留出，跨两弧线） |
| bilinear baseline | 弧线 1：25.46dB；弧线 2：24.66dB（与 M0 的 ~24.4 吻合） |

采集命令（本机，Darwin/Debug 树的 Vulkan 二进制）：

```bash
PRIMAL_RENDER_SCALE=0.5 SPONZA_CAPTURE_FRAME=380 SPONZA_CAPTURE_COUNT=100 \
  SPONZA_CAPTURE_DIR=/tmp/sr_game_low ./Darwin/Debug/TestVulkanSponzaRenderGraph &
# 同参数 PRIMAL_RENDER_SCALE=1.0 → /tmp/sr_game_ref
# SPONZA_CAM_SCRIPT=1 重复一遍得第二弧线（frame 360 切镜后为另一视角）
# 配对+基线：python ContentTools/neural/build_game_pairs.py <low> <ref> <out> [编号偏移]
```

## 工具链

- [`sr_train_v2.py`](../ContentTools/neural/sr_train_v2.py) — train/eval/export 一体
  （EDSR-lite 16×64，1.19M 参数；在线退化 bicubic/bilinear/box+噪声；瓦片推理；
  TorchScript 导出；CUDA/MPS 双后端）
- [`prep_npy_cache.py`](../ContentTools/neural/prep_npy_cache.py) — DIV2K PNG→npy
  （吞吐 6.6→10.2 it/s，瓶颈是 2K PNG 反复解码）
- [`build_game_pairs.py`](../ContentTools/neural/build_game_pairs.py) — 采集配对构建
  + bilinear 基线测量

## 结果

模型：EDSR-lite（16 resblock × 64 feat，**1.19M 参数**），2× PixelShuffle。

| 阶段 | 指标 | 数值 |
|---|---|---|
| 预训练（DIV2K 790 张，28k iter 截断） | DIV2K valid10 PSNR | **34.60 dB**（bicubic LR；~10.2 it/s @5090） |
| 预训练零样本迁移 | game_val 40 帧 | 25.17 dB（vs bilinear 25.06，**仅 +0.11**——域差距实证） |
| **游戏域微调**（160 对 ×4 采样 + DIV2K 混合，6k iter，lr 3e-5） | DIV2K valid10 | 33.59 dB（域适应代价 -1.0，可接受） |
| **微调后（最终模型）** | **game_val 40 帧留出** | **32.96 dB（bilinear 25.06，+7.91）** ✅ |
| 微调后单帧示例（low_1400，arc2） | 全图 | 35.36 dB（PyTorch/TorchScript/CoreML 三端一致） |

注：game_val 与训练帧来自同一确定性相机路径（每 5 帧取 1 留出），内容多样性有限，
32.96dB 应理解为"同分布未见帧"口径；跨场景泛化需扩充采集（见遗留）。

产物（[`ContentTools/neural/sr_output/`](../ContentTools/neural/sr_output/)）：

- [`sr_best_finetuned.pth`](../ContentTools/neural/sr_output/sr_best_finetuned.pth) /
  [`sr_best_pretrained.pth`](../ContentTools/neural/sr_output/sr_best_pretrained.pth) —
  权重（含 opt/sched 可续训）
- [`sr_edsr2x.ts.pt`](../ContentTools/neural/sr_output/sr_edsr2x.ts.pt) — TorchScript
  （任意尺寸输入，三尺寸往返校验通过）
- [`sr_edsr2x.mlpackage`](../ContentTools/neural/sr_output/sr_edsr2x.mlpackage) — CoreML
  （固定 640×360 输入 → 1280×720，实测 35.36dB 与 PyTorch 一致）
- [compare_game_low_390.png](../ContentTools/neural/sr_output/compare_game_low_390.png) /
  [compare_game_low_1385.png](../ContentTools/neural/sr_output/compare_game_low_1385.png) —
  对比条（bilinear｜SR｜GT，两弧线各一）
- [train_log_pretrain.txt](../ContentTools/neural/sr_output/train_log_pretrain.txt) /
  [train_log_finetune.txt](../ContentTools/neural/sr_output/train_log_finetune.txt) —
  训练曲线（每次 eval 一行）

## 现状评估与后续路线（2026-09-24 复盘）

### 结论一：当前域内不需要加强训练

微调已收敛（lr 退火至 1e-7），game_val +7.91dB 余量充足。预训练因服务器窗口截断在
28k/100k iter，但补完只影响 DIV2K 榜面约 +0.2~0.3dB，对游戏域几乎无增益——瓶颈不在
训练量。微调后 DIV2K 回落 1.0dB（34.60→33.59）属正常域折衷；若需兼顾可在微调时上调
DIV2K 混合比例或加回放正则。

### 结论二：真正的短板是数据多样性

200 对全部来自 Sponza 单场景、单确定性相机路径的两条弧线，且留出帧与训练帧内容相近
（每 5 帧取 1），32.96dB 严格说是"同分布未见帧"口径，无法完全区分域适应与记忆。
零样本迁移仅 +0.11dB 说明分布之外基本无效。

加强方式（收益排序）：

1. **换视角/换场景采集**（非 Sponza 场景、不同光照状态、不同时段 GI 配方）——协议已
   固化在 [`build_game_pairs.py`](../ContentTools/neural/build_game_pairs.py)，Mac 本地
   两次 Vulkan 测试 ≈ 2 分钟/百帧，零 GPU 服务器依赖；
2. 采集时同步扩留出集（不同弧线各留 20），把口径做实；
3. 场景采集后用现有权重继续微调即可（无需从头预训练）。

触发条件：若只在本引擎 Sponza 域内使用，现权重可直接用；一旦渲染内容大改（新场景、
光照管线变化）或要对外声称泛化能力，先做上面第 1 条。

### 结论三：Mac 全链路可用，后续微调不必依赖 5090

2026-09-24 在 M4 Pro（MPS，torch 2.14）实测基准：正式权重 + 游戏帧数据，标准微调
配置（batch 16，patch 48²→96²，workers 4）跑出 **13.6 it/s**（150 iter 仅 11s），
与 5090 的 ~10 it/s 相当甚至更快——此规模模型瓶颈在数据加载不在 GPU。

| 工作负载 | Mac (MPS) | 5090 (CUDA) |
|---|---|---|
| 游戏域微调（6k iter） | ≈8-10 分钟 | 343s |
| DIV2K 大图预训练 | 数小时级（可用） | ~2.8h/100k iter（实跑 28k） |
| 采集（Vulkan 测试双跑） | 本机 2 分钟/百帧 | 不适用 |

全链路已在本机闭环：采集（`TestVulkanSponzaRenderGraph` 双跑）→ 微调（MPS）→ 导出
（TorchScript/CoreML 均在本机转换并验证）→ 评估。5090 只在需要大规模从头预训练时才
值得动用（hf-mirror 直连 ~10MB/s 传数据，容器 torch 2.6 CUDA 可用，但窗口有限，长
任务要倒排截断点并优先回传产物）。

### 引擎集成（未做，下一步入口）

替换 FinalBlit 双线性的接线未做，本任务只交付模型。两条路径：
[`sr_edsr2x.ts.pt`](../ContentTools/neural/sr_output/sr_edsr2x.ts.pt)（TorchScript，
需 libtorch，任意尺寸）或
[`sr_edsr2x.mlpackage`](../ContentTools/neural/sr_output/sr_edsr2x.mlpackage)（CoreML，
固定 640×360 输入，macOS 渲染链可直接吃）。M0 的 DLSS-SR 阻塞记录见
[经验文档](2026-09-24-dlss-sr-capture-experience-and-blockers.md)（CUDA 后端不支持 SR
等结论仍在）。

### 延伸：GI 各 Pass 的神经渲染生成

GI 链路（降噪/融合、探针供给、质量档提升、NRC 式代理）的训练方式与数据集规划已立项
暂缓——**决策：GI 基线观感达标后再启动**（神经代理的监督目标就是当前 GI 输出，GI 有
结构性缺陷时训练会把缺陷固化）。详见
[GI 各 Pass 神经渲染生成规划](2026-09-24-neural-gi-pass-training-plan.md)；
本文档的双跑配对范式与 Mac 训练基建是其 R2 路线的直接前置。
