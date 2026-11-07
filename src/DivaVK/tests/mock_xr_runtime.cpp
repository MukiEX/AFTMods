/*
    A minimal mock OpenXR runtime for divavk_xr_tests.

    Implements just what DivaVK's OpenXR output uses: XR_KHR_vulkan_enable2, one session,
    LOCAL/VIEW spaces, swapchains backed by real Vulkan images on the app's device, and a
    frame loop paced at 90 Hz. xrEndFrame records the submitted layers and reads the
    swapchain image back, so the test can check exactly what a headset would be shown.

    Loaded like a real runtime: through a manifest and xrNegotiateLoaderRuntimeInterface.
*/

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {
    struct MockImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct MockSwapchain {
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::vector<MockImage> images;
        uint32_t next = 0;
        int acquired = -1;
        int last_released = -1;
    };

    struct Mock {
        std::mutex m;
        // Vulkan, from the app's create calls
        PFN_vkGetInstanceProcAddr gipa = nullptr;
        VkInstance vk_instance = VK_NULL_HANDLE;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queue_family = 0;
        VkQueue queue = VK_NULL_HANDLE;
        bool requirements_called = false;
        // State
        bool instance = false, session = false, running = false, in_frame = false;
        std::deque<XrSessionState> events;
        std::vector<MockSwapchain*> swapchains;
        std::chrono::steady_clock::time_point next_frame;
        // What the test reads
        int frames_ended = 0, layers_last = 0, swapchains_created = 0;
        XrCompositionLayerQuad quads[3] = {};
        std::vector<uint8_t> pixels;     // last released image of the layers' swapchain, RGBA
        uint32_t pix_w = 0, pix_h = 0;
        VkFormat pix_format = VK_FORMAT_UNDEFINED;
        bool queue_entered = false;      // checks the app holds its queue lock: not reentrant
        int errors = 0;
    } g;

    template <typename T> T vkfn(const char* name) {
        T f = (T)g.gipa(g.vk_instance, name);
        if (!f) {
            fprintf(stderr, "mock xr: missing %s\n", name);
            g.errors++;
        }
        return f;
    }

    XrInstance INSTANCE = (XrInstance)(uintptr_t)0x1001;
    XrSession SESSION = (XrSession)(uintptr_t)0x2001;
    XrSpace LOCAL = (XrSpace)(uintptr_t)0x3001;
    XrSpace VIEW = (XrSpace)(uintptr_t)0x3002;
    XrSystemId SYSTEM = 0x42;

    void push_state(XrSessionState s) { g.events.push_back(s); }

    // ---------------------------------------------------------------- instance

    XrResult XRAPI_CALL EnumerateInstanceExtensionProperties(const char*, uint32_t cap, uint32_t* count,
            XrExtensionProperties* props) {
        *count = 1;
        if (cap >= 1 && props)
            snprintf(props[0].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* ci, XrInstance* out) {
        bool vk2 = false;
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            vk2 |= !strcmp(ci->enabledExtensionNames[i], XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        if (!vk2)
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        g.instance = true;
        *out = INSTANCE;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL DestroyInstance(XrInstance) { g.instance = false; return XR_SUCCESS; }
    XrResult XRAPI_CALL GetInstanceProperties(XrInstance, XrInstanceProperties* p) {
        snprintf(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "DivaVK mock runtime");
        p->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo* gi, XrSystemId* out) {
        if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
            return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        *out = SYSTEM;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p) {
        snprintf(p->systemName, XR_MAX_SYSTEM_NAME_SIZE, "Mock HMD");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer* ev) {
        std::lock_guard<std::mutex> l(g.m);
        if (g.events.empty())
            return XR_EVENT_UNAVAILABLE;
        auto* sc = (XrEventDataSessionStateChanged*)ev;
        *sc = { XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
        sc->session = SESSION;
        sc->state = g.events.front();
        g.events.pop_front();
        return XR_SUCCESS;
    }

    // ---------------------------------------------------------------- Vulkan enable2

    XrResult XRAPI_CALL GetVulkanGraphicsRequirements2KHR(XrInstance, XrSystemId, XrGraphicsRequirementsVulkan2KHR* r) {
        r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
        r->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
        g.requirements_called = true;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL CreateVulkanInstanceKHR(XrInstance, const XrVulkanInstanceCreateInfoKHR* ci, VkInstance* out,
            VkResult* vr) {
        g.gipa = ci->pfnGetInstanceProcAddr;
        auto create = (PFN_vkCreateInstance)g.gipa(VK_NULL_HANDLE, "vkCreateInstance");
        *vr = create(ci->vulkanCreateInfo, nullptr, out);
        g.vk_instance = *out;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL GetVulkanGraphicsDevice2KHR(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR* gi,
            VkPhysicalDevice* out) {
        auto enumerate = vkfn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t n = 1;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        VkResult r = enumerate(gi->vulkanInstance, &n, &pd);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !pd)
            return XR_ERROR_RUNTIME_FAILURE;
        g.phys = *out = pd;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL CreateVulkanDeviceKHR(XrInstance, const XrVulkanDeviceCreateInfoKHR* ci, VkDevice* out, VkResult* vr) {
        if (ci->vulkanPhysicalDevice != g.phys)
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        auto create = vkfn<PFN_vkCreateDevice>("vkCreateDevice");
        *vr = create(ci->vulkanPhysicalDevice, ci->vulkanCreateInfo, nullptr, out);
        g.device = *out;
        return XR_SUCCESS;
    }

    // ---------------------------------------------------------------- session

    XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* out) {
        if (!g.requirements_called)
            return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        auto* b = (const XrGraphicsBindingVulkan2KHR*)ci->next;
        if (!b || b->type != XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR || b->device != g.device || b->physicalDevice != g.phys)
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        g.queue_family = b->queueFamilyIndex;
        auto gdpa = vkfn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        auto get_queue = (PFN_vkGetDeviceQueue)gdpa(g.device, "vkGetDeviceQueue");
        get_queue(g.device, b->queueFamilyIndex, b->queueIndex, &g.queue);
        g.session = true;
        std::lock_guard<std::mutex> l(g.m);
        push_state(XR_SESSION_STATE_IDLE);
        push_state(XR_SESSION_STATE_READY);
        *out = SESSION;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL DestroySession(XrSession) { g.session = false; g.running = false; return XR_SUCCESS; }
    XrResult XRAPI_CALL BeginSession(XrSession, const XrSessionBeginInfo* bi) {
        if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
            return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
        g.running = true;
        g.next_frame = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> l(g.m);
        push_state(XR_SESSION_STATE_SYNCHRONIZED);
        push_state(XR_SESSION_STATE_VISIBLE);
        push_state(XR_SESSION_STATE_FOCUSED);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL EndSession(XrSession) { g.running = false; return XR_SUCCESS; }
    XrResult XRAPI_CALL CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* ci, XrSpace* out) {
        *out = ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? VIEW : LOCAL;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL DestroySpace(XrSpace) { return XR_SUCCESS; }
    XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime, XrSpaceLocation* loc) {
        // The head is 1.6 m up, turned 90 degrees to the left (looking down -X).
        loc->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (space == VIEW && base == LOCAL)
            loc->pose = { { 0.0f, 0.70710678f, 0.0f, 0.70710678f }, { 0.0f, 1.6f, 0.0f } };
        else
            loc->pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
        return XR_SUCCESS;
    }

    // ---------------------------------------------------------------- swapchains

    XrResult XRAPI_CALL EnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* count, int64_t* formats) {
        // MOCK_XR_FORMATS=bgra or rgba limits the list, so both paths get tested.
        std::vector<int64_t> list;
        const char* env = getenv("MOCK_XR_FORMATS");
        if (!env || strcmp(env, "rgba"))
            list.push_back(VK_FORMAT_B8G8R8A8_SRGB);
        if (!env || strcmp(env, "bgra"))
            list.push_back(VK_FORMAT_R8G8B8A8_SRGB);
        *count = (uint32_t)list.size();
        if (formats && cap >= *count)
            memcpy(formats, list.data(), list.size() * sizeof(int64_t));
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL CreateSwapchain(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
        auto gdpa = vkfn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        auto create_image = (PFN_vkCreateImage)gdpa(g.device, "vkCreateImage");
        auto reqs = (PFN_vkGetImageMemoryRequirements)gdpa(g.device, "vkGetImageMemoryRequirements");
        auto alloc = (PFN_vkAllocateMemory)gdpa(g.device, "vkAllocateMemory");
        auto bind = (PFN_vkBindImageMemory)gdpa(g.device, "vkBindImageMemory");
        auto mem_props = vkfn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
        VkPhysicalDeviceMemoryProperties mp;
        mem_props(g.phys, &mp);

        auto* sc = new MockSwapchain();
        sc->width = ci->width;
        sc->height = ci->height;
        sc->format = (VkFormat)ci->format;
        for (int i = 0; i < 3; i++) {
            VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = sc->format;
            ici.extent = { ci->width, ci->height, 1 };
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) ici.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            MockImage mi;
            if (create_image(g.device, &ici, nullptr, &mi.image) != VK_SUCCESS)
                return XR_ERROR_RUNTIME_FAILURE;
            VkMemoryRequirements r;
            reqs(g.device, mi.image, &r);
            VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = r.size;
            for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
                if (r.memoryTypeBits & (1u << t)) { mai.memoryTypeIndex = t; break; }
            alloc(g.device, &mai, nullptr, &mi.memory);
            bind(g.device, mi.image, mi.memory, 0);
            sc->images.push_back(mi);
        }
        g.swapchains.push_back(sc);
        g.swapchains_created++;
        *out = (XrSwapchain)sc;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL DestroySwapchain(XrSwapchain s) {
        auto* sc = (MockSwapchain*)s;
        auto gdpa = vkfn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        auto destroy_image = (PFN_vkDestroyImage)gdpa(g.device, "vkDestroyImage");
        auto free_mem = (PFN_vkFreeMemory)gdpa(g.device, "vkFreeMemory");
        for (MockImage& mi : sc->images) {
            destroy_image(g.device, mi.image, nullptr);
            free_mem(g.device, mi.memory, nullptr);
        }
        for (size_t i = 0; i < g.swapchains.size(); i++)
            if (g.swapchains[i] == sc)
                g.swapchains.erase(g.swapchains.begin() + i);
        delete sc;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL EnumerateSwapchainImages(XrSwapchain s, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* imgs) {
        auto* sc = (MockSwapchain*)s;
        *count = (uint32_t)sc->images.size();
        if (imgs && cap >= *count) {
            auto* vk = (XrSwapchainImageVulkan2KHR*)imgs;
            for (uint32_t i = 0; i < *count; i++) {
                if (vk[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR)
                    return XR_ERROR_VALIDATION_FAILURE;
                vk[i].image = sc->images[i].image;
            }
        }
        return XR_SUCCESS;
    }

    // Queue use by the runtime happens here; the app must have its queue lock held.
    struct QueueGuard {
        QueueGuard() { if (g.queue_entered) { fprintf(stderr, "mock xr: queue used concurrently\n"); g.errors++; } g.queue_entered = true; }
        ~QueueGuard() { g.queue_entered = false; }
    };

    XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain s, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
        QueueGuard q;
        auto* sc = (MockSwapchain*)s;
        if (sc->acquired >= 0)
            return XR_ERROR_CALL_ORDER_INVALID;
        sc->acquired = (int)sc->next;
        *index = sc->next;
        sc->next = (sc->next + 1) % sc->images.size();
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL WaitSwapchainImage(XrSwapchain s, const XrSwapchainImageWaitInfo*) {
        return ((MockSwapchain*)s)->acquired >= 0 ? XR_SUCCESS : XR_ERROR_CALL_ORDER_INVALID;
    }
    XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain s, const XrSwapchainImageReleaseInfo*) {
        QueueGuard q;
        auto* sc = (MockSwapchain*)s;
        if (sc->acquired < 0)
            return XR_ERROR_CALL_ORDER_INVALID;
        sc->last_released = sc->acquired;
        sc->acquired = -1;
        return XR_SUCCESS;
    }

    // ---------------------------------------------------------------- frames

    XrResult XRAPI_CALL WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* fs) {
        if (!g.running)
            return XR_ERROR_SESSION_NOT_RUNNING;
        g.next_frame += std::chrono::microseconds(11111);
        std::this_thread::sleep_until(g.next_frame);
        fs->predictedDisplayTime = (XrTime)std::chrono::duration_cast<std::chrono::nanoseconds>(
            g.next_frame.time_since_epoch()).count();
        fs->predictedDisplayPeriod = 11111111;
        fs->shouldRender = XR_TRUE;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo*) {
        QueueGuard q;
        if (g.in_frame)
            return XR_ERROR_CALL_ORDER_INVALID;
        g.in_frame = true;
        return XR_SUCCESS;
    }

    // Reads the released image (COLOR_ATTACHMENT_OPTIMAL) back to g.pixels on the app's queue.
    void read_back(MockSwapchain* sc) {
        auto gdpa = vkfn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
#define DF(name) auto name = (PFN_##name)gdpa(g.device, #name);
        DF(vkCreateCommandPool) DF(vkDestroyCommandPool) DF(vkAllocateCommandBuffers) DF(vkBeginCommandBuffer)
        DF(vkEndCommandBuffer) DF(vkCmdPipelineBarrier) DF(vkCmdCopyImageToBuffer) DF(vkQueueSubmit)
        DF(vkQueueWaitIdle) DF(vkCreateBuffer) DF(vkDestroyBuffer) DF(vkGetBufferMemoryRequirements)
        DF(vkAllocateMemory) DF(vkFreeMemory) DF(vkBindBufferMemory) DF(vkMapMemory) DF(vkUnmapMemory)
#undef DF
        auto mem_props = vkfn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
        VkPhysicalDeviceMemoryProperties mp;
        mem_props(g.phys, &mp);

        const VkDeviceSize size = (VkDeviceSize)sc->width * sc->height * 4;
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buf;
        vkCreateBuffer(g.device, &bci, nullptr, &buf);
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(g.device, buf, &r);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = r.size;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if ((r.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags &
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                mai.memoryTypeIndex = t;
                break;
            }
        VkDeviceMemory mem;
        vkAllocateMemory(g.device, &mai, nullptr, &mem);
        vkBindBufferMemory(g.device, buf, mem, 0);

        VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.queueFamilyIndex = g.queue_family;
        VkCommandPool pool;
        vkCreateCommandPool(g.device, &cpci, nullptr, &pool);
        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = pool;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(g.device, &cbai, &cb);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cb, &bi);
        VkImage img = sc->images[sc->last_released].image;
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { sc->width, sc->height, 1 };
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(g.queue);

        void* p = nullptr;
        vkMapMemory(g.device, mem, 0, size, 0, &p);
        std::vector<uint8_t> px((size_t)size);
        memcpy(px.data(), p, (size_t)size);
        vkUnmapMemory(g.device, mem);
        if (sc->format == VK_FORMAT_B8G8R8A8_SRGB || sc->format == VK_FORMAT_B8G8R8A8_UNORM)
            for (size_t i = 0; i < px.size(); i += 4)
                std::swap(px[i], px[i + 2]);
        {
            std::lock_guard<std::mutex> l(g.m);
            g.pixels.swap(px);
            g.pix_w = sc->width;
            g.pix_h = sc->height;
            g.pix_format = sc->format;
        }
        vkDestroyCommandPool(g.device, pool, nullptr);
        vkDestroyBuffer(g.device, buf, nullptr);
        vkFreeMemory(g.device, mem, nullptr);
    }

    XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* fe) {
        QueueGuard q;
        if (!g.in_frame)
            return XR_ERROR_CALL_ORDER_INVALID;
        g.in_frame = false;
        MockSwapchain* sc = nullptr;
        {
            std::lock_guard<std::mutex> l(g.m);
            g.frames_ended++;
            g.layers_last = (int)fe->layerCount;
            for (uint32_t i = 0; i < fe->layerCount && i < 3; i++) {
                if (fe->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
                    g.errors++;
                    continue;
                }
                g.quads[i] = *(const XrCompositionLayerQuad*)fe->layers[i];
                sc = (MockSwapchain*)g.quads[i].subImage.swapchain;
            }
        }
        if (sc && sc->last_released >= 0)
            read_back(sc);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL RequestExitSession(XrSession) {
        std::lock_guard<std::mutex> l(g.m);
        push_state(XR_SESSION_STATE_STOPPING);
        return XR_SUCCESS;
    }

    XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* out) {
#define F(n) if (!strcmp(name, "xr" #n)) { *out = (PFN_xrVoidFunction)n; return XR_SUCCESS; }
        F(GetInstanceProcAddr) F(EnumerateInstanceExtensionProperties) F(CreateInstance) F(DestroyInstance)
        F(GetInstanceProperties) F(GetSystem) F(GetSystemProperties) F(PollEvent)
        F(GetVulkanGraphicsRequirements2KHR) F(CreateVulkanInstanceKHR) F(GetVulkanGraphicsDevice2KHR)
        F(CreateVulkanDeviceKHR) F(CreateSession) F(DestroySession) F(BeginSession) F(EndSession)
        F(CreateReferenceSpace) F(DestroySpace) F(LocateSpace) F(EnumerateSwapchainFormats) F(CreateSwapchain)
        F(DestroySwapchain) F(EnumerateSwapchainImages) F(AcquireSwapchainImage) F(WaitSwapchainImage)
        F(ReleaseSwapchainImage) F(WaitFrame) F(BeginFrame) F(EndFrame) F(RequestExitSession)
#undef F
        *out = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
}

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* li, XrNegotiateRuntimeRequest* rr) {
    if (!li || !rr || li->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO)
        return XR_ERROR_INITIALIZATION_FAILED;
    rr->runtimeInterfaceVersion = 1;
    rr->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    rr->getInstanceProcAddr = GetInstanceProcAddr;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- test access

struct MockXrReport {
    int frames_ended, layers_last, swapchains_created, errors;
    bool instance_alive, session_alive;
    XrCompositionLayerQuad quads[3];
    uint32_t pix_w, pix_h;
    int pix_format;
};

EXPORT void mock_xr_report(MockXrReport* r, uint8_t* pixels, size_t pixels_size) {
    std::lock_guard<std::mutex> l(g.m);
    r->frames_ended = g.frames_ended;
    r->layers_last = g.layers_last;
    r->swapchains_created = g.swapchains_created;
    r->errors = g.errors;
    r->instance_alive = g.instance;
    r->session_alive = g.session;
    memcpy(r->quads, g.quads, sizeof(g.quads));
    r->pix_w = g.pix_w;
    r->pix_h = g.pix_h;
    r->pix_format = (int)g.pix_format;
    if (pixels && pixels_size >= g.pixels.size())
        memcpy(pixels, g.pixels.data(), g.pixels.size());
}
