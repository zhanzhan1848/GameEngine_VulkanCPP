// ============================================================================
// DirectXMath 形状替身头（Mac 语法门禁专用）
//
// !!!  仅供 Mac 语法门禁使用，不是 DirectXMath 实现，                !!!
// !!!  禁止进入任何真实构建的 include 路径。                          !!!
//
// - 只声明类型形状与函数签名（无实现），供 clang++ -fsyntax-only 在
//   "Windows 视角"（_WIN32/_WIN64/_MSC_VER 已定义、__APPLE__ 已 undef）
//   下检查共享编译单元里的 DirectXMath 调用点。
// - 成员与构造函数按 Microsoft Learn 文档核对（各声明旁注明 URL）。
// - 故意【不】给 XMFLOAT*/XMINT*/XMUINT* 存储类型提供任何运算符、
//   columns 成员或 operator[] —— 这些正是门禁要抓的不兼容写法。
//   （注：真实 DirectXMath >= 3.14 为存储类型提供了 operator== /
//   operator<=>，本替身按门禁设计省略；纯 == 比较若被门禁报出，
//   属已知假阳性，人工分诊即可。）
// - 函数只声明不实现：-fsyntax-only 无需函数体。任何 constexpr 语境下
//   的调用会因缺定义而报错——按垫片噪声分类，不按数学缺陷分类。
// ============================================================================
#ifndef DXMATH_SHAPE_GATE_DIRECTXMATH_STUB_H
#define DXMATH_SHAPE_GATE_DIRECTXMATH_STUB_H
#pragma once

#include <cstddef>
#include <cstdint>

// XM_CALLCONV：MSVC 上是 __vectorcall/__fastcall，非 MSVC（本门禁 clang）
// 上为空 —— 与真实 DirectXMath.h 对非 MSVC 编译器的行为一致
// （Learn: Library Internals / Calling Conventions，
//  https://learn.microsoft.com/en-us/windows/win32/dxmath/pg-xnamath-internals）
#ifndef XM_CALLCONV
#define XM_CALLCONV
#endif

// 常量（真实 DirectXMath.h 为 #define，此处同形）
// https://learn.microsoft.com/en-us/windows/win32/dxmath/pg-xnamath-internals#global-constants-in-the-directxmath-library
#define XM_PI       3.141592654f
#define XM_2PI      6.283185307f
#define XM_PIDIV2   1.570796327f
#define XM_PIDIV4   0.785398163f
#define XM_1DIV_PI  0.318309886f

namespace DirectX
{

// ----------------------------------------------------------------------------
// XMVECTOR：不透明 16 字节结构体替身（真实实现是 __m128/float4 联合体，
// 门禁只需形状）。Learn: XMVECTOR Data Type
// https://learn.microsoft.com/en-us/windows/win32/dxmath/xmvector-data-type
// ----------------------------------------------------------------------------
struct XMVECTOR
{
    // 不透明：无公开成员（真实类型可经 intrinsic 访问，门禁不需要）
    unsigned char _opaque[16];
};

// ----------------------------------------------------------------------------
// 调用约定 typedef —— 采用 Learn "Library Internals / Calling Conventions"
// 64 位 Windows __vectorcall 表：FX/G/H 按值、C 按 const 引用、矩阵按 const
// 引用。__fastcall 表与 __vectorcall 表在调用点层面（传左/右值均兼容）
// 等价，本替身不区分。
// https://learn.microsoft.com/en-us/windows/win32/dxmath/pg-xnamath-internals
// ----------------------------------------------------------------------------
typedef const XMVECTOR  FXMVECTOR;
typedef const XMVECTOR  GXMVECTOR;
typedef const XMVECTOR  HXMVECTOR;
typedef const XMVECTOR& CXMVECTOR;

// 前向声明 XMMATRIX 供 CXMMATRIX/FXMMATRIX typedef 使用
struct XMMATRIX;

typedef const XMMATRIX& FXMMATRIX;
typedef const XMMATRIX& CXMMATRIX;

// XMMATRIX：4 行 XMVECTOR。Learn:
// https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmmatrix
struct XMMATRIX
{
    XMVECTOR r[4];

    XMMATRIX() = default;
    XMMATRIX(const XMMATRIX&) = default;
    XMMATRIX& operator=(const XMMATRIX&) = default;

    XMMATRIX(XMMATRIX&&) = default;
    XMMATRIX& operator=(XMMATRIX&&) = default;

    XMMATRIX(FXMVECTOR R0, FXMVECTOR R1, FXMVECTOR R2, CXMVECTOR R3); // 仅声明

    // 运算符（真实头存在；门禁按声明提供，供调用点匹配）
    // Learn: XMMATRIX operators（ovw-xmmatrix-extensions）
    XMMATRIX operator- () const;                              // 仅声明
    XMMATRIX operator+ (FXMMATRIX M) const;                   // 仅声明
    XMMATRIX operator- (FXMMATRIX M) const;                   // 仅声明
    XMMATRIX operator* (FXMMATRIX M) const;                   // 仅声明
    XMMATRIX operator* (float S) const;                       // 仅声明
    XMMATRIX& operator+= (FXMMATRIX M);                       // 仅声明
    XMMATRIX& operator-= (FXMMATRIX M);                       // 仅声明
    XMMATRIX& operator*= (FXMMATRIX M);                       // 仅声明
    XMMATRIX& operator*= (float S);                           // 仅声明
};

// XMVECTOR 运算符（真实头存在；仅声明）
// Learn: XMVECTOR operators（ovw-xmvector-extensions）
XMVECTOR operator+ (FXMVECTOR V1, FXMVECTOR V2);       // 仅声明
XMVECTOR operator- (FXMVECTOR V1, FXMVECTOR V2);       // 仅声明
XMVECTOR operator* (FXMVECTOR V1, FXMVECTOR V2);       // 仅声明
XMVECTOR operator/ (FXMVECTOR V1, FXMVECTOR V2);       // 仅声明
XMVECTOR operator* (FXMVECTOR V, float S);             // 仅声明
XMVECTOR operator/ (FXMVECTOR V, float S);             // 仅声明
XMVECTOR operator* (float S, FXMVECTOR V);             // 仅声明
XMVECTOR operator- (FXMVECTOR V);                      // 仅声明
XMVECTOR& operator+= (XMVECTOR& V1, FXMVECTOR V2);     // 仅声明
XMVECTOR& operator-= (XMVECTOR& V1, FXMVECTOR V2);     // 仅声明
XMVECTOR& operator*= (XMVECTOR& V1, FXMVECTOR V2);     // 仅声明
XMVECTOR& operator/= (XMVECTOR& V1, FXMVECTOR V2);     // 仅声明

// ----------------------------------------------------------------------------
// 存储类型。规则（所有类型一致，按 Learn 各类型页核对）：
//   - 默认构造 = default；拷贝/移动 = default
//   - constexpr 分量构造
//   - explicit constexpr const T* 数组构造（真实头带 explicit；Learn 构造
//     子页面的 syntax 渲染省略了 explicit，以真实头与任务契约为准）
//   - 无任何算术/下标运算符（门禁设计，见文件头注释）
// ----------------------------------------------------------------------------

// XMFLOAT2 — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat2
struct XMFLOAT2
{
    float x;
    float y;

    XMFLOAT2() = default;
    XMFLOAT2(const XMFLOAT2&) = default;
    XMFLOAT2& operator=(const XMFLOAT2&) = default;
    XMFLOAT2(XMFLOAT2&&) = default;
    XMFLOAT2& operator=(XMFLOAT2&&) = default;

    constexpr XMFLOAT2(float _x, float _y) noexcept : x(_x), y(_y) {}
    explicit constexpr XMFLOAT2(const float* pArray) noexcept : x(pArray[0]), y(pArray[1]) {}
};

// XMFLOAT2A（16 字节对齐，继承非 A 版本）—
// https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat2a
struct alignas(16) XMFLOAT2A : public XMFLOAT2
{
    XMFLOAT2A() = default;
    XMFLOAT2A(const XMFLOAT2A&) = default;
    XMFLOAT2A& operator=(const XMFLOAT2A&) = default;
    XMFLOAT2A(XMFLOAT2A&&) = default;
    XMFLOAT2A& operator=(XMFLOAT2A&&) = default;

    constexpr XMFLOAT2A(float _x, float _y) noexcept : XMFLOAT2(_x, _y) {}
    explicit constexpr XMFLOAT2A(const float* pArray) noexcept : XMFLOAT2(pArray) {}
};

// XMFLOAT3 — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat3
struct XMFLOAT3
{
    float x;
    float y;
    float z;

    XMFLOAT3() = default;
    XMFLOAT3(const XMFLOAT3&) = default;
    XMFLOAT3& operator=(const XMFLOAT3&) = default;
    XMFLOAT3(XMFLOAT3&&) = default;
    XMFLOAT3& operator=(XMFLOAT3&&) = default;

    constexpr XMFLOAT3(float _x, float _y, float _z) noexcept : x(_x), y(_y), z(_z) {}
    explicit constexpr XMFLOAT3(const float* pArray) noexcept : x(pArray[0]), y(pArray[1]), z(pArray[2]) {}
};

// XMFLOAT3A — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat3a
struct alignas(16) XMFLOAT3A : public XMFLOAT3
{
    XMFLOAT3A() = default;
    XMFLOAT3A(const XMFLOAT3A&) = default;
    XMFLOAT3A& operator=(const XMFLOAT3A&) = default;
    XMFLOAT3A(XMFLOAT3A&&) = default;
    XMFLOAT3A& operator=(XMFLOAT3A&&) = default;

    constexpr XMFLOAT3A(float _x, float _y, float _z) noexcept : XMFLOAT3(_x, _y, _z) {}
    explicit constexpr XMFLOAT3A(const float* pArray) noexcept : XMFLOAT3(pArray) {}
};

// XMFLOAT4 — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat4
struct XMFLOAT4
{
    float x;
    float y;
    float z;
    float w;

    XMFLOAT4() = default;
    XMFLOAT4(const XMFLOAT4&) = default;
    XMFLOAT4& operator=(const XMFLOAT4&) = default;
    XMFLOAT4(XMFLOAT4&&) = default;
    XMFLOAT4& operator=(XMFLOAT4&&) = default;

    constexpr XMFLOAT4(float _x, float _y, float _z, float _w) noexcept : x(_x), y(_y), z(_z), w(_w) {}
    explicit constexpr XMFLOAT4(const float* pArray) noexcept : x(pArray[0]), y(pArray[1]), z(pArray[2]), w(pArray[3]) {}
};

// XMFLOAT4A — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat4a
struct alignas(16) XMFLOAT4A : public XMFLOAT4
{
    XMFLOAT4A() = default;
    XMFLOAT4A(const XMFLOAT4A&) = default;
    XMFLOAT4A& operator=(const XMFLOAT4A&) = default;
    XMFLOAT4A(XMFLOAT4A&&) = default;
    XMFLOAT4A& operator=(XMFLOAT4A&&) = default;

    constexpr XMFLOAT4A(float _x, float _y, float _z, float _w) noexcept : XMFLOAT4(_x, _y, _z, _w) {}
    explicit constexpr XMFLOAT4A(const float* pArray) noexcept : XMFLOAT4(pArray) {}
};

// XMFLOAT3X3 — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat3x3
struct XMFLOAT3X3
{
    union
    {
        struct
        {
            float _11, _12, _13;
            float _21, _22, _23;
            float _31, _32, _33;
        };
        float m[3][3];
    };

    XMFLOAT3X3() = default;
    XMFLOAT3X3(const XMFLOAT3X3&) = default;
    XMFLOAT3X3& operator=(const XMFLOAT3X3&) = default;
    XMFLOAT3X3(XMFLOAT3X3&&) = default;
    XMFLOAT3X3& operator=(XMFLOAT3X3&&) = default;

    XMFLOAT3X3(float m00, float m01, float m02,
               float m10, float m11, float m12,
               float m20, float m21, float m22) noexcept
        : _11(m00), _12(m01), _13(m02)
        , _21(m10), _22(m11), _23(m12)
        , _31(m20), _32(m21), _33(m22) {}
    explicit XMFLOAT3X3(const float* pArray) noexcept
        : _11(pArray[0]),  _12(pArray[1]),  _13(pArray[2])
        , _21(pArray[3]),  _22(pArray[4]),  _23(pArray[5])
        , _31(pArray[6]),  _32(pArray[7]),  _33(pArray[8]) {}

    float  operator() (size_t Row, size_t Column) const noexcept { return m[Row][Column]; }
    float& operator() (size_t Row, size_t Column) noexcept { return m[Row][Column]; }
};

// XMFLOAT4X4 — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat4x4
struct XMFLOAT4X4
{
    union
    {
        struct
        {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };

    XMFLOAT4X4() = default;
    XMFLOAT4X4(const XMFLOAT4X4&) = default;
    XMFLOAT4X4& operator=(const XMFLOAT4X4&) = default;
    XMFLOAT4X4(XMFLOAT4X4&&) = default;
    XMFLOAT4X4& operator=(XMFLOAT4X4&&) = default;

    XMFLOAT4X4(float m00, float m01, float m02, float m03,
               float m10, float m11, float m12, float m13,
               float m20, float m21, float m22, float m23,
               float m30, float m31, float m32, float m33) noexcept
        : _11(m00), _12(m01), _13(m02), _14(m03)
        , _21(m10), _22(m11), _23(m12), _24(m13)
        , _31(m20), _32(m21), _33(m22), _34(m23)
        , _41(m30), _42(m31), _43(m32), _44(m33) {}
    explicit XMFLOAT4X4(const float* pArray) noexcept
        : _11(pArray[ 0]), _12(pArray[ 1]), _13(pArray[ 2]), _14(pArray[ 3])
        , _21(pArray[ 4]), _22(pArray[ 5]), _23(pArray[ 6]), _24(pArray[ 7])
        , _31(pArray[ 8]), _32(pArray[ 9]), _33(pArray[10]), _34(pArray[11])
        , _41(pArray[12]), _42(pArray[13]), _43(pArray[14]), _44(pArray[15]) {}

    float  operator() (size_t Row, size_t Column) const noexcept { return m[Row][Column]; }
    float& operator() (size_t Row, size_t Column) noexcept { return m[Row][Column]; }
};

// XMFLOAT4X4A — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat4x4a
struct alignas(16) XMFLOAT4X4A : public XMFLOAT4X4
{
    XMFLOAT4X4A() = default;
    XMFLOAT4X4A(const XMFLOAT4X4A&) = default;
    XMFLOAT4X4A& operator=(const XMFLOAT4X4A&) = default;
    XMFLOAT4X4A(XMFLOAT4X4A&&) = default;
    XMFLOAT4X4A& operator=(XMFLOAT4X4A&&) = default;

    XMFLOAT4X4A(float m00, float m01, float m02, float m03,
                float m10, float m11, float m12, float m13,
                float m20, float m21, float m22, float m23,
                float m30, float m31, float m32, float m33) noexcept
        : XMFLOAT4X4(m00, m01, m02, m03, m10, m11, m12, m13,
                     m20, m21, m22, m23, m30, m31, m32, m33) {}
    explicit XMFLOAT4X4A(const float* pArray) noexcept : XMFLOAT4X4(pArray) {}
};

// XMINT2/3/4 — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmint2
struct XMINT2
{
    int32_t x;
    int32_t y;

    XMINT2() = default;
    XMINT2(const XMINT2&) = default;
    XMINT2& operator=(const XMINT2&) = default;
    XMINT2(XMINT2&&) = default;
    XMINT2& operator=(XMINT2&&) = default;

    constexpr XMINT2(int32_t _x, int32_t _y) noexcept : x(_x), y(_y) {}
    explicit constexpr XMINT2(const int32_t* pArray) noexcept : x(pArray[0]), y(pArray[1]) {}
};

struct XMINT3
{
    int32_t x;
    int32_t y;
    int32_t z;

    XMINT3() = default;
    XMINT3(const XMINT3&) = default;
    XMINT3& operator=(const XMINT3&) = default;
    XMINT3(XMINT3&&) = default;
    XMINT3& operator=(XMINT3&&) = default;

    constexpr XMINT3(int32_t _x, int32_t _y, int32_t _z) noexcept : x(_x), y(_y), z(_z) {}
    explicit constexpr XMINT3(const int32_t* pArray) noexcept : x(pArray[0]), y(pArray[1]), z(pArray[2]) {}
};

struct XMINT4
{
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t w;

    XMINT4() = default;
    XMINT4(const XMINT4&) = default;
    XMINT4& operator=(const XMINT4&) = default;
    XMINT4(XMINT4&&) = default;
    XMINT4& operator=(XMINT4&&) = default;

    constexpr XMINT4(int32_t _x, int32_t _y, int32_t _z, int32_t _w) noexcept : x(_x), y(_y), z(_z), w(_w) {}
    explicit constexpr XMINT4(const int32_t* pArray) noexcept : x(pArray[0]), y(pArray[1]), z(pArray[2]), w(pArray[3]) {}
};

// XMUINT2/3/4 — https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmuint2
struct XMUINT2
{
    uint32_t x;
    uint32_t y;

    XMUINT2() = default;
    XMUINT2(const XMUINT2&) = default;
    XMUINT2& operator=(const XMUINT2&) = default;
    XMUINT2(XMUINT2&&) = default;
    XMUINT2& operator=(XMUINT2&&) = default;

    constexpr XMUINT2(uint32_t _x, uint32_t _y) noexcept : x(_x), y(_y) {}
    explicit constexpr XMUINT2(const uint32_t* pArray) noexcept : x(pArray[0]), y(pArray[1]) {}
};

struct XMUINT3
{
    uint32_t x;
    uint32_t y;
    uint32_t z;

    XMUINT3() = default;
    XMUINT3(const XMUINT3&) = default;
    XMUINT3& operator=(const XMUINT3&) = default;
    XMUINT3(XMUINT3&&) = default;
    XMUINT3& operator=(XMUINT3&&) = default;

    constexpr XMUINT3(uint32_t _x, uint32_t _y, uint32_t _z) noexcept : x(_x), y(_y), z(_z) {}
    explicit constexpr XMUINT3(const uint32_t* pArray) noexcept : x(pArray[0]), y(pArray[1]), z(pArray[2]) {}
};

struct XMUINT4
{
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;

    XMUINT4() = default;
    XMUINT4(const XMUINT4&) = default;
    XMUINT4& operator=(const XMUINT4&) = default;
    XMUINT4(XMUINT4&&) = default;
    XMUINT4& operator=(XMUINT4&&) = default;

    constexpr XMUINT4(uint32_t _x, uint32_t _y, uint32_t _z, uint32_t _w) noexcept : x(_x), y(_y), z(_z), w(_w) {}
    explicit constexpr XMUINT4(const uint32_t* pArray) noexcept : x(pArray[0]), y(pArray[1]), z(pArray[2]), w(pArray[3]) {}
};

// ----------------------------------------------------------------------------
// 函数声明（只声明不实现）。URL 规则：
//   https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-<小写函数名>
// 各函数签名与 Learn 页面一致（含 FXMVECTOR/GXMVECTOR/HXMVECTOR/CXMVECTOR/
// FXMMATRIX/CXMMATRIX 的位置与顺序）。
// ----------------------------------------------------------------------------

// ---- Load / Store ----
// XMLoadFloat2  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat2
XMVECTOR XM_CALLCONV XMLoadFloat2(const XMFLOAT2* pSource);
// XMLoadFloat2A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat2a
XMVECTOR XM_CALLCONV XMLoadFloat2A(const XMFLOAT2A* pSource);
// XMLoadFloat3  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat3
XMVECTOR XM_CALLCONV XMLoadFloat3(const XMFLOAT3* pSource);
// XMLoadFloat3A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat3a
XMVECTOR XM_CALLCONV XMLoadFloat3A(const XMFLOAT3A* pSource);
// XMLoadFloat4  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat4
XMVECTOR XM_CALLCONV XMLoadFloat4(const XMFLOAT4* pSource);
// XMLoadFloat4A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat4a
XMVECTOR XM_CALLCONV XMLoadFloat4A(const XMFLOAT4A* pSource);
// XMLoadFloat4x4  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat4x4
XMMATRIX XM_CALLCONV XMLoadFloat4x4(const XMFLOAT4X4* pSource);
// XMLoadFloat4x4A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat4x4a
XMMATRIX XM_CALLCONV XMLoadFloat4x4A(const XMFLOAT4X4A* pSource);

// XMStoreFloat2  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat2
void XM_CALLCONV XMStoreFloat2(XMFLOAT2* pDestination, FXMVECTOR V);
// XMStoreFloat2A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat2a
void XM_CALLCONV XMStoreFloat2A(XMFLOAT2A* pDestination, FXMVECTOR V);
// XMStoreFloat3  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat3
void XM_CALLCONV XMStoreFloat3(XMFLOAT3* pDestination, FXMVECTOR V);
// XMStoreFloat3A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat3a
void XM_CALLCONV XMStoreFloat3A(XMFLOAT3A* pDestination, FXMVECTOR V);
// XMStoreFloat4  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat4
void XM_CALLCONV XMStoreFloat4(XMFLOAT4* pDestination, FXMVECTOR V);
// XMStoreFloat4A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat4a
void XM_CALLCONV XMStoreFloat4A(XMFLOAT4A* pDestination, FXMVECTOR V);
// XMStoreFloat4x4  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat4x4
void XM_CALLCONV XMStoreFloat4x4(XMFLOAT4X4* pDestination, FXMMATRIX M);
// XMStoreFloat4x4A https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmstorefloat4x4a
void XM_CALLCONV XMStoreFloat4x4A(XMFLOAT4X4A* pDestination, FXMMATRIX M);

// ---- 向量基础 ----
// XMVectorSet   https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorset
XMVECTOR XM_CALLCONV XMVectorSet(float x, float y, float z, float w);
// XMVectorZero  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorzero
XMVECTOR XM_CALLCONV XMVectorZero();
// XMVectorReplicate https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorreplicate
XMVECTOR XM_CALLCONV XMVectorReplicate(float Value);
// XMVectorAdd  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectoradd
XMVECTOR XM_CALLCONV XMVectorAdd(FXMVECTOR V1, FXMVECTOR V2);
// XMVectorSubtract https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorsubtract
XMVECTOR XM_CALLCONV XMVectorSubtract(FXMVECTOR V1, FXMVECTOR V2);
// XMVectorMultiply https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectormultiply
XMVECTOR XM_CALLCONV XMVectorMultiply(FXMVECTOR V1, FXMVECTOR V2);
// XMVectorDivide  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectordivide
XMVECTOR XM_CALLCONV XMVectorDivide(FXMVECTOR V1, FXMVECTOR V2);
// XMVectorScale  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorscale
XMVECTOR XM_CALLCONV XMVectorScale(FXMVECTOR V, float ScaleFactor);
// XMVectorNegate https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectornegate
XMVECTOR XM_CALLCONV XMVectorNegate(FXMVECTOR V);
// XMVectorGetX https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorgetx
float XM_CALLCONV XMVectorGetX(FXMVECTOR V);
// XMVectorGetY https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorgety
float XM_CALLCONV XMVectorGetY(FXMVECTOR V);
// XMVectorGetZ https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorgetz
float XM_CALLCONV XMVectorGetZ(FXMVECTOR V);
// XMVectorGetW https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorgetw
float XM_CALLCONV XMVectorGetW(FXMVECTOR V);
// XMVectorSetW https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvectorsetw
XMVECTOR XM_CALLCONV XMVectorSetW(FXMVECTOR V, float w);

// ---- 2D/3D/4D 向量运算 ----
// XMVector2Dot https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector2dot
XMVECTOR XM_CALLCONV XMVector2Dot(FXMVECTOR V1, FXMVECTOR V2);
// XMVector2Length https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector2length
XMVECTOR XM_CALLCONV XMVector2Length(FXMVECTOR V);
// XMVector3Dot https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3dot
XMVECTOR XM_CALLCONV XMVector3Dot(FXMVECTOR V1, FXMVECTOR V2);
// XMVector3Cross https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3cross
XMVECTOR XM_CALLCONV XMVector3Cross(FXMVECTOR V1, FXMVECTOR V2);
// XMVector3Length https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3length
XMVECTOR XM_CALLCONV XMVector3Length(FXMVECTOR V);
// XMVector3LengthSq https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3lengthsq
XMVECTOR XM_CALLCONV XMVector3LengthSq(FXMVECTOR V);
// XMVector3Normalize https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3normalize
XMVECTOR XM_CALLCONV XMVector3Normalize(FXMVECTOR V);
// XMVector3Equal https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3equal
bool XM_CALLCONV XMVector3Equal(FXMVECTOR V1, FXMVECTOR V2);
// XMVector3Transform https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3transform
XMVECTOR XM_CALLCONV XMVector3Transform(FXMVECTOR V, FXMMATRIX M);
// XMVector3TransformNormal https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3transformnormal
XMVECTOR XM_CALLCONV XMVector3TransformNormal(FXMVECTOR V, FXMMATRIX M);
// XMVector3Rotate https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3rotate
XMVECTOR XM_CALLCONV XMVector3Rotate(FXMVECTOR V1, FXMVECTOR V2);
// XMVector4Dot https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector4dot
XMVECTOR XM_CALLCONV XMVector4Dot(FXMVECTOR V1, FXMVECTOR V2);
// XMVector4Length https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector4length
XMVECTOR XM_CALLCONV XMVector4Length(FXMVECTOR V);
// XMVector4Equal https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector4equal
bool XM_CALLCONV XMVector4Equal(FXMVECTOR V1, FXMVECTOR V2);
// XMVector4Transform https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector4transform
XMVECTOR XM_CALLCONV XMVector4Transform(FXMVECTOR V, FXMMATRIX M);

// ---- 矩阵 ----
// XMMatrixIdentity https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixidentity
XMMATRIX XM_CALLCONV XMMatrixIdentity();
// XMMatrixMultiply  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixmultiply
XMMATRIX XM_CALLCONV XMMatrixMultiply(FXMMATRIX M1, CXMMATRIX M2);
// XMMatrixTranspose https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixtranspose
XMMATRIX XM_CALLCONV XMMatrixTranspose(FXMMATRIX M);
// XMMatrixDeterminant https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixdeterminant
XMVECTOR XM_CALLCONV XMMatrixDeterminant(FXMMATRIX M);
// XMMatrixInverse https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixinverse
XMMATRIX XM_CALLCONV XMMatrixInverse(XMVECTOR* pDeterminant, FXMMATRIX M);
// XMMatrixTranslation https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixtranslation
XMMATRIX XM_CALLCONV XMMatrixTranslation(float OffsetX, float OffsetY, float OffsetZ);
// XMMatrixTranslationFromVector https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixtranslationfromvector
XMMATRIX XM_CALLCONV XMMatrixTranslationFromVector(FXMVECTOR Offset);
// XMMatrixScaling https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixscaling
XMMATRIX XM_CALLCONV XMMatrixScaling(float ScaleX, float ScaleY, float ScaleZ);
// XMMatrixScalingFromVector https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixscalingfromvector
XMMATRIX XM_CALLCONV XMMatrixScalingFromVector(FXMVECTOR Scale);
// XMMatrixRotationX https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixrotationx
XMMATRIX XM_CALLCONV XMMatrixRotationX(float Angle);
// XMMatrixRotationY https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixrotationy
XMMATRIX XM_CALLCONV XMMatrixRotationY(float Angle);
// XMMatrixRotationZ https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixrotationz
XMMATRIX XM_CALLCONV XMMatrixRotationZ(float Angle);
// XMMatrixRotationAxis https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixrotationaxis
XMMATRIX XM_CALLCONV XMMatrixRotationAxis(FXMVECTOR Axis, float Angle);
// XMMatrixRotationNormal https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixrotationnormal
XMMATRIX XM_CALLCONV XMMatrixRotationNormal(FXMVECTOR NormalAxis, float Angle);
// XMMatrixRotationQuaternion https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixrotationquaternion
XMMATRIX XM_CALLCONV XMMatrixRotationQuaternion(FXMVECTOR Quaternion);
// XMMatrixRotationRollPitchYaw https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixrotationrollpitchyaw
XMMATRIX XM_CALLCONV XMMatrixRotationRollPitchYaw(float Pitch, float Yaw, float Roll);
// XMMatrixAffineTransformation（签名顺序按 Learn 页面核对）https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixaffinetransformation
XMMATRIX XM_CALLCONV XMMatrixAffineTransformation(FXMVECTOR Scaling, FXMVECTOR RotationOrigin, FXMVECTOR RotationQuaternion, GXMVECTOR Translation);
// XMMatrixPerspectiveFovRH https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixperspectivefovrh
XMMATRIX XM_CALLCONV XMMatrixPerspectiveFovRH(float FovAngleY, float AspectRatio, float NearZ, float FarZ);
// XMMatrixPerspectiveFovLH https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixperspectivefovlh
XMMATRIX XM_CALLCONV XMMatrixPerspectiveFovLH(float FovAngleY, float AspectRatio, float NearZ, float FarZ);
// XMMatrixOrthographicOffCenterRH https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixorthographicoffcenterrh
XMMATRIX XM_CALLCONV XMMatrixOrthographicOffCenterRH(float ViewLeft, float ViewRight, float ViewBottom, float ViewTop, float NearZ, float FarZ);
// XMMatrixOrthographicLH https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixorthographiclh
XMMATRIX XM_CALLCONV XMMatrixOrthographicLH(float ViewWidth, float ViewHeight, float NearZ, float FarZ);
// XMMatrixOrthographicRH（legacy Vulkan 侧同族补齐）https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixorthographicrh
XMMATRIX XM_CALLCONV XMMatrixOrthographicRH(float ViewWidth, float ViewHeight, float NearZ, float FarZ);
// XMMatrixLookAtRH https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixlookatrh
XMMATRIX XM_CALLCONV XMMatrixLookAtRH(FXMVECTOR EyePosition, FXMVECTOR FocusPosition, FXMVECTOR UpDirection);
// XMMatrixLookToRH https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixlooktorh
XMMATRIX XM_CALLCONV XMMatrixLookToRH(FXMVECTOR EyePosition, FXMVECTOR FocusPosition, FXMVECTOR UpDirection);
// XMMatrixLookToLH https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixlooktolh
XMMATRIX XM_CALLCONV XMMatrixLookToLH(FXMVECTOR EyePosition, FXMVECTOR FocusPosition, GXMVECTOR UpDirection);
// XMMatrixDecompose（out 参数顺序按 Learn 页面核对）https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmmatrixdecompose
bool XM_CALLCONV XMMatrixDecompose(XMVECTOR* outScale, XMVECTOR* outRotQuat, XMVECTOR* outTrans, FXMMATRIX M);

// ---- 四元数 ----
// XMQuaternionRotationRollPitchYaw https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionrotationrollpitchyaw
XMVECTOR XM_CALLCONV XMQuaternionRotationRollPitchYaw(float Pitch, float Yaw, float Roll);
// XMQuaternionRotationRollPitchYawFromVector https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionrotationrollpitchyawfromvector
XMVECTOR XM_CALLCONV XMQuaternionRotationRollPitchYawFromVector(FXMVECTOR Angles);
// XMQuaternionRotationMatrix https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionrotationmatrix
XMVECTOR XM_CALLCONV XMQuaternionRotationMatrix(FXMMATRIX M);
// XMQuaternionRotationAxis https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionrotationaxis
XMVECTOR XM_CALLCONV XMQuaternionRotationAxis(FXMVECTOR Axis, float Angle);
// XMQuaternionRotationNormal https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionrotationnormal
XMVECTOR XM_CALLCONV XMQuaternionRotationNormal(FXMVECTOR NormalAxis, float Angle);
// XMQuaternionMultiply https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionmultiply
XMVECTOR XM_CALLCONV XMQuaternionMultiply(FXMVECTOR Q1, FXMVECTOR Q2);
// XMQuaternionNormalize https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionnormalize
XMVECTOR XM_CALLCONV XMQuaternionNormalize(FXMVECTOR Q);
// XMQuaternionInverse https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternioninverse
XMVECTOR XM_CALLCONV XMQuaternionInverse(FXMVECTOR Q);
// XMQuaternionConjugate https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionconjugate
XMVECTOR XM_CALLCONV XMQuaternionConjugate(FXMVECTOR Q);
// XMQuaternionIdentity https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmquaternionidentity
XMVECTOR XM_CALLCONV XMQuaternionIdentity();

// ---- 标量 / 角度 ----
// XMScalarSin  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmscalarsin
float XM_CALLCONV XMScalarSin(float Value);
// XMScalarCos  https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmscalarcos
float XM_CALLCONV XMScalarCos(float Value);
// XMScalarSinCos（指针 out 参，按 Learn 页面核对）https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmscalarsincos
void XM_CALLCONV XMScalarSinCos(float* pSin, float* pCos, float Value);
// XMScalarNearEqual https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmscalarnearequal
bool XM_CALLCONV XMScalarNearEqual(float S1, float S2, float Epsilon);
// XMScalarACos（EngineTest/Scripts.cpp、legacy VulkanLight.cpp 在用）
// https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmscalaracos
float XM_CALLCONV XMScalarACos(float Value);
// XMScalarASin（ACos 同族防御性补齐）
// https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmscalarasin
float XM_CALLCONV XMScalarASin(float Value);
// XMConvertToRadians https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmconverttoradians
float XM_CALLCONV XMConvertToRadians(float fDegrees);
// XMConvertToDegrees https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmconverttodegrees
float XM_CALLCONV XMConvertToDegrees(float fRadians);

} // namespace DirectX

#endif // DXMATH_SHAPE_GATE_DIRECTXMATH_STUB_H
