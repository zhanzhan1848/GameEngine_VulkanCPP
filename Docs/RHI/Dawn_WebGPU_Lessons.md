# Dawn WebGPU 渲染 Lessons Learned

## 1. WGSL Normal/Tangent 解包字节序

**问题**: 拱门(matIdx=5)渲染全黑，看起来"没纹理"
**根因**: `unpackNormal`/`unpackTangent` 中 u32 的高低16位提取顺序与 Metal `packed_ushort2` 不一致

源数据是小端序 `packed_ushort2`:
- bytes 0-1: x (u16, 低16位)
- bytes 2-3: y (u16, 高16位)

Metal 读取: `float2(packed_ushort2)` → `[x, y]` (内存顺序)
WGSL 错误写法:
```wgsl
let nx = f32((packed >> 16u) & 0xFFFFu);  // 高16位 = y (错了!)
let ny = f32(packed & 0xFFFFu);            // 低16位 = x (错了!)
```
WGSL 正确写法:
```wgsl
let nx = f32(packed & 0xFFFFu);            // 低16位 = x
let ny = f32((packed >> 16u) & 0xFFFFu);  // 高16位 = y
```

**影响**: 法线 x↔y 互换导致 PBR 光照 NdotL≈0，几何体几乎全黑

**教训**: 从 u32 手动提取 packed u16 时，必须匹配平台字节序。小端序下 `packed & 0xFFFF` = 第一个 u16(低地址) = x

---

## 2. WGSL UV V-flip

**问题**: 二楼布料(matIdx=14-19)纹理错乱
**根因**: 缺少 UV.y 翻转

模型数据使用 OpenGL UV 约定 (V=0 在底部):
- Metal: `out.uv = float2(rawUV.x, 1.0 - rawUV.y)`
- WGSL 缺少翻转: `output.uv = input.uv`

布料 UV 重度平铺 (U=[-1.4, 5.0])，V 方向错误 + 平铺 = 看起来纹理错乱

**修复**:
```wgsl
output.uv = vec2<f32>(input.uv.x, 1.0 - input.uv.y);
```

**教训**: 不同图形 API 的纹理坐标系可能不同 (OpenGL vs DirectX vs Metal vs WebGPU)。从 .model 二进制加载的 UV 数据需确认其约定，通常 OpenGL 约定需要 V-flip

---

## 3. 元素缓冲区 24 字节步幅 (macOS)

`static_normal_texture` 在 macOS 上 `sizeof = 24` (而非预期的 20)，因为 `math::v2 = simd::float2` 有 8 字节对齐要求，编译器在 offset 12-15 插入了 4 字节 padding:

```
Offset 0-2:   color[3]     (3 bytes, u8 RGB)
Offset 3:     t_sign       (1 byte, u8)
Offset 4-7:   normal[2]    (4 bytes, 2x u16)
Offset 8-11:  tangent[2]   (4 bytes, 2x u16)
Offset 12-15: PADDING      (4 bytes)
Offset 16-23: uv           (8 bytes, float2)
```

SceneDataAdapter 的交错处理已正确处理这个 padding:
```cpp
memcpy(dstElem, srcElem, 12);           // ColorTSign + Normal + Tangent
memcpy(dstElem + 12, srcElem + 16, 8);  // UV (skip padding)
```

---

## 4. Sampler Comparison Type 不兼容

**问题**: 材质纹理采样报 Dawn validation error: "Comparison sampler is incompatible with non-comparison sampler binding"
**根因**: `SamplerDesc::comparisonFunc` 默认值为 `ComparisonFunc::Always`，Dawn 将其创建为 comparison sampler。但 descriptor set layout 声明的是 `WGPUSamplerBindingType_Filtering`。

**修复**: Dawn 端创建材质 sampler 时显式设置 `comparisonFunc = ComparisonFunc::Never`

**教训**: WebGPU 区分 filtering sampler 和 comparison sampler，两者不可混用。默认值 `Always` 会隐式创建 comparison sampler。

---

## 5. DepthPrePass + DepthEqual 导致无几何绘制

**问题**: Dawn 上只渲染背景色，无任何几何体
**根因**: DepthPrePass 创建 depth-only pipeline (无 color target)。如果 pipeline 创建失败（shader 编译错误等），深度缓冲保持 1.0。OpaquePass 用 `depthFunc = Equal`，所有片段深度 ≠ 1.0 → 全部剔除。

**修复**: Dawn 跳过 DepthPrePass，使用普通 `Less` 深度测试，depth loadOp 设为 Clear。

**教训**: DepthPrePass + DepthEqual 是优化手段而非必需。在新后端验证时，先用基础路径通过再添加优化。

---

## 6. RenderMesh Registry 空

**问题**: 有 393 个 mesh 但 OpaquePass 找不到任何 mesh (RenderMesh::GetByEntityId 返回 null)
**根因**: `SceneDataAdapter::LoadRenderItemData` 传 `primal::id::invalid_id` 给 `RenderMesh::Create()`。`Register()` 跳过 invalid_id，registry 为空。

**修复**: 传 `meshEntityId` 而非 `invalid_id`

**教训**: Registry pattern 中 ID 的传递链路需要端到端验证。free-list 分配的 ID 从 0 开始递增，与 invalid_id (0xFFFFFFFF) 不同。

---

## 7. Frustum::FromMatrix 行/列混淆 (关键 bug)

**问题**: 视锥体裁剪错误剔除视锥体内的元素 (46/393 visible → 应该 ~300+)
**根因**: `Frustum::FromMatrix()` 对 column-major 矩阵使用了 **列加法** (`col3[i] + col0[i]`)，但 Gribbs-Hartmann 方法要求 **行加法** (`col_j[3] + col_j[0]`)。

Column-major `M * p`:
```
clip.x = row0 · p = {col0[0], col1[0], col2[0], col3[0]} · p
clip.w = row3 · p = {col0[3], col1[3], col2[3], col3[3]} · p
```

Left 平面 (clip.x + clip.w >= 0) 应为 row0 + row3:
```cpp
// 正确: {col0[0]+col0[3], col1[0]+col1[3], col2[0]+col2[3], col3[0]+col3[3]}
// 错误: {col0[0]+col3[0], col0[1]+col3[1], col0[2]+col3[2], col0[3]+col3[3]}
```

**修复**: 将所有 6 个平面的提取从 `columns[j1][i] ± columns[j2][i]` 改为 `columns[j][i1] ± columns[j][i2]`

**教训**: column-major 矩阵的行提取模式是 `{col0[i], col1[i], col2[i], col3[i]}`。Gribbs-Hartmann 方法操作的是矩阵的行，不是列。这个 bug 影响所有 6 个平面，但因为 perspective 矩阵接近对角，在简单场景下部分平面碰巧接近正确。

---

## 8. 近裁面 OpenGL vs Metal 深度范围

**问题**: 视锥体上半部分的物体被错误剔除 (天花板等)
**根因**: Metal/Dawn 使用 [0,1] 深度范围，近裁面条件是 `z_clip >= 0` (plane = row2)。代码使用了 OpenGL 的 `z_clip >= -w_clip` (plane = row2 + row3)，导致近裁面过于激进。

**修复**: 近裁面从 `row2 + row3` 改为 `row2`。远裁面 `row3 - row2` 不受影响（OpenGL 和 Metal 相同）。

**教训**: 不同图形 API 的 clip-space 深度范围不同：
- OpenGL: z ∈ [-w, w] → Near = row2+row3, Far = row3-row2
- Metal/WebGPU/Vulkan: z ∈ [0, w] → Near = row2, Far = row3-row2
