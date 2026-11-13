// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The frame path's drawing, without a GPU (docs/memory-allocation-policy.md,
// "Frame path"): the production pass.c, renderer.c and texture.c, with the
// Vulkan entry points replaced by stubs that do nothing and succeed. Every
// frame draws an output and a capture, both passes open at once, with
// rectangles, shadows, a blur, a shared-memory texture and an imported one
// read with explicit sync. Once warmed, those frames must make no allocation
// through Vela's helpers (alloc_count.c, linked with -Wl,--wrap), and every
// buffer lock and timeline reference a pass took must be back.
//
// The same fixture covers the passes' lifetime: one ended by a lost device,
// the cap on the free list, an array trimmed after one huge drawing, and the
// renderer destroyed with passes kept and one still open (each freed once;
// ASan reports any leak or double free).
#undef NDEBUG
#include <assert.h>
#include <drm_fourcc.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include "../src/render/renderer.c"
#include "../src/render/texture.c"
#include "../src/render/pass.c"
#include "alloc_count.h"

#define WARM_FRAMES 4
#define FRAMES 240

void _wlr_log(enum wlr_log_importance importance, const char *format, ...) {}

// ---------------------------------------------------- Vulkan, stubbed --

static uint64_t completed_point; // what the "GPU" has finished
static uintptr_t next_handle = 0x1000;
static int open_handles; // created minus destroyed images, views and memory

#define HANDLE(type) ((type)(next_handle++))

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

bool vela_vulkan_supports_dmabuf(const struct vela_vulkan *vk, VkFormat format, uint64_t modifier,
    VkImageUsageFlags usage)
{
    return true;
}

bool vela_vulkan_import_dmabuf(struct vela_vulkan *vk, const struct wlr_dmabuf_attributes *dmabuf,
    VkFormat format, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory)
{
    *image = HANDLE(VkImage);
    *memory = HANDLE(VkDeviceMemory);
    open_handles += 2;
    return true;
}

VkShaderModule vela_vulkan_shader(struct vela_vulkan *vk, const uint32_t *code, size_t bytes)
{
    return HANDLE(VkShaderModule);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *image)
{
    *image = HANDLE(VkImage);
    ++open_handles;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator)
{
    if (image) --open_handles;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view)
{
    *view = HANDLE(VkImageView);
    ++open_handles;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view,
    const VkAllocationCallbacks *allocator)
{
    if (view) --open_handles;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    *memory = HANDLE(VkDeviceMemory);
    ++open_handles;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
    if (memory) --open_handles;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
    VkMemoryRequirements *requirements)
{
    *requirements = (VkMemoryRequirements) { .size = 4096, .alignment = 16, .memoryTypeBits = 1 };
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize offset)
{
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator) {}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    for (uint32_t i = 0; i < count; ++i) pipelines[i] = HANDLE(VkPipeline);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
    const VkAllocationCallbacks *allocator) {}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *commands)
{
    *commands = HANDLE(VkCommandBuffer);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cmd, VkCommandBufferResetFlags flags)
{
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd)
{
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submissions,
    VkFence fence)
{
    // The work is done at once: the timeline reaches every signaled point.
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < submissions[i].signalSemaphoreInfoCount; ++j) {
            uint64_t value = submissions[i].pSignalSemaphoreInfos[j].value;
            if (value > completed_point) completed_point = value;
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t *value)
{
    *value = completed_point;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout)
{
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks *allocator) {}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool pool,
    const VkAllocationCallbacks *allocator) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule module,
    const VkAllocationCallbacks *allocator) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout,
    const VkAllocationCallbacks *allocator) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
    const VkAllocationCallbacks *allocator) {}
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler,
    const VkAllocationCallbacks *allocator) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool pool,
    const VkAllocationCallbacks *allocator) {}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSemaphore *semaphore)
{
    *semaphore = HANDLE(VkSemaphore);
    return VK_SUCCESS;
}

// Uploads, readbacks, timestamps and the renderer's own creation: outside
// what this test draws, so never reached.
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    VkDeviceSize offset) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
    VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cmd, VkImage src, VkImageLayout src_layout, VkImage dst,
    VkImageLayout dst_layout, uint32_t count, const VkImageCopy *regions) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first,
    uint32_t count) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer cmd, VkPipelineStageFlags2 stage,
    VkQueryPool pool, uint32_t query) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *pool) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *layout) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkPipelineLayout *layout) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkQueryPool *pool) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSampler *sampler) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
    VkMemoryRequirements *requirements) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device,
    VkPhysicalDeviceMemoryProperties *properties) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first,
    uint32_t count, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags flags) { abort(); }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data) { abort(); }

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo *dependency) {}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cmd) {}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count,
    const VkViewport *viewports) {}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count,
    const VkRect2D *scissors) {}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet(VkCommandBuffer cmd, VkPipelineBindPoint point,
    VkPipelineLayout layout, uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
    VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values) {}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances,
    uint32_t first_vertex, uint32_t first_instance) {}

// --------------------------------------------- explicit sync, faked --

// A timeline with its own reference count; no DRM device needed.
struct fake_timeline {
    struct wlr_drm_syncobj_timeline base;
    int refs;
};

struct wlr_drm_syncobj_timeline *wlr_drm_syncobj_timeline_ref(struct wlr_drm_syncobj_timeline *timeline)
{
    ++((struct fake_timeline *)timeline)->refs;
    return timeline;
}

void wlr_drm_syncobj_timeline_unref(struct wlr_drm_syncobj_timeline *timeline)
{
    if (timeline) {
        struct fake_timeline *fake = (struct fake_timeline *)timeline;
        assert(fake->refs > 0);
        --fake->refs;
    }
}

int wlr_drm_syncobj_timeline_export_sync_file(struct wlr_drm_syncobj_timeline *timeline, uint64_t src_point)
{
    // An already signaled fence: the submission closes it without waiting.
    return eventfd(1, EFD_CLOEXEC);
}

bool wlr_drm_syncobj_timeline_signal(struct wlr_drm_syncobj_timeline *timeline, uint64_t point)
{
    return true;
}

bool wlr_drm_syncobj_timeline_import_sync_file(struct wlr_drm_syncobj_timeline *timeline, uint64_t dst_point,
    int sync_file_fd)
{
    return true;
}

// ---------------------------------------------------- dmabuf buffers --

// A buffer that says it is a dmabuf; its descriptor is a memfd, on which the
// implicit-sync ioctls fail as for a buffer without fences.
struct fake_buffer {
    struct wlr_buffer base;
    int fd;
};

static void fake_buffer_destroy(struct wlr_buffer *buffer)
{
    struct fake_buffer *fake = wl_container_of(buffer, fake, base);
    close(fake->fd);
    free(fake);
}

static bool fake_buffer_dmabuf(struct wlr_buffer *buffer, struct wlr_dmabuf_attributes *attributes)
{
    struct fake_buffer *fake = wl_container_of(buffer, fake, base);
    *attributes = (struct wlr_dmabuf_attributes) {
        .width = buffer->width,
        .height = buffer->height,
        .format = DRM_FORMAT_ARGB8888,
        .modifier = DRM_FORMAT_MOD_LINEAR,
        .n_planes = 1,
        .fd = { fake->fd },
        .stride = { (uint32_t)buffer->width * 4 },
    };
    return true;
}

static const struct wlr_buffer_impl fake_buffer_impl = {
    .destroy = fake_buffer_destroy,
    .get_dmabuf = fake_buffer_dmabuf,
};

static struct wlr_buffer *new_buffer(int width, int height)
{
    struct fake_buffer *fake = calloc(1, sizeof(*fake));
    assert(fake);
    wlr_buffer_init(&fake->base, &fake_buffer_impl, width, height);
    fake->fd = memfd_create("vela-frame-path-test", MFD_CLOEXEC);
    assert(fake->fd >= 0);
    return wlr_buffer_lock(&fake->base); // the test's own lock
}

// ------------------------------------------------------------ fixture --

struct fixture {
    struct vela_vulkan vk;
    struct vela_renderer *renderer;
    struct wlr_buffer *output, *capture; // render targets
    struct vela_texture *shm, *imported;
    struct fake_timeline acquire, done;
    pixman_region32_t damage;
};

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->vk.device = (VkDevice)HANDLE(VkDevice);
    f->vk.queue = (VkQueue)HANDLE(VkQueue);
    wlr_drm_format_set_add(&f->vk.render_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
    // Like vela_renderer_create, without init(): no shaders or layouts to make.
    struct vela_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    r->vk = &f->vk;
    r->timeline = (VkSemaphore)HANDLE(VkSemaphore);
    wl_list_init(&r->passes_open);
    wl_list_init(&r->passes_free);
    wl_list_init(&r->targets);
    wl_list_init(&r->textures);
    f->renderer = r;
    f->output = new_buffer(1920, 1080);
    f->capture = new_buffer(640, 480);
    const struct vela_pixel_format *format = vela_pixel_format_from_drm(DRM_FORMAT_ARGB8888);
    f->shm = new_texture(r, format, 400, 300);
    f->shm->image = HANDLE(VkImage);
    f->shm->view = HANDLE(VkImageView);
    f->shm->memory = HANDLE(VkDeviceMemory);
    f->shm->uploaded = true; // as if its buffer had been copied: drawn
    f->imported = new_texture(r, format, 300, 200);
    f->imported->image = HANDLE(VkImage);
    f->imported->view = HANDLE(VkImageView);
    f->imported->memory = HANDLE(VkDeviceMemory);
    f->imported->foreign = true;
    f->imported->dmabuf_fd = memfd_create("vela-frame-path-texture", MFD_CLOEXEC);
    open_handles += 6;
    f->acquire.refs = 1;
    f->done.refs = 1;
    // Damage in three pieces: every quad is drawn in several clip pieces.
    pixman_region32_init_rect(&f->damage, 0, 0, 600, 400);
    pixman_region32_union_rect(&f->damage, &f->damage, 800, 100, 300, 300);
    pixman_region32_union_rect(&f->damage, &f->damage, 100, 700, 900, 200);
}

static void fixture_finish(struct fixture *f)
{
    // The real teardown: renderer.c's destroy, through wlroots.
    destroy(f->renderer);
    // vela_texture_release left the textures to wlroots: they are ours here.
    close(f->imported->dmabuf_fd);
    free(f->shm);
    free(f->imported);
    // The targets went with the renderer: nothing is attached to the buffers.
    wlr_buffer_unlock(f->output);
    wlr_buffer_unlock(f->capture);
    wlr_buffer_drop(f->output);
    wlr_buffer_drop(f->capture);
    pixman_region32_fini(&f->damage);
    wlr_drm_format_set_finish(&f->vk.render_formats);
    assert(f->acquire.refs == 1 && f->done.refs == 1);
    assert(open_handles == 0); // every image, view and memory destroyed
}

// What a frame of an output with a window and a blurred panel draws.
static void draw_scene(struct fixture *f, struct vela_pass *pass, int width, int height)
{
    const struct wlr_box all = { 0, 0, width, height };
    const struct wlr_render_color black = { 0.0f, 0.0f, 0.0f, 1.0f };
    const struct wlr_render_color shadow = { 0.0f, 0.0f, 0.0f, 0.3f };
    vela_pass_add_rect(pass, &all, &black, &f->damage, false, NULL, 0.0f);
    const struct wlr_box window = { 100, 100, 400, 300 };
    const struct wlr_box around = { 60, 60, 480, 380 };
    vela_pass_add_shadow(pass, &around, &window, &window, 8.0f, 12.0f, &shadow, &f->damage);
    struct vela_texture_draw texture = {
        .texture = &f->shm->base,
        .dst = window,
        .alpha = 1.0f,
        .linear = true,
        .blend = true,
        .clip = &f->damage,
        .shape_rect = window,
        .shape_radius = 8.0f,
    };
    vela_pass_add_texture(pass, &texture);
    texture.texture = &f->imported->base;
    texture.dst = (struct wlr_box) { 700, 150, 300, 200 };
    texture.wait_timeline = &f->acquire.base;
    texture.wait_point = 7;
    vela_pass_add_texture(pass, &texture);
    // A translucent panel with Vela's acrylic behind it.
    const struct wlr_render_color tint = { 0.1f, 0.1f, 0.1f, 0.5f };
    struct vela_texture_draw panel = {
        .texture = &f->shm->base,
        .dst = { 0, height - 48, width, 48 },
        .alpha = 0.9f,
        .blend = true,
    };
    pixman_region32_t behind;
    pixman_region32_init_rect(&behind, 0, height - 48, (unsigned)width, 48);
    vela_pass_add_blur(pass, &panel, &behind, 2.0f, &tint, 1.25f, false);
    pixman_region32_fini(&behind);
    vela_pass_add_texture(pass, &panel);
}

// One frame: the output's pass and a capture's, open at the same time, the
// capture's begun after the output's and submitted first (or the other way
// round on odd frames).
static void frame(struct fixture *f, int number)
{
    struct vela_pass *output = vela_renderer_begin_pass(f->renderer, f->output);
    assert(output);
    vela_pass_measure(output);
    draw_scene(f, output, 1920, 1080);
    // Several clip pieces, the blur, the explicit-sync wait: every array.
    assert(output->draw_count > 8 && output->blur_count == 1 && output->wait_count == 1);
    struct vela_pass *capture = vela_renderer_begin_pass(f->renderer, f->capture);
    assert(capture && capture != output);
    vela_pass_signal_on_done(capture, &f->done.base, (uint64_t)number + 1);
    draw_scene(f, capture, 640, 480);
    struct vela_pass_result result;
    if (number % 2) {
        assert(vela_pass_submit(capture, &result));
        assert(vela_pass_submit(output, &result));
    } else {
        assert(vela_pass_submit(output, &result));
        assert(vela_pass_submit(capture, &result));
    }
    // Nothing a pass held survives it.
    assert(f->output->n_locks == 1 && f->capture->n_locks == 1);
    assert(f->acquire.refs == 1 && f->done.refs == 1);
    assert(wl_list_empty(&f->renderer->passes_open));
}

static void steady_frames_allocate_nothing(void)
{
    struct fixture f;
    fixture_init(&f);
    for (int i = 0; i < WARM_FRAMES; ++i) {
        frame(&f, i);
    }
    struct vela_alloc_count before = vela_alloc_count_get();
    for (int i = 0; i < FRAMES; ++i) {
        frame(&f, WARM_FRAMES + i);
    }
    struct vela_alloc_count after = vela_alloc_count_get();
    printf("frame path: %llu allocation(s) and %llu grow check(s) in %d frames of two passes\n",
        (unsigned long long)(after.allocations - before.allocations),
        (unsigned long long)(after.checks - before.checks), FRAMES);
    fflush(stdout);
    assert(after.allocations == before.allocations);
    assert(after.checks > before.checks); // the counter does see the frame's calls
    // Both passes are kept for the next frame.
    assert(f.renderer->passes_free_count == 2);
    fixture_finish(&f);
}

// A pass ended by a lost device is never recorded, and still goes back to the
// list with what it held dropped. The renderer then frees it once.
static void lost_device_releases_the_pass(void)
{
    struct fixture f;
    fixture_init(&f);
    frame(&f, 0);
    struct vela_pass *pass = vela_renderer_begin_pass(f.renderer, f.output);
    vela_pass_signal_on_done(pass, &f.done.base, 1);
    draw_scene(&f, pass, 1920, 1080);
    assert(f.output->n_locks == 2 && f.acquire.refs == 2 && f.done.refs == 2);
    f.vk.loss.lost = true;
    assert(!vela_pass_submit(pass, NULL));
    assert(f.output->n_locks == 1 && f.acquire.refs == 1 && f.done.refs == 1);
    assert(wl_list_empty(&f.renderer->passes_open) && f.renderer->passes_free_count == 2);
    // No new pass on a lost device.
    assert(!vela_renderer_begin_pass(f.renderer, f.output));
    fixture_finish(&f);
}

// Six passes open at once: the list keeps VELA_PASS_FREE_MAX of them.
static void free_list_is_capped(void)
{
    struct fixture f;
    fixture_init(&f);
    struct vela_pass *passes[VELA_PASS_FREE_MAX + 2];
    for (int i = 0; i < VELA_PASS_FREE_MAX + 2; ++i) {
        passes[i] = vela_renderer_begin_pass(f.renderer, i % 2 ? f.capture : f.output);
        draw_scene(&f, passes[i], 640, 480);
    }
    assert(f.output->n_locks == 1 + (VELA_PASS_FREE_MAX + 3) / 2);
    for (int i = 0; i < VELA_PASS_FREE_MAX + 2; ++i) {
        assert(vela_pass_submit(passes[i], NULL));
    }
    assert(f.renderer->passes_free_count == VELA_PASS_FREE_MAX);
    assert(wl_list_length(&f.renderer->passes_free) == VELA_PASS_FREE_MAX);
    assert(f.output->n_locks == 1 && f.capture->n_locks == 1 && f.acquire.refs == 1);
    fixture_finish(&f);
}

// One drawing of 6000 clip pieces grows draws past VELA_PASS_KEEP_BYTES: the
// pass gives that array back when it ends, and the next ordinary drawing
// grows a small one again.
static void huge_drawing_is_not_kept(void)
{
    struct fixture f;
    fixture_init(&f);
    pixman_region32_t pieces;
    pixman_region32_init(&pieces);
    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 100; ++x) {
            pixman_region32_union_rect(&pieces, &pieces, x * 16, y * 16, 8, 8);
        }
    }
    assert(pixman_region32_n_rects(&pieces) == 6000);
    struct vela_pass *pass = vela_renderer_begin_pass(f.renderer, f.output);
    const struct wlr_box all = { 0, 0, 1920, 1080 };
    const struct wlr_render_color gray = { 0.5f, 0.5f, 0.5f, 1.0f };
    vela_pass_add_rect(pass, &all, &gray, &pieces, false, NULL, 0.0f);
    assert(pass->draw_count == 6000);
    assert((size_t)pass->draw_capacity * sizeof(*pass->draws) > VELA_PASS_KEEP_BYTES);
    assert(vela_pass_submit(pass, NULL));
    assert(f.renderer->passes_free_count == 1);
    assert(!pass->draws && pass->draw_capacity == 0); // the same object, kept
    frame(&f, 0);
    assert(pass->draws && (size_t)pass->draw_capacity * sizeof(*pass->draws) <= VELA_PASS_KEEP_BYTES);
    pixman_region32_fini(&pieces);
    fixture_finish(&f);
}

// The renderer destroyed with two passes kept and one still open: all three
// are freed once, and the open one's lock and references are dropped.
static void destroy_frees_every_pass_once(void)
{
    struct fixture f;
    fixture_init(&f);
    frame(&f, 0);
    struct vela_pass *open = vela_renderer_begin_pass(f.renderer, f.output);
    vela_pass_signal_on_done(open, &f.done.base, 1);
    draw_scene(&f, open, 1920, 1080);
    assert(f.renderer->passes_free_count == 1 && f.output->n_locks == 2 && f.done.refs == 2);
    vela_renderer_destroy_passes(f.renderer);
    assert(wl_list_empty(&f.renderer->passes_open) && wl_list_empty(&f.renderer->passes_free));
    assert(f.renderer->passes_free_count == 0);
    assert(f.output->n_locks == 1 && f.acquire.refs == 1 && f.done.refs == 1);
    // destroy() calls it again: nothing is left to free twice.
    fixture_finish(&f);
}

int main(void)
{
    steady_frames_allocate_nothing();
    lost_device_releases_the_pass();
    free_list_is_capped();
    huge_drawing_is_not_kept();
    destroy_frees_every_pass_once();
    printf("frame path: all passed\n");
    return 0;
}
