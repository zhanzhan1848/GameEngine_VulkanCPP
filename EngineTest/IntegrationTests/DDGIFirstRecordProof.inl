// DUMP=2-only, read-only proof for the first record's native shading normal.
// This file is included inside TestVulkanSponzaRenderGraph's anonymous
// namespace.  It deliberately consumes the analytic-first-rho audit already
// produced by the current Finalize dispatch; it never enables or writes a
// production first-surface/native-normal candidate.

struct DDGIFirstRecordProofOwned {
    RHIDeviceBase* device{nullptr};
    std::vector<ResourceHandle> buffers;
    ShaderHandle shader{handles::INVALID_SHADER};
    DescriptorSetLayoutHandle layout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle pipelineLayout{handles::INVALID_PIPELINE_LAYOUT};
    DescriptorSetHandle descriptorSet{handles::INVALID_DESCRIPTOR_SET};
    PipelineHandle pipeline{handles::INVALID_PIPELINE};
    CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};

    ~DDGIFirstRecordProofOwned() {
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
        for (ResourceHandle buffer : buffers) {
            if (buffer != handles::INVALID_RESOURCE)
                device->DestroyBuffer(buffer);
        }
    }
};

#include "DDGIFirstCoherentReplay.inl"
#include "DDGIFirstFactorReplay.inl"

static u32 DDGIFirstRecordProofBits(float value) {
    u32 bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool DDGIFirstRecordProofFinite(float value) {
    const u32 bits = DDGIFirstRecordProofBits(value);
    return (bits & 0x7f800000u) != 0x7f800000u;
}

// Read-only active device/global-driver precision metadata. Advertised
// support and MoltenVK global configuration are not proof of per-shader
// floating options; exact SPIR-V execution modes/decorations are joined by
// the independent reader.
static bool SaveDDGIFirstRecordPrecision(VulkanDevice* vk, const std::string& prefix,
                                         u32 frame, bool requireStrict) {
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceFloatControlsProperties controls{};
    controls.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
    controls.pNext = &driver;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &controls;
    vkGetPhysicalDeviceProperties2(vk->GetNativePhysicalDevice(), &props);
    if (requireStrict &&
        (props.properties.apiVersion < VK_API_VERSION_1_2 ||
         !controls.shaderRoundingModeRTEFloat32 ||
         !controls.shaderSignedZeroInfNanPreserveFloat32)) return false;
    auto quoted = [](const char* value) {
        std::string result = "\"";
        for (const char* p = value; p && *p; ++p) {
            if (*p == '\\' || *p == '\"') result += '\\';
            if (static_cast<unsigned char>(*p) < 32u) return std::string("\"unsupported control char\"");
            result += *p;
        }
        return result + "\"";
    };
    std::ofstream meta(prefix + ".precision.json", std::ios::trunc);
    if (!meta) return false;
    meta << "{\"frame\":" << frame
         << ",\"native_physical_device\":" << (u64)(uintptr_t)vk->GetNativePhysicalDevice()
         << ",\"driver_id\":" << driver.driverID
         << ",\"driver_name\":" << quoted(driver.driverName)
         << ",\"driver_info\":" << quoted(driver.driverInfo)
         << ",\"api_version\":" << props.properties.apiVersion
         << ",\"advertised_float32\":{\"signed_zero_inf_nan_preserve\":"
         << controls.shaderSignedZeroInfNanPreserveFloat32
         << ",\"denorm_preserve\":" << controls.shaderDenormPreserveFloat32
         << ",\"denorm_flush_to_zero\":" << controls.shaderDenormFlushToZeroFloat32
         << ",\"rounding_RTE\":" << controls.shaderRoundingModeRTEFloat32
         << ",\"rounding_RTZ\":" << controls.shaderRoundingModeRTZFloat32 << "}"
         << ",\"shader_options_independently_queried\":false";
#if defined(DDGI_RECORD_MVK_PRIVATE_QUERY_AVAILABLE)
    void* library = driver.driverID == VK_DRIVER_ID_MOLTENVK
        ? dlopen("/usr/local/lib/libMoltenVK.dylib", RTLD_LAZY | RTLD_NOLOAD) : nullptr;
    auto query = library ? reinterpret_cast<PFN_vkGetMoltenVKConfigurationMVK>(
        dlsym(library, "vkGetMoltenVKConfigurationMVK")) : nullptr;
    MVKConfiguration config{};
    size_t size = sizeof(config);
    const VkResult result = query
        ? query(VK_NULL_HANDLE, &config, &size) : VK_ERROR_EXTENSION_NOT_PRESENT;
    const bool field = query && (result == VK_SUCCESS || result == VK_INCOMPLETE) &&
        size >= offsetof(MVKConfiguration, fastMathEnabled) + sizeof(config.fastMathEnabled);
    meta << ",\"mvk_global_configuration\":{\"query_result\":" << result
         << ",\"copied_bytes\":" << size
         << ",\"header_struct_bytes\":" << sizeof(config)
         << ",\"already_loaded_library\":" << (library ? "true" : "false")
         << ",\"fast_math_field_available\":" << (field ? "true" : "false")
         << ",\"fast_math_mode\":" << (field ? int(config.fastMathEnabled) : -1)
         << ",\"scope\":\"read-only global driver config; not actual per-shader compile options\"}";
    if (library) dlclose(library);
#else
    meta << ",\"mvk_global_configuration\":null";
#endif
    meta << ",\"configuration_changed\":false}\n";
    return bool(meta);
}

static bool RunDDGIFirstRecordProof(
    RHIDeviceBase* device,
    lumen::LumenDDGIPass* pass,
    u32 frame,
    const std::vector<u32>& updateList,
    u32 fullRayCapacity,
    u32 raysPerProbe,
    const nanite::GPUMaterialRegistry* materialRegistry) {
    const char* prefixValue = std::getenv("DDGI_FIRST_RECORD_PROOF_PREFIX");
    if (!prefixValue || !prefixValue[0]) return true;
    const std::string prefix(prefixValue);
    const char* strictValue = std::getenv("DDGI_FIRST_RECORD_STRICT");
    const bool strict = strictValue && std::strcmp(strictValue, "1") == 0;
    const char* primaryValue = std::getenv("DDGI_FIRST_RECORD_PRIMARY_FRAME");
    const bool primaryFrame = primaryValue && std::strcmp(primaryValue, "1") == 0;
    const char* intervalValue = std::getenv("DDGI_FIRST_RECORD_INTERVAL");
    const bool interval = intervalValue && std::strcmp(intervalValue, "1") == 0;
    bool rawEvidenceSaved = false;
    auto fail = [&](const char* why) {
        std::ofstream meta(prefix + ".json", std::ios::trunc);
        if (meta) {
            meta << "{\"schema\":\"ddgi_first_record_proof_v1\","
                    "\"complete\":false,\"readback_completed\":false,"
                    "\"explicit_buffer_dependencies\":false,"
                    "\"begin_end_submit_checked\":false,"
                    "\"completion_checked\":false,\"map_after_completion\":false,"
                    "\"raw_evidence_preserved\":"
                 << (rawEvidenceSaved ? "true" : "false")
                 << ",\"error\":\"" << why << "\"}\n";
        }
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_RECORD_PROOF] rejected: " << why << std::endl;
        return false;
    };

    if (interval && (!strict || !primaryFrame))
        return fail("interval proof requires strict arithmetic and primary-frame variant");
    if (primaryFrame && !strict)
        return fail("primary-frame proof requires DDGI_FIRST_RECORD_STRICT=1");
    const char* dump = std::getenv("DDGI_DUMP");
    const char* auditPrefix = std::getenv("DDGI_ANALYTIC_FIRST_RHO_PREFIX");
    if (!device || !pass || device->GetPlatform() != RHIPlatform::Vulkan ||
        std::strcmp(dump ? dump : "", "2") != 0 ||
        !auditPrefix || !auditPrefix[0] || !pass->IsAnalyticFirstRhoEnabled() ||
        !materialRegistry)
        return fail("requires Vulkan, DDGI_DUMP=2, analytic-first-rho audit, and material registry");
    if (raysPerProbe != 128u || fullRayCapacity == 0u || updateList.empty() ||
        updateList.size() > 16384u ||
        pass->GetVolumeData().ProbeCountTotal != fullRayCapacity / raysPerProbe)
        return fail("unsupported fresh update-list or scalar volume contract");

    std::vector<u32> sortedUpdates = updateList;
    std::sort(sortedUpdates.begin(), sortedUpdates.end());
    if (std::adjacent_find(sortedUpdates.begin(), sortedUpdates.end()) != sortedUpdates.end() ||
        sortedUpdates.back() >= pass->GetVolumeData().ProbeCountTotal)
        return fail("fresh update list is not unique and bounded");

    const u64 freshRayCount = u64(updateList.size()) * raysPerProbe;
    static constexpr u64 kAuditVec4Stride = 16u;
    const u64 kProofVec4Stride = interval ? 69u : 41u;
    const u64 proofBytes = freshRayCount * kProofVec4Stride * sizeof(math::v4);
    const u64 denseAuditBytes = freshRayCount * kAuditVec4Stride * sizeof(math::v4);
    if (freshRayCount == 0u || proofBytes == 0u || proofBytes > 64ull * 1024ull * 1024ull)
        return fail("fresh proof output exceeds the bounded 64 MiB diagnostic budget");

    auto* vk = static_cast<VulkanDevice*>(device);
    if (!SaveDDGIFirstRecordPrecision(vk, prefix, frame, strict))
        return fail("precision metadata read-only write failed");
    auto bufferBytes = [&](ResourceHandle handle, u64& bytes) {
        return handle != handles::INVALID_RESOURCE &&
               vk->GetBufferByteSize(handle, bytes) && bytes != 0u &&
               bytes <= 256ull * 1024ull * 1024ull;
    };

    const ResourceHandle audit = pass->GetAnalyticFirstRhoAuditBuffer();
    const ResourceHandle nodes = pass->GetAnalyticFirstRhoNodesBuffer();
    const ResourceHandle triangles = pass->GetAnalyticFirstRhoTrianglesBuffer();
    const ResourceHandle mapping = pass->GetAnalyticFirstRhoMappingBuffer();
    const ResourceHandle positions = pass->GetAnalyticFirstRhoPositions();
    const ResourceHandle elements = pass->GetAnalyticFirstRhoElements();
    const ResourceHandle instances = pass->GetAnalyticFirstRhoInstances();
    const ResourceHandle materials = pass->GetAnalyticFirstRhoMaterials();
    const ResourceHandle normalTexture = nanite::GPUDrivenDrawPipeline::Get().GetNormalTextureArray();
    const SamplerHandle normalSampler = nanite::GPUDrivenDrawPipeline::Get().GetTextureSampler();
    const ResourceHandle liveSources[4] = {
        nanite::GPUDrivenDrawPipeline::Get().GetGlobalVertexBuffer(),
        nanite::GPUDrivenDrawPipeline::Get().GetGlobalElementBuffer(),
        nanite::GPUDrivenDrawPipeline::Get().GetGlobalInstanceDataBuffer(),
        nanite::GPUDrivenDrawPipeline::Get().GetMaterialDataBuffer()};
    const ResourceHandle analyticSources[4] = {positions, elements, instances, materials};
    for (u32 i = 0; i < 4u; ++i)
        if (analyticSources[i] != liveSources[i])
            return fail("analytic proof source identity differs from current GPUDrivenDraw");
    if (normalTexture == handles::INVALID_RESOURCE ||
        normalSampler == handles::INVALID_SAMPLER)
        return fail("current native normal texture or sampler is unavailable");

    const ResourceHandle inputBuffers[7] = {
        nodes, triangles, mapping, positions, elements, instances, materials};
    u64 inputBytes[7]{};
    for (u32 i = 0; i < 7u; ++i)
        if (!bufferBytes(inputBuffers[i], inputBytes[i]))
            return fail("proof source buffer handle or size is invalid");
    u64 auditBytes = 0u;
    if (!bufferBytes(audit, auditBytes) || auditBytes < denseAuditBytes ||
        auditBytes % (kAuditVec4Stride * sizeof(math::v4)) != 0u)
        return fail("analytic first-rho audit is incomplete for the fresh rays");
    if (inputBytes[2] % 32u != 0u || inputBytes[1] % 48u != 0u ||
        inputBytes[0] % 48u != 0u || inputBytes[3] % 12u != 0u ||
        inputBytes[4] % 24u != 0u || inputBytes[5] % 192u != 0u ||
        inputBytes[6] % 48u != 0u)
        return fail("proof source byte layout is not nodes/triangles/mapping32/native ABI");

    auto* normalObject = vk->GetTexture(normalTexture);
    if (!normalObject) return fail("current native normal texture object is unavailable");
    const TextureDesc normalDesc = normalObject->GetTextureDesc();
    if (normalDesc.type != TextureType::Texture2DArray || normalDesc.size.x == 0u ||
        normalDesc.size.y == 0u || normalDesc.arraySize == 0u || normalDesc.mipLevels == 0u ||
        (normalDesc.format != DataFormat::RGBA8_UNorm &&
         normalDesc.format != DataFormat::RGBA8_sRGB))
        return fail("current native normal texture descriptor is unsupported");

    const ResourceHandle production[5] = {
        pass->GetHitDistanceBuffer(), pass->GetHitNormalBuffer(),
        pass->GetRayDataBuffer(), pass->GetIrradianceBufferLatest(),
        pass->GetProbeUpdateListBuffer(frame)};
    const char* productionNames[5] = {
        "hit_distance", "hit_normal", "ray_data", "current_sh", "update_list"};
    u64 productionBytes[5]{};
    for (u32 i = 0; i < 5u; ++i)
        if (!bufferBytes(production[i], productionBytes[i]))
            return fail("production snapshot source is unavailable");

    auto createReadback = [&](u64 bytes, const char* name) -> ResourceHandle {
        BufferDesc desc{};
        desc.size = bytes;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Readback;
        desc.memoryUsage = GPUMemoryUsage::Readback;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        desc.name = name;
        return device->CreateBuffer(desc);
    };

    auto captureSnapshot = [&](const char* label,
                               std::vector<std::vector<u8>>& result) -> bool {
        std::vector<ResourceHandle> readbacks(5u, handles::INVALID_RESOURCE);
        auto cleanup = [&]() {
            for (ResourceHandle rb : readbacks)
                if (rb != handles::INVALID_RESOURCE) device->DestroyBuffer(rb);
        };
        for (u32 i = 0; i < 5u; ++i) {
            readbacks[i] = createReadback(productionBytes[i], "DDGIFirstRecordProductionReadback");
            if (readbacks[i] == handles::INVALID_RESOURCE) {
                cleanup();
                return false;
            }
        }
        CommandBufferHandle handle = device->CreateCommandBuffer(CommandQueueType::Graphics);
        auto* command = vk->GetCommandBuffer(handle);
        if (!command || !command->Reset() || !command->Begin()) {
            if (handle != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(handle);
            cleanup();
            return false;
        }
        command->MemoryBarrier(
            PipelineStage::Host | PipelineStage::AllCommands |
                PipelineStage::ComputeShader | PipelineStage::VertexShader |
                PipelineStage::FragmentShader | PipelineStage::Transfer,
            PipelineStage::Transfer,
            AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::ShaderRead |
                AccessFlag::ShaderWrite | AccessFlag::VertexAttributeRead |
                AccessFlag::TransferWrite,
            AccessFlag::TransferRead);
        for (u32 i = 0; i < 5u; ++i)
            command->CopyBuffer(production[i], readbacks[i], 0u, 0u, productionBytes[i]);
        command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                               AccessFlag::TransferWrite, AccessFlag::HostRead);
        const bool submitted = command->End() && command->Submit() && command->WaitForCompletion();
        if (!submitted) {
            device->DestroyCommandBuffer(handle);
            cleanup();
            return false;
        }
        result.clear();
        result.resize(5u);
        bool mapped = true;
        for (u32 i = 0; i < 5u; ++i) {
            result[i].resize(static_cast<size_t>(productionBytes[i]));
            const void* data = device->MapBuffer(readbacks[i]);
            if (!data) {
                mapped = false;
                break;
            }
            std::memcpy(result[i].data(), data, result[i].size());
            device->UnmapBuffer(readbacks[i]);
            std::ofstream raw(prefix + ".production_" + label + "_" + productionNames[i] + ".raw",
                              std::ios::binary | std::ios::trunc);
            if (!raw) mapped = false;
            else {
                raw.write(reinterpret_cast<const char*>(result[i].data()),
                          std::streamsize(result[i].size()));
                if (!raw) mapped = false;
            }
            if (!mapped) break;
        }
        device->DestroyCommandBuffer(handle);
        cleanup();
        return mapped;
    };

    std::vector<std::vector<u8>> productionBefore;
    if (!captureSnapshot("before", productionBefore))
        return fail("production before snapshot End/Submit/Wait/Map failed");

    DDGIFirstRecordProofOwned owned;
    owned.device = device;
    const u64 outputBytes = proofBytes;

    BufferDesc outputDesc{};
    outputDesc.size = outputBytes;
    outputDesc.type = BufferType::Structured;
    outputDesc.usage = GPUMemoryUsage::Dynamic;
    outputDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    outputDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage |
                                             BufferUsageFlags::TransferSrc);
    outputDesc.name = "DDGIFirstRecordProofOutput";
    ResourceHandle outputBuffer = device->CreateBuffer(outputDesc);
    ResourceHandle outputReadback = createReadback(outputBytes, "DDGIFirstRecordProofOutputReadback");
    ResourceHandle auditReadback = createReadback(denseAuditBytes, "DDGIFirstRecordProofAuditReadback");
    ResourceHandle paramsBuffer = handles::INVALID_RESOURCE;
    BufferDesc paramsDesc{};
    paramsDesc.size = sizeof(u32) * 4u;
    paramsDesc.type = BufferType::Structured;
    paramsDesc.usage = GPUMemoryUsage::Dynamic;
    paramsDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    paramsDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage | BufferUsageFlags::TransferSrc);
    paramsDesc.name = "DDGIFirstRecordProofParams";
    paramsBuffer = device->CreateBuffer(paramsDesc);
    owned.buffers = {outputBuffer, outputReadback, auditReadback, paramsBuffer};
    if (outputBuffer == handles::INVALID_RESOURCE ||
        outputReadback == handles::INVALID_RESOURCE ||
        auditReadback == handles::INVALID_RESOURCE ||
        paramsBuffer == handles::INVALID_RESOURCE)
        return fail("proof output/readback allocation failed");
    for (u32 i = 0; i < 7u; ++i) {
        ResourceHandle rb = createReadback(inputBytes[i], "DDGIFirstRecordProofSourceReadback");
        if (rb == handles::INVALID_RESOURCE) return fail("proof source readback allocation failed");
        owned.buffers.push_back(rb);
    }

    ResourceHandle paramsReadback = createReadback(sizeof(u32) * 4u, "DDGIFirstRecordProofParamsReadback");
    if (paramsReadback == handles::INVALID_RESOURCE) return fail("params readback allocation failed");
    owned.buffers.push_back(paramsReadback);

    u32 params[4] = {0u, static_cast<u32>(freshRayCount),
                     static_cast<u32>(kProofVec4Stride), 1u};
    params[0] = DDGIFirstRecordProofBits(pass->GetVolumeData().RayMaxDistance);
    if (!device->UpdateBufferData(paramsBuffer, params, sizeof(params), 0u))
        return fail("proof params upload failed");

    const char* proofModule = interval
        ? "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstRecordProof_strict_primary_interval.comp.spv"
        : primaryFrame
        ? "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstRecordProof_strict_primary.comp.spv"
        : strict
        ? "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstRecordProof_strict.comp.spv"
        : "Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstRecordProof.comp.spv";
    auto spv = LoadSpvBytes(proofModule);
    if (spv.empty()) return fail("first-record proof SPIR-V is unavailable");
    if (strict) {
        // Fail closed on a stale/non-strict file before native shader creation.
        if (spv.size() < 20u || spv.size() % 4u != 0u)
            return fail("strict proof SPIR-V size invalid");
        std::vector<u32> words(spv.size() / 4u);
        std::memcpy(words.data(), spv.data(), spv.size());
        bool rte = false, preserve = false;
        std::vector<u32> arithmetic, decorated;
        if (words[0] != 0x07230203u) return fail("strict proof SPIR-V magic invalid");
        for (size_t i = 5u; i < words.size();) {
            const u32 count = words[i] >> 16u, op = words[i] & 65535u;
            if (!count || i + count > words.size()) return fail("strict proof SPIR-V instruction invalid");
            if (op == 16u && count == 4u && words[i + 3u] == 32u) {
                rte |= words[i + 2u] == 4462u;
                preserve |= words[i + 2u] == 4461u;
            }
            if (op == 71u && count >= 3u) {
                if (words[i + 2u] == 0u || words[i + 2u] == 40u)
                    return fail("strict proof unexpected relaxed/fast arithmetic decoration");
                if (words[i + 2u] == 42u) decorated.push_back(words[i + 1u]);
            }
            if (op == 129u || op == 131u || op == 133u || op == 136u ||
                op == 140u || op == 141u || (op >= 142u && op <= 148u)) {
                if (count < 4u) return fail("strict proof arithmetic instruction invalid");
                arithmetic.push_back(words[i + 2u]);
            }
            i += count;
        }
        std::sort(arithmetic.begin(), arithmetic.end());
        std::sort(decorated.begin(), decorated.end());
        if (!rte || !preserve || arithmetic.empty() || arithmetic != decorated)
            return fail("strict proof float controls/core NoContraction incomplete");
    }
    owned.shader = device->CreateShader(spv.data(), spv.size(), ShaderStage::Compute,
                                        "ddgi_first_record_proof");
    if (owned.shader == handles::INVALID_SHADER) return fail("first-record proof shader creation failed");

    DescriptorSetLayoutBinding bindings[12]{};
    for (u32 i = 0; i < 12u; ++i) {
        DescriptorType type = DescriptorType::StorageBuffer;
        if (i == 9u) type = DescriptorType::SampledImage;
        if (i == 10u) type = DescriptorType::Sampler;
        bindings[i] = {i, type, 1u, ShaderStage::Compute, nullptr};
    }
    DescriptorSetLayoutDesc layoutDesc{12u, bindings};
    owned.layout = device->CreateDescriptorSetLayout(layoutDesc);
    if (owned.layout == handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        return fail("first-record proof descriptor layout failed");
    PipelineLayoutDesc pipelineLayoutDesc{};
    pipelineLayoutDesc.setLayoutCount = 1u;
    pipelineLayoutDesc.setLayouts = &owned.layout;
    owned.pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    if (owned.pipelineLayout == handles::INVALID_PIPELINE_LAYOUT)
        return fail("first-record proof pipeline layout failed");
    DescriptorSetDesc setDesc{};
    setDesc.layout = owned.layout;
    owned.descriptorSet = device->CreateDescriptorSet(setDesc);
    if (owned.descriptorSet == handles::INVALID_DESCRIPTOR_SET)
        return fail("first-record proof descriptor set failed");

    DescriptorBufferInfo bufferInfos[10] = {
        {audit, 0u, auditBytes}, {nodes, 0u, inputBytes[0]},
        {triangles, 0u, inputBytes[1]}, {mapping, 0u, inputBytes[2]},
        {positions, 0u, inputBytes[3]}, {elements, 0u, inputBytes[4]},
        {instances, 0u, inputBytes[5]}, {materials, 0u, inputBytes[6]},
        {outputBuffer, 0u, outputBytes}, {paramsBuffer, 0u, sizeof(params)}};
    DescriptorImageInfo imageInfo{handles::INVALID_SAMPLER, normalTexture,
                                  ResourceState::ShaderResource};
    DescriptorImageInfo samplerInfo{normalSampler, handles::INVALID_RESOURCE,
                                    ResourceState::ShaderResource};
    WriteDescriptorSet writes[12]{};
    for (u32 i = 0; i < 12u; ++i) {
        const DescriptorBufferInfo* buffer = nullptr;
        const DescriptorImageInfo* image = nullptr;
        if (i <= 8u) buffer = &bufferInfos[i];
        else if (i == 9u) image = &imageInfo;
        else if (i == 10u) image = &samplerInfo;
        else buffer = &bufferInfos[9];
        writes[i] = {owned.descriptorSet, i, 0u, 1u, bindings[i].descriptorType,
                     image, buffer};
    }
    device->UpdateDescriptorSets(12u, writes);

    ComputePipelineDesc computeDesc{};
    computeDesc.computeShader = owned.shader;
    computeDesc.layout = owned.pipelineLayout;
    owned.pipeline = device->CreateComputePipeline(computeDesc);
    owned.command = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (owned.pipeline == handles::INVALID_PIPELINE ||
        owned.command == handles::INVALID_COMMAND_BUFFER)
        return fail("first-record proof compute pipeline or command allocation failed");

    auto* command = vk->GetCommandBuffer(owned.command);
    if (!command || !command->Reset() || !command->Begin())
        return fail("first-record proof command begin failed");
    command->MemoryBarrier(
        PipelineStage::Host | PipelineStage::AllCommands | PipelineStage::ComputeShader |
            PipelineStage::VertexShader | PipelineStage::FragmentShader | PipelineStage::Transfer,
        PipelineStage::ComputeShader,
        AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::ShaderRead |
            AccessFlag::ShaderWrite | AccessFlag::VertexAttributeRead | AccessFlag::TransferWrite,
        AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
    command->BindComputePipeline(owned.pipeline);
    command->BindDescriptorSets(PipelineBindPoint::Compute, owned.pipelineLayout, 0u, 1u,
                                &owned.descriptorSet, 0u, nullptr);
    command->Dispatch((static_cast<u32>(freshRayCount) + 31u) / 32u, 1u, 1u);
    command->MemoryBarrier(PipelineStage::ComputeShader | PipelineStage::Host |
                               PipelineStage::AllCommands | PipelineStage::Transfer,
                           PipelineStage::Transfer,
                           AccessFlag::ShaderRead | AccessFlag::ShaderWrite |
                               AccessFlag::HostWrite | AccessFlag::MemoryWrite |
                               AccessFlag::TransferWrite,
                           AccessFlag::TransferRead);
    command->CopyBuffer(outputBuffer, outputReadback, 0u, 0u, outputBytes);
    command->CopyBuffer(audit, auditReadback, 0u, 0u, denseAuditBytes);
    command->CopyBuffer(paramsBuffer, paramsReadback, 0u, 0u, sizeof(params));
    for (u32 i = 0; i < 7u; ++i)
        command->CopyBuffer(inputBuffers[i], owned.buffers[4u + i], 0u, 0u, inputBytes[i]);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    if (!command->End() || !command->Submit() || !command->WaitForCompletion())
        return fail("first-record proof End/Submit/WaitForCompletion failed");

    std::vector<u8> outputBytesHost(static_cast<size_t>(outputBytes));
    std::vector<u8> auditBytesHost(static_cast<size_t>(denseAuditBytes));
    auto mapBuffer = [&](ResourceHandle buffer, void* destination, u64 bytes) {
        const void* mapped = device->MapBuffer(buffer);
        if (!mapped) return false;
        std::memcpy(destination, mapped, static_cast<size_t>(bytes));
        device->UnmapBuffer(buffer);
        return true;
    };
    if (!mapBuffer(outputReadback, outputBytesHost.data(), outputBytes) ||
        !mapBuffer(auditReadback, auditBytesHost.data(), denseAuditBytes))
        return fail("first-record proof output/audit Map after completion failed");

    auto writeRaw = [&](const std::string& suffix, const void* data, u64 bytes) {
        std::ofstream file(prefix + suffix, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(static_cast<const char*>(data), std::streamsize(bytes));
        return bool(file);
    };
    if (!writeRaw(".raw", outputBytesHost.data(), outputBytes) ||
        !writeRaw(".audit_input.raw", auditBytesHost.data(), denseAuditBytes) ||
        !writeRaw(".params-upload.bin", params, sizeof(params)))
        return fail("first-record proof raw output write failed");
    u32 actualParams[4]{};
    if (!mapBuffer(paramsReadback, actualParams, sizeof(actualParams)) ||
        !writeRaw(".params.bin", actualParams, sizeof(actualParams)) ||
        std::memcmp(params, actualParams, sizeof(params)) != 0)
        return fail("completed GPU params readback differs from uploaded params");
    for (u32 i = 0; i < 7u; ++i) {
        const char* suffixes[7] = {".nodes.bin", ".triangles.bin", ".mapping.bin",
                                   ".positions.bin", ".elements.bin", ".instances.bin",
                                   ".materials.bin"};
        std::vector<u8> source(static_cast<size_t>(inputBytes[i]));
        if (!mapBuffer(owned.buffers[4u + i], source.data(), inputBytes[i]) ||
            !writeRaw(suffixes[i], source.data(), inputBytes[i]))
            return fail("first-record proof source readback/write failed");
    }
    rawEvidenceSaved = true;

    const math::v4* output = reinterpret_cast<const math::v4*>(outputBytesHost.data());
    const math::v4* auditInput = reinterpret_cast<const math::v4*>(auditBytesHost.data());
    const u32 expectedFrame = pass->GetVolumeData().FrameIndex & 0xffffu;
    for (u64 ray = 0u; ray < freshRayCount; ++ray) {
        const u64 inBase = ray * kAuditVec4Stride;
        const u64 outBase = ray * kProofVec4Stride;
        if (std::memcmp(output + outBase, auditInput + inBase,
                        static_cast<size_t>(kAuditVec4Stride * sizeof(math::v4))) != 0)
            return fail("proof output audit echo differs from the completed input audit");
        const u32 probeSlot = static_cast<u32>(ray / raysPerProbe);
        const u32 rayId = static_cast<u32>(ray % raysPerProbe);
        const u32 stamp = DDGIFirstRecordProofBits(output[outBase + 0u].w);
        const u32 expectedStamp = (expectedFrame << 16u) | ((updateList[probeSlot] + 1u) & 0xffffu);
        if (stamp != expectedStamp || output[outBase + 1u].w != float(rayId + 1u))
            return fail("proof fresh audit stamp or ray identity is stale");
        if (interval && DDGIFirstRecordProofBits(output[outBase + 41u].w) != 1u)
            return fail("interval private tail schema mismatch");
        if (!DDGIFirstRecordProofFinite(output[outBase + 40u].w))
            return fail("proof GPU first-point guard is nonfinite");
    }

    // Enclose the optional raster work in the same five-resource identity check.
    if (!RunDDGIFirstPrimaryRasterProof(device, pass, frame, updateList,
            fullRayCapacity, raysPerProbe, static_cast<u32>(kProofVec4Stride), materialRegistry))
        return fail("private first primary raster proof failed");
    if (!RunDDGIFirstCoherentReplay(device, pass, frame, outputBuffer,
            static_cast<u32>(kProofVec4Stride), static_cast<u32>(freshRayCount)))
        return fail("private same-record coherent replay failed");
    if (!RunDDGIFirstFactorReplay(device, pass, frame, outputBuffer,
            static_cast<u32>(kProofVec4Stride), static_cast<u32>(freshRayCount)))
        return fail("private same-record factor replay failed");

    std::vector<std::vector<u8>> productionAfter;
    if (!captureSnapshot("after", productionAfter))
        return fail("production after snapshot End/Submit/Wait/Map failed");
    for (u32 i = 0; i < 5u; ++i) {
        if (productionBefore[i].size() != productionAfter[i].size() ||
            std::memcmp(productionBefore[i].data(), productionAfter[i].data(),
                        productionBefore[i].size()) != 0)
            return fail("diagnostic proof changed a production snapshot");
    }

    std::ofstream meta(prefix + ".json", std::ios::trunc);
    if (!meta) return fail("first-record proof metadata open failed");
    meta << "{\"schema\":\"ddgi_first_record_proof_v1\",\"complete\":true"
         << ",\"frame\":" << frame
         << ",\"ddgi_frame\":" << pass->GetVolumeData().FrameIndex
         << ",\"update_count\":" << updateList.size()
         << ",\"fresh_ray_count\":" << freshRayCount
         << ",\"full_ray_capacity\":" << fullRayCapacity
         << ",\"rays_per_probe\":" << raysPerProbe
         << ",\"audit_handle\":" << audit << ",\"audit_bytes\":" << auditBytes
         << ",\"audit_input_bytes\":" << denseAuditBytes
         << ",\"output_handle\":" << outputBuffer
         << ",\"output_bytes\":" << outputBytes
         << ",\"audit_vec4_stride\":16,\"native_vec4_stride\":24"
         << ",\"point_vec4_stride\":1,\"result_vec4_stride\":" << kProofVec4Stride
         << ",\"result_byte_stride\":" << kProofVec4Stride * sizeof(math::v4)
         << ",\"native_mapping_stride_bytes\":32"
         << ",\"params_handle\":" << paramsBuffer << ",\"params_bytes\":16"
         << ",\"shader_handle\":" << owned.shader
         << ",\"pipeline_handle\":" << owned.pipeline
         << ",\"pipeline_layout_handle\":" << owned.pipelineLayout
         << ",\"descriptor_set_handle\":" << owned.descriptorSet
         << ",\"descriptor_set_layout_handle\":" << owned.layout
         << ",\"command_handle\":" << owned.command
         << ",\"binding_handles\":{\"audit\":" << audit
         << ",\"nodes\":" << nodes << ",\"triangles\":" << triangles
         << ",\"mapping\":" << mapping << ",\"positions\":" << positions
         << ",\"elements\":" << elements << ",\"instances\":" << instances
         << ",\"materials\":" << materials << ",\"output\":" << outputBuffer
         << ",\"normal_texture\":" << normalTexture
         << ",\"normal_sampler\":" << normalSampler << "}"
         << ",\"normal_texture_desc\":{\"extent\":[" << normalDesc.size.x
         << ',' << normalDesc.size.y << ',' << normalDesc.size.z
         << "],\"array_layers\":" << normalDesc.arraySize
         << ",\"mip_levels\":" << normalDesc.mipLevels
         << ",\"runtime_format\":" << static_cast<u32>(normalDesc.format) << "}"
         << ",\"binding_layout\":{\"0\":\"analytic_rho_audit\",\"1\":\"nodes\",\"2\":\"triangles\",\"3\":\"mapping32\",\"4\":\"positions\",\"5\":\"elements\",\"6\":\"instances\",\"7\":\"materials\",\"8\":\"proof_output\",\"9\":\"normal_texture\",\"10\":\"normal_sampler\",\"11\":\"params\"}"
         << ",\"source_bytes\":[" << inputBytes[0] << ',' << inputBytes[1]
         << ',' << inputBytes[2] << ',' << inputBytes[3] << ',' << inputBytes[4]
         << ',' << inputBytes[5] << ',' << inputBytes[6] << "]"
         << ",\"production_snapshot_bytes\":[" << productionBytes[0] << ','
         << productionBytes[1] << ',' << productionBytes[2] << ',' << productionBytes[3]
         << ',' << productionBytes[4] << "]"
         << ",\"production_snapshot_files\":[\"hit_distance\",\"hit_normal\",\"ray_data\",\"current_sh\",\"update_list\"]"
         << ",\"production_before_after_byte_identical\":true"
         << ",\"readback_completed\":true,\"explicit_buffer_dependencies\":true"
         << ",\"insert_barrier_buffer_ignored\":true"
         << ",\"begin_end_submit_checked\":true,\"completion_checked\":true"
         << ",\"map_after_completion\":true,\"fresh_dense_only\":true"
         << ",\"missing_update_rays_substituted\":false"
         << ",\"strict_arithmetic_requested\":" << (strict ? "true" : "false")
         << ",\"strict_core_decorations_checked_before_create\":" << (strict ? "true" : "false")
         << ",\"primary_frame_requested\":" << (primaryFrame ? "true" : "false")
         << ",\"interval_guard_requested\":" << (interval ? "true" : "false")
         << ",\"interval_schema\":" << std::quoted(interval ? "ddgi_twin_interval_v1" : "none")
         << ",\"interval_tail_vec4_stride\":" << (interval ? 28u : 0u)
         << ",\"interval_math_guard_only\":true"
         << ",\"proof_module\":" << std::quoted(proofModule)
         << ",\"params_actual_gpu_readback\":true"
         << ",\"diagnostic_continue_rejected_bounds\":true"
         << ",\"rejected_normal_payload_is_uncertified\":true"
         << ",\"production_writes\":false,\"default_changed\":false"
         << ",\"algorithm_changed\":false"
         << ",\"counterfactual\":true"
         << ",\"counterfactual_reason\":\"diagnostic native-normal proof; no production HN write\"}\n";
    if (!meta) return fail("first-record proof metadata write failed");
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_FIRST_RECORD_PROOF] complete frame=" << frame
        << " fresh_rays=" << freshRayCount << " output_vec4_stride=" << kProofVec4Stride << std::endl;
    return true;
}
