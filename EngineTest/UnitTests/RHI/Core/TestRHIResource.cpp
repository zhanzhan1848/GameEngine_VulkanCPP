/**
 * @file TestRHIResource.cpp
 * @brief RHI资源管理独立测试
 * @note 完全独立的测试文件，不依赖任何Engine头文件
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>
#include <cassert>
#include <atomic>
#include <memory>

// 测试结果输出到文件的宏
#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            output << "✗ " << msg << " (expected: " << static_cast<uint64_t>(b) << ", actual: " << static_cast<uint64_t>(a) << ")" << std::endl; \
            return false; \
        } else { \
            output << "✓ " << msg << std::endl; \
        } \
    } while(0)

#define TEST_ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            output << "✗ " << msg << std::endl; \
            return false; \
        } else { \
            output << "✓ " << msg << std::endl; \
        } \
    } while(0)

#define TEST_ASSERT_NULL(ptr, msg) \
    do { \
        if ((ptr) != nullptr) { \
            output << "✗ " << msg << " (expected: nullptr, actual: " << static_cast<const void*>(ptr) << ")" << std::endl; \
            return false; \
        } else { \
            output << "✓ " << msg << std::endl; \
        } \
    } while(0)

#define TEST_ASSERT_NOT_NULL(ptr, msg) \
    do { \
        if ((ptr) == nullptr) { \
            output << "✗ " << msg << " (expected: not null, actual: nullptr)" << std::endl; \
            return false; \
        } else { \
            output << "✓ " << msg << std::endl; \
        } \
    } while(0)

// 模拟RHI类型定义
namespace primal {
namespace graphics {
namespace rhi {

// 基础句柄类型
using RHIResourceHandle = uint64_t;

namespace handles {
    constexpr RHIResourceHandle INVALID_RESOURCE = 0;
}

// 资源类型枚举
enum class ResourceType : uint8_t {
    Unknown = 0,
    Buffer = 1,
    Texture1D = 2,
    Texture2D = 3,
    Texture3D = 4,
    TextureCube = 5,
    Pipeline = 6,
    Shader = 7,
    Sampler = 8,
    DescriptorHeap = 9
};

// GPU内存使用模式
enum class GPUMemoryUsage : uint8_t {
    Unknown = 0,
    GPUOnly = 1,      // 仅GPU访问
    CPUToGPU = 2,     // CPU写入，GPU读取
    GPUToCPU = 3,     // GPU写入，CPU读取
    CPUCopy = 4       // CPU复制优化
};

// 资源状态枚举
enum class ResourceState : uint8_t {
    Unknown = 0,
    Created = 1,
    Allocated = 2,
    PendingUpload = 3,
    Ready = 4,
    InUse = 5,
    PendingDestroy = 6,
    Destroyed = 7
};

// 资源使用标志位
enum class ResourceUsage : uint32_t {
    None = 0x00000000,
    ShaderResource = 0x00000001,
    RenderTarget = 0x00000002,
    DepthStencil = 0x00000004,
    UnorderedAccess = 0x00000008,
    CopySource = 0x00000010,
    CopyDest = 0x00000020,
    ResolveSource = 0x00000040,
    ResolveDest = 0x00000080,
    Present = 0x00000100,
    IndexBuffer = 0x00000200,
    VertexBuffer = 0x00000400,
    ConstantBuffer = 0x00000800,
    IndirectArg = 0x00001000
};

// 支持位运算操作
inline ResourceUsage operator|(ResourceUsage a, ResourceUsage b) {
    return static_cast<ResourceUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ResourceUsage operator&(ResourceUsage a, ResourceUsage b) {
    return static_cast<ResourceUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasUsage(ResourceUsage usage, ResourceUsage flag) {
    return (usage & flag) != ResourceUsage::None;
}

// 资源描述符基类
struct ResourceDesc {
    ResourceType type;
    ResourceUsage usage;
    GPUMemoryUsage memoryUsage;
    uint64_t size;
    const char* name;
    
    ResourceDesc() : type(ResourceType::Unknown), usage(ResourceUsage::None),
                    memoryUsage(GPUMemoryUsage::Unknown), size(0), name(nullptr) {}
    
    ResourceDesc(ResourceType t, ResourceUsage u, GPUMemoryUsage mem, uint64_t sz, const char* n = nullptr)
        : type(t), usage(u), memoryUsage(mem), size(sz), name(n) {}
};

// 资源子资源描述符
struct SubresourceDesc {
    uint32_t mipLevel;
    uint32_t arraySlice;
    uint32_t plane;
    
    SubresourceDesc() : mipLevel(0), arraySlice(0), plane(0) {}
    SubresourceDesc(uint32_t mip, uint32_t array, uint32_t p = 0)
        : mipLevel(mip), arraySlice(array), plane(p) {}
};

// 资源映射描述符
struct ResourceMapDesc {
    void* data;
    uint64_t offset;
    uint64_t size;
    bool isReadback;
    bool isPersistent;
    
    ResourceMapDesc() : data(nullptr), offset(0), size(0), isReadback(false), isPersistent(false) {}
};

// 资源统计信息
struct ResourceStats {
    uint32_t bufferCount;
    uint32_t textureCount;
    uint32_t pipelineCount;
    uint64_t totalMemoryUsage;
    uint64_t bufferMemoryUsage;
    uint64_t textureMemoryUsage;
    
    ResourceStats() : bufferCount(0), textureCount(0), pipelineCount(0),
                     totalMemoryUsage(0), bufferMemoryUsage(0), textureMemoryUsage(0) {}
};

// 模拟设备类
class MockDevice {
public:
    static MockDevice& Instance() {
        static MockDevice instance;
        return instance;
    }
    void AddResource() { resourceCount_++; }
    void RemoveResource() { resourceCount_--; }
    uint32_t GetResourceCount() const { return resourceCount_; }
private:
    uint32_t resourceCount_ = 0;
};

// 模拟RHI资源基类
class MockResource {
private:
    MockDevice& device_;
    ResourceDesc desc_;
    RHIResourceHandle handle_;
    ResourceState state_;
    std::atomic<uint32_t> refCount_;
    void* mappedData_;
    static std::atomic<uint64_t> nextHandle_;
    
public:
    MockResource(MockDevice& device, const ResourceDesc& desc)
        : device_(device), desc_(desc), handle_(++nextHandle_),
          state_(ResourceState::Created), refCount_(1), mappedData_(nullptr) {
        device_.AddResource();
    }
    
    virtual ~MockResource() {
        if (state_ != ResourceState::Destroyed) {
            Destroy();
        }
    }
    
    // 禁用拷贝，支持移动
    MockResource(const MockResource&) = delete;
    MockResource& operator=(const MockResource&) = delete;
    
    MockResource(MockResource&& other) noexcept
        : device_(other.device_), desc_(other.desc_), handle_(other.handle_),
          state_(other.state_), refCount_(other.refCount_.load()), mappedData_(other.mappedData_) {
        other.handle_ = handles::INVALID_RESOURCE;
        other.state_ = ResourceState::Destroyed;
        other.refCount_ = 0;
        other.mappedData_ = nullptr;
    }
    
    MockResource& operator=(MockResource&& other) noexcept {
        if (this != &other) {
            Destroy();
            
            device_ = other.device_;
            desc_ = other.desc_;
            handle_ = other.handle_;
            state_ = other.state_;
            refCount_ = other.refCount_.load();
            mappedData_ = other.mappedData_;
            
            other.handle_ = handles::INVALID_RESOURCE;
            other.state_ = ResourceState::Destroyed;
            other.refCount_ = 0;
            other.mappedData_ = nullptr;
        }
        return *this;
    }
    
    virtual bool Initialize() {
        if (state_ == ResourceState::Created) {
            state_ = ResourceState::Allocated;
            return true;
        }
        return false;
    }
    
    virtual void Destroy() {
        if (state_ != ResourceState::Destroyed) {
            state_ = ResourceState::PendingDestroy;
            
            if (mappedData_) {
                delete[] static_cast<char*>(mappedData_);
                mappedData_ = nullptr;
            }
            
            state_ = ResourceState::Destroyed;
            device_.RemoveResource();
        }
    }
    
    ResourceMapDesc Map(uint64_t offset = 0, uint64_t size = 0, bool isReadback = false) {
        ResourceMapDesc mapDesc;
        
        if (state_ != ResourceState::Allocated && state_ != ResourceState::Ready) {
            return mapDesc;
        }
        
        if (!mappedData_) {
            mappedData_ = new char[desc_.size];
        }
        
        mapDesc.data = static_cast<char*>(mappedData_) + offset;
        mapDesc.offset = offset;
        mapDesc.size = (size == 0) ? desc_.size : size;
        mapDesc.isReadback = isReadback;
        mapDesc.isPersistent = false;
        
        state_ = ResourceState::PendingUpload;
        
        return mapDesc;
    }
    
    void Unmap() {
        if (state_ == ResourceState::PendingUpload) {
            state_ = ResourceState::Ready;
        }
    }
    
    void AddRef() { refCount_++; }
    void Release() {
        if (--refCount_ == 0) {
            delete this;
        }
    }
    
    // 访问器
    RHIResourceHandle GetHandle() const { return handle_; }
    const ResourceDesc& GetDesc() const { return desc_; }
    ResourceState GetState() const { return state_; }
    uint32_t GetRefCount() const { return refCount_.load(); }
    bool IsMapped() const { return mappedData_ != nullptr; }
    
    // 静态工具方法
    static ResourceStats GetGlobalStats() {
        static ResourceStats stats;
        return stats;
    }
};

std::atomic<uint64_t> MockResource::nextHandle_{0};

// 特化的资源类型
class MockBuffer : public MockResource {
public:
    MockBuffer(MockDevice& device, const ResourceDesc& desc) : MockResource(device, desc) {}
    
    bool Initialize() override {
        if (GetDesc().type != ResourceType::Buffer) {
            return false;
        }
        return MockResource::Initialize();
    }
};

class MockTexture : public MockResource {
public:
    MockTexture(MockDevice& device, const ResourceDesc& desc) : MockResource(device, desc) {}
    
    bool Initialize() override {
        if (GetDesc().type != ResourceType::Texture2D) {
            return false;
        }
        return MockResource::Initialize();
    }
};

} // namespace rhi
} // namespace graphics
} // namespace primal

using namespace primal::graphics::rhi;

// 测试函数声明
bool TestResourceDesc(std::ofstream& output);
bool TestResourceStates(std::ofstream& output);
bool TestResourceUsage(std::ofstream& output);
bool TestResourceCreation(std::ofstream& output);
bool TestResourceLifecycle(std::ofstream& output);
bool TestResourceMapping(std::ofstream& output);
bool TestResourceReferenceCounting(std::ofstream& output);
bool TestResourceSubresource(std::ofstream& output);
bool TestResourceStats(std::ofstream& output);

/**
 * @brief 测试资源描述符
 */
bool TestResourceDesc(std::ofstream& output) {
    output << "=== 测试资源描述符 ===" << std::endl;
    
    // 测试默认构造
    ResourceDesc desc1;
    TEST_ASSERT_EQ(desc1.type, ResourceType::Unknown, "默认类型应该为Unknown");
    TEST_ASSERT_EQ(desc1.usage, ResourceUsage::None, "默认用途应该为None");
    TEST_ASSERT_EQ(desc1.memoryUsage, GPUMemoryUsage::Unknown, "默认内存使用应该为Unknown");
    TEST_ASSERT_EQ(desc1.size, 0, "默认大小应该为0");
    TEST_ASSERT_NULL(desc1.name, "默认名称应该为nullptr");
    
    // 测试自定义描述符
    ResourceDesc desc2(ResourceType::Buffer, ResourceUsage::VertexBuffer | ResourceUsage::ShaderResource,
                       GPUMemoryUsage::CPUToGPU, 1024, "TestBuffer");
    TEST_ASSERT_EQ(desc2.type, ResourceType::Buffer, "类型应该为Buffer");
    TEST_ASSERT_TRUE(HasUsage(desc2.usage, ResourceUsage::VertexBuffer), "应该包含顶点缓冲用途");
    TEST_ASSERT_TRUE(HasUsage(desc2.usage, ResourceUsage::ShaderResource), "应该包含着色器资源用途");
    TEST_ASSERT_EQ(desc2.memoryUsage, GPUMemoryUsage::CPUToGPU, "内存使用应该为CPUToGPU");
    TEST_ASSERT_EQ(desc2.size, 1024, "大小应该为1024");
    TEST_ASSERT_NOT_NULL(desc2.name, "名称应该不为nullptr");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源状态
 */
bool TestResourceStates(std::ofstream& output) {
    output << "=== 测试资源状态 ===" << std::endl;
    
    // 测试状态枚举值
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceState::Unknown), 0, "Unknown状态值应该为0");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceState::Created), 1, "Created状态值应该为1");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceState::Allocated), 2, "Allocated状态值应该为2");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceState::Ready), 4, "Ready状态值应该为4");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceState::Destroyed), 7, "Destroyed状态值应该为7");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源用途
 */
bool TestResourceUsage(std::ofstream& output) {
    output << "=== 测试资源用途 ===" << std::endl;
    
    // 测试基本用途
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceUsage::None), 0, "None用途值应该为0");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceUsage::ShaderResource), 1, "ShaderResource用途值应该为1");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceUsage::RenderTarget), 2, "RenderTarget用途值应该为2");
    
    // 测试位运算
    ResourceUsage usage1 = ResourceUsage::VertexBuffer | ResourceUsage::ShaderResource;
    TEST_ASSERT_TRUE(HasUsage(usage1, ResourceUsage::VertexBuffer), "应该包含顶点缓冲用途");
    TEST_ASSERT_TRUE(HasUsage(usage1, ResourceUsage::ShaderResource), "应该包含着色器资源用途");
    TEST_ASSERT_TRUE(!HasUsage(usage1, ResourceUsage::RenderTarget), "不应该包含渲染目标用途");
    
    // 测试组合用途
    ResourceUsage usage2 = ResourceUsage::CopySource | ResourceUsage::CopyDest;
    TEST_ASSERT_TRUE(HasUsage(usage2, ResourceUsage::CopySource), "应该包含复制源用途");
    TEST_ASSERT_TRUE(HasUsage(usage2, ResourceUsage::CopyDest), "应该包含复制目标用途");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源创建
 */
bool TestResourceCreation(std::ofstream& output) {
    output << "=== 测试资源创建 ===" << std::endl;
    
    MockDevice& device = MockDevice::Instance();
    
    // 测试缓冲区创建
    ResourceDesc bufferDesc(ResourceType::Buffer, ResourceUsage::VertexBuffer,
                             GPUMemoryUsage::CPUToGPU, 4096, "TestVertexBuffer");
    MockBuffer* buffer = new MockBuffer(device, bufferDesc);
    TEST_ASSERT_NOT_NULL(buffer, "缓冲区创建应该成功");
    TEST_ASSERT_EQ(buffer->GetDesc().type, ResourceType::Buffer, "缓冲区类型应该正确");
    TEST_ASSERT_EQ(buffer->GetDesc().size, 4096, "缓冲区大小应该正确");
    TEST_ASSERT_EQ(buffer->GetState(), ResourceState::Created, "初始状态应该为Created");
    
    // 测试纹理创建
    ResourceDesc textureDesc(ResourceType::Texture2D, ResourceUsage::RenderTarget,
                              GPUMemoryUsage::GPUOnly, 1024*1024*4, "TestTexture");
    MockTexture* texture = new MockTexture(device, textureDesc);
    TEST_ASSERT_NOT_NULL(texture, "纹理创建应该成功");
    TEST_ASSERT_EQ(texture->GetDesc().type, ResourceType::Texture2D, "纹理类型应该正确");
    TEST_ASSERT_EQ(texture->GetState(), ResourceState::Created, "初始状态应该为Created");
    
    // 清理
    buffer->Release();
    texture->Release();
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源生命周期
 */
bool TestResourceLifecycle(std::ofstream& output) {
    output << "=== 测试资源生命周期 ===" << std::endl;
    
    MockDevice& device = MockDevice::Instance();
    
    ResourceDesc desc(ResourceType::Buffer, ResourceUsage::ConstantBuffer,
                      GPUMemoryUsage::CPUToGPU, 256, "TestLifeCycle");
    MockResource* resource = new MockBuffer(device, desc);
    
    // 测试初始状态
    TEST_ASSERT_EQ(resource->GetState(), ResourceState::Created, "初始状态应该为Created");
    TEST_ASSERT_EQ(resource->GetRefCount(), 1, "初始引用计数应该为1");
    
    // 测试初始化
    bool initResult = resource->Initialize();
    TEST_ASSERT_TRUE(initResult, "初始化应该成功");
    TEST_ASSERT_EQ(resource->GetState(), ResourceState::Allocated, "初始化后状态应该为Allocated");
    
    // 测试重复初始化
    bool reinitResult = resource->Initialize();
    TEST_ASSERT_TRUE(!reinitResult, "重复初始化应该失败");
    
    // 测试销毁
    resource->Destroy();
    TEST_ASSERT_EQ(resource->GetState(), ResourceState::Destroyed, "销毁后状态应该为Destroyed");
    
    // 清理
    resource->Release();
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源映射
 */
bool TestResourceMapping(std::ofstream& output) {
    output << "=== 测试资源映射 ===" << std::endl;
    
    MockDevice& device = MockDevice::Instance();
    
    ResourceDesc desc(ResourceType::Buffer, ResourceUsage::ConstantBuffer,
                      GPUMemoryUsage::CPUToGPU, 1024, "TestMapping");
    MockResource* resource = new MockBuffer(device, desc);
    
    // 测试初始化
    resource->Initialize();
    
    // 测试映射
    TEST_ASSERT_TRUE(!resource->IsMapped(), "初始状态应该未映射");
    
    ResourceMapDesc mapDesc = resource->Map(0, 512);
    TEST_ASSERT_NOT_NULL(mapDesc.data, "映射数据指针应该不为空");
    TEST_ASSERT_EQ(mapDesc.offset, 0, "映射偏移应该为0");
    TEST_ASSERT_EQ(mapDesc.size, 512, "映射大小应该为512");
    TEST_ASSERT_TRUE(!mapDesc.isReadback, "不应该为回读映射");
    TEST_ASSERT_TRUE(!mapDesc.isPersistent, "不应该为持久映射");
    TEST_ASSERT_TRUE(resource->IsMapped(), "映射后应该为已映射状态");
    TEST_ASSERT_EQ(resource->GetState(), ResourceState::PendingUpload, "映射后状态应该为PendingUpload");
    
    // 写入测试数据
    char* data = static_cast<char*>(mapDesc.data);
    strcpy(data, "Test Resource Mapping Data");
    
    // 测试取消映射
    resource->Unmap();
    TEST_ASSERT_EQ(resource->GetState(), ResourceState::Ready, "取消映射后状态应该为Ready");
    
    // 清理
    resource->Release();
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源引用计数
 */
bool TestResourceReferenceCounting(std::ofstream& output) {
    output << "=== 测试资源引用计数 ===" << std::endl;
    
    MockDevice& device = MockDevice::Instance();
    
    ResourceDesc desc(ResourceType::Buffer, ResourceUsage::ShaderResource,
                      GPUMemoryUsage::GPUOnly, 2048, "TestRefCount");
    MockResource* resource = new MockBuffer(device, desc);
    
    // 测试初始引用计数
    TEST_ASSERT_EQ(resource->GetRefCount(), 1, "初始引用计数应该为1");
    
    // 测试增加引用
    resource->AddRef();
    TEST_ASSERT_EQ(resource->GetRefCount(), 2, "增加引用后应该为2");
    
    resource->AddRef();
    resource->AddRef();
    TEST_ASSERT_EQ(resource->GetRefCount(), 4, "多次增加引用后应该为4");
    
    // 测试释放引用
    resource->Release();
    TEST_ASSERT_EQ(resource->GetRefCount(), 3, "释放一次后应该为3");
    
    resource->Release();
    TEST_ASSERT_EQ(resource->GetRefCount(), 2, "释放两次后应该为2");
    
    // 最后释放（不会自动删除，因为还有其他引用）
    resource->Release();
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源子资源
 */
bool TestResourceSubresource(std::ofstream& output) {
    output << "=== 测试资源子资源 ===" << std::endl;
    
    // 测试默认构造
    SubresourceDesc sub1;
    TEST_ASSERT_EQ(sub1.mipLevel, 0, "默认Mip层级应该为0");
    TEST_ASSERT_EQ(sub1.arraySlice, 0, "默认数组切片应该为0");
    TEST_ASSERT_EQ(sub1.plane, 0, "默认平面应该为0");
    
    // 测试自定义子资源
    SubresourceDesc sub2(2, 3, 1);
    TEST_ASSERT_EQ(sub2.mipLevel, 2, "Mip层级应该为2");
    TEST_ASSERT_EQ(sub2.arraySlice, 3, "数组切片应该为3");
    TEST_ASSERT_EQ(sub2.plane, 1, "平面应该为1");
    
    // 测试构造函数重载
    SubresourceDesc sub3(1, 0);
    TEST_ASSERT_EQ(sub3.mipLevel, 1, "Mip层级应该为1");
    TEST_ASSERT_EQ(sub3.arraySlice, 0, "数组切片应该为0");
    TEST_ASSERT_EQ(sub3.plane, 0, "默认平面应该为0");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源统计
 */
bool TestResourceStats(std::ofstream& output) {
    output << "=== 测试资源统计 ===" << std::endl;
    
    ResourceStats stats;
    
    // 测试默认值
    TEST_ASSERT_EQ(stats.bufferCount, 0, "默认缓冲区数量应该为0");
    TEST_ASSERT_EQ(stats.textureCount, 0, "默认纹理数量应该为0");
    TEST_ASSERT_EQ(stats.pipelineCount, 0, "默认管线数量应该为0");
    TEST_ASSERT_EQ(stats.totalMemoryUsage, 0, "默认总内存使用应该为0");
    TEST_ASSERT_EQ(stats.bufferMemoryUsage, 0, "默认缓冲区内存使用应该为0");
    TEST_ASSERT_EQ(stats.textureMemoryUsage, 0, "默认纹理内存使用应该为0");
    
    // 测试全局统计
    ResourceStats globalStats = MockResource::GetGlobalStats();
    TEST_ASSERT_EQ(globalStats.bufferCount, 0, "全局缓冲区数量应该为0");
    TEST_ASSERT_EQ(globalStats.textureCount, 0, "全局纹理数量应该为0");
    
    output << std::endl;
    return true;
}

/**
 * @brief 主测试函数
 */
int main() {
    std::ofstream output("rhi_resource_test_results.txt");
    if (!output.is_open()) {
        std::cout << "无法创建输出文件" << std::endl;
        return 1;
    }
    
    output << "🚀 开始RHI资源管理独立测试" << std::endl;
    output << "测试时间: " << __DATE__ << " " << __TIME__ << std::endl << std::endl;
    
    bool allPassed = true;
    
    // 运行所有测试
    allPassed &= TestResourceDesc(output);
    allPassed &= TestResourceStates(output);
    allPassed &= TestResourceUsage(output);
    allPassed &= TestResourceCreation(output);
    allPassed &= TestResourceLifecycle(output);
    allPassed &= TestResourceMapping(output);
    allPassed &= TestResourceReferenceCounting(output);
    allPassed &= TestResourceSubresource(output);
    allPassed &= TestResourceStats(output);
    
    output << "=== 测试总结 ===" << std::endl;
    if (allPassed) {
        output << "🎉 所有RHI资源管理测试通过！" << std::endl;
        std::cout << "✅ 测试完成，所有测试通过！结果已保存到 rhi_resource_test_results.txt" << std::endl;
        return 0;
    } else {
        output << "❌ 部分测试失败" << std::endl;
        std::cout << "❌ 测试完成，部分测试失败。结果已保存到 rhi_resource_test_results.txt" << std::endl;
        return 1;
    }
}