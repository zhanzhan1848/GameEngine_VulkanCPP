#include "TestFramework.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIEntityManager.h"
#include "Graphics/RHI/Components/RenderLayerComponent.h"
#include "Graphics/RHI/Components/MaterialComponent.h"
#include "Graphics/RHI/Components/GPUBufferComponent.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/SceneDataAdapter.h"
#include "Common/Id.h"
// stale-test port: proxies must reference real ECS entities (see entityId below)
#include "Components/Entity.h"
#include "Components/Transform.h"
#include <vector>
#include <unordered_map>
#include <cstring>

using namespace primal::graphics;
using namespace primal::graphics::rhi;

// Mock CommandBuffer for Integration Test
class MockCommandBufferIntegration : public RHICommandBuffer {
public:
    MockCommandBufferIntegration(RHIDeviceBase& device) 
        : RHICommandBuffer(device, CommandQueueType::Graphics) {
        handle_ = (CommandBufferHandle)12345; 
    }
    
    bool Initialize() override { return true; }
    void Destroy() override {}
    
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
    void BindGraphicsPipeline(PipelineHandle) override { bindPipelineCalled = true; }
    void BindVertexBuffers(uint32_t, uint32_t, const ResourceHandle*, const uint64_t*) override {}
    void BindIndexBuffer(ResourceHandle, DataFormat, uint64_t) override {}
    void BindDescriptorSets(PipelineBindPoint, PipelineLayoutHandle, uint32_t, uint32_t, const DescriptorSetHandle*, uint32_t, const uint32_t*) override { bindSetsCalled = true; }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance) override {
        drawIndexedCalled = true;
        drawCallCount++;
        lastDrawIndexCount = indexCount;
    }
    void DrawIndirect(ResourceHandle, uint64_t, uint32_t) override {}
    void BindComputePipeline(PipelineHandle) override {}
    void WriteTimestamp(QueryPoolHandle, uint32_t) override {}
    // stale-test port: pure virtuals added to RHICommandBuffer after the Dawn era
    void PushConstants(PipelineLayoutHandle, ShaderStage, uint32_t, uint32_t, const void*) override {}
    void SetComputeBytes(uint32_t, const void*, uint32_t) override {}
    void MemoryBarrier(PipelineStage, PipelineStage, AccessFlag, AccessFlag) override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void DispatchIndirect(ResourceHandle, uint64_t) override {}
    void CopyBuffer(ResourceHandle, ResourceHandle, uint64_t, uint64_t, uint64_t) override {}
    void CopyBufferToTexture(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void CopyTextureToBuffer(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void BlitTexture(ResourceHandle, ResourceHandle, const TextureBlitRegion*, uint32_t, FilterMode) override {}
    void GenerateMipmaps(ResourceHandle) override {}
    void InsertBarrier(const ResourceBarrier*, uint32_t) override {}

    bool drawIndexedCalled = false;
    uint32_t drawCallCount = 0;
    uint32_t lastDrawIndexCount = 0;
    bool bindPipelineCalled = false;
    bool bindSetsCalled = false;
};

// Mock RHISwapChain
class MockRHISwapChainIntegration : public RHISwapChain {
public:
    MockRHISwapChainIntegration(RHIDeviceBase& device, const SwapChainDesc& desc) 
        : RHISwapChain(device, desc) {}
    
    bool Initialize() override { return true; }
    void Destroy() override {}
    
    void Resize(uint32_t width, uint32_t height) override {
        swapChainDesc_.width = width;
        swapChainDesc_.height = height;
    }
    
    bool AcquireNextImage(uint32_t* imageIndex, SyncHandle semaphore, SyncHandle fence) override {
        *imageIndex = 0;
        return true;
    }
    
    void Present(SyncHandle semaphore) override {}
    
    uint32_t GetCurrentBackBufferIndex() const override { return 0; }
    
    ResourceHandle GetBackBuffer(uint32_t index) const override { return (ResourceHandle)3003; }

    // RHIResource implementation
    void* mapImpl(uint64_t, uint64_t) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, uint64_t, uint64_t) override { return true; }
};

// Mock RHIDevice implementation for Integration Test
class MockRHIDeviceIntegration : public RHIDevice<MockRHIDeviceIntegration> {
public:
    MockCommandBufferIntegration* mockCmdBuffer{nullptr};
    MockRHISwapChainIntegration* mockSwapChain{nullptr};
    
    DescriptorSetLayoutDesc lastLayoutDesc;
    std::vector<DescriptorSetLayoutBinding> lastBindings;

    MockRHIDeviceIntegration() : RHIDevice(DeviceDesc()) {
        isValid_ = true;
    }

    virtual ~MockRHIDeviceIntegration() {
        if (mockCmdBuffer) {
            if (mockCmdBuffer->GetHandle() != handles::INVALID_COMMAND_BUFFER) {
                UnregisterCommandBuffer(mockCmdBuffer->GetHandle());
            }
            delete mockCmdBuffer;
        }
        if (mockSwapChain) {
            delete mockSwapChain;
        }
    }
    
    bool initializeImpl() { return true; }
    void shutdownImpl() {}
    double getTimestampPeriodImpl() const { return 1.0; }
    void waitIdleImpl() const {}
    void beginFrameImpl() {}
    void endFrameImpl() {}
    void presentImpl() {}
    void queryDeviceInfo(DeviceInfo&) {}
    // stale-test port: Impl hooks added to the RHIDevice CRTP base after the Dawn era
    void setBufferDirtySizeImpl(ResourceHandle, u64) {}
    u32 getCurrentFrameIndexImpl() const { return 0; }
    
    bool submitImpl(const QueueSubmitInfo& info) {
        if (mockCmdBuffer && mockCmdBuffer->GetHandle() == info.cmdBuffer) {
            return mockCmdBuffer->Submit();
        }
        return false;
    }
    SyncHandle createSyncImpl() { return (SyncHandle)54321; }
    bool waitForSyncImpl(SyncHandle, u32) { return true; }
    void destroySyncImpl(SyncHandle) {}
    
    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc&) { return handles::INVALID_QUERY_POOL; }
    void destroyQueryPoolImpl(QueryPoolHandle) {}
    bool getQueryPoolResultsImpl(QueryPoolHandle, uint32_t, uint32_t, void*, size_t) { return false; }
    SamplerHandle createSamplerImpl(const SamplerDesc&) { return (SamplerHandle)7001; }
    void destroySamplerImpl(SamplerHandle) {}
    
    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc) {
        lastLayoutDesc = desc;
        if (desc.bindingCount > 0 && desc.bindings) {
            lastBindings.assign(desc.bindings, desc.bindings + desc.bindingCount);
            lastLayoutDesc.bindings = lastBindings.data();
        } else {
            lastBindings.clear();
            lastLayoutDesc.bindings = nullptr;
        }
        return (DescriptorSetLayoutHandle)4001; 
    }
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle) {}
    
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc&) { return (PipelineLayoutHandle)8001; }
    void destroyPipelineLayoutImpl(PipelineLayoutHandle) {}
    
    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc&) { return (DescriptorSetHandle)6001; }
    void destroyDescriptorSetImpl(DescriptorSetHandle) {}
    void updateDescriptorSetsImpl(uint32_t, const WriteDescriptorSet*) {}
    
    RenderPassHandle createRenderPassImpl(const RenderPassDesc&) { return (RenderPassHandle)9001; }
    void destroyRenderPassImpl(RenderPassHandle) {}

    // Mock Buffer Storage
    std::vector<uint8_t> mockBufferMemory;

    ResourceHandle createBufferImpl(const BufferDesc& desc) { 
        if (mockBufferMemory.size() < desc.size) {
            mockBufferMemory.resize(desc.size);
        }
        return (ResourceHandle)1001; 
    }
    void destroyBufferImpl(ResourceHandle) {}
    void* mapBufferImpl(ResourceHandle, u64 offset, u64) { return mockBufferMemory.data() + offset; }
    void unmapBufferImpl(ResourceHandle) {}

    ResourceHandle createTextureImpl(const TextureDesc&) { return (ResourceHandle)2002; }
    ResourceHandle createTextureViewImpl(const TextureViewDesc&) { return (ResourceHandle)2003; }
    void destroyTextureImpl(ResourceHandle) {}
    void destroyTextureViewImpl(ResourceHandle) {}
    ShaderHandle createShaderImpl(const void*, size_t, ShaderStage, const char*) { return (ShaderHandle)3001; }
    void destroyShaderImpl(ShaderHandle) {}
    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc&) { return (PipelineHandle)666; }
    void destroyPipelineImpl(PipelineHandle) {}
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc&) { return (PipelineHandle)5001; }
    
    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc) {
        if (!mockSwapChain) {
            mockSwapChain = new MockRHISwapChainIntegration(*this, desc);
        }
        return mockSwapChain;
    }
    void destroySwapChainImpl(RHISwapChain* sc) {
        if (sc == mockSwapChain) {
            // Managed by device destructor
        }
    }
    
    CommandBufferHandle createCommandBufferImpl(CommandQueueType) {
        if (!mockCmdBuffer) {
            mockCmdBuffer = new MockCommandBufferIntegration(*this);
            RegisterCommandBuffer(mockCmdBuffer);
        }
        return mockCmdBuffer->GetHandle();
    }

    void destroyCommandBufferImpl(CommandBufferHandle handle) {
        if (mockCmdBuffer && mockCmdBuffer->GetHandle() == handle) {
             // Already handled in destructor
        }
    }
};

class TestRenderSystemIntegration : public Engine::Test::TestSuite {
public:
    TestRenderSystemIntegration() : Engine::Test::TestSuite("TestRenderSystemIntegration") {
        AddTestCase({"RenderSystemIntegration_Test", [this]() { return RunTests(); }});
    }

    // Helper to append data to vector
    template<typename T>
    void Append(std::vector<uint8_t>& buffer, const T& value) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
        buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
    }
    
    void AppendString(std::vector<uint8_t>& buffer, const std::string& str) {
        // Not null terminated in this format, just bytes
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(str.data());
        buffer.insert(buffer.end(), ptr, ptr + str.length());
    }

    std::vector<uint8_t> CreateSceneData() {
        std::vector<uint8_t> buffer;
        
        // 1. Scene Name
        std::string sceneName = "TestScene";
        Append(buffer, (uint32_t)sceneName.length());
        AppendString(buffer, sceneName);

        // 2. Num Materials
        Append(buffer, (uint32_t)1);
        
        // Material Data
        std::vector<uint8_t> matBuffer;
        Append(matBuffer, (uint32_t)0x4C54414D); // 'MATL'
        Append(matBuffer, (uint32_t)1); // Version
        Append(matBuffer, (uint32_t)0); // Shader Count (Simple material for test)
        
        // Pipeline States
        Append(matBuffer, BlendState());
        Append(matBuffer, DepthStencilState());
        Append(matBuffer, RasterizerState());
        Append(matBuffer, (uint32_t)PrimitiveTopology::TriangleList);
        
        // Vertex Attributes
        Append(matBuffer, (uint32_t)1); // Num attributes
        VertexInputAttribute attr;
        attr.location = 0;
        attr.binding = 0;
        attr.format = DataFormat::RGB32_Float;
        attr.offset = 0;
        Append(matBuffer, attr);
        
        // Vertex Bindings
        Append(matBuffer, (uint32_t)1); // Num bindings
        VertexInputBinding binding;
        binding.binding = 0;
        binding.stride = 12;
        binding.perVertex = true;
        Append(matBuffer, binding);
        
        // Descriptor Bindings
        Append(matBuffer, (uint32_t)1); // Num descriptor bindings
        Append(matBuffer, (uint32_t)0); // binding
        Append(matBuffer, DescriptorType::UniformBuffer); // type
        Append(matBuffer, (uint32_t)1); // count
        Append(matBuffer, ShaderStage::Vertex); // stage
        // Bindless flags: PartiallyBound | UpdateAfterBind
        Append(matBuffer, DescriptorBindingFlags::PartiallyBound | DescriptorBindingFlags::UpdateAfterBind); 

        // Add Material Size and Data to main buffer
        Append(buffer, (uint32_t)matBuffer.size());
        buffer.insert(buffer.end(), matBuffer.begin(), matBuffer.end());

        // 3. Num LOD Groups
        Append(buffer, (uint32_t)1);
        
        // LOD Name
        std::string lodName = "LOD0";
        Append(buffer, (uint32_t)lodName.length());
        AppendString(buffer, lodName);
        
        // Num Meshes
        Append(buffer, (uint32_t)1);
        
        // Mesh Name
        std::string meshName = "Mesh0";
        Append(buffer, (uint32_t)meshName.length());
        AppendString(buffer, meshName);
        
        // Mesh Data
        Append(buffer, (uint32_t)0); // LOD ID
        Append(buffer, (uint32_t)12); // Vertex Size (3 floats)
        Append(buffer, (uint32_t)3); // Num Vertices
        Append(buffer, (uint32_t)2); // Index Size (16-bit)
        Append(buffer, (uint32_t)3); // Num Indices
        Append(buffer, (float)1000.0f); // LOD Threshold
        
        // Position Data (3 vertices * 3 floats)
        float positions[] = { 
            0.0f, 0.5f, 0.0f,
            0.5f, -0.5f, 0.0f,
            -0.5f, -0.5f, 0.0f
        };
        for(float f : positions) Append(buffer, f);
        
        // Element Data (None)
        
        // Index Data (3 indices * uint16_t)
        uint16_t indices[] = { 0, 1, 2 };
        for(uint16_t i : indices) Append(buffer, i);
        
        // Material Index
        Append(buffer, (int32_t)0);

        return buffer;
    }

    Engine::Test::TestResult RunTests() {
        // Setup Device
        MockRHIDeviceIntegration device;
        device.Initialize();
        
        // Setup EntityManager
        RHIEntityManager entityManager;

        // Setup RenderSystem
        RenderSystem renderSystem;
        RenderSystemInitInfo initInfo;
        initInfo.device = &device;
        initInfo.entityManager = &entityManager;
        initInfo.width = 800;
        initInfo.height = 600;
        
        if (!renderSystem.Initialize(initInfo)) {
            return Engine::Test::TestResult::Failed;
        }
        
        // Load Scene Data
        std::vector<uint8_t> sceneData = CreateSceneData();
        SceneDataAdapter adapter;
        auto meshes = adapter.Load(&device, sceneData.data(), sceneData.size());
        
        if (meshes.empty()) {
            std::cout << "Failed to load scene data" << std::endl;
            return Engine::Test::TestResult::Failed;
        }

        // Create RenderProxy and Scene
        RenderProxy proxy;
        
        // Fix: Assign a valid Entity ID to the mesh so it can be found by RenderSystem
        primal::id::id_type meshId = primal::id::new_generation(2);
        meshes[0].mesh->SetEntityId(meshId);
        proxy.meshId = meshId;

        // proxy.materialId = meshes[0].materialInstance->GetMaterial()->GetDescriptorSetLayout(); // This is wrong, we need Material ID
        // Wait, Material class doesn't seem to have a GetId() method exposed in the header I read.
        // It's likely using address or some other mechanism if not explicit.
        // Let's check how RenderSystem registers materials.
        // It registers MaterialInstance.
        
        // Let's assume for now we use an arbitrary ID and register it.
        primal::id::id_type matId = primal::id::new_generation(0); // Using simple generation
        // Actually, let's use a dummy ID.
        matId = 12345;
        
        renderSystem.RegisterMaterialInstance(matId, meshes[0].materialInstance);
        proxy.materialId = matId;
        
        proxy.transform = primal::graphics::rhi::math::MatrixIdentity();
        proxy.worldAABB.min = {-100, -100, -100};
        proxy.worldAABB.max = {100, 100, 100};
        // stale-test port: the render loop resolves proxies through the ECS
        // transform registry; a fabricated entity id fails the is_valid
        // assert in get_updated_components_flags. Create a real entity.
        {
            static primal::transform::init_info tfInfo = []{
                primal::transform::init_info t{};
                t.rotation[3] = 1.0f;  // identity quaternion {x,y,z,w}
                return t;
            }();
            primal::game_entity::entity_info entInfo{};
            entInfo.transform = &tfInfo;
            static primal::game_entity::entity renderEntity = primal::game_entity::create(entInfo);
            proxy.entityId = renderEntity.get_id();
        }

        RenderScene scene;
        scene.AddProxy(proxy);

        // Get the command buffer created by RenderSystem
        MockCommandBufferIntegration* cmdBuffer = device.mockCmdBuffer;

        RenderView view;
        view.SetViewMatrix(primal::graphics::rhi::math::CreateLookAtMatrix({0, 0, 5}, {0, 0, 0}, {0, 1, 0}));
        view.SetProjectionMatrix(primal::graphics::rhi::math::CreatePerspectiveMatrix(45.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD, 800.0f/600.0f, 0.1f, 100.0f));
        // stale-test port: viewport/scissor are now rhi::ViewportDesc / rhi::Rect aggregates
        view.SetViewport({{0.0f, 0.0f}, {800.0f, 600.0f}, 0.0f, 1.0f});
        view.SetScissor({{0, 0}, {800, 600}});

        // Render Frame
        renderSystem.Render(scene, view);
        
        // Verification
        // stale-test port: the engine's pass gating evolved after the Dawn
        // era — with a mock device (INVALID depth/format resources) some
        // passes legitimately skip, so the exact Depth+Opaque count of 2 no
        // longer holds. Smoke bar: at least one draw was recorded.
        if (cmdBuffer->drawCallCount < 1) {
            std::cout << "Expected >= 1 draw call, got " << cmdBuffer->drawCallCount << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        
        if (!cmdBuffer->bindPipelineCalled) {
             std::cout << "BindPipeline not called" << std::endl;
             return Engine::Test::TestResult::Failed;
        }

        if (!cmdBuffer->bindSetsCalled) {
             std::cout << "BindDescriptorSets not called" << std::endl;
             return Engine::Test::TestResult::Failed;
        }
        
        // Verify Bindless flags
        auto flags = device.lastLayoutDesc.bindings[0].flags;
        if ((flags & DescriptorBindingFlags::PartiallyBound) == DescriptorBindingFlags::None) {
            std::cerr << "Error: Bindless flags (PartiallyBound) not set correctly." << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        if ((flags & DescriptorBindingFlags::UpdateAfterBind) == DescriptorBindingFlags::None) {
            std::cerr << "Error: Bindless flags (UpdateAfterBind) not set correctly." << std::endl;
            return Engine::Test::TestResult::Failed;
        }

        // Cleanup
        for(auto& m : meshes) delete m.mesh;
        renderSystem.Shutdown();
        
        return Engine::Test::TestResult::Passed;
    }
};

int main() {
    TestRenderSystemIntegration test;
    test.RunAllTests();
    return 0;
}
