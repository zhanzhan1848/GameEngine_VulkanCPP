# GI 各 Pass 神经渲染生成——训练方式与数据集规划

日期：2026-09-24 ｜ 状态：📋 **规划（暂缓执行）** ｜ 决策：GI 基线观感达标后再启动
｜ 上游：[SR 训练完工记录](2026-09-24-sr-training-div2k-sponza.md) · [M0 神经渲染训练规划](2026-09-22-neural-rendering-inference-training-plan.md)

---

## 0. 为什么现在不做（决策记录）

**核心论点：神经代理的监督目标就是现有 GI 的输出——GI 本身有结构性缺陷时，网络会把缺陷固化，甚至放大（网络学到的不是"正确的 GI"，而是"我们当前 GI 的样子"）。**

当前 GI 观感仍未达标（[Sponza vs Lumen 四差距](2026-09-20-sponza-vs-lumen-visual-gap-analysis.md)行动轮虽已处理 P0-P6，但中庭能量链排查未结案，见 [courtyard 调查](2026-09-21-courtyard-upstream-energy-investigation.md)）。因此本规划**只立项不执行**；启动条件见 §2 前置门槛。

已具备的基础（无需重建）：

| 资产 | 状态 | 位置 |
|---|---|---|
| GI 降噪 take 数据集（16ch 契约） | ✅ 60 takes @1440p×128帧 | `take_<tag>_<i>.bin/.json`（`/tmp/neural_capture_*`，**/tmp 会清空，需迁持久路径**） |
| 降噪器从零训练 | ✅ 11k 参数基线 | [`train_denoiser.py`](../ContentTools/neural/train_denoiser.py) |
| NR transformer 微调闭环 | ✅ 冻结主干+适配器 | [`train_nr.py`](../ContentTools/neural/train_nr.py)（输出可回注 `.dlssmodel`） |
| take→npz 数据集构建 | ✅ | [`make_dataset.py`](../ContentTools/neural/make_dataset.py) |
| NR vs 降噪器 A/B 评估 | ✅ | [`eval_nr_vs_denoiser.py`](../ContentTools/neural/eval_nr_vs_denoiser.py) |
| 图像级配对采集+训练（SR 范式） | ✅ 本次完工 | [`build_game_pairs.py`](../ContentTools/neural/build_game_pairs.py) / [`sr_train_v2.py`](../ContentTools/neural/sr_train_v2.py) |
| GI 逐段读回仪表 | ✅ | `GI_LADDER_DUMP=1`（vis→直射→探针→融合逐腿，`FusionCompositeModule.cpp`）；`PRIMAL_PASS_DEBUG` 14 视图 |
| 质量档 A/B 旋钮 | ✅ | SPGI tier/lightVar/tierRays、WRC 档位（[仪表清单](../AGENTS.md)与 Docs 诊断区） |
| 自动门禁样板 | ✅ | TestVulkanDLSSRenderGraph FG/NR 独立断言 |

---

## 1. 目标与范围

**做什么**：为 GI 链路各 pass（间接光降噪/融合、SPGI 探针供给、SC 卡片供给、最终 indirect composite）建立"采集→数据集→训练→引擎回注→门禁"的可重复管线，先易后难分三条路线（§3）。

**不做什么**（明确排除）：

- ❌ 生成式模型（diffusion 类）：渲染管线要求确定性、时序稳定、低开销，业界主流（NRC 一系）均为回归式；
- ❌ 探针/卡片**表示本身**的神经化（结构性强、无法配对监督，等回归路线出收益后再议）；
- ❌ NR 145.7M vendor 主干的重训（只做微调，M0 已验证路径）。

---

## 2. 前置门槛（Phase G：GI 基线验收）

启动训练前必须过这道门，否则等于教网络复刻缺陷。验收沿用既有方法学（fixed-exposure ROI + 分腿判读，**不能只看成片观感**——[courtyard §经验](2026-09-21-courtyard-upstream-energy-investigation.md)）：

| # | 门槛 | 度量 | 引用方法学 |
|---|---|---|---|
| G1 | 中庭能量链排查结案 | 四差距行动项无 Open | gap-analysis + courtyard 文档 |
| G2 | 逐腿能量账本自洽 | `GI_LADDER_DUMP` 各腿无钳死/双乘类异常 | courtyard §实验判读 |
| G3 | 观感对照达"可用" | Sponza 对照 Lumen 参考，用户验收 | gap-analysis §观感协议 |
| G4 | 时序稳定基线 | 连续帧差异在既有门禁内（sequence stats） | TestVulkanSponzaRenderGraph `SPONZA_CAPTURE_SEQUENCE_STATS` |

**责任人**：GI 校准线（进行中文档），不归本规划管。本规划所有后续 Phase 以 G1-G4 全绿为启动信号。

---

## 3. 三条训练路线

### R1（第一颗子弹）：间接光降噪/融合增强

- **输入/监督**：noisy GI（1spp/低档）+ var + depth + normal → 收敛 GI（gt_mean）。**监督信号最好定义：收敛 GI 即 GT**。
- **现状**：60 takes 数据现成、11k 降噪器是已测基线、NR 微调闭环可叠加。
- **增量工作**：噪声条件化（M0 遗留下一步——当前降噪器对噪声强度不自适应）；时序通道（§4）。
- **引擎回注面**：`.pnn` v2 conv kernel（train_denoiser.py 已预留）或 NR 适配器路线。
- **估计**：数据就绪后 1-2 周人日 + 训练机时小时级（MPS 实测 13.6 it/s @EDSR-lite 量级；11k 小网络更快）。

### R2：GI 质量档神经提升（"GI 超分"，复用 SR 范式）

- **输入/监督**：低档 GI（低 ray 数/降档探针/tier 降级）→ 高档收敛 GI。**与 SR 的双跑配对完全同构**：同确定性相机、两档 env 配置各跑一遍，逐帧配对。
- **数据入口**：质量档 env 旋钮（SPGI tier/lightVar/tierRays、WRC 档位）+ `SPONZA_CAPTURE_DIR` 序列采集 + `PRIMAL_PASS_DEBUG=spgi_raw` 离屏视图；数值级核对走 `GI_LADDER_DUMP`。
- **增量工作**：写"双档双跑"采集脚本（改造 `build_game_pairs.py`，换 env 不换分辨率）；HDR 域训练（GI 是线性 HDR，SR 是 LDR——loss 要换 log1p/tonemap 域，train_denoiser.py 已用 log1p 可参考）；pass 输出的离屏回读面确认（PassDebugView 已能出实体，但高精度 float 回读需要确认走 capture 通道而非 debug 视图）。
- **估计**：采集脚本 2-3 天 + 数据半天/场景 + 训练同 R1。

### R3（长期立项评估）：神经缓存/代理（NRC 式）

- **输入/监督**：GBuffer 特征（depth/normal/albedo/材质+光照方向）→ 逐像素间接光。PassDebugView 已能出 normal/depth/albedo 实体，即输入面大体存在；监督=R2 的高档收敛 GI。
- **定位**：R1/R2 出收益后的演进方向（推理在引擎内的开销预算问题先调研——11k 降噪器的 `.pnn` kernel 路径是开销锚点）。
- **估计**：先立项调研，不排期。

---

## 4. 数据集规划

### 4.1 现有资产与共同短板

降噪 60 takes + SR 200 对，**全部来自 Sponza 单场景**，且降噪 take 未版本化绑定 GI 配方（风险见 §6）。多样性是两条线共同的债，R1/R2 数据需求合并解决。

### 4.2 目标数据规格（R1/R2 通用）

| 维度 | 规格 | 采集成本锚点 |
|---|---|---|
| 场景 | Sponza + ≥2 个新场景（不同材质/开敞度） | 场景资产已有（§5 待确认清单） |
| 相机 | 每场景 ≥2 弧线（确定性脚本，含一次切镜） | 实测 ~2 分钟/百帧（Mac 本地 Vulkan 双跑） |
| 光照/GI 配方 | ≥2 组（太阳角度/天空 IBL/GI 配方帧），**每组绑定配方 commit+配方帧号** | env 旋钮现成 |
| 时序 | 每弧线连续 ≥32 帧 + 运动矢量（FG 采集链已有 motion 面） | 需扩展 take json 契约 |
| 质量档 | R2 需低/高两档；R1 需 1spp/多档噪声强度 | env 旋钮现成 |
| 量化目标 | 首批：3 场景 × 2 弧线 × 100 帧 ≈ 600 帧配对（R1 可先复用现有 60 takes 起步） | ~1 天采集（含双跑） |

### 4.3 数据集版本化（新要求）

每次采集登记：GI 配方 commit、`effective_params` 快照（sun-audit 已有该机制）、env 全集、引擎构建号。**监督目标漂移时旧数据集可追溯是否可复用/需重采。** 放 `Data/neural_datasets/manifest-*.md`（或 Docs 诊断区，落点执行时定）。

---

## 5. 训练基础设施

- **复用**：[`sr_train_v2.py`](../ContentTools/neural/sr_train_v2.py)（train/eval/export 骨架、瓦片推理、TorchScript 导出、npy 缓存）+ [`train_denoiser.py`](../ContentTools/neural/train_denoiser.py)（HDR log1p 域、残差形式）= R1/R2 的起点代码；改造点：通道契约、时序窗口、HDR loss。
- **算力**：微调/小模型全在 Mac（M4 Pro MPS 实测 13.6 it/s @batch16 EDSR-lite，6k iter ≈ 8-10 分钟）；大规模从头预训练可选 5090（窗口有限，产物优先回传，[用法与坑](2026-09-24-sr-training-div2k-sponza.md) §5090）。
- **导出面**（按回注路径选）：TorchScript（libtorch）/ CoreML（macOS 固定尺寸）/ `.pnn` v2（引擎内 compute kernel）/ safetensors→`.dlssmodel`（NR 适配器路线，M0 已通）。

---

## 6. 风险与开放问题

| # | 风险 | 缓解 |
|---|---|---|
| 1 | **监督目标漂移**：GI 还在迭代，训好的网络绑定旧配方 | §4.3 版本化；R1/R2 都用"继续微调"而非从头训 |
| 2 | 时序 flicker：网络逐帧独立推理引入不稳定 | 时序窗口输入（§4.2）；sequence stats 门禁硬卡 |
| 3 | 单场景过拟合（SR 已实证：零样本迁移仅 +0.11dB） | §4.2 场景矩阵先行；每轮必测跨场景留出 |
| 4 | HDR 域指标失真（tonemap 选择影响结论） | 沿用 fixed-exposure ROI + log1p 域评估，不看成片均值 |
| 5 | 推理开销超预算（引擎内实时性） | R3 前置调研；R1 降噪器 11k 参数级起步，`.pnn` 开销锚点先行 |
| 6 | 采集数据在 /tmp（重启清空） | Phase 1 第一件事：60 takes 迁持久路径 + 版本化登记 |

---

## 7. 执行清单（启动时按序领取）

1. **Phase 1 数据基建**：takes 迁移+版本化 → 时序 take 契约（+motion）→ 双档双跑采集脚本（改造 build_game_pairs.py）→ 3 场景首批采集；
2. **Phase 2 = R1**：噪声条件化降噪器重训（现有 60 takes 即可先行）→ 时序版 → 引擎回注 + TestVulkanDLSSRenderGraph 增加降噪断言；
3. **Phase 3 = R2**：HDR 配对训练 → 跨场景留出评估 → 与 R1 组合；
4. **Phase 4 = R3**：NRC 式代理立项调研（开销预算书先行）。

每阶段验收都走 §2 同款判读纪律（ROI + 分腿 + sequence stats），并按仓库纪律登记 ARCHIVE。
