#version 450 core

// VT M2.14 — GBuffer 的 Virtual Texture 采样变体。
// 与 GBuffer.frag 输出/输入完全一致;材质纹理采样换成两段式
// (页表 texelFetch + 物理 atlas 采样)+ 片元级 feedback 写入。
//
// Descriptor set layout (matches ForwardSceneRenderer::material_set_vt_layout_):
//   set 1 binding 0 = SampledImage   页表(R32_UINT)
//   set 1 binding 1 = SampledImage   atlas0(albedo 层物理池)
//   set 1 binding 2 = SampledImage   atlas1(normal 层物理池)
//   set 1 binding 3 = SampledImage   atlas2(orm 层物理池)
//   set 1 binding 4 = Sampler
//   set 1 binding 5 = UniformBuffer  VTMaterialParams
//   set 1 binding 6 = StorageBuffer  feedback(片元写)
//
// 页表条目位布局(与 VirtualTextureSystem 一致):
//   bit0 valid / bits[8:19) atlasOriginX / bits[19:30) atlasOriginY
// 页表为 mip-金字塔打包子矩形布局:各 mip 页嵌套于子矩形左上,
// 回退 = 坐标右移。feedback: |4bit vtId|4bit mip|12bit x|12bit y|。

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 1, binding = 0) uniform utexture2D vtPageTable;
layout(set = 1, binding = 1) uniform texture2D atlas0;
layout(set = 1, binding = 2) uniform texture2D atlas1;
layout(set = 1, binding = 3) uniform texture2D atlas2;
layout(set = 1, binding = 4) uniform sampler    vtSampler;

layout(set = 1, binding = 5) uniform VTMaterialParams {
    vec4 p0;   // albedo: x=ptOriginX y=ptOriginY z=pagesTop w=avgColor
    vec4 p1;   // normal 同构
    vec4 p2;   // orm    同构
    vec4 p3;   // x=tileSize y=atlasSizeInv z=uvScaleX w=uvScaleY
    vec4 p4;   // x=vtId0 y=vtId1 z=vtId2 w=feedbackWidth
} vt;

layout(set = 1, binding = 6) buffer VTFeedback {
    uint data[];
} feedback;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec3 inWorldTangent;
layout(location = 3) in vec2 inUV;
layout(location = 4) in vec4 inCurrentPos;
layout(location = 5) in vec4 inPreviousPos;
layout(location = 6) in vec4 inInstanceBaseColor;
layout(location = 7) in float inInstanceRoughness;
layout(location = 8) in float inInstanceMetallic;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outORM;
layout(location = 3) out vec2 outVelocity;

// 两段式采样 + feedback(params = 该层 {originX, originY, pagesTop, avgColor};
// atlasIndex 选择绑定 1/2/3;feedbackSlot 为该层写入的 feedback 下标)
vec4 vtSampleLayer(vec4 lp, uint vtId, int atlasIndex, uint feedbackSlot) {
    float tileSize = vt.p3.x;
    float atlasInv = vt.p3.y;
    uint pagesTop = uint(lp.z);
    uint maxLevel = uint(log2(float(pagesTop)));

    // 导数驱动的 mip 选择(M3: 各向异性 footprint)
    float virtualSize = float(pagesTop) * tileSize;
    vec2 dx = dFdx(inUV * virtualSize);
    vec2 dy = dFdy(inUV * virtualSize);
    float levF = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));
    uint level = uint(clamp(levF, 0.0, float(maxLevel)));
    uint s = max(pagesTop >> level, 1u);
    vec2 f = clamp(inUV, vec2(0.0), vec2(0.99999)) * float(s);
    uvec2 vPage = min(uvec2(f), uvec2(s - 1u));

    // feedback:请求该层当前 mip 的页
    feedback.data[feedbackSlot] = (vtId << 28) | (level << 24)
                                | ((vPage.y & 0xFFFu) << 12) | (vPage.x & 0xFFFu);

    // 页表查找 + 逐级上移回退
    uint entry = 0u;
    for (uint k = 0; (level + k) <= maxLevel; ++k) {
        ivec2 c = ivec2(int(lp.x), int(lp.y)) + ivec2(vPage >> k);
        entry = texelFetch(vtPageTable, c, 0).r;
        if ((entry & 1u) != 0u) break;
    }
    if ((entry & 1u) == 0u) {
        return vec4(lp.w);   // 平均色兜底
    }
    uint ox = (entry >> 8) & 0x7FFu;
    uint oy = (entry >> 19) & 0x7FFu;
    vec2 frac = f - vec2(vPage);
    vec2 pUV = (vec2(float(ox), float(oy)) + 4.0 + frac * tileSize) * atlasInv;
    if (atlasIndex == 0) return texture(sampler2D(atlas0, vtSampler), pUV);
    if (atlasIndex == 1) return texture(sampler2D(atlas1, vtSampler), pUV);
    return texture(sampler2D(atlas2, vtSampler), pUV);
}

void main() {
    // feedback 槽:每像素 3 个(albedo/normal/orm 各一)
    uint px = uint(gl_FragCoord.x) + uint(gl_FragCoord.y) * uint(vt.p4.w);
    vec4 albedoSample = vtSampleLayer(vt.p0, uint(vt.p4.x), 0, px * 3u + 0u);
    vec4 normalSample = vtSampleLayer(vt.p1, uint(vt.p4.y), 1, px * 3u + 1u);
    vec4 ormSample    = vtSampleLayer(vt.p2, uint(vt.p4.z), 2, px * 3u + 2u);

    outAlbedo = vec4(albedoSample.rgb * inInstanceBaseColor.rgb,
                     albedoSample.a * inInstanceBaseColor.a);

    if (length(ormSample.rgb) < 0.01) {
        outORM = vec4(1.0, inInstanceRoughness, inInstanceMetallic, 1.0);
    } else {
        outORM = vec4(ormSample.r, ormSample.g * inInstanceRoughness,
                      ormSample.b * inInstanceMetallic, 1.0);
        outORM.r = max(outORM.r, 0.1);
    }

    if (length(normalSample.rgb) > 0.1) {
        vec3 normal = normalSample.rgb * 2.0 - 1.0;
        vec3 N = normalize(inWorldNormal);
        vec3 T = normalize(inWorldTangent);
        vec3 B = cross(N, T);
        mat3 TBN = mat3(T, B, N);
        outNormal = vec4(normalize(TBN * normal) * 0.5 + 0.5, 1.0);
    } else {
        outNormal = vec4(normalize(inWorldNormal) * 0.5 + 0.5, 1.0);
    }

    vec2 currentNDC = inCurrentPos.xy / inCurrentPos.w;
    vec2 previousNDC = inPreviousPos.xy / inPreviousPos.w;
    outVelocity = (currentNDC - previousNDC) * 0.5;
}
