/**
 * @file RHIBatchRenderer.cpp
 * @brief RHI智能批渲染优化器实现
 * @details 实现渲染项智能分类、动态批处理优化和GPU实例化功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#include "RHIBatchRenderer.h"
#include "RHIDevice.h"
#include "RHICommand.h"
#include "RHIResource.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace primal::graphics::rhi {

// === Frustum 类实现 ===

void Frustum::FromMatrix(const math::m4x4& viewProjection) {
    // 从视图投影矩阵提取6个裁剪平面
    // 左平面: row4 + row1
    planes[0] = math::v4{
        viewProjection.columns[3][0] + viewProjection.columns[0][0],
        viewProjection.columns[3][1] + viewProjection.columns[0][1],
        viewProjection.columns[3][2] + viewProjection.columns[0][2],
        viewProjection.columns[3][3] + viewProjection.columns[0][3]
    };
    // 右平面: row4 - row1
    planes[1] = math::v4{
        viewProjection.columns[3][0] - viewProjection.columns[0][0],
        viewProjection.columns[3][1] - viewProjection.columns[0][1],
        viewProjection.columns[3][2] - viewProjection.columns[0][2],
        viewProjection.columns[3][3] - viewProjection.columns[0][3]
    };
    // 上平面: row4 - row2
    planes[2] = math::v4{
        viewProjection.columns[3][0] - viewProjection.columns[1][0],
        viewProjection.columns[3][1] - viewProjection.columns[1][1],
        viewProjection.columns[3][2] - viewProjection.columns[1][2],
        viewProjection.columns[3][3] - viewProjection.columns[1][3]
    };
    // 下平面: row4 + row2
    planes[3] = math::v4{
        viewProjection.columns[3][0] + viewProjection.columns[1][0],
        viewProjection.columns[3][1] + viewProjection.columns[1][1],
        viewProjection.columns[3][2] + viewProjection.columns[1][2],
        viewProjection.columns[3][3] + viewProjection.columns[1][3]
    };
    // 近平面: row4 + row3
    planes[4] = math::v4{
        viewProjection.columns[3][0] + viewProjection.columns[2][0],
        viewProjection.columns[3][1] + viewProjection.columns[2][1],
        viewProjection.columns[3][2] + viewProjection.columns[2][2],
        viewProjection.columns[3][3] + viewProjection.columns[2][3]
    };
    // 远平面: row4 - row3
    planes[5] = math::v4{
        viewProjection.columns[3][0] - viewProjection.columns[2][0],
        viewProjection.columns[3][1] - viewProjection.columns[2][1],
        viewProjection.columns[3][2] - viewProjection.columns[2][2],
        viewProjection.columns[3][3] - viewProjection.columns[2][3]
    };
    
    // 归一化平面方程
    for (int i = 0; i < 6; ++i) {
        f32 length = std::sqrt(planes[i].x * planes[i].x + 
                               planes[i].y * planes[i].y + 
                               planes[i].z * planes[i].z);
        if (length > 0.0f) {
            planes[i] /= length;
        }
    }
}

bool Frustum::IsSphereVisible(const math::v3& center, f32 radius) const {
    for (int i = 0; i < 6; ++i) {
        const math::v4& plane = planes[i];
        f32 distance = plane.x * center.x + plane.y * center.y + 
                       plane.z * center.z + plane.w;
        if (distance < -radius) {
            return false;
        }
    }
    return true;
}

bool Frustum::IsBoxVisible(const math::v3& min, const math::v3& max) const {
    // 检查包围盒的8个顶点
    math::v3 corners[8] = {
        math::v3{min.x, min.y, min.z},
        math::v3{max.x, min.y, min.z},
        math::v3{min.x, max.y, min.z},
        math::v3{max.x, max.y, min.z},
        math::v3{min.x, min.y, max.z},
        math::v3{max.x, min.y, max.z},
        math::v3{min.x, max.y, max.z},
        math::v3{max.x, max.y, max.z}
    };
    
    for (int i = 0; i < 6; ++i) {
        const math::v4& plane = planes[i];
        bool allOutside = true;
        
        for (int j = 0; j < 8; ++j) {
            f32 distance = plane.x * corners[j].x + plane.y * corners[j].y + 
                           plane.z * corners[j].z + plane.w;
            if (distance >= 0.0f) {
                allOutside = false;
                break;
            }
        }
        
        if (allOutside) {
            return false;
        }
    }
    return true;
}

// === RHIBatchRenderer 类实现 ===

RHIBatchRenderer::RHIBatchRenderer(RHIDeviceBase& device, const BatchConfig& config)
    : device_(device), config_(config), isInitialized_(false), frameCounter_(0) {
    
    // 预分配内存
    renderItems_.reserve(config_.maxBatchSize * 4);
    batches_.reserve(config_.maxBatchSize);
    batchMap_.reserve(config_.maxBatchSize);
    
    // 预分配临时缓冲区
    tempIndices_.reserve(config_.maxIndicesPerBatch);
    tempInstanceData_.reserve(config_.maxInstancesPerBatch);
    tempBatches_.reserve(config_.maxBatchSize);
}

RHIBatchRenderer::~RHIBatchRenderer() {
    if (isInitialized_) {
        Shutdown();
    }
}

bool RHIBatchRenderer::Initialize() {
    if (isInitialized_) {
        return true;
    }
    
    // 初始化统计信息
    ResetStats();
    
    // 预分配内存池
    renderItems_.reserve(config_.maxBatchSize * 8);
    batches_.reserve(config_.maxBatchSize * 2);
    batchMap_.reserve(config_.maxBatchSize * 2);
    
    tempIndices_.reserve(config_.maxIndicesPerBatch * 2);
    tempInstanceData_.reserve(config_.maxInstancesPerBatch * 2);
    tempBatches_.reserve(config_.maxBatchSize * 2);
    
    isInitialized_ = true;
    return true;
}

void RHIBatchRenderer::Shutdown() {
    if (!isInitialized_) {
        return;
    }
    
    // 清空所有数据
    ClearRenderItems();
    batchMap_.clear();
    batches_.clear();
    tempIndices_.clear();
    tempInstanceData_.clear();
    tempBatches_.clear();
    
    isInitialized_ = false;
}

void RHIBatchRenderer::AddRenderItem(const RenderItem& item) {
    if (!isInitialized_) {
        return;
    }
    
    renderItems_.push_back(item);
}

void RHIBatchRenderer::AddRenderItems(const RenderItem* items, u32 count) {
    if (!isInitialized_ || !items || count == 0) {
        return;
    }
    
    renderItems_.insert(renderItems_.end(), items, items + count);
}

void RHIBatchRenderer::ClearRenderItems() {
    renderItems_.clear();
    batchMap_.clear();
    batches_.clear();
    frameCounter_++;
}

u32 RHIBatchRenderer::ProcessBatches(const math::m4x4& viewMatrix, 
                                     const math::m4x4& projectionMatrix) {
    if (!isInitialized_ || renderItems_.empty()) {
        return 0;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // 保存当前矩阵
    viewMatrix_ = viewMatrix;
    projectionMatrix_ = projectionMatrix;
    
    // 计算当前视锥
    math::m4x4 viewProjection = viewMatrix * projectionMatrix;
    currentFrustum_.FromMatrix(viewProjection);
    
    // 清空之前的批次
    batchMap_.clear();
    batches_.clear();
    
    // 分类渲染项到批次
    ClassifyRenderItems();
    
    // 执行视锥剔除（如果启用）
    if (config_.enableFrustumCulling) {
        PerformFrustumCulling(currentFrustum_);
    }
    
    // 优化批次（合并、分割等）
    OptimizeBatches();
    
    // 执行深度排序（如果启用）
    if (config_.enableDepthSorting) {
        PerformDepthSorting();
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    stats_.totalProcessingTime = static_cast<double>(duration.count()) / 1000.0; // 转换为毫秒
    
    // 更新统计信息
    UpdateStats();
    
    return static_cast<u32>(batches_.size());
}

u32 RHIBatchRenderer::SubmitBatches(RHICommandBuffer* commandBuffer) {
    if (!isInitialized_ || !commandBuffer || batches_.empty()) {
        return 0;
    }
    
    u32 submittedCount = 0;
    
    for (const auto& batch : batches_) {
        if (batch.items.empty()) {
            continue;
        }
        
        SubmitBatch(commandBuffer, batch);
        submittedCount++;
    }
    
    return submittedCount;
}

void RHIBatchRenderer::ClassifyRenderItems() {
    for (const auto& item : renderItems_) {
        // 查找或创建对应的批次
        auto it = batchMap_.find(item.key);
        if (it == batchMap_.end()) {
            // 创建新批次
            auto batch = std::make_unique<RenderBatch>();
            batch->key = item.key;
            batch->AddItem(item);
            batchMap_[item.key] = std::move(batch);
        } else {
            // 添加到现有批次
            it->second->AddItem(item);
        }
    }
    
    // 将批次转换为向量
    batches_.reserve(batchMap_.size());
    for (auto& pair : batchMap_) {
        batches_.push_back(std::move(*pair.second));
    }
}

void RHIBatchRenderer::OptimizeBatches() {
    tempBatches_.clear();
    
    // 检查每个批次是否需要优化
    for (auto& batch : batches_) {
        if (batch.items.empty()) {
            continue;
        }
        
        // 检查是否超过最大批次大小
        if (batch.items.size() > config_.maxBatchSize ||
            batch.totalVertices > config_.maxVerticesPerBatch ||
            batch.totalIndices > config_.maxIndicesPerBatch) {
            
            // 分割批次
            u32 splitCount = SplitBatch(batch);
            stats_.mergedBatches += splitCount - 1;
        } else if (config_.enableInstancing && batch.items.size() > 1) {
            // 检查是否可以创建实例化批次
            bool canInstance = true;
            for (const auto& item : batch.items) {
                if (!item.isInstanced) {
                    canInstance = false;
                    break;
                }
            }
            
            if (canInstance) {
                // 创建实例化批次
                auto instancedBatches = CreateInstancedBatches(batch);
                for (auto& instancedBatch : instancedBatches) {
                    tempBatches_.push_back(std::move(instancedBatch));
                    stats_.instancedBatches++;
                }
                continue; // 跳过原始批次
            }
        }
        
        tempBatches_.push_back(std::move(batch));
    }
    
    // 尝试合并小批次
    for (size_t i = 0; i < tempBatches_.size(); ++i) {
        if (tempBatches_[i].items.empty()) {
            continue;
        }
        
        for (size_t j = i + 1; j < tempBatches_.size(); ++j) {
            if (tempBatches_[j].items.empty()) {
                continue;
            }
            
            if (CanMergeBatches(tempBatches_[i], tempBatches_[j])) {
                MergeBatches(tempBatches_[i], tempBatches_[j]);
                tempBatches_[j].Clear(); // 标记为空，稍后清理
                stats_.mergedBatches++;
            }
        }
    }
    
    // 清理空批次并更新批次列表
    batches_.clear();
    for (auto& batch : tempBatches_) {
        if (!batch.items.empty()) {
            batches_.push_back(std::move(batch));
        }
    }
}

void RHIBatchRenderer::PerformFrustumCulling(const Frustum& frustum) {
    u32 culledCount = 0;
    
    for (auto& batch : batches_) {
        auto it = batch.items.begin();
        while (it != batch.items.end()) {
            // 简单的球体剔除
            math::v3 center{it->worldMatrix.columns[3].x, it->worldMatrix.columns[3].y, it->worldMatrix.columns[3].z};
            f32 radius = batch.boundingRadius; // 使用批次的包围半径
            
            if (!frustum.IsSphereVisible(center, radius)) {
                it = batch.items.erase(it);
                culledCount++;
            } else {
                ++it;
            }
        }
        
        // 重新计算批次统计
        batch.totalVertices = 0;
        batch.totalIndices = 0;
        batch.totalInstances = 0;
        for (const auto& item : batch.items) {
            batch.totalVertices += item.vertexCount;
            batch.totalIndices += item.indexCount;
            batch.totalInstances += item.instanceCount;
        }
    }
    
    stats_.culledItems += culledCount;
}

void RHIBatchRenderer::PerformDepthSorting() {
    // 对每个批次按深度排序
    for (auto& batch : batches_) {
        if (batch.needsSorting) {
            std::sort(batch.items.begin(), batch.items.end(),
                      [](const RenderItem& a, const RenderItem& b) {
                          // 从前到后排序（不透明物体）
                          return a.depth < b.depth;
                      });
            batch.needsSorting = false;
        }
    }
    
    // 对批次列表按深度排序（从前到后）
    std::sort(batches_.begin(), batches_.end(),
              [](const RenderBatch& a, const RenderBatch& b) {
                  f32 aDepth = a.items.empty() ? 0.0f : a.items[0].depth;
                  f32 bDepth = b.items.empty() ? 0.0f : b.items[0].depth;
                  return aDepth < bDepth;
              });
}

bool RHIBatchRenderer::CanMergeBatches(const RenderBatch& batch1, const RenderBatch& batch2) const {
    // 检查批次键是否相同
    if (!(batch1.key == batch2.key)) {
        return false;
    }
    
    // 检查合并后是否超过限制
    u32 totalItems = batch1.items.size() + batch2.items.size();
    u32 totalVertices = batch1.totalVertices + batch2.totalVertices;
    u32 totalIndices = batch1.totalIndices + batch2.totalIndices;
    u32 totalInstances = batch1.totalInstances + batch2.totalInstances;
    
    return (totalItems <= config_.maxBatchSize &&
            totalVertices <= config_.maxVerticesPerBatch &&
            totalIndices <= config_.maxIndicesPerBatch &&
            totalInstances <= config_.maxInstancesPerBatch);
}

void RHIBatchRenderer::MergeBatches(RenderBatch& target, const RenderBatch& source) {
    // 合并渲染项
    target.items.insert(target.items.end(), source.items.begin(), source.items.end());
    
    // 更新统计信息
    target.totalVertices += source.totalVertices;
    target.totalIndices += source.totalIndices;
    target.totalInstances += source.totalInstances;
    target.boundingRadius = std::max(target.boundingRadius, source.boundingRadius);
    target.needsSorting = true;
}

u32 RHIBatchRenderer::SplitBatch(RenderBatch& batch) {
    if (batch.items.size() <= 1) {
        return 1;
    }
    
    std::vector<RenderBatch> splitBatches;
    RenderBatch currentBatch;
    currentBatch.key = batch.key;
    
    u32 currentVertices = 0;
    u32 currentIndices = 0;
    u32 currentInstances = 0;
    
    for (const auto& item : batch.items) {
        // 检查添加当前项目是否会超过限制
        bool wouldExceed = (currentBatch.items.size() + 1 > config_.maxBatchSize) ||
                          (currentVertices + item.vertexCount > config_.maxVerticesPerBatch) ||
                          (currentIndices + item.indexCount > config_.maxIndicesPerBatch) ||
                          (currentInstances + item.instanceCount > config_.maxInstancesPerBatch);
        
        if (wouldExceed && !currentBatch.items.empty()) {
            // 保存当前批次并开始新批次
            splitBatches.push_back(std::move(currentBatch));
            currentBatch = RenderBatch();
            currentBatch.key = batch.key;
            currentVertices = 0;
            currentIndices = 0;
            currentInstances = 0;
        }
        
        // 添加项目到当前批次
        currentBatch.AddItem(item);
        currentVertices += item.vertexCount;
        currentIndices += item.indexCount;
        currentInstances += item.instanceCount;
    }
    
    // 添加最后一个批次
    if (!currentBatch.items.empty()) {
        splitBatches.push_back(std::move(currentBatch));
    }
    
    // 更新原始批次为第一个分割批次
    if (!splitBatches.empty()) {
        batch = std::move(splitBatches[0]);
        
        // 添加其余批次到临时列表
        for (size_t i = 1; i < splitBatches.size(); ++i) {
            tempBatches_.push_back(std::move(splitBatches[i]));
        }
    }
    
    return static_cast<u32>(splitBatches.size());
}

std::vector<RenderBatch> RHIBatchRenderer::CreateInstancedBatches(const RenderBatch& batch) {
    std::vector<RenderBatch> instancedBatches;
    
    if (batch.items.empty()) {
        return instancedBatches;
    }
    
    // 按最大实例数分割
    const u32 maxInstancesPerBatch = config_.maxInstancesPerBatch;
    u32 totalInstances = 0;
    
    RenderBatch currentInstancedBatch;
    currentInstancedBatch.key = batch.key;
    currentInstancedBatch.isInstancedBatch = true;
    
    for (const auto& item : batch.items) {
        if (totalInstances + item.instanceCount > maxInstancesPerBatch) {
            // 创建新批次
            if (!currentInstancedBatch.items.empty()) {
                instancedBatches.push_back(std::move(currentInstancedBatch));
                currentInstancedBatch = RenderBatch();
                currentInstancedBatch.key = batch.key;
                currentInstancedBatch.isInstancedBatch = true;
                totalInstances = 0;
            }
        }
        
        currentInstancedBatch.AddItem(item);
        totalInstances += item.instanceCount;
    }
    
    // 添加最后一个批次
    if (!currentInstancedBatch.items.empty()) {
        instancedBatches.push_back(std::move(currentInstancedBatch));
    }
    
    return instancedBatches;
}

void RHIBatchRenderer::SubmitBatch(RHICommandBuffer* commandBuffer, const RenderBatch& batch) {
    if (!commandBuffer || batch.items.empty()) {
        return;
    }
    
    // 绑定图形管线
    commandBuffer->BindGraphicsPipeline(batch.key.pipeline);
    
    // 绑定顶点缓冲区
    if (batch.key.vertexBuffer != handles::INVALID_RESOURCE) {
        commandBuffer->BindVertexBuffers(0, 1, &batch.key.vertexBuffer, nullptr);
    }
    
    // 绑定索引缓冲区
    if (batch.key.indexBuffer != handles::INVALID_RESOURCE) {
        commandBuffer->BindIndexBuffer(batch.key.indexBuffer, batch.key.indexFormat, 0);
    }
    
    // 绑定材质资源
    if (batch.key.material != handles::INVALID_RESOURCE) {
        // 这里需要根据具体的资源绑定方式来实现
        // commandBuffer->BindDescriptorSets(batch.key.materialSlot, 1, &batch.key.material, nullptr, nullptr);
    }
    
    if (batch.isInstancedBatch) {
        SubmitInstancedBatch(commandBuffer, batch);
    } else {
        // 普通批次：逐个提交渲染项
        for (const auto& item : batch.items) {
            if (item.indexCount > 0) {
                // 索引绘制
                commandBuffer->DrawIndexed(item.indexCount, item.startIndex, 
                                          item.startVertex, item.instanceCount, item.startInstance);
            } else {
                // 非索引绘制
                commandBuffer->Draw(item.vertexCount, item.startVertex, 
                                   item.instanceCount, item.startInstance);
            }
        }
    }
}

void RHIBatchRenderer::SubmitInstancedBatch(RHICommandBuffer* commandBuffer, const RenderBatch& batch) {
    // 准备实例数据
    tempInstanceData_.clear();
    tempInstanceData_.reserve(batch.totalInstances);
    
    for (const auto& item : batch.items) {
        // 每个实例的变换矩阵（这里简化处理，实际可能需要更多数据）
        for (u32 i = 0; i < item.instanceCount; ++i) {
            tempInstanceData_.push_back(item.worldMatrix);
        }
    }
    
    // 创建或更新实例缓冲区（这里需要与RHI设备交互）
    // ResourceHandle instanceBuffer = device_.CreateInstanceBuffer(tempInstanceData_);
    
    // 绑定实例缓冲区
    // commandBuffer->BindVertexBuffers(1, 1, &instanceBuffer, nullptr);
    
    // 提交实例化绘制
    u32 startInstance = 0;
    for (const auto& item : batch.items) {
        if (item.indexCount > 0) {
            commandBuffer->DrawIndexed(item.indexCount, item.startIndex,
                                      item.startVertex, item.instanceCount, startInstance);
        } else {
            commandBuffer->Draw(item.vertexCount, item.startVertex,
                              item.instanceCount, startInstance);
        }
        startInstance += item.instanceCount;
    }
}

void RHIBatchRenderer::UpdateStats() {
    // 重置统计信息
    stats_.totalRenderItems = static_cast<u32>(renderItems_.size());
    stats_.totalBatches = static_cast<u32>(batches_.size());
    
    // 计算平均批次大小
    if (stats_.totalBatches > 0) {
        stats_.averageBatchSize = static_cast<f32>(stats_.totalRenderItems) / stats_.totalBatches;
    } else {
        stats_.averageBatchSize = 0.0f;
    }
    
    // 计算批处理效率
    if (stats_.totalRenderItems > 0) {
        u32 potentialBatches = stats_.totalRenderItems; // 每个渲染项一个批次
        stats_.batchingEfficiency = 1.0f - (static_cast<f32>(stats_.totalBatches) / potentialBatches);
    } else {
        stats_.batchingEfficiency = 0.0f;
    }
    
    // 计算内存使用量
    stats_.memoryUsage = 
        renderItems_.size() * sizeof(RenderItem) +
        batches_.size() * sizeof(RenderBatch) +
        batchMap_.size() * sizeof(std::pair<const RenderItemKey, std::unique_ptr<RenderBatch>>);
}

void RHIBatchRenderer::ResetStats() {
    stats_ = BatchStats();
    frameCounter_ = 0;
}

void RHIBatchRenderer::PrintStats() const {
    printf("=== RHI批渲染器统计信息 ===\n");
    printf("总渲染项数: %u\n", stats_.totalRenderItems);
    printf("总批次数: %u\n", stats_.totalBatches);
    printf("实例化批次数: %u\n", stats_.instancedBatches);
    printf("合并的批次数: %u\n", stats_.mergedBatches);
    printf("剔除的渲染项数: %u\n", stats_.culledItems);
    printf("平均批次大小: %.2f\n", stats_.averageBatchSize);
    printf("批处理效率: %.2f%%\n", stats_.batchingEfficiency * 100.0f);
    printf("总处理时间: %.3f ms\n", stats_.totalProcessingTime);
    printf("内存使用量: %llu bytes\n", static_cast<unsigned long long>(stats_.memoryUsage));
    printf("帧计数器: %u\n", frameCounter_);
    printf("============================\n");
}

bool RHIBatchRenderer::ValidateBatches() const {
    // 验证批次完整性
    for (const auto& batch : batches_) {
        if (batch.items.empty()) {
            continue; // 空批次是允许的
        }
        
        // 检查批次键的一致性
        for (const auto& item : batch.items) {
            if (!(item.key == batch.key)) {
                return false; // 批次键不一致
            }
        }
        
        // 检查统计信息的正确性
        u32 actualVertices = 0, actualIndices = 0, actualInstances = 0;
        for (const auto& item : batch.items) {
            actualVertices += item.vertexCount;
            actualIndices += item.indexCount;
            actualInstances += item.instanceCount;
        }
        
        if (actualVertices != batch.totalVertices ||
            actualIndices != batch.totalIndices ||
            actualInstances != batch.totalInstances) {
            return false; // 统计信息不正确
        }
    }
    
    return true;
}

} // namespace primal::graphics::rhi