
## CI 环境配置指南 (GitHub Actions)

### 方案更新：本地集成 FBX SDK（推荐）

为了避免 CI 环境中的网络下载和认证问题，我们将 FBX SDK 的必要文件（头文件和动态库）直接集成到了项目仓库中。

路径：`third_party/FBX_SDK/Mac`

#### 优势
1. **无需下载**：CI 流程不再依赖外部网络下载 FBX SDK。
2. **无需认证**：避开了 Autodesk 的登录验证机制。
3. **版本统一**：确保所有开发环境和 CI 环境使用完全一致的 SDK 版本。

#### 目录结构
```
third_party/
  FBX_SDK/
    Mac/
      include/          # 头文件
      lib/
        debug/          # Debug 版本的动态库 (libfbxsdk.dylib)
        release/        # Release 版本的动态库 (libfbxsdk.dylib)
```

### GitHub Actions 配置

由于 SDK 文件已在仓库中，CI 配置可以大大简化。你不再需要运行安装脚本。

#### 示例 Workflow (.github/workflows/ci.yml)

```yaml
steps:
  - name: Checkout code
    uses: actions/checkout@v3
    with:
      submodules: recursive  # 确保拉取 meshoptimizer 等子模块
      lfs: true              # 如果使用了 Git LFS 存储大文件（推荐）

  - name: Configure CMake
    run: |
      mkdir build
      cd build
      cmake ..
      
  - name: Build
    run: |
      cd build
      make -j$(sysctl -n hw.ncpu)
```

### 维护指南

如果需要更新 FBX SDK 版本：

1. **本地下载**：从 Autodesk 官网下载新版 SDK 并安装。
2. **复制文件**：
   - 将 `include` 目录内容复制到 `third_party/FBX_SDK/Mac/include/`
   - 将 `lib/clang/debug/libfbxsdk.dylib` 复制到 `third_party/FBX_SDK/Mac/lib/debug/`
   - 将 `lib/clang/release/libfbxsdk.dylib` 复制到 `third_party/FBX_SDK/Mac/lib/release/`
3. **提交更改**：提交新文件到 Git 仓库。

### 注意事项

- **Git LFS**: 动态库文件 (`.dylib`) 可能较大，建议对这些文件启用 Git LFS。
- **权限**: 确保 `.dylib` 文件在提交后保持可执行权限（通常 Git 会保留）。
