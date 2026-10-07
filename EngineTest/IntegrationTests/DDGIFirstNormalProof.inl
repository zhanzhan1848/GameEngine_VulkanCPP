// Explicit opt-in Vulkan diagnostic.  The proof is fed only by the current
// DDGI_DUMP=2 readback iteration; it never reconstructs missing rays from an
// older dump and never changes a production descriptor or output.

struct DDGIFirstNormalProofQuery {
    u32 vertex_ids_mesh_instance[4];
    float bary_stamp[4];
    float ray_ray_plus_one[4];
};
static_assert(sizeof(DDGIFirstNormalProofQuery) == 48,
              "DDGI first-normal query ABI changed");

static bool DDGIFirstNormalProofFinite(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static bool DDGIFirstNormalProofFinite3(const float* v) {
    return DDGIFirstNormalProofFinite(v[0]) && DDGIFirstNormalProofFinite(v[1]) &&
           DDGIFirstNormalProofFinite(v[2]);
}

static u32 DDGIFirstNormalProofFloatBits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool RunDDGIFirstNormalProof(
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
    const char* prefix = std::getenv("DDGI_FIRST_NORMAL_PROOF_PREFIX");
    if (!prefix || !prefix[0]) return true;
    bool rawEvidenceSaved = false;
    auto fail = [&](const char* why) {
        std::ofstream failureMeta(std::string(prefix) + ".json", std::ios::trunc);
        if (failureMeta)
            failureMeta << "{\"complete\":false,\"schema\":1,\"error\":\"" << why
                         << "\",\"raw_evidence_preserved\":" << (rawEvidenceSaved ? "true" : "false") << "}\n";
        PRIMAL_LOG_STREAM(Error, "Render") << "[DDGI_FIRST_NORMAL_PROOF] rejected: "
                                            << why << std::endl;
        return false;
    };
    const char* zeroEnv = std::getenv("DDGI_FIRST_ZERO_CROSS");
    const bool zeroParent = zeroEnv && std::strcmp(zeroEnv, "1") == 0;
    const bool fullParent = EnvEnabled("DDGI_FIRST_SURFACE");
    if (!device || !pass || (!fullParent && !zeroParent) ||
        (fullParent && zeroParent) || (zeroParent && !pass->IsFirstZeroCrossEnabled()) ||
        !EnvEnabled("DDGI_RAY_SOURCE_AUDIT") || !EnvEnabled("DDGI_DUMP") ||
        std::strcmp(std::getenv("DDGI_DUMP"), "2") != 0)
        return fail("requires one enabled FIRST_SURFACE/ FIRST_ZERO_CROSS parent, ray source audit and DUMP=2");
    if (!pass->IsFirstSurfaceEnabled() || !materialRegistry || sceneMeshes.empty())
        return fail("first-surface pass, material registry, or scene is unavailable");
    const auto& volume = pass->GetVolumeData();
    if (zeroParent && volume.RaySampleMode != 0)
        return fail("zero-parent proof requires scalar mode0");
    if (volume.RaysPerProbe != raysPerProbe || raysPerProbe == 0 || raysPerProbe != 128u)
        return fail("unsupported ray count");
    if (volume.ProbeCountTotal != fullRayCapacity / raysPerProbe ||
        fullRayCapacity == 0 || updateList.empty() || updateList.size() > 16384u)
        return fail("update list or full ray capacity is invalid");
    if (firstSurfaceRecords.size() != size_t(fullRayCapacity) * 3u ||
        firstSurfaceTriangles.empty() || firstSurfaceTriangles.size() % 3u != 0u ||
        raySourceRecords.size() != size_t(fullRayCapacity) * 4u)
        return fail("same-frame source readback is incomplete");

    // The update list is the freshness boundary.  Every accepted query below
    // is indexed by an update slot and ray, never by an old absolute-probe
    // ray.  Reject duplicate/out-of-range entries before building queries.
    std::vector<u32> sortedUpdates = updateList;
    std::sort(sortedUpdates.begin(), sortedUpdates.end());
    if (std::adjacent_find(sortedUpdates.begin(), sortedUpdates.end()) != sortedUpdates.end() ||
        sortedUpdates.back() >= volume.ProbeCountTotal)
        return fail("update list is not unique and bounded");

    auto& draw = nanite::GPUDrivenDrawPipeline::Get();
    const ResourceHandle resident[4] = {
        draw.GetGlobalVertexBuffer(), draw.GetGlobalElementBuffer(),
        draw.GetGlobalInstanceDataBuffer(), draw.GetMaterialDataBuffer()};
    u64 residentBytes[4]{};
    for (u32 i = 0; i < 4; ++i) {
        if (resident[i] == handles::INVALID_RESOURCE ||
            !static_cast<VulkanDevice*>(device)->GetBufferByteSize(resident[i], residentBytes[i]) ||
            residentBytes[i] == 0 || residentBytes[i] > 256ull * 1024ull * 1024ull)
            return fail("resident source buffer handle, size, or cap is invalid");
    }
    if (residentBytes[0] % 12u != 0 || residentBytes[1] % 24u != 0 ||
        residentBytes[1] != (residentBytes[0] / 12u) * 24u ||
        residentBytes[2] % 192u != 0 || residentBytes[3] % 48u != 0)
        return fail("resident source strides do not match native GPUDrivenDraw");
    const u64 residentVertexCount = residentBytes[0] / 12u;
    const u64 residentInstanceCount = residentBytes[2] / 192u;
    const u64 residentMaterialCount = residentBytes[3] / 48u;
    if (!residentVertexCount || residentInstanceCount != sceneMeshes.size() ||
        residentMaterialCount != materialRegistry->GetMaterialCount() ||
        residentVertexCount > 16ull * 1024ull * 1024ull ||
        residentInstanceCount > 1ull * 1024ull * 1024ull ||
        residentMaterialCount > 1ull * 1024ull * 1024ull)
        return fail("resident source counts do not match the current scene");

    const ResourceHandle normalTexture = draw.GetNormalTextureArray();
    const SamplerHandle normalSampler = draw.GetTextureSampler();
    auto* vk = static_cast<VulkanDevice*>(device);
    auto* normalTexObject = vk->GetTexture(normalTexture);
    if (normalTexture == handles::INVALID_RESOURCE ||
        normalSampler == handles::INVALID_SAMPLER || !normalTexObject)
        return fail("native normal texture array or sampler is unavailable");
    const TextureDesc normalDesc = normalTexObject->GetTextureDesc();
    if (normalDesc.type != TextureType::Texture2DArray || normalDesc.size.x == 0 ||
        normalDesc.size.y == 0 || normalDesc.arraySize == 0 || normalDesc.mipLevels == 0 ||
        (normalDesc.format != DataFormat::RGBA8_UNorm &&
         normalDesc.format != DataFormat::RGBA8_sRGB))
        return fail("native normal texture format/layout is unsupported");

    // Reconstruct the exact unique-geometry packing order used by
    // GPUDrivenDrawPipeline.  The CPU assets only provide the mapping; the
    // command below still copies and verifies the resident GPU bytes.
    struct MeshPacking {
        u32 vertexOffset{0};
        u32 instanceIndex{0};
        u32 vertexCount{0};
        u32 geometryId{0};
        rhi::RHIMeshAsset asset{};
    };
    std::vector<MeshPacking> meshPacking(sceneMeshes.size());
    std::vector<u32> uniqueGeometry;
    u32 nextVertex = 0;
    for (u32 mi = 0; mi < sceneMeshes.size(); ++mi) {
        const auto& info = sceneMeshes[mi];
        u32 previous = mi;
        for (u32 prior = 0; prior < mi; ++prior)
            if (sceneMeshes[prior].meshEntityId == info.meshEntityId) { previous = prior; break; }
        if (previous != mi) {
            // Reuse the already materialized asset for duplicate geometry IDs;
            // get_rhi_mesh_asset returns a deep copy of the complete asset.
            meshPacking[mi] = meshPacking[previous];
            meshPacking[mi].instanceIndex = mi;
            continue;
        }
        rhi::RHIMeshAsset asset{};
        if (!primal::content::get_rhi_mesh_asset(info.meshEntityId, asset) ||
            asset.index_size != 2u && asset.index_size != 4u ||
            asset.num_indices == 0 || asset.num_indices % 3u != 0u ||
            asset.num_vertices == 0 ||
            asset.position_buffer.size() < size_t(asset.num_vertices) * 12u ||
            asset.index_buffer.size() < size_t(asset.num_indices) * asset.index_size)
            return fail("scene mesh asset identity/stride is invalid");
        MeshPacking packed{};
        packed.vertexOffset = nextVertex;
        packed.instanceIndex = mi;
        packed.vertexCount = asset.num_vertices;
        packed.geometryId = info.meshEntityId;
        packed.asset = asset;
        meshPacking[mi] = packed;
        uniqueGeometry.push_back(info.meshEntityId);
        if (nextVertex > std::numeric_limits<u32>::max() - asset.num_vertices)
            return fail("global vertex offset overflow");
        nextVertex += asset.num_vertices;
    }
    if (u64(nextVertex) != residentVertexCount)
        return fail("cumulative global vertex packing differs from resident buffer");

    // Validate the reordered first-surface triangle table against the current
    // scene ordering.  a.w is the original firstScene triangle ID; e2.w is the
    // source mesh index.  This rejects a stale BVH or a silent triangle remap.
    const u32 totalTriangles = u32(firstSurfaceTriangles.size() / 3u);
    std::vector<u32> triangleRuntimeIndex(totalTriangles, 0xFFFFFFFFu);
    std::vector<u32> triangleMesh(totalTriangles, 0xFFFFFFFFu);
    std::vector<u32> triangleLocal(totalTriangles, 0xFFFFFFFFu);
    std::vector<u32> triangleBases(sceneMeshes.size() + 1u, 0u);
    for (u32 mi = 0; mi < sceneMeshes.size(); ++mi) {
        const auto& asset = meshPacking[mi].asset;
        triangleBases[mi + 1u] = triangleBases[mi] + asset.num_indices / 3u;
    }
    if (triangleBases.back() != totalTriangles)
        return fail("first-surface triangle table count changed");
    for (u32 runtime = 0; runtime < totalTriangles; ++runtime) {
        const math::v4& a = firstSurfaceTriangles[runtime * 3u + 0u];
        const math::v4& e1 = firstSurfaceTriangles[runtime * 3u + 1u];
        const math::v4& e2 = firstSurfaceTriangles[runtime * 3u + 2u];
        const float logicalF = a.w;
        const float meshF = e2.w;
        const u32 encodedFlags = DDGIFirstNormalProofFloatBits(e1.w);
        if (!DDGIFirstNormalProofFinite(logicalF) || !DDGIFirstNormalProofFinite(meshF) ||
            logicalF < 0.0f || meshF < 0.0f || logicalF != std::floor(logicalF) ||
            meshF != std::floor(meshF) || logicalF >= float(totalTriangles) ||
            meshF >= float(sceneMeshes.size()))
            return fail("first-surface triangle identity is not integral/bounded");
        const u32 logical = u32(logicalF), mesh = u32(meshF);
        if (triangleRuntimeIndex[logical] != 0xFFFFFFFFu)
            return fail("first-surface triangle IDs are duplicated");
        auto upper = std::upper_bound(triangleBases.begin(), triangleBases.end(), logical);
        const u32 expectedMesh = u32(std::distance(triangleBases.begin(), upper)) - 1u;
        const u32 local = logical - triangleBases[expectedMesh];
        if (mesh != expectedMesh || local >= (triangleBases[expectedMesh + 1u] - triangleBases[expectedMesh]))
            return fail("first-surface triangle mesh/local ordering changed");
        const auto& asset = meshPacking[mesh].asset;
        const u32 materialId = sceneMeshes[mesh].gpuMaterialId;
        if (materialId >= materialRegistry->GetMaterialCount() ||
            (encodedFlags & 0x7FFFFFFFu) !=
                (materialRegistry->GetMaterialData()[materialId].flags & 0x7FFFFFFFu))
            return fail("first-surface material flags differ from the current registry");
        auto indexAt = [&](u32 at) {
            u32 value = 0;
            std::memcpy(&value, asset.index_buffer.data() + size_t(at) * asset.index_size,
                        asset.index_size);
            return value;
        };
        const u32 ia = indexAt(local * 3u + 0u), ib = indexAt(local * 3u + 1u),
                  ic = indexAt(local * 3u + 2u);
        if (ia >= asset.num_vertices || ib >= asset.num_vertices || ic >= asset.num_vertices)
            return fail("first-surface triangle index is out of range");
        float v[3][3]{};
        std::memcpy(v[0], asset.position_buffer.data() + size_t(ia) * 12u, 12u);
        std::memcpy(v[1], asset.position_buffer.data() + size_t(ib) * 12u, 12u);
        std::memcpy(v[2], asset.position_buffer.data() + size_t(ic) * 12u, 12u);
        for (u32 k = 0; k < 3u; ++k) {
            const float expectedE1 = v[1][k] - v[0][k];
            const float expectedE2 = v[2][k] - v[0][k];
            if (std::abs(a[k] - v[0][k]) > 2.0e-5f ||
                std::abs(e1[k] - expectedE1) > 2.0e-5f ||
                std::abs(e2[k] - expectedE2) > 2.0e-5f)
                return fail("first-surface triangle positions differ from current assets");
        }
        triangleRuntimeIndex[logical] = runtime;
        triangleMesh[logical] = mesh;
        triangleLocal[logical] = local;
    }
    for (u32 logical = 0; logical < totalTriangles; ++logical)
        if (triangleRuntimeIndex[logical] == 0xFFFFFFFFu) return fail("first-surface triangle table has a gap");

    // Build only accepted, same-frame queries.  A source record is valid only
    // when both its origin and direction stamps agree with the first-surface
    // record and the actual update slot/ray.  Missing records are rejected.
    std::vector<DDGIFirstNormalProofQuery> queries;
    queries.reserve(std::min<size_t>(16384u, size_t(updateList.size()) * raysPerProbe));
    const u32 expectedFrame = volume.FrameIndex & 0xFFFFu;
    const u32 expectedFrameStamp = expectedFrame << 16u;
    for (u32 slot = 0; slot < updateList.size(); ++slot) {
        const u32 probe = updateList[slot];
        for (u32 ray = 0; ray < raysPerProbe; ++ray) {
            const u64 rayIndex = u64(slot) * raysPerProbe + ray;
            const auto& first0 = firstSurfaceRecords[rayIndex * 3u + 0u];
            const auto& first1 = firstSurfaceRecords[rayIndex * 3u + 1u];
            const auto& first2 = firstSurfaceRecords[rayIndex * 3u + 2u];
            const u32 firstStamp = DDGIFirstNormalProofFloatBits(first1.w);
            const u32 firstFlags = DDGIFirstNormalProofFloatBits(first1.y);
            if (first0.w != 1.0f || firstFlags != 0u || first2.w != float(ray + 1u) ||
                firstStamp != (expectedFrameStamp | ((probe + 1u) & 0xFFFFu)))
                continue;
            const auto& sourceTraceOrigin = raySourceRecords[rayIndex * 4u + 0u];
            const auto& sourceTraceDirection = raySourceRecords[rayIndex * 4u + 1u];
            const auto& sourceFinalizeOrigin = raySourceRecords[rayIndex * 4u + 2u];
            const auto& sourceFinalizeDirection = raySourceRecords[rayIndex * 4u + 3u];
            if (DDGIFirstNormalProofFloatBits(sourceTraceOrigin.w) != firstStamp ||
                DDGIFirstNormalProofFloatBits(sourceFinalizeOrigin.w) != firstStamp ||
                sourceTraceDirection.w != float(ray + 1u) ||
                sourceFinalizeDirection.w != float(ray + 1u))
                continue;
            const float sourceDirectionValues[3] = {
                sourceFinalizeDirection.x, sourceFinalizeDirection.y, sourceFinalizeDirection.z};
            if (!DDGIFirstNormalProofFinite3(sourceDirectionValues)) continue;
            const float rayLen2 = sourceFinalizeDirection.x * sourceFinalizeDirection.x +
                                  sourceFinalizeDirection.y * sourceFinalizeDirection.y +
                                  sourceFinalizeDirection.z * sourceFinalizeDirection.z;
            if (!DDGIFirstNormalProofFinite(rayLen2) || std::abs(rayLen2 - 1.0f) > 2.0e-5f)
                continue;
            const float logicalF = first1.x;
            if (!DDGIFirstNormalProofFinite(logicalF) || logicalF < 0.0f ||
                logicalF != std::floor(logicalF) || logicalF >= float(totalTriangles))
                continue;
            const u32 logical = u32(logicalF);
            const u32 mesh = triangleMesh[logical], local = triangleLocal[logical];
            if (mesh >= meshPacking.size()) return fail("accepted triangle has no mesh mapping");
            const auto& asset = meshPacking[mesh].asset;
            auto indexAt = [&](u32 at) {
                u32 value = 0;
                std::memcpy(&value, asset.index_buffer.data() + size_t(at) * asset.index_size,
                            asset.index_size);
                return value;
            };
            const u32 ia = indexAt(local * 3u + 0u), ib = indexAt(local * 3u + 1u),
                      ic = indexAt(local * 3u + 2u);
            const float barySum = first2.x + first2.y + first2.z;
            const float baryValues[3] = {first2.x, first2.y, first2.z};
            if (ia >= asset.num_vertices || ib >= asset.num_vertices || ic >= asset.num_vertices ||
                !DDGIFirstNormalProofFinite3(baryValues) || std::abs(barySum - 1.0f) > 2.0e-5f ||
                first2.x < -2.0e-5f || first2.y < -2.0e-5f || first2.z < -2.0e-5f)
                continue;
            float va[3]{}, vb[3]{}, vc[3]{};
            std::memcpy(va, asset.position_buffer.data() + size_t(ia) * 12u, 12u);
            std::memcpy(vb, asset.position_buffer.data() + size_t(ib) * 12u, 12u);
            std::memcpy(vc, asset.position_buffer.data() + size_t(ic) * 12u, 12u);
            if (!DDGIFirstNormalProofFinite(first0.z) || first0.z <= 0.0f) continue;
            float residual2 = 0.0f;
            for (u32 k = 0; k < 3u; ++k) {
                const float reconstructed = first2.x * va[k] + first2.y * vb[k] + first2.z * vc[k];
                const float rayPoint = sourceFinalizeOrigin[k] + sourceFinalizeDirection[k] * first0.z;
                const float delta = reconstructed - rayPoint;
                residual2 += delta * delta;
            }
            if (!DDGIFirstNormalProofFinite(residual2) || std::sqrt(residual2) > 2.0e-5f) continue;
            DDGIFirstNormalProofQuery q{};
            q.vertex_ids_mesh_instance[0] = meshPacking[mesh].vertexOffset + ia;
            q.vertex_ids_mesh_instance[1] = meshPacking[mesh].vertexOffset + ib;
            q.vertex_ids_mesh_instance[2] = meshPacking[mesh].vertexOffset + ic;
            q.vertex_ids_mesh_instance[3] = meshPacking[mesh].instanceIndex;
            q.bary_stamp[0] = first2.x; q.bary_stamp[1] = first2.y; q.bary_stamp[2] = first2.z;
            std::memcpy(&q.bary_stamp[3], &firstStamp, sizeof(firstStamp));
            q.ray_ray_plus_one[0] = sourceFinalizeDirection.x;
            q.ray_ray_plus_one[1] = sourceFinalizeDirection.y;
            q.ray_ray_plus_one[2] = sourceFinalizeDirection.z;
            q.ray_ray_plus_one[3] = float(ray + 1u);
            queries.push_back(q);
        }
    }
    if (queries.empty() || queries.size() > 16384u)
        return fail("no bounded accepted fresh first-surface queries");

    std::vector<u8> queryBytes(16u + queries.size() * sizeof(DDGIFirstNormalProofQuery), 0u);
    const u32 queryHeader[4] = {u32(queries.size()), 1u, 0u, 0u};
    std::memcpy(queryBytes.data(), queryHeader, sizeof(queryHeader));
    std::memcpy(queryBytes.data() + 16u, queries.data(), queries.size() * sizeof(queries[0]));
    const u64 resultBytes = u64(queries.size()) * 20u * sizeof(math::v4);

    struct ProofResources {
        RHIDeviceBase* device{nullptr};
        ResourceHandle buffers[24]{};
        ShaderHandle shader{handles::INVALID_SHADER};
        DescriptorSetLayoutHandle layout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        PipelineLayoutHandle pipelineLayout{handles::INVALID_PIPELINE_LAYOUT};
        DescriptorSetHandle descriptorSet{handles::INVALID_DESCRIPTOR_SET};
        PipelineHandle pipeline{handles::INVALID_PIPELINE};
        CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ProofResources() {
            for (auto& buffer : buffers) buffer = handles::INVALID_RESOURCE;
        }
        ~ProofResources() {
            if (command != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(command);
            if (pipeline != handles::INVALID_PIPELINE) device->DestroyPipeline(pipeline);
            if (descriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(descriptorSet);
            if (pipelineLayout != handles::INVALID_PIPELINE_LAYOUT) device->DestroyPipelineLayout(pipelineLayout);
            if (layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device->DestroyDescriptorSetLayout(layout);
            if (shader != handles::INVALID_SHADER) device->DestroyShader(shader);
            for (auto b : buffers) if (b != handles::INVALID_RESOURCE) device->DestroyBuffer(b);
        }
    } resources;
    resources.device = device;
    auto createBuffer = [&](u32 slot, u64 bytes, bool readback, const char* name) {
        BufferDesc desc{};
        desc.size = bytes;
        desc.type = BufferType::Structured;
        desc.name = name;
        desc.usage = readback ? GPUMemoryUsage::Readback : GPUMemoryUsage::Dynamic;
        desc.memoryUsage = readback ? GPUMemoryUsage::Readback : GPUMemoryUsage::Dynamic;
        resources.buffers[slot] = device->CreateBuffer(desc);
        return resources.buffers[slot] != handles::INVALID_RESOURCE;
    };
    if (!createBuffer(0, queryBytes.size(), false, "DDGIFirstNormalProofQueries") ||
        !createBuffer(1, resultBytes, false, "DDGIFirstNormalProofResults") ||
        !createBuffer(2, resultBytes, true, "DDGIFirstNormalProofResultReadback") ||
        !createBuffer(3, queryBytes.size(), true, "DDGIFirstNormalProofQueryReadback"))
        return fail("proof query/output allocation failed");
    for (u32 i = 0; i < 4u; ++i)
        if (!createBuffer(4u + i, residentBytes[i], true, "DDGIFirstNormalProofResidentReadback"))
            return fail("resident source readback allocation failed");
    if (!device->UpdateBufferData(resources.buffers[0], queryBytes.data(), queryBytes.size(), 0))
        return fail("query upload failed");

    // Prove the private zero-parent query cannot write incumbent production
    // planes. These copies surround the dispatch in the same checked command.
    const ResourceHandle protectedHandles[8] = {
        pass->GetHitDistanceBuffer(), pass->GetHitNormalBuffer(),
        pass->GetRayDataBuffer(), pass->GetFirstSurfaceAuditBuffer(),
        pass->GetIrradianceBufferBySlot((frame + 2u) % 3u),
        pass->GetIrradianceBuffer(frame), pass->GetRelocatedOriginsBuffer(),
        pass->GetProbeUpdateListBuffer(frame)};
    const char* protectedNames[8] = {"hd", "hn", "rays", "first", "history", "output", "origins", "updates"};
    u64 protectedBytes[8]{};
    if (zeroParent) {
        u64 totalBytes = 0;
        for (u32 i = 0; i < 8u; ++i) {
            if (!vk->GetBufferByteSize(protectedHandles[i], protectedBytes[i]) ||
                protectedBytes[i] == 0 || protectedBytes[i] > 64ull * 1024ull * 1024ull)
                return fail("protected production plane extent unsupported");
            totalBytes += 2u * protectedBytes[i];
            if (totalBytes > 192ull * 1024ull * 1024ull ||
                !createBuffer(8u + i, protectedBytes[i], true, "FirstNormalProductionBefore") ||
                !createBuffer(16u + i, protectedBytes[i], true, "FirstNormalProductionAfter"))
                return fail("protected production snapshot cap/allocation failed");
        }
    }

    // A distinct entrypoint closes the native module identity without
    // capturing unrelated shaders named main. The proof body is unchanged.
    const char* proofEntry = zeroParent ? "ddgi_first_normal_proof" : "main";
    auto spv = LoadSpvBytes(zeroParent
        ? "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstNormalProof_named.comp.spv"
        : "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstNormalProof.comp.spv");
    if (spv.empty()) return fail("proof SPIR-V is unavailable");
    resources.shader = device->CreateShader(spv.data(), spv.size(), ShaderStage::Compute, proofEntry);
    if (resources.shader == handles::INVALID_SHADER) return fail("proof shader creation failed");
    DescriptorSetLayoutBinding bindings[8]{};
    for (u32 i = 0; i < 8u; ++i) {
        bindings[i] = {i, (i == 6u ? DescriptorType::SampledImage :
                           (i == 7u ? DescriptorType::Sampler : DescriptorType::StorageBuffer)),
                       1u, ShaderStage::Compute, nullptr};
    }
    DescriptorSetLayoutDesc layoutDesc{8u, bindings};
    resources.layout = device->CreateDescriptorSetLayout(layoutDesc);
    if (resources.layout == handles::INVALID_DESCRIPTOR_SET_LAYOUT) return fail("proof descriptor layout failed");
    PipelineLayoutDesc pipelineLayoutDesc{};
    pipelineLayoutDesc.setLayoutCount = 1u;
    pipelineLayoutDesc.setLayouts = &resources.layout;
    resources.pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    if (resources.pipelineLayout == handles::INVALID_PIPELINE_LAYOUT) return fail("proof pipeline layout failed");
    DescriptorSetDesc setDesc{}; setDesc.layout = resources.layout;
    resources.descriptorSet = device->CreateDescriptorSet(setDesc);
    if (resources.descriptorSet == handles::INVALID_DESCRIPTOR_SET) return fail("proof descriptor set failed");
    DescriptorBufferInfo bufferInfos[6] = {
        {resources.buffers[0], 0, queryBytes.size()},
        {resident[0], 0, residentBytes[0]}, {resident[1], 0, residentBytes[1]},
        {resident[2], 0, residentBytes[2]}, {resident[3], 0, residentBytes[3]},
        {resources.buffers[1], 0, resultBytes}};
    DescriptorImageInfo imageInfo{handles::INVALID_SAMPLER, normalTexture, ResourceState::ShaderResource};
    DescriptorImageInfo samplerInfo{normalSampler, handles::INVALID_RESOURCE, ResourceState::ShaderResource};
    WriteDescriptorSet writes[8]{};
    for (u32 i = 0; i < 8u; ++i) {
        writes[i] = {resources.descriptorSet, i, 0u, 1u, bindings[i].descriptorType,
                     i == 6u ? &imageInfo : (i == 7u ? &samplerInfo : nullptr),
                     i < 6u ? &bufferInfos[i] : nullptr};
    }
    device->UpdateDescriptorSets(8u, writes);
    ComputePipelineDesc computeDesc{};
    computeDesc.computeShader = resources.shader;
    computeDesc.layout = resources.pipelineLayout;
    resources.pipeline = device->CreateComputePipeline(computeDesc);
    resources.command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (resources.pipeline == handles::INVALID_PIPELINE ||
        resources.command == handles::INVALID_COMMAND_BUFFER)
        return fail("proof compute pipeline or command allocation failed");
    auto* command = vk->GetCommandBuffer(resources.command);
    if (!command || !command->Reset() || !command->Begin()) return fail("proof command begin failed");
    if (zeroParent) {
        command->MemoryBarrier(PipelineStage::Host | PipelineStage::AllCommands,
                               PipelineStage::Transfer,
                               AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::MemoryRead,
                               AccessFlag::TransferRead);
        for (u32 i = 0; i < 8u; ++i)
            command->CopyBuffer(protectedHandles[i], resources.buffers[8u + i], 0, 0, protectedBytes[i]);
        command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::ComputeShader,
                               AccessFlag::TransferRead, AccessFlag::ShaderRead);
    }
    command->MemoryBarrier(PipelineStage::Host | PipelineStage::AllCommands |
                               PipelineStage::ComputeShader | PipelineStage::VertexShader |
                               PipelineStage::FragmentShader,
                           PipelineStage::ComputeShader,
                           AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::ShaderRead |
                               AccessFlag::ShaderWrite | AccessFlag::VertexAttributeRead,
                           AccessFlag::ShaderRead);
    command->BindComputePipeline(resources.pipeline);
    command->BindDescriptorSets(PipelineBindPoint::Compute, resources.pipelineLayout, 0u, 1u,
                                &resources.descriptorSet, 0u, nullptr);
    command->Dispatch((u32(queries.size()) + 31u) / 32u, 1u, 1u);
    command->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                           AccessFlag::ShaderRead | AccessFlag::ShaderWrite,
                           AccessFlag::TransferRead);
    if (zeroParent) {
        for (u32 i = 0; i < 8u; ++i)
            command->CopyBuffer(protectedHandles[i], resources.buffers[16u + i], 0, 0, protectedBytes[i]);
    }
    command->CopyBuffer(resources.buffers[1], resources.buffers[2], 0, 0, resultBytes);
    command->CopyBuffer(resources.buffers[0], resources.buffers[3], 0, 0, queryBytes.size());
    for (u32 i = 0; i < 4u; ++i)
        command->CopyBuffer(resident[i], resources.buffers[4u + i], 0, 0, residentBytes[i]);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!command->End() || !command->Submit() || !command->WaitForCompletion())
        return fail("proof End/Submit/WaitForCompletion failed");

    std::vector<u8> result(resultBytes), queryCopy(queryBytes.size());
    auto mapCopy = [&](ResourceHandle b, void* dst, u64 bytes) {
        const void* mapped = device->MapBuffer(b);
        if (!mapped) return false;
        std::memcpy(dst, mapped, size_t(bytes));
        device->UnmapBuffer(b);
        return true;
    };
    if (!mapCopy(resources.buffers[2], result.data(), resultBytes) ||
        !mapCopy(resources.buffers[3], queryCopy.data(), queryBytes.size()))
        return fail("proof/query readback is incomplete");
    std::vector<std::vector<u8>> residentCopy(4);
    for (u32 i = 0; i < 4u; ++i) {
        residentCopy[i].resize(size_t(residentBytes[i]));
        if (!mapCopy(resources.buffers[4u + i], residentCopy[i].data(), residentBytes[i]))
            return fail("resident source readback map failed");
    }
    auto writeRaw = [&](const std::string& suffix, const void* data, u64 bytes) {
        std::ofstream file(std::string(prefix) + suffix, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), std::streamsize(bytes));
        file.close();
        return bool(file);
    };
    // Preserve every completed readback before any derived validation can
    // reject an unsupported query.  The metadata failure marker records that
    // these raw bytes are evidence only, never a successful proof.
    if (!writeRaw(".raw", result.data(), resultBytes) ||
        !writeRaw(".queries.bin", queryCopy.data(), queryCopy.size()))
        return fail("proof output raw write failed");
    const char* inputSuffix[4] = {".positions.bin", ".elements.bin", ".instances.bin", ".materials.bin"};
    for (u32 i = 0; i < 4u; ++i)
        if (!writeRaw(inputSuffix[i], residentCopy[i].data(), residentBytes[i]))
            return fail("proof resident raw write failed");
    rawEvidenceSaved = true;
    if (zeroParent) {
        bool unchanged = true;
        for (u32 i = 0; i < 8u; ++i) {
            std::vector<u8> before(protectedBytes[i]), after(protectedBytes[i]);
            if (!mapCopy(resources.buffers[8u + i], before.data(), protectedBytes[i]) ||
                !mapCopy(resources.buffers[16u + i], after.data(), protectedBytes[i]))
                return fail("protected production snapshot map failed");
            if (!writeRaw(std::string(".before-") + protectedNames[i] + ".raw", before.data(), protectedBytes[i]) ||
                !writeRaw(std::string(".after-") + protectedNames[i] + ".raw", after.data(), protectedBytes[i]))
                return fail("protected production snapshot raw write failed");
            unchanged = unchanged && before == after;
        }
        if (!unchanged) return fail("private normal proof changed protected production planes");
    }
    if (std::memcmp(queryCopy.data(), queryBytes.data(), queryBytes.size()) != 0)
        return fail("proof query readback changed");

    // Verify the resident positions, elements, instances, and materials that
    // the proof actually consumed.  This makes a CPU-side asset guess unable
    // to masquerade as a GPU proof.
    const u8* instanceBytes = residentCopy[2].data();
    for (u32 mi = 0; mi < sceneMeshes.size(); ++mi) {
        u32 geometry = 0, material = 0;
        std::memcpy(&geometry, instanceBytes + size_t(mi) * 192u + 128u, 4u);
        std::memcpy(&material, instanceBytes + size_t(mi) * 192u + 132u, 4u);
        if (geometry != sceneMeshes[mi].meshEntityId || material != sceneMeshes[mi].gpuMaterialId)
            return fail("resident instance identity/material differs from the current scene");
        const float* matrix = reinterpret_cast<const float*>(instanceBytes + size_t(mi) * 192u);
        static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (u32 k = 0; k < 16u; ++k)
            if (!DDGIFirstNormalProofFinite(matrix[k]) || matrix[k] != identity[k])
                return fail("resident instance transform is not the accepted identity transform");
    }
    if (std::memcmp(residentCopy[3].data(), materialRegistry->GetMaterialData(), size_t(residentBytes[3])) != 0)
        return fail("resident material bytes differ from the same-frame registry");
    const u8* positions = residentCopy[0].data();
    auto expectedElementWords = [](const rhi::RHIMeshAsset& asset, u32 vertex,
                                   u32& color, u32& normal, u32& tangent) {
        color = 0xFFFFFFFFu; normal = 0x80008000u; tangent = 0x80008000u;
        const u64 stride = asset.num_vertices ? asset.element_buffer.size() / asset.num_vertices : 0u;
        if ((stride == 20u || stride == 24u) &&
            asset.element_buffer.size() >= size_t(vertex + 1u) * stride) {
            const u8* at = asset.element_buffer.data() + size_t(vertex) * stride;
            std::memcpy(&color, at + 0u, 4u);
            u16 n0 = 0, n1 = 0, t0 = 0, t1 = 0;
            std::memcpy(&n0, at + 4u, 2u); std::memcpy(&n1, at + 6u, 2u);
            std::memcpy(&t0, at + 8u, 2u); std::memcpy(&t1, at + 10u, 2u);
            normal = (u32(n0) << 16u) | u32(n1);
            tangent = (u32(t0) << 16u) | u32(t1);
        } else if (asset.element_buffer.size() >= size_t(vertex + 1u) * 8u) {
            const u8* at = asset.element_buffer.data() + size_t(vertex) * 8u;
            std::memcpy(&color, at + 0u, 4u);
            u16 n0 = 0, n1 = 0;
            std::memcpy(&n0, at + 4u, 2u); std::memcpy(&n1, at + 6u, 2u);
            normal = (u32(n0) << 16u) | u32(n1);
        }
    };
    // Validate only the vertices consumed by accepted queries.  UV is read
    // from the actual resident element buffer by the proof shader and checked
    // through its sample/texel witness; this keeps the audit scoped to the
    // selected source rays instead of becoming a broad UV-upload audit.
    for (const auto& query : queries) {
        const u32 instance = query.vertex_ids_mesh_instance[3];
        if (instance >= meshPacking.size()) return fail("query instance mapping is out of range");
        const auto& packing = meshPacking[instance];
        for (u32 k = 0; k < 3u; ++k) {
            const u32 globalVertex = query.vertex_ids_mesh_instance[k];
            if (globalVertex < packing.vertexOffset ||
                globalVertex >= packing.vertexOffset + packing.vertexCount)
                return fail("query vertex mapping is outside the resident mesh segment");
            const u32 localVertex = globalVertex - packing.vertexOffset;
            float expectedPosition[3]{};
            std::memcpy(expectedPosition, packing.asset.position_buffer.data() + size_t(localVertex) * 12u, 12u);
            if (std::memcmp(positions + size_t(globalVertex) * 12u, expectedPosition, 12u) != 0)
                return fail("selected resident position differs from current mesh asset");
            u32 expectedColor = 0, expectedNormal = 0, expectedTangent = 0;
            expectedElementWords(packing.asset, localVertex, expectedColor, expectedNormal, expectedTangent);
            const u8* actual = residentCopy[1].data() + size_t(globalVertex) * 24u;
            u32 actualColor = 0, actualNormal = 0, actualTangent = 0;
            std::memcpy(&actualColor, actual + 0u, 4u);
            std::memcpy(&actualNormal, actual + 4u, 4u);
            std::memcpy(&actualTangent, actual + 8u, 4u);
            if (actualColor != expectedColor || actualNormal != expectedNormal || actualTangent != expectedTangent)
                return fail("selected resident normal/tangent source words differ from current mesh asset");
        }
    }

    // Output status and source identity are checked only after the completed
    // raw readbacks above have been saved. A failed query is an unusable proof;
    // the preserved raw bytes remain evidence, never a successful proof.
    const float* resultFloats = reinterpret_cast<const float*>(result.data());
    for (u32 qi = 0; qi < queries.size(); ++qi) {
        const float* r = resultFloats + size_t(qi) * 20u * 4u;
        const u32 validStatus = DDGIFirstNormalProofFloatBits(r[7u * 4u + 3u]);
        if (validStatus != 1u) return fail("native proof shader rejected a fresh accepted query");
        const u32 numericRecords[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 9u, 10u,
                                      11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u};
        for (u32 record : numericRecords) {
            const u32 laneCount = (record == 0u || record == 4u || record == 5u ||
                                   record == 6u || record == 11u || record == 17u ||
                                   record == 19u) ? 3u : 4u;
            for (u32 lane = 0; lane < laneCount; ++lane)
                if (!DDGIFirstNormalProofFinite(r[record * 4u + lane]))
                    return fail("proof output contains non-finite numeric data");
        }
        if (DDGIFirstNormalProofFloatBits(r[8u * 4u + 3u]) != qi)
            return fail("proof output query identity changed");
        const u32 layerBits = DDGIFirstNormalProofFloatBits(r[11u * 4u + 3u]);
        if (layerBits != 0xFFFFFFFFu) {
            const float sx = r[10u * 4u + 2u], sy = r[10u * 4u + 3u];
            const float px = sx * float(normalDesc.size.x) - 0.5f;
            const float py = sy * float(normalDesc.size.y) - 0.5f;
            const float fx = px - std::floor(px), fy = py - std::floor(py);
            const float* t0 = r + 13u * 4u;
            const float* t1 = r + 14u * 4u;
            const float* t2 = r + 15u * 4u;
            const float* t3 = r + 16u * 4u;
            const float* sample = r + 12u * 4u;
            for (u32 c = 0; c < 4u; ++c) {
                const float expected = t0[c] * (1.0f - fx) * (1.0f - fy) +
                    t1[c] * fx * (1.0f - fy) + t2[c] * (1.0f - fx) * fy + t3[c] * fx * fy;
                if (std::abs(sample[c] - expected) > 2.0e-3f)
                    return fail("native mip0 normal sample differs from independent four-texel bilinear");
            }
        }
    }

    std::ofstream meta(std::string(prefix) + ".json", std::ios::trunc);
    if (!meta) return fail("proof metadata open failed");
    meta << "{\"complete\":true,\"schema\":1,\"scope\":\"DDGI first-surface accepted-ray native vertex and normal-texture source proof\""
         << ",\"frame\":" << frame << ",\"ddgi_frame\":" << volume.FrameIndex
         << ",\"queries\":" << queries.size() << ",\"update_count\":" << updateList.size()
         << ",\"fresh_ray_count\":" << (u64(updateList.size()) * raysPerProbe)
         << ",\"accepted_first_surface_status\":1,\"query_stride\":48,\"result_vec4_stride\":20"
         << ",\"result_stride\":320,\"query_schema\":\"uvec4(vertex_ids_xyz,mesh_instance),vec4(bary,stampBits),vec4(finalize_ray_dir,ray_plus_one)\""
         << ",\"result_layout\":[\"world_point_stampBits\",\"interp_nativeN_preNormalizeLength\",\"rayFacingNativeN_rayPlusOne\",\"rayFacingGeomN_geomLength\",\"vertex0_worldN\",\"vertex1_worldN\",\"vertex2_worldN\",\"material_flags_instance_valid\",\"vertexIDs_queryIndex\",\"rawGeomN_flipGeom\",\"uv_scaledUV\",\"interpT_normalLayerBits\",\"normalSampleRGBA\",\"texelFetch00\",\"texelFetch10\",\"texelFetch01\",\"texelFetch11\",\"normalMappedN_pathCode\",\"tangentNormal_normalScale\",\"baseTexel_flipNative_normalLayerBits\"]"
         << ",\"first_surface_variant\":\""
         << (zeroParent ? "DDGI_FIRST_ZERO_CROSS" : "DDGI_FIRST_SURFACE") << "\""
         << ",\"proof_entrypoint\":\"" << proofEntry << "\""
         << ",\"pipeline_handle\":" << resources.pipeline
         << ",\"pipeline_layout_handle\":" << resources.pipelineLayout
         << ",\"descriptor_set_handle\":" << resources.descriptorSet
         << ",\"command_handle\":" << resources.command
         << ",\"readonly_proof\":true"
         << ",\"protected_production_planes\":" << (zeroParent ? 8u : 0u)
         << ",\"protected_before_after_byteidentical\":" << (zeroParent ? "true" : "false")
         << ",\"resident_handles\":[" << resident[0] << ',' << resident[1] << ',' << resident[2] << ',' << resident[3] << "]"
         << ",\"resident_bytes\":[" << residentBytes[0] << ',' << residentBytes[1] << ',' << residentBytes[2] << ',' << residentBytes[3] << "]"
         << ",\"resident_caps_bytes\":[268435456,268435456,268435456,268435456]"
         << ",\"query_handle\":" << resources.buffers[0] << ",\"output_handle\":" << resources.buffers[1]
         << ",\"first_surface_audit_handle\":" << pass->GetFirstSurfaceAuditBuffer()
         << ",\"ray_source_handle\":" << pass->GetHitNormalBuffer()
         << ",\"update_list_handle\":" << pass->GetProbeUpdateListBuffer(frame)
         << ",\"first_surface_raw\":\"ddgi_first_surface.raw\",\"ray_source_raw\":\"ddgi_ray_source.raw\""
         << ",\"normal_texture_handle\":" << normalTexture << ",\"normal_sampler_handle\":" << normalSampler
         << ",\"native_normal_binding\":11,\"native_sampler_binding\":13"
         << ",\"normal_runtime_format\":" << u32(normalDesc.format)
         << ",\"normal_extent\":[" << normalDesc.size.x << ',' << normalDesc.size.y << ",1]"
         << ",\"normal_layers\":" << normalDesc.arraySize << ",\"normal_mips\":" << normalDesc.mipLevels
         << ",\"normal_texture_type\":" << u32(normalDesc.type)
         << ",\"normal_decoder\":\"GPUDrivenDraw.wgsl unpack_normal then normalize(world_matrix*normal) per vertex\""
         << ",\"normal_map_rule\":\"ray-facing geometric/native orientation, interpolated tangent, textureLod mip0\""
         << ",\"normal_scale_used\":false,\"normal_texture_consumed\":true,\"normal_sample_bilinear_tolerance\":0.002"
         << ",\"fresh_stamp_validation\":true,\"resident_inputs_actual_readback\":true,\"resident_inputs_bit_identical\":false,\"selected_query_inputs_bit_identical\":true"
         << ",\"readback_completed\":true,\"begin_end_submit_checked\":true,\"completion_checked\":true"
         << ",\"explicit_buffer_dependencies\":true,\"insert_barrier_buffer_ignored\":true,\"map_after_completion\":true"
         << ",\"default_changed\":false,\"algorithm_changed\":false,\"physical_opacity_certified\":false"
         << ",\"native_fragment_pixel_certified\":false,\"gl_front_facing_certified\":false,\"transport_certified\":false"
         << "}\n";
    meta.close();
    if (!meta) return fail("proof metadata write failed");
    PRIMAL_LOG_STREAM(Info, "Render") << "[DDGI_FIRST_NORMAL_PROOF] complete queries="
                                       << queries.size() << " frame=" << frame << std::endl;
    return RunDDGIZeroNormalFactor(device, pass, frame, resources.buffers[1],
                                  u32(queries.size()), updateList);
}
