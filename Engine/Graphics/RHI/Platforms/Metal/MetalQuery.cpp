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

MetalQueryPool::MetalQueryPool(MTL::Device* device, MetalQueryType type, uint32_t count)
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
        // Occlusion 查询使用 Buffer 存储结果 (uint64_t per query)
        visibilityBuffer_ = device->newBuffer(count * sizeof(uint64_t), MTL::ResourceStorageModeShared);
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

bool MetalQueryPool::GetResults(uint32_t firstQuery, uint32_t queryCount, void* data, size_t stride) {
    if (firstQuery + queryCount > count_) return false;
    if (!data) return false;

    if (type_ == MetalQueryType::Timestamp && buffer_) {
        NS::Range range = NS::Range(firstQuery, queryCount);
        NS::Data* resultData = buffer_->resolveCounterRange(range);
        if (resultData) {
            // Timestamp 结果通常是 uint64_t
            const void* bytes = resultData->mutableBytes();
            // 这里假设 data 也是 uint64_t 数组，且 stride 是 sizeof(uint64_t)
            // 如果 stride 不同，需要逐个拷贝
            if (stride == sizeof(uint64_t)) {
                memcpy(data, bytes, queryCount * sizeof(uint64_t));
            } else {
                const uint8_t* src = static_cast<const uint8_t*>(bytes);
                uint8_t* dst = static_cast<uint8_t*>(data);
                for (uint32_t i = 0; i < queryCount; ++i) {
                    memcpy(dst, src, sizeof(uint64_t));
                    src += sizeof(uint64_t);
                    dst += stride;
                }
            }
            return true;
        }
    } else if (type_ == MetalQueryType::Occlusion && visibilityBuffer_) {
        // 直接从 Buffer 读取
        const uint8_t* src = static_cast<const uint8_t*>(visibilityBuffer_->contents()) + firstQuery * sizeof(uint64_t);
        uint8_t* dst = static_cast<uint8_t*>(data);
        for (uint32_t i = 0; i < queryCount; ++i) {
            memcpy(dst, src, sizeof(uint64_t));
            src += sizeof(uint64_t);
            dst += stride;
        }
        return true;
    }

    return false;
}

} // namespace primal::graphics::rhi
