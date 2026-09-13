# PFX emissive→GI 注入设计 —— 发光粒子接入 Lumen T4.4 emissive 闭环

- 日期：2026-09-13
- 上游：`Docs/2026-09-12-particle-framework-architecture.md` §7-N6（light 渲染器已落地，§10.6.3）、
  `Docs/2026-09-12-particle-framework-architecture.md` §10.6.3（"注入 DeferredLighting 与 DDGI 探头更新属 Lumen 核心改造"的结论修订——见 §3 选项分析，存在免核心改造路径）
- 本文回答：发光粒子的 radiance 以什么形式、从哪个注入点进入 GI 反弹链；T4.4 强度门禁如何复用；分几期、每期的门禁是什么。

---

## 0. 一页结论

1. **注入点选 T4.4 现有链路，不动 Lumen 核心**。T4.4 已提供完整闭环：per-instance emissive 表（1024×vec4 host-visible）→ capture 次帧散射进 emissive_atlas（RGBA16_Float）→ LightEval 采样（**8.0 钳制**）→ SC↔probe 反弹。PFX 只需作为该链路的**新生产者**写入表——链路每一环（散射、钳制、反弹、量化门禁）原样复用。
2. **v1 = 聚合单槽注入**：光渲染器每帧把全部活光斑的 radiance 聚合为单一 rgb 值（≤8.0），写入保留实例槽。语义 = "发光粒子群对 GI 的整体能量贡献"（空间均匀），实现零 shader 改动。
3. **v2 = 分块空间注入**（可选后续）：把场景按 probe 粒度分块，每块聚合各自 radiance 写入多槽——恢复空间局部性；仍零 shader 改动，但需块→槽映射与占用管理。
4. **明确不做**（本文档非目标）：把每颗粒子作为独立点光写进 LightEval 光源循环（需 Lumen 核心改动 + shader 双后端）；粒子 radiance 直接写入 DDGI 探头（绕过 capture 的写路径会破坏 T4.4 能量审计）。

---

## 1. T4.4 现有机制（注入面盘点）

```
生产者（写 per-instance 表）
  SetInstanceEmissive(instance_id, rgb)         ← 唯一写入口，host-visible mapped，
                                                  a>0.5 覆盖场景级 PRIMAL_SC_EMISSIVE
    ↓ capture（下一帧）
emissive_atlas（RGBA16_Float，卡面布局）
    ↓ LightEval（采样 + 8.0 钳制）
SC↔probe 反弹（GI 间接光）
    ↓
T4.4 强度门禁（量化统计，PRIMAL 门禁沿用）
```

关键事实（代码依据，2026-09-13）：

- `SurfaceCachePass::SetInstanceEmissive(uint32_t, math::v3)`：范围检查（kMaxEmissiveInstances）、`instance_emissive_buffer_` mapped 直写、alpha=1 生效语义。
- 表容量 1024×vec4，零初始化 = 无 emissive；capture 每帧消费。
- LightEval 侧钳制常量 8.0（强度上限，防止 GI 能量爆炸）；T4.4 强度门禁统计该值。
- 场景级源：`PRIMAL_SC_EMISSIVE="r,g,b"` 环境变量，a≤0.5 时生效。

## 2. 注入点选项分析

| 选项 | 机制 | 空间局部性 | Lumen 核心改动 | 结论 |
|---|---|---|---|---|
| A. per-particle 点光进 LightEval 循环 | 光源数组 + shader 循环 | 点级 | **大**（shader 双后端 + 输入结构） | 否决（v1） |
| B. 直写 DDGI 探头 | 绕过 capture 写探头辐射 | 探头级 | 中（破坏 T4.4 能量审计链） | 否决（能量审计不可破） |
| C1. **聚合单槽 → SetInstanceEmissive** | 复用 T4.4 全链 | 无（场景均匀） | **零** | **v1 采纳** |
| C2. 分块多槽 → SetInstanceEmissive | 同上 + 块映射 | 块级 | 零 | v2 可选 |
| D. screen-space 注入光照缓冲 | 光照输出前加色 | 像素级 | 零 | 已由 light 渲染器覆盖（非 GI） |

决策：**C1 先行**。理由：T4.4 的 capture→atlas→LightEval→反弹链与钳制/门禁全部复用，PFX 侧仅新增一个聚合写入器（纯 CPU、可单测）；空间局部性损失由 v2 补救，且发光粒子群的 GI 贡献本就近似环境项。

## 3. 设计

### 3.1 v1：聚合单槽注入

```cpp
// Engine/ParticleFX/interop/EmissiveGI.h
namespace primal::pfx::interop {

struct emissive_gi_config {
    u32   reserved_instance_id;        // 保留槽（默认 kMaxEmissiveInstances-1）
    f32   radiance_scale{ 1.f };       // 聚合 radiance 缩放（与 T4.4 8.0 钳制配合）
    u32   min_alive{ 1 };              // 活光斑低于此值不注入
};

class emissive_gi_injector {
public:
    explicit emissive_gi_injector(emissive_gi_config cfg);
    // 每帧（light 渲染器 stage 之后）：聚合光斑 radiance → SetInstanceEmissive
    void tick(const std::vector<light_sample>& lights,
              SurfaceCacheEmissiveWriter& writer);   // 窄接口，见 §4
private:
    emissive_gi_config _cfg;
};

} // namespace primal::pfx::interop
```

- **聚合公式**：`radiance = Σ(light.color × light.intensity) × radiance_scale`，逐通道钳制至 ≤8.0（T4.4 门禁值）。
- **保留槽**：`kMaxEmissiveInstances-1`（1023），与网格实例表共用但约定末槽专用；`ParticleFX` 的 debug dump 打印该槽当前值（可观测）。
- **窄接口 `SurfaceCacheEmissiveWriter`**：`void write(u32 id, math::v3 rgb)`——由宿主以 SurfaceCachePass::SetInstanceEmissive 适配。PFX interop 不直接依赖 SurfaceCachePass 类型（松耦合规则 3 的同类处理）。

### 3.2 v2：分块空间注入（可选）

- 场景 AABB → N×N×N 块（默认 4³），每块一槽；聚合改为分块求和；块槽位表在 emitter_program 里静态声明。
- 依赖 probe 粒度与块尺寸匹配——等 DDGI 侧需求明确再排期，本文档只占位接口。

### 3.3 强度门禁复用

- 钳制：注入前逐通道 ≤8.0（与 LightEval 一致——双重钳制无害，保证下游不变量）。
- 门禁：沿用 T4.4 强度量化统计，新增断言「聚合 radiance ≤ 8.0」与「粒子光源全灭 ⇒ 注入值归零」两条。

## 4. 集成与接线

- 接线点：宿主每帧 `light 渲染器 stage_lights` 之后调用 `injector.tick(...)`（PipelineTick 语义内，capture 之前）。
- 宿主适配：`SurfaceCacheEmissiveWriter` 的实现 = `SurfaceCachePass::SetInstanceEmissive` 的 lambda 包装（由宿主持有 SurfaceCachePass 指针注入）。
- 调试：`primal-engine pfx-debug` 增加 `--emissive` 段（聚合 radiance 与钳制命中计数）。

## 5. 门禁

1. 聚合纯函数单测：已知光斑集合 → 精确聚合值 + 8.0 钳制边界。
2. 保留槽写入单测：写入值/范围防御（id 越界告警不崩）。
3. 集成差分门禁：同场景两次运行，光斑全灭 vs 全开，capture 后 emissive_atlas 亮度差 > 阈值（证明进入 GI 链）；T4.4 强度门禁不回归。
4. 预算：`radiance_scale` 调大至钳制边界时无 NaN/Inf（fuzz 几组极值）。

## 6. 里程碑与工作量

| 期 | 内容 | 估工 |
|---|---|---|
| v1 | 聚合单槽注入 + 窄接口 + 单测 + pfx-debug --emissive + Sponza 差分门禁 | 2-3 人日 |
| v2 | 分块空间注入（可选） | 3-5 人日 |

依赖：无新增第三方库；不改 Lumen shader/管线；SurfaceCachePass 零改动（复用既有 SetInstanceEmissive）。
