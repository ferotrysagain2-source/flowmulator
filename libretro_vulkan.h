#pragma once

#include "libretro.h"
#include "vulkan_minimal.h"

#define RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION 5
#define RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION 2

struct retro_vulkan_image {
  VkImageView image_view;
  VkImageLayout image_layout;
  VkImageViewCreateInfo create_info;
};

using retro_vulkan_set_image_t =
    void (*)(void *, const retro_vulkan_image *, uint32_t, const VkSemaphore *,
             uint32_t);
using retro_vulkan_get_sync_index_t = uint32_t (*)(void *);
using retro_vulkan_get_sync_index_mask_t = uint32_t (*)(void *);
using retro_vulkan_set_command_buffers_t =
    void (*)(void *, uint32_t, const VkCommandBuffer *);
using retro_vulkan_wait_sync_index_t = void (*)(void *);
using retro_vulkan_lock_queue_t = void (*)(void *);
using retro_vulkan_unlock_queue_t = void (*)(void *);
using retro_vulkan_set_signal_semaphore_t = void (*)(void *, VkSemaphore);
using retro_vulkan_get_application_info_t = const VkApplicationInfo *(*)();

struct retro_vulkan_context {
  VkPhysicalDevice gpu;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family_index;
  VkQueue presentation_queue;
  uint32_t presentation_queue_family_index;
};

using retro_vulkan_create_device_t =
    bool (*)(retro_vulkan_context *, VkInstance, VkPhysicalDevice, VkSurfaceKHR,
             PFN_vkGetInstanceProcAddr, const char **, unsigned, const char **,
             unsigned, const VkPhysicalDeviceFeatures *);
using retro_vulkan_destroy_device_t = void (*)();
using retro_vulkan_create_instance_wrapper_t =
    VkInstance (*)(void *, const VkInstanceCreateInfo *);
using retro_vulkan_create_instance_t =
    VkInstance (*)(PFN_vkGetInstanceProcAddr, const VkApplicationInfo *,
                   retro_vulkan_create_instance_wrapper_t, void *);
using retro_vulkan_create_device_wrapper_t =
    VkDevice (*)(VkPhysicalDevice, void *, const VkDeviceCreateInfo *);
using retro_vulkan_create_device2_t =
    bool (*)(retro_vulkan_context *, VkInstance, VkPhysicalDevice, VkSurfaceKHR,
             PFN_vkGetInstanceProcAddr, retro_vulkan_create_device_wrapper_t,
             void *);

struct retro_hw_render_context_negotiation_interface_vulkan {
  retro_hw_render_context_negotiation_interface_type interface_type;
  unsigned interface_version;
  retro_vulkan_get_application_info_t get_application_info;
  retro_vulkan_create_device_t create_device;
  retro_vulkan_destroy_device_t destroy_device;
  retro_vulkan_create_instance_t create_instance;
  retro_vulkan_create_device2_t create_device2;
};

struct retro_hw_render_interface_vulkan {
  retro_hw_render_interface_type interface_type;
  unsigned interface_version;
  void *handle;
  VkInstance instance;
  VkPhysicalDevice gpu;
  VkDevice device;
  PFN_vkGetDeviceProcAddr get_device_proc_addr;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
  VkQueue queue;
  unsigned queue_index;
  retro_vulkan_set_image_t set_image;
  retro_vulkan_get_sync_index_t get_sync_index;
  retro_vulkan_get_sync_index_mask_t get_sync_index_mask;
  retro_vulkan_set_command_buffers_t set_command_buffers;
  retro_vulkan_wait_sync_index_t wait_sync_index;
  retro_vulkan_lock_queue_t lock_queue;
  retro_vulkan_unlock_queue_t unlock_queue;
  retro_vulkan_set_signal_semaphore_t set_signal_semaphore;
};
