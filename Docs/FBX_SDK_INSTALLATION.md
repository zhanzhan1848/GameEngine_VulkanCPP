# FBX SDK 安装指南

本指南说明如何在 macOS 上为 GameEngine_VulkanCPP 项目安装 FBX SDK。

## 自动安装（推荐）

我们提供了自动安装脚本，可以简化 FBX SDK 的安装过程：

### 方法一：使用自动安装脚本

```bash
# 运行自动安装脚本
./scripts/install_fbx_sdk_macos.sh
```

脚本将：
1. 检查是否已安装 FBX SDK
2. 尝试通过 Homebrew 安装（如果可用）
3. 提供手动安装指导
4. 验证安装结果
5. 设置正确的权限

### 方法二：使用 Homebrew

如果您已安装 Homebrew，可以直接运行：

```bash
brew install --cask fbx-sdk
```

安装完成后，CMake 将自动检测 FBX SDK。

## 手动安装

如果自动方法不工作，您可以手动安装：

### 步骤 1：下载 FBX SDK

1. 访问 Autodesk Developer Network：
   https://aps.autodesk.com/developer/overview/fbx-sdk

2. 登录您的 Autodesk 账户（如果没有账户，请创建一个）

3. 点击 "Download SDK for mac"

4. 选择版本（推荐 2020.3.7 或更高版本）

5. 下载 "Clang (Universal Binary)" 版本

### 步骤 2：安装 FBX SDK

```bash
# 创建安装目录
sudo mkdir -p "/Applications/Autodesk/FBX SDK"

# 解压下载的文件（假设文件名为 fbxsdk.tar.gz）
tar -xzf fbxsdk.tar.gz

# 移动到目标目录（假设解压后目录名为 fbxsdk）
sudo mv fbxsdk "/Applications/Autodesk/FBX SDK/2020.3.7"
```

### 步骤 3：验证安装

检查以下文件是否存在：
- `/Applications/Autodesk/FBX SDK/2020.3.7/include/fbxsdk.h`
- `/Applications/Autodesk/FBX SDK/2020.3.7/lib/clang/debug/libfbxsdk.dylib` 或
- `/Applications/Autodesk/FBX SDK/2020.3.7/lib/clang/release/libfbxsdk.dylib`

## CMake 配置

安装 FBX SDK 后，重新运行 CMake：

```bash
cd build
cmake ..
```

CMake 将自动检测 FBX SDK 并配置项目。如果检测成功，您将看到：

```
-- Found FBX SDK at: /Applications/Autodesk/FBX SDK/2020.3.7
-- FBX SDK configured successfully at: /Applications/Autodesk/FBX SDK/2020.3.7
```

如果未找到 FBX SDK，CMake 将显示详细的安装指导。

## 支持的版本

项目支持以下 FBX SDK 版本：
- 2020.3.7（主要支持版本）
- 2020.3.4
- 2020.3.2
- 2020.3.1
- 2020.0.1

CMake 会自动搜索这些版本的安装路径。

## 故障排除

### 问题 1：CMake 找不到 FBX SDK

**解决方案：**
1. 确认 FBX SDK 已正确安装
2. 检查安装路径是否为：`/Applications/Autodesk/FBX SDK/<version>/`
3. 确保 `fbxsdk.h` 文件存在于 `include` 目录中

### 问题 2：链接错误

**解决方案：**
1. 检查库文件是否存在：
   - `lib/clang/debug/libfbxsdk.dylib`
   - `lib/clang/release/libfbxsdk.dylib`
2. 确保 dylib 文件具有正确的权限

### 问题 3：权限问题

**解决方案：**
```bash
sudo chmod -R 755 "/Applications/Autodesk/FBX SDK"
```

### 问题 4：Homebrew 安装失败

**解决方案：**
1. 更新 Homebrew：
   ```bash
   brew update
   brew upgrade
   ```
2. 如果仍然失败，使用手动安装方法

## 环境变量（可选）

如果 FBX SDK 安装在非标准位置，可以设置环境变量：

```bash
export FBX_SDK_ROOT="/path/to/your/fbx-sdk"
```

然后重新运行 CMake。

## 验证安装

要验证 FBX SDK 是否正确配置，可以编译项目并检查：

```bash
cd build
make

# 检查是否成功链接了 FBX SDK
otool -L path/to/your/executable | grep fbx
```

## 自动下载选项

CMake 提供了自动下载选项（需要用户确认）：

```bash
cmake -DAUTO_DOWNLOAD_FBX_SDK=ON ..
```

**注意：** 由于 Autodesk 需要账户登录，自动下载可能无法完全工作，但仍会提供详细的安装指导。

## 获取帮助

如果遇到问题：

1. 检查 CMake 输出的详细错误信息
2. 确认安装的 FBX SDK 版本兼容性
3. 查看项目文档：`.trae/docs/`
4. 检查 Autodesk 官方文档

## 相关文件

- `scripts/install_fbx_sdk_macos.sh` - 自动安装脚本
- `CMakeLists.txt` - CMake 配置文件
- `.trae/docs/` - 项目文档目录

---

**注意：** FBX SDK 是 Autodesk 的商业产品，使用时需要遵守其许可协议。