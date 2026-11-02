# Sponza / Lumen 主视口采集

固定和移动相机的纯计时入口统一为 `python3 Tools/LumenCompare/run_stutter_timing.py --help`（旧的 `scripts/run_vulkan_stutter_timing.py` 通过符号链接保留）；结果写入本地诊断报告目录，不用于采集帧图。

**当前排查入口：只抓本引擎原生启动前 6 帧。** 不预热、不提前发布几何、不重置 GI，也不强制放行剔除。

```sh
python3 Tools/LumenCompare/capture.py primal --output /tmp/primal-startup6 \
  --frames 1,2,3,4,5,6 --raw-startup --passes Culling,GI.GBuffer
python3 Tools/LumenCompare/startup_report.py --run /tmp/primal-startup6 --output /tmp/primal-startup6-report
```

保存四种 GBuffer，以及实际 culling 的 instance/cluster visibility、两种 GPU counter、visible list、DrawIndirect args、constants 和输入映射。报告验证 GPU counter、visibility flags、可见 ID 集合和间接参数相符，并输出每帧 CSV。

首帧 HZB 有效历史检查修复后，重采全 pass 使用 `--passes '*'`。对应浏览器：

```sh
python3 Tools/LumenCompare/capture.py primal --output /tmp/primal-allpasses \
  --frames 1,2,3,4,5,6 --raw-startup --passes '*'
python3 Tools/LumenCompare/capture.py archive --output /tmp/primal-allpasses --clean
python3 Tools/LumenCompare/pass_report.py --run /tmp/primal-allpasses --output /tmp/primal-allpasses-report
python3 Tools/LumenCompare/timeline_report.py --report /tmp/primal-allpasses-report
```

该页为每个实际登记输出生成 PNG 和 SHA/归档链接。纹理图按格式解码；buffer 仅生成明确标注的原始字节灰度图，原始数值通过 gzip 或 `restore` 取回。未执行 pass 与未登记私有资源不伪造截图。

`timeline_report.py` 将入口页改为全局帧进度条：拖动时所有 pass 同步切帧，支持播放、pass 选择、名称筛选与原始数据下载。原长页保留为 `all-passes.html`。页面内嵌数据，可直接用 `file://` 打开，不需要 HTTP 服务；未执行输出明确显示空缺，不借用其他帧填充。

旧双端首帧报告已发现元素缺失，不能作为 GI 收敛基线。资源准备原型尚需通过实际 GBuffer 完整度门禁；不要仅凭 `complete=true` 或 CPU 资源计数提升为有效基线。

固定采样主视图的 **1、5、35、75、150、360** 次提交。两端各用一个新进程连续运行，不将六个独立启动的截图拼成收敛过程。

Primal 的采集仅在 `LUMEN_COMPARE_DIR` 存在时启用，默认不改动渲染路径。配置见 `LumenCompareCapture.h`；默认采集 DDGI、Surface Cache、Screen Probe、Fusion、TAA、Tone 与额外注册的 GBuffer 数据。`LUMEN_COMPARE_PASSES` 可设置逗号分隔的大小写敏感名称片段；不要默认设为 `*`。

## 构建与运行

在仓库根目录运行。Vulkan/Metal 两棵构建树共用 `Darwin/Debug/libEngine.a`；切换树时先按 AGENTS.md 清除该归档再构建。

```sh
cmake --build Darwin/DebugVulkan --target TestVulkanSponzaRenderGraph -j2
python3 Tools/LumenCompare/capture.py primal --output /tmp/my-lumen-primal

"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin \
  -Plugin="$PWD/Tools/LumenCompare/Unreal/LumenCompareCapture/LumenCompareCapture.uplugin" \
  -Package=/tmp/my-lumen-plugin -TargetPlatforms=Mac

python3 Tools/LumenCompare/capture.py prepare-ue \
  --project "/path/to/DCC_MCP_Test.uproject" \
  --plugin /tmp/my-lumen-plugin --output /tmp/my-lumen-host

python3 Tools/LumenCompare/capture.py unreal \
  --engine "/Users/Shared/Epic Games/UE_5.7" \
  --project /tmp/my-lumen-host/LumenCompareHost.uproject \
  --output /tmp/my-lumen-unreal

python3 Tools/LumenCompare/report.py \
  --primal /tmp/my-lumen-primal --unreal /tmp/my-lumen-unreal \
  --output /tmp/my-lumen-report
```

输出目录必须是新目录，防止混入旧帧。顺序运行两个引擎。UE 临时工程通过 Content 符号链接复用原工程，生成文件写入临时工程；采集插件不保存或修改原关卡。

本入口针对现有 `/Game/Sponza/Maps/Sponza_VulkanReference` 与 `CameraActor_0`。不是任意 UE 工程的自动建场景工具。主视图 2560×1440；Primal 使用垂直 FOV 75°，UE 插件换算为水平 FOV。UE 使用软件 Lumen、100% screen percentage、关闭动态分辨率；不削减射线或更新预算。固定步长 .016 秒对应 Primal 现有 GI delta override；UE 首次引擎 tick 前的启动 Draw 仍可能使用 .033333 秒，须以原生 Frame scope 记录为准。

## 保存内容与验证边界

- Primal：实际生产者之后立即复制，避免后续 PASS 覆盖同一物理分配。buffer 与 texture 显式区分；等待同 graphics queue 完成后读 Readback。记录 ordinal、引擎时钟、GI age、PASS、RG index/version、物理 handle、格式、尺寸、字节数。RG 当前没有真正的多版本管理，`version=0` 不代表同一写入；文件中的生产者序号用于区分。原始 buffer 包含未更新槽位，必须结合 update list / 有效位解读。
- UE：`BeginRenderViewFamily` 开启 DumpGPU，`GameViewport.OnEndDraw` 结束，避免启动阶段额外 Draw 混入。分别保存主视图 ordinal、UE 原生 FrameNumber、引擎 tick counter、view state、相机矩阵和闭合记录。六帧原生 FrameNumber 必须分别等于请求帧；不依靠 Python/Slate tick 或截图请求计数。
- UE 原生 `Status=ok` 仅证明转储任务完成，不证明所有资源体都存在。`inventory.json` 单独列出每个输出引用的文件状态。当前 UE 5.7 的 Radiance Cache 3D indirection texture 数据体缺失；不得填零、声称完整 Lumen 状态可重放或据此认定漏光。
- Primal 的 `complete=true` 范围是配置的 RG 输出和具名内部 dispatch 采样点，不代表所有私有分配或每次内部 shader 调用。当前单 mip、单 array layer 的常用 2D/3D 格式可读回；其余格式/布局明确报缺口。
- 两端 atlas、探针索引和 SH 表示不同。先按 `pass-map.json` 建立世界空间接收点、方向、材料和曝光对应，才能比较数值。最终 PNG 只用于观察显示结果，不作为 GI 能量或因果证据。
- DumpGPU 和读回会扰动执行时间；这些运行不用于比较性能。

## 归档与选取恢复

先生成 inventory / 报告，再归档。归档保留原目录逻辑路径，以未压缩 SHA-256 对象去重。全部对象独立解压验证后才允许清理本轮 raw/bin；保留 JSON、PPM/截图、日志和清理空间记录。不要对仓库根目录或其他会话的目录执行归档。

```sh
python3 Tools/LumenCompare/capture.py archive --output /tmp/my-lumen-primal --clean
python3 Tools/LumenCompare/capture.py archive --output /tmp/my-lumen-unreal --clean

# 根据 raw-manifest.json 选择一个原始路径恢复，不默认展开整轮数据。
python3 Tools/LumenCompare/capture.py restore \
  --output /tmp/my-lumen-primal --destination /tmp/my-selected-buffer \
  --path frame-360/pass-REPLACE-resource-REPLACE-v0.raw

# 报告支持从 SHA 归档重建 Primal 显示截图。
python3 -m unittest discover -s Tools/LumenCompare -p test_capture.py -v
```

归档对象应视为只读；恢复写入独立目录。相同目标已有不同内容时拒绝覆盖。
