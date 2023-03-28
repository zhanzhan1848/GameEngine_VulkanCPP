// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
bool vulkan_success(VkResult result);

void copyBuffer(VkDevice device, const VkCommandPool& pool, const VkQueue& queue, VkBuffer& srcBuffer, VkBuffer& dstBuffer, VkDeviceSize size);

uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

void createBuffer(VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
}