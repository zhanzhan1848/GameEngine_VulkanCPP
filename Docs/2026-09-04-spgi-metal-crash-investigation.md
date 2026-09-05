# SPGI Metal 崩溃修复记录

## 已确认并修正

- HZB 描述符：原先向 5 元素数组复制 6 个描述符，再提交 6 个，构成 CPU 栈越界。现在使用推导长度的数组，并从数组长度计算提交数量。
- Average 归约：默认 48 rays/probe，但共享归约数组与循环固定 64。Metal 原先仅派发 48 个线程，读取未初始化的共享槽 48..63；Vulkan/WGSL 固定 64 个线程，原先无条件读取射线，导致跨探针读取以及末探针越界。现在统一 64 个线程，超过实际射线数的线程不读取输入，以零贡献参与全部 barrier 和归约。初始化拒绝 0 或超过 64 的射线配置及零 downsample。
- FusionComposite：SPGI 只传 RenderGraph handle 时，原先仍绑定黑纹理。现在先解析 SPGI RenderGraph 输出，再使用黑纹理作为缺失资源的兜底。

## 证据与限制

用户提供的 WindowServer 报告记录 WATCHDOG termination：40 秒未成功 checkin。修复 HZB 描述符后、修复 Average 前的一次整链路运行在第 3 帧后等待 GPU fence，随后用户报告桌面会话崩溃。该报告不能单独证明具体 shader 是系统级故障根因。

Average 修复后未再次运行 GPU 测试；未验证运行稳定性或 Metal/Vulkan 图像一致性。没有默认开启 SPGI，也没有通过关闭 SPGI 宣称问题解决。

## 已完成的离线验证

- TestModularPipeline 构建通过。
- ScreenProbeAverage.metal 经 xcrun metal 离线编译通过。
- ScreenProbeAverage.comp 重新生成 SPIR-V，保留 screen_probe_average 入口，spirv-val 通过。
- StandaloneRHI_Test 与 StandaloneRHI_Device_Test 两项 CPU 测试通过；这些测试不覆盖 shader 执行。
- 顶层 ctest -R Unit 未匹配任何测试，不能算单测通过；以上两项使用 UnitTests 子目录按实际测试名称运行。

后续运行验证必须另外安排；本次未继续启动可能影响 WindowServer 的整链路测试。

## 第二轮：跨帧资源读写

- Temporal 输出改为独立的三缓冲 scratch。EMAVarianceClamp 读取当前帧邻居时，不再与邻居线程的写入别名。
- Spatial 从 Temporal 输出读取，写回当前帧 SH；不再覆盖 histIdx。Gather 使用实际滤波输出，下一帧的 SH 与位置/法线槽位对应。可选滤波 pipeline 缺失时，把最终输出复制回当前历史槽位。
- 无 HZB 时移除每帧 CPU MapBuffer/memset hit buffer。SDF 根据 hzb_params.z 判断是否需要保留 HZB 命中，无 HZB 时在 GPU 上逐射线覆盖输出。HZB 可用性同时检查 pipeline 和纹理。
- 新增 scratch 占用约 2.64 MiB（160×90 探针、三帧、每探针四个 float4）。
- 修改后的 TraceRays.metal 离线编译通过；TraceSDF SPIR-V 重新生成并通过 spirv-val。未进行 GPU 运行验证，仍不能声称系统级崩溃已消除。

## 第三轮：缓存元数据校验

- SPGI 不再将空 SurfaceCacheView 标记为可用。只有 atlas、card data、lookup、grid offsets、grid cards 和尺寸完整时才启用卡片查询；缺失时沿现有前帧颜色回退路径处理。
- DDGI fallback 拒绝无效句柄、零/负/非整数/非有限探针数量、无效间距或原点，以及九个 SH 系数地址可能溢出的网格。
- 新增 UnitScreenProbeCacheValidation：纯 CPU 输入边界测试，不创建图形设备。与两项 StandaloneRHI 测试一起通过（3/3）。
- 修正 HZB 字段偏移注释为 384，并用 static_assert 固定全局常量尺寸和 DDGI/HZB/cache 字段偏移。
- CMake 开启 CMAKE_SUPPRESS_REGENERATION，添加测试后已手动重新配置 Darwin/Debug；现在顶层 CTest 可以找到该测试。

## 再次运行失败（2026-09-04 15:56）

在前述修复之后，经用户同意进行了一轮短时 SPGI 运行。日志显示第 3 帧后 frame 0 fence 超过 1 秒仍未信号。外部监测发送 TERM 和 KILL，均未在等待窗口内确认退出。用户随后提供的 WindowServer 报告记录 15:56:05 的 WATCHDOG termination：主线程及 HIDEvents 队列失去响应，40 秒未 checkin。这些修复没有消除系统级故障。/tmp 运行日志在随后环境中已不存在，后续应将证据保存在项目目录。

## 第四轮：诊断与优化构建校验

- 现有 SPGI stage mark 是 CPU 命令录制日志，不能证明 GPU 已运行到该阶段；将 done 改为 encoded (CPU)。
- Metal debug command buffer 开启 EncoderExecutionStatus；compute pipeline 绑定记录 shader 函数名作为 encoder label/signpost。错误回调输出编码器状态及 signposts。该机制依赖驱动返回错误，不能保证在驱动卡死时收到回调，也不是系统级防护。
- 对缓存校验测试启用 -O2 -ffast-math 后，原有 std::isfinite 校验确实失败：非法间距及 NaN 原点被放行。
- 改为从 const float& 复制 IEEE-754 指数位后检查，避免 fast-math 的按值浮点参数有限值假设。相同优化配置下测试由失败变为通过。
- 本轮没有启动 GPU 测试，系统级卡死根因仍未确认。

## 第五轮：验证层未开启及缓冲初始化分支失效

- 检查 MetalDevice::initializeImpl 确认，DeviceDesc.enableDebug 不负责加载 Apple API/Shader Validation 层。先前仅设置 PRIMAL_METAL_VALIDATE=1 的运行，不能视为已启用这些层。
- TestModularPipeline/TestMetalGIFusion 的 macOS main 现在在 Cocoa 初始化前设置 MTL_DEBUG_LAYER=1、ERROR_MODE=assert；SPGI 启用时同时请求 Shader Validation、stderr 报告、zerofill 及 ABORT_ON_FAULT。该请求是否实际生效仍需运行时证据，不能保证防止驱动卡死。
- Apple 官方 API Validation 文档：https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage
- Apple 官方 Shader Validation 文档：https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage/
- MetalBuffer 原先用 options & ResourceStorageModeShared 判断是否清零，但仓库 SDK 中该常量为 0；分支恒假。改用原生 buffer 的 storageMode() == StorageModeShared。该缺陷会破坏现有的共享缓冲初始化意图，但尚无证据把系统故障唯一归因于此。
- TestModularPipeline 编译通过，三项 CPU 测试通过；未启动 GPU 测试。

## Xcode 控制台报错处理

从 Xcode UI 读取到 UBSan 未对齐读取、NS::String release 转换告警以及 Metal 全局变量地址空间错误。本轮未在 Xcode 重新启动进程。

- SceneDataAdapter：以 memcpy 实现标量读取，替换 BlobReader、格式探测、MSHL/SDF/UVLM 标记和诊断 header 的未对齐 typed load。修改已有 MockDevice 测试，使输入基址偏移一个字节，并为该测试启用 alignment sanitizer 和不可恢复模式。修改前在 SceneDataAdapter.cpp:26 复现 UBSan 终止，修改后通过。
- GBufferVT.metal：离线编译复现 program scope variable must reside in constant address space，定位到 InvIntervals。改成 constant 地址空间后离线编译通过（有一条未使用常量警告）。
- MetalShader：不涉及 Metal/GPU 的 Foundation 小程序用短字符串 blurCS 复现 alloc/init 后 release 时的 metal-cpp 对齐告警。使用 NS::String::string 自动释放版本通过同样的 alignment sanitizer。入口名称改用该方式，没有关闭 sanitizer，也没有把 tagged pointer 告警归因于 GPU 越界。
- TestModularPipeline 构建通过；SceneDataAdapter、SPGI cache 和两项 RHI CPU 测试合计 4/4 通过。
- Xcode 的编译器 stack nearly exhausted 警告仍需后续关注；上述修复不构成系统级卡死已消除的证据。

## Xcode 再次运行：仍出现 GPU 等待停滞

2026-09-04，经用户要求通过 Xcode Run 启动 TestModularPipeline（PID 14943），保留现有 API/Shader Validation 和 sanitizer 配置。启动日志包含 `API validation requested; shader validation=1`、`SPGI enabled via PRIMAL_SPGI`，场景加载 393 个 mesh。

- 控制台观察到 SPGI Frame 19，随后仍有 SurfaceCache 和 RenderSystem 输出，以及 `DDGI-Perf samples=60`。这些不是完整 GPU 完成计数。
- 此后 frame 1 fence 连续报告等待 1/5/10/15/20 秒，点击 Xcode Stop 后最终日志到 25 秒。没有在所读取的控制台片段中发现验证断言；不能因此断定整个运行无验证错误。
- Xcode 随后显示 `Debug session ended with code 9: killed`、`Program ended with exit code: 9`、Finished running，Stop 按钮禁用。退出码来自主动停止，不能算测试通过。未继续重试。
- 本轮证实 Xcode 配置下仍可出现 GPU fence 长时间不完成；先前的“可以运行”不足以证明长期稳定。仍需定位具体 GPU 工作负载或同步问题，不能认定 SPGI 系统级故障已修复。

## 后续同步检查与残留进程

- MetalCommandBuffer::InsertBarrier 以 `if texture ... else if buffer` 判断无类型 ResourceHandle，而两种资源来自独立 allocator，数字 ID 可以重叠。改为独立检查并合并两个 scope，避免漏掉 buffer barrier。该修正是真实缺陷修复，尚未证明它是本次 GPU 停滞的根因。
- 用 Xcode 启动修正版（PID 16750），只观察到第一条 Metal Diagnostics deployment target 提示，约 30 秒没有进入引擎初始化。主动 Stop，显示 code 9。没有形成有效渲染回归。
- 随后 `ps -p 14943,16750 -o pid,ppid,stat,etime,comm` 显示 PID 14943 仍存在：PPID 1，STAT `?E`，elapsed 25:52；16750 已不存在。因此前轮 Xcode 的 Finished running 仅能证明调试会话结束，不能证明测试进程已完全退出。停止继续 GPU 重试，后续运行前需重启系统清除残留状态。
- MetalSync::Wait 原先遗漏释放调用方的 dispatch semaphore 引用，listener 创建失败时也只释放两份引用中的一份；补齐释放。超时后回调仍持有独立引用，避免释放后访问。
- fence 记录并 retain 对应原生 command buffer，下一次提交或销毁时释放；每个目标值首次超时记录 target、signaled、commandBuffer 状态及 error。该诊断用于区分提交/执行停滞和完成后的 fence 异常，不依赖已回收的 RHI wrapper。
- TestModularPipeline 重新构建成功，四项现有 CPU 回归通过，git diff --check 通过。CPU 回归不覆盖真实 Metal 同步，也不能替代重启后的 GPU 验证。本轮未运行 Vulkan。

## 重启后捕获的确定越界与内存池耗尽

- Xcode 启动 PID 5313，SPGI Frame 6 附近 Shader Validation 暂停：`Invalid device load at offset 34359738368`，`card_capture_vertex /program_source:101:16`，buffer length=36448。这一行对应 `cmap[global_cluster_id]`；偏移除以 uint4 的 16 字节得到非法 cluster ID 0x80000000。主动停止后 ps 确认 PID 已不存在。
- 初始化的第一个 fence 曾等待 1496.76ms 后自行恢复，新增诊断为 target=1 signaled=0 commandBuffer=scheduled error=none。因此单次 1 秒超时不等同于永久 GPU 停滞。
- 根因候选有明确的代码链：card capture 96 个可见列表各 4MiB，合计 384MiB，超过 Metal 256MiB heap。RHIAdaptiveMemoryPool::Allocate 耗尽返回 u32_invalid_id (0xFFFFFFFF)，MetalDevice 却用 handle!=0 判成功。GetMemoryBlock(非法句柄)返回空块，offset=0，随后 heap newBuffer 在 offset=0 反复覆盖已有资源。可解释 cluster ID 被浮点位模式污染。
- 修正为检查 u32_invalid_id 并确认 IsValidBlock，池耗尽时不设置 heap allocation，让 MetalBuffer 走现有独立 newBuffer 路径。SPGI 和 card capture 保持开启。
- 添加 CPU pool exhaustion 测试，确认失败 sentinel、活跃块不变以及释放容量后能恢复分配。TestModularPipeline 编译成功，五项 CPU 测试通过。待 Xcode 验证修正后的 GPU 行为。

- 修正 pool sentinel 后，PID 7386 暴露下一处 card_capture_vertex 越界：program_source:122 和 124（读取三角形局部索引，再查顶点映射）。例如 length=786804 的三角形缓冲读 offset=1564768，length=1055964 的顶点映射缓冲读 offset=673984524。验证层暂停，主动 Stop，ps 确认退出。
- CPU `mergedTriangles` 是 `vector<u8>`，上传也是 sizeof(u8)，CardCapture.metal 却声明 `device const uint* mts`，将四个局部顶点索引拼成一个大整数，且寻址步长放大四倍。改成 `device const uchar*`；Vulkan CardCapture.vert 使用 uint SSBO，改成按字节位置选择 word 并移位解包，保持同一数据格式。修正 CardMeshlet 的尺寸注释为 64 字节（结构本身无需改动）。
- Vulkan shader 经 glslangValidator 编译及 spirv-val 校验通过（输出在 /tmp，仅验证）。Metal 离线工具链当前报告 missing Metal Toolchain，沙箱外同样失败；没有下载组件，改由 Xcode 启动时的 Metal 运行时编译验证。后续运行成功推进超过 Frame 240，继续观察中。
