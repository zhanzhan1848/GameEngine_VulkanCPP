#include "GPUCullingPipeline.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHIResource.h"
#include "../RHI/Core/RHICommand.h"
#include "../Scene/RenderSceneSnapshot.h"
#include "../Nanite/NaniteStreamingManager.h"
#include "../Nanite/NaniteResourceManager.h"
#include "../RHI/Core/RHIGpuMesh.h"
#include "../RHI/Core/RHIMath.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>

namespace primal::graphics::nanite {

GPUCullingPipeline& GPUCullingPipeline::Get() {
    static GPUCullingPipeline instance;
    return instance;
}

namespace {
    std::vector<u8> LoadShaderBytecode(const char* shaderName, const char* entryPoint) {
        std::string shaderPath = std::string("shaders/") + shaderName + ".metal";
        
        std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            shaderPath = std::string("Darwin/Debug/shaders/") + shaderName + ".metal";
            file.open(shaderPath, std::ios::binary | std::ios::ate);
        }
        
        if (!file.is_open()) {
            std::cerr << "Failed to load shader: " << shaderName << std::endl;
            return {};
        }
        
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        std::vector<u8> buffer(size);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        
        return buffer;
    }
}

bool GPUCullingPipeline::Initialize(rhi::RHIDeviceBase* device, const CullingConfig& config) {
    if (!device) return false;
    if (initialized_) return true;
    
    device_ = device;
    config_ = config;
    
    if (!CreatePipelines()) {
        std::cerr << "Failed to create pipelines" << std::endl;
        return false;
    }
    
    if (!CreateBuffers()) {
        std::cerr << "Failed to create buffers" << std::endl;
        return false;
    }
    
    initialized_ = true;
    return true;
}

bool GPUCullingPipeline::CreatePipelines() {
    // Create main culling descriptor layout for all stages
    rhi::DescriptorSetLayoutBinding cullingBindings[] = {
        // Binding 0: Instance data buffer (read-only)
        { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 1: Instance bounds buffer (read-only)
        { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 2: Instance visibility buffer (read-write)
        { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 3: Culling uniforms (read-only) - MOVED from binding 9
        { 3, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 4: Cluster refs buffer (read-only)
        { 4, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 5: Cluster visibility buffer (read-write)
        { 5, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 6: Visible counter (atomic)
        { 6, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 7: Visible cluster list (write-only)
        { 7, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 8: Indirect commands buffer (write-only)
        { 8, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr },
        // Binding 9: HZB texture (read-only, for occlusion culling)
        { 9, rhi::DescriptorType::SampledImage, 1, rhi::ShaderStage::Compute, nullptr },
    };

    rhi::DescriptorSetLayoutDesc cullingLayoutDesc{
        .bindings = cullingBindings,
        .bindingCount = 10
    };

    culling_descriptor_layout_ = device_->CreateDescriptorSetLayout(cullingLayoutDesc);
    if (culling_descriptor_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        std::cerr << "Failed to create culling descriptor layout" << std::endl;
        return false;
    }

    rhi::PipelineLayoutDesc cullingPipelineLayoutDesc{
        .setLayoutCount = 1,
        .setLayouts = &culling_descriptor_layout_,
        .pushConstantRangeCount = 0,
        .pushConstantRanges = nullptr
    };

    culling_pipeline_layout_ = device_->CreatePipelineLayout(cullingPipelineLayoutDesc);
    if (culling_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
        std::cerr << "Failed to create culling pipeline layout" << std::endl;
        return false;
    }

    // Load and create GPU culling compute pipelines
    auto loadComputePipeline = [&](const char* shaderName, const char* entryPoint) -> rhi::PipelineHandle {
        auto shaderCode = LoadShaderBytecode(shaderName, entryPoint);
        if (shaderCode.empty()) {
            std::cout << "GPU Culling shader " << shaderName << " not found, skipping" << std::endl;
            return rhi::handles::INVALID_PIPELINE;
        }

        rhi::ShaderHandle shader = device_->CreateShader(
            shaderCode.data(),
            shaderCode.size(),
            rhi::ShaderStage::Compute,
            entryPoint
        );

        if (shader == rhi::handles::INVALID_SHADER) {
            std::cerr << "Failed to create shader: " << shaderName << std::endl;
            return rhi::handles::INVALID_PIPELINE;
        }

        rhi::ComputePipelineDesc pipelineDesc{};
        pipelineDesc.layout = culling_pipeline_layout_;
        pipelineDesc.computeShader = shader;

        // Set appropriate thread group size for each shader
        // This is critical for Metal performance - must match shader's [[threadgroup_position_in_grid]] usage
        if (std::string(shaderName) == "GPUCullingPipeline") {
            if (std::string(entryPoint) == "stage1_instance_frustum_culling") {
                // Process instances in parallel - 64 threads per group for good GPU utilization
                pipelineDesc.threadGroupSize = {64, 1, 1};
            } else if (std::string(entryPoint) == "stage2_distance_small_object_culling") {
                // Cluster culling - 64 threads per group
                pipelineDesc.threadGroupSize = {64, 1, 1};
            } else if (std::string(entryPoint) == "stage3_lod_selection") {
                // LOD selection - 64 threads per group
                pipelineDesc.threadGroupSize = {64, 1, 1};
            } else if (std::string(entryPoint) == "stage4_cluster_expansion") {
                // Cluster expansion - 64 threads per group
                pipelineDesc.threadGroupSize = {64, 1, 1};
            } else if (std::string(entryPoint) == "stage5_occlusion_culling") {
                // Occlusion culling - 64 threads per group
                pipelineDesc.threadGroupSize = {64, 1, 1};
            } else if (std::string(entryPoint) == "stage6_compact_visible_list") {
                // Compaction - 64 threads per group
                pipelineDesc.threadGroupSize = {64, 1, 1};
            } else if (std::string(entryPoint) == "stage7_build_indirect_commands") {
                // Command building - 64 threads per group
                pipelineDesc.threadGroupSize = {64, 1, 1};
            } else {
                // Default fallback
                pipelineDesc.threadGroupSize = {64, 1, 1};
            }
        } else {
            // Default for other shaders
            pipelineDesc.threadGroupSize = {64, 1, 1};
        }

        rhi::PipelineHandle pipeline = device_->CreateComputePipeline(pipelineDesc);
        if (pipeline == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "Failed to create pipeline: " << shaderName << std::endl;
        } else {
            std::cout << "Created GPU culling pipeline: " << shaderName
                      << " (ThreadGroupSize: " << pipelineDesc.threadGroupSize.x
                      << ", " << pipelineDesc.threadGroupSize.y
                      << ", " << pipelineDesc.threadGroupSize.z << ")" << std::endl;
        }

        return pipeline;
    };

    // Create progressive filtering pipeline stages
    frustum_culling_pipeline_ = loadComputePipeline("GPUCullingPipeline", "stage1_instance_frustum_culling");
    distance_culling_pipeline_ = loadComputePipeline("GPUCullingPipeline", "stage2_distance_small_object_culling");
    lod_selection_pipeline_ = loadComputePipeline("GPUCullingPipeline", "stage3_lod_selection");
    cluster_expansion_pipeline_ = loadComputePipeline("GPUCullingPipeline", "stage4_cluster_expansion");
    occlusion_culling_pipeline_ = loadComputePipeline("GPUCullingPipeline", "stage5_occlusion_culling");
    compaction_pipeline_ = loadComputePipeline("GPUCullingPipeline", "stage6_compact_visible_list");
    indirect_command_pipeline_ = loadComputePipeline("GPUCullingPipeline", "stage7_build_indirect_commands");

    // Check if we have at least the basic GPU pipelines
    if (frustum_culling_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "Critical: Failed to create frustum culling pipeline" << std::endl;
        return false;
    }

    std::cout << "GPU Culling pipelines created successfully (progressive filtering)" << std::endl;
    return true;
}

bool GPUCullingPipeline::CreateBuffers() {
    // Instance bounds buffer (bounding spheres) - use Dynamic for CPU access
    rhi::BufferDesc instanceBoundsDesc{};
    instanceBoundsDesc.size = sizeof(float) * 4 * config_.max_instances_per_dispatch; // center(3) + radius(1)
    instanceBoundsDesc.type = rhi::BufferType::Structured;
    instanceBoundsDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic; // Changed to Dynamic for CPU access
    instanceBoundsDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ShaderResource) | static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    instance_bounds_buffer_ = device_->CreateBuffer(instanceBoundsDesc);

    // Instance visibility buffer (progressive filtering results)
    rhi::BufferDesc instanceVisDesc{};
    instanceVisDesc.size = sizeof(u32) * 4 * config_.max_instances_per_dispatch; // is_visible + instance_index + lod_level + distance_rank
    instanceVisDesc.type = rhi::BufferType::Structured;
    instanceVisDesc.memoryUsage = rhi::GPUMemoryUsage::Readback; // Changed from Dynamic to Readback for CPU access
    instanceVisDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    instance_visibility_buffer_ = device_->CreateBuffer(instanceVisDesc);

    std::cout << "[GPUCulling] Created instance_visibility_buffer_: size=" << instanceVisDesc.size
              << " bytes, max_instances=" << config_.max_instances_per_dispatch
              << ", elements=" << (instanceVisDesc.size / (sizeof(u32) * 4)) << std::endl;

    // Cluster visibility buffer (expanded from instances)
    rhi::BufferDesc clusterVisDesc{};
    clusterVisDesc.size = sizeof(u32) * 4 * config_.max_clusters_per_dispatch; // is_visible + cluster_index + instance_index + lod_level
    clusterVisDesc.type = rhi::BufferType::Structured;
    clusterVisDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    clusterVisDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    cluster_visibility_buffer_ = device_->CreateBuffer(clusterVisDesc);

    // Visible counter (atomic)
    rhi::BufferDesc counterDesc{};
    counterDesc.size = sizeof(u32);
    counterDesc.type = rhi::BufferType::Structured;
    counterDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    counterDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    visible_counter_buffer_ = device_->CreateBuffer(counterDesc);

    // Visible cluster list (compacted)
    rhi::BufferDesc visibleListDesc{};
    visibleListDesc.size = sizeof(u32) * config_.max_clusters_per_dispatch;
    visibleListDesc.type = rhi::BufferType::Structured;
    visibleListDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    visibleListDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    visible_cluster_list_buffer_ = device_->CreateBuffer(visibleListDesc);

    // Indirect draw commands buffer
    rhi::BufferDesc indirectDesc{};
    indirectDesc.size = sizeof(u32) * 5; // vertex_count + instance_count + first_vertex + first_instance + padding
    indirectDesc.type = rhi::BufferType::Structured;
    indirectDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    indirectDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::IndirectArg) | static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    indirect_args_buffer_ = device_->CreateBuffer(indirectDesc);

    // HZB texture for occlusion culling
    rhi::TextureDesc hizDesc{};
    hizDesc.size = {2048, 2048, 1};
    hizDesc.mipLevels = 1;
    hizDesc.arraySize = 1;
    hizDesc.format = rhi::DataFormat::D32_Float;
    hizDesc.type = rhi::TextureType::Texture2D;
    hizDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    hizDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    hiz_buffer_ = device_->CreateTexture(hizDesc);

    // Culling constants buffer
    rhi::BufferDesc constantsDesc{};
    constantsDesc.size = sizeof(float) * 68; // Match Metal shader CullingUniforms size (272 bytes)
    constantsDesc.type = rhi::BufferType::Constant;
    constantsDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    constantsDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ConstantBuffer) | static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    culling_constants_buffer_ = device_->CreateBuffer(constantsDesc);

    // Debug readback buffer (GPU-to-CPU for debugging)
    rhi::BufferDesc debugDesc{};
    debugDesc.size = sizeof(u32) * 4 * config_.max_instances_per_dispatch; // Same structure as instance_visibility
    debugDesc.type = rhi::BufferType::Structured;
    debugDesc.memoryUsage = rhi::GPUMemoryUsage::Readback; // CPU accessible
    debugDesc.bindFlags = static_cast<u32>(rhi::BufferUsageFlags::TransferDst);
    debug_readback_buffer_ = device_->CreateBuffer(debugDesc);

    std::cout << "GPU Culling buffers created successfully" << std::endl;
    return true;
}

void GPUCullingPipeline::Shutdown() {
    if (!device_) return;
    
    if (streaming_feedback_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(streaming_feedback_pipeline_);
        streaming_feedback_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (update_access_time_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(update_access_time_pipeline_);
        update_access_time_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (check_residency_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(check_residency_pipeline_);
        check_residency_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    
    if (streaming_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(streaming_pipeline_layout_);
        streaming_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    if (streaming_descriptor_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(streaming_descriptor_layout_);
        streaming_descriptor_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (streaming_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(streaming_descriptor_set_);
        streaming_descriptor_set_ = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    
    if (instance_bounds_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(instance_bounds_buffer_);
        instance_bounds_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (instance_visibility_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(instance_visibility_buffer_);
        instance_visibility_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (cluster_visibility_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(cluster_visibility_buffer_);
        cluster_visibility_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (visible_counter_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(visible_counter_buffer_);
        visible_counter_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (visible_cluster_list_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(visible_cluster_list_buffer_);
        visible_cluster_list_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (indirect_args_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(indirect_args_buffer_);
        indirect_args_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (culling_constants_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(culling_constants_buffer_);
        culling_constants_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (hiz_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(hiz_buffer_);
        hiz_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    
    device_ = nullptr;
    initialized_ = false;
}

bool GPUCullingPipeline::Execute(rhi::RHICommandBuffer* cmdBuffer,
                 const RenderSceneSnapshot& snapshot,
                 const math::m4x4& viewMatrix,
                 const math::m4x4& projectionMatrix,
                 NaniteStreamingManager* streamingManager,
                 u32 frameIndex) {
    std::cout << "[GPUCulling] GPU Progressive Filtering Execute called, initialized_=" << initialized_ << std::endl;

    if (!initialized_) {
        std::cerr << "GPUCullingPipeline: Not initialized" << std::endl;
        return false;
    }

    if (!config_.enable_gpu_culling) {
        std::cout << "[GPUCulling] GPU culling disabled, skipping" << std::endl;
        return true;
    }

    const u32 instanceCount = snapshot.GetInstanceCount();
    const u32 clusterCount = snapshot.GetClusterRefCount();

    if (instanceCount == 0 || clusterCount == 0) {
        results_.visible_instance_count = 0;
        results_.visible_cluster_count = 0;
        return true;
    }

    math::m4x4 viewProjection = projectionMatrix * viewMatrix;
    math::v3 cameraPos = extract_camera_position(viewMatrix);

    // Fill instance bounds buffer with actual geometry data
    if (instance_bounds_buffer_ != rhi::handles::INVALID_RESOURCE) {
        // Map the buffer and fill with bounding spheres for each instance
        void* mapped = device_->MapBuffer(instance_bounds_buffer_);
        if (mapped) {
            struct BoundingSphere {
                math::v3 center;
                float radius;
            };

            BoundingSphere* bounds = static_cast<BoundingSphere*>(mapped);

            // For each instance, get its geometry bounds
            for (u32 i = 0; i < instanceCount; ++i) {
                // Get instance data to find which geometry it uses
                const auto& instanceData = snapshot.GetInstanceData();
                if (i < instanceData.size()) {
                    // Get geometry ID from instance data
                    id::id_type geometryId = instanceData[i].geometry_id;

                    // Get the Nanite resource for this geometry
                    auto& resource_manager = nanite::NaniteResourceManager::Get();
                    nanite::NaniteRuntimeResource* resource = resource_manager.GetOrCreateResource(geometryId);

                    if (resource && resource->gpu_mesh) {
                        // Get actual mesh bounds
                        const f32* boundsMin = resource->gpu_mesh->GetBoundsMin();
                        const f32* boundsMax = resource->gpu_mesh->GetBoundsMax();

                        // Calculate bounding sphere from AABB
                        math::v3 minPoint{boundsMin[0], boundsMin[1], boundsMin[2]};
                        math::v3 maxPoint{boundsMax[0], boundsMax[1], boundsMax[2]};
                        math::v3 center = (minPoint + maxPoint) * 0.5f;
                        math::v3 extent = maxPoint - minPoint;
                        float radius = std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z) * 0.5f;

                        // Transform to world space using instance transform
                        math::v4 worldCenter4 = instanceData[i].world_matrix * math::v4{center.x, center.y, center.z, 1.0f};
                        bounds[i].center = math::v3{worldCenter4.x, worldCenter4.y, worldCenter4.z};

                        // Scale radius by the maximum scale factor from the transform matrix
                        math::v3 col0{instanceData[i].world_matrix.columns[0].x,
                                        instanceData[i].world_matrix.columns[0].y,
                                        instanceData[i].world_matrix.columns[0].z};
                        math::v3 col1{instanceData[i].world_matrix.columns[1].x,
                                        instanceData[i].world_matrix.columns[1].y,
                                        instanceData[i].world_matrix.columns[1].z};
                        math::v3 col2{instanceData[i].world_matrix.columns[2].x,
                                        instanceData[i].world_matrix.columns[2].y,
                                        instanceData[i].world_matrix.columns[2].z};
                        float scale0 = std::sqrt(col0.x * col0.x + col0.y * col0.y + col0.z * col0.z);
                        float scale1 = std::sqrt(col1.x * col1.x + col1.y * col1.y + col1.z * col1.z);
                        float scale2 = std::sqrt(col2.x * col2.x + col2.y * col2.y + col2.z * col2.z);
                        float maxScale = std::max({scale0, scale1, scale2});
                        bounds[i].radius = radius * maxScale;
                    } else {
                        // Fallback to default bounds if no mesh data
                        bounds[i].center = math::v3{0.0f, 0.0f, 0.0f};
                        bounds[i].radius = 1.0f;
                    }
                } else {
                    bounds[i].center = math::v3{0.0f, 0.0f, 0.0f};
                    bounds[i].radius = 1.0f;
                }
            }

            device_->UnmapBuffer(instance_bounds_buffer_);
            std::cout << "[GPUCulling] Filled instance bounds buffer for " << instanceCount << " instances" << std::endl;
        }
    }

    // Properly upload snapshot data from staging buffers to GPU buffers
    // This fixes the fundamental issue where data was stuck in staging buffers
    // Note: We need const_cast because UploadToGPUBuffers is not const (it performs GPU operations)
    if (!const_cast<RenderSceneSnapshot&>(snapshot).UploadToGPUBuffers(cmdBuffer)) {
        std::cerr << "GPUCullingPipeline: Failed to upload snapshot data to GPU" << std::endl;
        return false;
    }

    // Update culling descriptor set with current frame data
    if (!UpdateCullingDescriptorSet(snapshot, viewMatrix, projectionMatrix, viewProjection, cameraPos, frameIndex)) {
        std::cerr << "GPUCullingPipeline: Failed to update descriptor set" << std::endl;
        return false;
    }

    u32 zero = 0;
    if (visible_counter_buffer_ != rhi::handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(visible_counter_buffer_);
        if (mapped) {
            memcpy(mapped, &zero, sizeof(u32));
            device_->UnmapBuffer(visible_counter_buffer_);
        }
    }

    // === STAGE 1: Instance Frustum Culling ===
    std::cout << "[GPUCulling] Stage1: GPU Instance Frustum Culling (" << instanceCount << " instances)" << std::endl;
    if (!Stage1_FrustumCulling(cmdBuffer, snapshot, viewProjection, frameIndex)) {
        std::cerr << "GPUCullingPipeline: Stage 1 failed" << std::endl;
        return false;
    }

    // === STAGE 2: Distance & Small Object Culling ===
    if (config_.enable_small_object_culling) {
        std::cout << "[GPUCulling] Stage2: GPU Distance & Small Object Culling" << std::endl;
        if (!Stage2_DistanceCulling(cmdBuffer, snapshot, frameIndex)) {
            std::cerr << "GPUCullingPipeline: Stage 2 failed" << std::endl;
            return false;
        }
    }

    // === STAGE 3: LOD Selection ===
    if (config_.enable_lod_selection) {
        std::cout << "[GPUCulling] Stage3: GPU LOD Selection" << std::endl;
        if (!Stage3_LODSelection(cmdBuffer, snapshot, viewMatrix, cameraPos, frameIndex)) {
            std::cerr << "GPUCullingPipeline: Stage 3 failed" << std::endl;
            return false;
        }
    }

    // === STAGE 4: Cluster Expansion ===
    std::cout << "[GPUCulling] Stage4: GPU Cluster Expansion" << std::endl;
    if (!Stage4_ClusterExpansion(cmdBuffer, snapshot, frameIndex)) {
        std::cerr << "GPUCullingPipeline: Stage 4 failed" << std::endl;
        return false;
    }

    // === STAGE 5: Occlusion Culling (HZB) ===
    if (config_.enable_occlusion_culling) {
        std::cout << "[GPUCulling] Stage5: GPU Occlusion Culling (HZB)" << std::endl;
        if (!Stage5_OcclusionCulling(cmdBuffer, snapshot, frameIndex)) {
            std::cerr << "GPUCullingPipeline: Stage 5 failed" << std::endl;
            return false;
        }
    }

    // === STAGE 6: Visible List Compaction ===
    std::cout << "[GPUCulling] Stage6: GPU Visible List Compaction" << std::endl;
    if (!Stage6_Compaction(cmdBuffer, snapshot, frameIndex)) {
        std::cerr << "GPUCullingPipeline: Stage 6 failed" << std::endl;
        return false;
    }

    // === STAGE 7: Build Indirect Commands ===
    std::cout << "[GPUCulling] Stage7: GPU Build Indirect Commands" << std::endl;
    if (!Stage7_BuildIndirectCommands(cmdBuffer, snapshot, frameIndex)) {
        std::cerr << "GPUCullingPipeline: Stage 7 failed" << std::endl;
        return false;
    }

    // === GPU TO CPU COPY FOR DEBUGGING ===
    // Copy instance visibility from GPU buffer to CPU readback buffer
    if (instance_visibility_buffer_ != rhi::handles::INVALID_RESOURCE &&
        debug_readback_buffer_ != rhi::handles::INVALID_RESOURCE) {
        u32 copySize = sizeof(u32) * 4 * instanceCount; // InstanceVisibility struct size
        cmdBuffer->CopyBuffer(instance_visibility_buffer_, debug_readback_buffer_, 0, 0, copySize);
        std::cout << "[GPUCulling] Copied " << instanceCount << " instance visibility results to debug readback buffer" << std::endl;

        // NOTE: We cannot synchronize here because we're in the middle of RenderGraph execution
        // The caller (RenderGraph) will handle command buffer submission and synchronization
        // Mark that we need readback after GPU completes
        results_.needs_readback = true;
    }

    // Set buffers for GPU-driven draw pipeline
    results_.indirect_args_buffer = indirect_args_buffer_;
    results_.visible_cluster_list_buffer = visible_cluster_list_buffer_;
    results_.instance_visibility_buffer = instance_visibility_buffer_;
    results_.cluster_visibility_buffer = cluster_visibility_buffer_;

    // NOTE: Actual visibility counts will be determined by GPU atomic counter
    // These are maximum possible values - actual counts depend on GPU culling results
    results_.visible_instance_count = instanceCount;
    results_.visible_cluster_count = clusterCount;

    std::cout << "[GPUCulling] GPU Progressive Filtering Complete: "
              << results_.visible_instance_count << "/" << instanceCount << " instances, "
              << results_.visible_cluster_count << "/" << clusterCount << " clusters visible" << std::endl;

    return true;
}

bool GPUCullingPipeline::Stage1_FrustumCulling(rhi::RHICommandBuffer* cmdBuffer,
                                               const RenderSceneSnapshot& snapshot,
                                               const math::m4x4& viewProjection,
                                               u32 frameIndex) {
    if (frustum_culling_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[GPUCulling] Frustum culling pipeline not available" << std::endl;
        return false;
    }

    const u32 instanceCount = snapshot.GetInstanceCount();
    if (instanceCount == 0) {
        results_.visible_instance_count = 0;
        return true;
    }

    std::cout << "[GPUCulling] Stage1: GPU Instance Frustum Culling (" << instanceCount << " instances)" << std::endl;
    std::cout << "[GPUCulling] Stage1: Dispatching " << ((instanceCount + 63) / 64) << " thread groups (total threads: " << (((instanceCount + 63) / 64) * 64) << ")" << std::endl;

    // Bind compute pipeline and descriptor sets
    cmdBuffer->BindComputePipeline(frustum_culling_pipeline_);

    if (culling_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        const rhi::DescriptorSetHandle descriptor_sets[] = { culling_descriptor_set_ };
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       culling_pipeline_layout_,
                                       0, 1, descriptor_sets,
                                       0, nullptr);
    }

    // Dispatch compute shader - one thread per instance
    u32 threadGroups = (instanceCount + 63) / 64;
    cmdBuffer->Dispatch(threadGroups, 1, 1);

    return true;
}

bool GPUCullingPipeline::Stage2_OcclusionCulling(rhi::RHICommandBuffer* cmdBuffer,
                                                 const RenderSceneSnapshot& snapshot,
                                                 u32 frameIndex) {
    return true;
}

bool GPUCullingPipeline::Stage4_InstanceCulling(rhi::RHICommandBuffer* cmdBuffer,
                                                const RenderSceneSnapshot& snapshot,
                                                u32 frameIndex) {
    results_.visible_instance_count = snapshot.GetInstanceCount();
    return true;
}

void GPUCullingPipeline::StreamingFeedback(rhi::RHICommandBuffer* cmdBuffer,
                                            NaniteStreamingManager* streamingManager,
                                            u32 frameIndex) {
    if (!streamingManager || !cmdBuffer) {
        return;
    }

    rhi::ResourceHandle residencyBuffer = streamingManager->GetResidencyBuffer();
    rhi::ResourceHandle requestBuffer = streamingManager->GetRequestBuffer();
    rhi::ResourceHandle feedbackBuffer = streamingManager->GetFeedbackBuffer();
    
    if (residencyBuffer == rhi::handles::INVALID_RESOURCE ||
        requestBuffer == rhi::handles::INVALID_RESOURCE ||
        feedbackBuffer == rhi::handles::INVALID_RESOURCE) {
        streamingManager->ProcessRequests(frameIndex);
        return;
    }

    if (streaming_feedback_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        streamingManager->ProcessRequests(frameIndex);
        return;
    }

    struct StreamingConstants {
        u32 cluster_count;
        u32 max_requests;
        u32 frame_index;
        u32 padding;
    };
    
    StreamingConstants constants{};
    constants.cluster_count = config_.max_clusters_per_dispatch;
    constants.max_requests = streamingManager->GetConfig().max_requests_per_frame;
    constants.frame_index = frameIndex;
    
    rhi::BufferDesc constantBufferDesc{};
    constantBufferDesc.size = sizeof(StreamingConstants);
    constantBufferDesc.type = rhi::BufferType::Constant;
    constantBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    rhi::ResourceHandle constantBuffer = device_->CreateBuffer(constantBufferDesc);
    
    if (constantBuffer == rhi::handles::INVALID_RESOURCE) {
        streamingManager->ProcessRequests(frameIndex);
        return;
    }
    
    void* mapped = device_->MapBuffer(constantBuffer);
    if (mapped) {
        memcpy(mapped, &constants, sizeof(StreamingConstants));
        device_->UnmapBuffer(constantBuffer);
    }
    
    if (streaming_descriptor_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) {
        rhi::DescriptorSetDesc desc{};
        desc.layout = streaming_descriptor_layout_;
        streaming_descriptor_set_ = device_->CreateDescriptorSet(desc);
    }
    
    if (streaming_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        rhi::WriteDescriptorSet writes[4];
        rhi::DescriptorBufferInfo bufferInfos[4];
        
        bufferInfos[0].buffer = residencyBuffer;
        bufferInfos[0].offset = 0;
        bufferInfos[0].range = ~0ull;
        writes[0].dstSet = streaming_descriptor_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[0].bufferInfo = &bufferInfos[0];
        
        bufferInfos[1].buffer = requestBuffer;
        bufferInfos[1].offset = 0;
        bufferInfos[1].range = ~0ull;
        writes[1].dstSet = streaming_descriptor_set_;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[1].bufferInfo = &bufferInfos[1];
        
        bufferInfos[2].buffer = feedbackBuffer;
        bufferInfos[2].offset = 0;
        bufferInfos[2].range = ~0ull;
        writes[2].dstSet = streaming_descriptor_set_;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[2].bufferInfo = &bufferInfos[2];
        
        bufferInfos[3].buffer = constantBuffer;
        bufferInfos[3].offset = 0;
        bufferInfos[3].range = sizeof(StreamingConstants);
        writes[3].dstSet = streaming_descriptor_set_;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = rhi::DescriptorType::UniformBuffer;
        writes[3].bufferInfo = &bufferInfos[3];
        
        device_->UpdateDescriptorSets(4, writes);
        
        cmdBuffer->BindComputePipeline(streaming_feedback_pipeline_);
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       streaming_pipeline_layout_,
                                       0, 1, &streaming_descriptor_set_,
                                       0, nullptr);
        
        u32 threadGroups = (constants.cluster_count + 63) / 64;
        cmdBuffer->Dispatch(threadGroups, 1, 1);
    }
    
    device_->DestroyBuffer(constantBuffer);
    
    streamingManager->ProcessRequests(frameIndex);
}

bool GPUCullingPipeline::CompactResults(rhi::RHICommandBuffer* cmdBuffer, u32 frameIndex) {
    (void)cmdBuffer;
    (void)frameIndex;

    std::cout << "[GPUCulling] CompactResults called: visible_clusters=" << results_.visible_cluster_count << std::endl;

    results_.indirect_args_buffer = indirect_args_buffer_;
    results_.instance_visibility_buffer = instance_visibility_buffer_;
    results_.cluster_visibility_buffer = cluster_visibility_buffer_;
    results_.visible_cluster_list_buffer = visible_cluster_list_buffer_;

    if (results_.visible_cluster_count == 0) {
        std::cout << "[GPUCulling] No visible clusters, skipping indirect draw setup" << std::endl;
        return true;
    }

    struct IndirectDrawArgs {
        u32 vertex_count_per_instance;
        u32 instance_count;
        u32 first_vertex;
        u32 first_instance;
        u32 padding;
    };

    std::cout << "[GPUCulling] Setting up indirect draw args..." << std::endl;
    std::cout << "[GPUCulling]   indirect_args_buffer_: " << indirect_args_buffer_ << std::endl;

    // NOTE: Indirect args are set by GPU shader (stage7_build_indirect_commands) which reads the atomic counter
    // But we also set CPU-side values as fallback for debugging
    if (indirect_args_buffer_ != rhi::handles::INVALID_RESOURCE) {
        IndirectDrawArgs args{};
        // For meshlet rendering: each meshlet can have up to 384 triangles = 1152 vertices
        args.vertex_count_per_instance = 384 * 3;  // Max triangles per meshlet * 3 vertices per triangle
        args.instance_count = results_.visible_cluster_count;  // Expected: total number of clusters (3258)
        args.first_vertex = 0;
        args.first_instance = 0;

        std::cout << "[GPUCulling] CPU-side indirect draw args (will be overwritten by GPU): "
                  << args.vertex_count_per_instance << " vertices, "
                  << args.instance_count << " instances" << std::endl;

        void* mapped = device_->MapBuffer(indirect_args_buffer_);
        if (mapped) {
            memcpy(mapped, &args, sizeof(IndirectDrawArgs));
            device_->UnmapBuffer(indirect_args_buffer_);
        }
    }
    
    if (cluster_visibility_buffer_ != rhi::handles::INVALID_RESOURCE && !results_.visible_cluster_indices.empty()) {
        void* mapped = device_->MapBuffer(cluster_visibility_buffer_);
        if (mapped) {
            u32* visibilityData = static_cast<u32*>(mapped);
            memset(visibilityData, 0, config_.max_clusters_per_dispatch * sizeof(u32) * 4); // 4 fields per entry

            for (u32 idx : results_.visible_cluster_indices) {
                if (idx < config_.max_clusters_per_dispatch) {
                    visibilityData[idx * 4] = 1; // is_visible
                }
            }

            device_->UnmapBuffer(cluster_visibility_buffer_);
        }
    }

    // Fill visible cluster list buffer
    if (visible_cluster_list_buffer_ != rhi::handles::INVALID_RESOURCE && !results_.visible_cluster_indices.empty()) {
        void* mapped = device_->MapBuffer(visible_cluster_list_buffer_);
        if (mapped) {
            u32* clusterData = static_cast<u32*>(mapped);
            // Copy all visible cluster indices to the buffer
            memcpy(clusterData, results_.visible_cluster_indices.data(), results_.visible_cluster_indices.size() * sizeof(u32));
            device_->UnmapBuffer(visible_cluster_list_buffer_);
            
            std::cout << "[GPUCulling] Compacted cluster buffer updated with " << results_.visible_cluster_indices.size() << " indices" << std::endl;
        } else {
             std::cout << "[GPUCulling] Failed to map compact cluster buffer" << std::endl;
        }
    }
    
    return true;
}
void GPUCullingPipeline::UpdateResults() {
}

// Helper function to extract camera position from view matrix
math::v3 GPUCullingPipeline::extract_camera_position(const math::m4x4& view_matrix) {
    // Simply invert the view matrix to get the camera position
    math::m4x4 inv_view = rhi::math::Inverse(view_matrix);
    math::v3 camera_pos;
    camera_pos.x = inv_view.columns[3][0];
    camera_pos.y = inv_view.columns[3][1];
    camera_pos.z = inv_view.columns[3][2];
    return camera_pos;
}

// Update culling descriptor set with current frame data
bool GPUCullingPipeline::UpdateCullingDescriptorSet(const RenderSceneSnapshot& snapshot,
                                                   const math::m4x4& view_matrix,
                                                   const math::m4x4& projection_matrix,
                                                   const math::m4x4& view_projection,
                                                   const math::v3& camera_position,
                                                   u32 frame_index) {
    static bool descriptor_set_initialized = false;

    if (culling_descriptor_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) {
        rhi::DescriptorSetDesc desc{};
        desc.layout = culling_descriptor_layout_;
        culling_descriptor_set_ = device_->CreateDescriptorSet(desc);
    }

    if (culling_descriptor_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) {
        std::cerr << "Failed to create culling descriptor set" << std::endl;
        return false;
    }

    // Update culling constants
    struct CullingConstants {
        math::m4x4 view_matrix;
        math::m4x4 projection_matrix;
        math::m4x4 view_projection_matrix;
        math::v3 camera_position;
        float near_plane;
        float far_plane;
        u32 frame_index;
        u32 enable_occlusion_culling;
        u32 enable_lod_selection;
        u32 enable_small_object_culling;
        float small_object_threshold;
        float lod_bias;
        u32 max_lod_levels;
        u32 instance_count;
        u32 cluster_count;
        u32 padding[2]; // Match Metal shader struct layout
    } constants;

    constants.view_matrix = view_matrix;
    constants.projection_matrix = projection_matrix;
    constants.view_projection_matrix = view_projection;
    constants.camera_position = camera_position;
    constants.near_plane = 0.1f;
    constants.far_plane = 1000.0f;
    constants.frame_index = frame_index;
    constants.enable_occlusion_culling = config_.enable_occlusion_culling ? 1 : 0;
    constants.enable_lod_selection = config_.enable_lod_selection ? 1 : 0;
    constants.enable_small_object_culling = config_.enable_small_object_culling ? 1 : 0;
    constants.small_object_threshold = config_.small_object_threshold;
    constants.lod_bias = config_.lod_bias;
    constants.max_lod_levels = config_.max_lod_levels;
    constants.instance_count = snapshot.GetInstanceCount();
    constants.cluster_count = snapshot.GetClusterRefCount();
    constants.padding[0] = 0;
    constants.padding[1] = 0;

    // Only update culling constants buffer every frame (this is the only thing that changes)
    if (culling_constants_buffer_ != rhi::handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(culling_constants_buffer_);
        if (mapped) {
            memcpy(mapped, &constants, sizeof(CullingConstants));
            device_->UnmapBuffer(culling_constants_buffer_);
        }
    }

    // Only initialize descriptor set bindings once (not every frame!)
    if (!descriptor_set_initialized) {
        std::cout << "[GPUCulling] Initializing descriptor set bindings (once)" << std::endl;

        // Update descriptor set bindings
        rhi::WriteDescriptorSet writes[10]; // Need space for all 10 bindings (0-9)
        rhi::DescriptorBufferInfo bufferInfos[10];
        rhi::DescriptorImageInfo imageInfo;

        u32 writeCount = 0;

        // Binding 0: Instance data
        bufferInfos[writeCount].buffer = snapshot.GetInstanceBuffer();
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = ~0ull;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 0;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 1: Instance bounds (will be populated from geometry data)
        bufferInfos[writeCount].buffer = instance_bounds_buffer_;
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = ~0ull;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 1;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 2: Instance visibility
        bufferInfos[writeCount].buffer = instance_visibility_buffer_;
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = ~0ull;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 2;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 3: Culling constants (IMPORTANT: Metal shader expects this at binding 3!)
        bufferInfos[writeCount].buffer = culling_constants_buffer_;
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = sizeof(CullingConstants);
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 3;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::UniformBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 4: Cluster refs
        bufferInfos[writeCount].buffer = snapshot.GetClusterRefBuffer();
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = ~0ull;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 4;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 5: Cluster visibility
        bufferInfos[writeCount].buffer = cluster_visibility_buffer_;
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = ~0ull;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 5;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 6: Visible counter
        bufferInfos[writeCount].buffer = visible_counter_buffer_;
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = sizeof(u32);
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 6;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 7: Visible cluster list
        bufferInfos[writeCount].buffer = visible_cluster_list_buffer_;
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = ~0ull;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 7;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 8: Indirect commands
        bufferInfos[writeCount].buffer = indirect_args_buffer_;
        bufferInfos[writeCount].offset = 0;
        bufferInfos[writeCount].range = sizeof(u32) * 5;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 8;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[writeCount].bufferInfo = &bufferInfos[writeCount];
        writeCount++;

        // Binding 9: HZB texture (placeholder)
        // TODO: Implement proper HZB texture binding
        imageInfo.imageView = hiz_buffer_;
        imageInfo.imageLayout = rhi::ResourceState::ShaderResource;
        writes[writeCount].dstSet = culling_descriptor_set_;
        writes[writeCount].dstBinding = 9;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = rhi::DescriptorType::SampledImage;
        writes[writeCount].imageInfo = &imageInfo;
        writeCount++;

        device_->UpdateDescriptorSets(writeCount, writes);
        descriptor_set_initialized = true;
    }

    return true;
}

// Additional stage implementations
bool GPUCullingPipeline::Stage2_DistanceCulling(rhi::RHICommandBuffer* cmdBuffer,
                                                const RenderSceneSnapshot& snapshot,
                                                u32 frameIndex) {
    if (distance_culling_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cout << "[GPUCulling] Distance culling pipeline not available, skipping" << std::endl;
        return true;
    }

    const u32 instanceCount = snapshot.GetInstanceCount();
    if (instanceCount == 0) return true;

    cmdBuffer->BindComputePipeline(distance_culling_pipeline_);

    if (culling_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        const rhi::DescriptorSetHandle descriptor_sets[] = { culling_descriptor_set_ };
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       culling_pipeline_layout_,
                                       0, 1, descriptor_sets,
                                       0, nullptr);
    }

    u32 threadGroups = (instanceCount + 63) / 64;
    cmdBuffer->Dispatch(threadGroups, 1, 1);

    return true;
}

bool GPUCullingPipeline::Stage3_LODSelection(rhi::RHICommandBuffer* cmdBuffer,
                                            const RenderSceneSnapshot& snapshot,
                                            const math::m4x4& view_matrix,
                                            const math::v3& camera_position,
                                            u32 frameIndex) {
    if (lod_selection_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cout << "[GPUCulling] LOD selection pipeline not available, skipping" << std::endl;
        return true;
    }

    const u32 instanceCount = snapshot.GetInstanceCount();
    if (instanceCount == 0) return true;

    cmdBuffer->BindComputePipeline(lod_selection_pipeline_);

    if (culling_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        const rhi::DescriptorSetHandle descriptor_sets[] = { culling_descriptor_set_ };
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       culling_pipeline_layout_,
                                       0, 1, descriptor_sets,
                                       0, nullptr);
    }

    u32 threadGroups = (instanceCount + 63) / 64;
    cmdBuffer->Dispatch(threadGroups, 1, 1);

    return true;
}

bool GPUCullingPipeline::Stage4_ClusterExpansion(rhi::RHICommandBuffer* cmdBuffer,
                                                 const RenderSceneSnapshot& snapshot,
                                                 u32 frameIndex) {
    if (cluster_expansion_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cout << "[GPUCulling] Cluster expansion pipeline not available, skipping" << std::endl;
        return true;
    }

    const u32 instanceCount = snapshot.GetInstanceCount();
    if (instanceCount == 0) return true;

    cmdBuffer->BindComputePipeline(cluster_expansion_pipeline_);

    if (culling_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        const rhi::DescriptorSetHandle descriptor_sets[] = { culling_descriptor_set_ };
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       culling_pipeline_layout_,
                                       0, 1, descriptor_sets,
                                       0, nullptr);
    }

    u32 threadGroups = (instanceCount + 63) / 64;
    cmdBuffer->Dispatch(threadGroups, 1, 1);

    return true;
}

bool GPUCullingPipeline::Stage5_OcclusionCulling(rhi::RHICommandBuffer* cmdBuffer,
                                                 const RenderSceneSnapshot& snapshot,
                                                 u32 frameIndex) {
    if (occlusion_culling_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cout << "[GPUCulling] Occlusion culling pipeline not available, skipping" << std::endl;
        return true;
    }

    const u32 clusterCount = snapshot.GetClusterRefCount();
    if (clusterCount == 0) return true;

    cmdBuffer->BindComputePipeline(occlusion_culling_pipeline_);

    if (culling_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        const rhi::DescriptorSetHandle descriptor_sets[] = { culling_descriptor_set_ };
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       culling_pipeline_layout_,
                                       0, 1, descriptor_sets,
                                       0, nullptr);
    }

    u32 threadGroups = (clusterCount + 63) / 64;
    cmdBuffer->Dispatch(threadGroups, 1, 1);

    return true;
}

bool GPUCullingPipeline::Stage6_Compaction(rhi::RHICommandBuffer* cmdBuffer,
                                          const RenderSceneSnapshot& snapshot,
                                          u32 frameIndex) {
    if (compaction_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cout << "[GPUCulling] Compaction pipeline not available, skipping" << std::endl;
        return true;
    }

    const u32 instanceCount = snapshot.GetInstanceCount();
    if (instanceCount == 0) return true;

    cmdBuffer->BindComputePipeline(compaction_pipeline_);

    if (culling_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        const rhi::DescriptorSetHandle descriptor_sets[] = { culling_descriptor_set_ };
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       culling_pipeline_layout_,
                                       0, 1, descriptor_sets,
                                       0, nullptr);
    }

    // We need to process the full visibility array range: instance_count * 256
    // This handles the sparse distribution where clusters are stored at instance_id * 256 + cluster_offset
    const u32 visibilityArraySize = instanceCount * 256;
    u32 threadGroups = (visibilityArraySize + 63) / 64;

    std::cout << "[GPUCulling] Stage6 Compaction: processing " << visibilityArraySize
              << " visibility entries (" << instanceCount << " instances * 256)" << std::endl;

    cmdBuffer->Dispatch(threadGroups, 1, 1);

    return true;
}

bool GPUCullingPipeline::Stage7_BuildIndirectCommands(rhi::RHICommandBuffer* cmdBuffer,
                                                     const RenderSceneSnapshot& snapshot,
                                                     u32 frameIndex) {
    if (indirect_command_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cout << "[GPUCulling] Indirect command pipeline not available, skipping" << std::endl;
        return true;
    }

    cmdBuffer->BindComputePipeline(indirect_command_pipeline_);

    if (culling_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        const rhi::DescriptorSetHandle descriptor_sets[] = { culling_descriptor_set_ };
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                       culling_pipeline_layout_,
                                       0, 1, descriptor_sets,
                                       0, nullptr);
    }

    // Single thread to build the indirect command
    cmdBuffer->Dispatch(1, 1, 1);

    return true;
}

// Read back results for debugging
void GPUCullingPipeline::ReadbackResults(const RenderSceneSnapshot& snapshot, u32 frameIndex) {
    // Read from debug readback buffer (GPU has already copied data here)
    if (debug_readback_buffer_ != rhi::handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(debug_readback_buffer_);
        if (mapped) {
            u32* visibilityData = static_cast<u32*>(mapped);
            u32 visibleInstances = 0;
            u32 instanceCount = snapshot.GetInstanceCount();

            // DEBUG: Print first few entries to see what GPU wrote
            std::cout << "[GPUCulling] DEBUG - InstanceVisibility struct size: " << sizeof(u32) * 4 << " bytes" << std::endl;
            std::cout << "[GPUCulling] DEBUG - First 5 instance visibility entries:" << std::endl;
            for (u32 i = 0; i < 5 && i < instanceCount; i++) {
                std::cout << "  Instance " << i << ": is_visible=" << visibilityData[i * 4]
                          << ", instance_index=" << visibilityData[i * 4 + 1]
                          << ", lod_level=" << visibilityData[i * 4 + 2]
                          << ", distance_rank=" << visibilityData[i * 4 + 3] << std::endl;
            }

            // DEBUG: Print raw memory for first instance to check alignment
            std::cout << "[GPUCulling] DEBUG - Raw memory dump (first 20 u32 values):" << std::endl;
            for (u32 i = 0; i < 20; i++) {
                std::cout << "  [" << i << "] = " << visibilityData[i];
                if (i % 4 == 3) std::cout << " (end of struct)" << std::endl;
                else std::cout << ", ";
            }

            // Count visible instances (using magic number detection for debugging)
            for (u32 i = 0; i < instanceCount; i++) {
                u32 isVisible = visibilityData[i * 4];
                // Check for magic number OR actual visibility
                if (isVisible == 0xDEADBEEF || isVisible == 1) {
                    visibleInstances++;
                }
            }
            results_.visible_instance_count = visibleInstances;
            device_->UnmapBuffer(debug_readback_buffer_);

            std::cout << "[GPUCulling] Frame " << frameIndex << ": " << visibleInstances << "/" << instanceCount << " instances visible (from GPU)" << std::endl;
        }
    }

    // Read visible counter (atomic counter should also work)
    if (visible_counter_buffer_ != rhi::handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(visible_counter_buffer_);
        if (mapped) {
            u32 visibleCount = *static_cast<u32*>(mapped);
            results_.visible_cluster_count = visibleCount;
            device_->UnmapBuffer(visible_counter_buffer_);

            std::cout << "[GPUCulling] Frame " << frameIndex << ": " << visibleCount << " clusters visible (atomic counter)" << std::endl;
        }
    }

    // Update results buffers for next stage
    results_.indirect_args_buffer = indirect_args_buffer_;
    results_.instance_visibility_buffer = instance_visibility_buffer_;
    results_.cluster_visibility_buffer = cluster_visibility_buffer_;
    results_.visible_cluster_list_buffer = visible_cluster_list_buffer_;
}

}
