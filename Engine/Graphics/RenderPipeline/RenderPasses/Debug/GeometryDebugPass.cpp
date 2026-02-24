#include "CommonHeaders.h"
#include "GeometryDebugPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Content/ContentToEngine.h"
#include "Graphics/RHI/Core/RHIGpuMesh.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Graphics/RHI/Core/RHIShaderCommon.h"
#include "Graphics/RHI/Core/RHIDescriptorSetLayout.h"
#include "Graphics/RHI/Core/RHIRenderPass.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RenderView.h"
#include <fstream>
#include <vector>
#include <iostream>
#include <set>

namespace primal::graphics {

using namespace rhi;

struct DebugUniforms {
    math::m4x4 viewProjection;
    math::m4x4 model;
    // Extra parameters for different modes
    union {
        struct {
            float slice_depth;
            float mode;
            float padding[2];
        } sdf;
        struct {
            float scale;
            float step; // Added for throttling
            float padding[2];
            uint32_t resolution[4];
        } vf;
        struct {
            float scale;
            float threshold;
            float step; // Added for throttling
            float padding;
            uint32_t resolution[4];
        } voxel;
    };
};

// Ensure DebugUniforms matches shader alignment
static_assert(sizeof(DebugUniforms) == 160, "DebugUniforms size mismatch");

struct GeometryDebugContext {
    PipelineHandle meshlet_pipeline = handles::INVALID_PIPELINE;
    PipelineHandle sdf_pipeline = handles::INVALID_PIPELINE;
    PipelineHandle voxel_pipeline = handles::INVALID_PIPELINE;
    PipelineHandle vector_field_pipeline = handles::INVALID_PIPELINE;
    
    PipelineLayoutHandle meshlet_layout = handles::INVALID_PIPELINE_LAYOUT;
    PipelineLayoutHandle sdf_layout = handles::INVALID_PIPELINE_LAYOUT;
    PipelineLayoutHandle voxel_layout = handles::INVALID_PIPELINE_LAYOUT;
    PipelineLayoutHandle vector_field_layout = handles::INVALID_PIPELINE_LAYOUT;
    
    DescriptorSetLayoutHandle meshlet_set_layout = handles::INVALID_RESOURCE;
    DescriptorSetLayoutHandle sdf_set_layout = handles::INVALID_RESOURCE;
    DescriptorSetLayoutHandle voxel_set_layout = handles::INVALID_RESOURCE;
    DescriptorSetLayoutHandle vector_field_set_layout = handles::INVALID_RESOURCE;
    
    bool initialized = false;
    RenderPassHandle compatibleRenderPass = handles::INVALID_RESOURCE;
    SamplerHandle defaultSampler = handles::INVALID_SAMPLER;
    
    // Shader Paths
    const char* meshlet_vs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/MeshletDebug.metal";
    const char* meshlet_fs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/MeshletDebug.metal";
    
    const char* sdf_vs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/SDFDebug.metal";
    const char* sdf_fs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/SDFDebug.metal";
    
    const char* vf_vs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/VectorFieldDebug.metal";
    const char* vf_fs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/VectorFieldDebug.metal";

    const char* voxel_vs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/VoxelDebug.metal";
    const char* voxel_fs_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/Debug/VoxelDebug.metal";

    // Uniform Buffer Management
    struct FrameData {
        ResourceHandle uniformBuffer = handles::INVALID_RESOURCE;
        void* mappedPtr = nullptr;
        uint32_t capacity = 0;
        uint32_t currentOffset = 0;
    };
    FrameData frames[3];
    uint32_t frameIndex = 0;
    
    // Descriptor Set Cache
    std::unordered_map<uint64_t, DescriptorSetHandle> meshlet_ds_cache[3];
    std::unordered_map<uint64_t, DescriptorSetHandle> sdf_ds_cache[3];
    std::unordered_map<uint64_t, DescriptorSetHandle> vf_ds_cache[3];
    std::unordered_map<uint64_t, DescriptorSetHandle> voxel_ds_cache[3];

    // Helper to load shader data
    std::vector<char> LoadShaderData(const char* path) {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open shader file: " << path << std::endl;
            return {};
        }
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        return buffer;
    }

    void Initialize(RHIDeviceBase& device, RenderPassHandle renderPass) {
        if (initialized) return;

        // Create Default Sampler
        if (defaultSampler == handles::INVALID_SAMPLER) {
            SamplerDesc desc;
            desc.minFilter = FilterMode::Linear;
            desc.magFilter = FilterMode::Linear;
            desc.mipFilter = FilterMode::Linear;
            desc.addressU = TextureAddressMode::Clamp;
            desc.addressV = TextureAddressMode::Clamp;
            desc.addressW = TextureAddressMode::Clamp;
            defaultSampler = device.CreateSampler(desc);
        }

        // 1. Meshlet Pipeline
        {
            // Load Shaders
            std::vector<char> vs_data = LoadShaderData(meshlet_vs_path);
            std::vector<char> fs_data = LoadShaderData(meshlet_fs_path);
            
            ShaderHandle vs = handles::INVALID_SHADER;
            ShaderHandle fs = handles::INVALID_SHADER;

            if (!vs_data.empty()) {
                vs = device.CreateShader(vs_data.data(), vs_data.size(), ShaderStage::Vertex, "meshlet_debug_vs");
            } else {
                std::cerr << "Failed to load Meshlet VS data from: " << meshlet_vs_path << std::endl;
            }
            if (!fs_data.empty()) {
                fs = device.CreateShader(fs_data.data(), fs_data.size(), ShaderStage::Pixel, "meshlet_debug_fs");
            } else {
                std::cerr << "Failed to load Meshlet FS data from: " << meshlet_fs_path << std::endl;
            }
            
            if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                std::cout << "Meshlet Debug Pipeline Created Successfully." << std::endl;
                
                DescriptorSetLayoutBinding bindings[] = {
                    { 0, DescriptorType::UniformBufferDynamic, 1, ShaderStage::Vertex }, // DebugUniforms
                    { 1, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex }, // Meshlets
                    { 2, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex }, // MeshletVertices
                    { 3, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex }, // MeshletTriangles
                    { 4, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex }  // Positions
                };
                
                DescriptorSetLayoutDesc layout_desc;
                layout_desc.bindingCount = 5;
                layout_desc.bindings = bindings;
                
                DescriptorSetLayoutHandle set_layout = device.CreateDescriptorSetLayout(layout_desc);
                meshlet_set_layout = set_layout;
                
                PipelineLayoutDesc pl_desc;
                pl_desc.setLayoutCount = 1;
                pl_desc.setLayouts = &set_layout;
                meshlet_layout = device.CreatePipelineLayout(pl_desc);
                
                GraphicsPipelineDesc p_desc;
                p_desc.vertexShader = vs;
                p_desc.pixelShader = fs;
                p_desc.layout = meshlet_layout;
                p_desc.topology = PrimitiveTopology::TriangleList;
                
                // Formats
                p_desc.renderTargetCount = 1;
                p_desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
                p_desc.depthStencilFormat = DataFormat::D32_Float;
                
                // Enable blending for transparency
                p_desc.enableBlend = true;
                p_desc.srcColorBlendFactor = BlendFactor::SrcAlpha;
                p_desc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
                p_desc.srcAlphaBlendFactor = BlendFactor::One;
                p_desc.dstAlphaBlendFactor = BlendFactor::InvSrcAlpha;
                p_desc.colorBlendOp = BlendOp::Add;
                p_desc.alphaBlendOp = BlendOp::Add;
                
                p_desc.enableDepthTest = true;
                p_desc.enableDepthWrite = false;
                p_desc.depthFunc = ComparisonFunc::Always; // DEBUG
                
                p_desc.cullMode = CullMode::None;
                p_desc.fillMode = FillMode::Solid;
                
                meshlet_pipeline = device.CreateGraphicsPipeline(p_desc);
            } else {
                std::cerr << "Failed to create Meshlet Shaders." << std::endl;
            }
        }
        

        // 2. SDF Pipeline
        {
            // Load Shaders
            std::vector<char> vs_data = LoadShaderData(sdf_vs_path);
            std::vector<char> fs_data = LoadShaderData(sdf_fs_path);
            
            ShaderHandle vs = handles::INVALID_SHADER;
            ShaderHandle fs = handles::INVALID_SHADER;

            if (!vs_data.empty()) {
                vs = device.CreateShader(vs_data.data(), vs_data.size(), ShaderStage::Vertex, "sdf_debug_vs");
            }
            if (!fs_data.empty()) {
                fs = device.CreateShader(fs_data.data(), fs_data.size(), ShaderStage::Pixel, "sdf_slice_debug_fs");
            }
            
            if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                DescriptorSetLayoutBinding bindings[] = {
                    { 0, DescriptorType::SampledImage, 1, ShaderStage::Pixel },      // SDF Texture
                    { 1, DescriptorType::UniformBufferDynamic, 1, ShaderStage::Vertex | ShaderStage::Pixel } // Uniforms (Dynamic)
                };
                
                DescriptorSetLayoutDesc layout_desc;
                layout_desc.bindingCount = 2;
                layout_desc.bindings = bindings;
                
                DescriptorSetLayoutHandle set_layout = device.CreateDescriptorSetLayout(layout_desc);
                sdf_set_layout = set_layout;
                
                PipelineLayoutDesc pl_desc;
                pl_desc.setLayoutCount = 1;
                pl_desc.setLayouts = &set_layout;
                sdf_layout = device.CreatePipelineLayout(pl_desc);
                
                GraphicsPipelineDesc p_desc;
                p_desc.vertexShader = vs;
                p_desc.pixelShader = fs;
                p_desc.layout = sdf_layout;
                p_desc.topology = PrimitiveTopology::TriangleList;
                
                // Formats
                p_desc.renderTargetCount = 1;
                p_desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
                p_desc.depthStencilFormat = DataFormat::D32_Float;
                
                p_desc.enableBlend = true;
                p_desc.srcColorBlendFactor = BlendFactor::SrcAlpha;
                p_desc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
                
                p_desc.enableDepthTest = true;
                p_desc.enableDepthWrite = false;
                p_desc.depthFunc = ComparisonFunc::Always; // DEBUG: Always draw to see if it's occluded
                
                sdf_pipeline = device.CreateGraphicsPipeline(p_desc);
                if (sdf_pipeline != handles::INVALID_PIPELINE) {
                    std::cout << "SDF Debug Pipeline Created Successfully." << std::endl;
                } else {
                    std::cerr << "Failed to create SDF Pipeline." << std::endl;
                }
            }
        }
        
        // 3. Vector Field Pipeline
        {
            // Load Shaders
            std::vector<char> vs_data = LoadShaderData(vf_vs_path);
            std::vector<char> fs_data = LoadShaderData(vf_fs_path);
            
            ShaderHandle vs = handles::INVALID_SHADER;
            ShaderHandle fs = handles::INVALID_SHADER;

            if (!vs_data.empty()) {
                vs = device.CreateShader(vs_data.data(), vs_data.size(), ShaderStage::Vertex, "vector_field_debug_vs");
            }
            if (!fs_data.empty()) {
                fs = device.CreateShader(fs_data.data(), fs_data.size(), ShaderStage::Pixel, "vector_field_debug_fs");
            }
            
            if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                DescriptorSetLayoutBinding bindings[] = {
                    { 0, DescriptorType::SampledImage, 1, ShaderStage::Vertex | ShaderStage::Pixel },        // VF Texture
                    { 1, DescriptorType::UniformBufferDynamic, 1, ShaderStage::Vertex | ShaderStage::Pixel } // Uniforms (Dynamic)
                };
                
                DescriptorSetLayoutDesc layout_desc;
                layout_desc.bindingCount = 2;
                layout_desc.bindings = bindings;
                
                DescriptorSetLayoutHandle set_layout = device.CreateDescriptorSetLayout(layout_desc);
                vector_field_set_layout = set_layout;
                
                PipelineLayoutDesc pl_desc;
                pl_desc.setLayoutCount = 1;
                pl_desc.setLayouts = &set_layout;
                vector_field_layout = device.CreatePipelineLayout(pl_desc);
                
                GraphicsPipelineDesc p_desc;
                p_desc.vertexShader = vs;
                p_desc.pixelShader = fs;
                p_desc.layout = vector_field_layout;
                p_desc.topology = PrimitiveTopology::LineList;
                
                // Formats
                p_desc.renderTargetCount = 1;
                p_desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
                p_desc.depthStencilFormat = DataFormat::D32_Float;
                
                p_desc.enableDepthTest = true;
                p_desc.enableDepthWrite = false;
                p_desc.depthFunc = ComparisonFunc::Always; // DEBUG
                
                vector_field_pipeline = device.CreateGraphicsPipeline(p_desc);
                if (vector_field_pipeline != handles::INVALID_PIPELINE) {
                    std::cout << "VectorField Debug Pipeline Created Successfully." << std::endl;
                } else {
                    std::cerr << "Failed to create VectorField Pipeline." << std::endl;
                }
            }
        }

        // 4. Voxel Pipeline
        {
            // Load Shaders
            std::vector<char> vs_data = LoadShaderData(voxel_vs_path);
            std::vector<char> fs_data = LoadShaderData(voxel_fs_path);
            
            ShaderHandle vs = handles::INVALID_SHADER;
            ShaderHandle fs = handles::INVALID_SHADER;

            if (!vs_data.empty()) {
                vs = device.CreateShader(vs_data.data(), vs_data.size(), ShaderStage::Vertex, "voxel_debug_vs");
            }
            if (!fs_data.empty()) {
                fs = device.CreateShader(fs_data.data(), fs_data.size(), ShaderStage::Pixel, "voxel_debug_fs");
            }
            
            if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                DescriptorSetLayoutBinding bindings[] = {
                    { 0, DescriptorType::SampledImage, 1, ShaderStage::Vertex | ShaderStage::Pixel },        // Voxel Texture
                    { 1, DescriptorType::UniformBufferDynamic, 1, ShaderStage::Vertex | ShaderStage::Pixel } // Uniforms (Dynamic)
                };
                
                DescriptorSetLayoutDesc layout_desc;
                layout_desc.bindingCount = 2;
                layout_desc.bindings = bindings;
                
                DescriptorSetLayoutHandle set_layout = device.CreateDescriptorSetLayout(layout_desc);
                voxel_set_layout = set_layout;
                
                PipelineLayoutDesc pl_desc;
                pl_desc.setLayoutCount = 1;
                pl_desc.setLayouts = &set_layout;
                voxel_layout = device.CreatePipelineLayout(pl_desc);
                
                GraphicsPipelineDesc p_desc;
                p_desc.vertexShader = vs;
                p_desc.pixelShader = fs;
                p_desc.layout = voxel_layout;
                p_desc.topology = PrimitiveTopology::TriangleList; // Cube Triangles
                
                // Formats
                p_desc.renderTargetCount = 1;
                p_desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
                p_desc.depthStencilFormat = DataFormat::D32_Float;
                
                p_desc.enableDepthTest = true;
                p_desc.enableDepthWrite = true; // Voxels are opaque usually
                p_desc.depthFunc = ComparisonFunc::Always; // DEBUG
                
                p_desc.cullMode = CullMode::Back;
                // p_desc.frontFace = FrontFace::CounterClockwise; // Not supported in GraphicsPipelineDesc currently
                
                voxel_pipeline = device.CreateGraphicsPipeline(p_desc);
                if (voxel_pipeline != handles::INVALID_PIPELINE) {
                    std::cout << "Voxel Debug Pipeline Created Successfully." << std::endl;
                } else {
                    std::cerr << "Failed to create Voxel Pipeline." << std::endl;
                }
            }
        }

        initialized = true;
        std::cout << "GeometryDebugPass Initialized." << std::endl;
    }

    void BeginFrame(RHIDeviceBase& device) {
        frameIndex = (frameIndex + 1) % 3;
        auto& frame = frames[frameIndex];
        
        if (frame.uniformBuffer == handles::INVALID_RESOURCE) {
            BufferDesc desc{
                .size = 1024 * 1024, // 1MB
                .usage = GPUMemoryUsage::Dynamic,
                .memoryUsage = GPUMemoryUsage::Dynamic,
            };
            frame.uniformBuffer = device.CreateBuffer(desc);
            frame.capacity = desc.size;
            frame.mappedPtr = device.MapBuffer(frame.uniformBuffer);
        }
        
        frame.currentOffset = 0;
    }
    
    uint32_t AllocateUniforms(uint32_t size) {
        auto& frame = frames[frameIndex];
        // Align offset
        uint32_t alignedOffset = (frame.currentOffset + 255) & ~255; // Min Uniform Offset Alignment 256
        if (alignedOffset + size > frame.capacity) return 0xFFFFFFFF;
        frame.currentOffset = alignedOffset + size;
        return alignedOffset;
    }
    
    void* GetMappedPtr() {
        return frames[frameIndex].mappedPtr;
    }
    
    ResourceHandle GetUniformBuffer() {
        return frames[frameIndex].uniformBuffer;
    }


    DescriptorSetHandle GetMeshletDescriptorSet(RHIDeviceBase& device, 
                                                    ResourceHandle positionBuffer,
                                                    ResourceHandle meshletBuffer,
                                                    ResourceHandle meshletVerticesBuffer,
                                                    ResourceHandle meshletTrianglesBuffer) {
        // Create a combined key from all buffers
        uint64_t key = (uint64_t)positionBuffer ^ (uint64_t)meshletBuffer ^ 
                       (uint64_t)meshletVerticesBuffer ^ (uint64_t)meshletTrianglesBuffer;
        if (meshlet_ds_cache[frameIndex].find(key) != meshlet_ds_cache[frameIndex].end()) {
            return meshlet_ds_cache[frameIndex][key];
        }

        if (meshlet_set_layout == handles::INVALID_RESOURCE) return handles::INVALID_DESCRIPTOR_SET;

        DescriptorSetHandle ds = device.CreateDescriptorSet({meshlet_set_layout});
        if (ds == handles::INVALID_DESCRIPTOR_SET) return handles::INVALID_DESCRIPTOR_SET;

        // Binding 0: DebugUniforms (Dynamic Uniform Buffer)
        DescriptorBufferInfo uniformBufferInfo;
        uniformBufferInfo.buffer = frames[frameIndex].uniformBuffer;
        uniformBufferInfo.offset = 0;
        uniformBufferInfo.range = sizeof(DebugUniforms);

        WriteDescriptorSet uniformWrite;
        uniformWrite.dstSet = ds;
        uniformWrite.dstBinding = 0;
        uniformWrite.descriptorCount = 1;
        uniformWrite.descriptorType = DescriptorType::UniformBufferDynamic;
        uniformWrite.bufferInfo = &uniformBufferInfo;

        // Binding 1: Meshlets (Storage Buffer)
        DescriptorBufferInfo meshletBufferInfo;
        meshletBufferInfo.buffer = meshletBuffer;
        meshletBufferInfo.offset = 0;
        meshletBufferInfo.range = ~0ULL;

        WriteDescriptorSet meshletWrite;
        meshletWrite.dstSet = ds;
        meshletWrite.dstBinding = 1;
        meshletWrite.descriptorCount = 1;
        meshletWrite.descriptorType = DescriptorType::StorageBuffer;
        meshletWrite.bufferInfo = &meshletBufferInfo;
        
        // Binding 2: MeshletVertices (Storage Buffer)
        DescriptorBufferInfo meshletVerticesInfo;
        meshletVerticesInfo.buffer = meshletVerticesBuffer;
        meshletVerticesInfo.offset = 0;
        meshletVerticesInfo.range = ~0ULL;

        WriteDescriptorSet meshletVerticesWrite;
        meshletVerticesWrite.dstSet = ds;
        meshletVerticesWrite.dstBinding = 2;
        meshletVerticesWrite.descriptorCount = 1;
        meshletVerticesWrite.descriptorType = DescriptorType::StorageBuffer;
        meshletVerticesWrite.bufferInfo = &meshletVerticesInfo;

        // Binding 3: MeshletTriangles (Storage Buffer)
        DescriptorBufferInfo meshletTrianglesInfo;
        meshletTrianglesInfo.buffer = meshletTrianglesBuffer;
        meshletTrianglesInfo.offset = 0;
        meshletTrianglesInfo.range = ~0ULL;

        WriteDescriptorSet meshletTrianglesWrite;
        meshletTrianglesWrite.dstSet = ds;
        meshletTrianglesWrite.dstBinding = 3;
        meshletTrianglesWrite.descriptorCount = 1;
        meshletTrianglesWrite.descriptorType = DescriptorType::StorageBuffer;
        meshletTrianglesWrite.bufferInfo = &meshletTrianglesInfo;

        // Binding 4: Positions (Storage Buffer)
        DescriptorBufferInfo positionBufferInfo;
        positionBufferInfo.buffer = positionBuffer;
        positionBufferInfo.offset = 0;
        positionBufferInfo.range = ~0ULL;

        WriteDescriptorSet positionWrite;
        positionWrite.dstSet = ds;
        positionWrite.dstBinding = 4;
        positionWrite.descriptorCount = 1;
        positionWrite.descriptorType = DescriptorType::StorageBuffer;
        positionWrite.bufferInfo = &positionBufferInfo;

        WriteDescriptorSet writes[] = {uniformWrite, meshletWrite, meshletVerticesWrite, meshletTrianglesWrite, positionWrite};
        device.UpdateDescriptorSets(5, writes);

        meshlet_ds_cache[frameIndex][key] = ds;
        return ds;
    }

    DescriptorSetHandle GetSDFDescriptorSet(RHIDeviceBase& device, ResourceHandle sdfTexture) {
        uint64_t key = (uint64_t)sdfTexture;
        if (sdf_ds_cache[frameIndex].find(key) != sdf_ds_cache[frameIndex].end()) {
            return sdf_ds_cache[frameIndex][key];
        }

        if (sdf_set_layout == handles::INVALID_RESOURCE) return handles::INVALID_DESCRIPTOR_SET;

        DescriptorSetHandle ds = device.CreateDescriptorSet({sdf_set_layout});
        if (ds == handles::INVALID_DESCRIPTOR_SET) return handles::INVALID_DESCRIPTOR_SET;

        DescriptorImageInfo texInfo;
        texInfo.imageView = sdfTexture;
        texInfo.imageLayout = ResourceState::ShaderResource;
        texInfo.sampler = defaultSampler;

        WriteDescriptorSet texWrite;
        texWrite.dstSet = ds;
        texWrite.dstBinding = 0;
        texWrite.descriptorCount = 1;
        texWrite.descriptorType = DescriptorType::SampledImage;
        texWrite.imageInfo = &texInfo;
        
        DescriptorBufferInfo uniformBufferInfo;
        uniformBufferInfo.buffer = frames[frameIndex].uniformBuffer;
        uniformBufferInfo.offset = 0;
        uniformBufferInfo.range = sizeof(DebugUniforms);

        WriteDescriptorSet uniformWrite;
        uniformWrite.dstSet = ds;
        uniformWrite.dstBinding = 1;
        uniformWrite.descriptorCount = 1;
        uniformWrite.descriptorType = DescriptorType::UniformBufferDynamic;
        uniformWrite.bufferInfo = &uniformBufferInfo;

        WriteDescriptorSet writes[] = {texWrite, uniformWrite};
        device.UpdateDescriptorSets(2, writes);

        sdf_ds_cache[frameIndex][key] = ds;
        return ds;
    }

    DescriptorSetHandle GetVectorFieldDescriptorSet(RHIDeviceBase& device, ResourceHandle vfTexture) {
        uint64_t key = (uint64_t)vfTexture;
        if (vf_ds_cache[frameIndex].find(key) != vf_ds_cache[frameIndex].end()) {
            return vf_ds_cache[frameIndex][key];
        }

        if (vector_field_set_layout == handles::INVALID_RESOURCE) return handles::INVALID_DESCRIPTOR_SET;

        DescriptorSetHandle ds = device.CreateDescriptorSet({vector_field_set_layout});
        if (ds == handles::INVALID_DESCRIPTOR_SET) return handles::INVALID_DESCRIPTOR_SET;

        DescriptorImageInfo texInfo;
        texInfo.imageView = vfTexture;
        texInfo.imageLayout = ResourceState::ShaderResource;
        texInfo.sampler = defaultSampler;

        WriteDescriptorSet texWrite;
        texWrite.dstSet = ds;
        texWrite.dstBinding = 0;
        texWrite.descriptorCount = 1;
        texWrite.descriptorType = DescriptorType::SampledImage;
        texWrite.imageInfo = &texInfo;
        
        DescriptorBufferInfo uniformBufferInfo;
        uniformBufferInfo.buffer = frames[frameIndex].uniformBuffer;
        uniformBufferInfo.offset = 0;
        uniformBufferInfo.range = sizeof(DebugUniforms);

        WriteDescriptorSet uniformWrite;
        uniformWrite.dstSet = ds;
        uniformWrite.dstBinding = 1;
        uniformWrite.descriptorCount = 1;
        uniformWrite.descriptorType = DescriptorType::UniformBufferDynamic;
        uniformWrite.bufferInfo = &uniformBufferInfo;

        WriteDescriptorSet writes[] = {texWrite, uniformWrite};
        device.UpdateDescriptorSets(2, writes);

        vf_ds_cache[frameIndex][key] = ds;
        return ds;
    }

    DescriptorSetHandle GetVoxelDescriptorSet(RHIDeviceBase& device, ResourceHandle voxelTexture) {
        uint64_t key = (uint64_t)voxelTexture;
        if (voxel_ds_cache[frameIndex].find(key) != voxel_ds_cache[frameIndex].end()) {
            return voxel_ds_cache[frameIndex][key];
        }

        if (voxel_set_layout == handles::INVALID_RESOURCE) return handles::INVALID_DESCRIPTOR_SET;

        DescriptorSetHandle ds = device.CreateDescriptorSet({voxel_set_layout});
        if (ds == handles::INVALID_DESCRIPTOR_SET) return handles::INVALID_DESCRIPTOR_SET;

        DescriptorImageInfo texInfo;
        texInfo.imageView = voxelTexture;
        texInfo.imageLayout = ResourceState::ShaderResource;
        texInfo.sampler = defaultSampler;

        WriteDescriptorSet texWrite;
        texWrite.dstSet = ds;
        texWrite.dstBinding = 0;
        texWrite.descriptorCount = 1;
        texWrite.descriptorType = DescriptorType::SampledImage;
        texWrite.imageInfo = &texInfo;
        
        DescriptorBufferInfo uniformBufferInfo;
        uniformBufferInfo.buffer = frames[frameIndex].uniformBuffer;
        uniformBufferInfo.offset = 0;
        uniformBufferInfo.range = sizeof(DebugUniforms);

        WriteDescriptorSet uniformWrite;
        uniformWrite.dstSet = ds;
        uniformWrite.dstBinding = 1;
        uniformWrite.descriptorCount = 1;
        uniformWrite.descriptorType = DescriptorType::UniformBufferDynamic;
        uniformWrite.bufferInfo = &uniformBufferInfo;

        WriteDescriptorSet writes[] = {texWrite, uniformWrite};
        device.UpdateDescriptorSets(2, writes);

        voxel_ds_cache[frameIndex][key] = ds;
        return ds;
    }
};

static GeometryDebugContext g_debugContext;

static void ExecuteGeometryDebug(
    rhi::RHIDeviceBase& device,
    rhi::RHICommandBuffer* cmdList,
    const RenderView& view,
    const GeometryDebugSettings& settings)
{
    std::cout << "[GeometryDebug] ExecuteGeometryDebug START - visualize_meshlets=" << settings.visualize_meshlets << std::endl;
    
    g_debugContext.BeginFrame(device);
    
    // Set Viewport and Scissor
    cmdList->SetViewport(view.GetViewport());
    cmdList->SetScissor(view.GetScissor());
    
    void* mappedData = g_debugContext.GetMappedPtr();
    if (!mappedData) {
        std::cerr << "Failed to map debug uniform buffer." << std::endl;
        return;
    }
    
    int meshCount = 0;
    int drawCount = 0;
    
    content::foreach_gpu_mesh([&](id::id_type id, RHIGpuMesh* mesh) {
        if (!mesh || !mesh->IsValid()) return;
        meshCount++;
        
        math::m4x4 modelMatrix;
        float* m = (float*)&modelMatrix;
        for(int i=0; i<16; ++i) m[i] = 0.0f;
        m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
        
        math::m4x4 viewProj = view.GetViewProjectionMatrix();
        
        // Calculate Bounds and Volume Model Matrix
        const float* min = mesh->GetBoundsMin();
        const float* max = mesh->GetBoundsMax();
        
        float width = max[0] - min[0];
        float height = max[1] - min[1];
        float depth = max[2] - min[2];
        
        // Avoid zero dimensions
        if (width < 0.001f) width = 1.0f;
        if (height < 0.001f) height = 1.0f;
        if (depth < 0.001f) depth = 1.0f;

        float centerX = min[0] + width * 0.5f;
        float centerY = min[1] + height * 0.5f;
        float centerZ = min[2] + depth * 0.5f;

        math::m4x4 volumeModel;
        float* vm = (float*)&volumeModel;
        for(int i=0; i<16; ++i) vm[i] = 0.0f;
        vm[0] = width;
        vm[5] = height;
        vm[10] = depth;
        vm[15] = 1.0f;
        vm[12] = centerX;
        vm[13] = centerY;
        vm[14] = centerZ;

        // 1. Meshlet Debug
        // Debug: Log mesh info
        static std::set<id::id_type> logged_mesh_ids;
        if (logged_mesh_ids.find(id) == logged_mesh_ids.end()) {
            const float* c = mesh->GetBoundsMin();
            const float* cx = mesh->GetBoundsMax();
            std::cout << "[GeometryDebug] Mesh " << id 
                      << " MeshletCount=" << mesh->GetMeshletCount() 
                      << " BoundsMin=(" << c[0] << "," << c[1] << "," << c[2] << ")"
                      << " BoundsMax=(" << cx[0] << "," << cx[1] << "," << cx[2] << ")"
                      << std::endl;
            logged_mesh_ids.insert(id);
        }
        
        if (settings.visualize_meshlets && mesh->GetMeshletCount() > 0 && g_debugContext.meshlet_pipeline != handles::INVALID_PIPELINE) {
            uint32_t offset = g_debugContext.AllocateUniforms(sizeof(DebugUniforms));
            if (offset != 0xFFFFFFFF) {
                DebugUniforms* uniforms = (DebugUniforms*)((uint8_t*)mappedData + offset);
                uniforms->viewProjection = viewProj;
                uniforms->model = modelMatrix;
                
                DescriptorSetHandle ds = g_debugContext.GetMeshletDescriptorSet(device, 
                                                                            mesh->GetPositionBuffer(),
                                                                            mesh->GetMeshletBuffer(),
                                                                            mesh->GetMeshletVerticesBuffer(),
                                                                            mesh->GetMeshletTrianglesBuffer());
                
                // DEBUG: Log if descriptor set is invalid
                static bool logged_invalid_ds = false;
                if (ds == handles::INVALID_DESCRIPTOR_SET && !logged_invalid_ds) {
                    std::cerr << "[GeometryDebug] MESHLET: Descriptor set is INVALID!" << std::endl;
                    logged_invalid_ds = true;
                }
                
                if (ds != handles::INVALID_DESCRIPTOR_SET) {
                    cmdList->BindGraphicsPipeline(g_debugContext.meshlet_pipeline);
                    
                    uint32_t dynamicOffsets[] = { offset };
                    cmdList->BindDescriptorSets(PipelineBindPoint::Graphics, g_debugContext.meshlet_layout, 0, 1, &ds, 1, dynamicOffsets);
                    
                    // DEBUG: Log first draw call
                    static bool logged_first_draw = false;
                    if (!logged_first_draw) {
                        std::cout << "[GeometryDebug] MESHLET: Issuing Draw(3, " << mesh->GetMeshletCount() << ")" << std::endl;
                        logged_first_draw = true;
                    }
                    
                    // Draw triangles (Max 128 triangles * 3 vertices per meshlet)
                    // We use 384 vertices to cover the maximum possible triangles in a meshlet (usually 124 or 126).
                    // The shader will discard vertices that exceed the actual triangle count of the meshlet.
                    cmdList->Draw(384, 0, mesh->GetMeshletCount(), 0);
                    drawCount++;
                }
            } else {
                std::cerr << "Failed to allocate debug uniforms for mesh: " << id << std::endl;
            }
        }
        
        // 2. SDF Debug
        if (settings.visualize_sdf) {
             if (mesh->GetSDFTexture() == handles::INVALID_RESOURCE) {
                 // Only log once per mesh to avoid spam
                 static std::set<id::id_type> logged_meshes;
                 if (logged_meshes.find(id) == logged_meshes.end()) {
                     std::cout << "[GeometryDebug] Mesh " << id << " has invalid SDF texture." << std::endl;
                     logged_meshes.insert(id);
                 }
             }
             if (g_debugContext.sdf_pipeline == handles::INVALID_PIPELINE) {
                 static bool logged_pipeline = false;
                 if (!logged_pipeline) {
                     std::cout << "[GeometryDebug] SDF Pipeline is invalid." << std::endl;
                     logged_pipeline = true;
                 }
             }
        }

        if (settings.visualize_sdf && mesh->GetSDFTexture() != handles::INVALID_RESOURCE && g_debugContext.sdf_pipeline != handles::INVALID_PIPELINE) {
            // Log valid SDF texture for debugging
            static std::set<id::id_type> valid_logged_meshes;
            if (valid_logged_meshes.find(id) == valid_logged_meshes.end()) {
                std::cout << "[GeometryDebug] Mesh " << id << " has valid SDF texture. Slice Depth: " << settings.slice_depth << std::endl;
                valid_logged_meshes.insert(id);
            }

            uint32_t offset = g_debugContext.AllocateUniforms(sizeof(DebugUniforms));
            if (offset != 0xFFFFFFFF) {
                DebugUniforms* uniforms = (DebugUniforms*)((uint8_t*)mappedData + offset);
                // Zero initialize the whole struct to avoid garbage
                memset(uniforms, 0, sizeof(DebugUniforms));
                
                // Use Volume Model X/Y but specific Z for Slice
                math::m4x4 sliceModel = volumeModel;
                float* m = (float*)&sliceModel;
                // Z Scale doesn't matter for flat quad, but Translation Z does
                m[14] = min[2] + depth * settings.slice_depth;

                uniforms->viewProjection = viewProj;
                uniforms->model = sliceModel;
                uniforms->sdf.slice_depth = settings.slice_depth;
                uniforms->sdf.mode = 0.0f;
                
                DescriptorSetHandle ds = g_debugContext.GetSDFDescriptorSet(device, mesh->GetSDFTexture());
                if (ds != handles::INVALID_DESCRIPTOR_SET) {
                    cmdList->BindGraphicsPipeline(g_debugContext.sdf_pipeline);
                    
                    uint32_t dynamicOffsets[] = { offset };
                    cmdList->BindDescriptorSets(PipelineBindPoint::Graphics, g_debugContext.sdf_layout, 0, 1, &ds, 1, dynamicOffsets);
                    
                    cmdList->Draw(6, 0, 1, 0);
                }
            }
        }
        
        // 3. Vector Field Debug
        if (settings.visualize_vector_field && mesh->GetVectorFieldTexture() != handles::INVALID_RESOURCE && g_debugContext.vector_field_pipeline != handles::INVALID_PIPELINE) {
            uint32_t offset = g_debugContext.AllocateUniforms(sizeof(DebugUniforms));
            if (offset != 0xFFFFFFFF) {
                DebugUniforms* uniforms = (DebugUniforms*)((uint8_t*)mappedData + offset);
                uniforms->viewProjection = viewProj;
                uniforms->model = volumeModel;
                uniforms->vf.scale = 0.05f;
                
                const uint32_t* res = mesh->GetVectorFieldResolution();
                uniforms->vf.resolution[0] = res[0];
                uniforms->vf.resolution[1] = res[1];
                uniforms->vf.resolution[2] = res[2];
                
                DescriptorSetHandle ds = g_debugContext.GetVectorFieldDescriptorSet(device, mesh->GetVectorFieldTexture());
                if (ds != handles::INVALID_DESCRIPTOR_SET) {
                    cmdList->BindGraphicsPipeline(g_debugContext.vector_field_pipeline);
                    
                    uint32_t dynamicOffsets[] = { offset };
                    cmdList->BindDescriptorSets(PipelineBindPoint::Graphics, g_debugContext.vector_field_layout, 0, 1, &ds, 1, dynamicOffsets);
                    
                    uint32_t total_voxels = res[0] * res[1] * res[2];
                
                // Calculate step for throttling to avoid GPU hang
                uint32_t step = 1;
                const uint32_t MAX_VF_LINES = 200000;
                if (total_voxels > MAX_VF_LINES) {
                    step = (total_voxels + MAX_VF_LINES - 1) / MAX_VF_LINES;
                }
                uniforms->vf.step = (float)step;
                
                uint32_t draw_count = total_voxels / step;
                if (draw_count > 0) {
                     static bool logged_vf_size = false;
                     if (!logged_vf_size && step > 1) {
                         std::cout << "[GeometryDebug] VectorField too large: " << total_voxels << " voxels. Throttling with step " << step << " (" << draw_count << " lines)" << std::endl;
                         logged_vf_size = true;
                     }
                     cmdList->Draw(draw_count * 2, 0, 1, 0);
                }
                }
            }
        }

        // 4. Voxel Debug
        if (settings.visualize_voxels && mesh->GetVoxelTexture() != handles::INVALID_RESOURCE && g_debugContext.voxel_pipeline != handles::INVALID_PIPELINE) {
            uint32_t offset = g_debugContext.AllocateUniforms(sizeof(DebugUniforms));
            if (offset != 0xFFFFFFFF) {
                DebugUniforms* uniforms = (DebugUniforms*)((uint8_t*)mappedData + offset);
                uniforms->viewProjection = viewProj;
                uniforms->model = volumeModel;
                uniforms->voxel.scale = 1.0f; // Scale handled in shader based on resolution
                uniforms->voxel.threshold = 0.1f;
                
                const uint32_t* res = mesh->GetVoxelResolution();
                
                static std::set<id::id_type> logged_voxels;
                if (logged_voxels.find(id) == logged_voxels.end()) {
                    std::cout << "[GeometryDebug] Mesh " << id << " Voxel Res: " << res[0] << "x" << res[1] << "x" << res[2] 
                              << " Texture: " << (mesh->GetVoxelTexture() != handles::INVALID_RESOURCE ? "Valid" : "Invalid") << std::endl;
                    logged_voxels.insert(id);
                }

                uniforms->voxel.resolution[0] = res[0];
                uniforms->voxel.resolution[1] = res[1];
                uniforms->voxel.resolution[2] = res[2];
                
                DescriptorSetHandle ds = g_debugContext.GetVoxelDescriptorSet(device, mesh->GetVoxelTexture());
                if (ds != handles::INVALID_DESCRIPTOR_SET) {
                    cmdList->BindGraphicsPipeline(g_debugContext.voxel_pipeline);
                    
                    uint32_t dynamicOffsets[] = { offset };
                    cmdList->BindDescriptorSets(PipelineBindPoint::Graphics, g_debugContext.voxel_layout, 0, 1, &ds, 1, dynamicOffsets);
                    
                    uint32_t total_voxels = res[0] * res[1] * res[2];
                
                // Calculate step for throttling to avoid GPU hang
                uint32_t step = 1;
                const uint32_t MAX_VOXELS = 100000;
                if (total_voxels > MAX_VOXELS) {
                    step = (total_voxels + MAX_VOXELS - 1) / MAX_VOXELS;
                }
                uniforms->voxel.step = (float)step;
                
                uint32_t draw_count = total_voxels / step;
                if (draw_count > 0) {
                     static bool logged_voxel_size = false;
                     if (!logged_voxel_size && step > 1) {
                         std::cout << "[GeometryDebug] Voxel too large: " << total_voxels << " instances. Throttling with step " << step << " (" << draw_count << " instances)" << std::endl;
                         logged_voxel_size = true;
                     }
                     cmdList->Draw(36, 0, draw_count, 0);
                }
                }
            }
        }
    });
    
    // Only print debug info once every 60 frames to avoid spam
    static int frameCounter = 0;
    if (frameCounter++ % 60 == 0) {
        if (settings.enable) {
            std::cout << "[GeometryDebug] Frame " << frameCounter << ": Meshes: " << meshCount << ", Draws: " << drawCount << ", Mode: " << (int)settings.mode << std::endl;
        }
    }
}

const GeometryDebugData& AddGeometryDebugPass(
    rendergraph::RenderGraph& graph, 
    rendergraph::RGResourceHandle target,
    rendergraph::RGResourceHandle depth,
    const RenderView& view,
    const GeometryDebugSettings* settings)  // Pass by pointer!
{
    return graph.AddPass<GeometryDebugData>("GeometryDebug", rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::UI,
        [&](GeometryDebugData& data, rendergraph::RenderGraphBuilder& builder) {
            // Write to target and depth (LoadAction::Load will preserve contents)
            data.target = builder.Write(target, rhi::ResourceState::RenderTarget);
            builder.Write(depth, rhi::ResourceState::DepthStencil);

            rendergraph::RGRenderPassDesc desc;
            desc.colors.resize(1);
            desc.colors[0].texture = data.target;
            desc.colors[0].loadOp = rhi::LoadAction::Load;
            desc.colors[0].storeOp = rhi::StoreAction::Store;
            
            // Use original depth handle, not the written one (same as SkyboxPass)
            desc.depthStencil.texture = depth;
            desc.depthStencil.depthLoadOp = rhi::LoadAction::Load;
            desc.depthStencil.depthStoreOp = rhi::StoreAction::Store;
            
            builder.DeclareRenderPass(desc);

            // Note: We don't store view/settings in data because we capture them in the execute lambda
            // However, if we needed them in other passes, we would put them in data.
        },
        [=](const GeometryDebugData& data, rendergraph::RenderGraphContext& context) {
            auto& device = context.graph->GetDevice();
            auto* cmdList = context.cmdBuffer;
            
            // DEBUG: Check if render pass is active
            std::cout << "[GeometryDebug] Execute lambda called" << std::endl;
            
            if (!g_debugContext.initialized) {
                if (g_debugContext.compatibleRenderPass == handles::INVALID_RESOURCE) {
                    auto* targetRes = static_cast<rendergraph::RenderGraphTexture*>(context.graph->GetResource(data.target));
                    auto* depthRes = static_cast<rendergraph::RenderGraphTexture*>(context.graph->GetResource(depth));
                    
                    rhi::RenderPassDesc desc;
                    desc.colorAttachments.resize(1);
                    desc.colorAttachments[0].format = targetRes->GetDesc().format;
                    desc.colorAttachments[0].loadOp = rhi::LoadAction::Load;
                    desc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
                    
                    desc.depthAttachment.format = depthRes->GetDesc().format;
                    desc.depthAttachment.loadOp = rhi::LoadAction::Load;
                    desc.depthAttachment.storeOp = rhi::StoreAction::Store;
                    
                    g_debugContext.compatibleRenderPass = device.CreateRenderPass(desc);
                }
                g_debugContext.Initialize(device, g_debugContext.compatibleRenderPass);
            }
            
            ExecuteGeometryDebug(device, cmdList, view, *settings);  // Dereference pointer!
        }
    );
}

void RenderGeometryDebug(
    rhi::RHIDeviceBase& device,
    rhi::RHICommandBuffer* cmdBuffer,
    const RenderView& view,
    rhi::ResourceHandle renderTarget,
    rhi::ResourceHandle depthStencil,
    rhi::DataFormat colorFormat,
    rhi::DataFormat depthFormat,
    const GeometryDebugSettings& settings
) {
    if (!g_debugContext.initialized) {
        // Create compatible render pass if not initialized
        if (g_debugContext.compatibleRenderPass == handles::INVALID_RESOURCE) {
            RenderPassDesc desc;
            desc.colorAttachments.resize(1);
            desc.colorAttachments[0].format = colorFormat;
            desc.colorAttachments[0].loadOp = LoadAction::Load;
            desc.colorAttachments[0].storeOp = StoreAction::Store;
            
            desc.depthAttachment.format = depthFormat;
            desc.depthAttachment.loadOp = LoadAction::Load;
            desc.depthAttachment.storeOp = StoreAction::Store;
            
            // Note: We don't bind actual textures here for the compatibility pass
            // We only need formats for pipeline creation.
            
            g_debugContext.compatibleRenderPass = device.CreateRenderPass(desc);
        }
        g_debugContext.Initialize(device, g_debugContext.compatibleRenderPass);
    }
    
    ExecuteGeometryDebug(device, cmdBuffer, view, settings);
}

}
