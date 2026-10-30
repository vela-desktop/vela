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
static int counter_polls, end_calls, submit_calls, wait_calls, semaphore_creates, pipeline_creates;
static int imported_fds, exported_fds, semaphore_destroys;
static VkResult submit_result, wait_result, end_result, semaphore_result, counter_result, pipeline_result;

void _wlr_log(enum wlr_log_importance importance, const char *format, ...) {}

// Production VkResult policy uses a Wayland display; the unit fixture needs
// only the same first-loss state transition, not a server/event loop.
bool vela_vulkan_result(struct vela_vulkan *vk, VkResult result, const char *operation)
{
    vela_device_loss_mark(&vk->loss, (int32_t)result, operation);
    return result == VK_SUCCESS && !vela_vulkan_unusable(vk);
}

void vela_vulkan_abort(struct vela_vulkan *vk, const char *operation)
{
    vk->terminal_error = true;
}

// Sanitizer global metadata can retain renderer vtables. These unrelated
// entry points must never be reached by this device-free test.
struct vela_pass *vela_renderer_begin_pass(struct vela_renderer *renderer, struct wlr_buffer *buffer) { abort(); }
struct wlr_render_pass *vela_pass_wlr(struct vela_pass *pass) { abort(); }
void vela_pass_signal_on_done(struct vela_pass *pass, struct wlr_drm_syncobj_timeline *timeline, uint64_t point) { abort(); }
bool vela_vulkan_import_dmabuf(struct vela_vulkan *vk, const struct wlr_dmabuf_attributes *dmabuf,
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
    ++counter_polls;
    *value = completed_point;
    return counter_result;
}

// Queue/timeline faults are injected at Vulkan entry points, not by faking
// loss *after* a successful GPU operation.
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd)
{
    ++end_calls;
    return end_result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSemaphore *semaphore)
{
    ++semaphore_creates;
    *semaphore = semaphore_result == VK_SUCCESS ? (VkSemaphore)(uintptr_t)42 : VK_NULL_HANDLE;
    return semaphore_result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks *allocator)
{
    ++semaphore_destroys;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator,
    VkPipeline *pipelines)
{
    ++pipeline_creates;
    *pipelines = VK_NULL_HANDLE;
    return pipeline_result;
}

static VkResult VKAPI_PTR fault_import_fd(VkDevice device, const VkImportSemaphoreFdInfoKHR *info)
{
    ++imported_fds;
    return VK_ERROR_DEVICE_LOST;
}

static VkResult VKAPI_PTR fault_export_fd(VkDevice device, const VkSemaphoreGetFdInfoKHR *info, int *fd)
{
    ++exported_fds;
    *fd = -1;
    return VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count,
    const VkSubmitInfo2 *submissions, VkFence fence)
{
    ++submit_calls;
    return submit_result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info,
    uint64_t timeout)
{
    ++wait_calls;
    return wait_result;
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

static void device_loss_stops_reuse(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk, .last_point = 3 };
    struct vela_staging staging;
    maps = destroyed = freed = command_allocations = counter_polls = 0;
    bind_result = VK_ERROR_DEVICE_LOST;
    map_result = VK_SUCCESS;
    null_mapping = false;
    assert(!vela_renderer_stage_readback(&renderer, sizeof(mapping), &staging));
    assert(vk.loss.lost);
    assert(strcmp(vk.loss.first_operation, "vkBindBufferMemory(staging)") == 0);
    assert(destroyed == 1 && freed == 1);
    // The first staging request legitimately polls *before* vkBindBufferMemory
    // reports loss; only new polling after the loss must be forbidden.
    const int polls_before_loss = counter_polls;
    assert(!vela_renderer_stage_readback(&renderer, sizeof(mapping), &staging));
    assert(!vela_renderer_begin_commands(&renderer));
    assert(!vela_renderer_wait(&renderer, 3));
    assert(vela_renderer_completed(&renderer) == 0);
    vela_renderer_collect(&renderer);
    assert(counter_polls == polls_before_loss && command_allocations == 0 && maps == 0);
    bind_result = VK_SUCCESS;
}

static void map_loss_stops_new_allocations(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk };
    struct vela_staging staging;
    bind_result = VK_SUCCESS;
    map_result = VK_ERROR_DEVICE_LOST;
    maps = destroyed = freed = 0;
    assert(!vela_renderer_stage_readback(&renderer, sizeof(mapping), &staging));
    assert(vk.loss.lost);
    assert(strcmp(vk.loss.first_operation, "vkMapMemory(staging)") == 0);
    assert(maps == 1 && destroyed == 1 && freed == 1);
    assert(!vela_renderer_stage_readback(&renderer, sizeof(mapping), &staging));
    assert(maps == 1);
    map_result = VK_SUCCESS;
}

static void semaphore_creation_loss(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk };
    semaphore_creates = 0;
    semaphore_result = VK_ERROR_DEVICE_LOST;
    assert(!take_semaphore(&renderer, false));
    assert(vk.loss.lost);
    assert(strcmp(vk.loss.first_operation, "vkCreateSemaphore(sync)") == 0);
    assert(!take_semaphore(&renderer, false));
    assert(semaphore_creates == 1);
    semaphore_result = VK_SUCCESS;
}

static void submission_loss_stops_new_gpu_work(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk, .timeline = (VkSemaphore)(uintptr_t)1 };
    end_calls = submit_calls = wait_calls = 0;
    submit_result = VK_ERROR_DEVICE_LOST;
    assert(!vela_renderer_submit(&renderer, (VkCommandBuffer)(uintptr_t)1, NULL, 0, NULL));
    assert(vk.loss.lost && vk.loss.first_result == VK_ERROR_DEVICE_LOST);
    assert(strcmp(vk.loss.first_operation, "vkQueueSubmit2") == 0);
    assert(renderer.last_point == 0 && end_calls == 1 && submit_calls == 1);
    assert(!vela_renderer_submit(&renderer, (VkCommandBuffer)(uintptr_t)1, NULL, 0, NULL));
    assert(end_calls == 1 && submit_calls == 1); // no new Vulkan work
}

static void failed_end_releases_command_reservations(void)
{
    struct vela_vulkan vk = { 0 };
    const VkCommandBuffer draw = (VkCommandBuffer)(uintptr_t)1;
    const VkCommandBuffer upload = (VkCommandBuffer)(uintptr_t)2;
    struct command_slot slots[2] = { { .cmd = draw, .busy = true }, { .cmd = upload, .busy = true } };
    struct staging_chunk chunk = { .point = UINT64_MAX };
    struct vela_renderer renderer = { .vk = &vk, .commands = slots, .command_count = 2,
        .upload = upload, .staging = &chunk, .staging_count = 1, .last_point = 7 };
    end_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    end_calls = submit_calls = 0;
    assert(!vela_renderer_submit(&renderer, draw, NULL, 0, NULL));
    assert(!vk.loss.lost && vk.terminal_error && renderer.upload == VK_NULL_HANDLE);
    assert(!slots[0].busy && !slots[1].busy);
    assert(slots[0].point == 0 && slots[1].point == 0 && chunk.point == 7);
    assert(end_calls == 1 && submit_calls == 0);
    end_result = VK_SUCCESS;
}

static void pipeline_loss_blocks_new_pipeline_work(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk };
    pipeline_creates = 0;
    pipeline_result = VK_ERROR_DEVICE_LOST;
    assert(!vela_renderer_pipeline(&renderer, VK_FORMAT_R8G8B8A8_UNORM, VELA_PIPELINE_RECT, false));
    assert(vk.loss.lost && pipeline_creates == 1);
    assert(!vela_renderer_pipeline(&renderer, VK_FORMAT_R8G8B8A8_UNORM, VELA_PIPELINE_RECT, false));
    assert(pipeline_creates == 1);
    pipeline_result = VK_SUCCESS;
}

static void semaphore_fd_export_loss(void)
{
    struct vela_vulkan vk = { .sync_file = true, .get_semaphore_fd = fault_export_fd };
    struct vela_renderer renderer = { .vk = &vk, .timeline = (VkSemaphore)(uintptr_t)1 };
    exported_fds = submit_calls = end_calls = 0;
    semaphore_result = submit_result = end_result = VK_SUCCESS;
    int fd = -1;
    assert(!vela_renderer_submit(&renderer, (VkCommandBuffer)(uintptr_t)1, NULL, 0, &fd));
    assert(vk.loss.lost && fd == -1 && exported_fds == 1 && submit_calls == 1);
    assert(strcmp(vk.loss.first_operation, "vkGetSemaphoreFdKHR") == 0);
    free(renderer.used_semaphores);
}

static void semaphore_fd_import_loss(void)
{
    struct vela_vulkan vk = { .sync_file = true, .import_semaphore_fd = fault_import_fd };
    struct vela_renderer renderer = { .vk = &vk, .timeline = (VkSemaphore)(uintptr_t)1 };
    int fds[2];
    assert(pipe(fds) == 0);
    imported_fds = submit_calls = end_calls = semaphore_destroys = 0;
    semaphore_result = submit_result = end_result = VK_SUCCESS;
    assert(!vela_renderer_submit(&renderer, (VkCommandBuffer)(uintptr_t)1, fds, 1, NULL));
    close(fds[1]);
    assert(vk.loss.lost && imported_fds == 1 && submit_calls == 0 && semaphore_destroys == 1);
    assert(strcmp(vk.loss.first_operation, "vkImportSemaphoreFdKHR") == 0);
    free(renderer.wait_infos);
    free(renderer.wait_semaphores);
}

static void counter_query_loss_stops_polling(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk };
    counter_polls = 0;
    counter_result = VK_ERROR_DEVICE_LOST;
    assert(vela_renderer_completed(&renderer) == 0);
    assert(vk.loss.lost && counter_polls == 1);
    assert(strcmp(vk.loss.first_operation, "vkGetSemaphoreCounterValue") == 0);
    assert(vela_renderer_completed(&renderer) == 0);
    assert(counter_polls == 1);
    counter_result = VK_SUCCESS;
}

static void timeline_wait_nonterminal_error_is_fatal(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk, .timeline = (VkSemaphore)(uintptr_t)1 };
    wait_calls = counter_polls = 0;
    wait_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(!vela_renderer_wait(&renderer, 5));
    assert(!vk.loss.lost && vk.terminal_error && wait_calls == 1);
    assert(!vela_renderer_wait(&renderer, 5));
    assert(vela_renderer_completed(&renderer) == 0);
    assert(wait_calls == 1 && counter_polls == 0);
    wait_result = VK_SUCCESS;
}

static void timeline_wait_loss_stops_polling(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk, .timeline = (VkSemaphore)(uintptr_t)1 };
    wait_calls = counter_polls = 0;
    wait_result = VK_ERROR_DEVICE_LOST;
    assert(!vela_renderer_wait(&renderer, 5));
    assert(vk.loss.lost && strcmp(vk.loss.first_operation, "vkWaitSemaphores") == 0);
    assert(wait_calls == 1 && counter_polls == 0);
    assert(!vela_renderer_wait(&renderer, 5));
    assert(!vela_renderer_begin_commands(&renderer));
    assert(vela_renderer_completed(&renderer) == 0);
    assert(wait_calls == 1 && counter_polls == 0);
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

static void shared_staging_retains_other_consumers(void)
{
    struct vela_vulkan vk = { 0 };
    struct vela_renderer renderer = { .vk = &vk, .staging_count = 1, .staging_capacity = 1 };
    renderer.staging = calloc(1, sizeof(*renderer.staging));
    assert(renderer.staging);
    const VkBuffer buffer = (VkBuffer)(uintptr_t)88;
    renderer.staging[0] = (struct staging_chunk) {
        .buffer = buffer, .data = mapping, .size = 1024, .allocation_size = 1024,
        .point = 12, .readback = true,
    };
    pin_staging(&renderer, buffer, true);
    pin_staging(&renderer, buffer, true);
    completed_point = 12;
    maps = destroyed = freed = 0;
    // GPU completion and one consumer's cancellation must not expose the
    // shared bytes to eviction/reuse while another destination is unfinished.
    pin_staging(&renderer, buffer, false);
    assert(!make_readback_room(&renderer, VELA_READBACK_MAX_BYTES));
    assert(renderer.staging_count == 1 && renderer.staging[0].readers == 1);
    assert(destroyed == 0 && freed == 0);
    pin_staging(&renderer, buffer, false);
    assert(make_readback_room(&renderer, VELA_READBACK_MAX_BYTES));
    assert(renderer.staging_count == 0 && destroyed == 1 && freed == 1);
    free(renderer.staging);
    completed_point = 0;
}

int main(void)
{
    staging_failures();
    device_loss_stops_reuse();
    map_loss_stops_new_allocations();
    semaphore_creation_loss();
    failed_end_releases_command_reservations();
    pipeline_loss_blocks_new_pipeline_work();
    semaphore_fd_export_loss();
    semaphore_fd_import_loss();
    counter_query_loss_stops_polling();
    timeline_wait_nonterminal_error_is_fatal();
    submission_loss_stops_new_gpu_work();
    timeline_wait_loss_stops_polling();
    command_allocation_failure();
    cancelled_gpu_allocations_still_count();
    shared_staging_retains_other_consumers();
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
