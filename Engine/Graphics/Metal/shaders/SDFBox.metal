#include "CommonFunction.metal"

// Box Distance
float boxDistance( float3 position, float3 center ) 
{
    float3 distance = (abs(position) - center).xy;
    float3 maxD = max(distance, 0.0);                                           // 取正值部分
    float outerDist = length(maxD);                                             // 外部距离
    float innerDist = min(max(max(distance.x, distance.y), distance.z), 0.0);   // 内部距离，手动实现 maxcomp
    return outerDist + innerDist;                                               // 最终距离
}

// Box Gradient / Normal
float3 boxGradient(float3 position, float3 center) {
    float3 distance = abs(position) - center;                                   // 距离差
    float3 s = sign(position);                                                  // 符号向量 (替代 msign)
    float g = max(max(distance.x, distance.y), distance.z);                     // 手动实现 maxcomp，找出最大分量
    
    float3 result;
    if (g > 0.0) {
        result = s * normalize(max(distance, 0.0));                             // 外部梯度，标准化正值部分
    } else {
        // 内部梯度，使用 step 函数模拟逐分量比较
        float3 step1 = step(distance.yzx, distance.xyz);                        // 比较循环移位后的分量
        float3 step2 = step(distance.zxy, distance.xyz);                        // 再次循环移位比较
        result = s * (step1 * step2);                                           // 结合符号和比较结果
    }
    return result;
}

// Box Ambient Occlusion
// 输入 位置（pos）、 法线（nor）、 变换矩阵（txx）、 box大小（rad）
float boxOcclusion(float3 pos, float3 nor, float4x4 txx, float3 rad) {
    // 变换位置和法线
    float3 p = (txx * float4(pos, 1.0)).xyz;  // 位置变换
    float3 n = (txx * float4(nor, 0.0)).xyz;  // 法线变换（无平移）

    // 8 个顶点方向向量
    float3 v0 = normalize(float3(-1.0, -1.0, -1.0) * rad - p);
    float3 v1 = normalize(float3( 1.0, -1.0, -1.0) * rad - p);
    float3 v2 = normalize(float3(-1.0,  1.0, -1.0) * rad - p);
    float3 v3 = normalize(float3( 1.0,  1.0, -1.0) * rad - p);
    float3 v4 = normalize(float3(-1.0, -1.0,  1.0) * rad - p);
    float3 v5 = normalize(float3( 1.0, -1.0,  1.0) * rad - p);
    float3 v6 = normalize(float3(-1.0,  1.0,  1.0) * rad - p);
    float3 v7 = normalize(float3( 1.0,  1.0,  1.0) * rad - p);

    // 12 条边的遮挡贡献
    float k02 = dot(n, normalize(cross(v2, v0))) * acos(dot(v0, v2));
    float k23 = dot(n, normalize(cross(v3, v2))) * acos(dot(v2, v3));
    float k31 = dot(n, normalize(cross(v1, v3))) * acos(dot(v3, v1));
    float k10 = dot(n, normalize(cross(v0, v1))) * acos(dot(v1, v0));
    float k45 = dot(n, normalize(cross(v5, v4))) * acos(dot(v4, v5));
    float k57 = dot(n, normalize(cross(v7, v5))) * acos(dot(v5, v7));
    float k76 = dot(n, normalize(cross(v6, v7))) * acos(dot(v7, v6));
    float k37 = dot(n, normalize(cross(v7, v3))) * acos(dot(v3, v7));
    float k64 = dot(n, normalize(cross(v4, v6))) * acos(dot(v6, v4));
    float k51 = dot(n, normalize(cross(v1, v5))) * acos(dot(v5, v1));
    float k04 = dot(n, normalize(cross(v4, v0))) * acos(dot(v0, v4));
    float k62 = dot(n, normalize(cross(v2, v6))) * acos(dot(v6, v2));

    // 6 个面的遮挡贡献
    float occ = 0.0;
    occ += (k02 + k23 + k31 + k10) * step(0.0,  v0.z);  // -z 面
    occ += (k45 + k57 + k76 + k64) * step(0.0, -v4.z);  // +z 面
    occ += (k51 - k31 + k37 - k57) * step(0.0, -v5.x);  // +x 面
    occ += (k04 - k64 + k62 - k02) * step(0.0,  v0.x);  // -x 面
    occ += (-k76 - k37 - k23 - k62) * step(0.0, -v6.y); // +y 面
    occ += (-k10 - k51 - k45 - k04) * step(0.0,  v0.y); // -y 面

    // 归一化，返回遮挡值
    return occ / 6.283185;
}

// Box Soft Shadows
float segShadow(float3 ro, float3 rd, float3 pa, float sh) {
    float k1 = 1.0 - rd.x * rd.x;            // dot(rd.yz, rd.yz)
    float k4 = (ro.x - pa.x) * k1;
    float k6 = (ro.x + pa.x) * k1;
    float2 k5 = ro.yz * k1;                   // ro.yz 提取 yz 分量
    float2 k7 = pa.yz * k1;                   // pa.yz 提取 yz 分量
    float k2 = -dot(ro.yz, rd.yz);           // 负的点积
    float2 k3 = pa.yz * rd.yz;               // 分量乘积

    for (int i = 0; i < 4; i++) {
        float2 ss = float2(float(i & 1), float(i >> 1)) * 2.0 - 1.0;  // 位运算生成 (x, y) = (±1, ±1)
        float thx = k2 + dot(ss, k3);
        if (thx < 0.0) continue;              // 射线在后面，跳过

        float thy = clamp(-rd.x * thx, k4, k6);
        float3 diff = float3(thy, k5 - k7 * ss) + rd * thx;
        sh = min(sh, length2(diff) / (thx * thx));
    }
    return sh;
}

// 射线起点 row，射线方向 rdw，盒子变换矩阵 txx，盒子半尺寸 rad，阴影柔和度 sk
float boxSoftShadow(float3 row, float3 rdw, float4x4 txx, float3 rad, float sk) {
    // 变换射线起点和方向
    float3 rd = (txx * float4(rdw, 0.0)).xyz;  // 方向变换（无平移）
    float3 ro = (txx * float4(row, 1.0)).xyz;  // 位置变换

    // 射线与盒子的相交测试
    float3 m = 1.0 / rd;                        // 每分量倒数
    float3 n = m * ro;                          // 起点调整
    float3 k = abs(m) * rad;                    // 边界距离
    float3 t1 = -n - k;                         // 近端交点
    float3 t2 = -n + k;                         // 远端交点

    float tN = max(max(t1.x, t1.y), t1.z);      // 最近交点
    float tF = min(min(t2.x, t2.y), t2.z);      // 最远交点

    if (tN > tF || tF < 0.0) {
        float sh = 1.0;
        sh = segShadow(ro, rd, rad, sh);        // x 轴方向
        sh = segShadow(float3(ro.y, ro.z, ro.x), float3(rd.y, rd.z, rd.x), float3(rad.y, rad.z, rad.x), sh); // yzx
        sh = segShadow(float3(ro.z, ro.x, ro.y), float3(rd.z, rd.x, rd.y), float3(rad.z, rad.x, rad.y), sh); // zxy
        sh = clamp(sk * sqrt(sh), 0.0, 1.0);   // 柔和度调整
        return sh * sh * (3.0 - 2.0 * sh);     // 平滑插值
    }
    return 0.0;  // 无阴影
}

// Box Density
// wro 和 wrd：射线起点和方向 (vec3)。txx：盒子的旋转和平移矩阵 (mat4)。rad：盒子半尺寸 (vec3)。dbuffer：深度缓冲值，用于裁剪。
float boxDensity(float3 wro, float3 wrd, float4x4 txx, float3 rad, float dbuffer) {
    // 变换射线到盒子空间
    float3 d = (txx * float4(wrd, 0.0)).xyz;  // 方向变换（无平移）
    float3 o = (txx * float4(wro, 1.0)).xyz;  // 位置变换

    // 射线与盒子的相交测试
    float3 m = 1.0 / d;                       // 每分量倒数
    float3 n = m * o;                         // 起点调整
    float3 k = abs(m) * rad;                  // 边界距离
    float3 ta = -n - k;                       // 近端交点
    float3 tb = -n + k;                       // 远端交点
    float tN = max(max(ta.x, ta.y), ta.z);    // 最近交点
    float tF = min(min(tb.x, tb.y), tb.z);    // 最远交点

    // 无相交或在相机后面
    if (tN > tF || tF < 0.0) return 0.0;

    // 不可见（在相机后面或超过 dbuffer）
    if (tF < 0.0 || tN > dbuffer) return 0.0;

    // 裁剪射线段到 [0, dbuffer]
    tN = max(tN, 0.0);
    tF = min(tF, dbuffer);

    // 移动射线到近交点
    o += tN * d;
    tF = tF - tN;
    tN = 0.0;

    // 密度函数的解析积分
    float3 ir2 = 1.0 / (rad * rad);           // 1 / (rad^2)
    float3 a = 1.0 - (o * o) * ir2;           // 1 - (o/rad)^2
    float3 b = -2.0 * (o * d) * ir2;          // -2 * (o*d) / rad^2
    float3 c = -(d * d) * ir2;                // -(d^2) / rad^2

    // 计算 t 的幂
    float t1 = tF;
    float t2 = t1 * t1;
    float t3 = t2 * t1;
    float t4 = t2 * t2;
    float t5 = t2 * t3;
    float t6 = t3 * t3;
    float t7 = t3 * t4;

    // 积分结果
    return (t1 / 1.0) * (a.x * a.y * a.z) +
           (t2 / 2.0) * (a.x * a.y * b.z + a.x * b.y * a.z + b.x * a.y * a.z) +
           (t3 / 3.0) * (a.x * a.y * c.z + a.x * b.y * b.z + a.x * c.y * a.z +
                         b.x * a.y * b.z + b.x * b.y * a.z + c.x * a.y * a.z) +
           (t4 / 4.0) * (a.x * b.y * c.z + a.x * c.y * b.z + b.x * a.y * c.z +
                         b.x * b.y * b.z + b.x * c.y * a.z + c.x * a.y * b.z +
                         c.x * b.y * a.z) +
           (t5 / 5.0) * (a.x * c.y * c.z + b.x * b.y * c.z + b.x * c.y * b.z +
                         c.x * a.y * c.z + c.x * b.y * b.z + c.x * c.y * a.z) +
           (t6 / 6.0) * (b.x * c.y * c.z + c.x * b.y * c.z + c.x * c.y * b.z) +
           (t7 / 7.0) * (c.x * c.y * c.z);
}