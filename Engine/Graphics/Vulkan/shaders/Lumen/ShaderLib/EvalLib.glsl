#ifndef LUMEN_EVAL_LIB_GLSL
#define LUMEN_EVAL_LIB_GLSL

// ============================================================================
// EvalLib.glsl — DDGI probe SH 评基单源（GI 能量所有权修复 Phase 1b；审计
// 文档 §2.8/P0：cos-lobe 因子曾三处手写展开，靠注释互相警告维持一致）。
//
// 单位铁律（审计文档 §5，消费端头注释同款）：
//   1. DDGI probe SH 是 radiance 投影（mcWeight = 4π/N）。evalProbeRadianceSH9
//      返回方向均值 radiance L̄(d)；evalProbeIrradianceSH9 内置 cos-lobe
//      因子（L1 ×2/3、L2 ×1/4），返回 E(n)/π——消费端按 PROBE_SH_RAD_TO_IRRAD
//      = π 惯例 ×π 得 E。
//   2. cos-lobe 因子只存在于本文件——消费端严禁自行展开（显示增益同理
//      永不进入反馈边）。
//
// 符号约定 = DDGIUpdateIrradiance 的 real-SH 投影基（Y1y/Y1x 为负）。
// 注意：EncodeLib.glsl 的 evalSH9 是 SPGI ScreenProbe 投影（Y1 全正），
// 两者不是同一约定，勿混用。
//
// Metal 端等价复制实现（行为一致的复制，注释互指）：
//   DDGIGIGather.metal / SurfaceCacheLightEval.metal 的
//   lumenEvalProbeIrradianceSH9 / lumenEvalProbeRadianceSH9。
// Binding-free 纯函数库：无 uniform、无嵌套 #include（兼容 build_spv.sh
// 的 awk 内联与 glslangValidator 双路径）。
// ============================================================================

// float32 常量（与原 GIGather 内联展开逐位一致的高精度字面量）。
const float EVAL_C0   = 0.28209479;  // L0, Y00
const float EVAL_C1   = 0.48860251;  // L1, Y1x/y/z
const float EVAL_C2_0 = 1.09254843;  // L2, Y2xy/yz/zx
const float EVAL_C2_1 = 0.31539157;  // L2, Y2zz (3zz - 1)
const float EVAL_C2_2 = 0.54627421;  // L2, Y2xx-yy

// 纯 radiance 评基 Σ c_i Y_i(d) → L̄(d)（语义：射线 radiance / ρ·L_prev
// 回灌等直用场景；余弦卷积因子严禁在此出现——那是 irradiance 评基）。
// 表达式顺序逐字复制自原 DDGITraceFinalize.comp::shDot9（位等价）。
vec3 evalProbeRadianceSH9(vec3 sh[9], vec3 d) {
    return sh[0] * EVAL_C0
         + sh[1] * (-EVAL_C1 * d.y)
         + sh[2] * ( EVAL_C1 * d.z)
         + sh[3] * (-EVAL_C1 * d.x)
         + sh[4] * ( EVAL_C2_0 * d.y * d.x)
         + sh[5] * (-EVAL_C2_0 * d.y * d.z)
         + sh[6] * ( EVAL_C2_1 * (3.0 * d.z * d.z - 1.0))
         + sh[7] * (-EVAL_C2_0 * d.x * d.z)
         + sh[8] * ( EVAL_C2_2 * (d.x * d.x - d.y * d.y));
}

// 余弦卷积评基 → E(n)/π（调用端 ×π 得 E；与 PROBE_SH_RAD_TO_IRRAD 惯例
// 对齐）。L1 ×2/3（A1 = 2π/3 ÷π）、L2 ×1/4（A2 = π/4 ÷π）只在此处出现。
// 表达式顺序逐字复制自原 DDGIGIGather.comp 主/R-lobe 内联展开（位等价）。
vec3 evalProbeIrradianceSH9(vec3 sh[9], vec3 n) {
    const float x = n.x, y = n.y, z = n.z;
    const float xy = x * y, yz = y * z, zx = z * x;
    const float xx = x * x, yy = y * y, zz = z * z;
    return sh[0] * EVAL_C0
         + sh[1] * (-EVAL_C1 * 0.6666667 * y)   // B4: A1 = 2π/3 (÷π ⇒ ×2/3)
         + sh[2] * ( EVAL_C1 * 0.6666667 * z)
         + sh[3] * (-EVAL_C1 * 0.6666667 * x)
         + sh[4] * ( EVAL_C2_0 * 0.25 * xy)     // B4: A2 = π/4 (÷π ⇒ ×1/4)
         + sh[5] * (-EVAL_C2_0 * 0.25 * yz)
         + sh[6] * ( EVAL_C2_1 * 0.25 * (3.0*zz - 1.0))
         + sh[7] * (-EVAL_C2_0 * 0.25 * zx)
         + sh[8] * ( EVAL_C2_2 * 0.25 * (xx - yy));
}

#endif // LUMEN_EVAL_LIB_GLSL
