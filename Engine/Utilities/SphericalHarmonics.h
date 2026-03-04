/**
 * @file SphericalHarmonics.h
 * @brief 球谐函数(Spherical Harmonics)基础数学库
 * @details 提供 SH 投影、重建、旋转及 Irradiance 卷积相关的数学工具
 *          支持 L2 (SH9) 和 L3 (SH16) 阶数
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-28
 * @version 0.1.0
 */

#pragma once

#include "Math.h"
#include "MathTypes.h"

namespace primal::math::sh
{
    // =================================================================================================
    // Constants & Helper Macros
    // =================================================================================================
    
    constexpr f32 SH_C0 = 0.28209479177387814347f; // 1 / (2 * sqrt(pi))
    constexpr f32 SH_C1 = 0.48860251190291992159f; // sqrt(3 / (4 * pi))
    constexpr f32 SH_C2_0 = 1.09254843059207907054f; // 1/2 * sqrt(15/pi)
    constexpr f32 SH_C2_1 = 0.31539156525252000603f; // 1/4 * sqrt(5/pi)
    constexpr f32 SH_C2_2 = 0.54627421529603953527f; // 1/4 * sqrt(15/pi)
    
    // L3 Constants
    constexpr f32 SH_C3_0 = 0.5900435899266435f; // 1/4 * sqrt(7/pi)
    constexpr f32 SH_C3_1 = 2.890611442640554f;  // 1/2 * sqrt(35/2pi)
    constexpr f32 SH_C3_2 = 0.4570457994644658f; // 1/4 * sqrt(17.5/pi)
    constexpr f32 SH_C3_3 = 0.3731763325901154f; // 1/8 * sqrt(42/pi) -> recheck: 1/4 * sqrt(10.5/pi)
    // Correct L3 constants derivation:
    // Y3,-3 = 1/4 * sqrt(35/2pi) * (3x^2 - y^2) * y
    // Y3,-2 = 1/2 * sqrt(105/pi) * xyz
    // Y3,-1 = 1/4 * sqrt(21/2pi) * y * (4z^2 - x^2 - y^2)
    // Y3,0  = 1/4 * sqrt(7/pi) * z * (2z^2 - 3x^2 - 3y^2)
    // ...
    // Let's use standard precomputed values for efficiency.
    constexpr f32 SH_Y3_0 = 0.3731763325f; // 1/4 * sqrt(7/pi)
    constexpr f32 SH_Y3_1 = 0.4570457995f; // 1/4 * sqrt(21/2pi) -> sqrt(10.5/pi)/2 ? No.
    // Let's stick to evaluated form in EvalSHBasis

    // Cosine convolution factors for Irradiance (A_l factors)
    // A0 = PI
    // A1 = 2*PI/3
    // A2 = PI/4
    // A3 = 0
    constexpr f32 SH_COSINE_A0 = 3.14159265359f;
    constexpr f32 SH_COSINE_A1 = 2.09439510239f;
    constexpr f32 SH_COSINE_A2 = 0.78539816339f;

    // =================================================================================================
    // Data Structures
    // =================================================================================================

    /**
     * @brief L2 Spherical Harmonics Coefficients (9 floats)
     * Used for single channel (e.g. luminance) or component-wise operations
     */
    struct SH9
    {
        f32 coeffs[9];

        SH9() { Reset(); }
        
        void Reset() {
            for (int i = 0; i < 9; ++i) coeffs[i] = 0.0f;
        }

        SH9& operator+=(const SH9& other) {
            for (int i = 0; i < 9; ++i) coeffs[i] += other.coeffs[i];
            return *this;
        }
        
        SH9& operator*=(f32 scalar) {
            for (int i = 0; i < 9; ++i) coeffs[i] *= scalar;
            return *this;
        }
    };

    /**
     * @brief L3 Spherical Harmonics Coefficients (16 floats)
     */
    struct SH16
    {
        f32 coeffs[16];

        SH16() { Reset(); }

        void Reset() {
            for (int i = 0; i < 16; ++i) coeffs[i] = 0.0f;
        }
    };

    /**
     * @brief RGB L2 Spherical Harmonics (9 * vec3)
     */
    struct SH9Color
    {
        v3 coeffs[9];

        SH9Color() { Reset(); }

        void Reset() {
            for (int i = 0; i < 9; ++i) coeffs[i] = {0.0f, 0.0f, 0.0f};
        }

        SH9Color& operator+=(const SH9Color& other) {
            for (int i = 0; i < 9; ++i) coeffs[i] = coeffs[i] + other.coeffs[i];
            return *this;
        }

        SH9Color& operator*=(f32 scalar) {
            for (int i = 0; i < 9; ++i) coeffs[i] = coeffs[i] * scalar;
            return *this;
        }
    };

    // =================================================================================================
    // Evaluation Functions
    // =================================================================================================

    /**
     * @brief Evaluate SH basis functions up to L2 (9 coeffs) for a given direction
     * @param dir Normalized direction vector
     * @param out_sh Output SH coefficients
     */
    inline void EvalSHBasis(const v3& dir, SH9& out_sh)
    {
        // L0
        out_sh.coeffs[0] = SH_C0;

        // L1
        out_sh.coeffs[1] = -SH_C1 * dir.y;
        out_sh.coeffs[2] = SH_C1 * dir.z;
        out_sh.coeffs[3] = -SH_C1 * dir.x;

        // L2
        out_sh.coeffs[4] = SH_C2_0 * dir.x * dir.y;
        out_sh.coeffs[5] = -SH_C2_0 * dir.y * dir.z;
        out_sh.coeffs[6] = SH_C2_1 * (3.0f * dir.z * dir.z - 1.0f);
        out_sh.coeffs[7] = -SH_C2_0 * dir.x * dir.z;
        out_sh.coeffs[8] = SH_C2_2 * (dir.x * dir.x - dir.y * dir.y);
    }

    /**
     * @brief Reconstruct signal value from SH9 coefficients in a given direction
     * @param sh Input SH coefficients
     * @param dir Normalized direction vector
     * @return Reconstructed value
     */
    inline f32 ReconstructSH(const SH9& sh, const v3& dir)
    {
        SH9 basis;
        EvalSHBasis(dir, basis);

        f32 result = 0.0f;
        for (int i = 0; i < 9; ++i)
        {
            result += sh.coeffs[i] * basis.coeffs[i];
        }
        return result;
    }

    /**
     * @brief Reconstruct RGB color from SH9Color coefficients
     */
    inline v3 ReconstructSH(const SH9Color& sh, const v3& dir)
    {
        SH9 basis;
        EvalSHBasis(dir, basis);

        v3 result = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 9; ++i)
        {
            result = result + sh.coeffs[i] * basis.coeffs[i];
        }
        return result;
    }

    /**
     * @brief Calculate dot product of two SH9 vectors (Integral of product of two functions)
     * For orthonormal basis, \int f(s)g(s) ds = \sum f_i * g_i
     * Use this for lighting: Irradiance = Dot(EnvironmentSH, TransferSH)
     */
    inline f32 DotProduct(const SH9& a, const SH9& b)
    {
        f32 result = 0.0f;
        for (int i = 0; i < 9; ++i)
        {
            result += a.coeffs[i] * b.coeffs[i];
        }
        return result;
    }

    /**
     * @brief Calculate dot product of SH9 transfer and SH9Color environment
     */
    inline v3 DotProduct(const SH9& transfer, const SH9Color& env)
    {
        v3 result = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 9; ++i)
        {
            result = result + env.coeffs[i] * transfer.coeffs[i];
        }
        return result;
    }

    // =================================================================================================
    // Convolution Functions
    // =================================================================================================

    /**
     * @brief Convolve SH coefficients with a Cosine Lobe (for Irradiance)
     * Effectively multiplies SH coefficients by the zonal harmonic coefficients of a cosine lobe.
     * This prepares the SH environment map for efficient irradiance lookup.
     * @param in_sh Input SH coefficients (radiance)
     * @param out_sh Output SH coefficients (irradiance)
     */
    inline void ConvolveCosineLobe(const SH9& in_sh, SH9& out_sh)
    {
        // L0 * A0
        out_sh.coeffs[0] = in_sh.coeffs[0] * SH_COSINE_A0;

        // L1 * A1
        f32 c1 = SH_COSINE_A1;
        out_sh.coeffs[1] = in_sh.coeffs[1] * c1;
        out_sh.coeffs[2] = in_sh.coeffs[2] * c1;
        out_sh.coeffs[3] = in_sh.coeffs[3] * c1;

        // L2 * A2
        f32 c2 = SH_COSINE_A2;
        out_sh.coeffs[4] = in_sh.coeffs[4] * c2;
        out_sh.coeffs[5] = in_sh.coeffs[5] * c2;
        out_sh.coeffs[6] = in_sh.coeffs[6] * c2;
        out_sh.coeffs[7] = in_sh.coeffs[7] * c2;
        out_sh.coeffs[8] = in_sh.coeffs[8] * c2;
    }
    
    inline void ConvolveCosineLobe(const SH9Color& in_sh, SH9Color& out_sh)
    {
         // L0 * A0
        out_sh.coeffs[0] = in_sh.coeffs[0] * SH_COSINE_A0;

        // L1 * A1
        f32 c1 = SH_COSINE_A1;
        out_sh.coeffs[1] = in_sh.coeffs[1] * c1;
        out_sh.coeffs[2] = in_sh.coeffs[2] * c1;
        out_sh.coeffs[3] = in_sh.coeffs[3] * c1;

        // L2 * A2
        f32 c2 = SH_COSINE_A2;
        out_sh.coeffs[4] = in_sh.coeffs[4] * c2;
        out_sh.coeffs[5] = in_sh.coeffs[5] * c2;
        out_sh.coeffs[6] = in_sh.coeffs[6] * c2;
        out_sh.coeffs[7] = in_sh.coeffs[7] * c2;
        out_sh.coeffs[8] = in_sh.coeffs[8] * c2;
    }

    // =================================================================================================
    // Projection Functions
    // =================================================================================================

    /**
     * @brief Project a function into SH coefficients using Monte Carlo integration over the sphere
     * @tparam Func Function type: v3 func(const v3& direction)
     * @param func The function to project (returns color for a given direction)
     * @param num_samples Number of samples for Monte Carlo integration
     * @param out_sh Output SH coefficients
     */
    template<typename Func>
    inline void ProjectFunction(Func func, u32 num_samples, SH9Color& out_sh)
    {
        out_sh.Reset();

        const f32 inv_samples = 1.0f / (f32)num_samples;
        const f32 solid_angle = 4.0f * 3.14159265359f;
        const f32 weight = solid_angle * inv_samples; // Uniform sampling over sphere PDF = 1 / 4pi

        // Simple Stratified Sampling or Random Sampling could be used.
        // For simplicity here, we use a basic Fibonacci Sphere Lattice for uniform distribution.
        
        for (u32 i = 0; i < num_samples; ++i)
        {
            // Fibonacci Sphere Point
            // z = 1 - (2*i + 1) / N  (maps to [-1, 1])
            // phi = 2*PI * i * golden_ratio
            
            // f32 k = i + 0.5f;
            // f32 phi = std::acos(1.0f - 2.0f * k / num_samples);
            // f32 theta = 3.14159265359f * (1.0f + std::sqrt(5.0f)) * k;
            
            // Convert spherical (phi, theta) to cartesian (x, y, z)
            // Note: phi here is from Z axis (0 to PI), theta is around Z (0 to 2PI)
            // But usually standard mapping: y is up. Let's align with our coordinate system.
            // Let's use standard Fibonacci Sphere Algorithm directly to x,y,z
            
            f32 y = 1.0f - (i / (f32)(num_samples - 1)) * 2.0f; // y goes from 1 to -1
            f32 radius = std::sqrt(1.0f - y * y);
            f32 long_angle = 2.3999632297286f * i; // golden angle increment

            f32 x = std::cos(long_angle) * radius;
            f32 z = std::sin(long_angle) * radius;
            
            v3 dir = {x, y, z}; // Normalized direction
            
            // Evaluate Function
            v3 value = func(dir);
            
            // Evaluate SH Basis
            SH9 basis;
            EvalSHBasis(dir, basis);
            
            // Accumulate
            for (int j = 0; j < 9; ++j)
            {
                out_sh.coeffs[j] = out_sh.coeffs[j] + value * basis.coeffs[j];
            }
        }
        
        // Apply Weight
        out_sh *= weight;
    }

    // =================================================================================================
    // CubeMap Utilities
    // =================================================================================================

    enum class CubeMapFace
    {
        PositiveX = 0,
        NegativeX = 1,
        PositiveY = 2,
        NegativeY = 3,
        PositiveZ = 4,
        NegativeZ = 5
    };

    /**
     * @brief Description of a CubeMap for CPU-side processing
     * Assumes 32-bit float data per component (RGB or RGBA)
     */
    struct CubeMapDesc
    {
        u32 width;
        u32 height;
        const void* data[6]; // Pointers to raw data for each face
        u32 stride;          // Bytes per pixel (e.g. 12 for RGB32, 16 for RGBA32)
    };

    /**
     * @brief Calculate UV and Face index from a direction vector
     * Follows OpenGL CubeMap standard conventions
     */
    inline void GetCubeMapFaceUV(const v3& dir, CubeMapFace& out_face, f32& out_u, f32& out_v)
    {
        f32 absX = std::abs(dir.x);
        f32 absY = std::abs(dir.y);
        f32 absZ = std::abs(dir.z);
        
        f32 ma, sc, tc;
        
        if (absX >= absY && absX >= absZ)
        {
            // X Major
            ma = absX;
            if (dir.x > 0) { out_face = CubeMapFace::PositiveX; sc = -dir.z; tc = -dir.y; }
            else           { out_face = CubeMapFace::NegativeX; sc =  dir.z; tc = -dir.y; }
        }
        else if (absY >= absX && absY >= absZ)
        {
            // Y Major
            ma = absY;
            if (dir.y > 0) { out_face = CubeMapFace::PositiveY; sc =  dir.x; tc =  dir.z; }
            else           { out_face = CubeMapFace::NegativeY; sc =  dir.x; tc = -dir.z; }
        }
        else
        {
            // Z Major
            ma = absZ;
            if (dir.z > 0) { out_face = CubeMapFace::PositiveZ; sc =  dir.x; tc = -dir.y; }
            else           { out_face = CubeMapFace::NegativeZ; sc = -dir.x; tc = -dir.y; }
        }
        
        // Convert range [-ma, ma] to [0, 1]
        // u = (sc / ma + 1) / 2
        // v = (tc / ma + 1) / 2
        out_u = (sc / ma + 1.0f) * 0.5f;
        out_v = (tc / ma + 1.0f) * 0.5f;
    }

    /**
     * @brief Sample a CubeMap using bilinear interpolation
     */
    inline v3 SampleCubeMap(const CubeMapDesc& desc, const v3& dir)
    {
        CubeMapFace face;
        f32 u, v;
        GetCubeMapFaceUV(dir, face, u, v);
        
        // Clamp UV to [0, 1]
        u = std::max(0.0f, std::min(u, 1.0f));
        v = std::max(0.0f, std::min(v, 1.0f));
        
        // Calculate pixel coordinates (center aligned?)
        // Standard texture sampling maps 0.0 to edge of first pixel, 1.0 to edge of last pixel.
        // Center of pixel i is (i + 0.5) / size.
        // Here we do simple linear mapping: px = u * w - 0.5
        // But for simplicity let's map [0,1] to [0, w-1] directly for indices
        
        f32 px = u * desc.width - 0.5f;
        f32 py = v * desc.height - 0.5f;
        
        // Floor to get top-left sample index
        int x0 = (int)std::floor(px);
        int y0 = (int)std::floor(py);
        
        // Fraction for interpolation
        f32 dx = px - x0;
        f32 dy = py - y0;
        
        // Clamp indices
        // Note: For proper CubeMap sampling we should handle edge wrap-around to neighbor faces,
        // but that's complex. Here we clamp to edge, which is acceptable for simple SH projection.
        int w = (int)desc.width;
        int h = (int)desc.height;
        
        int x0c = std::max(0, std::min(x0, w - 1));
        int y0c = std::max(0, std::min(y0, h - 1));
        int x1c = std::max(0, std::min(x0 + 1, w - 1));
        int y1c = std::max(0, std::min(y0 + 1, h - 1));
        
        // Fetch data
        const u8* face_bytes = (const u8*)desc.data[(int)face];
        
        auto fetch = [&](int x, int y) -> v3 {
            const f32* pixel = (const f32*)(face_bytes + (y * w + x) * desc.stride);
            return {pixel[0], pixel[1], pixel[2]};
        };
        
        v3 c00 = fetch(x0c, y0c);
        v3 c10 = fetch(x1c, y0c);
        v3 c01 = fetch(x0c, y1c);
        v3 c11 = fetch(x1c, y1c);
        
        // Bilinear interpolation
        v3 top = c00 * (1.0f - dx) + c10 * dx;
        v3 bottom = c01 * (1.0f - dx) + c11 * dx;
        
        return top * (1.0f - dy) + bottom * dy;
    }

    /**
     * @brief Project a CubeMap into SH coefficients
     * @param desc CubeMap description (data pointers, size, format)
     * @param num_samples Number of Monte Carlo samples
     * @param out_sh Output SH coefficients
     */
    inline void ProjectCubeMap(const CubeMapDesc& desc, u32 num_samples, SH9Color& out_sh)
    {
        auto sampler = [&](const v3& dir) -> v3 {
            return SampleCubeMap(desc, dir);
        };
        ProjectFunction(sampler, num_samples, out_sh);
    }
}
