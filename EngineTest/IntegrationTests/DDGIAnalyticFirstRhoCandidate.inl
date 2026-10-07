// Bounded diagnostic/host bridge for DDGI_ANALYTIC_FIRST_RHO.  This file is
// included inside TestVulkanSponzaRenderGraph's anonymous namespace.

struct DDGIAnalyticFirstRhoMappingRecord {
    u32 global_vertex_ids[3];
    u32 instance_index;
    u32 scene_material_id;
    u32 source_mesh_id;
    u32 cluster_material_id;
    u32 unsupported_flags;
};
static_assert(sizeof(DDGIAnalyticFirstRhoMappingRecord) == 32,
              "analytic-first-rho mapping ABI changed");

// Read the current GPUDrivenDraw cluster map before constructing a candidate
// mapping. Each entry is {meshlet, instance, material, padding}; a native
// instance with two cluster materials is ambiguous and fails closed.
static bool ReadDDGIAnalyticFirstRhoClusterMaterials(
    RHIDeviceBase* device, std::vector<u32>& instanceMaterials,
    std::vector<u8>& rawBytes, std::string& error) {
    instanceMaterials.clear();
    rawBytes.clear();
    error.clear();
    if (!device || device->GetPlatform() != RHIPlatform::Vulkan) {
        error = "cluster-map proof requires Vulkan";
        return false;
    }
    auto& draw = nanite::GPUDrivenDrawPipeline::Get();
    const ResourceHandle cluster = draw.GetClusterMapBuffer();
    const u32 entryCount = draw.GetClusterMapEntryCount();
    const ResourceHandle instances = draw.GetGlobalInstanceDataBuffer();
    if (cluster == handles::INVALID_RESOURCE || entryCount == 0u ||
        instances == handles::INVALID_RESOURCE) {
        error = "current cluster map or instance buffer is unavailable";
        return false;
    }
    auto* vk = static_cast<VulkanDevice*>(device);
    u64 clusterBytes = 0u, instanceBytes = 0u;
    if (!vk->GetBufferByteSize(cluster, clusterBytes) ||
        !vk->GetBufferByteSize(instances, instanceBytes) ||
        clusterBytes != u64(entryCount) * 16u || instanceBytes % 192u) {
        error = "cluster-map byte size does not match the native ABI";
        return false;
    }
    const u32 instanceCount = static_cast<u32>(instanceBytes / 192u);
    BufferDesc desc{};
    desc.size = clusterBytes;
    desc.usage = GPUMemoryUsage::Readback;
    desc.memoryUsage = GPUMemoryUsage::Readback;
    desc.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
    desc.name = "DDGI_AnalyticFirstRhoClusterReadback";
    const ResourceHandle staging = device->CreateBuffer(desc);
    const CommandBufferHandle commandHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (staging == handles::INVALID_RESOURCE || commandHandle == handles::INVALID_COMMAND_BUFFER) {
        if (staging != handles::INVALID_RESOURCE) device->DestroyBuffer(staging);
        if (commandHandle != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(commandHandle);
        error = "cluster-map readback allocation failed";
        return false;
    }
    auto* command = vk->GetCommandBuffer(commandHandle);
    if (!command || !command->Reset() || !command->Begin()) {
        device->DestroyCommandBuffer(commandHandle);
        device->DestroyBuffer(staging);
        error = "cluster-map readback Reset/Begin failed";
        return false;
    }
    command->MemoryBarrier(
        PipelineStage::ComputeShader | PipelineStage::AllCommands | PipelineStage::Host |
            PipelineStage::Transfer,
        PipelineStage::Transfer,
        AccessFlag::ShaderWrite | AccessFlag::ShaderRead | AccessFlag::HostWrite |
            AccessFlag::TransferWrite,
        AccessFlag::TransferRead);
    command->CopyBuffer(cluster, staging, 0, 0, clusterBytes);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!command->End() || !command->Submit() || !command->WaitForCompletion()) {
        device->DestroyCommandBuffer(commandHandle);
        device->DestroyBuffer(staging);
        error = "cluster-map readback End/Submit/WaitForCompletion failed";
        return false;
    }
    const void* mapped = device->MapBuffer(staging);
    if (!mapped) {
        device->DestroyCommandBuffer(commandHandle);
        device->DestroyBuffer(staging);
        error = "cluster-map readback map after completion failed";
        return false;
    }
    rawBytes.resize(static_cast<size_t>(clusterBytes));
    std::memcpy(rawBytes.data(), mapped, rawBytes.size());
    device->UnmapBuffer(staging);
    device->DestroyCommandBuffer(commandHandle);
    device->DestroyBuffer(staging);
    const auto* words = reinterpret_cast<const u32*>(rawBytes.data());
    instanceMaterials.assign(instanceCount, 0xffffffffu);
    for (u32 i = 0; i < entryCount; ++i) {
        const u32 instance = words[i * 4u + 1u];
        const u32 material = words[i * 4u + 2u];
        if (instance >= instanceCount) {
            error = "cluster-map instance index is outside native instance data";
            return false;
        }
        if (instanceMaterials[instance] != 0xffffffffu &&
            instanceMaterials[instance] != material) {
            error = "native instance has multiple cluster material identities";
            return false;
        }
        instanceMaterials[instance] = material;
    }
    for (u32 i = 0; i < instanceCount; ++i) {
        if (instanceMaterials[i] == 0xffffffffu) {
            error = "native instance has no resident cluster material identity";
            return false;
        }
    }
    return true;
}

static bool BuildDDGIAnalyticFirstRhoMapping(
    const std::vector<SCProofTriangle>& original,
    const std::vector<u8>& nativeMapping,
    const primal::utl::vector<SceneDataMeshInfo>& sceneMeshes,
    const nanite::GPUMaterialRegistry* materialRegistry,
    const std::vector<u32>& actualInstanceMaterials,
    std::vector<u8>& mappingBytes, std::string& error) {
    mappingBytes.clear();
    error.clear();
    if (original.empty() || nativeMapping.size() != original.size() * 16u ||
        sceneMeshes.empty() || !materialRegistry || materialRegistry->GetMaterialCount() == 0u ||
        actualInstanceMaterials.empty()) {
        error = "analytic source/mapping/material inputs are incomplete";
        return false;
    }
    std::vector<DDGIAnalyticFirstRhoMappingRecord> records(original.size());
    std::vector<u8> seen(original.size(), 0u);
    for (u32 i = 0; i < original.size(); ++i) {
        const auto& tri = original[i];
        if (!SCProofFinite(tri.a[3]) || tri.a[3] < 0.0f ||
            tri.a[3] != std::floor(tri.a[3]) ||
            !SCProofFinite(tri.e2[3]) || tri.e2[3] < 0.0f ||
            tri.e2[3] != std::floor(tri.e2[3])) {
            error = "triangle identity is not integral";
            return false;
        }
        const u32 logical = static_cast<u32>(tri.a[3]);
        const u32 mesh = static_cast<u32>(tri.e2[3]);
        if (logical >= records.size() || mesh >= sceneMeshes.size() || seen[logical]) {
            error = "triangle identity is duplicated or out of range";
            return false;
        }
        DDGIFirstNativeTriangleMappingRecord base{};
        std::memcpy(&base, nativeMapping.data() + size_t(logical) * sizeof(base), sizeof(base));
        const u32 material = sceneMeshes[mesh].gpuMaterialId;
        if (material >= materialRegistry->GetMaterialCount()) {
            error = "triangle scene material is outside the current registry";
            return false;
        }
        if (base.instance_index >= actualInstanceMaterials.size()) {
            error = "triangle native instance is outside the current cluster map";
            return false;
        }
        const u32 clusterMaterial = actualInstanceMaterials[base.instance_index];
        if (clusterMaterial == 0xffffffffu || clusterMaterial != material) {
            error = "triangle scene material disagrees with current cluster-map material";
            return false;
        }
        u32 flags = 0u;
        std::memcpy(&flags, &tri.e1[3], sizeof(flags));
        records[logical] = {{base.global_vertex_ids[0], base.global_vertex_ids[1],
                             base.global_vertex_ids[2]},
                            base.instance_index, material, mesh, clusterMaterial, flags};
        seen[logical] = 1u;
    }
    for (u8 value : seen) {
        if (!value) {
            error = "analytic mapping has an identity gap";
            return false;
        }
    }
    mappingBytes.resize(records.size() * sizeof(records[0]));
    std::memcpy(mappingBytes.data(), records.data(), mappingBytes.size());
    return true;
}

static bool RunDDGIAnalyticFirstRhoCandidate(
    RHIDeviceBase* device, lumen::LumenDDGIPass* pass, u32 frame,
    const std::vector<u32>& updateList, u32 fullRayCapacity, u32 raysPerProbe,
    const nanite::GPUMaterialRegistry* materialRegistry) {
    const char* prefixValue = std::getenv("DDGI_ANALYTIC_FIRST_RHO_PREFIX");
    if (!prefixValue || !prefixValue[0]) return true;
    const std::string prefix(prefixValue);
    bool rawEvidenceSaved = false;
    auto fail = [&](const char* why) {
        std::ofstream meta(prefix + ".json", std::ios::trunc);
        if (meta) {
            meta << "{\"complete\":false,\"schema\":2,\"error\":\""
                 << why << "\",\"raw_evidence_preserved\":"
                 << (rawEvidenceSaved ? "true" : "false") << "}\n";
        }
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] rejected: " << why << std::endl;
        return false;
    };
    const char* dump = std::getenv("DDGI_DUMP");
    const char* sourceAudit = std::getenv("DDGI_RAY_SOURCE_AUDIT");
    if (!device || !pass || device->GetPlatform() != RHIPlatform::Vulkan ||
        !dump || std::strcmp(dump, "2") != 0 || !sourceAudit ||
        std::strcmp(sourceAudit, "1") != 0 || !materialRegistry ||
        !pass->IsAnalyticFirstRhoRequested() || !pass->IsAnalyticFirstRhoEnabled())
        return fail("requires Vulkan, DDGI_DUMP=2, DDGI_RAY_SOURCE_AUDIT=1, and candidate setup");
    const auto& volume = pass->GetVolumeData();
    if (volume.RaySampleMode != 0u || volume.RaysPerProbe != raysPerProbe ||
        raysPerProbe != 128u || fullRayCapacity == 0u ||
        volume.ProbeCountTotal != fullRayCapacity / raysPerProbe || updateList.empty())
        return fail("unsupported scalar/update-list contract");
    const u64 auditBytes = pass->GetAnalyticFirstRhoAuditBytes();
    constexpr u64 kVec4Stride = 16u;
    if (pass->GetAnalyticFirstRhoAuditBuffer() == handles::INVALID_RESOURCE ||
        auditBytes == 0u || auditBytes % sizeof(math::v4) != 0u ||
        auditBytes != u64(fullRayCapacity) * kVec4Stride * sizeof(math::v4))
        return fail("candidate audit capacity does not match the full ray domain");
    auto* vk = static_cast<VulkanDevice*>(device);
    const ResourceHandle source[5] = {
        pass->GetAnalyticFirstRhoPositions(), pass->GetAnalyticFirstRhoElements(),
        pass->GetAnalyticFirstRhoInstances(), pass->GetAnalyticFirstRhoMaterials(),
        pass->GetAnalyticFirstRhoMappingBuffer()};
    const ResourceHandle proofSource[2] = {
        pass->GetAnalyticFirstRhoNodesBuffer(), pass->GetAnalyticFirstRhoTrianglesBuffer()};
    u64 sourceBytes[5]{};
    u64 proofSourceBytes[2]{};
    for (u32 i = 0; i < 5u; ++i) {
        if (source[i] == handles::INVALID_RESOURCE ||
            !vk->GetBufferByteSize(source[i], sourceBytes[i]) || sourceBytes[i] == 0u)
            return fail("candidate source handle or byte size is invalid");
    }
    for (u32 i = 0; i < 2u; ++i) {
        if (proofSource[i] == handles::INVALID_RESOURCE ||
            !vk->GetBufferByteSize(proofSource[i], proofSourceBytes[i]) ||
            proofSourceBytes[i] == 0u)
            return fail("candidate BVH source handle or byte size is invalid");
    }
    if (sourceBytes[0] % 12u || sourceBytes[1] % 24u || sourceBytes[2] % 192u ||
        sourceBytes[3] % 48u || sourceBytes[4] % 32u || proofSourceBytes[0] % 48u ||
        proofSourceBytes[1] % 48u)
        return fail("candidate native/mapping strides are invalid");
    auto& draw = nanite::GPUDrivenDrawPipeline::Get();
    if (source[0] != draw.GetGlobalVertexBuffer() || source[1] != draw.GetGlobalElementBuffer() ||
        source[2] != draw.GetGlobalInstanceDataBuffer() || source[3] != draw.GetMaterialDataBuffer())
        return fail("candidate native source identity differs from current GPUDrivenDraw");
    const ResourceHandle albedo = pass->GetAnalyticFirstRhoAlbedoTexture();
    const SamplerHandle sampler = pass->GetAnalyticFirstRhoAlbedoSampler();
    if (albedo != draw.GetAlbedoTextureArray() || sampler != draw.GetTextureSampler())
        return fail("candidate albedo/sampler identity differs from current GPUDrivenDraw");
    auto* albedoObject = vk->GetTexture(albedo);
    if (!albedoObject || albedo == handles::INVALID_RESOURCE || sampler == handles::INVALID_SAMPLER)
        return fail("candidate albedo texture or sampler is unavailable");
    const TextureDesc albedoDesc = albedoObject->GetTextureDesc();
    if (albedoDesc.type != TextureType::Texture2DArray || albedoDesc.size.x == 0u ||
        albedoDesc.size.y == 0u || albedoDesc.arraySize == 0u || albedoDesc.mipLevels == 0u ||
        (albedoDesc.format != DataFormat::RGBA8_UNorm &&
         albedoDesc.format != DataFormat::RGBA8_sRGB))
        return fail("candidate albedo descriptor is unsupported");
    auto* samplerObject = vk->GetSampler(sampler);
    if (!samplerObject || !albedoObject->GetNativeView() || !samplerObject->GetNativeSampler())
        return fail("candidate albedo native view or sampler is unavailable");

    struct ReadbackSet {
        RHIDeviceBase* device{nullptr};
        std::vector<ResourceHandle> buffers;
        CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ~ReadbackSet() {
            if (command != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(command);
            for (auto h : buffers) if (h != handles::INVALID_RESOURCE) device->DestroyBuffer(h);
        }
    } rb;
    rb.device = device;
    const ResourceHandle input[8] = {
        pass->GetAnalyticFirstRhoAuditBuffer(), source[4], proofSource[0], proofSource[1],
        source[0], source[1], source[2], source[3]};
    const u64 bytes[8] = {
        auditBytes, sourceBytes[4], proofSourceBytes[0], proofSourceBytes[1],
        sourceBytes[0], sourceBytes[1], sourceBytes[2], sourceBytes[3]};
    rb.buffers.resize(8u, handles::INVALID_RESOURCE);
    for (u32 i = 0; i < 8u; ++i) {
        BufferDesc desc{};
        desc.size = bytes[i];
        desc.usage = GPUMemoryUsage::Readback;
        desc.memoryUsage = GPUMemoryUsage::Readback;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        desc.name = "DDGI_AnalyticFirstRhoReadback";
        rb.buffers[i] = device->CreateBuffer(desc);
        if (rb.buffers[i] == handles::INVALID_RESOURCE) return fail("candidate readback allocation failed");
    }
    rb.command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* command = vk->GetCommandBuffer(rb.command);
    if (!command || !command->Reset() || !command->Begin())
        return fail("candidate readback Reset/Begin failed");
    command->MemoryBarrier(
        PipelineStage::ComputeShader | PipelineStage::AllCommands | PipelineStage::Host | PipelineStage::Transfer,
        PipelineStage::Transfer,
        AccessFlag::ShaderWrite | AccessFlag::ShaderRead | AccessFlag::HostWrite | AccessFlag::TransferWrite,
        AccessFlag::TransferRead);
    for (u32 i = 0; i < 8u; ++i) command->CopyBuffer(input[i], rb.buffers[i], 0, 0, bytes[i]);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!command->End() || !command->Submit() || !command->WaitForCompletion())
        return fail("candidate readback End/Submit/WaitForCompletion failed");
    std::vector<std::vector<u8>> raw(8u);
    for (u32 i = 0; i < 8u; ++i) {
        raw[i].resize(static_cast<size_t>(bytes[i]));
        const void* mapped = device->MapBuffer(rb.buffers[i]);
        if (!mapped) return fail("candidate readback map after completion failed");
        std::memcpy(raw[i].data(), mapped, raw[i].size());
        device->UnmapBuffer(rb.buffers[i]);
    }
    auto writeRaw = [&](const char* suffix, const std::vector<u8>& data) {
        std::ofstream out(prefix + suffix, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        return bool(out);
    };
    std::vector<u32> actualInstanceMaterials;
    std::vector<u8> clusterMapRaw;
    std::string clusterMapError;
    if (!ReadDDGIAnalyticFirstRhoClusterMaterials(
            device, actualInstanceMaterials, clusterMapRaw, clusterMapError))
        return fail(clusterMapError.c_str());
    if (raw[1].empty() || raw[1].size() % sizeof(DDGIAnalyticFirstRhoMappingRecord) != 0u)
        return fail("completed mapping readback does not contain whole records");
    const size_t mappingCount = raw[1].size() / sizeof(DDGIAnalyticFirstRhoMappingRecord);
    for (size_t i = 0; i < mappingCount; ++i) {
        DDGIAnalyticFirstRhoMappingRecord record{};
        std::memcpy(&record, raw[1].data() + i * sizeof(record), sizeof(record));
        if (record.instance_index >= actualInstanceMaterials.size() ||
            record.cluster_material_id == 0xffffffffu ||
            record.cluster_material_id != actualInstanceMaterials[record.instance_index] ||
            record.scene_material_id != record.cluster_material_id)
            return fail("mapping readback disagrees with current cluster-map material identity");
    }
    const u64 freshRayCount = u64(updateList.size()) * raysPerProbe;
    if (freshRayCount == 0u || freshRayCount > fullRayCapacity)
        return fail("update list exceeds the full ray domain");
    const u64 freshAuditBytes = freshRayCount * kVec4Stride * sizeof(math::v4);
    if (freshAuditBytes > raw[0].size())
        return fail("fresh audit range exceeds the completed readback");
    std::vector<u8> freshAudit(raw[0].begin(), raw[0].begin() + freshAuditBytes);
    if (!writeRaw(".raw", freshAudit) || !writeRaw(".mapping.bin", raw[1]) ||
        !writeRaw(".cluster_map.bin", clusterMapRaw) ||
        !writeRaw(".nodes.bin", raw[2]) || !writeRaw(".triangles.bin", raw[3]) ||
        !writeRaw(".positions.bin", raw[4]) || !writeRaw(".elements.bin", raw[5]) ||
        !writeRaw(".instances.bin", raw[6]) || !writeRaw(".materials.bin", raw[7]))
        return fail("candidate raw evidence write failed");
    rawEvidenceSaved = true;
    const ResourceHandle audit = pass->GetAnalyticFirstRhoAuditBuffer();
    const u64 nativeAlbedoView = static_cast<u64>(
        reinterpret_cast<uintptr_t>(albedoObject->GetNativeView()));
    const u64 nativeAlbedoSampler = static_cast<u64>(
        reinterpret_cast<uintptr_t>(samplerObject->GetNativeSampler()));
    const ResourceHandle clusterMap = draw.GetClusterMapBuffer();
    const bool scFirstSourceAudit = volume.SdfExtents[1].w == 7.0f;
    const u64 nativeActual[4] = {sourceBytes[0], sourceBytes[1], sourceBytes[2], sourceBytes[3]};
    std::ofstream meta(prefix + ".json", std::ios::trunc);
    if (!meta) return fail("candidate metadata open failed");
    meta << "{\"complete\":true,\"schema\":2"
         << ",\"scope\":\""
         << (scFirstSourceAudit ? "DDGI SC-selected first source diagnostic only"
                                : "DDGI analytic fallback source11/source14 rho only") << "\""
         << ",\"sc_first_source_audit\":" << (scFirstSourceAudit ? "true" : "false")
         << ",\"experimental\":true,\"physical_certified\":false"
         << ",\"frame\":" << frame << ",\"ddgi_frame\":" << volume.FrameIndex
         << ",\"update_count\":" << updateList.size()
         << ",\"fresh_ray_count\":" << freshRayCount
         << ",\"full_ray_capacity\":" << fullRayCapacity
         << ",\"audit_record_count\":" << freshRayCount
         << ",\"full_audit_record_count\":" << fullRayCapacity
         << ",\"compact_fresh\":true"
         << ",\"raw_bytes\":" << freshAuditBytes
         << ",\"full_audit_bytes\":" << auditBytes
         << ",\"audit_vec4_stride\":16,\"audit_byte_stride\":256"
         << ",\"format\":\"vec4x16_f32\",\"binding\":27,\"audit_binding\":27"
         << ",\"candidate_entry\":\"ddgi_analytic_first_rho\""
         << ",\"application\":" << (pass->IsAnalyticFirstRhoApplied() ? 1 :
                                           pass->IsAnalyticFirstPositionApplied() ? 2 : 0)
         << ",\"audit_enabled\":true"
         << ",\"audit_handle\":" << audit
         << ",\"nodes_handle\":" << pass->GetAnalyticFirstRhoNodesBuffer()
         << ",\"triangles_handle\":" << pass->GetAnalyticFirstRhoTrianglesBuffer()
         << ",\"mapping_handle\":" << pass->GetAnalyticFirstRhoMappingBuffer()
         << ",\"mapping_bytes\":" << sourceBytes[4]
         << ",\"nodes_bytes\":" << proofSourceBytes[0]
         << ",\"triangles_bytes\":" << proofSourceBytes[1]
         << ",\"native_buffers\":[" << nativeActual[0] << ',' << nativeActual[1] << ','
         << nativeActual[2] << ',' << nativeActual[3] << ']'
         << ",\"binding_resource_handles\":{\"18\":" << proofSource[0]
         << ",\"19\":" << proofSource[1]
         << ",\"20\":" << source[0]
         << ",\"21\":" << source[1]
         << ",\"22\":" << source[2]
         << ",\"23\":" << source[3]
         << ",\"24\":" << source[4]
         << ",\"25\":" << albedo
         << ",\"26\":" << sampler
         << ",\"27\":" << audit << '}'
         << ",\"native_binding_handles\":{\"25\":" << nativeAlbedoView
         << ",\"26\":" << nativeAlbedoSampler << '}'
         << ",\"native_source_handles\":[" << source[0] << ',' << source[1] << ','
         << source[2] << ',' << source[3] << ']'
         << ",\"albedo_texture_handle\":" << albedo
         << ",\"albedo_texture_view\":" << nativeAlbedoView
         << ",\"albedo_sampler_handle\":" << sampler
         << ",\"albedo_native_sampler\":" << nativeAlbedoSampler
         << ",\"albedo_extent\":[" << albedoDesc.size.x << ',' << albedoDesc.size.y << ','
         << albedoDesc.size.z << ']'
         << ",\"albedo_array\":" << albedoDesc.arraySize
         << ",\"albedo_mips\":" << albedoDesc.mipLevels
         << ",\"cluster_map_bytes\":" << clusterMapRaw.size()
         << ",\"cluster_map_entries\":" << (clusterMapRaw.size() / 16u)
         << ",\"cluster_map_handle\":" << clusterMap
         << ",\"cluster_map_readback_completed\":true"
         << ",\"mapping_material_source\":\"scene_gpuMaterialId_equal_current_cluster_map_z\""
         << ",\"ray_max_distance\":" << volume.RayMaxDistance
         << ",\"layout\":[\"origin_stamp\",\"direction_rayPlusOne\",\"logicalTriangle_instance_source_originalDistance\",\"firstT_bary\",\"material_status_flags_mappingMaterial\",\"oldRho_path\",\"sampleRho_residual\",\"sampleRGBA\",\"tint_clusterMaterial\",\"uv_uvScale\",\"texel00\",\"texel10\",\"texel01\",\"texel11\",\"originalHN\",\"firstStatus_residual_materialFlags_application\"]"
         << ",\"missing_update_rays_substituted\":false"
         << ",\"readback_completed\":true,\"begin_end_submit_checked\":true"
         << ",\"completion_checked\":true,\"explicit_buffer_dependencies\":true"
         << ",\"insert_barrier_buffer_ignored\":true,\"map_after_completion\":true"
         << ",\"default_changed\":false,\"algorithm_changed\":"
         << (pass->IsAnalyticFirstRhoApplied() || pass->IsAnalyticFirstPositionApplied() ? "true" : "false") << "}\n";
    if (!meta) return fail("candidate metadata write failed");
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_ANALYTIC_FIRST_RHO] raw candidate complete frame=" << frame
        << " update_count=" << updateList.size() << " audit_bytes=" << auditBytes << std::endl;
    return true;
}
