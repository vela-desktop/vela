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
// A compute-only queue is preferred, but resources can still contend. This
// fixture does not establish absence of GPU contention. If the
// compositor waited for the fence in its frame, it would miss vblanks (Vela's
// "state" counts them per output).
//
// Usage: vela-slowgpu [MS] [--implicit] [--fullscreen]    (150 ms by default)
// --fullscreen: on an output of exactly 512x320, a direct scanout candidate.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
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
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static int WIDTH = 512; // JSON workload uses a larger window behind the neutral client.
static int HEIGHT = 320;
#define BUFFERS 3

static const uint32_t shaderCode[] = {
#include "slow.comp.spv.inc"
};

static struct wl_compositor* compositor;
static struct xdg_wm_base* wmBase;
static struct zwp_linux_dmabuf_v1* dmabufManager;
static struct wp_linux_drm_syncobj_manager_v1* syncobjManager;
static struct wp_presentation* presentation;

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
static int jsonOutput, untilEof;
static unsigned timeoutSeconds = 110;
static volatile sig_atomic_t stopping;
static VkQueryPool timing;
static uint32_t timestampBits;
static double timestampPeriod, requestedDelay = 150;
static uint64_t submittedJobs, completedJobs, pendingFrame;
static uint64_t pendingSubmitNs;
static uint64_t producerPresented, producerDiscarded;

static void terminate(int signum) { stopping = 1; }
static void expired(int signum)
{
    static const char message[] = "vela-slowgpu: deadline expired\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void die(const char* what)
{
    fprintf(stderr, "vela-slowgpu: %s\n", what);
    exit(1);
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

static void presentationClock(void* data, struct wp_presentation* object, uint32_t clock) { }
static const struct wp_presentation_listener presentationListener = { presentationClock };

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
    } else if (jsonOutput && !strcmp(interface, wp_presentation_interface.name)) {
        presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
        wp_presentation_add_listener(presentation, &presentationListener, NULL);
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
    timestampBits = family[chosen].timestampValidBits;
    VkPhysicalDeviceProperties identity;
    vkGetPhysicalDeviceProperties(physical, &identity);
    timestampPeriod = identity.limits.timestampPeriod;
    if (jsonOutput && (!timestampBits || !isfinite(timestampPeriod) || timestampPeriod <= 0)) {
        die("GPU queue timestamps unsupported");
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
    if (jsonOutput) {
        const VkQueryPoolCreateInfo query = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
        check(vkCreateQueryPool(device, &query, NULL, &timing), "GPU timestamp queries");
        printf("{\"event\":\"producer\",\"schema\":1,\"method\":\"delayed-gpu-acquire-v1\","
               "\"sync\":\"%s\",\"requested_delay_ms\":%.3f,\"vendor_id\":%u,\"device_id\":%u,"
               "\"drm_major\":%u,\"drm_minor\":%u,\"queue_family\":%d,\"compute_only_queue\":%s,"
               "\"contention_excluded\":false,\"timestamp_valid_bits\":%u,\"timestamp_period_ns\":%.9g,\"buffer_width\":%d,\"buffer_height\":%d}\n",
               implicitSync ? "implicit" : "explicit", requestedDelay, identity.vendorID, identity.deviceID,
               major(node.st_rdev), minor(node.st_rdev), chosen,
               family[chosen].queueFlags & VK_QUEUE_GRAPHICS_BIT ? "false" : "true", timestampBits, timestampPeriod, WIDTH, HEIGHT);
    }
    getSemaphoreFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");

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
static void finishJob(void)
{
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait GPU fence");
    if (jsonOutput && pendingFrame) {
        uint64_t values[2];
        check(vkGetQueryPoolResults(device, timing, 0, 2, sizeof(values), values, sizeof(uint64_t),
                                   VK_QUERY_RESULT_64_BIT), "GPU timestamp results");
        const uint64_t mask = timestampBits == 64 ? UINT64_MAX : (1ull << timestampBits) - 1;
        const double duration = ((values[1] - values[0]) & mask) * timestampPeriod / 1e6;
        printf("{\"event\":\"gpu-complete\",\"frame\":%" PRIu64 ",\"gpu_duration_ms\":%.6f,"
               "\"submitted_monotonic_ns\":%" PRIu64 ",\"observed_monotonic_ns\":%.0f}\n",
               pendingFrame, duration, pendingSubmitNs, nowMs() * 1e6);
        ++completedJobs;
        pendingFrame = 0;
    }
}

static int runJob(int exportFence)
{
    finishJob();
    vkResetFences(device, 1, &fence);
    vkResetCommandBuffer(commandBuffer, 0);
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(commandBuffer, &begin);
    if (jsonOutput) {
        vkCmdResetQueryPool(commandBuffer, timing, 0, 2);
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing, 0);
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &iterations);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    if (jsonOutput) vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing, 1);
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
        double scaled = (double)iterations * targetMs / elapsed;
        if (!isfinite(scaled) || scaled < 1 || scaled > (double)(1u << 30)) {
            die("GPU delay calibration is outside the supported iteration range");
        }
        iterations = (uint32_t)scaled;
        const double start = nowMs();
        runJob(0);
        elapsed = nowMs() - start;
    }
    if (jsonOutput) printf("{\"event\":\"calibrated\",\"iterations\":%u,\"wall_duration_ms\":%.6f}\n", iterations, elapsed);
    else printf("GPU job: %u iterations, %.0f ms\n", iterations, elapsed);
    fflush(stdout);
}

// -------------------------------------------------------------- buffers --

static void setupBuffers(void)
{
    for (int i = 0; i < BUFFERS; ++i) {
        struct Buffer* b = &buffers[i];
        // Linear: the CPU writes the color directly.
        b->bo = gbm_bo_create(gbm, WIDTH, HEIGHT, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        if (!b->bo) {
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
    uint32_t* pixels = gbm_bo_map(b->bo, 0, 0, WIDTH, HEIGHT, GBM_BO_TRANSFER_WRITE, &stride, &mapping);
    if (!pixels) {
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

static void presentedOutput(void* data, struct wp_presentation_feedback* feedback, struct wl_output* output) { }
static void producerPresent(void* data, struct wp_presentation_feedback* feedback, uint32_t hi, uint32_t lo,
                            uint32_t ns, uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags)
{
    uint64_t* frame = data;
    ++producerPresented;
    printf("{\"event\":\"producer-presented\",\"frame\":%" PRIu64 ",\"received_monotonic_ns\":%.0f,\"flags\":%u}\n",
           *frame, nowMs() * 1e6, flags);
    wp_presentation_feedback_destroy(feedback);
    free(frame);
}
static void producerDiscard(void* data, struct wp_presentation_feedback* feedback)
{
    uint64_t* frame = data;
    ++producerDiscarded;
    printf("{\"event\":\"producer-discarded\",\"frame\":%" PRIu64 ",\"received_monotonic_ns\":%.0f}\n", *frame, nowMs() * 1e6);
    wp_presentation_feedback_destroy(feedback);
    free(frame);
}
static const struct wp_presentation_feedback_listener producerFeedback = { presentedOutput, producerPresent, producerDiscard };

static void draw(void)
{
    static const uint32_t colors[] = { 0xffd04040, 0xff40b040, 0xff4060d0, 0xffd0b040 };
    static unsigned frame;
    struct Buffer* b = freeBuffer(frame);
    fill(b, colors[frame++ % 4]);

    const int syncFile = runJob(1); // the GPU is still working when the commit goes out
    const uint64_t submissionNs = (uint64_t)(nowMs() * 1e6);
    const VkResult fenceState = vkGetFenceStatus(device, fence);
    if (jsonOutput && fenceState != VK_NOT_READY) die("GPU fence was already ready before commit");
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
    if (jsonOutput) {
        uint64_t* id = malloc(sizeof(*id));
        if (!id) die("feedback allocation");
        *id = submittedJobs + 1;
        wp_presentation_feedback_add_listener(wp_presentation_feedback(presentation, surface), &producerFeedback, id);
        if (vkGetFenceStatus(device, fence) != VK_NOT_READY) die("GPU fence became ready before commit");
    }
    wl_surface_commit(surface);
    if (jsonOutput) {
        // A fence cannot become unsignaled again before the next reset.
        // Pending immediately after commit also verifies it was pending
        // during the commit, including time spent importing its sync_file.
        if (vkGetFenceStatus(device, fence) != VK_NOT_READY) die("GPU fence became ready while committing");
        pendingFrame = ++submittedJobs;
        pendingSubmitNs = submissionNs;
        printf("{\"event\":\"producer-commit\",\"frame\":%" PRIu64 ",\"monotonic_ns\":%.0f,\"fence_pending\":true}\n",
               pendingFrame, nowMs() * 1e6);
    }
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
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--implicit")) {
            implicitSync = 1;
        } else if (!strcmp(argv[i], "--fullscreen")) {
            wantFullscreen = 1;
        } else if (!strcmp(argv[i], "--json")) {
            jsonOutput = 1;
        } else if (!strcmp(argv[i], "--until-stdin-eof")) {
            untilEof = 1;
        } else if (!strcmp(argv[i], "--timeout")) {
            if (++i == argc) return 2;
            char* end = NULL;
            errno = 0;
            unsigned long value = strtoul(argv[i], &end, 10);
            if (errno || !*argv[i] || *end || value < 1 || value > 110) return 2;
            timeoutSeconds = (unsigned)value;
        } else if (!strcmp(argv[i], "--help")) {
            puts("Usage: vela-slowgpu [MS] [--implicit] [--fullscreen] [--json --until-stdin-eof [--timeout 1..110]]\n"
                 "JSON mode: bounded startup/runtime, GPU timestamps and fixed synchronization evidence.");
            return 0;
        } else {
            char* end = NULL;
            errno = 0;
            double value = strtod(argv[i], &end);
            if (errno || !*argv[i] || *end || !isfinite(value) || value < 1 || value > 1000) {
                fprintf(stderr, "usage: vela-slowgpu [MS] [--implicit] [--fullscreen] [--json --until-stdin-eof]\n");
                return 2;
            }
            targetMs = value;
        }
    }
    if (!isfinite(targetMs) || targetMs < 1 || targetMs > 1000 || jsonOutput != untilEof) return 2;
    requestedDelay = targetMs;
    if (jsonOutput) {
        WIDTH = 1024;
        HEIGHT = 640;
        setvbuf(stdout, NULL, _IOLBF, 0);
        signal(SIGTERM, terminate);
        signal(SIGINT, terminate);
        signal(SIGALRM, expired);
        alarm(timeoutSeconds);
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
    if (jsonOutput && !presentation) die("presentation-time is required for visible producer evidence");
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

    setupVulkan();
    calibrate(targetMs);
    setupBuffers();

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

    while (running && !stopping) {
        if (frameDue) {
            draw();
        }
        if (!jsonOutput) {
            if (wl_display_dispatch(display) < 0) break;
        } else {
            if (wl_display_dispatch_pending(display) < 0) die("Wayland dispatch failed");
            if (wl_display_flush(display) < 0 && errno != EAGAIN) die("Wayland flush failed");
            struct pollfd fds[] = { { wl_display_get_fd(display), POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
            int result = poll(fds, 2, 50);
            if (result < 0 && errno != EINTR) die("poll failed");
            if (fds[1].revents & (POLLHUP | POLLERR | POLLIN)) {
                char byte;
                if (read(STDIN_FILENO, &byte, 1) <= 0) break;
                die("stdin accepts EOF only");
            }
            if (fds[0].revents & (POLLHUP | POLLERR)) die("Wayland connection lost");
            if (fds[0].revents & POLLIN) {
                if (wl_display_dispatch(display) < 0) die("Wayland connection lost");
            }
        }
    }
    if (jsonOutput) {
        finishJob();
        printf("{\"event\":\"producer-summary\",\"complete\":%s,\"submitted\":%" PRIu64 ",\"completed\":%" PRIu64
               ",\"presented\":%" PRIu64 ",\"discarded\":%" PRIu64 ",\"monotonic_ns\":%.0f}\n",
               !stopping && running ? "true" : "false", submittedJobs, completedJobs, producerPresented, producerDiscarded, nowMs() * 1e6);
    }
    vkDeviceWaitIdle(device);
    return 0;
}
