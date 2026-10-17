// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Vela's Vulkan device (docs/renderer.md §7). One, for the GPU driving the
// outputs; all the renderer's drawing goes through here.

#include "render/render.h"

#include "util.h"
#include "sync.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

// The minimum to exchange buffers with the kernel and with apps (§7.1).
static const char *const required_extensions[] = {
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
};
#define REQUIRED_COUNT (sizeof(required_extensions) / sizeof(required_extensions[0]))

static VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    enum wlr_log_importance level = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? WLR_ERROR
        : severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT                       ? WLR_INFO
                                                                                             : WLR_DEBUG;
    wlr_log(level, "Vulkan: %s", data->pMessage);
    return VK_FALSE;
}

// A device's extensions, in an allocated array to free.
static VkExtensionProperties *device_extensions(VkPhysicalDevice physical, uint32_t *count)
{
    *count = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, count, NULL);
    VkExtensionProperties *list = calloc(*count ? *count : 1, sizeof(*list));
    vkEnumerateDeviceExtensionProperties(physical, NULL, count, list);
    return list;
}

static bool has_extension(const VkExtensionProperties *list, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(list[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool create_instance(struct vela_vulkan *vk)
{
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_4) {
        wlr_log(WLR_ERROR, "Vela needs Vulkan 1.4: the system offers at most %u.%u", VK_API_VERSION_MAJOR(version),
            VK_API_VERSION_MINOR(version));
        return false;
    }

    // Validation layers on request (VELA_VULKAN_VALIDATION=1), if installed.
    const char *layers[1];
    const char *extensions[1];
    uint32_t layer_count = 0;
    if (vela_env_flag("VELA_VULKAN_VALIDATION")) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, NULL);
        VkLayerProperties *available = calloc(count ? count : 1, sizeof(*available));
        vkEnumerateInstanceLayerProperties(&count, available);
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) {
            found = found || strcmp(available[i].layerName, "VK_LAYER_KHRONOS_validation") == 0;
        }
        free(available);
        if (found) {
            layers[0] = "VK_LAYER_KHRONOS_validation";
            extensions[0] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            layer_count = 1;
        } else {
            wlr_log(WLR_ERROR, "VELA_VULKAN_VALIDATION: validation layers not installed "
                               "(package vulkan-validation-layers)");
        }
    }

    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vela-compositor",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "vela",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = layer_count,
        .ppEnabledExtensionNames = extensions,
    };
    if (vkCreateInstance(&info, NULL, &vk->instance) != VK_SUCCESS) {
        wlr_log(WLR_ERROR, "Can't create the Vulkan instance");
        return false;
    }

    if (layer_count > 0) {
        const VkDebugUtilsMessengerCreateInfoEXT messenger = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity
            = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = on_debug_message,
        };
        PFN_vkCreateDebugUtilsMessengerEXT create
            = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk->instance, "vkCreateDebugUtilsMessengerEXT");
        create(vk->instance, &messenger, NULL, &vk->messenger);
        wlr_log(WLR_INFO, "Vulkan validation layers enabled");
    }
    return true;
}

static bool pick_physical_device(struct vela_vulkan *vk, int backend_drm_fd)
{
    dev_t backend_dev = 0;
    struct stat st;
    if (backend_drm_fd >= 0 && fstat(backend_drm_fd, &st) == 0) {
        backend_dev = st.st_rdev;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk->instance, &count, NULL);
    VkPhysicalDevice *devices = calloc(count ? count : 1, sizeof(*devices));
    vkEnumeratePhysicalDevices(vk->instance, &count, devices);

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    for (uint32_t d = 0; d < count; ++d) {
        VkPhysicalDevice candidate = devices[d];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);

        uint32_t ext_count = 0;
        VkExtensionProperties *exts = device_extensions(candidate, &ext_count);
        const char *missing = NULL;
        for (size_t i = 0; i < REQUIRED_COUNT && !missing; ++i) {
            if (!has_extension(exts, ext_count, required_extensions[i])) {
                missing = required_extensions[i];
            }
        }
        free(exts);
        if (props.apiVersion < VK_API_VERSION_1_4 || missing) {
            wlr_log(WLR_INFO, "GPU %s skipped: %s", props.deviceName, missing ? missing : "Vulkan 1.4 not supported");
            continue;
        }

        VkPhysicalDeviceDrmPropertiesEXT drm = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm };
        vkGetPhysicalDeviceProperties2(candidate, &props2);
        if (!drm.hasRender) {
            continue;
        }

        // The output's GPU always wins; without a hint (headless) a dedicated
        // card is preferred.
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
            : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU    ? 1
                                                                            : 0;
        if (backend_dev != 0) {
            dev_t primary = makedev(drm.primaryMajor, drm.primaryMinor);
            dev_t render = makedev(drm.renderMajor, drm.renderMinor);
            if ((drm.hasPrimary && primary == backend_dev) || render == backend_dev) {
                score = 100;
            }
        }
        if (score > best_score) {
            best = candidate;
            best_score = score;
            vk->drm = drm;
            snprintf(vk->name, sizeof(vk->name), "%s", props.deviceName);
        }
    }
    free(devices);

    if (!best) {
        wlr_log(WLR_ERROR, "No GPU with Vulkan 1.4 and the dmabuf extensions: Vela can't draw");
        return false;
    }
    if (backend_dev != 0 && best_score < 100) {
        wlr_log(WLR_ERROR, "The output's GPU has no Vulkan 1.4: using %s", vk->name);
    }
    vk->physical = best;
    return true;
}

// The process can ask for a high-priority queue (CAP_SYS_NICE, bit 23 of
// CapEff, or root): a refused attempt would fill the log with loader errors.
static bool may_raise_priority(void)
{
    bool allowed = geteuid() == 0;
    FILE *status = fopen("/proc/self/status", "r");
    if (status) {
        char line[256];
        while (fgets(line, sizeof(line), status)) {
            unsigned long long caps = 0;
            if (sscanf(line, "CapEff: %llx", &caps) == 1) {
                allowed = allowed || (caps & (1ULL << 23));
            }
        }
        fclose(status);
    }
    return allowed;
}

static bool supports_calibrated_timestamps(struct vela_vulkan *vk)
{
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR get_domains
        = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)vkGetInstanceProcAddr(vk->instance,
            "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
    uint32_t count = 0;
    if (!get_domains || get_domains(vk->physical, &count, NULL) != VK_SUCCESS) {
        return false;
    }
    VkTimeDomainKHR *domains = calloc(count ? count : 1, sizeof(*domains));
    get_domains(vk->physical, &count, domains);
    bool device = false;
    bool monotonic = false;
    for (uint32_t i = 0; i < count; ++i) {
        device = device || domains[i] == VK_TIME_DOMAIN_DEVICE_KHR;
        monotonic = monotonic || domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    }
    free(domains);
    return device && monotonic;
}

static bool create_device(struct vela_vulkan *vk)
{
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count ? family_count : 1, sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &family_count, families);
    bool found = false;
    for (uint32_t i = 0; i < family_count && !found; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            vk->queue_family = i;
            found = true;
        }
    }
    uint32_t valid_bits = found ? families[vk->queue_family].timestampValidBits : 0;
    free(families);
    if (!found) {
        wlr_log(WLR_ERROR, "%s: no graphics queue", vk->name);
        return false;
    }

    // What we use from core: timeline semaphores (1.2) to know when the GPU is
    // done, dynamic rendering and synchronization2 (1.3), push descriptors
    // (1.4) to bind textures without descriptor pools.
    VkPhysicalDeviceVulkan14Features supported14 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
    VkPhysicalDeviceVulkan13Features supported13
        = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supported14 };
    VkPhysicalDeviceVulkan12Features supported12
        = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supported13 };
    VkPhysicalDeviceFeatures2 supported = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported12 };
    vkGetPhysicalDeviceFeatures2(vk->physical, &supported);
    if (!supported12.timelineSemaphore || !supported13.dynamicRendering || !supported13.synchronization2
        || !supported14.pushDescriptor) {
        wlr_log(WLR_ERROR, "%s: required Vulkan 1.4 features are missing", vk->name);
        return false;
    }
    VkPhysicalDeviceVulkan14Features enabled14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pushDescriptor = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features enabled13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &enabled14,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features enabled12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &enabled13,
        .timelineSemaphore = VK_TRUE,
    };

    // The extensions: the required ones, calibrated timestamps (optional:
    // when, on our clock, the GPU finishes a frame) and the high-priority
    // queue (§4.3: the compositor's frames go ahead of the apps' work, even a
    // game keeping the GPU at 100%; the kernel grants it only to whoever has
    // CAP_SYS_NICE, like kwin_wayland).
    uint32_t ext_count = 0;
    VkExtensionProperties *exts = device_extensions(vk->physical, &ext_count);
    const char *extensions[REQUIRED_COUNT + 2];
    uint32_t extension_count = 0;
    for (size_t i = 0; i < REQUIRED_COUNT; ++i) {
        extensions[extension_count++] = required_extensions[i];
    }
    bool calibrated = has_extension(exts, ext_count, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
        && supports_calibrated_timestamps(vk);
    if (calibrated) {
        extensions[extension_count++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }
    bool may_raise = may_raise_priority();
    bool global_priority = may_raise && has_extension(exts, ext_count, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);
    if (global_priority) {
        extensions[extension_count++] = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
    }
    free(exts);

    const VkDeviceQueueGlobalPriorityCreateInfoKHR high_priority = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
        .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,
    };
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = global_priority ? &high_priority : NULL,
        .queueFamilyIndex = vk->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extensions,
    };
    VkResult result = vkCreateDevice(vk->physical, &device_info, NULL, &vk->device);
    if (global_priority && (result == VK_ERROR_NOT_PERMITTED_KHR || result == VK_ERROR_INITIALIZATION_FAILED)) {
        queue_info.pNext = NULL;
        result = vkCreateDevice(vk->physical, &device_info, NULL, &vk->device);
        wlr_log(WLR_INFO, "%s: normal-priority GPU queue (high priority was refused)", vk->name);
    } else if (global_priority && result == VK_SUCCESS) {
        wlr_log(WLR_INFO, "%s: high-priority GPU queue", vk->name);
    } else if (!may_raise) {
        wlr_log(WLR_INFO, "%s: normal-priority GPU queue (high priority needs CAP_SYS_NICE)", vk->name);
    }
    if (result != VK_SUCCESS) {
        vk->device = VK_NULL_HANDLE;
        wlr_log(WLR_ERROR, "%s: can't create the Vulkan device", vk->name);
        return false;
    }
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);

    if (valid_bits > 0) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vk->physical, &props);
        vk->timestamp_period = props.limits.timestampPeriod;
        vk->timestamp_mask = valid_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << valid_bits) - 1;
    }
    if (calibrated) {
        vk->get_calibrated_timestamps
            = (PFN_vkGetCalibratedTimestampsKHR)vkGetDeviceProcAddr(vk->device, "vkGetCalibratedTimestampsKHR");
    }
    if (vk->timestamp_period <= 0.0f) {
        wlr_log(WLR_INFO, "%s: no GPU timestamps, frame cost is estimated from the CPU", vk->name);
    } else if (!vk->get_calibrated_timestamps) {
        wlr_log(WLR_INFO, "%s: no calibrated timestamps, only the GPU work duration is measured", vk->name);
    }

    vk->get_memory_fd_properties
        = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");
    vk->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(vk->device, "vkGetSemaphoreFdKHR");
    vk->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(vk->device, "vkImportSemaphoreFdKHR");

    const VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkExternalSemaphoreProperties semaphore_props = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vkGetPhysicalDeviceExternalSemaphoreProperties(vk->physical, &semaphore_info, &semaphore_props);
    const VkExternalSemaphoreFeatureFlags both
        = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    bool sync_supported = (semaphore_props.externalSemaphoreFeatures & both) == both;
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &driver,
    };
    vkGetPhysicalDeviceProperties2(vk->physical, &properties);
    bool nvidia = driver.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    unsigned driver_major = properties.properties.driverVersion >> 22;
    const char *override = getenv("VELA_SYNC_FILE");
    vk->sync_file = vela_sync_file_enabled(sync_supported, nvidia, driver_major, override);
    if (!vk->sync_file) {
        wlr_log(WLR_INFO, "%s: CPU synchronization (driver %s; sync_file support=%d; VELA_SYNC_FILE=%s)",
            vk->name, driver.driverInfo, sync_supported, override ? override : "auto");
        if (sync_supported && nvidia && driver_major == 580 && !override) {
            wlr_log(WLR_INFO, "NVIDIA 580 compatibility: Vulkan sync_file imports/exports and client explicit sync "
                "disabled to avoid descriptor exhaustion; VELA_SYNC_FILE=1 opts back in");
        }
    }
    return true;
}

static bool open_render_node(struct vela_vulkan *vk)
{
    drmDevice *dev = NULL;
    if (drmGetDeviceFromDevId(makedev(vk->drm.renderMajor, vk->drm.renderMinor), 0, &dev) != 0) {
        wlr_log(WLR_ERROR, "%s: render node %ld:%ld not found", vk->name, (long)vk->drm.renderMajor,
            (long)vk->drm.renderMinor);
        return false;
    }
    char path[256] = "";
    if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
        snprintf(path, sizeof(path), "%s", dev->nodes[DRM_NODE_RENDER]);
    }
    drmFreeDevice(&dev);
    if (!path[0] || (vk->render_fd = open(path, O_RDWR | O_CLOEXEC)) < 0) {
        wlr_log_errno(WLR_ERROR, "%s: can't open the render node %s", vk->name, path);
        return false;
    }
    wlr_log(WLR_INFO, "Vela renderer: %s (%s)", vk->name, path);
    return true;
}

bool vela_vulkan_supports_dmabuf(const struct vela_vulkan *vk, VkFormat format, uint64_t modifier,
    VkImageUsageFlags usage)
{
    const VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VkPhysicalDeviceExternalImageFormatInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &modifier_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    const VkPhysicalDeviceImageFormatInfo2 image_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_props = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 image_props
        = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external_props };
    if (vkGetPhysicalDeviceImageFormatProperties2(vk->physical, &image_info, &image_props) != VK_SUCCESS) {
        return false;
    }
    return external_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
}

// A format's modifiers with their properties, in an allocated array.
static VkDrmFormatModifierPropertiesEXT *format_modifiers(const struct vela_vulkan *vk, VkFormat format,
    uint32_t *count, VkFormatFeatureFlags *optimal_features)
{
    VkDrmFormatModifierPropertiesListEXT list = { .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list };
    vkGetPhysicalDeviceFormatProperties2(vk->physical, format, &props);
    *count = list.drmFormatModifierCount;
    VkDrmFormatModifierPropertiesEXT *modifiers = calloc(*count ? *count : 1, sizeof(*modifiers));
    list.pDrmFormatModifierProperties = modifiers;
    vkGetPhysicalDeviceFormatProperties2(vk->physical, format, &props);
    if (optimal_features) {
        *optimal_features = props.formatProperties.optimalTilingFeatures;
    }
    return modifiers;
}

static void query_formats(struct vela_vulkan *vk)
{
    const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    for (int f = 0; f < vela_pixel_format_count; ++f) {
        const struct vela_pixel_format *format = &vela_pixel_formats[f];
        // dmabuf: for each modifier, whether it can be read (app textures) and
        // drawn on (outputs, captures, cursors).
        uint32_t count = 0;
        VkFormatFeatureFlags optimal = 0;
        VkDrmFormatModifierPropertiesEXT *modifiers = format_modifiers(vk, format->unorm, &count, &optimal);
        for (uint32_t i = 0; i < count; ++i) {
            if ((modifiers[i].drmFormatModifierTilingFeatures & sampled) == sampled
                && vela_vulkan_supports_dmabuf(vk, format->unorm, modifiers[i].drmFormatModifier,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
                wlr_drm_format_set_add(&vk->texture_formats, format->drm, modifiers[i].drmFormatModifier);
            }
        }
        free(modifiers);

        if (format->srgb != VK_FORMAT_UNDEFINED) {
            modifiers = format_modifiers(vk, format->srgb, &count, NULL);
            for (uint32_t i = 0; i < count; ++i) {
                if ((modifiers[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
                    && vela_vulkan_supports_dmabuf(vk, format->srgb, modifiers[i].drmFormatModifier,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
                    wlr_drm_format_set_add(&vk->render_formats, format->drm, modifiers[i].drmFormatModifier);
                }
            }
            free(modifiers);
        }

        // Shared memory: we copy it into an image of ours.
        const VkFormatFeatureFlags shm_needs = sampled | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((optimal & shm_needs) == shm_needs && vk->shm_format_count < VELA_MAX_SHM_FORMATS) {
            vk->shm_formats[vk->shm_format_count++] = format->drm;
        }
    }
}

struct vela_vulkan *vela_vulkan_create(int backend_drm_fd)
{
    struct vela_vulkan *vk = calloc(1, sizeof(*vk));
    vk->render_fd = -1;
    if (!create_instance(vk) || !pick_physical_device(vk, backend_drm_fd) || !create_device(vk)
        || !open_render_node(vk)) {
        vela_vulkan_destroy(vk);
        return NULL;
    }
    query_formats(vk);
    if (vk->render_formats.len == 0) {
        wlr_log(WLR_ERROR, "%s: no format usable for drawing to outputs", vk->name);
        vela_vulkan_destroy(vk);
        return NULL;
    }
    return vk;
}

void vela_vulkan_destroy(struct vela_vulkan *vk)
{
    if (!vk) {
        return;
    }
    wlr_drm_format_set_finish(&vk->render_formats);
    wlr_drm_format_set_finish(&vk->texture_formats);
    if (vk->render_fd >= 0) {
        close(vk->render_fd);
    }
    if (vk->device) {
        vkDestroyDevice(vk->device, NULL);
    }
    if (vk->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            vk->instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy(vk->instance, vk->messenger, NULL);
    }
    if (vk->instance) {
        vkDestroyInstance(vk->instance, NULL);
    }
    free(vk);
}

int vela_vulkan_render_fd(const struct vela_vulkan *vk)
{
    return vk->render_fd;
}

uint32_t vela_vulkan_memory_type(const struct vela_vulkan *vk, uint32_t type_bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool vela_vulkan_import_dmabuf(const struct vela_vulkan *vk, const struct wlr_dmabuf_attributes *dmabuf,
    VkFormat format, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory)
{
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    if (dmabuf->n_planes < 1 || dmabuf->n_planes > WLR_DMABUF_MAX_PLANES) {
        wlr_log(WLR_ERROR, "DMABUF import: invalid plane count %d", dmabuf->n_planes);
        return false;
    }
    // No "disjoint" images: RGB formats don't need them.
    struct stat first;
    if (fstat(dmabuf->fd[0], &first) < 0) {
        wlr_log_errno(WLR_ERROR, "DMABUF import: fstat plane 0 fd=%d", dmabuf->fd[0]);
        return false;
    }
    for (int i = 1; i < dmabuf->n_planes; ++i) {
        struct stat other;
        if (fstat(dmabuf->fd[i], &other) < 0) {
            wlr_log_errno(WLR_ERROR, "DMABUF import: fstat plane %d fd=%d", i, dmabuf->fd[i]);
            return false;
        }
        if (other.st_dev != first.st_dev || other.st_ino != first.st_ino) {
            wlr_log(WLR_ERROR, "DMABUF import: planes in different buffers (plane %d): not supported", i);
            return false;
        }
    }

    VkSubresourceLayout planes[WLR_DMABUF_MAX_PLANES] = { 0 };
    for (int i = 0; i < dmabuf->n_planes; ++i) {
        planes[i].offset = dmabuf->offset[i];
        planes[i].rowPitch = dmabuf->stride[i];
    }
    const VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = dmabuf->modifier,
        .drmFormatModifierPlaneCount = (uint32_t)dmabuf->n_planes,
        .pPlaneLayouts = planes,
    };
    const VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { (uint32_t)dmabuf->width, (uint32_t)dmabuf->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult image_result = vkCreateImage(vk->device, &image_info, NULL, image);
    if (image_result != VK_SUCCESS) {
        wlr_log(WLR_ERROR, "DMABUF import: vkCreateImage failed: VkResult=%d", (int)image_result);
        *image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk->device, *image, &requirements);
    VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VkResult fd_result = vk->get_memory_fd_properties(
        vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf->fd[0], &fd_props);
    if (fd_result != VK_SUCCESS) {
        wlr_log(WLR_ERROR, "DMABUF import: vkGetMemoryFdPropertiesKHR failed: VkResult=%d", (int)fd_result);
        vkDestroyImage(vk->device, *image, NULL);
        *image = VK_NULL_HANDLE;
        return false;
    }
    uint32_t allowed_types = requirements.memoryTypeBits & fd_props.memoryTypeBits;
    uint32_t memory_type = vela_vulkan_memory_type(vk, allowed_types, 0);
    if (memory_type == UINT32_MAX) {
        wlr_log(WLR_ERROR, "DMABUF import: no compatible Vulkan memory type (mask=0x%x)", allowed_types);
        vkDestroyImage(vk->device, *image, NULL);
        *image = VK_NULL_HANDLE;
        return false;
    }

    // Vulkan takes ownership of the descriptor on successful import, so give it a copy.
    int fd = fcntl(dmabuf->fd[0], F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        wlr_log_errno(WLR_ERROR, "DMABUF import: cannot duplicate fd=%d", dmabuf->fd[0]);
        vkDestroyImage(vk->device, *image, NULL);
        *image = VK_NULL_HANDLE;
        return false;
    }
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = *image,
    };
    const VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd,
    };
    const VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VkResult allocate_result = vkAllocateMemory(vk->device, &alloc_info, NULL, memory);
    if (allocate_result != VK_SUCCESS) {
        wlr_log(WLR_ERROR, "DMABUF import: vkAllocateMemory failed: VkResult=%d", (int)allocate_result);
        // Vulkan consumes the duplicated FD only if the import succeeds.
        close(fd);
        vkDestroyImage(vk->device, *image, NULL);
        *image = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        return false;
    }
    VkResult bind_result = vkBindImageMemory(vk->device, *image, *memory, 0);
    if (bind_result != VK_SUCCESS) {
        wlr_log(WLR_ERROR, "DMABUF import: vkBindImageMemory failed: VkResult=%d", (int)bind_result);
        vkDestroyImage(vk->device, *image, NULL);
        vkFreeMemory(vk->device, *memory, NULL);
        *image = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

VkShaderModule vela_vulkan_shader(const struct vela_vulkan *vk, const uint32_t *code, size_t bytes)
{
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode = code,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(vk->device, &info, NULL, &module);
    return module;
}
