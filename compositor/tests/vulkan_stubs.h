// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Vulkan entry points that the fake devices of upload_test.c and
// frame_path_test.c stub alike. Included after the renderer's sources, which
// declare what these replace; each test defines the rest (images, memory,
// submission) its own way.

// The device's own error handling, as vulkan.c does it.
bool vela_vulkan_result(struct vela_vulkan *vk, VkResult result, const char *operation)
{
    vela_device_loss_mark(&vk->loss, (int32_t)result, operation);
    return result == VK_SUCCESS && !vela_vulkan_unusable(vk);
}

void vela_vulkan_abort(struct vela_vulkan *vk, const char *operation)
{
    vk->terminal_error = true;
}

uint32_t vela_vulkan_memory_type(const struct vela_vulkan *vk, uint32_t bits, VkMemoryPropertyFlags flags)
{
    return 0;
}

bool vela_vulkan_supports_dmabuf(
    const struct vela_vulkan *vk, VkFormat format, uint64_t modifier, VkImageUsageFlags usage)
{
    return true;
}

// Commands that change nothing a test looks at.
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd)
{
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo *dependency)
{
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info)
{
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cmd)
{
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
    VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport *viewports)
{
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
    VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D *scissors)
{
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline)
{
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
    uint32_t offset, uint32_t size, const void *values)
{
}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(
    VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
}

// Objects with nothing to free.
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(
    VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *allocator)
{
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(
    VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *allocator)
{
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator)
{
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
    VkDevice device, VkShaderModule module, const VkAllocationCallbacks *allocator)
{
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks *allocator)
{
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks *allocator)
{
}
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *allocator)
{
}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks *allocator)
{
}

// Timestamps and the renderer's own creation (init()): never reached.
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first, uint32_t count)
{
    abort();
}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(
    VkCommandBuffer cmd, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query)
{
    abort();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *allocator, VkCommandPool *pool)
{
    abort();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout)
{
    abort();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkPipelineLayout *layout)
{
    abort();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(
    VkDevice device, const VkQueryPoolCreateInfo *info, const VkAllocationCallbacks *allocator, VkQueryPool *pool)
{
    abort();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo *info, const VkAllocationCallbacks *allocator, VkSampler *sampler)
{
    abort();
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count,
    size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    abort();
}
