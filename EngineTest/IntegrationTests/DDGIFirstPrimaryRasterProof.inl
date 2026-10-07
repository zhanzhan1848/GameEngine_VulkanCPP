// Bounded, opt-in Vulkan raster witness for the DDGI first record.
//
// This file is included inside TestVulkanSponzaRenderGraph's anonymous
// namespace.  It owns only private resources.  The production GPUDrivenDraw
// buffers, textures and shaders are borrowed read-only and are joined to the
// completed DDGI_FIRST_RECORD proof by absolute (update-slot, ray) identity.
//
// The original primary fragment and the companion witness fragment are
// separate passes and separate readbacks.  A companion value is never
// presented as an output of the original primary fragment.

struct DDGIFirstPrimaryRasterMapping {
    u32 global_vertex_ids[3];
    u32 instance_index;
    u32 scene_material_id;
    u32 source_mesh_id;
    u32 cluster_material_id;
    u32 unsupported_flags;
};
static_assert(sizeof(DDGIFirstPrimaryRasterMapping) == 32,
              "DDGI primary raster mapping ABI changed");

struct DDGIFirstPrimaryRasterMeshlet {
    u32 vertex_offset;
    u32 triangle_offset;
    u32 vertex_count;
    u32 triangle_count;
    float cone_apex[3];
    float cone_axis[3];
    float cone_cutoff;
    float center[3];
    float radius;
    u32 padding;
};
static_assert(sizeof(DDGIFirstPrimaryRasterMeshlet) == 64,
              "DDGI primary raster meshlet ABI changed");

struct DDGIFirstPrimaryRasterDrawConstants {
    math::m4x4 view_matrix;
    math::m4x4 proj_matrix;
    math::m4x4 world_matrix;
    u32 view_width;
    u32 view_height;
    u32 meshlet_count;
    u32 padding;
    math::m4x4 prev_view_matrix;
    math::m4x4 prev_proj_matrix;
    u32 has_prev_frame;
    u32 debug_mode;
    u32 pad2_1;
    u32 pad2_2;
    math::v2 jitter;
    math::v2 prev_jitter;
};
static_assert(sizeof(DDGIFirstPrimaryRasterDrawConstants) == 368,
              "GPUDrivenDraw DrawConstants ABI changed");

struct DDGIFirstPrimaryRasterSample {
    u32 probe{0};
    u32 ray{0};
    u32 update_slot{0};
    u32 status{0};
    u32 logical_triangle{0};
    u32 cluster{0};
    u32 meshlet{0};
    u32 local_triangle{0};
    u32 instance{0};
    float first_t{0.0f};
    float bary[3]{0.0f, 0.0f, 0.0f};
    float origin[3]{0.0f, 0.0f, 0.0f};
    float direction[3]{0.0f, 0.0f, 1.0f};
    float first_point[3]{0.0f, 0.0f, 0.0f};
    float native_point[3]{0.0f, 0.0f, 0.0f};
};

static u32 DDGIFirstPrimaryRasterBits(float value) {
    u32 bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float DDGIFirstPrimaryRasterFloat(u32 bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool DDGIFirstPrimaryRasterFinite(float value) {
    const u32 bits = DDGIFirstPrimaryRasterBits(value);
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static bool DDGIFirstPrimaryRasterFinite3(const float* values) {
    return DDGIFirstPrimaryRasterFinite(values[0]) &&
           DDGIFirstPrimaryRasterFinite(values[1]) &&
           DDGIFirstPrimaryRasterFinite(values[2]);
}

static float DDGIFirstPrimaryRasterDot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float DDGIFirstPrimaryRasterLength(const float* a) {
    return std::sqrt(std::max(0.0f, DDGIFirstPrimaryRasterDot(a, a)));
}

static bool DDGIFirstPrimaryRasterNormalize(float* a) {
    const float length = DDGIFirstPrimaryRasterLength(a);
    if (!DDGIFirstPrimaryRasterFinite(length) || length <= 1.0e-8f) return false;
    a[0] /= length; a[1] /= length; a[2] /= length;
    return DDGIFirstPrimaryRasterFinite3(a);
}

static void DDGIFirstPrimaryRasterCross(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float DDGIFirstPrimaryRasterDistance3(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return DDGIFirstPrimaryRasterLength(d);
}

static void DDGIFirstPrimaryRasterTransformPoint(const float* matrix,
                                                  const float* point,
                                                  float* out) {
    // C++/WGSL matrices are column major.
    out[0] = matrix[0] * point[0] + matrix[4] * point[1] +
             matrix[8] * point[2] + matrix[12];
    out[1] = matrix[1] * point[0] + matrix[5] * point[1] +
             matrix[9] * point[2] + matrix[13];
    out[2] = matrix[2] * point[0] + matrix[6] * point[1] +
             matrix[10] * point[2] + matrix[14];
}

static void DDGIFirstPrimaryRasterSetMatrix(math::m4x4& matrix,
                                             const float* values) {
    for (u32 c = 0; c < 4u; ++c)
        for (u32 r = 0; r < 4u; ++r)
            matrix.columns[c][r] = values[c * 4u + r];
}

static bool DDGIFirstPrimaryRasterReadBuffer(
    RHIDeviceBase* device, ResourceHandle source, u64 bytes,
    std::vector<u8>& output, std::string& error, const char* name) {
    output.clear();
    if (!device || source == handles::INVALID_RESOURCE || bytes == 0u ||
        bytes > 256ull * 1024ull * 1024ull) {
        error = "invalid private readback source/extent";
        return false;
    }
    BufferDesc desc{};
    desc.size = bytes;
    desc.type = BufferType::Structured;
    desc.usage = GPUMemoryUsage::Readback;
    desc.memoryUsage = GPUMemoryUsage::Readback;
    desc.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
    desc.name = name;
    const ResourceHandle staging = device->CreateBuffer(desc);
    const CommandBufferHandle commandHandle =
        device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* vk = static_cast<VulkanDevice*>(device);
    auto* command = commandHandle == handles::INVALID_COMMAND_BUFFER
        ? nullptr : vk->GetCommandBuffer(commandHandle);
    if (staging == handles::INVALID_RESOURCE || !command ||
        !command->Reset() || !command->Begin()) {
        if (commandHandle != handles::INVALID_COMMAND_BUFFER)
            device->DestroyCommandBuffer(commandHandle);
        if (staging != handles::INVALID_RESOURCE) device->DestroyBuffer(staging);
        error = "private readback Reset/Begin failed";
        return false;
    }
    // Vulkan InsertBarrier is texture-only in this RHI.  The explicit memory
    // dependency below is intentional and is the evidence boundary for the
    // buffer copy; no InsertBarrier claim is used for fresh buffer data.
    command->MemoryBarrier(
        PipelineStage::ComputeShader | PipelineStage::AllCommands |
            PipelineStage::Host | PipelineStage::Transfer,
        PipelineStage::Transfer,
        AccessFlag::ShaderWrite | AccessFlag::ShaderRead | AccessFlag::HostWrite |
            AccessFlag::TransferWrite,
        AccessFlag::TransferRead);
    command->CopyBuffer(source, staging, 0u, 0u, bytes);
    command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                           AccessFlag::TransferWrite, AccessFlag::HostRead);
    const bool ended = command->End();
    const bool submitted = ended && command->Submit();
    const bool completed = submitted && command->WaitForCompletion();
    if (!ended || !submitted || !completed) {
        device->DestroyCommandBuffer(commandHandle);
        device->DestroyBuffer(staging);
        error = "private readback End/Submit/WaitForCompletion failed";
        return false;
    }
    const void* mapped = device->MapBuffer(staging);
    if (!mapped) {
        device->DestroyCommandBuffer(commandHandle);
        device->DestroyBuffer(staging);
        error = "private readback Map after completion failed";
        return false;
    }
    output.resize(static_cast<size_t>(bytes));
    std::memcpy(output.data(), mapped, output.size());
    device->UnmapBuffer(staging);
    device->DestroyCommandBuffer(commandHandle);
    device->DestroyBuffer(staging);
    return true;
}

static bool DDGIFirstPrimaryRasterReadRecord(
    const std::string& prefix, u64 freshRayCount, u32 recordVec4Stride,
    std::vector<u8>& raw, std::string& error) {
    std::ifstream input(prefix + ".raw", std::ios::binary | std::ios::ate);
    if (!input) { error = "DDGI_FIRST_RECORD raw output is unavailable"; return false; }
    const std::streamsize size = input.tellg();
    if (size <= 0 || u64(size) != freshRayCount * recordVec4Stride * sizeof(math::v4)) {
        error = "DDGI_FIRST_RECORD raw stride/extent differs from the requested frame";
        return false;
    }
    input.seekg(0);
    raw.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(raw.data()), size);
    if (!input) { error = "DDGI_FIRST_RECORD raw read failed"; return false; }
    return true;
}

static bool DDGIFirstPrimaryRasterFindPrimitive(
    const std::vector<u8>& mappingBytes,
    const std::vector<u8>& clusterBytes,
    const std::vector<u8>& meshletBytes,
    const std::vector<u8>& meshletVertexBytes,
    const std::vector<u8>& meshletTriangleBytes,
    const DDGIFirstPrimaryRasterMapping& mapping,
    u32 activeClusterCount,
    u32& cluster, u32& meshlet, u32& localTriangle) {
    if (mappingBytes.empty() || clusterBytes.size() % 16u != 0u ||
        meshletBytes.size() % sizeof(DDGIFirstPrimaryRasterMeshlet) != 0u ||
        meshletVertexBytes.size() % sizeof(u32) != 0u) return false;
    const auto* clusters = reinterpret_cast<const u32*>(clusterBytes.data());
    const auto* meshlets = reinterpret_cast<const DDGIFirstPrimaryRasterMeshlet*>(meshletBytes.data());
    const auto* meshletVertices = reinterpret_cast<const u32*>(meshletVertexBytes.data());
    const u32 clusterCount = std::min<u32>(activeClusterCount,
                                           static_cast<u32>(clusterBytes.size() / 16u));
    const u32 meshletCount = static_cast<u32>(meshletBytes.size() / sizeof(DDGIFirstPrimaryRasterMeshlet));
    for (u32 c = 0; c < clusterCount; ++c) {
        const u32* cmap = clusters + c * 4u;
        if (cmap[1] != mapping.instance_index ||
            cmap[2] != mapping.cluster_material_id || cmap[0] >= meshletCount) continue;
        const auto& ml = meshlets[cmap[0]];
        if (ml.vertex_offset > meshletVertexBytes.size() / 4u ||
            ml.vertex_count > meshletVertexBytes.size() / 4u - ml.vertex_offset ||
            ml.triangle_count == 0u || ml.triangle_count > 128u) continue;
        const u64 lastByte = u64(ml.triangle_offset) + u64(ml.triangle_count) * 3u;
        if (((lastByte + 3u) / 4u) * 4u > meshletTriangleBytes.size()) continue;
        for (u32 t = 0; t < ml.triangle_count; ++t) {
            u32 ids[3]{};
            for (u32 k = 0; k < 3u; ++k) {
                const u64 byteIndex = u64(ml.triangle_offset) + u64(t * 3u + k);
                const u32 word = *reinterpret_cast<const u32*>(
                    meshletTriangleBytes.data() + (byteIndex / 4u) * 4u);
                const u32 local = (word >> ((byteIndex % 4u) * 8u)) & 0xffu;
                if (local >= ml.vertex_count) { ids[0] = ids[1] = ids[2] = 0xffffffffu; break; }
                ids[k] = meshletVertices[ml.vertex_offset + local];
            }
            u32 expected[3] = {mapping.global_vertex_ids[0], mapping.global_vertex_ids[1],
                               mapping.global_vertex_ids[2]};
            std::sort(ids, ids + 3u);
            std::sort(expected, expected + 3u);
            if (ids[0] == expected[0] && ids[1] == expected[1] && ids[2] == expected[2]) {
                cluster = c;
                meshlet = cmap[0];
                localTriangle = t;
                return true;
            }
        }
    }
    return false;
}

static void DDGIFirstPrimaryRasterBuildCamera(
    const DDGIFirstPrimaryRasterSample& sample,
    const float* worldVertices,
    math::m4x4& view,
    math::m4x4& projection,
    float maxViewport,
    float& virtualViewport) {
    float forward[3] = {sample.direction[0], sample.direction[1], sample.direction[2]};
    DDGIFirstPrimaryRasterNormalize(forward);
    float upReference[3] = {0.0f, 1.0f, 0.0f};
    if (std::abs(DDGIFirstPrimaryRasterDot(upReference, forward)) > 0.95f) {
        upReference[0] = 1.0f; upReference[1] = 0.0f; upReference[2] = 0.0f;
    }
    float right[3]{};
    DDGIFirstPrimaryRasterCross(upReference, forward, right);
    DDGIFirstPrimaryRasterNormalize(right);
    float up[3]{};
    DDGIFirstPrimaryRasterCross(forward, right, up);
    DDGIFirstPrimaryRasterNormalize(up);
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    const float cameraCenter[3] = {sample.first_point[0], sample.first_point[1], sample.first_point[2]};
    for (u32 v = 0; v < 3u; ++v) {
        const float delta[3] = {worldVertices[v * 3u + 0u] - cameraCenter[0],
                                worldVertices[v * 3u + 1u] - cameraCenter[1],
                                worldVertices[v * 3u + 2u] - cameraCenter[2]};
        maxX = std::max(maxX, std::abs(DDGIFirstPrimaryRasterDot(right, delta)));
        maxY = std::max(maxY, std::abs(DDGIFirstPrimaryRasterDot(up, delta)));
        maxZ = std::max(maxZ, std::abs(DDGIFirstPrimaryRasterDot(forward, delta)));
    }
    const float halfX = std::max(1.0e-4f, maxX * 1.5f);
    const float halfY = std::max(1.0e-4f, maxY * 1.5f);
    const float cameraDistance = std::max(1.0f, maxZ * 4.0f + 1.0f);
    const float camera[3] = {cameraCenter[0] - forward[0] * cameraDistance,
                             cameraCenter[1] - forward[1] * cameraDistance,
                             cameraCenter[2] - forward[2] * cameraDistance};
    float viewValues[16] = {
        right[0], up[0], forward[0], 0.0f,
        right[1], up[1], forward[1], 0.0f,
        right[2], up[2], forward[2], 0.0f,
        -DDGIFirstPrimaryRasterDot(right, camera),
        -DDGIFirstPrimaryRasterDot(up, camera),
        -DDGIFirstPrimaryRasterDot(forward, camera), 1.0f};
    const float nearPlane = std::max(0.01f, cameraDistance - maxZ * 2.0f - 1.0f);
    const float farPlane = cameraDistance + maxZ * 2.0f + 1.0f;
    float projectionValues[16] = {
        1.0f / halfX, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f / halfY, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f / (farPlane - nearPlane), 0.0f,
        0.0f, 0.0f, -nearPlane / (farPlane - nearPlane), 1.0f};
    DDGIFirstPrimaryRasterSetMatrix(view, viewValues);
    DDGIFirstPrimaryRasterSetMatrix(projection, projectionValues);
    // 16,384 is inside MoltenVK's usual viewport bound and is deliberately
    // recorded.  The physical 17x17 scissor still keeps the readback small.
    virtualViewport = std::min(16384.0f, std::max(17.0f, maxViewport));
}

static bool DDGIFirstPrimaryRasterProject(const math::m4x4& view,
                                          const math::m4x4& projection,
                                          const float* point,
                                          float* ndc) {
    float viewPoint[4]{};
    float clip[4]{};
    for (u32 r = 0; r < 4u; ++r)
        viewPoint[r] = view.columns[0][r] * point[0] +
                       view.columns[1][r] * point[1] +
                       view.columns[2][r] * point[2] +
                       view.columns[3][r];
    for (u32 r = 0; r < 4u; ++r)
        clip[r] = projection.columns[0][r] * viewPoint[0] +
                  projection.columns[1][r] * viewPoint[1] +
                  projection.columns[2][r] * viewPoint[2] +
                  projection.columns[3][r] * viewPoint[3];
    if (!DDGIFirstPrimaryRasterFinite(clip[0]) || !DDGIFirstPrimaryRasterFinite(clip[1]) ||
        !DDGIFirstPrimaryRasterFinite(clip[2]) || !DDGIFirstPrimaryRasterFinite(clip[3]) ||
        std::abs(clip[3]) <= 1.0e-8f) return false;
    ndc[0] = clip[0] / clip[3]; ndc[1] = clip[1] / clip[3]; ndc[2] = clip[2] / clip[3];
    return DDGIFirstPrimaryRasterFinite3(ndc);
}

static bool RunDDGIFirstPrimaryRasterProof(
    RHIDeviceBase* device,
    lumen::LumenDDGIPass* pass,
    u32 frame,
    const std::vector<u32>& updateList,
    u32 fullRayCapacity,
    u32 raysPerProbe,
    u32 recordVec4Stride,
    const nanite::GPUMaterialRegistry* materialRegistry) {
    const char* prefixValue = std::getenv("DDGI_FIRST_PRIMARY_RASTER_PREFIX");
    if (!prefixValue || !prefixValue[0]) return true;
    const std::string prefix(prefixValue);
    bool rawEvidenceSaved = false;
    auto fail = [&](const char* why) {
        std::ofstream meta(prefix + ".json", std::ios::trunc);
        if (meta) {
            meta << "{\"schema\":\"ddgi_first_primary_raster_v2\",\"complete\":false"
                 << ",\"readback_completed\":false,\"submit_checked\":false"
                 << ",\"completion_checked\":false,\"map_after_completion\":false"
                 << ",\"raw_evidence_preserved\":" << (rawEvidenceSaved ? "true" : "false")
                 << ",\"error\":\"" << why << "\"}\n";
        }
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_PRIMARY_RASTER] rejected: " << why << std::endl;
        return false;
    };
    const char* dump = std::getenv("DDGI_DUMP");
    const char* raster = std::getenv("DDGI_FIRST_PRIMARY_RASTER");
    if (!device || !pass || device->GetPlatform() != RHIPlatform::Vulkan ||
        !dump || std::strcmp(dump, "2") != 0 || !materialRegistry ||
        !pass->IsAnalyticFirstRhoEnabled() || !raster || std::strcmp(raster, "1") != 0 ||
        updateList.empty() ||
        raysPerProbe != 128u || fullRayCapacity == 0u ||
        fullRayCapacity % raysPerProbe != 0u ||
        (recordVec4Stride != 41u && recordVec4Stride != 69u))
        return fail("requires Vulkan DDGI_DUMP=2 analytic first-rho and record stride 41/69");
    if (updateList.size() > 16384u)
        return fail("update list is outside the bounded diagnostic contract");

    const u64 freshRayCount = u64(updateList.size()) * raysPerProbe;
    if (freshRayCount == 0u || freshRayCount > fullRayCapacity)
        return fail("fresh ray count is outside the full frame domain");
    const char* recordPrefixValue = std::getenv("DDGI_FIRST_RECORD_PROOF_PREFIX");
    if (!recordPrefixValue || !recordPrefixValue[0])
        return fail("same-frame DDGI_FIRST_RECORD proof prefix is missing");
    std::vector<u8> recordRaw;
    std::string error;
    if (!DDGIFirstPrimaryRasterReadRecord(recordPrefixValue, freshRayCount,
                                          recordVec4Stride, recordRaw, error))
        return fail(error.c_str());

    auto writeRaw = [&](const char* suffix, const std::vector<u8>& bytes) {
        std::ofstream out(prefix + suffix, std::ios::binary | std::ios::trunc);
        if (!out) return false; out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); return bool(out);
    };
    auto& draw = nanite::GPUDrivenDrawPipeline::Get();
    auto* vk = static_cast<VulkanDevice*>(device);
    const ResourceHandle sourceHandles[8] = {
        draw.GetGlobalMeshletBuffer(), draw.GetGlobalMeshletVerticesBuffer(),
        draw.GetGlobalMeshletTrianglesBuffer(), draw.GetGlobalVertexBuffer(),
        draw.GetGlobalElementBuffer(), draw.GetClusterMapBuffer(),
        draw.GetGlobalInstanceDataBuffer(), draw.GetMaterialDataBuffer()};
    const char* sourceNames[8] = {"meshlets", "meshlet_vertices", "meshlet_triangles",
                                  "positions", "elements", "cluster_map", "instances", "materials"};
    u64 sourceBytes[8]{};
    std::vector<std::vector<u8>> sourceRaw(8u);
    for (u32 i = 0; i < 8u; ++i) {
        if (sourceHandles[i] == handles::INVALID_RESOURCE ||
            !vk->GetBufferByteSize(sourceHandles[i], sourceBytes[i]) ||
            sourceBytes[i] == 0u || sourceBytes[i] > 256ull * 1024ull * 1024ull)
            return fail("native GPUDrivenDraw source handle/extent is unavailable");
        if (!DDGIFirstPrimaryRasterReadBuffer(device, sourceHandles[i], sourceBytes[i],
                                               sourceRaw[i], error, sourceNames[i]))
            return fail(error.c_str());
    }
    const ResourceHandle mappingHandle = pass->GetAnalyticFirstRhoMappingBuffer();
    const ResourceHandle auditHandle = pass->GetAnalyticFirstRhoAuditBuffer();
    const ResourceHandle updateHandle = pass->GetProbeUpdateListBuffer(frame);
    const ResourceHandle relocatedHandle = pass->GetRelocatedOriginsBuffer();
    u64 mappingBytes = 0u, auditBytes = 0u, updateBytes = 0u, relocatedBytes = 0u;
    if (!vk->GetBufferByteSize(mappingHandle, mappingBytes) || mappingBytes == 0u ||
        mappingBytes % sizeof(DDGIFirstPrimaryRasterMapping) != 0u ||
        !vk->GetBufferByteSize(auditHandle, auditBytes) ||
        auditBytes < freshRayCount * 16u * sizeof(math::v4) ||
        !vk->GetBufferByteSize(updateHandle, updateBytes) || updateBytes < updateList.size() * sizeof(u32) ||
        !vk->GetBufferByteSize(relocatedHandle, relocatedBytes) || relocatedBytes == 0u)
        return fail("same-frame analytic/update/relocated source extent is incomplete");
    std::vector<u8> mappingRaw, auditRaw, updateRaw, relocatedRaw;
    if (!DDGIFirstPrimaryRasterReadBuffer(device, mappingHandle, mappingBytes, mappingRaw, error, "mapping") ||
        !DDGIFirstPrimaryRasterReadBuffer(device, auditHandle,
                                           freshRayCount * 16u * sizeof(math::v4), auditRaw,
                                           error, "audit") ||
        !DDGIFirstPrimaryRasterReadBuffer(device, updateHandle, updateBytes, updateRaw,
                                           error, "update_list") ||
        !DDGIFirstPrimaryRasterReadBuffer(device, relocatedHandle, relocatedBytes,
                                           relocatedRaw, error, "relocated_origins"))
        return fail(error.c_str());
    if (std::memcmp(updateRaw.data(), updateList.data(), updateList.size() * sizeof(u32)) != 0)
        return fail("GPU update list differs from the same-frame host list");

    // Save source evidence before any raster classification or result guard.
    if (!writeRaw(".record.raw", recordRaw) || !writeRaw(".mapping.bin", mappingRaw) ||
        !writeRaw(".audit.raw", auditRaw) || !writeRaw(".update_list.bin", updateRaw) ||
        !writeRaw(".relocated_origins.bin", relocatedRaw)) return fail("source raw write failed");
    for (u32 i = 0; i < 8u; ++i) {
        const std::string suffix = std::string(".source-") + sourceNames[i] + ".bin";
        if (!writeRaw(suffix.c_str(), sourceRaw[i])) return fail("native source raw write failed");
    }
    rawEvidenceSaved = true;
    const auto* mapping = reinterpret_cast<const DDGIFirstPrimaryRasterMapping*>(mappingRaw.data());
    const auto* audit = reinterpret_cast<const math::v4*>(auditRaw.data());
    const auto* records = reinterpret_cast<const math::v4*>(recordRaw.data());
    std::vector<DDGIFirstPrimaryRasterSample> samples;
    // Diagnose the current SC-selected high-energy/guard rays using the
    // original primary fragment.  This opt-in selection changes only which
    // private proof samples are drawn; the default 409/410/507 selection and
    // all production draws remain untouched.
    const char* scTargetsFlag = std::getenv("DDGI_FIRST_PRIMARY_RASTER_SC_TARGETS");
    const bool scTargets = scTargetsFlag && std::strcmp(scTargetsFlag, "1") == 0;
    const char* scFocusFlag = std::getenv("DDGI_FIRST_PRIMARY_RASTER_SC_FOCUS");
    const bool scFocus = scTargets && scFocusFlag && std::strcmp(scFocusFlag, "1") == 0;
    const u32 targetProbes[3] = {409u, scTargets ? 439u : 410u, 507u};
    const u32 targetProbeCount = scTargets ? 2u : 3u;
    for (u32 probeIndex = 0u; probeIndex < targetProbeCount; ++probeIndex) {
        const u32 target = targetProbes[probeIndex];
        u32 updateSlot = 0xffffffffu;
        for (u32 i = 0; i < updateList.size(); ++i)
            if (updateList[i] == target) { updateSlot = i; break; }
        if (updateSlot == 0xffffffffu) continue;
        u32 selected = 0u;
        for (u32 ray = 0; ray < raysPerProbe && selected < 8u; ++ray) {
            if (scTargets) {
                const u32 rays409[6] = {3u, 24u, 37u, 58u, 79u, 92u};
                const u32 rays439[8] = {10u, 31u, 52u, 65u, 86u, 99u, 116u, 120u};
                const u32 focus409[2] = {37u, 79u};
                const u32 focus439[2] = {99u, 116u};
                const u32* wanted = scFocus ? (target == 409u ? focus409 : focus439) :
                                             (target == 409u ? rays409 : rays439);
                const u32 wantedCount = scFocus ? 2u : (target == 409u ? 6u : 8u);
                bool listed = false;
                for (u32 j = 0u; j < wantedCount; ++j) listed = listed || ray == wanted[j];
                if (!listed) continue;
            }
            const u64 compactIndex = u64(updateSlot) * raysPerProbe + ray;
            const math::v4* a = audit + compactIndex * 16u;
            const math::v4* r = records + compactIndex * recordVec4Stride;
            if (DDGIFirstPrimaryRasterBits(r[4].y) != 12u ||
                DDGIFirstPrimaryRasterBits(a[4].y) != 12u ||
                DDGIFirstPrimaryRasterBits(a[4].z) != 0u ||
                !DDGIFirstPrimaryRasterFinite(a[3].x) || a[3].x <= 0.0f ||
                (!DDGIFirstPrimaryRasterFinite(a[3].y) || !DDGIFirstPrimaryRasterFinite(a[3].z) || !DDGIFirstPrimaryRasterFinite(a[3].w)) ||
                (!DDGIFirstPrimaryRasterFinite(r[40].x) || !DDGIFirstPrimaryRasterFinite(r[40].y) || !DDGIFirstPrimaryRasterFinite(r[40].z)) ||
                std::abs(r[40].w - 1.0f) > 1.0e-6f)
                continue;
            if (!DDGIFirstPrimaryRasterFinite(a[2].x) || a[2].x < 0.0f ||
                a[2].x >= float(mappingBytes / sizeof(DDGIFirstPrimaryRasterMapping)) ||
                a[2].x != std::floor(a[2].x)) continue;
            const u32 logical = static_cast<u32>(a[2].x);
            if (logical >= mappingBytes / sizeof(DDGIFirstPrimaryRasterMapping)) continue;
            DDGIFirstPrimaryRasterSample sample{};
            sample.probe = target; sample.ray = ray; sample.update_slot = updateSlot;
            sample.status = 12u; sample.logical_triangle = logical;
            sample.instance = mapping[logical].instance_index;
            sample.first_t = a[3].x;
            sample.bary[0] = a[3].y; sample.bary[1] = a[3].z; sample.bary[2] = a[3].w;
            for (u32 k = 0; k < 3u; ++k) {
                sample.origin[k] = a[0][k]; sample.direction[k] = a[1][k];
                sample.first_point[k] = r[40][k];
            }
            if (!DDGIFirstPrimaryRasterFinite3(sample.origin) ||
                !DDGIFirstPrimaryRasterFinite3(sample.direction) ||
                !DDGIFirstPrimaryRasterNormalize(sample.direction)) continue;
            if (!DDGIFirstPrimaryRasterFindPrimitive(
                    mappingRaw, sourceRaw[5], sourceRaw[0], sourceRaw[1], sourceRaw[2],
                    mapping[logical], draw.GetClusterMapEntryCount(),
                    sample.cluster, sample.meshlet, sample.local_triangle))
                continue;
            const float* positions = reinterpret_cast<const float*>(sourceRaw[3].data());
            const float* instances = reinterpret_cast<const float*>(sourceRaw[6].data());
            bool vertexBounds = true;
            for (u32 v = 0; v < 3u; ++v)
                vertexBounds = vertexBounds &&
                    u64(mapping[logical].global_vertex_ids[v]) * 3u + 2u < sourceBytes[3] / sizeof(float);
            if (!vertexBounds || u64(sample.instance) * 48u + 15u >= sourceBytes[6] / sizeof(float)) continue;
            float local[3][3]{};
            float worldMatrix[16]{};
            std::memcpy(worldMatrix, instances + u64(sample.instance) * 48u, sizeof(worldMatrix));
            for (u32 v = 0; v < 3u; ++v) {
                const u32 globalVertex = mapping[logical].global_vertex_ids[v];
                const float* p = positions + u64(globalVertex) * 3u;
                DDGIFirstPrimaryRasterTransformPoint(worldMatrix, p, local[v]);
                for (u32 k = 0; k < 3u; ++k)
                    sample.native_point[k] += sample.bary[v] * local[v][k];
            }
            if (DDGIFirstPrimaryRasterDistance3(sample.first_point, sample.native_point) > 2.0e-5f)
                continue;
            samples.push_back(sample);
            ++selected;
        }
    }
    if (samples.empty() || samples.size() > 24u)
        return fail("no bounded status12 first-point/native primitive samples");

    const ResourceHandle albedo = draw.GetAlbedoTextureArray();
    const ResourceHandle normal = draw.GetNormalTextureArray();
    const ResourceHandle orm = materialRegistry->GetORMTextureArray();
    const SamplerHandle sampler = draw.GetTextureSampler();
    if (albedo == handles::INVALID_RESOURCE || normal == handles::INVALID_RESOURCE ||
        orm == handles::INVALID_RESOURCE || sampler == handles::INVALID_SAMPLER)
        return fail("native albedo/normal/ORM arrays or sampler are unavailable");
    auto primarySpv = LoadSpvBytes("Engine/Graphics/Vulkan/Shaders/Nanite/GPUDrivenDraw.spv");
    auto companionSpv = LoadSpvBytes("Engine/Graphics/Vulkan/Shaders/Lumen/DDGIFirstPrimaryRasterWitness.frag.spv");
    if (primarySpv.empty() || companionSpv.empty())
        return fail("actual GPUDrivenDraw or companion SPIR-V is unavailable");

    struct Resources {
        RHIDeviceBase* device{nullptr};
        ShaderHandle vs{handles::INVALID_SHADER}, primaryFs{handles::INVALID_SHADER}, companionFs{handles::INVALID_SHADER};
        DescriptorSetLayoutHandle setLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        PipelineLayoutHandle pipelineLayout{handles::INVALID_PIPELINE_LAYOUT};
        PipelineHandle primaryPipeline{handles::INVALID_PIPELINE}, companionPipeline{handles::INVALID_PIPELINE};
        ~Resources() {
            if (!device) return;
            if (primaryPipeline != handles::INVALID_PIPELINE) device->DestroyPipeline(primaryPipeline);
            if (companionPipeline != handles::INVALID_PIPELINE) device->DestroyPipeline(companionPipeline);
            if (pipelineLayout != handles::INVALID_PIPELINE_LAYOUT) device->DestroyPipelineLayout(pipelineLayout);
            if (setLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device->DestroyDescriptorSetLayout(setLayout);
            if (vs != handles::INVALID_SHADER) device->DestroyShader(vs);
            if (primaryFs != handles::INVALID_SHADER) device->DestroyShader(primaryFs);
            if (companionFs != handles::INVALID_SHADER) device->DestroyShader(companionFs);
        }
    } resources;
    resources.device = device;
    resources.vs = device->CreateShader(primarySpv.data(), primarySpv.size(), ShaderStage::Vertex,
                                        "gpu_driven_vertex_shader");
    resources.primaryFs = device->CreateShader(primarySpv.data(), primarySpv.size(), ShaderStage::Pixel,
                                               "gpu_driven_fragment_shader");
    resources.companionFs = device->CreateShader(companionSpv.data(), companionSpv.size(), ShaderStage::Pixel, "main");
    if (resources.vs == handles::INVALID_SHADER || resources.primaryFs == handles::INVALID_SHADER ||
        resources.companionFs == handles::INVALID_SHADER)
        return fail("private original/companion shader creation failed");
    DescriptorSetLayoutBinding bindings[14]{};
    for (u32 i = 0; i < 14u; ++i) {
        bindings[i].binding = i; bindings[i].descriptorCount = 1u;
        bindings[i].descriptorType = i == 0u ? DescriptorType::UniformBufferDynamic :
            (i >= 10u && i <= 12u ? DescriptorType::SampledImage :
             (i == 13u ? DescriptorType::Sampler : DescriptorType::StorageBuffer));
        bindings[i].stageFlags = i == 0u ? (ShaderStage::Vertex | ShaderStage::Pixel) :
            ((i == 9u) ? (ShaderStage::Vertex | ShaderStage::Pixel) :
             (i >= 10u ? ShaderStage::Pixel : ShaderStage::Vertex));
        bindings[i].isArray = i >= 10u && i <= 12u;
    }
    resources.setLayout = device->CreateDescriptorSetLayout({14u, bindings});
    if (resources.setLayout == handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        return fail("private 14-binding descriptor layout creation failed");
    resources.pipelineLayout = device->CreatePipelineLayout({1u, &resources.setLayout, 0u, nullptr});
    if (resources.pipelineLayout == handles::INVALID_PIPELINE_LAYOUT)
        return fail("private pipeline layout creation failed");
    GraphicsPipelineDesc primaryDesc{};
    primaryDesc.vertexShader = resources.vs; primaryDesc.pixelShader = resources.primaryFs;
    primaryDesc.layout = resources.pipelineLayout; primaryDesc.cullMode = CullMode::None;
    primaryDesc.enableDepthTest = false; primaryDesc.enableDepthWrite = false;
    primaryDesc.renderTargetCount = 4u;
    for (u32 i = 0; i < 4u; ++i) primaryDesc.renderTargetFormats[i] = DataFormat::RGBA32_Float;
    resources.primaryPipeline = device->CreateGraphicsPipeline(primaryDesc);
    GraphicsPipelineDesc companionDesc = primaryDesc;
    companionDesc.pixelShader = resources.companionFs; companionDesc.renderTargetCount = 8u;
    for (u32 i = 0; i < 8u; ++i) companionDesc.renderTargetFormats[i] = DataFormat::RGBA32_Float;
    resources.companionPipeline = device->CreateGraphicsPipeline(companionDesc);
    if (resources.primaryPipeline == handles::INVALID_PIPELINE ||
        resources.companionPipeline == handles::INVALID_PIPELINE)
        return fail("private primary/companion pipeline creation failed");

    VkPhysicalDeviceProperties physicalProperties{};
    vkGetPhysicalDeviceProperties(vk->GetNativePhysicalDevice(), &physicalProperties);
    const auto& limits = physicalProperties.limits;
    const float maxViewport = std::min({16384.0f,
        float(limits.maxViewportDimensions[0]), float(limits.maxViewportDimensions[1]),
        2.0f * (8.5f - limits.viewportBoundsRange[0]),
        2.0f * (limits.viewportBoundsRange[1] - 8.5f)});
    if (limits.maxColorAttachments < 8u)
        return fail("device cannot support eight private float attachments");
    if (!DDGIFirstPrimaryRasterFinite(maxViewport) || maxViewport < 17.0f)
        return fail("device viewport limits cannot support the 17x17 witness");
    const u64 alignment = std::max<u64>(1u, physicalProperties.limits.minUniformBufferOffsetAlignment);
    const u32 uboStride = static_cast<u32>((sizeof(DDGIFirstPrimaryRasterDrawConstants) + alignment - 1u) / alignment * alignment);
    const u32 targetSize = 17u;
    std::vector<u8> primaryRaw, companionRaw;
    struct SampleMeta {
        DDGIFirstPrimaryRasterSample sample;
        u64 primaryOffset, companionOffset, nativeCommand;
        ResourceHandle uniform, compact;
        DescriptorSetHandle descriptor;
        u32 variant{0u}; float viewport{0.0f};
    };
    std::vector<SampleMeta> outputMeta;
    const char* pairFlag = std::getenv("DDGI_FIRST_PRIMARY_RASTER_PAIR");
    const u32 variantCount = pairFlag && std::strcmp(pairFlag, "1") == 0 ? 4u : 1u;
    outputMeta.reserve(samples.size() * variantCount);
    const u64 primaryTextureBytes = u64(targetSize) * targetSize * 4u * sizeof(float);
    const u64 companionTextureBytes = primaryTextureBytes;
    const u64 primaryBytesPerSample = primaryTextureBytes * 4u;
    const u64 companionBytesPerSample = companionTextureBytes * 8u;
    std::vector<u8> uniformUploadRaw, uniformGpuRaw, compactUploadRaw, compactGpuRaw;
    struct RowOwned {
        RHIDeviceBase* device;
        std::vector<ResourceHandle> buffers, textures;
        DescriptorSetHandle descriptor{handles::INVALID_DESCRIPTOR_SET};
        CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ~RowOwned() {
            if (command != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(command);
            if (descriptor != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(descriptor);
            for (auto h : textures) if (h != handles::INVALID_RESOURCE) device->DestroyTexture(h);
            for (auto h : buffers) if (h != handles::INVALID_RESOURCE) device->DestroyBuffer(h);
        }
    };
    for (u32 sampleOrdinal = 0u; sampleOrdinal < samples.size() * variantCount; ++sampleOrdinal) {
        RowOwned rowOwned{device};
        auto createBuffer = [&](const BufferDesc& desc) {
            auto h = device->CreateBuffer(desc); rowOwned.buffers.push_back(h); return h;
        };
        auto createTexture = [&](const TextureDesc& desc) {
            auto h = device->CreateTexture(desc); rowOwned.textures.push_back(h); return h;
        };
        const auto& sample = samples[sampleOrdinal / variantCount];
        const u32 variant = sampleOrdinal % variantCount;
        float worldVertices[9]{};
        const auto& map = mapping[sample.logical_triangle];
        const float* positions = reinterpret_cast<const float*>(sourceRaw[3].data());
        const float* instances = reinterpret_cast<const float*>(sourceRaw[6].data());
        float worldMatrix[16]{};
        std::memcpy(worldMatrix, instances + u64(sample.instance) * 48u, sizeof(worldMatrix));
        for (u32 v = 0; v < 3u; ++v)
            DDGIFirstPrimaryRasterTransformPoint(worldMatrix,
                positions + u64(map.global_vertex_ids[v]) * 3u, worldVertices + v * 3u);
        math::m4x4 view{}, projection{};
        float virtualViewport = 0.0f;
        DDGIFirstPrimaryRasterBuildCamera(sample, worldVertices, view, projection, maxViewport, virtualViewport);
        // Same fresh primitive, four diagnostic conditions; production camera is untouched.
        // Reflect only camera X (including translation), preserving U/F and clip depth.
        if (variant >= 2u) for (u32 column = 0u; column < 4u; ++column) view.columns[column][0] = -view.columns[column][0];
        if (variantCount == 4u && (variant & 1u)) virtualViewport = 17.0f;
        float centerNdc[3]{};
        if (!DDGIFirstPrimaryRasterProject(view, projection, sample.first_point, centerNdc) ||
            std::abs(centerNdc[0]) > 2.0e-5f || std::abs(centerNdc[1]) > 2.0e-5f ||
            centerNdc[2] < 0.0f || centerNdc[2] > 1.0f)
            return fail("first-point camera projection is not centered/in-range");
        for (u32 v = 0; v < 3u; ++v) {
            float vertexNdc[3]{};
            if (!DDGIFirstPrimaryRasterProject(view, projection, worldVertices + v * 3u, vertexNdc) ||
                vertexNdc[0] < -1.0f || vertexNdc[0] > 1.0f ||
                vertexNdc[1] < -1.0f || vertexNdc[1] > 1.0f ||
                vertexNdc[2] < 0.0f || vertexNdc[2] > 1.0f)
                return fail("native triangle is outside the private raster frustum");
        }
        DDGIFirstPrimaryRasterDrawConstants constants{};
        constants.view_matrix = view; constants.proj_matrix = projection;
        const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        DDGIFirstPrimaryRasterSetMatrix(constants.world_matrix, identity);
        constants.view_width = targetSize; constants.view_height = targetSize;
        constants.meshlet_count = draw.GetTotalMeshletCount(); constants.has_prev_frame = 0u;
        constants.debug_mode = 0u;
        // Finite float32 stamps survive any denorm flush and are written by
        // the companion at W0.a/W1.w.  Draw row is always zero because each
        // private compact-id buffer contains one cluster.
        constants.pad2_1 = 0x3f000000u | (frame & 0x007fffffu);
        constants.pad2_2 = 0x3f800000u | (sampleOrdinal + 1u);
        BufferDesc compactDesc{}; compactDesc.size = 4u; compactDesc.type = BufferType::Structured;
        compactDesc.usage = GPUMemoryUsage::Dynamic; compactDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        compactDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage); compactDesc.name = "DDGIPrimaryRasterCompactId";
        const ResourceHandle compact = createBuffer(compactDesc);
        BufferDesc uniformDesc{}; uniformDesc.size = uboStride; uniformDesc.type = BufferType::Constant;
        uniformDesc.usage = GPUMemoryUsage::Dynamic; uniformDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        uniformDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform); uniformDesc.name = "DDGIPrimaryRasterConstants";
        const ResourceHandle uniform = createBuffer(uniformDesc);
        if (compact == handles::INVALID_RESOURCE || uniform == handles::INVALID_RESOURCE)
            return fail("private compact-id/368-byte UBO allocation failed");
        const u32 compactId = sample.cluster;
        if (!device->UpdateBufferData(compact, &compactId, sizeof(compactId), 0u) ||
            !device->UpdateBufferData(uniform, &constants, sizeof(constants), 0u))
            return fail("private compact-id/UBO upload failed");
        TextureDesc colorDesc{}; colorDesc.size = {targetSize, targetSize, 1u};
        colorDesc.format = DataFormat::RGBA32_Float; colorDesc.type = TextureType::Texture2D;
        colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
        ResourceHandle primaryTextures[4]{}, companionTextures[8]{};
        for (u32 i = 0; i < 4u; ++i) primaryTextures[i] = createTexture(colorDesc);
        for (u32 i = 0; i < 8u; ++i) companionTextures[i] = createTexture(colorDesc);
        const u64 primaryBytes = primaryBytesPerSample, companionBytes = companionBytesPerSample;
        ResourceHandle primaryReadback[4]{}, companionReadback[8]{};
        auto makeReadback = [&](u64 bytes, const char* name) {
            BufferDesc d{}; d.size = bytes; d.type = BufferType::Structured;
            d.usage = GPUMemoryUsage::Readback; d.memoryUsage = GPUMemoryUsage::Readback;
            d.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst); d.name = name;
            return createBuffer(d);
        };
        for (u32 i = 0; i < 4u; ++i) primaryReadback[i] = makeReadback(primaryTextureBytes, "DDGIPrimaryRasterReadback");
        for (u32 i = 0; i < 8u; ++i) companionReadback[i] = makeReadback(companionTextureBytes, "DDGICompanionRasterReadback");
        if (std::any_of(primaryTextures, primaryTextures + 4u, [](ResourceHandle h){return h == handles::INVALID_RESOURCE;}) ||
            std::any_of(companionTextures, companionTextures + 8u, [](ResourceHandle h){return h == handles::INVALID_RESOURCE;}) ||
            std::any_of(primaryReadback, primaryReadback + 4u, [](ResourceHandle h){return h == handles::INVALID_RESOURCE;}) ||
            std::any_of(companionReadback, companionReadback + 8u, [](ResourceHandle h){return h == handles::INVALID_RESOURCE;}))
            return fail("private raster target/readback allocation failed");
        DescriptorSetHandle descriptorSet = device->CreateDescriptorSet({resources.setLayout});
        rowOwned.descriptor = descriptorSet;
        if (descriptorSet == handles::INVALID_DESCRIPTOR_SET) return fail("private descriptor set allocation failed");
        DescriptorBufferInfo bufferInfos[10] = {
            {uniform, 0u, sizeof(constants)}, {sourceHandles[0], 0u, ~0ull},
            {sourceHandles[1], 0u, ~0ull}, {sourceHandles[2], 0u, ~0ull},
            {sourceHandles[3], 0u, ~0ull}, {compact, 0u, sizeof(u32)},
            {sourceHandles[5], 0u, ~0ull}, {sourceHandles[6], 0u, ~0ull},
            {sourceHandles[4], 0u, ~0ull}, {sourceHandles[7], 0u, ~0ull}};
        DescriptorImageInfo imageInfos[4] = {
            {sampler, albedo, ResourceState::ShaderResource},
            {sampler, normal, ResourceState::ShaderResource},
            {sampler, orm, ResourceState::ShaderResource},
            {sampler, handles::INVALID_RESOURCE, ResourceState::General}};
        WriteDescriptorSet writes[14]{};
        for (u32 i = 0; i < 10u; ++i) {
            writes[i].dstSet = descriptorSet; writes[i].dstBinding = i;
            writes[i].descriptorCount = 1u; writes[i].descriptorType = i == 0u ? DescriptorType::UniformBufferDynamic : DescriptorType::StorageBuffer;
            writes[i].bufferInfo = &bufferInfos[i];
        }
        for (u32 i = 0; i < 4u; ++i) {
            writes[10u + i].dstSet = descriptorSet; writes[10u + i].dstBinding = 10u + i;
            writes[10u + i].descriptorCount = 1u;
            writes[10u + i].descriptorType = i == 3u ? DescriptorType::Sampler : DescriptorType::SampledImage;
            writes[10u + i].imageInfo = &imageInfos[i];
        }
        device->UpdateDescriptorSets(14u, writes);
        CommandBufferHandle commandHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
        rowOwned.command = commandHandle;
        auto* command = commandHandle == handles::INVALID_COMMAND_BUFFER ? nullptr : vk->GetCommandBuffer(commandHandle);
        if (!command || !command->Reset() || !command->Begin()) return fail("private raster command Begin failed");
        command->MemoryBarrier(PipelineStage::Host | PipelineStage::AllCommands,
            PipelineStage::VertexShader | PipelineStage::FragmentShader,
            AccessFlag::HostWrite | AccessFlag::MemoryWrite | AccessFlag::ShaderWrite,
            AccessFlag::UniformRead | AccessFlag::ShaderRead);
        auto beginPass = [&](ResourceHandle* textures, u32 count) {
            RenderPassDesc desc{};
            for (u32 i = 0; i < count; ++i) {
                RenderPassDesc::Attachment attachment{}; attachment.texture = textures[i];
                attachment.format = DataFormat::RGBA32_Float; attachment.loadOp = LoadAction::Clear;
                attachment.storeOp = StoreAction::Store; attachment.clearValue.color = math::v4{0, 0, 0, 0};
                desc.colorAttachments.push_back(attachment);
            }
            desc.viewport.size = {17.0f, 17.0f}; desc.scissor.extent = {17u, 17u};
            command->BeginRenderPass(desc);
            const float virtualSize = virtualViewport;
            command->SetViewport({{8.5f - virtualSize * 0.5f, 8.5f - virtualSize * 0.5f}, {virtualSize, virtualSize}, 0.0f, 1.0f});
            command->SetScissor({{0, 0}, {17u, 17u}});
        };
        const DescriptorSetHandle sets[1] = {descriptorSet};
        const u32 dynamicOffsets[1] = {0u};
        beginPass(primaryTextures, 4u);
        command->BindGraphicsPipeline(resources.primaryPipeline);
        command->BindDescriptorSets(PipelineBindPoint::Graphics, resources.pipelineLayout, 0u, 1u, sets, 1u, dynamicOffsets);
        command->Draw(3u, sample.local_triangle * 3u, 1u, 0u);
        command->EndRenderPass();
        command->MemoryBarrier(PipelineStage::ColorAttachmentOutput, PipelineStage::Transfer,
                               AccessFlag::ColorAttachmentWrite, AccessFlag::TransferRead);
        BufferTextureCopyRegion region{}; region.imageExtent = {17u, 17u, 1u};
        for (u32 i = 0; i < 4u; ++i) command->CopyTextureToBuffer(primaryTextures[i], primaryReadback[i], &region, 1u);
        beginPass(companionTextures, 8u);
        command->BindGraphicsPipeline(resources.companionPipeline);
        command->BindDescriptorSets(PipelineBindPoint::Graphics, resources.pipelineLayout, 0u, 1u, sets, 1u, dynamicOffsets);
        command->Draw(3u, sample.local_triangle * 3u, 1u, 0u);
        command->EndRenderPass();
        command->MemoryBarrier(PipelineStage::ColorAttachmentOutput, PipelineStage::Transfer,
                               AccessFlag::ColorAttachmentWrite, AccessFlag::TransferRead);
        for (u32 i = 0; i < 8u; ++i) command->CopyTextureToBuffer(companionTextures[i], companionReadback[i], &region, 1u);
        command->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                               AccessFlag::TransferWrite, AccessFlag::HostRead);
        const bool ended = command->End(); const bool submitted = ended && command->Submit();
        const bool completed = submitted && command->WaitForCompletion();
        if (!ended || !submitted || !completed) return fail("private raster End/Submit/Wait failed");
        SampleMeta meta{}; meta.sample = sample; meta.variant = variant; meta.viewport = virtualViewport; meta.primaryOffset = primaryRaw.size(); meta.companionOffset = companionRaw.size();
        meta.nativeCommand = (u64)(uintptr_t)command->GetNativeCmdBuffer();
        meta.uniform = uniform; meta.compact = compact; meta.descriptor = descriptorSet;
        for (u32 i = 0; i < 4u; ++i) {
            const void* mapped = device->MapBuffer(primaryReadback[i]); if (!mapped) return fail("primary readback map failed");
            primaryRaw.insert(primaryRaw.end(), static_cast<const u8*>(mapped), static_cast<const u8*>(mapped) + primaryTextureBytes);
            device->UnmapBuffer(primaryReadback[i]);
        }
        for (u32 i = 0; i < 8u; ++i) {
            const void* mapped = device->MapBuffer(companionReadback[i]); if (!mapped) return fail("companion readback map failed");
            companionRaw.insert(companionRaw.end(), static_cast<const u8*>(mapped), static_cast<const u8*>(mapped) + companionTextureBytes);
            device->UnmapBuffer(companionReadback[i]);
        }
        if (!writeRaw(".primary.raw", primaryRaw) || !writeRaw(".companion.raw", companionRaw))
            return fail("partial raster raw write failed");
        outputMeta.push_back(meta);
        std::vector<u8> gpuUniform, gpuCompact;
        if (!DDGIFirstPrimaryRasterReadBuffer(device, uniform, sizeof(constants), gpuUniform, error, "uniform") ||
            !DDGIFirstPrimaryRasterReadBuffer(device, compact, sizeof(compactId), gpuCompact, error, "compact"))
            return fail(error.c_str());
        if (std::memcmp(gpuUniform.data(), &constants, sizeof(constants)) != 0 ||
            std::memcmp(gpuCompact.data(), &compactId, sizeof(compactId)) != 0)
            return fail("private GPU uniform/compact bytes differ from upload");
        const auto* uploadBytes = reinterpret_cast<const u8*>(&constants);
        uniformUploadRaw.insert(uniformUploadRaw.end(), uploadBytes, uploadBytes + sizeof(constants));
        uniformGpuRaw.insert(uniformGpuRaw.end(), gpuUniform.begin(), gpuUniform.end());
        const auto* idBytes = reinterpret_cast<const u8*>(&compactId);
        compactUploadRaw.insert(compactUploadRaw.end(), idBytes, idBytes + sizeof(compactId));
        compactGpuRaw.insert(compactGpuRaw.end(), gpuCompact.begin(), gpuCompact.end());
    }
    if (!writeRaw(".primary.raw", primaryRaw) || !writeRaw(".companion.raw", companionRaw) ||
        !writeRaw(".record.raw", recordRaw) || !writeRaw(".mapping.bin", mappingRaw) ||
        !writeRaw(".update_list.bin", updateRaw) || !writeRaw(".relocated_origins.bin", relocatedRaw) || !writeRaw(".audit.raw", auditRaw) ||
        !writeRaw(".uniform-upload.bin", uniformUploadRaw) || !writeRaw(".uniform-gpu.bin", uniformGpuRaw) ||
        !writeRaw(".compact-upload.bin", compactUploadRaw) || !writeRaw(".compact-gpu.bin", compactGpuRaw))
        return fail("private raster raw evidence write failed");
    for (u32 i = 0; i < 8u; ++i) {
        const std::string suffix = std::string(".source-") + sourceNames[i] + ".bin";
        if (!writeRaw(suffix.c_str(), sourceRaw[i])) return fail("native source raw write failed");
    }
    rawEvidenceSaved = true;
    const auto nativePipeline = [&](PipelineHandle handle) -> u64 {
        auto* p = vk->GetPipeline(handle);
        return p ? static_cast<u64>(reinterpret_cast<uintptr_t>(p->GetNativePipeline())) : 0u;
    };
    std::ofstream meta(prefix + ".json", std::ios::trunc);
    if (!meta) return fail("private raster metadata open failed");
    meta << "{\"schema\":\"" << (variantCount == 4u ? "ddgi_first_primary_raster_v3" : "ddgi_first_primary_raster_v2") << "\",\"complete\":true"
         << ",\"frame\":" << frame << ",\"ddgi_frame\":" << pass->GetVolumeData().FrameIndex
         << ",\"target_probes\":" << (scTargets ? "[409,439]" : "[409,410,507]")
         << ",\"sc_target_selection\":" << (scTargets ? "true" : "false")
         << ",\"sc_focus_selection\":" << (scFocus ? "true" : "false")
         << ",\"sample_count\":" << outputMeta.size()
         << ",\"variant_count\":" << variantCount << ",\"unique_sample_count\":" << samples.size()
         << ",\"target_size\":17,\"center_pixel\":[8,8],\"virtual_viewport\":" << maxViewport
         << ",\"frame_stamp_bits\":" << (0x3f000000u | (frame & 0x007fffffu))
         << ",\"uniform_stride\":" << uboStride
         << ",\"uniform_record_bytes\":368,\"private_upload_gpu_byteidentical\":true"
         << ",\"max_viewport_dimensions\":[" << limits.maxViewportDimensions[0] << ',' << limits.maxViewportDimensions[1] << "]"
         << ",\"viewport_bounds_range\":[" << limits.viewportBoundsRange[0] << ',' << limits.viewportBoundsRange[1] << "]"
         << ",\"subpixel_precision_bits\":" << limits.subPixelPrecisionBits
         << ",\"viewport_subpixel_bits\":" << limits.viewportSubPixelBits
         << ",\"record_vec4_stride\":" << recordVec4Stride
         << ",\"primary_bytes_per_sample\":" << primaryBytesPerSample
         << ",\"companion_bytes_per_sample\":" << companionBytesPerSample
         << ",\"primary_raw_suffix\":\".primary.raw\""
         << ",\"companion_raw_suffix\":\".companion.raw\""
         << ",\"primary_format\":\"RGBA32_Float_x4\",\"companion_format\":\"RGBA32_Float_x8\""
         << ",\"actual_primary_shader_entrypoints\":[\"gpu_driven_vertex_shader\",\"gpu_driven_fragment_shader\"]"
         << ",\"companion_layout\":\"W0=P+frameStamp,W1=UV+materialBits+rowStamp,W2=rawN+triangleBits,W3=rawT+meshletBits,W4=derivativeG+triangleBits,W5=normalSample+sourceInstanceBits,W6=actualVSCurrentClip,W7=nativeFragCoord\""
         << ",\"companion_entrypoint\":\"main\",\"production_writes\":false"
         << ",\"source_buffers_readonly\":true,\"missing_update_rays_substituted\":false"
         << ",\"native_draw_observer_claimed\":false,\"draw_evidence\":\"host_issued_draw_source\""
         << ",\"readback_completed\":true,\"begin_end_submit_checked\":true,\"submit_checked\":true,\"completion_checked\":true,\"map_after_completion\":true"
         << ",\"explicit_buffer_dependencies\":true,\"insert_barrier_buffer_ignored\":true"
         << ",\"update_list_handle\":" << updateHandle << ",\"relocated_origins_handle\":" << relocatedHandle
         << ",\"native_source_handles\":[";
    for (u32 i = 0; i < 8u; ++i) { if (i) meta << ','; meta << sourceHandles[i]; }
    meta << "] ,\"private_handles\":{\"vertex_shader\":" << resources.vs
         << ",\"primary_fragment_shader\":" << resources.primaryFs
         << ",\"companion_fragment_shader\":" << resources.companionFs
         << ",\"primary_pipeline\":" << resources.primaryPipeline
         << ",\"companion_pipeline\":" << resources.companionPipeline
         << ",\"primary_native_pipeline\":" << nativePipeline(resources.primaryPipeline)
         << ",\"companion_native_pipeline\":" << nativePipeline(resources.companionPipeline)
         << ",\"pipeline_layout\":" << resources.pipelineLayout << "}"
         << ",\"sc_atlas_independent\":{\"grid_offsets\":" << pass->GetSCGridOffsetsBuffer()
         << ",\"cards\":" << pass->GetSCGridCardsBuffer() << ",\"card_data\":" << pass->GetSCCardDataBuffer()
         << ",\"joined_to_firstpoint\":false}"
         << ",\"samples\":[";
    for (u32 i = 0; i < outputMeta.size(); ++i) {
        const auto& s = outputMeta[i].sample;
        if (i) meta << ',';
        meta << "{\"probe\":" << s.probe << ",\"ray\":" << s.ray
             << ",\"variant\":" << outputMeta[i].variant << ",\"viewport\":" << outputMeta[i].viewport
             << ",\"camera_x_reflected\":" << (outputMeta[i].variant >= 2u ? "true" : "false")
             << ",\"update_slot\":" << s.update_slot << ",\"status\":12"
             << ",\"native_command_buffer\":" << outputMeta[i].nativeCommand
             << ",\"uniform_handle\":" << outputMeta[i].uniform
             << ",\"compact_handle\":" << outputMeta[i].compact
             << ",\"descriptor_set_handle\":" << outputMeta[i].descriptor
             << ",\"global_ray\":" << (s.update_slot * raysPerProbe + s.ray)
             << ",\"expected_row_stamp_bits\":" << (0x3f800000u | (i + 1u))
             << ",\"material\":" << mapping[s.logical_triangle].scene_material_id
             << ",\"source_instance\":" << s.instance
             << ",\"logical_triangle\":" << s.logical_triangle << ",\"cluster\":" << s.cluster
             << ",\"meshlet\":" << s.meshlet << ",\"local_triangle\":" << s.local_triangle
             << ",\"draw\":[3," << s.local_triangle * 3u << ",1,0]"
             << ",\"primary_offset\":" << outputMeta[i].primaryOffset
             << ",\"companion_offset\":" << outputMeta[i].companionOffset << "}";
    }
    meta << "]}\n";
    if (!meta) return fail("private raster metadata write failed");
    PRIMAL_LOG_STREAM(Info, "Render") << "[DDGI_FIRST_PRIMARY_RASTER] complete frame=" << frame
                                       << " samples=" << outputMeta.size() << std::endl;
    return true;
}
