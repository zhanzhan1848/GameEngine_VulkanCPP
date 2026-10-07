#version 450 core
// ProbeSphere.vert — REAL mesh path: one shared icosphere VB/IB, per-instance
// transform matrix (instance vertex input), view_proj via push constants.
// No descriptors, no GPU culling — every probe draws, exactly like a scene
// mesh would. Probe positions come from the instance matrix buffer, which is
// filled once before the render loop from the probe grid definition (and will
// carry per-probe relocation matrices when that lands).

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
// Instance transform, COLUMN-major vec4s (column 3 = translation).
layout(location = 2) in float aProbe;
// 2026-09-17 审计 §8.3：重定位分类色（每顶点烘焙，CPU 读回 relocated origins
// 后重建 VB）。2.0 = 无状态数据（legacy 色相）；0=未 trace 灰、0.5=借格红、
// 0.75=本格重定位黄、1.0=本格自由绿。
layout(location = 3) in float aState;
// 探针中心（每探针 60 顶点相同）——相机隐藏按中心判定。逐顶点判定会把
// 2m 边界探针的部分顶点塌缩、其余保留，产生横跨屏幕的拉伸三角形（§8.3
// "异常大小形状"的根因）。
layout(location = 4) in vec3 aCenter;

layout(push_constant) uniform PC {
    mat4 view_proj;
    vec4 sun_dir;   // xyz = toward light
    vec4 cam_pos;   // xyz = camera world position, w = hide radius (m)
    vec4 misc;      // [P0] x = 着色模式（0=state 1=radiance 2=confidence
                    //     3=variance；PRIMAL_PROBE_SPHERE_MODE）——状态=生命
                    //     周期诊断，辐射度=能量诊断（P0 解耦）。
} pc;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vClip;
layout(location = 3) flat out uint vProbe;
layout(location = 4) flat out float vState;
layout(location = 5) flat out float vMode;
layout(location = 6) flat out float vTotal;  // [P2 SoA] probeTotal（misc.y）

void main() {
    // Probe index from the flat vertex stream (no gl_InstanceIndex anywhere).
    vProbe = uint(aProbe);
    vState = aState;
    vMode = pc.misc.x;
    vTotal = pc.misc.y;
    vWorld = aPos;
    vNormal = aNormal;
    // Hide probes within the hide radius of the camera — per-PROBE (center),
    // so all 60 vertices collapse uniformly into a degenerate, fully-clipped
    // primitive. 切片带在相机高度，半径太小会让 2-3m 处的球糊满屏幕。
    if (distance(aCenter, pc.cam_pos.xyz) < pc.cam_pos.w) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // outside the clip volume
        vClip = gl_Position;
        return;
    }
    vClip = pc.view_proj * vec4(aPos, 1.0);
    gl_Position = vClip;
}
