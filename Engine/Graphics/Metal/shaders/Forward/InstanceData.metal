// Shared InstanceData struct for GBuffer technique shaders.
// All technique shaders (Opaque, AlphaClip, Unlit) include this to avoid duplication.

struct InstanceData {
    float4x4 transform;
    float4   baseColor;
    float    roughness;
    float    metallic;
    float    alphaCutoff;
    float    transmission;  // P2-B: per-instance 透光材质掩码（0=石材，>0=薄布料）
};
