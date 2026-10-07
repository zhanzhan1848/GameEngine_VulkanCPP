// Included after DDGIFirstRecordProofOwned. Private DUMP2 computation only.
static bool RunDDGIFirstCoherentReplay(
    RHIDeviceBase* device, lumen::LumenDDGIPass* pass, u32 frame,
    ResourceHandle record, u32 recordStride, u32 freshRayCount) {
    const char* enabled = std::getenv("DDGI_FIRST_COHERENT_REPLAY");
    if (!enabled || std::strcmp(enabled, "1") != 0) return true;
    const char* value = std::getenv("DDGI_FIRST_RECORD_PROOF_PREFIX");
    if (!value || !*value) return false;
    const std::string prefix = std::string(value) + ".coherent";
    bool rawSaved = false;
    auto fail = [&](const char* reason) {
        std::ofstream meta(prefix + ".json", std::ios::trunc);
        if (meta) meta << "{\"complete\":false,\"raw_evidence_preserved\":"
            << (rawSaved ? "true" : "false") << ",\"error\":\"" << reason << "\"}\n";
        PRIMAL_LOG_STREAM(Error, "Render") << "[DDGI_FIRST_COHERENT_REPLAY] rejected: " << reason << std::endl;
        return false;
    };
    const char* dump = std::getenv("DDGI_DUMP");
    if (!device || !pass || device->GetPlatform() != RHIPlatform::Vulkan ||
        !dump || std::strcmp(dump, "2") != 0 || recordStride != 69u ||
        freshRayCount == 0u || freshRayCount > 32768u)
        return fail("requires DUMP2 Vulkan bounded interval69 record");
    auto* vk = static_cast<VulkanDevice*>(device);
    const auto slots = lumen::DDGISlotsForFrame(frame);
    const ResourceHandle history = pass->GetIrradianceBufferBySlot(slots.previousIndex);
    const ResourceHandle volume = pass->GetVolumeUniformBuffer(frame);
    const ResourceHandle normals = pass->GetHitNormalBuffer();
    const auto& vd = pass->GetVolumeData();
    if (vd.FrameIndex != frame || vd.RaysPerProbe != 128u ||
        vd.ProbeUpdateCount * u64(vd.RaysPerProbe) != freshRayCount ||
        vd.SdfExtents[2].w <= 0.5f || vd.SkyColor.w != 0.0f)
        return fail("requires sameframe actual query source and default signed history mode");
    const u64 fullCapacity = u64(vd.ProbeCountTotal) * vd.RaysPerProbe;
    const u64 queryOffset = 9u * fullCapacity * sizeof(math::v4);
    ResourceHandle sources[5] = {record, history, volume, normals, normals};
    u64 offsets[5] = {0u, 0u, 0u, queryOffset, 0u};
    u64 extents[5]{};
    for (u32 i = 0; i < 5u; ++i)
        if (sources[i] == handles::INVALID_RESOURCE || !vk->GetBufferByteSize(sources[i], extents[i]))
            return fail("actual source buffer unavailable");
    u64 bytes[5] = {u64(freshRayCount) * 69u * sizeof(math::v4),
        u64(vd.ProbeCountTotal) * 9u * sizeof(math::v4), extents[2],
        u64(freshRayCount) * 18u * sizeof(math::v4), extents[4]};
    if (extents[0] != bytes[0] || bytes[2] < sizeof(lumen::DDGIVolumeData) ||
        bytes[2] > 4096u || bytes[2] % 16u || bytes[4] != 27u * fullCapacity * sizeof(math::v4))
        return fail("unsupported actual CB or record/query/HN extent");
    for (u32 i = 0; i < 5u; ++i)
        if (offsets[i] > extents[i] || bytes[i] > extents[i] - offsets[i])
            return fail("actual source range out of bounds");
    DDGIFirstRecordProofOwned owned;
    owned.device = device;
    auto makeBuffer = [&](u64 size, bool readback, const char* name) {
        BufferDesc desc{}; desc.size = size; desc.type = BufferType::Structured;
        desc.usage = desc.memoryUsage = readback ? GPUMemoryUsage::Readback : GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(readback ? BufferUsageFlags::TransferDst
            : BufferUsageFlags::Storage | BufferUsageFlags::TransferSrc);
        desc.name = name;
        const auto h = device->CreateBuffer(desc); owned.buffers.push_back(h); return h;
    };
    const u64 outputBytes = u64(freshRayCount) * 32u * sizeof(math::v4);
    if (outputBytes > 32u * 1024u * 1024u) return fail("private output budget exceeded");
    const auto output = makeBuffer(outputBytes, false, "DDGIFirstCoherentReplayOutput");
    const auto outputReadback = makeBuffer(outputBytes, true, "DDGIFirstCoherentReplayReadback");
    ResourceHandle sourceReadbacks[5]{};
    for (u32 i = 0; i < 5u; ++i)
        sourceReadbacks[i] = makeBuffer(bytes[i], true, "DDGIFirstCoherentReplaySource");
    // Extra before/after snapshots certify the two sources not covered by
    // the enclosing FirstRecord production five-resource snapshot.
    const auto historyBefore = makeBuffer(bytes[1], true, "DDGIFirstCoherentHistoryBefore");
    const auto volumeBefore = makeBuffer(bytes[2], true, "DDGIFirstCoherentVolumeBefore");
    for (const auto h : owned.buffers)
        if (h == handles::INVALID_RESOURCE) return fail("private readback allocation failed");
    const char* module = "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstCoherentReplay.comp.spv";
    const auto spv = LoadSpvBytes(module);
    if (spv.empty()) return fail("private replay SPIR-V missing");
    owned.shader = device->CreateShader(spv.data(), spv.size(), ShaderStage::Compute, "ddgi_first_coherent_replay");
    DescriptorSetLayoutBinding bindings[6]{};
    for (u32 i = 0; i < 6u; ++i)
        bindings[i] = {i, i == 2u ? DescriptorType::UniformBuffer : DescriptorType::StorageBuffer,
            1u, ShaderStage::Compute, nullptr};
    DescriptorSetLayoutDesc ld{6u, bindings};
    owned.layout = device->CreateDescriptorSetLayout(ld);
    PipelineLayoutDesc pd{}; pd.setLayoutCount = 1u; pd.setLayouts = &owned.layout;
    owned.pipelineLayout = device->CreatePipelineLayout(pd);
    DescriptorSetDesc sd{}; sd.layout = owned.layout;
    owned.descriptorSet = device->CreateDescriptorSet(sd);
    if (owned.shader == handles::INVALID_SHADER || owned.layout == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        owned.pipelineLayout == handles::INVALID_PIPELINE_LAYOUT || owned.descriptorSet == handles::INVALID_DESCRIPTOR_SET)
        return fail("private shader/layout/set creation failed");
    DescriptorBufferInfo infos[6]{};
    for (u32 i = 0; i < 5u; ++i) infos[i] = {sources[i], offsets[i], bytes[i]};
    infos[5] = {output, 0u, outputBytes};
    WriteDescriptorSet writes[6]{};
    for (u32 i = 0; i < 6u; ++i)
        writes[i] = {owned.descriptorSet, i, 0u, 1u, bindings[i].descriptorType, nullptr, &infos[i]};
    device->UpdateDescriptorSets(6u, writes);
    ComputePipelineDesc cd{}; cd.computeShader = owned.shader; cd.layout = owned.pipelineLayout;
    owned.pipeline = device->CreateComputePipeline(cd);
    owned.command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (owned.pipeline == handles::INVALID_PIPELINE || owned.command == handles::INVALID_COMMAND_BUFFER)
        return fail("private pipeline/command creation failed");
    auto* cmd = vk->GetCommandBuffer(owned.command);
    if (!cmd || !cmd->Reset() || !cmd->Begin()) return fail("private Reset/Begin failed");
    cmd->MemoryBarrier(PipelineStage::Host | PipelineStage::AllCommands | PipelineStage::ComputeShader | PipelineStage::Transfer,
        PipelineStage::Transfer, AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::ShaderWrite | AccessFlag::TransferWrite,
        AccessFlag::TransferRead);
    cmd->CopyBuffer(history, historyBefore, 0u, 0u, bytes[1]);
    cmd->CopyBuffer(volume, volumeBefore, 0u, 0u, bytes[2]);
    cmd->MemoryBarrier(PipelineStage::Host | PipelineStage::AllCommands | PipelineStage::ComputeShader | PipelineStage::Transfer,
        PipelineStage::ComputeShader, AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::ShaderWrite | AccessFlag::TransferWrite,
        AccessFlag::ShaderRead | AccessFlag::UniformRead | AccessFlag::ShaderWrite);
    cmd->BindComputePipeline(owned.pipeline);
    cmd->BindDescriptorSets(PipelineBindPoint::Compute, owned.pipelineLayout, 0u, 1u, &owned.descriptorSet, 0u, nullptr);
    cmd->Dispatch((freshRayCount + 31u) / 32u, 1u, 1u);
    cmd->MemoryBarrier(PipelineStage::ComputeShader | PipelineStage::Host | PipelineStage::AllCommands | PipelineStage::Transfer,
        PipelineStage::Transfer, AccessFlag::ShaderRead | AccessFlag::ShaderWrite | AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::TransferWrite,
        AccessFlag::TransferRead);
    cmd->CopyBuffer(output, outputReadback, 0u, 0u, outputBytes);
    for (u32 i = 0; i < 5u; ++i) cmd->CopyBuffer(sources[i], sourceReadbacks[i], offsets[i], 0u, bytes[i]);
    cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host, AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!cmd->End() || !cmd->Submit() || !cmd->WaitForCompletion()) return fail("private End/Submit/Wait failed");
    auto map = [&](ResourceHandle h, u64 size, const char* suffix, std::vector<u8>& data) {
        const void* p = device->MapBuffer(h); if (!p) return false;
        data.resize(static_cast<size_t>(size)); std::memcpy(data.data(), p, data.size()); device->UnmapBuffer(h);
        std::ofstream f(prefix + suffix, std::ios::binary | std::ios::trunc);
        if (!f) return false; f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())); return bool(f);
    };
    std::vector<u8> results, raw[5], beforeHistory, beforeVolume;
    if (!map(outputReadback, outputBytes, ".raw", results)) return fail("private output Map/write failed");
    rawSaved = true;
    const char* suffixes[5] = {".record.bin", ".history.bin", ".volume.bin", ".query.bin", ".hn.bin"};
    for (u32 i = 0; i < 5u; ++i)
        if (!map(sourceReadbacks[i], bytes[i], suffixes[i], raw[i])) return fail("private source Map/write failed");
    if (!map(historyBefore, bytes[1], ".history-before.bin", beforeHistory) ||
        !map(volumeBefore, bytes[2], ".volume-before.bin", beforeVolume)) return fail("private extra source before Map failed");
    if (beforeHistory != raw[1] || beforeVolume != raw[2]) return fail("private replay altered resident history/volume");
    lumen::DDGIVolumeData resident{}; std::memcpy(&resident, raw[2].data(), sizeof(resident));
    if (resident.FrameIndex != frame || resident.ProbeUpdateCount * u64(resident.RaysPerProbe) != freshRayCount)
        return fail("resident GPU volume stamp mismatch");
    std::ofstream meta(prefix + ".json", std::ios::trunc);
    if (!meta) return fail("private metadata open failed");
    meta << "{\"schema\":\"ddgi_first_coherent_replay_v1\",\"complete\":true,\"frame\":" << frame
        << ",\"fresh_ray_count\":" << freshRayCount << ",\"full_ray_capacity\":" << fullCapacity
        << ",\"result_vec4_stride\":32,\"record_vec4_stride\":69,\"output_bytes\":" << outputBytes
        << ",\"module\":\"" << module << "\",\"entrypoint\":\"ddgi_first_coherent_replay\""
        << ",\"shader_handle\":" << owned.shader << ",\"pipeline_handle\":" << owned.pipeline
        << ",\"pipeline_layout_handle\":" << owned.pipelineLayout << ",\"descriptor_set_handle\":" << owned.descriptorSet
        << ",\"source_handles\":[" << sources[0] << ',' << sources[1] << ',' << sources[2] << ',' << sources[3] << ',' << sources[4] << ']'
        << ",\"source_offsets\":[" << offsets[0] << ',' << offsets[1] << ',' << offsets[2] << ',' << offsets[3] << ',' << offsets[4] << ']'
        << ",\"source_bytes\":[" << bytes[0] << ',' << bytes[1] << ',' << bytes[2] << ',' << bytes[3] << ',' << bytes[4] << ']'
        << ",\"output_handle\":" << output << ",\"volume_slot\":" << slots.currentIndex << ",\"history_slot\":" << slots.previousIndex
        << ",\"history_volume_before_after_byteidentical\":true,\"enclosed_by_production_five_snapshot\":true"
        << ",\"readback_completed\":true,\"begin_end_submit_checked\":true,\"completion_checked\":true"
        << ",\"explicit_buffer_dependencies\":true,\"map_after_completion\":true,\"production_writes\":false"
        << ",\"production_eligible\":0,\"scope\":\"same-record GPU counterfactual; preserves original native rejected status, not physical transport or production accept\"}\n";
    meta.close();
    if (!meta) return fail("private metadata write failed");
    PRIMAL_LOG_STREAM(Error, "Render") << "[DDGI_FIRST_COHERENT_REPLAY] complete frame=" << frame << " fresh=" << freshRayCount << std::endl;
    return true;
}
