#include <metal_stdlib>
using namespace metal;

// =================================================================================================
// Spherical Harmonics Data Structures
// =================================================================================================

struct SH9
{
    float c[9];
};

struct SH9Color
{
    float3 c[9];
};

// =================================================================================================
// SH Evaluation Functions
// =================================================================================================

// Evaluate Irradiance from SH9 Transfer and SH9 Environment (Single Channel)
// L = dot(Transfer, Env)
inline float DotSH9(thread const SH9& transfer, thread const SH9& env)
{
    float result = 0.0f;
    for (int i = 0; i < 9; ++i)
    {
        result += transfer.c[i] * env.c[i];
    }
    return result;
}

// Evaluate Irradiance from SH9 Transfer and SH9Color Environment (RGB)
// L = dot(Transfer, Env) per channel
inline float3 DotSH9Color(thread const SH9& transfer, thread const SH9Color& env)
{
    float3 result = float3(0.0f);
    for (int i = 0; i < 9; ++i)
    {
        result += transfer.c[i] * env.c[i];
    }
    return result;
}

inline float3 DotSH9Color(thread const SH9& transfer, constant const SH9Color& env)
{
    float3 result = float3(0.0f);
    for (int i = 0; i < 9; ++i)
    {
        result += transfer.c[i] * env.c[i];
    }
    return result;
}

// Evaluate SH Basis functions for direction dir
// Returns SH9 coefficients for the delta function in direction dir (Zonal Harmonics / Basis)
// Note: This is useful for projecting lights, but here we usually pre-project.
// Constants match SphericalHarmonics.h
inline SH9 EvalSHBasis9(float3 dir)
{
    SH9 sh;
    
    // L0
    const float C0 = 0.28209479f;
    sh.c[0] = C0;

    // L1
    const float C1 = 0.48860251f;
    sh.c[1] = -C1 * dir.y;
    sh.c[2] =  C1 * dir.z;
    sh.c[3] = -C1 * dir.x;

    // L2
    
    // Using standard formulations:
    // Y2,-2 = 1/2 * sqrt(15/pi) * xy          = 1.092548 * xy
    // Y2,-1 = 1/2 * sqrt(15/pi) * yz          = 1.092548 * yz
    // Y2,0  = 1/4 * sqrt(5/pi) * (3z^2 - 1)   = 0.315392 * (3z^2 - 1)
    // Y2,1  = 1/2 * sqrt(15/pi) * xz          = 1.092548 * xz
    // Y2,2  = 1/4 * sqrt(15/pi) * (x^2 - y^2) = 0.546274 * (x^2 - y^2)
    
    const float C2_XY = 1.09254843f;

    const float C2_YZ = 1.09254843f;
    const float C2_Z2 = 0.31539157f;
    const float C2_XZ = 1.09254843f;
    const float C2_X2Y2 = 0.54627422f;

    sh.c[4] = C2_XY * dir.x * dir.y;
    sh.c[5] = -C2_YZ * dir.y * dir.z;
    sh.c[6] = C2_Z2 * (3.0f * dir.z * dir.z - 1.0f);
    sh.c[7] = -C2_XZ * dir.x * dir.z;
    sh.c[8] = C2_X2Y2 * (dir.x * dir.x - dir.y * dir.y);

    return sh;
}
