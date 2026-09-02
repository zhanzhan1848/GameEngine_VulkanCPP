# Metal↔Vulkan 视觉对齐——伪影定性与下一步（2026-09-02）

## 已修复（本轮提交 01925cc6..77511788）

1. **白爆**：DDGI gather 的 compute 写纹理对后续采样不可见 → Metal 默认
   ddgi 全幅 blit flush（FusionCompositeModule）。注：部分区域拷贝/仅声明
   RG 边均无效，必须全幅 blit 读。
2. **后处理链从未执行**：FinalBlit 在 AddPass 时预解析瞬态纹理物理句柄
   （分配发生在 Compile）恒 INVALID → 永久回退 fusion 原始帧，TAA/Bloom/
   ToneMapping 被整体裁剪（两平台同病）。修复：FinalBlit exec 时解析。
3. **GI 暖色温差**：Metal ddgi_trace_finalize 为 SC-first，但 SC LightEval
   每帧仅 ~8 张可见卡（393 注册），atlas 大部分黑 → 探针退化为解析白+蓝天。
   修复：对齐 Vulkan 层级 prev_frame_color 主导 + 能量门 isfinite 防御
  （屏幕颜色 NaN 使 dot 门恒 false，prev 路径静默旁路——路径可视化实证）。
4. **直射过曝**：fragmentLighting_gpuDriven 扔掉 ORM（roughness 硬编码
   0.5/metallic 0）。改为读取真实值；light 3.0（直射峰值 ~1.4 对齐 Vulkan
   实测 1.38）。
5. GI gather Metal 升全分辨率（半分辨率边缘深度/法线错位=块状锯齿）。
6. Vulkan 黑屏/崩溃：新启用的 Vulkan 后处理链存量 bug（Bloom 管线
   INVALID、ToneMapping render pass 附件畸形→validation
   AttachmentTracker 崩溃）——Vulkan 默认回退 legacy，基准观感已恢复。

## 遗留伪影定性（用户标注，A/B 排除记录）

1. **楔形加性亮斑**（左下暗墙，恒定位置）：三角形楔状、楼梯边缘。
   已排除：SSGI（SSGI_SILENCE=1 仍在）、阴影泄漏（翻转光照方向仍在）、
   SC atlas（已降为 fallback 仍在）。剩余候选：SSAO 半分辨率滤波、
   shadow visibility 半分辨率上采样、DDGI 探针穿墙泄漏（gather 无
   depth/normal 权重，两平台同式但内容对比度不同）。定位法：逐项加
   静音开关二分（SSAO 的 mute、shadow vis 直连、DDGI 场归零）。
2. **阴影边界楼梯**（中央竖条）：shadow map texel 台阶。对比 Vulkan
   同机位边界更柔——查两侧 ShadowMapModule 配置差（分辨率/PCF 核/
   cascade 覆盖）。
3. **几何边缘锯齿**：legacy 无 TAA。阻塞于后处理链启用即楔死 GPU 驱动
   （validation 已默认开，PRIMAL_FULL_POST=1 复现抓报告）。

## 工具入库

- PRIMAL_IND_ASYNC=<掩码>：无同步污染的多路条带统计（RGB 分量），
  兼任白爆 flush 的生产路径（Metal 默认 mask=2）。
- PRIMAL_LIGHT_DIR="x,y,z"：GI 光照响应 A/B（实测 GI 场随光翻转迁移）。
- SPGI_CAM + SPGI_CAM_LOCK：两平台同机位对比（标定前提）。
- [BLOOM/TONE-EXEC] abort stderr、RG 分配失败日志、BeginRenderPass
  附件缺失日志。

## 下一轮主线

1. 驱动楔死根因：PRIMAL_FULL_POST=1 + validation 报告 → 修非法用法 →
   全链安全启用 → TAA 消锯齿（伪影 3）→ 曝光标定。
2. Vulkan 后处理链逐环（Bloom 管线、ToneMapping 附件）。
3. 伪影 1/2 的二分定位（各加静音开关）。
4. GI 弹射幅度档位（FusionIndirect ddgiBase 0.7 与 Vulkan 7.5 直射的
   配比差异——待同机位定量）。
