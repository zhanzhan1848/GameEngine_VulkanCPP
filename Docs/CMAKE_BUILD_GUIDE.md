# GameEngine Vulkan C++ - CMake 编译指南

## 概述

本文档详细介绍了 GameEngine Vulkan C++ 项目的完整 CMake 编译流程，包括多平台支持、依赖项管理、编译选项配置以及常见问题解决方案。

## 系统要求

### 基本要求
- **CMake**: >= 3.20
- **C++ 编译器**: 支持 C++17 标准的编译器
  - GCC: >= 9.0
  - Clang: >= 10.0
  - MSVC: >= 2019 (16.0)

### 平台特定要求

#### Windows
- Visual Studio 2019 或更高版本
- Windows 10 SDK (最新版本)
- [可选] Vulkan SDK (用于 Vulkan 渲染后端)

#### macOS
- Xcode 12.0 或更高版本
- Command Line Tools for Xcode
- macOS 10.15 或更高版本

#### Linux
- GCC 9.0+ 或 Clang 10.0+
- 开发工具包: build-essential
- [可选] Vulkan 开发包

## 快速开始

### 1. 克隆项目
```bash
git clone https://github.com/your-repo/GameEngine_VulkanCPP.git
cd GameEngine_VulkanCPP
```

### 2. 项目配置
```bash
# 创建构建目录并配置项目
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
```

### 3. 编译项目
```bash
# 编译整个项目
cmake --build build

# 或者使用 make (Unix 系统)
cd build && make -j$(nproc)
```

### 4. 运行测试
```bash
# 运行所有测试
ctest --test-dir build --verbose

# 或者直接运行特定测试
./build/Tests/UnitTests/StandaloneRHI_Test
./build/Tests/UnitTests/StandaloneRHI_Device_Test
```

## 详细编译流程

### 项目配置命令详解

#### 基础配置
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
```

**参数说明:**
- `-B build`: 指定构建目录为 `build`
- `-S .`: 指定源代码目录为当前目录
- `-DCMAKE_BUILD_TYPE=Debug`: 设置编译类型为 Debug

#### 编译类型选项
- **Debug**: 包含调试信息，无优化，适合开发调试
- **Release**: 启用优化，不包含调试信息，适合发布
- **RelWithDebInfo**: 启用优化且包含调试信息
- **MinSizeRel**: 优化代码体积

```bash
# 发布版本配置
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# 调试信息版本配置
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

#### 高级配置选项
```bash
# 指定安装目录
cmake -B build -S . -DCMAKE_INSTALL_PREFIX=/usr/local/GameEngine

# 启用特定功能
cmake -B build -S . -DENABLE_VULKAN=ON -DENABLE_METAL=ON

# 设置编译器
cmake -B build -S . -DCMAKE_CXX_COMPILER=clang++

# 设置并行编译数
cmake -B build -S . -DCMAKE_BUILD_PARALLEL_LEVEL=4
```

### 编译命令详解

#### 基础编译
```bash
# 编译整个项目
cmake --build build

# 指定并行编译数
cmake --build build --parallel 4
```

#### 目标编译
```bash
# 只编译 RHI 测试
cmake --build build --target StandaloneRHI_Test StandaloneRHI_Device_Test

# 编译所有测试
cmake --build build --target RHI_All_Tests

# 编译引擎库
cmake --build build --target Engine
```

#### 安装项目
```bash
# 安装到配置的目录
cmake --install build

# 只安装特定组件
cmake --install build --component runtime
```

## 多平台编译指南

### Windows 平台

#### 使用 Visual Studio
```cmd
# 配置项目
cmake -B build -S . -G "Visual Studio 16 2019" -A x64

# 编译
cmake --build build --config Debug

# 或者使用 MSBuild
cd build
MSBuild GameEngine.sln /p:Configuration=Debug /m
```

#### 使用 MinGW
```bash
# 配置
cmake -B build -S . -G "MinGW Makefiles"

# 编译
cmake --build build
```

### macOS 平台

#### 使用 Xcode
```bash
# 配置
cmake -B build -S . -G Xcode

# 编译
cmake --build build --config Debug

# 或者使用 xcodebuild
cd build
xcodebuild -project GameEngine.xcodeproj -scheme ALL_BUILD -configuration Debug
```

#### 使用 Make (推荐)
```bash
# 配置
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build --parallel $(sysctl -n hw.ncpu)
```

### Linux 平台

#### 使用 Make
```bash
# 配置
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build --parallel $(nproc)
```

#### 使用 Ninja
```bash
# 安装 Ninja (如果未安装)
sudo apt-get install ninja-build

# 配置
cmake -B build -S . -G Ninja

# 编译
cmake --build build
```

## 依赖项管理

### 自动依赖项
项目使用 CMake 的 FetchContent 模块管理以下依赖：
- **moodycamel ConcurrentQueue**: 高性能无锁队列

### 手动依赖项
某些平台可能需要手动安装的依赖项：

#### macOS
```bash
# 安装 Xcode Command Line Tools
xcode-select --install

# 使用 Homebrew 安装可选依赖
brew install cmake vulkan-sdk
```

#### Ubuntu/Debian
```bash
# 基础开发工具
sudo apt update
sudo apt install build-essential cmake git

# Vulkan 开发 (可选)
sudo apt install libvulkan-dev vulkan-tools
```

#### Windows
- Visual Studio Installer 中安装 C++ 开发工具
- 下载并安装 Vulkan SDK (可选)

## 编译产物说明

### 目录结构
```
build/
├── Darwin/                 # macOS 编译产物
│   └── Debug/
│       ├── Engine/         # 引擎库
│       ├── Tests/          # 测试程序
│       └── Examples/       # 示例程序
├── Windows/                # Windows 编译产物
└── Linux/                  # Linux 编译产物
```

### 主要输出文件
- **StandaloneRHI_Test**: RHI 类型系统测试 (约 36KB)
- **StandaloneRHI_Device_Test**: RHI 设备管理测试 (约 55KB)
- **libEngine.a**: 静态引擎库
- **GameEngine**: 可执行引擎示例

### 测试结果文件
- **rhi_test_results.txt**: RHI 类型系统测试结果
- **rhi_device_test_results.txt**: RHI 设备管理测试结果

## 常见编译错误及解决方案

### 1. CMake 版本过低
**错误**: `CMake 3.20 or higher is required`

**解决方案**:
```bash
# Ubuntu/Debian
sudo apt install cmake=3.20*

# macOS
brew install cmake

# Windows
# 下载最新版本从 cmake.org
```

### 2. 编译器不支持 C++17
**错误**: `error: 'std::filesystem' has not been declared`

**解决方案**:
```bash
# 更新编译器
sudo apt install gcc-9 g++-9

# 或指定编译器
cmake -B build -S . -DCMAKE_CXX_COMPILER=g++-9
```

### 3. 找不到 CommonHeaders.h
**错误**: `fatal error: 'CommonHeaders.h' file not found`

**解决方案**:
```bash
# 确保在项目根目录运行 CMake
cd /path/to/GameEngine_VulkanCPP
cmake -B build -S .

# 检查 include 路径
cmake -B build -S . -DCMAKE_INCLUDE_PATH=/path/to/Engine/Common
```

### 4. 链接错误
**错误**: `undefined reference to 'SomeFunction'`

**解决方案**:
```bash
# 清理构建目录重新编译
rm -rf build
cmake -B build -S .
cmake --build build

# 检查依赖项是否正确链接
cmake --build build --verbose
```

### 5. 权限问题 (macOS)
**错误**: `Permission denied` 或 codesign 错误

**解决方案**:
```bash
# 使用本地 cmake 配置
cmake -B build -S . -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15

# 或禁用代码签名 (仅用于开发)
codesign --remove-signature build/Tests/UnitTests/StandaloneRHI_Test
```

### 6. 内存不足
**错误**: 编译时系统内存不足

**解决方案**:
```bash
# 减少并行编译数
cmake --build build --parallel 1

# 或使用 make
cd build
make -j1
```

## 性能优化建议

### 编译优化
```bash
# Release 版本优化
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"

# 链接时优化 (LTO)
cmake -B build -S . -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
```

### 调试优化
```bash
# 快速调试构建
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DDEBUG"
```

## CI/CD 集成

### GitHub Actions 示例
```yaml
name: Build and Test

on: [push, pull_request]

jobs:
  build:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, windows-latest, macos-latest]
        build_type: [Debug, Release]

    steps:
    - uses: actions/checkout@v2
    
    - name: Configure CMake
      run: cmake -B build -S . -DCMAKE_BUILD_TYPE=${{ matrix.build_type }}
    
    - name: Build
      run: cmake --build build --parallel
    
    - name: Test
      run: ctest --test-dir build --output-on-failure
```

## 验证编译结果

### 运行完整测试套件
```bash
# 运行所有测试
cmake --build build --target RHI_All_Tests

# 使用 ctest
ctest --test-dir build --verbose

# 检查测试输出
cat build/rhi_test_results.txt
cat build/rhi_device_test_results.txt
```

### 验证二进制兼容性
```bash
# 检查符号表 (Linux/macOS)
nm build/Tests/UnitTests/StandaloneRHI_Test | grep Test

# 检查依赖项 (Linux)
ldd build/Tests/UnitTests/StandaloneRHI_Test

# 检查依赖项 (macOS)
otool -L build/Tests/UnitTests/StandaloneRHI_Test
```

## 故障排除清单

在遇到编译问题时，请按以下顺序检查：

1. ✅ CMake 版本 >= 3.20
2. ✅ 编译器支持 C++17
3. ✅ 在项目根目录运行命令
4. ✅ 清理构建目录重新编译
5. ✅ 检查依赖项是否安装
6. ✅ 查看详细的编译输出
7. ✅ 检查系统资源是否充足

## 技术支持

如果遇到本文档未涵盖的问题，请：

1. 检查项目的 GitHub Issues
2. 查看 CMake 官方文档
3. 联系项目维护者

---

**注意**: 本文档基于项目当前版本 (v0.1.0) 编写，后续版本可能会有更新。请定期检查文档的时效性。