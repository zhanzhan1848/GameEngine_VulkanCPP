#include <metal_stdlib>
using namespace metal;

// DebugUniforms must match C++ struct (160 bytes)
struct DebugUniforms {
    float4x4 viewProjection;
    float4x4 model;
    uint mesh_id;
    float wireframe_enabled;
    float padding[6];
};

// Meshlet structure matching RHIMeshlet
struct Meshlet {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    float cone_apex[3];
    float cone_axis[3];
    float cone_cutoff;
    float center[3];
    float radius;
    uint padding;
};

struct VertexOut {
    float4 position [[position]];
    float3 color;
    float alpha;
    float3 barycentric;
};

// Hash function for random color based on meshlet ID
float3 hashColor(uint n) {
    // Use prime numbers for better distribution
    float r = fract(sin(float(n) * 12.9898) * 43758.5453);
    float g = fract(sin(float(n + 1) * 78.233) * 43758.5453);
    float b = fract(sin(float(n + 2) * 45.164) * 43758.5453);
    // Ensure colors are bright enough to be visible
    return float3(
        0.3 + 0.7 * r,
        0.3 + 0.7 * g,
        0.3 + 0.7 * b
    );
}

// Meshlet Debug Vertex Shader
// Draws meshlets using instanced rendering:
// - Draw(384, firstVertex, instanceCount, firstInstance)
// - vertexID: 0..383 (covers 128 triangles * 3 vertices)
// - instanceID: meshlet index
vertex VertexOut meshlet_debug_vs(
    uint vertexID [[vertex_id]],
    uint instanceID [[instance_id]],
    constant DebugUniforms& uniforms [[buffer(0)]],
    constant Meshlet* meshlets [[buffer(1)]],
    constant uint* meshletVertices [[buffer(2)]],
    constant uchar* meshletTriangles [[buffer(3)]],
    constant packed_float3* positions [[buffer(4)]]
) {
    VertexOut out;
    
    // Get the meshlet for this instance
    Meshlet meshlet = meshlets[instanceID];
    
    // Get triangle index within this meshlet
    // Each triangle has 3 indices packed as u8
    uint triIndex = vertexID;  // 0..383
    
    // Check if we're within triangle count
    if (triIndex >= meshlet.triangle_count * 3) {
        // This vertex is not part of a valid triangle
        // Project to infinity to discard
        out.position = float4(0, 0, 0, 0); // W=0 puts it at infinity usually, or just clipped
        out.color = float3(0, 0, 0);
        out.alpha = 0.0;
        out.barycentric = float3(0,0,0);
        return out;
    }
    
    // Get the local vertex index (0-126 max) from compressed triangles
    uint localVertIdx = uint(meshletTriangles[meshlet.triangle_offset + triIndex]);
    
    // Get the actual vertex index from meshlet vertices
    uint vertIdx = meshletVertices[meshlet.vertex_offset + localVertIdx];
    
    // Get the position
    float3 pos = positions[vertIdx];
    
    // Transform to clip space
    float4 worldPos = uniforms.model * float4(pos, 1.0);
    out.position = uniforms.viewProjection * worldPos;
    
    // Color based on meshlet ID (instanceID) mixed with Mesh ID
    // This ensures different meshes don't have identical color patterns
    uint seed = instanceID + uniforms.mesh_id * 1024; 
    out.color = hashColor(seed);
    out.alpha = 0.6;  // Semi-transparent
    
    // Barycentric coordinates for wireframe
    uint vertexInTriangle = vertexID % 3;
    if (vertexInTriangle == 0) out.barycentric = float3(1, 0, 0);
    else if (vertexInTriangle == 1) out.barycentric = float3(0, 1, 0);
    else out.barycentric = float3(0, 0, 1);
    
    return out;
}

fragment float4 meshlet_debug_fs(
    VertexOut in [[stage_in]],
    constant DebugUniforms& uniforms [[buffer(0)]]
) {
    float3 color = in.color;
    
    // Wireframe visualization
    if (uniforms.wireframe_enabled > 0.5) {
        // Calculate distance to nearest edge in screen space
        float3 d = fwidth(in.barycentric);
        float3 a3 = smoothstep(float3(0.0), d * 1.5, in.barycentric);
        float minBary = min(min(a3.x, a3.y), a3.z);
        
        // Mix black edge color
        color = mix(float3(0.0), color, minBary);
    }
    
    return float4(color, in.alpha);
}
