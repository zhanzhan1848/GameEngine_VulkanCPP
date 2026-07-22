# macOS 26 SDK 升级后渲染测试优化积压

**日期**: 2026-07-02
**背景**: 升级到 Xcode 26 / macOS 26 SDK 后,`TestNaniteStreamingPipeline` 出现若干模式黑屏/稀疏输出问题。已完成根因分析和紧急修复(见 `tasks/lessons.md`),剩余三项作为优化项延后处理。

---

## 已完成(本次)

- ✅ **光照方向错误**: `lightForward.y = +1.5f`(从下往上打光) → `-1.5f`(从上方打光)。`TestNaniteStreamingPipeline.cpp:2885`
- ✅ **RenderGraph 导入纹理 barrier 缺失**: 引擎层增加 `ImportTexture/ImportBuffer` 的 `initialState` 参数 + RG `InsertBarriers` 消费该状态。modes 0/2/3/6 diag_0 黑屏修复。
- ✅ **手动 barrier 清理**: 移除测试中 4 处手写 `InsertBarrier`,改由 RG 自动生成(modes 1/2/6 diag_0/6 diag_1 fallback)。保留 mode 4 albedo(外部纹理)和 mode 5 SPGI(外部纹理)两处手写 barrier。

---

## 延后优化项

### 1. Mode 1 SSGI 稀疏输出

**现象**: `ssgiVisMode_ == 1`(纯 SSGI 视图)时,只有贴近 mesh 的少量像素有间接光。其余像素全黑。

**疑似根因**: SSGI 本身设计上就是 "subtle by design",只对近距离表面有响应。可能是预期的渲染效果,需要对比 main 分支或参考截图确认。

**验证步骤**:
1. 对比 main 分支同模式输出
2. 检查 `LumenSSGIPass` 配置(HZB mip 起始层级、ray march 步长)
3. 如果是 sparse probe / sparse fill 行为,记录到 lessons.md 关闭此项
4. 如果确实有 bug,定位 shader 或 trace loop

**优先级**: 低(可能不是 bug)

---

### 2. Mode 5 SPGI 像素数极少

**现象**: `ssgiVisMode_ == 5`(纯 SPGI 视图)时,只有零星像素有内容。

**疑似根因**: SPGI (Screen Probe GI) 的 probe 密度可能太低,或 probe placement 算法只覆盖了少量像素。

**验证步骤**:
1. 检查 `ScreenProbeGIPass` 配置(probe 网格密度、trace 半径)
2. 对比 main 分支同模式输出
3. 检查 SPGI 资源是否被正确导入 RG(参考 `initialState = ShaderResource`)
4. 如果 probe density 是设计值,记录到 lessons.md 关闭此项

**优先级**: 中

---

### 3. Mode 6 diag_1 (Fusion) 黑屏 — volumeScatter 未绑定

**现象**: `ssgiVisMode_ == 6` 且 `diagMode == 1` 时,Fusion Pass 2 输出全黑。

**根因**(已确认):
- `EngineTest/shaders/DeferredLighting.metal:576-593` 的 `fragmentFusion` 着色器要求 `volumeScatter [[texture(2)]]`
- 测试代码 `TestNaniteStreamingPipeline.cpp:5139-5143` 只绑定 2 个纹理(sceneColor, indirectColor),slot 2 未绑定
- Metal 对未绑定纹理返回 `(0,0,0,0)`
- 着色器运算 `(scene + indirect) * vol.a + vol.rgb` → `(scene+indirect) * 0 + 0 = 0` → 黑屏

**修复方案**(任选其一):

**方案 A**(推荐): 测试侧绑定 fallback texture
- 新增成员 `volume_scatter_fallback_texture_`(1x1 RGBA = `(0,0,0,1)`,vol.a=1 让 scene+indirect 通过)
- 在 fusion pass 描述符集中扩展为 3 个纹理
- 不改 shader

**方案 B**: 修改 shader 增加默认值
- `fragmentFusion` 增加 `constant bool useVolume [[function_constant(0)]]` 或 `if (volumeScatter.get_width() == 0)` 分支
- Metal 中 texture 总是有有效 handle(无法 truly unbound),不能靠 null 检查
- 修改 shader 不太可行

**方案 C**: 修改 shader 常数化
- 删除 `volumeScatter` 参数,直接 `vol = float4(0,0,0,1)`
- 简单但失去未来接入 volume fog 的扩展性

**建议采用方案 A**。

**优先级**: 中(影响 mode 6 diag_1 视觉验证,但不阻塞主线)

---

## 实施顺序

1. (本步)记录到本文档
2. 执行方案 A 修复 mode 6 diag_1
3. 提交 SDK 升级 + RG barrier 修复 + 光照方向修复 commit
4. 更新 `tasks/lessons.md`
5. 进入 PR2(Staging Allocator)
6. 之后再回头处理 SPGI / SSGI 稀疏输出(优先级低)
