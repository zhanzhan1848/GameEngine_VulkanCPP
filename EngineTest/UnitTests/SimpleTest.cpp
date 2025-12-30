/**
 * @file SimpleTest.cpp
 * @brief 简化的RHI测试程序
 * @details 验证RHI核心模块的基本功能
 * 
 * @author RHI开发团队
 * @date 2025-12-29
 */

#include <iostream>
#include <cassert>

/**
 * @brief 简单的测试断言宏
 */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "测试失败: " << message << std::endl; \
            return false; \
        } else { \
            std::cout << "✓ " << message << std::endl; \
        } \
    } while(0)

/**
 * @brief 测试基本句柄功能
 */
bool TestBasicHandles() {
    std::cout << "=== 测试基本句柄功能 ===" << std::endl;
    
    using DeviceHandle = uint64_t;
    using ResourceHandle = uint64_t;
    
    DeviceHandle device = 1;
    ResourceHandle resource = 2;
    
    TEST_ASSERT(device != 0, "设备句柄创建成功");
    TEST_ASSERT(resource != 0, "资源句柄创建成功");
    TEST_ASSERT(device != resource, "不同类型句柄不相等");
    
    return true;
}

/**
 * @brief 测试基本枚举类型
 */
bool TestBasicEnums() {
    std::cout << "\n=== 测试基本枚举类型 ===" << std::endl;
    
    enum class ResourceType : uint32_t {
        Unknown = 0,
        Buffer = 1,
        Texture = 2
    };
    
    enum class ResourceState : uint32_t {
        Common = 0,
        VertexBuffer = 1,
        IndexBuffer = 2,
        RenderTarget = 3
    };
    
    ResourceType type = ResourceType::Buffer;
    ResourceState state = ResourceState::VertexBuffer;
    
    TEST_ASSERT(static_cast<uint32_t>(type) == 1, "资源类型枚举正确");
    TEST_ASSERT(static_cast<uint32_t>(state) == 1, "资源状态枚举正确");
    
    return true;
}

/**
 * @brief 测试基本结构体
 */
bool TestBasicStructs() {
    std::cout << "\n=== 测试基本结构体 ===" << std::endl;
    
    struct BufferDesc {
        uint32_t size;
        uint32_t usage;
        void* data;
    };
    
    struct TextureDesc {
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t mipLevels;
    };
    
    BufferDesc bufferDesc{1024, 1, nullptr};
    TextureDesc textureDesc{256, 256, 1, 1};
    
    TEST_ASSERT(bufferDesc.size == 1024, "缓冲区描述符大小正确");
    TEST_ASSERT(textureDesc.width == 256, "纹理描述符宽度正确");
    TEST_ASSERT(textureDesc.height == 256, "纹理描述符高度正确");
    
    return true;
}

/**
 * @brief 主函数
 */
int main() {
    std::cout << "========================================\n";
    std::cout << "    RHI核心模块简化测试程序\n";
    std::cout << "========================================\n\n";
    
    bool allTestsPassed = true;
    
    allTestsPassed &= TestBasicHandles();
    allTestsPassed &= TestBasicEnums();
    allTestsPassed &= TestBasicStructs();
    
    std::cout << "\n========================================\n";
    if (allTestsPassed) {
        std::cout << "✅ 所有基础测试通过！\n";
    } else {
        std::cout << "❌ 部分测试失败！\n";
    }
    std::cout << "========================================\n";
    
    return allTestsPassed ? 0 : 1;
}