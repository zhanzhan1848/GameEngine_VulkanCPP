// Screen-probe SH and DDGI radiance/irradiance conventions.
static const float SH9_C0   = 0.28209479;  // L0, Y00
static const float SH9_C1   = 0.48860251;  // L1, Y1x/y/z
static const float SH9_C2_0 = 1.09254843;  // L2, Y2xy/yz/zx
static const float SH9_C2_1 = 0.31539157;  // L2, Y2zz (3zz - 1)
static const float SH9_C2_2 = 0.54627421;  // L2, Y2xx-yy

// Evaluate a 9-coefficient SH irradiance field in direction d.
// Coefficient layout matches DDGIGIGather / DDGI probe buffers:
//   [0]=Y00, [1]=Y1y, [2]=Y1z, [3]=Y1x, [4]=Y2xy, [5]=Y2yz, [6]=Y2zx,
//   [7]=Y2zz, [8]=Y2xx-yy
float3 evalSH9(float3 sh[9], float3 d) {
    float x = d.x, y = d.y, z = d.z;
    float xy = x * y, yz = y * z, zx = z * x;
    float xx = x * x, yy = y * y, zz = z * z;
    return sh[0] * SH9_C0
         + sh[1] * (SH9_C1 * y)
         + sh[2] * (SH9_C1 * z)
         + sh[3] * (SH9_C1 * x)
         + sh[4] * (SH9_C2_0 * xy)
         + sh[5] * (SH9_C2_0 * yz)
         + sh[6] * (SH9_C2_0 * zx)
         + sh[7] * (SH9_C2_1 * (3.0 * zz - 1.0))
         + sh[8] * (SH9_C2_2 * (xx - yy));
}


static const float EVAL_C0   = 0.28209479;  // L0, Y00
static const float EVAL_C1   = 0.48860251;  // L1, Y1x/y/z
static const float EVAL_C2_0 = 1.09254843;  // L2, Y2xy/yz/zx
static const float EVAL_C2_1 = 0.31539157;  // L2, Y2zz (3zz - 1)
static const float EVAL_C2_2 = 0.54627421;  // L2, Y2xx-yy

// 纯 radiance 评基 Σ c_i Y_i(d) → L̄(d)（语义：射线 radiance / ρ·L_prev
// 回灌等直用场景；余弦卷积因子严禁在此出现——那是 irradiance 评基）。
// 表达式顺序逐字复制自原 DDGITraceFinalize.comp::shDot9（位等价）。
float3 evalProbeRadianceSH9(float3 sh[9], float3 d) {
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
float3 evalProbeIrradianceSH9(float3 sh[9], float3 n) {
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
