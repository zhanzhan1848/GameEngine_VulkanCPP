// Opt-in Vulkan test-host diagnostic: exact same-frame shadow-map depth planes
// and matrices.  No production pass, descriptor, or lighting output changes.
static bool RunDDGIFirstShadowMapDump(
    RHIDeviceBase* device, StandardRenderPipeline* pipeline,
    u32 frame, u32 renderSlot, const char* prefix) {
    if (!device || !pipeline || !prefix || !prefix[0] ||
        device->GetPlatform() != RHIPlatform::Vulkan) return false;
    auto* shadow = pipeline->GetShadowMapModule();
    if (!shadow) return false;
    constexpr u32 kCascades = 4;
    ResourceHandle textures[kCascades]{};
    u32 width = 0, height = 0;
    for (u32 c = 0; c < kCascades; ++c) {
        textures[c] = nanite::GPUDrivenDrawPipeline::Get().GetShadowMap(c, renderSlot);
        u32 w = 0, h = 0;
        if (textures[c] == handles::INVALID_RESOURCE ||
            device->GetTextureFormat(textures[c]) != DataFormat::R32_Float ||
            !device->GetTextureSize(textures[c], w, h) || !w || !h ||
            w > 4096 || h > 4096 || (c && (w != width || h != height))) return false;
        width = w; height = h;
    }
    const auto* matrices = shadow->GetCachedShadowVP(renderSlot);
    if (!matrices || sizeof(math::m4x4) != 64) return false;
    const u64 cascadeBytes = u64(width) * height * sizeof(float);
    const u64 totalBytes = cascadeBytes * kCascades;
    if (!cascadeBytes || totalBytes > 256ull * 1024ull * 1024ull) return false;

    BufferDesc desc{};
    desc.size = totalBytes;
    desc.type = BufferType::Raw;
    desc.memoryUsage = GPUMemoryUsage::Readback;
    desc.bindFlags = u32(BufferUsageFlags::TransferDst);
    desc.name = "DDGI_FirstShadowMapDepthReadback";
    const ResourceHandle staging = device->CreateBuffer(desc);
    const auto command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto cleanup = [&] {
        if (command != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(command);
        if (staging != handles::INVALID_RESOURCE) device->DestroyBuffer(staging);
    };
    if (staging == handles::INVALID_RESOURCE || command == handles::INVALID_COMMAND_BUFFER) {
        cleanup(); return false;
    }
    auto* cmd = static_cast<VulkanDevice*>(device)->GetCommandBuffer(command);
    if (!cmd || !cmd->Reset() || !cmd->Begin()) { cleanup(); return false; }
    cmd->MemoryBarrier(PipelineStage::AllCommands, PipelineStage::Transfer,
                       AccessFlag::MemoryWrite, AccessFlag::TransferRead);
    for (u32 c = 0; c < kCascades; ++c) {
        ResourceBarrier toCopy{};
        toCopy.resource = textures[c];
        toCopy.beforeState = ResourceState::ShaderResource;
        toCopy.afterState = ResourceState::CopySource;
        toCopy.subresource = 0xFFFFFFFFu;
        cmd->InsertBarrier(&toCopy, 1);
        BufferTextureCopyRegion region{};
        region.bufferOffset = cascadeBytes * c;
        region.bufferRowLength = width;
        region.imageSubresource = {0, 0, 1};
        region.imageExtent = {width, height, 1};
        cmd->CopyTextureToBuffer(textures[c], staging, &region, 1);
        ResourceBarrier back = toCopy;
        back.beforeState = ResourceState::CopySource;
        back.afterState = ResourceState::ShaderResource;
        cmd->InsertBarrier(&back, 1);
    }
    cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                       AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!cmd->End() || !cmd->Submit() || !cmd->WaitForCompletion()) {
        cleanup(); return false;
    }
    const auto* mapped = static_cast<const char*>(device->MapBuffer(staging));
    if (!mapped) { cleanup(); return false; }
    const std::string base(prefix);
    std::ofstream raw(base + ".depth.raw", std::ios::binary);
    raw.write(mapped, totalBytes);
    const bool rawOk = bool(raw);
    raw.close();
    device->UnmapBuffer(staging);
    if (!rawOk) { cleanup(); return false; }
    std::ofstream matrixFile(base + ".matrices.raw", std::ios::binary);
    matrixFile.write(reinterpret_cast<const char*>(matrices), sizeof(math::m4x4) * kCascades);
    const bool matricesOk = bool(matrixFile);
    matrixFile.close();
    if (!matricesOk) { cleanup(); return false; }
    std::ofstream meta(base + ".json");
    meta << "{\"schema\":\"ddgi_first_shadow_map_depth_v1\",\"complete\":true,"
         << "\"frame\":" << frame << ",\"render_slot\":" << renderSlot
         << ",\"cascade_count\":" << kCascades
         << ",\"width\":" << width << ",\"height\":" << height
         << ",\"format\":\"R32_Float\",\"cascade_bytes\":" << cascadeBytes
         << ",\"depth_bytes\":" << totalBytes
         << ",\"matrix_bytes\":" << sizeof(math::m4x4) * kCascades
         << ",\"begin_end_submit_checked\":true,\"completion_checked\":true,"
         << "\"image_layout_dependencies\":true,\"explicit_host_dependency\":true,"
         << "\"map_after_completion\":true,\"default_changed\":false}";
    const bool metaOk = bool(meta);
    cleanup();
    if (metaOk) PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_FIRST_SHADOW_MAP_DUMP] frame=" << frame
        << " cascades=" << kCascades << " size=" << width << "x" << height << std::endl;
    return metaOk;
}
