// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Textures (render.h): dmabufs imported as they are, shared-memory buffers
// copied into an image of ours (only the changed part), and pixel readback for
// captures.

#include "render/render.h"

#include "util.h"

#include <assert.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>

static const struct wlr_texture_impl texture_impl;
static const struct wlr_addon_interface texture_addon_impl;

struct vela_texture *vela_texture_from_wlr_texture(struct wlr_texture *texture)
{
    return texture && texture->impl == &texture_impl ? (struct vela_texture *)texture : NULL;
}

bool vela_texture_is_opaque(struct wlr_texture *wlr)
{
    struct vela_texture *texture = vela_texture_from_wlr_texture(wlr);
    return texture && !texture->format->alpha;
}

// The texture goes away: its Vulkan resources when the GPU is done with them,
// the struct at once.
static void retire(struct vela_texture *texture)
{
    if (!texture->renderer) {
        free(texture); // the renderer is already closed: nothing left to free
        return;
    }
    if (texture->buffer) {
        wlr_addon_finish(&texture->addon);
        texture->buffer = NULL;
    }
    if (texture->image) {
        vela_renderer_retire_image(texture->renderer, texture->image, texture->view, texture->memory);
    }
    wl_list_remove(&texture->link);
    free(texture);
}

static void handle_buffer_destroy(struct wlr_addon *addon)
{
    struct vela_texture *texture = wl_container_of(addon, texture, addon);
    retire(texture);
}

static const struct wlr_addon_interface texture_addon_impl = {
    .name = "vela-texture",
    .destroy = handle_buffer_destroy,
};

static void texture_destroy(struct wlr_texture *wlr)
{
    struct vela_texture *texture = (struct vela_texture *)wlr;
    --texture->refs;
    struct wlr_buffer *buffer = texture->buffer;
    if (buffer && texture->refs > 0) {
        wlr_buffer_unlock(buffer);
        return;
    }
    // A dmabuf we no longer use doesn't stay imported, even if the app will
    // reuse it. RADV puts every imported memory in all our GPU submissions,
    // and the kernel makes each submission wait for the dmabufs' write fences
    // under implicit sync: a cached buffer the app is redrawing would stall
    // every frame of ours until its GPU finished (docs/renderer.md §7.3). The
    // resources are freed when the GPU has finished reading them; unlocking
    // the buffer can destroy it, so it comes last.
    retire(texture);
    if (buffer) {
        wlr_buffer_unlock(buffer);
    }
}

static bool create_view(struct vela_texture *texture)
{
    const VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = texture->format->unorm,
        // X... formats have an undefined fourth channel: it counts as 1.
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            texture->format->alpha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE,
        },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkDevice device = texture->renderer->vk->device;
    return vkCreateImageView(device, &info, NULL, &texture->view) == VK_SUCCESS;
}

static struct vela_texture *new_texture(struct vela_renderer *renderer, const struct vela_pixel_format *format,
    int width, int height)
{
    struct vela_texture *texture = calloc(1, sizeof(*texture));
    wlr_texture_init(&texture->base, vela_renderer_wlr(renderer), &texture_impl, (uint32_t)width, (uint32_t)height);
    texture->renderer = renderer;
    texture->format = format;
    texture->dmabuf_fd = -1;
    texture->refs = 1;
    wl_list_insert(&renderer->textures, &texture->link);
    return texture;
}

// --------------------------------------------------------------- dmabuf --

static struct wlr_texture *import_dmabuf(struct vela_renderer *renderer, struct wlr_buffer *buffer,
    const struct wlr_dmabuf_attributes *dmabuf)
{
    // Already imported and still in use (the same buffer in several places).
    struct wlr_addon *addon = wlr_addon_find(&buffer->addons, renderer, &texture_addon_impl);
    if (addon) {
        struct vela_texture *texture = wl_container_of(addon, texture, addon);
        ++texture->refs;
        wlr_buffer_lock(buffer);
        return &texture->base;
    }

    struct vela_vulkan *vk = renderer->vk;
    const struct vela_pixel_format *format = vela_pixel_format_from_drm(dmabuf->format);
    if (!format || !wlr_drm_format_set_has(&vk->texture_formats, dmabuf->format, dmabuf->modifier)) {
        wlr_log(WLR_DEBUG, "Texture: dmabuf 0x%08x (modifier 0x%" PRIx64 ") not supported", dmabuf->format,
            dmabuf->modifier);
        return NULL;
    }
    struct vela_texture *texture = new_texture(renderer, format, dmabuf->width, dmabuf->height);
    if (!vela_vulkan_import_dmabuf(vk, dmabuf, format->unorm,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &texture->image, &texture->memory)
        || !create_view(texture)) {
        wlr_log(WLR_ERROR, "Texture: can't import the dmabuf");
        retire(texture);
        return NULL;
    }
    texture->foreign = true;
    texture->dmabuf_fd = dmabuf->fd[0];
    texture->buffer = wlr_buffer_lock(buffer);
    wlr_addon_init(&texture->addon, &buffer->addons, renderer, &texture_addon_impl);
    return &texture->base;
}

// ----------------------------------------------------- shared memory --

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
    VkAccessFlags2 dst_access)
{
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// Copies the `rects` zones of the `data` pixels into the image.
static bool upload(struct vela_texture *texture, const uint8_t *data, size_t stride, const pixman_box32_t *rects,
    int count)
{
    struct vela_renderer *renderer = texture->renderer;
    uint32_t bpp = texture->format->bytes_per_pixel;
    VkCommandBuffer cmd = vela_renderer_upload_commands(renderer);

    image_barrier(cmd, texture->image,
        texture->uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT);

    bool ok = true;
    for (int i = 0; i < count; ++i) {
        const pixman_box32_t *r = &rects[i];
        uint32_t width = (uint32_t)(r->x2 - r->x1);
        uint32_t height = (uint32_t)(r->y2 - r->y1);
        if (width == 0 || height == 0) {
            continue;
        }
        size_t row_bytes = (size_t)width * bpp;
        struct vela_staging staging;
        if (!vela_renderer_stage(renderer, row_bytes * height, &staging)) {
            ok = false;
            break;
        }
        const uint8_t *src = data + (size_t)r->y1 * stride + (size_t)r->x1 * bpp;
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(staging.data + y * row_bytes, src + y * stride, row_bytes);
        }
        const VkBufferImageCopy copy = {
            .bufferOffset = staging.offset,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { r->x1, r->y1, 0 },
            .imageExtent = { width, height, 1 },
        };
        vkCmdCopyBufferToImage(cmd, staging.buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    image_barrier(cmd, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    texture->uploaded = true;
    return ok;
}

static bool shm_format_supported(const struct vela_vulkan *vk, uint32_t drm)
{
    for (int i = 0; i < vk->shm_format_count; ++i) {
        if (vk->shm_formats[i] == drm) {
            return true;
        }
    }
    return false;
}

static struct wlr_texture *upload_buffer(struct vela_renderer *renderer, struct wlr_buffer *buffer)
{
    void *data = NULL;
    uint32_t drm_format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &drm_format, &stride)) {
        return NULL;
    }
    struct vela_vulkan *vk = renderer->vk;
    const struct vela_pixel_format *format = vela_pixel_format_from_drm(drm_format);
    if (!format || !shm_format_supported(vk, drm_format)) {
        wlr_buffer_end_data_ptr_access(buffer);
        wlr_log(WLR_DEBUG, "Texture: format 0x%08x not supported", drm_format);
        return NULL;
    }

    struct vela_texture *texture = new_texture(renderer, format, buffer->width, buffer->height);
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format->unorm,
        .extent = { (uint32_t)buffer->width, (uint32_t)buffer->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    bool ok = vkCreateImage(vk->device, &image_info, NULL, &texture->image) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(vk->device, texture->image, &requirements);
        const VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex
            = vela_vulkan_memory_type(vk, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        ok = alloc_info.memoryTypeIndex != UINT32_MAX
            && vkAllocateMemory(vk->device, &alloc_info, NULL, &texture->memory) == VK_SUCCESS
            && vkBindImageMemory(vk->device, texture->image, texture->memory, 0) == VK_SUCCESS && create_view(texture);
    }
    if (ok) {
        const pixman_box32_t whole = { 0, 0, buffer->width, buffer->height };
        ok = upload(texture, data, stride, &whole, 1);
    }
    wlr_buffer_end_data_ptr_access(buffer);
    if (!ok) {
        wlr_log(WLR_ERROR, "Texture: can't upload a %dx%d buffer", buffer->width, buffer->height);
        if (!texture->memory && texture->image) {
            vkDestroyImage(vk->device, texture->image, NULL);
            texture->image = VK_NULL_HANDLE;
        }
        retire(texture);
        return NULL;
    }
    return &texture->base;
}

// The app drew again (in this buffer or another of the same format and size):
// only the changed part is copied again.
static bool update_from_buffer(struct wlr_texture *wlr, struct wlr_buffer *buffer, const pixman_region32_t *damage)
{
    struct vela_texture *texture = (struct vela_texture *)wlr;
    if (texture->foreign || buffer->width != (int)wlr->width || buffer->height != (int)wlr->height) {
        return false;
    }
    void *data = NULL;
    uint32_t drm_format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &drm_format, &stride)) {
        return false;
    }
    bool ok = drm_format == texture->format->drm;
    if (ok) {
        pixman_region32_t region;
        pixman_region32_init(&region);
        pixman_region32_intersect_rect(&region, damage, 0, 0, (unsigned)buffer->width, (unsigned)buffer->height);
        int count = 0;
        const pixman_box32_t *rects = pixman_region32_rectangles(&region, &count);
        // Too many little pieces: better a single copy of the rectangle
        // containing them.
        if (count > 16) {
            rects = pixman_region32_extents(&region);
            count = 1;
        }
        ok = upload(texture, data, stride, rects, count);
        pixman_region32_fini(&region);
    }
    wlr_buffer_end_data_ptr_access(buffer);
    return ok;
}

// ------------------------------------------------------- pixel readback --

// Record the identical image/host barriers for synchronous and deferred reads.
static VkCommandBuffer record_readback(struct vela_texture *texture, const struct wlr_box *box,
    const struct vela_staging *staging, size_t row_bytes)
{
    struct vela_renderer *renderer = texture->renderer;
    struct vela_vulkan *vk = renderer->vk;
    VkCommandBuffer cmd = vela_renderer_begin_commands(renderer);
    if (!cmd) {
        return VK_NULL_HANDLE;
    }
    VkImageLayout rest_layout = texture->foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    uint32_t foreign_family = texture->foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    uint32_t own_family = texture->foreign ? vk->queue_family : VK_QUEUE_FAMILY_IGNORED;
    const VkImageMemoryBarrier2 before = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = texture->foreign ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = texture->foreign ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = rest_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = foreign_family,
        .dstQueueFamilyIndex = own_family,
        .image = texture->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    const VkDependencyInfo before_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &before,
    };
    vkCmdPipelineBarrier2(cmd, &before_dependency);
    const VkBufferImageCopy copy = {
        .bufferOffset = staging->offset,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { box->x, box->y, 0 },
        .imageExtent = { (uint32_t)box->width, (uint32_t)box->height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->buffer, 1, &copy);
    const VkImageMemoryBarrier2 after = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask = texture->foreign ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = rest_layout,
        .srcQueueFamilyIndex = own_family,
        .dstQueueFamilyIndex = foreign_family,
        .image = texture->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    // The copy into the buffer must be visible to the CPU.
    const VkBufferMemoryBarrier2 host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging->buffer,
        .offset = staging->offset,
        .size = row_bytes * (size_t)box->height,
    };
    const VkDependencyInfo after_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &host_barrier,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &after,
    };
    vkCmdPipelineBarrier2(cmd, &after_dependency);

    return cmd;
}

enum readback_alpha { READBACK_ALPHA_NONE, READBACK_ALPHA_8, READBACK_ALPHA_2, READBACK_ALPHA_HALF };

static enum readback_alpha opaque_alpha_for(const struct vela_pixel_format *src, const struct vela_pixel_format *dst)
{
    if (src->alpha || !dst->alpha) return READBACK_ALPHA_NONE;
    switch (dst->unorm) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
        return READBACK_ALPHA_8;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return READBACK_ALPHA_2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return READBACK_ALPHA_HALF;
    default:
        return READBACK_ALPHA_NONE;
    }
}

static void copy_row(uint8_t *dst, const uint8_t *src, size_t bytes, enum readback_alpha alpha)
{
    memcpy(dst, src, bytes);
    if (alpha == READBACK_ALPHA_HALF) {
        const uint16_t one = 0x3c00; // IEEE 754 binary16 1.0
        for (size_t x = 6; x < bytes; x += 8) memcpy(dst + x, &one, sizeof(one));
    } else if (alpha != READBACK_ALPHA_NONE) {
        const uint32_t mask = alpha == READBACK_ALPHA_8 ? UINT32_C(0xff000000) : UINT32_C(0xc0000000);
        // Packed 10-bit pixels share the last byte with a color channel.
        // Use native pixel words and preserve every RGB bit, including on an
        // unaligned client row.
        for (size_t x = 0; x < bytes; x += sizeof(mask)) {
            uint32_t pixel;
            memcpy(&pixel, dst + x, sizeof(pixel));
            pixel |= mask;
            memcpy(dst + x, &pixel, sizeof(pixel));
        }
    }
}

// Recording failed before submission: release this readback-only chunk's
// reservation. Other pending CPU readers still prevent reuse of its storage.
static void discard_readback_staging(struct vela_renderer *renderer, const struct vela_staging *staging)
{
    for (int i = 0; i < renderer->staging_count; ++i) {
        struct staging_chunk *chunk = &renderer->staging[i];
        if (chunk->buffer == staging->buffer) {
            assert(chunk->readback && chunk->point == UINT64_MAX);
            chunk->point = renderer->last_point;
            return;
        }
    }
    abort();
}

// wlroots' texture API remains synchronous for legacy screencopy and other
// consumers. Never return from it while the caller's pixels are unfinished.
static bool read_pixels(struct wlr_texture *wlr, const struct wlr_texture_read_pixels_options *options)
{
    struct vela_texture *texture = (struct vela_texture *)wlr;
    struct vela_renderer *renderer = texture->renderer;
    const struct vela_pixel_format *wanted = vela_pixel_format_from_drm(options->format);
    if (!wanted || wanted->unorm != texture->format->unorm) {
        return false;
    }
    struct wlr_box box;
    wlr_texture_read_pixels_options_get_src_box(options, wlr, &box);
    size_t row_bytes = (size_t)box.width * texture->format->bytes_per_pixel;
    struct vela_staging staging;
    if (box.width <= 0 || box.height <= 0
        || !vela_renderer_stage_readback(renderer, row_bytes * (size_t)box.height, &staging)) {
        return false;
    }
    VkCommandBuffer cmd = record_readback(texture, &box, &staging, row_bytes);
    if (!cmd) {
        discard_readback_staging(renderer, &staging);
        return false;
    }

    // A dmabuf: first wait for whoever is writing it; afterwards, whoever
    // wants to write it will wait for our read.
    int waits[1];
    int wait_count = 0;
    if (texture->foreign) {
        struct dma_buf_export_sync_file request = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
        if (ioctl(texture->dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) == 0 && request.fd >= 0) {
            waits[wait_count++] = request.fd;
        }
    }
    int release_fd = -1;
    uint64_t point = vela_renderer_submit(renderer, cmd, waits, wait_count, texture->foreign ? &release_fd : NULL);
    if (point == 0) {
        return false;
    }
    if (release_fd >= 0) {
        struct dma_buf_import_sync_file request = { .flags = DMA_BUF_SYNC_READ, .fd = release_fd };
        ioctl(texture->dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &request);
        close(release_fd);
    }
    vela_renderer_wait(renderer, point);

    uint8_t *dst = wlr_texture_read_pixel_options_get_data(options);
    for (int y = 0; y < box.height; ++y) {
        copy_row(dst + (size_t)y * options->stride, staging.data + (size_t)y * row_bytes,
            row_bytes, opaque_alpha_for(texture->format, wanted));
    }
    return true;
}

// There is one timer and one aggregate copy budget, even with several clients.
// Timers yield to epoll between slices; re-adding an idle callback would drain
// all slices in one event-loop dispatch and recreate the original stall.
#define READBACK_MAX_BYTES (128u * 1024u * 1024u)
#define READBACK_SLICE_BYTES (512u * 1024u)
#define READBACK_SLICE_NS 500000
#define READBACK_TIMEOUT_NS (5LL * VELA_NS_PER_SEC)

struct vela_readback {
    struct vela_renderer *renderer;
    struct wlr_texture *texture;
    struct wlr_buffer *dst;
    struct vela_staging staging;
    uint64_t point;
    size_t row_bytes, bytes, stride;
    uint32_t format;
    int row;
    enum readback_alpha alpha;
    int64_t started;
    void (*done)(void *data, bool ok);
    void *data;
};

static void pin_staging(struct vela_renderer *renderer, VkBuffer buffer, bool pin)
{
    for (int i = 0; i < renderer->staging_count; ++i) {
        struct staging_chunk *chunk = &renderer->staging[i];
        if (chunk->buffer == buffer) {
            if (pin) {
                ++chunk->readers;
            } else {
                assert(chunk->readers > 0);
                --chunk->readers;
            }
            return;
        }
    }
    abort(); // a reserved slice must never have been collected
}

void vela_readback_cancel(struct vela_readback *readback)
{
    if (!readback) {
        return;
    }
    struct vela_renderer *renderer = readback->renderer;
    const size_t count = renderer->readback_count;
    if (count == 0 || count > VELA_READBACK_MAX_JOBS) {
        abort(); // the queue must contain this live request
    }
    size_t index = 0;
    while (index < count && renderer->readbacks[index] != readback) ++index;
    if (index == count) abort();
    for (size_t i = index + 1; i < count; ++i) {
        renderer->readbacks[i - 1] = renderer->readbacks[i];
    }
    renderer->readbacks[count - 1] = NULL;
    renderer->readback_count = count - 1;
    if (index < renderer->readback_next) --renderer->readback_next;
    if (renderer->readback_next >= renderer->readback_count) renderer->readback_next = 0;
    if (readback->done) ++renderer->readback_cancelled;
    renderer->readback_bytes -= readback->bytes;
    pin_staging(renderer, readback->staging.buffer, false);
    // GPU lifetime still follows the submission point, even on cancellation.
    wlr_texture_destroy(readback->texture);
    wlr_buffer_unlock(readback->dst);
    free(readback);
    if (!renderer->readback_count && renderer->readback_timer) {
        wl_event_source_remove(renderer->readback_timer);
        renderer->readback_timer = NULL;
    }
}

static void finish_readback(struct vela_readback *readback, bool ok)
{
    void (*done)(void *, bool) = readback->done;
    void *data = readback->data;
    if (ok) ++readback->renderer->readback_completed;
    else ++readback->renderer->readback_failed;
    readback->done = NULL;
    vela_readback_cancel(readback);
    done(data, ok);
}

void vela_renderer_cancel_readbacks(struct vela_renderer *renderer)
{
    while (renderer->readback_count > 0) {
        finish_readback(renderer->readbacks[renderer->readback_count - 1], false);
    }
}

struct vela_readback_stats vela_renderer_readback_stats(const struct vela_renderer *renderer)
{
    return (struct vela_readback_stats) { renderer->readback_count, renderer->readback_bytes,
        renderer->readback_completed, renderer->readback_cancelled, renderer->readback_failed,
        renderer->readback_rejected };
}

static int dispatch_readback(void *data)
{
    struct vela_renderer *renderer = data;
    // Arm before invoking terminal callbacks: they may remove this source.
    // Handling an arm failure here never traverses a just-completed request.
    if (wl_event_source_timer_update(renderer->readback_timer, 1) < 0) {
        vela_renderer_cancel_readbacks(renderer);
        return 0;
    }
    struct vela_readback *readback = renderer->readbacks[renderer->readback_next];
    // Round robin; one bounded slice across all requests per timer tick.
    renderer->readback_next = (renderer->readback_next + 1) % renderer->readback_count;
    uint64_t completed = 0;
    VkResult result = vkGetSemaphoreCounterValue(renderer->vk->device, renderer->timeline, &completed);
    if (result != VK_SUCCESS || vela_now_ns() - readback->started > READBACK_TIMEOUT_NS) {
        finish_readback(readback, false);
    } else if (completed >= readback->point) {
        void *pixels = NULL;
        uint32_t format = 0;
        size_t stride = 0;
        bool ok = wlr_buffer_begin_data_ptr_access(readback->dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
            &pixels, &format, &stride);
        if (ok) {
            ok = format == readback->format && stride == readback->stride;
            if (ok) {
                int64_t start = vela_now_ns();
                size_t copied = 0;
                do {
                    copy_row((uint8_t *)pixels + (size_t)readback->row * stride,
                        readback->staging.data + (size_t)readback->row * readback->row_bytes,
                        readback->row_bytes, readback->alpha);
                    copied += readback->row_bytes;
                    ++readback->row;
                } while (readback->row < readback->dst->height && copied < READBACK_SLICE_BYTES
                    && vela_now_ns() - start < READBACK_SLICE_NS);
            }
            // Every wl_shm access (including SIGBUS protection) stays on this
            // thread. No worker dereferences a Wayland client's mapping.
            wlr_buffer_end_data_ptr_access(readback->dst);
        }
        if (!ok || readback->row == readback->dst->height) {
            finish_readback(readback, ok);
        }
    }
    return 0;
}

struct vela_readback *vela_renderer_readback(struct vela_renderer *renderer, struct wl_event_loop *loop,
    struct wlr_buffer *src, struct wlr_buffer *dst, void (*done)(void *, bool), void *data)
{
    if (src->width != dst->width || src->height != dst->height || src->width <= 0 || src->height <= 0) {
        return NULL;
    }
    if (renderer->readback_count >= VELA_READBACK_MAX_JOBS) {
        ++renderer->readback_rejected;
        return NULL;
    }
    void *pixels = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &pixels, &format, &stride)) {
        return NULL;
    }
    wlr_buffer_end_data_ptr_access(dst);
    const struct vela_pixel_format *wanted = vela_pixel_format_from_drm(format);
    if (!wanted || (size_t)src->width > SIZE_MAX / wanted->bytes_per_pixel) {
        return NULL;
    }
    size_t row_bytes = (size_t)src->width * wanted->bytes_per_pixel;
    if (stride < row_bytes || (size_t)src->height > READBACK_MAX_BYTES / row_bytes) {
        return NULL;
    }
    size_t bytes = row_bytes * (size_t)src->height;
    if (bytes > READBACK_MAX_BYTES - renderer->readback_bytes) {
        ++renderer->readback_rejected;
        return NULL;
    }
    struct wlr_texture *wlr = wlr_texture_from_buffer(&renderer->base, src);
    struct vela_texture *texture = vela_texture_from_wlr_texture(wlr);
    if (!texture || wanted->unorm != texture->format->unorm) {
        if (wlr) wlr_texture_destroy(wlr);
        return NULL;
    }
    struct vela_readback *readback = calloc(1, sizeof(*readback));
    if (!readback) {
        wlr_texture_destroy(wlr);
        return NULL;
    }
    if (!renderer->readback_timer) {
        renderer->readback_timer = wl_event_loop_add_timer(loop, dispatch_readback, renderer);
    }
    if (!renderer->readback_timer || !vela_renderer_stage_readback(renderer, bytes, &readback->staging)) {
        if (renderer->readback_timer && !renderer->readback_count) {
            wl_event_source_remove(renderer->readback_timer);
            renderer->readback_timer = NULL;
        }
        free(readback);
        wlr_texture_destroy(wlr);
        return NULL;
    }
    // Unlike the synchronous pool's GPU reservation, this CPU reservation
    // survives GPU completion, collection, and further renderer submissions.
    pin_staging(renderer, readback->staging.buffer, true);
    const struct wlr_box box = { 0, 0, src->width, src->height };
    VkCommandBuffer cmd = record_readback(texture, &box, &readback->staging, row_bytes);
    if (!cmd) discard_readback_staging(renderer, &readback->staging);
    // Contract: src was drawn by THIS renderer (output/cursor/window swapchain),
    // never a directly scanned-out client buffer. Its producer and this copy
    // have explicit GPU timeline dependencies in both directions. Keeping the
    // texture locked prevents normal reuse; the dependency on this copy also
    // protects reuse after cancellation. No CPU or external fence wait is used.
    uint64_t point = cmd ? vela_renderer_submit_readback(renderer, cmd) : 0;
    if (!point) {
        pin_staging(renderer, readback->staging.buffer, false);
        if (!renderer->readback_count) {
            wl_event_source_remove(renderer->readback_timer);
            renderer->readback_timer = NULL;
        }
        free(readback);
        wlr_texture_destroy(wlr);
        return NULL;
    }
    readback->renderer = renderer;
    readback->texture = wlr;
    readback->dst = wlr_buffer_lock(dst);
    readback->point = point;
    readback->row_bytes = row_bytes;
    readback->bytes = bytes;
    readback->stride = stride;
    readback->format = format;
    readback->alpha = opaque_alpha_for(texture->format, wanted);
    readback->started = vela_now_ns();
    readback->done = done;
    readback->data = data;
    renderer->readbacks[renderer->readback_count++] = readback;
    renderer->readback_bytes += bytes;
    if (wl_event_source_timer_update(renderer->readback_timer, 1) < 0) {
        vela_readback_cancel(readback);
        return NULL;
    }
    return readback;
}

static uint32_t preferred_read_format(struct wlr_texture *wlr)
{
    return ((struct vela_texture *)wlr)->format->drm;
}

static const struct wlr_texture_impl texture_impl = {
    .update_from_buffer = update_from_buffer,
    .read_pixels = read_pixels,
    .preferred_read_format = preferred_read_format,
    .destroy = texture_destroy,
};

struct wlr_texture *vela_texture_create(struct vela_renderer *renderer, struct wlr_buffer *buffer)
{
    struct wlr_dmabuf_attributes dmabuf;
    if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
        return import_dmabuf(renderer, buffer, &dmabuf);
    }
    return upload_buffer(renderer, buffer);
}

void vela_texture_release(struct vela_texture *texture)
{
    VkDevice device = texture->renderer->vk->device;
    if (texture->image) {
        vkDestroyImageView(device, texture->view, NULL);
        vkDestroyImage(device, texture->image, NULL);
        vkFreeMemory(device, texture->memory, NULL);
        texture->view = VK_NULL_HANDLE;
        texture->image = VK_NULL_HANDLE;
        texture->memory = VK_NULL_HANDLE;
    }
    wl_list_remove(&texture->link);
    wl_list_init(&texture->link);
    if (texture->buffer) {
        wlr_addon_finish(&texture->addon);
        texture->buffer = NULL;
        if (texture->refs <= 0) {
            free(texture); // only the buffer cache used it
            return;
        }
    }
    texture->renderer = NULL; // wlroots will destroy it later
}
