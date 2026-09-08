// ============================================================================
// MathLib.glsl — T1.10 canonical shared math library (GLSL 规范源)
//
// Master Plan §5.2 单源迁移第一片：本文件是下列函数的**唯一权威定义**，
// Vulkan .comp 直接 include；Metal 侧镜像（.metal）在对齐迁移完成前保持
// 手工同步，并由 scripts/check_shader_copies.sh 的基线门禁跟踪。
//
// Canonical:  Engine/Graphics/Vulkan/Shaders/Lumen/ShaderLib/MathLib.glsl
// Metal 镜像: Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheData.metal (octEncode/octDecode)
//             Engine/Graphics/Metal/shaders/Lumen/SDFTraceCommon.metal   (SDF trace, 待抽取)
//             Engine/Graphics/Metal/shaders/Lumen/DDGIVolumeData.metal   (SH 基, 待抽取)
//
// 修改本文件的任何函数必须同步上述 Metal 镜像，且 pull request 中附
// 前后 dump 对比（AGENTS.md 纪律）。
//
// Binding-free pure function library — no uniforms, no extensions required.
// ============================================================================

#ifndef PRIMAL_MATHLIB_GLSL
#define PRIMAL_MATHLIB_GLSL

// ----------------------------------------------------------------------------
// Octahedral normal encoding — matches the Metal mirror in
// SurfaceCacheData.metal (octEncode / octDecode). encode folds z<0 with the
// standard replace rule; decode inverts it (n.xy >= 0 subtracts t, else adds).
// ----------------------------------------------------------------------------
vec2 octEncode(vec3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    vec2 result = n.xy / l1norm;
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * mix(vec2(-1.0), vec2(1.0), step(vec2(0.0), result.xy));
    }
    return result * 0.5 + 0.5;
}

vec3 octDecode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    // Inverse of the replace-fold in octEncode: n.xy >= 0 subtracts t, else adds.
    n.xy += mix(vec2(t), vec2(-t), step(vec2(0.0), n.xy));
    return normalize(n);
}

// ----------------------------------------------------------------------------
// SH basis constants — irradiance reconstruction (Ramamoorthi):
//   L(n) ≈ c0 * L0 + c1 * (L1.y·n.y + L1.z·n.z - L1.x·n.x)
// (待抽取的 Metal 镜像：DDGIVolumeData.metal 的 DDGI_SH_C0/C1)
// ----------------------------------------------------------------------------
#define PRIMAL_SH_C0 0.282095
#define PRIMAL_SH_C1 0.488603

#endif // PRIMAL_MATHLIB_GLSL
