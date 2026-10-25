// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-slowgpu: an app whose GPU finishes late (docs/renderer.md §7.3).
//
// Every frame is a solid color, written by the CPU into a dmabuf (GBM,
// linear); then a job on a compute queue lasting about MS milliseconds stands
// in for "rendering": its fence becomes the buffer's acquire point
// (linux-drm-syncobj-v1) or, with --implicit, the dmabuf's write fence. The
// commit goes out at once, with the GPU still working, as a real app drawing
// slower than the output does.
//
// The compute queue leaves the graphics one, which the compositor uses, free:
// only waiting for the fence is measured, not GPU contention. If the
// compositor waited for the fence in its frame, it would miss vblanks (Vela's
// "state" counts them per output).
//
// Usage: vela-slowgpu [MS] [--implicit] [--fullscreen] [--monolithic] (150 ms by default)
// --fullscreen: on an output of exactly 512x320, a direct scanout candidate.
// --monolithic: keep the old single long dispatch for comparative testing.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <drm_fourcc.h>
#include <xf86drm.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define WIDTH 512 // 2048-byte rows: linear dmabufs want aligned strides
#define HEIGHT 320
#define BUFFERS 3

static const uint32_t shaderCode[] = {
#include "slow.comp.spv.inc"
};

static struct wl_compositor* compositor;
static struct xdg_wm_base* wmBase;
static struct zwp_linux_dmabuf_v1* dmabufManager;
static struct wp_linux_drm_syncobj_manager_v1* syncobjManager;

static struct wl_surface* surface;
static struct wp_linux_drm_syncobj_surface_v1* syncobjSurface;
static struct xdg_surface* xdgSurface;
static struct xdg_toplevel* toplevel;
static int wantFullscreen;

static dev_t mainDevice;
static int haveMainDevice;
static int configured;
static int frameDue = 1;
static int running = 1;
static int implicitSync;

static int drmFd = -1;
static struct gbm_device* gbm;

struct Buffer {
    struct gbm_bo* bo;
    int fd;
    struct wl_buffer* wl;
    int busy; // implicit: until the compositor releases it
    uint64_t releasePoint; // explicit: the point to wait for before reusing it
};
static struct Buffer buffers[BUFFERS];

// Explicit: two timelines of ours, one for acquire and one for release.
static uint32_t acquireHandle, releaseHandle;
static struct wp_linux_drm_syncobj_timeline_v1* acquireTimeline;
static struct wp_linux_drm_syncobj_timeline_v1* releaseTimeline;
static uint64_t nextPoint = 1;

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandPool commandPool;
static VkCommandBuffer commandBuffer;
static VkPipelineLayout pipelineLayout;
static VkPipeline pipeline;
static VkDescriptorSet descriptorSet;
static VkFence fence;
static VkSemaphore semaphore;
static PFN_vkGetSemaphoreFdKHR getSemaphoreFd;
static uint32_t iterations = 1 << 14;
static uint32_t dispatches = 1; // bound the duration of each compute dispatch

static void die(const char* what)
{
    fprintf(stderr, "vela-slowgpu: %s\n", what);
    exit(1);
}

static void progress(const char* step)
{
    fprintf(stderr, "vela-slowgpu: %s\n", step);
    fflush(stderr);
}

static void check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vela-slowgpu: %s (VkResult %d)\n", what, result);
        exit(1);
    }
}

static double nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// ------------------------------------------------------------ Wayland --

static void onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        wmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= 4) {
        dmabufManager = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4);
    } else if (!strcmp(interface, wp_linux_drm_syncobj_manager_v1_interface.name)) {
        syncobjManager = wl_registry_bind(registry, name, &wp_linux_drm_syncobj_manager_v1_interface, 1);
    }
}

static void onGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) { }
static const struct wl_registry_listener registryListener = { onGlobal, onGlobalRemove };

// Of the dmabuf feedback only the compositor's main device is needed: buffers
// are allocated there, and the GPU work runs there.
static void onFeedbackDone(void* data, struct zwp_linux_dmabuf_feedback_v1* feedback) { }
static void onFormatTable(void* data, struct zwp_linux_dmabuf_feedback_v1* feedback, int32_t fd, uint32_t size)
{
    close(fd);
}
static void onMainDevice(void* data, struct zwp_linux_dmabuf_feedback_v1* feedback, struct wl_array* device)
{
    if (device->size == sizeof(dev_t)) {
        memcpy(&mainDevice, device->data, sizeof(dev_t));
        haveMainDevice = 1;
    }
}
static void onTrancheDone(void* data, struct zwp_linux_dmabuf_feedback_v1* feedback) { }
static void onTrancheTarget(void* data, struct zwp_linux_dmabuf_feedback_v1* feedback, struct wl_array* device) { }
static void onTrancheFormats(void* data, struct zwp_linux_dmabuf_feedback_v1* feedback, struct wl_array* indices) { }
static void onTrancheFlags(void* data, struct zwp_linux_dmabuf_feedback_v1* feedback, uint32_t flags) { }
static const struct zwp_linux_dmabuf_feedback_v1_listener feedbackListener = {
    onFeedbackDone, onFormatTable, onMainDevice, onTrancheDone, onTrancheTarget, onTrancheFormats, onTrancheFlags,
};

static void onBufferRelease(void* data, struct wl_buffer* wl)
{
    ((struct Buffer*)data)->busy = 0;
}
static const struct wl_buffer_listener bufferListener = { onBufferRelease };

static void onPing(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wmBaseListener = { onPing };

static void onSurfaceConfigure(void* data, struct xdg_surface* xdg, uint32_t serial)
{
    xdg_surface_ack_configure(xdg, serial);
    configured = 1;
}
static const struct xdg_surface_listener surfaceListener = { onSurfaceConfigure };

static void onToplevelConfigure(void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states) { }
static void onToplevelClose(void* data, struct xdg_toplevel* t)
{
    running = 0;
}
static void onConfigureBounds(void* data, struct xdg_toplevel* t, int32_t w, int32_t h) { }
static void onWmCapabilities(void* data, struct xdg_toplevel* t, struct wl_array* capabilities) { }
static const struct xdg_toplevel_listener toplevelListener = {
    onToplevelConfigure, onToplevelClose, onConfigureBounds, onWmCapabilities,
};

static void onFrame(void* data, struct wl_callback* callback, uint32_t time)
{
    wl_callback_destroy(callback);
    frameDue = 1;
}
static const struct wl_callback_listener frameListener = { onFrame };

// ------------------------------------------------------------- Vulkan --

static void setupVulkan(void)
{
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vela-slowgpu",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    check(vkCreateInstance(&instanceInfo, NULL, &instance), "vkCreateInstance");

    // The compositor's device (same DRM node), with
    // VK_EXT_physical_device_drm.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice devices[16];
    if (count > 16) {
        count = 16;
    }
    vkEnumeratePhysicalDevices(instance, &count, devices);
    struct stat node;
    if (fstat(drmFd, &node) != 0) {
        die("fstat of the DRM node");
    }
    for (uint32_t i = 0; i < count && !physical; ++i) {
        VkPhysicalDeviceDrmPropertiesEXT drm = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm };
        vkGetPhysicalDeviceProperties2(devices[i], &properties);
        if (drm.hasRender && makedev(drm.renderMajor, drm.renderMinor) == node.st_rdev) {
            physical = devices[i];
        }
    }
    if (!physical) {
        die("no Vulkan device for the compositor's DRM node");
    }

    // A compute-only queue, if there is one: it doesn't take the graphics one.
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties family[16];
    if (families > 16) {
        families = 16;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, family);
    int chosen = -1;
    for (uint32_t i = 0; i < families; ++i) {
        const VkQueueFlags flags = family[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)) {
            chosen = (int)i;
            break;
        }
        if ((flags & VK_QUEUE_COMPUTE_BIT) && chosen < 0) {
            chosen = (int)i;
        }
    }
    if (chosen < 0) {
        die("no compute queue");
    }

    const float priority = 0.0f;
    const VkDeviceQueueCreateInfo queueInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)chosen,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char* extensions[] = { VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
    const VkDeviceCreateInfo deviceInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    check(vkCreateDevice(physical, &deviceInfo, NULL, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, (uint32_t)chosen, 0, &queue);
    getSemaphoreFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
    if (!getSemaphoreFd) {
        die("VK_KHR_external_semaphore_fd is enabled but vkGetSemaphoreFdKHR is unavailable");
    }

    // The result buffer (64 floats), only so the loop doesn't vanish.
    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 64 * sizeof(float),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    VkBuffer result;
    check(vkCreateBuffer(device, &bufferInfo, NULL, &result), "vkCreateBuffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, result, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t type = 0;
    while (type < memory.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) {
        ++type;
    }
    const VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory resultMemory;
    check(vkAllocateMemory(device, &allocateInfo, NULL, &resultMemory), "vkAllocateMemory");
    vkBindBufferMemory(device, result, resultMemory, 0);

    const VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo setLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    VkDescriptorSetLayout setLayout;
    check(vkCreateDescriptorSetLayout(device, &setLayoutInfo, NULL, &setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
    const VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    check(vkCreatePipelineLayout(device, &layoutInfo, NULL, &pipelineLayout), "vkCreatePipelineLayout");

    const VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    const VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(device, &poolInfo, NULL, &pool), "vkCreateDescriptorPool");
    const VkDescriptorSetAllocateInfo setInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &setLayout,
    };
    check(vkAllocateDescriptorSets(device, &setInfo, &descriptorSet), "vkAllocateDescriptorSets");
    const VkDescriptorBufferInfo resultInfo = { result, 0, VK_WHOLE_SIZE };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSet,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &resultInfo,
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    const VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(shaderCode),
        .pCode = shaderCode,
    };
    VkShaderModule module;
    check(vkCreateShaderModule(device, &moduleInfo, NULL, &module), "vkCreateShaderModule");
    const VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
        },
        .layout = pipelineLayout,
    };
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline), "vkCreateComputePipelines");

    const VkCommandPoolCreateInfo commandPoolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = (uint32_t)chosen,
    };
    check(vkCreateCommandPool(device, &commandPoolInfo, NULL, &commandPool), "vkCreateCommandPool");
    const VkCommandBufferAllocateInfo commandInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    check(vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer), "vkAllocateCommandBuffers");
    const VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    check(vkCreateFence(device, &fenceInfo, NULL, &fence), "vkCreateFence");

    // A semaphore exportable as a sync_file: the "rendering" fence.
    const VkExportSemaphoreCreateInfo exportInfo = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    const VkSemaphoreCreateInfo semaphoreInfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &exportInfo };
    check(vkCreateSemaphore(device, &semaphoreInfo, NULL, &semaphore), "vkCreateSemaphore");
}

// Launches the slow job. With exportFence, returns its sync_file (not signaled
// yet); without, waits for it to finish.
static int runJob(int exportFence)
{
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fence);
    vkResetCommandBuffer(commandBuffer, 0);
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(commandBuffer, &begin);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
    // A single 150 ms compute dispatch can starve a separate compositor
    // graphics queue on drivers without fine-grained preemption. Splitting
    // the same calibrated work into short dispatches leaves scheduling points
    // for the compositor without signaling the final acquire fence early.
    // A barrier orders repeated writes to the result buffer.
    uint32_t per_dispatch = iterations / dispatches;
    uint32_t remainder = iterations % dispatches;
    for (uint32_t i = 0; i < dispatches; ++i) {
        uint32_t count = per_dispatch + (i < remainder ? 1 : 0);
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
        vkCmdDispatch(commandBuffer, 1, 1, 1);
        if (i + 1 < dispatches) {
            const VkMemoryBarrier barrier = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            };
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        }
    }
    vkEndCommandBuffer(commandBuffer);
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
        .signalSemaphoreCount = exportFence ? 1 : 0,
        .pSignalSemaphores = &semaphore,
    };
    check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
    if (!exportFence) {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        return -1;
    }
    const VkSemaphoreGetFdInfoKHR getFd = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = semaphore,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int fd = -1;
    check(getSemaphoreFd(device, &getFd, &fd), "vkGetSemaphoreFdKHR");
    return fd;
}

// How many iterations to last `targetMs` on this GPU.
static void calibrate(double targetMs)
{
    double elapsed = 0.0;
    for (;;) {
        const double start = nowMs();
        runJob(0);
        elapsed = nowMs() - start;
        if (elapsed >= 20.0 || iterations >= (1u << 30)) {
            break;
        }
        iterations *= 2;
    }
    for (int round = 0; round < 2; ++round) {
        iterations = (uint32_t)((double)iterations * targetMs / elapsed);
        // Every dispatched shader must execute at least one iteration.
        if (iterations < dispatches) {
            iterations = dispatches;
        }
        const double start = nowMs();
        runJob(0);
        elapsed = nowMs() - start;
    }
    printf("GPU job: %u iterations over %u dispatches, %.0f ms\n", iterations, dispatches, elapsed);
    fflush(stdout);
}

// -------------------------------------------------------------- buffers --

static void setupBuffers(void)
{
    // CPU-painted DMA-BUFs are never Vulkan render targets. Requiring
    // GBM_BO_USE_RENDERING as well as LINEAR needlessly rejects formats on
    // drivers that cannot render to linear buffers (notably NVIDIA).
    const uint32_t usage = GBM_BO_USE_LINEAR;
    const char* backend = gbm_device_get_backend_name(gbm);
    fprintf(stderr, "vela-slowgpu: GBM backend=%s, usage=0x%x (LINEAR only)\n",
            backend ? backend : "unknown", usage);
    for (int i = 0; i < BUFFERS; ++i) {
        struct Buffer* b = &buffers[i];
        errno = 0;
        b->bo = gbm_bo_create(gbm, WIDTH, HEIGHT, GBM_FORMAT_XRGB8888, usage);
        if (!b->bo) {
            fprintf(stderr, "vela-slowgpu: gbm_bo_create buffer %d failed: %s (errno=%d)\n",
                    i, strerror(errno), errno);
            die("gbm_bo_create");
        }
        b->fd = gbm_bo_get_fd(b->bo);
        // With GBM_BO_USE_LINEAR the layout is linear even when GBM doesn't
        // say so (an implicit modifier, which Vela doesn't accept).
        uint64_t linear = gbm_bo_get_modifier(b->bo);
        if (linear == DRM_FORMAT_MOD_INVALID) {
            linear = DRM_FORMAT_MOD_LINEAR;
        }
        struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabufManager);
        zwp_linux_buffer_params_v1_add(params, b->fd, 0, gbm_bo_get_offset(b->bo, 0), gbm_bo_get_stride(b->bo),
                                       (uint32_t)(linear >> 32), (uint32_t)linear);
        b->wl = zwp_linux_buffer_params_v1_create_immed(params, WIDTH, HEIGHT, GBM_FORMAT_XRGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        wl_buffer_add_listener(b->wl, &bufferListener, b);
    }
}

static void fill(struct Buffer* b, uint32_t color)
{
    uint32_t stride = 0;
    void* mapping = NULL;
    errno = 0;
    uint32_t* pixels = gbm_bo_map(b->bo, 0, 0, WIDTH, HEIGHT, GBM_BO_TRANSFER_WRITE, &stride, &mapping);
    if (!pixels) {
        fprintf(stderr, "vela-slowgpu: gbm_bo_map failed: %s (errno=%d)\n", strerror(errno), errno);
        die("gbm_bo_map");
    }
    for (int y = 0; y < HEIGHT; ++y) {
        uint32_t* row = (uint32_t*)((uint8_t*)pixels + (size_t)y * stride);
        for (int x = 0; x < WIDTH; ++x) {
            row[x] = color;
        }
    }
    gbm_bo_unmap(b->bo, mapping);
}

static struct Buffer* freeBuffer(unsigned frame)
{
    if (implicitSync) {
        for (;;) {
            for (int i = 0; i < BUFFERS; ++i) {
                if (!buffers[i].busy) {
                    return &buffers[i];
                }
            }
            if (wl_display_dispatch(wl_proxy_get_display((struct wl_proxy*)surface)) < 0) {
                die("connection lost");
            }
        }
    }
    // Explicit: taking turns, waiting for the release (the compositor signals
    // it when it has finished reading and has a newer buffer).
    struct Buffer* b = &buffers[frame % BUFFERS];
    if (b->releasePoint) {
        uint64_t point = b->releasePoint;
        const int64_t deadline = (int64_t)(nowMs() * 1e6) + 5 * 1000000000LL;
        if (drmSyncobjTimelineWait(drmFd, &releaseHandle, &point, 1, deadline, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                                   NULL)) {
            die("the compositor doesn't release the buffer");
        }
    }
    return b;
}

static void draw(void)
{
    static const uint32_t colors[] = { 0xffd04040, 0xff40b040, 0xff4060d0, 0xffd0b040 };
    static unsigned frame;
    struct Buffer* b = freeBuffer(frame);
    fill(b, colors[frame++ % 4]);

    const int syncFile = runJob(1); // the GPU is still working when the commit goes out
    if (implicitSync) {
        struct dma_buf_import_sync_file request = { .flags = DMA_BUF_SYNC_WRITE, .fd = syncFile };
        if (ioctl(b->fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &request) != 0) {
            die("DMA_BUF_IOCTL_IMPORT_SYNC_FILE");
        }
        b->busy = 1;
    } else {
        const uint64_t acquire = nextPoint++;
        uint32_t temporary = 0;
        if (drmSyncobjCreate(drmFd, 0, &temporary) || drmSyncobjImportSyncFile(drmFd, temporary, syncFile)
            || drmSyncobjTransfer(drmFd, acquireHandle, acquire, temporary, 0, 0)) {
            die("fence into the timeline");
        }
        drmSyncobjDestroy(drmFd, temporary);
        b->releasePoint = nextPoint++;
        wp_linux_drm_syncobj_surface_v1_set_acquire_point(syncobjSurface, acquireTimeline, (uint32_t)(acquire >> 32),
                                                          (uint32_t)acquire);
        wp_linux_drm_syncobj_surface_v1_set_release_point(syncobjSurface, releaseTimeline,
                                                          (uint32_t)(b->releasePoint >> 32), (uint32_t)b->releasePoint);
    }
    close(syncFile);

    wl_surface_attach(surface, b->wl, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    wl_callback_add_listener(wl_surface_frame(surface), &frameListener, NULL);
    wl_surface_commit(surface);
    frameDue = 0;
}

static struct wp_linux_drm_syncobj_timeline_v1* newTimeline(uint32_t* handle)
{
    int fd = -1;
    if (drmSyncobjCreate(drmFd, 0, handle) || drmSyncobjHandleToFD(drmFd, *handle, &fd)) {
        die("timeline syncobj");
    }
    struct wp_linux_drm_syncobj_timeline_v1* timeline = wp_linux_drm_syncobj_manager_v1_import_timeline(syncobjManager, fd);
    close(fd);
    return timeline;
}

int main(int argc, char* argv[])
{
    double targetMs = 150.0;
    int monolithic = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--implicit")) {
            implicitSync = 1;
        } else if (!strcmp(argv[i], "--fullscreen")) {
            wantFullscreen = 1;
        } else if (!strcmp(argv[i], "--monolithic")) {
            monolithic = 1;
        } else if (atof(argv[i]) > 0) {
            targetMs = atof(argv[i]);
        } else {
            fprintf(stderr, "usage: vela-slowgpu [MS] [--implicit] [--fullscreen] [--monolithic]\n");
            return 2;
        }
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        die("no Wayland compositor");
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !wmBase || !dmabufManager) {
        die("mancano wl_compositor, xdg_wm_base o linux-dmabuf v4");
    }
    if (!implicitSync && !syncobjManager) {
        die("the compositor doesn't offer linux-drm-syncobj-v1");
    }
    struct zwp_linux_dmabuf_feedback_v1* feedback = zwp_linux_dmabuf_v1_get_default_feedback(dmabufManager);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &feedbackListener, NULL);
    wl_display_roundtrip(display);
    zwp_linux_dmabuf_feedback_v1_destroy(feedback);
    if (!haveMainDevice) {
        die("the dmabuf feedback doesn't name the device");
    }
    drmDevice* drm = NULL;
    if (drmGetDeviceFromDevId(mainDevice, 0, &drm) || !(drm->available_nodes & (1 << DRM_NODE_RENDER))) {
        die("the compositor's render node wasn't found");
    }
    drmFd = open(drm->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
    drmFreeDevice(&drm);
    if (drmFd < 0) {
        die("opening the render node");
    }
    gbm = gbm_create_device(drmFd);
    if (!gbm) {
        die("gbm_create_device");
    }

    progress("initializing Vulkan");
    setupVulkan();
    // Leave <= 20 ms jobs as-is (resource and scanout suites); only slow
    // producer tests need a cooperative schedule. The calibration measures
    // the actual multi-dispatch workload, including barriers and overhead.
    if (!monolithic && targetMs > 20.0) {
        uint32_t wanted = (uint32_t)(targetMs / 3.0 + 0.5);
        dispatches = wanted > 64 ? 64 : wanted;
    }
    progress("calibrating compute workload");
    calibrate(targetMs);
    progress("allocating linear DMA-BUFs");
    setupBuffers();
    progress("creating Wayland toplevel");

    xdg_wm_base_add_listener(wmBase, &wmBaseListener, NULL);
    surface = wl_compositor_create_surface(compositor);
    if (!implicitSync) {
        syncobjSurface = wp_linux_drm_syncobj_manager_v1_get_surface(syncobjManager, surface);
        acquireTimeline = newTimeline(&acquireHandle);
        releaseTimeline = newTimeline(&releaseHandle);
    }
    xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
    xdg_surface_add_listener(xdgSurface, &surfaceListener, NULL);
    toplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, NULL);
    xdg_toplevel_set_title(toplevel, "vela-slowgpu");
    xdg_toplevel_set_app_id(toplevel, "vela.slowgpu");
    if (wantFullscreen) {
        xdg_toplevel_set_fullscreen(toplevel, NULL);
    }
    wl_surface_commit(surface);
    while (!configured && wl_display_dispatch(display) >= 0) {
    }
    progress("Wayland toplevel configured; submitting first frame");

    while (running) {
        if (frameDue) {
            draw();
        }
        if (wl_display_dispatch(display) < 0) {
            break;
        }
    }
    vkDeviceWaitIdle(device);
    return 0;
}
