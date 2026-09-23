# glTF 2.0 导入支持（.gltf / .glb）— 完工记录

**日期**：2026-09-23
**状态**：Shipped（内容域测试 + 双格式端到端实证通过）
**目标**：生成式 3D 工具（TRELLIS / Tripo / Hunyuan3D / Meshy 等）的默认产出
格式 `.glb`/`.gltf` 此前被 `ProcessAIAsset` 拒绝（只认 .fbx/.obj），需外部转换
才能进 AI Asset Pipeline。本记录落地原生 glTF 2.0 导入。

## 支持范围（生成式模型子集）

- **容器**：`.glb`（12 字节头 + JSON chunk + BIN chunk，chunk 按 4 字节对齐）与
  `.gltf`（JSON 文档，外置 .bin buffer + 外部图片）。
- **几何**：TRIANGLES（mode=4）primitive → 每 primitive 一个 submesh mesh；
  POSITION（必需）/ NORMAL / TEXCOORD_0 / TANGENT（f32，或 UV 的 normalized
  u8/u16）/ 任意 componentType 索引（u8/u16/u32 等）；view 级 byteStride
  交错访问；索引缺省时生成 0..n-1。
- **节点变换**：matrix（列主序）或 TRS（四元数）全支持，世界矩阵烘焙进顶点；
  法线走逆转置（非均匀缩放正确）+ 归一化；深层级上限 128。
- **材质**：`pbrMetallicRoughness.baseColorTexture` → diffuse、
  `normalTexture` → normal，进 `scene.materials{name, diffuse_texture,
  normal_texture}`，沿用 OBJ 的 Murmur3(名字, seed 123) 哈希引用方案。
- **贴图**：
  - 外部 URI → 原样记录（FBX 式相对路径约定，C# 侧按源目录解析）；
  - data URI → base64 解码后落盘；
  - bufferView 内嵌 → 字节原样落盘（不解码图像，cook 由 C# 做）；
  - 落盘位置：`<源目录>/<stem>_extracted/embedded_<i>.<png|jpg>`；源目录不可
    写时回退临时目录并记录**绝对路径**（C# 的 `Path.Combine` 对绝对路径直通）。
- **明确拒绝**：`extensionsRequired` 非空（Draco / mesh quantization / BasisU
  等）→ 带首个扩展名的清晰报错，宁可失败不误导入；sparse accessor、非
  TRIANGLES mode（跳过该 primitive 并告警）。
- **不做**：skin/动画/相机/COLOR_0 顶点色（OBJ 亦填白）、材质标量因子
  （baseColorFactor 等——`material` 结构无对应字段）、KHR 材质扩展。

## 关键实现决策

1. **JSON 解析复用引擎自研 PfxJson**（`Engine/ParticleFX/core/PfxJson.{h,cpp}`，
   递归下降 + 无异常 + 行号报错）——遵守"单一事实源"纪律，不 vendor 第二个
   JSON 库。PfxJson.cpp 直接编进 ContentTools 目标（ContentTools 已含 Engine
   include 路径，零新依赖）。
2. **行为孪生 ObjImporter**：glTF importer 产出与 OBJ 完全相同的 `scene` IR
   形状后走共享的 `process_scene`/`pack_data`。**Legacy 场景 IR 的对齐不变量**
   （本次踩坑实证）：`positions` 按唯一顶点对齐，而
   **normals/tangents/uv_sets/colors 按 index 槽位对齐**（长度 ==
   raw_indices.size()）——`process_vertices`（Geometry.cpp:199 的 UV 拆分）与
   `split_meshes_by_material` 都按 index-buffer 位置索引这些流。首版按"直觉"
   做成顶点对齐，Vector.h 断言当场抓住。
   - UV 不翻转：glTF 原点在左上，正是引擎约定（OBJ 导入器做的 `1-v` 翻转
     就是翻到这个约定）。
   - 缺 NORMAL 填 (0,0,1) 默认值（OBJ 同款），由 `calculate_normals` 路径重算；
     TANGENT 仅在 `calculate_tangents=0`（用户要求引擎不自算）时导入。
   - coalesce_meshes 设置生效；OBJ 式递归 meshopt LOD 链（80% 目标、5 级）
     由 `generate_lod_chain` 复刻（注意 `std::move(lod)` 后必须用
     `out.back().meshes` 递归，搬空后的局部变量会让链在第 1 级断掉）。
3. **零 ABI 变更**：`ai_asset_pipeline_params` 未动（内嵌贴图提取走源目录/
   临时目录方案而非扩 POD）。C# 侧零逻辑改动——`MeshImporterPlugin.
   CookReferencedTextures` 的现有查找（源目录 BFS + 绝对路径直通）天然覆盖
   提取产物。
4. **GLB chunk 对齐陷阱**（测试驱动抓出）：chunk 长度字段不含尾部 padding，
   下一 chunk 从 4 字节对齐处开始。解析器必须 `pos = (pos + len + 3) & ~3`，
   否则 JSON chunk 后的 padding 会把 BIN chunk 头读成垃圾。

## 触点清单

### C++（ContentTools）
- **新增** `GltfImporter.{h,cpp}`：`gltf_context`（pimpl，~800 行）+
  `EDITOR_INTERFACE ImportGltfAPI(file, scene_data*, callback)`。
- `ContentTools.cpp`：`ProcessAIAsset` / `CookMeshWithSettings` 增加
  gltf/glb 分支（extern "C" 声明）；`get_extension` 注释同步。
- `CMakeLists.txt`：PROJECTION_SOURCES 增 GltfImporter + PfxJson.cpp。
- `ContentTools.vcxproj`：同步三个新源（Windows 构建保持可配）。
- `main.cpp`：`--pipeline` 用法文本标注支持的四种输入格式。

### C#（编辑器）
- `Import/ImportSettingsDialog.axaml.cs`：`HasAnySupportedPipelineFormat`
  增 `.gltf`/`.glb` → 导入面板管线区解锁。
- `Import/Settings/GeometryImportPanel.axaml`：不可用提示文案改写（.dae/.blend
  仍走 legacy；glTF 需 plain glTF 2.0 无 Draco）。
- `Import/Settings/GeometryImportPanelViewModel.cs`：`PipelineAvailable` 文档注释同步。
- `Native/Importers/MeshImporterPlugin.cs`：注释更新（内嵌贴图由 C++ 提取，
  C# cook 链不变）。

### 测试
- **新增** `tests/TestGltfImporter.cpp`（ctest 注册）：
  1. `.glb`：容器解析 / u16 索引 / 节点 scale 烘焙进顶点 / 材质哈希 /
     bufferView 内嵌 PNG 原字节落盘 / sidecar diffuse 引用提取产物 / blob
     6 mesh（base + 5 级 LOD 链）通过 `ValidateSceneBlob`；
  2. **OBJ 行为孪生对拍**：同一四边形走 `ImportObjAPI`，mesh_count / 顶点 /
     索引 / elements_type 四项逐一相等（含 OBJ 翻 UV 后 UV 一致）；
  3. `.gltf`：外置 .bin buffer 解析 + data URI 提取字节校验；
  4. `.glb` 带 `KHR_draco_mesh_compression` → 干净拒绝、空 buffer。

## 门禁数据（2026-09-23）

- 内容域 ctest：**14/15**（新增 TestGltfImporter PASS；TestCollision 2 断言
  失败为**既有问题**——`third_party/VHACD` 子模块工作区脏导致分解质量检查
  不过，与本次改动无关，测试二进制未重编且代码路径零交集）。
- 端到端 CLI：`ContentToolsCLI --pipeline *.glb/*.gltf → *.mesh` 两格式均
  6 meshes / 6 meshlets / 0 errors，`--validate` PASS。
- C# 构建（DebugEditor）：0 错误。
- 构建环境留痕：工作区 `Engine/Graphics/RenderPipeline/StandardRenderPipeline.cpp`
  处于编译不过的 WIP（非本次改动），为链接 ContentTools（`add_dependencies
  (ContentTools Engine)` 仅为排序约束，dylib 不链接 libEngine.a）对**构建
  产物** `Darwin/Debug/Engine/CMakeFiles/Engine.dir/.../StandardRenderPipeline.cpp.o`
  做了时间戳对齐；下次全量构建自然重编覆盖。

## 后续可选

- `.dae`/`.blend`（OpenAssetImport 或 Blender 导出桥）——低优先。
- 材质标量（baseColorFactor/metallicFactor/roughnessFactor）→ 需先扩
  `material` 结构与 sidecar 字段（跨 C++/C#/材质图三端）。
- KHR_texture_transform（UV 变换）按需补。
