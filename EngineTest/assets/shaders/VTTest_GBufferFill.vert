#version 450 core
// VT M2.14 验证用填充顶点着色器:全屏三角形,输出 GBufferVT.frag 的完整
// 顶点接口(location 0-8)。uv 覆盖 [0,1],实例参数给中性值。
layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec3 outWorldTangent;
layout(location = 3) out vec2 outUV;
layout(location = 4) out vec4 outCurrentPos;
layout(location = 5) out vec4 outPreviousPos;
layout(location = 6) out vec4 outInstanceBaseColor;
layout(location = 7) out float outInstanceRoughness;
layout(location = 8) out float outInstanceMetallic;

void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    outUV = pos;
    outWorldPos = vec3(pos, 0.0);
    outWorldNormal = vec3(0.0, 0.0, 1.0);
    outWorldTangent = vec3(1.0, 0.0, 0.0);
    outCurrentPos = gl_Position;
    outPreviousPos = gl_Position;
    outInstanceBaseColor = vec4(1.0);
    outInstanceRoughness = 0.5;
    outInstanceMetallic = 0.0;
}
