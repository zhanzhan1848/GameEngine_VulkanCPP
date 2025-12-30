/**
 * @file TestRHIDevice.cpp
 * @brief RHI设备管理独立测试
 * @note 完全独立的测试文件，不依赖任何Engine头文件
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>
#include <cassert>

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

// 模拟RHI类型定义
namespace primal {
namespace graphics {
namespace rhi {

// 基础句柄类型
using RHIDeviceHandle = uint64_t;
using RHIResourceHandle = uint64_t;
using RHICommandHandle = uint64_t;
using RHIQueueHandle = uint64_t;
using RHIShaderHandle = uint64_t;

// 平台枚举
enum class RHIPlatform : uint8_t {
    Unknown = 0,
    Windows = 1,
    Linux = 2,
    MacOS = 3,
    iOS = 4,
    Android = 5
};

// 设备描述符
struct DeviceDesc {
    RHIPlatform platform;
    bool enableDebug;
    bool enableValidation;
    uint32_t adapterIndex;
    uint32_t maxFramesInFlight;
    
    DeviceDesc() : platform(RHIPlatform::Unknown), enableDebug(false), 
                  enableValidation(false), adapterIndex(0), maxFramesInFlight(3) {}
};

// 设备信息
struct DeviceInfo {
    RHIPlatform platform;
    char deviceName[256];
    char driverVersion[128];
    uint64_t dedicatedVideoMemory;
    uint64_t sharedSystemMemory;
    uint32_t maxTexture1DSize;
    uint32_t maxTexture2DSize;
    uint32_t maxTexture3DSize;
    uint32_t maxTextureCubeSize;
    uint32_t maxRenderTargets;
    uint32_t maxVertexAttributes;
    uint32_t maxSamplerStates;
    uint32_t maxConstantBufferSize;
    bool supportsRayTracing;
    bool supportsMeshShaders;
    bool supportsVariableRateShading;
    
    DeviceInfo() : platform(RHIPlatform::Unknown), deviceName{0}, driverVersion{0},
                  dedicatedVideoMemory(0), sharedSystemMemory(0),
                  maxTexture1DSize(0), maxTexture2DSize(0), maxTexture3DSize(0),
                  maxTextureCubeSize(0), maxRenderTargets(0), maxVertexAttributes(0),
                  maxSamplerStates(0), maxConstantBufferSize(0),
                  supportsRayTracing(false), supportsMeshShaders(false),
                  supportsVariableRateShading(false) {}
};

// 模拟设备实现类
class MockDevice {
private:
    DeviceDesc desc_;
    DeviceInfo info_;
    bool isValid_;
    uint32_t currentFrame_;
    
public:
    explicit MockDevice(const DeviceDesc& desc) : desc_(desc), isValid_(false), currentFrame_(0) {
        // 模拟设备信息
        info_.platform = desc.platform;
        strcpy(info_.deviceName, "Mock RHI Device");
        strcpy(info_.driverVersion, "1.0.0");
        info_.dedicatedVideoMemory = 8ULL * 1024 * 1024 * 1024; // 8GB
        info_.sharedSystemMemory = 16ULL * 1024 * 1024 * 1024; // 16GB
        info_.maxTexture1DSize = 16384;
        info_.maxTexture2DSize = 16384;
        info_.maxTexture3DSize = 2048;
        info_.maxTextureCubeSize = 16384;
        info_.maxRenderTargets = 8;
        info_.maxVertexAttributes = 32;
        info_.maxSamplerStates = 4096;
        info_.maxConstantBufferSize = 65536;
        info_.supportsRayTracing = false;
        info_.supportsMeshShaders = false;
        info_.supportsVariableRateShading = true;
    }
    
    bool Initialize() {
        if (isValid_) return true;
        
        // 模拟初始化过程
        if (desc_.platform == RHIPlatform::Unknown) {
            return false;
        }
        
        isValid_ = true;
        return true;
    }
    
    void Shutdown() {
        if (!isValid_) return;
        isValid_ = false;
    }
    
    void WaitIdle() const {
        assert(isValid_ && "Device not initialized");
        // 模拟等待空闲
    }
    
    void BeginFrame() {
        assert(isValid_ && "Device not initialized");
        currentFrame_ = (currentFrame_ + 1) % desc_.maxFramesInFlight;
    }
    
    void EndFrame() {
        assert(isValid_ && "Device not initialized");
        // 模拟结束帧
    }
    
    bool IsValid() const { return isValid_; }
    const DeviceDesc& GetDesc() const { return desc_; }
    const DeviceInfo& GetInfo() const { return info_; }
    uint32_t GetCurrentFrame() const { return currentFrame_; }
};

} // namespace rhi
} // namespace graphics
} // namespace primal

using namespace primal::graphics::rhi;

// 测试函数声明
bool TestDeviceDesc(std::ofstream& output);
bool TestDeviceInfo(std::ofstream& output);
bool TestDeviceCreation(std::ofstream& output);
bool TestDeviceLifecycle(std::ofstream& output);
bool TestDeviceFrameManagement(std::ofstream& output);
bool TestDeviceCapabilities(std::ofstream& output);
bool TestDeviceValidation(std::ofstream& output);

/**
 * @brief 测试设备描述符
 */
bool TestDeviceDesc(std::ofstream& output) {
    output << "=== 测试设备描述符 ===" << std::endl;
    
    // 测试默认构造
    DeviceDesc desc1;
    TEST_ASSERT_EQ(desc1.platform, RHIPlatform::Unknown, "默认平台应该为Unknown");
    TEST_ASSERT_TRUE(!desc1.enableDebug, "默认调试应该关闭");
    TEST_ASSERT_TRUE(!desc1.enableValidation, "默认验证应该关闭");
    TEST_ASSERT_EQ(desc1.adapterIndex, 0, "默认适配器索引应该为0");
    TEST_ASSERT_EQ(desc1.maxFramesInFlight, 3, "默认最大帧数应该为3");
    
    // 测试自定义描述符
    DeviceDesc desc2;
    desc2.platform = RHIPlatform::MacOS;
    desc2.enableDebug = true;
    desc2.enableValidation = true;
    desc2.adapterIndex = 1;
    desc2.maxFramesInFlight = 2;
    
    TEST_ASSERT_EQ(desc2.platform, RHIPlatform::MacOS, "平台应该为MacOS");
    TEST_ASSERT_TRUE(desc2.enableDebug, "调试应该开启");
    TEST_ASSERT_TRUE(desc2.enableValidation, "验证应该开启");
    TEST_ASSERT_EQ(desc2.adapterIndex, 1, "适配器索引应该为1");
    TEST_ASSERT_EQ(desc2.maxFramesInFlight, 2, "最大帧数应该为2");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试设备信息
 */
bool TestDeviceInfo(std::ofstream& output) {
    output << "=== 测试设备信息 ===" << std::endl;
    
    DeviceInfo info;
    
    // 测试默认值
    TEST_ASSERT_EQ(info.platform, RHIPlatform::Unknown, "默认平台应该为Unknown");
    TEST_ASSERT_TRUE(strlen(info.deviceName) == 0, "默认设备名应该为空");
    TEST_ASSERT_TRUE(strlen(info.driverVersion) == 0, "默认驱动版本应该为空");
    TEST_ASSERT_EQ(info.dedicatedVideoMemory, 0, "默认显存应该为0");
    TEST_ASSERT_EQ(info.sharedSystemMemory, 0, "默认共享内存应该为0");
    TEST_ASSERT_EQ(info.maxTexture2DSize, 0, "默认2D纹理大小应该为0");
    TEST_ASSERT_TRUE(!info.supportsRayTracing, "默认不支持光线追踪");
    TEST_ASSERT_TRUE(!info.supportsMeshShaders, "默认不支持网格着色器");
    TEST_ASSERT_TRUE(!info.supportsVariableRateShading, "默认不支持可变速率着色");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试设备创建
 */
bool TestDeviceCreation(std::ofstream& output) {
    output << "=== 测试设备创建 ===" << std::endl;
    
    // 测试有效设备创建
    DeviceDesc validDesc;
    validDesc.platform = RHIPlatform::MacOS;
    validDesc.enableDebug = true;
    
    MockDevice* validDevice = new MockDevice(validDesc);
    TEST_ASSERT_TRUE(validDevice != nullptr, "有效设备创建应该成功");
    
    // 测试初始化
    bool initResult = validDevice->Initialize();
    TEST_ASSERT_TRUE(initResult, "有效设备初始化应该成功");
    TEST_ASSERT_TRUE(validDevice->IsValid(), "设备应该处于有效状态");
    
    // 验证设备信息
    const DeviceInfo& info = validDevice->GetInfo();
    TEST_ASSERT_EQ(info.platform, RHIPlatform::MacOS, "设备平台应该为MacOS");
    TEST_ASSERT_TRUE(strlen(info.deviceName) > 0, "设备名应该不为空");
    TEST_ASSERT_TRUE(info.dedicatedVideoMemory > 0, "显存应该大于0");
    
    // 清理
    validDevice->Shutdown();
    delete validDevice;
    
    // 测试无效设备创建
    DeviceDesc invalidDesc;
    invalidDesc.platform = RHIPlatform::Unknown; // 无效平台
    
    MockDevice* invalidDevice = new MockDevice(invalidDesc);
    TEST_ASSERT_TRUE(invalidDevice != nullptr, "设备对象创建应该成功");
    
    bool invalidInitResult = invalidDevice->Initialize();
    TEST_ASSERT_TRUE(!invalidInitResult, "无效设备初始化应该失败");
    TEST_ASSERT_TRUE(!invalidDevice->IsValid(), "设备应该处于无效状态");
    
    delete invalidDevice;
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试设备生命周期
 */
bool TestDeviceLifecycle(std::ofstream& output) {
    output << "=== 测试设备生命周期 ===" << std::endl;
    
    DeviceDesc desc;
    desc.platform = RHIPlatform::Windows;
    
    MockDevice device(desc);
    
    // 测试未初始化状态
    TEST_ASSERT_TRUE(!device.IsValid(), "新设备应该无效");
    
    // 测试初始化
    bool initResult = device.Initialize();
    TEST_ASSERT_TRUE(initResult, "初始化应该成功");
    TEST_ASSERT_TRUE(device.IsValid(), "初始化后设备应该有效");
    
    // 测试重复初始化
    bool reinitResult = device.Initialize();
    TEST_ASSERT_TRUE(reinitResult, "重复初始化应该返回true");
    
    // 测试关闭
    device.Shutdown();
    TEST_ASSERT_TRUE(!device.IsValid(), "关闭后设备应该无效");
    
    // 测试重复关闭
    device.Shutdown(); // 应该不会崩溃
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试设备帧管理
 */
bool TestDeviceFrameManagement(std::ofstream& output) {
    output << "=== 测试设备帧管理 ===" << std::endl;
    
    DeviceDesc desc;
    desc.platform = RHIPlatform::Linux;
    desc.maxFramesInFlight = 3;
    
    MockDevice device(desc);
    bool initResult = device.Initialize();
    TEST_ASSERT_TRUE(initResult, "设备初始化应该成功");
    
    // 测试帧管理
    TEST_ASSERT_EQ(device.GetCurrentFrame(), 0, "初始帧应该为0");
    
    device.BeginFrame();
    TEST_ASSERT_EQ(device.GetCurrentFrame(), 1, "第一帧后应该为1");
    
    device.EndFrame();
    
    device.BeginFrame();
    TEST_ASSERT_EQ(device.GetCurrentFrame(), 2, "第二帧后应该为2");
    
    device.EndFrame();
    
    device.BeginFrame();
    TEST_ASSERT_EQ(device.GetCurrentFrame(), 0, "第三帧后应该循环回0");
    
    device.EndFrame();
    
    device.Shutdown();
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试设备能力
 */
bool TestDeviceCapabilities(std::ofstream& output) {
    output << "=== 测试设备能力 ===" << std::endl;
    
    DeviceDesc desc;
    desc.platform = RHIPlatform::MacOS;
    
    MockDevice device(desc);
    bool initResult = device.Initialize();
    TEST_ASSERT_TRUE(initResult, "设备初始化应该成功");
    
    const DeviceInfo& info = device.GetInfo();
    
    // 测试基本能力
    TEST_ASSERT_TRUE(info.maxTexture1DSize >= 1024, "1D纹理最大尺寸应该至少1024");
    TEST_ASSERT_TRUE(info.maxTexture2DSize >= 1024, "2D纹理最大尺寸应该至少1024");
    TEST_ASSERT_TRUE(info.maxTexture3DSize >= 256, "3D纹理最大尺寸应该至少256");
    TEST_ASSERT_TRUE(info.maxRenderTargets >= 4, "最大渲染目标数应该至少4");
    TEST_ASSERT_TRUE(info.maxVertexAttributes >= 16, "最大顶点属性数应该至少16");
    TEST_ASSERT_TRUE(info.maxSamplerStates >= 1024, "最大采样器状态数应该至少1024");
    TEST_ASSERT_TRUE(info.maxConstantBufferSize >= 4096, "最大常量缓冲区大小应该至少4096");
    
    // 测试内存信息
    TEST_ASSERT_TRUE(info.dedicatedVideoMemory >= 1024*1024*1024, "显存应该至少1GB");
    TEST_ASSERT_TRUE(info.sharedSystemMemory >= 1024*1024*1024, "共享内存应该至少1GB");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试设备验证
 */
bool TestDeviceValidation(std::ofstream& output) {
    output << "=== 测试设备验证 ===" << std::endl;
    
    DeviceDesc desc;
    desc.platform = RHIPlatform::Windows;
    desc.enableValidation = true;
    
    MockDevice device(desc);
    bool initResult = device.Initialize();
    TEST_ASSERT_TRUE(initResult, "设备初始化应该成功");
    
    // 测试WaitIdle（在有效状态下）
    device.WaitIdle(); // 应该不会崩溃
    
    // 测试BeginFrame和EndFrame（在有效状态下）
    device.BeginFrame();
    device.EndFrame();
    
    // 关闭设备后测试
    device.Shutdown();
    
    output << std::endl;
    return true;
}

/**
 * @brief 主测试函数
 */
int main() {
    std::ofstream output("rhi_device_test_results.txt");
    if (!output.is_open()) {
        std::cout << "无法创建输出文件" << std::endl;
        return 1;
    }
    
    output << "🚀 开始RHI设备管理独立测试" << std::endl;
    output << "测试时间: " << __DATE__ << " " << __TIME__ << std::endl << std::endl;
    
    bool allPassed = true;
    
    // 运行所有测试
    allPassed &= TestDeviceDesc(output);
    allPassed &= TestDeviceInfo(output);
    allPassed &= TestDeviceCreation(output);
    allPassed &= TestDeviceLifecycle(output);
    allPassed &= TestDeviceFrameManagement(output);
    allPassed &= TestDeviceCapabilities(output);
    allPassed &= TestDeviceValidation(output);
    
    output << "=== 测试总结 ===" << std::endl;
    if (allPassed) {
        output << "🎉 所有RHI设备管理测试通过！" << std::endl;
        std::cout << "✅ 测试完成，所有测试通过！结果已保存到 rhi_device_test_results.txt" << std::endl;
        return 0;
    } else {
        output << "❌ 部分测试失败" << std::endl;
        std::cout << "❌ 测试完成，部分测试失败。结果已保存到 rhi_device_test_results.txt" << std::endl;
        return 1;
    }
}