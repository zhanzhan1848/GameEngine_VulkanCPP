// Shared InstanceData struct for GBuffer technique shaders.
// All technique shaders (Opaque, AlphaClip, Unlit) include this to avoid duplication.

struct InstanceData {
    float4x4 transform;
    float4   baseColor;
    float    roughness;
    float    metallic;
    float    alphaCutoff;
    float    _pad;
};
