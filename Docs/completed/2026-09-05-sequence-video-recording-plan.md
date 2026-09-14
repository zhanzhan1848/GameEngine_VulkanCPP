# 2026-09-05 视频录制模块规划与落地：A. Sequence 拍片（Sequencer+MRQ 对标） + B. Run 后渲染录屏（分析用）

> 状态：Part B 已落地 + Part A 一期已落地（本文档为实施后归档，含实测结论）
> 日期：2026-09-05
> 分支基线：dev

两部分相互独立、共享同一套帧序列/编码/遥测惯例（**PNG 帧序列是主产物**、ffmpeg 可选、逐帧亮度遥测），可分别落地。实施顺序：先 B 后 A。

---

## Part B：Run 后渲染录屏（PrimalPlayer 逐帧录制，供后续分析）

### 关键判断：零引擎改动

PrimalPlayer 本来就把每帧渲染进**离屏 RGBA16F RenderTarget** 再 Blit 到窗口
（`Tools/PrimalPlayer/main.cpp` 主循环），捕获点天然存在；`SaveRenderTargetAsPng` /
`DebugReadRenderTarget` 已由 EngineDLL 导出。player 侧只在 extern 声明镜像里追加函数
（"只镜像函数声明、不镜像 POD"纪律不变）。

录制模式语义 = **渲染帧与捕获帧 1:1**：主循环每帧 `PipelineRenderFrame` 之后对 RT 调
`SaveRenderTargetAsPng`，一张 PNG 对应一次真实渲染、无丢帧（外部屏幕录制做不到）。
读回+PNG 编码拖慢循环是分析模式的可接受代价；GI 收敛/闪烁类分析正需要逐帧完整性。

### PrimalPlayer（`Tools/PrimalPlayer/main.cpp`）

- `--record <dir>`：逐帧 PNG 落盘（`frame_%04d.png`，目录自动创建）
- `--record-frames N`：捕获帧上限（计捕获数，受 stride 影响）；**录满即退出进程**
  （编辑器侧依赖"进程退出 = 录制完成"契约）；缺省 0 = 跟随窗口关闭
- `--record-every N`：帧步长，缺省 1
- `--record-metrics <file.csv>`：逐帧亮度遥测 CSV（`frame,meanLuma,minLuma,maxLuma,captureMs`，
  RGBA16F 二次读回后按 Rec.709 线性亮度统计，**HDR 原值不截断，口径对齐 editor.metrics/FrameMetrics**，
  两条通道的数字可直接对比）；逐帧 fflush（崩溃取证不丢尾部样本）
- 半精度转换 `HalfToFloat` 不依赖 `_Float16`（Linux x86 工具链支持不齐），已知位型单测过
- stderr 每 60 帧报进度；退出时 `record summary: frames/rendered/avgCaptureMs/dir`
- **修复存量缺陷**：`player_window.mm` 从未列入 `add_executable`——该目标此前从未成功链接过

### 编辑器集成

- `Common/RuntimeLauncher.cs`：`PlayerRecordOptions` + `AppendRecordArguments`（纯函数，单测覆盖）
  + `ResolveRecordDirectory`（`<project>/Captures/<scene>-<yyyyMMdd-HHmmss>`，场景名跨平台净化）；
  `Run()` 读 VM 的 `RecordOnRun/RecordFrames/RecordMetrics` 拼参数
- `Views/WorldEditorView.axaml`：API 下拉旁加 `Rec` 开关 + 帧数输入
- 录制 summary 经 player stderr → `BuildService.OnOutput` 自动流入 BuildLog 面板；
  mp4 编码入口由 Part A 的 `Common/VideoEncoder.cs` 统一提供

### 验证实录

- 60 帧 @640x360：60 PNG + 61 行 CSV（含表头）+ summary，录满即退
- stride：`--record-every 2 --record-frames 8` → rendered=15、captured=8 ✓
- C# 单测 9 项（参数拼装/目录净化/构建树映射）全绿

---

## Part A：Sequence 拍片模块（对标 Sequencer + Movie Render Queue 一期）

### 范围决策

- 一期轨道 = **CameraCutTrack（分镜头切相机）+ TransformTrack（实体位置/旋转/缩放
  关键帧；相机运镜 = 相机实体的 Transform 轨）**；float 属性轨/曲线编辑归二期
- 渲染走**编辑器进程内**通道（复用已加载场景与离屏捕获链）；序列资产是 JSON、
  求值器纯函数化，天生进程无关，为将来 `primal-engine render-sequence` 独立进程预留
- **一期零 C++ 引擎改动**

### 数据模型（`PrimalEditor_Avalonia/Sequence/`）

- `SequenceDocument.cs`：Name/Fps(1..120)/DurationFrames/Tracks；轨道多态用
  `[JsonDerivedType]`（"type" 判别符，同 `Component.cs` 惯例）；`.pseq` 独立文件存
  `<project>/Sequences/`（同 Scenes/ 惯例，不进 .scene、不触碰 Serializer 的组件
  type-switch）；版本字段拒载过新格式。**时间是帧号轴**——fps 只决定帧↔秒换算与
  输出速率，不参与求值
- 关键帧：`{Frame, Position[3]米, RotationEuler[3]度, Scale[3], Interp: Linear|Constant}`
  （Interp 描述"本帧→右邻帧"过渡）；`CameraShot` 半开区间 `[Start, End)`
- `SequenceEvaluator.cs`：纯函数（无状态/无随机/无墙钟）——同一序列同一帧任意次求值
  逐位一致，是确定性 DoD 的根基；帧区间外 clamp；同帧双 key 不产生 0/0
- `SequenceUndoActions.cs`：轨/键/shot 六个动作实现 `IUndoRedoAction`（同帧替换型
  AddKey 精确往返）；动作只改文档，求值应用由驱动器负责
- `SequenceDriver.cs`：ApplyFrame（求值→`TransformData.ApplyTo`+`PushTransformsToNative`，
  与属性面板/entity.set-transform 同一同步通道；播放/seek 是播放态，**有意不推 undo**）；
  CapturePoses/RestorePoses（录制前后场景无残留）
- `SequenceEditorState.cs`：当前打开序列/播放头/播放态的唯一事实源（面板与 CLI 共享，
  全部经 UI 线程——CliHost 已编组）

### CLI 命令面（`Cli/SequenceCommands.cs`，14 个 verb，§6.3 描述规范）

- 生命周期：`sequence.new / open / save / info`
- 编辑（推 undo，复合操作用 `BeginTransaction` 合成一步——如 add-key 自动建轨）：
  `sequence.add-transform-track / add-camera-cut / add-key`（pos/rot/scale 可省=取当前
  pose，即 K 帧）/ `remove-track / remove-key`
- 播放态（不入 undo）：`sequence.set-frame`（越界 clamp 不报错，对齐视频播放器 seek 语义）/
  `play / pause / stop`（`SequencerController` 单例 DispatcherTimer，面板与 CLI 共用）
- 离线渲染：`sequence.render`（下述）

### Movie Render Queue 一期：`sequence.render`

逐帧 `seek → ApplyFrame → 按相机 cut 取景（无 cut 帧回落编辑器相机）→ TryRenderSequencePng`；
分辨率 = 3D 视口当前分辨率；渲染前快照 pose + 播放头，结束恢复（场景与播放头无残留）；
帧间 `Dispatcher.InvokeAsync` 让出排空 socket 队列（editor.metrics 同款）；帧数上限 3600；
可选 `video=x.mp4` 走 ffmpeg。`EngineViewport3D.TryRenderSequencePng(path, sceneCameraEntityId)`
与 TryCaptureScreenshot 同套 `_renderInProgress` 互锁，相机 cut 逐帧切机位且不改视口
look-through 状态。

### 时间轴面板（`Views/Sequencer/`）

- 底部面板（工具栏 `SEQ` 开关切换，Row3）；传输条（|< < Play > >|、帧数显示）+ 文件
  （New/Open…/Save）+ 编辑（K / +Track / +Cut）+ Render…
- Canvas 自绘标尺/相机 cut 段/关键帧菱形/playhead；左键拖拽 scrub（拖动即求值应用预览）；
  滚轮逐帧步进（Shift ×10）；右键删 key/删 shot；相机 cut 自动切机位
- 按钮内部调用 `SequenceCommands` 的方法（§5.2 "UI 按钮 = 对自家命令的内部调用"——
  与 CLI 完全同路径、同错误语义、同 undo）

### 共用组件

- `Common/VideoEncoder.cs`：从 `FrameCaptureCommands.EncodeVideo` 抽取公共化（+framerate
  参数、ffmpeg 参数构造纯函数化供单测）；`editor.record`/`sequence.render` 共用

### 资产面

- `AssetType.Sequence` + `.pseq` 映射（`Content/ContentHelper.cs`）——序列若放进 Content/
  也会被注册表正确分类；`asset.list` 免改
- 一期序列默认存 `<project>/Sequences/`（面板 Open 对话框不受限），content browser 双击
  编辑入口归二期

### 测试与门禁

- `SequenceEvaluationTests`（13 项）：线性/阶梯插值、clamp、同帧双 key、cut 半开区间/
  空隙回落/无效 shot、序列化 roundtrip、版本拒载、undo 精确往返
- `CliSequenceCommandTests`（13 项）：生命周期落盘/回读、K 帧取当前 pose、undo 一步撤销
  复合操作、set-frame 求值→应用链（真文档模型断言）、clamp 语义、render/play 守卫路径
- `RuntimeLauncherRecordTests`（9 项，Part B）
- schema 快照门禁已把 `SequenceCommands` 纳入 `CliSchemaSnapshotTests.BuildCanonicalSchema`，
  快照随本模块提交（`Testing/CliSchema/editor-schema-snapshot.json`）
- 确定性口径：同序列两遍渲染同帧 `editor.diff` ——`identical` 为位级一致；
  smoke 用 `within-threshold`（阈值内 0 差异像素）容忍 GI 时域收敛类噪声，真闪烁由
  changedPixelRatio 捕获

---

## 已验证 / 待验证

**已实测**：PrimalPlayer 60 帧录制 + stride + CSV 遥测 + 录满即退；C# 全量套件
670+ 通过（本模块新增 31 项全绿）；`primal selftest` 通过；引擎 `ctest -R Unit` 通过；
`primal-engine engine info` 离线通道不受影响。

**待用户 GUI 会话验证**：`./scripts/primal_smoke.sh`（已加 sequence 闭环分支：建轨→K 帧→
seek→离线渲染×2→确定性比对→落盘）。本自动化环境无法启动 Avalonia 编辑器
（CVDisplayLink -6661，非 Aqua 会话），需人工执行：

```bash
PRIMAL_OPEN_PROJECT=$PWD/projects_test/TestProject_01/TestProject_01.primal \
  Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor_Avalonia &
./scripts/primal_smoke.sh
```

## 实测发现的存量问题（与本模块无关，记录待查）

1. **空场景（无网格实体）渲染输出全 NaN**：mode 0/3 均复现（录制器如实上报 meanLuma=nan，
   PNG 走 NaN→0 映射为黑帧）——建议排查 Deferred 清屏/环境项的退化路径。
2. **PrimalPlayer 退出后 mutex abort**：`PipelineRenderFrame` 全部正常、summary 输出、
   "clean exit" 之后 `std::system_error: mutex lock failed`——进程拆除期（静态析构/
   后台线程）问题；player 此前从未链接成功，属首次可见。
3. **Run 链路 mesh 导入被拒**：`ImportMeshAsync` 对 FBX 源路径返回 0（player 的
   primal-runtime-scene v1 里 mesh 实体进不了渲染）——Run+录制在含网格场景下的可用性
   依赖此缺口修复。
4. **`PrimalPlayer` 此前从未成功链接**（player_window.mm 未列入 add_executable）——本模块已修。

## 二期路线图

float 属性轨（FOV/灯光强度）、平滑插值+曲线编辑器、任意分辨率输出（独立 CreateRenderTarget，
需验证非视口 RT 的 viewport 推导）、VideoToolbox 原生编码、脚本 fixed-dt 驱动（需引擎加
`TickScripts(dt)` 导出）、独立离线渲染进程（前置：offline 通道场景加载）、EXR/运动模糊、
`run.record` CLI 命令、player 录制走 RHIReadbackRing 异步回读、content browser 双击 .pseq
编辑入口、相机 cut 轨的时长拖拽编辑。
