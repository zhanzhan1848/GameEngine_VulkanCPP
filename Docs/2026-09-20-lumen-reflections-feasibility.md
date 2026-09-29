# Lumen Reflections 对应物：间接镜面缺口立项评估（P6）

> 日期：2026-09-20 ｜ 状态：📋 已并入整合计划（本文档保留作选项调研依据）
> **→ 实施计划见 `Docs/2026-09-20-sc-correctness-and-reflections-integrated-plan.md`**
> （选项 A/B/C 编排为 R3/R4/R5 三阶段；SC 正确性/覆盖缺口为其前置 R1/R2）
> 上游：`Docs/2026-09-20-sponza-vs-lumen-visual-gap-analysis.md` §3 显示链缺口清单
> 定位：功能缺口（非本轮观感瓶颈）；本文档给出选项、建议路线与验收口径，供排期决策。

---

## 1. 现状（Vulkan 链）

间接 specular 的现有来源（`FusionComposite.frag:51-63`）：

1. **DDGI 半分辨率 glossy 球谐 relay**：`GIGatherModule.cpp:165-168` 输出
   `RGB=SH(R) radiance, A=NdotV`；合成 `ddgiGlossy × (F0·ab.x + ab.y) × (1-rough²) × 0.75`
   （`fusion_glossy_strength`，`GIDebugParams.h:42`）。低频：无反向/形状正确的反射，
   只提供"表面朝向的环境色"。
2. **SSR**：已实现（HZB trace + 时间累积 + 粗糙度衰减，`SSRPass.cpp`；强度 0.7 /
   max_roughness 0.85 / max_trace 30，`PipelineQualityConfig.h:101-108`），
   **默认关**（`SSR` env 开）。屏幕外无 fallback（DDGI relay 兜底）。
3. 直射 IBL specular 0.35（sunset.hdr prefilter cube）。

与 UE Lumen Reflections 的差距：无屏幕外反射体的形状正确反射（Lumen 用 SC 卡片
作为屏幕外 radiance 源 trace）；粗糙度上的镜像→漫射过渡依赖低频球谐。

## 2. 选项

| 选项 | 内容 | 工作量 | 风险 |
|---|---|---|---|
| **A. SC 卡片 specular trace** | Reflection ray 未命中屏幕 → 沿射线查 SC 卡片 radiance（复用 `scQueryCardCandidate` 公共真值层 + 网格加速，与 WRC finalize 同构） | 中-大：新 trace pass + FusionComposite 接线 + 噪声/时间滤波 | 卡片覆盖率（boundsFail 50%）直接限制反射质量；需 P0 之后的诚实漏斗基线 |
| **B. SSR 默认开 + DDGI relay 兜底** | 现成两段拼接：屏幕内 SSR、屏幕外 DDGI 球谐（已有 ssrW 接力公式，`FusionComposite.frag:51-55` 注释） | 小：默认值 + 观感调参（强度/粗糙度上限/去噪） | 屏幕外反射仍是低频色块（非形状正确）；SSR 噪声 |
| **C. WRC 探针 specular 采样** | WRC 近场探针已存 per-direction radiance（Fibonacci K=64），反射向量直接探针插值——比 SH relay 高频 | 中：探针场 specular 采样 + 与 SSR 接力；依赖 `PRIMAL_WRC_ENABLE` 默认化（P4） | 探针间距 2m 限制高频；视线相关闪烁需滤波 |

## 3. 建议

分两步走：
1. **先 B 后 A**：B 是纯默认值/调参（一个会话内可完成 + 观感 A/B），立即改善
   湿面/光滑石材的屏幕内反射；A 作为独立专项（依赖 SC 查询产出率在 GI 线标准
   配方稳定 >40% 后立项，否则卡片覆盖缺口直接映到反射缺失）。
2. C 与 A 二选一推进（C 复用 WRC 基建、边际成本低，建议优先 C 的原型验证：
   在 `WRCTraceFinalize` 的探针 radiance 上做 specular lobe 积分打样）。

## 4. 验收口径

- 光滑石材（roughness<0.3）屏幕内：SSR on/off 对照抓帧，反射可见且无时间闪烁
  （frame 700-900 三帧差分 < 2/255）。
- 屏幕外反射（背对中庭的光滑面）：B 验收 = DDGI relay 无黑边；A/C 验收 =
  形状近似正确（至少明暗结构对）。
- 性能：1080p 下增加 ≤ 1.5ms（B）/ ≤ 3ms（A/C）。
- 回滚：B = `PRIMAL_SSR_DEFAULT=0`；A/C 独立开关。
