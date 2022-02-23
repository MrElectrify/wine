/* Wine Vulkan ICD implementation
 *
 * Copyright 2017 Roderick Colenbrander
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <math.h>
#include <time.h>
#include <stdlib.h>

#include "vulkan_private.h"
#include "winreg.h"
#include "ntuser.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#define wine_vk_find_struct(s, t) wine_vk_find_struct_((void *)s, VK_STRUCTURE_TYPE_##t)
static void *wine_vk_find_struct_(void *s, VkStructureType t)
{
    VkBaseOutStructure *header;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t)
            return header;
    }

    return NULL;
}

#define wine_vk_count_struct(s, t) wine_vk_count_struct_((void *)s, VK_STRUCTURE_TYPE_##t)
static uint32_t wine_vk_count_struct_(void *s, VkStructureType t)
{
    const VkBaseInStructure *header;
    uint32_t result = 0;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t)
            result++;
    }

    return result;
}

static const struct vulkan_funcs *vk_funcs;

#define WINE_VK_ADD_DISPATCHABLE_MAPPING(instance, object, native_handle) \
    wine_vk_add_handle_mapping((instance), (uint64_t) (uintptr_t) (object), (uint64_t) (uintptr_t) (native_handle), &(object)->mapping)
#define WINE_VK_ADD_NON_DISPATCHABLE_MAPPING(instance, object, native_handle) \
    wine_vk_add_handle_mapping((instance), (uint64_t) (uintptr_t) (object), (uint64_t) (native_handle), &(object)->mapping)
static void  wine_vk_add_handle_mapping(struct VkInstance_T *instance, uint64_t wrapped_handle,
        uint64_t native_handle, struct wine_vk_mapping *mapping)
{
    if (instance->enable_wrapper_list)
    {
        mapping->native_handle = native_handle;
        mapping->wine_wrapped_handle = wrapped_handle;
        pthread_rwlock_wrlock(&instance->wrapper_lock);
        list_add_tail(&instance->wrappers, &mapping->link);
        pthread_rwlock_unlock(&instance->wrapper_lock);
    }
}

#define WINE_VK_REMOVE_HANDLE_MAPPING(instance, object) \
    wine_vk_remove_handle_mapping((instance), &(object)->mapping)
static void wine_vk_remove_handle_mapping(struct VkInstance_T *instance, struct wine_vk_mapping *mapping)
{
    if (instance->enable_wrapper_list)
    {
        pthread_rwlock_wrlock(&instance->wrapper_lock);
        list_remove(&mapping->link);
        pthread_rwlock_unlock(&instance->wrapper_lock);
    }
}

static uint64_t wine_vk_get_wrapper(struct VkInstance_T *instance, uint64_t native_handle)
{
    struct wine_vk_mapping *mapping;
    uint64_t result = 0;

    pthread_rwlock_rdlock(&instance->wrapper_lock);
    LIST_FOR_EACH_ENTRY(mapping, &instance->wrappers, struct wine_vk_mapping, link)
    {
        if (mapping->native_handle == native_handle)
        {
            result = mapping->wine_wrapped_handle;
            break;
        }
    }
    pthread_rwlock_unlock(&instance->wrapper_lock);
    return result;
}

static VkBool32 debug_utils_callback_conversion(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT message_types,
    const VkDebugUtilsMessengerCallbackDataEXT_host *callback_data,
    void *user_data)
{
    struct wine_vk_debug_utils_params params;
    VkDebugUtilsObjectNameInfoEXT *object_name_infos;
    struct wine_debug_utils_messenger *object;
    void *ret_ptr;
    ULONG ret_len;
    VkBool32 result;
    unsigned int i;

    TRACE("%i, %u, %p, %p\n", severity, message_types, callback_data, user_data);

    object = user_data;

    if (!object->instance->instance)
    {
        /* instance wasn't yet created, this is a message from the native loader */
        return VK_FALSE;
    }

    /* FIXME: we should pack all referenced structs instead of passing pointers */
    params.user_callback = object->user_callback;
    params.user_data = object->user_data;
    params.severity = severity;
    params.message_types = message_types;
    params.data = *((VkDebugUtilsMessengerCallbackDataEXT *) callback_data);

    object_name_infos = calloc(params.data.objectCount, sizeof(*object_name_infos));

    for (i = 0; i < params.data.objectCount; i++)
    {
        object_name_infos[i].sType = callback_data->pObjects[i].sType;
        object_name_infos[i].pNext = callback_data->pObjects[i].pNext;
        object_name_infos[i].objectType = callback_data->pObjects[i].objectType;
        object_name_infos[i].pObjectName = callback_data->pObjects[i].pObjectName;

        if (wine_vk_is_type_wrapped(callback_data->pObjects[i].objectType))
        {
            object_name_infos[i].objectHandle = wine_vk_get_wrapper(object->instance, callback_data->pObjects[i].objectHandle);
            if (!object_name_infos[i].objectHandle)
            {
                WARN("handle conversion failed 0x%s\n", wine_dbgstr_longlong(callback_data->pObjects[i].objectHandle));
                free(object_name_infos);
                return VK_FALSE;
            }
        }
        else
        {
            object_name_infos[i].objectHandle = callback_data->pObjects[i].objectHandle;
        }
    }

    params.data.pObjects = object_name_infos;

    /* applications should always return VK_FALSE */
    result = KeUserModeCallback( NtUserCallVulkanDebugUtilsCallback, &params, sizeof(params),
                                 &ret_ptr, &ret_len );

    free(object_name_infos);

    return result;
}

static VkBool32 debug_report_callback_conversion(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT object_type,
    uint64_t object_handle, size_t location, int32_t code, const char *layer_prefix, const char *message, void *user_data)
{
    struct wine_vk_debug_report_params params;
    struct wine_debug_report_callback *object;
    void *ret_ptr;
    ULONG ret_len;

    TRACE("%#x, %#x, 0x%s, 0x%s, %d, %p, %p, %p\n", flags, object_type, wine_dbgstr_longlong(object_handle),
        wine_dbgstr_longlong(location), code, layer_prefix, message, user_data);

    object = user_data;

    if (!object->instance->instance)
    {
        /* instance wasn't yet created, this is a message from the native loader */
        return VK_FALSE;
    }

    /* FIXME: we should pack all referenced structs instead of passing pointers */
    params.user_callback = object->user_callback;
    params.user_data = object->user_data;
    params.flags = flags;
    params.object_type = object_type;
    params.location = location;
    params.code = code;
    params.layer_prefix = layer_prefix;
    params.message = message;

    params.object_handle = wine_vk_get_wrapper(object->instance, object_handle);
    if (!params.object_handle)
        params.object_type = VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT;

    return KeUserModeCallback( NtUserCallVulkanDebugReportCallback, &params, sizeof(params),
                               &ret_ptr, &ret_len );
}

static void wine_vk_physical_device_free(struct VkPhysicalDevice_T *phys_dev)
{
    if (!phys_dev)
        return;

    WINE_VK_REMOVE_HANDLE_MAPPING(phys_dev->instance, phys_dev);
    free(phys_dev->extensions);
    free(phys_dev);
}

static struct VkPhysicalDevice_T *wine_vk_physical_device_alloc(struct VkInstance_T *instance,
        VkPhysicalDevice phys_dev)
{
    struct VkPhysicalDevice_T *object;
    uint32_t num_host_properties, num_properties = 0;
    VkExtensionProperties *host_properties = NULL;
    VkResult res;
    unsigned int i, j;

    if (!(object = calloc(1, sizeof(*object))))
        return NULL;

    object->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
    object->instance = instance;
    object->phys_dev = phys_dev;

    WINE_VK_ADD_DISPATCHABLE_MAPPING(instance, object, phys_dev);

    res = instance->funcs.p_vkEnumerateDeviceExtensionProperties(phys_dev,
            NULL, &num_host_properties, NULL);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to enumerate device extensions, res=%d\n", res);
        goto err;
    }

    host_properties = calloc(num_host_properties, sizeof(*host_properties));
    if (!host_properties)
    {
        ERR("Failed to allocate memory for device properties!\n");
        goto err;
    }

    res = instance->funcs.p_vkEnumerateDeviceExtensionProperties(phys_dev,
            NULL, &num_host_properties, host_properties);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to enumerate device extensions, res=%d\n", res);
        goto err;
    }

    /* Count list of extensions for which we have an implementation.
     * TODO: perform translation for platform specific extensions.
     */
    for (i = 0; i < num_host_properties; i++)
    {
        if (wine_vk_device_extension_supported(host_properties[i].extensionName))
        {
            TRACE("Enabling extension '%s' for physical device %p\n", host_properties[i].extensionName, object);
            num_properties++;
        }
        else
        {
            TRACE("Skipping extension '%s', no implementation found in winevulkan.\n", host_properties[i].extensionName);
        }
    }

    TRACE("Host supported extensions %u, Wine supported extensions %u\n", num_host_properties, num_properties);

    if (!(object->extensions = calloc(num_properties, sizeof(*object->extensions))))
    {
        ERR("Failed to allocate memory for device extensions!\n");
        goto err;
    }

    for (i = 0, j = 0; i < num_host_properties; i++)
    {
        if (wine_vk_device_extension_supported(host_properties[i].extensionName))
        {
            object->extensions[j] = host_properties[i];
            j++;
        }
    }
    object->extension_count = num_properties;

    free(host_properties);
    return object;

err:
    wine_vk_physical_device_free(object);
    free(host_properties);
    return NULL;
}

static void wine_vk_free_command_buffers(struct VkDevice_T *device,
        struct wine_cmd_pool *pool, uint32_t count, const VkCommandBuffer *buffers)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        if (!buffers[i])
            continue;

        device->funcs.p_vkFreeCommandBuffers(device->device, pool->command_pool, 1, &buffers[i]->command_buffer);
        list_remove(&buffers[i]->pool_link);
        WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, buffers[i]);
        free(buffers[i]);
    }
}

static void wine_vk_device_get_queues(struct VkDevice_T *device,
        uint32_t family_index, uint32_t queue_count, VkDeviceQueueCreateFlags flags,
        struct VkQueue_T* queues)
{
    VkDeviceQueueInfo2 queue_info;
    unsigned int i;

    for (i = 0; i < queue_count; i++)
    {
        struct VkQueue_T *queue = &queues[i];

        queue->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
        queue->device = device;
        queue->family_index = family_index;
        queue->queue_index = i;
        queue->flags = flags;

        /* The Vulkan spec says:
         *
         * "vkGetDeviceQueue must only be used to get queues that were created
         * with the flags parameter of VkDeviceQueueCreateInfo set to zero."
         */
        if (flags && device->funcs.p_vkGetDeviceQueue2)
        {
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
            queue_info.pNext = NULL;
            queue_info.flags = flags;
            queue_info.queueFamilyIndex = family_index;
            queue_info.queueIndex = i;
            device->funcs.p_vkGetDeviceQueue2(device->device, &queue_info, &queue->queue);
        }
        else
        {
            device->funcs.p_vkGetDeviceQueue(device->device, family_index, i, &queue->queue);
        }

        WINE_VK_ADD_DISPATCHABLE_MAPPING(device->phys_dev->instance, queue, queue->queue);
    }
}

static void wine_vk_device_free_create_info(VkDeviceCreateInfo *create_info)
{
    free_VkDeviceCreateInfo_struct_chain(create_info);
}

static VkResult wine_vk_device_convert_create_info(const VkDeviceCreateInfo *src,
        VkDeviceCreateInfo *dst)
{
    unsigned int i;
    VkResult res;

    *dst = *src;

    if ((res = convert_VkDeviceCreateInfo_struct_chain(src->pNext, dst)) < 0)
    {
        WARN("Failed to convert VkDeviceCreateInfo pNext chain, res=%d.\n", res);
        return res;
    }

    /* Should be filtered out by loader as ICDs don't support layers. */
    dst->enabledLayerCount = 0;
    dst->ppEnabledLayerNames = NULL;

    TRACE("Enabled %u extensions.\n", dst->enabledExtensionCount);
    for (i = 0; i < dst->enabledExtensionCount; i++)
    {
        const char *extension_name = dst->ppEnabledExtensionNames[i];
        TRACE("Extension %u: %s.\n", i, debugstr_a(extension_name));
        if (!wine_vk_device_extension_supported(extension_name))
        {
            WARN("Extension %s is not supported.\n", debugstr_a(extension_name));
            wine_vk_device_free_create_info(dst);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    return VK_SUCCESS;
}

/* Helper function used for freeing a device structure. This function supports full
 * and partial object cleanups and can thus be used for vkCreateDevice failures.
 */
static void wine_vk_device_free(struct VkDevice_T *device)
{
    struct VkQueue_T *queue;

    if (!device)
        return;

    if (device->queues)
    {
        unsigned int i;
        for (i = 0; i < device->queue_count; i++)
        {
            queue = &device->queues[i];
            if (queue && queue->queue)
                WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, queue);
        }
        free(device->queues);
        device->queues = NULL;
    }

    if (device->device && device->funcs.p_vkDestroyDevice)
    {
        WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, device);
        device->funcs.p_vkDestroyDevice(device->device, NULL /* pAllocator */);
    }

    free(device);
}

NTSTATUS init_vulkan(void *args)
{
    vk_funcs = *(const struct vulkan_funcs **)args;
    *(const struct unix_funcs **)args = &loader_funcs;
    return STATUS_SUCCESS;
}

/* Helper function for converting between win32 and host compatible VkInstanceCreateInfo.
 * This function takes care of extensions handled at winevulkan layer, a Wine graphics
 * driver is responsible for handling e.g. surface extensions.
 */
static VkResult wine_vk_instance_convert_create_info(const VkInstanceCreateInfo *src,
        VkInstanceCreateInfo *dst, struct VkInstance_T *object)
{
    VkDebugUtilsMessengerCreateInfoEXT *debug_utils_messenger;
    VkDebugReportCallbackCreateInfoEXT *debug_report_callback;
    VkBaseInStructure *header;
    unsigned int i;
    VkResult res;

    *dst = *src;

    if ((res = convert_VkInstanceCreateInfo_struct_chain(src->pNext, dst)) < 0)
    {
        WARN("Failed to convert VkInstanceCreateInfo pNext chain, res=%d.\n", res);
        return res;
    }

    object->utils_messenger_count = wine_vk_count_struct(dst, DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    object->utils_messengers =  calloc(object->utils_messenger_count, sizeof(*object->utils_messengers));
    header = (VkBaseInStructure *) dst;
    for (i = 0; i < object->utils_messenger_count; i++)
    {
        header = wine_vk_find_struct(header->pNext, DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
        debug_utils_messenger = (VkDebugUtilsMessengerCreateInfoEXT *) header;

        object->utils_messengers[i].instance = object;
        object->utils_messengers[i].debug_messenger = VK_NULL_HANDLE;
        object->utils_messengers[i].user_callback = debug_utils_messenger->pfnUserCallback;
        object->utils_messengers[i].user_data = debug_utils_messenger->pUserData;

        /* convert_VkInstanceCreateInfo_struct_chain already copied the chain,
         * so we can modify it in-place.
         */
        debug_utils_messenger->pfnUserCallback = (void *) &debug_utils_callback_conversion;
        debug_utils_messenger->pUserData = &object->utils_messengers[i];
    }

    debug_report_callback = wine_vk_find_struct(header->pNext, DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT);
    if (debug_report_callback)
    {
        object->default_callback.instance = object;
        object->default_callback.debug_callback = VK_NULL_HANDLE;
        object->default_callback.user_callback = debug_report_callback->pfnCallback;
        object->default_callback.user_data = debug_report_callback->pUserData;

        debug_report_callback->pfnCallback = (void *) &debug_report_callback_conversion;
        debug_report_callback->pUserData = &object->default_callback;
    }

    /* ICDs don't support any layers, so nothing to copy. Modern versions of the loader
     * filter this data out as well.
     */
    if (object->quirks & WINEVULKAN_QUIRK_IGNORE_EXPLICIT_LAYERS) {
        dst->enabledLayerCount = 0;
        dst->ppEnabledLayerNames = NULL;
        WARN("Ignoring explicit layers!\n");
    } else if (dst->enabledLayerCount) {
        FIXME("Loading explicit layers is not supported by winevulkan!\n");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    TRACE("Enabled %u instance extensions.\n", dst->enabledExtensionCount);
    for (i = 0; i < dst->enabledExtensionCount; i++)
    {
        const char *extension_name = dst->ppEnabledExtensionNames[i];
        TRACE("Extension %u: %s.\n", i, debugstr_a(extension_name));
        if (!wine_vk_instance_extension_supported(extension_name))
        {
            WARN("Extension %s is not supported.\n", debugstr_a(extension_name));
            free_VkInstanceCreateInfo_struct_chain(dst);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (!strcmp(extension_name, "VK_EXT_debug_utils") || !strcmp(extension_name, "VK_EXT_debug_report"))
        {
            object->enable_wrapper_list = VK_TRUE;
        }
    }

    return VK_SUCCESS;
}

/* Helper function which stores wrapped physical devices in the instance object. */
static VkResult wine_vk_instance_load_physical_devices(struct VkInstance_T *instance)
{
    VkPhysicalDevice *tmp_phys_devs;
    uint32_t phys_dev_count;
    unsigned int i;
    VkResult res;

    res = instance->funcs.p_vkEnumeratePhysicalDevices(instance->instance, &phys_dev_count, NULL);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to enumerate physical devices, res=%d\n", res);
        return res;
    }
    if (!phys_dev_count)
        return res;

    if (!(tmp_phys_devs = calloc(phys_dev_count, sizeof(*tmp_phys_devs))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = instance->funcs.p_vkEnumeratePhysicalDevices(instance->instance, &phys_dev_count, tmp_phys_devs);
    if (res != VK_SUCCESS)
    {
        free(tmp_phys_devs);
        return res;
    }

    instance->phys_devs = calloc(phys_dev_count, sizeof(*instance->phys_devs));
    if (!instance->phys_devs)
    {
        free(tmp_phys_devs);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    /* Wrap each native physical device handle into a dispatchable object for the ICD loader. */
    for (i = 0; i < phys_dev_count; i++)
    {
        struct VkPhysicalDevice_T *phys_dev = wine_vk_physical_device_alloc(instance, tmp_phys_devs[i]);
        if (!phys_dev)
        {
            ERR("Unable to allocate memory for physical device!\n");
            free(tmp_phys_devs);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        instance->phys_devs[i] = phys_dev;
        instance->phys_dev_count = i + 1;
    }
    instance->phys_dev_count = phys_dev_count;

    free(tmp_phys_devs);
    return VK_SUCCESS;
}

static struct VkPhysicalDevice_T *wine_vk_instance_wrap_physical_device(struct VkInstance_T *instance,
        VkPhysicalDevice physical_device)
{
    unsigned int i;

    for (i = 0; i < instance->phys_dev_count; ++i)
    {
        struct VkPhysicalDevice_T *current = instance->phys_devs[i];
        if (current->phys_dev == physical_device)
            return current;
    }

    ERR("Unrecognized physical device %p.\n", physical_device);
    return NULL;
}

/* Helper function used for freeing an instance structure. This function supports full
 * and partial object cleanups and can thus be used for vkCreateInstance failures.
 */
static void wine_vk_instance_free(struct VkInstance_T *instance)
{
    if (!instance)
        return;

    if (instance->phys_devs)
    {
        unsigned int i;

        for (i = 0; i < instance->phys_dev_count; i++)
        {
            wine_vk_physical_device_free(instance->phys_devs[i]);
        }
        free(instance->phys_devs);
    }

    if (instance->instance)
    {
        vk_funcs->p_vkDestroyInstance(instance->instance, NULL /* allocator */);
        WINE_VK_REMOVE_HANDLE_MAPPING(instance, instance);
    }

    pthread_rwlock_destroy(&instance->wrapper_lock);
    free(instance->utils_messengers);

    free(instance);
}

NTSTATUS wine_vkAllocateCommandBuffers(void *args)
{
    struct vkAllocateCommandBuffers_params *params = args;
    VkDevice device = params->device;
    const VkCommandBufferAllocateInfo *allocate_info = params->pAllocateInfo;
    VkCommandBuffer *buffers = params->pCommandBuffers;
    struct wine_cmd_pool *pool;
    VkResult res = VK_SUCCESS;
    unsigned int i;

    TRACE("%p, %p, %p\n", device, allocate_info, buffers);

    pool = wine_cmd_pool_from_handle(allocate_info->commandPool);

    memset(buffers, 0, allocate_info->commandBufferCount * sizeof(*buffers));

    for (i = 0; i < allocate_info->commandBufferCount; i++)
    {
        VkCommandBufferAllocateInfo_host allocate_info_host;

        /* TODO: future extensions (none yet) may require pNext conversion. */
        allocate_info_host.pNext = allocate_info->pNext;
        allocate_info_host.sType = allocate_info->sType;
        allocate_info_host.commandPool = pool->command_pool;
        allocate_info_host.level = allocate_info->level;
        allocate_info_host.commandBufferCount = 1;

        TRACE("Allocating command buffer %u from pool 0x%s.\n",
                i, wine_dbgstr_longlong(allocate_info_host.commandPool));

        if (!(buffers[i] = calloc(1, sizeof(**buffers))))
        {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }

        buffers[i]->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
        buffers[i]->device = device;
        list_add_tail(&pool->command_buffers, &buffers[i]->pool_link);
        res = device->funcs.p_vkAllocateCommandBuffers(device->device,
                &allocate_info_host, &buffers[i]->command_buffer);
        WINE_VK_ADD_DISPATCHABLE_MAPPING(device->phys_dev->instance, buffers[i], buffers[i]->command_buffer);
        if (res != VK_SUCCESS)
        {
            ERR("Failed to allocate command buffer, res=%d.\n", res);
            buffers[i]->command_buffer = VK_NULL_HANDLE;
            break;
        }
    }

    if (res != VK_SUCCESS)
    {
        wine_vk_free_command_buffers(device, pool, i + 1, buffers);
        memset(buffers, 0, allocate_info->commandBufferCount * sizeof(*buffers));
    }

    return res;
}

NTSTATUS wine_vkCreateDevice(void *args)
{
    struct vkCreateDevice_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkDeviceCreateInfo *create_info = params->pCreateInfo;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    VkDevice *device = params->pDevice;
    VkPhysicalDeviceFeatures features = {0};
    VkPhysicalDeviceFeatures2 *features2;
    VkDeviceCreateInfo create_info_host;
    struct VkQueue_T *next_queue;
    struct VkDevice_T *object;
    unsigned int i;
    VkResult res;

    TRACE("%p, %p, %p, %p\n", phys_dev, create_info, allocator, device);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (TRACE_ON(vulkan))
    {
        VkPhysicalDeviceProperties_host properties;

        phys_dev->instance->funcs.p_vkGetPhysicalDeviceProperties(phys_dev->phys_dev, &properties);

        TRACE("Device name: %s.\n", debugstr_a(properties.deviceName));
        TRACE("Vendor ID: %#x, Device ID: %#x.\n", properties.vendorID, properties.deviceID);
        TRACE("Driver version: %#x.\n", properties.driverVersion);
    }

    if (!(object = calloc(1, sizeof(*object))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    object->base.base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
    object->phys_dev = phys_dev;

    res = wine_vk_device_convert_create_info(create_info, &create_info_host);
    if (res != VK_SUCCESS)
        goto fail;

    /* Enable shaderStorageImageWriteWithoutFormat for fshack
     * XXX check if available
     */
    if (create_info_host.pEnabledFeatures)
    {
        features = *create_info_host.pEnabledFeatures;
        features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        create_info_host.pEnabledFeatures = &features;
    }
    if ((features2 = wine_vk_find_struct(&create_info_host, PHYSICAL_DEVICE_FEATURES_2)))
    {
        features2->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    }
    else if (!create_info_host.pEnabledFeatures)
    {
        features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        create_info_host.pEnabledFeatures = &features;
    }

    res = phys_dev->instance->funcs.p_vkCreateDevice(phys_dev->phys_dev,
            &create_info_host, NULL /* allocator */, &object->device);
    wine_vk_device_free_create_info(&create_info_host);
    WINE_VK_ADD_DISPATCHABLE_MAPPING(phys_dev->instance, object, object->device);
    if (res != VK_SUCCESS)
    {
        WARN("Failed to create device, res=%d.\n", res);
        goto fail;
    }

    /* Just load all function pointers we are aware off. The loader takes care of filtering.
     * We use vkGetDeviceProcAddr as opposed to vkGetInstanceProcAddr for efficiency reasons
     * as functions pass through fewer dispatch tables within the loader.
     */
#define USE_VK_FUNC(name) \
    object->funcs.p_##name = (void *)vk_funcs->p_vkGetDeviceProcAddr(object->device, #name); \
    if (object->funcs.p_##name == NULL) \
        TRACE("Not found '%s'.\n", #name);
    ALL_VK_DEVICE_FUNCS()
#undef USE_VK_FUNC

    /* We need to cache all queues within the device as each requires wrapping since queues are
     * dispatchable objects.
     */
    for (i = 0; i < create_info_host.queueCreateInfoCount; i++)
    {
        object->queue_count += create_info_host.pQueueCreateInfos[i].queueCount;
    }

    if (!(object->queues = calloc(object->queue_count, sizeof(*object->queues))))
    {
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }

    next_queue = object->queues;
    for (i = 0; i < create_info_host.queueCreateInfoCount; i++)
    {
        uint32_t flags = create_info_host.pQueueCreateInfos[i].flags;
        uint32_t family_index = create_info_host.pQueueCreateInfos[i].queueFamilyIndex;
        uint32_t queue_count = create_info_host.pQueueCreateInfos[i].queueCount;

        TRACE("Queue family index %u, queue count %u.\n", family_index, queue_count);

        wine_vk_device_get_queues(object, family_index, queue_count, flags, next_queue);
        next_queue += queue_count;
    }

    object->base.quirks = phys_dev->instance->quirks;

    *device = object;
    TRACE("Created device %p (native device %p).\n", object, object->device);
    return VK_SUCCESS;

fail:
    wine_vk_device_free(object);
    return res;
}

NTSTATUS wine_vkCreateInstance(void *args)
{
    struct vkCreateInstance_params *params = args;
    const VkInstanceCreateInfo *create_info = params->pCreateInfo;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    VkInstance *instance = params->pInstance;
    VkInstanceCreateInfo create_info_host;
    const VkApplicationInfo *app_info;
    uint32_t new_mxcsr, old_mxcsr;
    struct VkInstance_T *object;
    VkResult res;

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (!(object = calloc(1, sizeof(*object))))
    {
        ERR("Failed to allocate memory for instance\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    object->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
    list_init(&object->wrappers);
    pthread_rwlock_init(&object->wrapper_lock, NULL);

    res = wine_vk_instance_convert_create_info(create_info, &create_info_host, object);
    if (res != VK_SUCCESS)
    {
        wine_vk_instance_free(object);
        return res;
    }

    res = vk_funcs->p_vkCreateInstance(&create_info_host, NULL /* allocator */, &object->instance);
    free_VkInstanceCreateInfo_struct_chain(&create_info_host);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create instance, res=%d\n", res);
        wine_vk_instance_free(object);
        return res;
    }

    WINE_VK_ADD_DISPATCHABLE_MAPPING(object, object, object->instance);

    /* Load all instance functions we are aware of. Note the loader takes care
     * of any filtering for extensions which were not requested, but which the
     * ICD may support.
     */
#define USE_VK_FUNC(name) \
    object->funcs.p_##name = (void *)vk_funcs->p_vkGetInstanceProcAddr(object->instance, #name);
    ALL_VK_INSTANCE_FUNCS()
#undef USE_VK_FUNC

    /* Cache physical devices for vkEnumeratePhysicalDevices within the instance as
     * each vkPhysicalDevice is a dispatchable object, which means we need to wrap
     * the native physical devices and present those to the application.
     * Cleanup happens as part of wine_vkDestroyInstance.
     */
    __asm__ volatile("stmxcsr %0" : "=m"(old_mxcsr));
    new_mxcsr = 0x1f80;
    __asm__ volatile("ldmxcsr %0" : : "m"(new_mxcsr));
    res = wine_vk_instance_load_physical_devices(object);
    __asm__ volatile("ldmxcsr %0" : : "m"(old_mxcsr));
    TRACE("old_mxcsr %#x.\n", old_mxcsr);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to load physical devices, res=%d\n", res);
        wine_vk_instance_free(object);
        return res;
    }

    if ((app_info = create_info->pApplicationInfo))
    {
        TRACE("Application name %s, application version %#x.\n",
                debugstr_a(app_info->pApplicationName), app_info->applicationVersion);
        TRACE("Engine name %s, engine version %#x.\n", debugstr_a(app_info->pEngineName),
                app_info->engineVersion);
        TRACE("API version %#x.\n", app_info->apiVersion);

        if (app_info->pEngineName && !strcmp(app_info->pEngineName, "idTech"))
            object->quirks |= WINEVULKAN_QUIRK_GET_DEVICE_PROC_ADDR;
    }

    object->quirks |= WINEVULKAN_QUIRK_ADJUST_MAX_IMAGE_COUNT;

    *instance = object;
    TRACE("Created instance %p (native instance %p).\n", object, object->instance);
    return VK_SUCCESS;
}

NTSTATUS wine_vkDestroyDevice(void *args)
{
    struct vkDestroyDevice_params *params = args;
    VkDevice device = params->device;
    const VkAllocationCallbacks *allocator = params->pAllocator;

    TRACE("%p %p\n", device, allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    wine_vk_device_free(device);
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkDestroyInstance(void *args)
{
    struct vkDestroyInstance_params *params = args;
    VkInstance instance = params->instance;
    const VkAllocationCallbacks *allocator = params->pAllocator;

    TRACE("%p, %p\n", instance, allocator);

    if (allocator)
        FIXME("Support allocation allocators\n");

    wine_vk_instance_free(instance);
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkEnumerateDeviceExtensionProperties(void *args)
{
    struct vkEnumerateDeviceExtensionProperties_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const char *layer_name = params->pLayerName;
    uint32_t *count = params->pPropertyCount;
    VkExtensionProperties *properties = params->pProperties;

    TRACE("%p, %p, %p, %p\n", phys_dev, layer_name, count, properties);

    /* This shouldn't get called with layer_name set, the ICD loader prevents it. */
    if (layer_name)
    {
        ERR("Layer enumeration not supported from ICD.\n");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    if (!properties)
    {
        *count = phys_dev->extension_count;
        return VK_SUCCESS;
    }

    *count = min(*count, phys_dev->extension_count);
    memcpy(properties, phys_dev->extensions, *count * sizeof(*properties));

    TRACE("Returning %u extensions.\n", *count);
    return *count < phys_dev->extension_count ? VK_INCOMPLETE : VK_SUCCESS;
}

NTSTATUS wine_vkEnumerateInstanceExtensionProperties(void *args)
{
    struct vkEnumerateInstanceExtensionProperties_params *params = args;
    uint32_t *count = params->pPropertyCount;
    VkExtensionProperties *properties = params->pProperties;
    uint32_t num_properties = 0, num_host_properties;
    VkExtensionProperties *host_properties;
    unsigned int i, j;
    VkResult res;

    res = vk_funcs->p_vkEnumerateInstanceExtensionProperties(NULL, &num_host_properties, NULL);
    if (res != VK_SUCCESS)
        return res;

    if (!(host_properties = calloc(num_host_properties, sizeof(*host_properties))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = vk_funcs->p_vkEnumerateInstanceExtensionProperties(NULL, &num_host_properties, host_properties);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to retrieve host properties, res=%d.\n", res);
        free(host_properties);
        return res;
    }

    /* The Wine graphics driver provides us with all extensions supported by the host side
     * including extension fixup (e.g. VK_KHR_xlib_surface -> VK_KHR_win32_surface). It is
     * up to us here to filter the list down to extensions for which we have thunks.
     */
    for (i = 0; i < num_host_properties; i++)
    {
        if (wine_vk_instance_extension_supported(host_properties[i].extensionName))
            num_properties++;
        else
            TRACE("Instance extension '%s' is not supported.\n", host_properties[i].extensionName);
    }

    if (!properties)
    {
        TRACE("Returning %u extensions.\n", num_properties);
        *count = num_properties;
        free(host_properties);
        return VK_SUCCESS;
    }

    for (i = 0, j = 0; i < num_host_properties && j < *count; i++)
    {
        if (wine_vk_instance_extension_supported(host_properties[i].extensionName))
        {
            TRACE("Enabling extension '%s'.\n", host_properties[i].extensionName);
            properties[j++] = host_properties[i];
        }
    }
    *count = min(*count, num_properties);

    free(host_properties);
    return *count < num_properties ? VK_INCOMPLETE : VK_SUCCESS;
}

NTSTATUS wine_vkEnumerateDeviceLayerProperties(void *args)
{
    struct vkEnumerateDeviceLayerProperties_params *params = args;
    uint32_t *count = params->pPropertyCount;

    TRACE("%p, %p, %p\n", params->physicalDevice, count, params->pProperties);

    *count = 0;
    return VK_SUCCESS;
}

NTSTATUS wine_vkEnumerateInstanceVersion(void *args)
{
    struct vkEnumerateInstanceVersion_params *params = args;
    uint32_t *version = params->pApiVersion;
    VkResult res;

    static VkResult (*p_vkEnumerateInstanceVersion)(uint32_t *version);
    if (!p_vkEnumerateInstanceVersion)
        p_vkEnumerateInstanceVersion = vk_funcs->p_vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");

    if (p_vkEnumerateInstanceVersion)
    {
        res = p_vkEnumerateInstanceVersion(version);
    }
    else
    {
        *version = VK_API_VERSION_1_0;
        res = VK_SUCCESS;
    }

    TRACE("API version %u.%u.%u.\n",
            VK_VERSION_MAJOR(*version), VK_VERSION_MINOR(*version), VK_VERSION_PATCH(*version));
    *version = min(WINE_VK_VERSION, *version);
    return res;
}

NTSTATUS wine_vkEnumeratePhysicalDevices(void *args)
{
    struct vkEnumeratePhysicalDevices_params *params = args;
    VkInstance instance = params->instance;
    uint32_t *count = params->pPhysicalDeviceCount;
    VkPhysicalDevice *devices = params->pPhysicalDevices;
    unsigned int i;

    TRACE("%p %p %p\n", instance, count, devices);

    if (!devices)
    {
        *count = instance->phys_dev_count;
        return VK_SUCCESS;
    }

    *count = min(*count, instance->phys_dev_count);
    for (i = 0; i < *count; i++)
    {
        devices[i] = instance->phys_devs[i];
    }

    TRACE("Returning %u devices.\n", *count);
    return *count < instance->phys_dev_count ? VK_INCOMPLETE : VK_SUCCESS;
}

NTSTATUS wine_vkFreeCommandBuffers(void *args)
{
    struct vkFreeCommandBuffers_params *params = args;
    VkDevice device = params->device;
    struct wine_cmd_pool *pool = wine_cmd_pool_from_handle(params->commandPool);
    uint32_t count = params->commandBufferCount;
    const VkCommandBuffer *buffers = params->pCommandBuffers;

    TRACE("%p, 0x%s, %u, %p\n", device, wine_dbgstr_longlong(params->commandPool), count, buffers);

    wine_vk_free_command_buffers(device, pool, count, buffers);
    return STATUS_SUCCESS;
}

static VkQueue wine_vk_device_find_queue(VkDevice device, const VkDeviceQueueInfo2 *info)
{
    struct VkQueue_T* queue;
    uint32_t i;

    for (i = 0; i < device->queue_count; i++)
    {
        queue = &device->queues[i];
        if (queue->family_index == info->queueFamilyIndex
                && queue->queue_index == info->queueIndex
                && queue->flags == info->flags)
        {
            return queue;
        }
    }

    return VK_NULL_HANDLE;
}

NTSTATUS wine_vkGetDeviceQueue(void *args)
{
    struct vkGetDeviceQueue_params *params = args;
    VkDevice device = params->device;
    uint32_t family_index = params->queueFamilyIndex;
    uint32_t queue_index = params->queueIndex;
    VkQueue *queue = params->pQueue;
    VkDeviceQueueInfo2 queue_info;

    TRACE("%p, %u, %u, %p\n", device, family_index, queue_index, queue);

    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
    queue_info.pNext = NULL;
    queue_info.flags = 0;
    queue_info.queueFamilyIndex = family_index;
    queue_info.queueIndex = queue_index;

    *queue = wine_vk_device_find_queue(device, &queue_info);
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkGetDeviceQueue2(void *args)
{
    struct vkGetDeviceQueue2_params *params = args;
    VkDevice device = params->device;
    const VkDeviceQueueInfo2 *info = params->pQueueInfo;
    VkQueue *queue = params->pQueue;
    const VkBaseInStructure *chain;

    TRACE("%p, %p, %p\n", device, info, queue);

    if ((chain = info->pNext))
        FIXME("Ignoring a linked structure of type %u.\n", chain->sType);

    *queue = wine_vk_device_find_queue(device, info);
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkCreateCommandPool(void *args)
{
    struct vkCreateCommandPool_params *params = args;
    VkDevice device = params->device;
    const VkCommandPoolCreateInfo *info = params->pCreateInfo;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    VkCommandPool *command_pool = params->pCommandPool;
    struct wine_cmd_pool *object;
    VkResult res;

    TRACE("%p, %p, %p, %p\n", device, info, allocator, command_pool);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (!(object = calloc(1, sizeof(*object))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    list_init(&object->command_buffers);

    res = device->funcs.p_vkCreateCommandPool(device->device, info, NULL, &object->command_pool);

    if (res == VK_SUCCESS)
    {
        WINE_VK_ADD_NON_DISPATCHABLE_MAPPING(device->phys_dev->instance, object, object->command_pool);
        *command_pool = wine_cmd_pool_to_handle(object);
    }
    else
    {
        free(object);
    }

    return res;
}

NTSTATUS wine_vkDestroyCommandPool(void *args)
{
    struct vkDestroyCommandPool_params *params = args;
    VkDevice device = params->device;
    VkCommandPool handle = params->commandPool;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    struct wine_cmd_pool *pool = wine_cmd_pool_from_handle(handle);
    struct VkCommandBuffer_T *buffer, *cursor;

    TRACE("%p, 0x%s, %p\n", device, wine_dbgstr_longlong(handle), allocator);

    if (!handle)
        return STATUS_SUCCESS;

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* The Vulkan spec says:
     *
     * "When a pool is destroyed, all command buffers allocated from the pool are freed."
     */
    LIST_FOR_EACH_ENTRY_SAFE(buffer, cursor, &pool->command_buffers, struct VkCommandBuffer_T, pool_link)
    {
        WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, buffer);
        free(buffer);
    }

    WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, pool);

    device->funcs.p_vkDestroyCommandPool(device->device, pool->command_pool, NULL);
    free(pool);
    return STATUS_SUCCESS;
}

static VkResult wine_vk_enumerate_physical_device_groups(struct VkInstance_T *instance,
        VkResult (*p_vkEnumeratePhysicalDeviceGroups)(VkInstance, uint32_t *, VkPhysicalDeviceGroupProperties *),
        uint32_t *count, VkPhysicalDeviceGroupProperties *properties)
{
    unsigned int i, j;
    VkResult res;

    res = p_vkEnumeratePhysicalDeviceGroups(instance->instance, count, properties);
    if (res < 0 || !properties)
        return res;

    for (i = 0; i < *count; ++i)
    {
        VkPhysicalDeviceGroupProperties *current = &properties[i];
        for (j = 0; j < current->physicalDeviceCount; ++j)
        {
            VkPhysicalDevice dev = current->physicalDevices[j];
            if (!(current->physicalDevices[j] = wine_vk_instance_wrap_physical_device(instance, dev)))
                return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    return res;
}

NTSTATUS wine_vkEnumeratePhysicalDeviceGroups(void *args)
{
    struct vkEnumeratePhysicalDeviceGroups_params *params = args;
    VkInstance instance = params->instance;
    uint32_t *count = params->pPhysicalDeviceGroupCount;
    VkPhysicalDeviceGroupProperties *properties = params->pPhysicalDeviceGroupProperties;

    TRACE("%p, %p, %p\n", instance, count, properties);
    return wine_vk_enumerate_physical_device_groups(instance,
            instance->funcs.p_vkEnumeratePhysicalDeviceGroups, count, properties);
}

NTSTATUS wine_vkEnumeratePhysicalDeviceGroupsKHR(void *args)
{
    struct vkEnumeratePhysicalDeviceGroupsKHR_params *params = args;
    VkInstance instance = params->instance;
    uint32_t *count = params->pPhysicalDeviceGroupCount;
    VkPhysicalDeviceGroupProperties *properties = params->pPhysicalDeviceGroupProperties;

    TRACE("%p, %p, %p\n", instance, count, properties);
    return wine_vk_enumerate_physical_device_groups(instance,
            instance->funcs.p_vkEnumeratePhysicalDeviceGroupsKHR, count, properties);
}

NTSTATUS wine_vkGetPhysicalDeviceExternalFenceProperties(void *args)
{
    struct vkGetPhysicalDeviceExternalFenceProperties_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceExternalFenceInfo *fence_info = params->pExternalFenceInfo;
    VkExternalFenceProperties *properties = params->pExternalFenceProperties;

    TRACE("%p, %p, %p\n", phys_dev, fence_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalFenceFeatures = 0;
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkGetPhysicalDeviceExternalFencePropertiesKHR(void *args)
{
    struct vkGetPhysicalDeviceExternalFencePropertiesKHR_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceExternalFenceInfo *fence_info = params->pExternalFenceInfo;
    VkExternalFenceProperties *properties = params->pExternalFenceProperties;

    TRACE("%p, %p, %p\n", phys_dev, fence_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalFenceFeatures = 0;
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkGetPhysicalDeviceExternalBufferProperties(void *args)
{
    struct vkGetPhysicalDeviceExternalBufferProperties_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceExternalBufferInfo *buffer_info = params->pExternalBufferInfo;
    VkExternalBufferProperties *properties = params->pExternalBufferProperties;

    TRACE("%p, %p, %p\n", phys_dev, buffer_info, properties);
    memset(&properties->externalMemoryProperties, 0, sizeof(properties->externalMemoryProperties));
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkGetPhysicalDeviceExternalBufferPropertiesKHR(void *args)
{
    struct vkGetPhysicalDeviceExternalBufferPropertiesKHR_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceExternalBufferInfo *buffer_info = params->pExternalBufferInfo;
    VkExternalBufferProperties *properties = params->pExternalBufferProperties;

    TRACE("%p, %p, %p\n", phys_dev, buffer_info, properties);
    memset(&properties->externalMemoryProperties, 0, sizeof(properties->externalMemoryProperties));
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkGetPhysicalDeviceImageFormatProperties2(void *args)
{
    struct vkGetPhysicalDeviceImageFormatProperties2_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceImageFormatInfo2 *format_info = params->pImageFormatInfo;
    VkImageFormatProperties2 *properties = params->pImageFormatProperties;
    VkExternalImageFormatProperties *external_image_properties;
    VkResult res;

    TRACE("%p, %p, %p\n", phys_dev, format_info, properties);

    res = thunk_vkGetPhysicalDeviceImageFormatProperties2(phys_dev, format_info, properties);

    if ((external_image_properties = wine_vk_find_struct(properties, EXTERNAL_IMAGE_FORMAT_PROPERTIES)))
    {
        VkExternalMemoryProperties *p = &external_image_properties->externalMemoryProperties;
        p->externalMemoryFeatures = 0;
        p->exportFromImportedHandleTypes = 0;
        p->compatibleHandleTypes = 0;
    }

    return res;
}

NTSTATUS wine_vkGetPhysicalDeviceImageFormatProperties2KHR(void *args)
{
    struct vkGetPhysicalDeviceImageFormatProperties2KHR_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceImageFormatInfo2 *format_info = params->pImageFormatInfo;
    VkImageFormatProperties2 *properties = params->pImageFormatProperties;
    VkExternalImageFormatProperties *external_image_properties;
    VkResult res;

    TRACE("%p, %p, %p\n", phys_dev, format_info, properties);

    res = thunk_vkGetPhysicalDeviceImageFormatProperties2KHR(phys_dev, format_info, properties);

    if ((external_image_properties = wine_vk_find_struct(properties, EXTERNAL_IMAGE_FORMAT_PROPERTIES)))
    {
        VkExternalMemoryProperties *p = &external_image_properties->externalMemoryProperties;
        p->externalMemoryFeatures = 0;
        p->exportFromImportedHandleTypes = 0;
        p->compatibleHandleTypes = 0;
    }

    return res;
}

/* From ntdll/unix/sync.c */
#define NANOSECONDS_IN_A_SECOND 1000000000
#define TICKSPERSEC             10000000

static inline VkTimeDomainEXT get_performance_counter_time_domain(void)
{
#if !defined(__APPLE__) && defined(HAVE_CLOCK_GETTIME)
# ifdef CLOCK_MONOTONIC_RAW
    return VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
# else
    return VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
# endif
#else
    FIXME("No mapping for VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT on this platform.\n");
    return VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT;
#endif
}

static VkTimeDomainEXT map_to_host_time_domain(VkTimeDomainEXT domain)
{
    /* Matches ntdll/unix/sync.c's performance counter implementation. */
    if (domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT)
        return get_performance_counter_time_domain();

    return domain;
}

static inline uint64_t convert_monotonic_timestamp(uint64_t value)
{
    return value / (NANOSECONDS_IN_A_SECOND / TICKSPERSEC);
}

static inline uint64_t convert_timestamp(VkTimeDomainEXT host_domain, VkTimeDomainEXT target_domain, uint64_t value)
{
    if (host_domain == target_domain)
        return value;

    /* Convert between MONOTONIC time in ns -> QueryPerformanceCounter */
    if ((host_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT || host_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT)
            && target_domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT)
        return convert_monotonic_timestamp(value);

    FIXME("Couldn't translate between host domain %d and target domain %d\n", host_domain, target_domain);
    return value;
}

NTSTATUS wine_vkGetCalibratedTimestampsEXT(void *args)
{
    struct vkGetCalibratedTimestampsEXT_params *params = args;
    VkDevice device = params->device;
    uint32_t timestamp_count = params->timestampCount;
    const VkCalibratedTimestampInfoEXT *timestamp_infos = params->pTimestampInfos;
    uint64_t *timestamps = params->pTimestamps;
    uint64_t *max_deviation = params->pMaxDeviation;
    VkCalibratedTimestampInfoEXT* host_timestamp_infos;
    unsigned int i;
    VkResult res;
    TRACE("%p, %u, %p, %p, %p\n", device, timestamp_count, timestamp_infos, timestamps, max_deviation);

    if (!(host_timestamp_infos = malloc(sizeof(VkCalibratedTimestampInfoEXT) * timestamp_count)))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (i = 0; i < timestamp_count; i++)
    {
        host_timestamp_infos[i].sType = timestamp_infos[i].sType;
        host_timestamp_infos[i].pNext = timestamp_infos[i].pNext;
        host_timestamp_infos[i].timeDomain = map_to_host_time_domain(timestamp_infos[i].timeDomain);
    }

    res = device->funcs.p_vkGetCalibratedTimestampsEXT(device->device, timestamp_count, host_timestamp_infos, timestamps, max_deviation);
    if (res != VK_SUCCESS)
        return res;

    for (i = 0; i < timestamp_count; i++)
        timestamps[i] = convert_timestamp(host_timestamp_infos[i].timeDomain, timestamp_infos[i].timeDomain, timestamps[i]);

    free(host_timestamp_infos);

    return res;
}

NTSTATUS wine_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(void *args)
{
    struct vkGetPhysicalDeviceCalibrateableTimeDomainsEXT_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    uint32_t *time_domain_count = params->pTimeDomainCount;
    VkTimeDomainEXT *time_domains = params->pTimeDomains;
    BOOL supports_device = FALSE, supports_monotonic = FALSE, supports_monotonic_raw = FALSE;
    const VkTimeDomainEXT performance_counter_domain = get_performance_counter_time_domain();
    VkTimeDomainEXT *host_time_domains;
    uint32_t host_time_domain_count;
    VkTimeDomainEXT out_time_domains[2];
    uint32_t out_time_domain_count;
    unsigned int i;
    VkResult res;

    TRACE("%p, %p, %p\n", phys_dev, time_domain_count, time_domains);

    /* Find out the time domains supported on the host */
    res = phys_dev->instance->funcs.p_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(phys_dev->phys_dev, &host_time_domain_count, NULL);
    if (res != VK_SUCCESS)
        return res;

    if (!(host_time_domains = malloc(sizeof(VkTimeDomainEXT) * host_time_domain_count)))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = phys_dev->instance->funcs.p_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(phys_dev->phys_dev, &host_time_domain_count, host_time_domains);
    if (res != VK_SUCCESS)
    {
        free(host_time_domains);
        return res;
    }

    for (i = 0; i < host_time_domain_count; i++)
    {
        if (host_time_domains[i] == VK_TIME_DOMAIN_DEVICE_EXT)
            supports_device = TRUE;
        else if (host_time_domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT)
            supports_monotonic = TRUE;
        else if (host_time_domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT)
            supports_monotonic_raw = TRUE;
        else
            FIXME("Unknown time domain %d\n", host_time_domains[i]);
    }

    free(host_time_domains);

    out_time_domain_count = 0;

    /* Map our monotonic times -> QPC */
    if (supports_monotonic_raw && performance_counter_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT)
        out_time_domains[out_time_domain_count++] = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT;
    else if (supports_monotonic && performance_counter_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT)
        out_time_domains[out_time_domain_count++] = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT;
    else
        FIXME("VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT not supported on this platform.\n");

    /* Forward the device domain time */
    if (supports_device)
        out_time_domains[out_time_domain_count++] = VK_TIME_DOMAIN_DEVICE_EXT;

    /* Send the count/domains back to the app */
    if (!time_domains)
    {
        *time_domain_count = out_time_domain_count;
        return VK_SUCCESS;
    }

    for (i = 0; i < min(*time_domain_count, out_time_domain_count); i++)
        time_domains[i] = out_time_domains[i];

    res = *time_domain_count < out_time_domain_count ? VK_INCOMPLETE : VK_SUCCESS;
    *time_domain_count = out_time_domain_count;
    return res;
}

NTSTATUS wine_vkGetPhysicalDeviceExternalSemaphoreProperties(void *args)
{
    struct vkGetPhysicalDeviceExternalSemaphoreProperties_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info = params->pExternalSemaphoreInfo;
    VkExternalSemaphoreProperties *properties = params->pExternalSemaphoreProperties;

    TRACE("%p, %p, %p\n", phys_dev, semaphore_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalSemaphoreFeatures = 0;
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(void *args)
{
    struct vkGetPhysicalDeviceExternalSemaphorePropertiesKHR_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info = params->pExternalSemaphoreInfo;
    VkExternalSemaphoreProperties *properties = params->pExternalSemaphoreProperties;

    TRACE("%p, %p, %p\n", phys_dev, semaphore_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalSemaphoreFeatures = 0;
    return STATUS_SUCCESS;
}

VkResult WINAPI wine_vkSetPrivateDataEXT(VkDevice device, VkObjectType object_type, uint64_t object_handle,
        VkPrivateDataSlotEXT private_data_slot, uint64_t data)
{
    TRACE("%p, %#x, 0x%s, 0x%s, 0x%s\n", device, object_type, wine_dbgstr_longlong(object_handle),
            wine_dbgstr_longlong(private_data_slot), wine_dbgstr_longlong(data));

    object_handle = wine_vk_unwrap_handle(object_type, object_handle);
    return device->funcs.p_vkSetPrivateDataEXT(device->device, object_type, object_handle, private_data_slot, data);
}

void WINAPI wine_vkGetPrivateDataEXT(VkDevice device, VkObjectType object_type, uint64_t object_handle,
        VkPrivateDataSlotEXT private_data_slot, uint64_t *data)
{
    TRACE("%p, %#x, 0x%s, 0x%s, %p\n", device, object_type, wine_dbgstr_longlong(object_handle),
            wine_dbgstr_longlong(private_data_slot), data);

    object_handle = wine_vk_unwrap_handle(object_type, object_handle);
    device->funcs.p_vkGetPrivateDataEXT(device->device, object_type, object_handle, private_data_slot, data);
}

/*
#version 450

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1) uniform writeonly image2D outImage;
layout(push_constant) uniform pushConstants {
    //both in real image coords
    vec2 offset;
    vec2 extents;
} constants;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    vec2 texcoord = (vec2(gl_GlobalInvocationID.xy) - constants.offset) / constants.extents;
    vec4 c = texture(texSampler, texcoord);
    imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), c);
}
*/
const uint32_t blit_comp_spv[] = {
    0x07230203,0x00010000,0x0008000a,0x00000036,0x00000000,0x00020011,0x00000001,0x00020011,
    0x00000038,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,
    0x00000000,0x00000001,0x0006000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000d,
    0x00060010,0x00000004,0x00000011,0x00000008,0x00000008,0x00000001,0x00030003,0x00000002,
    0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00050005,0x00000009,0x63786574,
    0x64726f6f,0x00000000,0x00080005,0x0000000d,0x475f6c67,0x61626f6c,0x766e496c,0x7461636f,
    0x496e6f69,0x00000044,0x00060005,0x00000012,0x68737570,0x736e6f43,0x746e6174,0x00000073,
    0x00050006,0x00000012,0x00000000,0x7366666f,0x00007465,0x00050006,0x00000012,0x00000001,
    0x65747865,0x0073746e,0x00050005,0x00000014,0x736e6f63,0x746e6174,0x00000073,0x00030005,
    0x00000021,0x00000063,0x00050005,0x00000025,0x53786574,0x6c706d61,0x00007265,0x00050005,
    0x0000002c,0x4974756f,0x6567616d,0x00000000,0x00040047,0x0000000d,0x0000000b,0x0000001c,
    0x00050048,0x00000012,0x00000000,0x00000023,0x00000000,0x00050048,0x00000012,0x00000001,
    0x00000023,0x00000008,0x00030047,0x00000012,0x00000002,0x00040047,0x00000025,0x00000022,
    0x00000000,0x00040047,0x00000025,0x00000021,0x00000000,0x00040047,0x0000002c,0x00000022,
    0x00000000,0x00040047,0x0000002c,0x00000021,0x00000001,0x00030047,0x0000002c,0x00000019,
    0x00040047,0x00000035,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000002,
    0x00040020,0x00000008,0x00000007,0x00000007,0x00040015,0x0000000a,0x00000020,0x00000000,
    0x00040017,0x0000000b,0x0000000a,0x00000003,0x00040020,0x0000000c,0x00000001,0x0000000b,
    0x0004003b,0x0000000c,0x0000000d,0x00000001,0x00040017,0x0000000e,0x0000000a,0x00000002,
    0x0004001e,0x00000012,0x00000007,0x00000007,0x00040020,0x00000013,0x00000009,0x00000012,
    0x0004003b,0x00000013,0x00000014,0x00000009,0x00040015,0x00000015,0x00000020,0x00000001,
    0x0004002b,0x00000015,0x00000016,0x00000000,0x00040020,0x00000017,0x00000009,0x00000007,
    0x0004002b,0x00000015,0x0000001b,0x00000001,0x00040017,0x0000001f,0x00000006,0x00000004,
    0x00040020,0x00000020,0x00000007,0x0000001f,0x00090019,0x00000022,0x00000006,0x00000001,
    0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,0x00000023,0x00000022,
    0x00040020,0x00000024,0x00000000,0x00000023,0x0004003b,0x00000024,0x00000025,0x00000000,
    0x0004002b,0x00000006,0x00000028,0x00000000,0x00090019,0x0000002a,0x00000006,0x00000001,
    0x00000000,0x00000000,0x00000000,0x00000002,0x00000000,0x00040020,0x0000002b,0x00000000,
    0x0000002a,0x0004003b,0x0000002b,0x0000002c,0x00000000,0x00040017,0x00000030,0x00000015,
    0x00000002,0x0004002b,0x0000000a,0x00000033,0x00000008,0x0004002b,0x0000000a,0x00000034,
    0x00000001,0x0006002c,0x0000000b,0x00000035,0x00000033,0x00000033,0x00000034,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,
    0x00000009,0x00000007,0x0004003b,0x00000020,0x00000021,0x00000007,0x0004003d,0x0000000b,
    0x0000000f,0x0000000d,0x0007004f,0x0000000e,0x00000010,0x0000000f,0x0000000f,0x00000000,
    0x00000001,0x00040070,0x00000007,0x00000011,0x00000010,0x00050041,0x00000017,0x00000018,
    0x00000014,0x00000016,0x0004003d,0x00000007,0x00000019,0x00000018,0x00050083,0x00000007,
    0x0000001a,0x00000011,0x00000019,0x00050041,0x00000017,0x0000001c,0x00000014,0x0000001b,
    0x0004003d,0x00000007,0x0000001d,0x0000001c,0x00050088,0x00000007,0x0000001e,0x0000001a,
    0x0000001d,0x0003003e,0x00000009,0x0000001e,0x0004003d,0x00000023,0x00000026,0x00000025,
    0x0004003d,0x00000007,0x00000027,0x00000009,0x00070058,0x0000001f,0x00000029,0x00000026,
    0x00000027,0x00000002,0x00000028,0x0003003e,0x00000021,0x00000029,0x0004003d,0x0000002a,
    0x0000002d,0x0000002c,0x0004003d,0x0000000b,0x0000002e,0x0000000d,0x0007004f,0x0000000e,
    0x0000002f,0x0000002e,0x0000002e,0x00000000,0x00000001,0x0004007c,0x00000030,0x00000031,
    0x0000002f,0x0004003d,0x0000001f,0x00000032,0x00000021,0x00040063,0x0000002d,0x00000031,
    0x00000032,0x000100fd,0x00010038
};

/*
#version 460
#extension GL_GOOGLE_include_directive: require

layout(local_size_x=8, local_size_y=8, local_size_z=1) in;

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1) uniform writeonly image2D outImage;

#define A_GPU 1
#define A_GLSL 1
//#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p){return AF4(textureGather(texSampler, p, 0));}
AF4 FsrEasuGF(AF2 p){return AF4(textureGather(texSampler, p, 1));}
AF4 FsrEasuBF(AF2 p){return AF4(textureGather(texSampler, p, 2));}
//#include "ffx_fsr1.h"

layout(push_constant) uniform pushConstants {
    uvec4 c1, c2, c3, c4;
};


void main()
{
    vec3 color;

    if (any(greaterThanEqual(gl_GlobalInvocationID.xy, c4.zw)))
        return;

    FsrEasuF(color, uvec2(gl_GlobalInvocationID.xy), c1, c2, c3, c4);

    imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), vec4(color, 1.0));
}
*/
const uint32_t fsr_easu_comp_spv[] = {
    0x07230203,0x00010000,0x0008000a,0x0000129e,0x00000000,0x00020011,0x00000001,0x00020011,
    0x00000038,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,
    0x00000000,0x00000001,0x0006000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x000004ee,
    0x00060010,0x00000004,0x00000011,0x00000008,0x00000008,0x00000001,0x00030003,0x00000002,
    0x000001cc,0x000a0004,0x475f4c47,0x4c474f4f,0x70635f45,0x74735f70,0x5f656c79,0x656e696c,
    0x7269645f,0x69746365,0x00006576,0x00080004,0x475f4c47,0x4c474f4f,0x6e695f45,0x64756c63,
    0x69645f65,0x74636572,0x00657669,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00050005,
    0x000000bc,0x53786574,0x6c706d61,0x00007265,0x00080005,0x000004ee,0x475f6c67,0x61626f6c,
    0x766e496c,0x7461636f,0x496e6f69,0x00000044,0x00060005,0x000004f1,0x68737570,0x736e6f43,
    0x746e6174,0x00000073,0x00040006,0x000004f1,0x00000000,0x00003163,0x00040006,0x000004f1,
    0x00000001,0x00003263,0x00040006,0x000004f1,0x00000002,0x00003363,0x00040006,0x000004f1,
    0x00000003,0x00003463,0x00030005,0x000004f3,0x00000000,0x00050005,0x00000517,0x4974756f,
    0x6567616d,0x00000000,0x00040047,0x000000bc,0x00000022,0x00000000,0x00040047,0x000000bc,
    0x00000021,0x00000000,0x00040047,0x000004ee,0x0000000b,0x0000001c,0x00050048,0x000004f1,
    0x00000000,0x00000023,0x00000000,0x00050048,0x000004f1,0x00000001,0x00000023,0x00000010,
    0x00050048,0x000004f1,0x00000002,0x00000023,0x00000020,0x00050048,0x000004f1,0x00000003,
    0x00000023,0x00000030,0x00030047,0x000004f1,0x00000002,0x00040047,0x00000517,0x00000022,
    0x00000000,0x00040047,0x00000517,0x00000021,0x00000001,0x00030047,0x00000517,0x00000019,
    0x00040047,0x00000523,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x0000000c,0x00000006,0x00000002,
    0x00040017,0x00000011,0x00000006,0x00000003,0x00040017,0x00000016,0x00000006,0x00000004,
    0x00040015,0x0000001b,0x00000020,0x00000000,0x00020014,0x0000004f,0x00040017,0x00000060,
    0x0000001b,0x00000002,0x00040017,0x00000062,0x0000001b,0x00000004,0x0004002b,0x00000006,
    0x00000093,0x3f800000,0x0004002b,0x00000006,0x0000009b,0x00000000,0x0004002b,0x0000001b,
    0x000000a3,0x7ef07ebb,0x0004002b,0x0000001b,0x000000ac,0x5f347d74,0x0004002b,0x0000001b,
    0x000000b1,0x00000001,0x00090019,0x000000b9,0x00000006,0x00000001,0x00000000,0x00000000,
    0x00000000,0x00000001,0x00000000,0x0003001b,0x000000ba,0x000000b9,0x00040020,0x000000bb,
    0x00000000,0x000000ba,0x0004003b,0x000000bb,0x000000bc,0x00000000,0x00040015,0x000000bf,
    0x00000020,0x00000001,0x0004002b,0x000000bf,0x000000c0,0x00000000,0x0004002b,0x000000bf,
    0x000000cb,0x00000001,0x0004002b,0x000000bf,0x000000d6,0x00000002,0x0004002b,0x0000001b,
    0x000000e0,0x00000000,0x0004002b,0x00000006,0x0000010d,0x3ecccccd,0x0004002b,0x00000006,
    0x00000112,0xbf800000,0x0004002b,0x00000006,0x00000123,0x3fc80000,0x0004002b,0x00000006,
    0x00000128,0xbf100000,0x0004002b,0x00000006,0x00000230,0x3f000000,0x0004002b,0x00000006,
    0x000002f5,0x38000000,0x0004002b,0x00000006,0x0000033e,0xbf000000,0x0004002b,0x00000006,
    0x00000348,0xbe947ae1,0x0005002c,0x0000000c,0x0000039c,0x0000009b,0x00000112,0x0005002c,
    0x0000000c,0x000003b7,0x00000093,0x00000112,0x0005002c,0x0000000c,0x000003d2,0x00000112,
    0x00000093,0x0005002c,0x0000000c,0x000003ed,0x0000009b,0x00000093,0x0005002c,0x0000000c,
    0x00000423,0x00000112,0x0000009b,0x0005002c,0x0000000c,0x0000043e,0x00000093,0x00000093,
    0x0004002b,0x00000006,0x00000459,0x40000000,0x0005002c,0x0000000c,0x0000045a,0x00000459,
    0x00000093,0x0005002c,0x0000000c,0x00000475,0x00000459,0x0000009b,0x0005002c,0x0000000c,
    0x00000490,0x00000093,0x0000009b,0x0005002c,0x0000000c,0x000004ab,0x00000093,0x00000459,
    0x0005002c,0x0000000c,0x000004c6,0x0000009b,0x00000459,0x00040017,0x000004ec,0x0000001b,
    0x00000003,0x00040020,0x000004ed,0x00000001,0x000004ec,0x0004003b,0x000004ed,0x000004ee,
    0x00000001,0x0006001e,0x000004f1,0x00000062,0x00000062,0x00000062,0x00000062,0x00040020,
    0x000004f2,0x00000009,0x000004f1,0x0004003b,0x000004f2,0x000004f3,0x00000009,0x0004002b,
    0x000000bf,0x000004f4,0x00000003,0x00040020,0x000004f5,0x00000009,0x00000062,0x00040017,
    0x000004f9,0x0000004f,0x00000002,0x00090019,0x00000515,0x00000006,0x00000001,0x00000000,
    0x00000000,0x00000000,0x00000002,0x00000000,0x00040020,0x00000516,0x00000000,0x00000515,
    0x0004003b,0x00000516,0x00000517,0x00000000,0x00040017,0x0000051b,0x000000bf,0x00000002,
    0x0004002b,0x0000001b,0x00000522,0x00000008,0x0006002c,0x000004ec,0x00000523,0x00000522,
    0x00000522,0x000000b1,0x0007002c,0x00000016,0x0000127a,0x00000230,0x00000230,0x00000230,
    0x00000230,0x00030001,0x0000000c,0x0000129d,0x00050036,0x00000002,0x00000004,0x00000000,
    0x00000003,0x000200f8,0x00000005,0x000300f7,0x00000524,0x00000000,0x000300fb,0x000000e0,
    0x00000525,0x000200f8,0x00000525,0x0004003d,0x000004ec,0x000004ef,0x000004ee,0x0007004f,
    0x00000060,0x000004f0,0x000004ef,0x000004ef,0x00000000,0x00000001,0x00050041,0x000004f5,
    0x000004f6,0x000004f3,0x000004f4,0x0004003d,0x00000062,0x000004f7,0x000004f6,0x0007004f,
    0x00000060,0x000004f8,0x000004f7,0x000004f7,0x00000002,0x00000003,0x000500ae,0x000004f9,
    0x000004fa,0x000004f0,0x000004f8,0x0004009a,0x0000004f,0x000004fb,0x000004fa,0x000300f7,
    0x000004fd,0x00000000,0x000400fa,0x000004fb,0x000004fc,0x000004fd,0x000200f8,0x000004fc,
    0x000200f9,0x00000524,0x000200f8,0x000004fd,0x00050051,0x0000001b,0x00000502,0x000004ef,
    0x00000000,0x00050051,0x0000001b,0x00000503,0x000004ef,0x00000001,0x00050050,0x00000060,
    0x00000504,0x00000502,0x00000503,0x00050041,0x000004f5,0x00000508,0x000004f3,0x000000c0,
    0x0004003d,0x00000062,0x00000509,0x00000508,0x00050041,0x000004f5,0x0000050b,0x000004f3,
    0x000000cb,0x0004003d,0x00000062,0x0000050c,0x0000050b,0x00050041,0x000004f5,0x0000050e,
    0x000004f3,0x000000d6,0x0004003d,0x00000062,0x0000050f,0x0000050e,0x00040070,0x0000000c,
    0x00000618,0x00000504,0x00050051,0x0000001b,0x0000061b,0x00000509,0x00000000,0x00050051,
    0x0000001b,0x0000061c,0x00000509,0x00000001,0x00050050,0x00000060,0x0000061d,0x0000061b,
    0x0000061c,0x0004007c,0x0000000c,0x0000061e,0x0000061d,0x00050085,0x0000000c,0x0000061f,
    0x00000618,0x0000061e,0x00050051,0x0000001b,0x00000622,0x00000509,0x00000002,0x00050051,
    0x0000001b,0x00000623,0x00000509,0x00000003,0x00050050,0x00000060,0x00000624,0x00000622,
    0x00000623,0x0004007c,0x0000000c,0x00000625,0x00000624,0x00050081,0x0000000c,0x00000626,
    0x0000061f,0x00000625,0x0006000c,0x0000000c,0x00000628,0x00000001,0x00000008,0x00000626,
    0x00050083,0x0000000c,0x0000062b,0x00000626,0x00000628,0x00050051,0x0000001b,0x0000062f,
    0x0000050c,0x00000000,0x00050051,0x0000001b,0x00000630,0x0000050c,0x00000001,0x00050050,
    0x00000060,0x00000631,0x0000062f,0x00000630,0x0004007c,0x0000000c,0x00000632,0x00000631,
    0x00050085,0x0000000c,0x00000633,0x00000628,0x00000632,0x00050051,0x0000001b,0x00000636,
    0x0000050c,0x00000002,0x00050051,0x0000001b,0x00000637,0x0000050c,0x00000003,0x00050050,
    0x00000060,0x00000638,0x00000636,0x00000637,0x0004007c,0x0000000c,0x00000639,0x00000638,
    0x00050081,0x0000000c,0x0000063a,0x00000633,0x00000639,0x00050051,0x0000001b,0x0000063e,
    0x0000050f,0x00000000,0x00050051,0x0000001b,0x0000063f,0x0000050f,0x00000001,0x00050050,
    0x00000060,0x00000640,0x0000063e,0x0000063f,0x0004007c,0x0000000c,0x00000641,0x00000640,
    0x00050081,0x0000000c,0x00000642,0x0000063a,0x00000641,0x00050051,0x0000001b,0x00000646,
    0x0000050f,0x00000002,0x00050051,0x0000001b,0x00000647,0x0000050f,0x00000003,0x00050050,
    0x00000060,0x00000648,0x00000646,0x00000647,0x0004007c,0x0000000c,0x00000649,0x00000648,
    0x00050081,0x0000000c,0x0000064a,0x0000063a,0x00000649,0x00050051,0x0000001b,0x0000064e,
    0x000004f7,0x00000000,0x00050051,0x0000001b,0x0000064f,0x000004f7,0x00000001,0x00050050,
    0x00000060,0x00000650,0x0000064e,0x0000064f,0x0004007c,0x0000000c,0x00000651,0x00000650,
    0x00050081,0x0000000c,0x00000652,0x0000063a,0x00000651,0x0004003d,0x000000ba,0x00000845,
    0x000000bc,0x00060060,0x00000016,0x00000847,0x00000845,0x0000063a,0x000000c0,0x00060060,
    0x00000016,0x00000851,0x00000845,0x0000063a,0x000000cb,0x00060060,0x00000016,0x0000085b,
    0x00000845,0x0000063a,0x000000d6,0x00060060,0x00000016,0x00000865,0x00000845,0x00000642,
    0x000000c0,0x00060060,0x00000016,0x0000086f,0x00000845,0x00000642,0x000000cb,0x00060060,
    0x00000016,0x00000879,0x00000845,0x00000642,0x000000d6,0x00060060,0x00000016,0x00000883,
    0x00000845,0x0000064a,0x000000c0,0x00060060,0x00000016,0x0000088d,0x00000845,0x0000064a,
    0x000000cb,0x00060060,0x00000016,0x00000897,0x00000845,0x0000064a,0x000000d6,0x00060060,
    0x00000016,0x000008a1,0x00000845,0x00000652,0x000000c0,0x00060060,0x00000016,0x000008ab,
    0x00000845,0x00000652,0x000000cb,0x00060060,0x00000016,0x000008b5,0x00000845,0x00000652,
    0x000000d6,0x00050085,0x00000016,0x0000066d,0x0000085b,0x0000127a,0x00050085,0x00000016,
    0x00000670,0x00000847,0x0000127a,0x00050081,0x00000016,0x00000672,0x00000670,0x00000851,
    0x00050081,0x00000016,0x00000673,0x0000066d,0x00000672,0x00050085,0x00000016,0x00000676,
    0x00000879,0x0000127a,0x00050085,0x00000016,0x00000679,0x00000865,0x0000127a,0x00050081,
    0x00000016,0x0000067b,0x00000679,0x0000086f,0x00050081,0x00000016,0x0000067c,0x00000676,
    0x0000067b,0x00050085,0x00000016,0x0000067f,0x00000897,0x0000127a,0x00050085,0x00000016,
    0x00000682,0x00000883,0x0000127a,0x00050081,0x00000016,0x00000684,0x00000682,0x0000088d,
    0x00050081,0x00000016,0x00000685,0x0000067f,0x00000684,0x00050085,0x00000016,0x00000688,
    0x000008b5,0x0000127a,0x00050085,0x00000016,0x0000068b,0x000008a1,0x0000127a,0x00050081,
    0x00000016,0x0000068d,0x0000068b,0x000008ab,0x00050081,0x00000016,0x0000068e,0x00000688,
    0x0000068d,0x00050051,0x00000006,0x00000690,0x00000673,0x00000000,0x00050051,0x00000006,
    0x00000692,0x00000673,0x00000001,0x00050051,0x00000006,0x00000694,0x0000067c,0x00000000,
    0x00050051,0x00000006,0x00000696,0x0000067c,0x00000001,0x00050051,0x00000006,0x00000698,
    0x0000067c,0x00000002,0x00050051,0x00000006,0x0000069a,0x0000067c,0x00000003,0x00050051,
    0x00000006,0x0000069c,0x00000685,0x00000000,0x00050051,0x00000006,0x0000069e,0x00000685,
    0x00000001,0x00050051,0x00000006,0x000006a0,0x00000685,0x00000002,0x00050051,0x00000006,
    0x000006a2,0x00000685,0x00000003,0x00050051,0x00000006,0x000006a4,0x0000068e,0x00000002,
    0x00050051,0x00000006,0x000006a6,0x0000068e,0x00000003,0x00050051,0x00000006,0x00000913,
    0x0000062b,0x00000000,0x00050083,0x00000006,0x00000914,0x00000093,0x00000913,0x00050051,
    0x00000006,0x00000917,0x0000062b,0x00000001,0x00050083,0x00000006,0x00000918,0x00000093,
    0x00000917,0x00050085,0x00000006,0x00000919,0x00000914,0x00000918,0x00050083,0x00000006,
    0x00000939,0x000006a2,0x00000698,0x00050083,0x00000006,0x0000093c,0x00000698,0x0000069a,
    0x0006000c,0x00000006,0x0000093e,0x00000001,0x00000004,0x00000939,0x0006000c,0x00000006,
    0x00000940,0x00000001,0x00000004,0x0000093c,0x0007000c,0x00000006,0x00000941,0x00000001,
    0x00000028,0x0000093e,0x00000940,0x0004007c,0x0000001b,0x00000993,0x00000941,0x00050082,
    0x0000001b,0x00000994,0x000000a3,0x00000993,0x0004007c,0x00000006,0x00000995,0x00000994,
    0x00050083,0x00000006,0x00000946,0x000006a2,0x0000069a,0x00050085,0x00000006,0x00000949,
    0x00000946,0x00000919,0x0006000c,0x00000006,0x0000094f,0x00000001,0x00000004,0x00000946,
    0x00050085,0x00000006,0x00000951,0x0000094f,0x00000995,0x0008000c,0x00000006,0x000009a0,
    0x00000001,0x0000002b,0x00000951,0x0000009b,0x00000093,0x00050085,0x00000006,0x00000955,
    0x000009a0,0x000009a0,0x00050085,0x00000006,0x00000958,0x00000955,0x00000919,0x00050083,
    0x00000006,0x0000095d,0x00000696,0x00000698,0x00050083,0x00000006,0x00000960,0x00000698,
    0x00000690,0x0006000c,0x00000006,0x00000962,0x00000001,0x00000004,0x0000095d,0x0006000c,
    0x00000006,0x00000964,0x00000001,0x00000004,0x00000960,0x0007000c,0x00000006,0x00000965,
    0x00000001,0x00000028,0x00000962,0x00000964,0x0004007c,0x0000001b,0x000009ac,0x00000965,
    0x00050082,0x0000001b,0x000009ad,0x000000a3,0x000009ac,0x0004007c,0x00000006,0x000009ae,
    0x000009ad,0x00050083,0x00000006,0x0000096a,0x00000696,0x00000690,0x00050085,0x00000006,
    0x0000096d,0x0000096a,0x00000919,0x0006000c,0x00000006,0x00000973,0x00000001,0x00000004,
    0x0000096a,0x00050085,0x00000006,0x00000975,0x00000973,0x000009ae,0x0008000c,0x00000006,
    0x000009b9,0x00000001,0x0000002b,0x00000975,0x0000009b,0x00000093,0x00050085,0x00000006,
    0x00000979,0x000009b9,0x000009b9,0x00050085,0x00000006,0x0000097c,0x00000979,0x00000919,
    0x00050081,0x00000006,0x0000097e,0x00000958,0x0000097c,0x00050085,0x00000006,0x000009e8,
    0x00000913,0x00000918,0x00050083,0x00000006,0x000009fe,0x000006a0,0x000006a2,0x0006000c,
    0x00000006,0x00000a03,0x00000001,0x00000004,0x000009fe,0x0007000c,0x00000006,0x00000a06,
    0x00000001,0x00000028,0x00000a03,0x0000093e,0x0004007c,0x0000001b,0x00000a58,0x00000a06,
    0x00050082,0x0000001b,0x00000a59,0x000000a3,0x00000a58,0x0004007c,0x00000006,0x00000a5a,
    0x00000a59,0x00050083,0x00000006,0x00000a0b,0x000006a0,0x00000698,0x00050085,0x00000006,
    0x00000a0e,0x00000a0b,0x000009e8,0x00050081,0x00000006,0x00000a11,0x00000949,0x00000a0e,
    0x0006000c,0x00000006,0x00000a14,0x00000001,0x00000004,0x00000a0b,0x00050085,0x00000006,
    0x00000a16,0x00000a14,0x00000a5a,0x0008000c,0x00000006,0x00000a65,0x00000001,0x0000002b,
    0x00000a16,0x0000009b,0x00000093,0x00050085,0x00000006,0x00000a1a,0x00000a65,0x00000a65,
    0x00050085,0x00000006,0x00000a1d,0x00000a1a,0x000009e8,0x00050081,0x00000006,0x00000a1f,
    0x0000097e,0x00000a1d,0x00050083,0x00000006,0x00000a22,0x0000069c,0x000006a2,0x00050083,
    0x00000006,0x00000a25,0x000006a2,0x00000692,0x0006000c,0x00000006,0x00000a27,0x00000001,
    0x00000004,0x00000a22,0x0006000c,0x00000006,0x00000a29,0x00000001,0x00000004,0x00000a25,
    0x0007000c,0x00000006,0x00000a2a,0x00000001,0x00000028,0x00000a27,0x00000a29,0x0004007c,
    0x0000001b,0x00000a71,0x00000a2a,0x00050082,0x0000001b,0x00000a72,0x000000a3,0x00000a71,
    0x0004007c,0x00000006,0x00000a73,0x00000a72,0x00050083,0x00000006,0x00000a2f,0x0000069c,
    0x00000692,0x00050085,0x00000006,0x00000a32,0x00000a2f,0x000009e8,0x00050081,0x00000006,
    0x00000a35,0x0000096d,0x00000a32,0x0006000c,0x00000006,0x00000a38,0x00000001,0x00000004,
    0x00000a2f,0x00050085,0x00000006,0x00000a3a,0x00000a38,0x00000a73,0x0008000c,0x00000006,
    0x00000a7e,0x00000001,0x0000002b,0x00000a3a,0x0000009b,0x00000093,0x00050085,0x00000006,
    0x00000a3e,0x00000a7e,0x00000a7e,0x00050085,0x00000006,0x00000a41,0x00000a3e,0x000009e8,
    0x00050081,0x00000006,0x00000a43,0x00000a1f,0x00000a41,0x00050085,0x00000006,0x00000ab7,
    0x00000914,0x00000917,0x00050083,0x00000006,0x00000ac3,0x0000069c,0x00000696,0x00050083,
    0x00000006,0x00000ac6,0x00000696,0x00000694,0x0006000c,0x00000006,0x00000ac8,0x00000001,
    0x00000004,0x00000ac3,0x0006000c,0x00000006,0x00000aca,0x00000001,0x00000004,0x00000ac6,
    0x0007000c,0x00000006,0x00000acb,0x00000001,0x00000028,0x00000ac8,0x00000aca,0x0004007c,
    0x0000001b,0x00000b1d,0x00000acb,0x00050082,0x0000001b,0x00000b1e,0x000000a3,0x00000b1d,
    0x0004007c,0x00000006,0x00000b1f,0x00000b1e,0x00050083,0x00000006,0x00000ad0,0x0000069c,
    0x00000694,0x00050085,0x00000006,0x00000ad3,0x00000ad0,0x00000ab7,0x00050081,0x00000006,
    0x00000ad6,0x00000a11,0x00000ad3,0x0006000c,0x00000006,0x00000ad9,0x00000001,0x00000004,
    0x00000ad0,0x00050085,0x00000006,0x00000adb,0x00000ad9,0x00000b1f,0x0008000c,0x00000006,
    0x00000b2a,0x00000001,0x0000002b,0x00000adb,0x0000009b,0x00000093,0x00050085,0x00000006,
    0x00000adf,0x00000b2a,0x00000b2a,0x00050085,0x00000006,0x00000ae2,0x00000adf,0x00000ab7,
    0x00050081,0x00000006,0x00000ae4,0x00000a43,0x00000ae2,0x00050083,0x00000006,0x00000ae7,
    0x000006a6,0x00000696,0x0006000c,0x00000006,0x00000aec,0x00000001,0x00000004,0x00000ae7,
    0x0007000c,0x00000006,0x00000aef,0x00000001,0x00000028,0x00000aec,0x00000962,0x0004007c,
    0x0000001b,0x00000b36,0x00000aef,0x00050082,0x0000001b,0x00000b37,0x000000a3,0x00000b36,
    0x0004007c,0x00000006,0x00000b38,0x00000b37,0x00050083,0x00000006,0x00000af4,0x000006a6,
    0x00000698,0x00050085,0x00000006,0x00000af7,0x00000af4,0x00000ab7,0x00050081,0x00000006,
    0x00000afa,0x00000a35,0x00000af7,0x0006000c,0x00000006,0x00000afd,0x00000001,0x00000004,
    0x00000af4,0x00050085,0x00000006,0x00000aff,0x00000afd,0x00000b38,0x0008000c,0x00000006,
    0x00000b43,0x00000001,0x0000002b,0x00000aff,0x0000009b,0x00000093,0x00050085,0x00000006,
    0x00000b03,0x00000b43,0x00000b43,0x00050085,0x00000006,0x00000b06,0x00000b03,0x00000ab7,
    0x00050081,0x00000006,0x00000b08,0x00000ae4,0x00000b06,0x00050085,0x00000006,0x00000b84,
    0x00000913,0x00000917,0x00050083,0x00000006,0x00000b88,0x0000069e,0x0000069c,0x0006000c,
    0x00000006,0x00000b8d,0x00000001,0x00000004,0x00000b88,0x0007000c,0x00000006,0x00000b90,
    0x00000001,0x00000028,0x00000b8d,0x00000ac8,0x0004007c,0x0000001b,0x00000be2,0x00000b90,
    0x00050082,0x0000001b,0x00000be3,0x000000a3,0x00000be2,0x0004007c,0x00000006,0x00000be4,
    0x00000be3,0x00050083,0x00000006,0x00000b95,0x0000069e,0x00000696,0x00050085,0x00000006,
    0x00000b98,0x00000b95,0x00000b84,0x00050081,0x00000006,0x00000b9b,0x00000ad6,0x00000b98,
    0x00060052,0x0000000c,0x0000116f,0x00000b9b,0x0000129d,0x00000000,0x0006000c,0x00000006,
    0x00000b9e,0x00000001,0x00000004,0x00000b95,0x00050085,0x00000006,0x00000ba0,0x00000b9e,
    0x00000be4,0x0008000c,0x00000006,0x00000bef,0x00000001,0x0000002b,0x00000ba0,0x0000009b,
    0x00000093,0x00050085,0x00000006,0x00000ba4,0x00000bef,0x00000bef,0x00050085,0x00000006,
    0x00000ba7,0x00000ba4,0x00000b84,0x00050081,0x00000006,0x00000ba9,0x00000b08,0x00000ba7,
    0x00050083,0x00000006,0x00000bac,0x000006a4,0x0000069c,0x0006000c,0x00000006,0x00000bb1,
    0x00000001,0x00000004,0x00000bac,0x0007000c,0x00000006,0x00000bb4,0x00000001,0x00000028,
    0x00000bb1,0x00000a27,0x0004007c,0x0000001b,0x00000bfb,0x00000bb4,0x00050082,0x0000001b,
    0x00000bfc,0x000000a3,0x00000bfb,0x0004007c,0x00000006,0x00000bfd,0x00000bfc,0x00050083,
    0x00000006,0x00000bb9,0x000006a4,0x000006a2,0x00050085,0x00000006,0x00000bbc,0x00000bb9,
    0x00000b84,0x00050081,0x00000006,0x00000bbf,0x00000afa,0x00000bbc,0x00060052,0x0000000c,
    0x00001172,0x00000bbf,0x0000116f,0x00000001,0x0006000c,0x00000006,0x00000bc2,0x00000001,
    0x00000004,0x00000bb9,0x00050085,0x00000006,0x00000bc4,0x00000bc2,0x00000bfd,0x0008000c,
    0x00000006,0x00000c08,0x00000001,0x0000002b,0x00000bc4,0x0000009b,0x00000093,0x00050085,
    0x00000006,0x00000bc8,0x00000c08,0x00000c08,0x00050085,0x00000006,0x00000bcb,0x00000bc8,
    0x00000b84,0x00050081,0x00000006,0x00000bcd,0x00000ba9,0x00000bcb,0x00050085,0x0000000c,
    0x000006d7,0x00001172,0x00001172,0x00050051,0x00000006,0x000006d9,0x000006d7,0x00000000,
    0x00050051,0x00000006,0x000006db,0x000006d7,0x00000001,0x00050081,0x00000006,0x000006dc,
    0x000006d9,0x000006db,0x000500b8,0x0000004f,0x000006df,0x000006dc,0x000002f5,0x0004007c,
    0x0000001b,0x00000c18,0x000006dc,0x000500c2,0x0000001b,0x00000c1a,0x00000c18,0x000000b1,
    0x00050082,0x0000001b,0x00000c1b,0x000000ac,0x00000c1a,0x0004007c,0x00000006,0x00000c1c,
    0x00000c1b,0x000200f9,0x000006e7,0x000200f8,0x000006e7,0x000600a9,0x00000006,0x0000129c,
    0x000006df,0x00000093,0x00000c1c,0x000300f7,0x000006ef,0x00000000,0x000400fa,0x000006df,
    0x000006ea,0x000006ec,0x000200f8,0x000006ec,0x000200f9,0x000006ef,0x000200f8,0x000006ea,
    0x000200f9,0x000006ef,0x000200f8,0x000006ef,0x000700f5,0x00000006,0x0000127e,0x00000b9b,
    0x000006ec,0x00000093,0x000006ea,0x00060052,0x0000000c,0x00001177,0x0000127e,0x00001172,
    0x00000000,0x00050050,0x0000000c,0x00000c2d,0x0000129c,0x0000129c,0x00050085,0x0000000c,
    0x000006f5,0x00001177,0x00000c2d,0x00050085,0x00000006,0x000006f8,0x00000bcd,0x00000230,
    0x00050085,0x00000006,0x000006fb,0x000006f8,0x000006f8,0x00050051,0x00000006,0x000006fd,
    0x000006f5,0x00000000,0x00050085,0x00000006,0x00000700,0x000006fd,0x000006fd,0x00050051,
    0x00000006,0x00000702,0x000006f5,0x00000001,0x00050085,0x00000006,0x00000705,0x00000702,
    0x00000702,0x00050081,0x00000006,0x00000706,0x00000700,0x00000705,0x0006000c,0x00000006,
    0x00000709,0x00000001,0x00000004,0x000006fd,0x0006000c,0x00000006,0x0000070c,0x00000001,
    0x00000004,0x00000702,0x0007000c,0x00000006,0x0000070d,0x00000001,0x00000028,0x00000709,
    0x0000070c,0x0004007c,0x0000001b,0x00000c36,0x0000070d,0x00050082,0x0000001b,0x00000c37,
    0x000000a3,0x00000c36,0x0004007c,0x00000006,0x00000c38,0x00000c37,0x00050085,0x00000006,
    0x0000070f,0x00000706,0x00000c38,0x00050083,0x00000006,0x00000713,0x0000070f,0x00000093,
    0x00050085,0x00000006,0x00000715,0x00000713,0x000006fb,0x00050081,0x00000006,0x00000716,
    0x00000093,0x00000715,0x00050085,0x00000006,0x0000071a,0x0000033e,0x000006fb,0x00050081,
    0x00000006,0x0000071b,0x00000093,0x0000071a,0x00050050,0x0000000c,0x0000071c,0x00000716,
    0x0000071b,0x00050085,0x00000006,0x00000720,0x00000348,0x000006fb,0x00050081,0x00000006,
    0x00000721,0x00000230,0x00000720,0x0004007c,0x0000001b,0x00000c53,0x00000721,0x00050082,
    0x0000001b,0x00000c54,0x000000a3,0x00000c53,0x0004007c,0x00000006,0x00000c55,0x00000c54,
    0x00050051,0x00000006,0x00000725,0x00000865,0x00000002,0x00050051,0x00000006,0x00000727,
    0x0000086f,0x00000002,0x00050051,0x00000006,0x00000729,0x00000879,0x00000002,0x00060050,
    0x00000011,0x0000072a,0x00000725,0x00000727,0x00000729,0x00050051,0x00000006,0x0000072c,
    0x00000883,0x00000003,0x00050051,0x00000006,0x0000072e,0x0000088d,0x00000003,0x00050051,
    0x00000006,0x00000730,0x00000897,0x00000003,0x00060050,0x00000011,0x00000731,0x0000072c,
    0x0000072e,0x00000730,0x00050051,0x00000006,0x00000733,0x00000865,0x00000001,0x00050051,
    0x00000006,0x00000735,0x0000086f,0x00000001,0x00050051,0x00000006,0x00000737,0x00000879,
    0x00000001,0x00060050,0x00000011,0x00000738,0x00000733,0x00000735,0x00000737,0x0007000c,
    0x00000011,0x00000c5e,0x00000001,0x00000025,0x00000731,0x00000738,0x0007000c,0x00000011,
    0x00000c5f,0x00000001,0x00000025,0x0000072a,0x00000c5e,0x00050051,0x00000006,0x0000073b,
    0x00000883,0x00000000,0x00050051,0x00000006,0x0000073d,0x0000088d,0x00000000,0x00050051,
    0x00000006,0x0000073f,0x00000897,0x00000000,0x00060050,0x00000011,0x00000740,0x0000073b,
    0x0000073d,0x0000073f,0x0007000c,0x00000011,0x00000741,0x00000001,0x00000025,0x00000c5f,
    0x00000740,0x0007000c,0x00000011,0x00000c65,0x00000001,0x00000028,0x00000731,0x00000738,
    0x0007000c,0x00000011,0x00000c66,0x00000001,0x00000028,0x0000072a,0x00000c65,0x0007000c,
    0x00000011,0x0000075f,0x00000001,0x00000028,0x00000c66,0x00000740,0x00050083,0x0000000c,
    0x00000763,0x0000039c,0x0000062b,0x00050051,0x00000006,0x00000765,0x00000847,0x00000000,
    0x00050051,0x00000006,0x00000767,0x00000851,0x00000000,0x00050051,0x00000006,0x00000769,
    0x0000085b,0x00000000,0x00060050,0x00000011,0x0000076a,0x00000765,0x00000767,0x00000769,
    0x00050051,0x00000006,0x00000c7c,0x00000763,0x00000000,0x00050085,0x00000006,0x00000c7f,
    0x00000c7c,0x000006fd,0x00050051,0x00000006,0x00000c81,0x00000763,0x00000001,0x00050085,
    0x00000006,0x00000c84,0x00000c81,0x00000702,0x00050081,0x00000006,0x00000c85,0x00000c7f,
    0x00000c84,0x00060052,0x0000000c,0x0000119e,0x00000c85,0x0000129d,0x00000000,0x0004007f,
    0x00000006,0x00000c8b,0x00000702,0x00050085,0x00000006,0x00000c8c,0x00000c7c,0x00000c8b,
    0x00050085,0x00000006,0x00000c91,0x00000c81,0x000006fd,0x00050081,0x00000006,0x00000c92,
    0x00000c8c,0x00000c91,0x00060052,0x0000000c,0x000011a4,0x00000c92,0x0000119e,0x00000001,
    0x00050085,0x0000000c,0x00000c96,0x000011a4,0x0000071c,0x00050051,0x00000006,0x00000c98,
    0x00000c96,0x00000000,0x00050085,0x00000006,0x00000c9b,0x00000c98,0x00000c98,0x00050051,
    0x00000006,0x00000c9d,0x00000c96,0x00000001,0x00050085,0x00000006,0x00000ca0,0x00000c9d,
    0x00000c9d,0x00050081,0x00000006,0x00000ca1,0x00000c9b,0x00000ca0,0x0007000c,0x00000006,
    0x00000ca4,0x00000001,0x00000025,0x00000ca1,0x00000c55,0x00050085,0x00000006,0x00000ca7,
    0x0000010d,0x00000ca4,0x00050081,0x00000006,0x00000ca9,0x00000ca7,0x00000112,0x00050085,
    0x00000006,0x00000cac,0x00000721,0x00000ca4,0x00050081,0x00000006,0x00000cae,0x00000cac,
    0x00000112,0x00050085,0x00000006,0x00000cb1,0x00000ca9,0x00000ca9,0x00050085,0x00000006,
    0x00000cb4,0x00000cae,0x00000cae,0x00050085,0x00000006,0x00000cb7,0x00000123,0x00000cb1,
    0x00050081,0x00000006,0x00000cb9,0x00000cb7,0x00000128,0x00050085,0x00000006,0x00000cbc,
    0x00000cb9,0x00000cb4,0x0005008e,0x00000011,0x00000cbf,0x0000076a,0x00000cbc,0x00050083,
    0x0000000c,0x00000775,0x000003b7,0x0000062b,0x00050051,0x00000006,0x00000777,0x00000847,
    0x00000001,0x00050051,0x00000006,0x00000779,0x00000851,0x00000001,0x00050051,0x00000006,
    0x0000077b,0x0000085b,0x00000001,0x00060050,0x00000011,0x0000077c,0x00000777,0x00000779,
    0x0000077b,0x00050051,0x00000006,0x00000ce0,0x00000775,0x00000000,0x00050085,0x00000006,
    0x00000ce3,0x00000ce0,0x000006fd,0x00050051,0x00000006,0x00000ce5,0x00000775,0x00000001,
    0x00050085,0x00000006,0x00000ce8,0x00000ce5,0x00000702,0x00050081,0x00000006,0x00000ce9,
    0x00000ce3,0x00000ce8,0x00060052,0x0000000c,0x000011b1,0x00000ce9,0x0000129d,0x00000000,
    0x00050085,0x00000006,0x00000cf0,0x00000ce0,0x00000c8b,0x00050085,0x00000006,0x00000cf5,
    0x00000ce5,0x000006fd,0x00050081,0x00000006,0x00000cf6,0x00000cf0,0x00000cf5,0x00060052,
    0x0000000c,0x000011b7,0x00000cf6,0x000011b1,0x00000001,0x00050085,0x0000000c,0x00000cfa,
    0x000011b7,0x0000071c,0x00050051,0x00000006,0x00000cfc,0x00000cfa,0x00000000,0x00050085,
    0x00000006,0x00000cff,0x00000cfc,0x00000cfc,0x00050051,0x00000006,0x00000d01,0x00000cfa,
    0x00000001,0x00050085,0x00000006,0x00000d04,0x00000d01,0x00000d01,0x00050081,0x00000006,
    0x00000d05,0x00000cff,0x00000d04,0x0007000c,0x00000006,0x00000d08,0x00000001,0x00000025,
    0x00000d05,0x00000c55,0x00050085,0x00000006,0x00000d0b,0x0000010d,0x00000d08,0x00050081,
    0x00000006,0x00000d0d,0x00000d0b,0x00000112,0x00050085,0x00000006,0x00000d10,0x00000721,
    0x00000d08,0x00050081,0x00000006,0x00000d12,0x00000d10,0x00000112,0x00050085,0x00000006,
    0x00000d15,0x00000d0d,0x00000d0d,0x00050085,0x00000006,0x00000d18,0x00000d12,0x00000d12,
    0x00050085,0x00000006,0x00000d1b,0x00000123,0x00000d15,0x00050081,0x00000006,0x00000d1d,
    0x00000d1b,0x00000128,0x00050085,0x00000006,0x00000d20,0x00000d1d,0x00000d18,0x0005008e,
    0x00000011,0x00000d23,0x0000077c,0x00000d20,0x00050081,0x00000011,0x00000d25,0x00000cbf,
    0x00000d23,0x00050081,0x00000006,0x00000d28,0x00000cbc,0x00000d20,0x00050083,0x0000000c,
    0x00000787,0x000003d2,0x0000062b,0x00050051,0x00000006,0x00000789,0x00000865,0x00000000,
    0x00050051,0x00000006,0x0000078b,0x0000086f,0x00000000,0x00050051,0x00000006,0x0000078d,
    0x00000879,0x00000000,0x00060050,0x00000011,0x0000078e,0x00000789,0x0000078b,0x0000078d,
    0x00050051,0x00000006,0x00000d44,0x00000787,0x00000000,0x00050085,0x00000006,0x00000d47,
    0x00000d44,0x000006fd,0x00050051,0x00000006,0x00000d49,0x00000787,0x00000001,0x00050085,
    0x00000006,0x00000d4c,0x00000d49,0x00000702,0x00050081,0x00000006,0x00000d4d,0x00000d47,
    0x00000d4c,0x00060052,0x0000000c,0x000011c4,0x00000d4d,0x0000129d,0x00000000,0x00050085,
    0x00000006,0x00000d54,0x00000d44,0x00000c8b,0x00050085,0x00000006,0x00000d59,0x00000d49,
    0x000006fd,0x00050081,0x00000006,0x00000d5a,0x00000d54,0x00000d59,0x00060052,0x0000000c,
    0x000011ca,0x00000d5a,0x000011c4,0x00000001,0x00050085,0x0000000c,0x00000d5e,0x000011ca,
    0x0000071c,0x00050051,0x00000006,0x00000d60,0x00000d5e,0x00000000,0x00050085,0x00000006,
    0x00000d63,0x00000d60,0x00000d60,0x00050051,0x00000006,0x00000d65,0x00000d5e,0x00000001,
    0x00050085,0x00000006,0x00000d68,0x00000d65,0x00000d65,0x00050081,0x00000006,0x00000d69,
    0x00000d63,0x00000d68,0x0007000c,0x00000006,0x00000d6c,0x00000001,0x00000025,0x00000d69,
    0x00000c55,0x00050085,0x00000006,0x00000d6f,0x0000010d,0x00000d6c,0x00050081,0x00000006,
    0x00000d71,0x00000d6f,0x00000112,0x00050085,0x00000006,0x00000d74,0x00000721,0x00000d6c,
    0x00050081,0x00000006,0x00000d76,0x00000d74,0x00000112,0x00050085,0x00000006,0x00000d79,
    0x00000d71,0x00000d71,0x00050085,0x00000006,0x00000d7c,0x00000d76,0x00000d76,0x00050085,
    0x00000006,0x00000d7f,0x00000123,0x00000d79,0x00050081,0x00000006,0x00000d81,0x00000d7f,
    0x00000128,0x00050085,0x00000006,0x00000d84,0x00000d81,0x00000d7c,0x0005008e,0x00000011,
    0x00000d87,0x0000078e,0x00000d84,0x00050081,0x00000011,0x00000d89,0x00000d25,0x00000d87,
    0x00050081,0x00000006,0x00000d8c,0x00000d28,0x00000d84,0x00050083,0x0000000c,0x00000799,
    0x000003ed,0x0000062b,0x00050051,0x00000006,0x00000da8,0x00000799,0x00000000,0x00050085,
    0x00000006,0x00000dab,0x00000da8,0x000006fd,0x00050051,0x00000006,0x00000dad,0x00000799,
    0x00000001,0x00050085,0x00000006,0x00000db0,0x00000dad,0x00000702,0x00050081,0x00000006,
    0x00000db1,0x00000dab,0x00000db0,0x00060052,0x0000000c,0x000011d7,0x00000db1,0x0000129d,
    0x00000000,0x00050085,0x00000006,0x00000db8,0x00000da8,0x00000c8b,0x00050085,0x00000006,
    0x00000dbd,0x00000dad,0x000006fd,0x00050081,0x00000006,0x00000dbe,0x00000db8,0x00000dbd,
    0x00060052,0x0000000c,0x000011dd,0x00000dbe,0x000011d7,0x00000001,0x00050085,0x0000000c,
    0x00000dc2,0x000011dd,0x0000071c,0x00050051,0x00000006,0x00000dc4,0x00000dc2,0x00000000,
    0x00050085,0x00000006,0x00000dc7,0x00000dc4,0x00000dc4,0x00050051,0x00000006,0x00000dc9,
    0x00000dc2,0x00000001,0x00050085,0x00000006,0x00000dcc,0x00000dc9,0x00000dc9,0x00050081,
    0x00000006,0x00000dcd,0x00000dc7,0x00000dcc,0x0007000c,0x00000006,0x00000dd0,0x00000001,
    0x00000025,0x00000dcd,0x00000c55,0x00050085,0x00000006,0x00000dd3,0x0000010d,0x00000dd0,
    0x00050081,0x00000006,0x00000dd5,0x00000dd3,0x00000112,0x00050085,0x00000006,0x00000dd8,
    0x00000721,0x00000dd0,0x00050081,0x00000006,0x00000dda,0x00000dd8,0x00000112,0x00050085,
    0x00000006,0x00000ddd,0x00000dd5,0x00000dd5,0x00050085,0x00000006,0x00000de0,0x00000dda,
    0x00000dda,0x00050085,0x00000006,0x00000de3,0x00000123,0x00000ddd,0x00050081,0x00000006,
    0x00000de5,0x00000de3,0x00000128,0x00050085,0x00000006,0x00000de8,0x00000de5,0x00000de0,
    0x0005008e,0x00000011,0x00000deb,0x00000738,0x00000de8,0x00050081,0x00000011,0x00000ded,
    0x00000d89,0x00000deb,0x00050081,0x00000006,0x00000df0,0x00000d8c,0x00000de8,0x0004007f,
    0x0000000c,0x000007ab,0x0000062b,0x00050051,0x00000006,0x00000e0c,0x000007ab,0x00000000,
    0x00050085,0x00000006,0x00000e0f,0x00000e0c,0x000006fd,0x00050051,0x00000006,0x00000e11,
    0x000007ab,0x00000001,0x00050085,0x00000006,0x00000e14,0x00000e11,0x00000702,0x00050081,
    0x00000006,0x00000e15,0x00000e0f,0x00000e14,0x00060052,0x0000000c,0x000011ea,0x00000e15,
    0x0000129d,0x00000000,0x00050085,0x00000006,0x00000e1c,0x00000e0c,0x00000c8b,0x00050085,
    0x00000006,0x00000e21,0x00000e11,0x000006fd,0x00050081,0x00000006,0x00000e22,0x00000e1c,
    0x00000e21,0x00060052,0x0000000c,0x000011f0,0x00000e22,0x000011ea,0x00000001,0x00050085,
    0x0000000c,0x00000e26,0x000011f0,0x0000071c,0x00050051,0x00000006,0x00000e28,0x00000e26,
    0x00000000,0x00050085,0x00000006,0x00000e2b,0x00000e28,0x00000e28,0x00050051,0x00000006,
    0x00000e2d,0x00000e26,0x00000001,0x00050085,0x00000006,0x00000e30,0x00000e2d,0x00000e2d,
    0x00050081,0x00000006,0x00000e31,0x00000e2b,0x00000e30,0x0007000c,0x00000006,0x00000e34,
    0x00000001,0x00000025,0x00000e31,0x00000c55,0x00050085,0x00000006,0x00000e37,0x0000010d,
    0x00000e34,0x00050081,0x00000006,0x00000e39,0x00000e37,0x00000112,0x00050085,0x00000006,
    0x00000e3c,0x00000721,0x00000e34,0x00050081,0x00000006,0x00000e3e,0x00000e3c,0x00000112,
    0x00050085,0x00000006,0x00000e41,0x00000e39,0x00000e39,0x00050085,0x00000006,0x00000e44,
    0x00000e3e,0x00000e3e,0x00050085,0x00000006,0x00000e47,0x00000123,0x00000e41,0x00050081,
    0x00000006,0x00000e49,0x00000e47,0x00000128,0x00050085,0x00000006,0x00000e4c,0x00000e49,
    0x00000e44,0x0005008e,0x00000011,0x00000e4f,0x0000072a,0x00000e4c,0x00050081,0x00000011,
    0x00000e51,0x00000ded,0x00000e4f,0x00050081,0x00000006,0x00000e54,0x00000df0,0x00000e4c,
    0x00050083,0x0000000c,0x000007bd,0x00000423,0x0000062b,0x00050051,0x00000006,0x000007bf,
    0x00000865,0x00000003,0x00050051,0x00000006,0x000007c1,0x0000086f,0x00000003,0x00050051,
    0x00000006,0x000007c3,0x00000879,0x00000003,0x00060050,0x00000011,0x000007c4,0x000007bf,
    0x000007c1,0x000007c3,0x00050051,0x00000006,0x00000e70,0x000007bd,0x00000000,0x00050085,
    0x00000006,0x00000e73,0x00000e70,0x000006fd,0x00050051,0x00000006,0x00000e75,0x000007bd,
    0x00000001,0x00050085,0x00000006,0x00000e78,0x00000e75,0x00000702,0x00050081,0x00000006,
    0x00000e79,0x00000e73,0x00000e78,0x00060052,0x0000000c,0x000011fd,0x00000e79,0x0000129d,
    0x00000000,0x00050085,0x00000006,0x00000e80,0x00000e70,0x00000c8b,0x00050085,0x00000006,
    0x00000e85,0x00000e75,0x000006fd,0x00050081,0x00000006,0x00000e86,0x00000e80,0x00000e85,
    0x00060052,0x0000000c,0x00001203,0x00000e86,0x000011fd,0x00000001,0x00050085,0x0000000c,
    0x00000e8a,0x00001203,0x0000071c,0x00050051,0x00000006,0x00000e8c,0x00000e8a,0x00000000,
    0x00050085,0x00000006,0x00000e8f,0x00000e8c,0x00000e8c,0x00050051,0x00000006,0x00000e91,
    0x00000e8a,0x00000001,0x00050085,0x00000006,0x00000e94,0x00000e91,0x00000e91,0x00050081,
    0x00000006,0x00000e95,0x00000e8f,0x00000e94,0x0007000c,0x00000006,0x00000e98,0x00000001,
    0x00000025,0x00000e95,0x00000c55,0x00050085,0x00000006,0x00000e9b,0x0000010d,0x00000e98,
    0x00050081,0x00000006,0x00000e9d,0x00000e9b,0x00000112,0x00050085,0x00000006,0x00000ea0,
    0x00000721,0x00000e98,0x00050081,0x00000006,0x00000ea2,0x00000ea0,0x00000112,0x00050085,
    0x00000006,0x00000ea5,0x00000e9d,0x00000e9d,0x00050085,0x00000006,0x00000ea8,0x00000ea2,
    0x00000ea2,0x00050085,0x00000006,0x00000eab,0x00000123,0x00000ea5,0x00050081,0x00000006,
    0x00000ead,0x00000eab,0x00000128,0x00050085,0x00000006,0x00000eb0,0x00000ead,0x00000ea8,
    0x0005008e,0x00000011,0x00000eb3,0x000007c4,0x00000eb0,0x00050081,0x00000011,0x00000eb5,
    0x00000e51,0x00000eb3,0x00050081,0x00000006,0x00000eb8,0x00000e54,0x00000eb0,0x00050083,
    0x0000000c,0x000007cf,0x0000043e,0x0000062b,0x00050051,0x00000006,0x00000ed4,0x000007cf,
    0x00000000,0x00050085,0x00000006,0x00000ed7,0x00000ed4,0x000006fd,0x00050051,0x00000006,
    0x00000ed9,0x000007cf,0x00000001,0x00050085,0x00000006,0x00000edc,0x00000ed9,0x00000702,
    0x00050081,0x00000006,0x00000edd,0x00000ed7,0x00000edc,0x00060052,0x0000000c,0x00001210,
    0x00000edd,0x0000129d,0x00000000,0x00050085,0x00000006,0x00000ee4,0x00000ed4,0x00000c8b,
    0x00050085,0x00000006,0x00000ee9,0x00000ed9,0x000006fd,0x00050081,0x00000006,0x00000eea,
    0x00000ee4,0x00000ee9,0x00060052,0x0000000c,0x00001216,0x00000eea,0x00001210,0x00000001,
    0x00050085,0x0000000c,0x00000eee,0x00001216,0x0000071c,0x00050051,0x00000006,0x00000ef0,
    0x00000eee,0x00000000,0x00050085,0x00000006,0x00000ef3,0x00000ef0,0x00000ef0,0x00050051,
    0x00000006,0x00000ef5,0x00000eee,0x00000001,0x00050085,0x00000006,0x00000ef8,0x00000ef5,
    0x00000ef5,0x00050081,0x00000006,0x00000ef9,0x00000ef3,0x00000ef8,0x0007000c,0x00000006,
    0x00000efc,0x00000001,0x00000025,0x00000ef9,0x00000c55,0x00050085,0x00000006,0x00000eff,
    0x0000010d,0x00000efc,0x00050081,0x00000006,0x00000f01,0x00000eff,0x00000112,0x00050085,
    0x00000006,0x00000f04,0x00000721,0x00000efc,0x00050081,0x00000006,0x00000f06,0x00000f04,
    0x00000112,0x00050085,0x00000006,0x00000f09,0x00000f01,0x00000f01,0x00050085,0x00000006,
    0x00000f0c,0x00000f06,0x00000f06,0x00050085,0x00000006,0x00000f0f,0x00000123,0x00000f09,
    0x00050081,0x00000006,0x00000f11,0x00000f0f,0x00000128,0x00050085,0x00000006,0x00000f14,
    0x00000f11,0x00000f0c,0x0005008e,0x00000011,0x00000f17,0x00000740,0x00000f14,0x00050081,
    0x00000011,0x00000f19,0x00000eb5,0x00000f17,0x00050081,0x00000006,0x00000f1c,0x00000eb8,
    0x00000f14,0x00050083,0x0000000c,0x000007e1,0x0000045a,0x0000062b,0x00050051,0x00000006,
    0x000007e3,0x00000883,0x00000001,0x00050051,0x00000006,0x000007e5,0x0000088d,0x00000001,
    0x00050051,0x00000006,0x000007e7,0x00000897,0x00000001,0x00060050,0x00000011,0x000007e8,
    0x000007e3,0x000007e5,0x000007e7,0x00050051,0x00000006,0x00000f38,0x000007e1,0x00000000,
    0x00050085,0x00000006,0x00000f3b,0x00000f38,0x000006fd,0x00050051,0x00000006,0x00000f3d,
    0x000007e1,0x00000001,0x00050085,0x00000006,0x00000f40,0x00000f3d,0x00000702,0x00050081,
    0x00000006,0x00000f41,0x00000f3b,0x00000f40,0x00060052,0x0000000c,0x00001223,0x00000f41,
    0x0000129d,0x00000000,0x00050085,0x00000006,0x00000f48,0x00000f38,0x00000c8b,0x00050085,
    0x00000006,0x00000f4d,0x00000f3d,0x000006fd,0x00050081,0x00000006,0x00000f4e,0x00000f48,
    0x00000f4d,0x00060052,0x0000000c,0x00001229,0x00000f4e,0x00001223,0x00000001,0x00050085,
    0x0000000c,0x00000f52,0x00001229,0x0000071c,0x00050051,0x00000006,0x00000f54,0x00000f52,
    0x00000000,0x00050085,0x00000006,0x00000f57,0x00000f54,0x00000f54,0x00050051,0x00000006,
    0x00000f59,0x00000f52,0x00000001,0x00050085,0x00000006,0x00000f5c,0x00000f59,0x00000f59,
    0x00050081,0x00000006,0x00000f5d,0x00000f57,0x00000f5c,0x0007000c,0x00000006,0x00000f60,
    0x00000001,0x00000025,0x00000f5d,0x00000c55,0x00050085,0x00000006,0x00000f63,0x0000010d,
    0x00000f60,0x00050081,0x00000006,0x00000f65,0x00000f63,0x00000112,0x00050085,0x00000006,
    0x00000f68,0x00000721,0x00000f60,0x00050081,0x00000006,0x00000f6a,0x00000f68,0x00000112,
    0x00050085,0x00000006,0x00000f6d,0x00000f65,0x00000f65,0x00050085,0x00000006,0x00000f70,
    0x00000f6a,0x00000f6a,0x00050085,0x00000006,0x00000f73,0x00000123,0x00000f6d,0x00050081,
    0x00000006,0x00000f75,0x00000f73,0x00000128,0x00050085,0x00000006,0x00000f78,0x00000f75,
    0x00000f70,0x0005008e,0x00000011,0x00000f7b,0x000007e8,0x00000f78,0x00050081,0x00000011,
    0x00000f7d,0x00000f19,0x00000f7b,0x00050081,0x00000006,0x00000f80,0x00000f1c,0x00000f78,
    0x00050083,0x0000000c,0x000007f3,0x00000475,0x0000062b,0x00050051,0x00000006,0x000007f5,
    0x00000883,0x00000002,0x00050051,0x00000006,0x000007f7,0x0000088d,0x00000002,0x00050051,
    0x00000006,0x000007f9,0x00000897,0x00000002,0x00060050,0x00000011,0x000007fa,0x000007f5,
    0x000007f7,0x000007f9,0x00050051,0x00000006,0x00000f9c,0x000007f3,0x00000000,0x00050085,
    0x00000006,0x00000f9f,0x00000f9c,0x000006fd,0x00050051,0x00000006,0x00000fa1,0x000007f3,
    0x00000001,0x00050085,0x00000006,0x00000fa4,0x00000fa1,0x00000702,0x00050081,0x00000006,
    0x00000fa5,0x00000f9f,0x00000fa4,0x00060052,0x0000000c,0x00001236,0x00000fa5,0x0000129d,
    0x00000000,0x00050085,0x00000006,0x00000fac,0x00000f9c,0x00000c8b,0x00050085,0x00000006,
    0x00000fb1,0x00000fa1,0x000006fd,0x00050081,0x00000006,0x00000fb2,0x00000fac,0x00000fb1,
    0x00060052,0x0000000c,0x0000123c,0x00000fb2,0x00001236,0x00000001,0x00050085,0x0000000c,
    0x00000fb6,0x0000123c,0x0000071c,0x00050051,0x00000006,0x00000fb8,0x00000fb6,0x00000000,
    0x00050085,0x00000006,0x00000fbb,0x00000fb8,0x00000fb8,0x00050051,0x00000006,0x00000fbd,
    0x00000fb6,0x00000001,0x00050085,0x00000006,0x00000fc0,0x00000fbd,0x00000fbd,0x00050081,
    0x00000006,0x00000fc1,0x00000fbb,0x00000fc0,0x0007000c,0x00000006,0x00000fc4,0x00000001,
    0x00000025,0x00000fc1,0x00000c55,0x00050085,0x00000006,0x00000fc7,0x0000010d,0x00000fc4,
    0x00050081,0x00000006,0x00000fc9,0x00000fc7,0x00000112,0x00050085,0x00000006,0x00000fcc,
    0x00000721,0x00000fc4,0x00050081,0x00000006,0x00000fce,0x00000fcc,0x00000112,0x00050085,
    0x00000006,0x00000fd1,0x00000fc9,0x00000fc9,0x00050085,0x00000006,0x00000fd4,0x00000fce,
    0x00000fce,0x00050085,0x00000006,0x00000fd7,0x00000123,0x00000fd1,0x00050081,0x00000006,
    0x00000fd9,0x00000fd7,0x00000128,0x00050085,0x00000006,0x00000fdc,0x00000fd9,0x00000fd4,
    0x0005008e,0x00000011,0x00000fdf,0x000007fa,0x00000fdc,0x00050081,0x00000011,0x00000fe1,
    0x00000f7d,0x00000fdf,0x00050081,0x00000006,0x00000fe4,0x00000f80,0x00000fdc,0x00050083,
    0x0000000c,0x00000805,0x00000490,0x0000062b,0x00050051,0x00000006,0x00001000,0x00000805,
    0x00000000,0x00050085,0x00000006,0x00001003,0x00001000,0x000006fd,0x00050051,0x00000006,
    0x00001005,0x00000805,0x00000001,0x00050085,0x00000006,0x00001008,0x00001005,0x00000702,
    0x00050081,0x00000006,0x00001009,0x00001003,0x00001008,0x00060052,0x0000000c,0x00001249,
    0x00001009,0x0000129d,0x00000000,0x00050085,0x00000006,0x00001010,0x00001000,0x00000c8b,
    0x00050085,0x00000006,0x00001015,0x00001005,0x000006fd,0x00050081,0x00000006,0x00001016,
    0x00001010,0x00001015,0x00060052,0x0000000c,0x0000124f,0x00001016,0x00001249,0x00000001,
    0x00050085,0x0000000c,0x0000101a,0x0000124f,0x0000071c,0x00050051,0x00000006,0x0000101c,
    0x0000101a,0x00000000,0x00050085,0x00000006,0x0000101f,0x0000101c,0x0000101c,0x00050051,
    0x00000006,0x00001021,0x0000101a,0x00000001,0x00050085,0x00000006,0x00001024,0x00001021,
    0x00001021,0x00050081,0x00000006,0x00001025,0x0000101f,0x00001024,0x0007000c,0x00000006,
    0x00001028,0x00000001,0x00000025,0x00001025,0x00000c55,0x00050085,0x00000006,0x0000102b,
    0x0000010d,0x00001028,0x00050081,0x00000006,0x0000102d,0x0000102b,0x00000112,0x00050085,
    0x00000006,0x00001030,0x00000721,0x00001028,0x00050081,0x00000006,0x00001032,0x00001030,
    0x00000112,0x00050085,0x00000006,0x00001035,0x0000102d,0x0000102d,0x00050085,0x00000006,
    0x00001038,0x00001032,0x00001032,0x00050085,0x00000006,0x0000103b,0x00000123,0x00001035,
    0x00050081,0x00000006,0x0000103d,0x0000103b,0x00000128,0x00050085,0x00000006,0x00001040,
    0x0000103d,0x00001038,0x0005008e,0x00000011,0x00001043,0x00000731,0x00001040,0x00050081,
    0x00000011,0x00001045,0x00000fe1,0x00001043,0x00050081,0x00000006,0x00001048,0x00000fe4,
    0x00001040,0x00050083,0x0000000c,0x00000817,0x000004ab,0x0000062b,0x00050051,0x00000006,
    0x00000819,0x000008a1,0x00000002,0x00050051,0x00000006,0x0000081b,0x000008ab,0x00000002,
    0x00050051,0x00000006,0x0000081d,0x000008b5,0x00000002,0x00060050,0x00000011,0x0000081e,
    0x00000819,0x0000081b,0x0000081d,0x00050051,0x00000006,0x00001064,0x00000817,0x00000000,
    0x00050085,0x00000006,0x00001067,0x00001064,0x000006fd,0x00050051,0x00000006,0x00001069,
    0x00000817,0x00000001,0x00050085,0x00000006,0x0000106c,0x00001069,0x00000702,0x00050081,
    0x00000006,0x0000106d,0x00001067,0x0000106c,0x00060052,0x0000000c,0x0000125c,0x0000106d,
    0x0000129d,0x00000000,0x00050085,0x00000006,0x00001074,0x00001064,0x00000c8b,0x00050085,
    0x00000006,0x00001079,0x00001069,0x000006fd,0x00050081,0x00000006,0x0000107a,0x00001074,
    0x00001079,0x00060052,0x0000000c,0x00001262,0x0000107a,0x0000125c,0x00000001,0x00050085,
    0x0000000c,0x0000107e,0x00001262,0x0000071c,0x00050051,0x00000006,0x00001080,0x0000107e,
    0x00000000,0x00050085,0x00000006,0x00001083,0x00001080,0x00001080,0x00050051,0x00000006,
    0x00001085,0x0000107e,0x00000001,0x00050085,0x00000006,0x00001088,0x00001085,0x00001085,
    0x00050081,0x00000006,0x00001089,0x00001083,0x00001088,0x0007000c,0x00000006,0x0000108c,
    0x00000001,0x00000025,0x00001089,0x00000c55,0x00050085,0x00000006,0x0000108f,0x0000010d,
    0x0000108c,0x00050081,0x00000006,0x00001091,0x0000108f,0x00000112,0x00050085,0x00000006,
    0x00001094,0x00000721,0x0000108c,0x00050081,0x00000006,0x00001096,0x00001094,0x00000112,
    0x00050085,0x00000006,0x00001099,0x00001091,0x00001091,0x00050085,0x00000006,0x0000109c,
    0x00001096,0x00001096,0x00050085,0x00000006,0x0000109f,0x00000123,0x00001099,0x00050081,
    0x00000006,0x000010a1,0x0000109f,0x00000128,0x00050085,0x00000006,0x000010a4,0x000010a1,
    0x0000109c,0x0005008e,0x00000011,0x000010a7,0x0000081e,0x000010a4,0x00050081,0x00000011,
    0x000010a9,0x00001045,0x000010a7,0x00050081,0x00000006,0x000010ac,0x00001048,0x000010a4,
    0x00050083,0x0000000c,0x00000829,0x000004c6,0x0000062b,0x00050051,0x00000006,0x0000082b,
    0x000008a1,0x00000003,0x00050051,0x00000006,0x0000082d,0x000008ab,0x00000003,0x00050051,
    0x00000006,0x0000082f,0x000008b5,0x00000003,0x00060050,0x00000011,0x00000830,0x0000082b,
    0x0000082d,0x0000082f,0x00050051,0x00000006,0x000010c8,0x00000829,0x00000000,0x00050085,
    0x00000006,0x000010cb,0x000010c8,0x000006fd,0x00050051,0x00000006,0x000010cd,0x00000829,
    0x00000001,0x00050085,0x00000006,0x000010d0,0x000010cd,0x00000702,0x00050081,0x00000006,
    0x000010d1,0x000010cb,0x000010d0,0x00060052,0x0000000c,0x0000126f,0x000010d1,0x0000129d,
    0x00000000,0x00050085,0x00000006,0x000010d8,0x000010c8,0x00000c8b,0x00050085,0x00000006,
    0x000010dd,0x000010cd,0x000006fd,0x00050081,0x00000006,0x000010de,0x000010d8,0x000010dd,
    0x00060052,0x0000000c,0x00001275,0x000010de,0x0000126f,0x00000001,0x00050085,0x0000000c,
    0x000010e2,0x00001275,0x0000071c,0x00050051,0x00000006,0x000010e4,0x000010e2,0x00000000,
    0x00050085,0x00000006,0x000010e7,0x000010e4,0x000010e4,0x00050051,0x00000006,0x000010e9,
    0x000010e2,0x00000001,0x00050085,0x00000006,0x000010ec,0x000010e9,0x000010e9,0x00050081,
    0x00000006,0x000010ed,0x000010e7,0x000010ec,0x0007000c,0x00000006,0x000010f0,0x00000001,
    0x00000025,0x000010ed,0x00000c55,0x00050085,0x00000006,0x000010f3,0x0000010d,0x000010f0,
    0x00050081,0x00000006,0x000010f5,0x000010f3,0x00000112,0x00050085,0x00000006,0x000010f8,
    0x00000721,0x000010f0,0x00050081,0x00000006,0x000010fa,0x000010f8,0x00000112,0x00050085,
    0x00000006,0x000010fd,0x000010f5,0x000010f5,0x00050085,0x00000006,0x00001100,0x000010fa,
    0x000010fa,0x00050085,0x00000006,0x00001103,0x00000123,0x000010fd,0x00050081,0x00000006,
    0x00001105,0x00001103,0x00000128,0x00050085,0x00000006,0x00001108,0x00001105,0x00001100,
    0x0005008e,0x00000011,0x0000110b,0x00000830,0x00001108,0x00050081,0x00000011,0x0000110d,
    0x000010a9,0x0000110b,0x00050081,0x00000006,0x00001110,0x000010ac,0x00001108,0x00050088,
    0x00000006,0x00001125,0x00000093,0x00001110,0x00060050,0x00000011,0x0000112e,0x00001125,
    0x00001125,0x00001125,0x00050085,0x00000011,0x00000840,0x0000110d,0x0000112e,0x0007000c,
    0x00000011,0x00000841,0x00000001,0x00000028,0x00000741,0x00000840,0x0007000c,0x00000011,
    0x00000842,0x00000001,0x00000025,0x0000075f,0x00000841,0x0004003d,0x00000515,0x00000518,
    0x00000517,0x0004007c,0x0000051b,0x0000051c,0x000004f0,0x00050051,0x00000006,0x0000051e,
    0x00000842,0x00000000,0x00050051,0x00000006,0x0000051f,0x00000842,0x00000001,0x00050051,
    0x00000006,0x00000520,0x00000842,0x00000002,0x00070050,0x00000016,0x00000521,0x0000051e,
    0x0000051f,0x00000520,0x00000093,0x00040063,0x00000518,0x0000051c,0x00000521,0x000200f9,
    0x00000524,0x000200f8,0x00000524,0x000100fd,0x00010038
};

/*
#version 460
#extension GL_GOOGLE_include_directive: require

layout(local_size_x=8, local_size_y=8, local_size_z=1) in;

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1) uniform writeonly image2D outImage;

layout(push_constant) uniform pushConstants {
    uvec2 c1;
    ivec2 extent;
    ivec4 viewport;
};

#define A_GPU 1
#define A_GLSL 1
//#include "ffx_a.h"
#define FSR_RCAS_F 1
vec4 FsrRcasLoadF(ivec2 p) { return texelFetch(texSampler, clamp(p, ivec2(0), extent), 0); }
void FsrRcasInputF(inout float r, inout float g, inout float b) {}
//#include "ffx_fsr1.h"


void main()
{
    vec3 color;

    if (any(lessThan(gl_GlobalInvocationID.xy, uvec2(viewport.xy))) ||
        any(greaterThan(gl_GlobalInvocationID.xy, uvec2(viewport.zw))))
    {
        color = vec3(0.0, 0.0, 0.0);
    }
    else
    {
        FsrRcasF(color.r, color.g, color.b, uvec2(gl_GlobalInvocationID.xy - viewport.xy), c1.xyxx);
    }

    imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), vec4(color, 1.0));
}
*/
const uint32_t fsr_rcas_comp_spv[] = {
    0x07230203,0x00010000,0x0008000a,0x0000061e,0x00000000,0x00020011,0x00000001,0x00020011,
    0x00000038,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,
    0x00000000,0x00000001,0x0006000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x00000287,
    0x00060010,0x00000004,0x00000011,0x00000008,0x00000008,0x00000001,0x00030003,0x00000002,
    0x000001cc,0x000a0004,0x475f4c47,0x4c474f4f,0x70635f45,0x74735f70,0x5f656c79,0x656e696c,
    0x7269645f,0x69746365,0x00006576,0x00080004,0x475f4c47,0x4c474f4f,0x6e695f45,0x64756c63,
    0x69645f65,0x74636572,0x00657669,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00050005,
    0x0000007b,0x53786574,0x6c706d61,0x00007265,0x00060005,0x00000081,0x68737570,0x736e6f43,
    0x746e6174,0x00000073,0x00040006,0x00000081,0x00000000,0x00003163,0x00050006,0x00000081,
    0x00000001,0x65747865,0x0000746e,0x00060006,0x00000081,0x00000002,0x77656976,0x74726f70,
    0x00000000,0x00030005,0x00000083,0x00000000,0x00080005,0x00000287,0x475f6c67,0x61626f6c,
    0x766e496c,0x7461636f,0x496e6f69,0x00000044,0x00050005,0x000002c0,0x4974756f,0x6567616d,
    0x00000000,0x00040047,0x0000007b,0x00000022,0x00000000,0x00040047,0x0000007b,0x00000021,
    0x00000000,0x00050048,0x00000081,0x00000000,0x00000023,0x00000000,0x00050048,0x00000081,
    0x00000001,0x00000023,0x00000008,0x00050048,0x00000081,0x00000002,0x00000023,0x00000010,
    0x00030047,0x00000081,0x00000002,0x00040047,0x00000287,0x0000000b,0x0000001c,0x00040047,
    0x000002c0,0x00000022,0x00000000,0x00040047,0x000002c0,0x00000021,0x00000001,0x00030047,
    0x000002c0,0x00000019,0x00040047,0x000002cb,0x0000000b,0x00000019,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040015,0x0000000c,
    0x00000020,0x00000000,0x00040015,0x00000026,0x00000020,0x00000001,0x00040017,0x00000027,
    0x00000026,0x00000002,0x00040017,0x00000029,0x00000006,0x00000004,0x00040017,0x00000034,
    0x0000000c,0x00000002,0x0004002b,0x00000006,0x00000054,0x3f800000,0x0004002b,0x00000006,
    0x0000005c,0x00000000,0x0004002b,0x0000000c,0x00000065,0x7ef19fff,0x0004002b,0x00000006,
    0x00000071,0x40000000,0x00090019,0x00000078,0x00000006,0x00000001,0x00000000,0x00000000,
    0x00000000,0x00000001,0x00000000,0x0003001b,0x00000079,0x00000078,0x00040020,0x0000007a,
    0x00000000,0x00000079,0x0004003b,0x0000007a,0x0000007b,0x00000000,0x0004002b,0x00000026,
    0x0000007e,0x00000000,0x0005002c,0x00000027,0x0000007f,0x0000007e,0x0000007e,0x00040017,
    0x00000080,0x00000026,0x00000004,0x0005001e,0x00000081,0x00000034,0x00000027,0x00000080,
    0x00040020,0x00000082,0x00000009,0x00000081,0x0004003b,0x00000082,0x00000083,0x00000009,
    0x0004002b,0x00000026,0x00000084,0x00000001,0x00040020,0x00000085,0x00000009,0x00000027,
    0x00040017,0x00000090,0x00000006,0x00000003,0x0004002b,0x00000026,0x00000094,0xffffffff,
    0x0005002c,0x00000027,0x00000095,0x0000007e,0x00000094,0x0005002c,0x00000027,0x0000009c,
    0x00000094,0x0000007e,0x0005002c,0x00000027,0x000000a8,0x00000084,0x0000007e,0x0005002c,
    0x00000027,0x000000af,0x0000007e,0x00000084,0x0004002b,0x0000000c,0x000000b9,0x00000001,
    0x0004002b,0x00000006,0x00000154,0x3e800000,0x0004002b,0x00000006,0x000001d3,0xc0800000,
    0x0004002b,0x00000006,0x000001d7,0x40800000,0x0004002b,0x00000006,0x0000022e,0xbe400000,
    0x00020014,0x00000284,0x00040017,0x00000285,0x0000000c,0x00000003,0x00040020,0x00000286,
    0x00000001,0x00000285,0x0004003b,0x00000286,0x00000287,0x00000001,0x0004002b,0x00000026,
    0x0000028a,0x00000002,0x00040020,0x0000028b,0x00000009,0x00000080,0x00040017,0x00000290,
    0x00000284,0x00000002,0x0006002c,0x00000090,0x000002a2,0x0000005c,0x0000005c,0x0000005c,
    0x00040020,0x000002b3,0x00000009,0x00000034,0x00090019,0x000002be,0x00000006,0x00000001,
    0x00000000,0x00000000,0x00000000,0x00000002,0x00000000,0x00040020,0x000002bf,0x00000000,
    0x000002be,0x0004003b,0x000002bf,0x000002c0,0x00000000,0x0004002b,0x0000000c,0x000002ca,
    0x00000008,0x0006002c,0x00000285,0x000002cb,0x000002ca,0x000002ca,0x000000b9,0x00030001,
    0x00000090,0x0000061d,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,
    0x00000005,0x0004003d,0x00000285,0x00000288,0x00000287,0x0007004f,0x00000034,0x00000289,
    0x00000288,0x00000288,0x00000000,0x00000001,0x00050041,0x0000028b,0x0000028c,0x00000083,
    0x0000028a,0x0004003d,0x00000080,0x0000028d,0x0000028c,0x0007004f,0x00000027,0x0000028e,
    0x0000028d,0x0000028d,0x00000000,0x00000001,0x0004007c,0x00000034,0x0000028f,0x0000028e,
    0x000500b0,0x00000290,0x00000291,0x00000289,0x0000028f,0x0004009a,0x00000284,0x00000292,
    0x00000291,0x000400a8,0x00000284,0x00000293,0x00000292,0x000300f7,0x00000295,0x00000000,
    0x000400fa,0x00000293,0x00000294,0x00000295,0x000200f8,0x00000294,0x0007004f,0x00000027,
    0x0000029a,0x0000028d,0x0000028d,0x00000002,0x00000003,0x0004007c,0x00000034,0x0000029b,
    0x0000029a,0x000500ac,0x00000290,0x0000029c,0x00000289,0x0000029b,0x0004009a,0x00000284,
    0x0000029d,0x0000029c,0x000200f9,0x00000295,0x000200f8,0x00000295,0x000700f5,0x00000284,
    0x0000029e,0x00000292,0x00000005,0x0000029d,0x00000294,0x000300f7,0x000002a0,0x00000000,
    0x000400fa,0x0000029e,0x0000029f,0x000002a3,0x000200f8,0x0000029f,0x000200f9,0x000002a0,
    0x000200f8,0x000002a3,0x00050082,0x00000034,0x000002aa,0x00000289,0x0000028f,0x00050041,
    0x000002b3,0x000002b4,0x00000083,0x0000007e,0x0004003d,0x00000034,0x000002b5,0x000002b4,
    0x0004007c,0x00000027,0x00000353,0x000002aa,0x00050080,0x00000027,0x00000355,0x00000353,
    0x00000095,0x0004003d,0x00000079,0x000004b3,0x0000007b,0x00050041,0x00000085,0x000004b5,
    0x00000083,0x00000084,0x0004003d,0x00000027,0x000004b6,0x000004b5,0x0008000c,0x00000027,
    0x000004b7,0x00000001,0x0000002d,0x00000355,0x0000007f,0x000004b6,0x00040064,0x00000078,
    0x000004b8,0x000004b3,0x0007005f,0x00000029,0x000004b9,0x000004b8,0x000004b7,0x00000002,
    0x0000007e,0x00050080,0x00000027,0x00000359,0x00000353,0x0000009c,0x0008000c,0x00000027,
    0x000004c0,0x00000001,0x0000002d,0x00000359,0x0000007f,0x000004b6,0x00040064,0x00000078,
    0x000004c1,0x000004b3,0x0007005f,0x00000029,0x000004c2,0x000004c1,0x000004c0,0x00000002,
    0x0000007e,0x0008000c,0x00000027,0x000004c9,0x00000001,0x0000002d,0x00000353,0x0000007f,
    0x000004b6,0x00040064,0x00000078,0x000004ca,0x000004b3,0x0007005f,0x00000029,0x000004cb,
    0x000004ca,0x000004c9,0x00000002,0x0000007e,0x00050080,0x00000027,0x00000360,0x00000353,
    0x000000a8,0x0008000c,0x00000027,0x000004d2,0x00000001,0x0000002d,0x00000360,0x0000007f,
    0x000004b6,0x00040064,0x00000078,0x000004d3,0x000004b3,0x0007005f,0x00000029,0x000004d4,
    0x000004d3,0x000004d2,0x00000002,0x0000007e,0x00050080,0x00000027,0x00000364,0x00000353,
    0x000000af,0x0008000c,0x00000027,0x000004db,0x00000001,0x0000002d,0x00000364,0x0000007f,
    0x000004b6,0x00040064,0x00000078,0x000004dc,0x000004b3,0x0007005f,0x00000029,0x000004dd,
    0x000004dc,0x000004db,0x00000002,0x0000007e,0x00050051,0x00000006,0x00000368,0x000004b9,
    0x00000000,0x00050051,0x00000006,0x0000036a,0x000004b9,0x00000001,0x00050051,0x00000006,
    0x0000036c,0x000004b9,0x00000002,0x00050051,0x00000006,0x0000036e,0x000004c2,0x00000000,
    0x00050051,0x00000006,0x00000370,0x000004c2,0x00000001,0x00050051,0x00000006,0x00000372,
    0x000004c2,0x00000002,0x00050051,0x00000006,0x00000374,0x000004cb,0x00000000,0x00050051,
    0x00000006,0x00000376,0x000004cb,0x00000001,0x00050051,0x00000006,0x00000378,0x000004cb,
    0x00000002,0x00050051,0x00000006,0x0000037a,0x000004d4,0x00000000,0x00050051,0x00000006,
    0x0000037c,0x000004d4,0x00000001,0x00050051,0x00000006,0x0000037e,0x000004d4,0x00000002,
    0x00050051,0x00000006,0x00000380,0x000004dd,0x00000000,0x00050051,0x00000006,0x00000382,
    0x000004dd,0x00000001,0x00050051,0x00000006,0x00000384,0x000004dd,0x00000002,0x0007000c,
    0x00000006,0x0000055a,0x00000001,0x00000025,0x0000036e,0x0000037a,0x0007000c,0x00000006,
    0x0000055b,0x00000001,0x00000025,0x00000368,0x0000055a,0x0007000c,0x00000006,0x00000404,
    0x00000001,0x00000025,0x0000055b,0x00000380,0x0007000c,0x00000006,0x00000561,0x00000001,
    0x00000025,0x00000370,0x0000037c,0x0007000c,0x00000006,0x00000562,0x00000001,0x00000025,
    0x0000036a,0x00000561,0x0007000c,0x00000006,0x0000040a,0x00000001,0x00000025,0x00000562,
    0x00000382,0x0007000c,0x00000006,0x00000568,0x00000001,0x00000025,0x00000372,0x0000037e,
    0x0007000c,0x00000006,0x00000569,0x00000001,0x00000025,0x0000036c,0x00000568,0x0007000c,
    0x00000006,0x00000410,0x00000001,0x00000025,0x00000569,0x00000384,0x0007000c,0x00000006,
    0x0000056f,0x00000001,0x00000028,0x0000036e,0x0000037a,0x0007000c,0x00000006,0x00000570,
    0x00000001,0x00000028,0x00000368,0x0000056f,0x0007000c,0x00000006,0x00000416,0x00000001,
    0x00000028,0x00000570,0x00000380,0x0007000c,0x00000006,0x00000576,0x00000001,0x00000028,
    0x00000370,0x0000037c,0x0007000c,0x00000006,0x00000577,0x00000001,0x00000028,0x0000036a,
    0x00000576,0x0007000c,0x00000006,0x0000041c,0x00000001,0x00000028,0x00000577,0x00000382,
    0x0007000c,0x00000006,0x0000057d,0x00000001,0x00000028,0x00000372,0x0000037e,0x0007000c,
    0x00000006,0x0000057e,0x00000001,0x00000028,0x0000036c,0x0000057d,0x0007000c,0x00000006,
    0x00000422,0x00000001,0x00000028,0x0000057e,0x00000384,0x00050088,0x00000006,0x00000587,
    0x00000154,0x00000416,0x00050085,0x00000006,0x00000428,0x00000404,0x00000587,0x00050088,
    0x00000006,0x00000593,0x00000154,0x0000041c,0x00050085,0x00000006,0x0000042e,0x0000040a,
    0x00000593,0x00050088,0x00000006,0x0000059f,0x00000154,0x00000422,0x00050085,0x00000006,
    0x00000434,0x00000410,0x0000059f,0x00050083,0x00000006,0x00000438,0x00000054,0x00000416,
    0x00050085,0x00000006,0x0000043b,0x000001d7,0x00000404,0x00050081,0x00000006,0x0000043e,
    0x0000043b,0x000001d3,0x00050088,0x00000006,0x000005ab,0x00000054,0x0000043e,0x00050085,
    0x00000006,0x00000440,0x00000438,0x000005ab,0x00050083,0x00000006,0x00000444,0x00000054,
    0x0000041c,0x00050085,0x00000006,0x00000447,0x000001d7,0x0000040a,0x00050081,0x00000006,
    0x0000044a,0x00000447,0x000001d3,0x00050088,0x00000006,0x000005b7,0x00000054,0x0000044a,
    0x00050085,0x00000006,0x0000044c,0x00000444,0x000005b7,0x00050083,0x00000006,0x00000450,
    0x00000054,0x00000422,0x00050085,0x00000006,0x00000453,0x000001d7,0x00000410,0x00050081,
    0x00000006,0x00000456,0x00000453,0x000001d3,0x00050088,0x00000006,0x000005c3,0x00000054,
    0x00000456,0x00050085,0x00000006,0x00000458,0x00000450,0x000005c3,0x0004007f,0x00000006,
    0x0000045a,0x00000428,0x0007000c,0x00000006,0x0000045c,0x00000001,0x00000028,0x0000045a,
    0x00000440,0x0004007f,0x00000006,0x0000045e,0x0000042e,0x0007000c,0x00000006,0x00000460,
    0x00000001,0x00000028,0x0000045e,0x0000044c,0x0004007f,0x00000006,0x00000462,0x00000434,
    0x0007000c,0x00000006,0x00000464,0x00000001,0x00000028,0x00000462,0x00000458,0x0007000c,
    0x00000006,0x000005cf,0x00000001,0x00000028,0x00000460,0x00000464,0x0007000c,0x00000006,
    0x000005d0,0x00000001,0x00000028,0x0000045c,0x000005cf,0x0007000c,0x00000006,0x0000046b,
    0x00000001,0x00000025,0x000005d0,0x0000005c,0x0007000c,0x00000006,0x0000046c,0x00000001,
    0x00000028,0x0000022e,0x0000046b,0x00050051,0x0000000c,0x0000046e,0x000002b5,0x00000000,
    0x0004007c,0x00000006,0x0000046f,0x0000046e,0x00050085,0x00000006,0x00000470,0x0000046c,
    0x0000046f,0x00050085,0x00000006,0x00000473,0x000001d7,0x00000470,0x00050081,0x00000006,
    0x00000475,0x00000473,0x00000054,0x0004007c,0x0000000c,0x000005e1,0x00000475,0x00050082,
    0x0000000c,0x000005e2,0x00000065,0x000005e1,0x0004007c,0x00000006,0x000005e3,0x000005e2,
    0x0004007f,0x00000006,0x000005e6,0x000005e3,0x00050085,0x00000006,0x000005e8,0x000005e6,
    0x00000475,0x00050081,0x00000006,0x000005ea,0x000005e8,0x00000071,0x00050085,0x00000006,
    0x000005eb,0x000005e3,0x000005ea,0x00050081,0x00000006,0x00000611,0x00000368,0x0000036e,
    0x00050081,0x00000006,0x00000612,0x00000611,0x00000380,0x00050081,0x00000006,0x00000613,
    0x00000612,0x0000037a,0x00050085,0x00000006,0x00000485,0x00000470,0x00000613,0x00050081,
    0x00000006,0x00000487,0x00000485,0x00000374,0x00050085,0x00000006,0x00000489,0x00000487,
    0x000005eb,0x00050081,0x00000006,0x00000614,0x0000036a,0x00000370,0x00050081,0x00000006,
    0x00000615,0x00000614,0x00000382,0x00050081,0x00000006,0x00000616,0x00000615,0x0000037c,
    0x00050085,0x00000006,0x00000498,0x00000470,0x00000616,0x00050081,0x00000006,0x0000049a,
    0x00000498,0x00000376,0x00050085,0x00000006,0x0000049c,0x0000049a,0x000005eb,0x00050081,
    0x00000006,0x00000617,0x0000036c,0x00000372,0x00050081,0x00000006,0x00000618,0x00000617,
    0x00000384,0x00050081,0x00000006,0x00000619,0x00000618,0x0000037e,0x00050085,0x00000006,
    0x000004ab,0x00000470,0x00000619,0x00050081,0x00000006,0x000004ad,0x000004ab,0x00000378,
    0x00050085,0x00000006,0x000004af,0x000004ad,0x000005eb,0x00060052,0x00000090,0x00000609,
    0x00000489,0x0000061d,0x00000000,0x00060052,0x00000090,0x0000060b,0x0000049c,0x00000609,
    0x00000001,0x00060052,0x00000090,0x0000060d,0x000004af,0x0000060b,0x00000002,0x000200f9,
    0x000002a0,0x000200f8,0x000002a0,0x000700f5,0x00000090,0x0000061c,0x000002a2,0x0000029f,
    0x0000060d,0x000002a3,0x0004003d,0x000002be,0x000002c1,0x000002c0,0x0004007c,0x00000027,
    0x000002c4,0x00000289,0x00050051,0x00000006,0x000002c6,0x0000061c,0x00000000,0x00050051,
    0x00000006,0x000002c7,0x0000061c,0x00000001,0x00050051,0x00000006,0x000002c8,0x0000061c,
    0x00000002,0x00070050,0x00000029,0x000002c9,0x000002c6,0x000002c7,0x000002c8,0x00000054,
    0x00040063,0x000002c1,0x000002c4,0x000002c9,0x000100fd,0x00010038
};

static void destroy_pipeline(VkDevice device, struct fs_comp_pipeline *pipeline)
{
    device->funcs.p_vkDestroyPipeline(device->device, pipeline->pipeline, NULL);
    pipeline->pipeline = VK_NULL_HANDLE;

    device->funcs.p_vkDestroyPipelineLayout(device->device, pipeline->pipeline_layout, NULL);
    pipeline->pipeline_layout = VK_NULL_HANDLE;
}

static VkResult create_pipeline(VkDevice device, struct VkSwapchainKHR_T *swapchain,
    const uint32_t *code, uint32_t code_size, uint32_t push_size, struct fs_comp_pipeline *pipeline)
{
#if defined(USE_STRUCT_CONVERSION)
    VkComputePipelineCreateInfo_host pipelineInfo = {0};
#else
    VkComputePipelineCreateInfo pipelineInfo = {0};
#endif
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
    VkShaderModuleCreateInfo shaderInfo = {0};
    VkPushConstantRange pushConstants;
    VkShaderModule shaderModule = 0;
    VkResult res;

    pipeline->push_size = push_size;

    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.offset = 0;
    pushConstants.size = push_size;

    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &swapchain->descriptor_set_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;

    res = device->funcs.p_vkCreatePipelineLayout(device->device, &pipelineLayoutInfo, NULL, &pipeline->pipeline_layout);
    if(res != VK_SUCCESS)
    {
        ERR("vkCreatePipelineLayout: %d\n", res);
        goto fail;
    }

    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = code_size;
    shaderInfo.pCode = code;

    res = device->funcs.p_vkCreateShaderModule(device->device, &shaderInfo, NULL, &shaderModule);
    if(res != VK_SUCCESS)
    {
        ERR("vkCreateShaderModule: %d\n", res);
        goto fail;
    }

    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipeline->pipeline_layout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    res = device->funcs.p_vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline->pipeline);
    if(res == VK_SUCCESS)
        goto out;

    ERR("vkCreateComputePipelines: %d\n", res);

fail:
    destroy_pipeline(device, pipeline);

out:
    device->funcs.p_vkDestroyShaderModule(device->device, shaderModule, NULL);

    return res;
}

static VkResult create_descriptor_set(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
    VkResult res;
#if defined(USE_STRUCT_CONVERSION)
    VkDescriptorSetAllocateInfo_host descriptorAllocInfo = {0};
    VkWriteDescriptorSet_host descriptorWrites[2] = {{0}, {0}};
    VkDescriptorImageInfo_host userDescriptorImageInfo = {0}, realDescriptorImageInfo = {0};
#else
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {0};
    VkWriteDescriptorSet descriptorWrites[2] = {{0}, {0}};
    VkDescriptorImageInfo userDescriptorImageInfo = {0}, realDescriptorImageInfo = {0};
#endif

    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = swapchain->descriptor_pool;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &swapchain->descriptor_set_layout;

    res = device->funcs.p_vkAllocateDescriptorSets(device->device, &descriptorAllocInfo, &hack->descriptor_set);
    if (res != VK_SUCCESS)
    {
        ERR("vkAllocateDescriptorSets: %d\n", res);
        return res;
    }

    userDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    userDescriptorImageInfo.imageView = hack->user_view;
    userDescriptorImageInfo.sampler = swapchain->sampler;

    realDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    realDescriptorImageInfo.imageView = swapchain->fsr ? hack->fsr_view : hack->swapchain_view;

    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = hack->descriptor_set;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &userDescriptorImageInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = hack->descriptor_set;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &realDescriptorImageInfo;

    device->funcs.p_vkUpdateDescriptorSets(device->device, 2, descriptorWrites, 0, NULL);

    if (swapchain->fsr)
    {
        res = device->funcs.p_vkAllocateDescriptorSets(device->device, &descriptorAllocInfo, &hack->fsr_set);
        if (res != VK_SUCCESS)
        {
            ERR("vkAllocateDescriptorSets: %d\n", res);
            return res;
        }

        userDescriptorImageInfo.imageView = hack->fsr_view;

        realDescriptorImageInfo.imageView = hack->swapchain_view;

        descriptorWrites[0].dstSet = hack->fsr_set;
        descriptorWrites[1].dstSet = hack->fsr_set;

        device->funcs.p_vkUpdateDescriptorSets(device->device, 2, descriptorWrites, 0, NULL);
    }

    return VK_SUCCESS;
}

static VkFormat srgb_to_unorm(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8_SRGB: return VK_FORMAT_R8G8B8_UNORM;
        case VK_FORMAT_B8G8R8_SRGB: return VK_FORMAT_B8G8R8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return format;
    }
}

static BOOL is_srgb(VkFormat format)
{
    return format != srgb_to_unorm(format);
}

static VkResult init_compute_state(VkDevice device, struct VkSwapchainKHR_T *swapchain)
{
    VkResult res;
    VkSamplerCreateInfo samplerInfo = {0};
    VkDescriptorPoolSize poolSizes[2] = {{0}, {0}};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    VkDescriptorSetLayoutBinding layoutBindings[2] = {{0}, {0}};
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo = {0};
    VkDeviceSize fsrMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
#if defined(USE_STRUCT_CONVERSION)
    VkMemoryRequirements_host fsrMemReq;
    VkMemoryAllocateInfo_host allocInfo = {0};
    VkPhysicalDeviceMemoryProperties_host memProperties;
    VkImageViewCreateInfo_host viewInfo = {0};
#else
    VkMemoryRequirements fsrMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
#endif
    uint32_t fsr_memory_type = -1, i;

    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = swapchain->fs_hack_filter;
    samplerInfo.minFilter = swapchain->fs_hack_filter;
    samplerInfo.addressModeU = swapchain->fsr ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = swapchain->fsr ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = swapchain->fsr ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    res = device->funcs.p_vkCreateSampler(device->device, &samplerInfo, NULL, &swapchain->sampler);
    if (res != VK_SUCCESS)
    {
        WARN("vkCreateSampler failed, res=%d\n", res);
        return res;
    }

    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = swapchain->n_images;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = swapchain->n_images;

    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = swapchain->n_images;

    if (swapchain->fsr)
    {
        poolSizes[0].descriptorCount *= 2;
        poolSizes[1].descriptorCount *= 2;
        poolInfo.maxSets *= 2;
    }

    res = device->funcs.p_vkCreateDescriptorPool(device->device, &poolInfo, NULL, &swapchain->descriptor_pool);
    if (res != VK_SUCCESS)
    {
        ERR("vkCreateDescriptorPool: %d\n", res);
        goto fail;
    }

    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[0].pImmutableSamplers = NULL;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    layoutBindings[1].pImmutableSamplers = NULL;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 2;
    descriptorLayoutInfo.pBindings = layoutBindings;

    res = device->funcs.p_vkCreateDescriptorSetLayout(device->device, &descriptorLayoutInfo, NULL, &swapchain->descriptor_set_layout);
    if (res != VK_SUCCESS)
    {
        ERR("vkCreateDescriptorSetLayout: %d\n", res);
        goto fail;
    }

    res = create_pipeline(device, swapchain, blit_comp_spv, sizeof(blit_comp_spv), 4 * sizeof(float) /* 2 * vec2 */, &swapchain->blit_pipeline);
    if(res != VK_SUCCESS)
        goto fail;

    if (swapchain->fsr)
    {
        res = create_pipeline(device, swapchain, fsr_easu_comp_spv, sizeof(fsr_easu_comp_spv), 16 * sizeof(uint32_t) /* 4 * uvec4 */, &swapchain->fsr_easu_pipeline);
        if (res != VK_SUCCESS)
            goto fail;
        res = create_pipeline(device, swapchain, fsr_rcas_comp_spv, sizeof(fsr_rcas_comp_spv), 8 * sizeof(uint32_t) /* uvec4 + ivec4 */, &swapchain->fsr_rcas_pipeline);
        if (res != VK_SUCCESS)
            goto fail;

        /* create intermediate fsr images */
        for (i = 0; i < swapchain->n_images; ++i)
        {
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = swapchain->blit_dst.extent.width;
            imageInfo.extent.height = swapchain->blit_dst.extent.height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            res = device->funcs.p_vkCreateImage(device->device, &imageInfo, NULL, &hack->fsr_image);
            if (res != VK_SUCCESS)
            {
                ERR("vkCreateImage failed: %d\n", res);
                goto fail;
            }

            device->funcs.p_vkGetImageMemoryRequirements(device->device, hack->fsr_image, &fsrMemReq);

            offs = fsrMemTotal % fsrMemReq.alignment;
            if(offs)
                fsrMemTotal += fsrMemReq.alignment - offs;

            fsrMemTotal += fsrMemReq.size;
        }

        /* allocate backing memory */
        device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceMemoryProperties(device->phys_dev->phys_dev, &memProperties);

        for (i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            {
                if (fsrMemReq.memoryTypeBits & (1 << i))
                {
                    fsr_memory_type = i;
                    break;
                }
            }
        }

        if (fsr_memory_type == -1)
        {
            ERR("unable to find suitable memory type\n");
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }

        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = fsrMemTotal;
        allocInfo.memoryTypeIndex = fsr_memory_type;

        res = device->funcs.p_vkAllocateMemory(device->device, &allocInfo, NULL, &swapchain->fsr_image_memory);
        if (res != VK_SUCCESS)
        {
            ERR("vkAllocateMemory: %d\n", res);
            goto fail;
        }

        /* bind backing memory and create imageviews */
        fsrMemTotal = 0;
        for (i = 0; i < swapchain->n_images; ++i)
        {
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            device->funcs.p_vkGetImageMemoryRequirements(device->device, hack->fsr_image, &fsrMemReq);

            offs = fsrMemTotal % fsrMemReq.alignment;
            if(offs)
                fsrMemTotal += fsrMemReq.alignment - offs;

            res = device->funcs.p_vkBindImageMemory(device->device, hack->fsr_image, swapchain->fsr_image_memory, fsrMemTotal);
            if(res != VK_SUCCESS)
            {
                ERR("vkBindImageMemory: %d\n", res);
                goto fail;
            }

            fsrMemTotal += fsrMemReq.size;
        }

        /* create imageviews */
        for (i = 0; i < swapchain->n_images; ++i)
        {
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = hack->fsr_image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            res = device->funcs.p_vkCreateImageView(device->device, &viewInfo, NULL, &hack->fsr_view);
            if(res != VK_SUCCESS)
            {
                ERR("vkCreateImageView(blit): %d\n", res);
                goto fail;
            }
        }
    }


    /* create imageviews */
    for(i = 0; i < swapchain->n_images; ++i){
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = hack->swapchain_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = srgb_to_unorm(swapchain->format);
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        res = device->funcs.p_vkCreateImageView(device->device, &viewInfo, NULL, &hack->swapchain_view);
        if(res != VK_SUCCESS){
            ERR("vkCreateImageView(blit): %d\n", res);
            goto fail;
        }

        res = create_descriptor_set(device, swapchain, hack);
        if(res != VK_SUCCESS)
            goto fail;
    }

    return VK_SUCCESS;

fail:
    for(i = 0; i < swapchain->n_images; ++i){
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        device->funcs.p_vkDestroyImageView(device->device, hack->fsr_view, NULL);
        hack->fsr_view = VK_NULL_HANDLE;

        device->funcs.p_vkDestroyImageView(device->device, hack->swapchain_view, NULL);
        hack->swapchain_view = VK_NULL_HANDLE;

        device->funcs.p_vkDestroyImage(device->device, hack->fsr_image, NULL);
        hack->fsr_image = VK_NULL_HANDLE;
    }

    destroy_pipeline(device, &swapchain->blit_pipeline);
    destroy_pipeline(device, &swapchain->fsr_easu_pipeline);
    destroy_pipeline(device, &swapchain->fsr_rcas_pipeline);

    device->funcs.p_vkDestroyDescriptorSetLayout(device->device, swapchain->descriptor_set_layout, NULL);
    swapchain->descriptor_set_layout = VK_NULL_HANDLE;

    device->funcs.p_vkDestroyDescriptorPool(device->device, swapchain->descriptor_pool, NULL);
    swapchain->descriptor_pool = VK_NULL_HANDLE;

    device->funcs.p_vkFreeMemory(device->device, swapchain->fsr_image_memory, NULL);
    swapchain->fsr_image_memory = VK_NULL_HANDLE;

    device->funcs.p_vkDestroySampler(device->device, swapchain->sampler, NULL);
    swapchain->sampler = VK_NULL_HANDLE;

    return res;
}

static void destroy_fs_hack_image(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
    device->funcs.p_vkDestroyImageView(device->device, hack->user_view, NULL);
    device->funcs.p_vkDestroyImageView(device->device, hack->swapchain_view, NULL);
    device->funcs.p_vkDestroyImageView(device->device, hack->fsr_view, NULL);
    device->funcs.p_vkDestroyImage(device->device, hack->user_image, NULL);
    device->funcs.p_vkDestroyImage(device->device, hack->fsr_image, NULL);
    if(hack->cmd)
        device->funcs.p_vkFreeCommandBuffers(device->device,
                swapchain->cmd_pools[hack->cmd_queue_idx],
                    1, &hack->cmd);
    device->funcs.p_vkDestroySemaphore(device->device, hack->blit_finished, NULL);
}

#if defined(USE_STRUCT_CONVERSION)
static VkResult init_fs_hack_images(VkDevice device, struct VkSwapchainKHR_T *swapchain, VkSwapchainCreateInfoKHR_host *createinfo)
#else
static VkResult init_fs_hack_images(VkDevice device, struct VkSwapchainKHR_T *swapchain, VkSwapchainCreateInfoKHR *createinfo)
#endif
{
    VkResult res;
    VkImage *real_images = NULL;
    VkDeviceSize userMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
    VkSemaphoreCreateInfo semaphoreInfo = {0};
#if defined(USE_STRUCT_CONVERSION)
    VkMemoryRequirements_host userMemReq;
    VkMemoryAllocateInfo_host allocInfo = {0};
    VkPhysicalDeviceMemoryProperties_host memProperties;
    VkImageViewCreateInfo_host viewInfo = {0};
#else
    VkMemoryRequirements userMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
#endif
    uint32_t count, i = 0, user_memory_type = -1;

    res = device->funcs.p_vkGetSwapchainImagesKHR(device->device, swapchain->swapchain, &count, NULL);
    if(res != VK_SUCCESS)
    {
        WARN("vkGetSwapchainImagesKHR failed, res=%d\n", res);
        return res;
    }

    real_images = malloc(count * sizeof(VkImage));
    swapchain->cmd_pools = calloc(device->queue_count, sizeof(VkCommandPool));
    swapchain->fs_hack_images = calloc(count, sizeof(struct fs_hack_image));
    if(!real_images || !swapchain->cmd_pools || !swapchain->fs_hack_images)
        goto fail;

    res = device->funcs.p_vkGetSwapchainImagesKHR(device->device, swapchain->swapchain, &count, real_images);
    if(res != VK_SUCCESS)
    {
        WARN("vkGetSwapchainImagesKHR failed, res=%d\n", res);
        goto fail;
    }

    /* create user images */
    for(i = 0; i < count; ++i){
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        hack->swapchain_image = real_images[i];

        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        res = device->funcs.p_vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &hack->blit_finished);
        if(res != VK_SUCCESS)
        {
            WARN("vkCreateSemaphore failed, res=%d\n", res);
            goto fail;
        }

        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = swapchain->user_extent.width;
        imageInfo.extent.height = swapchain->user_extent.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = createinfo->imageArrayLayers;
        imageInfo.format = createinfo->imageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = createinfo->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = createinfo->imageSharingMode;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.queueFamilyIndexCount = createinfo->queueFamilyIndexCount;
        imageInfo.pQueueFamilyIndices = createinfo->pQueueFamilyIndices;

        if (is_srgb(createinfo->imageFormat))
            imageInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

        if (createinfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
            imageInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

        res = device->funcs.p_vkCreateImage(device->device, &imageInfo, NULL, &hack->user_image);
        if(res != VK_SUCCESS){
            ERR("vkCreateImage failed: %d\n", res);
            goto fail;
        }

        device->funcs.p_vkGetImageMemoryRequirements(device->device, hack->user_image, &userMemReq);

        offs = userMemTotal % userMemReq.alignment;
        if(offs)
            userMemTotal += userMemReq.alignment - offs;

        userMemTotal += userMemReq.size;

        swapchain->n_images++;
    }

    /* allocate backing memory */
    device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceMemoryProperties(device->phys_dev->phys_dev, &memProperties);

    for (i = 0; i < memProperties.memoryTypeCount; i++){
        if((memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT){
            if(userMemReq.memoryTypeBits & (1 << i)){
                user_memory_type = i;
                break;
            }
        }
    }

    if(user_memory_type == -1){
        ERR("unable to find suitable memory type\n");
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = userMemTotal;
    allocInfo.memoryTypeIndex = user_memory_type;

    res = device->funcs.p_vkAllocateMemory(device->device, &allocInfo, NULL, &swapchain->user_image_memory);
    if(res != VK_SUCCESS){
        ERR("vkAllocateMemory: %d\n", res);
        goto fail;
    }

    /* bind backing memory and create imageviews */
    userMemTotal = 0;
    for(i = 0; i < count; ++i){
        device->funcs.p_vkGetImageMemoryRequirements(device->device, swapchain->fs_hack_images[i].user_image, &userMemReq);

        offs = userMemTotal % userMemReq.alignment;
        if(offs)
            userMemTotal += userMemReq.alignment - offs;

        res = device->funcs.p_vkBindImageMemory(device->device, swapchain->fs_hack_images[i].user_image, swapchain->user_image_memory, userMemTotal);
        if(res != VK_SUCCESS){
            ERR("vkBindImageMemory: %d\n", res);
            goto fail;
        }

        userMemTotal += userMemReq.size;

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain->fs_hack_images[i].user_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = srgb_to_unorm(createinfo->imageFormat);
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        res = device->funcs.p_vkCreateImageView(device->device, &viewInfo, NULL, &swapchain->fs_hack_images[i].user_view);
        if(res != VK_SUCCESS){
            ERR("vkCreateImageView(user): %d\n", res);
            goto fail;
        }
    }

    free(real_images);

    return VK_SUCCESS;

fail:
    for(i = 0; i < swapchain->n_images; ++i)
        destroy_fs_hack_image(device, swapchain, &swapchain->fs_hack_images[i]);
    free(real_images);
    free(swapchain->cmd_pools);
    free(swapchain->fs_hack_images);
    return res;
}

#if defined(USE_STRUCT_CONVERSION)
static inline void convert_VkSwapchainCreateInfoKHR_win_to_host(const VkSwapchainCreateInfoKHR *in, VkSwapchainCreateInfoKHR_host *out)
#else
static inline void convert_VkSwapchainCreateInfoKHR_win_to_host(const VkSwapchainCreateInfoKHR *in, VkSwapchainCreateInfoKHR *out)
#endif
{
    if (!in) return;

    out->sType = in->sType;
    out->pNext = in->pNext;
    out->flags = in->flags;
    out->surface = wine_surface_from_handle(in->surface)->driver_surface;
    out->minImageCount = in->minImageCount;
    out->imageFormat = in->imageFormat;
    out->imageColorSpace = in->imageColorSpace;
    out->imageExtent = in->imageExtent;
    out->imageArrayLayers = in->imageArrayLayers;
    out->imageUsage = in->imageUsage;
    out->imageSharingMode = in->imageSharingMode;
    out->queueFamilyIndexCount = in->queueFamilyIndexCount;
    out->pQueueFamilyIndices = in->pQueueFamilyIndices;
    out->preTransform = in->preTransform;
    out->compositeAlpha = in->compositeAlpha;
    out->presentMode = in->presentMode;
    out->clipped = in->clipped;
    out->oldSwapchain = in->oldSwapchain;
}

NTSTATUS wine_vkCreateSwapchainKHR(void *args)
{
    struct vkCreateSwapchainKHR_params *params = args;
    VkDevice device = params->device;
    const VkSwapchainCreateInfoKHR *create_info = params->pCreateInfo;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    VkSwapchainKHR *swapchain = params->pSwapchain;
#if defined(USE_STRUCT_CONVERSION)
    VkSwapchainCreateInfoKHR_host native_info;
#else
    VkSwapchainCreateInfoKHR native_info;
#endif
    VkResult result;
    VkExtent2D user_sz;
    struct VkSwapchainKHR_T *object;

    TRACE("%p, %p, %p, %p\n", device, create_info, allocator, swapchain);

    if (!(object = calloc(1, sizeof(*object))))
    {
        ERR("Failed to allocate memory for swapchain\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    convert_VkSwapchainCreateInfoKHR_win_to_host(create_info, &native_info);

    if(native_info.oldSwapchain)
        native_info.oldSwapchain = ((struct VkSwapchainKHR_T *)(UINT_PTR)native_info.oldSwapchain)->swapchain;

    if(vk_funcs->query_fs_hack &&
            vk_funcs->query_fs_hack(native_info.surface, &object->real_extent, &user_sz, &object->blit_dst, &object->fs_hack_filter, &object->fsr, &object->sharpness) &&
            native_info.imageExtent.width == user_sz.width &&
            native_info.imageExtent.height == user_sz.height)
    {
        uint32_t count;
        VkSurfaceCapabilitiesKHR caps = {0};

        device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceQueueFamilyProperties(device->phys_dev->phys_dev, &count, NULL);

        device->queue_props = malloc(sizeof(VkQueueFamilyProperties) * count);

        device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceQueueFamilyProperties(device->phys_dev->phys_dev, &count, device->queue_props);

        result = device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->phys_dev->phys_dev, native_info.surface, &caps);
        if(result != VK_SUCCESS)
        {
            TRACE("vkGetPhysicalDeviceSurfaceCapabilities failed, res=%d\n", result);
            free(object);
            return result;
        }

        object->surface_usage = caps.supportedUsageFlags;
        TRACE("surface usage flags: 0x%x\n", object->surface_usage);

        native_info.imageExtent = object->real_extent;
        native_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT; /* XXX: check if supported by surface */

        object->format = native_info.imageFormat;

        if (object->fsr)
            native_info.imageFormat = srgb_to_unorm(native_info.imageFormat);

        if(native_info.imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
                native_info.imageFormat != VK_FORMAT_B8G8R8A8_SRGB){
            FIXME("swapchain image format is not BGRA8 UNORM/SRGB. Things may go badly. %d\n", native_info.imageFormat);
        }

        object->fs_hack_enabled = TRUE;
    }

    result = device->funcs.p_vkCreateSwapchainKHR(device->device, &native_info, NULL, &object->swapchain);
    if(result != VK_SUCCESS)
    {
        TRACE("vkCreateSwapchainKHR failed, res=%d\n", result);
        free(object);
        return result;
    }

    WINE_VK_ADD_NON_DISPATCHABLE_MAPPING(device->phys_dev->instance, object, object->swapchain);

    if(object->fs_hack_enabled){
        object->user_extent = create_info->imageExtent;

        result = init_fs_hack_images(device, object, &native_info);
        if(result != VK_SUCCESS){
            ERR("creating fs hack images failed: %d\n", result);
            device->funcs.p_vkDestroySwapchainKHR(device->device, object->swapchain, NULL);
            WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, object);
            free(object);
            return result;
        }

        result = init_compute_state(device, object);
        if(result != VK_SUCCESS){
            ERR("creating blit images failed: %d\n", result);
            device->funcs.p_vkDestroySwapchainKHR(device->device, object->swapchain, NULL);
            WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, object);
            free(object);
            return result;
        }
    }

    *swapchain = (uint64_t)(UINT_PTR)object;

    return result;
}

NTSTATUS wine_vkCreateWin32SurfaceKHR(void *args)
{
    struct vkCreateWin32SurfaceKHR_params *params = args;
    VkInstance instance = params->instance;
    const VkWin32SurfaceCreateInfoKHR *createInfo = params->pCreateInfo;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    VkSurfaceKHR *surface = params->pSurface;
    struct wine_surface *object;
    VkResult res;

    TRACE("%p, %p, %p, %p\n", instance, createInfo, allocator, surface);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    object = calloc(1, sizeof(*object));

    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = instance->funcs.p_vkCreateWin32SurfaceKHR(instance->instance, createInfo, NULL, &object->driver_surface);

    if (res != VK_SUCCESS)
    {
        free(object);
        return res;
    }

    object->surface = vk_funcs->p_wine_get_native_surface(object->driver_surface);

    WINE_VK_ADD_NON_DISPATCHABLE_MAPPING(instance, object, object->surface);

    *surface = wine_surface_to_handle(object);

    return VK_SUCCESS;
}

NTSTATUS wine_vkDestroySurfaceKHR(void *args)
{
    struct vkDestroySurfaceKHR_params *params = args;
    VkInstance instance = params->instance;
    VkSurfaceKHR surface = params->surface;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    struct wine_surface *object = wine_surface_from_handle(surface);

    TRACE("%p, 0x%s, %p\n", instance, wine_dbgstr_longlong(surface), allocator);

    if (!object)
        return STATUS_SUCCESS;

    instance->funcs.p_vkDestroySurfaceKHR(instance->instance, object->driver_surface, NULL);

    WINE_VK_REMOVE_HANDLE_MAPPING(instance, object);
    free(object);
    return STATUS_SUCCESS;
}

static inline void adjust_max_image_count(VkPhysicalDevice phys_dev, VkSurfaceCapabilitiesKHR* capabilities)
{
    /* Many Windows games, for example Strange Brigade, No Man's Sky, Path of Exile
     * and World War Z, do not expect that maxImageCount can be set to 0.
     * A value of 0 means that there is no limit on the number of images.
     * Nvidia reports 8 on Windows, AMD 16.
     * https://vulkan.gpuinfo.org/displayreport.php?id=9122#surface
     * https://vulkan.gpuinfo.org/displayreport.php?id=9121#surface
     */
    if ((phys_dev->instance->quirks & WINEVULKAN_QUIRK_ADJUST_MAX_IMAGE_COUNT) && !capabilities->maxImageCount)
    {
        capabilities->maxImageCount = max(capabilities->minImageCount, 16);
    }
}

NTSTATUS wine_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(void *args)
{
    struct vkGetPhysicalDeviceSurfaceCapabilitiesKHR_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    VkSurfaceKHR surface = params->surface;
    VkSurfaceCapabilitiesKHR *capabilities = params->pSurfaceCapabilities;
    VkResult res;
    VkExtent2D user_res;

    TRACE("%p, 0x%s, %p\n", phys_dev, wine_dbgstr_longlong(surface), capabilities);

    res = thunk_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface, capabilities);

    if (res == VK_SUCCESS)
        adjust_max_image_count(phys_dev, capabilities);

    if (res == VK_SUCCESS && vk_funcs->query_fs_hack &&
            vk_funcs->query_fs_hack(wine_surface_from_handle(surface)->driver_surface, NULL, &user_res, NULL, NULL, NULL, NULL)){
        capabilities->currentExtent = user_res;
        capabilities->minImageExtent = user_res;
        capabilities->maxImageExtent = user_res;
    }

    return res;
}

NTSTATUS wine_vkGetPhysicalDeviceSurfaceCapabilities2KHR(void *args)
{
    struct vkGetPhysicalDeviceSurfaceCapabilities2KHR_params *params = args;
    VkPhysicalDevice phys_dev = params->physicalDevice;
    const VkPhysicalDeviceSurfaceInfo2KHR *surface_info = params->pSurfaceInfo;
    VkSurfaceCapabilities2KHR *capabilities = params->pSurfaceCapabilities;
    VkResult res;
    VkExtent2D user_res;

    TRACE("%p, %p, %p\n", phys_dev, surface_info, capabilities);

    res = thunk_vkGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, surface_info, capabilities);

    if (res == VK_SUCCESS)
        adjust_max_image_count(phys_dev, &capabilities->surfaceCapabilities);

    if (res == VK_SUCCESS && vk_funcs->query_fs_hack &&
            vk_funcs->query_fs_hack(wine_surface_from_handle(surface_info->surface)->driver_surface, NULL, &user_res, NULL, NULL, NULL, NULL)){
        capabilities->surfaceCapabilities.currentExtent = user_res;
        capabilities->surfaceCapabilities.minImageExtent = user_res;
        capabilities->surfaceCapabilities.maxImageExtent = user_res;
    }

    return res;
}

NTSTATUS wine_vkCreateDebugUtilsMessengerEXT(void *args)
{
    struct vkCreateDebugUtilsMessengerEXT_params *params = args;
    VkInstance instance = params->instance;
    const VkDebugUtilsMessengerCreateInfoEXT *create_info = params->pCreateInfo;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    VkDebugUtilsMessengerEXT *messenger = params->pMessenger;
    VkDebugUtilsMessengerCreateInfoEXT wine_create_info;
    struct wine_debug_utils_messenger *object;
    VkResult res;

    TRACE("%p, %p, %p, %p\n", instance, create_info, allocator, messenger);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (!(object = calloc(1, sizeof(*object))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    object->instance = instance;
    object->user_callback = create_info->pfnUserCallback;
    object->user_data = create_info->pUserData;

    wine_create_info = *create_info;

    wine_create_info.pfnUserCallback = (void *) &debug_utils_callback_conversion;
    wine_create_info.pUserData = object;

    res = instance->funcs.p_vkCreateDebugUtilsMessengerEXT(instance->instance, &wine_create_info, NULL,  &object->debug_messenger);

    if (res != VK_SUCCESS)
    {
        free(object);
        return res;
    }

    WINE_VK_ADD_NON_DISPATCHABLE_MAPPING(instance, object, object->debug_messenger);
    *messenger = wine_debug_utils_messenger_to_handle(object);

    return VK_SUCCESS;
}

NTSTATUS wine_vkDestroyDebugUtilsMessengerEXT(void *args)
{
    struct vkDestroyDebugUtilsMessengerEXT_params *params = args;
    VkInstance instance = params->instance;
    VkDebugUtilsMessengerEXT messenger = params->messenger;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    struct wine_debug_utils_messenger *object;

    TRACE("%p, 0x%s, %p\n", instance, wine_dbgstr_longlong(messenger), allocator);

    object = wine_debug_utils_messenger_from_handle(messenger);

    if (!object)
        return STATUS_SUCCESS;

    instance->funcs.p_vkDestroyDebugUtilsMessengerEXT(instance->instance, object->debug_messenger, NULL);
    WINE_VK_REMOVE_HANDLE_MAPPING(instance, object);

    free(object);
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkCreateDebugReportCallbackEXT(void *args)
{
    struct vkCreateDebugReportCallbackEXT_params *params = args;
    VkInstance instance = params->instance;
    const VkDebugReportCallbackCreateInfoEXT *create_info = params->pCreateInfo;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    VkDebugReportCallbackEXT *callback = params->pCallback;
    VkDebugReportCallbackCreateInfoEXT wine_create_info;
    struct wine_debug_report_callback *object;
    VkResult res;

    TRACE("%p, %p, %p, %p\n", instance, create_info, allocator, callback);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (!(object = calloc(1, sizeof(*object))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    object->instance = instance;
    object->user_callback = create_info->pfnCallback;
    object->user_data = create_info->pUserData;

    wine_create_info = *create_info;

    wine_create_info.pfnCallback = (void *) debug_report_callback_conversion;
    wine_create_info.pUserData = object;

    res = instance->funcs.p_vkCreateDebugReportCallbackEXT(instance->instance, &wine_create_info, NULL, &object->debug_callback);

    if (res != VK_SUCCESS)
    {
        free(object);
        return res;
    }

    WINE_VK_ADD_NON_DISPATCHABLE_MAPPING(instance, object, object->debug_callback);
    *callback = wine_debug_report_callback_to_handle(object);

    return VK_SUCCESS;
}

NTSTATUS wine_vkDestroyDebugReportCallbackEXT(void *args)
{
    struct vkDestroyDebugReportCallbackEXT_params *params = args;
    VkInstance instance = params->instance;
    VkDebugReportCallbackEXT callback = params->callback;
    const VkAllocationCallbacks *allocator = params->pAllocator;
    struct wine_debug_report_callback *object;

    TRACE("%p, 0x%s, %p\n", instance, wine_dbgstr_longlong(callback), allocator);

    object = wine_debug_report_callback_from_handle(callback);

    if (!object)
        return STATUS_SUCCESS;

    instance->funcs.p_vkDestroyDebugReportCallbackEXT(instance->instance, object->debug_callback, NULL);

    WINE_VK_REMOVE_HANDLE_MAPPING(instance, object);

    free(object);
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkAcquireNextImage2KHR(void *args)
{
    struct vkAcquireNextImage2KHR_params *params = args;
    VkDevice device = params->device;
    const VkAcquireNextImageInfoKHR *pAcquireInfo = params->pAcquireInfo;
    uint32_t *pImageIndex = params->pImageIndex;
#if defined(USE_STRUCT_CONVERSION)
    VkAcquireNextImageInfoKHR_host image_info_host = {0};
#else
    VkAcquireNextImageInfoKHR image_info_host = {0};
#endif
    struct VkSwapchainKHR_T *object = (struct VkSwapchainKHR_T *)(UINT_PTR)pAcquireInfo->swapchain;
    TRACE("%p, %p, %p\n", device, pAcquireInfo, pImageIndex);

    image_info_host.sType = pAcquireInfo->sType;
    image_info_host.pNext = pAcquireInfo->pNext;
    image_info_host.swapchain = object->swapchain;
    image_info_host.timeout = pAcquireInfo->timeout;
    image_info_host.semaphore = pAcquireInfo->semaphore;
    image_info_host.fence = pAcquireInfo->fence;
    image_info_host.deviceMask = pAcquireInfo->deviceMask;

    return device->funcs.p_vkAcquireNextImage2KHR(device->device, &image_info_host, pImageIndex);
}

NTSTATUS wine_vkDestroySwapchainKHR(void *args)
{
    struct vkDestroySwapchainKHR_params *params = args;
    VkDevice device = params->device;
    VkSwapchainKHR swapchain = params->swapchain;
    const VkAllocationCallbacks *pAllocator = params->pAllocator;
    struct VkSwapchainKHR_T *object = (struct VkSwapchainKHR_T *)(UINT_PTR)swapchain;
    uint32_t i;

    TRACE("%p, 0x%s, %p\n", device, wine_dbgstr_longlong(swapchain), pAllocator);

    if(!object)
        return STATUS_SUCCESS;

    if(object->fs_hack_enabled){
        for(i = 0; i < object->n_images; ++i)
            destroy_fs_hack_image(device, object, &object->fs_hack_images[i]);

        for(i = 0; i < device->queue_count; ++i)
            if(object->cmd_pools[i])
                device->funcs.p_vkDestroyCommandPool(device->device, object->cmd_pools[i], NULL);

        destroy_pipeline(device, &object->blit_pipeline);
        destroy_pipeline(device, &object->fsr_easu_pipeline);
        destroy_pipeline(device, &object->fsr_rcas_pipeline);
        device->funcs.p_vkDestroyDescriptorSetLayout(device->device, object->descriptor_set_layout, NULL);
        device->funcs.p_vkDestroyDescriptorPool(device->device, object->descriptor_pool, NULL);
        device->funcs.p_vkDestroySampler(device->device, object->sampler, NULL);
        device->funcs.p_vkFreeMemory(device->device, object->user_image_memory, NULL);
        device->funcs.p_vkFreeMemory(device->device, object->fsr_image_memory, NULL);
        free(object->cmd_pools);
        free(object->fs_hack_images);
    }

    device->funcs.p_vkDestroySwapchainKHR(device->device, object->swapchain, NULL);

    WINE_VK_REMOVE_HANDLE_MAPPING(device->phys_dev->instance, object);
    free(object);
    return STATUS_SUCCESS;
}

NTSTATUS wine_vkGetSwapchainImagesKHR(void *args)
{
    struct vkGetSwapchainImagesKHR_params *params = args;
    VkDevice device = params->device;
    VkSwapchainKHR swapchain = params->swapchain;
    uint32_t *pSwapchainImageCount = params->pSwapchainImageCount;
    VkImage *pSwapchainImages = params->pSwapchainImages;
    struct VkSwapchainKHR_T *object = (struct VkSwapchainKHR_T *)(UINT_PTR)swapchain;
    uint32_t i;

    TRACE("%p, 0x%s, %p, %p\n", device, wine_dbgstr_longlong(swapchain), pSwapchainImageCount, pSwapchainImages);

    if(pSwapchainImages && object->fs_hack_enabled){
        if(*pSwapchainImageCount > object->n_images)
            *pSwapchainImageCount = object->n_images;
        for(i = 0; i < *pSwapchainImageCount ; ++i)
            pSwapchainImages[i] = object->fs_hack_images[i].user_image;
        return *pSwapchainImageCount == object->n_images ? VK_SUCCESS : VK_INCOMPLETE;
    }

    return device->funcs.p_vkGetSwapchainImagesKHR(device->device, object->swapchain, pSwapchainImageCount, pSwapchainImages);
}

static VkCommandBuffer create_hack_cmd(VkQueue queue, struct VkSwapchainKHR_T *swapchain, uint32_t queue_idx)
{
#if defined(USE_STRUCT_CONVERSION)
    VkCommandBufferAllocateInfo_host allocInfo = {0};
#else
    VkCommandBufferAllocateInfo allocInfo = {0};
#endif
    VkCommandBuffer cmd;
    VkResult result;

    if(!swapchain->cmd_pools[queue_idx]){
        VkCommandPoolCreateInfo poolInfo = {0};

        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queue_idx;

        result = queue->device->funcs.p_vkCreateCommandPool(queue->device->device, &poolInfo, NULL, &swapchain->cmd_pools[queue_idx]);
        if(result != VK_SUCCESS){
            ERR("vkCreateCommandPool failed, res=%d\n", result);
            return NULL;
        }
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = swapchain->cmd_pools[queue_idx];
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    result = queue->device->funcs.p_vkAllocateCommandBuffers(queue->device->device, &allocInfo, &cmd);
    if(result != VK_SUCCESS){
        ERR("vkAllocateCommandBuffers failed, res=%d\n", result);
        return NULL;
    }

    return cmd;
}

static void bind_pipeline(VkDevice device, VkCommandBuffer cmd, struct fs_comp_pipeline *pipeline, VkDescriptorSet set, void *push_data)
{
    device->funcs.p_vkCmdBindPipeline(cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

    device->funcs.p_vkCmdBindDescriptorSets(cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline_layout,
            0, 1, &set, 0, NULL);

    device->funcs.p_vkCmdPushConstants(cmd,
            pipeline->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, pipeline->push_size, push_data);
}

#if defined(USE_STRUCT_CONVERSION)
static void init_barrier(VkImageMemoryBarrier_host *barrier)
#else
static void init_barrier(VkImageMemoryBarrier *barrier)
#endif
{
    barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier->pNext = NULL;
    barrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier->subresourceRange.baseMipLevel = 0;
    barrier->subresourceRange.levelCount = 1;
    barrier->subresourceRange.baseArrayLayer = 0;
    barrier->subresourceRange.layerCount = 1;
}


static VkResult record_compute_cmd(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
#if defined(USE_STRUCT_CONVERSION)
    VkImageMemoryBarrier_host barriers[2] = {{0}};
    VkCommandBufferBeginInfo_host beginInfo = {0};
#else
    VkImageMemoryBarrier barriers[2] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
#endif
    float constants[4];
    VkResult result;

    TRACE("recording compute command\n");

    init_barrier(&barriers[0]);
    init_barrier(&barriers[1]);

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->funcs.p_vkBeginCommandBuffer(hack->cmd, &beginInfo);

    /* for the cs we run... */
    /* transition user image from PRESENT_SRC to SHADER_READ */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* storage image... */
    /* transition swapchain image from whatever to GENERAL */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].image = hack->swapchain_image;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );

    /* perform blit shader */

    /* vec2: blit dst offset in real coords */
    constants[0] = swapchain->blit_dst.offset.x;
    constants[1] = swapchain->blit_dst.offset.y;
    /* vec2: blit dst extents in real coords */
    constants[2] = swapchain->blit_dst.extent.width;
    constants[3] = swapchain->blit_dst.extent.height;

    bind_pipeline(device, hack->cmd, &swapchain->blit_pipeline, hack->descriptor_set, constants);

    /* local sizes in shader are 8 */
    device->funcs.p_vkCmdDispatch(hack->cmd, ceil(swapchain->real_extent.width / 8.),
            ceil(swapchain->real_extent.height / 8.), 1);

    /* transition user image from SHADER_READ back to PRESENT_SRC */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = 0;

    /* transition swapchain image from GENERAL to PRESENT_SRC */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].image = hack->swapchain_image;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );


    result = device->funcs.p_vkEndCommandBuffer(hack->cmd);
    if(result != VK_SUCCESS){
        ERR("vkEndCommandBuffer: %d\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult record_graphics_cmd(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
    VkResult result;
    VkImageBlit blitregion = {0};
    VkImageSubresourceRange range = {0};
    VkClearColorValue black = {{0.f, 0.f, 0.f}};
#if defined(USE_STRUCT_CONVERSION)
    VkImageMemoryBarrier_host barriers[2] = {{0}};
    VkCommandBufferBeginInfo_host beginInfo = {0};
#else
    VkImageMemoryBarrier barriers[2] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
#endif

    TRACE("recording graphics command\n");

    init_barrier(&barriers[0]);
    init_barrier(&barriers[1]);

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->funcs.p_vkBeginCommandBuffer(hack->cmd, &beginInfo);

    /* transition real image from whatever to TRANSFER_DST_OPTIMAL */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].image = hack->swapchain_image;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = 0;

    /* transition user image from PRESENT_SRC to TRANSFER_SRC_OPTIMAL */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[1].image = hack->user_image;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );

    /* clear the image */
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    device->funcs.p_vkCmdClearColorImage(
            hack->cmd, hack->swapchain_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &black, 1, &range);

    /* perform blit */
    blitregion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitregion.srcSubresource.layerCount = 1;
    blitregion.srcOffsets[0].x = 0;
    blitregion.srcOffsets[0].y = 0;
    blitregion.srcOffsets[0].z = 0;
    blitregion.srcOffsets[1].x = swapchain->user_extent.width;
    blitregion.srcOffsets[1].y = swapchain->user_extent.height;
    blitregion.srcOffsets[1].z = 1;
    blitregion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitregion.dstSubresource.layerCount = 1;
    blitregion.dstOffsets[0].x = swapchain->blit_dst.offset.x;
    blitregion.dstOffsets[0].y = swapchain->blit_dst.offset.y;
    blitregion.dstOffsets[0].z = 0;
    blitregion.dstOffsets[1].x = swapchain->blit_dst.offset.x + swapchain->blit_dst.extent.width;
    blitregion.dstOffsets[1].y = swapchain->blit_dst.offset.y + swapchain->blit_dst.extent.height;
    blitregion.dstOffsets[1].z = 1;

    device->funcs.p_vkCmdBlitImage(hack->cmd,
            hack->user_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            hack->swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blitregion, swapchain->fs_hack_filter);

    /* transition real image from TRANSFER_DST to PRESENT_SRC */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].image = hack->swapchain_image;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = 0;

    /* transition user image from TRANSFER_SRC_OPTIMAL to back to PRESENT_SRC */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].image = hack->user_image;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );

    result = device->funcs.p_vkEndCommandBuffer(hack->cmd);
    if(result != VK_SUCCESS){
        ERR("vkEndCommandBuffer: %d\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult record_fsr_cmd(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
#if defined(USE_STRUCT_CONVERSION)
    VkImageMemoryBarrier_host barriers[3] = {{0}};
    VkCommandBufferBeginInfo_host beginInfo = {0};
#else
    VkImageMemoryBarrier barriers[3] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
#endif
    union
    {
        uint32_t uint[16];
        float    fp[16];
    } c;
    VkResult result;

    TRACE("recording compute command\n");

    init_barrier(&barriers[0]);
    init_barrier(&barriers[1]);
    init_barrier(&barriers[2]);

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->funcs.p_vkBeginCommandBuffer(hack->cmd, &beginInfo);

    /* 1st pass (easu) */
    /* transition user image from PRESENT_SRC to SHADER_READ */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* storage image... */
    /* transition fsr image from whatever to GENERAL */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].image = hack->swapchain_image;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );

    /* perform easu shader */

    c.fp[0] = swapchain->user_extent.width * (1.0f / swapchain->blit_dst.extent.width);
    c.fp[1] = swapchain->user_extent.height * (1.0f / swapchain->blit_dst.extent.height);
    c.fp[2] = 0.5f * c.fp[0] - 0.5f;
    c.fp[3] = 0.5f * c.fp[1] - 0.5f;
    // Viewport pixel position to normalized image space.
    // This is used to get upper-left of 'F' tap.
    c.fp[4] = 1.0f / swapchain->user_extent.width;
    c.fp[5] = 1.0f / swapchain->user_extent.height;
    // Centers of gather4, first offset from upper-left of 'F'.
    //      +---+---+
    //      |   |   |
    //      +--(0)--+
    //      | b | c |
    //  +---F---+---+---+
    //  | e | f | g | h |
    //  +--(1)--+--(2)--+
    //  | i | j | k | l |
    //  +---+---+---+---+
    //      | n | o |
    //      +--(3)--+
    //      |   |   |
    //      +---+---+
    c.fp[6] =  1.0f * c.fp[4];
    c.fp[7] = -1.0f * c.fp[5];
    // These are from (0) instead of 'F'.
    c.fp[8] = -1.0f * c.fp[4];
    c.fp[9] =  2.0f * c.fp[5];
    c.fp[10] =  1.0f * c.fp[4];
    c.fp[11] =  2.0f * c.fp[5];
    c.fp[12] =  0.0f * c.fp[4];
    c.fp[13] =  4.0f * c.fp[5];
    c.uint[14] = swapchain->blit_dst.extent.width;
    c.uint[15] = swapchain->blit_dst.extent.height;

    bind_pipeline(device, hack->cmd, &swapchain->fsr_easu_pipeline, hack->descriptor_set, c.uint);

    /* local sizes in shader are 8 */
    device->funcs.p_vkCmdDispatch(hack->cmd, ceil(swapchain->blit_dst.extent.width / 8.),
            ceil(swapchain->blit_dst.extent.height / 8.), 1);

    /* transition user image from SHADER_READ back to PRESENT_SRC */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = 0;

    /* transition fsr image from GENERAL to SHADER_READ */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].image = hack->swapchain_image;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* transition swapchain image from whatever to GENERAL */
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].image = hack->swapchain_image;
    barriers[2].srcAccessMask = 0;
    barriers[2].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, NULL,
            0, NULL,
            3, barriers
    );

    /* 2nd pass (rcas) */

    c.fp[0] = exp2f(-swapchain->sharpness);
    c.uint[2] = swapchain->blit_dst.extent.width;
    c.uint[3] = swapchain->blit_dst.extent.height;
    c.uint[4] = swapchain->blit_dst.offset.x;
    c.uint[5] = swapchain->blit_dst.offset.y;
    c.uint[6] = swapchain->blit_dst.offset.x + swapchain->blit_dst.extent.width;
    c.uint[7] = swapchain->blit_dst.offset.y + swapchain->blit_dst.extent.height;

    bind_pipeline(device, hack->cmd, &swapchain->fsr_rcas_pipeline, hack->fsr_set, c.uint);

    /* local sizes in shader are 8 */
    device->funcs.p_vkCmdDispatch(hack->cmd, ceil(swapchain->real_extent.width / 8.),
            ceil(swapchain->real_extent.height / 8.), 1);

    /* transition swapchain image from GENERAL to PRESENT_SRC */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = hack->swapchain_image;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, NULL,
            0, NULL,
            1, barriers
    );

    result = device->funcs.p_vkEndCommandBuffer(hack->cmd);
    if (result != VK_SUCCESS)
    {
        ERR("vkEndCommandBuffer: %d\n", result);
        return result;
    }

    return VK_SUCCESS;
}

NTSTATUS wine_vkQueuePresentKHR(void *args)
{
    struct vkQueuePresentKHR_params *params = args;
    VkQueue queue = params->queue;
    const VkPresentInfoKHR *pPresentInfo = params->pPresentInfo;
    VkResult res;
    VkPresentInfoKHR our_presentInfo;
    VkSwapchainKHR *arr;
    VkCommandBuffer *blit_cmds = NULL;
    VkSubmitInfo submitInfo = {0};
    VkSemaphore blit_sema;
    struct VkSwapchainKHR_T *swapchain;
    uint32_t i, n_hacks = 0;
    uint32_t queue_idx;

    TRACE("%p, %p\n", queue, pPresentInfo);

    our_presentInfo = *pPresentInfo;

    for(i = 0; i < our_presentInfo.swapchainCount; ++i){
        swapchain = (struct VkSwapchainKHR_T *)(UINT_PTR)our_presentInfo.pSwapchains[i];

        if(swapchain->fs_hack_enabled){
            struct fs_hack_image *hack = &swapchain->fs_hack_images[our_presentInfo.pImageIndices[i]];

            if(!blit_cmds){
                queue_idx = queue->family_index;
                blit_cmds = malloc(our_presentInfo.swapchainCount * sizeof(VkCommandBuffer));
                blit_sema = hack->blit_finished;
            }

            if(!hack->cmd || hack->cmd_queue_idx != queue_idx){
                if(hack->cmd)
                    queue->device->funcs.p_vkFreeCommandBuffers(queue->device->device,
                            swapchain->cmd_pools[hack->cmd_queue_idx],
                            1, &hack->cmd);

                hack->cmd_queue_idx = queue_idx;
                hack->cmd = create_hack_cmd(queue, swapchain, queue_idx);

                if(!hack->cmd){
                    free(blit_cmds);
                    return VK_ERROR_DEVICE_LOST;
                }

                if (swapchain->fsr)
                {
                    if(queue->device->queue_props[queue_idx].queueFlags & VK_QUEUE_COMPUTE_BIT)
                        res = record_fsr_cmd(queue->device, swapchain, hack);
                    else
                    {
                        ERR("Present queue is not a compute queue!\n");
                        res = VK_ERROR_DEVICE_LOST;
                    }
                }
                else
                {
                    if(queue->device->queue_props[queue_idx].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                        res = record_graphics_cmd(queue->device, swapchain, hack);
                    if(queue->device->queue_props[queue_idx].queueFlags & VK_QUEUE_COMPUTE_BIT && !is_srgb(swapchain->format))
                        res = record_compute_cmd(queue->device, swapchain, hack);
                    else
                    {
                        ERR("Present queue is neither graphics nor compute queue with unorm format!\n");
                        res = VK_ERROR_DEVICE_LOST;
                    }
                }

                if(res != VK_SUCCESS){
                    queue->device->funcs.p_vkFreeCommandBuffers(queue->device->device,
                            swapchain->cmd_pools[hack->cmd_queue_idx],
                            1, &hack->cmd);
                    hack->cmd = NULL;
                    free(blit_cmds);
                    return res;
                }
            }

            blit_cmds[n_hacks] = hack->cmd;

            ++n_hacks;
        }
    }

    if(n_hacks > 0){
        VkPipelineStageFlags waitStage, *waitStages, *waitStages_arr = NULL;

        if(pPresentInfo->waitSemaphoreCount > 1){
            waitStages_arr = malloc(sizeof(VkPipelineStageFlags) * pPresentInfo->waitSemaphoreCount);
            for(i = 0; i < pPresentInfo->waitSemaphoreCount; ++i)
                waitStages_arr[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitStages = waitStages_arr;
        }else{
            waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitStages = &waitStage;
        }

        /* blit user image to real image */
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = n_hacks;
        submitInfo.pCommandBuffers = blit_cmds;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &blit_sema;

        res = queue->device->funcs.p_vkQueueSubmit(queue->queue, 1, &submitInfo, VK_NULL_HANDLE);
        if(res != VK_SUCCESS)
            ERR("vkQueueSubmit: %d\n", res);

        free(waitStages_arr);
        free(blit_cmds);

        our_presentInfo.waitSemaphoreCount = 1;
        our_presentInfo.pWaitSemaphores = &blit_sema;
    }

    arr = malloc(our_presentInfo.swapchainCount * sizeof(VkSwapchainKHR));
    if(!arr){
        ERR("Failed to allocate memory for swapchain array\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for(i = 0; i < our_presentInfo.swapchainCount; ++i)
        arr[i] = ((struct VkSwapchainKHR_T *)(UINT_PTR)our_presentInfo.pSwapchains[i])->swapchain;

    our_presentInfo.pSwapchains = arr;

    res = queue->device->funcs.p_vkQueuePresentKHR(queue->queue, &our_presentInfo);

    free(arr);

    return res;

}

BOOL WINAPI wine_vk_is_available_instance_function(VkInstance instance, const char *name)
{
    return !!vk_funcs->p_vkGetInstanceProcAddr(instance->instance, name);
}

BOOL WINAPI wine_vk_is_available_device_function(VkDevice device, const char *name)
{
    return !!vk_funcs->p_vkGetDeviceProcAddr(device->device, name);
}
