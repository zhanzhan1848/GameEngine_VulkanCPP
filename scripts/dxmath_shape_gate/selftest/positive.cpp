// 正例：全部走 P1 契约的合法写法，门禁必须零错误通过。
#include "mini_contract.h"

using namespace primal::math;
using namespace DirectX; // 与 EngineDLL/EntityAPI.cpp 的 _MSC_VER 分支同款

void ok() {
    v3 a{ 1.f, 2.f, 3.f };
    v3 b{ 4.f, 5.f, 6.f };
    v3 c = a + b;             // namespace DirectX 运算符
    v3 d = c * 2.f;
    v3 e = 0.5f * d;
    v3 f = c - d;
    v4 q = make_v4(a, 1.f);   // MathAccess 组合构造（替代 v4{v3,w}）
    (void)e; (void)f;
    f32 x = component(a, 1);
    (void)x;

    m4x4 m = make_m4x4(v4{1,0,0,0}, v4{0,1,0,0}, v4{0,0,1,0}, v4{5,6,7,1});
    f32 e00 = elem(m, 0, 0);          // 引擎语义 m.columns[0][0]
    f32 e12 = elem(m, 1, 2);
    v4 col = column(m, 3);
    set_elem(m, 2, 1, 9.f);
    set_column(m, 0, v4{8,0,0,0});
    (void)e00; (void)e12; (void)col; (void)q;

    // 走 XM 函数与类型（EngineDLL _MSC_VER 分支同款形状）
    XMFLOAT3A rot{ a.x, a.y, a.z };
    XMVECTOR quat{ XMQuaternionRotationRollPitchYawFromVector(XMLoadFloat3A(&rot)) };
    XMFLOAT4A out{};
    XMStoreFloat4A(&out, quat);
    (void)out;

    // splat 替代标量广播构造
    v3 s = splat3(1.f);
    (void)s;
    u32v2 uv{ 1u, 2u };
    (void)uv;
    s32v4 sv{ 1, 2, 3, 4 };
    (void)sv;
}
