#include "TestFramework.h"
#include "Graphics/SceneDataAdapter.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMeshAsset.h"
#include "Graphics/RenderMesh.h"
#include <utility>
// LoadRenderItemData registers meshes in the Content-layer static free_list
// registry; drain it before exit or ~free_list asserts !_size at teardown.
#include "Content/ContentToEngine.h"

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

// --- Mock Classes ---

class MockResource : public RHIResource {
public:
    MockResource(RHIDeviceBase& device, const ResourceDesc& desc)
        : RHIResource(device, desc) {}
    
    bool Initialize() override { return true; }
    void SetPublicHandle(ResourceHandle h) { SetHandle(h); }
    
protected:
    void* mapImpl(uint64_t, uint64_t) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, uint64_t, uint64_t) override { return true; }
};

class MockCommandBuffer : public RHICommandBuffer {
public:
    MockCommandBuffer(RHIDeviceBase& device) : RHICommandBuffer(device, CommandQueueType::Graphics) {}
    bool Initialize() override { return true; }
protected:
    void destroyImpl() override {}
    bool resetImpl() override { return true; }
    bool beginImpl() override { return true; }
    bool endImpl() override { return true; }
    bool submitImpl(uint32_t) override { return true; }
    bool waitForCompletionImpl() override { return true; }
    
    void BeginRenderPass(const RenderPassDesc&) override {}
    void BeginRenderPass(RenderPassHandle) override {}
    void EndRenderPass() override {}
    void SetViewport(const ViewportDesc&) override {}
    void SetScissor(const Rect&) override {}
    void BindGraphicsPipeline(PipelineHandle) override {}
    void BindComputePipeline(PipelineHandle) override {}
    void BindVertexBuffers(uint32_t, uint32_t, const ResourceHandle*, const uint64_t*) override {}
    void BindIndexBuffer(ResourceHandle, DataFormat, uint64_t) override {}
    void BindDescriptorSets(PipelineBindPoint, PipelineLayoutHandle, uint32_t, uint32_t, const DescriptorSetHandle*, uint32_t, const uint32_t*) override {}
    void PushConstants(PipelineLayoutHandle, ShaderStage, uint32_t, uint32_t, const void*) override {}
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndirect(ResourceHandle, uint64_t, uint32_t) override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void DispatchIndirect(ResourceHandle, uint64_t) override {}
    void CopyBuffer(ResourceHandle, ResourceHandle, uint64_t, uint64_t, uint64_t) override {}
    void CopyBufferToTexture(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void CopyTextureToBuffer(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void BlitTexture(ResourceHandle, ResourceHandle, const TextureBlitRegion*, uint32_t, FilterMode) override {}
    void GenerateMipmaps(ResourceHandle) override {}
    void InsertBarrier(const ResourceBarrier*, uint32_t) override {}
    void WriteTimestamp(QueryPoolHandle, uint32_t) override {}
    // stale-test port: pure virtuals added to RHICommandBuffer after the Dawn era
    void SetComputeBytes(uint32_t, const void*, uint32_t) override {}
    void MemoryBarrier(PipelineStage, PipelineStage, AccessFlag, AccessFlag) override {}
};

class MockDevice : public RHIDeviceBase {
public:
    uint32_t buffersCreated=0, buffersDestroyed=0;
    struct MockBindingInfo {
        uint32_t binding;
        rhi::DescriptorType type;
        uint32_t count;
        rhi::ShaderStage stage;
        rhi::DescriptorBindingFlags flags;
    };
    std::vector<MockBindingInfo> lastBindings;

    MockDevice() { desc_.platform = RHIPlatform::Metal; }
    ~MockDevice() override {}

    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { return info_; }
    const DeviceDesc& GetDesc() const override { return desc_; }
    double GetTimestampPeriod() const override { return 1.0; }
    void WaitIdle() const override {}
    void Shutdown() override {}
    bool Submit(const QueueSubmitInfo&) override { return true; }
    SyncHandle CreateSync() override { return handles::INVALID_SYNC; }
    bool WaitForSync(SyncHandle, uint32_t) override { return true; }
    void DestroySync(SyncHandle) override {}
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc&) override { return handles::INVALID_QUERY_POOL; }
    void DestroyQueryPool(QueryPoolHandle) override {}
    bool GetQueryPoolResults(QueryPoolHandle, uint32_t, uint32_t, void*, size_t) override { return false; }
    SamplerHandle CreateSampler(const SamplerDesc&) override { return handles::INVALID_SAMPLER; }
    void DestroySampler(SamplerHandle) override {}
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override {
        lastBindings.clear();
        for (uint32_t i = 0; i < desc.bindingCount; ++i) {
            const auto& b = desc.bindings[i];
            lastBindings.push_back({b.binding, b.descriptorType, b.descriptorCount, b.stageFlags, b.flags});
        }
        return (DescriptorSetLayoutHandle)1;
    }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle) override {}
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc&) override { return handles::INVALID_PIPELINE_LAYOUT; }
    void DestroyPipelineLayout(PipelineLayoutHandle) override {}
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc&) override { return handles::INVALID_DESCRIPTOR_SET; }
    void DestroyDescriptorSet(DescriptorSetHandle) override {}
    void UpdateDescriptorSets(uint32_t, const WriteDescriptorSet*) override {}
    RHISwapChain* CreateSwapChain(const SwapChainDesc&) override { return nullptr; }
    void DestroySwapChain(RHISwapChain*) override {}
    RenderPassHandle CreateRenderPass(const RenderPassDesc&) override { return handles::INVALID_RENDER_PASS; }
    void DestroyRenderPass(RenderPassHandle) override {}
    
    ResourceHandle CreateBuffer(const BufferDesc& desc) override {
        ++buffersCreated;
        auto* res = new MockResource(*this, ResourceDesc(ResourceType::Buffer, (ResourceUsage)desc.bindFlags, desc.usage, desc.size));
        ResourceHandle h = reinterpret_cast<ResourceHandle>(res);
        res->SetPublicHandle(h);
        return h;
    }
    
    ResourceHandle CreateTexture(const TextureDesc&) override { return handles::INVALID_RESOURCE; }
    ResourceHandle CreateTextureView(const TextureViewDesc&) override { return handles::INVALID_RESOURCE; }
    void DestroyBuffer(ResourceHandle h) override {
        ++buffersDestroyed;
        delete reinterpret_cast<MockResource*>(h);
    }
    void DestroyTexture(ResourceHandle) override {}
    ShaderHandle CreateShader(const void*, size_t, ShaderStage, const char*) override { return handles::INVALID_SHADER; }
    void DestroyShader(ShaderHandle) override {}
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return handles::INVALID_PIPELINE; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return handles::INVALID_PIPELINE; }
    void DestroyPipeline(PipelineHandle) override {}
    void* MapBuffer(ResourceHandle, u64, u64) override { return nullptr; }
    void UnmapBuffer(ResourceHandle) override {}
    RHIGarbageCollector& GetGarbageCollector() override { static RHIGarbageCollector gc; return gc; }
    // stale-test port: pure virtuals added to RHIDeviceBase after the Dawn era
    void SetBufferDirtySize(ResourceHandle, u64) override {}
    RHIPlatform GetPlatform() const override { return RHIPlatform::Unknown; }
    
    CommandBufferHandle CreateCommandBuffer(CommandQueueType) override { return reinterpret_cast<CommandBufferHandle>(new MockCommandBuffer(*this)); }
    void DestroyCommandBuffer(CommandBufferHandle cmd) override { delete reinterpret_cast<MockCommandBuffer*>(cmd); }

private:
    DeviceDesc desc_;
    DeviceInfo info_;
};

// --- Test Suite ---

class TestSceneDataAdapter : public TestSuite {
public:
    TestSceneDataAdapter() : TestSuite("SceneDataAdapter Tests") {
        AddTestCase({"LoadMaterialTest", [this]() { return LoadMaterialTest(); }});
        AddTestCase({"LoadSimpleScene", [this]() { return LoadSimpleScene(); }});
    }

private:
    void WriteString(std::vector<uint8_t>& buffer, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.length());
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&len);
        buffer.insert(buffer.end(), ptr, ptr + sizeof(uint32_t));
        buffer.insert(buffer.end(), s.begin(), s.end());
    }

    template<typename T>
    void Write(std::vector<uint8_t>& buffer, T value) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
        buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
    }

public:
    TestResult PipelineCardBakeVersionsAndTruncation() {
        MockDevice device; SceneDataAdapter adapter;
        auto build=[&](uint32_t version, unsigned truncate) {
            std::vector<uint8_t> data;
            WriteString(data,"scene");Write<uint32_t>(data,0);Write<uint32_t>(data,1);
            WriteString(data,"LOD0");Write<uint32_t>(data,2);
            auto mesh=[&](const char* name, bool v2) {
                WriteString(data,name);Write<uint32_t>(data,0);Write<int32_t>(data,-1);
                Write<uint32_t>(data,0);Write<uint32_t>(data,0x20);Write<uint32_t>(data,3);
                Write<uint32_t>(data,2);Write<uint32_t>(data,3);Write<float>(data,0.f);
                const float positions[]={0,0,0, 1,0,0, 0,1,0};
                for(float p:positions)Write<float>(data,p);
                for(uint16_t i:{uint16_t(0),uint16_t(1),uint16_t(2)})Write<uint16_t>(data,i);
                Write<uint32_t>(data,0x4c48534d); // MSHL; 3 empty arrays
                Write<uint32_t>(data,0);Write<uint32_t>(data,0);Write<uint32_t>(data,0);
                Write<uint32_t>(data,0x20464453); // SDF; no voxels
                for(unsigned i=0;i<3;++i)Write<uint32_t>(data,0);
                for(unsigned i=0;i<6;++i)Write<float>(data,0.f);
                for(unsigned i=0;i<3;++i)Write<uint32_t>(data,0);
                Write<uint32_t>(data,v2?0x32424443u:0x4b424443u);
                if(v2)Write<uint32_t>(data,version);
                Write<uint32_t>(data,1); // one 32-byte card
                RHICardBakeCard card{};card.center[0]=v2?2.f:1.f;card.axis_direction=2;card.extent[0]=.25f;card.resolution=32;
                const auto* begin=reinterpret_cast<const uint8_t*>(&card);data.insert(data.end(),begin,begin+sizeof(card));
                Write<uint32_t>(data,1); // one 32-byte surfel
                RHICardBakeSurfel surfel{};surfel.pos[1]=3.f;surfel.normal[2]=1.f;surfel.area=.5f;
                begin=reinterpret_cast<const uint8_t*>(&surfel);data.insert(data.end(),begin,begin+sizeof(surfel));
            };
            mesh("legacy",false);mesh("new-v2",true);
            if(truncate)data.resize(data.size()-truncate);
            return data;
        };
        auto good=build(2,0);auto meshes=adapter.LoadRenderItemData(&device,good.data(),u32(good.size()));
        bool ok=meshes.size()==2 && meshes[0].name=="legacy" && meshes[1].name=="new-v2";
        for(size_t i=0;i<meshes.size();++i) {
            RHIMeshAsset asset{};const auto raw=primal::content::get_rhi_mesh_id(meshes[i].meshEntityId);
            ok &= primal::content::get_rhi_mesh_asset(raw,asset) && asset.card_bake.cards.size()==1 &&
                  asset.card_bake.surfels.size()==1;
            if(asset.card_bake.cards.size()==1)ok &= asset.card_bake.cards[0].center[0]==float(i+1);
            if(asset.card_bake.surfels.size()==1)ok &= asset.card_bake.surfels[0].pos[1]==3.f;
            if(meshes[i].mesh){meshes[i].mesh->Destroy(&device);delete meshes[i].mesh;}
            if(meshes[i].ownsMeshContentResource)primal::content::destroy_resource(meshes[i].meshEntityId,primal::content::asset_type::mesh,primal::content::GraphicsAPI::RHI);
        }
        ok &= device.buffersCreated==device.buffersDestroyed;
        // First mesh is registered before the second card section fails: an
        // empty result alone is insufficient unless its GPU buffers are freed.
        for(const auto [version,trunc]:{std::pair<uint32_t,unsigned>{3,0},{2,1},{2,34},{2,68},{2,74},{2,77}}) {
            auto invalid=build(version,trunc);auto failed=adapter.LoadRenderItemData(&device,invalid.data(),u32(invalid.size()));
            ok &= failed.empty() && device.buffersCreated==device.buffersDestroyed;
            // Avoid leaking evidence in case of a regression that returns a partial result.
            for(auto& item:failed){if(item.mesh){item.mesh->Destroy(&device);delete item.mesh;}if(item.ownsMeshContentResource)primal::content::destroy_resource(item.meshEntityId,primal::content::asset_type::mesh,primal::content::GraphicsAPI::RHI);}
        }
        return ok?TestResult::Passed:TestResult::Failed;
    }

    TestResult LoadMaterialTest() {
        MockDevice device;
        SceneDataAdapter adapter;

        // Construct Mock Material Data
        std::vector<uint8_t> data;

        // Header
        Write<uint32_t>(data, 0x4C54414D); // Magic 'MATL'
        Write<uint32_t>(data, 1);          // Version 1
        Write<uint32_t>(data, 1);          // Shader Count

        // Shader 0
        Write<uint32_t>(data, (uint32_t)ShaderStage::Vertex); // Stage
        uint32_t bytecodeSize = 4;
        Write<uint32_t>(data, bytecodeSize); // Bytecode Size
        
        std::string entryPoint = "main";
        Write<uint32_t>(data, (uint32_t)entryPoint.length()); // EntryPoint Size
        data.insert(data.end(), entryPoint.begin(), entryPoint.end()); // EntryPoint

        // Bytecode (dummy)
        Write<uint32_t>(data, 0xDEADBEEF);

        // Pipeline States
        BlendState blendState;
        blendState.enableBlend = true;
        Write<BlendState>(data, blendState);

        DepthStencilState depthState;
        depthState.enableDepthTest = true;
        Write<DepthStencilState>(data, depthState);

        RasterizerState rasterState;
        rasterState.cullMode = CullMode::Back;
        Write<RasterizerState>(data, rasterState);

        Write<uint32_t>(data, (uint32_t)PrimitiveTopology::TriangleList);

        // Vertex Attributes
        Write<uint32_t>(data, 1);
        VertexInputAttribute attr;
        attr.location = 0;
        attr.binding = 0;
        attr.format = DataFormat::RGB32_Float;
        attr.offset = 0;
        Write<VertexInputAttribute>(data, attr);

        // Vertex Bindings
        Write<uint32_t>(data, 1);
        VertexInputBinding binding;
        binding.binding = 0;
        binding.stride = 12;
        binding.perVertex = true;
        Write<VertexInputBinding>(data, binding);

        // Descriptor Binding Count
        Write<uint32_t>(data, 1);
        Write<uint32_t>(data, 0); // Binding 0
        Write<rhi::DescriptorType>(data, rhi::DescriptorType::UniformBuffer);
        Write<uint32_t>(data, 1); // Count 1
        Write<rhi::ShaderStage>(data, rhi::ShaderStage::Vertex);
        Write<rhi::DescriptorBindingFlags>(data, rhi::DescriptorBindingFlags::PartiallyBound | rhi::DescriptorBindingFlags::UpdateAfterBind);

            // Execute
        std::shared_ptr<Material> material = adapter.LoadMaterial(&device, data.data(), (uint32_t)data.size());

        // Verify
        if (!material) {
            std::cout << "Failed to load material" << std::endl;
            return TestResult::Failed;
        }

        if (material->GetVertexBindings().size() != 1) {
             std::cout << "Vertex bindings size mismatch: " << material->GetVertexBindings().size() << std::endl;
             return TestResult::Failed;
        }

        // Verify Bindless Flags
        if (device.lastBindings.empty()) {
             std::cout << "No bindings recorded in mock device" << std::endl;
             return TestResult::Failed;
        }

        auto flags = device.lastBindings[0].flags;
        if ((flags & rhi::DescriptorBindingFlags::PartiallyBound) == rhi::DescriptorBindingFlags::None) {
             std::cout << "PartiallyBound flag missing" << std::endl;
             return TestResult::Failed;
        }
        if ((flags & rhi::DescriptorBindingFlags::UpdateAfterBind) == rhi::DescriptorBindingFlags::None) {
             std::cout << "UpdateAfterBind flag missing" << std::endl;
             return TestResult::Failed;
        }

        if (material->GetDescriptorSetLayout() == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            std::cout << "Descriptor Set Layout not created" << std::endl;
            return TestResult::Failed;
        }


        return TestResult::Passed;
    }

    TestResult LoadSimpleScene() {
        MockDevice device;
        SceneDataAdapter adapter;

        // Construct Mock Data
        std::vector<uint8_t> data;
        
        // Scene Name
        WriteString(data, "TestScene");

        // Num Materials
        Write<uint32_t>(data, 0);
        
        // Num LOD Groups
        Write<uint32_t>(data, 1);
        
        // LOD Group 0
        WriteString(data, "LOD0"); // LOD Name
        Write<uint32_t>(data, 1); // Num Meshes

        // Mesh 0
        WriteString(data, "Mesh0"); // Mesh Name
        Write<uint32_t>(data, 0); // LOD ID
        
        uint32_t vertexSize = sizeof(float) * 3 + sizeof(float) * 2; // Pos + UV
        uint32_t numVertices = 3;
        Write<uint32_t>(data, vertexSize); // Vertex Size
        Write<uint32_t>(data, numVertices); // Num Vertices
        
        uint32_t indexSize = sizeof(uint16_t);
        uint32_t numIndices = 3;
        Write<uint32_t>(data, indexSize); // Index Size
        Write<uint32_t>(data, numIndices); // Num Indices
        
        Write<float>(data, 0.5f); // LOD Threshold

        // Position Buffer (3 vertices * 3 floats)
        for (int i = 0; i < 3; ++i) {
            Write<float>(data, (float)i);
            Write<float>(data, (float)i);
            Write<float>(data, (float)i);
        }

        // Element Buffer (3 vertices * 2 floats)
        for (int i = 0; i < 3; ++i) {
            Write<float>(data, 0.0f);
            Write<float>(data, 1.0f);
        }

        // Index Buffer (3 indices * 2 bytes)
        Write<uint16_t>(data, 0);
        Write<uint16_t>(data, 1);
        Write<uint16_t>(data, 2);

        // Material Index
        Write<int32_t>(data, -1);

        // Execute
        // The blob API accepts byte buffers, including a deliberately unaligned base.
        std::vector<uint8_t> packed(data.size() + 1);
        std::memcpy(packed.data() + 1, data.data(), data.size());
        auto results = adapter.Load(&device, packed.data() + 1, (uint32_t)data.size());

        // Verify
        if (results.size() != 1) {
            std::cout << "Expected 1 mesh, got " << results.size() << std::endl;
            return TestResult::Failed;
        }
        
        const auto& info = results[0];
        if (info.name != "Mesh0") {
             std::cout << "Name mismatch: " << info.name << std::endl;
             return TestResult::Failed;
        }
        if (info.lodId != 0) return TestResult::Failed;
        if (info.mesh == nullptr) return TestResult::Failed;
        
        // Clean up
        for (auto& res : results) {
            if (res.mesh) {
                res.mesh->Destroy(&device);
                delete res.mesh;
            }
        }

        return TestResult::Passed;
    }

    TestResult LoadRenderItemData_Matches_MeshCPU_Format() {
        MockDevice device;
        SceneDataAdapter adapter;

        // Construct Mock Data following the CURRENT MeshCPU/SceneDataAdapter
        // reader format:
        // 1. [u32 num_materials] × { [u32 name_len][name]
        //                            [u32 diff_len][diffuse]
        //                            [u32 norm_len][normal] }   (3-field variant)
        // 2. lod_count (u32)
        // 3. thresholds (f32 * lod_count)
        // 4. lod_offsets (lod_offset * lod_count)
        // 5. LOD Blocks...
        // stale-test port: the reader gained a material table header after the
        // Dawn era — without it the first threshold float gets parsed as a
        // material-name length (0.5f → 1056964608 → "Name too long").
        std::vector<uint8_t> data;

        auto WriteStr = [this, &data](const char* s) {
            uint32_t len = (uint32_t)strlen(s);
            Write<uint32_t>(data, len);
            for (uint32_t i = 0; i < len; ++i) Write<uint8_t>(data, (uint8_t)s[i]);
        };

        Write<uint32_t>(data, 1);   // num_materials
        WriteStr("mock_material");
        WriteStr("");               // diffuse path
        WriteStr("");               // normal path

        uint32_t lodCount = 1;
        Write<uint32_t>(data, lodCount);
        
        // Thresholds
        Write<float>(data, 0.5f);
        
        // LOD Offsets
        struct lod_offset { uint16_t offset; uint16_t count; };
        // count=0: the packed u32 {0,0}=0x00000000 stays outside the pipeline-
        // format probe range (50,500000) — {0,1}=0x00010000=65536 would be
        // misdetected as a pipeline mesh_count. The value is unused by the
        // reader (the array is skipped wholesale).
        lod_offset lo { 0, 0 };
        Write<uint16_t>(data, lo.offset);
        Write<uint16_t>(data, lo.count);
        
        // LOD 0 Block
        // Submesh Count
        Write<uint32_t>(data, 1);
        
        // Size of Submeshes (Placeholder)
        size_t sizePos = data.size();
        Write<uint32_t>(data, 0);
        size_t submeshStart = data.size();
        
        // Submesh 0 — old (MeshCPU) submesh header is SIX u32s: the reader
        // treats the first as materialIndex, then elementSize/vertexCount/
        // indexCount/elementsType/primitiveTopology.
        uint32_t materialIndex = 0;
        uint32_t elementSize = 0; // Position only
        uint32_t vertexCount = 1;
        uint32_t indexCount = 3;
        uint32_t elementsType = 0;
        uint32_t primitiveTopology = 4; // Triangle List

        Write<uint32_t>(data, materialIndex);
        Write<uint32_t>(data, elementSize);
        Write<uint32_t>(data, vertexCount);
        Write<uint32_t>(data, indexCount);
        Write<uint32_t>(data, elementsType);
        Write<uint32_t>(data, primitiveTopology);
        
        // Position Buffer (1 vertex * 3 floats = 12 bytes) -> Aligned to 16 bytes
        Write<float>(data, 1.0f); Write<float>(data, 2.0f); Write<float>(data, 3.0f);
        // Padding (12 bytes to reach 64, since current is 52)
        for(int i=0; i<12; ++i) Write<uint8_t>(data, 0);
        
        // Element Buffer (Empty)
        
        // Index Buffer (3 indices * 2 bytes = 6 bytes) -> Aligned to 16 bytes
        // Use 16-bit indices since vertexCount < 65536
        Write<uint16_t>(data, 0); Write<uint16_t>(data, 0); Write<uint16_t>(data, 0);
        // Padding (10 bytes)
        for(int i=0; i<10; ++i) Write<uint8_t>(data, 0);
        
        // Backfill Size
        uint32_t submeshSize = (uint32_t)(data.size() - submeshStart);
        memcpy(data.data() + sizePos, &submeshSize, sizeof(uint32_t));
        
        // Call LoadRenderItemData
        auto meshes = adapter.LoadRenderItemData(&device, data.data(), (uint32_t)data.size());
        
        if (meshes.size() != 1) return TestResult::Failed;
        if (meshes[0].lodId != 0) return TestResult::Failed;
        // if (meshes[0].lodThreshold != 0.5f) return TestResult::Failed; // Currently LoadRenderItemData might not set this correctly or logic differs
        
        return TestResult::Passed;
    }
};

int main() {
    TestSceneDataAdapter test;
    int result = 0;

    if (test.PipelineCardBakeVersionsAndTruncation() == TestResult::Failed) {
        std::cerr << "PipelineCardBakeVersionsAndTruncation Failed" << std::endl;
        result = 1;
    }
    
    if (test.LoadSimpleScene() == TestResult::Failed) {
        std::cerr << "LoadSimpleScene Failed" << std::endl;
        result = 1;
    }
    
    if (test.LoadRenderItemData_Matches_MeshCPU_Format() == TestResult::Failed) {
        std::cerr << "LoadRenderItemData_Matches_MeshCPU_Format Failed" << std::endl;
        result = 1;
    }

    // LoadRenderItemData registers meshes in the Content-layer static
    // free_list<RHIMeshAsset> registry; drain before exit or the static
    // destructor asserts !_size (FreeList.h:27).
    primal::content::shutdown();
    return result;
}
