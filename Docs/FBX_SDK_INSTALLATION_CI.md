
## CI 环境配置指南 (GitHub Actions)

在 GitHub Actions 等 CI 环境中，由于无法手动交互，建议使用以下方式配置 FBX SDK：

### macOS Runner 配置

对于 macOS Runner，可以使用我们提供的安装脚本或直接使用 Homebrew。

#### 方法一：使用安装脚本（推荐）

在 `.github/workflows/your-workflow.yml` 中添加步骤：

```yaml
steps:
  - name: Checkout code
    uses: actions/checkout@v3
    with:
      submodules: recursive  # 确保拉取 submodule (如 meshoptimizer)

  - name: Install FBX SDK
    run: |
      chmod +x scripts/install_fbx_sdk_macos.sh
      ./scripts/install_fbx_sdk_macos.sh
      
  - name: Configure CMake
    run: |
      mkdir build
      cd build
      cmake ..
```

#### 方法二：使用 Homebrew 直接安装

```yaml
steps:
  - name: Install Dependencies
    run: |
      brew update
      brew install --cask fbx-sdk
      
  - name: Configure CMake
    run: |
      mkdir build
      cd build
      cmake ..
```

### 注意事项

1. **Submodules**: 务必在 checkout 步骤中设置 `submodules: recursive`，以确保下载 `meshoptimizer` 等第三方库。
2. **权限**: GitHub Actions 的 macOS runner 默认拥有 sudo 权限，脚本中的 sudo 命令可以正常执行。
3. **缓存**: 为了加快构建速度，可以考虑缓存 FBX SDK 安装目录，但这可能比较复杂，因为 FBX SDK 安装在系统目录。Homebrew 缓存通常由 runner 自动处理。
