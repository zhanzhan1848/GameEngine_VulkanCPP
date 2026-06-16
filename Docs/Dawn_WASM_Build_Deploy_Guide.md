# Dawn WebGPU WASM 构建与发布指南

## 1. 架构概览

```
Browser
  └─ index.html (加载页 + loading overlay + FPS counter)
      ├─ wasm_manifest.js (CMake 自动生成的贴图清单)
      ├─ TestDawnWASM.js  (Emscripten loader)
      └─ TestDawnWASM.wasm (引擎 + 嵌入 shaders + Sponza.model)
          │
          ├─ preRun: stageAllAssets()
          │    └─ XHR 拉取贴图到 MEMFS (8 并发, 进度条)
          │    └─ addRunDependency / removeRunDependency
          │
          └─ main()
               └─ initialize() → LoadSponzaScene()
               └─ emscripten_run_script("hideLoading()")
               └─ emscripten_set_main_loop(RenderFrame, 0, 1)
```

## 2. 本地构建

```bash
# 配置
emcmake cmake -B build_wasm -DENABLE_WEBGPU=ON -DCMAKE_BUILD_TYPE=MinSizeRel

# 编译 (只编译 Engine 静态库, 用于快速验证)
emmake make -C build_wasm -j$(sysctl -n hw.ncpu) Engine

# 完整构建 (含链接 WASM)
cmake --build build_wasm --parallel $(nproc)
```

**输出目录**: `WASM/` (TestDawnWASM.js, TestDawnWASM.wasm, libEngine.a)

## 3. 发布流程

1. 本地 `emmake make` 编译通过
2. Push 到 `features/dawn-webgpu-backend` 分支
3. CI (`.github/workflows/wasm-build.yml`) 自动构建并部署到 GitHub Pages (gh-pages)

```
CI 步骤:
  checkout → setup emsdk → embuilder emdawnwebgpu
  → emcmake cmake → cmake --build
  → 拷贝 JS/WASM/HTML/资产到 deploy/
  → peaceiris/actions-gh-pages 部署
```

**只需 push，CI 自动完成构建部署。**

## 4. 关键踩坑与注意事项

### 4.1 WebGPU 坐标系 Y-Flip

WebGPU viewport transform 会翻转 Y 轴: NDC y=+1 映射到 framebuffer row 0 (顶部)。

所有从 NDC 转换到纹理坐标的代码都需要补偿:

```wgsl
// Shadow UV — 需要 Y-flip
let shadowUV = vec2f(lightNDC.x * 0.5 + 0.5, 1.0 - (lightNDC.y * 0.5 + 0.5));

// SSAO/SSGI depth reconstruction — 需要 Y-flip
let ndcY = 1.0 - uv.y * 2.0;

// Forward pass texture UV — 需要 V-flip
output.uv = vec2f(input.uv.x, 1.0 - input.uv.y);
```

### 4.2 Shadow 渲染

**规则**: Dawn 的 shadow pass 由 Test 的 `RenderShadowPass()` 独立负责，ForwardRenderer 不参与。

- `skipShadows = true` 跳过所有 Metal 专用 shadow pass (VSM/CSM/SSR/Reflection)
- **禁止**在 ForwardRenderer::Render() 中调用 `RenderDawnShadowPass()` — 会用不同的 VP 覆盖 `dawnShadowLightVP_`，导致 shadow map 和采样 VP 不匹配
- Shadow light VP 必须用**固定场景中心** (如 `{0, 5, 0}`)，不能用 `cameraPos_`

```
正确流程:
  Test::RenderShadowPass()    → 渲染 shadow depth + 计算 lightVP
  Test::SetDawnShadowLightVP() → 传入 ForwardRenderer
  ForwardRenderer::Render()   → SetupLights() 将 VP 写入 light buffer
                                 shader 用 VP 采样 shadow map
```

### 4.3 纹理加载时序

贴图必须在 `Module.preRun` 中完成拉取，不能在 `postRun`:

```javascript
// index.html — preRun 中用 addRunDependency 阻塞 main()
preRun: [function() {
    addRunDependency('textures');
    fetchAllTextures().then(() => {
        removeRunDependency('textures');  // main() 在此之后才执行
    });
}]
```

**错误做法**: 在 `postRun` 中拉取贴图 — 此时 `main()` 已经开始，`LoadSponzaScene()` 只能拿到 1x1 占位贴图。

### 4.4 Shader 双重嵌入机制

修改 WGSL shader 时**必须同时更新两处**:

| 位置 | 作用 |
|------|------|
| `Engine/Graphics/Dawn/ShaderLoader.h` | `#ifdef __EMSCRIPTEN__` 中所有 WGSL 作为 `static const char*` 嵌入 |
| `Engine/Graphics/Dawn/shaders/*.wgsl` | CMake 通过 `--embed-file` 嵌入 MEMFS |

**漏更新任何一处都会导致 WASM 和 Native 行为不一致。**

### 4.5 Sampler comparisonFunc

```cpp
SamplerDesc.comparisonFunc = ComparisonFunc::Never;  // 必须是 Never
```

WebGPU 中 filtering sampler 的 `comparisonFunc` 必须是 `Never`，否则会创建为 comparison sampler，采样结果错误。

### 4.6 GPU 同步

Dawn WASM 不能 busy-wait:

```cpp
// ❌ 错误: busy-wait 阻塞浏览器
while (!submissionsDone) { wgpuInstanceProcessEvents(inst); }

// ✅ 正确: submit 不同步, EndFrame 做单次 flush
void DawnDevice::submitImpl(...) {
    wgpuQueueSubmit(queue_, ...);  // 无同步
}
void DawnDevice::endFrameImpl() {
    wgpuInstanceProcessEvents(wgpuInstance_);  // 帧边界同步
}
```

### 4.7 SSGI 条纹修复

`feedback = 0.0` 完全禁用时间累积，半分辨率 4-ray/pixel 的噪声表现为条纹:

```cpp
// ❌ 条纹
p->feedback = 0.0f;

// ✅ 正确: 90% 历史 + 10% 当前, 几帧收敛
p->feedback = 0.9f;
```

### 4.8 WGSL packed u16 提取

小端序: 低 16 位 = x, 高 16 位 = y:

```wgsl
let nx = f32(packed & 0xFFFFu);
let ny = f32((packed >> 16u) & 0xFFFFu);
```

### 4.9 Emscripten operator[] UB

`(&x)[i]` 在 WASM 上会被优化为 0:

```cpp
// ❌ UB on WASM
float val = (&array[0])[offset];

// ✅ 用 switch/conditional 替代
float val = (offset == 0) ? array[0] : (offset == 1) ? array[1] : ...;
```

### 4.10 Depth 纹理采样

WebGPU depth 纹理采样需要:

- Texture 创建时带 `TextureBinding` usage flag
- Descriptor type 用 `SampledDepthImage` (不是 `SampledImage`)

## 5. 关键文件索引

| 用途 | 路径 |
|------|------|
| Root CMake (WASM 目标定义) | `CMakeLists.txt` (line 309-387) |
| Engine CMake (Dawn 编译) | `Engine/CMakeLists.txt` (line 121-212) |
| CI 构建工作流 | `.github/workflows/wasm-build.yml` |
| Dawn ShaderLoader (双模式) | `Engine/Graphics/Dawn/ShaderLoader.h` |
| WGSL shader 源文件 | `Engine/Graphics/Dawn/shaders/*.wgsl` |
| 用户加载页 | `wasm/index.html` |
| Emscripten shell 模板 | `wasm/shell.html` |
| 资产清单 (自动生成) | `wasm/wasm_manifest.js` |
| WASM 入口 main() | `EngineTest/IntegrationTests/TestDawnMain.cpp` |
| 测试渲染器 | `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` |
| Forward Renderer | `Engine/Graphics/ForwardRenderer.cpp` |
| SSGI Pass | `Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/LumenSSGIDawnPass.cpp` |
| SSAO Pass | `Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/SSAOPass.cpp` |
| Dawn RHI 平台层 | `Engine/Graphics/RHI/Platforms/Dawn/*.cpp` |
| 矩阵/数学工具 | `Engine/Graphics/RHI/Core/RHIMath.h` |
| Emscripten 平台实现 | `Engine/Platform/PlatformEmscripten.cpp` |

## 6. 调试技巧

- **Shader 验证**: WebGPU 有详细 validation error，查看浏览器 console
- **Texture 验证**: 用 1x1 solid color 作为 fallback，确认绑定正确性
- **MEMFS 内容**: 在浏览器 console 中 `FS.readdir('/')` 查看已加载文件
- **性能**: `std::chrono` 测量 CPU 帧时间, GPU query 测量 pass 执行时间

## 7. 发布检查清单

- [ ] `emmake make` 本地编译通过
- [ ] ShaderLoader.h 和 .wgsl 文件同步更新
- [ ] Push 到 `features/dawn-webgpu-backend`
- [ ] CI 构建通过 (wasm-build.yml)
- [ ] gh-pages 页面正常加载，loading overlay 正确显示/隐藏
- [ ] 贴图全部加载 (console 无 `[Assets]` 错误)
- [ ] 无 WebGPU validation error
- [ ] Shadow/Lighting/SSAO/SSGI 效果正常
