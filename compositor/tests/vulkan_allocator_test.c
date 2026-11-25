// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Run the real allocator and wlroots buffer lifetime with deterministic Vulkan
// entry points. Check FD ownership and cleanup at every allocation boundary.
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "../src/render/vulkan_allocator.c"

static int fail_at, calls, images, memories, destroys, frees, exports, last_fd;
static uint32_t plane_count;
static VkDeviceSize pitch;
static bool exportable, sampled_exportable, missing_proc, wrong_modifier;
static VkResult failure;

void _wlr_log(enum wlr_log_importance importance, const char *format, ...) {}

bool vela_vulkan_result(struct vela_vulkan *vk, VkResult result, const char *operation)
{
    vela_device_loss_mark(&vk->loss, (int32_t)result, operation);
    return result == VK_SUCCESS && !vela_vulkan_unusable(vk);
}

bool vela_vulkan_supports_dmabuf(const struct vela_vulkan *vk, VkFormat format, uint64_t modifier,
    VkImageUsageFlags usage)
{
    return true;
}

bool vela_vulkan_can_export_dmabuf(const struct vela_vulkan *vk, VkFormat format, uint64_t modifier,
    VkImageUsageFlags usage)
{
    return exportable && modifier == DRM_FORMAT_MOD_LINEAR
        && (!(usage & VK_IMAGE_USAGE_SAMPLED_BIT) || sampled_exportable);
}

uint32_t vela_vulkan_memory_type(const struct vela_vulkan *vk, uint32_t bits, VkMemoryPropertyFlags flags)
{
    return bits ? 0 : UINT32_MAX;
}

static VkResult next_result(void)
{
    return ++calls == fail_at ? failure : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice device,
    VkFormat format, VkFormatProperties2 *properties)
{
    VkDrmFormatModifierPropertiesListEXT *list = properties->pNext;
    if (list->pDrmFormatModifierProperties) {
        list->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierPropertiesEXT) {
            .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
            .drmFormatModifierPlaneCount = plane_count,
        };
    }
    list->drmFormatModifierCount = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *callbacks, VkImage *image)
{
    const VkExternalMemoryImageCreateInfo *external = info->pNext;
    assert(external->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    assert(info->usage == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | (sampled_exportable ? VK_IMAGE_USAGE_SAMPLED_BIT : 0)));
    assert(info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
    VkResult result = next_result();
    *image = VK_NULL_HANDLE;
    if (result == VK_SUCCESS) {
        ++images;
        *image = (VkImage)(uintptr_t)1;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
    VkMemoryRequirements *requirements)
{
    *requirements = (VkMemoryRequirements) { .size = 4096, .memoryTypeBits = 1 };
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *callbacks, VkDeviceMemory *memory)
{
    const VkExportMemoryAllocateInfo *export = info->pNext;
    const VkMemoryDedicatedAllocateInfo *dedicated = export->pNext;
    assert(export->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    assert(dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    assert(dedicated->image == (VkImage)(uintptr_t)1);
    VkResult result = next_result();
    *memory = VK_NULL_HANDLE;
    if (result == VK_SUCCESS) {
        ++memories;
        *memory = (VkDeviceMemory)(uintptr_t)2;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image,
    VkDeviceMemory memory, VkDeviceSize offset)
{
    return next_result();
}

static VkResult VKAPI_PTR get_modifier(VkDevice device, VkImage image,
    VkImageDrmFormatModifierPropertiesEXT *properties)
{
    properties->drmFormatModifier = wrong_modifier ? 42 : DRM_FORMAT_MOD_LINEAR;
    return next_result();
}

static VkResult VKAPI_PTR get_fd(VkDevice device, const VkMemoryGetFdInfoKHR *info, int *fd)
{
    VkResult result = next_result();
    *fd = -1;
    if (result == VK_SUCCESS) {
        *fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(*fd >= 0);
        last_fd = *fd;
        ++exports;
    }
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (missing_proc) return NULL;
    if (strcmp(name, "vkGetMemoryFdKHR") == 0) return (PFN_vkVoidFunction)get_fd;
    if (strcmp(name, "vkGetImageDrmFormatModifierPropertiesEXT") == 0) return (PFN_vkVoidFunction)get_modifier;
    abort();
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device, VkImage image,
    const VkImageSubresource *subresource, VkSubresourceLayout *layout)
{
    unsigned plane = 0;
    while (((VkImageAspectFlags)VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane) != subresource->aspectMask) {
        assert(++plane < plane_count);
    }
    *layout = (VkSubresourceLayout) { .offset = plane * 1024, .rowPitch = pitch };
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *callbacks)
{
    ++destroys;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *callbacks)
{
    assert(destroys > frees); // destroy the image before its dedicated memory
    ++frees;
}

static void reset(void)
{
    fail_at = calls = images = memories = destroys = frees = exports = 0;
    last_fd = -1;
    plane_count = 1;
    pitch = 256;
    exportable = true;
    sampled_exportable = true;
    missing_proc = wrong_modifier = false;
    failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    unsetenv("VELA_DEBUG_LINEAR");
}

static struct wlr_buffer *allocate(struct vela_vulkan *vk, const struct wlr_drm_format *format)
{
    struct wlr_allocator *allocator = vela_vulkan_allocator_create(vk);
    assert(allocator && allocator->buffer_caps == WLR_BUFFER_CAP_DMABUF);
    struct wlr_buffer *buffer = wlr_allocator_create_buffer(allocator, 64, 32, format);
    // Buffers own the Vulkan context reference by lifetime contract, not the
    // allocator struct: destroying the allocator first must be safe.
    wlr_allocator_destroy(allocator);
    return buffer;
}

static void clean(void)
{
    assert(images == destroys && memories == frees);
    if (last_fd >= 0) {
        errno = 0;
        assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    }
}

int main(void)
{
    uint64_t modifiers[] = { DRM_FORMAT_MOD_INVALID, 42, DRM_FORMAT_MOD_LINEAR };
    struct wlr_drm_format format = {
        .format = DRM_FORMAT_ARGB8888, .len = 3, .capacity = 3, .modifiers = modifiers,
    };
    for (uint32_t planes = 1; planes <= WLR_DMABUF_MAX_PLANES; ++planes) {
        reset();
        plane_count = planes;
        struct vela_vulkan vk = { 0 };
        struct wlr_buffer *buffer = allocate(&vk, &format);
        assert(buffer && exports == 1);
        struct wlr_dmabuf_attributes d;
        assert(wlr_buffer_get_dmabuf(buffer, &d));
        assert(d.width == 64 && d.height == 32 && d.n_planes == (int)planes);
        assert(d.modifier == DRM_FORMAT_MOD_LINEAR && d.format == format.format);
        struct stat first;
        assert(fstat(d.fd[0], &first) == 0);
        for (uint32_t i = 0; i < planes; ++i) {
            assert(d.offset[i] == i * 1024 && d.stride[i] == pitch);
            assert(fcntl(d.fd[i], F_GETFD) & FD_CLOEXEC);
            struct stat other;
            assert(fstat(d.fd[i], &other) == 0 && first.st_ino == other.st_ino && first.st_dev == other.st_dev);
            for (uint32_t j = 0; j < i; ++j) assert(d.fd[i] != d.fd[j]);
        }
        // A consumer's lock keeps exports alive after the producer drops them.
        wlr_buffer_lock(buffer);
        wlr_buffer_drop(buffer);
        assert(fcntl(d.fd[0], F_GETFD) >= 0 && destroys == 0);
        wlr_buffer_unlock(buffer);
        for (uint32_t i = 0; i < planes; ++i) assert(fcntl(d.fd[i], F_GETFD) == -1);
        clean();
    }
    // Attachment-only export still works when sampling the same image is unsupported.
    reset();
    sampled_exportable = false;
    {
        struct vela_vulkan vk = { 0 };
        struct wlr_buffer *buffer = allocate(&vk, &format);
        assert(buffer && images == 1 && exports == 1);
        wlr_buffer_drop(buffer);
        clean();
    }
    for (int stage = 1; stage <= 5; ++stage) {
        reset();
        fail_at = stage;
        struct vela_vulkan vk = { 0 };
        assert(!allocate(&vk, &format));
        clean();
        reset();
        fail_at = stage;
        failure = VK_ERROR_DEVICE_LOST;
        vk = (struct vela_vulkan) { 0 };
        assert(!allocate(&vk, &format) && vk.loss.lost);
        clean();
    }
    for (int invalid = 0; invalid < 5; ++invalid) {
        reset();
        if (invalid == 0) plane_count = 0;
        if (invalid == 1) plane_count = WLR_DMABUF_MAX_PLANES + 1;
        if (invalid == 2) pitch = (VkDeviceSize)UINT32_MAX + 1;
        if (invalid == 3) pitch = 0;
        if (invalid == 4) wrong_modifier = true;
        struct vela_vulkan vk = { 0 };
        assert(!allocate(&vk, &format));
        clean();
    }
    reset();
    exportable = false;
    struct vela_vulkan vk = { 0 };
    assert(!allocate(&vk, &format) && images == 0);
    reset();
    missing_proc = true;
    assert(!vela_vulkan_allocator_create(&vk));
    return 0;
}
