// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The renderer: GPU submissions with a timeline, what must be destroyed once
// the GPU is done, staging memory for uploads, pipelines, blur images, GPU
// times and the wlr_renderer wlroots sees.

#include "render/render.h"
#include "sync.h"

#include "util.h"

#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

static const uint32_t quad_vert[] = {
#include "quad.vert.spv.inc"
};
static const uint32_t texture_frag[] = {
#include "texture.frag.spv.inc"
};
static const uint32_t rect_frag[] = {
#include "rect.frag.spv.inc"
};
static const uint32_t shadow_frag[] = {
#include "shadow.frag.spv.inc"
};
static const uint32_t blur_down_frag[] = {
#include "blur_down.frag.spv.inc"
};
static const uint32_t blur_up_frag[] = {
#include "blur_up.frag.spv.inc"
};
static const uint32_t blur_mix_frag[] = {
#include "blur_mix.frag.spv.inc"
};

#define STAGING_CHUNK_SIZE (8 * 1024 * 1024)
#define IDLE_STAGING_CHUNKS_KEPT 2

static const struct wlr_renderer_impl renderer_impl;
static const struct wlr_addon_interface target_addon_impl;

struct wlr_renderer *vela_renderer_wlr(struct vela_renderer *renderer)
{
    return &renderer->base;
}

int vela_renderer_render_fd(const struct vela_renderer *renderer)
{
    return renderer->vk->render_fd;
}

const struct wlr_drm_format_set *vela_renderer_render_formats(const struct vela_renderer *renderer)
{
    return &renderer->vk->render_formats;
}

const struct wlr_drm_format_set *vela_renderer_texture_formats(const struct vela_renderer *renderer)
{
    return &renderer->vk->texture_formats;
}

struct wlr_drm_syncobj_timeline *vela_renderer_sync_timeline(const struct vela_renderer *renderer)
{
    return renderer->sync_timeline;
}

// -------------------------------------------------------------- creation --

static bool create_samplers_and_layout(struct vela_renderer *r)
{
    VkDevice device = r->vk->device;
    // Nearest for 1:1 copies (exact sharpness, §3.3); bilinear for the rest
    // (bicubic is in the shader).
    for (int linear = 0; linear < 2; ++linear) {
        const VkSamplerCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.25f,
        };
        if (vkCreateSampler(device, &info, NULL, linear ? &r->linear : &r->nearest) != VK_SUCCESS) {
            return false;
        }
    }

    // One texture per draw (two for the blur: the panel and the blurred
    // background), bound with push descriptors (Vulkan 1.4): no descriptor
    // pools to manage.
    const VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    const VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(device, &set_info, NULL, &r->set_layout) != VK_SUCCESS) {
        return false;
    }
    const VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(struct vela_quad_push),
    };
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    return vkCreatePipelineLayout(device, &layout_info, NULL, &r->layout) == VK_SUCCESS;
}

static bool init(struct vela_renderer *r)
{
    struct vela_vulkan *vk = r->vk;
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->queue_family,
    };
    if (vkCreateCommandPool(vk->device, &pool_info, NULL, &r->pool) != VK_SUCCESS) {
        return false;
    }
    const VkSemaphoreTypeCreateInfo timeline_type = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    const VkSemaphoreCreateInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_type,
    };
    if (vkCreateSemaphore(vk->device, &timeline_info, NULL, &r->timeline) != VK_SUCCESS) {
        return false;
    }

    // Explicit sync with apps (linux-drm-syncobj-v1): it needs a kernel
    // timeline to signal release points on, and sync_files to pass fences
    // between Vulkan and the kernel.
    uint64_t syncobj_timeline = 0;
    if (vk->sync_file && drmGetCap(vk->render_fd, DRM_CAP_SYNCOBJ_TIMELINE, &syncobj_timeline) == 0
        && syncobj_timeline) {
        r->sync_timeline = wlr_drm_syncobj_timeline_create(vk->render_fd);
    }
    r->base.features.timeline = r->sync_timeline != NULL;
    if (!r->sync_timeline) {
        wlr_log(WLR_INFO, "Renderer: no syncobj timeline, no explicit sync with apps");
    }

    // GPU timestamps: two per measured drawing (start and end).
    if (vk->timestamp_period > 0.0f) {
        const VkQueryPoolCreateInfo query_info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = VELA_TIMING_SLOTS * 2,
        };
        if (vkCreateQueryPool(vk->device, &query_info, NULL, &r->query_pool) != VK_SUCCESS) {
            r->query_pool = VK_NULL_HANDLE;
        }
    }

    if (!create_samplers_and_layout(r)) {
        return false;
    }
    r->vert = vela_vulkan_shader(vk, quad_vert, sizeof(quad_vert));
    r->texture_frag = vela_vulkan_shader(vk, texture_frag, sizeof(texture_frag));
    r->rect_frag = vela_vulkan_shader(vk, rect_frag, sizeof(rect_frag));
    r->shadow_frag = vela_vulkan_shader(vk, shadow_frag, sizeof(shadow_frag));
    r->blur_down_frag = vela_vulkan_shader(vk, blur_down_frag, sizeof(blur_down_frag));
    r->blur_up_frag = vela_vulkan_shader(vk, blur_up_frag, sizeof(blur_up_frag));
    r->blur_mix_frag = vela_vulkan_shader(vk, blur_mix_frag, sizeof(blur_mix_frag));
    return r->vert && r->texture_frag && r->rect_frag && r->shadow_frag && r->blur_down_frag && r->blur_up_frag
        && r->blur_mix_frag;
}

static void destroy(struct vela_renderer *r);

struct vela_renderer *vela_renderer_create(struct vela_vulkan *vk)
{
    struct vela_renderer *r = calloc(1, sizeof(*r));
    wlr_renderer_init(&r->base, &renderer_impl, WLR_BUFFER_CAP_DMABUF);
    r->vk = vk;
    wl_list_init(&r->targets);
    wl_list_init(&r->textures);
    for (int i = 0; i < vk->shm_format_count; ++i) {
        wlr_drm_format_set_add(&r->shm_formats, vk->shm_formats[i], DRM_FORMAT_MOD_LINEAR);
    }
    if (!init(r)) {
        destroy(r);
        return NULL;
    }
    return r;
}

static void destroy_blur_image(VkDevice device, struct vela_blur_image *image)
{
    if (image->view) {
        vkDestroyImageView(device, image->view, NULL);
    }
    if (image->image) {
        vkDestroyImage(device, image->image, NULL);
    }
    if (image->memory) {
        vkFreeMemory(device, image->memory, NULL);
    }
    memset(image, 0, sizeof(*image));
}

static void destroy_target(struct vela_target *target);

static void destroy(struct vela_renderer *r)
{
    VkDevice device = r->vk->device;
    if (device) {
        vkDeviceWaitIdle(device);
    }
    vela_renderer_collect(r);

    // Buffers still alive (such as the outputs'): we detach from all of them.
    // (The renderer is destroyed only with VELA_VULKAN_VALIDATION: see
    // vela_server_destroy.)
    struct vela_target *target, *target_tmp;
    wl_list_for_each_safe (target, target_tmp, &r->targets, link) {
        destroy_target(target);
    }
    // Textures still alive lose their Vulkan resources; those wlroots no
    // longer uses vanish entirely, wlroots will destroy the others.
    struct vela_texture *texture, *texture_tmp;
    wl_list_for_each_safe (texture, texture_tmp, &r->textures, link) {
        vela_texture_release(texture);
    }
    vela_renderer_collect(r);

    for (int i = 0; i < r->staging_count; ++i) {
        vkDestroyBuffer(device, r->staging[i].buffer, NULL);
        vkFreeMemory(device, r->staging[i].memory, NULL);
    }
    for (int i = 0; i < r->free_wait_count; ++i) {
        vkDestroySemaphore(device, r->free_waits[i], NULL);
    }
    for (int i = 0; i < r->free_release_count; ++i) {
        vkDestroySemaphore(device, r->free_releases[i], NULL);
    }
    for (int i = 0; i < r->pipeline_count; ++i) {
        vkDestroyPipeline(device, r->pipelines[i].pipeline, NULL);
    }
    VkShaderModule shaders[] = { r->vert, r->texture_frag, r->rect_frag, r->shadow_frag, r->blur_down_frag,
        r->blur_up_frag, r->blur_mix_frag };
    for (size_t i = 0; i < sizeof(shaders) / sizeof(shaders[0]); ++i) {
        vkDestroyShaderModule(device, shaders[i], NULL);
    }
    for (int level = 0; level < VELA_BLUR_LEVELS; ++level) {
        destroy_blur_image(device, &r->blur[level]);
    }
    vkDestroyPipelineLayout(device, r->layout, NULL);
    vkDestroyDescriptorSetLayout(device, r->set_layout, NULL);
    vkDestroySampler(device, r->nearest, NULL);
    vkDestroySampler(device, r->linear, NULL);
    vkDestroySemaphore(device, r->timeline, NULL);
    if (r->query_pool) {
        vkDestroyQueryPool(device, r->query_pool, NULL);
    }
    if (r->sync_timeline) {
        // Nobody must be left waiting for one of our points.
        wlr_drm_syncobj_timeline_signal(r->sync_timeline, UINT64_MAX);
        wlr_drm_syncobj_timeline_unref(r->sync_timeline);
    }
    vkDestroyCommandPool(device, r->pool, NULL);
    wlr_drm_format_set_finish(&r->shm_formats);
    free(r->commands);
    free(r->free_waits);
    free(r->free_releases);
    free(r->used_semaphores);
    free(r->retired);
    free(r->staging);
    free(r->wait_infos);
    free(r->wait_semaphores);
    free(r->pipelines);
    free(r);
}

// ------------------------------------------------------- explicit sync --

uint64_t vela_renderer_signal_sync_point(struct vela_renderer *r, int sync_file)
{
    if (!r->sync_timeline) {
        return 0;
    }
    uint64_t point = ++r->sync_point;
    bool ok = sync_file >= 0 ? wlr_drm_syncobj_timeline_import_sync_file(r->sync_timeline, point, sync_file)
                             : wlr_drm_syncobj_timeline_signal(r->sync_timeline, point);
    if (!ok) {
        // Better an early release than an app blocked forever: the CPU waits
        // for the GPU and the point signals at once.
        vela_renderer_wait(r, r->last_point);
        wlr_drm_syncobj_timeline_signal(r->sync_timeline, point);
    }
    return point;
}

// ---------------------------------------------------------- GPU times --

int vela_renderer_timing_slot(struct vela_renderer *r)
{
    if (!r->query_pool) {
        return -1;
    }
    uint64_t done = vela_renderer_completed(r);
    for (int i = 0; i < VELA_TIMING_SLOTS; ++i) {
        int slot = (r->next_timing + i) % VELA_TIMING_SLOTS;
        if (r->timing[slot] <= done) {
            r->timing[slot] = UINT64_MAX; // recording
            r->next_timing = (slot + 1) % VELA_TIMING_SLOTS;
            return slot;
        }
    }
    return -1;
}

void vela_renderer_write_timestamp(struct vela_renderer *r, VkCommandBuffer cmd, int slot, bool end)
{
    if (slot < 0) {
        return;
    }
    if (!end) {
        vkCmdResetQueryPool(cmd, r->query_pool, (uint32_t)(slot * 2), 2);
    }
    vkCmdWriteTimestamp2(cmd, end ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        r->query_pool, (uint32_t)(slot * 2 + (end ? 1 : 0)));
}

void vela_renderer_timing_submitted(struct vela_renderer *r, int slot, uint64_t point)
{
    if (slot >= 0) {
        r->timing[slot] = point; // 0: submission failed, the slot is free again
    }
}

// From GPU ticks to CLOCK_MONOTONIC. The two clocks drift a little:
// calibration is redone every second.
static int64_t gpu_to_monotonic(struct vela_renderer *r, uint64_t ticks)
{
    struct vela_vulkan *vk = r->vk;
    uint64_t mask = vk->timestamp_mask;
    int64_t now = vela_now_ns();
    if (r->calibrated_at == 0 || now - r->calibrated_at > VELA_NS_PER_SEC) {
        const VkCalibratedTimestampInfoKHR infos[2] = {
            { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR },
            { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
                .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
        };
        uint64_t values[2] = { 0 };
        uint64_t deviation = 0;
        if (vk->get_calibrated_timestamps(vk->device, 2, infos, values, &deviation) == VK_SUCCESS) {
            r->calibration_ticks = values[0] & mask;
            r->calibration_ns = (int64_t)values[1];
            r->calibrated_at = now;
        }
    }
    // Signed difference, even when the counter has fewer than 64 bits.
    uint64_t diff = (ticks - r->calibration_ticks) & mask;
    int64_t signed_diff = (int64_t)diff;
    if (mask != UINT64_MAX && diff > mask / 2) {
        signed_diff = (int64_t)diff - (int64_t)mask - 1;
    }
    return r->calibration_ns + (int64_t)((double)signed_diff * vk->timestamp_period);
}

bool vela_renderer_read_timing(struct vela_renderer *r, int slot, uint64_t point, struct vela_gpu_timing *out)
{
    // The slot may already have been reused by a later drawing.
    if (slot < 0 || point == 0 || r->timing[slot] != point || point > vela_renderer_completed(r)) {
        return false;
    }
    uint64_t ticks[2] = { 0 };
    if (vkGetQueryPoolResults(r->vk->device, r->query_pool, (uint32_t)(slot * 2), 2, sizeof(ticks), ticks,
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT)
        != VK_SUCCESS) {
        return false;
    }
    uint64_t mask = r->vk->timestamp_mask;
    if (r->vk->get_calibrated_timestamps) {
        out->start_ns = gpu_to_monotonic(r, ticks[0] & mask);
        out->end_ns = gpu_to_monotonic(r, ticks[1] & mask);
        out->absolute = true;
    } else {
        out->start_ns = 0;
        out->end_ns = (int64_t)((double)((ticks[1] - ticks[0]) & mask) * r->vk->timestamp_period);
        out->absolute = false;
    }
    return true;
}

// ------------------------------------------------------------- targets --

static void destroy_target(struct vela_target *target)
{
    struct vela_renderer *r = target->renderer;
    wl_list_remove(&target->link);
    wlr_addon_finish(&target->addon);
    // The GPU may still be drawing on it.
    vela_renderer_retire_image(r, target->image, target->view, target->memory);
    free(target);
}

static void handle_target_buffer_destroy(struct wlr_addon *addon)
{
    struct vela_target *target = wl_container_of(addon, target, addon);
    destroy_target(target);
}

static const struct wlr_addon_interface target_addon_impl = {
    .name = "vela-render-target",
    .destroy = handle_target_buffer_destroy,
};

struct vela_target *vela_renderer_target(struct vela_renderer *r, struct wlr_buffer *buffer)
{
    struct wlr_addon *addon = wlr_addon_find(&buffer->addons, r, &target_addon_impl);
    if (addon) {
        struct vela_target *target = wl_container_of(addon, target, addon);
        return target;
    }

    struct wlr_dmabuf_attributes dmabuf;
    if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
        wlr_log(WLR_ERROR, "Renderer: can only render to dmabuf buffers");
        return NULL;
    }
    const struct vela_pixel_format *format = vela_pixel_format_from_drm(dmabuf.format);
    if (!format || format->srgb == VK_FORMAT_UNDEFINED
        || !wlr_drm_format_set_has(&r->vk->render_formats, dmabuf.format, dmabuf.modifier)) {
        wlr_log(WLR_ERROR, "Renderer: format 0x%08x (modifier 0x%" PRIx64 ") not renderable", dmabuf.format,
            dmabuf.modifier);
        return NULL;
    }

    struct vela_target *target = calloc(1, sizeof(*target));
    if (!target) {
        return NULL;
    }
    target->renderer = r;
    target->format = format->srgb;
    target->dmabuf_fd = dmabuf.fd[0];
    target->width = (uint32_t)dmabuf.width;
    target->height = (uint32_t)dmabuf.height;
    // Readable too, if the format allows it: the blur reads what is already
    // drawn under a zone (§8.3).
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (vela_vulkan_supports_dmabuf(r->vk, format->srgb, dmabuf.modifier, usage | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        target->sampleable = true;
    }
    if (!vela_vulkan_import_dmabuf(r->vk, &dmabuf, format->srgb, usage, &target->image, &target->memory)) {
        wlr_log(WLR_ERROR, "Renderer: can't import the target buffer");
        free(target);
        return NULL;
    }
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = target->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format->srgb,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkResult view_result = vkCreateImageView(r->vk->device, &view_info, NULL, &target->view);
    if (view_result != VK_SUCCESS) {
        wlr_log(WLR_ERROR, "Renderer: vkCreateImageView for target failed: VkResult=%d", (int)view_result);
        vkDestroyImage(r->vk->device, target->image, NULL);
        vkFreeMemory(r->vk->device, target->memory, NULL);
        free(target);
        return NULL;
    }

    wlr_addon_init(&target->addon, &buffer->addons, r, &target_addon_impl);
    wl_list_insert(&r->targets, &target->link);
    return target;
}

// ----------------------------------------------------- command buffers --

VkCommandBuffer vela_renderer_begin_commands(struct vela_renderer *r)
{
    uint64_t done = vela_renderer_completed(r);
    struct command_slot *slot = NULL;
    for (int i = 0; i < r->command_count && !slot; ++i) {
        if (!r->commands[i].busy && r->commands[i].point <= done) {
            slot = &r->commands[i];
        }
    }
    if (!slot) {
        const VkCommandBufferAllocateInfo info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = r->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(r->vk->device, &info, &cmd) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        r->commands = vela_grow(r->commands, &r->command_capacity, r->command_count + 1, sizeof(*r->commands));
        slot = &r->commands[r->command_count++];
        slot->cmd = cmd;
    }
    slot->busy = true;
    slot->point = 0;
    vkResetCommandBuffer(slot->cmd, 0);
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(slot->cmd, &begin);
    return slot->cmd;
}

VkCommandBuffer vela_renderer_upload_commands(struct vela_renderer *r)
{
    if (!r->upload) {
        r->upload = vela_renderer_begin_commands(r);
    }
    return r->upload;
}

// ------------------------------------------------------------- staging --

static struct staging_chunk *new_staging_chunk(struct vela_renderer *r, VkDeviceSize size)
{
    VkDevice device = r->vk->device;
    struct staging_chunk chunk = { .size = size };
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = chunk.size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(device, &buffer_info, NULL, &chunk.buffer) != VK_SUCCESS) {
        return NULL;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, chunk.buffer, &requirements);
    uint32_t type = vela_vulkan_memory_type(r->vk, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (type == UINT32_MAX || vkAllocateMemory(device, &alloc_info, NULL, &chunk.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, chunk.buffer, NULL);
        return NULL;
    }
    vkBindBufferMemory(device, chunk.buffer, chunk.memory, 0);
    void *mapped = NULL;
    vkMapMemory(device, chunk.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    chunk.data = mapped;
    r->staging = vela_grow(r->staging, &r->staging_capacity, r->staging_count + 1, sizeof(*r->staging));
    r->staging[r->staging_count] = chunk;
    return &r->staging[r->staging_count++];
}

bool vela_renderer_stage(struct vela_renderer *r, VkDeviceSize size, struct vela_staging *out)
{
    size = (size + 15) & ~(VkDeviceSize)15;
    uint64_t done = vela_renderer_completed(r);
    struct staging_chunk *chosen = NULL;
    // First the block we are already filling, then one the GPU has finished
    // reading, otherwise a new one.
    for (int i = 0; i < r->staging_count && !chosen; ++i) {
        struct staging_chunk *chunk = &r->staging[i];
        if (chunk->point == UINT64_MAX && chunk->size - chunk->used >= size) {
            chosen = chunk;
        }
    }
    for (int i = 0; i < r->staging_count && !chosen; ++i) {
        struct staging_chunk *chunk = &r->staging[i];
        if (chunk->point != UINT64_MAX && chunk->point <= done && chunk->size >= size) {
            chunk->used = 0;
            chosen = chunk;
        }
    }
    if (!chosen) {
        chosen = new_staging_chunk(r, size > STAGING_CHUNK_SIZE ? size : STAGING_CHUNK_SIZE);
        if (!chosen) {
            return false;
        }
    }
    chosen->point = UINT64_MAX; // in use by commands not submitted yet
    out->buffer = chosen->buffer;
    out->offset = chosen->used;
    out->data = chosen->data + chosen->used;
    chosen->used += size;
    return true;
}

// ---------------------------------------------------------- submission --

static VkSemaphore take_semaphore(struct vela_renderer *r, bool release)
{
    if (release && r->free_release_count > 0) {
        return r->free_releases[--r->free_release_count];
    }
    if (!release && r->free_wait_count > 0) {
        return r->free_waits[--r->free_wait_count];
    }
    const VkExportSemaphoreCreateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    const VkSemaphoreCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = release ? &export_info : NULL,
    };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    vkCreateSemaphore(r->vk->device, &info, NULL, &semaphore);
    return semaphore;
}

static void give_back_semaphore(struct vela_renderer *r, VkSemaphore semaphore, bool release)
{
    if (release) {
        r->free_releases
            = vela_grow(r->free_releases, &r->free_release_capacity, r->free_release_count + 1, sizeof(VkSemaphore));
        r->free_releases[r->free_release_count++] = semaphore;
    } else {
        r->free_waits = vela_grow(r->free_waits, &r->free_wait_capacity, r->free_wait_count + 1, sizeof(VkSemaphore));
        r->free_waits[r->free_wait_count++] = semaphore;
    }
}

static void semaphore_used_until(struct vela_renderer *r, VkSemaphore semaphore, bool release, uint64_t point)
{
    r->used_semaphores = vela_grow(r->used_semaphores, &r->used_semaphore_capacity, r->used_semaphore_count + 1,
        sizeof(*r->used_semaphores));
    r->used_semaphores[r->used_semaphore_count++] = (struct used_semaphore) { point, semaphore, release };
}

uint64_t vela_renderer_submit(struct vela_renderer *r, VkCommandBuffer cmd, const int *wait_fds, int wait_count,
    int *release_fd)
{
    struct vela_vulkan *vk = r->vk;
    if (release_fd) {
        *release_fd = -1;
    }
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmds[2];
    uint32_t cmd_count = 0;
    if (r->upload) {
        vkEndCommandBuffer(r->upload);
        cmds[cmd_count++] = (VkCommandBufferSubmitInfo) {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = r->upload,
        };
    }
    cmds[cmd_count++] = (VkCommandBufferSubmitInfo) {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };

    // Kernel fences (sync_files) become semaphores the GPU waits for.
    if (wait_count > r->wait_capacity) {
        r->wait_infos = realloc(r->wait_infos, (size_t)wait_count * sizeof(*r->wait_infos));
        r->wait_semaphores = realloc(r->wait_semaphores, (size_t)wait_count * sizeof(*r->wait_semaphores));
        r->wait_capacity = wait_count;
    }
    uint32_t waits = 0;
    bool inputs_ready = true;
    for (int i = 0; i < wait_count; ++i) {
        int fd = wait_fds[i];
        // Most app commits have already been held until ready by scene/ready.
        // Avoid importing a completed fence into the driver for every draw.
        if (vela_sync_ready(fd)) {
            close(fd);
            continue;
        }
        if (!vk->sync_file) {
            inputs_ready = vela_sync_wait_close(fd) && inputs_ready;
            continue;
        }
        VkSemaphore semaphore = take_semaphore(r, false);
        const VkImportSemaphoreFdInfoKHR import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = fd,
        };
        if (!semaphore || vk->import_semaphore_fd(vk->device, &import_info) != VK_SUCCESS) {
            // Fallback: the CPU waits.
            inputs_ready = vela_sync_wait_close(fd) && inputs_ready;
            if (semaphore) {
                give_back_semaphore(r, semaphore, false);
            }
            continue;
        }
        r->wait_semaphores[waits] = semaphore;
        r->wait_infos[waits] = (VkSemaphoreSubmitInfo) {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        ++waits;
    }

    uint64_t point = r->last_point + 1;
    VkSemaphoreSubmitInfo signals[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = r->timeline,
            .value = point,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        },
    };
    uint32_t signal_count = 1;
    VkSemaphore release = VK_NULL_HANDLE;
    if (release_fd && vk->sync_file) {
        release = take_semaphore(r, true);
        if (release) {
            signals[signal_count++] = (VkSemaphoreSubmitInfo) {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = release,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            };
        }
    }

    const VkSubmitInfo2 submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = waits,
        .pWaitSemaphoreInfos = r->wait_infos,
        .commandBufferInfoCount = cmd_count,
        .pCommandBufferInfos = cmds,
        .signalSemaphoreInfoCount = signal_count,
        .pSignalSemaphoreInfos = signals,
    };
    bool ok = inputs_ready && vkQueueSubmit2(vk->queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS;

    // Command buffers become available again once the GPU has executed them.
    VkCommandBuffer upload = r->upload;
    r->upload = VK_NULL_HANDLE;
    for (int i = 0; i < r->command_count; ++i) {
        if (r->commands[i].cmd == cmd || r->commands[i].cmd == upload) {
            r->commands[i].busy = false;
            r->commands[i].point = ok ? point : 0;
        }
    }

    if (!ok) {
        wlr_log(WLR_ERROR, "Renderer: GPU submission failed");
        // Semaphores in an uncertain state: better throw them away.
        for (uint32_t i = 0; i < waits; ++i) {
            vkDestroySemaphore(vk->device, r->wait_semaphores[i], NULL);
        }
        if (release) {
            vkDestroySemaphore(vk->device, release, NULL);
        }
        for (int i = 0; i < r->staging_count; ++i) {
            if (r->staging[i].point == UINT64_MAX) {
                r->staging[i].point = r->last_point;
            }
        }
        return 0;
    }

    r->last_point = point;
    for (int i = 0; i < r->staging_count; ++i) {
        if (r->staging[i].point == UINT64_MAX) {
            r->staging[i].point = point;
        }
    }
    for (uint32_t i = 0; i < waits; ++i) {
        semaphore_used_until(r, r->wait_semaphores[i], false, point);
    }
    if (release) {
        const VkSemaphoreGetFdInfoKHR get_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = release,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        if (vk->get_semaphore_fd(vk->device, &get_info, release_fd) != VK_SUCCESS) {
            *release_fd = -1;
        }
        semaphore_used_until(r, release, true, point);
    }
    // Diagnostics: VELA_DEBUG_SYNC=1 makes the CPU wait for the GPU at every
    // submission, so whoever reads our buffers surely finds them finished.
    // Without sync_files the CPU waits anyway.
    if ((release_fd && *release_fd < 0) || vela_env_one("VELA_DEBUG_SYNC")) {
        vela_renderer_wait(r, point);
    }
    return point;
}

uint64_t vela_renderer_completed(struct vela_renderer *r)
{
    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(r->vk->device, r->timeline, &value) == VK_SUCCESS) {
        r->completed = value;
    }
    return r->completed;
}

void vela_renderer_wait(struct vela_renderer *r, uint64_t point)
{
    if (point == 0 || point <= r->completed) {
        return;
    }
    const VkSemaphoreWaitInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &r->timeline,
        .pValues = &point,
    };
    vkWaitSemaphores(r->vk->device, &info, UINT64_MAX);
    vela_renderer_completed(r);
}

void vela_renderer_retire_image(struct vela_renderer *r, VkImage image, VkImageView view, VkDeviceMemory memory)
{
    // Uploads recorded but not yet submitted count too: they will end up in
    // the next submission.
    uint64_t point = r->upload ? r->last_point + 1 : r->last_point;
    if (point <= vela_renderer_completed(r)) {
        VkDevice device = r->vk->device;
        if (view) {
            vkDestroyImageView(device, view, NULL);
        }
        vkDestroyImage(device, image, NULL);
        if (memory) {
            vkFreeMemory(device, memory, NULL);
        }
        return;
    }
    r->retired = vela_grow(r->retired, &r->retired_capacity, r->retired_count + 1, sizeof(*r->retired));
    r->retired[r->retired_count++] = (struct retired_image) { point, image, view, memory };
}

void vela_renderer_collect(struct vela_renderer *r)
{
    VkDevice device = r->vk->device;
    uint64_t done = vela_renderer_completed(r);

    int kept = 0;
    for (int i = 0; i < r->used_semaphore_count; ++i) {
        struct used_semaphore used = r->used_semaphores[i];
        if (used.point <= done) {
            give_back_semaphore(r, used.semaphore, used.release);
        } else {
            r->used_semaphores[kept++] = used;
        }
    }
    r->used_semaphore_count = kept;

    kept = 0;
    for (int i = 0; i < r->retired_count; ++i) {
        struct retired_image retired = r->retired[i];
        if (retired.point > done) {
            r->retired[kept++] = retired;
            continue;
        }
        if (retired.view) {
            vkDestroyImageView(device, retired.view, NULL);
        }
        vkDestroyImage(device, retired.image, NULL);
        if (retired.memory) {
            vkFreeMemory(device, retired.memory, NULL);
        }
    }
    r->retired_count = kept;

    // Unused staging blocks: only a few are kept.
    int idle = 0;
    kept = 0;
    for (int i = 0; i < r->staging_count; ++i) {
        struct staging_chunk chunk = r->staging[i];
        bool unused = chunk.point != UINT64_MAX && chunk.point <= done;
        if (unused && ++idle > IDLE_STAGING_CHUNKS_KEPT) {
            vkDestroyBuffer(device, chunk.buffer, NULL);
            vkFreeMemory(device, chunk.memory, NULL);
            continue;
        }
        r->staging[kept++] = chunk;
    }
    r->staging_count = kept;
}

// ----------------------------------------------------------- pipelines --

VkPipeline vela_renderer_pipeline(struct vela_renderer *r, VkFormat target, enum vela_pipeline_kind kind, bool blend)
{
    for (int i = 0; i < r->pipeline_count; ++i) {
        const struct pipeline_entry *entry = &r->pipelines[i];
        if (entry->format == target && entry->kind == kind && entry->blend == blend) {
            return entry->pipeline;
        }
    }

    VkShaderModule fragment = r->rect_frag;
    switch (kind) {
    case VELA_PIPELINE_TEXTURE: fragment = r->texture_frag; break;
    case VELA_PIPELINE_RECT: break; // already the default shader
    case VELA_PIPELINE_SHADOW: fragment = r->shadow_frag; break;
    case VELA_PIPELINE_BLUR_DOWN: fragment = r->blur_down_frag; break;
    case VELA_PIPELINE_BLUR_UP: fragment = r->blur_up_frag; break;
    case VELA_PIPELINE_BLUR_MIX: fragment = r->blur_mix_frag; break;
    }
    const VkPipelineShaderStageCreateInfo stages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->vert,
            .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment,
            .pName = "main" },
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    // Premultiplied alpha, blended in linear space (the view is _SRGB).
    const VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask
        = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    const VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &target,
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend_state,
        .pDynamicState = &dynamic,
        .layout = r->layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r->vk->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS) {
        wlr_log(WLR_ERROR, "Renderer: can't create a pipeline");
        return VK_NULL_HANDLE;
    }
    r->pipelines = vela_grow(r->pipelines, &r->pipeline_capacity, r->pipeline_count + 1, sizeof(*r->pipelines));
    r->pipelines[r->pipeline_count++] = (struct pipeline_entry) { target, kind, blend, pipeline };
    return pipeline;
}

// ---------------------------------------------------------------- blur --

bool vela_renderer_prepare_blur(struct vela_renderer *r, uint32_t width, uint32_t height)
{
    VkDevice device = r->vk->device;
    for (int level = 0; level < VELA_BLUR_LEVELS; ++level) {
        uint32_t need_width = (width + (2u << level) - 1) >> (level + 1);
        uint32_t need_height = (height + (2u << level) - 1) >> (level + 1);
        need_width = need_width ? need_width : 1;
        need_height = need_height ? need_height : 1;
        struct vela_blur_image *current = &r->blur[level];
        if (current->image && current->width >= need_width && current->height >= need_height) {
            continue;
        }
        // Larger than needed, so it isn't redone for every slightly larger
        // zone.
        uint32_t alloc_width = (need_width > current->width ? need_width : current->width) + 64;
        uint32_t alloc_height = (need_height > current->height ? need_height : current->height) + 64;
        if (current->image) {
            vela_renderer_retire_image(r, current->image, current->view, current->memory);
            memset(current, 0, sizeof(*current));
        }
        const VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VELA_BLUR_FORMAT,
            .extent = { alloc_width, alloc_height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        struct vela_blur_image image = { .width = alloc_width, .height = alloc_height };
        bool ok = vkCreateImage(device, &image_info, NULL, &image.image) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements requirements;
            vkGetImageMemoryRequirements(device, image.image, &requirements);
            const VkMemoryAllocateInfo alloc_info = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = requirements.size,
                .memoryTypeIndex
                = vela_vulkan_memory_type(r->vk, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            };
            ok = alloc_info.memoryTypeIndex != UINT32_MAX
                && vkAllocateMemory(device, &alloc_info, NULL, &image.memory) == VK_SUCCESS
                && vkBindImageMemory(device, image.image, image.memory, 0) == VK_SUCCESS;
        }
        if (ok) {
            const VkImageViewCreateInfo view_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VELA_BLUR_FORMAT,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            ok = vkCreateImageView(device, &view_info, NULL, &image.view) == VK_SUCCESS;
        }
        if (!ok) {
            wlr_log(WLR_ERROR, "Renderer: no memory for the blur (%ux%u)", alloc_width, alloc_height);
            destroy_blur_image(device, &image);
            return false;
        }
        *current = image;
    }
    return true;
}

// -------------------------------------------------------- wlr_renderer --

static const struct wlr_drm_format_set *get_texture_formats(struct wlr_renderer *wlr, uint32_t caps)
{
    struct vela_renderer *r = (struct vela_renderer *)wlr;
    if (caps == WLR_BUFFER_CAP_DMABUF) {
        return &r->vk->texture_formats;
    }
    if (caps == WLR_BUFFER_CAP_DATA_PTR) {
        return &r->shm_formats;
    }
    return NULL;
}

static const struct wlr_drm_format_set *get_render_formats(struct wlr_renderer *wlr)
{
    return &((struct vela_renderer *)wlr)->vk->render_formats;
}

static void renderer_destroy(struct wlr_renderer *wlr)
{
    destroy((struct vela_renderer *)wlr);
}

static int get_drm_fd(struct wlr_renderer *wlr)
{
    return ((struct vela_renderer *)wlr)->vk->render_fd;
}

static struct wlr_texture *texture_from_buffer(struct wlr_renderer *wlr, struct wlr_buffer *buffer)
{
    return vela_texture_create((struct vela_renderer *)wlr, buffer);
}

static struct wlr_render_pass *begin_buffer_pass(struct wlr_renderer *wlr, struct wlr_buffer *buffer,
    const struct wlr_buffer_pass_options *options)
{
    struct vela_pass *pass = vela_renderer_begin_pass((struct vela_renderer *)wlr, buffer);
    if (!pass) {
        return NULL;
    }
    if (options && options->signal_timeline) {
        vela_pass_signal_on_done(pass, options->signal_timeline, options->signal_point);
    }
    return vela_pass_wlr(pass);
}

static const struct wlr_renderer_impl renderer_impl = {
    .get_texture_formats = get_texture_formats,
    .get_render_formats = get_render_formats,
    .destroy = renderer_destroy,
    .get_drm_fd = get_drm_fd,
    .texture_from_buffer = texture_from_buffer,
    .begin_buffer_pass = begin_buffer_pass,
};
