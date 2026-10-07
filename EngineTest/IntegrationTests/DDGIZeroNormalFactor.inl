// DDGI_ZERO_NORMAL_FACTOR=1: bounded, default-off N-only factor audit.
// The caller includes this file before DDGIFirstNormalProof.inl.  It consumes
// the already completed named 20-vec4 native proof while that private result
// buffer is still alive, then runs a separate read-only Vulkan compute pass.
// No production DDGI descriptor is written and no candidate is enabled by
// this diagnostic.

struct DDGIZeroNormalFactorPlane {
    const char* name{nullptr};
    ResourceHandle handle{handles::INVALID_RESOURCE};
    u64 bytes{0};
    ResourceHandle before{handles::INVALID_RESOURCE};
    ResourceHandle after{handles::INVALID_RESOURCE};
    std::vector<u8> beforeBytes;
    std::vector<u8> afterBytes;
};

struct DDGIZeroNormalFactorOwned {
    RHIDeviceBase* device{nullptr};
    std::vector<ResourceHandle> buffers;
    ShaderHandle shader{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle layout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle pipelineLayout{handles::INVALID_PIPELINE_LAYOUT};
    DescriptorSetHandle descriptorSet{handles::INVALID_DESCRIPTOR_SET};
    PipelineHandle pipeline{handles::INVALID_PIPELINE};
    CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};

    ~DDGIZeroNormalFactorOwned() {
        if (!device) return;
        if (command != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(command);
        if (pipeline != handles::INVALID_PIPELINE) device->DestroyPipeline(pipeline);
        if (descriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(descriptorSet);
        if (pipelineLayout != handles::INVALID_PIPELINE_LAYOUT) device->DestroyPipelineLayout(pipelineLayout);
        if (layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device->DestroyDescriptorSetLayout(layout);
        if (shader != handles::INVALID_SHADER) device->DestroyShader(shader);
        for (auto buffer : buffers)
            if (buffer != handles::INVALID_RESOURCE) device->DestroyBuffer(buffer);
    }
};

static bool RunDDGIZeroNormalFactor(
    RHIDeviceBase* device,
    lumen::LumenDDGIPass* pass,
    u32 frame,
    ResourceHandle normalProof,
    u32 proofCount,
    const std::vector<u32>& updateList) {
    const char* enabled = std::getenv("DDGI_ZERO_NORMAL_FACTOR");
    if (!enabled || std::strcmp(enabled, "1") != 0) return true;

    const char* normalPrefix = std::getenv("DDGI_FIRST_NORMAL_PROOF_PREFIX");
    if (!normalPrefix || !normalPrefix[0]) return false;
    const std::string prefix = std::string(normalPrefix) + ".factor";
    bool rawEvidenceSaved = false;
    auto fail = [&](const char* reason) {
        std::ofstream meta(prefix + ".json", std::ios::trunc);
        if (meta) {
            meta << "{\"complete\":false,\"schema\":\"ddgi_zero_normal_factor_v1\","
                 << "\"raw_evidence_preserved\":" << (rawEvidenceSaved ? "true" : "false")
                 << ",\"default_on\":false,\"error\":\"" << reason << "\"}\n";
        }
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ZERO_NORMAL_FACTOR] rejected: " << reason << std::endl;
        return false;
    };

    auto exactOne = [](const char* name) {
        const char* value = std::getenv(name);
        return value && std::strcmp(value, "1") == 0;
    };
    auto nonEmpty = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] != '\0';
    };
    const char* dump = std::getenv("DDGI_DUMP");
    const char* zeroParent = std::getenv("DDGI_FIRST_ZERO_CROSS");
    const char* rayAudit = std::getenv("DDGI_RAY_SOURCE_AUDIT");
    const bool evidenceConflict =
        exactOne("DDGI_FIRST_SURFACE") || exactOne("DDGI_FIRST_NATIVE_NORMAL") ||
        exactOne("DDGI_ANALYTIC_FIRST_RHO") || nonEmpty("DDGI_ANALYTIC_FIRST_RHO_PREFIX") ||
        exactOne("DDGI_EVIDENCE_NORMAL") || exactOne("DDGI_DEPTH_ONLY_EVIDENCE") ||
        exactOne("DDGI_SDF_EVIDENCE_MODE") || exactOne("DDGI_FIRST_FACTOR_REPLAY");
    if (!device || !pass || device->GetPlatform() != RHIPlatform::Vulkan ||
        !zeroParent || std::strcmp(zeroParent, "1") != 0 ||
        !pass->IsFirstZeroCrossEnabled() ||
        !dump || std::strcmp(dump, "2") != 0 ||
        !rayAudit || std::strcmp(rayAudit, "1") != 0 ||
        evidenceConflict || proofCount == 0u || proofCount > 32768u ||
        updateList.empty() || updateList.size() > 32768u)
        return fail("requires exact zero-cross scalar DUMP2 ray-audit parent and bounded proof/update counts");

    const auto& volumeData = pass->GetVolumeData();
    if (!pass->IsFirstSurfaceEnabled() || !pass->IsQuerySourceAuditEnabled() ||
        volumeData.FrameIndex != frame || volumeData.RaysPerProbe != 128u ||
        volumeData.ProbeUpdateCount != updateList.size() ||
        volumeData.ProbeCountTotal == 0u || volumeData.RaySampleMode != 0 ||
        volumeData.SkyColor.w != 0.0f || volumeData.Albedo.w <= 0.5f ||
        volumeData.SdfExtents[2].w <= 0.5f || sizeof(lumen::DDGIVolumeData) != 336u)
        return fail("requires actual same-frame scalar volume/query-source audit and 336-byte UBO");

    for (u32 i = 0; i < updateList.size(); ++i) {
        if (updateList[i] >= volumeData.ProbeCountTotal)
            return fail("update prefix contains an out-of-range probe");
        for (u32 j = 0; j < i; ++j)
            if (updateList[j] == updateList[i])
                return fail("update prefix contains a duplicate probe");
    }

    auto* vk = static_cast<VulkanDevice*>(device);
    const auto slots = lumen::DDGISlotsForFrame(frame);
    const ResourceHandle previousHistory =
        pass->GetIrradianceBufferBySlot(slots.previousIndex);
    const ResourceHandle currentHistory =
        pass->GetIrradianceBufferBySlot(slots.currentIndex);
    const ResourceHandle volumeBuffer = pass->GetVolumeUniformBuffer(frame);
    const ResourceHandle fullHitNormal = pass->GetHitNormalBuffer();
    const ResourceHandle updateBuffer = pass->GetProbeUpdateListBuffer(frame);
    const u64 fullRayCapacity = u64(volumeData.ProbeCountTotal) * volumeData.RaysPerProbe;
    if (fullRayCapacity == 0u || fullRayCapacity > std::numeric_limits<u32>::max())
        return fail("full ray capacity overflow");

    const u64 proofBytes = u64(proofCount) * 20u * sizeof(math::v4);
    const u64 historyBytes = u64(volumeData.ProbeCountTotal) * 9u * sizeof(math::v4);
    const u64 volumeBytes = sizeof(lumen::DDGIVolumeData);
    const u64 hitNormalBytes = fullRayCapacity * 27u * sizeof(math::v4);
    const u64 updateBytes = u64(updateList.size()) * sizeof(u32);
    const u64 outputBytes = u64(proofCount) * 52u * sizeof(math::v4);
    constexpr u64 kPlaneCap = 64ull * 1024ull * 1024ull;
    constexpr u64 kSnapshotCap = 192ull * 1024ull * 1024ull;
    if (proofBytes == 0u || historyBytes == 0u || hitNormalBytes == 0u ||
        updateBytes == 0u || outputBytes == 0u || proofBytes > kPlaneCap ||
        historyBytes > kPlaneCap || volumeBytes > kPlaneCap ||
        hitNormalBytes > kPlaneCap || updateBytes > kPlaneCap ||
        outputBytes > kPlaneCap)
        return fail("private/source range exceeds bounded factor audit capacity");

    struct Source {
        const char* name;
        ResourceHandle handle;
        u64 offset;
        u64 bytes;
        const char* rawSuffix;
        u64 extent{0};
        ResourceHandle readback{handles::INVALID_RESOURCE};
        std::vector<u8> raw;
    };
    Source sources[5] = {
        {"normal_proof", normalProof, 0u, proofBytes, ".proof.bin"},
        {"previous_sh", previousHistory, 0u, historyBytes, ".history.bin"},
        {"volume_ubo", volumeBuffer, 0u, volumeBytes, ".volume.bin"},
        {"full_hn", fullHitNormal, 0u, hitNormalBytes, ".hn.bin"},
        {"update_prefix", updateBuffer, 0u, updateBytes, ".updates.bin"},
    };
    for (auto& source : sources) {
        if (source.handle == handles::INVALID_RESOURCE ||
            !vk->GetBufferByteSize(source.handle, source.extent) ||
            source.extent == 0u || source.bytes > source.extent)
            return fail("factor source handle or exact range is unavailable");
    }
    if (sources[0].extent != proofBytes || sources[3].extent != hitNormalBytes)
        return fail("normal proof or full HN extent differs from the fixed shader contract");
    if (volumeBuffer == handles::INVALID_RESOURCE ||
        volumeBytes > sources[2].extent || updateBuffer == handles::INVALID_RESOURCE)
        return fail("volume/update source extent is unavailable");

    std::vector<DDGIZeroNormalFactorPlane> planes = {
        {"hit_distance", pass->GetHitDistanceBuffer()},
        {"hit_normal", pass->GetHitNormalBuffer()},
        {"ray_data", pass->GetRayDataBuffer()},
        {"first_surface_audit", pass->GetFirstSurfaceAuditBuffer()},
        {"previous_sh", previousHistory},
        {"current_sh", currentHistory},
        {"relocated_origins", pass->GetRelocatedOriginsBuffer()},
        {"update_prefix", updateBuffer},
        {"volume_ubo", volumeBuffer},
    };
    u64 snapshotBytes = 0u;
    for (auto& plane : planes) {
        if (plane.handle == handles::INVALID_RESOURCE ||
            !vk->GetBufferByteSize(plane.handle, plane.bytes) ||
            plane.bytes == 0u || plane.bytes > kPlaneCap ||
            plane.bytes > (kSnapshotCap - snapshotBytes) / 2u)
            return fail("production protection plane extent exceeds 192MiB snapshot cap");
        snapshotBytes += plane.bytes * 2u;
    }
    if (proofBytes > (kSnapshotCap - snapshotBytes) / 2u)
        return fail("private proof before/after snapshot exceeds total cap");
    snapshotBytes += proofBytes * 2u;

    DDGIZeroNormalFactorOwned owned;
    owned.device = device;
    auto makeBuffer = [&](u64 size, bool readback, const char* name) {
        BufferDesc desc{};
        desc.size = size;
        desc.type = BufferType::Structured;
        desc.usage = desc.memoryUsage = readback ? GPUMemoryUsage::Readback
                                                  : GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(readback ? BufferUsageFlags::TransferDst
            : BufferUsageFlags::Storage | BufferUsageFlags::TransferSrc);
        desc.name = name;
        const ResourceHandle buffer = device->CreateBuffer(desc);
        owned.buffers.push_back(buffer);
        return buffer;
    };
    const ResourceHandle output = makeBuffer(outputBytes, false,
                                              "DDGIZeroNormalFactorOutput");
    const ResourceHandle outputReadback = makeBuffer(outputBytes, true,
                                                       "DDGIZeroNormalFactorReadback");
    for (auto& source : sources)
        source.readback = makeBuffer(source.bytes, true, "DDGIZeroNormalFactorSource");
    const ResourceHandle proofBefore = makeBuffer(proofBytes, true,
                                                   "DDGIZeroNormalFactorProofBefore");
    const ResourceHandle proofAfter = makeBuffer(proofBytes, true,
                                                  "DDGIZeroNormalFactorProofAfter");
    for (auto& plane : planes) {
        plane.before = makeBuffer(plane.bytes, true,
                                  "DDGIZeroNormalFactorProductionBefore");
        plane.after = makeBuffer(plane.bytes, true,
                                 "DDGIZeroNormalFactorProductionAfter");
    }
    for (const auto buffer : owned.buffers)
        if (buffer == handles::INVALID_RESOURCE)
            return fail("factor private/readback allocation failed");

    const char* module =
        "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIZeroNormalFactor.comp.spv";
    const char* entrypoint = "ddgi_zero_normal_factor";
    const std::vector<u8> spv = LoadSpvBytes(module);
    if (spv.empty()) return fail("zero normal factor SPIR-V is unavailable");
    owned.shader = device->CreateShader(spv.data(), spv.size(), ShaderStage::Compute,
                                         entrypoint);
    DescriptorSetLayoutBinding bindings[6]{};
    for (u32 i = 0; i < 6u; ++i) {
        bindings[i] = {i, i == 2u ? DescriptorType::UniformBuffer
                                  : DescriptorType::StorageBuffer,
                       1u, ShaderStage::Compute, nullptr};
    }
    DescriptorSetLayoutDesc layoutDesc{6u, bindings};
    owned.layout = device->CreateDescriptorSetLayout(layoutDesc);
    PipelineLayoutDesc pipelineLayoutDesc{};
    pipelineLayoutDesc.setLayoutCount = 1u;
    pipelineLayoutDesc.setLayouts = &owned.layout;
    owned.pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    DescriptorSetDesc setDesc{};
    setDesc.layout = owned.layout;
    owned.descriptorSet = device->CreateDescriptorSet(setDesc);
    if (owned.shader == handles::INVALID_SHADER ||
        owned.layout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        owned.pipelineLayout == handles::INVALID_PIPELINE_LAYOUT ||
        owned.descriptorSet == handles::INVALID_DESCRIPTOR_SET)
        return fail("factor shader/layout/set creation failed");

    DescriptorBufferInfo infos[6] = {
        {normalProof, 0u, proofBytes},
        {previousHistory, 0u, historyBytes},
        {volumeBuffer, 0u, volumeBytes},
        {fullHitNormal, 0u, hitNormalBytes},
        {updateBuffer, 0u, updateBytes},
        {output, 0u, outputBytes},
    };
    WriteDescriptorSet writes[6]{};
    for (u32 i = 0; i < 6u; ++i) {
        writes[i] = {owned.descriptorSet, i, 0u, 1u, bindings[i].descriptorType,
                     nullptr, &infos[i]};
    }
    device->UpdateDescriptorSets(6u, writes);
    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.computeShader = owned.shader;
    pipelineDesc.layout = owned.pipelineLayout;
    owned.pipeline = device->CreateComputePipeline(pipelineDesc);
    owned.command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (owned.pipeline == handles::INVALID_PIPELINE ||
        owned.command == handles::INVALID_COMMAND_BUFFER)
        return fail("factor pipeline/command allocation failed");

    auto* command = vk->GetCommandBuffer(owned.command);
    if (!command || !command->Reset() || !command->Begin())
        return fail("factor checked Reset/Begin failed");
    // Vulkan's resource-state helper does not certify buffers.  Every source
    // and every private readback is ordered with explicit host/all-command/compute/
    // transfer dependencies, then MapBuffer is delayed until Wait completes.
    command->MemoryBarrier(
        PipelineStage::Host | PipelineStage::AllCommands |
            PipelineStage::ComputeShader | PipelineStage::Transfer,
        PipelineStage::Transfer,
        AccessFlag::HostWrite | AccessFlag::MemoryWrite |
            AccessFlag::ShaderWrite | AccessFlag::TransferWrite,
        AccessFlag::TransferRead);
    for (auto& plane : planes)
        command->CopyBuffer(plane.handle, plane.before, 0u, 0u, plane.bytes);
    command->CopyBuffer(normalProof, proofBefore, 0u, 0u, proofBytes);
    command->MemoryBarrier(
        PipelineStage::Host | PipelineStage::AllCommands |
            PipelineStage::ComputeShader | PipelineStage::Transfer,
        PipelineStage::ComputeShader,
        AccessFlag::HostWrite | AccessFlag::MemoryWrite |
            AccessFlag::ShaderWrite | AccessFlag::TransferWrite,
        AccessFlag::ShaderRead | AccessFlag::UniformRead | AccessFlag::ShaderWrite);
    command->BindComputePipeline(owned.pipeline);
    command->BindDescriptorSets(PipelineBindPoint::Compute, owned.pipelineLayout,
                                0u, 1u, &owned.descriptorSet, 0u, nullptr);
    command->Dispatch((proofCount + 31u) / 32u, 1u, 1u);
    command->MemoryBarrier(
        PipelineStage::ComputeShader | PipelineStage::Host |
            PipelineStage::AllCommands | PipelineStage::Transfer,
        PipelineStage::Transfer,
        AccessFlag::ShaderRead | AccessFlag::ShaderWrite |
            AccessFlag::HostWrite | AccessFlag::MemoryWrite |
            AccessFlag::TransferWrite,
        AccessFlag::TransferRead);
    command->CopyBuffer(output, outputReadback, 0u, 0u, outputBytes);
    for (auto& source : sources)
        command->CopyBuffer(source.handle, source.readback, source.offset, 0u,
                            source.bytes);
    for (auto& plane : planes)
        command->CopyBuffer(plane.handle, plane.after, 0u, 0u, plane.bytes);
    command->CopyBuffer(normalProof, proofAfter, 0u, 0u, proofBytes);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!command->End() || !command->Submit() || !command->WaitForCompletion())
        return fail("factor checked End/Submit/WaitForCompletion failed");

    auto mapCopy = [&](ResourceHandle buffer, u64 bytes, std::vector<u8>& result) {
        const void* mapped = device->MapBuffer(buffer);
        if (!mapped) return false;
        result.resize(static_cast<size_t>(bytes));
        std::memcpy(result.data(), mapped, result.size());
        device->UnmapBuffer(buffer);
        return true;
    };
    auto writeRaw = [&](const std::string& suffix, const std::vector<u8>& data) {
        std::ofstream file(prefix + suffix, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        return bool(file);
    };
    std::vector<u8> result;
    if (!mapCopy(outputReadback, outputBytes, result) ||
        !writeRaw(".raw", result))
        return fail("factor output Map/write failed");
    for (auto& source : sources) {
        if (!mapCopy(source.readback, source.bytes, source.raw) ||
            !writeRaw(source.rawSuffix, source.raw))
            return fail("factor source Map/write failed");
    }
    std::vector<u8> proofBeforeBytes;
    std::vector<u8> proofAfterBytes;
    if (!mapCopy(proofBefore, proofBytes, proofBeforeBytes) ||
        !mapCopy(proofAfter, proofBytes, proofAfterBytes) ||
        !writeRaw(".proof-before.raw", proofBeforeBytes) ||
        !writeRaw(".proof-after.raw", proofAfterBytes))
        return fail("factor private proof before/after Map/write failed");
    for (auto& plane : planes) {
        if (!mapCopy(plane.before, plane.bytes, plane.beforeBytes) ||
            !mapCopy(plane.after, plane.bytes, plane.afterBytes) ||
            !writeRaw(std::string(".prod-before-") + plane.name + ".raw",
                      plane.beforeBytes) ||
            !writeRaw(std::string(".prod-after-") + plane.name + ".raw",
                      plane.afterBytes))
            return fail("factor production before/after Map/write failed");
    }
    rawEvidenceSaved = true;

    // Validate only after every completed output, source, and before/after raw
    // has been saved.  The private factor must leave every protected plane and
    // the proof input byte-identical.
    for (const auto& plane : planes)
        if (plane.beforeBytes != plane.afterBytes)
            return fail("factor changed a protected production plane");
    if (sources[0].raw.size() != proofBytes)
        return fail("factor proof source size changed");
    if (proofBeforeBytes != proofAfterBytes || proofBeforeBytes != sources[0].raw ||
        proofAfterBytes != sources[0].raw)
        return fail("factor private proof input changed");
    if (sources[4].raw.size() != updateBytes ||
        std::memcmp(sources[4].raw.data(), updateList.data(),
                    static_cast<size_t>(updateBytes)) != 0)
        return fail("factor update prefix readback differs from current CPU list");

    lumen::DDGIVolumeData resident{};
    std::memcpy(&resident, sources[2].raw.data(), sizeof(resident));
    if (resident.FrameIndex != frame || resident.RaysPerProbe != 128u ||
        resident.ProbeUpdateCount != updateList.size() || resident.RaySampleMode != 0 ||
        resident.SdfExtents[2].w <= 0.5f || resident.Albedo.w <= 0.5f)
        return fail("factor volume source readback is not the same-frame scalar UBO");

    std::ofstream meta(prefix + ".json", std::ios::trunc);
    if (!meta) return fail("factor metadata open failed");
    meta << "{\"schema\":\"ddgi_zero_normal_factor_v1\",\"complete\":true"
         << ",\"default_on\":false,\"frame\":" << frame
         << ",\"proof_count\":" << proofCount
         << ",\"update_count\":" << updateList.size()
         << ",\"full_ray_capacity\":" << fullRayCapacity
         << ",\"output_vec4_stride\":52,\"output_bytes\":" << outputBytes
         << ",\"identity_vec4_count\":4,\"sample_vec4_stride\":12"
         << ",\"identity_layout\":[\"proof_record0_world_point_stampBits\",\"proof_record2_vertex_nativeN_rayPlusOne\",\"proof_record7_material_instance_valid_status1\",\"proof_record8_vertexIDs_queryIndex\"]"
         << ",\"sample_blocks\":[\"baseline_Finalize_minusRD_or_actualHN\",\"vertex_proof_record2\",\"mapped_proof_record17_path2\",\"geom_proof_record3\"]"
         << ",\"sample_layout\":[\"P_gridInside\",\"N_normalValid\",\"rho\",\"skyLo\",\"historyLo\",\"Lo\",\"owners\",\"weights\",\"gp_totalWeight\",\"Lprev_activeCount\",\"Esky\",\"oldhistoryEcho\"]"
         << ",\"counterfact_history_echo\":\"qNaN\",\"baseline_history_echo\":\"HN_analytic_tail_record2\""
         << ",\"module\":\"" << module << "\",\"entrypoint\":\"" << entrypoint << "\""
         << ",\"shader_handle\":" << owned.shader
         << ",\"pipeline_handle\":" << owned.pipeline
         << ",\"pipeline_layout_handle\":" << owned.pipelineLayout
         << ",\"descriptor_set_handle\":" << owned.descriptorSet
         << ",\"command_handle\":" << owned.command
         << ",\"output_handle\":" << output
         << ",\"output_readback_handle\":" << outputReadback
         << ",\"source_readback_handles\":[" << sources[0].readback << ','
         << sources[1].readback << ',' << sources[2].readback << ','
         << sources[3].readback << ',' << sources[4].readback << ']'
         << ",\"bindings\":{\"0\":\"normalProof20vec4\",\"1\":\"previousSH\",\"2\":\"actualVolume336UBO\",\"3\":\"fullHN27N\",\"4\":\"currentFreshUpdatePrefix\",\"5\":\"private52vec4Output\"}"
         << ",\"source_handles\":[" << sources[0].handle << ',' << sources[1].handle
         << ',' << sources[2].handle << ',' << sources[3].handle << ',' << sources[4].handle << ']'
         << ",\"source_offsets\":[0,0,0,0,0]"
         << ",\"source_bytes\":[" << proofBytes << ',' << historyBytes << ',' << volumeBytes
         << ',' << hitNormalBytes << ',' << updateBytes << ']'
         << ",\"source_raw_files\":[\".proof.bin\",\".history.bin\",\".volume.bin\",\".hn.bin\",\".updates.bin\"]"
         << ",\"proof_input_before_file\":\".proof-before.raw\",\"proof_input_after_file\":\".proof-after.raw\""
         << ",\"proof_input_before_after_byteidentical\":true"
         << ",\"production_planes\":[";
    for (u32 i = 0; i < planes.size(); ++i) {
        if (i) meta << ',';
        meta << "{\"name\":\"" << planes[i].name << "\",\"handle\":"
             << planes[i].handle << ",\"bytes\":" << planes[i].bytes
             << ",\"before_file\":\".prod-before-" << planes[i].name
             << ".raw\",\"after_file\":\".prod-after-" << planes[i].name
             << ".raw\",\"before_after_byteidentical\":true}";
    }
    meta << "]"
         << ",\"snapshot_cap_bytes\":201326592,\"per_plane_cap_bytes\":67108864"
         << ",\"raw_saved_before_validation\":true,\"readback_completed\":true"
         << ",\"begin_end_submit_checked\":true,\"completion_checked\":true"
         << ",\"explicit_buffer_dependencies\":true,\"insert_barrier_buffer_ignored\":true"
         << ",\"map_after_completion\":true,\"production_writes\":false"
         << ",\"gpu_proof_status_not_cpu_certified\":true,\"physical_transport_certified\":false"
         << ",\"production_candidate\":false,\"default_changed\":false"
         << ",\"scope\":\"zero-cross scalar parent only; N-only factor replay; no N/rho/sampler/history/SC writes\"}\n";
    meta.close();
    if (!meta) return fail("factor metadata write failed");
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_ZERO_NORMAL_FACTOR] complete proof=" << proofCount
        << " frame=" << frame << std::endl;
    return true;
}
