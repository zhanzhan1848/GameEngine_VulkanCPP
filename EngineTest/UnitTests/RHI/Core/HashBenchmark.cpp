/**
 * @file HashBenchmark.cpp
 * @brief 哈希算法性能基准测试
 * @details 对比 std::hash 与 MurmurHash3 在 RHIBatchRenderer 场景下的性能表现
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#include "../../../../Engine/Utilities/Hash.h"
#include <chrono>
#include <vector>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <iomanip>

// 简化版本，不依赖测试框架

/**
 * @brief 模拟 RenderItemKey 结构（用于独立测试）
 */
struct BenchmarkRenderItemKey {
    u64 pipeline;
    u64 vertexBuffer;
    u64 indexBuffer;
    u64 material;
    u32 materialSlot;
    u32 vertexStride;
    u32 indexFormat;
    u8 renderTarget;
    
    bool operator==(const BenchmarkRenderItemKey& other) const {
        return pipeline == other.pipeline &&
               vertexBuffer == other.vertexBuffer &&
               indexBuffer == other.indexBuffer &&
               material == other.material &&
               materialSlot == other.materialSlot &&
               vertexStride == other.vertexStride &&
               indexFormat == other.indexFormat &&
               renderTarget == other.renderTarget;
    }
};

/**
 * @brief std::hash 实现（原始版本）
 */
struct StdHashBenchmark {
    size_t operator()(const BenchmarkRenderItemKey& key) const {
        size_t hash = 0;
        hash ^= std::hash<u64>{}(key.pipeline) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<u64>{}(key.vertexBuffer) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<u64>{}(key.indexBuffer) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<u64>{}(key.material) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<u32>{}(key.materialSlot) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<u32>{}(key.vertexStride) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<u32>{}(key.indexFormat) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<u8>{}(key.renderTarget) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
    }
};

/**
 * @brief MurmurHash3 实现（新版本）
 */
struct MurmurHash3Benchmark {
    size_t operator()(const BenchmarkRenderItemKey& key) const {
        u32 hashResult = 0;
        static constexpr u32 HASH_SEED = 42;
        
        primal::utl::MurmurHash3_x86_32(&key, sizeof(BenchmarkRenderItemKey), HASH_SEED, &hashResult);
        
        return static_cast<size_t>(hashResult);
    }
};

/**
 * @brief 生成测试数据
 * @param count 数据数量
 * @return 测试键列表
 */
std::vector<BenchmarkRenderItemKey> GenerateTestData(size_t count) {
    std::vector<BenchmarkRenderItemKey> keys;
    keys.reserve(count);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<u64> dist64(1, 1000000);
    std::uniform_int_distribution<u32> dist32(1, 1000);
    std::uniform_int_distribution<u8> dist8(1, 10);
    
    for (size_t i = 0; i < count; ++i) {
        BenchmarkRenderItemKey key;
        key.pipeline = dist64(gen);
        key.vertexBuffer = dist64(gen);
        key.indexBuffer = dist64(gen);
        key.material = dist64(gen);
        key.materialSlot = dist32(gen);
        key.vertexStride = 32 + (dist32(gen) % 3) * 16; // 32, 48, 64
        key.indexFormat = dist32(gen) % 3;
        key.renderTarget = dist8(gen) % 2;
        
        keys.push_back(key);
    }
    
    return keys;
}

/**
 * @brief 执行哈希性能基准测试
 * @param testName 测试名称
 * @param hasher 哈希函数
 * @param keys 测试数据
 * @param iterations 迭代次数
 * @return 耗时（微秒）
 */
template<typename Hasher>
double RunHashBenchmark(const std::string& testName, Hasher hasher, 
                       const std::vector<BenchmarkRenderItemKey>& keys, 
                       int iterations) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& key : keys) {
            volatile size_t hash = hasher(key); // volatile 防止编译器优化
            (void)hash; // 避免未使用变量警告
        }
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    return static_cast<double>(duration.count());
}

/**
 * @brief 测试 unordered_map 性能
 * @param testName 测试名称
 * @param keys 测试数据
 * @return 耗时（微秒）
 */
template<typename Hasher>
double RunMapBenchmark(const std::string& testName, const std::vector<BenchmarkRenderItemKey>& keys) {
    std::unordered_map<BenchmarkRenderItemKey, size_t, Hasher> hashMap;
    hashMap.reserve(keys.size() * 2);
    
    // 插入测试
    auto startTime = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < keys.size(); ++i) {
        hashMap[keys[i]] = i;
    }
    
    // 查找测试
    for (size_t i = 0; i < keys.size(); ++i) {
        volatile auto it = hashMap.find(keys[i]); // volatile 防止编译器优化
        (void)it;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    return static_cast<double>(duration.count());
}

/**
 * @brief 主基准测试函数
 */
void RunHashBenchmark() {
    std::cout << "🚀 开始哈希算法性能基准测试" << std::endl;
    std::cout << "=====================================" << std::endl;
    
    const size_t DATA_COUNT = 10000;
    const int ITERATIONS = 100;
    
    // 生成测试数据
    auto testData = GenerateTestData(DATA_COUNT);
    
    // 哈希计算性能测试
    std::cout << "\n📊 哈希计算性能测试：" << std::endl;
    std::cout << "数据量: " << DATA_COUNT << " 个键" << std::endl;
    std::cout << "迭代次数: " << ITERATIONS << " 次" << std::endl;
    
    double stdHashTime = RunHashBenchmark("std::hash", StdHashBenchmark{}, testData, ITERATIONS);
    double murmurHashTime = RunHashBenchmark("MurmurHash3", MurmurHash3Benchmark{}, testData, ITERATIONS);
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "std::hash 耗时: " << stdHashTime << " 微秒" << std::endl;
    std::cout << "MurmurHash3 耗时: " << murmurHashTime << " 微秒" << std::endl;
    
    double speedup = stdHashTime / murmurHashTime;
    std::cout << "性能提升: " << speedup << "x" << std::endl;
    
    // unordered_map 性能测试
    std::cout << "\n🗺️ unordered_map 性能测试：" << std::endl;
    std::cout << "数据量: " << DATA_COUNT << " 个键值对" << std::endl;
    
    double stdMapTime = RunMapBenchmark<StdHashBenchmark>("std::hash map", testData);
    double murmurMapTime = RunMapBenchmark<MurmurHash3Benchmark>("MurmurHash3 map", testData);
    
    std::cout << "std::hash unordered_map 耗时: " << stdMapTime << " 微秒" << std::endl;
    std::cout << "MurmurHash3 unordered_map 耗时: " << murmurMapTime << " 微秒" << std::endl;
    
    double mapSpeedup = stdMapTime / murmurMapTime;
    std::cout << "性能提升: " << mapSpeedup << "x" << std::endl;
    
    // 哈希分布质量测试
    std::cout << "\n🎯 哈希分布质量测试：" << std::endl;
    
    StdHashBenchmark stdHasher;
    MurmurHash3Benchmark murmurHasher;
    
    std::unordered_set<size_t> stdUniqueHashes;
    std::unordered_set<size_t> murmurUniqueHashes;
    
    for (const auto& key : testData) {
        stdUniqueHashes.insert(stdHasher(key));
        murmurUniqueHashes.insert(murmurHasher(key));
    }
    
    float stdDiversity = static_cast<float>(stdUniqueHashes.size()) / testData.size();
    float murmurDiversity = static_cast<float>(murmurUniqueHashes.size()) / testData.size();
    
    std::cout << "std::hash 哈希多样性: " << (stdDiversity * 100.0f) << "%" << std::endl;
    std::cout << "MurmurHash3 哈希多样性: " << (murmurDiversity * 100.0f) << "%" << std::endl;
    
    // 结论
    std::cout << "\n📋 测试结论：" << std::endl;
    std::cout << "✅ MurmurHash3 在哈希计算性能上 ";
    if (speedup > 1.0) {
        std::cout << "优于 std::hash，提升 " << speedup << "x";
    } else {
        std::cout << "略慢于 std::hash，性能比为 " << speedup;
    }
    std::cout << std::endl;
    
    std::cout << "✅ MurmurHash3 在 unordered_map 场景下 ";
    if (mapSpeedup > 1.0) {
        std::cout << "优于 std::hash，提升 " << mapSpeedup << "x";
    } else {
        std::cout << "略慢于 std::hash，性能比为 " << mapSpeedup;
    }
    std::cout << std::endl;
    
    std::cout << "✅ 哈希分布质量：";
    if (murmurDiversity >= stdDiversity) {
        std::cout << "MurmurHash3 不劣于 std::hash";
    } else {
        std::cout << "std::hash 略优于 MurmurHash3";
    }
    std::cout << std::endl;
    
    std::cout << "🎉 哈希算法性能基准测试完成！" << std::endl;
}

/**
 * @brief 主函数
 */
int main() {
    RunHashBenchmark();
    return 0;
}