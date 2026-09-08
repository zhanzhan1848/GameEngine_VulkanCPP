# 环境变量全量清单（T1.8 / E4）

**Date**: 2026-09-08
**Scope**: Engine / EngineTest / Tools 内全部 `std::getenv` 旋钮（自动盘点 74 个，shader 侧无）。
**约定**: 【一次性】= 进程启动读取一次（static 初始化）；【每帧】= 帧循环内读取；
**优先级**: 多个机制控制同一行为时的胜负关系。
**维护纪律**: 新增 env 必须登记本表（名称 / 域 / lifetime / 语义 / 优先级）；
删除实现时同步删行。审计工具：`grep -rhoE 'std::getenv\("([A-Z0-9_]+)"\)' Engine EngineTest Tools | sort -u`。

## 1. GI / DDGI / SPGI

| 变量 | Lifetime | 语义 | 优先级/备注 |
|---|---|---|---|
| `DDGI_DUMP` | 一次性（测试） | frame 350 读回探针 SH；`1`=SH 摘要，`2`=全探针×射线路径 CSV（需 `DDGI_PATH_DEBUG`），`3`=GIGather 半分辨率 PPM | TestVulkanSponza 诊断 |
| `DDGI_PATH_DEBUG` | 一次性 | finalize kernel 换 `*_pathdbg` 变体（射线命中路径打标） | 配合 `DDGI_DUMP=2` |
| `DDGI_RAY_SAMPLE` | 一次性 | 探针射线采样模式（0=UNIFORM/1=JITTER/2=COSINE） | |
| `DDGI_DBG_PATH` | 一次性 | DDGI 路径调试输出（TestModularPipeline） | |
| `DDGI_RECT` | 一次性 | `DDGI_DUMP=3` 的任意统计窗口 `name:x0,y0,x1,y1;...` | |
| `DDGI_SILENCE` | 一次性 | 静默 DDGI 日志 | |
| `PRIMAL_DDGI_PARITY` | 每次编译 shader | Metal DDGI 优先加载 `*Parity.metal`（Vulkan 数学对齐版） | per-CompileShader |
| `PRIMAL_PROBE_HYSTERESIS` | 一次性（测试） | 探针滞回系数覆盖（默认 0.2） | |
| `PRIMAL_SPGI` | 一次性（测试） | Metal 上启用 SPGI 链（SetQualityOverride） | |
| `PRIMAL_SPGI_SKIP` | 一次性 | 强制走 monolith trace（A/B split 路径） | |
| `PRIMAL_SPGI_PARITY` | 一次性 | **no-op 死旋钮**（F4 在案；接线或删除待 M5） | |
| `SPGI_OFF` / `SPGI_SILENCE` | 一次性 | SPGI 关闭 / 静默日志 | SPGI_OFF 胜过 PRIMAL_SPGI |
| `SPGI_CAM` / `SPGI_CAM_LOCK` | 一次性（测试） | 相机机位覆盖（x,y,z,pitch,yaw）/锁定 | |
| `SPGI_NO_OFFLINE_SDF` | 一次性（测试） | 禁用离线 SDF（运行时体素化路径验证） | A1 验收用 |
| `SPGI_PROBE_SPHERES` / `SPGI_PROBE_VIS` / `SPGI_SDFVIZ` | 一次性（测试） | 探针球可视化 / 探针访存可视化 / SDF 体可视化 | |
| `SCREEN_PROBE_RAYS` | 一次性 | 每探针射线数覆盖 | |
| `GI_ALL_OFF` | 一次性（测试） | 全 GI 链关闭（消融基线） | |
| `GI_DEBUG_STRIP` | 一次性（测试） | fusion 输出按竖条切分各 GI 输入 | |
| `GI_ISOLATE` | 一次性 | FusionIndirect 消融变体选择（noSsgi/noDdgi/noSpgi） | 编译期变体 + env 选择 |
| `GI_VERIFY_FRAME` | 一次性（测试） | GIFusion 数值门禁触发帧（默认 90） | |
| `SSGI_FORCE` | 每帧 | SPGI 激活时仍保留 SSGI（A/B） | 胜过 SPGI 抑制策略 |
| `SSGI_OFF` / `SSGI_SILENCE` | 一次性 | SSGI 关闭 / 静默 | |
| `SSAO_DIRS` / `SSAO_SAMPLES` / `SSAO_SILENCE` | 一次性 | SSAO 质量 / 关闭 | |

## 2. Surface Cache / 离线 SDF

| 变量 | Lifetime | 语义 | 优先级/备注 |
|---|---|---|---|
| `PRIMAL_SC_BUDGET` | 一次性 | SC 页预算**上限**（0=全量）；与 governor 取 min（A6 仲裁） | governor 缩放值低于它时生效 |
| `SC_CARDVIEW` / `SC_CARDVIEW_BATCH` | 一次性 | 卡片视捕获启用 / 每批面数 | |
| `SC_DUMP_CARDS` / `SC_DUMP_INTERVAL` | 一次性 | 卡片转储 / 间隔 | |
| `SC_INJECT_OFF` / `SC_MIRROR_OFF` / `SC_MIN_CARD_RES` | 一次性 | 卡片注入/镜像诊断 | |
| `OFFLINE_SDF_SEAL` / `OFFLINE_SDF_VERIFY` / `PRIMAL_OFFLINE_SDF_ZERO` | 一次性 | 离线 SDF 缓存封存 / 校验 / 清零诊断 | |

## 3. 阴影 / 后处理

| 变量 | Lifetime | 语义 |
|---|---|---|
| `PRIMAL_SHADOW_PCSS` | 每帧 | PCSS 软阴影路径 |
| `PRIMAL_SHADOW_WHITE` | 一次性 | 阴影全 1（消融） |
| `PRIMAL_VSM_SHADOWS` | 一次性 | VSM moments 链 |
| `SHADOW_BLUR` / `SHADOW_QUALITY` | 一次性 | 模糊/质量 |
| `PRIMAL_BLOOM` / `TONE_OFF` / `PRIMAL_TONE_ACES` / `BLUR_OFF` / `TAA_OFF` | 一次性 | 各后处理开关/消融 |
| `PRIMAL_LEGACY_POST` | 一次性 | 旧后处理路径 |

## 4. RenderGraph / RHI（M1 新增）

| 变量 | Lifetime | 语义 | 备注 |
|---|---|---|---|
| `PRIMAL_RG_STRICT_IMPORT` | 一次性 | RG 单一物理 import 身份违例时断言（T1.1/C2） | 默认警告 |
| `PRIMAL_DEFERRED_FREE` | 一次性 | RHI 分配器 3 帧延迟槽位释放（T0.13） | **默认关**——与 MetalCommandBuffer 生命周期假设冲突（frame-1 vtable 清零 SEGV，待 M1 generation 方案完解） |

## 5. 诊断捕获 / 平台

| 变量 | Lifetime | 语义 |
|---|---|---|
| `PRIMAL_METAL_VALIDATE` | 一次性 | `0` 关闭 Metal API validation（默认开） |
| `PRIMAL_DUMP_MATRIX` | 一次性 | 矩阵真值打印（对账工具） |
| `PRIMAL_DUMP_GRAPH_MSL` | 一次性 | 转储 RG 生成的 MSL |
| `PRIMAL_GBUFFER_DEBUG` / `PRIMAL_DISABLE_GRAPH_DRAW` | 一次性 | GBuffer 调试 / 关闭图绘制 |
| `RG_PROF` | 一次性 | RG pass 计时 Top-N |
| `SPONZA_CAPTURE` / `SPONZA_CAPTURE_FRAME` / `SPONZA_DIAGNOSTIC_CAPTURE` / `SPONZA_REFERENCE` | 一次性（测试） | Sponza 截图 / 目标帧 / 参考图 |
| `ENGINE_SHADER_ROOT` / `PRIMAL_SHADER_ROOT` / `PRIMAL_BUILD_DIR` / `PRIMAL_VTBAKE_BIN` | 一次性 | 路径覆盖（shader 根 / 构建目录 / VT 烘焙器） |
| `PRIMAL_NO_HEAP_POOL` | 一次性 | Metal 内存池关闭（诊断） |
| `PRIMAL_IND_ASYNC` | 一次性 | Fusion indirect 异步路径 |
| `PRIMAL_LIGHT_DIR` / `PRIMAL_DIRECT_LIGHT` | 一次性（测试） | 光方向/颜色覆盖 |
| `SPONZA_*` 见上；`PRIMAL_GATHER` | 一次性（测试） | GI gather 从指定文件编译（A/B） |

## 6. 死旋钮在案（F4，待 M5 处置）

- `PRIMAL_SPGI_PARITY`（no-op）
- `max_gi_time_ms`、`ssdo_*`（5 字段）、`shadow_cascade_split`、`axes.directional/dfg_path`、
  `sc_capture/sc_lighting/sc_shadow` 面板开关、`fusion_glossy_strength`、`ssgi_ray_count`、
  `surface_cache_update_distance/importance_weight` —— 字段存在但无消费者（`LumenTypes.h`/`GIDebugParams.h`）

## 7. 诊断变体机制（统一约定）

1. **编译期变体**（同一源 + `-D` 宏 → 独立 `.spv`/独立文件名）：`GI_ISOLATE` 的
   FusionIndirect 三变体、`DDGI_PATH_DEBUG` 的 `DDGITraceFinalize_pathdbg.comp.spv`。
   Vulkan 由 `build_spv.sh` 产出；Metal 由 env 在 `LoadShaderBytecode` 处选择文件。
   新变体必须走此模式（单一源 + 变体产出），禁止手拷贝源文件。
2. **运行时分支**（shader 内 uniform 分支）：仅限单次 dispatch 级别的低成本开关
   （如 `RaySampleMode`）；高频内层循环禁止。
3. **文件替换**（`*Parity.metal` 模式）：仅用于对齐迁移期，迁移完成后删除。
