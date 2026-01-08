#pragma once

#include "Engine/Utilities/Math.h"
#include "MetalCommon.h"

namespace primal::graphics::rhi::metal {

    using namespace primal::math;

    /**
     * @brief Metal平台特定的透视投影矩阵 (右手坐标系, Y向下, Z: 0-1)
     * @details
     * Metal NDC: X[-1,1], Y[-1,1], Z[0,1]
     * 这里的实现假设 View Space 是 Y-down? 或者为了适配 Metal NDC (Y-up) 而做了翻转?
     * 根据文档：Y轴向下为正方向。
     */
    inline m4x4 CreatePerspectiveMatrix(float fovy, float aspect, float near, float far) {
        float yScale = 1.0f / tanf(fovy * 0.5f);
        float xScale = yScale / aspect;
        float zRange = far - near;
        float zScale = -far / zRange; // Right-handed: map -far to 1, -near to 0
        float zOffset = -near * far / zRange;

        // simd::float4x4 是列主序存储
        // Col 0: [xScale, 0, 0, 0]
        // Col 1: [0, yScale, 0, 0]
        // Col 2: [0, 0, zScale, -1]  <-- w = -z (Right-handed perspective divide)
        // Col 3: [0, 0, zOffset, 0]
        
        // 注意：文档中的代码示例可能是行主序或者伪代码。
        // 如果是标准的透视投影：
        // x' = x * xScale / z
        // y' = y * yScale / z
        // z' = z * zScale + zOffset
        // w' = z
        
        // 矩阵形式 (列向量乘法 M * v):
        // [ xScale 0      0       0       ]
        // [ 0      yScale 0       0       ]
        // [ 0      0      zScale  zOffset ]
        // [ 0      0      1       0       ]
        
        // Metal simd_matrix 是列优先构造：
        // col0, col1, col2, col3
        
        simd::float4 col0 = { xScale, 0.0f, 0.0f, 0.0f };
        simd::float4 col1 = { 0.0f, yScale, 0.0f, 0.0f };
        simd::float4 col2 = { 0.0f, 0.0f, zScale, -1.0f }; 
        simd::float4 col3 = { 0.0f, 0.0f, zOffset, 0.0f };

        return simd_matrix(col0, col1, col2, col3);
    }

    /**
     * @brief Metal平台特定的正交投影矩阵
     */
    inline m4x4 CreateOrthographicMatrix(float left, float right, float top, float bottom, float near, float far) {
        float ral = right + left;
        float rsl = right - left;
        float tab = top + bottom;
        float tsb = top - bottom; // 注意这里 top 和 bottom 的方向取决于Y轴定义
        float fan = far + near;
        float fsn = far - near;

        // Metal NDC Z [0, 1]
        // x' = 2x / (r-l) - (r+l)/(r-l)
        // y' = 2y / (t-b) - (t+b)/(t-b)
        // z' = z / (f-n) - n/(f-n)   <-- map [n, f] to [0, 1]
        
        // 如果 Y 轴向下（Top < Bottom? 还是 Top > Bottom?）
        // 通常屏幕坐标 Top=0, Bottom=Height (Y Down)
        // 映射到 NDC [-1, 1] (Y Up) 需要翻转
        
        // 假设输入遵循 Y Down (Top=0, Bottom=H)
        // 我们希望 Top(0) -> NDC(1), Bottom(H) -> NDC(-1)
        
        float xScale = 2.0f / rsl;
        float yScale = 2.0f / (top - bottom); // Flip Y? 
        // 如果 top=0, bottom=1080. 
        // y=0 -> 1. y=1080 -> -1.
        // y' = a*y + b
        // 0 = a*0 + b => b = 1
        // -1 = a*1080 + 1 => a = -2/1080
        // scale = -2 / (bottom - top) = 2 / (top - bottom)
        
        float zScale = 1.0f / fsn;
        float xOffset = -ral / rsl;
        float yOffset = -tab / (top - bottom); // (0+1080)/(0-1080) * 2 ? No.
        // b = -(top+bottom)/(top-bottom) ?
        // if top=0, bottom=H. -(H)/(-H) = 1. Correct.
        
        float zOffset = -near / fsn;

        simd::float4 col0 = { xScale, 0.0f, 0.0f, 0.0f };
        simd::float4 col1 = { 0.0f, yScale, 0.0f, 0.0f };
        simd::float4 col2 = { 0.0f, 0.0f, zScale, 0.0f };
        simd::float4 col3 = { xOffset, yOffset, zOffset, 1.0f };

        return simd_matrix(col0, col1, col2, col3);
    }

    /**
     * @brief LookAt矩阵 (右手系)
     */
    inline m4x4 CreateLookAtMatrix(const v3& eye, const v3& center, const v3& up) {
        v3 zaxis = simd::normalize(center - eye); // Forward (Right Hand: Camera looks -Z, so Forward is -Z?)
        // RH LookAt: usually Camera looks at -Z. 
        // So Forward vector (eye to center) is -Z direction.
        // zaxis (base) = normalize(eye - center)
        
        // 但是文档说：Z轴向前（屏幕外部）为正方向。相机看向Z轴负方向。
        // 所以 eye - center 是正 Z 方向。
        v3 f = simd::normalize(center - eye); // forward vector
        v3 s = simd::normalize(simd::cross(f, up)); // right vector
        v3 u = simd::cross(s, f); // up vector

        // View Matrix 构建:
        // [ s.x  s.y  s.z  -dot(s, eye) ]
        // [ u.x  u.y  u.z  -dot(u, eye) ]
        // [ -f.x -f.y -f.z  dot(f, eye) ]  <-- 注意这里，如果是 RH，Z basis 是 -f
        // [ 0    0    0    1            ]
        
        // Z basis (backwards) = -f = normalize(eye - center)
        v3 z = -f;
        
        // Recalculate right (s) = cross(up, z)? No, cross(f, up) is right?
        // Right Hand: X = Y x Z? No, X x Y = Z.
        // If Z is coming out of screen, Y is up, then X is right.
        // cross(Y, Z) = X.
        // cross(up, z_back) = right.
        
        s = simd::normalize(simd::cross(up, z));
        u = simd::cross(z, s);

        simd::float4 col0 = { s.x, u.x, z.x, 0.0f };
        simd::float4 col1 = { s.y, u.y, z.y, 0.0f };
        simd::float4 col2 = { s.z, u.z, z.z, 0.0f };
        simd::float4 col3 = { -simd::dot(s, eye), -simd::dot(u, eye), -simd::dot(z, eye), 1.0f };

        return simd_matrix(col0, col1, col2, col3);
    }

}
