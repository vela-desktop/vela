// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Textures (render.h): dmabufs imported as they are, shared-memory buffers
// copied into an image of ours (only the changed part), and pixel readback for
// captures.

#include "render/render.h"
#include "render/readback_copy.h"

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

// For captures (screencopy, previews): copies a zone of the texture to the
// given memory. The CPU waits for the GPU.
static bool read_pixels(struct wlr_texture *wlr, const struct wlr_texture_read_pixels_options *options)
{
    struct vela_texture *texture = (struct vela_texture *)wlr;
    struct vela_renderer *renderer = texture->renderer;
    struct vela_vulkan *vk = renderer->vk;

    const struct vela_pixel_format *wanted = vela_pixel_format_from_drm(options->format);
    if (!wanted || wanted->unorm != texture->format->unorm) {
        wlr_log(WLR_ERROR, "Texture: reading in format 0x%08x not supported", options->format);
        return false;
    }
    // From X... to A...: the fourth channel must be set to "opaque".
    bool force_opaque = wanted->alpha && !texture->format->alpha && wanted->bytes_per_pixel == 4
        && (wanted->unorm == VK_FORMAT_B8G8R8A8_UNORM || wanted->unorm == VK_FORMAT_R8G8B8A8_UNORM);

    struct wlr_box box;
    wlr_texture_read_pixels_options_get_src_box(options, wlr, &box);
    uint32_t bpp = texture->format->bytes_per_pixel;
    size_t row_bytes = (size_t)box.width * bpp;
    struct vela_staging staging;
    if (box.width <= 0 || box.height <= 0 || !vela_renderer_stage_readback(renderer, row_bytes * (size_t)box.height, &staging)) {
        return false;
    }

    VkCommandBuffer cmd = vela_renderer_begin_commands(renderer);
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
        .bufferOffset = staging.offset,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { box.x, box.y, 0 },
        .imageExtent = { (uint32_t)box.width, (uint32_t)box.height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
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
        .buffer = staging.buffer,
        .offset = staging.offset,
        .size = row_bytes * (size_t)box.height,
    };
    const VkDependencyInfo after_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &host_barrier,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &after,
    };
    vkCmdPipelineBarrier2(cmd, &after_dependency);

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
    // Experiment only: reduce the HOST_CACHED NVIDIA readback memcpy wall time
    // without touching Vulkan synchronization or wlroots' synchronous return
    // contract. Every worker is joined before this call returns.
    unsigned copy_workers = 1;
    const char *workers_env = getenv("VELA_READBACK_COPY_THREADS");
    if (workers_env && workers_env[0] >= '2' && workers_env[0] <= '4' && workers_env[1] == '\0') {
        copy_workers = (unsigned)(workers_env[0] - '0');
    }
    vela_readback_copy(dst, options->stride, staging.data, row_bytes, row_bytes,
        (size_t)box.height, force_opaque, copy_workers);
    return true;
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
