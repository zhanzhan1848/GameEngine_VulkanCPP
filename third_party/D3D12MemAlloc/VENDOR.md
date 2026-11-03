# D3D12MemAlloc（D3D12 Memory Allocator，AMD GPUOpen）— 供应商落库记录

## 来源

- 上游仓库：https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator
- 落库版本/Tag：**v3.0.1**（Release "D3D12 Memory Allocator v3.0.1"，上游头文件内版本串
  `Version 3.0.1 (2025-05-08)`，见 include/D3D12MemAlloc.h L27）
- 落库日期：2026-11-03（D3D12 RHI 现代化 M0 批次 B，F0-4）
- 许可证：MIT（LICENSE.txt，AMD Copyright 2019-2025）

## 落库文件与 SHA256

| 文件 | SHA256 | 字节数 |
|---|---|---|
| `include/D3D12MemAlloc.h` | `18fdfb56bd842f5958ded62d8104f36d9fbfbff9cfccb2b2d33f71c4adb487ac` | 169221 |
| `src/D3D12MemAlloc.cpp` | `d66f6392fa92414ecf9e3c72b138d9bd61e4a383b4aac92a3434222a334d9f90` | 344281 |
| `LICENSE.txt` | `9dab190f56ec64bd4dbad25c349b4a381436d54fc908c312ffa3bbf7d1608fa0` | 1099 |

复算命令：

```bash
shasum -a 256 third_party/D3D12MemAlloc/include/D3D12MemAlloc.h \
              third_party/D3D12MemAlloc/src/D3D12MemAlloc.cpp \
              third_party/D3D12MemAlloc/LICENSE.txt
```

## 下载命令（可复现）

```bash
BASE=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/v3.0.1
curl -sL --max-time 120 -o include/D3D12MemAlloc.h  "$BASE/include/D3D12MemAlloc.h"
curl -sL --max-time 120 -o src/D3D12MemAlloc.cpp     "$BASE/src/D3D12MemAlloc.cpp"
curl -sL --max-time 60  -o LICENSE.txt               "$BASE/LICENSE.txt"
```

（等价方式：GitHub Release 归档
`https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/releases/download/v3.0.1/D3D12MemoryAllocator-3.0.1.zip`，
取其中 `include/D3D12MemAlloc.h`、`src/D3D12MemAlloc.cpp`、`LICENSE.txt` 三文件，内容一致。）

## 构建接入（Engine/CMakeLists.txt）

- 仅 `WIN32 AND ENABLE_D3D12` 时 `src/D3D12MemAlloc.cpp` 进 Engine 源集，
  include 路径 `third_party/D3D12MemAlloc/include` 对 Engine 目标可见。
- **macOS / 非Windows 树不编该文件**：`D3D12MemAlloc.cpp` 直接 include
  `<d3d12.h>/<dxgi.h>` 等 Windows 头，进 macOS 编译必然失败。门控在
  `Engine/CMakeLists.txt` 的 `if(WIN32 AND ENABLE_D3D12)` D3D12 块内。

## 消费点

- `Engine/Graphics/RHI/Platforms/D3D12/D3D12Device.cpp` `createAllocator()`：
  `D3D12MA::Allocator::Create<D3D12MA::Allocator>(&allocDesc, &allocator_)`
  （Windows 专属编译块，macOS 仅静态审阅，DEFERRED-WIN）。
