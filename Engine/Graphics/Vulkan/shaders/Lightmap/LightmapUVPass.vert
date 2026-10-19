#version 450 core
// LightmapUV pre-pass 顶点着色器（2026-10-19 计划，M3）。
// Entry point: main（GLSL_NAMES 平行数组编译，vert+frag 对）。
//
// 静态几何按实体直绘（LightmapModule 自管 SSBO + 索引缓冲，与
// GPU-driven 管线零耦合）：顶点拉取世界坐标 + lightmap UV，UV 经实体
// scale/bias 变换后写入 RT —— composite 全屏 pass 只读 RT 即可采样，
// 不需要任何逐实体数据。
//
// bindings:
//   0 = StorageBuffer  positions（vec4 xyz+pad，世界空间，模块自建）
//   1 = StorageBuffer  lightmapUVs（vec2）
// push constants（88B）:
//   mat4 view_proj; uint position_offset; uint uv_offset; vec4 scale_bias;

layout(set = 0, binding = 0) readonly buffer PositionBuffer {
    vec4 positions[];
} posBuf;

layout(set = 0, binding = 1) readonly buffer UvBuffer {
    vec2 uvs[];
} uvBuf;

layout(push_constant) uniform LightmapDrawParams {
    mat4 view_proj;
    uint position_offset;
    uint uv_offset;
    vec4 scale_bias;  // xy = scale, zw = bias（atlas 归一化）
} draw;

layout(location = 0) out vec2 vAtlasUV;

void main() {
    const vec3 worldPos = posBuf.positions[draw.position_offset + gl_VertexIndex].xyz;
    gl_Position = draw.view_proj * vec4(worldPos, 1.0);
    const vec2 lmUv = uvBuf.uvs[draw.uv_offset + gl_VertexIndex];
    vAtlasUV = lmUv * draw.scale_bias.xy + draw.scale_bias.zw;
}
