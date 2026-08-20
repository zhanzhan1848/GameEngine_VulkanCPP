#version 450 core
// MeshletDebug.vert — hand-written GLSL port of RHI/Shaders/Debug/MeshletDebug.metal.
// Entry point: main (Metal: meshlet_debug_vs).
//
// Draws meshlets via instanced rendering: Draw(384 verts, meshletCount instances).
// Vertex pulling from the meshlet SSBOs — matches the Metal bindings:
//   binding 0 = DebugUniforms (dynamic UBO, std140)
//   binding 1 = meshlets      (RHIMeshlet, 64B stride, header u32s at +0..15)
//   binding 2 = meshlet vertex indices (u32)
//   binding 3 = meshlet triangle indices (u8 packed in u32)
//   binding 4 = positions (packed_float3, 3 floats per vertex)

layout(std140, set = 0, binding = 0) uniform DebugUniforms {
    mat4 viewProjection;       // offset 0
    mat4 model;                // offset 64
    uint mesh_id;              // offset 128 (union: meshlet_debug)
    float wireframe_enabled;   // offset 132
    vec4 _pad0;                // pad to 160 like the C++ struct
};

struct MeshletHeader {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    vec4 cone0;   // cone_apex[3] + first axis component
    vec4 cone1;
    vec4 cone2;
};

layout(set = 0, binding = 1) readonly buffer MeshletBuffer  { MeshletHeader meshlets[]; };
layout(set = 0, binding = 2) readonly buffer VertexIndices { uint meshletVertices[]; };
layout(set = 0, binding = 3) readonly buffer TriIndices     { uint meshletTrianglesPacked[]; };
layout(set = 0, binding = 4) readonly buffer Positions      { float positions[]; };

layout(location = 0) out vec3 vColor;
layout(location = 1) out float vAlpha;
layout(location = 2) out vec3 vBarycentric;

// Hash function for random color based on meshlet ID (same constants as Metal).
vec3 hashColor(uint n) {
    float r = fract(sin(float(n) * 12.9898) * 43758.5453);
    float g = fract(sin(float(n + 1) * 78.233) * 43758.5453);
    float b = fract(sin(float(n + 2) * 45.164) * 43758.5453);
    return vec3(0.3 + 0.7 * r, 0.3 + 0.7 * g, 0.3 + 0.7 * b);
}

// Extract byte i from the u8-packed triangle index array.
uint readTriangleByte(uint byteIndex) {
    uint packed = meshletTrianglesPacked[byteIndex >> 2u];
    return (packed >> (8u * (byteIndex & 3u))) & 0xFFu;
}

void main() {
    uint vertexID = gl_VertexIndex;
    uint instanceID = gl_InstanceIndex;

    MeshletHeader meshlet = meshlets[instanceID];

    uint triIndex = vertexID;  // 0..383
    if (triIndex >= meshlet.triangle_count * 3u) {
        // Not part of a valid triangle — degenerate output, gets clipped.
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        vColor = vec3(0.0);
        vAlpha = 0.0;
        vBarycentric = vec3(0.0);
        return;
    }

    uint localVertIdx = readTriangleByte(meshlet.triangle_offset + triIndex);
    uint vertIdx = meshletVertices[meshlet.vertex_offset + localVertIdx];

    vec3 pos = vec3(positions[3u * vertIdx + 0u],
                    positions[3u * vertIdx + 1u],
                    positions[3u * vertIdx + 2u]);

    vec4 worldPos = model * vec4(pos, 1.0);
    gl_Position = viewProjection * worldPos;

    // Color from meshlet ID mixed with mesh ID so meshes differ.
    uint seed = instanceID + mesh_id * 1024u;
    vColor = hashColor(seed);
    vAlpha = 0.6;  // semi-transparent

    uint vertexInTriangle = vertexID % 3u;
    if (vertexInTriangle == 0u)      vBarycentric = vec3(1.0, 0.0, 0.0);
    else if (vertexInTriangle == 1u) vBarycentric = vec3(0.0, 1.0, 0.0);
    else                             vBarycentric = vec3(0.0, 0.0, 1.0);
}
