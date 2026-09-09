# T4.2 离线 Card Bake — 新 Session 提示词

> 把下面整段作为新 session 的首条消息使用。前置：GI 路线图见 `tasks/gi-remaining-roadmap.md` 第四批。

---

## 任务

在 ContentTools 实现**离线 card bake**：体素化 mesh → 生成 6 向卡片布局 + Surfel 数据，持久化进 mesh 资产；运行时 `CardGenerator` 优先加载烘焙结果，把当前的运行时 AABB 派生降级为 fallback。这是路线图第四批的 T4.2。

## 背景事实（已核实，直接信任）

- **运行时现状**：`Engine/Graphics/Lumen/SurfaceCache/CardGenerator.{h,cpp}` 的 `RebuildCardAllocation()` 从 `meshes_`（经 `RegisterMesh(aabb_min, aabb_max, instance_id)` 注册）推导卡片：每个 AABB 最多 6 张轴对齐卡（`axis | dir<<8`），卡片 center = AABB 中心、extent = AABB 半径、分辨率 = 投影尺寸 × 64 texels/unit，投影面积 < 最大投影 5% 的薄视角被过滤。调用点在 `StandardRenderPipeline.cpp` 搜索 `RegisterMesh`（Rebind 后用 `bounds_center ± bounds_radius` 球派生的立方 AABB——**这就是要被烘焙数据替换的粗糙来源**）。
- **资产载体**：`Engine/Graphics/RHI/Core/RHIMeshAsset.h` 的 `RHIMeshAsset` 已有 `RHISDFData sdf` 字段（resolution[3] / bounds_min[3] / bounds_max[3] / data(u16) / voxels(u8) / vector_field(u16)），运行时 `RHIGpuMesh.cpp:190-256` 已能上传三张 3D 纹理。card bake 输出应仿照此模式加一个平级字段（如 `RHICardBakeData`：per-card {axis, dir, center, extent, resolution} + surfel 数据），走同一条 cook/加载路径。
- **ContentTools 现状**：`ContentTools/common/SDF.cpp` 已有 mesh→SDF 体素化（生成 RHISDFData 的那套），`ObjImporter/FbxImporter`、`ToolsCommon.h`、`main.cpp`；离线执行器是 `Tools/PrimalEngineCli`（primal-engine，子命令 render/cook-mesh/schema，经 `scripts/primal.sh` 调用）。**新增子命令或扩展现有 cook-mesh 均可，倾向扩展 cook-mesh**。
- 卡片消费端无需改：`SurfaceCacheCard` 结构（center/extent/axis_direction/resolution/atlas_offset/mesh_instance_id）是稳定 ABI，bake 结果只需填出更准的 `center/extent/resolution`。

## 实施步骤

1. **读代码**：`CardGenerator.cpp` 全文（重点是 RebuildCardAllocation 的 6 向推导与分辨率公式）、`RHIMeshAsset.h`、`ContentTools/common/SDF.cpp` 的体素化入口、`Tools/PrimalEngineCli/main.cpp` 的 cook-mesh 子命令结构。
2. **离线算法**（ContentTools 新文件，如 `common/CardBake.cpp`）：
   - 输入 mesh（复用 SDF.cpp 的体素化得到 occupancy 网格，分辨率建议 64³，可配置）
   - 对 6 个轴方向投影 occupancy → 每向得到**紧致投影包围盒**（非 0 体素的 min/max，单位=体素）→ 换算成该向卡片的 center/extent（相比现在用整 mesh AABB，薄墙/非立方体的卡片面积会显著缩小）
   - 分辨率：沿用 64 texels/unit 公式，钳到 [16, 256]；投影面积 < 5% 最大投影的向剔除（与运行时规则一致，保证 fallback 语义兼容）
   - （可选加分）Surfel：每非空体素朝 6 向暴露面生成 surfel {pos, normal, area}
3. **资产扩展**：`RHIMeshAsset` 加 `RHICardBakeData card_bake`（POD 数组 + static_assert 锁尺寸，参考 `AssetAPI.h` 惯例）；ContentTools 写入；`RHIGpuMesh` 加载持有；注意 ABI 纪律——只增不改。
4. **运行时消费**：`CardGenerator` 新增 `RegisterMeshBaked(const RHICardBakeData&, uint32_t instance_id)`；`StandardRenderPipeline` 的 RegisterMesh 循环改为：实例的 mesh 有 bake 数据 → 走 baked，否则 fallback 现有 AABB 推导。`RebuildCardAllocation` 遍历逻辑兼容两种来源（建议给 `meshes_` 条目加 `baked_cards` 可选字段而非平行数组）。
5. **一次性日志**：加载路径打印 `[LumenSC] cards source = baked (N meshes) / fallback (M meshes)`，与 `[LumenSC] Registered ...` 并列，验收可观测。

## 验证（不要跳过）

```bash
# ContentTools 构建 + 离线 bake 往返
cmake --build Darwin/Debug -j8 --target ContentTools primal-engine
# 用测试项目的一个 mesh 赑 cook-mesh，确认输出含 card_bake 字段
./scripts/primal.sh --help   # 确认子命令面

# 单测：ContentTools/tests 或 EngineTest/UnitTests 加一个合成 mesh（如 2×0.2×1 薄板）的
# bake 单测——断言薄向卡片被剔除、投影包围盒紧致（< 整 AABB 投影）

# 运行时门禁（GI 数值不应变化——bake 只改卡片布局，不改数学）
ctest --test-dir Darwin/Debug -R "Unit" --output-on-failure
Darwin/Debug/TestModularPipeline 2>&1 | tee /tmp/t42.log   # 跑 90s，kill 后查 [LumenSC] 日志与无 fault
# 若跑 Vulkan：先 rm -f Darwin/Debug/libEngine.a（两棵构建树共用该库，交叉构建会混链）
```

## 已知坑

- 两棵构建树共用 `Darwin/Debug/libEngine.a`，切换前 `rm -f`（AGENTS.md）
- 卡片总数受 `LumenConfig::surface_cache_max_cards` 硬上限；bake 使卡片更紧致通常会**减少**所需卡数，但 RebuildCardAllocation 里 `cards_.size() >= max_cards_` 的截断逻辑要保证 baked 路径也生效
- `SC_CARDVIEW` 默认开（Metal 用 GPUDrivenDrawPipeline 卡片光栅化采集）；Vulkan 默认走 GBuffer-gather compute capture（`SC_CARDVIEW=0` 才走）。bake 改的是卡片布局，两条采集路径都受益，无需区分
- 别动 `SurfaceCacheCard` 的字段布局（GPU ABI，双端 shader 消费）
