# DLSS SR 权重捕获：完整经验、调试历程与环境边界定论

日期：2026-09-24
状态：定论（容器硬边界已严格证实，工具链全部就绪，换环境即可 10 分钟收尾）
前置：`Docs/2026-09-22-neural-rendering-inference-training-plan.md`（R1-R4 主计划）

---

## 0. 一句话结论

在 compute-only GPU 云容器上，NVIDIA NGX DLSS-SR 运行时初始化**必然**返回
`FAIL_FeatureNotSupported (0xBAD00001)`——原因是容器未挂载驱动的 NGX 能力
（snippet 编译内核 + NGX 服务文件）。这是 NVIDIA 容器驱动挂载机制的硬边界，
无软件绕过。`Tools/DLSSCapture/` 工具链已全部就绪，在**任何带 ngx 驱动能力
或物理显示器的 NVIDIA Linux 机器**上均可 10 分钟完成 capture。

---

## 1. MLX-DLSS 四种权重挖掘方式（源码考古结论）

MLX-DLSS（github.com/iamwavecut/MLX-DLSS，Apache 2.0）按模型类型用四种不同
技术从 NVIDIA 闭源库提取权重：

### 1.1 NR（nvngx_dlssnr.dll）——PE 资源直读（唯一"无损"路径）

NVIDIA 把权重作为 **Windows PE 的 RCDATA 资源**内嵌在 DLL 里，资源名
`WEIGHTS_HT`。提取器是标准 PE 解析器：

```
DOS 头(0x3C) → PE 签名 → COFF → 节表 → 资源目录(RVA) → RCDATA
  → 按名取 WEIGHTS_HT payload → 序列化 weight map 逐张量解析
```

每张量记录：名字长度(u64) + 名字(UTF-8) + record_size(u64) + byte_count(u64)
+ dtype_code(u32) + 维度数(u32) + 各维(u32×N) + 原始字节。切出后直接存为
"packed safetensors"——厂商打包布局原样保留。再由 `weight_spec.json`
（v8-v18 版本化布局规范）解码成逻辑张量。

**这条路的成本最低，因为容器是自描述的。** 对应工具：`mlxdlss-weights extract`
+ `decode`。我们的 649 张量 / 145.7M 参数 NR 权重就是这么来的。

### 1.2 FG（libnvidia-ngx-dlssg.so）——ELF 硬编码偏移表

权重以 **GPU kernel 的私有打包布局**散布在 .so 数据段：
- `custom_*` 卷积：`[co/8][tap][ci/8][co%8][ci%8]`
- `block1.stem0`：plain `[co][tap][ci]`（Cin=18）
- U-Net `k_conv_fp16_nhwc`：mma.m16n8k16 A-fragment（用查表 `FRAG=[0,1,4,5,8,9,...]` 还原顺序）

作者手工定位了全部 42 层的 weight/bias 字节偏移（`SPEC_310_7_0` 表），
逆变换成 dense `[Cout,Cin,kh,kw]`。SHA 钉死 build——**换一个 build 偏移表
全部作废**。我们的 libnvidia-ngx-dlssg.so.310.7.0 匹配，提取成功。

### 1.3 RTX VSR（Linux 驱动库）——同 FG 的静态偏移法

66 张量、OHWI fp16 布局、硬编码偏移，与 GPU 捕获逐字节对拍验证。
SHA 钉定 `c7f2387a…`（NVIDIA Linux driver 580.173.02 内的 VSR 1.8.2 库）。
**纯静态提取，不需要 capture**——但需要找到那个特定版本的驱动文件。

### 1.4 DLSS SR（libnvidia-ngx-dlss.so）——混合法（唯一走 capture 的）

**Shader 部分（静态）**：从库的固定偏移（entry=25420872）读 LZ4 压缩的 PTX
（CUDA 后处理 kernel），自写裸 LZ4 块解码器（44,810 B → 149,075 B），再用
`ptx_metal.py` 把 PTX 转译成 Metal 源码。

**权重部分（动态）**：网络权重**运行时组装**（静态数据段没有），必须在
NVIDIA GPU Linux 机器上跑一次 first-frame 推理，用 LD_PRELOAD 钩
`cuLaunchKernel` 把参数块和显存 buffer dump 下来：
- 权重 blob 3,545,824 B（launch 1 参数 64 指向的 buffer）
- 重建滤波器 65,536 B（launch 12 参数 280 指向的 buffer）

转译后 `package-sr` 校验权重/滤波器 SHA → 输出 `.srmodel`。

**MLX-DLSS 文档原话**："SR extraction directly from the library on a Mac
is not implemented"——必须走 capture。

---

## 2. 为什么我们的环境走不通（完整因果链）

### 2.1 库版本排查（排除过程）

| SHA-256 前 8 | 来源 | 是否 fc19b68（SR 钉定） |
|---|---|---|
| `0c353084` | GitHub NVIDIA/DLSS v310.7.0 release 原始文件 | ❌ |
| `a9b85a15` | 用户后来更新的版本 | ❌ |
| `fc19b68` | **目标**（MLX-DLSS 钉定） | — |

NVIDIA 同版本号发布多个 build（不同架构/配置），工具按 SHA 硬校验。
但注意：**capture 权重身份由 WEIGHTS_SHA256 保证（ddaa59cd…），库版本
只影响 shader 提取**——capture 本身理论上可在任何 build 上跑通
（只要 NGX 运行时能启动）。

### 2.2 容器环境限制（根因）

这台云容器（RTX 5090 / 驱动 610.43.02 / CUDA UMD 13.3）是 **compute-only
驱动挂载**——NVIDIA 容器运行时按 `NVIDIA_DRIVER_CAPABILITIES` 控制挂载
哪些驱动用户态库。当前只挂了 compute/utility：

- **缺失**：`libnvidia-ngx.so`（驱动 NGX 服务库）→ 已手动从 /tmp/nvdrv 补拷
- **缺失**：驱动的 NGX snippet 编译内核 → 无处可找（只存在于宿主驱动安装）
- **缺失**：`/dev/nvidia-modeset` 等图形设备节点 → 已用 mknod 补造
- **缺失**：`/dev/dri`（DRM 渲染节点）→ 容器未挂，Mesa/NGX 均探测失败

### 2.3 NGX 运行时初始化失败（最终死因）

```
NVSDK_NGX_VULKAN_Init(appid, wpath, VkInstance, VkPhysicalDevice, VkDevice, ver)
  → NGX 运行时查询驱动 NGX snippet 支持
  → 查不到（容器无 NGX 文件/服务）→ FAIL_FeatureNotSupported (0xBAD00001)
  → 所有 SDK 版本 (0x15/0x1E/0x20/0x28/0x30) 均秒拒
  → NGX 日志目录为空（运行时在日志初始化前就放弃了）
```

**对照实验**：官方裸 demo（无 LD_PRELOAD）同样死在
`getSurfacePresentModesKHR: ErrorUnknown`——排除了我方工具的干扰。

### 2.4 已确认非问题的干扰项

| 现象 | 定性 |
|---|---|
| `vkCreateSwapchainKHR` 段错误 | NVIDIA 无头 X 的 WSI 缺陷，与 NGX 无关 |
| `getSurfacePresentModesKHR: ErrorUnknown` | 同上——X 屏幕为 NULL 模式 |
| `Cannot initialize a Vulkan graphics device…` | NGX 运行时内部建设备失败（/dev/dri 缺失） |
| driver 610 版本号 | 不是问题——问题在容器挂载层 |

---

## 3. 本次交付的全部工具（Tools/DLSSCapture/，已提交 3a217861）

| 文件 | 功能 |
|---|---|
| nvcuda_capture.c | LD_PRELOAD 钩 cuLaunchKernel/Ex + cuMemAlloc/Pitch + vkGetDeviceQueue + WSI 合成（NVIDIA-only PD 过滤 + 假 swapchain 发放真实 VkImage） |
| dlss_sr_host_vk.cpp | 无头 NGX Vulkan 宿主（NGXWrapper 对齐）：FeatureDiscoveryInfo → requirements → VkInstance/Device → VULKAN_Init → DLSS CreateFeature/Evaluate |
| triage_capture.py | 按 WEIGHTS/FILTER SHA 自动定位 launch 并重命名 L0001/L0012 |
| setup_capture_machine.sh | 一键装配（自检/编译/头文件下载） |
| README.md | 跑机手册 + 排障表 + Result 代码速查 |

辅助工具（同 commit 或本目录）：
- `ContentTools/neural/nr_adapter.py` — GI 7ch → NGX 16ch 适配器
- `ContentTools/neural/eval_nr_vs_denoiser.py` — NR vs 降噪器并排对比
- `ContentTools/neural/make_sr_atlas.py` — 低分辨率×模型放大对比图集
- `ContentTools/neural/metrics.py` — PSNR/SSIM
- `Tools/DLSSCapture/` — 同一工具集的库内副本

---

## 4. 三个可行出路（按成本排序）

### 4.1 平台侧：获取 ngx 驱动能力

重建 GPU 容器/实例时设置 `NVIDIA_DRIVER_CAPABILITIES=all`（或至少含 `ngx`）。
NVIDIA 容器运行时会把驱动的 NGX 文件（snippet 内核 + NGX 服务库）挂进容器。

- RunPod/Lambda 等平台：模板 ENV 区加此变量
- 裸 VM：正常安装完整 NVIDIA 驱动即可（非 compute-only）
- 验证：`find /usr -iname "*ngx*" | grep -v capture` 应看到驱动 NGX 文件集

### 4.2 作者侧：向 iamwavecut 要现成 capture

GitHub issue 附上本次分析（组件库导出面、`__NGX_LOG_*` 变量、FeatureNotSupported
复现条件）——对他是有效信息。他的 capture 格式即我们的格式（`L0001_params.bin`
+ `L0001_buf*.bin`），拿到后 `package-sr` 一步出 `.srmodel`。

### 4.3 物理机侧：任何带显示器的 NVIDIA Linux 机器

`Tools/DLSSCapture/` 目录整个拷过去，三条命令十分钟收尾。这是唯一 100%
确定可行的物理路线——工具已全部就绪且与本容器无关。

---

## 5. 调试历程中确认的通用经验（防止重复踩坑）

### 5.1 NVIDIA 容器驱动挂载

- `NVIDIA_DRIVER_CAPABILITIES` 控制挂载子集：`compute,utility` 只给 CUDA；
  `graphics` 加 GLX/EGL；`ngx` 加 NGX snippet/服务；`display` 加 DRM 设备
- compute-only 容器内 `nvidia-smi` 正常但 Vulkan WSI/NGX 全不可用
- `/dev/dri`（DRM）缺失 = Mesa/llvmpipe 只能软件渲染
- `/dev/nvidia-modeset`（major 195, minor 254）缺失 = NVIDIA Vulkan WSI 初始化失败
- mknod 造设备节点需要 CAP_MKNOD；cgroup device 白名单可能仍拦

### 5.2 NVIDIA NGX Linux 运行时

- NGX 组件库（libnvidia-ngx-dlss.so）的 CUDA standalone 后端**不支持 DLSS-SR**
  （返回 FeatureNotSupported）——只有 RR（310.5.3+）走 CUDA
- DLSS-SR 的 Linux 后端是 Vulkan——但 Vulkan 路径要求 WSI（swapchain），
  无头环境需 NVIDIA X server（NULL 模式不够，需 ConnectedMonitor + 虚拟 mode）
- NGX 运行时的 requirements 查询（GetFeatureInstanceExtensionRequirements 等）
  在 compute-only 容器上返回 n=0 rc=0——**空结果不代表不需要**，代表运行时
  自身未能初始化到能枚举需求的程度
- 日志开关：`__NGX_ENABLE_OVERRIDE_LOG_PATH=1` + `__NGX_LOG_PATH_OVERRIDE=<dir>`
  + `__NGX_LOG_LEVEL=2`（VERBOSE）；但运行时过早失败时日志目录为空

### 5.3 MLX-DLSS 库版本陷阱

- NVIDIA 同版本号（310.7.0）发布多个 build——SHA 逐字不同
- `package-sr` 的 `SOURCE_SHA256` 钉定 `fc19b68…`（Linux 310.7.0 的某个
  特定 build）——官方 GitHub release 的 `.so` 不匹配
- capture 的权重身份由 `WEIGHTS_SHA256`/`FILTER_SHA256` 保证（内容级校验），
  库 build 版本不匹配只影响 shader PTX 提取部分

### 5.4 bash / 编译环境

- macOS `/bin/bash` 3.2.57 在 darwin25 上有 malloc 堆损坏 heisenbug（经
  make/cmake 中间层调用时 SIGSEGV/SIGABRT）→ 装 Homebrew bash + shebang
  改 `#!/usr/bin/env bash`
- C++ 文件里 dlsym 返回 void* 赋给函数指针需要 `reinterpret_cast`（C 编译器
  允许隐式转换但 C++ 不行）
- extern "C" 函数定义不能与 vulkan.h 的 extern "C" 声明签名不一致——
  否则 C++ 报 "conflicting declaration"
- 注释内出现 `*/` 会提前关闭块注释（`L0001_*/L0012_*` 场景）

### 5.5 GPU 容器 WSI 限制

- 无头 X（NULL mode）上 NVIDIA Vulkan WSI 的 present modes/capabilities
  查询返回 ErrorUnknown——NVIDIA WSI 需要 X server 有 NVIDIA DDX 驱动
  + 连接的显示设备（或至少 ConnectedMonitor 声明）
- 正确做法：`NVIDIA_DRIVER_CAPABILITIES=graphics` + `ConnectedMonitor DFP-0`
  + 虚拟 mode（云游戏容器的标准方案）
- 假 swapchain 需要**真实 VkImage**（GPU 侧可渲染），不能 fabricate handle
