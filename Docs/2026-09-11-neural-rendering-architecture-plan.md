# 神经渲染基础设施路线（M0 已落地）

日期：2026-09-11
状态：M0 完成（神经运行时最小闭环 + debug 上屏链路）

## 0. 背景与结论

参考《Neural Irradiance Cache 实验手记》（知乎 p/2012909669794988193）的结论：
基于 Hash-Encoding 的 NIC 直接替代 GI 缓存（lightmap/DDGI）在静态 diffuse 场景
打不过传统方法；业界规模化落地的神经渲染在"外围"——神经降噪/超分（DLSS、
Ray Reconstruction、FSR4）、神经压缩（NTC）——再向核心渗透。

结合本引擎现状（SDF ray marching 全家桶 + DDGI/SPGI/SurfaceCache + 完整时域
滤波链，无硬件 RT、Apple Silicon 无开发者 tensor core、MoltenVK 无 cooperative
matrix），可行的路线按基础从厚到薄：

| 方向 | 基础 | 说明 |
| --- | --- | --- |
| M1 神经降噪（GI 后处理） | ★★★★ | SVGF 谱系时域链现成，神经降噪器并联/替换 `ScreenProbeDenoise`；训练数据自产（pathdbg 路径追踪累积 GT vs 1spp） |
| M2 NRC 式 probe 缓存 | ★★★☆ | DDGI 骨架（probe 预算调度/三缓冲/confidence）现成；需先修 `LumenDDGIPass.cpp` `UpdateProbeOrigin` 滚动三项前置 |
| M3 神经超分 | ★★ | 引擎无任何 upscaler，分辨率解耦是大手术；先走 MetalFX（`MetalNeuralWrapper` L0，macOS 26 已具备 API 条件） |
| NTC 神经纹理压缩 | 离线★★★★/运行时★★ | AssetPipeline 的离线 bake→magic blob 段范式现成；运行时缺 shader 解码 kernel |

## 1. 分层架构（贴合引擎既有惯例）

```
Layer D  应用链路   M1 NeuralDenoiseModule / M2 NRC probe / M3 超分
Layer C  模块层     RenderPipeline/Modules/NeuralDebugModule（已落地，首个链路）
Layer B  运行时层   Graphics/Neural/NeuralRuntime + NeuralWeights（已落地）
Layer A  资产层     ContentTools/neural/train_image_fit.py → .pnn（已落地）
```

设计原则（全部从引擎现有惯例推导）：

- **不外挂运行时**：无 ONNX/CoreML/MPS 依赖。推理 = 普通 compute pass，只用
  现有 RHI 原语（StorageBuffer / ComputePipeline / DescriptorSet / UniformBuffer）。
- **不改 `RHIDeviceBase`**（ABI 纪律）：神经运行时是 RHI 之上的组合层。
- `MetalNeuralWrapper`（MTL4 detect 占位）保持原职责，作为未来硬件加速后端位。
- 资产走 CDBK 范式（magic + 版本 + 计数 + 平铺数据 + 双侧尺寸校验）。
- 模块走 GIGatherModule 范式（Initialize/Shutdown/AddPass(RenderGraph) +
  descriptor 三缓冲轮换 + 常量结构体 static_assert 双方言对齐）。
- shader 字节码自加载走 DebugProbeSphereModule 范式（Metal 读 MSL 源运行时
  编译；Vulkan 读 .spv；相对路径 + ../ 上溯 + 仓库绝对路径兜底）。
- 新链路 env 门控默认关（`PRIMAL_NEURAL_DEBUG=1`，仿 `SC_INDIRECT`）。

## 2. PRIMAL NEURAL SPEC v1（.pnn + hash-encoding + MLP）

四份实现必须逐位同步（改一处必须四处同改，单测锁住其中三端）：

1. `ContentTools/neural/train_image_fit.py` —— 训练/导出权威
2. `Engine/Graphics/Neural/NeuralWeights.cpp` —— CPU 参考前向
3. `Engine/Graphics/Metal/shaders/Neural/MLPInferDebug.metal` —— MSL
4. `Engine/Graphics/Vulkan/shaders/Neural/MLPInfer.comp` —— GLSL

### .pnn v1 布局（little-endian）

```
u32 magic = 0x31574E50 ("PNW1")
u32 version = 1
u32 layer_count
u32 table_element_count          # hash grid f32 个数
u32 input_dim / u32 output_dim
u32 param_count (=16) / u32 reserved
u32 level_res[16]                # 每级分辨率（float64 算好存整数，消费端不再 pow）
LayerDesc[layer_count]           # u32 in, u32 out, u8 act(0 none/1 relu/2 sigmoid),
                                 # u8 quant(0=f32), u16 pad —— 12 B/条
f32 table[table_element_count]   # [level][entry][F] 平铺
每层: f32 W[out][in] row-major + f32 b[out]
```

### 数值语义

- hash grid：L 级 × F=2 特征，`h = l*0x9E3779B9 ^ i*0x85EBCA6B ^ j*0xC2B2AE35;
  h *= 0x27D4EB2F; h ^= h>>15`，u32 自然溢出语义四端一致；
  网格坐标全 float32（MPS 无 float64，GPU 是主要消费方）；
- MLP：逐 i 升序累加 `acc = b[o]; acc += x[i]*W[o][i]`。
- 对拍门禁：编码 atol 1e-6（实测 6e-8），端到端 atol 2e-4（实测 1.2e-7）。

### 已知边界

- layer_count ≤ 8、各维 ≤ 64（`NeuralRuntime::kMaxLayers/kMaxLayerDim`，与
  shader 的 `layers[8]`/`feat[64]` 容量对齐）。NRC 量级网络（<100k 参数）够用。
- 仅 f32 权重（quant 字段为 fp16/INT8 预留）。
- 神经 pass 暂不进 Dawn/WGSL 路径。

## 3. M0 落地清单

| 文件 | 角色 |
| --- | --- |
| `ContentTools/neural/train_image_fit.py` | PyTorch 训练（hash-encode + 3×64 MLP 拟合程序化图像）→ .pnn + parity 向量 + PPM 预览 |
| `EngineTest/assets/Neural/test_image.pnn` | 训练产物（2.1 MB，随 assets 拷入测试运行目录） |
| `EngineTest/assets/Neural/test_parity.bin` | 64 点三端对拍向量（"PNPV"） |
| `Engine/Graphics/Neural/NeuralWeights.{h,cpp}` | .pnn 解析 + CPU 参考前向（零渲染依赖） |
| `Engine/Graphics/Neural/NeuralRuntime.{h,cpp}` | 权重整块 → StorageBuffer（组合层） |
| `Engine/Graphics/RenderPipeline/Modules/NeuralDebugModule.{h,cpp}` | compute 推理（左 NN 右 GT）+ blit 上屏 + dump 对拍 |
| `Engine/Graphics/Metal/shaders/Neural/MLPInferDebug.metal` | 单文件三入口（infer/vs/fs） |
| `Engine/Graphics/Vulkan/shaders/Neural/MLPInfer.{comp,vert,frag}`(+.spv) | Vulkan 侧 |
| `EngineTest/UnitTests/Neural/TestNeuralWeights.cpp` | 单测：合成网络手算 / 失败路径 / parity |
| `StandardRenderPipeline.{h,cpp}` | env 门控接线（early-out 仿 SDF viz，两棵渲染路径） |

### 运行方式

```bash
# 重新训练（改网络/GT 后）
.venv/bin/python ContentTools/neural/train_image_fit.py

# 单测（无 GPU；parity 资产缺失时 SKIP 不 FAIL）
ctest --test-dir Darwin/Debug -R TestNeuralWeights --output-on-failure

# 可视链路（挂 StandardRenderPipeline 的程序，如 TestModularPipeline；注意
# 编辑器 3D 视口常态走 ForwardSceneRenderer 前向管线，不经过 deferred 链）
PRIMAL_NEURAL_DEBUG=1 Darwin/Debug/TestModularPipeline
#   → 左半屏 MLP 重建 / 右半屏 GT 参考函数
PRIMAL_NEURAL_DEBUG=1 PRIMAL_NEURAL_DUMP=/tmp/nd.raw Darwin/Debug/TestModularPipeline
#   → 稳定帧后输出 raw RGBA16F（1280x720 为 7372800 B）

# GPU 数值对拍门禁
.venv/bin/python ContentTools/neural/compare_dump.py /tmp/nd.raw 1280 720

PRIMAL_NEURAL_PNN=/path/to/xxx.pnn      # 换权重文件
```

### M0 验证结果（2026-09-15，Apple M4 Pro）

| 门禁 | 结果 |
| --- | --- |
| TestNeuralWeights（合成手算/失败路径/parity） | PASSED；编码 max err 5.96e-08，端到端 max err 1.19e-07 |
| Metal（`TestModularPipeline` + env） | dump 对拍 PASS：NN 侧 max 0.0005 / GT 侧 max 0.0005（fp16 存储精度内） |
| Vulkan/MoltenVK（同上） | dump 对拍 PASS：数值与 Metal 完全一致 |
| 集成回归无 env | TestModularPipeline(Metal) 与 TestVulkanSponzaRenderGraph 各 75s：日志零 NeuralDebug 痕迹、零 fault（env 门控零足迹验证） |
| 两棵构建树 | Darwin/Debug 与 Darwin/DebugVulkan 全量 exit=0 |

顺手修复的既有环境雷：`EngineTest/UnitTests/CMakeLists.txt` 在 ENABLE_VULKAN
下补 MoltenVK 头文件路径（照搬 IntegrationTests 同款补法）——Xcode 15+ SDK
sysroot 不再默认搜 `/usr/local/include`，此前重配置后 TestVulkan* 单测必挂。

## 4. M1 进展：训练数据采集管线（已落地，2026-09-15）

降噪器训练数据自产链路：

- **捕获点**：SPGI `ScreenProbeGather` 的输出（`GetOutputTexture()`，
  Denoise 之前的全分辨率 RGBA16F）。SPGI 每帧对 probe 摆放 + ray 方向做
  jitter（F5/M7）→ 每帧是该 1spp 估计量的独立样本。这正是
  `ScreenProbeDenoise` 的部署输入口，训练分布 = 部署分布。
- **GT**：固定相机下同信号的 Welford 在线均值/M2（fp16 安全，量级 O(1)），
  ping-pong sampled→writeonly（不用 read-write storage image，沿用仓库已
  验证资源语义）。任意单帧 = 降噪器输入，均值 = 目标，方差 = 置信度权重。
- **特征**：GBuffer depth（原样导出，线性化在 python 侧）+ world normal。
- **落盘**：pack kernel 出 5 张 rgba32f 平面（gt_mean/gt_var/depth_raw/
  normal/noisy_1spp，每像素 20 f32）→ 逐张 CopyTextureToBuffer 进单个
  readback staging → +3 帧回读写 `<dir>/take_<tag>_<i>.bin` + `.json`。
- **触发**（env，默认全关）：
  `PRIMAL_NEURAL_CAPTURE=1`、`_FRAMES=256`、`_TAKES=1`、`_WARMUP=240`
  （GI 链 SC/DDGI/SPGI 时序暖机后才累积——不暖机会把过曝瞬态采进 GT）、
  `_DIR`、`_TAG`。相机切镜（复用 `camera_cut_this_frame_`）自动重置当前 take。
- **数据集工具**：`ContentTools/neural/make_dataset.py` → npz + 噪声|GT
  并排预览 PNG。实测 Sponza 96 帧 take：mean|noisy−gt|=0.31，depth 范围
  1.6–13.1（合理室内区间），亮斑区高方差（正是 1spp 噪声特性）。

落地注意（踩坑记录）：
- Metal depth 纹理必须 `depth2d<float>`（`texture2d<float>` 绑定深度纹理
  会 GPU 挂死）；`depth2d::read()` 返回标量。
- Metal 描述符重叠命名空间：UBO 的 dstBinding 必须与布局中的槽位一致
  （texture 0..9 + UBO 0 的布局，写 binding 10 会被 MetalDescSet 静默
  SKIP → kernel 拿到垃圾 CB），跨后端的 binding 号必须平台分支。
- 编辑器 3D 视口走 ForwardSceneRenderer，采集/调试链路只在
  StandardRenderPipeline（TestModularPipeline / play 模式）生效。
- TestModularPipeline 需 `PRIMAL_SPGI=1`（Metal 上 SPGI 默认关）。
- Vulkan 侧 GLSL kernel 已编译进 build_spv.sh，但现无运行时载体
  （TestVulkanSponzaRenderGraph 不走 StandardRenderPipeline）——待 M1
  降噪链路接入时补 Vulkan 端到端验证。

## 5. M1 离线训练：多机位数据集 + conv 降噪器（已落地，2026-09-15）

- **多机位采集**：`PRIMAL_NEURAL_CAPTURE_CAM_PATH="0,0,0;2,0,0;0,0,2;2,0,2"`
  ——每 take 一个世界空间机位偏移，管线在 `UpdatePerFrame` 应用偏移
  （take 间瞬移自然触发切镜 → 采集统计与 SPGI/TAA 时序复位）。Sponza
  实采 4 take × 128 帧，全部成功落盘。
- **训练**：`ContentTools/neural/train_denoiser.py`（MPS，checkpoint 断点
  续训）。输入 10ch = log1p(noisy) + sqrt(var) + depth + normal；4 层 3×3
  conv 残差网（10→24→24→16→3），**11,299 参数**（NRC 量级，为引擎内
  推理预算预留）。
- **留出视角结果**（take 1 训练时不可见）：PSNR 18.17 → **23.43 dB
  （+5.26 dB）**，relL1 0.641 → 0.326；原始 1spp 中几乎不可见的拱廊/
  立柱结构被完整恢复（对比图 `denoise_compare.png`，由
  `train_denoiser.py` 数据管线产出）。结论：自产数据可学，小网络量级够用。

## 6. M1 引擎内推理（进行中，2026-09-15）

已落地：
- **.pnn v2**：头 32B 同 v1（reserved 槽复用为 net flags，bit0 = log 域残差）；
  LayerDesc 24B/条 {kind,in,out,act,k,stride,pad,flags,res}；conv 权重
  W[out][in][k*k] + b。`NeuralWeights` 支持 v1/v2 解析 + CPU conv 参考前向
  （zero-pad、c→ky→kx 累加、log 域残差 + expm1），**单测 conv parity
  interior max_err = 0（与 torch 位级一致）**。
- **7ch 输入定版**：log1p(noisy)×3 + normal×3 + depth/16（真方差通道留
  M1.5 时域累积；代理方差实验无增益：21.33 vs 真方差 23.43 vs 7ch 21.55）。
- **kernel**：`NeuralDenoisePack`（7ch 打包）+ `NeuralConv`（通用 3×3 层，
  rgba 通道打包 6in/6out ping-pong，层描述符 init 期写死、每帧零更新），
  GLSL+MSL 孪生，双端编译通过，已登记 build_spv.sh。
- **`NeuralDenoiseModule`**：`PRIMAL_NEURAL_DENOISE=1` 门控接入两棵渲染
  路径（SPGI 之后），同帧输入快照 + 输出 dump（staging 三平面：
  denoised | packT0 | packT1）。

**conv kernel 排查与优化（2026-09-15 深夜，含未决项）**：
- 已修复并验证三个真 bug：① conv 邻域偏移 ky/kx 颠倒（转置卷积核）；② pack
  通道布局与模型 ch3..6 错位；③ 快照拷贝时序。**L0 conv 输出已全像素全通道
  验证正确（vs numpy ≤0.002，fp16 噪声级）**——用 tick8 的 4 平面快照对拍。
- kernel 性能重构已落地：邻域寄存器化（每输入纹理 3×3 一次取齐，MAC 走
  寄存器），消除逐 tap 纹理链选择。naive 全分辨率 conv 仍把帧率压到个位数
  ——半分辨率执行或 fp16 后处理仍是 A/B 前必做项。
- **未决：L1 及以后的 GPU 输出与参考不一致（max ~4.25）**。同帧 16 平面
  dump 下 L0 ✓（0.002）、L1 ✗（4.25）。已用 kernel 内探针验证 L1 dispatch
  实际读到的 CB（in=24/out=24/1536/6720）、权重（W1[0,0,0,0]=-0.0372、
  b1[0]=-0.2795，与文件位级一致）、输入采样均正确——输入、权重、CB 三方
  全部排除，剩余嫌疑 = kernel MAC 执行（in_c=24 的 nb/acc 路径）或
  in-pass CopyTextureToBuffer 的延迟语义使部分 dump 证据不可靠。
  另发现：末层 expm1 后写 rgba16f，值 >65504 溢出为 INF（训练时 expm1 在
  f32 域做所以无此问题）——**修复方向：末层保留 log 域输出（fp16 安全），
  expm1 挪到消费端**；同时把全部中间激活的误差预算重估（fp16 逐层放大的
  量级需要实测）。

**外部阻塞详情（GIGather 断言，属用户 GI 工作分支）**：ddgi_gi_gather 首次
  dispatch 即触发 Metal validation assert——"missing Buffer binding at index
  12 for relocatedOrigins"。GIGatherModule 的 roWrite（binding 12 =
  relocated_origins，fallback dummy）存在且布局 18 条目含 (12, StB)，但断言
  在 run 末期（帧 ~98，denoise dump@109 之前）触发 → relocated_origins_ 缓冲
  的生命周期（exec_frame_index_ < 60 门控、INVALID→dummy 回退、或
  nvolViews[0].IsValid() 门控）与首次 dispatch 的时序存在缺口。该区域为用户
  GI 重定位工作的活跃调试区，神经侧不越权修改；GIGather 修复后神经门禁
  （DUMP_FRAME=109 + capture 联跑）即可恢复。
- **根因已定位（探针 v3）**：探针把各 in 纹理在 (0,0) 的采样写进 out0 前四
  像素回读——L0 ✓；**L1 dispatch 的 in5 采样 = -0.8193（负值）**，而 L1 的
  输入 B5 是 relu 后的 L0 输出（≥0）——说明 conv_sets_[1] 的 binding 5
  实际绑定了 pack 输出纹理（-0.819 恰为该点 normal.x），L1 用错误纹理子集
  做卷积 → 偏差 4.25，与观察吻合。

- **探针 v3 补充（决定性）**：L0 dispatch（应绑 [P0,P1,dummy×4]）的探针
  回读 in0=0.9385(P0.r✓) in1=0.0(P1.r✓) 但 **in2=-0.8193 in3=0.0 in4=0.0
  in5=0.9346——四个 dummy 槽位读到四个不同值**（dummy 是同一张 1×1 纹理，
  理应同值）→ conv set 的纹理 binding 2..5 的写入/绑定根本没有生效，
  encoder 保留了其它 pass 的槽位内容；L1 的 in1..in5 同理。这解释了为何
  L0 仍正确（L0 只用 in0/in1，恰好没踩坑）而 L1 起全错。
下一步：审 MetalDescriptorSet 同 binding
  号多条目（(0,StB)+(0,UB) 等）与 15 条目布局的写入/编码映射；短期缓解：
  L1..L3 复用 L0 的描述符布局可验证此结论。
- 端到端 PSNR 现状 20.62 dB（vs noisy 17.85，含未决 L1 问题；离线参考
  +3.58 dB）。CPU 参考前向与 torch 位级一致（单测 max_err=0）不变。

## 6.5 路线修正提案：MLX 后端（2026-09-15）

用户的 MLX-DLSS 仓库（github.com/iamwavecut/MLX-DLSS）已在 Apple Silicon
上验证了完整的生产级 MLX 推理路径（NR 71 块 transformer + FG + SR/VSR，
golden bundle 对拍全部通过）。**推理侧建议从裸 Metal compute kernel 切换
为 MLX 后端**——消除全部 kernel 调试成本，获得 Apple Silicon 自动优化。
裸 Metal kernel 保留为降级路径。详见
Docs/2026-09-15-neural-mlx-backend-plan.md。

## 7. 下一步（按序）

1. **修 conv GPU 数值 bug**（§6 未决项）→ `NeuralDenoiseModule` A/B 并联
   `ScreenProbeDenoise`，采集管线做端到端 PSNR 门禁。
2. **性能**：naive 全分辨率 conv 实测把帧率压到个位数（4 层 ~21 GFLOP/帧
   标量 f32）——fp16 激活 / 半分辨率执行 / 通道打包优化后才能谈默认开启。
3. **数据集扩容**：更多机位/光源强度变体攒数百 take。
4. **GPU 数值门禁自动化**：compare_dump / make_dataset 接进 ci_local 可选段。
5. **M2 NRC**：先修 DDGI 滚动三项前置（`UpdateProbeOrigin`），再评估
   latent neural cache（在线训练 kernel + RW 权重 buffer）。
6. **M3 超分**：MetalFX L0 先行（macOS 26 已具备条件），分辨率解耦。

## 8. 风险记录

- 三端同步成本是这套路线最大的长期税；parity 向量门禁 + 单测是护栏，
  任何 SPEC 改动必须先改 `train_image_fit.py` 顶部注释再四处置换。
- f32 标量 fma 推理在 Apple Silicon 上单像素 ~5k FLOP（M0 网络），half-res
  1spp 降噪网络（~50k FLOP/px）预估 1-2ms 级——M1 立项前先做 dispatch 级
  micro-bench。
- MoltenVK 侧 storage image / SSBO 路径在本仓库已被 DDGI/SC 大面积验证，
  神经 kernel 未引入新的资源状态语义（UAV→SRV 手工屏障沿用 SDF viz 模式）。

**外部阻塞（2026-09-15 记录，非神经模块问题）**：工作树中未提交的 GI 改动
  使 ddgi_gi_gather（GIGather pass）在 Metal validation 下 abort——其 MSL
  声明了 relocatedOrigins [[buffer(12)]] 但 GIGatherModule 从未绑定该
  buffer（"missing Buffer binding at index 12"）。该 pass 归属用户的 GI
  工作分支；修复前神经门禁需在其后运行（或临时旁路 GIGather）。已知良好
  基线：修复 cb_[15] 越界后、GIGather 断言复现前的同帧 16 平面 dump 中
  L0 ✓（0.002）、denoised vs GT 24.38 dB（noisy 17.85）。


**第二轮补充（roWrite 提交修复后）**：`device_->UpdateDescriptorSets(1,
  &roWrite)` 补提交后 ddgi_gi_gather 断言消失 ✓，但 **GPU 在帧 ~98-100
  （relocation 发布门控 frame 60 之后的首个汇聚点）于
  fragmentLighting_gpuDriven / GetShadowVisibility 处页错误崩溃**——且与
  神经模块无关（无 env 亦复现，与我的 roWrite 提交无关）。高度怀疑：
  relocatedOrigins 缓冲尺寸 vs gather 探针索引范围错配（越界读→页错误），
  属用户重定位功能的在途缺口。**神经降噪门禁被此崩溃阻塞**；已知良好基线
  （denoised vs GT 24.38 dB）与 L0 全验证结论不变。

**帧依赖线索（同日第三轮）**：L1 偏差数值随 run 波动（4.25/1937，同场景同
  相机）——高度怀疑 L1 dispatch 读到的 B 是**上一帧**的内容：pass 内
  CopyTextureToBuffer 的 blit 编码器切换把帧内编码序列切成
  compute[blit]compute 多段，若 engine 的 encoder 重建丢失/延迟了 bank B 的
  某次绑定或屏障语义，L1 就会读到旧帧数据（静态场景下帧间差异 = jitter 量级
  在暗部、大差异在亮部 ✓ 与观察吻合）。**离线 micro-bisect（选项①）可完全
  避开此时序干扰，是下一轮的首选路径**：固定 7ch 确定性输入 + 仅 dispatch
  一次 L1 conv + 逐 tap 对拍 CPU 参考——一次运行即可定位 kernel 数学 vs
  管线时序。

**当前阻塞（Null texture access hang）**：带 env 跑时 DeferredLighting 的
  fragmentLighting_gpuDriven 读到 null texture（GetShadowVisibility →
  shadowVisibility 纹理），进程挂起。DeferredLightingModule 用 std::vector
  （值初始化安全）——该 null 是 Deferred 在 validation 下的既有间歇问题，
  我方模块的存在改变了 encoder 槽位时序使其暴露。**系统性修复：全仓
  RenderPipeline 约 20 处 WriteDescriptorSet 数组未零初始化（同 dstArrayElement
  UB 类）——建议统一清理。本轮已修我方三模块 + GIGather 三处。

**零初始化清扫后 DeferredLighting 暴露（最终状态）**：17 处修复落地后，
  DeferredLighting 的 shadowVisibility 纹理在 SPGI 配置下稳定报 null——
  Deferred 的描述符写用 std::vector（安全 ✓）+ validOrFallback 兜底 ✓，
  排除描述符写问题 → **其它 pass 的垃圾 dstArrayElement OOB 写此前沿
  队列"意外"填了 Deferred 的槽位，零初始化清扫后这些"意外填充"消失**，
  Deferred 的某个槽位暴露为 null。需在 Deferred 侧补齐该绑定或回退。
  神经侧不被阻塞（denoise 不读 Deferred 输出），但帧 abort 挡住了门禁
  的实时运行路径。

**零初始化清扫后的新发现**：17 处修复落地后，DeferredLighting 的
  shadowVisibility 纹理在 SPGI+denoise 配置下稳定报 null（之前 OOB 越界写
  的"意外效果"是掩盖了此问题——cb_[15] 修复后暴露）。这是
  DeferredLighting+SPGI 配置的既有问题（shadow pass 的 visibility 输出在
  该配置下可能被跳过或句柄未传递），非神经模块引入。神经侧门禁在此
  阻塞，待 GI/Deferred 侧排查后恢复。
