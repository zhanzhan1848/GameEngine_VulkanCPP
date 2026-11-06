#pragma once

// DirectXMath 后端的向量/矩阵运算符。
//
// 为什么放在 namespace DirectX：DirectXMath 的 XMFLOAT* 是无运算符的 POD，
// 引擎代码写的 `a + b`、`m1 * m2` 必须经参数相关查找（ADL）在实参类型
// 所属命名空间（DirectX）里找到这些运算符。放进 primal::math 则 ADL 找不到。
//
// 第三方命名空间带来的风险：
//  - 这些运算符对本编译单元内所有可见的 XMFLOAT2/2A/3A/4A/4X4A 实参全局生效，
//    任何第三方库若也在 namespace DirectX 定义同名运算符（DirectXMath 官方
//    刻意不提供运算符，此风险当前不存在），会与本头产生重定义/二义性；
//  - 运算符语义以本仓可移植分支为准（一一对应，不多不少），不是 DirectXMath
//    官方语义（官方无运算符）。
//
// 约定（与可移植结构体逐项一致）：
//  - m4x4 * m4x4：引擎列向量约定 a*b == DXM 行向量约定 XMMatrixMultiply(b, a)；
//  - m4x4 * v4：引擎列向量 M*v == XMVector4Transform(v, M)；
//  - 除法 /f、/=f 用 XMVectorDivide + XMVectorReplicate（不乘倒数，保持
//    与可移植分支相同的舍入语义）；
//  - == 为逐分量精确比较（XMVector3Equal/XMVector4Equal，等价于可移植分支
//    的 x==x && y==y && z==z）；
//  - 全部经 XMLoad* / XMVector* / XMStore* 组合实现，不使用 XMVECTOR 的
//    运算符重载；
//  - 加载/存储一律用非对齐版本（XMLoadFloat3/4/4x4、XMStoreFloat3/4/4x4，
//    接受 A 类型的基类指针）。数值与 A 版本逐位相同（Float3 同样 w=0、同样
//    不写填充分量），但不要求 16 字节对齐：A 版本在 SSE 下是 _mm_load_ps，
//    遇到未对齐地址 Release 直接访问违例。引擎里有从外部缓冲 reinterpret_cast
//    出的 v3/v4 指针（如 EngineDLL/PhysicsAPI.cpp 的 C ABI 点数组、
//    ParticleFX/interop/RigidBodySpawner.cpp 的粒子属性），可移植结构体的
//    标量运算对这类地址不会崩，换成 A 版本会。

#include <DirectXMath.h>

namespace DirectX {

// ============================================================
// v2 = XMFLOAT2（及对齐版本 XMFLOAT2A，运算符集合相同）
// ============================================================

inline XMFLOAT2 operator+(const XMFLOAT2& a, const XMFLOAT2& b) {
	XMFLOAT2 r; XMStoreFloat2(&r, XMVectorAdd(XMLoadFloat2(&a), XMLoadFloat2(&b))); return r;
}
inline XMFLOAT2 operator-(const XMFLOAT2& a, const XMFLOAT2& b) {
	XMFLOAT2 r; XMStoreFloat2(&r, XMVectorSubtract(XMLoadFloat2(&a), XMLoadFloat2(&b))); return r;
}
inline XMFLOAT2 operator+(const XMFLOAT2& a, float s) {
	XMFLOAT2 r; XMStoreFloat2(&r, XMVectorAdd(XMLoadFloat2(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT2 operator-(const XMFLOAT2& a, float s) {
	XMFLOAT2 r; XMStoreFloat2(&r, XMVectorSubtract(XMLoadFloat2(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT2 operator*(const XMFLOAT2& a, float s) {
	XMFLOAT2 r; XMStoreFloat2(&r, XMVectorScale(XMLoadFloat2(&a), s)); return r;
}
inline XMFLOAT2 operator/(const XMFLOAT2& a, float s) {
	XMFLOAT2 r; XMStoreFloat2(&r, XMVectorDivide(XMLoadFloat2(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT2& operator+=(XMFLOAT2& a, const XMFLOAT2& b) {
	XMStoreFloat2(&a, XMVectorAdd(XMLoadFloat2(&a), XMLoadFloat2(&b))); return a;
}
inline XMFLOAT2& operator-=(XMFLOAT2& a, const XMFLOAT2& b) {
	XMStoreFloat2(&a, XMVectorSubtract(XMLoadFloat2(&a), XMLoadFloat2(&b))); return a;
}
inline XMFLOAT2& operator+=(XMFLOAT2& a, float s) {
	XMStoreFloat2(&a, XMVectorAdd(XMLoadFloat2(&a), XMVectorReplicate(s))); return a;
}
inline XMFLOAT2& operator-=(XMFLOAT2& a, float s) {
	XMStoreFloat2(&a, XMVectorSubtract(XMLoadFloat2(&a), XMVectorReplicate(s))); return a;
}

inline XMFLOAT2A operator+(const XMFLOAT2A& a, const XMFLOAT2A& b) {
	XMFLOAT2A r; XMStoreFloat2(&r, XMVectorAdd(XMLoadFloat2(&a), XMLoadFloat2(&b))); return r;
}
inline XMFLOAT2A operator-(const XMFLOAT2A& a, const XMFLOAT2A& b) {
	XMFLOAT2A r; XMStoreFloat2(&r, XMVectorSubtract(XMLoadFloat2(&a), XMLoadFloat2(&b))); return r;
}
inline XMFLOAT2A operator+(const XMFLOAT2A& a, float s) {
	XMFLOAT2A r; XMStoreFloat2(&r, XMVectorAdd(XMLoadFloat2(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT2A operator-(const XMFLOAT2A& a, float s) {
	XMFLOAT2A r; XMStoreFloat2(&r, XMVectorSubtract(XMLoadFloat2(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT2A operator*(const XMFLOAT2A& a, float s) {
	XMFLOAT2A r; XMStoreFloat2(&r, XMVectorScale(XMLoadFloat2(&a), s)); return r;
}
inline XMFLOAT2A operator/(const XMFLOAT2A& a, float s) {
	XMFLOAT2A r; XMStoreFloat2(&r, XMVectorDivide(XMLoadFloat2(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT2A& operator+=(XMFLOAT2A& a, const XMFLOAT2A& b) {
	XMStoreFloat2(&a, XMVectorAdd(XMLoadFloat2(&a), XMLoadFloat2(&b))); return a;
}
inline XMFLOAT2A& operator-=(XMFLOAT2A& a, const XMFLOAT2A& b) {
	XMStoreFloat2(&a, XMVectorSubtract(XMLoadFloat2(&a), XMLoadFloat2(&b))); return a;
}
inline XMFLOAT2A& operator+=(XMFLOAT2A& a, float s) {
	XMStoreFloat2(&a, XMVectorAdd(XMLoadFloat2(&a), XMVectorReplicate(s))); return a;
}
inline XMFLOAT2A& operator-=(XMFLOAT2A& a, float s) {
	XMStoreFloat2(&a, XMVectorSubtract(XMLoadFloat2(&a), XMVectorReplicate(s))); return a;
}

// ============================================================
// v3 = v3a = XMFLOAT3A
// 成员运算符：+v3、-v3、+f、-f、*f、/f、*v3（逐分量）、+=v3、-=v3、+=f、-=f、/=f、==
// 自由函数：一元负号、f*v3、f+v3
// ============================================================

inline XMFLOAT3A operator+(const XMFLOAT3A& a, const XMFLOAT3A& b) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorAdd(XMLoadFloat3(&a), XMLoadFloat3(&b))); return r;
}
inline XMFLOAT3A operator-(const XMFLOAT3A& a, const XMFLOAT3A& b) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorSubtract(XMLoadFloat3(&a), XMLoadFloat3(&b))); return r;
}
inline XMFLOAT3A operator+(const XMFLOAT3A& a, float s) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorAdd(XMLoadFloat3(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT3A operator-(const XMFLOAT3A& a, float s) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorSubtract(XMLoadFloat3(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT3A operator*(const XMFLOAT3A& a, float s) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorScale(XMLoadFloat3(&a), s)); return r;
}
inline XMFLOAT3A operator/(const XMFLOAT3A& a, float s) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorDivide(XMLoadFloat3(&a), XMVectorReplicate(s))); return r;
}
// 逐分量乘（lane-wise product），非点积。
inline XMFLOAT3A operator*(const XMFLOAT3A& a, const XMFLOAT3A& b) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorMultiply(XMLoadFloat3(&a), XMLoadFloat3(&b))); return r;
}
inline XMFLOAT3A& operator+=(XMFLOAT3A& a, const XMFLOAT3A& b) {
	XMStoreFloat3(&a, XMVectorAdd(XMLoadFloat3(&a), XMLoadFloat3(&b))); return a;
}
inline XMFLOAT3A& operator-=(XMFLOAT3A& a, const XMFLOAT3A& b) {
	XMStoreFloat3(&a, XMVectorSubtract(XMLoadFloat3(&a), XMLoadFloat3(&b))); return a;
}
inline XMFLOAT3A& operator+=(XMFLOAT3A& a, float s) {
	XMStoreFloat3(&a, XMVectorAdd(XMLoadFloat3(&a), XMVectorReplicate(s))); return a;
}
inline XMFLOAT3A& operator-=(XMFLOAT3A& a, float s) {
	XMStoreFloat3(&a, XMVectorSubtract(XMLoadFloat3(&a), XMVectorReplicate(s))); return a;
}
inline XMFLOAT3A& operator/=(XMFLOAT3A& a, float s) {
	XMStoreFloat3(&a, XMVectorDivide(XMLoadFloat3(&a), XMVectorReplicate(s))); return a;
}
// 逐分量精确比较（w 填充分量不参与，与可移植分支忽略 _pad 一致）。
inline bool operator==(const XMFLOAT3A& a, const XMFLOAT3A& b) {
	return XMVector3Equal(XMLoadFloat3(&a), XMLoadFloat3(&b));
}

// 一元负号用 XMVectorScale(v, -1)，不用 XMVectorNegate：后者在 SSE 下实现为
// 0 - x，对 +0 得 +0（丢失零的符号）；乘 -1 在 IEEE 下是精确的符号翻转
// （+0→-0、-0→+0），与 simd（fneg）和可移植分支（-x）逐位一致。
inline XMFLOAT3A operator-(const XMFLOAT3A& v) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorScale(XMLoadFloat3(&v), -1.0f)); return r;
}
inline XMFLOAT3A operator*(float s, const XMFLOAT3A& v) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorScale(XMLoadFloat3(&v), s)); return r;
}
inline XMFLOAT3A operator+(float s, const XMFLOAT3A& v) {
	XMFLOAT3A r; XMStoreFloat3(&r, XMVectorAdd(XMVectorReplicate(s), XMLoadFloat3(&v))); return r;
}

// ============================================================
// v4 = v4a = XMFLOAT4A
// 成员运算符：+v4、-v4、+f、-f、*f、/f、+=v4、+=f、-=f、/=f、==
// ============================================================

inline XMFLOAT4A operator+(const XMFLOAT4A& a, const XMFLOAT4A& b) {
	XMFLOAT4A r; XMStoreFloat4(&r, XMVectorAdd(XMLoadFloat4(&a), XMLoadFloat4(&b))); return r;
}
inline XMFLOAT4A operator-(const XMFLOAT4A& a, const XMFLOAT4A& b) {
	XMFLOAT4A r; XMStoreFloat4(&r, XMVectorSubtract(XMLoadFloat4(&a), XMLoadFloat4(&b))); return r;
}
inline XMFLOAT4A operator+(const XMFLOAT4A& a, float s) {
	XMFLOAT4A r; XMStoreFloat4(&r, XMVectorAdd(XMLoadFloat4(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT4A operator-(const XMFLOAT4A& a, float s) {
	XMFLOAT4A r; XMStoreFloat4(&r, XMVectorSubtract(XMLoadFloat4(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT4A operator*(const XMFLOAT4A& a, float s) {
	XMFLOAT4A r; XMStoreFloat4(&r, XMVectorScale(XMLoadFloat4(&a), s)); return r;
}
inline XMFLOAT4A operator/(const XMFLOAT4A& a, float s) {
	XMFLOAT4A r; XMStoreFloat4(&r, XMVectorDivide(XMLoadFloat4(&a), XMVectorReplicate(s))); return r;
}
inline XMFLOAT4A& operator+=(XMFLOAT4A& a, const XMFLOAT4A& b) {
	XMStoreFloat4(&a, XMVectorAdd(XMLoadFloat4(&a), XMLoadFloat4(&b))); return a;
}
inline XMFLOAT4A& operator+=(XMFLOAT4A& a, float s) {
	XMStoreFloat4(&a, XMVectorAdd(XMLoadFloat4(&a), XMVectorReplicate(s))); return a;
}
inline XMFLOAT4A& operator-=(XMFLOAT4A& a, float s) {
	XMStoreFloat4(&a, XMVectorSubtract(XMLoadFloat4(&a), XMVectorReplicate(s))); return a;
}
inline XMFLOAT4A& operator/=(XMFLOAT4A& a, float s) {
	XMStoreFloat4(&a, XMVectorDivide(XMLoadFloat4(&a), XMVectorReplicate(s))); return a;
}
// 逐分量精确比较（四分量全部参与，与可移植分支一致）。
inline bool operator==(const XMFLOAT4A& a, const XMFLOAT4A& b) {
	return XMVector4Equal(XMLoadFloat4(&a), XMLoadFloat4(&b));
}

// ============================================================
// m4x4 = m4x4a = XMFLOAT4X4A
// ============================================================

// 引擎 a*b（列向量）== XMMatrixMultiply(b, a)（DXM 行向量）；DXM 行 i ==
// 引擎列 i，两者结果字节一致。
inline XMFLOAT4X4A operator*(const XMFLOAT4X4A& a, const XMFLOAT4X4A& b) {
	XMFLOAT4X4A r;
	XMStoreFloat4x4(&r, XMMatrixMultiply(XMLoadFloat4x4(&b), XMLoadFloat4x4(&a)));
	return r;
}

// 引擎 M*v（列向量）== XMVector4Transform(v, M)（DXM 行向量）。
inline XMFLOAT4A operator*(const XMFLOAT4X4A& m, const XMFLOAT4A& v) {
	XMFLOAT4A r;
	XMStoreFloat4(&r, XMVector4Transform(XMLoadFloat4(&v), XMLoadFloat4x4(&m)));
	return r;
}

} // namespace DirectX
