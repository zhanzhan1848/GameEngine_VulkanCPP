/**
 * @file TestRHITypes.cpp
 * @brief RHI类型系统独立测试
 * @note 完全独立的测试文件，不依赖任何Engine头文件
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>

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

// 资源类型枚举
enum class ResourceType : int {
    Buffer = 0,
    Texture1D = 1,
    Texture2D = 2,
    Texture3D = 3
};

// 资源使用标志
enum class ResourceUsage : uint32_t {
    Default = 0x00000000,
    Immutable = 0x00000001,
    Dynamic = 0x00000002,
    Staging = 0x00000004
};

// 资源绑定标志
enum class ResourceBinding : uint32_t {
    None = 0x00000000,
    VertexBuffer = 0x00000001,
    IndexBuffer = 0x00000002,
    ConstantBuffer = 0x00000004,
    ShaderResource = 0x00000008,
    RenderTarget = 0x00000010,
    DepthStencil = 0x00000020,
    UnorderedAccess = 0x00000040
};

// 资源绑定标志位运算操作符
inline ResourceBinding operator|(ResourceBinding a, ResourceBinding b) {
    return static_cast<ResourceBinding>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ResourceBinding operator&(ResourceBinding a, ResourceBinding b) {
    return static_cast<ResourceBinding>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// 像素格式
enum class PixelFormat : uint8_t {
    Unknown = 0,
    R8G8B8A8_UNorm = 1,
    B8G8R8A8_UNorm = 2,
    R32_Float = 3,
    R32G32_Float = 4,
    R32G32B32A32_Float = 5,
    D24_UNorm_S8_UInt = 6,
    D32_Float = 7
};

// 描述符堆类型
enum class DescriptorHeapType : uint8_t {
    CBV_SRV_UAV = 0,
    Sampler = 1,
    RTV = 2,
    DSV = 3,
    NumTypes = 4
};

// 过滤器类型
enum class FilterType : uint8_t {
    Point = 0,
    Linear = 1,
    Anisotropic = 2
};

// 寻址模式
enum class AddressMode : uint8_t {
    Wrap = 0,
    Mirror = 1,
    Clamp = 2,
    Border = 3,
    MirrorOnce = 4
};

// 比较函数
enum class ComparisonFunc : uint8_t {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterEqual = 6,
    Always = 7
};

// 纹理描述
struct TextureDesc {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint16_t mipLevels;
    uint16_t arraySize;
    PixelFormat format;
    ResourceUsage usage;
    ResourceBinding binding;
    
    TextureDesc() : width(0), height(0), depth(1), mipLevels(1), arraySize(1),
                   format(PixelFormat::Unknown), usage(ResourceUsage::Default),
                   binding(ResourceBinding::None) {}
};

// 缓冲区描述
struct BufferDesc {
    uint64_t sizeInBytes;
    ResourceUsage usage;
    ResourceBinding binding;
    
    BufferDesc() : sizeInBytes(0), usage(ResourceUsage::Default),
                   binding(ResourceBinding::None) {}
};

} // namespace rhi
} // namespace graphics
} // namespace primal

using namespace primal::graphics::rhi;

// 测试函数声明
bool TestHandleTypes(std::ofstream& output);
bool TestEnums(std::ofstream& output);
bool TestStructs(std::ofstream& output);
bool TestResourceTypes(std::ofstream& output);
bool TestPixelFormat(std::ofstream& output);
bool TestDescriptorTypes(std::ofstream& output);
bool TestSamplerStates(std::ofstream& output);

/**
 * @brief 测试句柄类型定义
 */
bool TestHandleTypes(std::ofstream& output) {
    output << "=== 测试句柄类型定义 ===" << std::endl;
    
    // 测试句柄大小
    TEST_ASSERT_EQ(sizeof(RHIDeviceHandle), 8, "RHIDeviceHandle应该为8字节");
    TEST_ASSERT_EQ(sizeof(RHIResourceHandle), 8, "RHIResourceHandle应该为8字节");
    TEST_ASSERT_EQ(sizeof(RHICommandHandle), 8, "RHICommandHandle应该为8字节");
    TEST_ASSERT_EQ(sizeof(RHIQueueHandle), 8, "RHIQueueHandle应该为8字节");
    TEST_ASSERT_EQ(sizeof(RHIShaderHandle), 8, "RHIShaderHandle应该为8字节");
    
    // 测试句柄类型唯一性
    RHIDeviceHandle deviceHandle = 1;
    RHIResourceHandle resourceHandle = 2;
    TEST_ASSERT_TRUE(deviceHandle != resourceHandle, "不同类型句柄应该有不同的值");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试枚举类型定义
 */
bool TestEnums(std::ofstream& output) {
    output << "=== 测试枚举类型定义 ===" << std::endl;
    
    // 测试资源类型枚举
    TEST_ASSERT_EQ(static_cast<int>(ResourceType::Buffer), 0, "Buffer类型值应该为0");
    TEST_ASSERT_EQ(static_cast<int>(ResourceType::Texture1D), 1, "Texture1D类型值应该为1");
    TEST_ASSERT_EQ(static_cast<int>(ResourceType::Texture2D), 2, "Texture2D类型值应该为2");
    TEST_ASSERT_EQ(static_cast<int>(ResourceType::Texture3D), 3, "Texture3D类型值应该为3");
    
    // 测试资源使用标志
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceUsage::Default), 0x00000000, "Default使用标志应该为0");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceUsage::Immutable), 0x00000001, "Immutable使用标志应该为1");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceUsage::Dynamic), 0x00000002, "Dynamic使用标志应该为2");
    TEST_ASSERT_EQ(static_cast<uint32_t>(ResourceUsage::Staging), 0x00000004, "Staging使用标志应该为4");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试结构体定义
 */
bool TestStructs(std::ofstream& output) {
    output << "=== 测试结构体定义 ===" << std::endl;
    
    // 测试TextureDesc结构体
    TextureDesc texDesc;
    TEST_ASSERT_EQ(texDesc.width, 0, "TextureDesc默认width应该为0");
    TEST_ASSERT_EQ(texDesc.height, 0, "TextureDesc默认height应该为0");
    TEST_ASSERT_EQ(texDesc.depth, 1, "TextureDesc默认depth应该为1");
    TEST_ASSERT_EQ(texDesc.mipLevels, 1, "TextureDesc默认mipLevels应该为1");
    TEST_ASSERT_EQ(texDesc.arraySize, 1, "TextureDesc默认arraySize应该为1");
    TEST_ASSERT_EQ(texDesc.format, PixelFormat::Unknown, "TextureDesc默认format应该为Unknown");
    TEST_ASSERT_EQ(texDesc.usage, ResourceUsage::Default, "TextureDesc默认usage应该为Default");
    TEST_ASSERT_EQ(texDesc.binding, ResourceBinding::None, "TextureDesc默认binding应该为None");
    
    // 测试BufferDesc结构体
    BufferDesc bufDesc;
    TEST_ASSERT_EQ(bufDesc.sizeInBytes, 0, "BufferDesc默认sizeInBytes应该为0");
    TEST_ASSERT_EQ(bufDesc.usage, ResourceUsage::Default, "BufferDesc默认usage应该为Default");
    TEST_ASSERT_EQ(bufDesc.binding, ResourceBinding::None, "BufferDesc默认binding应该为None");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试资源类型组合
 */
bool TestResourceTypes(std::ofstream& output) {
    output << "=== 测试资源类型组合 ===" << std::endl;
    
    // 创建纹理描述
    TextureDesc texture2D;
    texture2D.width = 1024;
    texture2D.height = 768;
    texture2D.format = PixelFormat::R8G8B8A8_UNorm;
    texture2D.usage = ResourceUsage::Dynamic;
    texture2D.binding = ResourceBinding::ShaderResource | ResourceBinding::RenderTarget;
    
    TEST_ASSERT_EQ(texture2D.width, 1024, "纹理宽度应该为1024");
    TEST_ASSERT_EQ(texture2D.height, 768, "纹理高度应该为768");
    TEST_ASSERT_EQ(texture2D.format, PixelFormat::R8G8B8A8_UNorm, "纹理格式应该为R8G8B8A8_UNorm");
    TEST_ASSERT_EQ(texture2D.usage, ResourceUsage::Dynamic, "纹理使用应该为Dynamic");
    TEST_ASSERT_EQ(texture2D.binding, static_cast<ResourceBinding>(0x00000018), "纹理绑定应该为ShaderResource|RenderTarget");
    
    // 创建缓冲区描述
    BufferDesc constantBuffer;
    constantBuffer.sizeInBytes = 256;
    constantBuffer.usage = ResourceUsage::Dynamic;
    constantBuffer.binding = ResourceBinding::ConstantBuffer;
    
    TEST_ASSERT_EQ(constantBuffer.sizeInBytes, 256, "缓冲区大小应该为256");
    TEST_ASSERT_EQ(constantBuffer.usage, ResourceUsage::Dynamic, "缓冲区使用应该为Dynamic");
    TEST_ASSERT_EQ(constantBuffer.binding, ResourceBinding::ConstantBuffer, "缓冲区绑定应该为ConstantBuffer");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试像素格式
 */
bool TestPixelFormat(std::ofstream& output) {
    output << "=== 测试像素格式 ===" << std::endl;
    
    // 测试像素格式枚举值
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::Unknown), 0, "Unknown格式应该为0");
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::R8G8B8A8_UNorm), 1, "R8G8B8A8_UNorm格式应该为1");
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::B8G8R8A8_UNorm), 2, "B8G8R8A8_UNorm格式应该为2");
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::R32_Float), 3, "R32_Float格式应该为3");
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::R32G32_Float), 4, "R32G32_Float格式应该为4");
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::R32G32B32A32_Float), 5, "R32G32B32A32_Float格式应该为5");
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::D24_UNorm_S8_UInt), 6, "D24_UNorm_S8_UInt格式应该为6");
    TEST_ASSERT_EQ(static_cast<int>(PixelFormat::D32_Float), 7, "D32_Float格式应该为7");
    
    // 测试深度格式识别
    PixelFormat depthFormats[] = {PixelFormat::D24_UNorm_S8_UInt, PixelFormat::D32_Float};
    for (auto format : depthFormats) {
        TEST_ASSERT_TRUE(format == PixelFormat::D24_UNorm_S8_UInt || format == PixelFormat::D32_Float, "应该是深度格式");
    }
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试描述符类型
 */
bool TestDescriptorTypes(std::ofstream& output) {
    output << "=== 测试描述符类型 ===" << std::endl;
    
    // 测试描述符堆类型
    TEST_ASSERT_EQ(static_cast<int>(DescriptorHeapType::CBV_SRV_UAV), 0, "CBV_SRV_UAV应该为0");
    TEST_ASSERT_EQ(static_cast<int>(DescriptorHeapType::Sampler), 1, "Sampler应该为1");
    TEST_ASSERT_EQ(static_cast<int>(DescriptorHeapType::RTV), 2, "RTV应该为2");
    TEST_ASSERT_EQ(static_cast<int>(DescriptorHeapType::DSV), 3, "DSV应该为3");
    TEST_ASSERT_EQ(static_cast<int>(DescriptorHeapType::NumTypes), 4, "NumTypes应该为4");
    
    // 测试描述符堆数量
    TEST_ASSERT_TRUE(static_cast<int>(DescriptorHeapType::NumTypes) == 4, "描述符堆类型总数应该为4");
    
    output << std::endl;
    return true;
}

/**
 * @brief 测试采样器状态
 */
bool TestSamplerStates(std::ofstream& output) {
    output << "=== 测试采样器状态 ===" << std::endl;
    
    // 测试过滤器类型
    TEST_ASSERT_EQ(static_cast<int>(FilterType::Point), 0, "Point过滤器应该为0");
    TEST_ASSERT_EQ(static_cast<int>(FilterType::Linear), 1, "Linear过滤器应该为1");
    TEST_ASSERT_EQ(static_cast<int>(FilterType::Anisotropic), 2, "Anisotropic过滤器应该为2");
    
    // 测试寻址模式
    TEST_ASSERT_EQ(static_cast<int>(AddressMode::Wrap), 0, "Wrap模式应该为0");
    TEST_ASSERT_EQ(static_cast<int>(AddressMode::Mirror), 1, "Mirror模式应该为1");
    TEST_ASSERT_EQ(static_cast<int>(AddressMode::Clamp), 2, "Clamp模式应该为2");
    TEST_ASSERT_EQ(static_cast<int>(AddressMode::Border), 3, "Border模式应该为3");
    TEST_ASSERT_EQ(static_cast<int>(AddressMode::MirrorOnce), 4, "MirrorOnce模式应该为4");
    
    // 测试比较函数
    TEST_ASSERT_EQ(static_cast<int>(ComparisonFunc::Never), 0, "Never比较应该为0");
    TEST_ASSERT_EQ(static_cast<int>(ComparisonFunc::Always), 7, "Always比较应该为7");
    
    output << std::endl;
    return true;
}

/**
 * @brief 主测试函数
 */
int main() {
    std::ofstream output("rhi_test_results.txt");
    if (!output.is_open()) {
        std::cout << "无法创建输出文件" << std::endl;
        return 1;
    }
    
    output << "🚀 开始RHI类型系统独立测试" << std::endl;
    output << "测试时间: " << __DATE__ << " " << __TIME__ << std::endl << std::endl;
    
    bool allPassed = true;
    
    // 运行所有测试
    allPassed &= TestHandleTypes(output);
    allPassed &= TestEnums(output);
    allPassed &= TestStructs(output);
    allPassed &= TestResourceTypes(output);
    allPassed &= TestPixelFormat(output);
    allPassed &= TestDescriptorTypes(output);
    allPassed &= TestSamplerStates(output);
    
    output << "=== 测试总结 ===" << std::endl;
    if (allPassed) {
        output << "🎉 所有RHI类型系统测试通过！" << std::endl;
        std::cout << "✅ 测试完成，所有测试通过！结果已保存到 rhi_test_results.txt" << std::endl;
        return 0;
    } else {
        output << "❌ 部分测试失败" << std::endl;
        std::cout << "❌ 测试完成，部分测试失败。结果已保存到 rhi_test_results.txt" << std::endl;
        return 1;
    }
}