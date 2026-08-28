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

layout(push_constant) uniform PC {
    mat4 view_proj;
    vec4 sun_dir;   // xyz = toward light
    vec4 cam_pos;   // xyz = camera world position
} pc;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vClip;
layout(location = 3) flat out uint vProbe;

void main() {
    // Probe index from the flat vertex stream (no gl_InstanceIndex anywhere).
    vProbe = uint(aProbe);
    vWorld = aPos;
    vNormal = aNormal;
    // Hide probes within 2 m of the camera: the camera sits inside the probe
    // lattice, and the nearest 1.2 m spheres otherwise fill the whole screen.
    if (distance(vWorld, pc.cam_pos.xyz) < 2.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // outside the clip volume
        vClip = gl_Position;
        return;
    }
    vClip = pc.view_proj * vec4(aPos, 1.0);
    gl_Position = vClip;
}
