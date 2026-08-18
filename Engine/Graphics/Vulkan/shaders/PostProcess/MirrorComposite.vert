#version 450 core
// MirrorComposite.vert — hand-written GLSL. Entry point: main
// (Metal: mirror_composite_vs in Metal/shaders/MirrorComposite.metal).
// Draws the mirror quad in world space through the main camera so the
// fragment stage can project each pixel into the reflection texture.

layout(std140, set = 0, binding = 0) uniform CompositeParams {
    mat4 view_projection;            // main camera VP
    mat4 reflected_view_projection;  // reflected camera VP (for UV projection)
    vec4 plane_position;             // xyz = center, w unused
    vec4 plane_normal;               // xyz = normal
    vec4 plane_axes;                 // xy = half extents (u/v), zw unused
    vec4 camera_position;            // xyz, w = reflectivity
};

layout(location = 0) out vec3 vWorldPos;

const vec2 quadCorners[6] = vec2[6](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2(-1.0,  1.0),
    vec2(-1.0,  1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0)
);

void main() {
    vec2 corner = quadCorners[gl_VertexIndex];

    // Orthonormal basis around the plane normal (matches the C++ basis in
    // PlanarReflectionModule — keep the two in sync).
    vec3 N = normalize(plane_normal.xyz);
    vec3 U = normalize(cross(abs(N.y) < 0.99f ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), N));
    vec3 V = cross(N, U);

    vec3 worldPos = plane_position.xyz +
                    U * (corner.x * plane_axes.x) +
                    V * (corner.y * plane_axes.y);

    vWorldPos = worldPos;
    gl_Position = view_projection * vec4(worldPos, 1.0);
}
