// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the production private helpers with deterministic Vulkan failures,
// without a Vulkan device. Unrelated renderer sections are discarded at link.
#undef NDEBUG
#include "../src/render/renderer.c"
#include "../src/render/texture.c"

static VkResult bind_result, map_result;
static bool null_mapping;
static int maps, destroyed, freed, command_allocations;
static uint8_t mapping[256];
static VkDeviceSize buffer_size;
static uint64_t completed_point;

void _wlr_log(enum wlr_log_importance importance, const char *format, ...) {}

// Sanitizer global metadata can retain renderer vtables. These unrelated
// entry points must never be reached by this device-free test.
struct vela_pass *vela_renderer_begin_pass(struct vela_renderer *renderer, struct wlr_buffer *buffer) { abort(); }
struct wlr_render_pass *vela_pass_wlr(struct vela_pass *pass) { abort(); }
void vela_pass_signal_on_done(struct vela_pass *pass, struct wlr_drm_syncobj_timeline *timeline, uint64_t point) { abort(); }
bool vela_vulkan_import_dmabuf(const struct vela_vulkan *vk, const struct wlr_dmabuf_attributes *dmabuf,
    VkFormat format, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory) { abort(); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
    *buffer = (VkBuffer)(uintptr_t)1;
    buffer_size = info->size;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
    VkMemoryRequirements *requirements)
{
    *requirements = (VkMemoryRequirements) { .size = buffer_size, .alignment = 16, .memoryTypeBits = 1 };
}

uint32_t vela_vulkan_memory_type(const struct vela_vulkan *vk, uint32_t bits, VkMemoryPropertyFlags flags)
{
    return bits ? 0 : UINT32_MAX;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    *memory = (VkDeviceMemory)(uintptr_t)2;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device,
    VkPhysicalDeviceMemoryProperties *properties)
{
    *properties = (VkPhysicalDeviceMemoryProperties) {
        .memoryTypeCount = 1,
        .memoryTypes = { { .propertyFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT } },
    };
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer,
    VkDeviceMemory memory, VkDeviceSize offset)
{
    return bind_result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
    ++maps;
    *data = null_mapping ? NULL : mapping;
    return map_result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
    ++destroyed;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
    assert(destroyed > freed); // bound buffers must be destroyed before their memory
    ++freed;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t *value)
{
    *value = completed_point;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *commands)
{
    ++command_allocations;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void staging_failures(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk };
    for (int failure = 0; failure < 4; ++failure) {
        maps = destroyed = freed = 0;
        bind_result = failure == 0 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
        map_result = failure == 1 ? VK_ERROR_MEMORY_MAP_FAILED : VK_SUCCESS;
        null_mapping = failure == 2;
        struct staging_chunk *chunk = new_staging_chunk(&renderer, sizeof(mapping), true);
        if (failure < 3) {
            assert(!chunk && renderer.staging_count == 0);
            assert(destroyed == 1 && freed == 1);
            assert(maps == (failure == 0 ? 0 : 1));
        } else {
            assert(chunk && chunk->data == mapping && renderer.staging_count == 1);
            assert(!destroyed && !freed && maps == 1);
        }
    }
    free(renderer.staging);
}

static void command_allocation_failure(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk, .last_point = 7 };
    struct vela_texture texture = { .renderer = &renderer };
    const struct wlr_box box = { 0, 0, 1, 1 };
    struct vela_staging staging;
    maps = destroyed = freed = command_allocations = 0;
    bind_result = map_result = VK_SUCCESS;
    null_mapping = false;
    assert(vela_renderer_stage_readback(&renderer, sizeof(mapping), &staging));
    assert(!record_readback(&texture, &box, &staging, 4));
    assert(command_allocations == 1);
    discard_readback_staging(&renderer, &staging);
    assert(renderer.staging_count == 1);
    assert(renderer.staging[0].point == renderer.last_point);
    assert(renderer.staging[0].readers == 0);
    free(renderer.staging);
}

static void packed_pixels(uint32_t source, uint32_t destination, uint32_t expected_mask)
{
    const struct vela_pixel_format *src = vela_pixel_format_from_drm(source);
    const struct vela_pixel_format *dst = vela_pixel_format_from_drm(destination);
    const uint32_t colors[] = { 0x12345678, 0x3fffffff, 0x80c1d2e3 };
    uint8_t pixels[sizeof(colors) + 3];
    memset(pixels, 0x5a, sizeof(pixels));
    copy_row(pixels + 1, (const uint8_t *)colors, sizeof(colors), opaque_alpha_for(src, dst));
    assert(pixels[0] == 0x5a && pixels[sizeof(pixels) - 2] == 0x5a && pixels[sizeof(pixels) - 1] == 0x5a);
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        uint32_t pixel;
        memcpy(&pixel, pixels + 1 + i * sizeof(pixel), sizeof(pixel));
        assert(pixel == (colors[i] | expected_mask));
    }
}

static void cancelled_gpu_allocations_still_count(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk, .staging_count = 5, .staging_capacity = 5 };
    renderer.staging = calloc(5, sizeof(*renderer.staging));
    assert(renderer.staging);
    for (int i = 0; i < 4; ++i) {
        renderer.staging[i] = (struct staging_chunk) {
            .buffer = (VkBuffer)(uintptr_t)(100 + i), .data = mapping,
            .size = VELA_READBACK_MAX_BYTES / 4, .allocation_size = VELA_READBACK_MAX_BYTES / 4,
            .point = 12, .readback = true,
        };
    }
    renderer.staging[4].allocation_size = VELA_READBACK_MAX_BYTES; // upload memory has a separate pool
    // No protocol requests or CPU pins remain, but the GPU still owns 128 MiB.
    completed_point = 11;
    maps = destroyed = freed = 0;
    struct vela_staging staging;
    assert(!vela_renderer_stage_readback(&renderer, 16, &staging));
    assert(renderer.readback_rejected == 1 && renderer.staging_count == 5);
    assert(maps == 0 && destroyed == 1 && freed == 0); // only the unbound new buffer

    // Completion makes the same storage reusable without another allocation.
    completed_point = 12;
    maps = destroyed = freed = 0;
    assert(vela_renderer_stage_readback(&renderer, 16, &staging));
    assert(renderer.staging_count == 5 && maps == 0 && destroyed == 0 && freed == 0);
    renderer.staging[0].readers = 1;
    renderer.staging[0].point = 12;

    // Pressure may evict idle chunks, but never storage a CPU copy still uses.
    assert(!make_readback_room(&renderer, VELA_READBACK_MAX_BYTES));
    assert(renderer.staging_count == 2 && renderer.staging[0].readers == 1 && !renderer.staging[1].readback);
    assert(destroyed == 3 && freed == 3);
    free(renderer.staging);
    completed_point = 0;
}

int main(void)
{
    staging_failures();
    command_allocation_failure();
    cancelled_gpu_allocations_still_count();
    packed_pixels(DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, 0xff000000);
    packed_pixels(DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888, 0xff000000);
    packed_pixels(DRM_FORMAT_XRGB2101010, DRM_FORMAT_ARGB2101010, 0xc0000000);
    packed_pixels(DRM_FORMAT_XBGR2101010, DRM_FORMAT_ABGR2101010, 0xc0000000);
    packed_pixels(DRM_FORMAT_ARGB2101010, DRM_FORMAT_ARGB2101010, 0);
    packed_pixels(DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB2101010, 0);
    const uint16_t halves[] = { 0x1234, 0xabcd, 0x5678, 0xffff, 0x3c00, 0x0000, 0x8000, 0x0000 };
    uint16_t pixels[8];
    copy_row((uint8_t *)pixels, (const uint8_t *)halves, sizeof(halves),
        opaque_alpha_for(vela_pixel_format_from_drm(DRM_FORMAT_XBGR16161616F),
            vela_pixel_format_from_drm(DRM_FORMAT_ABGR16161616F)));
    for (int i = 0; i < 8; ++i) assert(pixels[i] == (i % 4 == 3 ? 0x3c00 : halves[i]));
    return 0;
}
