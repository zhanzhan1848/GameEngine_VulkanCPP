# AI Asset Pipeline — Phase 1 第三方库调研报告

**日期**：2026-08-07
**对应文档**：
- `~/Downloads/Primal_Engine_AI_Asset_Pipeline_Engine_Design_Document_V2.0.docx`
- `~/Downloads/AI_Asset_Pipeline_方案设计_V1.0.docx`

**范围**：Phase 1（传统几何处理）涉及的第三方库选型。Phase 2（AI 语义结构化）和 Phase 3（全自动）不在本次调研。

**评估维度**：(1) License 与商用风险；(2) 平台支持与构建难度；(3) API 形态与集成成本；(4) 功能定位与重叠。

**目标平台**：Apple Silicon macOS（主），Windows x64（次），Linux（占位）。

---

## 关键结论速览

| 功能 | 选定库 | 集成形态 | License | 关键理由 |
|---|---|---|---|---|
| 几何修复 / Remesh / Hole Fill / Subdivision | **PMP Library** | vcpkg link-in | MIT | Phase 1 五项需求覆盖 4 项，依赖只有 Eigen3 |
| 几何 cleanup / Decimate / Boolean（可选） | **libigl core**（避开 `copyleft/cgal`） | vcpkg link-in | MPL2 | Eigen 数据结构零拷贝 |
| UV Atlas | **xatlas** | vendor link-in（2 文件） | MIT | 行业事实标准（Unity 6.3 / Godot / Filament） |
| LOD / Meshlet / Cache opt | **meshoptimizer**（已集成） | submodule | MIT | 现状保留 |
| 凸分解（碰撞） | **Unity-Technologies/VHACD** fork | vendor link-in | BSD-3 | 原 kmammou 仓库 EOL；Unity fork 持续维护 |
| Retopo（Instant Meshes / QuadriFlow） | **Phase 1 不做** | — | — | AI mesh 已 dense isotropic；academic dormant code |
| Baker（Embree） | **Phase 1 不纳入** | — | — | Apple Silicon 上 NEON 128-bit 抹平 SIMD 优势；与自家 GPU RT 重叠 |

**明确禁用清单**：
- `CGAL`（GPLv3+ 传染，链接即触发）
- `libigl[cgal]` feature flag / `igl::copyleft::cgal::*`（同上）
- `kmammou/v-hacd`（已 EOL，迁移到 CoACD，但本调研建议用 Unity fork 而非 CoACD）
- `Embree`（Phase 1）

---

## 1. 几何修复 / Remesh

### 1.1 CGAL — 不采用

- **License**：GPLv3+ / LGPLv3+（多数 PMP 组件）+ GeometryFactory 商业许可（≈1000 €/年/研究组，商业产品另议）。**链接进闭源游戏二进制即触发 GPL 传染**。
- **平台/构建**：vcpkg `cgal` 6.2.x。**依赖链重**：Boost（数十子端口）+ GMP + MPFR，CI 初次安装 30 分钟–2 小时。任何 TU 引入 `<CGAL/...>` 模板膨胀 5-15 秒。
- **API/集成**：自有 `Surface_mesh<Kernel>` 半边结构，与引擎 SoA buffer 需复制转换。学习曲线最陡（Kernel / traits / concept 世界观）。
- **功能**：三者中最完整、最鲁棒（精确算术）：`remove_self_intersections` / `triangulate_hole` / `isotropic_remeshing` / `orient_polygon_soup`。
- **结论**：**功能满分，License 一票否决**。

### 1.2 libigl core — 采用（补 PMP 短板）

- **License**：[MPL-2.0](https://libigl.github.io/license/)（file-level copyleft，链接不传染）。**关键陷阱**：`include/igl/copyleft/cgal/` 子树实际 GPLv3（因 CGAL 头是 GPL），包含 `mesh_boolean` / `remesh_self_intersections` 等。**这些 header 必须避开**。
- **平台/构建**：vcpkg `libigl`（**勿开 `[cgal]` feature flag**）。依赖只有 Eigen3，编译影响 < 3 秒/TU。
- **API/集成**：header-only，纯 Eigen（`MatrixXd V` + `MatrixXi F`）。引擎 SoA buffer ↔ Eigen `Map` 几乎零拷贝，集成 friction 三者最低。官方 Python binding（PyPI `libigl` 2.5.x）。
- **功能**：`remove_duplicate_vertices` / `decimate` / `collapse_small_triangles` / `isotropic_remeshing`（要求 manifold 输入）/ `loop` / `upsample`。
- **短板**：非流形修复弱（假设输入 manifold）；robust 自相交修复走 `copyleft::cgal`（禁用）。
- **结论**：作为 PMP 的 cleanup / decimate 补充。**只用 `core` 子树**。

### 1.3 PMP Library — 采用（主）

- **License**：[MIT](https://github.com/pmp-library/pmp-library)，全代码 + 工具 + viewer 一致。三者中最干净。
- **平台/构建**：vcpkg `pmp-library` 3.0.x（C++17 release；main 分支已 C++20，按需选）。依赖只有 Eigen3。compiled lib 约 30-50 .cpp，全量编译 < 2 分钟。
- **API/集成**：compiled lib，核心 `pmp::SurfaceMesh` 半边结构，property system 类 OpenOpenMesh。需要约 50 行胶水做 vertex/face buffer ↔ SurfaceMesh 双向转换。学习曲线低-中。
- **功能**：`cleanup()` / `remove_degenerate_faces()` / `fill_hole()` / `remesh()`（教科书 isotropic）/ `catmull_clark_subdivision()` / `loop_subdivision()`。
- **短板**：无 robust 自相交修复（基础 AABB 有，但不是主推）。
- **结论**：Phase 1 主 pipeline 用 PMP。

---

## 2. 重拓扑（Retopo）— Phase 1 不做

### 2.1 Instant Meshes（wjakob/instant-meshes）

- **License**：BSD-3。3rd-party 子模块均为弱 copyleft 或宽松许可（Eigen MPL2 / TBB Apache 2.0 / NanoGUI BSD / GLFW zlib），**无 GPL**。
- **平台/构建**：源码编译（vcpkg/brew/conan 均无现成包）。**未提及 arm64 验证**（README 写于 Intel-only 时代）。仓库活跃度 "feature complete + dormant"，最后实质更新约 2020。
- **API/集成**：**CLI / GUI 应用，不是 library**。OBJ/PLY 输入输出。无 C API / 无 Python binding。集成只能 fork-exec + OBJ 文件交换。
- **功能**：quad quality 高（distortion 低），速度快（比 QuadriFlow 快 ~10×）。Foundry Modo 10.2+ 内嵌。

### 2.2 QuadriFlow（hjwdzh/QuadriFlow）

- **License**：MIT。**Eigen Sparse Cholesky 是 LGPL** —— 必须 `-DBUILD_FREE_LICENSE=ON` 切到 MPL2-only Sparse LU（稍慢）才能规避静态链接 LGPL 合规争议。**用户问题中"依赖 MKL"是错的，QuadriFlow 不需要 MKL**。
- **平台/构建**：源码编译，依赖 Boost/Eigen/OpenMP/TBB。仓库活跃度 dormant，issue 创建被锁。Blender 内部 fork 是另一条活跃线但已分叉。
- **API/集成**：**CLI 工具**，manifold OBJ → quad OBJ。无 C API / 无 Python binding。
- **功能**：比 Instant Meshes 更 robust，layout 更规整，但 distortion 略高，**慢约 10×**（min-cost flow solver）。新方法（QuadWild / NeuFrameQ ICCV 2025）已超越。

### 2.3 选型建议

**Phase 1 不做 retopo**。理由：
1. **AI 生成 mesh 已经是 dense isotropic 三角网格**。Retopo 的核心价值是 quad layout 服务动画/rigging，**对静态环境道具（PCG/WFC 上下文是 tile/ruins）收益≈0**。
2. 两个候选都是 dormant academic code，引入即长期维护负担。
3. 生产级 retopo（Modo / 3ds Max Quad Remesher）是商业闭源。

**降级路径**：将来真要做角色 mesh，选 **QuadriFlow + BUILD_FREE_LICENSE + fork-exec CLI**（Blender 已趟过问题清单）。

---

## 3. UV Atlas — xatlas

- **License**：[MIT](https://github.com/jpcy/xatlas)，Copyright **Jonathan Young**（GitHub `jpcy`；非用户提及的 "Joshua Davis"，已订正）。无外部依赖。
- **平台/构建**：单文件 `xatlas.cpp` + `xatlas.h`，标准 C++11。Premake 项目（社区维护 CMake wrapper）。vcpkg `xatlas` 有。可拷两个文件直接 vendor。最后一次实质更新约 2022-2023，"成熟稳定"状态。
- **API/集成**：**真正的 C++ library**（非 CLI），`xatlas::Create / AddMesh / Generate / ComputeCharts / PackCharts / Destroy`。**输入是任意 position/normal/uv 数组 + index 数组**（可直接喂 meshoptimizer 风格 vertex buffer，不需要 OBJ）；输出新 mesh 带 UV channel（顶点数会增加以插入 seam）。Python binding `xatlas-python`（mworchel）。
- **行业采用度（顶级背书）**：Unity 6.3（lightmap packing）、Godot Engine、Filament、UNIGINE、Wicked Engine、ArmorPaint、Bakery（GPU Lightmapper）、Skylicht、Blender PR #105821。
- **与 meshoptimizer 重叠确认**：meshoptimizer 只做 vertex cache / overdraw / vertex fetch / simplify / encode，**不做 UV atlas / parameterization / chart segmentation / packing**。两者是 pipeline 上下游。
- **结论**：link-in library，vendor 到 `ContentTools/vendor/xatlas/`。

---

## 4. 碰撞 — VHACD（Unity fork）

- **仓库现状（关键修正）**：
  - 原 [kmammou/v-hacd](https://github.com/kmammou/v-hacd) **已声明 EOL**，README 明示迁移到 CoACD。最后稳定版 v4.1（2022，header-only 单文件 `VHACD.h`）。
  - **活跃维护线是 [Unity-Technologies/VHACD](https://github.com/Unity-Technologies/VHACD)**，集成于 Unity 2020.2+。Unreal Engine 自 4.16 起内嵌。
- **License**：BSD-3，商业零障碍。
- **平台/构建**：纯 C++ 无外部依赖，v4 header-only + 一个 TU。macOS arm64 原生跑（CPU + std::thread，无 SIMD 假设）。vcpkg 无 port，Homebrew 无 formula，Conan Center `v-hacd/4.1.0` 有。或源码 vendoring。
- **API/集成**：v4 同时提供 C++（`VHACD::IVHACD` 抽象类 + `Compute()` / `GetNConvexHulls()`）和 C ABI。输入 `float* vertices + uint32_t* indices`，输出 `ConvexHull[]`（每 hull 自带顶点 + 索引）。社区 Python binding `vhacdx`（PyPI，2025-12）。
- **替代方案**：[CoACD](https://github.com/SarahWeiii/CoACD)（SIGGRAPH 2022，MIT，原作者推荐的继任者，collision-aware metric 在凹陷处填得更好；非 header-only，体量更大）。**Phase 1 推荐 Unity VHACD fork**（落地快）；若 Unity fork 也停滞且 bug 修不动，降级 CoACD。
- **结论**：vendor Unity fork 到 `ContentTools/third_party/vhacd/`，单 TU 编进去，CMake `add_library(vhacd STATIC VHACD.cpp)`，暴露 `ComputeConvexDecomposition(MeshView, params) -> std::vector<ConvexHull>`。**一天落地**。

---

## 5. 可选 Baker — Embree — Phase 1 不纳入

- **License**：[Apache 2.0](https://github.com/RenderKit/embree)，含显式专利授权。Embree 4 依赖的 oneTBB 自 2021 起已是 Apache 2.0（UXL Foundation）。CMake `-DEMBREE_TASKING_SYSTEM=INTERNAL` 可彻底去掉 TBB。
- **平台/构建**：v4.4.1（2025 活跃发版），vcpkg `embree` 4.4.0。**Apple Silicon 自 Embree 3.13.0（2021）起官方支持 aarch64**，lighttransport `embree-aarch64` 社区移植已 2021 Fall archived。
- **关键判断（Apple Silicon 性能）**：Embree 核心价值是 SSE/AVX/AVX2/AVX-512 宽 SIMD 内核；**Apple Silicon 只有 NEON 128-bit，宽向量优势消失大半**。M1/M2 上能用、稳定，但相对 Metal RT 或 GPU RT 不会有桌面 x86 + AVX2 那种数量级优势。
- **API**：C++ `RTCDevice / RTCScene / RTCGeometry / rtcIntersect1` + C ABI。需要在数据结构层做 mesh → RTCGeometry 转换。
- **与现有引擎重叠**：本仓库 memory 显示已有 GPU RT pipeline（`indirectTrace` / `SDF-tracing-enhancement.md`）。Phase 1 凸分解和几何处理不需要 raytrace；离线 AO / lightmap bake 用 GPU RT 走 readback 通常更快。
- **结论**：**Phase 1 完全跳过 Embree**。若未来真要做 CPU 离线 baker，优先评估 `meshoptimizer` + 自家 GPU RT 组合；次选 Embree v4.4.1 with `EMBREE_TASKING_SYSTEM=INTERNAL`。

---

## 6. Phase 1 最终选型与最小落地路径

### 6.1 依赖清单

**vcpkg.json（推荐）**：
```json
{
  "name": "primal-contenttools",
  "version-string": "0.1.0",
  "dependencies": [
    { "name": "pmp-library", "version>=": "3.0.0" },
    { "name": "eigen3", "version>=": "3.4.0" },
    { "name": "libigl", "default-features": false, "features": [] }
  ]
}
```

**Git submodule / vendor**：
- `third_party/meshoptimizer/`（已有）
- `ContentTools/vendor/xatlas/{xatlas.cpp,xatlas.h}`（vendor link-in）
- `ContentTools/third_party/vhacd/`（Unity-Technologies/VHACD fork，header-only）

### 6.2 Pipeline 流程（对应原文档 §8 数据流）

```
AI Mesh (FBX/glTF/OBJ)
    │
    ▼ [现有 Importer]
Triangle Mesh (引擎 SoA buffer)
    │
    ▼ [libigl] remove_duplicate_vertices / decimate
Cleanup Pass 1
    │
    ▼ [PMP] SurfaceMesh 转换 → cleanup → remove_degenerate_faces → fill_hole
Geometry Repair
    │
    ▼ [PMP] remesh(target_edge_len, iters=10)
Isotropic Remesh
    │
    ▼ [PMP] catmull_clark_subdivision / loop_subdivision（可选）
Subdivision (optional)
    │
    ▼ [libigl] fast_mesh_boolean / AABB self-intersection 检测
Self-Intersection Check（仅检测报告，不自动修复）
    │
    ▼ [xatlas] AddMesh → Generate
UV Atlas
    │
    ▼ [meshoptimizer] vcacheoptimize / overdrawoptimizer / vfetchoptimizer
Cache Optimization
    │
    ▼ [meshoptimizer] simplifier（多 LOD）
LOD Generation
    │
    ▼ [meshoptimizer] clusterizer + meshletencode
Meshlet Build
    │
    ▼ [Unity VHACD] ComputeConvexDecomposition
Collision Hulls
    │
    ▼ [Asset Compiler]
Engine Asset
```

### 6.3 ContentTools API surface（最小可行）

```cpp
namespace ContentTools {

struct MeshView {
    const float*    positions;  // Nx3
    const float*    normals;    // Nx3, optional
    const float*    uvs;        // Nx2, optional
    const uint32_t* indices;
    size_t          vertex_count;
    size_t          index_count;
};

struct AssetPipelineParams {
    float    target_edge_len       = 0.0f;   // 0 = auto from bbox
    int      remesh_iterations     = 10;
    bool     enable_subdivision    = false;
    int      lod_count             = 4;
    int      meshlet_max_vertices  = 64;
    int      meshlet_max_triangles = 124;
    float    vhacd_volume_percent  = 0.05f;
    int      vhacd_max_hulls       = 16;
};

struct ProcessResult {
    std::vector<LODMesh>     lods;       // 顶点/index/uv，已 vcache opt
    std::vector<MeshletData> meshlets;   // 每 LOD
    std::vector<ConvexHull>  collision;
    ErrorReport              errors;     // self-intersection 报告等
};

ProcessResult ProcessAIAsset(MeshView input, const AssetPipelineParams& params);

}  // namespace ContentTools
```

### 6.4 明确"不做"清单（Phase 1）

- ❌ Retopo（Instant Meshes / QuadriFlow）— 收益≈0，dormant code 维护负担
- ❌ Embree CPU baker — Apple Silicon 性能优势消失，自家 GPU RT 已覆盖
- ❌ CGAL / `igl::copyleft::cgal::*` — GPL 传染
- ❌ Robust self-intersection 自动修复 — Phase 1 只做检测报告，Phase 2 评估
- ❌ AI 语义分割 / 部件识别 / 自动骨骼 — 留给 Phase 2/3

### 6.5 风险与降级路径

| 风险 | 触发条件 | 降级方案 |
|---|---|---|
| PMP 在某些 pathological AI mesh 上 `fill_hole` 失败 | 输入有大非流形区域 | 先用 libigl `remove_unreferenced` + `decimate` 预处理；或跳过 hole fill 标记到 ErrorReport |
| xatlas 在高三角数（>1M）pack 慢 | 大型场景 mesh | 切分 mesh 后并行；或退到 per-chart pack |
| Unity VHACD fork 在某些 thin 部件过度切分 | 武器柄 / 细柱 | 调 `vhacd_max_hulls` 上限 + 后处理合并相邻 hull |
| meshoptimizer LOD 简化率超出目标但拓扑断裂 | 角色关节 | 锁定 boundary vertices（meshoptimizer `simplify` 支持锁定） |

---

## 7. 与现有 ContentTools 的整合

现状盘点（已存在）：
- `ContentTools/CMakeLists.txt` 已集成 meshoptimizer submodule（`third_party/meshoptimizer/`）
- `ContentTools/FbxImporter.{h,cpp}` / `ObjImporter.{h,cpp}` / `Geometry.{h,cpp}` / `TextureImporter.cpp` / `PrimitiveMesh.cpp` / `NormalMapIdentification.{h,cpp}` 已有
- Python 脚本：`BatchConvert.py` / `GeometryProcessor.py` / `MeshletValidator.py` / `pack_geometry.py` / `QuickMeshletValidator.py` / `check_fbx_submeshes.py`

整合增量：
1. **CMakeLists.txt 增量**：
   - `find_package(pmp CONFIG REQUIRED)` + `find_package(libigl CONFIG REQUIRED)`
   - vendor `xatlas.cpp` 单 TU 加入 `PROJECTION_SOURCES`
   - `add_library(vhacd STATIC third_party/vhacd/VHACD.cpp)` + `target_include_directories(... third_party/vhacd)`
2. **新增模块**：
   - `ContentTools/GeometryRepair.{h,cpp}` — PMP + libigl 包装
   - `ContentTools/UVAtlas.{h,cpp}` — xatlas 包装
   - `ContentTools/CollisionDecompose.{h,cpp}` — VHACD 包装
   - `ContentTools/AssetPipeline.{h,cpp}` — 全流程串联（对应原文档 §3 模块职责）
3. **Python 脚本归档**：`GeometryProcessor.py` 等已有脚本的功能与 Phase 1 pipeline 重叠，需在后续设计阶段评估是替换还是保留为 utility。

---

## 8. 后续 Phase 准备（非本次调研范围，仅记录指向）

- **Phase 2**（AI 语义理解）：ONNX Runtime / CoreML 推理框架；Point Transformer V3 / Mask3D / OpenShape / SAM 多视角 / DINOv3 多视角特征。需要 Phase 1 输出 mesh + per-vertex semantic field 作为训练/推理目标。
- **Phase 3**（全自动结构化）：RigNet 自动骨骼 + Socket 预测 + Prefab 生成；与 PCG/WFC 集成。

---

## 附录：调研来源（核心）

- [CGAL License](https://www.cgal.org/license.html)
- [libigl License](https://libigl.github.io/license/) / [libigl copyleft::cgal source](https://github.com/libigl/libigl/blob/main/include/igl/copyleft/cgal/remesh_self_intersections.h)
- [PMP Library GitHub](https://github.com/pmp-library/pmp-library) / [v3.0 release notes](https://danielsieger.com/blog/2023/08/24/pmp-library-version-3.0.html)
- [Instant Meshes](https://github.com/wjakob/instant-meshes) / [QuadriFlow](https://github.com/hjwdzh/QuadriFlow)
- [xatlas](https://github.com/jpcy/xatlas) / [Unity 6.3 lightmap packing](https://discussions.unity.com/t/lightmap-packing-with-xatlas-in-unity-6-3/1670184) / [Blender PR #105821](https://projects.blender.org/blender/blender/pulls/105821)
- [kmammou/v-hacd (EOL notice)](https://github.com/kmammou/v-hacd) / [Unity-Technologies/VHACD](https://github.com/Unity-Technologies/VHACD) / [CoACD](https://github.com/SarahWeiii/CoACD)
- [Embree](https://github.com/RenderKit/embree) / [Apple M1+ system requirements](https://www.intel.com/content/www/us/en/developer/articles/system-requirements/oneapi-rendering-toolkit-embree/2024.html)
