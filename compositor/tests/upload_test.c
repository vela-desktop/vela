// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared-memory uploads under the staging budget, and the image-extent cap,
// without a GPU (docs/memory-allocation-policy.md, point 6 and "Tests"): the
// production renderer.c, texture.c and pass.c on a fake Vulkan device that
// keeps images and buffers in memory and runs the recorded copies and draws
// when its timeline reaches them, as a GPU would. Every draw checks that the
// image it reads is one whole buffer a client committed, never a mix.
//
// The budget is the production code's, with smaller numbers: pieces of
// 64 KiB and 8 chunks (512 KiB), so buffers above it stay small.
#undef NDEBUG
#define VELA_UPLOAD_PIECE_BYTES (64u * 1024u)
#define VELA_UPLOAD_MAX_CHUNKS 8
#include <assert.h>
#include <drm_fourcc.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include "../src/render/renderer.c"
#include "../src/render/texture.c"
#include "../src/render/pass.c"
#include "alloc_count.h"

#define BUDGET (VELA_UPLOAD_MAX_CHUNKS * VELA_UPLOAD_PIECE_BYTES)
#define BPP 4

void _wlr_log(enum wlr_log_importance importance, const char *format, ...) {}

// ------------------------------------------------------ the fake device --

struct fake_memory {
    size_t size;
    uint8_t *data;
};

struct fake_buffer {
    size_t size;
    struct fake_memory *memory;
    bool destroyed;
};

struct fake_image {
    uint32_t width, height;
    struct fake_memory *memory;
    bool destroyed;
    bool target; // a render target: never checked
};

struct fake_view {
    struct fake_image *image;
    bool destroyed;
};

enum op_kind { OP_COPY_BUFFER, OP_COPY_IMAGE, OP_DRAW };

struct op {
    enum op_kind kind;
    struct fake_buffer *buffer;
    VkDeviceSize offset;
    struct fake_image *src, *dst;
    int32_t x, y;
    uint32_t width, height;
    struct fake_view *view;
};

struct fake_cmd {
    struct op *ops;
    int count, capacity;
};

struct submission {
    uint64_t point;
    struct op *ops;
    int count;
};

static uint64_t completed_point;
static bool gpu_lag; // the GPU runs only when complete_gpu() says so
static struct submission *queue;
static int queue_count, queue_capacity;

// Everything made, freed at the end (destroyed objects stay readable, so a
// late use is caught instead of crashing).
static void **made;
static int made_count, made_capacity;
static struct fake_cmd *cmds[64];
static int cmd_count;
static int live_images, live_memory; // created minus destroyed

// The largest upload staging allocated at once, the largest piece copied.
static size_t staging_bytes, staging_peak, largest_piece, upload_chunk_size;
static int images_created, imports, piece_copies;

static void *make(size_t size)
{
    void *object = calloc(1, size);
    assert(object);
    if (made_count == made_capacity) {
        made_capacity = made_capacity ? made_capacity * 2 : 64;
        made = realloc(made, (size_t)made_capacity * sizeof(*made));
        assert(made);
    }
    made[made_count++] = object;
    return object;
}

// What a draw reads must be one whole buffer the test committed.
static void (*check_drawn)(const struct fake_image *image);

static void run(const struct op *op)
{
    switch (op->kind) {
    case OP_COPY_BUFFER: {
        assert(!op->buffer->destroyed && !op->dst->destroyed); // nothing used after it went
        size_t row_bytes = (size_t)op->width * BPP;
        assert(op->offset + row_bytes * op->height <= op->buffer->size);
        for (uint32_t y = 0; y < op->height; ++y) {
            memcpy(op->dst->memory->data + ((size_t)(op->y + (int32_t)y) * op->dst->width + (size_t)op->x) * BPP,
                op->buffer->memory->data + op->offset + y * row_bytes, row_bytes);
        }
        break;
    }
    case OP_COPY_IMAGE:
        assert(!op->src->destroyed && !op->dst->destroyed);
        assert(op->src->width == op->dst->width && op->src->height == op->dst->height);
        memcpy(op->dst->memory->data, op->src->memory->data, (size_t)op->src->width * op->src->height * BPP);
        break;
    case OP_DRAW:
        assert(!op->view->destroyed && !op->view->image->destroyed);
        if (check_drawn) check_drawn(op->view->image);
        break;
    }
}

// The GPU reaches `point`: what was submitted up to it runs, in order.
static void complete_gpu(uint64_t point)
{
    int kept = 0;
    for (int i = 0; i < queue_count; ++i) {
        struct submission *s = &queue[i];
        if (s->point <= point) {
            for (int j = 0; j < s->count; ++j) run(&s->ops[j]);
            free(s->ops);
        } else {
            queue[kept++] = *s;
        }
    }
    queue_count = kept;
    if (point > completed_point) completed_point = point;
}

static void record_op(VkCommandBuffer cmd, struct op op)
{
    struct fake_cmd *fake = (struct fake_cmd *)cmd;
    if (fake->count == fake->capacity) {
        fake->capacity = fake->capacity ? fake->capacity * 2 : 16;
        fake->ops = realloc(fake->ops, (size_t)fake->capacity * sizeof(*fake->ops));
        assert(fake->ops);
    }
    fake->ops[fake->count++] = op;
}

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

// Render targets and imported textures: images of their own, never checked.
bool vela_vulkan_import_dmabuf(struct vela_vulkan *vk, const struct wlr_dmabuf_attributes *dmabuf,
    VkFormat format, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory)
{
    ++imports;
    struct fake_image *fake = make(sizeof(*fake));
    fake->width = (uint32_t)dmabuf->width;
    fake->height = (uint32_t)dmabuf->height;
    fake->target = true;
    fake->memory = make(sizeof(*fake->memory));
    *image = (VkImage)fake;
    *memory = (VkDeviceMemory)fake->memory;
    live_images += 1;
    live_memory += 1;
    return true;
}

VkShaderModule vela_vulkan_shader(struct vela_vulkan *vk, const uint32_t *code, size_t bytes)
{
    return (VkShaderModule)make(1);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *image)
{
    ++images_created;
    struct fake_image *fake = make(sizeof(*fake));
    fake->width = info->extent.width;
    fake->height = info->extent.height;
    *image = (VkImage)fake;
    ++live_images;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator)
{
    if (!image) return;
    struct fake_image *fake = (struct fake_image *)image;
    assert(!fake->destroyed);
    fake->destroyed = true;
    --live_images;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view)
{
    struct fake_view *fake = make(sizeof(*fake));
    fake->image = (struct fake_image *)info->image;
    *view = (VkImageView)fake;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view,
    const VkAllocationCallbacks *allocator)
{
    if (view) ((struct fake_view *)view)->destroyed = true;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    struct fake_memory *fake = make(sizeof(*fake));
    fake->size = info->allocationSize;
    fake->data = calloc(1, fake->size);
    assert(fake->data);
    *memory = (VkDeviceMemory)fake;
    ++live_memory;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
    if (!memory) return;
    struct fake_memory *fake = (struct fake_memory *)memory;
    free(fake->data);
    fake->data = NULL;
    --live_memory;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
    VkMemoryRequirements *requirements)
{
    const struct fake_image *fake = (const struct fake_image *)image;
    *requirements = (VkMemoryRequirements) {
        .size = (VkDeviceSize)fake->width * fake->height * BPP, .alignment = 16, .memoryTypeBits = 1 };
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize offset)
{
    ((struct fake_image *)image)->memory = (struct fake_memory *)memory;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
    struct fake_buffer *fake = make(sizeof(*fake));
    fake->size = info->size;
    *buffer = (VkBuffer)fake;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
    if (!buffer) return;
    struct fake_buffer *fake = (struct fake_buffer *)buffer;
    assert(!fake->destroyed);
    fake->destroyed = true;
    if (fake->memory) staging_bytes -= fake->size;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
    VkMemoryRequirements *requirements)
{
    *requirements = (VkMemoryRequirements) {
        .size = ((struct fake_buffer *)buffer)->size, .alignment = 16, .memoryTypeBits = 1 };
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    VkDeviceSize offset)
{
    struct fake_buffer *fake = (struct fake_buffer *)buffer;
    fake->memory = (struct fake_memory *)memory;
    // Only uploads stage memory in this test.
    staging_bytes += fake->size;
    if (staging_bytes > staging_peak) staging_peak = staging_bytes;
    if (!upload_chunk_size) upload_chunk_size = fake->size;
    assert(fake->size == upload_chunk_size); // every upload chunk is one piece
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
    *data = ((struct fake_memory *)memory)->data;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    for (uint32_t i = 0; i < count; ++i) pipelines[i] = (VkPipeline)make(1);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
    const VkAllocationCallbacks *allocator) {}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *commands)
{
    struct fake_cmd *cmd = make(sizeof(struct fake_cmd));
    assert(cmd_count < 64);
    cmds[cmd_count++] = cmd;
    *commands = (VkCommandBuffer)cmd;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cmd, VkCommandBufferResetFlags flags)
{
    ((struct fake_cmd *)cmd)->count = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{
    ((struct fake_cmd *)cmd)->count = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd)
{
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q, uint32_t count, const VkSubmitInfo2 *submissions,
    VkFence fence)
{
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t point = 0;
        for (uint32_t j = 0; j < submissions[i].signalSemaphoreInfoCount; ++j) {
            if (submissions[i].pSignalSemaphoreInfos[j].value > point) point = submissions[i].pSignalSemaphoreInfos[j].value;
        }
        // The command buffers in their order, copied: they may be reset
        // and recorded again before the GPU runs them only if the renderer
        // is wrong, which the copies then don't hide.
        struct submission s = { .point = point };
        for (uint32_t j = 0; j < submissions[i].commandBufferInfoCount; ++j) {
            const struct fake_cmd *cmd = (const struct fake_cmd *)submissions[i].pCommandBufferInfos[j].commandBuffer;
            s.ops = realloc(s.ops, (size_t)(s.count + cmd->count + 1) * sizeof(*s.ops));
            assert(s.ops);
            memcpy(s.ops + s.count, cmd->ops, (size_t)cmd->count * sizeof(*s.ops));
            s.count += cmd->count;
        }
        if (queue_count == queue_capacity) {
            queue_capacity = queue_capacity ? queue_capacity * 2 : 16;
            queue = realloc(queue, (size_t)queue_capacity * sizeof(*queue));
            assert(queue);
        }
        queue[queue_count++] = s;
        if (!gpu_lag) complete_gpu(point);
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
    complete_gpu(info->pValues[0]);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
    complete_gpu(UINT64_MAX - 1);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
    VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions)
{
    for (uint32_t i = 0; i < count; ++i) {
        const VkBufferImageCopy *r = &regions[i];
        assert(r->bufferRowLength == 0 && r->bufferImageHeight == 0); // tightly packed rows
        size_t bytes = (size_t)r->imageExtent.width * r->imageExtent.height * BPP;
        assert(bytes <= VELA_UPLOAD_PIECE_BYTES); // no piece larger than one
        assert(r->bufferOffset % 16 == 0);
        if (bytes > largest_piece) largest_piece = bytes;
        ++piece_copies;
        record_op(cmd, (struct op) { OP_COPY_BUFFER, (struct fake_buffer *)buffer, r->bufferOffset, NULL,
            (struct fake_image *)image, r->imageOffset.x, r->imageOffset.y, r->imageExtent.width,
            r->imageExtent.height, NULL });
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cmd, VkImage src, VkImageLayout src_layout, VkImage dst,
    VkImageLayout dst_layout, uint32_t count, const VkImageCopy *regions)
{
    assert(count == 1);
    record_op(cmd, (struct op) { OP_COPY_IMAGE, NULL, 0, (struct fake_image *)src, (struct fake_image *)dst, 0, 0,
        regions[0].extent.width, regions[0].extent.height, NULL });
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet(VkCommandBuffer cmd, VkPipelineBindPoint point,
    VkPipelineLayout layout, uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes)
{
    for (uint32_t i = 0; i < count; ++i) {
        record_op(cmd, (struct op) { .kind = OP_DRAW, .view = (struct fake_view *)writes[i].pImageInfo->imageView });
    }
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks *allocator) {}
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
    *semaphore = (VkSemaphore)make(1);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device,
    VkPhysicalDeviceMemoryProperties *properties)
{
    *properties = (VkPhysicalDeviceMemoryProperties) { .memoryTypeCount = 1 };
}

// Readbacks, timestamps and the renderer's own creation: never reached.
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first,
    uint32_t count) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer cmd, VkPipelineStageFlags2 stage,
    VkQueryPool pool, uint32_t query) { abort(); }
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
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first,
    uint32_t count, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags flags) { abort(); }

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo *dependency) {}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cmd) {}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count,
    const VkViewport *viewports) {}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count,
    const VkRect2D *scissors) {}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
    VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values) {}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances,
    uint32_t first_vertex, uint32_t first_instance) {}

struct wlr_drm_syncobj_timeline *wlr_drm_syncobj_timeline_ref(struct wlr_drm_syncobj_timeline *timeline)
{
    return timeline;
}
void wlr_drm_syncobj_timeline_unref(struct wlr_drm_syncobj_timeline *timeline) {}
int wlr_drm_syncobj_timeline_export_sync_file(struct wlr_drm_syncobj_timeline *timeline, uint64_t src_point)
{
    abort();
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

// Drawings export their end as a sync_file (so the CPU doesn't wait for the
// GPU, which in_flight_staging_counts keeps behind): an fd to close.
static VkResult VKAPI_PTR export_fd(VkDevice device, const VkSemaphoreGetFdInfoKHR *info, int *fd)
{
    *fd = eventfd(1, EFD_CLOEXEC);
    return *fd >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

// ---------------------------------------------------- client buffers --

// A wl_shm buffer as wlroots shows it: pixels the renderer reads through
// data-pointer access.
struct shm_buffer {
    struct wlr_buffer base;
    uint32_t *pixels;
    int accessing;
};

static void shm_destroy(struct wlr_buffer *buffer)
{
    struct shm_buffer *shm = wl_container_of(buffer, shm, base);
    assert(shm->accessing == 0);
    free(shm->pixels);
    free(shm);
}

static bool shm_begin(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    struct shm_buffer *shm = wl_container_of(buffer, shm, base);
    ++shm->accessing;
    *data = shm->pixels;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)buffer->width * BPP;
    return true;
}

static void shm_end(struct wlr_buffer *buffer)
{
    struct shm_buffer *shm = wl_container_of(buffer, shm, base);
    assert(shm->accessing > 0);
    --shm->accessing;
}

static const struct wlr_buffer_impl shm_impl = {
    .destroy = shm_destroy,
    .begin_data_ptr_access = shm_begin,
    .end_data_ptr_access = shm_end,
};

// The test's own lock is the only one left: the renderer released it.
static bool released(const struct shm_buffer *shm)
{
    return shm->base.n_locks == 1;
}

// Pixel (x, y) of version `version`: every version differs in every pixel,
// so a mix of two is always seen.
static uint32_t pixel(uint32_t version, int x, int y)
{
    return (version << 24) ^ ((uint32_t)y << 12) ^ (uint32_t)x ^ 0x00a5a5a5u;
}

static struct shm_buffer *new_shm(int width, int height, uint32_t version)
{
    struct shm_buffer *shm = calloc(1, sizeof(*shm));
    assert(shm);
    shm->pixels = malloc((size_t)width * height * BPP);
    assert(shm->pixels);
    wlr_buffer_init(&shm->base, &shm_impl, width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) shm->pixels[(size_t)y * width + x] = pixel(version, x, y);
    }
    wlr_buffer_lock(&shm->base);
    return shm;
}

static void drop_shm(struct shm_buffer *shm)
{
    wlr_buffer_unlock(&shm->base);
    wlr_buffer_drop(&shm->base);
}

// A render target: a "dmabuf" whose descriptor is a memfd.
struct target_buffer {
    struct wlr_buffer base;
    int fd;
};

static void target_destroy(struct wlr_buffer *buffer)
{
    struct target_buffer *target = wl_container_of(buffer, target, base);
    close(target->fd);
    free(target);
}

static bool target_dmabuf(struct wlr_buffer *buffer, struct wlr_dmabuf_attributes *attributes)
{
    struct target_buffer *target = wl_container_of(buffer, target, base);
    *attributes = (struct wlr_dmabuf_attributes) {
        .width = buffer->width,
        .height = buffer->height,
        .format = DRM_FORMAT_ARGB8888,
        .modifier = DRM_FORMAT_MOD_LINEAR,
        .n_planes = 1,
        .fd = { target->fd },
        .stride = { (uint32_t)buffer->width * BPP },
    };
    return true;
}

static const struct wlr_buffer_impl target_impl = {
    .destroy = target_destroy,
    .get_dmabuf = target_dmabuf,
};

static struct wlr_buffer *new_target(int width, int height)
{
    struct target_buffer *target = calloc(1, sizeof(*target));
    assert(target);
    wlr_buffer_init(&target->base, &target_impl, width, height);
    target->fd = memfd_create("vela-upload-test", MFD_CLOEXEC);
    assert(target->fd >= 0);
    return wlr_buffer_lock(&target->base);
}

// ------------------------------------------------------------ fixture --

#define MAX_SURFACES 4
#define MAX_VERSIONS 8

// A surface of the test: its texture and what it may show, the image of each
// buffer committed, in order.
struct surface {
    struct wlr_texture *texture;
    int width, height;
    uint32_t *versions[MAX_VERSIONS];
    int version_count;
    int shown[MAX_VERSIONS]; // draws that read version i
};

struct fixture {
    struct vela_vulkan vk;
    struct vela_renderer *renderer;
    struct wlr_buffer *output;
    struct surface surfaces[MAX_SURFACES];
    int surface_count;
    int frames;
};

static struct fixture *current;

// Every draw of a surface reads one whole committed buffer.
static void check(const struct fake_image *image)
{
    if (image->target) return;
    size_t bytes = (size_t)image->width * image->height * BPP;
    for (int i = 0; i < current->surface_count; ++i) {
        struct surface *s = &current->surfaces[i];
        if ((uint32_t)s->width != image->width || (uint32_t)s->height != image->height) continue;
        for (int v = 0; v < s->version_count; ++v) {
            if (memcmp(image->memory->data, s->versions[v], bytes) == 0) {
                ++s->shown[v];
                return;
            }
        }
    }
    fprintf(stderr, "a draw read an image that is no committed buffer (half an upload?)\n");
    abort();
}

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    current = f;
    check_drawn = check;
    gpu_lag = false;
    f->vk.device = (VkDevice)make(1);
    f->vk.queue = (VkQueue)make(1);
    f->vk.sync_file = true;
    f->vk.get_semaphore_fd = export_fd;
    wlr_drm_format_set_add(&f->vk.render_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
    f->vk.shm_formats[0] = DRM_FORMAT_ARGB8888;
    f->vk.shm_max_extent[0] = (struct vela_extent) { 4096, 4096 };
    f->vk.shm_format_count = 1;
    // Like vela_renderer_create, without init(): no shaders or layouts.
    struct vela_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    wlr_renderer_init(&r->base, &renderer_impl, WLR_BUFFER_CAP_DMABUF);
    r->vk = &f->vk;
    r->timeline = (VkSemaphore)make(1);
    wl_list_init(&r->passes_open);
    wl_list_init(&r->passes_free);
    wl_list_init(&r->targets);
    wl_list_init(&r->textures);
    wl_list_init(&r->uploads);
    f->renderer = r;
    f->output = new_target(1920, 1080);
    staging_bytes = staging_peak = largest_piece = upload_chunk_size = 0;
    images_created = imports = piece_copies = 0;
}

static void fixture_finish(struct fixture *f)
{
    for (int i = 0; i < f->surface_count; ++i) {
        if (f->surfaces[i].texture) wlr_texture_destroy(f->surfaces[i].texture);
        for (int v = 0; v < f->surfaces[i].version_count; ++v) free(f->surfaces[i].versions[v]);
    }
    // The real teardown, as wlroots would (unless the test did it already).
    if (f->renderer) destroy(f->renderer);
    wlr_buffer_unlock(f->output);
    wlr_buffer_drop(f->output);
    wlr_drm_format_set_finish(&f->vk.render_formats);
    complete_gpu(UINT64_MAX - 1);
    assert(queue_count == 0);
    assert(live_images == 0 && live_memory == 0); // every image and allocation freed
    assert(staging_bytes == 0);
    for (int i = 0; i < cmd_count; ++i) free(cmds[i]->ops);
    cmd_count = 0;
    for (int i = 0; i < made_count; ++i) free(made[i]);
    free(made);
    made = NULL;
    made_count = made_capacity = 0;
    free(queue);
    queue = NULL;
    queue_count = queue_capacity = 0;
    completed_point = 0;
    current = NULL;
}

// The image buffer `shm` holds, as the surface's next acceptable content.
static void remember(struct surface *s, const struct shm_buffer *shm)
{
    assert(s->version_count < MAX_VERSIONS);
    size_t bytes = (size_t)s->width * s->height * BPP;
    s->versions[s->version_count] = malloc(bytes);
    assert(s->versions[s->version_count]);
    memcpy(s->versions[s->version_count], shm->pixels, bytes);
    ++s->version_count;
}

// A client's first buffer on a new surface.
static struct surface *attach(struct fixture *f, struct shm_buffer *shm)
{
    struct surface *s = &f->surfaces[f->surface_count++];
    s->width = shm->base.width;
    s->height = shm->base.height;
    remember(s, shm);
    s->texture = texture_from_buffer(&f->renderer->base, &shm->base);
    assert(s->texture);
    return s;
}

// The same surface commits another buffer, with this damage (NULL: whole).
static void commit(struct surface *s, struct shm_buffer *shm, const pixman_box32_t *damage)
{
    remember(s, shm);
    pixman_region32_t region;
    if (damage) {
        pixman_region32_init_rect(&region, damage->x1, damage->y1, (unsigned)(damage->x2 - damage->x1),
            (unsigned)(damage->y2 - damage->y1));
    } else {
        pixman_region32_init_rect(&region, 0, 0, (unsigned)s->width, (unsigned)s->height);
    }
    assert(wlr_texture_update_from_buffer(s->texture, &shm->base, &region));
    pixman_region32_fini(&region);
}

// One output frame: the uploads continue, then every surface is drawn.
static struct vela_upload_progress frame(struct fixture *f)
{
    struct vela_upload_progress progress = vela_renderer_continue_uploads(f->renderer);
    // Never more upload staging than the budget, in flight included.
    assert(upload_chunk_count(f->renderer) <= VELA_UPLOAD_MAX_CHUNKS);
    assert(staging_bytes <= BUDGET);
    struct vela_pass *pass = vela_renderer_begin_pass(f->renderer, f->output);
    assert(pass);
    for (int i = 0; i < f->surface_count; ++i) {
        struct surface *s = &f->surfaces[i];
        if (!s->texture) continue;
        struct vela_texture_draw draw = {
            .texture = s->texture,
            .dst = { 10 * i, 10 * i, 300, 200 },
            .alpha = 1.0f,
            .linear = true,
        };
        vela_pass_add_texture(pass, &draw);
    }
    assert(vela_pass_submit(pass, NULL));
    ++f->frames;
    return progress;
}

static struct vela_texture *own(struct surface *s)
{
    return vela_texture_from_wlr_texture(s->texture);
}

// ------------------------------------------------------------ tests --

// A buffer at the device's limit is uploaded; one pixel more, in either
// direction, is refused before vkCreateImage, and the refusal is reported
// once for the commit that brought it.
static void extent_cap_shm(void)
{
    struct fixture f;
    fixture_init(&f);
    f.vk.shm_max_extent[0] = (struct vela_extent) { 300, 200 };
    struct shm_buffer *wide = new_shm(301, 10, 1);
    struct shm_buffer *tall = new_shm(10, 201, 1);
    struct shm_buffer *fits = new_shm(300, 200, 1);
    struct wlr_renderer *wlr = &f.renderer->base;
    assert(!texture_from_buffer(wlr, &wide->base));
    assert(images_created == 0);
    struct vela_extent_rejection rejection;
    assert(vela_renderer_take_extent_rejection(wlr, &wide->base, &rejection));
    assert(rejection.width == 301 && rejection.height == 10 && rejection.max_width == 300
        && rejection.max_height == 200 && !rejection.dmabuf);
    assert(!vela_renderer_take_extent_rejection(wlr, &wide->base, &rejection)); // once
    assert(!texture_from_buffer(wlr, &tall->base));
    assert(images_created == 0);
    assert(!vela_renderer_take_extent_rejection(wlr, &wide->base, &rejection)); // another buffer's
    assert(vela_renderer_take_extent_rejection(wlr, &tall->base, &rejection));
    struct wlr_texture *texture = texture_from_buffer(wlr, &fits->base);
    assert(texture && images_created == 1);
    assert(!vela_renderer_take_extent_rejection(wlr, &fits->base, &rejection));
    assert(f.renderer->upload_stats.rejected == 2);
    assert(released(wide) && released(tall));
    wlr_texture_destroy(texture);
    drop_shm(wide);
    drop_shm(tall);
    drop_shm(fits);
    fixture_finish(&f);
}

// A dmabuf above its format and modifier's maxExtent is refused before the
// import (where vkCreateImage is); one at the limit is imported. A modifier
// the probe never saw has no limit to pass.
static void extent_cap_dmabuf(void)
{
    struct fixture f;
    fixture_init(&f);
    wlr_drm_format_set_add(&f.vk.texture_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
    struct vela_dmabuf_extent limit = { DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, { 640, 480 } };
    f.vk.texture_extents = &limit;
    f.vk.texture_extent_count = 1;
    struct wlr_renderer *wlr = &f.renderer->base;
    struct wlr_buffer *above = new_target(641, 480);
    struct wlr_buffer *taller = new_target(640, 481);
    struct wlr_buffer *at = new_target(640, 480);
    assert(!texture_from_buffer(wlr, above) && imports == 0);
    struct vela_extent_rejection rejection;
    assert(vela_renderer_take_extent_rejection(wlr, above, &rejection) && rejection.dmabuf
        && rejection.max_width == 640);
    assert(!texture_from_buffer(wlr, taller) && imports == 0);
    struct wlr_texture *texture = texture_from_buffer(wlr, at);
    assert(texture && imports == 1);
    limit.modifier = 1234; // texture_formats still lists linear, the probe didn't
    assert(!texture_from_buffer(wlr, above) && imports == 1);
    wlr_texture_destroy(texture);
    f.vk.texture_extents = NULL;
    f.vk.texture_extent_count = 0;
    wlr_drm_format_set_finish(&f.vk.texture_formats);
    struct wlr_buffer *targets[] = { above, taller, at };
    for (int i = 0; i < 3; ++i) {
        wlr_buffer_unlock(targets[i]);
        wlr_buffer_drop(targets[i]);
    }
    fixture_finish(&f);
}

// A small buffer is copied when it comes (in place), in one frame's budget.
static void small_upload_is_in_place(void)
{
    struct fixture f;
    fixture_init(&f);
    struct shm_buffer *first = new_shm(256, 64, 1); // 64 KiB: one piece
    struct surface *s = attach(&f, first);
    assert(own(s)->uploaded && !own(s)->upload.source && released(first));
    frame(&f);
    assert(s->shown[0] == 1);
    struct shm_buffer *second = new_shm(256, 64, 2);
    commit(s, second, NULL);
    assert(released(second) && f.renderer->upload_stats.deferred == 0);
    frame(&f);
    assert(s->shown[1] == 1);
    assert(f.renderer->upload_stats.in_place == 2);
    drop_shm(first);
    drop_shm(second);
    fixture_finish(&f);
}

// One buffer of three budgets: copied over several frames, never more upload
// staging than the budget, no piece above one, the client's buffer locked
// until the last piece, nothing drawn before it (a first buffer has nothing
// older to show), then all of it.
static void oversized_buffer_stays_within_budget(void)
{
    struct fixture f;
    fixture_init(&f);
    const int height = 3 * BUDGET / (256 * BPP) + 7;
    struct shm_buffer *big = new_shm(256, height, 1);
    struct surface *s = attach(&f, big);
    assert(!own(s)->uploaded && own(s)->upload.source && !released(big));
    assert(f.renderer->upload_stats.deferred == 1 && piece_copies == 0);
    int frames = 0;
    struct vela_upload_progress progress;
    do {
        progress = frame(&f);
        ++frames;
        assert(frames <= 8);
        if (progress.pending) {
            assert(!released(big) && s->shown[0] == 0);
        }
    } while (progress.pending);
    assert(frames == 4); // three full budgets and a bit
    assert(released(big) && own(s)->uploaded);
    assert(staging_peak <= BUDGET && f.renderer->upload_stats.chunks_peak <= VELA_UPLOAD_MAX_CHUNKS);
    assert(largest_piece <= VELA_UPLOAD_PIECE_BYTES && upload_chunk_size == VELA_UPLOAD_PIECE_BYTES);
    frame(&f);
    assert(s->shown[0] >= 1); // all of it, from the frame it completed on
    printf("upload: %d rows of 1 KiB in %d frames, staging peak %zu KiB of %u\n", height, frames,
        staging_peak / 1024, BUDGET / 1024);
    // Its chunks stay for the next copy while recent, then all but two go.
    assert(upload_chunk_count(f.renderer) == VELA_UPLOAD_MAX_CHUNKS);
    for (int i = 0; i < f.renderer->staging_count; ++i) f.renderer->staging[i].staged_at -= 2 * VELA_UPLOAD_IDLE_NS;
    frame(&f);
    assert(upload_chunk_count(f.renderer) == 2 && staging_bytes == 2 * VELA_UPLOAD_PIECE_BYTES);
    drop_shm(big);
    fixture_finish(&f);
}

// Four surfaces commit buffers of half a budget each in the same frame: two
// are copied at once, the others wait for the next frames, and staging
// never goes above the budget. Every surface ends up showing its whole
// buffer.
static void several_surfaces_share_the_budget(void)
{
    struct fixture f;
    fixture_init(&f);
    const int height = BUDGET / 2 / (256 * BPP);
    struct shm_buffer *buffers[4];
    struct surface *surfaces[4];
    for (int i = 0; i < 4; ++i) {
        buffers[i] = new_shm(256, height + i, (uint32_t)i + 1); // sizes differ: each its own versions
        surfaces[i] = attach(&f, buffers[i]);
    }
    assert(own(surfaces[0])->uploaded && !own(surfaces[3])->uploaded);
    assert(staging_peak <= BUDGET);
    for (int i = 0; i < 6 && !wl_list_empty(&f.renderer->uploads); ++i) frame(&f);
    assert(wl_list_empty(&f.renderer->uploads));
    frame(&f);
    for (int i = 0; i < 4; ++i) {
        assert(own(surfaces[i])->uploaded && released(buffers[i]));
        assert(surfaces[i]->shown[0] >= 1);
    }
    assert(staging_peak <= BUDGET && f.renderer->upload_stats.chunks_peak <= VELA_UPLOAD_MAX_CHUNKS);
    for (int i = 0; i < 4; ++i) drop_shm(buffers[i]);
    fixture_finish(&f);
}

// A surface that shows a buffer commits one above the budget: every frame
// until the last piece draws the old one, whole; the frame after draws the
// new one, whole. Then the same with a partial damage, where the new image
// starts as a copy of the shown one.
static void deferred_update_switches_atomically(void)
{
    for (int partial = 0; partial < 2; ++partial) {
        struct fixture f;
        fixture_init(&f);
        const int height = 2 * BUDGET / (256 * BPP) + 30; // three frames of pieces
        struct shm_buffer *old = new_shm(256, height, 1);
        struct surface *s = attach(&f, old); // version 0
        while (frame(&f).pending) {
        }
        assert(own(s)->uploaded && released(old));
        uint64_t content = own(s)->content;
        struct shm_buffer *next = new_shm(256, height, 2);
        pixman_box32_t damage = { 0, 3, 256, height - 3 };
        if (partial) {
            // The undamaged rows are the old buffer's, as a client's would be.
            memcpy(next->pixels, old->pixels, (size_t)3 * 256 * BPP);
            memcpy(next->pixels + (size_t)(height - 3) * 256, old->pixels + (size_t)(height - 3) * 256,
                (size_t)3 * 256 * BPP);
        }
        commit(s, next, partial ? &damage : NULL); // version 1
        assert(own(s)->upload.source && own(s)->upload.image && own(s)->upload.copy_shown == (bool)partial);
        VkImage shown = own(s)->image;
        int old_draws = s->shown[0];
        int frames = 0;
        struct vela_upload_progress progress;
        do {
            progress = frame(&f);
            ++frames;
            if (progress.pending) {
                assert(own(s)->image == shown && !released(next));
                assert(s->shown[0] == old_draws + frames && s->shown[1] == 0); // the old one, whole
            }
        } while (progress.pending);
        assert(frames == 3 && progress.completed == 1);
        // Switched in the frame of the last piece: its draw reads the new one.
        assert(own(s)->image != shown && own(s)->content != content && released(next));
        assert(s->shown[1] == 1 && s->shown[0] == old_draws + frames - 1);
        frame(&f);
        assert(s->shown[1] == 2);
        assert(((struct fake_image *)shown)->destroyed); // the old image went, after its last use
        drop_shm(old);
        drop_shm(next);
        fixture_finish(&f);
    }
}

// A surface commits a new buffer while its previous upload is pending: the
// older buffer is released unseen, the pending upload continues from the
// newest, which is what ends up shown; nothing of the older one is ever
// drawn.
static void new_commit_replaces_pending_upload(void)
{
    struct fixture f;
    fixture_init(&f);
    const int height = 2 * BUDGET / (256 * BPP) + 30;
    struct shm_buffer *v1 = new_shm(256, height, 1);
    struct surface *s = attach(&f, v1);
    while (frame(&f).pending) {
    }
    struct shm_buffer *v2 = new_shm(256, height, 2);
    struct shm_buffer *v3 = new_shm(256, height, 3);
    commit(s, v2, NULL);
    frame(&f); // a piece of v2 is copied
    assert(!released(v2));
    // v3 damages only a few rows, but v2's rows aren't on screen either:
    // everything v2 brought is copied from v3.
    pixman_box32_t small = { 0, 0, 256, 2 };
    commit(s, v3, &small);
    assert(released(v2) && !released(v3) && f.renderer->upload_stats.replaced == 1);
    while (frame(&f).pending) {
    }
    frame(&f);
    assert(released(v3));
    assert(s->shown[1] == 0 && s->shown[2] > 0); // v2 (version 1) never drawn, v3 (version 2) drawn
    drop_shm(v1);
    drop_shm(v2);
    drop_shm(v3);
    fixture_finish(&f);
}

// One client commits a buffer above the budget every frame; another
// surface's upload, also above it, still completes within a bounded number of
// frames: the replaced upload goes behind it.
static void competing_uploads_do_not_starve(void)
{
    struct fixture f;
    fixture_init(&f);
    const int big_height = 2 * BUDGET / (256 * BPP) + 30;
    const int other_height = BUDGET / (128 * BPP) + 20;
    struct shm_buffer *greedy[MAX_VERSIONS];
    int greedy_count = 0;
    greedy[greedy_count++] = new_shm(256, big_height, 1);
    struct surface *a = attach(&f, greedy[0]);
    struct shm_buffer *other = new_shm(128, other_height, 9);
    struct surface *b = attach(&f, other);
    int frames = 0;
    while (!own(b)->uploaded) {
        frame(&f);
        ++frames;
        assert(frames <= 6);
        if (greedy_count < MAX_VERSIONS) {
            greedy[greedy_count] = new_shm(256, big_height, (uint32_t)greedy_count + 1);
            commit(a, greedy[greedy_count], NULL);
            ++greedy_count;
        } else {
            // The versions are all used: keep committing the last one.
            assert(wlr_texture_update_from_buffer(a->texture, &greedy[greedy_count - 1]->base,
                &(pixman_region32_t) { .extents = { 0, 0, 256, big_height } }));
        }
    }
    printf("upload: the other surface completed in %d frames under a competing client\n", frames);
    assert(released(other));
    frame(&f);
    assert(b->shown[0] >= 1);
    for (int i = 0; i < greedy_count; ++i) drop_shm(greedy[i]);
    drop_shm(other);
    fixture_finish(&f);
}

// A frame with no rendering (e.g. direct scanout) must still submit uploads
// recorded by previous buffer commits, even if a deferred upload cannot
// reserve a staging chunk and records no commands of its own.
static void blocked_deferred_upload_flushes_prior_commands(void)
{
    struct fixture f;
    fixture_init(&f);
    const int height = BUDGET / (256 * BPP);
    struct shm_buffer *first = new_shm(256, height, 1);
    struct surface *a = attach(&f, first);
    assert(own(a)->uploaded && f.renderer->upload);
    assert(upload_chunk_count(f.renderer) == VELA_UPLOAD_MAX_CHUNKS);

    struct shm_buffer *second = new_shm(256, 64, 2);
    struct surface *b = attach(&f, second);
    assert(!own(b)->uploaded && own(b)->upload.source && !released(second));

    uint64_t before = f.renderer->last_point;
    struct vela_upload_progress progress = vela_renderer_continue_uploads(f.renderer);
    assert(progress.pending == 1 && progress.completed == 0);
    assert(!f.renderer->upload && f.renderer->last_point > before);

    // The previous staging is now submitted, so the pending buffer can be
    // copied on the next call without a render pass in between.
    progress = vela_renderer_continue_uploads(f.renderer);
    assert(progress.pending == 0 && progress.completed == 1 && released(second));
    frame(&f);
    assert(a->shown[0] == 1 && b->shown[0] == 1);
    drop_shm(first);
    drop_shm(second);
    fixture_finish(&f);
}

// With the GPU behind, staging still in flight counts: a frame copies no
// more than the budget, the next one nothing until the GPU is done, and what
// the GPU copies is what the CPU staged (no chunk refilled before its
// submission ran).
static void in_flight_staging_counts(void)
{
    struct fixture f;
    fixture_init(&f);
    // No drawing while the GPU is behind: a drawing on this fake target
    // makes the CPU wait for it (its implicit-sync import fails).
    gpu_lag = true;
    const int height = 3 * BUDGET / (256 * BPP);
    struct shm_buffer *big = new_shm(256, height, 1);
    struct surface *s = attach(&f, big);
    vela_renderer_continue_uploads(f.renderer);
    int pieces = piece_copies;
    assert(pieces > 0 && staging_bytes == BUDGET);
    assert(vela_renderer_continue_uploads(f.renderer).pending == 1); // nothing came back: no new piece
    assert(piece_copies == pieces && upload_chunk_count(f.renderer) == VELA_UPLOAD_MAX_CHUNKS);
    int rounds = 0;
    while (own(s)->upload.source) {
        assert(++rounds <= 4 && staging_bytes <= BUDGET);
        complete_gpu(f.renderer->last_point);
        vela_renderer_continue_uploads(f.renderer);
    }
    assert(rounds == 2);
    gpu_lag = false;
    complete_gpu(f.renderer->last_point);
    frame(&f);
    assert(s->shown[0] == 1); // the GPU's copy matches the buffer exactly
    drop_shm(big);
    fixture_finish(&f);
}

// A lost device stops the pending uploads. The renderer's teardown (here
// before the textures, which wlroots destroys later) then releases the
// clients' buffers and frees the images being filled, once; so does
// destroying a texture whose upload is pending (fixture_finish, the other
// order, in the other tests).
static void device_loss_releases_pending_uploads(void)
{
    struct fixture f;
    fixture_init(&f);
    const int height = 2 * BUDGET / (256 * BPP) + 30;
    struct shm_buffer *v1 = new_shm(256, height, 1);
    struct surface *s = attach(&f, v1);
    while (frame(&f).pending) {
    }
    struct shm_buffer *v2 = new_shm(256, height, 2);
    struct shm_buffer *v3 = new_shm(128, height, 3);
    struct surface *t = attach(&f, v3); // new and pending: into its own image
    commit(s, v2, NULL); // pending: into an image of its own
    frame(&f);
    f.vk.loss.lost = true;
    int pieces = piece_copies;
    struct vela_upload_progress progress = vela_renderer_continue_uploads(f.renderer);
    assert(progress.pending == 2 && piece_copies == pieces);
    assert(!texture_from_buffer(&f.renderer->base, &v1->base));
    pixman_region32_t one;
    pixman_region32_init_rect(&one, 0, 0, 1, 1);
    assert(!wlr_texture_update_from_buffer(s->texture, &v1->base, &one));
    pixman_region32_fini(&one);
    assert(!released(v2) && !released(v3));
    destroy(f.renderer);
    f.renderer = NULL;
    assert(released(v2) && released(v3));
    (void)t;
    drop_shm(v1);
    drop_shm(v2);
    drop_shm(v3);
    fixture_finish(&f);
}

// Once warmed, frames that upload in place, and frames that continue a
// deferred upload a client replaces every frame (with a small one waiting
// behind it), make no allocation through Vela's helpers.
static void uploads_allocate_nothing_once_warm(void)
{
    struct fixture f;
    fixture_init(&f);
    const int big_height = 2 * BUDGET / (256 * BPP) + 30;
    struct shm_buffer *small[2] = { new_shm(64, 32, 1), new_shm(64, 32, 2) };
    struct shm_buffer *big[2] = { new_shm(256, big_height, 1), new_shm(256, big_height, 2) };
    struct surface *a = attach(&f, small[0]);
    struct surface *b = attach(&f, big[0]);
    remember(a, small[1]); // the buffers they alternate, both acceptable
    remember(b, big[1]);
    while (frame(&f).pending) {
    }
    pixman_region32_t whole_small, whole_big;
    pixman_region32_init_rect(&whole_small, 0, 0, 64, 32);
    pixman_region32_init_rect(&whole_big, 0, 0, 256, (unsigned)big_height);
    for (int phase = 0; phase < 2; ++phase) {
        struct vela_alloc_count before = { 0 };
        uint64_t in_place = f.renderer->upload_stats.in_place;
        for (int i = 0; i < 40; ++i) {
            if (i == 8) before = vela_alloc_count_get();
            assert(wlr_texture_update_from_buffer(a->texture, &small[(i + 1) % 2]->base, &whole_small));
            if (phase) assert(wlr_texture_update_from_buffer(b->texture, &big[(i + 1) % 2]->base, &whole_big));
            frame(&f);
        }
        struct vela_alloc_count after = vela_alloc_count_get();
        printf("upload: %llu allocation(s) in 32 frames %s\n",
            (unsigned long long)(after.allocations - before.allocations),
            phase ? "of deferred uploads replaced every frame" : "of uploads in place");
        assert(after.allocations == before.allocations);
        assert(after.checks > before.checks);
        // Phase 0 copies in place; phase 1 defers both (oldest first).
        assert((f.renderer->upload_stats.in_place - in_place == 40) == !phase);
    }
    pixman_region32_fini(&whole_small);
    pixman_region32_fini(&whole_big);
    for (int i = 0; i < 2; ++i) {
        drop_shm(small[i]);
        drop_shm(big[i]);
    }
    fixture_finish(&f);
}

int main(void)
{
    extent_cap_shm();
    extent_cap_dmabuf();
    small_upload_is_in_place();
    oversized_buffer_stays_within_budget();
    several_surfaces_share_the_budget();
    deferred_update_switches_atomically();
    new_commit_replaces_pending_upload();
    competing_uploads_do_not_starve();
    blocked_deferred_upload_flushes_prior_commands();
    in_flight_staging_counts();
    device_loss_releases_pending_uploads();
    uploads_allocate_nothing_once_warm();
    printf("upload: all passed\n");
    return 0;
}
