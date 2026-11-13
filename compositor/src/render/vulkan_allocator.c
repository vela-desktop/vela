// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Venus cannot necessarily import resources created by virgl's GBM backend.
// Allocate on our Vulkan device and export a DMA-BUF instead. The renderer
// imports that same resource through its normal target cache: no CPU copies,
// no second rendering device, and unchanged output synchronization.
#include "render/render.h"
#include "util.h"
#include "alloc/alloc.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/util/log.h>

struct vulkan_allocator {
    struct wlr_allocator base;
    struct vela_vulkan *vk;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier;
};

struct vulkan_buffer {
    struct wlr_buffer base;
    struct vela_vulkan *vk;
    VkImage image;
    VkDeviceMemory memory;
    struct wlr_dmabuf_attributes dmabuf;
};

static void release_buffer(struct vulkan_buffer *buffer)
{
    wlr_dmabuf_attributes_finish(&buffer->dmabuf);
    if (buffer->image) vkDestroyImage(buffer->vk->device, buffer->image, NULL);
    if (buffer->memory) vkFreeMemory(buffer->vk->device, buffer->memory, NULL);
    free(buffer);
}

static void buffer_destroy(struct wlr_buffer *base)
{
    struct vulkan_buffer *buffer = (struct vulkan_buffer *)base;
    // Finish addons (the imported render target) before releasing the export.
    wlr_buffer_finish(base);
    release_buffer(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *base, struct wlr_dmabuf_attributes *attributes)
{
    *attributes = ((struct vulkan_buffer *)base)->dmabuf;
    return true;
}

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .get_dmabuf = buffer_get_dmabuf,
};

static uint32_t modifier_planes(struct vela_vulkan *vk, VkFormat format, uint64_t modifier)
{
    VkDrmFormatModifierPropertiesListEXT list = { .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 properties = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list };
    vkGetPhysicalDeviceFormatProperties2(vk->physical, format, &properties);
    list.pDrmFormatModifierProperties = VELA_NEW_N(VkDrmFormatModifierPropertiesEXT, list.drmFormatModifierCount);
    vkGetPhysicalDeviceFormatProperties2(vk->physical, format, &properties);
    uint32_t planes = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        if (list.pDrmFormatModifierProperties[i].drmFormatModifier == modifier) {
            planes = list.pDrmFormatModifierProperties[i].drmFormatModifierPlaneCount;
            break;
        }
    }
    free(list.pDrmFormatModifierProperties);
    return planes;
}

static struct wlr_buffer *export_buffer(struct vulkan_allocator *alloc, int width, int height,
    const struct vela_pixel_format *format, uint64_t modifier)
{
    struct vela_vulkan *vk = alloc->vk;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Match the importing renderer's usage, including its optional blur read.
    if (vela_vulkan_supports_dmabuf(vk, format->srgb, modifier, usage | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (!vela_vulkan_can_export_dmabuf(vk, format->srgb, modifier, usage)) return NULL;
    uint32_t planes = modifier_planes(vk, format->srgb, modifier);
    if (planes < 1 || planes > WLR_DMABUF_MAX_PLANES) return NULL;

    struct vulkan_buffer *buffer = VELA_NEW(struct vulkan_buffer);
    buffer->vk = vk;
    struct wlr_dmabuf_attributes *d = &buffer->dmabuf;
    *d = (struct wlr_dmabuf_attributes) {
        .width = width, .height = height, .format = format->drm,
        .modifier = modifier, .n_planes = (int)planes,
    };
    for (uint32_t i = 0; i < planes; ++i) d->fd[i] = -1;

    const VkImageDrmFormatModifierListCreateInfoEXT modifiers = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &modifier,
    };
    const VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifiers,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    const VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format->srgb,
        .extent = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const bool trace = vela_env_one("VELA_DEBUG_DMABUF");
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: before vkCreateImage (export) %dx%d modifier=0x%llx usage=0x%x",
        width, height, (unsigned long long)modifier, (unsigned)usage);
    VkResult result = vkCreateImage(vk->device, &image, NULL, &buffer->image);
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: after vkCreateImage (export) result=%d", (int)result);
    if (!vela_vulkan_result(vk, result, "vkCreateImage(export)")) {
        buffer->image = VK_NULL_HANDLE;
        goto fail;
    }
    VkMemoryRequirements requirements;
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: before vkGetImageMemoryRequirements (export)");
    vkGetImageMemoryRequirements(vk->device, buffer->image, &requirements);
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: after vkGetImageMemoryRequirements (export) size=%llu memoryTypeBits=0x%x",
        (unsigned long long)requirements.size, requirements.memoryTypeBits);
    uint32_t type = vela_vulkan_memory_type(vk, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = vela_vulkan_memory_type(vk, requirements.memoryTypeBits, 0);
    if (type == UINT32_MAX) goto fail;

    // Dedicated allocations also cover drivers reporting DEDICATED_ONLY.
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = buffer->image,
    };
    const VkExportMemoryAllocateInfo export = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    const VkMemoryAllocateInfo memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: before vkAllocateMemory (export)");
    result = vkAllocateMemory(vk->device, &memory, NULL, &buffer->memory);
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: after vkAllocateMemory (export) result=%d", (int)result);
    if (!vela_vulkan_result(vk, result, "vkAllocateMemory(export)")) {
        buffer->memory = VK_NULL_HANDLE;
        goto fail;
    }
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: before vkBindImageMemory (export)");
    result = vkBindImageMemory(vk->device, buffer->image, buffer->memory, 0);
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: after vkBindImageMemory (export) result=%d", (int)result);
    if (!vela_vulkan_result(vk, result, "vkBindImageMemory(export)")) {
        goto fail;
    }
    VkImageDrmFormatModifierPropertiesEXT actual = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: before vkGetImageDrmFormatModifierPropertiesEXT (export)");
    result = alloc->get_modifier(vk->device, buffer->image, &actual);
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: after vkGetImageDrmFormatModifierPropertiesEXT result=%d modifier=0x%llx",
        (int)result, (unsigned long long)actual.drmFormatModifier);
    if (!vela_vulkan_result(vk, result, "vkGetImageDrmFormatModifierPropertiesEXT")) {
        goto fail;
    }
    if (actual.drmFormatModifier != modifier) {
        wlr_log(WLR_ERROR, "Vulkan allocator: driver returned an unrequested modifier");
        goto fail;
    }

    const VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = buffer->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: before vkGetMemoryFdKHR (export)");
    result = alloc->get_memory_fd(vk->device, &fd_info, &d->fd[0]);
    if (trace) wlr_log(WLR_INFO, "DMABUF trace: after vkGetMemoryFdKHR result=%d fd=%d", (int)result, d->fd[0]);
    if (!vela_vulkan_result(vk, result, "vkGetMemoryFdKHR")) goto fail;
    if (d->fd[0] < 0) goto fail;
    for (uint32_t i = 0; i < planes; ++i) {
        if (i > 0 && (d->fd[i] = fcntl(d->fd[0], F_DUPFD_CLOEXEC, 0)) < 0) goto fail;
        const VkImageSubresource subresource = {
            .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i,
        };
        VkSubresourceLayout layout;
        if (trace) wlr_log(WLR_INFO, "DMABUF trace: before vkGetImageSubresourceLayout (export) plane=%u", i);
        vkGetImageSubresourceLayout(vk->device, buffer->image, &subresource, &layout);
        if (trace) wlr_log(WLR_INFO, "DMABUF trace: after vkGetImageSubresourceLayout plane=%u offset=%llu stride=%llu",
            i, (unsigned long long)layout.offset, (unsigned long long)layout.rowPitch);
        if (layout.offset > UINT32_MAX || layout.rowPitch == 0 || layout.rowPitch > UINT32_MAX) goto fail;
        d->offset[i] = (uint32_t)layout.offset;
        d->stride[i] = (uint32_t)layout.rowPitch;
        if (trace) {
            struct stat info;
            if (fstat(d->fd[i], &info) == 0) {
                wlr_log(WLR_INFO, "DMABUF trace: Vulkan export plane=%u fd=%d dev=%llu ino=%llu offset=%u stride=%u",
                    i, d->fd[i], (unsigned long long)info.st_dev, (unsigned long long)info.st_ino,
                    d->offset[i], d->stride[i]);
            }
        }
    }
    wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
    return &buffer->base;
fail:
    release_buffer(buffer);
    return NULL;
}

static struct wlr_buffer *create_buffer(struct wlr_allocator *base, int width, int height,
    const struct wlr_drm_format *requested)
{
    struct vulkan_allocator *alloc = (struct vulkan_allocator *)base;
    const struct vela_pixel_format *format = vela_pixel_format_from_drm(requested->format);
    if (vela_vulkan_unusable(alloc->vk) || width <= 0 || height <= 0
        || !format || format->srgb == VK_FORMAT_UNDEFINED) return NULL;
    bool linear_only = false;
    if (vela_env_one("VELA_DEBUG_LINEAR")) {
        for (size_t i = 0; i < requested->len; ++i) linear_only |= requested->modifiers[i] == DRM_FORMAT_MOD_LINEAR;
    }
    for (size_t i = 0; i < requested->len; ++i) {
        uint64_t modifier = requested->modifiers[i];
        if (modifier == DRM_FORMAT_MOD_INVALID) continue;
        // Honor LINEAR diagnostics only if LINEAR is in the offered set.
        if (linear_only && modifier != DRM_FORMAT_MOD_LINEAR) continue;
        struct wlr_buffer *buffer = export_buffer(alloc, width, height, format, modifier);
        if (buffer) return buffer;
        if (vela_vulkan_unusable(alloc->vk)) break;
    }
    wlr_log(WLR_ERROR, "Vulkan allocator: cannot export %dx%d drm=0x%08x with the offered modifiers",
        width, height, requested->format);
    return NULL;
}

static void allocator_destroy(struct wlr_allocator *base)
{
    free(base);
}

static const struct wlr_allocator_interface allocator_impl = {
    .create_buffer = create_buffer,
    .destroy = allocator_destroy,
};

struct wlr_allocator *vela_vulkan_allocator_create(struct vela_vulkan *vk)
{
    struct vulkan_allocator *alloc = VELA_NEW(struct vulkan_allocator);
    alloc->vk = vk;
    alloc->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdKHR");
    alloc->get_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(vk->device,
        "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!alloc->get_memory_fd || !alloc->get_modifier) {
        free(alloc);
        return NULL;
    }
    wlr_allocator_init(&alloc->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
    return &alloc->base;
}
