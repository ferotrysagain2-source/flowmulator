#pragma once

/*
 * This is the small ABI-only portion of vulkan_core.h needed by Flowmulator.
 * The machine has the Vulkan loader and ICD installed, but not the development
 * headers.  These declarations intentionally contain no Vulkan implementation.
 */
#include <cstddef>
#include <cstdint>

#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR

using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;
using VkSampleMask = uint32_t;

#define VK_DEFINE_HANDLE(name) typedef struct name##_T *name;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(name) typedef struct name##_T *name;
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSwapchainKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImageView)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkCommandPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFence)
#undef VK_DEFINE_HANDLE
#undef VK_DEFINE_NON_DISPATCHABLE_HANDLE

constexpr auto VK_NULL_HANDLE = nullptr;
enum VkResult : int32_t {
  VK_SUCCESS = 0,
  VK_NOT_READY = 1,
  VK_TIMEOUT = 2,
  VK_EVENT_SET = 3,
  VK_EVENT_RESET = 4,
  VK_INCOMPLETE = 5,
  VK_ERROR_OUT_OF_HOST_MEMORY = -1,
  VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
  VK_ERROR_INITIALIZATION_FAILED = -3,
  VK_ERROR_DEVICE_LOST = -4,
  VK_ERROR_LAYER_NOT_PRESENT = -6,
  VK_ERROR_EXTENSION_NOT_PRESENT = -7,
  VK_ERROR_FEATURE_NOT_PRESENT = -8,
  VK_ERROR_INCOMPATIBLE_DRIVER = -9,
  VK_ERROR_OUT_OF_DATE_KHR = -1000001004,
  VK_SUBOPTIMAL_KHR = 1000001003,
};

enum VkStructureType : int32_t {
  VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
  VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
  VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
  VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
  VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
  VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
  VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
  VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
  VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
  VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
  VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
  VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
  VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
  VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
  VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR = 1000127001,
};

enum VkPhysicalDeviceType : int32_t {
  VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
  VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
  VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
};

enum VkImageType : int32_t { VK_IMAGE_TYPE_2D = 1 };
enum VkImageViewType : int32_t { VK_IMAGE_VIEW_TYPE_2D = 1 };
enum VkImageTiling : int32_t {
  VK_IMAGE_TILING_OPTIMAL = 0,
  VK_IMAGE_TILING_LINEAR = 1,
};
enum VkImageLayout : int32_t {
  VK_IMAGE_LAYOUT_UNDEFINED = 0,
  VK_IMAGE_LAYOUT_GENERAL = 1,
  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5,
  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002,
};
enum VkFormat : int32_t {
  VK_FORMAT_UNDEFINED = 0,
  VK_FORMAT_B8G8R8A8_UNORM = 44,
  VK_FORMAT_R8G8B8A8_UNORM = 37,
  VK_FORMAT_B8G8R8A8_SRGB = 50,
  VK_FORMAT_R8G8B8A8_SRGB = 43,
};
enum VkColorSpaceKHR : int32_t { VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0 };
enum VkPresentModeKHR : int32_t {
  VK_PRESENT_MODE_IMMEDIATE_KHR = 0,
  VK_PRESENT_MODE_MAILBOX_KHR = 1,
  VK_PRESENT_MODE_FIFO_KHR = 2,
};
enum VkSharingMode : int32_t {
  VK_SHARING_MODE_EXCLUSIVE = 0,
  VK_SHARING_MODE_CONCURRENT = 1,
};
enum VkSampleCountFlagBits : VkFlags { VK_SAMPLE_COUNT_1_BIT = 1 };
enum VkCommandBufferLevel : int32_t { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0 };
enum VkCommandPoolCreateFlagBits : VkFlags {
  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 2,
};
enum VkCommandBufferUsageFlagBits : VkFlags {
  VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 1,
};
enum VkImageCreateFlagBits : VkFlags {
  VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT = 8,
};
enum VkImageUsageFlagBits : VkFlags {
  VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 1,
  VK_IMAGE_USAGE_TRANSFER_DST_BIT = 2,
  VK_IMAGE_USAGE_SAMPLED_BIT = 4,
  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 16,
};
enum VkImageAspectFlagBits : VkFlags { VK_IMAGE_ASPECT_COLOR_BIT = 1 };
enum VkQueueFlagBits : VkFlags {
  VK_QUEUE_GRAPHICS_BIT = 1,
  VK_QUEUE_COMPUTE_BIT = 2,
};
enum VkMemoryPropertyFlagBits : VkFlags {
  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1,
};
enum VkPipelineStageFlagBits : VkFlags {
  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 1,
  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 128,
  VK_PIPELINE_STAGE_TRANSFER_BIT = 4096,
  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 1024,
  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 8192,
};
enum VkAccessFlagBits : VkFlags {
  VK_ACCESS_SHADER_READ_BIT = 32,
  VK_ACCESS_TRANSFER_READ_BIT = 2048,
  VK_ACCESS_TRANSFER_WRITE_BIT = 4096,
  VK_ACCESS_MEMORY_READ_BIT = 32768,
};
enum VkDependencyFlagBits : VkFlags {};
enum VkFenceCreateFlagBits : VkFlags {};

struct VkAllocationCallbacks;
struct VkExtensionProperties {
  char extensionName[256];
  uint32_t specVersion;
};
struct VkLayerProperties {
  char layerName[256];
  char specVersion[4 * sizeof(uint32_t)];
  char implementationVersion[4 * sizeof(uint32_t)];
  char description[256];
};
struct VkApplicationInfo {
  VkStructureType sType;
  const void *pNext;
  const char *pApplicationName;
  uint32_t applicationVersion;
  const char *pEngineName;
  uint32_t engineVersion;
  uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  const VkApplicationInfo *pApplicationInfo;
  uint32_t enabledLayerCount;
  const char *const *ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char *const *ppEnabledExtensionNames;
};
struct VkPhysicalDeviceFeatures {
  VkBool32 robustBufferAccess, fullDrawIndexUint32, imageCubeArray,
      independentBlend, geometryShader, tessellationShader, sampleRateShading,
      dualSrcBlend, logicOp, multiDrawIndirect, drawIndirectFirstInstance,
      depthClamp, depthBiasClamp, fillModeNonSolid, depthBounds, wideLines,
      largePoints, alphaToOne, multiViewport, samplerAnisotropy,
      textureCompressionETC2, textureCompressionASTC_LDR, textureCompressionBC,
      occlusionQueryPrecise, pipelineStatisticsQuery, vertexPipelineStoresAndAtomics,
      fragmentStoresAndAtomics, shaderTessellationAndGeometryPointSize,
      shaderImageGatherExtended, shaderStorageImageExtendedFormats,
      shaderStorageImageMultisample, shaderStorageImageReadWithoutFormat,
      shaderStorageImageWriteWithoutFormat, shaderUniformBufferArrayDynamicIndexing,
      shaderSampledImageArrayDynamicIndexing, shaderStorageBufferArrayDynamicIndexing,
      shaderStorageImageArrayDynamicIndexing, shaderClipDistance, shaderCullDistance,
      shaderFloat64, shaderInt64, shaderInt16, shaderResourceResidency,
      shaderResourceMinLod, sparseBinding, sparseResidencyBuffer,
      sparseResidencyImage2D, sparseResidencyImage3D, sparseResidency2Samples,
      sparseResidency4Samples, sparseResidency8Samples, sparseResidency16Samples,
      sparseResidencyAliased, variableMultisampleRate, inheritedQueries;
};
struct VkDeviceQueueCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  uint32_t queueFamilyIndex;
  uint32_t queueCount;
  const float *pQueuePriorities;
};
struct VkDeviceCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  uint32_t queueCreateInfoCount;
  const VkDeviceQueueCreateInfo *pQueueCreateInfos;
  uint32_t enabledLayerCount;
  const char *const *ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char *const *ppEnabledExtensionNames;
  const VkPhysicalDeviceFeatures *pEnabledFeatures;
};
struct VkQueueFamilyProperties {
  VkFlags queueFlags;
  uint32_t queueCount;
  uint32_t timestampValidBits;
  struct { uint32_t width, height, depth; } minImageTransferGranularity;
};
struct VkSurfaceCapabilitiesKHR {
  uint32_t minImageCount, maxImageCount;
  struct { uint32_t width, height; } currentExtent, minImageExtent, maxImageExtent;
  uint32_t maxImageArrayLayers;
  VkFlags supportedTransforms, currentTransform, supportedCompositeAlpha;
  VkFlags supportedUsageFlags;
};
struct VkSurfaceFormatKHR {
  VkFormat format;
  VkColorSpaceKHR colorSpace;
};
struct VkExtent3D { uint32_t width, height, depth; };
struct VkImageSubresourceRange {
  VkFlags aspectMask;
  uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount;
};
struct VkComponentMapping {
  int32_t r, g, b, a;
};
struct VkImageViewCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  VkImage image;
  VkImageViewType viewType;
  VkFormat format;
  VkComponentMapping components;
  VkImageSubresourceRange subresourceRange;
};
struct VkImageCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  VkImageType imageType;
  VkFormat format;
  VkExtent3D extent;
  uint32_t mipLevels, arrayLayers;
  VkSampleCountFlagBits samples;
  VkImageTiling tiling;
  VkFlags usage, sharingMode;
  uint32_t queueFamilyIndexCount;
  const uint32_t *pQueueFamilyIndices;
  VkImageLayout initialLayout;
};
struct VkSwapchainCreateInfoKHR {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  VkSurfaceKHR surface;
  uint32_t minImageCount;
  VkFormat imageFormat;
  VkColorSpaceKHR imageColorSpace;
  struct { uint32_t width, height; } imageExtent;
  uint32_t imageArrayLayers;
  VkFlags imageUsage;
  VkSharingMode imageSharingMode;
  uint32_t queueFamilyIndexCount;
  const uint32_t *pQueueFamilyIndices;
  VkFlags preTransform, compositeAlpha;
  VkPresentModeKHR presentMode;
  VkBool32 clipped;
  VkSwapchainKHR oldSwapchain;
};
struct VkMemoryRequirements {
  VkDeviceSize size, alignment;
  uint32_t memoryTypeBits;
};
struct VkMemoryAllocateInfo {
  VkStructureType sType;
  const void *pNext;
  VkDeviceSize allocationSize;
  uint32_t memoryTypeIndex;
};
struct VkMemoryDedicatedAllocateInfoKHR {
  VkStructureType sType;
  const void *pNext;
  VkImage image;
  void *buffer;
};
struct VkCommandPoolCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  uint32_t queueFamilyIndex;
};
struct VkCommandBufferAllocateInfo {
  VkStructureType sType;
  const void *pNext;
  VkCommandPool commandPool;
  VkCommandBufferLevel level;
  uint32_t commandBufferCount;
};
struct VkCommandBufferBeginInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  const void *pInheritanceInfo;
};
struct VkSubmitInfo {
  VkStructureType sType;
  const void *pNext;
  uint32_t waitSemaphoreCount;
  const VkSemaphore *pWaitSemaphores;
  const VkFlags *pWaitDstStageMask;
  uint32_t commandBufferCount;
  const VkCommandBuffer *pCommandBuffers;
  uint32_t signalSemaphoreCount;
  const VkSemaphore *pSignalSemaphores;
};
struct VkFenceCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
};
struct VkSemaphoreCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
};
struct VkImageMemoryBarrier {
  VkStructureType sType;
  const void *pNext;
  VkFlags srcAccessMask, dstAccessMask;
  VkImageLayout oldLayout, newLayout;
  uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex;
  VkImage image;
  VkImageSubresourceRange subresourceRange;
};
struct VkImageBlit {
  struct { int32_t x, y, z; } srcOffsets[2];
  struct { VkFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount; } srcSubresource;
  struct { int32_t x, y, z; } dstOffsets[2];
  struct { VkFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount; } dstSubresource;
};
struct VkPresentInfoKHR {
  VkStructureType sType;
  const void *pNext;
  uint32_t waitSemaphoreCount;
  const VkSemaphore *pWaitSemaphores;
  uint32_t swapchainCount;
  const VkSwapchainKHR *pSwapchains;
  const uint32_t *pImageIndices;
  VkResult *pResults;
};
struct VkMemoryType {
  VkFlags propertyFlags;
  uint32_t heapIndex;
};
struct VkMemoryHeap {
  VkDeviceSize size;
  VkFlags flags;
};
struct VkPhysicalDeviceMemoryProperties {
  uint32_t memoryTypeCount;
  VkMemoryType memoryTypes[32];
  uint32_t memoryHeapCount;
  VkMemoryHeap memoryHeaps[16];
};

using PFN_vkVoidFunction = void (*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction (*)(VkInstance, const char *);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction (*)(VkDevice, const char *);
#define VK_PROC(name, ret, args) using PFN_##name = ret (VKAPI_PTR *) args;
VK_PROC(vkCreateInstance, VkResult, (const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *))
VK_PROC(vkDestroyInstance, void, (VkInstance, const VkAllocationCallbacks *))
VK_PROC(vkDestroySurfaceKHR, void, (VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *))
VK_PROC(vkEnumerateInstanceExtensionProperties, VkResult, (const char *, uint32_t *, VkExtensionProperties *))
VK_PROC(vkEnumeratePhysicalDevices, VkResult, (VkInstance, uint32_t *, VkPhysicalDevice *))
VK_PROC(vkGetPhysicalDeviceQueueFamilyProperties, void, (VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *))
VK_PROC(vkGetPhysicalDeviceSurfaceSupportKHR, VkResult, (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *))
VK_PROC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *))
VK_PROC(vkGetPhysicalDeviceSurfaceFormatsKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *))
VK_PROC(vkGetPhysicalDeviceSurfacePresentModesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *))
VK_PROC(vkCreateDevice, VkResult, (VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *))
VK_PROC(vkDestroyDevice, void, (VkDevice, const VkAllocationCallbacks *))
VK_PROC(vkGetDeviceQueue, void, (VkDevice, uint32_t, uint32_t, VkQueue *))
VK_PROC(vkCreateSwapchainKHR, VkResult, (VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *))
VK_PROC(vkDestroySwapchainKHR, void, (VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *))
VK_PROC(vkGetSwapchainImagesKHR, VkResult, (VkDevice, VkSwapchainKHR, uint32_t *, VkImage *))
VK_PROC(vkAcquireNextImageKHR, VkResult, (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *))
VK_PROC(vkQueuePresentKHR, VkResult, (VkQueue, const VkPresentInfoKHR *))
VK_PROC(vkQueueSubmit, VkResult, (VkQueue, uint32_t, const VkSubmitInfo *, VkFence))
VK_PROC(vkQueueWaitIdle, VkResult, (VkQueue))
VK_PROC(vkDeviceWaitIdle, VkResult, (VkDevice))
VK_PROC(vkCreateImage, VkResult, (VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *))
VK_PROC(vkDestroyImage, void, (VkDevice, VkImage, const VkAllocationCallbacks *))
VK_PROC(vkGetImageMemoryRequirements, void, (VkDevice, VkImage, VkMemoryRequirements *))
VK_PROC(vkAllocateMemory, VkResult, (VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *))
VK_PROC(vkFreeMemory, void, (VkDevice, VkDeviceMemory, const VkAllocationCallbacks *))
VK_PROC(vkBindImageMemory, VkResult, (VkDevice, VkImage, VkDeviceMemory, VkDeviceSize))
VK_PROC(vkCreateImageView, VkResult, (VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *))
VK_PROC(vkDestroyImageView, void, (VkDevice, VkImageView, const VkAllocationCallbacks *))
VK_PROC(vkCreateCommandPool, VkResult, (VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *))
VK_PROC(vkDestroyCommandPool, void, (VkDevice, VkCommandPool, const VkAllocationCallbacks *))
VK_PROC(vkAllocateCommandBuffers, VkResult, (VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *))
VK_PROC(vkFreeCommandBuffers, void, (VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *))
VK_PROC(vkBeginCommandBuffer, VkResult, (VkCommandBuffer, const VkCommandBufferBeginInfo *))
VK_PROC(vkEndCommandBuffer, VkResult, (VkCommandBuffer))
VK_PROC(vkResetCommandBuffer, VkResult, (VkCommandBuffer, VkFlags))
VK_PROC(vkCmdPipelineBarrier, void, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier *))
VK_PROC(vkCmdBlitImage, void, (VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit *, int32_t))
VK_PROC(vkCreateSemaphore, VkResult, (VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *))
VK_PROC(vkDestroySemaphore, void, (VkDevice, VkSemaphore, const VkAllocationCallbacks *))
VK_PROC(vkCreateFence, VkResult, (VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *))
VK_PROC(vkDestroyFence, void, (VkDevice, VkFence, const VkAllocationCallbacks *))
VK_PROC(vkWaitForFences, VkResult, (VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t))
VK_PROC(vkResetFences, VkResult, (VkDevice, uint32_t, const VkFence *))
#undef VK_PROC
