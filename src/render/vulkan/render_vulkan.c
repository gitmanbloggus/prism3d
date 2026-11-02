/* render_vulkan.c: tier T3 backend on Vulkan 1.0 core (no extensions beyond surface/swapchain), so it
 * runs on the oldest Vulkan drivers. Raster and the raytraced pass share src/render/shaders with the
 * GLES3 backend; SPIR-V is embedded at build time (tools/shaders.py).
 *
 * Frame structure: FRAMES frames in flight, each with its own command buffer, fence, "image acquired"
 * semaphore and a host-visible uniform buffer holding [frame block | RT block | draw ring]. Per-draw
 * data goes to the ring through one dynamic uniform descriptor. Set the environment variable
 * P3D_VK_VALIDATION=1 to enable the Khronos validation layer. */
#include "../p3d_render.h"
#include "../rt_gpu.h"
#include "../../core/p3d_core.h"
#include "shaders_vk.h"

#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 2
#define MAX_IMAGES 8
#define MAX_MESHES 1024
#define MAX_TEXTURES 256
#define MAX_DRAWS 4096

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *mapped; VkDeviceSize size; } vk_buffer;
typedef struct { vk_buffer vb, ib; uint32_t count; int used; } vk_mesh;
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorSet set; int used; } vk_texture;

/* std140 mirrors of FrameBlock / DrawBlock (mesh_common.glsl) */
typedef struct {
    float ambient[4];
    float light_info[4];
    float light_vec[P3D_MAX_LIGHTS][4];
    float light_color[P3D_MAX_LIGHTS][4];
} frame_block;
typedef struct { float mvp[16], model[16], color[4], flags[4]; } draw_block;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    vk_buffer ubo;
    VkDescriptorSet set0;
    uint32_t draw_count;
} vk_frame;

static struct {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkDeviceSize ubo_align, draw_stride, rt_offset, ring_offset;

    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkFramebuffer framebuffers[MAX_IMAGES];
    VkSemaphore render_done[MAX_IMAGES];
    int can_read_back;
    VkImage depth;
    VkDeviceMemory depth_mem;
    VkImageView depth_view;
    VkFormat depth_format;

    VkRenderPass pass;
    VkDescriptorSetLayout set0_layout, tex_layout;
    VkPipelineLayout layout;
    VkPipeline mesh_pipe, rt_pipe;
    VkDescriptorPool pool;
    VkSampler sampler;
    VkCommandPool cmd_pool;

    vk_frame fr[FRAMES];
    uint32_t fi, image_index;
    int in_frame, submitted, swapchain_ok;

    vk_mesh meshes[MAX_MESHES];
    vk_texture textures[MAX_TEXTURES];
    p3d_texture_id white;
    vk_buffer readback;

    p3d_frame frame;
    p3d_mat4 view_proj;
} vk;

#define VK_CHECK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    P3D_ERROR("vulkan: %s failed (%d)", what, (int)r_); return 0; } } while (0)

/* ---------------------------------------------------------------- memory helpers */

static int find_memory(uint32_t type_bits, VkMemoryPropertyFlags props, uint32_t *index)
{
    uint32_t i;
    for (i = 0; i < vk.memprops.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (vk.memprops.memoryTypes[i].propertyFlags & props) == props) {
            *index = i;
            return 1;
        }
    return 0;
}

static int buffer_create(vk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    memset(b, 0, sizeof(*b));
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &b->buf), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(vk.device, b->buf, &req);
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    if (!find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &ai.memoryTypeIndex)) {
        P3D_ERROR("vulkan: no host-visible memory");
        return 0;
    }
    VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &b->mem), "vkAllocateMemory");
    VK_CHECK(vkBindBufferMemory(vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(vkMapMemory(vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->mapped), "vkMapMemory");
    b->size = size;
    return 1;
}

static void buffer_destroy(vk_buffer *b)
{
    if (b->mem) vkFreeMemory(vk.device, b->mem, NULL);   /* unmaps implicitly */
    if (b->buf) vkDestroyBuffer(vk.device, b->buf, NULL);
    memset(b, 0, sizeof(*b));
}

static int image_create(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vi;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &ii, NULL, img), "vkCreateImage");
    vkGetImageMemoryRequirements(vk.device, *img, &req);
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    if (!find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ai.memoryTypeIndex) &&
        !find_memory(req.memoryTypeBits, 0, &ai.memoryTypeIndex)) return 0;
    VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, mem), "vkAllocateMemory (image)");
    VK_CHECK(vkBindImageMemory(vk.device, *img, *mem, 0), "vkBindImageMemory");
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, view), "vkCreateImageView");
    return 1;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* one-shot command buffer for uploads; waits for completion */
static VkCommandBuffer one_shot_begin(void)
{
    VkCommandBufferAllocateInfo ai;
    VkCommandBufferBeginInfo bi;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vk.cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk.device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

static void one_shot_end(VkCommandBuffer cmd)
{
    VkSubmitInfo si;
    vkEndCommandBuffer(cmd);
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.queue);
    vkFreeCommandBuffers(vk.device, vk.cmd_pool, 1, &cmd);
}

/* ---------------------------------------------------------------- instance and device */

static int create_instance(void)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    unsigned count = 0;
    const char *const *exts = p3d_platform_vk_instance_extensions(&count);
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *env = getenv("P3D_VK_VALIDATION");
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Prism3D";
    app.pEngineName = "Prism3D";
    app.engineVersion = VK_MAKE_VERSION(P3D_VERSION_MAJOR, P3D_VERSION_MINOR, 0);
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = count;
    ci.ppEnabledExtensionNames = exts;
    if (env != NULL && env[0] == '1') {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    if (exts == NULL) { P3D_ERROR("vulkan: the platform has no Vulkan surface support"); return 0; }
    if (vkCreateInstance(&ci, NULL, &vk.instance) != VK_SUCCESS && ci.enabledLayerCount) {
        P3D_WARN("vulkan: validation layer unavailable, continuing without it");
        ci.enabledLayerCount = 0;
        vk.instance = VK_NULL_HANDLE;
    }
    if (vk.instance == VK_NULL_HANDLE) VK_CHECK(vkCreateInstance(&ci, NULL, &vk.instance), "vkCreateInstance");
    return 1;
}

static int pick_device(void)
{
    VkPhysicalDevice gpus[16];
    uint32_t n = 16, i, best_score = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(vk.instance, &n, gpus), "vkEnumeratePhysicalDevices");
    for (i = 0; i < n; ++i) {
        VkQueueFamilyProperties qf[16];
        VkPhysicalDeviceProperties props;
        uint32_t qn = 16, q, score;
        vkGetPhysicalDeviceProperties(gpus[i], &props);
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qn, qf);
        for (q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, vk.surface, &present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) break;
        }
        if (q == qn) continue;
        score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
              : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            vk.gpu = gpus[i];
            vk.queue_family = q;
        }
    }
    if (vk.gpu == VK_NULL_HANDLE) { P3D_ERROR("vulkan: no GPU can present to this window"); return 0; }
    return 1;
}

static int create_device(void)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo di;
    VkPhysicalDeviceProperties props;
    const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    memset(&qi, 0, sizeof(qi));
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = vk.queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    memset(&di, 0, sizeof(di));
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = &ext;
    VK_CHECK(vkCreateDevice(vk.gpu, &di, NULL, &vk.device), "vkCreateDevice");
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &vk.memprops);
    vkGetPhysicalDeviceProperties(vk.gpu, &props);
    P3D_INFO("Vulkan: %s (API %u.%u.%u)", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
             VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    vk.ubo_align = props.limits.minUniformBufferOffsetAlignment;
    if (vk.ubo_align < 16) vk.ubo_align = 16;
    return 1;
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

/* ---------------------------------------------------------------- swapchain */

static void destroy_swapchain(void)
{
    uint32_t i;
    for (i = 0; i < vk.image_count; ++i) {
        if (vk.framebuffers[i]) vkDestroyFramebuffer(vk.device, vk.framebuffers[i], NULL);
        if (vk.views[i]) vkDestroyImageView(vk.device, vk.views[i], NULL);
        if (vk.render_done[i]) vkDestroySemaphore(vk.device, vk.render_done[i], NULL);
        vk.framebuffers[i] = VK_NULL_HANDLE;
        vk.views[i] = VK_NULL_HANDLE;
        vk.render_done[i] = VK_NULL_HANDLE;
    }
    if (vk.depth_view) vkDestroyImageView(vk.device, vk.depth_view, NULL);
    if (vk.depth) vkDestroyImage(vk.device, vk.depth, NULL);
    if (vk.depth_mem) vkFreeMemory(vk.device, vk.depth_mem, NULL);
    vk.depth_view = VK_NULL_HANDLE;
    vk.depth = VK_NULL_HANDLE;
    vk.depth_mem = VK_NULL_HANDLE;
    vk.image_count = 0;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR sc;
    VkSurfaceFormatKHR formats[32];
    uint32_t nf = 32, i;
    VkSwapchainCreateInfoKHR ci;
    VkSwapchainKHR old = vk.swapchain;
    int w, h;

    vkDeviceWaitIdle(vk.device);
    destroy_swapchain();
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &sc), "surface capabilities");
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &nf, formats), "surface formats");
    /* UNORM (not sRGB) so colours match the GL backends */
    vk.format = formats[0].format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    for (i = 0; i < nf; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            vk.format = formats[i].format;
            break;
        }
    if (sc.currentExtent.width != 0xFFFFFFFFu) vk.extent = sc.currentExtent;
    else {
        p3d_platform_drawable_size(&w, &h);
        vk.extent.width = (uint32_t)w;
        vk.extent.height = (uint32_t)h;
    }
    if (vk.extent.width == 0 || vk.extent.height == 0) return 0;   /* minimized: try again later */

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = vk.surface;
    ci.minImageCount = sc.minImageCount + 1;
    if (sc.maxImageCount && ci.minImageCount > sc.maxImageCount) ci.minImageCount = sc.maxImageCount;
    if (ci.minImageCount > MAX_IMAGES) ci.minImageCount = MAX_IMAGES;
    ci.imageFormat = vk.format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = vk.extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    vk.can_read_back = (sc.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (vk.can_read_back) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = sc.currentTransform;
    ci.compositeAlpha = (sc.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)sc.supportedCompositeAlpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;      /* always supported; vsync */
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(vk.device, &ci, NULL, &vk.swapchain), "vkCreateSwapchainKHR");
    if (old) vkDestroySwapchainKHR(vk.device, old, NULL);

    vk.image_count = MAX_IMAGES;
    VK_CHECK(vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, vk.images), "swapchain images");

    if (!image_create(vk.extent.width, vk.extent.height, vk.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT, &vk.depth, &vk.depth_mem, &vk.depth_view)) return 0;

    for (i = 0; i < vk.image_count; ++i) {
        VkImageViewCreateInfo vi;
        VkFramebufferCreateInfo fi;
        VkSemaphoreCreateInfo si;
        VkImageView att[2];
        memset(&vi, 0, sizeof(vi));
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = vk.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vk.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, &vk.views[i]), "swapchain view");
        att[0] = vk.views[i];
        att[1] = vk.depth_view;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = vk.pass;
        fi.attachmentCount = 2;
        fi.pAttachments = att;
        fi.width = vk.extent.width;
        fi.height = vk.extent.height;
        fi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(vk.device, &fi, NULL, &vk.framebuffers[i]), "vkCreateFramebuffer");
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(vk.device, &si, NULL, &vk.render_done[i]), "vkCreateSemaphore");
    }
    vk.swapchain_ok = 1;
    return 1;
}

/* ---------------------------------------------------------------- render pass and pipelines */

static int create_render_pass(void)
{
    static const VkFormat depth_formats[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32,
                                              VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };
    VkAttachmentDescription att[2];
    VkAttachmentReference color_ref, depth_ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo ci;
    VkSurfaceFormatKHR formats[32];
    uint32_t nf = 32, i;

    for (i = 0; i < P3D_ARRAY_LEN(depth_formats); ++i) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(vk.gpu, depth_formats[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            vk.depth_format = depth_formats[i];
            break;
        }
    }
    if (i == P3D_ARRAY_LEN(depth_formats)) { P3D_ERROR("vulkan: no depth format"); return 0; }
    /* the render pass needs the colour format before the swapchain exists: pick it the same way */
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &nf, formats), "surface formats");
    vk.format = formats[0].format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    for (i = 0; i < nf; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            vk.format = formats[i].format;
            break;
        }

    memset(att, 0, sizeof(att));
    att[0].format = vk.format;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att[1].format = vk.depth_format;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;
    /* wait for the previous frame's use of the (shared) depth buffer and the acquired image */
    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 2;
    ci.pAttachments = att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(vk.device, &ci, NULL, &vk.pass), "vkCreateRenderPass");
    return 1;
}

static VkShaderModule shader_module(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = code;
    if (vkCreateShaderModule(vk.device, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static int create_layouts(void)
{
    VkDescriptorSetLayoutBinding b[3], t;
    VkDescriptorSetLayoutCreateInfo li;
    VkPipelineLayoutCreateInfo pi;
    VkDescriptorSetLayout sets[2];
    VkDescriptorPoolSize sizes[3];
    VkDescriptorPoolCreateInfo dp;
    VkSamplerCreateInfo si;
    VkCommandPoolCreateInfo cp;
    int i;

    memset(b, 0, sizeof(b));
    for (i = 0; i < 3; ++i) {
        b[i].binding = (uint32_t)i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    memset(&li, 0, sizeof(li));
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &li, NULL, &vk.set0_layout), "set 0 layout");
    memset(&t, 0, sizeof(t));
    t.binding = 0;
    t.descriptorCount = 1;
    t.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    t.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.bindingCount = 1;
    li.pBindings = &t;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &li, NULL, &vk.tex_layout), "texture layout");

    sets[0] = vk.set0_layout;
    sets[1] = vk.tex_layout;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 2;
    pi.pSetLayouts = sets;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &pi, NULL, &vk.layout), "pipeline layout");

    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = 2 * FRAMES;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[1].descriptorCount = FRAMES;
    sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[2].descriptorCount = MAX_TEXTURES;
    memset(&dp, 0, sizeof(dp));
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = FRAMES + MAX_TEXTURES;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &dp, NULL, &vk.pool), "descriptor pool");

    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vk.sampler), "vkCreateSampler");

    memset(&cp, 0, sizeof(cp));
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = vk.queue_family;
    VK_CHECK(vkCreateCommandPool(vk.device, &cp, NULL, &vk.cmd_pool), "vkCreateCommandPool");
    return 1;
}

static int create_pipeline(VkPipeline *out, const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size,
                           int mesh)
{
    VkShaderModule vm = shader_module(vs, vs_size), fm = shader_module(fs, fs_size);
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[3];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo pi;
    VkResult r;
    if (!vm || !fm) { P3D_ERROR("vulkan: bad shader module"); return 0; }

    memset(stages, 0, sizeof(stages));
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vm;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fm;
    stages[1].pName = "main";

    vb.binding = 0;
    vb.stride = sizeof(p3d_vertex);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32B32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R32G32B32_SFLOAT; va[1].offset = sizeof(float) * 3;
    va[2].location = 2; va[2].binding = 0; va[2].format = VK_FORMAT_R32G32_SFLOAT; va[2].offset = sizeof(float) * 6;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (mesh) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = va;
    }
    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof(vp));
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = mesh ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;   /* mesh.vert flips y in clip space, which keeps CCW */
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof(ds));
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = mesh ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = mesh ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof(dy));
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = vk.layout;
    pi.renderPass = vk.pass;
    r = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pi, NULL, out);
    vkDestroyShaderModule(vk.device, vm, NULL);
    vkDestroyShaderModule(vk.device, fm, NULL);
    if (r != VK_SUCCESS) { P3D_ERROR("vulkan: vkCreateGraphicsPipelines failed (%d)", (int)r); return 0; }
    return 1;
}

static int create_frames(void)
{
    VkCommandBufferAllocateInfo ai;
    VkCommandBuffer cmds[FRAMES];
    VkDescriptorSetLayout layouts[FRAMES];
    VkDescriptorSetAllocateInfo dsa;
    VkDescriptorSet sets[FRAMES];
    VkDeviceSize size;
    int i;

    vk.draw_stride = align_up(sizeof(draw_block), vk.ubo_align);
    vk.rt_offset = align_up(sizeof(frame_block), vk.ubo_align);
    vk.ring_offset = align_up(vk.rt_offset + sizeof(p3d_rt_gpu), vk.ubo_align);
    size = vk.ring_offset + vk.draw_stride * MAX_DRAWS;

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vk.cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, cmds), "vkAllocateCommandBuffers");
    for (i = 0; i < FRAMES; ++i) layouts[i] = vk.set0_layout;
    memset(&dsa, 0, sizeof(dsa));
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = vk.pool;
    dsa.descriptorSetCount = FRAMES;
    dsa.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsa, sets), "vkAllocateDescriptorSets");

    for (i = 0; i < FRAMES; ++i) {
        vk_frame *f = &vk.fr[i];
        VkFenceCreateInfo fi;
        VkSemaphoreCreateInfo si;
        VkDescriptorBufferInfo bi[3];
        VkWriteDescriptorSet w[3];
        int k;
        f->cmd = cmds[i];
        f->set0 = sets[i];
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &f->fence), "vkCreateFence");
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(vk.device, &si, NULL, &f->acquired), "vkCreateSemaphore");
        if (!buffer_create(&f->ubo, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return 0;
        bi[0].buffer = bi[1].buffer = bi[2].buffer = f->ubo.buf;
        bi[0].offset = 0;               bi[0].range = sizeof(frame_block);
        bi[1].offset = vk.ring_offset;  bi[1].range = sizeof(draw_block);
        bi[2].offset = vk.rt_offset;    bi[2].range = sizeof(p3d_rt_gpu);
        memset(w, 0, sizeof(w));
        for (k = 0; k < 3; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = f->set0;
            w[k].dstBinding = (uint32_t)k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = k == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[k].pBufferInfo = &bi[k];
        }
        vkUpdateDescriptorSets(vk.device, 3, w, 0, NULL);
    }
    return 1;
}

/* ---------------------------------------------------------------- backend interface */

static void vk_shutdown(void);
static p3d_texture_id vk_texture_create(int width, int height, const uint8_t *rgba);

static int vk_init(p3d_caps *caps)
{
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    VkPhysicalDeviceProperties props;
    memset(&vk, 0, sizeof(vk));
    if (!create_instance() ||
        !p3d_platform_vk_create_surface(vk.instance, &vk.surface) ||
        !pick_device() || !create_device() || !create_render_pass() || !create_layouts() ||
        !create_pipeline(&vk.mesh_pipe, p3d_spv_mesh_vert, sizeof(p3d_spv_mesh_vert),
                         p3d_spv_mesh_frag, sizeof(p3d_spv_mesh_frag), 1) ||
        !create_pipeline(&vk.rt_pipe, p3d_spv_rt_vert, sizeof(p3d_spv_rt_vert),
                         p3d_spv_rt_frag, sizeof(p3d_spv_rt_frag), 0) ||
        !create_frames() || !create_swapchain()) {
        vk_shutdown();
        return 0;
    }
    vk.white = vk_texture_create(1, 1, white);
    if (!vk.white) { vk_shutdown(); return 0; }
    vkGetPhysicalDeviceProperties(vk.gpu, &props);
    caps->max_texture_size = (int)props.limits.maxImageDimension2D;
    caps->npot_textures = 1;
    caps->max_lights = P3D_MAX_LIGHTS;
    caps->per_pixel_lighting = 1;
    caps->raytracing = props.limits.maxUniformBufferRange >= sizeof(p3d_rt_gpu);
    return 1;
}

static void vk_texture_destroy(p3d_texture_id id);
static void vk_mesh_destroy(p3d_mesh_id id);

static void vk_shutdown(void)
{
    int i;
    if (vk.device) {
        vkDeviceWaitIdle(vk.device);
        for (i = 0; i < MAX_MESHES; ++i) vk_mesh_destroy((p3d_mesh_id)(i + 1));
        for (i = 0; i < MAX_TEXTURES; ++i) vk_texture_destroy((p3d_texture_id)(i + 1));
        buffer_destroy(&vk.readback);
        destroy_swapchain();
        if (vk.swapchain) vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
        for (i = 0; i < FRAMES; ++i) {
            buffer_destroy(&vk.fr[i].ubo);
            if (vk.fr[i].fence) vkDestroyFence(vk.device, vk.fr[i].fence, NULL);
            if (vk.fr[i].acquired) vkDestroySemaphore(vk.device, vk.fr[i].acquired, NULL);
        }
        if (vk.cmd_pool) vkDestroyCommandPool(vk.device, vk.cmd_pool, NULL);
        if (vk.sampler) vkDestroySampler(vk.device, vk.sampler, NULL);
        if (vk.pool) vkDestroyDescriptorPool(vk.device, vk.pool, NULL);
        if (vk.mesh_pipe) vkDestroyPipeline(vk.device, vk.mesh_pipe, NULL);
        if (vk.rt_pipe) vkDestroyPipeline(vk.device, vk.rt_pipe, NULL);
        if (vk.layout) vkDestroyPipelineLayout(vk.device, vk.layout, NULL);
        if (vk.set0_layout) vkDestroyDescriptorSetLayout(vk.device, vk.set0_layout, NULL);
        if (vk.tex_layout) vkDestroyDescriptorSetLayout(vk.device, vk.tex_layout, NULL);
        if (vk.pass) vkDestroyRenderPass(vk.device, vk.pass, NULL);
        vkDestroyDevice(vk.device, NULL);
    }
    if (vk.surface) vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    if (vk.instance) vkDestroyInstance(vk.instance, NULL);
    memset(&vk, 0, sizeof(vk));
}

static p3d_mesh_id vk_mesh_create(const p3d_vertex *v, int vertex_count, const uint16_t *idx, int index_count)
{
    vk_mesh *m;
    int i;
    for (i = 0; i < MAX_MESHES; ++i)
        if (!vk.meshes[i].used) break;
    if (i == MAX_MESHES) return 0;
    m = &vk.meshes[i];
    /* host-visible buffers: simple, and fine for the small meshes the engine has today */
    if (!buffer_create(&m->vb, sizeof(*v) * (size_t)vertex_count, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !buffer_create(&m->ib, sizeof(*idx) * (size_t)index_count, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        buffer_destroy(&m->vb);
        buffer_destroy(&m->ib);
        return 0;
    }
    memcpy(m->vb.mapped, v, sizeof(*v) * (size_t)vertex_count);
    memcpy(m->ib.mapped, idx, sizeof(*idx) * (size_t)index_count);
    m->count = (uint32_t)index_count;
    m->used = 1;
    return (p3d_mesh_id)(i + 1);
}

static void vk_mesh_destroy(p3d_mesh_id id)
{
    vk_mesh *m;
    if (id == 0 || id > MAX_MESHES || !vk.meshes[id - 1].used) return;
    m = &vk.meshes[id - 1];
    vkDeviceWaitIdle(vk.device);   /* it may be in flight; destroying meshes is rare */
    buffer_destroy(&m->vb);
    buffer_destroy(&m->ib);
    m->used = 0;
}

static p3d_texture_id vk_texture_create(int width, int height, const uint8_t *rgba)
{
    vk_texture *t;
    vk_buffer staging;
    VkCommandBuffer cmd;
    VkBufferImageCopy copy;
    VkDescriptorSetAllocateInfo dsa;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    int i;
    for (i = 0; i < MAX_TEXTURES; ++i)
        if (!vk.textures[i].used) break;
    if (i == MAX_TEXTURES) return 0;
    t = &vk.textures[i];
    if (!image_create((uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      &t->img, &t->mem, &t->view)) return 0;
    if (!buffer_create(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    memcpy(staging.mapped, rgba, bytes);
    cmd = one_shot_begin();
    barrier(cmd, t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)width;
    copy.imageExtent.height = (uint32_t)height;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging.buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(cmd);
    buffer_destroy(&staging);

    memset(&dsa, 0, sizeof(dsa));
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = vk.pool;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &vk.tex_layout;
    if (vkAllocateDescriptorSets(vk.device, &dsa, &t->set) != VK_SUCCESS) return 0;
    ii.sampler = vk.sampler;
    ii.imageView = t->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w, 0, sizeof(w));
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = t->set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, NULL);
    t->used = 1;
    return (p3d_texture_id)(i + 1);
}

static void vk_texture_destroy(p3d_texture_id id)
{
    vk_texture *t;
    if (id == 0 || id > MAX_TEXTURES || !vk.textures[id - 1].used) return;
    t = &vk.textures[id - 1];
    vkDeviceWaitIdle(vk.device);
    vkFreeDescriptorSets(vk.device, vk.pool, 1, &t->set);
    vkDestroyImageView(vk.device, t->view, NULL);
    vkDestroyImage(vk.device, t->img, NULL);
    vkFreeMemory(vk.device, t->mem, NULL);
    memset(t, 0, sizeof(*t));
}

static void vk_begin_frame(const p3d_frame *f)
{
    vk_frame *fr = &vk.fr[vk.fi];
    VkCommandBufferBeginInfo bi;
    VkRenderPassBeginInfo rp;
    VkClearValue clear[2];
    VkViewport viewport;
    VkRect2D scissor;
    frame_block *fb;
    VkResult r;
    int i, n = f->light_count < P3D_MAX_LIGHTS ? f->light_count : P3D_MAX_LIGHTS;

    vk.in_frame = vk.submitted = 0;
    vk.frame = *f;
    vk.view_proj = p3d_m4_mul(f->proj, f->view);
    if (!vk.swapchain_ok && !create_swapchain()) return;

    vkWaitForFences(vk.device, 1, &fr->fence, VK_TRUE, UINT64_MAX);
    r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, fr->acquired, VK_NULL_HANDLE, &vk.image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk.swapchain_ok = 0; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { P3D_ERROR("vulkan: acquire failed (%d)", (int)r); return; }
    vkResetFences(vk.device, 1, &fr->fence);

    fb = (frame_block *)fr->ubo.mapped;
    memset(fb, 0, sizeof(*fb));
    memcpy(fb->ambient, f->ambient, sizeof(float) * 3);
    fb->light_info[0] = (float)n;
    for (i = 0; i < n; ++i) {
        fb->light_vec[i][0] = f->lights[i].vector.x;
        fb->light_vec[i][1] = f->lights[i].vector.y;
        fb->light_vec[i][2] = f->lights[i].vector.z;
        fb->light_vec[i][3] = f->lights[i].kind == P3D_LIGHT_POINT ? 1.0f : 0.0f;
        memcpy(fb->light_color[i], f->lights[i].color, sizeof(float) * 3);
        fb->light_color[i][3] = f->lights[i].range;
    }
    fr->draw_count = 0;

    vkResetCommandBuffer(fr->cmd, 0);
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fr->cmd, &bi);
    clear[0].color.float32[0] = f->clear_color[0];
    clear[0].color.float32[1] = f->clear_color[1];
    clear[0].color.float32[2] = f->clear_color[2];
    clear[0].color.float32[3] = 1.0f;
    clear[1].depthStencil.depth = 1.0f;
    clear[1].depthStencil.stencil = 0;
    memset(&rp, 0, sizeof(rp));
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = vk.pass;
    rp.framebuffer = vk.framebuffers[vk.image_index];
    rp.renderArea.extent = vk.extent;
    rp.clearValueCount = 2;
    rp.pClearValues = clear;
    vkCmdBeginRenderPass(fr->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)vk.extent.width;
    viewport.height = (float)vk.extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    scissor.offset.x = scissor.offset.y = 0;
    scissor.extent = vk.extent;
    vkCmdSetViewport(fr->cmd, 0, 1, &viewport);
    vkCmdSetScissor(fr->cmd, 0, 1, &scissor);
    vk.in_frame = 1;
}

static void vk_draw(const p3d_draw *d)
{
    vk_frame *fr = &vk.fr[vk.fi];
    const vk_mesh *m;
    const vk_texture *t;
    draw_block *db;
    p3d_mat4 mvp;
    uint32_t offset;
    VkDeviceSize zero = 0;
    if (!vk.in_frame || d->mesh == 0 || d->mesh > MAX_MESHES || !vk.meshes[d->mesh - 1].used) return;
    if (fr->draw_count == MAX_DRAWS) return;
    m = &vk.meshes[d->mesh - 1];
    t = (d->material.texture && d->material.texture <= MAX_TEXTURES && vk.textures[d->material.texture - 1].used)
        ? &vk.textures[d->material.texture - 1] : &vk.textures[vk.white - 1];
    offset = (uint32_t)(vk.draw_stride * fr->draw_count++);
    db = (draw_block *)((uint8_t *)fr->ubo.mapped + vk.ring_offset + offset);
    mvp = p3d_m4_mul(vk.view_proj, d->model);
    memcpy(db->mvp, mvp.m, sizeof(db->mvp));
    memcpy(db->model, d->model.m, sizeof(db->model));
    memcpy(db->color, d->material.color, sizeof(db->color));
    db->flags[0] = (float)d->material.shading;
    db->flags[1] = db->flags[2] = db->flags[3] = 0.0f;
    vkCmdBindPipeline(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_pipe);
    vkCmdBindDescriptorSets(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout, 0, 1, &fr->set0, 1, &offset);
    vkCmdBindDescriptorSets(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout, 1, 1, &t->set, 0, NULL);
    vkCmdBindVertexBuffers(fr->cmd, 0, 1, &m->vb.buf, &zero);
    vkCmdBindIndexBuffer(fr->cmd, m->ib.buf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(fr->cmd, m->count, 1, 0, 0, 0);
}

static void vk_raytrace(const p3d_rt_scene *scene)
{
    vk_frame *fr = &vk.fr[vk.fi];
    p3d_frame f = vk.frame;
    uint32_t zero = 0;
    if (!vk.in_frame) return;
    f.width = (int)vk.extent.width;    /* trace at the swapchain's real size */
    f.height = (int)vk.extent.height;
    p3d_rt_pack(&f, scene, (p3d_rt_gpu *)((uint8_t *)fr->ubo.mapped + vk.rt_offset));
    vkCmdBindPipeline(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.rt_pipe);
    vkCmdBindDescriptorSets(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout, 0, 1, &fr->set0, 1, &zero);
    vkCmdDraw(fr->cmd, 3, 1, 0, 0);
}

static void submit(void)
{
    vk_frame *fr = &vk.fr[vk.fi];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si;
    vkEndCommandBuffer(fr->cmd);
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &fr->acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr->cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk.render_done[vk.image_index];
    vkQueueSubmit(vk.queue, 1, &si, fr->fence);
    vk.submitted = 1;
}

static void vk_end_frame(void)
{
    VkPresentInfoKHR pi;
    VkResult r;
    if (!vk.in_frame) return;
    if (!vk.submitted) {
        vkCmdEndRenderPass(vk.fr[vk.fi].cmd);
        submit();
    }
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk.render_done[vk.image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk.swapchain;
    pi.pImageIndices = &vk.image_index;
    r = vkQueuePresentKHR(vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk.swapchain_ok = 0;
    vk.in_frame = 0;
    vk.fi = (vk.fi + 1) % FRAMES;
}

/* Called between the draws and end_frame: finishes the frame's commands with a copy of the image,
 * submits and waits, so the pixels are ready; end_frame then only presents. */
static int vk_read_pixels(int width, int height, uint8_t *rgba)
{
    vk_frame *fr = &vk.fr[vk.fi];
    VkImage img;
    VkBufferImageCopy copy;
    VkDeviceSize bytes;
    const uint8_t *src;
    int x, y, w, h, bgr;
    if (!vk.in_frame || vk.submitted || !vk.can_read_back) return 0;
    img = vk.images[vk.image_index];
    w = (int)vk.extent.width;
    h = (int)vk.extent.height;
    bytes = (VkDeviceSize)w * (VkDeviceSize)h * 4u;
    if (vk.readback.size < bytes) {
        buffer_destroy(&vk.readback);
        if (!buffer_create(&vk.readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 0;
    }
    vkCmdEndRenderPass(fr->cmd);
    barrier(fr->cmd, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)w;
    copy.imageExtent.height = (uint32_t)h;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(fr->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.readback.buf, 1, &copy);
    barrier(fr->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    submit();
    vkWaitForFences(vk.device, 1, &fr->fence, VK_TRUE, UINT64_MAX);
    src = (const uint8_t *)vk.readback.mapped;
    bgr = vk.format == VK_FORMAT_B8G8R8A8_UNORM || vk.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            uint8_t *d = rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
            if (x < w && y < h) {
                const uint8_t *s = src + ((size_t)y * (size_t)w + (size_t)x) * 4u;
                d[0] = bgr ? s[2] : s[0];
                d[1] = s[1];
                d[2] = bgr ? s[0] : s[2];
                d[3] = 255;
            } else d[0] = d[1] = d[2] = 0, d[3] = 255;
        }
    return 1;
}

const p3d_render_backend p3d_backend_vulkan = {
    "vulkan", P3D_TIER_MODERN, P3D_SURFACE_VULKAN,
    vk_init, vk_shutdown,
    vk_mesh_create, vk_mesh_destroy,
    vk_texture_create, vk_texture_destroy,
    vk_begin_frame, vk_draw, vk_raytrace, vk_end_frame,
    vk_read_pixels
};
