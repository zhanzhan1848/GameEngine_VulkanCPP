/**
 * @file VulkanPipeline.cpp
 * @brief VulkanPipeline 实现 — Graphics + Compute
 * @details Graphics:VkPipelineShaderStageCreateInfo + VkPipelineVertexInputState +
 *          VkPipelineInputAssemblyState + VkPipelineViewportState + VkPipelineRasterizationState +
 *          VkPipelineMultisampleState + VkPipelineColorBlendState + VkPipelineDepthStencilState。
 *          dynamic state:viewport/scissor 在 CommandBuffer record 时设置。
 *          RenderPass:Phase 4 用 VkRenderPass 兼容性,triangle 测试传入创建好的 RenderPassHandle。
 *          (Phase 5+ 改用 VK_KHR_dynamic_rendering 避免 RenderPass)
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanPipeline.h"
#include "VulkanDevice.h"
#include "VulkanShader.h"
#include "VulkanPipelineLayout.h"
#include "VulkanMath.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <vector>

namespace primal::graphics::rhi {

namespace {

// ============================================================================
// 枚举映射
// ============================================================================
VkPrimitiveTopology ToVkTopology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::PointList:        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PrimitiveTopology::LineList:         return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::LineStrip:        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::TriangleList:     return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip:    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::LineListAdj:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case PrimitiveTopology::LineStripAdj:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case PrimitiveTopology::TriangleListAdj:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case PrimitiveTopology::TriangleStripAdj: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        default:                                  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkPolygonMode ToVkPolygonMode(FillMode f) {
    switch (f) {
        case FillMode::Wireframe: return VK_POLYGON_MODE_LINE;
        case FillMode::Solid:
        default:                  return VK_POLYGON_MODE_FILL;
    }
}

VkCullModeFlags ToVkCullMode(CullMode c) {
    switch (c) {
        case CullMode::None:  return VK_CULL_MODE_NONE;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
        default:              return VK_CULL_MODE_NONE;
    }
}

VkCompareOp ToVkCompareOp(ComparisonFunc c) {
    switch (c) {
        case ComparisonFunc::Never:        return VK_COMPARE_OP_NEVER;
        case ComparisonFunc::Less:         return VK_COMPARE_OP_LESS;
        case ComparisonFunc::Equal:        return VK_COMPARE_OP_EQUAL;
        case ComparisonFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case ComparisonFunc::Greater:      return VK_COMPARE_OP_GREATER;
        case ComparisonFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case ComparisonFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case ComparisonFunc::Always:       return VK_COMPARE_OP_ALWAYS;
        default:                           return VK_COMPARE_OP_LESS;
    }
}

VkStencilOp ToVkStencilOp(StencilOp s) {
    switch (s) {
        case StencilOp::Keep:    return VK_STENCIL_OP_KEEP;
        case StencilOp::Zero:    return VK_STENCIL_OP_ZERO;
        case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
        case StencilOp::IncSat:  return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case StencilOp::DecSat:  return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case StencilOp::Invert:  return VK_STENCIL_OP_INVERT;
        case StencilOp::Inc:     return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case StencilOp::Dec:     return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default:                 return VK_STENCIL_OP_KEEP;
    }
}

VkBlendFactor ToVkBlendFactor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:           return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:            return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:       return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::InvSrcColor:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha:       return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DestAlpha:      return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::InvDestAlpha:   return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::DestColor:      return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::InvDestColor:   return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlphaSat:    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case BlendFactor::BlendFactor:    return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BlendFactor::InvBlendFactor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::Src1Color:      return VK_BLEND_FACTOR_SRC1_COLOR;
        case BlendFactor::InvSrc1Color:   return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case BlendFactor::Src1Alpha:      return VK_BLEND_FACTOR_SRC1_ALPHA;
        default:                          return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp ToVkBlendOp(BlendOp o) {
    switch (o) {
        case BlendOp::Add:         return VK_BLEND_OP_ADD;
        case BlendOp::Subtract:    return VK_BLEND_OP_SUBTRACT;
        case BlendOp::RevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min:         return VK_BLEND_OP_MIN;
        case BlendOp::Max:         return VK_BLEND_OP_MAX;
        default:                   return VK_BLEND_OP_ADD;
    }
}

VkFormat ToVkVertexFormat(DataFormat f) {
    // 复用 VulkanMath.h
    return primal::graphics::rhi::vulkan::ToVkFormat(f);
}

} // anonymous namespace

// ============================================================================
// Ctor / Dtor
// ============================================================================
VulkanPipeline::VulkanPipeline(VulkanDevice& device) : device_(device) {}

VulkanPipeline::VulkanPipeline(VulkanPipeline&& other) noexcept
    : device_(other.device_),
      handle_(other.handle_),
      isCompute_(other.isCompute_),
      graphicsDesc_(std::move(other.graphicsDesc_)),
      computeDesc_(std::move(other.computeDesc_)),
      pipeline_(other.pipeline_),
      ownedLayout_(other.ownedLayout_) {
    other.pipeline_ = VK_NULL_HANDLE;
    other.ownedLayout_ = VK_NULL_HANDLE;
    other.handle_ = handles::INVALID_PIPELINE;
}

VulkanPipeline& VulkanPipeline::operator=(VulkanPipeline&& other) noexcept {
    if (this != &other) {
        Destroy();
        handle_ = other.handle_;
        isCompute_ = other.isCompute_;
        graphicsDesc_ = std::move(other.graphicsDesc_);
        computeDesc_ = std::move(other.computeDesc_);
        pipeline_ = other.pipeline_;
        ownedLayout_ = other.ownedLayout_;
        other.pipeline_ = VK_NULL_HANDLE;
        other.ownedLayout_ = VK_NULL_HANDLE;
        other.handle_ = handles::INVALID_PIPELINE;
    }
    return *this;
}

VulkanPipeline::~VulkanPipeline() { Destroy(); }

// ============================================================================
// Graphics pipeline Initialize
// ============================================================================
bool VulkanPipeline::Initialize(const GraphicsPipelineDesc& desc) {
    graphicsDesc_ = desc;
    isCompute_ = false;

    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkDevice dev = vk.GetNativeDevice();

    // === 1. Shader stages ===
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    auto addStage = [&](ShaderHandle h, VkShaderStageFlagBits stage) {
        if (h == handles::INVALID_SHADER) return;
        VulkanShader* sh = vk.GetShader(h);
        if (!sh || !sh->GetNativeModule()) return;
        VkPipelineShaderStageCreateInfo s{};
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage = stage;
        s.module = sh->GetNativeModule();
        s.pName = sh->GetEntryPoint();
        stages.push_back(s);
    };
    addStage(desc.vertexShader,  VK_SHADER_STAGE_VERTEX_BIT);
    addStage(desc.pixelShader,   VK_SHADER_STAGE_FRAGMENT_BIT);
    addStage(desc.geometryShader,VK_SHADER_STAGE_GEOMETRY_BIT);
    addStage(desc.hullShader,    VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
    addStage(desc.domainShader,  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);

    if (stages.empty()) {
        std::cerr << "[VulkanPipeline] No valid shader stages" << std::endl;
        return false;
    }

    // === 2. Vertex input state ===
    std::vector<VkVertexInputBindingDescription> vbDescs;
    std::vector<VkVertexInputAttributeDescription> attrDescs;
    vbDescs.reserve(desc.vertexBindings.size());
    attrDescs.reserve(desc.vertexAttributes.size());

    for (const auto& vb : desc.vertexBindings) {
        VkVertexInputBindingDescription b{};
        b.binding = vb.binding;
        b.stride  = vb.stride;
        b.inputRate = vb.perVertex ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE;
        vbDescs.push_back(b);
    }
    for (const auto& a : desc.vertexAttributes) {
        VkVertexInputAttributeDescription ad{};
        ad.location = a.location;
        ad.binding  = a.binding;
        ad.offset   = a.offset;
        ad.format   = ToVkVertexFormat(a.format);
        attrDescs.push_back(ad);
    }

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount   = static_cast<u32>(vbDescs.size());
    vis.pVertexBindingDescriptions      = vbDescs.data();
    vis.vertexAttributeDescriptionCount = static_cast<u32>(attrDescs.size());
    vis.pVertexAttributeDescriptions    = attrDescs.data();

    // === 3. Input assembly ===
    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = ToVkTopology(desc.topology);
    ias.primitiveRestartEnable = VK_FALSE;

    // === 4. Viewport / scissor (dynamic) ===
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount  = 1;
    // 实际 viewport/scissor 通过 vkCmdSetViewport/Scissor dynamic 设置

    // === 5. Rasterization ===
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = (desc.depthBiasClamp != 0.0f) ? VK_TRUE : VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.polygonMode = ToVkPolygonMode(desc.fillMode);
    rs.cullMode     = ToVkCullMode(desc.cullMode);
    // naga (WGSL→SPIR-V) negates gl_Position.y to bridge WebGPU/Metal's
    // Y-up NDC into Vulkan's Y-down — which inverts the rasterized winding.
    // With CCW every triangle reads as back-facing: back-face culling eats
    // geometry, and GPUDrivenDraw's `if (!is_front) N = -N` flips ALL world
    // normals (deferred NdotL ≈ 0 → flat gray). CW matches the actual winding.
    rs.frontFace    = VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = (desc.depthBias != 0.0f || desc.slopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = desc.depthBias;
    rs.depthBiasClamp          = desc.depthBiasClamp;
    rs.depthBiasSlopeFactor    = desc.slopeScaledDepthBias;
    rs.lineWidth = 1.0f;

    // === 6. Multisample ===
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable  = VK_FALSE;
    ms.minSampleShading     = 1.0f;

    // === 7. Depth / stencil ===
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable       = desc.enableDepthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable      = desc.enableDepthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp        = ToVkCompareOp(desc.depthFunc);
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable     = desc.enableStencilTest ? VK_TRUE : VK_FALSE;
    ds.front.failOp      = ToVkStencilOp(desc.frontStencil.failOp);
    ds.front.passOp      = ToVkStencilOp(desc.frontStencil.passOp);
    ds.front.depthFailOp = ToVkStencilOp(desc.frontStencil.depthFailOp);
    ds.front.compareOp   = ToVkCompareOp(desc.frontStencil.func);
    ds.front.compareMask = desc.stencilReadMask;
    ds.front.writeMask   = desc.stencilWriteMask;
    ds.front.reference   = 0;
    ds.back = ds.front;
    ds.back.failOp      = ToVkStencilOp(desc.backStencil.failOp);
    ds.back.passOp      = ToVkStencilOp(desc.backStencil.passOp);
    ds.back.depthFailOp = ToVkStencilOp(desc.backStencil.depthFailOp);
    ds.back.compareOp   = ToVkCompareOp(desc.backStencil.func);
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;

    // === 8. Color blend state ===
    // Depth-only pipelines (renderTargetCount == 0) must have attachmentCount=0
    // here, otherwise VkRenderPass creation complains about VK_FORMAT_UNDEFINED
    // color attachment [VUID-VkAttachmentDescription-format-06698].
    std::vector<VkPipelineColorBlendAttachmentState> attachments(
        desc.renderTargetCount);
    for (u32 i = 0; i < attachments.size(); ++i) {
        VkPipelineColorBlendAttachmentState& a = attachments[i];
        a.blendEnable    = desc.enableBlend ? VK_TRUE : VK_FALSE;
        a.srcColorBlendFactor = ToVkBlendFactor(desc.srcColorBlendFactor);
        a.dstColorBlendFactor = ToVkBlendFactor(desc.dstColorBlendFactor);
        a.colorBlendOp        = ToVkBlendOp(desc.colorBlendOp);
        a.srcAlphaBlendFactor = ToVkBlendFactor(desc.srcAlphaBlendFactor);
        a.dstAlphaBlendFactor = ToVkBlendFactor(desc.dstAlphaBlendFactor);
        a.alphaBlendOp        = ToVkBlendOp(desc.alphaBlendOp);
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.logicOpEnable = VK_FALSE;
    cbs.logicOp = VK_LOGIC_OP_COPY;
    cbs.attachmentCount = static_cast<u32>(attachments.size());
    cbs.pAttachments    = attachments.data();
    cbs.blendConstants[0] = desc.blendConstants.x;
    cbs.blendConstants[1] = desc.blendConstants.y;
    cbs.blendConstants[2] = desc.blendConstants.z;
    cbs.blendConstants[3] = desc.blendConstants.w;

    // === 9. Dynamic state ===
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dys{};
    dys.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dys.dynamicStateCount = 2;
    dys.pDynamicStates    = dynStates;

    // === 10. Pipeline layout ===
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (desc.layout != handles::INVALID_PIPELINE_LAYOUT) {
        VulkanPipelineLayout* pl = vk.GetPipelineLayout(desc.layout);
        if (pl) pipelineLayout = pl->GetNativeLayout();
    }
    if (pipelineLayout == VK_NULL_HANDLE) {
        // 没指定 layout,创建一个空 layout(Phase 4 triangle 测试用)
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipelineLayout) != VK_SUCCESS) {
            std::cerr << "[VulkanPipeline] fallback vkCreatePipelineLayout failed" << std::endl;
            return false;
        }
        ownedLayout_ = pipelineLayout;  // 析构时需销毁
    }

    // === 11. RenderPass 兼容性 ===
    // Phase 4 必须传一个 RenderPassHandle 用来 VkRenderPass 兼容性。
    // Pipeline 创建时通过 desc 中提取 format 信息构建一个临时 VkRenderPass。
    // (Phase 5+ 改 VK_KHR_dynamic_rendering 免 RenderPass)
    VkAttachmentReference colorRef{}, depthRef{};
    std::vector<VkAttachmentDescription> attachments2;
    // Depth-only pipelines: renderTargetCount == 0 is valid; produces no
    // color attachments and the subpass references only depth/stencil.
    u32 colorCount = desc.renderTargetCount;
    for (u32 i = 0; i < colorCount; ++i) {
        VkAttachmentDescription a{};
        a.format         = vulkan::ToVkFormat(desc.renderTargetFormats[i]);
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments2.push_back(a);
    }
    std::vector<VkAttachmentReference> colorRefs(colorCount);
    for (u32 i = 0; i < colorCount; ++i) {
        colorRefs[i].attachment = i;
        colorRefs[i].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    bool hasDepth = (desc.depthStencilFormat != DataFormat::Unknown);
    if (hasDepth) {
        VkAttachmentDescription a{};
        a.format         = vulkan::ToVkFormat(desc.depthStencilFormat);
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments2.push_back(a);
        depthRef.attachment = colorCount;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = colorCount;
    subpass.pColorAttachments    = colorRefs.data();
    subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = static_cast<u32>(attachments2.size());
    rpci.pAttachments    = attachments2.data();
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &subpass;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &renderPass) != VK_SUCCESS) {
        std::cerr << "[VulkanPipeline] vkCreateRenderPass (compat) failed" << std::endl;
        return false;
    }

    // === 12. VkGraphicsPipelineCreateInfo ===
    VkGraphicsPipelineCreateInfo gci{};
    gci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gci.stageCount = static_cast<u32>(stages.size());
    gci.pStages    = stages.data();
    gci.pVertexInputState   = &vis;
    gci.pInputAssemblyState = &ias;
    gci.pViewportState      = &vps;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState   = &ms;
    gci.pDepthStencilState  = (hasDepth || desc.enableStencilTest) ? &ds : nullptr;
    gci.pColorBlendState    = &cbs;
    gci.pDynamicState       = &dys;
    gci.layout              = pipelineLayout;
    gci.renderPass          = renderPass;
    gci.subpass             = 0;
    gci.basePipelineHandle  = VK_NULL_HANDLE;
    gci.basePipelineIndex   = -1;

    VkResult res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, nullptr, &pipeline_);

    // RenderPass 仅用于 pipeline 创建时的兼容性,可立即销毁(GC 路径)
    device_.GetGarbageCollector().DeferredDestroy([dev, renderPass]() {
        if (dev != VK_NULL_HANDLE && renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(dev, renderPass, nullptr);
    });

    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanPipeline] vkCreateGraphicsPipelines failed: " << res << std::endl;
        pipeline_ = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

// ============================================================================
// Compute pipeline Initialize
// ============================================================================
bool VulkanPipeline::Initialize(const ComputePipelineDesc& desc) {
    computeDesc_ = desc;
    isCompute_ = true;

    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkDevice dev = vk.GetNativeDevice();

    VulkanShader* sh = (desc.computeShader != handles::INVALID_SHADER) ? vk.GetShader(desc.computeShader) : nullptr;
    if (!sh || !sh->GetNativeModule()) {
        std::cerr << "[VulkanPipeline] Compute: invalid shader" << std::endl;
        return false;
    }

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (desc.layout != handles::INVALID_PIPELINE_LAYOUT) {
        VulkanPipelineLayout* pl = vk.GetPipelineLayout(desc.layout);
        if (pl) pipelineLayout = pl->GetNativeLayout();
    }
    if (pipelineLayout == VK_NULL_HANDLE) {
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipelineLayout) != VK_SUCCESS) {
            std::cerr << "[VulkanPipeline] compute fallback vkCreatePipelineLayout failed" << std::endl;
            return false;
        }
        ownedLayout_ = pipelineLayout;
    }

    VkPipelineShaderStageCreateInfo css{};
    css.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    css.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    css.module = sh->GetNativeModule();
    css.pName  = sh->GetEntryPoint();

    VkComputePipelineCreateInfo cci{};
    cci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cci.stage  = css;
    cci.layout = pipelineLayout;

    VkResult res = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cci, nullptr, &pipeline_);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanPipeline] vkCreateComputePipelines failed: " << res << std::endl;
        pipeline_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

VkPipelineLayout VulkanPipeline::GetNativePipelineLayout() const {
    // Phase 4: pipeline 创建时若 desc.layout 为空,创建 fallback;此处无法再拿到。
    // 简化:测试侧需要显式拿 desc.layout 的 VulkanPipelineLayout;若空,则返回 VK_NULL_HANDLE,
    // BindDescriptorSets 时 caller 用 nullptr。
    VulkanDevice& vk = const_cast<VulkanDevice&>(static_cast<const VulkanDevice&>(device_));
    if (isCompute_) {
        if (computeDesc_.layout == handles::INVALID_PIPELINE_LAYOUT) return VK_NULL_HANDLE;
        VulkanPipelineLayout* pl = vk.GetPipelineLayout(computeDesc_.layout);
        return pl ? pl->GetNativeLayout() : VK_NULL_HANDLE;
    }
    if (graphicsDesc_.layout == handles::INVALID_PIPELINE_LAYOUT) return VK_NULL_HANDLE;
    VulkanPipelineLayout* pl = vk.GetPipelineLayout(graphicsDesc_.layout);
    return pl ? pl->GetNativeLayout() : VK_NULL_HANDLE;
}

void VulkanPipeline::Destroy() {
    if (pipeline_ == VK_NULL_HANDLE) return;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VkPipeline p = pipeline_;
    VkPipelineLayout layout = ownedLayout_;
    device_.GetGarbageCollector().DeferredDestroy([dev, p, layout]() {
        if (dev != VK_NULL_HANDLE) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev, p, nullptr);
            if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, layout, nullptr);
        }
    });
    pipeline_ = VK_NULL_HANDLE;
    ownedLayout_ = VK_NULL_HANDLE;
}

bool VulkanPipeline::Recreate() {
    Destroy();
    // 注意:必须先复制再 Initialize,因为 Initialize 内部 `graphicsDesc_ = desc;`
    // 当 desc 就是 graphicsDesc_(从 Recreate 调来)会触发 utl::vector 自赋值断言。
    if (isCompute_) {
        ComputePipelineDesc copy = computeDesc_;
        return Initialize(copy);
    }
    GraphicsPipelineDesc copy = graphicsDesc_;
    return Initialize(copy);
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
