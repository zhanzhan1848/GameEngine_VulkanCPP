# Virtual Texture 使用手册与调参指南

- 日期：2026-08-24
- 适用：`feature/virtual-texture-m0` 分支（M0–M6）
- 关联设计：`Docs/2026-08-21-virtual-texture-system-design.md`

---

## 1. 快速上手

### 1.1 离线烘焙（SVT）

```bash
# 单张
build/Tools/vtbake input.png output.vtex --kind auto

# Sponza 全套（75 张，双格式 BCn+ASTC）
scripts/vt/bake_sponza.sh /tmp/sponza_vt
```

`--kind auto` 按文件名推断（`_normal`→normal/BC5，`_roughness|_metallic`→scalar/BC4，其余 color/BC1）。

### 1.2 多层烘焙（Layer 打包，M2.7）

`bake_layers()` API：albedo/normal/orm 三层进单一 `.vtex`，采样一次页表查找取全层。

### 1.3 运行时接入

```cpp
auto& sys = vt::virtual_texture_system::instance();
sys.initialize(device);                       // 默认配置
u32 vt = sys.register_texture(data, size);    // SVT(磁盘容器)
u32 rvt = sys.register_runtime_texture(       // RVT(GPU 生产)
    256, 256, u32(DataFormat::RGBA8_UNorm), &volume);

// 帧循环:材质 pass 采样(内联 feedback)→ 帧末拷贝 → update
sys.record_feedback(cb, frame);               // 或 AttachVTFeedback
si.signalFence = sys.feedback_fence_handle(frame);
device->Submit(si);
sys.update(frame);                            // 流送/上传/页表/淘汰
```

渲染器侧（ForwardSceneRenderer）：`EnableVirtualTextures(vtexDir)` + `AttachVTFeedback(cb, frame, si)`。

RenderGraph 集成：`AddVTStreamingPass(graph, frame)`（页表 RG 声明 + execute 驱动 update）。

### 1.4 RVT（M5）

```cpp
vt::runtime_vt_volume volume;
vt::runtime_vt_volume::desc d;
d.world_x0 = 0; d.world_z0 = 0; d.world_x1 = 256; d.world_z1 = 256;
u32 rvt = sys.register_runtime_texture(256, 256,
            u32(DataFormat::RGBA8_UNorm), &volume);
d.vt_id = rvt;
volume.initialize(device, d);
```

三 pass 页生产：Draw 捕获 → 压缩 CS → GPUFinalizer 存储镜像直写。

---

## 2. 调参指南（vt_cvars，名字对齐 r.VT.*）

| cvar | 默认 | 说明 |
|---|---|---|
| `max_uploads_per_frame` | 8 | 每页上传节流（游戏 8 / 编辑器 64；提高=更快收敛、帧时波动增大） |
| `page_free_threshold` | 15 | 淘汰触发空闲页阈值（提高=更积极释放、更频繁重加载） |
| `pool_size_scale` | 1.0 | 物理池缩放（移动端 0.5） |
| `feedback_factor` | 2 | feedback N×N 降采样（提高=CPU/GPU 开销降低、流送延迟增大） |
| `space_release_frames` | 150 | 页表空间闲置回收帧数（0=关闭） |
| `max_anisotropy` | 4 | 各向异性上限（受 border=4 约束） |

修改示例：

```cpp
auto& cv = vt::vt_cvars::instance();
cv.max_uploads_per_frame = 16;   // 加快冷启动收敛
```

---

## 3. 格式与平台

| 平台 | tile 格式 | 说明 |
|---|---|---|
| Windows/Linux | BCn | TextureImporter/DirectXTex |
| Apple/mobile | ASTC | `third_party/astc-encoder` 离线烘焙 |
| MoltenVK 注意 | — | 硬件线性过滤/textureGrad/textureLod 在 ASTC atlas 上实测异常（恒周期平均）— 验证端用手动双线性；fragment 端合规驱动保留硬件线性 |

---

## 4. 调试

- 统计：`sys.stats()`（驻留/请求/上传/淘汰/命中率 `hit_rate()`）
- 页表回读：`page_table_texture()` 带 `CopySource` 用法，`CopyTextureToBuffer` 直接探查 RGBA32UI 条目
- 压测基线：`TestVTSVTRuntime`（11 用例：闭环/压测/材质 stack/GBufferVT 片元/Layer 打包/过滤质量/RG pass/材质注册/RVT/RVT 验收/cvar+长跑）

---

## 5. 常见问题

**Q: 页驻留但采样黑？** 检查页表条目（RGBA 四通道层打包）与 atlas UAV 条件化（ASTC/BCn 池无 UAV）。

**Q: RVT 页不生产？** worker peek-before-pop 让路 + 挂 producer 前 root 丢弃已修复；确认 volume 在 register 之后 initialize。

**Q: pop-in？** 提高 `max_uploads_per_frame`；启用 `prefetch_rect` 预取相机前方区域。
