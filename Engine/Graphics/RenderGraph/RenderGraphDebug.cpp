#include "RenderGraphDebug.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/Utils/ShaderLoader.h"
#include "Input/Input.h"
#include <iostream>

#include <algorithm>
#include <queue>
#include <map>

namespace primal::graphics::rendergraph {

static rhi::ShaderHandle CreateShaderFromSource(rhi::RHIDeviceBase& device, const std::string& source, rhi::ShaderStage stage, const char* entryPoint) {
    return device.CreateShader(source.data(), source.size(), stage, entryPoint);
}

struct DebugUniforms {
    math::v2 scale;
    math::v2 offset;
    math::v2 screenSize;
};

RenderGraphDebug::RenderGraphDebug(rhi::RHIDeviceBase& device, const std::string& shaderPath) 
    : device_(device), shaderPath_(shaderPath) {
    CreatePipeline();
}

RenderGraphDebug::~RenderGraphDebug() {
    // Only cleanup if device is still valid
    // Note: In some shutdown scenarios, the device might be destroyed before this destructor is called
    // (e.g. static global destruction order).
    // However, RHIDeviceBase is likely a managed pointer or reference.
    // If device_ is a reference, we can't easily check if it's dangling.
    // But RenderGraphDebug is usually owned by RenderGraph or DebugPass which should respect device lifetime.
    
    // Safety check: try to catch if device is already invalid/destroyed.
    // Assuming IsValid() is reliable.
    if (device_.IsValid()) {
        if (pipeline_ != rhi::handles::INVALID_PIPELINE) {
            device_.DestroyPipeline(pipeline_);
            pipeline_ = rhi::handles::INVALID_PIPELINE;
        }
        if (vertexBuffer_ != rhi::handles::INVALID_RESOURCE) {
            device_.DestroyBuffer(vertexBuffer_);
            vertexBuffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (uniformBuffer_ != rhi::handles::INVALID_RESOURCE) {
            device_.DestroyBuffer(uniformBuffer_);
            uniformBuffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (textureVertexBuffer_ != rhi::handles::INVALID_RESOURCE) {
            device_.DestroyBuffer(textureVertexBuffer_);
            textureVertexBuffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (textureUniformBuffer_ != rhi::handles::INVALID_RESOURCE) {
            device_.DestroyBuffer(textureUniformBuffer_);
            textureUniformBuffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (texturePipeline_ != rhi::handles::INVALID_PIPELINE) {
            device_.DestroyPipeline(texturePipeline_);
            texturePipeline_ = rhi::handles::INVALID_PIPELINE;
        }
        if (texturePipelineLayout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
            device_.DestroyPipelineLayout(texturePipelineLayout_);
            texturePipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
        }
        if (textureSetLayout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            device_.DestroyDescriptorSetLayout(textureSetLayout_);
            textureSetLayout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        }
        if (textureSampler_ != rhi::handles::INVALID_SAMPLER) {
            device_.DestroySampler(textureSampler_);
            textureSampler_ = rhi::handles::INVALID_SAMPLER;
        }
        for (auto set : textureSets_) {
            if (set != rhi::handles::INVALID_DESCRIPTOR_SET) {
                device_.DestroyDescriptorSet(set);
            }
        }
        textureSets_.clear();
    }
}

void RenderGraphDebug::CreatePipeline() {
    using namespace primal::graphics::utils;
    
    std::cout << "Loading shader from: " << shaderPath_ << std::endl;
    std::string source = ShaderLoader::LoadShaderSource(shaderPath_);
    if (source.empty()) {
        // Fallback to absolute path
        std::string absolutePath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + shaderPath_;
        std::cout << "Retrying with absolute path: " << absolutePath << std::endl;
        source = ShaderLoader::LoadShaderSource(absolutePath);
    }
    
    if (source.empty()) {
        std::cerr << "Failed to load shader source!" << std::endl;
        return;
    }
    std::cout << "Shader loaded successfully. Size: " << source.size() << std::endl;

    // 1. Graph Pipeline
    {
        rhi::GraphicsPipelineDesc desc;
        desc.vertexShader = CreateShaderFromSource(device_, source, rhi::ShaderStage::Vertex, "debug_vs");
        desc.pixelShader = CreateShaderFromSource(device_, source, rhi::ShaderStage::Pixel, "debug_fs");
        
        desc.vertexAttributes.resize(2);
        desc.vertexAttributes[0] = {0, 0, rhi::DataFormat::RG32_Float, 0}; // Position
        desc.vertexAttributes[1] = {1, 0, rhi::DataFormat::RGBA32_Float, 8}; // Color
        
        desc.vertexBindings.resize(1);
        desc.vertexBindings[0] = {0, sizeof(Vertex), true};
        
        desc.enableBlend = true;
        desc.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = rhi::BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = rhi::BlendOp::Add;
        desc.srcAlphaBlendFactor = rhi::BlendFactor::One;
        desc.dstAlphaBlendFactor = rhi::BlendFactor::Zero;
        desc.alphaBlendOp = rhi::BlendOp::Add;
        
        desc.renderTargetCount = 1;
        desc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm;
        
        desc.topology = rhi::PrimitiveTopology::TriangleList;
        desc.cullMode = rhi::CullMode::None; // Disable culling to avoid flip issues
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        
        pipeline_ = device_.CreateGraphicsPipeline(desc);
    }

    // 2. Texture Pipeline
    {
        // Sampler
        rhi::SamplerDesc samplerDesc;
        samplerDesc.minFilter = rhi::FilterMode::Linear;
        samplerDesc.magFilter = rhi::FilterMode::Linear;
        samplerDesc.addressU = rhi::TextureAddressMode::Clamp;
        samplerDesc.addressV = rhi::TextureAddressMode::Clamp;
        samplerDesc.addressW = rhi::TextureAddressMode::Clamp;
        textureSampler_ = device_.CreateSampler(samplerDesc);

        // Descriptor Set Layout
        rhi::DescriptorSetLayoutBinding binding;
        binding.binding = 0;
        binding.descriptorType = rhi::DescriptorType::CombinedImageSampler;
        binding.descriptorCount = 1;
        binding.stageFlags = rhi::ShaderStage::Pixel;
        binding.immutableSamplers = nullptr; // We will update it manually

        rhi::DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindingCount = 1;
        layoutDesc.bindings = &binding;
        textureSetLayout_ = device_.CreateDescriptorSetLayout(layoutDesc);

        // Pipeline Layout
        rhi::PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &textureSetLayout_;
        texturePipelineLayout_ = device_.CreatePipelineLayout(plDesc);

        // Pipeline
        rhi::GraphicsPipelineDesc desc;
        desc.vertexShader = CreateShaderFromSource(device_, source, rhi::ShaderStage::Vertex, "debug_texture_vs");
        desc.pixelShader = CreateShaderFromSource(device_, source, rhi::ShaderStage::Pixel, "debug_texture_fs");
        
        desc.vertexAttributes.resize(3);
        desc.vertexAttributes[0] = {0, 0, rhi::DataFormat::RG32_Float, 0}; // Position
        desc.vertexAttributes[1] = {1, 0, rhi::DataFormat::RG32_Float, 8}; // UV
        desc.vertexAttributes[2] = {2, 0, rhi::DataFormat::R32_Float, 16}; // Type
        
        desc.vertexBindings.resize(1);
        desc.vertexBindings[0] = {0, sizeof(TextureVertex), true};
        
        desc.enableBlend = true;
        desc.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = rhi::BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = rhi::BlendOp::Add;
        desc.srcAlphaBlendFactor = rhi::BlendFactor::One;
        desc.dstAlphaBlendFactor = rhi::BlendFactor::Zero;
        desc.alphaBlendOp = rhi::BlendOp::Add;
        
        desc.renderTargetCount = 1;
        desc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm;
        
        desc.topology = rhi::PrimitiveTopology::TriangleList;
        desc.layout = texturePipelineLayout_;
        desc.cullMode = rhi::CullMode::None;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        
        texturePipeline_ = device_.CreateGraphicsPipeline(desc);
    }
}

void RenderGraphDebug::Update(float deltaTime) {
    using namespace primal::input;
    input_value val;

    // Toggle: F1
    get(input_source::keyboard, input_code::key_f1, val);
    bool is_f1_down = val.current.x > 0.0f;
    
    if (is_f1_down && !f1_pressed_) {
        ToggleEnabled();
        f1_pressed_ = true;
    } else if (!is_f1_down) {
        f1_pressed_ = false;
    }

    if (!enabled_) return;
    
    // Pan: WASD / Arrow Keys
    float speed = 500.0f * deltaTime;
    
    get(input_source::keyboard, input_code::key_left, val);
    if (val.current.x > 0) offset_.x += speed;
    
    get(input_source::keyboard, input_code::key_right, val);
    if (val.current.x > 0) offset_.x -= speed;
    
    get(input_source::keyboard, input_code::key_up, val);
    if (val.current.x > 0) offset_.y += speed;
    
    get(input_source::keyboard, input_code::key_down, val);
    if (val.current.x > 0) offset_.y -= speed;
    
    // Zoom: +/-
    get(input_source::keyboard, input_code::key_plus, val); // +
    if (val.current.x > 0) scale_ *= 1.01f;
    
    get(input_source::keyboard, input_code::key_minus, val); // -
    if (val.current.x > 0) scale_ *= 0.99f;
}

void RenderGraphDebug::BuildGraphMesh(const RenderGraph& graph) {
    vertices_.clear();
    layoutCache_.clear();
    
    const auto& passes = graph.GetPasses();
    if (passes.empty()) return;

    // 1. Calculate Ranks (Topological Sort / Longest Path)
    std::unordered_map<RenderGraphPass*, int> ranks;
    std::unordered_map<RenderGraphPass*, utl::vector<RenderGraphPass*>> adj;
    std::unordered_map<RenderGraphPass*, int> inDegree;
    
    // Build adjacency based on resource dependencies
    // This is simplified; ideally we trace resource producers/consumers
    // For now, let's just use the order in activePasses_ as a hint, or assume sequential for simplicity?
    // No, we should do it properly.
    
    // Map resource to producer pass
    std::unordered_map<RGResourceHandle, RenderGraphPass*> producers;
    for (auto* pass : passes) {
        for (const auto& out : pass->GetOutputs()) {
            producers[out.resource->GetHandle()] = pass;
        }
    }
    
    for (auto* pass : passes) {
        ranks[pass] = 0;
        inDegree[pass] = 0;
        for (const auto& in : pass->GetInputs()) {
            if (producers.count(in.resource->GetHandle())) {
                auto* producer = producers[in.resource->GetHandle()];
                if (producer != pass) {
                    adj[producer].push_back(pass);
                    inDegree[pass]++;
                }
            }
        }
    }
    
    // Topological sort to find ranks
    std::queue<RenderGraphPass*> q;
    for (auto* pass : passes) {
        if (inDegree[pass] == 0) q.push(pass);
    }
    
    while (!q.empty()) {
        auto* u = q.front(); q.pop();
        for (auto* v : adj[u]) {
            ranks[v] = std::max(ranks[v], ranks[u] + 1);
            inDegree[v]--;
            if (inDegree[v] == 0) q.push(v);
        }
    }
    
    // 2. Layout Nodes
    std::map<int, utl::vector<RenderGraphPass*>> layers;
    for (auto* pass : passes) {
        layers[ranks[pass]].push_back(pass);
    }
    
    float xCursor = 50.0f; // Initial margin
    const float nodeWidth = 200.0f; // Increased width
    const float nodeHeight = 60.0f;
    const float xGap = 150.0f; // Increased gap
    const float yGap = 50.0f;
    
    for (auto& [rank, layerPasses] : layers) {
        float yCursor = 0.0f;
        // Center vertically
        float totalHeight = layerPasses.size() * (nodeHeight + yGap) - yGap;
        yCursor = -totalHeight / 2.0f + 100.0f; // Add vertical offset to avoid top edge clipping
        
        for (auto* pass : layerPasses) {
            NodeLayout layout;
            layout.pos = {xCursor, yCursor};
            layout.size = {nodeWidth, nodeHeight};
            
            // Color by category
            switch (pass->GetCategory()) {
                case RGPassCategory::Visibility: layout.color = {0.2f, 0.4f, 0.8f, 1.0f}; break; // Blue
                case RGPassCategory::Depth: layout.color = {0.2f, 0.8f, 0.8f, 1.0f}; break;      // Cyan
                case RGPassCategory::Main: layout.color = {0.2f, 0.8f, 0.2f, 1.0f}; break;       // Green
                case RGPassCategory::Lighting: layout.color = {0.8f, 0.8f, 0.2f, 1.0f}; break;   // Yellow
                case RGPassCategory::PostProcess: layout.color = {0.8f, 0.5f, 0.2f, 1.0f}; break;// Orange
                case RGPassCategory::UI: layout.color = {0.8f, 0.2f, 0.8f, 1.0f}; break;         // Purple
                case RGPassCategory::Present: layout.color = {0.8f, 0.2f, 0.2f, 1.0f}; break;    // Red
                default: layout.color = {0.5f, 0.5f, 0.5f, 1.0f}; break;
            }
            
            layoutCache_[pass->GetName()] = layout;
            yCursor += nodeHeight + yGap;
        }
        xCursor += nodeWidth + xGap;
    }
    
    // 3. Generate Geometry
    auto addQuad = [&](math::v2 p, math::v2 s, math::v4 c) {
        vertices_.push_back({p, c});
        vertices_.push_back({{p.x + s.x, p.y}, c});
        vertices_.push_back({{p.x, p.y + s.y}, c});
        
        vertices_.push_back({{p.x + s.x, p.y}, c});
        vertices_.push_back({{p.x + s.x, p.y + s.y}, c});
        vertices_.push_back({{p.x, p.y + s.y}, c});
    };
    
    auto addLine = [&](math::v2 p1, math::v2 p2, math::v4 c, float thickness) {
        math::v2 dir = p2 - p1;
        float len = sqrt(dir.x*dir.x + dir.y*dir.y);
        if (len < 0.001f) return;
        dir = dir * (1.0f / len);
        math::v2 perp = {-dir.y, dir.x};
        perp = perp * (thickness * 0.5f);
        
        vertices_.push_back({p1 - perp, c});
        vertices_.push_back({p1 + perp, c});
        vertices_.push_back({p2 - perp, c});
        
        vertices_.push_back({p1 + perp, c});
        vertices_.push_back({p2 + perp, c});
        vertices_.push_back({p2 - perp, c});
    };
    
    // Draw Nodes
    for (auto& [name, layout] : layoutCache_) {
        addQuad(layout.pos, layout.size, layout.color);
        // Selection highlight
        addQuad(layout.pos, math::v2{layout.size.x, 2.0f}, math::v4{1.0f, 1.0f, 1.0f, 1.0f});
        addQuad(layout.pos, math::v2{2.0f, layout.size.y}, math::v4{1.0f, 1.0f, 1.0f, 1.0f});
        addQuad(math::v2{layout.pos.x + layout.size.x - 2.0f, layout.pos.y}, math::v2{2.0f, layout.size.y}, math::v4{1.0f, 1.0f, 1.0f, 1.0f});
        addQuad(math::v2{layout.pos.x, layout.pos.y + layout.size.y - 2.0f}, math::v2{layout.size.x, 2.0f}, math::v4{1.0f, 1.0f, 1.0f, 1.0f});
    }
    
    // Draw Edges
    for (auto* pass : passes) {
        if (!layoutCache_.count(pass->GetName())) continue;
        auto& targetLayout = layoutCache_[pass->GetName()];
        math::v2 targetPos = targetLayout.pos + math::v2{0.0f, targetLayout.size.y * 0.5f}; // Left-Middle
        
        for (const auto& in : pass->GetInputs()) {
            if (producers.count(in.resource->GetHandle())) {
                auto* producer = producers[in.resource->GetHandle()];
                if (producer != pass && layoutCache_.count(producer->GetName())) {
                    auto& sourceLayout = layoutCache_[producer->GetName()];
                    math::v2 sourcePos = sourceLayout.pos + math::v2{sourceLayout.size.x, sourceLayout.size.y * 0.5f}; // Right-Middle
                    
                    addLine(sourcePos, targetPos, math::v4{0.8f, 0.8f, 0.8f, 1.0f}, 2.0f);
                }
            }
        }
    }
}

void RenderGraphDebug::Draw(rhi::RHICommandBuffer* cmdBuffer, const RenderGraph& graph, uint32_t width, uint32_t height) {
    if (!enabled_) {
        // std::cout << "RenderGraphDebug disabled." << std::endl; // Optional spam
        return;
    }
    if (pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "RenderGraphDebug: Invalid pipeline!" << std::endl;
        return;
    }
    
    // Always rebuild for now (dynamic graph support)
    BuildGraphMesh(graph);
    
    std::cout << "RenderGraphDebug: Drawing " << vertices_.size() << " vertices. Passes: " << graph.GetPasses().size() 
              << " Screen: " << width << "x" << height 
              << " Scale: " << scale_ << " Offset: " << offset_.x << "," << offset_.y << std::endl;

    // Set Viewport and Scissor
    rhi::ViewportDesc vp;
    vp.topLeft = math::v2{0.0f, 0.0f};
    vp.size = {static_cast<float>(width), static_cast<float>(height)};
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmdBuffer->SetViewport(vp);

    rhi::Rect scissor;
    scissor.offset = {0, 0};
    scissor.extent = {width, height};
    cmdBuffer->SetScissor(scissor);

    if (!vertices_.empty()) {
        // Create/Update Vertex Buffer
        size_t requiredSize = vertices_.size() * sizeof(Vertex);
        
        if (vertexBuffer_ == rhi::handles::INVALID_RESOURCE || vertexBufferSize_ < requiredSize) {
             if (vertexBuffer_ != rhi::handles::INVALID_RESOURCE) {
                device_.DestroyBuffer(vertexBuffer_);
            }
            rhi::BufferDesc vDesc;
            vDesc.size = requiredSize;
            vDesc.type = rhi::BufferType::Vertex;
            vDesc.usage = rhi::GPUMemoryUsage::Dynamic;
            vDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic; // Dynamic
            vertexBuffer_ = device_.CreateBuffer(vDesc);
            vertexBufferSize_ = requiredSize;
        }
        
        void* data = device_.MapBuffer(vertexBuffer_);
        if (data) {
            memcpy(data, vertices_.data(), requiredSize);
            device_.UnmapBuffer(vertexBuffer_);
        }
        
        // Draw
        cmdBuffer->BindGraphicsPipeline(pipeline_);
        
        DebugUniforms uniforms;
        uniforms.scale = {scale_, scale_};
        uniforms.offset = offset_;
        uniforms.screenSize = {static_cast<float>(width), static_cast<float>(height)};
        
        // Uniform Buffer
        if (uniformBuffer_ == rhi::handles::INVALID_RESOURCE) {
            rhi::BufferDesc uDesc;
            uDesc.size = sizeof(DebugUniforms);
            uDesc.type = rhi::BufferType::Constant;
            uDesc.usage = rhi::GPUMemoryUsage::Dynamic;
            uDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
            uniformBuffer_ = device_.CreateBuffer(uDesc);
        }
        
        void* uData = device_.MapBuffer(uniformBuffer_);
        if (uData) {
            memcpy(uData, &uniforms, sizeof(DebugUniforms));
            device_.UnmapBuffer(uniformBuffer_);
        }
        
        rhi::ResourceHandle vBuffers[] = {vertexBuffer_, uniformBuffer_};
        uint64_t offsets[] = {0, 0};
        cmdBuffer->BindVertexBuffers(0, 2, vBuffers, offsets);
        
        cmdBuffer->Draw(static_cast<uint32_t>(vertices_.size()), 0, 1, 0);
    } 

    // ---------------------------------------------------------
    // Draw Texture Previews
    // ---------------------------------------------------------
    if (!debugResources_.empty() && texturePipeline_ != rhi::handles::INVALID_PIPELINE) {
        
        // Ensure descriptor sets are allocated
        if (textureSets_.size() != debugResources_.size()) {
            // Resize pool
            for (auto set : textureSets_) {
                if (set != rhi::handles::INVALID_DESCRIPTOR_SET) device_.DestroyDescriptorSet(set);
            }
            textureSets_.clear();
            textureSets_.resize(debugResources_.size(), rhi::handles::INVALID_DESCRIPTOR_SET);
            
            for (size_t i = 0; i < debugResources_.size(); ++i) {
                rhi::DescriptorSetDesc setDesc;
                setDesc.layout = textureSetLayout_;
                textureSets_[i] = device_.CreateDescriptorSet(setDesc);
            }
        }
        
        // Update Descriptor Sets
        // NOTE: This might be expensive every frame, but physical handles might change (transient resources).
        for (size_t i = 0; i < debugResources_.size(); ++i) {
            auto* resource = debugResources_[i].second;
            if (!resource) continue;
            
            // Only support textures for now
            if (resource->GetType() != RGResourceType::Texture) continue;
            
            rhi::ResourceHandle texHandle = resource->GetPhysicalHandle();
            if (texHandle == rhi::handles::INVALID_RESOURCE) continue; // Not allocated yet or invalid
            
            rhi::DescriptorImageInfo imageInfo;
            imageInfo.imageView = texHandle; 
            imageInfo.sampler = textureSampler_;
            imageInfo.imageLayout = rhi::ResourceState::ShaderResource;
            
            rhi::WriteDescriptorSet write;
            write.dstSet = textureSets_[i];
            write.dstBinding = 0;
            write.descriptorType = rhi::DescriptorType::CombinedImageSampler;
            write.descriptorCount = 1;
            write.imageInfo = &imageInfo;
            
            device_.UpdateDescriptorSets(1, &write);
        }
        
        // Prepare Texture Quads
        utl::vector<TextureVertex> texVertices;
        float previewSize = 200.0f;
        float gap = 20.0f;
        float startX = 20.0f;
        float startY = height - previewSize - 20.0f;
        
        // We will collect valid textures to draw
        std::vector<size_t> validIndices;

        for (size_t i = 0; i < debugResources_.size(); ++i) {
            auto* resource = debugResources_[i].second;
            if (!resource) continue;
            if (resource->GetType() != RGResourceType::Texture) continue;
            if (textureSets_[i] == rhi::handles::INVALID_DESCRIPTOR_SET) continue;
            if (resource->GetPhysicalHandle() == rhi::handles::INVALID_RESOURCE) continue;

            validIndices.push_back(i);

            float x = startX + validIndices.size() * (previewSize + gap) - (previewSize + gap); // 0-based index
            float y = startY;
            
            math::v2 p = {x, y};
            math::v2 s = {previewSize, previewSize};
            
            // Determine type
            float type = 0.0f;
            if (resource->GetType() == RGResourceType::Texture) {
                auto* tex = static_cast<RenderGraphTexture*>(resource);
                rhi::DataFormat fmt = tex->GetDesc().format;
                if (fmt == rhi::DataFormat::D32_Float || fmt == rhi::DataFormat::D16_UNorm || fmt == rhi::DataFormat::D24_UNorm_S8_UInt) {
                    type = 1.0f; // Depth
                } else if (fmt == rhi::DataFormat::RG32_Float || fmt == rhi::DataFormat::RG16_Float) {
                    type = 2.0f; // Moments
                }
                
                // Override type based on name if available
                if (debugResources_[i].first == "Normal") {
                    type = 3.0f; // Normal
                } else if (debugResources_[i].first == "WorldPos") {
                    type = 4.0f; // WorldPos
                } else if (debugResources_[i].first == "UV") {
                    type = 5.0f; // UV
                }
            }

            texVertices.push_back({{p.x, p.y}, {0, 0}, type});
            texVertices.push_back({{p.x + s.x, p.y}, {1, 0}, type});
            texVertices.push_back({{p.x, p.y + s.y}, {0, 1}, type});
            
            texVertices.push_back({{p.x + s.x, p.y}, {1, 0}, type});
            texVertices.push_back({{p.x + s.x, p.y + s.y}, {1, 1}, type});
            texVertices.push_back({{p.x, p.y + s.y}, {0, 1}, type});
        }
        
        if (!texVertices.empty()) {
             // Create Vertex Buffer
            size_t requiredSize = texVertices.size() * sizeof(TextureVertex);
            
            if (textureVertexBuffer_ == rhi::handles::INVALID_RESOURCE || textureVertexBufferSize_ < requiredSize) {
                if (textureVertexBuffer_ != rhi::handles::INVALID_RESOURCE) {
                    device_.DestroyBuffer(textureVertexBuffer_);
                }
                rhi::BufferDesc tvDesc;
                tvDesc.size = requiredSize;
                tvDesc.type = rhi::BufferType::Vertex;
                tvDesc.usage = rhi::GPUMemoryUsage::Dynamic;
                tvDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                textureVertexBuffer_ = device_.CreateBuffer(tvDesc);
                textureVertexBufferSize_ = requiredSize;
            }
            
            void* tData = device_.MapBuffer(textureVertexBuffer_);
            if (tData) {
                memcpy(tData, texVertices.data(), requiredSize);
                device_.UnmapBuffer(textureVertexBuffer_);
            }
            
            // Create HUD Uniforms
            DebugUniforms hudUniforms;
            hudUniforms.scale = {1.0f, 1.0f};
            hudUniforms.offset = {0.0f, 0.0f};
            hudUniforms.screenSize = {static_cast<float>(width), static_cast<float>(height)};
            
            if (textureUniformBuffer_ == rhi::handles::INVALID_RESOURCE) {
                rhi::BufferDesc huDesc;
                huDesc.size = sizeof(DebugUniforms);
                huDesc.type = rhi::BufferType::Constant;
                huDesc.usage = rhi::GPUMemoryUsage::Dynamic;
                huDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                textureUniformBuffer_ = device_.CreateBuffer(huDesc);
            }
            
            void* huData = device_.MapBuffer(textureUniformBuffer_);
            if (huData) {
                memcpy(huData, &hudUniforms, sizeof(DebugUniforms));
                device_.UnmapBuffer(textureUniformBuffer_);
            }
            
            // Bind Pipeline
            cmdBuffer->BindGraphicsPipeline(texturePipeline_);

            // Bind Buffers
            rhi::ResourceHandle bindings[] = {textureVertexBuffer_, textureUniformBuffer_};
            uint64_t tOffsets[] = {0, 0};
            cmdBuffer->BindVertexBuffers(0, 2, bindings, tOffsets);
            
            // Draw Calls
            int vertexOffset = 0;
            std::cout << "[RenderGraphDebug] Drawing " << validIndices.size() << " texture previews." << std::endl;
            for (size_t idx : validIndices) {
                // Bind Descriptor Set
                rhi::DescriptorSetHandle sets[] = {textureSets_[idx]};
                cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, texturePipelineLayout_, 0, 1, sets, 0, nullptr);
                
                cmdBuffer->Draw(6, vertexOffset, 1, 0);
                vertexOffset += 6;
            }
        }
    }
}

}
