// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
bool vulkan_success(VkResult result);

void copyBuffer(VkDevice device, u32 index, const VkCommandPool& pool, const VkQueue& queue, VkBuffer& srcBuffer, VkBuffer& dstBuffer, VkDeviceSize size);

void createBuffer(VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);

VkCommandBuffer beginSingleCommand(VkDevice device, const VkCommandPool& pool);

void endSingleCommand(VkDevice device, u32 index, const VkCommandPool& pool, VkCommandBuffer commandBuffer);
}