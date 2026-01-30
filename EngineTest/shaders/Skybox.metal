
// Skybox Vertex Shader
struct SkyboxVertexOut {
    float4 position [[position]];
    float3 uv;
};

vertex SkyboxVertexOut vertexSkybox(VertexIn in [[stage_in]],
                                    constant Uniforms& uniforms [[buffer(1)]],
                                    constant SceneData& scene [[buffer(2)]]) {
    SkyboxVertexOut out;
    
    // Use raw position as UV (Cubemap direction)
    out.uv = in.position;
    
    // Remove translation from View Matrix
    // ViewProjections[0] is usually Main View
    float4x4 viewProj = uniforms.viewProjections[0];
    
    // We want the skybox to follow the camera but stay at infinity.
    // Standard trick: Model Matrix translates to Camera Pos? 
    // Or just use View Matrix rotation part.
    // Here we use the scene.model (which should be identity or scaling for skybox)
    // But better: Just use local position and assume Skybox Mesh is centered at 0,0,0
    // and we translate it to Camera Position in CPU or Shader.
    
    float4 worldPos = scene.model * float4(in.position, 1.0);
    out.position = viewProj * worldPos;
    
    // Force Z to far plane (1.0 in Metal with 0-1 depth?)
    // Metal NDC Z is 0 to 1. Far plane is 1.
    out.position.z = out.position.w; 
    
    return out;
}

fragment float4 fragmentSkybox(SkyboxVertexOut in [[stage_in]],
                               texturecube<float> skybox [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear);
    return skybox.sample(s, in.uv);
}
