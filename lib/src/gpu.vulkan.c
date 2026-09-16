// Vulkan GPU v2 backend (Linux/Wayland).
//
// The backend owns a swapchain on the view's wl_surface, compiles the GLSL the
// shader module emits with shaderc, and implements the memory/root/state/pass
// seam of doc/gpu.md. The view backend calls gpu_vk_begin_frame before the
// application's frame callback and gpu_vk_end_frame after it, which is where
// the swapchain image is acquired and presented.
//
// Every allocation is host-visible and persistently mapped: the v2 model
// writes through gpu_write and reads through gpu_read, and the root words a
// draw resolves its texture indices from are read on the CPU at draw time.
#include "gpu.h"

#ifdef NS_GPU_VULKAN

#include <shaderc/shaderc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>

// view.linux.c publishes the wl_display the surface belongs to. A program
// that links gpu without view still loads: the reference stays NULL.
extern void *view_linux_display(void) __attribute__((weak));

// The core's set_storage passes (3 + slot); GLSL places the descriptors at
// NS_SHADER_GLSL_STORAGE_BINDING_BASE and the mask map at
// NS_SHADER_GLSL_MASK_BINDING so neither collides with the root block at 2.
#define GPU_VK_CORE_STORAGE_BINDING_BASE 3
#define GPU_VK_STORAGE_BINDING_BASE 8
#define GPU_VK_MASK_BINDING 4
#define GPU_VK_STORAGE_SLOT_COUNT 16
#define GPU_VK_MAX_STORAGE_SLOTS GPU_VK_STORAGE_SLOT_COUNT
#define GPU_VK_ROOT_BLOCK_SIZE 4096
#define GPU_VK_MAX_UNIFORM_RANGE 65536
#define GPU_VK_MAX_SWAP_IMAGES 8
#define GPU_VK_MAX_COMMITS 8
#define GPU_VK_MAX_DESCRIPTOR_SETS 4096
#define GPU_VK_SHADER_NAME_CAPACITY 128

#define GPU_VK_USE_ROOT (1u << 0)
#define GPU_VK_USE_SCENE (1u << 1)
#define GPU_VK_USE_SHADOW (1u << 2)
#define GPU_VK_USE_TEXTURE_MAP (1u << 3)
#define GPU_VK_USE_MASK_MAP (1u << 4)
#define GPU_VK_USE_READ_TEXTURE (1u << 5)
#define GPU_VK_USE_WRITE_TEXTURE (1u << 6)
#define GPU_VK_USE_SECONDARY_WRITE_TEXTURE (1u << 7)

typedef struct gpu_vk_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    u8 *mapped;
    u64 size;    // allocation size, including root-block padding
    u64 logical; // size the core believes it allocated
} gpu_vk_buffer;

typedef struct gpu_vk_texture {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkImageLayout layout;
    VkFormat vk_format;
    i32 width, height, depth_layers;
    i32 mips, layers, kind;
    u32 usage;
    ns_bool depth;
    ns_bool transient;
} gpu_vk_texture;

typedef struct gpu_vk_pipeline_entry {
    gpu_v2_state_desc state;
    VkFormat colors[4];
    VkFormat depth;
    VkPipeline pipeline;
} gpu_vk_pipeline_entry;

typedef struct gpu_vk_shader {
    VkShaderModule vs, fs, cs;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
    u32 usage;
    u32 storage_count;
    gpu_vk_pipeline_entry *pipelines;
    u32 pipeline_count;
    u32 pipeline_capacity;
    char name[GPU_VK_SHADER_NAME_CAPACITY];
} gpu_vk_shader;

typedef struct gpu_vk_layout {
    u32 usage;
    u32 storage_count;
    VkDescriptorSetLayout layout;
} gpu_vk_layout;

// A released buffer may still be named by a descriptor a frame in flight is
// using. Freeing it waits for the slot's own fence, which retires every
// submission that could have recorded the buffer before its release.
#define GPU_VK_GRAVEYARD_CAPACITY 32

typedef struct gpu_vk_grave {
    VkBuffer buffer;
    VkDeviceMemory memory;
} gpu_vk_grave;

typedef struct gpu_vk_frame {
    VkCommandPool pool;
    VkDescriptorPool descriptor_pool;
    VkCommandBuffer commands[GPU_VK_MAX_COMMITS];
    u32 command_count;
    u32 command_index;
    VkFence fence;
    ns_bool fence_active;
    VkSemaphore image_available;
    gpu_vk_grave graveyard[GPU_VK_GRAVEYARD_CAPACITY];
    u32 graveyard_count;
} gpu_vk_frame;

typedef struct gpu_vk_state {
    ns_bool valid;
    view *owner;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    u32 queue_family;
    VkDevice device;
    VkQueue queue;

    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    VkImage swapchain_images[GPU_VK_MAX_SWAP_IMAGES];
    VkImageView swapchain_views[GPU_VK_MAX_SWAP_IMAGES];
    VkImageLayout swapchain_layout[GPU_VK_MAX_SWAP_IMAGES];
    VkSemaphore render_finished[GPU_VK_MAX_SWAP_IMAGES];
    u32 swapchain_image_count;
    u32 image_index;
    ns_bool image_acquired;
    ns_bool image_wait_consumed;
    ns_bool swapchain_dirty;

    gpu_vk_frame frames[GPU_SWAP_BUFFER_COUNT];
    u32 frame_slot;
    VkCommandBuffer commands;
    ns_bool commands_active;
    ns_bool frame_active;
    ns_bool frame_committed;
    ns_bool pass_open;
    u32 screen_pass_count;
    VkExtent2D pass_extent;
    VkFormat pass_colors[4];
    VkFormat pass_depth;

    u32 current_shader;
    gpu_v2_state_desc current_state;
    ns_bool root_valid;
    u32 root_slot;
    u64 root_offset;
    ns_bool storage_valid[GPU_VK_MAX_STORAGE_SLOTS];
    u32 storage_slots[GPU_VK_MAX_STORAGE_SLOTS];
    u64 storage_offsets[GPU_VK_MAX_STORAGE_SLOTS];
    u32 storage_slot_count;

    gpu_vk_buffer buffers[GPU_RESOURCE_POOL_SIZE];
    gpu_vk_texture textures[GPU_RESOURCE_POOL_SIZE];
    u32 texture_count;
    gpu_vk_shader shaders[GPU_RESOURCE_POOL_SIZE];
    u32 shader_count;

    VkSampler sampler_linear;
    VkSampler sampler_nearest;
    VkSampler sampler_shadow;

    gpu_vk_layout layouts[32];
    u32 layout_count;

    VkCommandPool transfer_pool;
    gpu_vk_texture dummy_texture;
    gpu_vk_texture dummy_depth;
    gpu_vk_buffer dummy_buffer;

    shaderc_compiler_t compiler;
    shaderc_compile_options_t compile_options;
} gpu_vk_state;

static gpu_vk_state _vk;
static const gpu_v2_ops _vulkan_v2_ops;

static void gpu_vk_grave_release(gpu_vk_grave *grave);

// ---- helpers ----------------------------------------------------------------

static void gpu_vk_log(const char *message) {
    ns_warn("gpu", "%s\n", message);
}

// NS_VK_TRACE=1 prints every pass and dispatch with its dimensions, which
// names the last command a GPU hang report is about.
static ns_bool gpu_vk_trace(void) {
    static i32 enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("NS_VK_TRACE");
        enabled = (value && value[0] && value[0] != '0') ? 1 : 0;
    }
    return enabled != 0;
}

// stderr is unbuffered, so a trace survives a GPU hang that kills the process.
static void gpu_vk_trace_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fflush(stderr);
}

static ns_bool gpu_vk_has_extension(const char *name, VkExtensionProperties *extensions, u32 count) {
    for (u32 i = 0; i < count; i++) {
        if (strcmp(extensions[i].extensionName, name) == 0) return true;
    }
    return false;
}

static u32 gpu_vk_memory_type(u32 type_bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(_vk.physical, &memory);
    for (u32 i = 0; i < memory.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

static VkFormat gpu_vk_format(gpu_pixel_format format) {
    switch (format) {
        case PIXELFORMAT_R8: return VK_FORMAT_R8_UNORM;
        case PIXELFORMAT_R8SN: return VK_FORMAT_R8_SNORM;
        case PIXELFORMAT_R8UI: return VK_FORMAT_R8_UINT;
        case PIXELFORMAT_R8SI: return VK_FORMAT_R8_SINT;
        case PIXELFORMAT_R16: return VK_FORMAT_R16_UNORM;
        case PIXELFORMAT_R16SN: return VK_FORMAT_R16_SNORM;
        case PIXELFORMAT_R16UI: return VK_FORMAT_R16_UINT;
        case PIXELFORMAT_R16SI: return VK_FORMAT_R16_SINT;
        case PIXELFORMAT_R16F: return VK_FORMAT_R16_SFLOAT;
        case PIXELFORMAT_RG8: return VK_FORMAT_R8G8_UNORM;
        case PIXELFORMAT_RG8SN: return VK_FORMAT_R8G8_SNORM;
        case PIXELFORMAT_RG8UI: return VK_FORMAT_R8G8_UINT;
        case PIXELFORMAT_RG8SI: return VK_FORMAT_R8G8_SINT;
        case PIXELFORMAT_R32UI: return VK_FORMAT_R32_UINT;
        case PIXELFORMAT_R32SI: return VK_FORMAT_R32_SINT;
        case PIXELFORMAT_R32F: return VK_FORMAT_R32_SFLOAT;
        case PIXELFORMAT_RG16: return VK_FORMAT_R16G16_UNORM;
        case PIXELFORMAT_RG16SN: return VK_FORMAT_R16G16_SNORM;
        case PIXELFORMAT_RG16UI: return VK_FORMAT_R16G16_UINT;
        case PIXELFORMAT_RG16SI: return VK_FORMAT_R16G16_SINT;
        case PIXELFORMAT_RG16F: return VK_FORMAT_R16G16_SFLOAT;
        case PIXELFORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
        case PIXELFORMAT_SRGB8A8: return VK_FORMAT_R8G8B8A8_SRGB;
        case PIXELFORMAT_RGBA8SN: return VK_FORMAT_R8G8B8A8_SNORM;
        case PIXELFORMAT_RGBA8UI: return VK_FORMAT_R8G8B8A8_UINT;
        case PIXELFORMAT_RGBA8SI: return VK_FORMAT_R8G8B8A8_SINT;
        case PIXELFORMAT_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
        case PIXELFORMAT_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case PIXELFORMAT_RG11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case PIXELFORMAT_RGB9E5: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case PIXELFORMAT_RG32UI: return VK_FORMAT_R32G32_UINT;
        case PIXELFORMAT_RG32SI: return VK_FORMAT_R32G32_SINT;
        case PIXELFORMAT_RG32F: return VK_FORMAT_R32G32_SFLOAT;
        case PIXELFORMAT_RGBA16: return VK_FORMAT_R16G16B16A16_UNORM;
        case PIXELFORMAT_RGBA16SN: return VK_FORMAT_R16G16B16A16_SNORM;
        case PIXELFORMAT_RGBA16UI: return VK_FORMAT_R16G16B16A16_UINT;
        case PIXELFORMAT_RGBA16SI: return VK_FORMAT_R16G16B16A16_SINT;
        case PIXELFORMAT_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PIXELFORMAT_RGBA32UI: return VK_FORMAT_R32G32B32A32_UINT;
        case PIXELFORMAT_RGBA32SI: return VK_FORMAT_R32G32B32A32_SINT;
        case PIXELFORMAT_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case PIXELFORMAT_DEPTH: return VK_FORMAT_D32_SFLOAT;
        case PIXELFORMAT_DEPTH_STENCIL: return VK_FORMAT_D24_UNORM_S8_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

static VkPrimitiveTopology gpu_vk_topology(gpu_primitive_type type) {
    switch (type) {
        case PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PRIMITIVE_LINE_STRIP:
        case PRIMITIVE_LINE_LOOP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PRIMITIVE_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static VkCullModeFlags gpu_vk_cull(gpu_cull_mode mode) {
    switch (mode) {
        case CULL_FRONT: return VK_CULL_MODE_FRONT_BIT;
        case CULL_BACK: return VK_CULL_MODE_BACK_BIT;
        default: return VK_CULL_MODE_NONE;
    }
}

static VkFrontFace gpu_vk_winding(gpu_face_winding winding) {
    return winding == FACE_WINDING_CW ? VK_FRONT_FACE_CLOCKWISE
                                       : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

static VkCompareOp gpu_vk_compare(gpu_compare_func func) {
    switch (func) {
        case COMPARE_NEVER: return VK_COMPARE_OP_NEVER;
        case COMPARE_LESS: return VK_COMPARE_OP_LESS;
        case COMPARE_EQUAL: return VK_COMPARE_OP_EQUAL;
        case COMPARE_LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case COMPARE_GREATER: return VK_COMPARE_OP_GREATER;
        case COMPARE_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case COMPARE_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkImageAspectFlags gpu_vk_aspect(const gpu_vk_texture *texture) {
    return texture->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

static ns_bool gpu_vk_texture_valid(u32 id) {
    return id > 0 && id < _vk.texture_count && _vk.textures[id].image != VK_NULL_HANDLE;
}

// ---- barriers ---------------------------------------------------------------

static void gpu_vk_transition_image(VkCommandBuffer commands, VkImage image,
                                    VkImageLayout *current, VkImageLayout next,
                                    VkImageAspectFlags aspect, i32 mips, i32 layers) {
    if (*current == next) return;
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = *current,
        .newLayout = next,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = (u32)(mips > 0 ? mips : 1),
            .baseArrayLayer = 0,
            .layerCount = (u32)(layers > 0 ? layers : 1),
        },
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
    *current = next;
}

static void gpu_vk_texture_transition(VkCommandBuffer commands, gpu_vk_texture *texture,
                                      VkImageLayout next) {
    if (!texture || texture->image == VK_NULL_HANDLE) return;
    gpu_vk_transition_image(commands, texture->image, &texture->layout, next,
                            gpu_vk_aspect(texture), texture->mips, texture->layers);
}

// ---- one-shot transfers -----------------------------------------------------

static VkCommandBuffer gpu_vk_oneshot_begin(void) {
    VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = _vk.transfer_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(_vk.device, &allocate, &commands) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(commands, &begin);
    return commands;
}

static void gpu_vk_oneshot_end(VkCommandBuffer commands) {
    if (commands == VK_NULL_HANDLE) return;
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commands,
    };
    vkQueueSubmit(_vk.queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(_vk.queue);
    vkFreeCommandBuffers(_vk.device, _vk.transfer_pool, 1, &commands);
}

// ---- swapchain --------------------------------------------------------------

static void gpu_vk_destroy_swapchain(void) {
    for (u32 i = 0; i < _vk.swapchain_image_count; i++) {
        if (_vk.swapchain_views[i]) vkDestroyImageView(_vk.device, _vk.swapchain_views[i], NULL);
        if (_vk.render_finished[i]) vkDestroySemaphore(_vk.device, _vk.render_finished[i], NULL);
        _vk.swapchain_views[i] = VK_NULL_HANDLE;
        _vk.render_finished[i] = VK_NULL_HANDLE;
        _vk.swapchain_images[i] = VK_NULL_HANDLE;
        _vk.swapchain_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    _vk.swapchain_image_count = 0;
    if (_vk.swapchain) {
        vkDestroySwapchainKHR(_vk.device, _vk.swapchain, NULL);
        _vk.swapchain = VK_NULL_HANDLE;
    }
    _vk.image_acquired = false;
}

static ns_bool gpu_vk_create_swapchain(i32 width, i32 height) {
    VkSurfaceCapabilitiesKHR capabilities;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_vk.physical, _vk.surface, &capabilities) != VK_SUCCESS) {
        return false;
    }

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_vk.physical, _vk.surface, &format_count, NULL);
    if (format_count == 0) return false;
    VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)malloc(sizeof(VkSurfaceFormatKHR) * format_count);
    if (!formats) return false;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_vk.physical, _vk.surface, &format_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (u32 i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = formats[i];
            break;
        }
    }
    if (chosen.format != VK_FORMAT_B8G8R8A8_UNORM) {
        for (u32 i = 0; i < format_count; i++) {
            if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosen = formats[i];
                break;
            }
        }
    }
    free(formats);

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xffffffffu || extent.height == 0xffffffffu) {
        extent.width = (u32)(width > 0 ? width : 1280);
        extent.height = (u32)(height > 0 ? height : 720);
        if (extent.width < capabilities.minImageExtent.width) extent.width = capabilities.minImageExtent.width;
        if (extent.width > capabilities.maxImageExtent.width) extent.width = capabilities.maxImageExtent.width;
        if (extent.height < capabilities.minImageExtent.height) extent.height = capabilities.minImageExtent.height;
        if (extent.height > capabilities.maxImageExtent.height) extent.height = capabilities.maxImageExtent.height;
    }

    u32 image_count = capabilities.minImageCount + 1;
    if (image_count < GPU_SWAP_BUFFER_COUNT) image_count = GPU_SWAP_BUFFER_COUNT;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    if (image_count > GPU_VK_MAX_SWAP_IMAGES) image_count = GPU_VK_MAX_SWAP_IMAGES;

    VkSwapchainCreateInfoKHR create = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = _vk.surface,
        .minImageCount = image_count,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    if (vkCreateSwapchainKHR(_vk.device, &create, NULL, &_vk.swapchain) != VK_SUCCESS) {
        return false;
    }
    if (vkGetSwapchainImagesKHR(_vk.device, _vk.swapchain, &image_count, NULL) != VK_SUCCESS) {
        return false;
    }
    if (image_count > GPU_VK_MAX_SWAP_IMAGES) image_count = GPU_VK_MAX_SWAP_IMAGES;
    vkGetSwapchainImagesKHR(_vk.device, _vk.swapchain, &image_count, _vk.swapchain_images);
    _vk.swapchain_image_count = image_count;
    _vk.swapchain_format = chosen.format;
    _vk.swapchain_extent = extent;

    for (u32 i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = _vk.swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = chosen.format,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        if (vkCreateImageView(_vk.device, &view_info, NULL, &_vk.swapchain_views[i]) != VK_SUCCESS) {
            return false;
        }
        _vk.swapchain_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(_vk.device, &semaphore_info, NULL, &_vk.render_finished[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

static ns_bool gpu_vk_ensure_swapchain(view *v) {
    if (!_vk.surface) return false;
    i32 width = v && v->framebuffer_width > 0 ? v->framebuffer_width : (i32)_vk.swapchain_extent.width;
    i32 height = v && v->framebuffer_height > 0 ? v->framebuffer_height : (i32)_vk.swapchain_extent.height;
    ns_bool needs = _vk.swapchain_dirty || _vk.swapchain == VK_NULL_HANDLE;
    if (!needs && _vk.swapchain_extent.width == (u32)width && _vk.swapchain_extent.height == (u32)height) {
        return true;
    }
    vkDeviceWaitIdle(_vk.device);
    gpu_vk_destroy_swapchain();
    if (!gpu_vk_create_swapchain(width, height)) {
        gpu_vk_log("failed to create the Vulkan swapchain");
        return false;
    }
    _vk.swapchain_dirty = false;
    return true;
}

// ---- shaders ----------------------------------------------------------------

static ns_bool gpu_vk_source_uses(const char *source, const char *symbol) {
    return source && strstr(source, symbol) != NULL;
}

// Highest referenced ns_storage_buffer_<n> slot, plus one. The GLSL the shader
// module emits declares exactly the buffers a program reaches, and the UI
// renderer's hand-written source follows the same naming.
static u32 gpu_vk_source_storage_slots(const char *source) {
    static const char prefix[] = "ns_storage_buffer_";
    u32 count = 0;
    if (!source) return 0;
    const char *at = source;
    while ((at = strstr(at, prefix)) != NULL) {
        at += sizeof(prefix) - 1;
        if (*at < '0' || *at > '9') continue;
        u64 index = 0;
        while (*at >= '0' && *at <= '9') {
            index = index * 10u + (u64)(*at - '0');
            at++;
        }
        u32 required = index >= 0xffffffffu ? 0xffffffffu : (u32)index + 1;
        if (required > count) count = required;
    }
    return count;
}

static u32 gpu_vk_source_usage(const char *source) {
    u32 usage = 0;
    if (gpu_vk_source_uses(source, "ns_root")) usage |= GPU_VK_USE_ROOT;
    if (gpu_vk_source_uses(source, "ns_uniforms")) usage |= GPU_VK_USE_SCENE;
    if (gpu_vk_source_uses(source, "ns_shadow_map")) usage |= GPU_VK_USE_SHADOW;
    if (gpu_vk_source_uses(source, "ns_texture_map")) usage |= GPU_VK_USE_TEXTURE_MAP;
    if (gpu_vk_source_uses(source, "ns_mask_map")) usage |= GPU_VK_USE_MASK_MAP;
    if (gpu_vk_source_uses(source, "ns_read_texture")) usage |= GPU_VK_USE_READ_TEXTURE;
    if (gpu_vk_source_uses(source, "ns_write_texture")) usage |= GPU_VK_USE_WRITE_TEXTURE;
    if (gpu_vk_source_uses(source, "ns_secondary_write_texture")) usage |= GPU_VK_USE_SECONDARY_WRITE_TEXTURE;
    return usage;
}

static VkDescriptorSetLayout gpu_vk_descriptor_layout(u32 usage, u32 storage_count) {
    for (u32 i = 0; i < _vk.layout_count; i++) {
        if (_vk.layouts[i].usage == usage && _vk.layouts[i].storage_count == storage_count) {
            return _vk.layouts[i].layout;
        }
    }
    if (_vk.layout_count >= (u32)(sizeof(_vk.layouts) / sizeof(_vk.layouts[0]))) return VK_NULL_HANDLE;

    VkDescriptorSetLayoutBinding bindings[GPU_VK_STORAGE_SLOT_COUNT + 4];
    u32 count = 0;
    if (usage & GPU_VK_USE_READ_TEXTURE) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
    } else if (usage & GPU_VK_USE_SHADOW) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    }
    if (usage & GPU_VK_USE_WRITE_TEXTURE) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
    } else if (usage & GPU_VK_USE_TEXTURE_MAP) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
    }
    if (usage & (GPU_VK_USE_ROOT | GPU_VK_USE_SCENE)) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
    }
    if (usage & GPU_VK_USE_MASK_MAP) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = GPU_VK_MASK_BINDING, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    }
    for (u32 i = 0; i < storage_count; i++) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = GPU_VK_STORAGE_BINDING_BASE + i,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
    }
    if (usage & GPU_VK_USE_SECONDARY_WRITE_TEXTURE) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){
            .binding = 15, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
    }

    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = count,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(_vk.device, &info, NULL, &layout) != VK_SUCCESS) {
        ns_warn("gpu", "failed to create a descriptor set layout (usage %u, %u storage)\n",
                usage, storage_count);
        return VK_NULL_HANDLE;
    }
    _vk.layouts[_vk.layout_count].usage = usage;
    _vk.layouts[_vk.layout_count].storage_count = storage_count;
    _vk.layouts[_vk.layout_count].layout = layout;
    _vk.layout_count++;
    return layout;
}

static VkShaderModule gpu_vk_compile(const char *source, const char *name, shaderc_shader_kind kind) {
    if (!source || !source[0]) return VK_NULL_HANDLE;
    shaderc_compilation_result_t result = shaderc_compile_into_spv(
        _vk.compiler, source, strlen(source), kind, name, "main", _vk.compile_options);
    if (!result) return VK_NULL_HANDLE;
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
        const char *message = shaderc_result_get_error_message(result);
        ns_warn("gpu", "shader %s failed to compile: %s\n", name, message ? message : "(no message)");
        shaderc_result_release(result);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaderc_result_get_length(result),
        .pCode = (const uint32_t *)shaderc_result_get_bytes(result),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(_vk.device, &info, NULL, &module) != VK_SUCCESS) {
        ns_warn("gpu", "failed to create a shader module for %s\n", name);
        module = VK_NULL_HANDLE;
    }
    shaderc_result_release(result);
    return module;
}

// ---- pipelines --------------------------------------------------------------

static VkPipeline gpu_vk_graphics_pipeline(gpu_vk_shader *shader) {
    if (!shader->vs || !shader->fs || shader->layout == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    for (u32 i = 0; i < shader->pipeline_count; i++) {
        gpu_vk_pipeline_entry *entry = &shader->pipelines[i];
        if (memcmp(&entry->state, &_vk.current_state, sizeof(gpu_v2_state_desc)) == 0 &&
            memcmp(entry->colors, _vk.pass_colors, sizeof(entry->colors)) == 0 &&
            entry->depth == _vk.pass_depth) {
            return entry->pipeline;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader->fs, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = gpu_vk_topology((gpu_primitive_type)_vk.current_state.primitive_type),
        .primitiveRestartEnable = VK_FALSE,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = gpu_vk_cull((gpu_cull_mode)_vk.current_state.cull_mode),
        .frontFace = gpu_vk_winding((gpu_face_winding)_vk.current_state.face_winding),
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    ns_bool depth_enabled = _vk.pass_depth != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depth_enabled ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = depth_enabled && _vk.current_state.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = gpu_vk_compare((gpu_compare_func)_vk.current_state.depth_compare),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };
    VkPipelineColorBlendAttachmentState blend[4];
    for (u32 i = 0; i < 4; i++) {
        memset(&blend[i], 0, sizeof(blend[i]));
        if (_vk.pass_colors[i] == VK_FORMAT_UNDEFINED) continue;
        blend[i].colorWriteMask = (VkColorComponentFlags)_vk.current_state.color_mask;
        if (_vk.current_state.blend_preset != GPU_BLEND_OFF) {
            blend[i].blendEnable = VK_TRUE;
            blend[i].colorBlendOp = VK_BLEND_OP_ADD;
            blend[i].alphaBlendOp = VK_BLEND_OP_ADD;
            if (_vk.current_state.blend_preset == GPU_BLEND_ADDITIVE) {
                blend[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                blend[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            } else if (_vk.current_state.blend_preset == GPU_BLEND_PREMULTIPLIED) {
                blend[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                blend[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            } else {
                blend[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                blend[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            }
            blend[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
    }
    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 4,
        .pAttachments = blend,
    };
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 4,
        .pColorAttachmentFormats = _vk.pass_colors,
        .depthAttachmentFormat = _vk.pass_depth,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic,
        .layout = shader->layout,
        .renderPass = VK_NULL_HANDLE,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(_vk.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
    if (result != VK_SUCCESS) {
        ns_warn("gpu", "failed to create a Vulkan pipeline for shader %s (result %d)\n", shader->name, (int)result);
        return VK_NULL_HANDLE;
    }
    if (shader->pipeline_count == shader->pipeline_capacity) {
        u32 capacity = shader->pipeline_capacity ? shader->pipeline_capacity * 2 : 8;
        gpu_vk_pipeline_entry *grown = (gpu_vk_pipeline_entry *)realloc(
            shader->pipelines, sizeof(gpu_vk_pipeline_entry) * capacity);
        if (!grown) {
            vkDestroyPipeline(_vk.device, pipeline, NULL);
            return VK_NULL_HANDLE;
        }
        shader->pipelines = grown;
        shader->pipeline_capacity = capacity;
    }
    gpu_vk_pipeline_entry *entry = &shader->pipelines[shader->pipeline_count++];
    entry->state = _vk.current_state;
    memcpy(entry->colors, _vk.pass_colors, sizeof(entry->colors));
    entry->depth = _vk.pass_depth;
    entry->pipeline = pipeline;
    return pipeline;
}

static VkPipeline gpu_vk_compute_pipeline(gpu_vk_shader *shader) {
    if (!shader->cs || shader->layout == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    for (u32 i = 0; i < shader->pipeline_count; i++) {
        if (shader->pipelines[i].pipeline && shader->pipelines[i].depth == VK_FORMAT_UNDEFINED &&
            shader->pipelines[i].state.primitive_type < 0) {
            return shader->pipelines[i].pipeline;
        }
    }
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader->cs,
                  .pName = "main"},
        .layout = shader->layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(_vk.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS) {
        ns_warn("gpu", "failed to create a Vulkan compute pipeline for shader %s\n", shader->name);
        return VK_NULL_HANDLE;
    }
    if (shader->pipeline_count == shader->pipeline_capacity) {
        u32 capacity = shader->pipeline_capacity ? shader->pipeline_capacity * 2 : 8;
        gpu_vk_pipeline_entry *grown = (gpu_vk_pipeline_entry *)realloc(
            shader->pipelines, sizeof(gpu_vk_pipeline_entry) * capacity);
        if (!grown) {
            vkDestroyPipeline(_vk.device, pipeline, NULL);
            return VK_NULL_HANDLE;
        }
        shader->pipelines = grown;
        shader->pipeline_capacity = capacity;
    }
    gpu_vk_pipeline_entry *entry = &shader->pipelines[shader->pipeline_count++];
    memset(entry, 0, sizeof(*entry));
    entry->state.primitive_type = -1;
    entry->pipeline = pipeline;
    return pipeline;
}

// ---- device -----------------------------------------------------------------

static ns_bool gpu_vk_create_instance(void) {
    u32 extension_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
    VkExtensionProperties *extensions = (VkExtensionProperties *)malloc(
        sizeof(VkExtensionProperties) * (extension_count ? extension_count : 1));
    if (!extensions) return false;
    vkEnumerateInstanceExtensionProperties(NULL, &extension_count, extensions);
    const char *required[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    for (u32 i = 0; i < 2; i++) {
        if (!gpu_vk_has_extension(required[i], extensions, extension_count)) {
            ns_warn("gpu", "Vulkan instance extension %s is not available\n", required[i]);
            free(extensions);
            return false;
        }
    }
    free(extensions);

    const char *layers[1];
    u32 layer_count = 0;
    const char *validation = getenv("NS_VK_VALIDATION");
    if (validation && validation[0] && validation[0] != '0') {
        u32 available = 0;
        vkEnumerateInstanceLayerProperties(&available, NULL);
        VkLayerProperties *properties = (VkLayerProperties *)malloc(sizeof(VkLayerProperties) * (available ? available : 1));
        if (properties) {
            vkEnumerateInstanceLayerProperties(&available, properties);
            for (u32 i = 0; i < available; i++) {
                if (strcmp(properties[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                    layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
                    break;
                }
            }
            free(properties);
        }
    }

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ns",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "ns",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = required,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layer_count ? layers : NULL,
    };
    return vkCreateInstance(&info, NULL, &_vk.instance) == VK_SUCCESS;
}

static ns_bool gpu_vk_pick_device(void) {
    u32 device_count = 0;
    vkEnumeratePhysicalDevices(_vk.instance, &device_count, NULL);
    if (device_count == 0) return false;
    VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * device_count);
    if (!devices) return false;
    vkEnumeratePhysicalDevices(_vk.instance, &device_count, devices);

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    u32 chosen_family = 0;
    i32 best_score = -1;
    for (u32 i = 0; i < device_count; i++) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features features13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &features13,
        };
        vkGetPhysicalDeviceFeatures2(devices[i], &features);
        if (!features13.dynamicRendering || !features13.synchronization2) continue;

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, NULL);
        VkQueueFamilyProperties *families = (VkQueueFamilyProperties *)malloc(
            sizeof(VkQueueFamilyProperties) * (family_count ? family_count : 1));
        if (!families) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families);
        for (u32 f = 0; f < family_count; f++) {
            if (!(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], f, _vk.surface, &present) != VK_SUCCESS) continue;
            if (!present) continue;
            i32 score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                chosen = devices[i];
                chosen_family = f;
            }
            break;
        }
        free(families);
    }
    free(devices);
    if (chosen == VK_NULL_HANDLE) return false;
    _vk.physical = chosen;
    _vk.queue_family = chosen_family;
    return true;
}

static ns_bool gpu_vk_create_device(void) {
    // robustBufferAccess gives shaders Metal's out-of-range read behavior:
    // without it an unclamped access is undefined and can fault the ring.
    VkPhysicalDeviceFeatures features = {
        .robustBufferAccess = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = _vk.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features13,
        .pEnabledFeatures = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    if (vkCreateDevice(_vk.physical, &info, NULL, &_vk.device) != VK_SUCCESS) return false;
    vkGetDeviceQueue(_vk.device, _vk.queue_family, 0, &_vk.queue);
    return true;
}

static ns_bool gpu_vk_create_samplers(void) {
    VkSamplerCreateInfo linear = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 64.0f,
    };
    VkSamplerCreateInfo nearest = linear;
    nearest.magFilter = VK_FILTER_NEAREST;
    nearest.minFilter = VK_FILTER_NEAREST;
    nearest.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerCreateInfo shadow = linear;
    shadow.compareEnable = VK_TRUE;
    shadow.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    return vkCreateSampler(_vk.device, &linear, NULL, &_vk.sampler_linear) == VK_SUCCESS &&
           vkCreateSampler(_vk.device, &nearest, NULL, &_vk.sampler_nearest) == VK_SUCCESS &&
           vkCreateSampler(_vk.device, &shadow, NULL, &_vk.sampler_shadow) == VK_SUCCESS;
}

static ns_bool gpu_vk_create_frame_resources(void) {
    for (u32 i = 0; i < GPU_SWAP_BUFFER_COUNT; i++) {
        gpu_vk_frame *frame = &_vk.frames[i];
        VkCommandPoolCreateInfo pool = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = _vk.queue_family,
        };
        if (vkCreateCommandPool(_vk.device, &pool, NULL, &frame->pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frame->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = GPU_VK_MAX_COMMITS,
        };
        if (vkAllocateCommandBuffers(_vk.device, &allocate, frame->commands) != VK_SUCCESS) return false;
        frame->command_count = GPU_VK_MAX_COMMITS;
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = GPU_VK_MAX_DESCRIPTOR_SETS},
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = GPU_VK_MAX_DESCRIPTOR_SETS * 2},
            {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = GPU_VK_MAX_DESCRIPTOR_SETS * 2},
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = GPU_VK_MAX_DESCRIPTOR_SETS},
        };
        VkDescriptorPoolCreateInfo descriptors = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = GPU_VK_MAX_DESCRIPTOR_SETS,
            .poolSizeCount = 4,
            .pPoolSizes = sizes,
        };
        if (vkCreateDescriptorPool(_vk.device, &descriptors, NULL, &frame->descriptor_pool) != VK_SUCCESS) return false;
        VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(_vk.device, &fence, NULL, &frame->fence) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(_vk.device, &semaphore, NULL, &frame->image_available) != VK_SUCCESS) return false;
    }
    VkCommandPoolCreateInfo transfer = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = _vk.queue_family,
    };
    return vkCreateCommandPool(_vk.device, &transfer, NULL, &_vk.transfer_pool) == VK_SUCCESS;
}

static ns_bool gpu_vk_create_dummy_image(gpu_vk_texture *texture, VkFormat format, ns_bool depth) {
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (depth) {
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (vkCreateImage(_vk.device, &image, NULL, &texture->image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(_vk.device, texture->image, &requirements);
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = gpu_vk_memory_type(requirements.memoryTypeBits, 0),
    };
    if (vkAllocateMemory(_vk.device, &allocate, NULL, &texture->memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(_vk.device, texture->image, texture->memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(_vk.device, &view, NULL, &texture->view) != VK_SUCCESS) return false;
    texture->vk_format = format;
    texture->width = texture->height = 1;
    texture->mips = texture->layers = 1;
    texture->depth = depth;
    // The fallback stays in GENERAL, the one layout a sampled, storage and
    // attachment binding can all agree on.
    texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkCommandBuffer commands = gpu_vk_oneshot_begin();
    if (commands != VK_NULL_HANDLE) {
        gpu_vk_texture_transition(commands, texture, VK_IMAGE_LAYOUT_GENERAL);
        gpu_vk_oneshot_end(commands);
    }
    return true;
}

static ns_bool gpu_vk_create_dummies(void) {
    // A 1x1 white texture, a 1x1 depth texture and a small zeroed buffer stand
    // in for bindings a shader declares but the current draw never set.
    if (!gpu_vk_create_dummy_image(&_vk.dummy_texture, VK_FORMAT_R8G8B8A8_UNORM, false)) return false;
    if (!gpu_vk_create_dummy_image(&_vk.dummy_depth, VK_FORMAT_D32_SFLOAT, true)) return false;

    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = GPU_VK_ROOT_BLOCK_SIZE,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    if (vkCreateBuffer(_vk.device, &buffer, NULL, &_vk.dummy_buffer.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(_vk.device, _vk.dummy_buffer.buffer, &requirements);
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = gpu_vk_memory_type(requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (vkAllocateMemory(_vk.device, &allocate, NULL, &_vk.dummy_buffer.memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(_vk.device, _vk.dummy_buffer.buffer, _vk.dummy_buffer.memory, 0) != VK_SUCCESS) return false;
    if (vkMapMemory(_vk.device, _vk.dummy_buffer.memory, 0, VK_WHOLE_SIZE, 0,
                    (void **)&_vk.dummy_buffer.mapped) != VK_SUCCESS) return false;
    memset(_vk.dummy_buffer.mapped, 0, GPU_VK_ROOT_BLOCK_SIZE);
    _vk.dummy_buffer.size = GPU_VK_ROOT_BLOCK_SIZE;
    _vk.dummy_buffer.logical = GPU_VK_ROOT_BLOCK_SIZE;
    return true;
}

ns_bool gpu_request_device(view *v) {
    if (_vk.valid && _vk.owner == v) return true;
    if (_vk.valid) gpu_destroy_device();
    memset(&_vk, 0, sizeof(_vk));
    _vk.owner = v;
    _vk.current_state = (gpu_v2_state_desc){
        .primitive_type = PRIMITIVE_TRIANGLES,
        .cull_mode = CULL_NONE,
        .face_winding = FACE_WINDING_CCW,
        .depth_compare = COMPARE_ALWAYS,
        .depth_write = false,
        .blend_preset = GPU_BLEND_OFF,
        .color_mask = COLOR_MASK_ALL,
    };
    _vk.pass_colors[0] = _vk.pass_colors[1] = _vk.pass_colors[2] = _vk.pass_colors[3] = VK_FORMAT_UNDEFINED;
    _vk.pass_depth = VK_FORMAT_UNDEFINED;

    if (!v || !v->native_window) {
        gpu_vk_log("no window surface: the Vulkan backend needs a view");
        return false;
    }
    if (!view_linux_display) {
        gpu_vk_log("view module is not loaded: no Wayland display for the Vulkan surface");
        return false;
    }
    if (!gpu_vk_create_instance()) {
        gpu_vk_log("failed to create the Vulkan instance");
        return false;
    }
    // From here on a failure has resources to release, so the ordinary device
    // teardown owns the cleanup.
    _vk.valid = true;
    VkWaylandSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = (struct wl_display *)view_linux_display(),
        .surface = (struct wl_surface *)v->native_window,
    };
    if (vkCreateWaylandSurfaceKHR(_vk.instance, &surface_info, NULL, &_vk.surface) != VK_SUCCESS) {
        gpu_vk_log("failed to create a Vulkan Wayland surface");
        gpu_destroy_device();
        return false;
    }
    if (!gpu_vk_pick_device()) {
        gpu_vk_log("no Vulkan 1.3 device can present to this surface");
        gpu_destroy_device();
        return false;
    }
    if (!gpu_vk_create_device()) {
        gpu_vk_log("failed to create the Vulkan device");
        gpu_destroy_device();
        return false;
    }
    if (!gpu_vk_create_samplers()) {
        gpu_vk_log("failed to create the default samplers");
        gpu_destroy_device();
        return false;
    }
    if (!gpu_vk_create_frame_resources()) {
        gpu_vk_log("failed to create the Vulkan frame resources");
        gpu_destroy_device();
        return false;
    }
    if (!gpu_vk_create_dummies()) {
        gpu_vk_log("failed to create the fallback GPU resources");
        gpu_destroy_device();
        return false;
    }
    if (!gpu_vk_create_swapchain(v->framebuffer_width, v->framebuffer_height)) {
        gpu_vk_log("failed to create the Vulkan swapchain");
        gpu_destroy_device();
        return false;
    }

    _vk.compiler = shaderc_compiler_initialize();
    _vk.compile_options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(_vk.compile_options, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(_vk.compile_options, shaderc_optimization_level_performance);

    // ID 0 is the invalid handle on the ns surface, so both pools start after
    // it exactly as the Metal backend's do.
    _vk.shader_count = 1;
    _vk.texture_count = 1;
    _vk.storage_slot_count = GPU_VK_STORAGE_SLOT_COUNT;
    gpu_v2_set_backend(&_vulkan_v2_ops,
                       GPU_CAP_RAW_POINTERS | GPU_CAP_INDIRECT_DRAW | GPU_CAP_READBACK,
                       _vk.storage_slot_count);
    return true;
}

void gpu_destroy_device(void) {
    if (!_vk.valid) return;
    gpu_v2_set_backend(NULL, 0, 0);
    _vk.valid = false;
    if (_vk.device) vkDeviceWaitIdle(_vk.device);
    for (u32 i = 0; i < GPU_RESOURCE_POOL_SIZE; i++) {
        if (_vk.buffers[i].buffer) {
            vkDestroyBuffer(_vk.device, _vk.buffers[i].buffer, NULL);
            vkFreeMemory(_vk.device, _vk.buffers[i].memory, NULL);
        }
        if (_vk.textures[i].view) vkDestroyImageView(_vk.device, _vk.textures[i].view, NULL);
        if (_vk.textures[i].image) vkDestroyImage(_vk.device, _vk.textures[i].image, NULL);
        if (_vk.textures[i].memory) vkFreeMemory(_vk.device, _vk.textures[i].memory, NULL);
        if (_vk.shaders[i].vs) vkDestroyShaderModule(_vk.device, _vk.shaders[i].vs, NULL);
        if (_vk.shaders[i].fs) vkDestroyShaderModule(_vk.device, _vk.shaders[i].fs, NULL);
        if (_vk.shaders[i].cs) vkDestroyShaderModule(_vk.device, _vk.shaders[i].cs, NULL);
        for (u32 p = 0; p < _vk.shaders[i].pipeline_count; p++) {
            if (_vk.shaders[i].pipelines[p].pipeline) {
                vkDestroyPipeline(_vk.device, _vk.shaders[i].pipelines[p].pipeline, NULL);
            }
        }
        free(_vk.shaders[i].pipelines);
        if (_vk.shaders[i].layout) vkDestroyPipelineLayout(_vk.device, _vk.shaders[i].layout, NULL);
    }
    for (u32 i = 0; i < _vk.layout_count; i++) {
        vkDestroyDescriptorSetLayout(_vk.device, _vk.layouts[i].layout, NULL);
    }
    if (_vk.dummy_texture.view) vkDestroyImageView(_vk.device, _vk.dummy_texture.view, NULL);
    if (_vk.dummy_texture.image) vkDestroyImage(_vk.device, _vk.dummy_texture.image, NULL);
    if (_vk.dummy_texture.memory) vkFreeMemory(_vk.device, _vk.dummy_texture.memory, NULL);
    if (_vk.dummy_depth.view) vkDestroyImageView(_vk.device, _vk.dummy_depth.view, NULL);
    if (_vk.dummy_depth.image) vkDestroyImage(_vk.device, _vk.dummy_depth.image, NULL);
    if (_vk.dummy_depth.memory) vkFreeMemory(_vk.device, _vk.dummy_depth.memory, NULL);
    if (_vk.dummy_buffer.buffer) vkDestroyBuffer(_vk.device, _vk.dummy_buffer.buffer, NULL);
    if (_vk.dummy_buffer.memory) {
        if (_vk.dummy_buffer.mapped) vkUnmapMemory(_vk.device, _vk.dummy_buffer.memory);
        vkFreeMemory(_vk.device, _vk.dummy_buffer.memory, NULL);
    }
    if (_vk.sampler_linear) vkDestroySampler(_vk.device, _vk.sampler_linear, NULL);
    if (_vk.sampler_nearest) vkDestroySampler(_vk.device, _vk.sampler_nearest, NULL);
    if (_vk.sampler_shadow) vkDestroySampler(_vk.device, _vk.sampler_shadow, NULL);
    gpu_vk_destroy_swapchain();
    for (u32 i = 0; i < GPU_SWAP_BUFFER_COUNT; i++) {
        for (u32 g = 0; g < _vk.frames[i].graveyard_count; g++) {
            gpu_vk_grave_release(&_vk.frames[i].graveyard[g]);
        }
        _vk.frames[i].graveyard_count = 0;
        if (_vk.frames[i].fence) vkDestroyFence(_vk.device, _vk.frames[i].fence, NULL);
        if (_vk.frames[i].image_available) vkDestroySemaphore(_vk.device, _vk.frames[i].image_available, NULL);
        if (_vk.frames[i].descriptor_pool) vkDestroyDescriptorPool(_vk.device, _vk.frames[i].descriptor_pool, NULL);
        if (_vk.frames[i].pool) vkDestroyCommandPool(_vk.device, _vk.frames[i].pool, NULL);
    }
    if (_vk.transfer_pool) vkDestroyCommandPool(_vk.device, _vk.transfer_pool, NULL);
    if (_vk.compile_options) shaderc_compile_options_release(_vk.compile_options);
    if (_vk.compiler) shaderc_compiler_release(_vk.compiler);
    if (_vk.device) vkDestroyDevice(_vk.device, NULL);
    if (_vk.surface) vkDestroySurfaceKHR(_vk.instance, _vk.surface, NULL);
    if (_vk.instance) vkDestroyInstance(_vk.instance, NULL);
    memset(&_vk, 0, sizeof(_vk));
}

// ---- frames -----------------------------------------------------------------

static VkCommandBuffer gpu_vk_open_commands(void) {
    gpu_vk_frame *frame = &_vk.frames[_vk.frame_slot];
    if (frame->command_index >= frame->command_count) {
        ns_warn("gpu", "frame used more than %d commits\n", GPU_VK_MAX_COMMITS);
        return VK_NULL_HANDLE;
    }
    VkCommandBuffer commands = frame->commands[frame->command_index++];
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
    _vk.commands = commands;
    _vk.commands_active = true;
    return commands;
}

static ns_bool gpu_vk_submit(VkCommandBuffer commands, VkSemaphore signal) {
    // The acquire semaphore belongs to the first submit of the frame only;
    // later submits in the same frame run after it on the same queue.
    VkSemaphore wait = VK_NULL_HANDLE;
    if (_vk.image_acquired && !_vk.image_wait_consumed) {
        wait = _vk.frames[_vk.frame_slot].image_available;
        _vk.image_wait_consumed = true;
    }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = wait ? 1u : 0u,
        .pWaitSemaphores = wait ? &wait : NULL,
        .pWaitDstStageMask = wait ? &wait_stage : NULL,
        .commandBufferCount = commands ? 1u : 0u,
        .pCommandBuffers = commands ? &commands : NULL,
        .signalSemaphoreCount = signal ? 1u : 0u,
        .pSignalSemaphores = signal ? &signal : NULL,
    };
    VkResult result = vkQueueSubmit(_vk.queue, 1, &submit, _vk.frames[_vk.frame_slot].fence);
    if (result != VK_SUCCESS) {
        ns_warn("gpu", "vkQueueSubmit failed (%d)\n", (int)result);
        return false;
    }
    _vk.frames[_vk.frame_slot].fence_active = true;
    return true;
}

void gpu_vk_begin_frame(view *v) {
    if (!_vk.valid) return;
    if (!gpu_vk_ensure_swapchain(v)) return;

    gpu_vk_frame *frame = &_vk.frames[_vk.frame_slot];
    if (frame->fence_active) {
        vkWaitForFences(_vk.device, 1, &frame->fence, VK_TRUE, UINT64_MAX);
    }
    vkResetFences(_vk.device, 1, &frame->fence);
    frame->fence_active = false;
    for (u32 i = 0; i < frame->graveyard_count; i++) gpu_vk_grave_release(&frame->graveyard[i]);
    frame->graveyard_count = 0;
    frame->command_index = 0;
    vkResetDescriptorPool(_vk.device, frame->descriptor_pool, 0);

    u32 image = 0;
    VkResult result = vkAcquireNextImageKHR(_vk.device, _vk.swapchain, UINT64_MAX,
                                            frame->image_available, VK_NULL_HANDLE, &image);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        _vk.swapchain_dirty = true;
        if (!gpu_vk_ensure_swapchain(v)) return;
        result = vkAcquireNextImageKHR(_vk.device, _vk.swapchain, UINT64_MAX,
                                       frame->image_available, VK_NULL_HANDLE, &image);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        ns_warn("gpu", "vkAcquireNextImageKHR failed (%d)\n", (int)result);
        return;
    }
    _vk.image_index = image;
    _vk.image_acquired = true;
    _vk.image_wait_consumed = false;

    _vk.frame_active = true;
    _vk.frame_committed = false;
    _vk.pass_open = false;
    _vk.screen_pass_count = 0;
    _vk.current_shader = 0;
    _vk.root_valid = false;
    memset(_vk.storage_valid, 0, sizeof(_vk.storage_valid));
    _vk.pass_colors[0] = _vk.pass_colors[1] = _vk.pass_colors[2] = _vk.pass_colors[3] = VK_FORMAT_UNDEFINED;
    _vk.pass_depth = VK_FORMAT_UNDEFINED;
    gpu_vk_open_commands();
}

void gpu_vk_end_frame(view *v) {
    ns_unused(v);
    if (!_vk.valid || !_vk.frame_active) return;
    if (_vk.pass_open) {
        vkCmdEndRendering(_vk.commands);
        _vk.pass_open = false;
    }
    if (_vk.commands_active) {
        // The presented image belongs to the presentation engine once the
        // frame's work lands, so the transition rides the last command buffer.
        if (_vk.image_acquired) {
            gpu_vk_transition_image(_vk.commands, _vk.swapchain_images[_vk.image_index],
                                    &_vk.swapchain_layout[_vk.image_index],
                                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                    VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        }
        vkEndCommandBuffer(_vk.commands);
    }

    VkSemaphore signal = _vk.image_acquired ? _vk.render_finished[_vk.image_index] : VK_NULL_HANDLE;
    ns_bool submitted = false;
    if (_vk.commands_active) {
        submitted = gpu_vk_submit(_vk.commands, signal);
    } else {
        submitted = gpu_vk_submit(VK_NULL_HANDLE, signal);
    }
    _vk.commands_active = false;
    _vk.commands = VK_NULL_HANDLE;

    if (!_vk.frame_committed) gpu_v2_frame_end();

    if (submitted && _vk.image_acquired) {
        VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &signal,
            .swapchainCount = 1,
            .pSwapchains = &_vk.swapchain,
            .pImageIndices = &_vk.image_index,
        };
        VkResult result = vkQueuePresentKHR(_vk.queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            _vk.swapchain_dirty = true;
        } else if (result != VK_SUCCESS) {
            ns_warn("gpu", "vkQueuePresentKHR failed (%d)\n", (int)result);
        }
    }
    _vk.image_acquired = false;
    _vk.frame_active = false;
    _vk.frame_slot = (_vk.frame_slot + 1) % GPU_SWAP_BUFFER_COUNT;
}

void gpu_set_viewport(int x, int y, int width, int height) {
    if (!_vk.valid || !_vk.pass_open) return;
    VkViewport viewport = {
        .x = (f32)x, .y = (f32)y,
        .width = (f32)width, .height = (f32)height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(_vk.commands, 0, 1, &viewport);
}

void gpu_set_scissor(int x, int y, int width, int height) {
    if (!_vk.valid || !_vk.pass_open) return;
    VkRect2D scissor = {.offset = {x, y}, .extent = {(u32)width, (u32)height}};
    vkCmdSetScissor(_vk.commands, 0, 1, &scissor);
}

void gpu_commit(void) {
    if (!_vk.valid || !_vk.frame_active) {
        gpu_v2_frame_end();
        return;
    }
    if (_vk.pass_open) {
        vkCmdEndRendering(_vk.commands);
        _vk.pass_open = false;
    }
    VkCommandBuffer submitted = VK_NULL_HANDLE;
    if (_vk.commands_active) {
        vkEndCommandBuffer(_vk.commands);
        submitted = _vk.commands;
        _vk.commands_active = false;
    }
    gpu_vk_submit(submitted, VK_NULL_HANDLE);
    gpu_v2_frame_end();
    _vk.frame_committed = true;
    // Later draws in the same frame land in a fresh command buffer.
    gpu_vk_open_commands();
}

// ---- memory -----------------------------------------------------------------

static ns_bool gpu_vk_mem_create(u32 slot, u64 size, u32 flags, const char *name, u64 *base_va) {
    ns_unused(flags);
    if (slot >= GPU_RESOURCE_POOL_SIZE || size == 0 || !name || !name[0]) return false;
    u64 allocation = size + GPU_VK_ROOT_BLOCK_SIZE;
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = allocation,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
    };
    gpu_vk_buffer *buffer = &_vk.buffers[slot];
    if (vkCreateBuffer(_vk.device, &info, NULL, &buffer->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(_vk.device, buffer->buffer, &requirements);
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = gpu_vk_memory_type(requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (vkAllocateMemory(_vk.device, &allocate, NULL, &buffer->memory) != VK_SUCCESS) {
        vkDestroyBuffer(_vk.device, buffer->buffer, NULL);
        buffer->buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(_vk.device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) {
        return false;
    }
    if (vkMapMemory(_vk.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, (void **)&buffer->mapped) != VK_SUCCESS) {
        return false;
    }
    buffer->size = allocation;
    buffer->logical = size;
    if (base_va) *base_va = 0;
    return true;
}

static void gpu_vk_grave_release(gpu_vk_grave *grave) {
    if (grave->buffer) vkDestroyBuffer(_vk.device, grave->buffer, NULL);
    if (grave->memory) vkFreeMemory(_vk.device, grave->memory, NULL);
    memset(grave, 0, sizeof(*grave));
}

static void gpu_vk_mem_destroy(u32 slot) {
    if (slot >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_vk_buffer *buffer = &_vk.buffers[slot];
    gpu_vk_frame *frame = &_vk.frames[_vk.frame_slot];
    if ((buffer->buffer || buffer->memory) && frame->fence_active &&
        frame->graveyard_count < GPU_VK_GRAVEYARD_CAPACITY) {
        gpu_vk_grave *grave = &frame->graveyard[frame->graveyard_count++];
        grave->buffer = buffer->buffer;
        grave->memory = buffer->memory;
        memset(buffer, 0, sizeof(*buffer));
        return;
    }
    if (buffer->memory && buffer->mapped) vkUnmapMemory(_vk.device, buffer->memory);
    gpu_vk_grave_release(&(gpu_vk_grave){.buffer = buffer->buffer, .memory = buffer->memory});
    memset(buffer, 0, sizeof(*buffer));
}

static void gpu_vk_mem_write(u32 slot, u64 offset, const void *src, u64 size) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !src) return;
    gpu_vk_buffer *buffer = &_vk.buffers[slot];
    if (!buffer->mapped || offset > buffer->logical || size > buffer->logical - offset) return;
    memcpy(buffer->mapped + offset, src, (size_t)size);
}

static ns_bool gpu_vk_mem_read(u32 slot, u64 offset, void *dst, u64 size) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !dst) return false;
    gpu_vk_buffer *buffer = &_vk.buffers[slot];
    if (!buffer->mapped || offset > buffer->logical || size > buffer->logical - offset) return false;
    memcpy(dst, buffer->mapped + offset, (size_t)size);
    return true;
}

static void *gpu_vk_mem_host_ptr(u32 slot) {
    if (slot >= GPU_RESOURCE_POOL_SIZE) return NULL;
    return _vk.buffers[slot].mapped;
}

// ---- textures and samplers --------------------------------------------------

static u32 gpu_vk_texture_create(i32 width, i32 height, i32 depth_or_layers,
                                 i32 format, u32 usage, i32 mip_count, i32 kind) {
    if (width <= 0 || height <= 0 || depth_or_layers <= 0) return 0;
    if (_vk.texture_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    VkFormat vk_format = gpu_vk_format((gpu_pixel_format)format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        ns_warn("gpu", "unsupported texture format %d\n", format);
        return 0;
    }
    ns_bool depth = format == PIXELFORMAT_DEPTH || format == PIXELFORMAT_DEPTH_STENCIL;
    ns_bool transient = (usage & TEXTURE_USAGE_RENDER_TARGET) &&
                        !(usage & (TEXTURE_USAGE_READ | TEXTURE_USAGE_WRITE));
    VkImageUsageFlags image_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depth) image_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & TEXTURE_USAGE_RENDER_TARGET) {
        if (depth) image_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else image_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (usage == TEXTURE_USAGE_DEFAULT || (usage & TEXTURE_USAGE_READ)) image_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & TEXTURE_USAGE_WRITE) image_usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    i32 mips = mip_count > 0 ? mip_count : 1;
    i32 layers = kind == TEXTURE_CUBE ? 6 : (kind == TEXTURE_ARRAY ? depth_or_layers : 1);
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = kind == TEXTURE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = {(u32)width, (u32)height, kind == TEXTURE_3D ? (u32)depth_or_layers : 1},
        .mipLevels = (u32)mips,
        .arrayLayers = (u32)(kind == TEXTURE_3D ? 1 : layers),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = image_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (kind == TEXTURE_CUBE) info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    gpu_vk_texture *texture = &_vk.textures[_vk.texture_count];
    if (vkCreateImage(_vk.device, &info, NULL, &texture->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(_vk.device, texture->image, &requirements);
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = gpu_vk_memory_type(requirements.memoryTypeBits, 0),
    };
    if (vkAllocateMemory(_vk.device, &allocate, NULL, &texture->memory) != VK_SUCCESS) {
        vkDestroyImage(_vk.device, texture->image, NULL);
        memset(texture, 0, sizeof(*texture));
        return 0;
    }
    if (vkBindImageMemory(_vk.device, texture->image, texture->memory, 0) != VK_SUCCESS) {
        vkDestroyImage(_vk.device, texture->image, NULL);
        vkFreeMemory(_vk.device, texture->memory, NULL);
        memset(texture, 0, sizeof(*texture));
        return 0;
    }
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = kind == TEXTURE_3D ? VK_IMAGE_VIEW_TYPE_3D
                                        : (kind == TEXTURE_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                 : (kind == TEXTURE_ARRAY
                                                                        ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                        : VK_IMAGE_VIEW_TYPE_2D)),
        .format = vk_format,
        .subresourceRange = {
            .aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = (u32)mips,
            .baseArrayLayer = 0,
            .layerCount = (u32)(kind == TEXTURE_3D ? 1 : layers),
        },
    };
    if (vkCreateImageView(_vk.device, &view, NULL, &texture->view) != VK_SUCCESS) {
        vkDestroyImage(_vk.device, texture->image, NULL);
        vkFreeMemory(_vk.device, texture->memory, NULL);
        memset(texture, 0, sizeof(*texture));
        return 0;
    }
    texture->vk_format = vk_format;
    texture->width = width;
    texture->height = height;
    texture->depth_layers = depth_or_layers;
    texture->mips = mips;
    texture->layers = kind == TEXTURE_3D ? 1 : layers;
    texture->kind = kind;
    texture->usage = usage;
    texture->depth = depth;
    texture->transient = transient;
    texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return _vk.texture_count++;
}

static void gpu_vk_texture_upload(u32 tex, i32 mip, i32 layer, const void *data, u64 size) {
    if (!gpu_vk_texture_valid(tex) || !data || size == 0 || mip < 0 || layer < 0) return;
    gpu_vk_texture *texture = &_vk.textures[tex];
    if (mip >= texture->mips || layer >= texture->layers) return;

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (vkCreateBuffer(_vk.device, &buffer_info, NULL, &staging) != VK_SUCCESS) return;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(_vk.device, staging, &requirements);
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = gpu_vk_memory_type(requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (vkAllocateMemory(_vk.device, &allocate, NULL, &staging_memory) != VK_SUCCESS) {
        vkDestroyBuffer(_vk.device, staging, NULL);
        return;
    }
    vkBindBufferMemory(_vk.device, staging, staging_memory, 0);
    void *mapped = NULL;
    if (vkMapMemory(_vk.device, staging_memory, 0, size, 0, &mapped) == VK_SUCCESS) {
        memcpy(mapped, data, (size_t)size);
        vkUnmapMemory(_vk.device, staging_memory);
    }

    VkCommandBuffer commands = gpu_vk_oneshot_begin();
    if (commands != VK_NULL_HANDLE) {
        gpu_vk_texture_transition(commands, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        u32 width = (u32)(texture->width >> mip);
        u32 height = (u32)(texture->height >> mip);
        u32 depth = texture->kind == TEXTURE_3D ? (u32)(texture->depth_layers >> mip) : 1;
        if (width == 0) width = 1;
        if (height == 0) height = 1;
        if (depth == 0) depth = 1;
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = texture->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = (u32)mip,
                .baseArrayLayer = (u32)layer,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        };
        vkCmdCopyBufferToImage(commands, staging, texture->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageLayout settled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (texture->usage & TEXTURE_USAGE_WRITE) settled = VK_IMAGE_LAYOUT_GENERAL;
        else if ((texture->usage & TEXTURE_USAGE_RENDER_TARGET) && !(texture->usage & TEXTURE_USAGE_READ)) {
            settled = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        gpu_vk_texture_transition(commands, texture, settled);
        gpu_vk_oneshot_end(commands);
    }
    vkDestroyBuffer(_vk.device, staging, NULL);
    vkFreeMemory(_vk.device, staging_memory, NULL);
}

static void gpu_vk_texture_destroy(u32 tex) {
    if (!tex || tex >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_vk_texture *texture = &_vk.textures[tex];
    if (!texture->image) return;
    vkDeviceWaitIdle(_vk.device);
    if (texture->view) vkDestroyImageView(_vk.device, texture->view, NULL);
    vkDestroyImage(_vk.device, texture->image, NULL);
    vkFreeMemory(_vk.device, texture->memory, NULL);
    memset(texture, 0, sizeof(*texture));
}

static u32 gpu_vk_sampler_create(i32 min_filter, i32 mag_filter, i32 mip_filter,
                                 i32 wrap_u, i32 wrap_v, i32 wrap_w,
                                 i32 compare_func, i32 max_anisotropy) {
    // v2 binds the fixed samplers the generated GLSL declares, so a created
    // sampler resolves to the closest of those rather than owning a new one.
    ns_unused(min_filter);
    ns_unused(mag_filter);
    ns_unused(mip_filter);
    ns_unused(wrap_u);
    ns_unused(wrap_v);
    ns_unused(wrap_w);
    ns_unused(compare_func);
    ns_unused(max_anisotropy);
    return 1;
}

static void gpu_vk_sampler_destroy(u32 smp) {
    ns_unused(smp);
}

// ---- shader creation --------------------------------------------------------

static u32 gpu_vk_shader_graphics_create(const char *vs_src, const char *fs_src,
                                         const char *vs_entry, const char *fs_entry,
                                         const char *name, u64 hash) {
    ns_unused(vs_entry);
    ns_unused(fs_entry);
    ns_unused(hash);
    if (!vs_src || !fs_src) return 0;
    if (_vk.shader_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    VkShaderModule vs = gpu_vk_compile(vs_src, name, shaderc_glsl_vertex_shader);
    VkShaderModule fs = gpu_vk_compile(fs_src, name, shaderc_glsl_fragment_shader);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(_vk.device, vs, NULL);
        if (fs) vkDestroyShaderModule(_vk.device, fs, NULL);
        return 0;
    }
    u32 usage = gpu_vk_source_usage(vs_src) | gpu_vk_source_usage(fs_src);
    u32 storage = gpu_vk_source_storage_slots(vs_src);
    u32 fs_storage = gpu_vk_source_storage_slots(fs_src);
    if (fs_storage > storage) storage = fs_storage;
    if (storage > GPU_VK_STORAGE_SLOT_COUNT) storage = GPU_VK_STORAGE_SLOT_COUNT;
    VkDescriptorSetLayout set_layout = gpu_vk_descriptor_layout(usage, storage);
    if (set_layout == VK_NULL_HANDLE) {
        vkDestroyShaderModule(_vk.device, vs, NULL);
        vkDestroyShaderModule(_vk.device, fs, NULL);
        return 0;
    }
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(_vk.device, &layout_info, NULL, &layout) != VK_SUCCESS) {
        ns_warn("gpu", "failed to create a pipeline layout for %s\n", name);
        vkDestroyShaderModule(_vk.device, vs, NULL);
        vkDestroyShaderModule(_vk.device, fs, NULL);
        return 0;
    }
    u32 id = _vk.shader_count++;
    gpu_vk_shader *shader = &_vk.shaders[id];
    memset(shader, 0, sizeof(*shader));
    shader->vs = vs;
    shader->fs = fs;
    shader->layout = layout;
    shader->set_layout = set_layout;
    shader->usage = usage;
    shader->storage_count = storage;
    snprintf(shader->name, sizeof(shader->name), "%s", name ? name : "shader");
    return id;
}

static u32 gpu_vk_shader_compute_create(const char *src, const char *entry,
                                        const char *name, u64 hash) {
    ns_unused(entry);
    ns_unused(hash);
    if (!src || _vk.shader_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    VkShaderModule cs = gpu_vk_compile(src, name, shaderc_glsl_compute_shader);
    if (!cs) return 0;
    u32 usage = gpu_vk_source_usage(src);
    u32 storage = gpu_vk_source_storage_slots(src);
    if (storage > GPU_VK_STORAGE_SLOT_COUNT) storage = GPU_VK_STORAGE_SLOT_COUNT;
    VkDescriptorSetLayout set_layout = gpu_vk_descriptor_layout(usage, storage);
    if (set_layout == VK_NULL_HANDLE) {
        vkDestroyShaderModule(_vk.device, cs, NULL);
        return 0;
    }
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(_vk.device, &layout_info, NULL, &layout) != VK_SUCCESS) {
        vkDestroyShaderModule(_vk.device, cs, NULL);
        return 0;
    }
    u32 id = _vk.shader_count++;
    gpu_vk_shader *shader = &_vk.shaders[id];
    memset(shader, 0, sizeof(*shader));
    shader->cs = cs;
    shader->layout = layout;
    shader->set_layout = set_layout;
    shader->usage = usage;
    shader->storage_count = storage;
    snprintf(shader->name, sizeof(shader->name), "%s", name ? name : "compute");
    return id;
}

static void gpu_vk_shader_destroy(u32 shader_id) {
    if (!shader_id || shader_id >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_vk_shader *shader = &_vk.shaders[shader_id];
    if (!shader->layout) return;
    vkDeviceWaitIdle(_vk.device);
    if (shader->vs) vkDestroyShaderModule(_vk.device, shader->vs, NULL);
    if (shader->fs) vkDestroyShaderModule(_vk.device, shader->fs, NULL);
    if (shader->cs) vkDestroyShaderModule(_vk.device, shader->cs, NULL);
    for (u32 i = 0; i < shader->pipeline_count; i++) {
        if (shader->pipelines[i].pipeline) vkDestroyPipeline(_vk.device, shader->pipelines[i].pipeline, NULL);
    }
    free(shader->pipelines);
    vkDestroyPipelineLayout(_vk.device, shader->layout, NULL);
    memset(shader, 0, sizeof(*shader));
}

// ---- passes -----------------------------------------------------------------

static void gpu_vk_begin_rendering(const char *label,
                                   VkRenderingAttachmentInfo *colors, u32 color_count,
                                   VkRenderingAttachmentInfo *depth,
                                   VkExtent2D extent) {
    ns_unused(label);
    VkRenderingInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = color_count,
        .pColorAttachments = colors,
        .pDepthAttachment = depth,
    };
    vkCmdBeginRendering(_vk.commands, &info);
    _vk.pass_open = true;
    _vk.pass_extent = extent;
    VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width = (f32)extent.width, .height = (f32)extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(_vk.commands, 0, 1, &viewport);
    VkRect2D scissor = {.offset = {0, 0}, .extent = extent};
    vkCmdSetScissor(_vk.commands, 0, 1, &scissor);
}

static VkAttachmentLoadOp gpu_vk_load_op(u32 load_flags, u32 shift) {
    switch ((load_flags >> shift) & GPU_PASS_LOAD_MASK) {
        case LOAD_ACTION_CLEAR: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LOAD_ACTION_LOAD: return VK_ATTACHMENT_LOAD_OP_LOAD;
        default: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
}

static void gpu_vk_pass_begin(const char *label,
                              u32 color0, u32 color1, u32 color2, u32 color3,
                              u32 depth, u32 load_flags, gpu_color clear, f32 depth_clear) {
    if (!_vk.valid || !_vk.frame_active || !_vk.commands_active || _vk.pass_open) return;
    if (gpu_vk_trace()) gpu_vk_trace_line("nsvk pass %s colors %u %u %u %u depth %u\n", label, color0, color1, color2, color3, depth);
    u32 colors[4] = {color0, color1, color2, color3};
    VkRenderingAttachmentInfo attachments[4];
    memset(attachments, 0, sizeof(attachments));
    VkExtent2D extent = {0, 0};
    for (u32 i = 0; i < 4; i++) {
        _vk.pass_colors[i] = VK_FORMAT_UNDEFINED;
        attachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!gpu_vk_texture_valid(colors[i])) continue;
        gpu_vk_texture *texture = &_vk.textures[colors[i]];
        gpu_vk_texture_transition(_vk.commands, texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        attachments[i].imageView = texture->view;
        attachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[i].loadOp = gpu_vk_load_op(load_flags, i * 2);
        attachments[i].storeOp = texture->transient ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                                    : VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].clearValue.color.float32[0] = clear.r;
        attachments[i].clearValue.color.float32[1] = clear.g;
        attachments[i].clearValue.color.float32[2] = clear.b;
        attachments[i].clearValue.color.float32[3] = clear.a;
        _vk.pass_colors[i] = texture->vk_format;
        extent.width = (u32)texture->width;
        extent.height = (u32)texture->height;
    }
    VkRenderingAttachmentInfo depth_attachment;
    memset(&depth_attachment, 0, sizeof(depth_attachment));
    _vk.pass_depth = VK_FORMAT_UNDEFINED;
    if (gpu_vk_texture_valid(depth)) {
        gpu_vk_texture *texture = &_vk.textures[depth];
        gpu_vk_texture_transition(_vk.commands, texture, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = texture->view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = gpu_vk_load_op(load_flags, GPU_PASS_DEPTH_SHIFT);
        depth_attachment.storeOp = texture->transient ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                                      : VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clearValue.depthStencil.depth = depth_clear;
        _vk.pass_depth = texture->vk_format;
        if (extent.width == 0) {
            extent.width = (u32)texture->width;
            extent.height = (u32)texture->height;
        }
    }
    if (extent.width == 0 || extent.height == 0) return;
    gpu_vk_begin_rendering(label, attachments, 4,
                           _vk.pass_depth != VK_FORMAT_UNDEFINED ? &depth_attachment : NULL,
                           extent);
}

static void gpu_vk_screen_pass_begin(const char *label, gpu_color clear) {
    if (!_vk.valid || !_vk.frame_active || !_vk.commands_active || _vk.pass_open) return;
    if (!_vk.image_acquired) return;
    if (gpu_vk_trace()) gpu_vk_trace_line("nsvk screen pass %s\n", label);
    VkImageLayout *layout = &_vk.swapchain_layout[_vk.image_index];
    if (*layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        gpu_vk_transition_image(_vk.commands, _vk.swapchain_images[_vk.image_index], layout,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    } else if (*layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        gpu_vk_transition_image(_vk.commands, _vk.swapchain_images[_vk.image_index], layout,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    }
    VkRenderingAttachmentInfo attachments[4];
    memset(attachments, 0, sizeof(attachments));
    for (u32 i = 0; i < 4; i++) {
        attachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        _vk.pass_colors[i] = VK_FORMAT_UNDEFINED;
    }
    attachments[0].imageView = _vk.swapchain_views[_vk.image_index];
    attachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].loadOp = _vk.screen_pass_count == 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                       : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].clearValue.color.float32[0] = clear.r;
    attachments[0].clearValue.color.float32[1] = clear.g;
    attachments[0].clearValue.color.float32[2] = clear.b;
    attachments[0].clearValue.color.float32[3] = clear.a;
    _vk.pass_colors[0] = _vk.swapchain_format;
    _vk.pass_depth = VK_FORMAT_UNDEFINED;
    _vk.screen_pass_count++;
    gpu_vk_begin_rendering(label, attachments, 4, NULL, _vk.swapchain_extent);
}

// Trace mode submits and drains after every pass and dispatch, so the last
// "ok" line names the command a GPU hang report is about. The drained buffer
// is idle, so it is reset and reused for the rest of the frame.
static void gpu_vk_trace_drain(void) {
    if (!gpu_vk_trace() || !_vk.commands_active) return;
    VkCommandBuffer commands = _vk.commands;
    vkEndCommandBuffer(commands);
    gpu_vk_submit(commands, VK_NULL_HANDLE);
    vkQueueWaitIdle(_vk.queue);
    vkResetCommandBuffer(commands, 0);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(commands, &begin);
    gpu_vk_trace_line("nsvk ok\n");
}

static void gpu_vk_pass_end(void) {
    if (!_vk.pass_open) return;
    vkCmdEndRendering(_vk.commands);
    _vk.pass_open = false;
    gpu_vk_trace_drain();
}

// ---- binding and drawing ----------------------------------------------------

static void gpu_vk_set_shader(u32 shader) {
    _vk.current_shader = shader;
}

static void gpu_vk_set_state(const gpu_v2_state_desc *desc) {
    if (desc) _vk.current_state = *desc;
}

static void gpu_vk_set_root(u32 slot, u64 offset, gpu_addr addr) {
    ns_unused(addr);
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_vk.buffers[slot].buffer) return;
    _vk.root_slot = slot;
    _vk.root_offset = offset;
    _vk.root_valid = true;
}

static void gpu_vk_set_storage(u32 binding, u32 slot, u64 offset, gpu_addr addr) {
    ns_unused(addr);
    if (binding < GPU_VK_CORE_STORAGE_BINDING_BASE) return;
    u32 index = binding - GPU_VK_CORE_STORAGE_BINDING_BASE;
    if (index >= _vk.storage_slot_count) return;
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_vk.buffers[slot].buffer) return;
    _vk.storage_slots[index] = slot;
    _vk.storage_offsets[index] = offset;
    _vk.storage_valid[index] = true;
}

static void gpu_vk_root_words(u32 *texture0, u32 *texture1, u32 *texture2) {
    *texture0 = 0;
    *texture1 = 0;
    *texture2 = 0;
    if (!_vk.root_valid || _vk.root_slot >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_vk_buffer *buffer = &_vk.buffers[_vk.root_slot];
    if (!buffer->mapped || _vk.root_offset + 12 > buffer->logical) return;
    const f32 *words = (const f32 *)(buffer->mapped + _vk.root_offset);
    if (words[0] > 0.0f) *texture0 = (u32)(words[0] + 0.5f);
    if (words[1] > 0.0f) *texture1 = (u32)(words[1] + 0.5f);
    if (words[2] > 0.0f) *texture2 = (u32)(words[2] + 0.5f);
}

static VkDescriptorSet gpu_vk_build_set(gpu_vk_shader *shader, u32 texture0, u32 texture1, u32 texture2) {
    gpu_vk_frame *frame = &_vk.frames[_vk.frame_slot];
    VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = frame->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &shader->set_layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(_vk.device, &allocate, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkWriteDescriptorSet writes[GPU_VK_STORAGE_SLOT_COUNT + 4];
    VkDescriptorBufferInfo buffer_infos[GPU_VK_STORAGE_SLOT_COUNT + 1];
    VkDescriptorImageInfo image_infos[4];
    u32 write_count = 0;
    u32 buffer_count = 0;
    u32 image_count = 0;

    if (shader->usage & (GPU_VK_USE_ROOT | GPU_VK_USE_SCENE)) {
        const gpu_vk_buffer *buffer = _vk.root_valid ? &_vk.buffers[_vk.root_slot] : NULL;
        VkDescriptorBufferInfo *info = &buffer_infos[buffer_count++];
        if (buffer && buffer->buffer) {
            u64 offset = _vk.root_offset;
            u64 range = buffer->size - offset;
            if (range > GPU_VK_MAX_UNIFORM_RANGE) range = GPU_VK_MAX_UNIFORM_RANGE;
            *info = (VkDescriptorBufferInfo){.buffer = buffer->buffer, .offset = offset, .range = range};
        } else {
            *info = (VkDescriptorBufferInfo){.buffer = _vk.dummy_buffer.buffer, .offset = 0,
                                              .range = GPU_VK_ROOT_BLOCK_SIZE};
        }
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = info,
        };
    }
    for (u32 i = 0; i < shader->storage_count && i < GPU_VK_MAX_STORAGE_SLOTS; i++) {
        VkDescriptorBufferInfo *info = &buffer_infos[buffer_count++];
        if (_vk.storage_valid[i]) {
            const gpu_vk_buffer *buffer = &_vk.buffers[_vk.storage_slots[i]];
            *info = (VkDescriptorBufferInfo){
                .buffer = buffer->buffer,
                .offset = _vk.storage_offsets[i],
                .range = VK_WHOLE_SIZE,
            };
        } else {
            *info = (VkDescriptorBufferInfo){.buffer = _vk.dummy_buffer.buffer, .offset = 0,
                                              .range = GPU_VK_ROOT_BLOCK_SIZE};
        }
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = GPU_VK_STORAGE_BINDING_BASE + i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = info,
        };
    }
    struct { u32 binding; u32 texture; VkSampler sampler; gpu_vk_texture *fallback; } images[] = {
        {0, texture0, _vk.sampler_shadow, &_vk.dummy_depth},
        {1, texture0, _vk.sampler_linear, &_vk.dummy_texture},
        {GPU_VK_MASK_BINDING, texture1, _vk.sampler_nearest, &_vk.dummy_texture},
    };
    u32 image_mask = 0;
    if (shader->usage & GPU_VK_USE_SHADOW) image_mask |= 1;
    if (shader->usage & GPU_VK_USE_TEXTURE_MAP) image_mask |= 2;
    if (shader->usage & GPU_VK_USE_MASK_MAP) image_mask |= 4;
    for (u32 i = 0; i < 3; i++) {
        if (!(image_mask & (1u << i))) continue;
        gpu_vk_texture *texture = gpu_vk_texture_valid(images[i].texture)
                                      ? &_vk.textures[images[i].texture]
                                      : images[i].fallback;
        VkDescriptorImageInfo *info = &image_infos[image_count++];
        *info = (VkDescriptorImageInfo){
            .sampler = images[i].sampler,
            .imageView = texture->view,
            .imageLayout = texture->layout == VK_IMAGE_LAYOUT_GENERAL
                               ? VK_IMAGE_LAYOUT_GENERAL
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = images[i].binding, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = info,
        };
    }
    if (shader->usage & GPU_VK_USE_READ_TEXTURE) {
        gpu_vk_texture *texture = gpu_vk_texture_valid(texture0) ? &_vk.textures[texture0]
                                                                 : &_vk.dummy_texture;
        VkDescriptorImageInfo *info = &image_infos[image_count++];
        *info = (VkDescriptorImageInfo){.sampler = VK_NULL_HANDLE, .imageView = texture->view,
                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = info,
        };
    }
    if (shader->usage & GPU_VK_USE_WRITE_TEXTURE) {
        gpu_vk_texture *texture = gpu_vk_texture_valid(texture1) ? &_vk.textures[texture1] : &_vk.dummy_texture;
        VkDescriptorImageInfo *info = &image_infos[image_count++];
        *info = (VkDescriptorImageInfo){.sampler = VK_NULL_HANDLE, .imageView = texture->view,
                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = info,
        };
    }
    if (shader->usage & GPU_VK_USE_SECONDARY_WRITE_TEXTURE) {
        gpu_vk_texture *texture = gpu_vk_texture_valid(texture2) ? &_vk.textures[texture2] : &_vk.dummy_texture;
        VkDescriptorImageInfo *info = &image_infos[image_count++];
        *info = (VkDescriptorImageInfo){.sampler = VK_NULL_HANDLE, .imageView = texture->view,
                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = 15, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = info,
        };
    }
    if (write_count > 0) vkUpdateDescriptorSets(_vk.device, write_count, writes, 0, NULL);
    return set;
}

static gpu_vk_shader *gpu_vk_current_shader(void) {
    if (!_vk.current_shader || _vk.current_shader >= _vk.shader_count) return NULL;
    return &_vk.shaders[_vk.current_shader];
}

static ns_bool gpu_vk_prepare_graphics(void) {
    if (!_vk.pass_open) return false;
    gpu_vk_shader *shader = gpu_vk_current_shader();
    if (!shader || !shader->vs || !shader->fs) return false;
    VkPipeline pipeline = gpu_vk_graphics_pipeline(shader);
    if (!pipeline) return false;

    u32 texture0, texture1, texture2;
    gpu_vk_root_words(&texture0, &texture1, &texture2);
    if ((shader->usage & GPU_VK_USE_SHADOW) && gpu_vk_texture_valid(texture0)) {
        gpu_vk_texture_transition(_vk.commands, &_vk.textures[texture0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if ((shader->usage & GPU_VK_USE_TEXTURE_MAP) && gpu_vk_texture_valid(texture0)) {
        gpu_vk_texture_transition(_vk.commands, &_vk.textures[texture0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if ((shader->usage & GPU_VK_USE_MASK_MAP) && gpu_vk_texture_valid(texture1)) {
        gpu_vk_texture_transition(_vk.commands, &_vk.textures[texture1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkDescriptorSet set = gpu_vk_build_set(shader, texture0, texture1, texture2);
    if (set == VK_NULL_HANDLE) return false;
    vkCmdBindPipeline(_vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(_vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shader->layout,
                            0, 1, &set, 0, NULL);
    return true;
}

static ns_bool gpu_vk_prepare_compute(gpu_vk_shader *shader) {
    VkPipeline pipeline = gpu_vk_compute_pipeline(shader);
    if (!pipeline) return false;
    u32 texture0, texture1, texture2;
    gpu_vk_root_words(&texture0, &texture1, &texture2);
    if ((shader->usage & GPU_VK_USE_READ_TEXTURE) && gpu_vk_texture_valid(texture0)) {
        gpu_vk_texture_transition(_vk.commands, &_vk.textures[texture0], VK_IMAGE_LAYOUT_GENERAL);
    }
    if ((shader->usage & GPU_VK_USE_WRITE_TEXTURE) && gpu_vk_texture_valid(texture1)) {
        gpu_vk_texture_transition(_vk.commands, &_vk.textures[texture1], VK_IMAGE_LAYOUT_GENERAL);
    }
    if ((shader->usage & GPU_VK_USE_SECONDARY_WRITE_TEXTURE) && gpu_vk_texture_valid(texture2)) {
        gpu_vk_texture_transition(_vk.commands, &_vk.textures[texture2], VK_IMAGE_LAYOUT_GENERAL);
    }
    VkDescriptorSet set = gpu_vk_build_set(shader, texture0, texture1, texture2);
    if (set == VK_NULL_HANDLE) return false;
    vkCmdBindPipeline(_vk.commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(_vk.commands, VK_PIPELINE_BIND_POINT_COMPUTE, shader->layout,
                            0, 1, &set, 0, NULL);
    return true;
}

static void gpu_vk_draw(i32 vertex_base, i32 vertex_count, i32 instance_count) {
    if (!gpu_vk_prepare_graphics()) return;
    vkCmdDraw(_vk.commands, (u32)vertex_count, (u32)instance_count, (u32)vertex_base, 0);
}

static void gpu_vk_draw_indexed(u32 slot, u64 offset, i32 index_type,
                                i32 index_count, i32 instance_count, i32 base_vertex) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_vk.buffers[slot].buffer) return;
    if (!gpu_vk_prepare_graphics()) return;
    vkCmdBindIndexBuffer(_vk.commands, _vk.buffers[slot].buffer, offset,
                         index_type == INDEX_UINT32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(_vk.commands, (u32)index_count, (u32)instance_count, 0, base_vertex, 0);
}

static void gpu_vk_draw_indirect(u32 slot, u64 offset, i32 draw_count, i32 stride) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_vk.buffers[slot].buffer) return;
    if (!gpu_vk_prepare_graphics()) return;
    u64 step = stride > 0 ? (u64)stride : sizeof(VkDrawIndirectCommand);
    for (i32 i = 0; i < draw_count; i++) {
        vkCmdDrawIndirect(_vk.commands, _vk.buffers[slot].buffer, offset + (u64)i * step, 1, 0);
    }
}

static void gpu_vk_dispatch(const char *label, i32 x, i32 y, i32 z) {
    if (!_vk.frame_active || !_vk.commands_active || x <= 0 || y <= 0 || z <= 0) return;
    gpu_vk_shader *shader = gpu_vk_current_shader();
    if (!shader || !shader->cs) return;
    if (gpu_vk_trace()) gpu_vk_trace_line("nsvk dispatch %s %dx%dx%d shader %u\n", label, x, y, z, _vk.current_shader);
    if (_vk.pass_open) {
        vkCmdEndRendering(_vk.commands);
        _vk.pass_open = false;
    }
    if (!gpu_vk_prepare_compute(shader)) return;
    // The generated GLSL declares an 8 x 8 x 1 workgroup; kernels bounds-check
    // the edge groups, as the WGSL path already requires.
    u32 groups_x = ((u32)x + 7) / 8;
    u32 groups_y = ((u32)y + 7) / 8;
    vkCmdDispatch(_vk.commands, groups_x, groups_y, (u32)z);
    gpu_vk_trace_drain();
}

static void gpu_vk_dispatch_indirect(const char *label, u32 slot, u64 offset) {
    ns_unused(label);
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_vk.buffers[slot].buffer) return;
    if (!_vk.frame_active || !_vk.commands_active) return;
    gpu_vk_shader *shader = gpu_vk_current_shader();
    if (!shader || !shader->cs) return;
    if (_vk.pass_open) {
        vkCmdEndRendering(_vk.commands);
        _vk.pass_open = false;
    }
    if (!gpu_vk_prepare_compute(shader)) return;
    vkCmdDispatchIndirect(_vk.commands, _vk.buffers[slot].buffer, offset);
}

static const gpu_v2_ops _vulkan_v2_ops = {
    .mem_create = gpu_vk_mem_create,
    .mem_destroy = gpu_vk_mem_destroy,
    .mem_write = gpu_vk_mem_write,
    .mem_read = gpu_vk_mem_read,
    .mem_host_ptr = gpu_vk_mem_host_ptr,
    .texture_create = gpu_vk_texture_create,
    .texture_upload = gpu_vk_texture_upload,
    .texture_destroy = gpu_vk_texture_destroy,
    .sampler_create = gpu_vk_sampler_create,
    .sampler_destroy = gpu_vk_sampler_destroy,
    .shader_graphics_create = gpu_vk_shader_graphics_create,
    .shader_compute_create = gpu_vk_shader_compute_create,
    .shader_destroy = gpu_vk_shader_destroy,
    .pass_begin = gpu_vk_pass_begin,
    .screen_pass_begin = gpu_vk_screen_pass_begin,
    .pass_end = gpu_vk_pass_end,
    .set_shader = gpu_vk_set_shader,
    .set_state = gpu_vk_set_state,
    .set_root = gpu_vk_set_root,
    .set_storage = gpu_vk_set_storage,
    .draw = gpu_vk_draw,
    .draw_indexed = gpu_vk_draw_indexed,
    .draw_indirect = gpu_vk_draw_indirect,
    .dispatch = gpu_vk_dispatch,
    .dispatch_indirect = gpu_vk_dispatch_indirect,
};

#endif // NS_GPU_VULKAN
