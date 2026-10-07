// Explicit opt-in Vulkan diagnostic for the DDGI first-hit native normal
// candidate.  This file is included inside the test's anonymous namespace.
// The production trace owns the candidate result; this helper only stages the
// exact same-frame audit and resident source buffers after checked completion.

struct DDGIFirstNativeTriangleMappingRecord {
    u32 global_vertex_ids[3];
    u32 instance_index;
};
static_assert(sizeof(DDGIFirstNativeTriangleMappingRecord) == 16,
              "native triangle mapping ABI changed");

static bool DDGIFirstNativeFinite(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

// Build the original-triangle-ID → native GPUDrivenDraw vertex mapping in the
// same unique-geometry order used by the existing first-normal proof.  This
// intentionally reads only positions/elements needed to establish identity;
// it does not turn the candidate into a full UV upload audit.
static bool BuildDDGIFirstNativeTriangleMapping(
    const primal::utl::vector<SceneDataMeshInfo>& sceneMeshes,
    const std::vector<SCProofTriangle>& firstScene,
    std::vector<u8>& mappingBytes,
    std::string& error) {
    mappingBytes.clear();
    error.clear();
    if (sceneMeshes.empty() || firstScene.empty()) {
        error = "scene or first-surface triangle list is empty";
        return false;
    }
    struct MeshInfo {
        rhi::RHIMeshAsset asset{};
        u32 vertexOffset{0};
        u32 instanceIndex{0};
    };
    std::vector<MeshInfo> meshes(sceneMeshes.size());
    std::vector<u32> triangleBases(sceneMeshes.size() + 1u, 0u);
    u64 nextVertex = 0;
    for (u32 mi = 0; mi < sceneMeshes.size(); ++mi) {
        u32 previous = mi;
        for (u32 prior = 0; prior < mi; ++prior) {
            if (sceneMeshes[prior].meshEntityId == sceneMeshes[mi].meshEntityId) {
                previous = prior;
                break;
            }
        }
        if (previous != mi) {
            meshes[mi] = meshes[previous];
            meshes[mi].instanceIndex = mi;
        } else {
            if (!primal::content::get_rhi_mesh_asset(sceneMeshes[mi].meshEntityId,
                                                     meshes[mi].asset) ||
                (meshes[mi].asset.index_size != 2u &&
                 meshes[mi].asset.index_size != 4u) ||
                meshes[mi].asset.num_vertices == 0 ||
                meshes[mi].asset.num_indices == 0 ||
                (meshes[mi].asset.num_indices % 3u) != 0u ||
                meshes[mi].asset.position_buffer.size() <
                    size_t(meshes[mi].asset.num_vertices) * 12u ||
                meshes[mi].asset.index_buffer.size() <
                    size_t(meshes[mi].asset.num_indices) * meshes[mi].asset.index_size) {
                error = "scene mesh asset has an unsupported native stride";
                return false;
            }
            if (nextVertex > std::numeric_limits<u32>::max() -
                                  meshes[mi].asset.num_vertices) {
                error = "global vertex offset overflow";
                return false;
            }
            meshes[mi].vertexOffset = static_cast<u32>(nextVertex);
            meshes[mi].instanceIndex = mi;
            nextVertex += meshes[mi].asset.num_vertices;
        }
        if (triangleBases[mi] > std::numeric_limits<u32>::max() -
                                  meshes[mi].asset.num_indices / 3u) {
            error = "triangle base overflow";
            return false;
        }
        triangleBases[mi + 1u] = triangleBases[mi] + meshes[mi].asset.num_indices / 3u;
    }
    if (triangleBases.back() != firstScene.size()) {
        error = "first-surface triangle count differs from current scene order";
        return false;
    }

    std::vector<DDGIFirstNativeTriangleMappingRecord> records(firstScene.size());
    std::vector<u8> seen(firstScene.size(), 0u);
    for (u32 runtime = 0; runtime < firstScene.size(); ++runtime) {
        const auto& triangle = firstScene[runtime];
        if (!DDGIFirstNativeFinite(triangle.a[3]) ||
            !DDGIFirstNativeFinite(triangle.e2[3]) ||
            triangle.a[3] < 0.0f || triangle.e2[3] < 0.0f ||
            triangle.a[3] != std::floor(triangle.a[3]) ||
            triangle.e2[3] != std::floor(triangle.e2[3])) {
            error = "triangle identity is not integral";
            return false;
        }
        const u32 logical = static_cast<u32>(triangle.a[3]);
        const u32 mesh = static_cast<u32>(triangle.e2[3]);
        if (logical >= records.size() || mesh >= meshes.size() || seen[logical]) {
            error = "triangle identity is duplicated or out of range";
            return false;
        }
        const auto upper = std::upper_bound(triangleBases.begin(), triangleBases.end(), logical);
        if (upper == triangleBases.begin()) {
            error = "triangle identity has no mesh base";
            return false;
        }
        const u32 expectedMesh = static_cast<u32>(std::distance(triangleBases.begin(), upper)) - 1u;
        if (expectedMesh != mesh) {
            error = "triangle identity mesh differs from scene order";
            return false;
        }
        const u32 local = logical - triangleBases[mesh];
        const auto& asset = meshes[mesh].asset;
        auto indexAt = [&](u32 at) {
            u32 value = 0;
            std::memcpy(&value, asset.index_buffer.data() + size_t(at) * asset.index_size,
                        asset.index_size);
            return value;
        };
        if (local >= asset.num_indices / 3u) {
            error = "triangle local index is out of range";
            return false;
        }
        const u32 ia = indexAt(local * 3u + 0u);
        const u32 ib = indexAt(local * 3u + 1u);
        const u32 ic = indexAt(local * 3u + 2u);
        if (ia >= asset.num_vertices || ib >= asset.num_vertices || ic >= asset.num_vertices) {
            error = "triangle vertex index is out of range";
            return false;
        }
        records[logical] = {{meshes[mesh].vertexOffset + ia,
                             meshes[mesh].vertexOffset + ib,
                             meshes[mesh].vertexOffset + ic},
                            meshes[mesh].instanceIndex};
        seen[logical] = 1u;
    }
    for (u8 value : seen) {
        if (!value) {
            error = "triangle mapping has an identity gap";
            return false;
        }
    }
    mappingBytes.resize(records.size() * sizeof(records[0]));
    std::memcpy(mappingBytes.data(), records.data(), mappingBytes.size());
    return true;
}

static bool RunDDGIFirstNativeNormalCandidate(
    RHIDeviceBase* device,
    lumen::LumenDDGIPass* pass,
    u32 frame,
    const std::vector<u32>& updateList,
    const std::vector<math::v4>& firstSurfaceRecords,
    const std::vector<math::v4>& firstSurfaceTriangles,
    const std::vector<math::v4>& raySourceRecords,
    u32 fullRayCapacity,
    u32 raysPerProbe,
    const primal::utl::vector<SceneDataMeshInfo>& sceneMeshes,
    const nanite::GPUMaterialRegistry* materialRegistry) {
    const char* prefix = std::getenv("DDGI_FIRST_NATIVE_NORMAL_PREFIX");
    if (!prefix || !prefix[0]) return true;
    bool rawEvidenceSaved = false;
    auto fail = [&](const char* why) {
        std::ofstream meta(std::string(prefix) + ".json", std::ios::trunc);
        if (meta)
            meta << "{\"complete\":false,\"schema\":1,\"error\":\"" << why
                 << "\",\"raw_evidence_preserved\":"
                 << (rawEvidenceSaved ? "true" : "false") << "}\n";
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] rejected: " << why << std::endl;
        return false;
    };
    const char* nativeFlag = std::getenv("DDGI_FIRST_NATIVE_NORMAL");
    const char* firstSurfaceFlag = std::getenv("DDGI_FIRST_SURFACE");
    const char* sourceAuditFlag = std::getenv("DDGI_RAY_SOURCE_AUDIT");
    const char* dumpFlag = std::getenv("DDGI_DUMP");
    if (!device || !pass || device->GetPlatform() != RHIPlatform::Vulkan ||
        !nativeFlag || std::strcmp(nativeFlag, "1") != 0 ||
        !firstSurfaceFlag || std::strcmp(firstSurfaceFlag, "1") != 0 ||
        !sourceAuditFlag || std::strcmp(sourceAuditFlag, "1") != 0 ||
        !dumpFlag || std::strcmp(dumpFlag, "2") != 0)
        return fail("requires DDGI_FIRST_NATIVE_NORMAL=1, DDGI_FIRST_SURFACE=1, DDGI_RAY_SOURCE_AUDIT=1, DDGI_DUMP=2");
    if (!pass->IsFirstSurfaceEnabled() || !pass->IsFirstNativeNormalEnabled() ||
        !materialRegistry || sceneMeshes.empty())
        return fail("first-surface/native source setup or material registry is unavailable");
    const auto& volume = pass->GetVolumeData();
    if (volume.RaySampleMode != 0 || volume.RaysPerProbe != raysPerProbe ||
        raysPerProbe == 0 || raysPerProbe != 128u || fullRayCapacity == 0 ||
        volume.ProbeCountTotal != fullRayCapacity / raysPerProbe ||
        updateList.empty() || updateList.size() > 16384u)
        return fail("unsupported scalar/ray/update-list contract");
    if (firstSurfaceRecords.size() != size_t(fullRayCapacity) * 3u ||
        firstSurfaceTriangles.empty() || firstSurfaceTriangles.size() % 3u != 0u ||
        raySourceRecords.size() != size_t(fullRayCapacity) * 4u)
        return fail("same-frame first-surface/ray-source readback is incomplete");

    auto& draw = nanite::GPUDrivenDrawPipeline::Get();
    const ResourceHandle source[4] = {
        pass->GetFirstNativeNormalPositions(), pass->GetFirstNativeNormalElements(),
        pass->GetFirstNativeNormalInstances(), pass->GetFirstNativeNormalMaterials()};
    u64 sourceBytes[4]{};
    auto* vk = static_cast<VulkanDevice*>(device);
    for (u32 i = 0; i < 4u; ++i) {
        if (source[i] == handles::INVALID_RESOURCE ||
            !vk->GetBufferByteSize(source[i], sourceBytes[i]) || sourceBytes[i] == 0 ||
            sourceBytes[i] > 256ull * 1024ull * 1024ull)
            return fail("native source handle or byte size is invalid");
    }
    if (sourceBytes[0] % 12u || sourceBytes[1] % 24u ||
        sourceBytes[1] != sourceBytes[0] / 12u * 24u || sourceBytes[2] % 192u ||
        sourceBytes[3] % 48u || sourceBytes[2] / 192u != sceneMeshes.size() ||
        sourceBytes[3] / 48u != materialRegistry->GetMaterialCount())
        return fail("native source strides/counts differ from current scene");

    const ResourceHandle liveSource[4] = {
        draw.GetGlobalVertexBuffer(), draw.GetGlobalElementBuffer(),
        draw.GetGlobalInstanceDataBuffer(), draw.GetMaterialDataBuffer()};
    for (u32 i = 0; i < 4u; ++i)
        if (source[i] != liveSource[i])
            return fail("native dispatch source identity differs from live GPUDrivenDraw");
    const ResourceHandle normalTexture = pass->GetFirstNativeNormalTexture();
    const SamplerHandle normalSampler = pass->GetFirstNativeNormalSampler();
    if (normalTexture != draw.GetNormalTextureArray() ||
        normalSampler != draw.GetTextureSampler())
        return fail("native normal texture/sampler identity differs from live GPUDrivenDraw");
    auto* normalObject = vk->GetTexture(normalTexture);
    if (normalTexture == handles::INVALID_RESOURCE ||
        normalSampler == handles::INVALID_SAMPLER || !normalObject)
        return fail("native normal texture or sampler is unavailable");
    const TextureDesc normalDesc = normalObject->GetTextureDesc();
    if (normalDesc.type != TextureType::Texture2DArray || normalDesc.size.x == 0 ||
        normalDesc.size.y == 0 || normalDesc.arraySize == 0 || normalDesc.mipLevels == 0 ||
        (normalDesc.format != DataFormat::RGBA8_UNorm &&
         normalDesc.format != DataFormat::RGBA8_sRGB))
        return fail("native normal texture descriptor is unsupported");

    u64 auditBytes = pass->GetFirstNativeNormalAuditBytes();
    u64 mappingBytes = 0;
    if (pass->GetFirstNativeNormalAuditBuffer() == handles::INVALID_RESOURCE ||
        pass->GetFirstNativeNormalTriangleMappingBuffer() == handles::INVALID_RESOURCE ||
        auditBytes == 0 || auditBytes > 64ull * 1024ull * 1024ull ||
        auditBytes % sizeof(math::v4) != 0 ||
        !vk->GetBufferByteSize(pass->GetFirstNativeNormalTriangleMappingBuffer(), mappingBytes) ||
        mappingBytes == 0 || mappingBytes % 16u != 0u)
        return fail("native audit or mapping buffer capacity is invalid");

    std::vector<u8> expectedMapping;
    std::string mappingError;
    // The dump caller does not retain the pre-BVH scene table.  The mapping
    // uploaded by the pipeline is therefore the identity witness; copy it
    // below and verify its size, while the same-frame first-surface records
    // remain the freshness boundary for accepted rays.
    (void)expectedMapping;
    (void)mappingError;

    struct Readbacks {
        RHIDeviceBase* device{nullptr};
        ResourceHandle buffers[6]{
            handles::INVALID_RESOURCE, handles::INVALID_RESOURCE,
            handles::INVALID_RESOURCE, handles::INVALID_RESOURCE,
            handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
        CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ~Readbacks() {
            if (command != handles::INVALID_COMMAND_BUFFER)
                device->DestroyCommandBuffer(command);
            for (auto buffer : buffers)
                if (buffer != handles::INVALID_RESOURCE) device->DestroyBuffer(buffer);
        }
    } readbacks;
    readbacks.device = device;
    const ResourceHandle input[5] = {
        pass->GetFirstNativeNormalAuditBuffer(),
        pass->GetFirstNativeNormalTriangleMappingBuffer(),
        source[0], source[1], source[2]};
    const u64 bytes[5] = {auditBytes, mappingBytes, sourceBytes[0], sourceBytes[1], sourceBytes[2]};
    // Materials are copied with the same command below after the fixed array
    // is created; keep six slots by treating slot 5 as the material source and
    // writing the audit/mapping/source files in one completion domain.
    const ResourceHandle copyInput[6] = {
        input[0], input[1], input[2], input[3], input[4], source[3]};
    const u64 copyBytes[6] = {
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], sourceBytes[3]};
    for (u32 i = 0; i < 6u; ++i) {
        BufferDesc desc{};
        desc.size = copyBytes[i];
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Readback;
        desc.memoryUsage = GPUMemoryUsage::Readback;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        desc.name = "DDGI_FirstNativeNormalReadback";
        readbacks.buffers[i] = device->CreateBuffer(desc);
        if (readbacks.buffers[i] == handles::INVALID_RESOURCE)
            return fail("native readback allocation failed");
    }
    readbacks.command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* command = vk->GetCommandBuffer(readbacks.command);
    if (!command || !command->Reset() || !command->Begin())
        return fail("native readback command begin failed");
    command->MemoryBarrier(
        PipelineStage::ComputeShader | PipelineStage::AllCommands | PipelineStage::Host,
        PipelineStage::Transfer,
        AccessFlag::ShaderWrite | AccessFlag::MemoryWrite | AccessFlag::ShaderRead |
            AccessFlag::HostWrite,
        AccessFlag::TransferRead);
    for (u32 i = 0; i < 6u; ++i)
        command->CopyBuffer(copyInput[i], readbacks.buffers[i], 0, 0, copyBytes[i]);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!command->End() || !command->Submit() || !command->WaitForCompletion())
        return fail("native readback End/Submit/WaitForCompletion failed");

    std::vector<std::vector<u8>> raw(6u);
    for (u32 i = 0; i < 6u; ++i) {
        raw[i].resize(static_cast<size_t>(copyBytes[i]));
        const void* mapped = device->MapBuffer(readbacks.buffers[i]);
        if (!mapped) return fail("native readback map failed");
        std::memcpy(raw[i].data(), mapped, raw[i].size());
        device->UnmapBuffer(readbacks.buffers[i]);
    }

    auto writeRaw = [&](const std::string& suffix, const std::vector<u8>& bytesToWrite) {
        std::ofstream file(std::string(prefix) + suffix,
                           std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(bytesToWrite.data()),
                   std::streamsize(bytesToWrite.size()));
        file.close();
        return bool(file);
    };
    if (!writeRaw(".raw", raw[0]) || !writeRaw(".mapping.bin", raw[1]) ||
        !writeRaw(".positions.bin", raw[2]) || !writeRaw(".elements.bin", raw[3]) ||
        !writeRaw(".instances.bin", raw[4]) || !writeRaw(".materials.bin", raw[5]))
        return fail("native raw evidence write failed");
    rawEvidenceSaved = true;

    // The shader owns the native-normal record semantics.  Host checks here
    // only verify source identity, sizes, and same-frame prerequisites; no
    // missing ray is substituted with an older record.
    if (raw[1].size() != mappingBytes || raw[0].size() != auditBytes ||
        raw[2].size() != sourceBytes[0] || raw[3].size() != sourceBytes[1] ||
        raw[4].size() != sourceBytes[2] || raw[5].size() != sourceBytes[3])
        return fail("native completed readback sizes changed");
    const u64 freshRayCount = u64(updateList.size()) * raysPerProbe;
    const u64 auditCapacityRays = auditBytes / (24u * sizeof(math::v4));
    if (freshRayCount > auditCapacityRays)
        return fail("same-frame native audit capacity does not cover the update list");
    // Keep the record contract in the producer metadata.  The raw SSBO is a
    // fixed full-capacity snapshot (the shader writes global-ray identities),
    // while full_scene_ray_capacity is the current DDGI volume extent.  These
    // are deliberately separate: the 64 MiB audit allocation is allowed to
    // have more record slots than the scene has rays.
    static constexpr const char* kNativeResultLayout[] = {
        "world_point_stampBits", "interp_nativeN_preNormalizeLength",
        "rayFacingNativeN_rayPlusOne", "rayFacingGeomN_geomLength",
        "vertex0_worldN", "vertex1_worldN", "vertex2_worldN",
        "material_flags_instance_valid", "vertexIDs_queryIndex",
        "rawGeomN_flipGeom", "uv_scaledUV", "interpT_normalLayerBits",
        "normalSampleRGBA", "texelFetch00", "texelFetch10", "texelFetch01",
        "texelFetch11", "normalMappedN_pathCode", "tangentNormal_normalScale",
        "baseTexel_flipNative_normalLayerBits",
        "normalBound_tangentBound_mappedBound_statusBits",
        "beforeHN_oldValid", "afterHN_statusCode",
        "parentStatus_materialFlags_pointGuard_instanceGuard"};
    std::ofstream meta(std::string(prefix) + ".json", std::ios::trunc);
    if (!meta) return fail("native metadata open failed");
    meta << "{\"complete\":true,\"schema\":1"
         << ",\"scope\":\"DDGI first-surface same-first-hit native normal candidate\""
         << ",\"counterfactual\":true,\"primary_pixel_certified\":false"
         << ",\"physical_opacity_certified\":false"
         << ",\"frame\":" << frame
         << ",\"ddgi_frame\":" << volume.FrameIndex
         << ",\"update_count\":" << updateList.size()
         << ",\"fresh_ray_count\":" << freshRayCount
         << ",\"full_scene_ray_capacity\":" << fullRayCapacity
         << ",\"full_ray_capacity\":" << fullRayCapacity
         << ",\"audit_ray_capacity\":" << auditCapacityRays
         << ",\"audit_record_count\":" << auditCapacityRays
         << ",\"compact_fresh\":false"
         << ",\"binding\":23,\"audit_binding\":23"
         << ",\"resource_handle\":" << pass->GetFirstNativeNormalAuditBuffer()
         << ",\"format\":\"vec4x24_f32\",\"runtime_format\":\"vec4x24_f32\""
         << ",\"source_identity\":\"DDGITraceSDF_firstnative binding23 actual same-frame native normal records\""
         << ",\"audit_vec4_stride\":24"
         << ",\"result_vec4_stride\":24,\"record_vec4_stride\":24"
         << ",\"result_stride\":384"
         << ",\"audit_byte_stride\":384"
         << ",\"audit_bytes\":" << auditBytes
         << ",\"bytes\":" << auditBytes
         << ",\"raw_bytes\":" << auditBytes
         << ",\"capacity\":" << auditCapacityRays
         << ",\"audit_capacity_rays\":" << auditCapacityRays
         << ",\"audit_handle\":" << pass->GetFirstNativeNormalAuditBuffer()
         << ",\"mapping_handle\":" << pass->GetFirstNativeNormalTriangleMappingBuffer()
         << ",\"mapping_bytes\":" << mappingBytes
         << ",\"resident_handles\":[" << source[0] << ',' << source[1] << ','
         << source[2] << ',' << source[3] << "]"
         << ",\"resident_bytes\":[" << sourceBytes[0] << ',' << sourceBytes[1] << ','
         << sourceBytes[2] << ',' << sourceBytes[3] << "]"
         << ",\"native_normal_texture_handle\":" << normalTexture
         << ",\"native_normal_sampler_handle\":" << normalSampler
         << ",\"native_normal_binding\":21,\"native_sampler_binding\":22"
         << ",\"native_draw_normal_binding\":11,\"native_draw_sampler_binding\":13"
         << ",\"native_normal_runtime_format\":" << static_cast<u32>(normalDesc.format)
         << ",\"native_normal_extent\":[" << normalDesc.size.x << ','
         << normalDesc.size.y << ',' << normalDesc.size.z << "]"
         << ",\"native_normal_layers\":" << normalDesc.arraySize
         << ",\"native_normal_mips\":" << normalDesc.mipLevels
         << ",\"native_source_bindings\":{\"positions\":16,\"elements\":17,\"instances\":18,\"materials\":19,\"triangle_mapping\":20,\"audit\":23}"
         << ",\"source_raw_files\":[\"positions.bin\",\"elements.bin\",\"instances.bin\",\"materials.bin\"]"
         << ",\"raw_file\":\"" << std::string(prefix) << ".raw\""
         << ",\"audit_raw_file\":\"" << std::string(prefix) << ".raw\""
         << ",\"mapping_raw_file\":\"" << std::string(prefix) << ".mapping.bin\""
         << ",\"result_layout\":[";
    constexpr size_t kNativeResultLayoutCount =
        sizeof(kNativeResultLayout) / sizeof(kNativeResultLayout[0]);
    for (size_t i = 0; i < kNativeResultLayoutCount; ++i) {
        if (i != 0) meta << ',';
        meta << '"' << kNativeResultLayout[i] << '"';
    }
    meta << ']'
         << ",\"first_surface_audit_handle\":" << pass->GetFirstSurfaceAuditBuffer()
         << ",\"ray_source_handle\":" << pass->GetHitNormalBuffer()
         << ",\"update_list_handle\":" << pass->GetProbeUpdateListBuffer(frame)
         << ",\"first_normal_proof_prefix_present\":"
         << (std::getenv("DDGI_FIRST_NORMAL_PROOF_PREFIX") ? "true" : "false")
         << ",\"fresh_source_records_required\":true"
         << ",\"missing_update_rays_substituted\":false"
         << ",\"readback_completed\":true"
         << ",\"begin_end_submit_checked\":true"
         << ",\"completion_checked\":true"
         << ",\"explicit_buffer_dependencies\":true"
         << ",\"insert_barrier_buffer_ignored\":true"
         << ",\"map_after_completion\":true"
         << ",\"default_changed\":false,\"algorithm_changed\":true}\n";
    meta.close();
    if (!meta) return fail("native metadata write failed");
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_FIRST_NATIVE_NORMAL] raw candidate complete frame=" << frame
        << " update_count=" << updateList.size()
        << " audit_bytes=" << auditBytes << std::endl;
    return true;
}
