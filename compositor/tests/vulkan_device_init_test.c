// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the real Vulkan creation path and its out-parameter without a GPU.
// Faults are injected at Vulkan API boundaries; no display or driver is opened.
#undef NDEBUG
#include <assert.h>
#include <string.h>
#include "../src/render/vulkan.c"

static VkResult device_creation_result;
static int device_creations, instance_destroys;
static VkResult format_result;
static VkExternalMemoryFeatureFlags external_features;

void _wlr_log(enum wlr_log_importance importance, const char *format, ...) {}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version)
{
    *version = VK_API_VERSION_1_4;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    *instance = (VkInstance)(uintptr_t)1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    ++instance_destroys;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
    VkPhysicalDevice *devices)
{
    *count = 1;
    if (devices) devices[0] = (VkPhysicalDevice)(uintptr_t)2;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physical,
    VkPhysicalDeviceProperties *properties)
{
    *properties = (VkPhysicalDeviceProperties) {
        .apiVersion = VK_API_VERSION_1_4,
        .deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
    };
    strcpy(properties->deviceName, "Vela Vulkan fault-injection GPU");
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical,
    VkPhysicalDeviceProperties2 *properties)
{
    VkBaseOutStructure *next = properties->pNext;
    if (next && next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
        VkPhysicalDeviceDrmPropertiesEXT *drm = (void *)next;
        drm->hasRender = VK_TRUE;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical,
    const char *layer, uint32_t *count, VkExtensionProperties *properties)
{
    *count = REQUIRED_COUNT;
    if (properties) {
        for (size_t i = 0; i < REQUIRED_COUNT; ++i) {
            snprintf(properties[i].extensionName, sizeof(properties[i].extensionName),
                "%s", required_extensions[i]);
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physical,
    uint32_t *count, VkQueueFamilyProperties *families)
{
    *count = 1;
    if (families) families[0] = (VkQueueFamilyProperties) { .queueFlags = VK_QUEUE_GRAPHICS_BIT };
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical,
    VkPhysicalDeviceFeatures2 *features)
{
    VkPhysicalDeviceVulkan12Features *v12 = features->pNext;
    VkPhysicalDeviceVulkan13Features *v13 = v12->pNext;
    VkPhysicalDeviceVulkan14Features *v14 = v13->pNext;
    v12->timelineSemaphore = VK_TRUE;
    v13->dynamicRendering = VK_TRUE;
    v13->synchronization2 = VK_TRUE;
    v14->pushDescriptor = VK_TRUE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device)
{
    ++device_creations;
    *device = VK_NULL_HANDLE;
    return device_creation_result;
}

static void failed_device_creation(VkResult fault, bool expected_loss)
{
    device_creation_result = fault;
    device_creations = instance_destroys = 0;

    // No real GPU, render node or Wayland session is created.
    bool device_lost = !expected_loss;
    assert(vela_vulkan_create(-1, &device_lost) == NULL);
    assert(device_lost == expected_loss);
    assert(device_creations == 1 && instance_destroys == 1);

    // The API must be safe even when the caller does not request the reason.
    assert(vela_vulkan_create(-1, NULL) == NULL);
    assert(device_creations == 2 && instance_destroys == 2);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physical,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *properties)
{
    VkExternalImageFormatProperties *external = properties->pNext;
    external->externalMemoryProperties.externalMemoryFeatures = external_features;
    return format_result;
}

static void allocator_selection(void)
{
    const VkDriverId drivers[] = { VK_DRIVER_ID_MESA_VENUS, VK_DRIVER_ID_MESA_RADV,
        VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA, VK_DRIVER_ID_NVIDIA_PROPRIETARY };
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); ++i) {
        VkPhysicalDeviceDriverProperties driver = { .driverID = drivers[i] };
        struct vela_vulkan vk = { 0 };
        unsetenv("VELA_ALLOCATOR");
        assert(select_buffer_allocator(&vk, &driver));
        assert(vk.export_buffers == (drivers[i] == VK_DRIVER_ID_MESA_VENUS));
        assert(vk.venus == (drivers[i] == VK_DRIVER_ID_MESA_VENUS));
        setenv("VELA_ALLOCATOR", "auto", 1);
        assert(select_buffer_allocator(&vk, &driver));
        assert(vk.export_buffers == (drivers[i] == VK_DRIVER_ID_MESA_VENUS));
        setenv("VELA_ALLOCATOR", "vulkan", 1);
        assert(select_buffer_allocator(&vk, &driver) && vk.export_buffers);
        setenv("VELA_ALLOCATOR", "gbm", 1);
        assert(select_buffer_allocator(&vk, &driver) && !vk.export_buffers);
        setenv("VELA_ALLOCATOR", "typo", 1);
        assert(!select_buffer_allocator(&vk, &driver));
    }
    unsetenv("VELA_ALLOCATOR");
}

static void client_environment(void)
{
    struct vela_vulkan vk = { 0 };
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
    assert(vela_vulkan_configure_clients(&vk));
    assert(getenv("MESA_LOADER_DRIVER_OVERRIDE") == NULL);
    vk.venus = true;
    assert(vela_vulkan_configure_clients(&vk));
    assert(strcmp(getenv("MESA_LOADER_DRIVER_OVERRIDE"), "zink") == 0);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "virgl", 1);
    assert(vela_vulkan_configure_clients(&vk));
    assert(strcmp(getenv("MESA_LOADER_DRIVER_OVERRIDE"), "virgl") == 0);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "", 1);
    assert(vela_vulkan_configure_clients(&vk));
    assert(strcmp(getenv("MESA_LOADER_DRIVER_OVERRIDE"), "") == 0);
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
}

static void export_capabilities(void)
{
    struct vela_vulkan vk = { 0 };
    for (unsigned flags = 0; flags < 8; ++flags) {
        external_features = flags;
        format_result = VK_SUCCESS;
        bool can_import = (flags & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
        bool can_export = can_import && (flags & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
        assert(vela_vulkan_supports_dmabuf(&vk, VK_FORMAT_B8G8R8A8_SRGB, 0,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == can_import);
        assert(vela_vulkan_can_export_dmabuf(&vk, VK_FORMAT_B8G8R8A8_SRGB, 0,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == can_export);
        format_result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        assert(!vela_vulkan_can_export_dmabuf(&vk, VK_FORMAT_B8G8R8A8_SRGB, 0,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    }
}

int main(void)
{
    failed_device_creation(VK_ERROR_DEVICE_LOST, true);
    failed_device_creation(VK_ERROR_OUT_OF_HOST_MEMORY, false);
    failed_device_creation(VK_ERROR_OUT_OF_DEVICE_MEMORY, false);
    failed_device_creation(VK_ERROR_INITIALIZATION_FAILED, false);
    allocator_selection();
    export_capabilities();
    client_environment();
    return 0;
}
