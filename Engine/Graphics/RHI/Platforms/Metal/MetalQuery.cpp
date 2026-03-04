/**
 * @file MetalQuery.cpp
 * @brief Metal 查询堆实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalQuery.h"
#include <iostream>

namespace primal::graphics::rhi {

MetalQueryPool::MetalQueryPool(MTL::Device* device, MetalQueryType type, u32 count)
    : type_(type), count_(count) {
    if (!device) return;

    if (type == MetalQueryType::Timestamp) {
        MTL::CounterSampleBufferDescriptor* desc = MTL::CounterSampleBufferDescriptor::alloc()->init();
        desc->setCounterSet(nullptr); // Timestamp 通常不需要特定的 CounterSet，或者需要查找名为 "timestamp" 的 set
        desc->setLabel(NS::String::string("Timestamp Query Pool", NS::UTF8StringEncoding));
        desc->setStorageMode(MTL::StorageModeShared);
        desc->setSampleCount(count);
        
        // 查找支持 Timestamp 的 CounterSet
        NS::Array* counterSets = device->counterSets();
        if (counterSets) {
            for (NS::UInteger i = 0; i < counterSets->count(); ++i) {
                MTL::CounterSet* set = static_cast<MTL::CounterSet*>(counterSets->object(i));
                if (set->name()->isEqualToString(NS::String::string("timestamp", NS::UTF8StringEncoding))) {
                    desc->setCounterSet(set);
                    break;
                }
            }
        }

        NS::Error* error = nullptr;
        buffer_ = device->newCounterSampleBuffer(desc, &error);
        if (error) {
            std::cerr << "Failed to create timestamp query pool: " << error->localizedDescription()->utf8String() << std::endl;
        }
        
        desc->release();
    } else if (type == MetalQueryType::Occlusion) {
        // Occlusion 查询使用 Buffer 存储结果 (u64 per query)
        visibilityBuffer_ = device->newBuffer(count * sizeof(u64), MTL::ResourceStorageModeShared);
        visibilityBuffer_->setLabel(NS::String::string("Occlusion Query Pool", NS::UTF8StringEncoding));
    }
}

MetalQueryPool::~MetalQueryPool() {
    if (buffer_) {
        buffer_->release();
        buffer_ = nullptr;
    }
    if (visibilityBuffer_) {
        visibilityBuffer_->release();
        visibilityBuffer_ = nullptr;
    }
}

bool MetalQueryPool::GetResults(u32 firstQuery, u32 queryCount, void* data, size_t stride) {
    if (firstQuery + queryCount > count_) return false;
    if (!data) return false;

    if (type_ == MetalQueryType::Timestamp && buffer_) {
        NS::Range range = NS::Range(firstQuery, queryCount);
        NS::Data* resultData = buffer_->resolveCounterRange(range);
        if (resultData) {
            // Timestamp 结果通常是 u64
            const void* bytes = resultData->mutableBytes();
            // 这里假设 data 也是 u64 数组，且 stride 是 sizeof(u64)
            // 如果 stride 不同，需要逐个拷贝
            if (stride == sizeof(u64)) {
                memcpy(data, bytes, queryCount * sizeof(u64));
            } else {
                const u8* src = static_cast<const u8*>(bytes);
                u8* dst = static_cast<u8*>(data);
                for (u32 i = 0; i < queryCount; ++i) {
                    memcpy(dst, src, sizeof(u64));
                    src += sizeof(u64);
                    dst += stride;
                }
            }
            return true;
        }
    } else if (type_ == MetalQueryType::Occlusion && visibilityBuffer_) {
        // 直接从 Buffer 读取
        const u8* src = static_cast<const u8*>(visibilityBuffer_->contents()) + firstQuery * sizeof(u64);
        u8* dst = static_cast<u8*>(data);
        for (u32 i = 0; i < queryCount; ++i) {
            memcpy(dst, src, sizeof(u64));
            src += sizeof(u64);
            dst += stride;
        }
        return true;
    }

    return false;
}

} // namespace primal::graphics::rhi
