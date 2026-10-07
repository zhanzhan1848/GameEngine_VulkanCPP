// Default-off, read-only first-triangle oracle for the DDGI source audit.
//
// This file is included inside TestVulkanSponzaRenderGraph's anonymous
// namespace.  It consumes the same-frame GPU source tail and update-list
// buffers directly.  The BVH is an explicitly labelled diagnostic upload;
// it is not a replacement for the production trace or a claim that the
// CPU asset is the native raster geometry.

struct DDGIFirstOracleMappingRecord {
    u32 global_vertex_ids[3];
    u32 instance_index;
};
static_assert(sizeof(DDGIFirstOracleMappingRecord) == 16,
              "default first oracle mapping ABI changed");

static u32 DDGIFirstOracleFloatBits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool DDGIFirstOracleFinite(float value) {
    const u32 bits = DDGIFirstOracleFloatBits(value);
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static bool DDGIFirstOracleFinite3(const float* value) {
    return DDGIFirstOracleFinite(value[0]) &&
           DDGIFirstOracleFinite(value[1]) &&
           DDGIFirstOracleFinite(value[2]);
}

static u64 DDGIFirstOracleFNV1a(const void* data, u64 bytes) {
    const auto* p = static_cast<const u8*>(data);
    u64 hash = 14695981039346656037ull;
    for (u64 i = 0; i < bytes; ++i) {
        hash ^= u64(p[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static std::string DDGIFirstOracleBaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1u);
}

static bool DDGIFirstOracleEnvIsOne(const char* name) {
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

static bool DDGIFirstOracleBuildScene(
    const primal::utl::vector<SceneDataMeshInfo>& sceneMeshes,
    const nanite::GPUMaterialRegistry* materialRegistry,
    const primal::utl::vector<RenderProxy>& proxies,
    std::vector<SCProofTriangle>& original,
    std::vector<SCProofTriangle>& triangles,
    std::vector<SCProofNode>& nodes,
    std::vector<u8>& nativeMapping,
    std::string& error) {
    original.clear();
    triangles.clear();
    nodes.clear();
    nativeMapping.clear();
    error.clear();
    if (sceneMeshes.empty() || !materialRegistry ||
        materialRegistry->GetMaterialCount() == 0u) {
        error = "scene mesh list is empty";
        return false;
    }

    // This is the same local-position/identity-scene contract used by the
    // earlier first-surface mapping helper.  No UV or material upload data is
    // read here; only positions and indices needed for the diagnostic BVH.
    for (u32 meshIndex = 0; meshIndex < sceneMeshes.size(); ++meshIndex) {
        const auto& info = sceneMeshes[meshIndex];
        const auto proxyIt = std::find_if(
            proxies.begin(), proxies.end(), [&](const auto& proxy) {
                return proxy.entityId == info.meshEntityId;
            });
        if (proxyIt == proxies.end() ||
            info.gpuMaterialId >= materialRegistry->GetMaterialCount()) {
            error = "scene proxy/material identity is unavailable";
            return false;
        }
        const auto identity = rhimath::MatrixIdentity();
        const float* actualMatrix =
            reinterpret_cast<const float*>(&proxyIt->transform);
        const float* expectedMatrix = reinterpret_cast<const float*>(&identity);
        static_assert(sizeof(identity) == 16u * sizeof(float));
        for (u32 k = 0; k < 16u; ++k) {
            if (!DDGIFirstOracleFinite(actualMatrix[k]) ||
                actualMatrix[k] != expectedMatrix[k]) {
                error = "diagnostic oracle requires the accepted identity scene transform";
                return false;
            }
        }
        rhi::RHIMeshAsset asset{};
        if (!primal::content::get_rhi_mesh_asset(
                info.meshEntityId, asset) ||
            (asset.index_size != 2u && asset.index_size != 4u) ||
            asset.num_vertices == 0u || asset.num_indices == 0u ||
            (asset.num_indices % 3u) != 0u ||
            asset.position_buffer.size() < size_t(asset.num_vertices) * 12u ||
            asset.index_buffer.size() <
                size_t(asset.num_indices) * asset.index_size) {
            error = "scene mesh positions or indices are unavailable";
            return false;
        }
        const u32 triangleCount = asset.num_indices / 3u;
        for (u32 triangleIndex = 0; triangleIndex < triangleCount;
             ++triangleIndex) {
            auto indexAt = [&](u32 index) {
                u32 value = 0u;
                std::memcpy(&value,
                            asset.index_buffer.data() +
                                size_t(index) * asset.index_size,
                            asset.index_size);
                return value;
            };
            const u32 i0 = indexAt(triangleIndex * 3u + 0u);
            const u32 i1 = indexAt(triangleIndex * 3u + 1u);
            const u32 i2 = indexAt(triangleIndex * 3u + 2u);
            if (i0 >= asset.num_vertices || i1 >= asset.num_vertices ||
                i2 >= asset.num_vertices) {
                error = "scene mesh index is outside the position stream";
                return false;
            }
            float vertex[3][3]{};
            std::memcpy(vertex[0], asset.position_buffer.data() + size_t(i0) * 12u, 12u);
            std::memcpy(vertex[1], asset.position_buffer.data() + size_t(i1) * 12u, 12u);
            std::memcpy(vertex[2], asset.position_buffer.data() + size_t(i2) * 12u, 12u);
            for (u32 k = 0; k < 3u; ++k) {
                if (!DDGIFirstOracleFinite3(vertex[k])) {
                    error = "scene mesh position is nonfinite";
                    return false;
                }
            }
            SCProofTriangle item{};
            for (u32 k = 0; k < 3u; ++k) {
                item.a[k] = vertex[0][k];
                item.e1[k] = vertex[1][k] - vertex[0][k];
                item.e2[k] = vertex[2][k] - vertex[0][k];
                if (!DDGIFirstOracleFinite(item.e1[k]) ||
                    !DDGIFirstOracleFinite(item.e2[k])) {
                    error = "scene mesh edge is nonfinite";
                    return false;
                }
            }
            item.a[3] = static_cast<float>(original.size());
            u32 flags = materialRegistry->GetMaterialData()[info.gpuMaterialId].flags;
            if (proxyIt->technique != ShaderTechnique::Opaque ||
                proxyIt->base_color[3] != 1.0f || proxyIt->graphVariant != 0u) {
                flags |= 0x80000000u;
            }
            std::memcpy(&item.e1[3], &flags, sizeof(flags));
            item.e2[3] = static_cast<float>(meshIndex);
            original.push_back(item);
        }
    }
    if (original.empty() || original.size() > 16u * 1024u * 1024u) {
        error = "diagnostic scene triangle count is outside the bounded range";
        return false;
    }

    // BuildDDGIFirstNativeTriangleMapping is the existing scene-order
    // mapping used by the native-normal proof.  Preserve it as a separate
    // CPU mapping artifact; it is not bound as an oracle input and is never
    // used to fabricate a GPU ray or a native source result.
    if (!BuildDDGIFirstNativeTriangleMapping(
            sceneMeshes, original, nativeMapping, error)) {
        return false;
    }
    BuildDDGIFirstSurfaceBVH(original, triangles, nodes);
    if (triangles.empty() || nodes.empty()) {
        error = "diagnostic BVH construction returned no records";
        return false;
    }
    if (triangles.size() > std::numeric_limits<u32>::max() ||
        nodes.size() > std::numeric_limits<u32>::max()) {
        error = "diagnostic BVH record count overflows shader indexing";
        return false;
    }
    return true;
}

static bool RunDDGIFirstDefaultOracle(
    RHIDeviceBase* device,
    lumen::LumenDDGIPass* pass,
    u32 frame,
    const std::vector<u32>& updateList,
    const std::vector<math::v4>& raySourceRecords,
    u32 fullRayCapacity,
    u32 raysPerProbe,
    const primal::utl::vector<SceneDataMeshInfo>& sceneMeshes,
    const nanite::GPUMaterialRegistry* materialRegistry,
    const primal::utl::vector<RenderProxy>& proxies) {
    const char* prefixValue = std::getenv("DDGI_DEFAULT_FIRST_ORACLE_PREFIX");
    if (!prefixValue || !prefixValue[0]) return true;
    const std::string prefix(prefixValue);
    bool rawEvidenceSaved = false;

    auto writeFailure = [&](const char* why) {
        std::ofstream meta(prefix + "_meta.json", std::ios::trunc);
        if (meta) {
            meta << "{\"schema\":\"ddgi_default_first_oracle_v1\","
                    "\"complete\":false,\"readback_completed\":false,"
                    "\"explicit_buffer_dependencies\":false,"
                    "\"begin_end_submit_checked\":false,"
                    "\"completion_checked\":false,\"map_after_completion\":false,"
                    "\"raw_evidence_preserved\":"
                 << (rawEvidenceSaved ? "true" : "false")
                 << ",\"error\":\"" << why << "\"}\n";
        }
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_DEFAULT_FIRST_ORACLE] rejected: " << why << std::endl;
        return false;
    };

    const char* dump = std::getenv("DDGI_DUMP");
    const char* sourceAudit = std::getenv("DDGI_RAY_SOURCE_AUDIT");
    if (!device || !pass || device->GetPlatform() != RHIPlatform::Vulkan ||
        !dump || std::strcmp(dump, "2") != 0 || !sourceAudit ||
        std::strcmp(sourceAudit, "1") != 0 ||
        DDGIFirstOracleEnvIsOne("DDGI_FIRST_SURFACE") ||
        DDGIFirstOracleEnvIsOne("DDGI_FIRST_NATIVE_NORMAL")) {
        return writeFailure(
            "requires Vulkan DDGI_DUMP=2 and DDGI_RAY_SOURCE_AUDIT=1 with first-surface candidates off");
    }

    const auto& volume = pass->GetVolumeData();
    if (volume.RaySampleMode != 0 || raysPerProbe != 128u ||
        volume.RaysPerProbe != raysPerProbe || fullRayCapacity == 0u ||
        volume.ProbeCountTotal != fullRayCapacity / raysPerProbe ||
        fullRayCapacity % raysPerProbe != 0u ||
        updateList.empty() ||
        u64(updateList.size()) * raysPerProbe > 32768u ||
        u64(updateList.size()) * raysPerProbe > fullRayCapacity ||
        raySourceRecords.size() != size_t(fullRayCapacity) * 4u) {
        return writeFailure("unsupported scalar, capacity, update-list, or source-tail contract");
    }
    if (frame != volume.FrameIndex) {
        return writeFailure("dump frame differs from the uploaded DDGI frame");
    }
    std::vector<u32> sortedUpdates(updateList);
    std::sort(sortedUpdates.begin(), sortedUpdates.end());
    if (std::adjacent_find(sortedUpdates.begin(), sortedUpdates.end()) !=
            sortedUpdates.end() ||
        sortedUpdates.back() >= volume.ProbeCountTotal) {
        return writeFailure("actual update list is not unique and bounded");
    }
    for (const u32 target : {409u, 410u, 507u}) {
        if (!std::binary_search(sortedUpdates.begin(), sortedUpdates.end(), target)) {
            return writeFailure("oracle target probe is absent from the actual update list");
        }
    }

    auto* vk = static_cast<VulkanDevice*>(device);
    const ResourceHandle hitNormal = pass->GetHitNormalBuffer();
    const ResourceHandle updateBuffer = pass->GetProbeUpdateListBuffer(frame);
    const ResourceHandle hitDistance = pass->GetHitDistanceBuffer();
    const ResourceHandle rayData = pass->GetRayDataBuffer();
    const auto slots = lumen::DDGISlotsForFrame(frame);
    const ResourceHandle currentSH =
        pass->GetIrradianceBufferBySlot(slots.currentIndex);
    u64 hitNormalBytes = 0;
    u64 updateBytes = 0;
    const u64 expectedHitNormalMinBytes =
        u64(fullRayCapacity) * 9u * sizeof(math::v4);
    const u64 expectedHitNormalExtentBytes =
        u64(fullRayCapacity) * 27u * sizeof(math::v4);
    if (hitNormal == handles::INVALID_RESOURCE ||
        updateBuffer == handles::INVALID_RESOURCE ||
        !vk->GetBufferByteSize(hitNormal, hitNormalBytes) ||
        !vk->GetBufferByteSize(updateBuffer, updateBytes) ||
        hitNormalBytes < expectedHitNormalMinBytes ||
        hitNormalBytes % sizeof(math::v4) != 0u ||
        updateBytes < u64(updateList.size()) * sizeof(u32) ||
        updateBytes % sizeof(u32) != 0u) {
        return writeFailure("actual source-tail/update-list resource extent is invalid");
    }

    struct Pair {
        const char* name;
        ResourceHandle source;
        u64 bytes;
        ResourceHandle before{handles::INVALID_RESOURCE};
        ResourceHandle after{handles::INVALID_RESOURCE};
        std::vector<u8> beforeBytes;
        std::vector<u8> afterBytes;
    };
    std::vector<Pair> pairs;
    pairs.push_back({"hit_distance", hitDistance, 0});
    pairs.push_back({"hit_normal", hitNormal, hitNormalBytes});
    pairs.push_back({"ray_data", rayData, 0});
    pairs.push_back({"current_sh", currentSH, 0});
    pairs.push_back({"update_list", updateBuffer, updateBytes});
    u64 totalPairBytes = 0;
    for (auto& pair : pairs) {
        if (pair.source == handles::INVALID_RESOURCE ||
            !vk->GetBufferByteSize(pair.source, pair.bytes) || pair.bytes == 0u ||
            pair.bytes > 256ull * 1024ull * 1024ull ||
            totalPairBytes > 512ull * 1024ull * 1024ull - pair.bytes * 2u) {
            return writeFailure("production pair resource handle, size, or aggregate cap is invalid");
        }
        totalPairBytes += pair.bytes * 2u;
    }

    std::vector<SCProofTriangle> originalScene;
    std::vector<SCProofTriangle> triangles;
    std::vector<SCProofNode> nodes;
    std::vector<u8> nativeMapping;
    std::string sceneError;
    if (!DDGIFirstOracleBuildScene(sceneMeshes, materialRegistry, proxies,
                                   originalScene, triangles, nodes,
                                   nativeMapping, sceneError)) {
        return writeFailure(sceneError.c_str());
    }

    // Record the actual resident native source identities while keeping them
    // out of the diagnostic descriptor. Existing SC source snapshots remain
    // the independent native-byte evidence; this oracle never treats the CPU
    // scene upload as a native GPU geometry certificate.
    auto& draw = nanite::GPUDrivenDrawPipeline::Get();
    const ResourceHandle nativeSources[4] = {
        draw.GetGlobalVertexBuffer(), draw.GetGlobalElementBuffer(),
        draw.GetGlobalInstanceDataBuffer(), draw.GetMaterialDataBuffer()};
    for (const ResourceHandle handle : nativeSources) {
        if (handle == handles::INVALID_RESOURCE)
            return writeFailure("resident native source handle is unavailable");
    }

    const u64 nodeBytes = u64(nodes.size()) * sizeof(SCProofNode);
    const u64 triangleBytes = u64(triangles.size()) * sizeof(SCProofTriangle);
    const u32 freshRayCount = static_cast<u32>(updateList.size()) * raysPerProbe;
    const u64 outputBytes = u64(freshRayCount) * 12u * sizeof(math::v4);
    if (!nodeBytes || !triangleBytes || nodeBytes > 256ull * 1024ull * 1024ull ||
        triangleBytes > 256ull * 1024ull * 1024ull || outputBytes == 0u ||
        outputBytes > 6ull * 1024ull * 1024ull) {
        return writeFailure("diagnostic BVH or output allocation exceeds the bounded cap");
    }

    struct OracleResources {
        RHIDeviceBase* device{nullptr};
        std::vector<ResourceHandle> buffers;
        ShaderHandle shader{handles::INVALID_SHADER};
        DescriptorSetLayoutHandle layout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        PipelineLayoutHandle pipelineLayout{handles::INVALID_PIPELINE_LAYOUT};
        DescriptorSetHandle descriptorSet{handles::INVALID_DESCRIPTOR_SET};
        PipelineHandle pipeline{handles::INVALID_PIPELINE};
        CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ~OracleResources() {
            if (!device) return;
            if (command != handles::INVALID_COMMAND_BUFFER)
                device->DestroyCommandBuffer(command);
            if (pipeline != handles::INVALID_PIPELINE)
                device->DestroyPipeline(pipeline);
            if (descriptorSet != handles::INVALID_DESCRIPTOR_SET)
                device->DestroyDescriptorSet(descriptorSet);
            if (pipelineLayout != handles::INVALID_PIPELINE_LAYOUT)
                device->DestroyPipelineLayout(pipelineLayout);
            if (layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
                device->DestroyDescriptorSetLayout(layout);
            if (shader != handles::INVALID_SHADER)
                device->DestroyShader(shader);
            for (const ResourceHandle buffer : buffers) {
                if (buffer != handles::INVALID_RESOURCE)
                    device->DestroyBuffer(buffer);
            }
        }
    } resources;
    resources.device = device;
    auto createBuffer = [&](const BufferDesc& desc) {
        const ResourceHandle handle = device->CreateBuffer(desc);
        if (handle == handles::INVALID_RESOURCE) return handle;
        resources.buffers.push_back(handle);
        return handle;
    };
    auto makeStructured = [&](u64 bytes, u32 count, u32 stride, const char* name) {
        BufferDesc desc{};
        desc.size = bytes;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = count;
        desc.structured.elementStride = stride;
        desc.name = name;
        return createBuffer(desc);
    };
    auto makeReadback = [&](u64 bytes, const char* name) {
        BufferDesc desc{};
        desc.size = bytes;
        desc.type = BufferType::Raw;
        desc.usage = GPUMemoryUsage::Readback;
        desc.memoryUsage = GPUMemoryUsage::Readback;
        desc.name = name;
        return createBuffer(desc);
    };

    BufferDesc volumeDesc{};
    volumeDesc.size = 512u;
    volumeDesc.type = BufferType::Constant;
    volumeDesc.usage = GPUMemoryUsage::Dynamic;
    volumeDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    volumeDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    volumeDesc.name = "DDGIFirstOracleVolume";
    const ResourceHandle volumeBuffer = createBuffer(volumeDesc);
    const ResourceHandle nodeBuffer = makeStructured(nodeBytes, u32(nodes.size()),
                                                      sizeof(SCProofNode),
                                                      "DDGIFirstOracleNodes");
    const ResourceHandle triangleBuffer = makeStructured(
        triangleBytes, u32(triangles.size()), sizeof(SCProofTriangle),
        "DDGIFirstOracleTriangles");
    const ResourceHandle outputBuffer = makeStructured(
        outputBytes, freshRayCount * 12u, sizeof(math::v4),
        "DDGIFirstOracleOutput");
    if (volumeBuffer == handles::INVALID_RESOURCE ||
        nodeBuffer == handles::INVALID_RESOURCE ||
        triangleBuffer == handles::INVALID_RESOURCE ||
        outputBuffer == handles::INVALID_RESOURCE) {
        return writeFailure("diagnostic input/output allocation failed");
    }
    std::vector<u8> zeroOutput(size_t(outputBytes), 0u);
    auto upload = [&](ResourceHandle buffer, const void* data, u64 bytes) {
        return device->UpdateBufferData(buffer, data, bytes, 0u);
    };
    lumen::DDGIVolumeData oracleVolume = volume;
    oracleVolume.ProbeUpdateCount = static_cast<u32>(updateList.size());
    if (!upload(volumeBuffer, &oracleVolume, sizeof(oracleVolume)) ||
        !upload(nodeBuffer, nodes.data(), nodeBytes) ||
        !upload(triangleBuffer, triangles.data(), triangleBytes) ||
        !upload(outputBuffer, zeroOutput.data(), outputBytes)) {
        return writeFailure("diagnostic input/output upload failed");
    }

    const std::vector<u8> shaderBytes = LoadSpvBytes(
        "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIDefaultFirstOracle.comp.spv");
    if (shaderBytes.empty()) return writeFailure("default first oracle SPIR-V is unavailable");
    {
        std::ofstream shaderFile(prefix + ".shader.spv", std::ios::binary | std::ios::trunc);
        shaderFile.write(reinterpret_cast<const char*>(shaderBytes.data()),
                         std::streamsize(shaderBytes.size()));
        shaderFile.close();
        if (!shaderFile) return writeFailure("oracle SPIR-V evidence write failed");
    }
    resources.shader = device->CreateShader(
        shaderBytes.data(), shaderBytes.size(), ShaderStage::Compute,
        "ddgi_default_first_oracle");
    if (resources.shader == handles::INVALID_SHADER)
        return writeFailure("default first oracle shader creation failed");

    DescriptorSetLayoutBinding bindings[6]{};
    bindings[0] = {3u, DescriptorType::UniformBuffer, 1u,
                   ShaderStage::Compute, nullptr};
    bindings[1] = {5u, DescriptorType::StorageBuffer, 1u,
                   ShaderStage::Compute, nullptr};
    bindings[2] = {6u, DescriptorType::StorageBuffer, 1u,
                   ShaderStage::Compute, nullptr};
    bindings[3] = {13u, DescriptorType::StorageBuffer, 1u,
                   ShaderStage::Compute, nullptr};
    bindings[4] = {14u, DescriptorType::StorageBuffer, 1u,
                   ShaderStage::Compute, nullptr};
    bindings[5] = {15u, DescriptorType::StorageBuffer, 1u,
                   ShaderStage::Compute, nullptr};
    for (auto& binding : bindings) binding.readonly = true;
    bindings[5].readonly = false;
    const DescriptorSetLayoutDesc layoutDesc{6u, bindings};
    resources.layout = device->CreateDescriptorSetLayout(layoutDesc);
    if (resources.layout == handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        return writeFailure("default first oracle descriptor layout failed");
    PipelineLayoutDesc pipelineLayoutDesc{};
    pipelineLayoutDesc.setLayoutCount = 1u;
    pipelineLayoutDesc.setLayouts = &resources.layout;
    resources.pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    if (resources.pipelineLayout == handles::INVALID_PIPELINE_LAYOUT)
        return writeFailure("default first oracle pipeline layout failed");
    DescriptorSetDesc setDesc{};
    setDesc.layout = resources.layout;
    resources.descriptorSet = device->CreateDescriptorSet(setDesc);
    if (resources.descriptorSet == handles::INVALID_DESCRIPTOR_SET)
        return writeFailure("default first oracle descriptor set failed");

    DescriptorBufferInfo descriptorBuffers[6] = {
        {volumeBuffer, 0u, 512u},
        {updateBuffer, 0u, updateBytes},
        {hitNormal, 0u, hitNormalBytes},
        {nodeBuffer, 0u, nodeBytes},
        {triangleBuffer, 0u, triangleBytes},
        {outputBuffer, 0u, outputBytes},
    };
    WriteDescriptorSet writes[6]{};
    for (u32 i = 0; i < 6u; ++i) {
        writes[i] = {resources.descriptorSet, bindings[i].binding, 0u, 1u,
                     bindings[i].descriptorType, nullptr, &descriptorBuffers[i]};
    }
    device->UpdateDescriptorSets(6u, writes);
    ComputePipelineDesc computeDesc{};
    computeDesc.computeShader = resources.shader;
    computeDesc.layout = resources.pipelineLayout;
    computeDesc.threadGroupSize = {64u, 1u, 1u};
    resources.pipeline = device->CreateComputePipeline(computeDesc);
    resources.command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (resources.pipeline == handles::INVALID_PIPELINE ||
        resources.command == handles::INVALID_COMMAND_BUFFER)
        return writeFailure("default first oracle pipeline or command allocation failed");

    for (auto& pair : pairs) {
        pair.before = makeReadback(pair.bytes, "DDGIFirstOraclePairBefore");
        pair.after = makeReadback(pair.bytes, "DDGIFirstOraclePairAfter");
        if (pair.before == handles::INVALID_RESOURCE ||
            pair.after == handles::INVALID_RESOURCE)
            return writeFailure("production pair readback allocation failed");
    }
    const ResourceHandle outputReadback = makeReadback(
        outputBytes, "DDGIFirstOracleOutputReadback");
    const ResourceHandle nodeReadback = makeReadback(nodeBytes,
                                                     "DDGIFirstOracleNodeReadback");
    const ResourceHandle triangleReadback = makeReadback(
        triangleBytes, "DDGIFirstOracleTriangleReadback");
    if (outputReadback == handles::INVALID_RESOURCE ||
        nodeReadback == handles::INVALID_RESOURCE ||
        triangleReadback == handles::INVALID_RESOURCE)
        return writeFailure("diagnostic readback allocation failed");

    auto* command = vk->GetCommandBuffer(resources.command);
    if (!command || !command->Reset() || !command->Begin())
        return writeFailure("oracle Reset/Begin failed");
    // The previous render/compute work and the host uploads are explicit
    // sources. InsertBarrier is not used for these buffers because this RHI
    // only emits image barriers there.
    command->MemoryBarrier(
        PipelineStage::Host | PipelineStage::AllCommands |
            PipelineStage::ComputeShader | PipelineStage::Transfer |
            PipelineStage::ColorAttachmentOutput,
        PipelineStage::Transfer,
        AccessFlag::HostWrite | AccessFlag::ShaderRead |
            AccessFlag::ShaderWrite | AccessFlag::TransferWrite |
            AccessFlag::ColorAttachmentWrite | AccessFlag::MemoryWrite,
        AccessFlag::TransferRead);
    for (const auto& pair : pairs)
        command->CopyBuffer(pair.source, pair.before, 0u, 0u, pair.bytes);
    command->MemoryBarrier(
        PipelineStage::Host | PipelineStage::AllCommands |
            PipelineStage::Transfer,
        PipelineStage::ComputeShader,
        AccessFlag::HostWrite | AccessFlag::ShaderRead |
            AccessFlag::ShaderWrite | AccessFlag::TransferWrite |
            AccessFlag::MemoryWrite,
        AccessFlag::ShaderRead | AccessFlag::UniformRead);
    command->BindComputePipeline(resources.pipeline);
    command->BindDescriptorSets(PipelineBindPoint::Compute,
                                resources.pipelineLayout, 0u, 1u,
                                &resources.descriptorSet, 0u, nullptr);
    command->Dispatch((freshRayCount + 63u) / 64u, 1u, 1u);
    command->MemoryBarrier(
        PipelineStage::ComputeShader, PipelineStage::Transfer,
        AccessFlag::ShaderRead | AccessFlag::ShaderWrite |
            AccessFlag::MemoryWrite,
        AccessFlag::TransferRead);
    command->CopyBuffer(outputBuffer, outputReadback, 0u, 0u, outputBytes);
    command->CopyBuffer(nodeBuffer, nodeReadback, 0u, 0u, nodeBytes);
    command->CopyBuffer(triangleBuffer, triangleReadback, 0u, 0u,
                        triangleBytes);
    for (const auto& pair : pairs)
        command->CopyBuffer(pair.source, pair.after, 0u, 0u, pair.bytes);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!command->End() || !command->Submit() || !command->WaitForCompletion())
        return writeFailure("oracle End/Submit/WaitForCompletion failed");

    std::vector<u8> output;
    std::vector<u8> nodeCopy;
    std::vector<u8> triangleCopy;
    output.resize(size_t(outputBytes));
    nodeCopy.resize(size_t(nodeBytes));
    triangleCopy.resize(size_t(triangleBytes));
    auto mapReadback = [&](ResourceHandle resource, void* dst, u64 bytes) {
        const void* mapped = device->MapBuffer(resource);
        if (!mapped) return false;
        std::memcpy(dst, mapped, size_t(bytes));
        device->UnmapBuffer(resource);
        return true;
    };
    if (!mapReadback(outputReadback, output.data(), outputBytes) ||
        !mapReadback(nodeReadback, nodeCopy.data(), nodeBytes) ||
        !mapReadback(triangleReadback, triangleCopy.data(), triangleBytes))
        return writeFailure("oracle output/BVH readback map failed");
    for (auto& pair : pairs) {
        pair.beforeBytes.resize(size_t(pair.bytes));
        pair.afterBytes.resize(size_t(pair.bytes));
        if (!mapReadback(pair.before, pair.beforeBytes.data(), pair.bytes) ||
            !mapReadback(pair.after, pair.afterBytes.data(), pair.bytes))
            return writeFailure("production pair readback map failed");
    }

    auto writeRaw = [&](const std::string& path, const void* data, u64 bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(static_cast<const char*>(data), std::streamsize(bytes));
        file.close();
        return bool(file);
    };
    if (!writeRaw(prefix + ".raw", output.data(), outputBytes) ||
        !writeRaw(prefix + ".nodes.raw", nodeCopy.data(), nodeBytes) ||
        !writeRaw(prefix + ".triangles.raw", triangleCopy.data(), triangleBytes) ||
        !writeRaw(prefix + ".scene.raw", originalScene.data(),
                  u64(originalScene.size()) * sizeof(SCProofTriangle)) ||
        !writeRaw(prefix + ".mapping.bin", nativeMapping.data(), nativeMapping.size()))
        return writeFailure("oracle raw evidence write failed");
    for (const auto& pair : pairs) {
        const std::string beforePath = prefix + ".production_" + pair.name + ".before.raw";
        const std::string afterPath = prefix + ".production_" + pair.name + ".after.raw";
        if (!writeRaw(beforePath, pair.beforeBytes.data(), pair.bytes) ||
            !writeRaw(afterPath, pair.afterBytes.data(), pair.bytes))
            return writeFailure("production pair raw evidence write failed");
    }
    rawEvidenceSaved = true;

    if (std::memcmp(nodeCopy.data(), nodes.data(), size_t(nodeBytes)) != 0 ||
        std::memcmp(triangleCopy.data(), triangles.data(), size_t(triangleBytes)) != 0)
        return writeFailure("uploaded diagnostic BVH did not read back bitwise");
    const auto* result = reinterpret_cast<const math::v4*>(output.data());
    for (u32 index = 0; index < freshRayCount; ++index) {
        const u32 sourceIndex = index * 4u;
        const u32 resultIndex = index * 12u;
        if (std::memcmp(&result[resultIndex + 0u],
                        &raySourceRecords[sourceIndex + 0u], sizeof(math::v4)) != 0 ||
            std::memcmp(&result[resultIndex + 1u],
                        &raySourceRecords[sourceIndex + 1u], sizeof(math::v4)) != 0 ||
            std::memcmp(&result[resultIndex + 2u],
                        &raySourceRecords[sourceIndex + 2u], sizeof(math::v4)) != 0 ||
            std::memcmp(&result[resultIndex + 3u],
                        &raySourceRecords[sourceIndex + 3u], sizeof(math::v4)) != 0)
            return writeFailure("oracle source echo differs from actual ray-source readback");
        const u32 expectedProbe = updateList[index / raysPerProbe];
        const u32 expectedStamp =
            ((volume.FrameIndex & 0xffffu) << 16u) |
            ((expectedProbe + 1u) & 0xffffu);
        if (DDGIFirstOracleFloatBits(result[resultIndex + 4u].x) !=
                index / raysPerProbe ||
            DDGIFirstOracleFloatBits(result[resultIndex + 4u].y) != expectedProbe ||
            DDGIFirstOracleFloatBits(result[resultIndex + 4u].z) !=
                index % raysPerProbe ||
            DDGIFirstOracleFloatBits(result[resultIndex + 4u].w) != expectedStamp ||
            DDGIFirstOracleFloatBits(result[resultIndex + 9u].w) != volume.FrameIndex)
            return writeFailure("oracle update-slot or frame stamp differs from actual list");
    }
    bool productionUnchanged = true;
    for (const auto& pair : pairs) {
        if (pair.beforeBytes != pair.afterBytes) productionUnchanged = false;
    }
    if (!productionUnchanged)
        return writeFailure("oracle changed a production buffer in its diagnostic command");

    const auto& info = device->GetDeviceInfo();
    const std::string rawName = DDGIFirstOracleBaseName(prefix + ".raw");
    const std::string shaderName = DDGIFirstOracleBaseName(prefix + ".shader.spv");
    std::ofstream meta(prefix + "_meta.json", std::ios::trunc);
    if (!meta) return writeFailure("oracle metadata open failed");
    meta << "{\"schema\":\"ddgi_default_first_oracle_v1\","
            "\"complete\":true,\"readback_completed\":true,"
            "\"begin_end_submit_checked\":true,\"completion_checked\":true,"
            "\"explicit_buffer_dependencies\":true,\"map_after_completion\":true,"
            "\"only_listed_fresh\":true,\"frame\":" << frame
         << ",\"ddgi_frame\":" << volume.FrameIndex
         << ",\"full_ray_capacity\":" << fullRayCapacity
         << ",\"rays_per_probe\":" << raysPerProbe
         << ",\"record_vec4_stride\":12,\"raw_vec4_count\":"
         << u64(freshRayCount) * 12u << ",\"raw_bytes\":" << outputBytes
         << ",\"raw_file\":\"" << rawName
         << "\",\"resource_handle\":" << outputBuffer
         << ",\"binding\":15,\"format\":\"vec4x12_f32\","
            "\"runtime_format\":\"vec4x12_f32\",\"default_on\":false,"
            "\"algorithm_changed\":false,\"production_writes\":false,"
            "\"geometry_gate_m\":0.00002,"
            "\"residual_semantics\":\"point_to_barycentric\","
            "\"distance_semantics\":\"actual_source_ray_to_diagnostic_triangle\","
            "\"status_codes\":{"
            "\"UNKNOWN\":0,\"HIT\":1,\"SEGMENT_MISS\":2,"
            "\"ENDPOINT\":3,\"OVERFLOW\":4,\"TRANSMIT\":5,"
            "\"RESIDUAL\":6},"
            "\"layout\":[\"trace_origin\",\"trace_direction\","
            "\"finalize_origin\",\"finalize_direction\","
            "\"slot_probe_ray_stamp_bits\",\"first_point_distance\","
            "\"barycentric_geometric_status_bits\","
            "\"logical_triangle_mesh_flags_residual\","
            "\"status_reason_source_mask_runtime_index_bits\","
            "\"capacity_update_count_rays_frame_bits\","
            "\"reconstructed_point_residual\","
            "\"source_origin_delta_source_mask\"],"
            "\"expected_frame_stamp\":"
         << (volume.FrameIndex & 0xffffu)
         << ",\"update_probes\":[";
    for (u32 i = 0; i < updateList.size(); ++i) {
        if (i) meta << ',';
        meta << updateList[i];
    }
    meta << "],\"fresh_ray_count\":" << freshRayCount
         << ",\"fresh_record_vec4_count\":" << u64(freshRayCount) * 12u
         << ",\"source_tail\":{\"buffer_handle\":" << hitNormal
         << ",\"base_vec4\":" << u64(fullRayCapacity) * 5u
         << ",\"record_vec4_stride\":4,"
            "\"layout\":[\"trace_origin\",\"trace_direction\","
            "\"finalize_origin\",\"finalize_direction\"],"
            "\"raw_file\":\"ddgi_ray_source.raw\"},"
            "\"native_mapping\":{\"raw_file\":\""
         << DDGIFirstOracleBaseName(prefix + ".mapping.bin")
         << "\",\"bytes\":" << nativeMapping.size()
         << ",\"stride\":16,\"record_count\":"
         << nativeMapping.size() / sizeof(DDGIFirstOracleMappingRecord)
         << ",\"source_identity_verified\":false,"
            "\"diagnostic_bvh_cpu_upload\":true,\"nodes_raw_file\":\""
         << DDGIFirstOracleBaseName(prefix + ".nodes.raw")
         << "\",\"triangles_raw_file\":\""
         << DDGIFirstOracleBaseName(prefix + ".triangles.raw")
         << "\",\"scene_raw_file\":\""
         << DDGIFirstOracleBaseName(prefix + ".scene.raw")
         << "\",\"readback_completed\":true,"
            "\"explicit_buffer_dependencies\":true,"
            "\"completion_checked\":true,\"map_after_completion\":true,"
            "\"same_frame\":true},"
            "\"source_identity\":{\"shader_file\":\""
         << shaderName << "\",\"shader_bytes\":" << shaderBytes.size()
         << ",\"shader_entrypoint\":\"ddgi_default_first_oracle\","
            "\"shader_fnv1a\":" << DDGIFirstOracleFNV1a(
                shaderBytes.data(), shaderBytes.size())
         << ",\"hit_normal_handle\":" << hitNormal
         << ",\"hit_normal_bytes\":" << hitNormalBytes
         << ",\"hit_normal_expected_min9N_bytes\":" << expectedHitNormalMinBytes
         << ",\"hit_normal_expected_27N_extent_bytes\":"
         << expectedHitNormalExtentBytes
         << ",\"update_list_handle\":" << updateBuffer
         << ",\"update_list_bytes\":" << updateBytes
         << ",\"native_positions_handle\":" << nativeSources[0]
         << ",\"native_elements_handle\":" << nativeSources[1]
         << ",\"native_instances_handle\":" << nativeSources[2]
         << ",\"native_materials_handle\":" << nativeSources[3]
         << ",\"static_scene_assumption\":true,"
            "\"historical_native_identity_verified\":false,"
            "\"volume_source\":\"CPU uploaded diagnostic mirror, not independent production CB history\","
            "\"frame_echo\":\"actual GPU source frame is independently checked by source stamps and production pair readback\","
            "\"production_pair_unchanged\":true,\"producer_completion_checked\":true,"
            "\"device_max_render_targets\":" << info.maxRenderTargets
         << ",\"device_max_texture_2d_size\":" << info.maxTexture2DSize
         << ",\"pair_hashes\":{";
    for (u32 i = 0; i < pairs.size(); ++i) {
        if (i) meta << ',';
        const auto& pair = pairs[i];
        meta << '"' << pair.name << "\":{\"before_fnv1a\":"
             << DDGIFirstOracleFNV1a(pair.beforeBytes.data(), pair.bytes)
             << ",\"after_fnv1a\":" << DDGIFirstOracleFNV1a(pair.afterBytes.data(), pair.bytes)
             << ",\"bytes\":" << pair.bytes
             << ",\"unchanged\":" << (pair.beforeBytes == pair.afterBytes ? "true" : "false")
             << '}';
    }
    meta << "}},\"notes\":\"actual source tail and full same-frame update list are bound directly; diagnostic BVH is not native geometry\"}\n";
    meta.close();
    if (!meta) return writeFailure("oracle metadata write failed");
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_DEFAULT_FIRST_ORACLE] ready frame=" << frame
        << " fresh_rays=" << freshRayCount
        << " output_bytes=" << outputBytes << std::endl;
    return true;
}
