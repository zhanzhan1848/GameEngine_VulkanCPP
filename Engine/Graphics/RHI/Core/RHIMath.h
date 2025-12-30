/**
 * @file RHIMath.h
 * @brief RHI数学库集成头文件
 * @details 提供RHI层所需的数学类型和函数，集成项目现有的数学库
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#pragma once

#include "RHITypes.h"
#include "../../../Utilities/Vector.h"
#include "../../../Utilities/MathTypes.h"
#include "../../../Utilities/Math.h"

namespace primal::graphics::rhi::math {

// === 类型别名，简化数学类型使用 ===

using namespace primal::math;  // 使用项目现有的数学库

// === RHI特定的数学常量 ===

/**
 * @brief RHI数学常量
 * @details 提供RHI渲染中常用的数学常量
 */
namespace constants {
    constexpr float PI = 3.14159265358979323846f;
    constexpr float TWO_PI = 2.0f * PI;
    constexpr float HALF_PI = PI * 0.5f;
    constexpr float QUARTER_PI = PI * 0.25f;
    constexpr float INV_PI = 1.0f / PI;
    constexpr float INV_TWO_PI = 1.0f / TWO_PI;
    constexpr float DEG_TO_RAD = PI / 180.0f;
    constexpr float RAD_TO_DEG = 180.0f / PI;
    constexpr float EPSILON = 1e-6f;
    constexpr float FLOAT_MAX = 3.402823466e+38f;
    constexpr float FLOAT_MIN = -3.402823466e+38f;
}

// === 向量扩展函数 ===

/**
 * @brief 计算向量的长度
 * @param v 输入向量
 * @return 向量长度
 */
template<typename T>
float Length(const T& v) {
    return sqrtf(dot(v, v));
}

/**
 * @brief 计算向量的平方长度
 * @param v 输入向量
 * @return 向量平方长度
 */
template<typename T>
float LengthSquared(const T& v) {
    return dot(v, v);
}

/**
 * @brief 计算向量归一化
 * @param v 输入向量
 * @return 归一化向量
 */
template<typename T>
T Normalize(const T& v) {
    float len = Length(v);
    return len > constants::EPSILON ? v / len : T{};
}

/**
 * @brief 计算向量点积
 * @param a 第一个向量
 * @param b 第二个向量
 * @return 点积结果
 */
inline float Dot(const v2& a, const v2& b) {
    return a.x * b.x + a.y * b.y;
}

/**
 * @brief 计算向量点积
 * @param a 第一个向量
 * @param b 第二个向量
 * @return 点积结果
 */
inline float Dot(const v3& a, const v3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/**
 * @brief 计算向量点积
 * @param a 第一个向量
 * @param b 第二个向量
 * @return 点积结果
 */
inline float Dot(const v4& a, const v4& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

/**
 * @brief 计算二维向量叉积（标量）
 * @param a 第一个向量
 * @param b 第二个向量
 * @return 叉积结果（标量）
 */
inline float Cross(const v2& a, const v2& b) {
    return a.x * b.y - a.y * b.x;
}

/**
 * @brief 计算三维向量叉积
 * @param a 第一个向量
 * @param b 第二个向量
 * @return 叉积结果向量
 */
inline v3 Cross(const v3& a, const v3& b) {
    return v3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

/**
 * @brief 计算向量间的夹角（弧度）
 * @param a 第一个向量
 * @param b 第二个向量
 * @return 夹角弧度值
 */
template<typename T>
float Angle(const T& a, const T& b) {
    float lenA = Length(a);
    float lenB = Length(b);
    if (lenA < constants::EPSILON || lenB < constants::EPSILON) {
        return 0.0f;
    }
    float cosAngle = Dot(a, b) / (lenA * lenB);
    cosAngle = Clamp(cosAngle, -1.0f, 1.0f);
    return acosf(cosAngle);
}

/**
 * @brief 线性插值
 * @param a 起始值
 * @param b 结束值
 * @param t 插值参数 [0, 1]
 * @return 插值结果
 */
template<typename T>
T Lerp(const T& a, const T& b, float t) {
    return a + (b - a) * t;
}

/**
 * @brief 平滑插值（Ease-in-out）
 * @param a 起始值
 * @param b 结束值
 * @param t 插值参数 [0, 1]
 * @return 插值结果
 */
template<typename T>
T SmoothLerp(const T& a, const T& b, float t) {
    t = Clamp(t, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);  // Smoothstep函数
    return Lerp(a, b, t);
}

// === 矩阵扩展函数 ===

/**
 * @brief 创建平移矩阵
 * @param translation 平移向量
 * @return 4x4平移矩阵
 */
inline m4x4 CreateTranslationMatrix(const v3& translation) {
    return m4x4{
        v4{1.0f, 0.0f, 0.0f, 0.0f},
        v4{0.0f, 1.0f, 0.0f, 0.0f},
        v4{0.0f, 0.0f, 1.0f, 0.0f},
        v4{translation.x, translation.y, translation.z, 1.0f}
    };
}

/**
 * @brief 创建缩放矩阵
 * @param scale 缩放向量
 * @return 4x4缩放矩阵
 */
inline m4x4 CreateScaleMatrix(const v3& scale) {
    return m4x4{
        v4{scale.x, 0.0f, 0.0f, 0.0f},
        v4{0.0f, scale.y, 0.0f, 0.0f},
        v4{0.0f, 0.0f, scale.z, 0.0f},
        v4{0.0f, 0.0f, 0.0f, 1.0f}
    };
}

/**
 * @brief 创建绕X轴旋转矩阵
 * @param angle 旋转角度（弧度）
 * @return 4x4旋转矩阵
 */
inline m4x4 CreateRotationMatrixX(float angle) {
    float cosAngle = cosf(angle);
    float sinAngle = sinf(angle);
    
    return m4x4{
        v4{1.0f, 0.0f, 0.0f, 0.0f},
        v4{0.0f, cosAngle, sinAngle, 0.0f},
        v4{0.0f, -sinAngle, cosAngle, 0.0f},
        v4{0.0f, 0.0f, 0.0f, 1.0f}
    };
}

/**
 * @brief 创建绕Y轴旋转矩阵
 * @param angle 旋转角度（弧度）
 * @return 4x4旋转矩阵
 */
inline m4x4 CreateRotationMatrixY(float angle) {
    float cosAngle = cosf(angle);
    float sinAngle = sinf(angle);
    
    return m4x4{
        v4{cosAngle, 0.0f, -sinAngle, 0.0f},
        v4{0.0f, 1.0f, 0.0f, 0.0f},
        v4{sinAngle, 0.0f, cosAngle, 0.0f},
        v4{0.0f, 0.0f, 0.0f, 1.0f}
    };
}

/**
 * @brief 创建绕Z轴旋转矩阵
 * @param angle 旋转角度（弧度）
 * @return 4x4旋转矩阵
 */
inline m4x4 CreateRotationMatrixZ(float angle) {
    float cosAngle = cosf(angle);
    float sinAngle = sinf(angle);
    
    return m4x4{
        v4{cosAngle, sinAngle, 0.0f, 0.0f},
        v4{-sinAngle, cosAngle, 0.0f, 0.0f},
        v4{0.0f, 0.0f, 1.0f, 0.0f},
        v4{0.0f, 0.0f, 0.0f, 1.0f}
    };
}

/**
 * @brief 创建任意轴旋转矩阵（使用Rodrigues旋转公式）
 * @param axis 旋转轴（必须归一化）
 * @param angle 旋转角度（弧度）
 * @return 4x4旋转矩阵
 */
inline m4x4 CreateRotationMatrixAxis(const v3& axis, float angle) {
    float cosAngle = cosf(angle);
    float sinAngle = sinf(angle);
    float oneMinusCos = 1.0f - cosAngle;
    
    float x = axis.x;
    float y = axis.y;
    float z = axis.z;
    
    return m4x4{
        v4{cosAngle + x * x * oneMinusCos, x * y * oneMinusCos + z * sinAngle, x * z * oneMinusCos - y * sinAngle, 0.0f},
        v4{y * x * oneMinusCos - z * sinAngle, cosAngle + y * y * oneMinusCos, y * z * oneMinusCos + x * sinAngle, 0.0f},
        v4{z * x * oneMinusCos + y * sinAngle, z * y * oneMinusCos - x * sinAngle, cosAngle + z * z * oneMinusCos, 0.0f},
        v4{0.0f, 0.0f, 0.0f, 1.0f}
    };
}

/**
 * @brief 创建欧拉角旋转矩阵（ZYX顺序）
 * @param euler 欧拉角向量（弧度）
 * @return 4x4旋转矩阵
 */
inline m4x4 CreateRotationMatrixEuler(const v3& euler) {
    m4x4 rotX = CreateRotationMatrixX(euler.x);
    m4x4 rotY = CreateRotationMatrixY(euler.y);
    m4x4 rotZ = CreateRotationMatrixZ(euler.z);
    
    // ZYX顺序：先绕X轴，再绕Y轴，最后绕Z轴
    return rotZ * rotY * rotX;
}

/**
 * @brief 创建透视投影矩阵
 * @param fovY 垂直视野角度（弧度）
 * @param aspect 宽高比
 * @param nearPlane 近裁剪面距离
 * @param farPlane 远裁剪面距离
 * @return 4x4透视投影矩阵
 */
inline m4x4 CreatePerspectiveMatrix(float fovY, float aspect, float nearPlane, float farPlane) {
    float tanHalfFov = tanf(fovY * 0.5f);
    
    return m4x4{
        v4{1.0f / (aspect * tanHalfFov), 0.0f, 0.0f, 0.0f},
        v4{0.0f, 1.0f / tanHalfFov, 0.0f, 0.0f},
        v4{0.0f, 0.0f, farPlane / (nearPlane - farPlane), -1.0f},
        v4{0.0f, 0.0f, (nearPlane * farPlane) / (nearPlane - farPlane), 0.0f}
    };
}

/**
 * @brief 创建正交投影矩阵
 * @param left 左边界
 * @param right 右边界
 * @param bottom 下边界
 * @param top 上边界
 * @param nearPlane 近裁剪面距离
 * @param farPlane 远裁剪面距离
 * @return 4x4正交投影矩阵
 */
inline m4x4 CreateOrthographicMatrix(float left, float right, float bottom, float top, float nearPlane, float farPlane) {
    return m4x4{
        v4{2.0f / (right - left), 0.0f, 0.0f, 0.0f},
        v4{0.0f, 2.0f / (top - bottom), 0.0f, 0.0f},
        v4{0.0f, 0.0f, 1.0f / (nearPlane - farPlane), 0.0f},
        v4{(left + right) / (left - right), (top + bottom) / (bottom - top), 
           nearPlane / (nearPlane - farPlane), 1.0f}
    };
}

/**
 * @brief 创建观察矩阵（LookAt）
 * @param eye 摄像机位置
 * @param target 观察目标
 * @param up 上方向向量
 * @return 4x4观察矩阵
 */
inline m4x4 CreateLookAtMatrix(const v3& eye, const v3& target, const v3& up) {
    v3 forward = Normalize(target - eye);
    v3 right = Normalize(Cross(up, forward));
    v3 newUp = Cross(forward, right);
    
    return m4x4{
        v4{right.x, newUp.x, -forward.x, 0.0f},
        v4{right.y, newUp.y, -forward.y, 0.0f},
        v4{right.z, newUp.z, -forward.z, 0.0f},
        v4{-Dot(right, eye), -Dot(newUp, eye), Dot(forward, eye), 1.0f}
    };
}

// === 矩阵操作函数 ===

/**
 * @brief 矩阵转置
 * @param mat 输入矩阵
 * @return 转置矩阵
 */
inline m4x4 Transpose(const m4x4& mat) {
    return m4x4{
        v4{mat[0][0], mat[1][0], mat[2][0], mat[3][0]},
        v4{mat[0][1], mat[1][1], mat[2][1], mat[3][1]},
        v4{mat[0][2], mat[1][2], mat[2][2], mat[3][2]},
        v4{mat[0][3], mat[1][3], mat[2][3], mat[3][3]}
    };
}

/**
 * @brief 矩阵求逆（适用于变换矩阵）
 * @param mat 输入矩阵
 * @return 逆矩阵，如果矩阵不可逆则返回单位矩阵
 */
inline m4x4 Inverse(const m4x4& mat) {
    // 提取平移、旋转、缩放分量
    v3 translation{mat[3][0], mat[3][1], mat[3][2]};
    
    // 提取3x3旋转缩放矩阵并求逆
    v3 scale{
        Length(v3{mat[0][0], mat[0][1], mat[0][2]}),
        Length(v3{mat[1][0], mat[1][1], mat[1][2]}),
        Length(v3{mat[2][0], mat[2][1], mat[2][2]})
    };
    
    // 避免除零
    scale.x = (scale.x < constants::EPSILON) ? 1.0f : 1.0f / scale.x;
    scale.y = (scale.y < constants::EPSILON) ? 1.0f : 1.0f / scale.y;
    scale.z = (scale.z < constants::EPSILON) ? 1.0f : 1.0f / scale.z;
    
    // 构建旋转矩阵的转置
    m4x4 invMat{
        v4{mat[0][0] * scale.x, mat[1][0] * scale.y, mat[2][0] * scale.z, 0.0f},
        v4{mat[0][1] * scale.x, mat[1][1] * scale.y, mat[2][1] * scale.z, 0.0f},
        v4{mat[0][2] * scale.x, mat[1][2] * scale.y, mat[2][2] * scale.z, 0.0f},
        v4{0.0f, 0.0f, 0.0f, 1.0f}
    };
    
    // 应用反向平移
    invMat[3][0] = -(mat[3][0] * invMat[0][0] + mat[3][1] * invMat[1][0] + mat[3][2] * invMat[2][0]);
    invMat[3][1] = -(mat[3][0] * invMat[0][1] + mat[3][1] * invMat[1][1] + mat[3][2] * invMat[2][1]);
    invMat[3][2] = -(mat[3][0] * invMat[0][2] + mat[3][1] * invMat[1][2] + mat[3][2] * invMat[2][2]);
    
    return invMat;
}

/**
 * @brief 矩阵向量乘法（忽略w分量）
 * @param mat 输入矩阵
 * @param vec 输入向量
 * @return 变换后的向量
 */
inline v3 TransformVector(const m4x4& mat, const v3& vec) {
    return v3{
        mat[0][0] * vec.x + mat[1][0] * vec.y + mat[2][0] * vec.z,
        mat[0][1] * vec.x + mat[1][1] * vec.y + mat[2][1] * vec.z,
        mat[0][2] * vec.x + mat[1][2] * vec.y + mat[2][2] * vec.z
    };
}

/**
 * @brief 矩阵点乘法（包含平移）
 * @param mat 输入矩阵
 * @param point 输入点
 * @return 变换后的点
 */
inline v3 TransformPoint(const m4x4& mat, const v3& point) {
    return v3{
        mat[0][0] * point.x + mat[1][0] * point.y + mat[2][0] * point.z + mat[3][0],
        mat[0][1] * point.x + mat[1][1] * point.y + mat[2][1] * point.z + mat[3][1],
        mat[0][2] * point.x + mat[1][2] * point.y + mat[2][2] * point.z + mat[3][2]
    };
}

/**
 * @brief 矩阵法向量变换（使用逆转置矩阵）
 * @param mat 输入矩阵
 * @param normal 输入法向量
 * @return 变换后的法向量（需重新归一化）
 */
inline v3 TransformNormal(const m4x4& mat, const v3& normal) {
    // 使用3x3旋转部分的转置变换法向量
    return Normalize(v3{
        mat[0][0] * normal.x + mat[0][1] * normal.y + mat[0][2] * normal.z,
        mat[1][0] * normal.x + mat[1][1] * normal.y + mat[1][2] * normal.z,
        mat[2][0] * normal.x + mat[2][1] * normal.y + mat[2][2] * normal.z
    });
}

// === 颜色空间转换函数 ===

/**
 * @brief RGB转换为线性颜色空间
 * @param rgb RGB颜色值（0-1范围）
 * @return 线性RGB颜色值
 */
inline v3 RGBToLinear(const v3& rgb) {
    // 假设输入为sRGB，转换为线性空间
    return v3{
        rgb.x <= 0.04045f ? rgb.x / 12.92f : powf((rgb.x + 0.055f) / 1.055f, 2.4f),
        rgb.y <= 0.04045f ? rgb.y / 12.92f : powf((rgb.y + 0.055f) / 1.055f, 2.4f),
        rgb.z <= 0.04045f ? rgb.z / 12.92f : powf((rgb.z + 0.055f) / 1.055f, 2.4f)
    };
}

/**
 * @brief 线性颜色空间转换为sRGB
 * @param linear 线性RGB颜色值
 * @return sRGB颜色值
 */
inline v3 LinearToRGB(const v3& linear) {
    return v3{
        linear.x <= 0.0031308f ? 12.92f * linear.x : 1.055f * powf(linear.x, 1.0f / 2.4f) - 0.055f,
        linear.y <= 0.0031308f ? 12.92f * linear.y : 1.055f * powf(linear.y, 1.0f / 2.4f) - 0.055f,
        linear.z <= 0.0031308f ? 12.92f * linear.z : 1.055f * powf(linear.z, 1.0f / 2.4f) - 0.055f
    };
}

/**
 * @brief RGB转换为HSV颜色空间
 * @param rgb RGB颜色值（0-1范围）
 * @return HSV颜色值
 */
inline v3 RGBToHSV(const v3& rgb) {
    float maxVal = max(max(rgb.x, rgb.y), rgb.z);
    float minVal = min(min(rgb.x, rgb.y), rgb.z);
    float delta = maxVal - minVal;
    
    v3 hsv;
    hsv.z = maxVal;  // Value
    
    if (delta < constants::EPSILON) {
        hsv.x = 0.0f;  // Hue
        hsv.y = 0.0f;  // Saturation
    } else {
        hsv.y = delta / maxVal;  // Saturation
        
        if (maxVal == rgb.x) {
            hsv.x = (rgb.y - rgb.z) / delta + (rgb.y < rgb.z ? 6.0f : 0.0f);
        } else if (maxVal == rgb.y) {
            hsv.x = (rgb.z - rgb.x) / delta + 2.0f;
        } else {
            hsv.x = (rgb.x - rgb.y) / delta + 4.0f;
        }
        
        hsv.x /= 6.0f;  // Normalize to [0,1]
    }
    
    return hsv;
}

/**
 * @brief HSV转换为RGB颜色空间
 * @param hsv HSV颜色值
 * @return RGB颜色值
 */
inline v3 HSVToRGB(const v3& hsv) {
    float h = hsv.x * 6.0f;  // Convert to [0,6]
    float c = hsv.y * hsv.z;  // Chroma
    float x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float m = hsv.z - c;
    
    v3 rgb;
    if (h < 1.0f) {
        rgb = v3{c, x, 0.0f};
    } else if (h < 2.0f) {
        rgb = v3{x, c, 0.0f};
    } else if (h < 3.0f) {
        rgb = v3{0.0f, c, x};
    } else if (h < 4.0f) {
        rgb = v3{0.0f, x, c};
    } else if (h < 5.0f) {
        rgb = v3{x, 0.0f, c};
    } else {
        rgb = v3{c, 0.0f, x};
    }
    
    return rgb + v3{m, m, m};
}

} // namespace primal::graphics::rhi::math