/*
    DivaVK - instance/device setup, frame lifecycle, uploads and presentation.
*/

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include "context.hpp"
#include "xr.hpp"
#include <set>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" bool dvk_xr_active(void);

namespace dvk {
    Context* g_ctx = nullptr;
    thread_local Context* t_ctx = nullptr;

    void program_global_init();
    void program_global_shutdown();

    static dvk_log_func s_log_func = nullptr;
    static void* s_log_user = nullptr;

    void log(const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (s_log_func)
            s_log_func(buf, s_log_user);
        else
            fprintf(stderr, "[DivaVK] %s\n", buf);
    }

    const char* env(const char* name) {
#ifdef _WIN32
        static std::mutex mtx;
        static std::unordered_map<std::string, std::string> values;
        char buf[512];
        const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        if (!n || n >= sizeof(buf))
            return getenv(name);
        std::lock_guard<std::mutex> lock(mtx);
        std::string& v = values[name];
        v.assign(buf, n);
        return v.c_str();
#else
        return getenv(name);
#endif
    }

    std::atomic<const char*> g_stage{ "starting" };

    void set_log_target(dvk_log_func fn, void* user) {
        s_log_func = fn;
        s_log_user = user;
    }

    void log_once(const char* key, const char* fmt, ...) {
        static std::set<std::string> seen;
        static std::mutex mtx;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!seen.insert(key).second)
                return;
        }
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log("%s", buf);
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            log("validation: %s", data->pMessage);
        return VK_FALSE;
    }

    // ------------------------------------------------------------ upload arena

    UploadArena::Alloc UploadArena::alloc(Context& ctx, VkDeviceSize size, VkDeviceSize align) {
        if (align < 16)
            align = 16;
        for (; current < chunks.size(); current++) {
            Chunk& c = chunks[current];
            VkDeviceSize off = align_up(c.used, align);
            if (off + size <= c.size) {
                c.used = off + size;
                return { c.buffer, off, c.ptr + off };
            }
        }

        Chunk c;
        c.size = std::max<VkDeviceSize>(8ull << 20, align_up<VkDeviceSize>(size, 1 << 20));
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = c.size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
            | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
            | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   // nothing here flushes or invalidates by hand
        VmaAllocationInfo info = {};
        DVK_CHECK(vmaCreateBuffer(ctx.vma, &bci, &aci, &c.buffer, &c.alloc, &info));
        c.ptr = (uint8_t*)info.pMappedData;
        c.used = size;
        chunks.push_back(c);
        current = chunks.size() - 1;
        return { c.buffer, 0, c.ptr };
    }

    void UploadArena::reset() {
        for (Chunk& c : chunks)
            c.used = 0;
        current = 0;
    }

    void UploadArena::destroy(Context& ctx) {
        for (Chunk& c : chunks)
            vmaDestroyBuffer(ctx.vma, c.buffer, c.alloc);
        chunks.clear();
        current = 0;
    }

    // ------------------------------------------------------------ init

    static bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name) {
        for (const VkExtensionProperties& e : exts)
            if (!strcmp(e.extensionName, name))
                return true;
        return false;
    }

    bool Context::init(const dvk_create_info& ci) {
        info = ci;
        s_log_func = ci.log;
        s_log_user = ci.user;
        vsync = ci.vsync;

        if (volkInitialize() != VK_SUCCESS) {
            log("Vulkan loader not found");
            return false;
        }

        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> inst_exts(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, inst_exts.data());

        std::vector<const char*> exts;
        for (uint32_t i = 0; i < ci.instance_extension_count; i++)
            exts.push_back(ci.instance_extensions[i]);

        std::vector<const char*> layers;
        if (ci.enable_validation) {
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> props(count);
            vkEnumerateInstanceLayerProperties(&count, props.data());
            for (const VkLayerProperties& p : props)
                if (!strcmp(p.layerName, "VK_LAYER_KHRONOS_validation"))
                    layers.push_back("VK_LAYER_KHRONOS_validation");
            if (layers.empty())
                log("Validation requested but VK_LAYER_KHRONOS_validation isn't installed");
        }
        if (has_extension(inst_exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            has_debug_utils = true;
        }

        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = ci.app_name ? ci.app_name : "DivaVK";
        app.pEngineName = "DivaVK";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = (uint32_t)exts.size();
        ici.ppEnabledExtensionNames = exts.data();
        ici.enabledLayerCount = (uint32_t)layers.size();
        ici.ppEnabledLayerNames = layers.data();
        if (xr_enabled() ? !xr_create_vk_instance(ici, instance) : vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
            log("vkCreateInstance failed");
            return false;
        }
        volkLoadInstance(instance);

        if (has_debug_utils && !layers.empty()) {
            VkDebugUtilsMessengerCreateInfoEXT mci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            mci.pfnUserCallback = debug_callback;
            vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &messenger);
        }

        if (ci.create_surface)
            surface = (VkSurfaceKHR)ci.create_surface((dvk_instance)instance, ci.user);

        // Pick a physical device: discrete > integrated > anything, with a graphics(+present) queue.
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (xr_enabled()) {
            // The headset's runtime decides which GPU renders.
            devices.assign(1, xr_physical_device(instance));
            if (!devices[0])
                return false;
        }
        int best_score = -1;
        for (VkPhysicalDevice pd : devices) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pd, &p);
            uint32_t qcount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
            std::vector<VkQueueFamilyProperties> qprops(qcount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qprops.data());
            for (uint32_t q = 0; q < qcount; q++) {
                if (!(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                    continue;
                if (surface && !xr_enabled()) {
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface, &present);
                    if (!present)
                        continue;
                }
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                if (score > best_score) {
                    best_score = score;
                    phys = pd;
                    queue_family = q;
                }
                break;
            }
        }
        if (!phys) {
            log("No suitable Vulkan device");
            return false;
        }

        vkGetPhysicalDeviceProperties(phys, &props);
        {
            uint32_t qcount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
            std::vector<VkQueueFamilyProperties> qprops(qcount);
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops.data());
            if (queue_family < qcount && qprops[queue_family].timestampValidBits && props.limits.timestampPeriod > 0.0f)
                timestamp_ns = props.limits.timestampPeriod;
            // For a HUD drawn apart from the PV: can a second queue with a higher priority exist?
            uint32_t ecount = 0;
            vkEnumerateDeviceExtensionProperties(phys, nullptr, &ecount, nullptr);
            std::vector<VkExtensionProperties> exts(ecount);
            vkEnumerateDeviceExtensionProperties(phys, nullptr, &ecount, exts.data());
            bool gp_ext = false, gp_khr = false;
            for (const VkExtensionProperties& e : exts) {
                gp_ext |= !strcmp(e.extensionName, "VK_EXT_global_priority");
                gp_khr |= !strcmp(e.extensionName, "VK_KHR_global_priority");
            }
            std::string fams;
            for (uint32_t q = 0; q < qcount; q++) {
                char b[96];
                snprintf(b, sizeof(b), "%s%u: %u queue%s%s%s%s", q ? "; " : "", q, qprops[q].queueCount,
                    qprops[q].queueCount == 1 ? "" : "s",
                    (qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? " graphics" : "",
                    (qprops[q].queueFlags & VK_QUEUE_COMPUTE_BIT) ? " compute" : "",
                    (qprops[q].queueFlags & VK_QUEUE_TRANSFER_BIT) ? " transfer" : "");
                fams += b;
            }
            log("Queues: family %u in use (%s); global priority: %s; timestamps %s", queue_family, fams.c_str(),
                gp_khr ? "VK_KHR_global_priority" : gp_ext ? "VK_EXT_global_priority" : "not offered",
                timestamp_ns > 0.0 ? "yes" : "no");
        }
        VkPhysicalDeviceFeatures avail;
        vkGetPhysicalDeviceFeatures(phys, &avail);
        features = {};
        features.samplerAnisotropy = avail.samplerAnisotropy;
        features.textureCompressionBC = avail.textureCompressionBC;
        features.wideLines = avail.wideLines;
        features.fillModeNonSolid = avail.fillModeNonSolid;
        features.independentBlend = avail.independentBlend;
        features.occlusionQueryPrecise = avail.occlusionQueryPrecise;
        log("Device: %s (Vulkan %u.%u.%u)%s", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
            features.textureCompressionBC ? "" : " - no BC texture support");
        {
            // CPU-visible memory types (mapped buffers require HOST_COHERENT: nothing flushes by hand)
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(phys, &mp);
            std::string types;
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
                VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
                if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                    continue;
                char b[96];
                snprintf(b, sizeof(b), "%s%u:%s%s%s", types.empty() ? "" : ", ", i,
                    f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
                    f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : " NOT coherent",
                    f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "");
                types += b;
            }
            log("CPU-visible memory types: %s", types.c_str());
        }

        vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> dev_exts(count);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, dev_exts.data());
        {
            // Hardware video decoding (Vulkan Video), for the movie player: report what the
            // driver offers, so a log shows whether FFmpeg's Vulkan decoder could be used.
            std::string video;
            for (const VkExtensionProperties& e : dev_exts) {
                const char* n = e.extensionName;
                if (strncmp(n, "VK_KHR_video_decode", 19) == 0 || strcmp(n, "VK_KHR_video_queue") == 0)
                    video += std::string(video.empty() ? "" : ", ") + (n + 3);
            }
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
            int decode_queues = 0;
            for (const VkQueueFamilyProperties& q : qf)
                if (q.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
                    decode_queues++;
            log("Video decode: %s; %d decode queue famil%s", video.empty() ? "no Vulkan Video extensions" : video.c_str(),
                decode_queues, decode_queues == 1 ? "y" : "ies");
        }
        if (surface && xr_enabled()) {
            // With a headset the window is only a mirror; drop it if this queue can't present.
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys, queue_family, surface, &present);
            if (!present) {
                log("The headset's GPU can't present to the game window; showing the game in the headset only");
                vkDestroySurfaceKHR(instance, surface, nullptr);
                surface = VK_NULL_HANDLE;
            }
        }
        std::vector<const char*> dexts;
        if (surface)
            dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        // A second queue in the same family, at a lower priority, for a second context (a PV
        // render thread), and timeline semaphores so the game thread can tell when that
        // context's work is done without waiting for it.
        uint32_t family_queues = 1;
        {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
            if (queue_family < qn)
                family_queues = qf[queue_family].queueCount;
        }
        // DIVAVK_SECOND_QUEUE=0: the PV render thread shares the game's queue (a test)
        if (const char* e = env("DIVAVK_SECOND_QUEUE"))
            if (!strcmp(e, "0"))
                family_queues = 1;
        VkPhysicalDeviceTimelineSemaphoreFeaturesKHR tsf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR };
        if (has_extension(dev_exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
            VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            VkPhysicalDeviceTimelineSemaphoreFeaturesKHR q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR };
            f2.pNext = &q;
            vkGetPhysicalDeviceFeatures2(phys, &f2);
            if (q.timelineSemaphore) {
                dexts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
                tsf.timelineSemaphore = VK_TRUE;
                sh->has_timeline = true;
            }
        }

        const float prios[2] = { 1.0f, 0.0f };
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = queue_family;
        qci.queueCount = family_queues >= 2 ? 2 : 1;
        qci.pQueuePriorities = prios;

        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        if (sh->has_timeline)
            dci.pNext = &tsf;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = (uint32_t)dexts.size();
        dci.ppEnabledExtensionNames = dexts.data();
        dci.pEnabledFeatures = &features;
        if (xr_enabled() ? !xr_create_vk_device(phys, dci, device) : vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
            log("vkCreateDevice failed");
            return false;
        }
        volkLoadDevice(device);
        vkGetDeviceQueue(device, queue_family, 0, &sh->queues[0]);
        if (qci.queueCount >= 2)
            vkGetDeviceQueue(device, queue_family, 1, &sh->queues[1]);
        log("Second queue for a PV render thread: %s; timeline semaphores: %s",
            sh->queues[1] ? "yes (lower priority)" : "no (it shares the game's queue)", sh->has_timeline ? "yes" : "no");
        {
            std::lock_guard<std::mutex> l(sh->contexts_mutex);
            sh->contexts.push_back(this);
        }

        VmaVulkanFunctions vf = {};
        vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo aci = {};
        aci.vulkanApiVersion = VK_API_VERSION_1_1;
        aci.physicalDevice = phys;
        aci.device = device;
        aci.instance = instance;
        aci.pVulkanFunctions = &vf;
        if (vmaCreateAllocator(&aci, &vma) != VK_SUCCESS) {
            log("vmaCreateAllocator failed");
            return false;
        }

        // D24S8 is missing on some desktop GPUs; fall back to D32S8.
        auto supports_ds = [&](VkFormat f) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(phys, f, &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            return (fp.optimalTilingFeatures & need) == need;
        };
        depth_stencil_format = supports_ds(VK_FORMAT_D24_UNORM_S8_UINT)
            ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        format_for_depth_stencil(depth_stencil_format);

        // Pipeline cache
        std::vector<uint8_t> cache_data;
        if (ci.pipeline_cache_path) {
            if (FILE* f = fopen(ci.pipeline_cache_path, "rb")) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size > 0) {
                    cache_data.resize((size_t)size);
                    if (fread(cache_data.data(), 1, cache_data.size(), f) != cache_data.size())
                        cache_data.clear();
                }
                fclose(f);
            }
        }
        VkPipelineCacheCreateInfo pcci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        pcci.initialDataSize = cache_data.size();
        pcci.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
        if (vkCreatePipelineCache(device, &pcci, nullptr, &pipeline_cache) != VK_SUCCESS) {
            pcci.initialDataSize = 0;
            pcci.pInitialData = nullptr;
            vkCreatePipelineCache(device, &pcci, nullptr, &pipeline_cache);
        }

        create_frames();
        if (sh->has_timeline) {
            // signalled with each frame's serial, so a shared context can order its work after ours
            VkSemaphoreTypeCreateInfoKHR stci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR };
            stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
            VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            sci.pNext = &stci;
            if (vkCreateSemaphore(device, &sci, nullptr, &timeline) != VK_SUCCESS)
                timeline = VK_NULL_HANDLE;
        }

        program_global_init();

        // Null vertex buffer: float (0,0,0,1) at offset 0, int zeros at offset 16.
        {
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = 32;
            bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo vaci = {};
            vaci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            vaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            vaci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            VmaAllocationInfo ainfo = {};
            DVK_CHECK(vmaCreateBuffer(vma, &bci, &vaci, &null_vertex_buffer, &null_vertex_alloc, &ainfo));
            const float data[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            memcpy(ainfo.pMappedData, data, sizeof(data));

            bci.size = 65536;
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            DVK_CHECK(vmaCreateBuffer(vma, &bci, &vaci, &dummy_buffer, &dummy_buffer_alloc, &ainfo));
            memset(ainfo.pMappedData, 0, 65536);
        }

        // Default framebuffer and dummy textures
        default_color = std::make_unique<TextureObj>();
        default_depth = std::make_unique<TextureObj>();
        texture_alloc(default_color.get(), GL_RENDERBUFFER, GL_RGBA8, ci.width, ci.height, 1, 1);
        texture_alloc(default_depth.get(), GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, ci.width, ci.height, 1, 1);
        default_fbo.name = 0;

        dummy_2d = std::make_unique<TextureObj>();
        dummy_cube = std::make_unique<TextureObj>();
        static const uint8_t black[4 * 6] = { 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
            0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 };
        texture_image(dummy_2d.get(), GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        for (int i = 0; i < 6; i++)
            texture_image(dummy_cube.get(), GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, 1, 1,
                GL_RGBA, GL_UNSIGNED_BYTE, black);

        rs.viewport[2] = rs.scissor[2] = ci.width;
        rs.viewport[3] = rs.scissor[3] = ci.height;

        if (surface && !create_swapchain())
            log("Swapchain creation failed; rendering offscreen");
        xr_start(*this);
        return true;
    }

    void Context::create_frames() {
        for (Frame& f : frames) {
            VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpci.queueFamilyIndex = queue_family;
            DVK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &f.pool));
            VkCommandBuffer cbs[2];
            VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cbai.commandPool = f.pool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 2;
            DVK_CHECK(vkAllocateCommandBuffers(device, &cbai, cbs));
            f.upload_cmd = cbs[0];
            f.cmd = cbs[1];
            VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            DVK_CHECK(vkCreateFence(device, &fci, nullptr, &f.fence));
            VkQueryPoolCreateInfo qpci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qpci.queryType = VK_QUERY_TYPE_OCCLUSION;
            qpci.queryCount = 1024;
            DVK_CHECK(vkCreateQueryPool(device, &qpci, nullptr, &f.query_pool));
            if (timestamp_ns > 0.0) {
                VkQueryPoolCreateInfo tci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                tci.queryType = VK_QUERY_TYPE_TIMESTAMP;
                tci.queryCount = 2;
                if (vkCreateQueryPool(device, &tci, nullptr, &f.ts_pool) != VK_SUCCESS)
                    f.ts_pool = VK_NULL_HANDLE;
            }
        }

    }

    // A second context sharing this one's device and objects (dvk_context_create_shared).
    bool Context::init_shared() {
        if (!sh->has_timeline || !device)
            return false;
        VkSemaphoreTypeCreateInfoKHR stci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR };
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
        stci.initialValue = 0;
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        sci.pNext = &stci;
        if (vkCreateSemaphore(device, &sci, nullptr, &timeline) != VK_SUCCESS)
            return false;
        frames_in_flight = 8;
        create_frames();
        serial = sh->next_serial.fetch_add(1);
        default_fbo.name = 0;
        rs.viewport[2] = rs.scissor[2] = default_color->width;
        rs.viewport[3] = rs.scissor[3] = default_color->height;
        {
            std::lock_guard<std::mutex> l(sh->contexts_mutex);
            sh->contexts.push_back(this);
        }
        return true;
    }

    void Context::save_pipeline_cache() {
        if (!pipeline_cache || !info.pipeline_cache_path)
            return;
        size_t size = 0;
        vkGetPipelineCacheData(device, pipeline_cache, &size, nullptr);
        std::vector<uint8_t> data(size);
        if (size && vkGetPipelineCacheData(device, pipeline_cache, &size, data.data()) == VK_SUCCESS)
            if (FILE* f = fopen(info.pipeline_cache_path, "wb")) {
                fwrite(data.data(), 1, size, f);
                fclose(f);
            }
        pipelines_at_last_save = stats.pipelines_created;
    }

    static void run_garbage(Shared* sh, bool all = false);

    void Context::shutdown() {
        if (!is_main()) {
            // a shared context: only what's its own
            if (!device)
                return;
            if (recording)
                submit_frame(false);
            for (Frame& f : frames)
                wait_frame(f);
            {
                std::lock_guard<std::mutex> l(sh->contexts_mutex);
                sh->contexts.erase(std::remove(sh->contexts.begin(), sh->contexts.end(), this), sh->contexts.end());
            }
            destroy_frames();
            if (timeline)
                vkDestroySemaphore(device, timeline, nullptr);
            timeline = VK_NULL_HANDLE;
            run_garbage(sh);
            return;
        }
        if (!device) {
            xr_shutdown(*this);
            return;
        }
        if (recording)
            submit_frame(false);
        wait_idle_all();
        xr_shutdown(*this);

        save_pipeline_cache();

        // Objects
        for (auto& it : programs.objects)
            program_destroy(it.second.get());
        for (auto& it : buffers.objects)
            buffer_destroy(it.second.get());
        for (auto& it : textures.objects)
            texture_destroy(it.second.get());
        texture_destroy(default_color.get());
        texture_destroy(default_depth.get());
        texture_destroy(dummy_2d.get());
        texture_destroy(dummy_cube.get());
        wait_idle_all();

        for (auto& it : pipelines)
            vkDestroyPipeline(device, it.second, nullptr);
        for (auto& it : vk_framebuffers)
            vkDestroyFramebuffer(device, it.second, nullptr);
        for (auto& it : render_passes)
            vkDestroyRenderPass(device, it.second, nullptr);
        for (auto& it : vk_samplers)
            vkDestroySampler(device, it.second, nullptr);

        destroy_swapchain();
        destroy_frames();
        vmaDestroyBuffer(vma, null_vertex_buffer, null_vertex_alloc);
        vmaDestroyBuffer(vma, dummy_buffer, dummy_buffer_alloc);
        vkDestroyPipelineCache(device, pipeline_cache, nullptr);
        if (timeline)
            vkDestroySemaphore(device, timeline, nullptr);
        vmaDestroyAllocator(vma);
        vkDestroyDevice(device, nullptr);
        if (surface)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (messenger)
            vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
        program_global_shutdown();
        device = VK_NULL_HANDLE;
    }

    void Context::destroy_frames() {
        for (Frame& f : frames) {
            f.arena.destroy(*this);
            for (VkDescriptorPool p : f.desc_pools)
                vkDestroyDescriptorPool(device, p, nullptr);
            f.desc_pools.clear();
            if (f.query_pool)
                vkDestroyQueryPool(device, f.query_pool, nullptr);
            if (f.ts_pool)
                vkDestroyQueryPool(device, f.ts_pool, nullptr);
            vkDestroyFence(device, f.fence, nullptr);
            vkDestroyCommandPool(device, f.pool, nullptr);
            f = Frame();
        }
    }

    // ------------------------------------------------------------ frames

    // Deferred destruction across contexts: an entry made at serial G runs once no context
    // still has a frame with a serial below G recording or unfinished on the GPU.
    void Context::update_unfinished() {
        uint64_t u = recording ? serial : UINT64_MAX;
        for (const Frame& f : frames)
            if (f.pending && f.serial < u)
                u = f.serial;
        unfinished.store(u);
    }

    static void run_garbage(Shared* sh, bool all) {
        uint64_t safe = UINT64_MAX;
        if (!all) {
            std::lock_guard<std::mutex> l(sh->contexts_mutex);
            for (Context* c : sh->contexts)
                safe = std::min(safe, c->unfinished.load());
        }
        std::vector<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> l(sh->garbage_mutex);
            size_t w = 0;
            for (size_t i = 0; i < sh->garbage.size(); i++) {
                if (all || sh->garbage[i].first <= safe)
                    run.push_back(std::move(sh->garbage[i].second));
                else
                    sh->garbage[w++] = std::move(sh->garbage[i]);
            }
            sh->garbage.resize(w);
        }
        for (auto& fn : run)
            fn();
    }

    void query_collect_frame(Context& ctx, Frame& f);

    void Context::wait_frame(Frame& f) {
        if (!f.pending)
            return;
        DVK_STAGE("waiting for the GPU to finish an earlier frame (vkWaitForFences)");
        if (vkGetFenceStatus(device, f.fence) != VK_SUCCESS) {
            const auto w0 = std::chrono::steady_clock::now();
            vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
            double prev = wait_ms_shared.load();
            while (!wait_ms_shared.compare_exchange_weak(prev, prev + ms)) {}
        }
        DVK_STAGE("running the game's GL calls");
        f.pending = false;
        if (f.ts_written) {
            uint64_t ts[2] = {};
            if (vkGetQueryPoolResults(device, f.ts_pool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS
                && ts[1] > ts[0])
            {
                const double ms = (double)(ts[1] - ts[0]) * timestamp_ns / 1e6;
                gpu_ms_total += ms;
                double prev = gpu_ms_shared.load();
                while (!gpu_ms_shared.compare_exchange_weak(prev, prev + ms)) {}
                gpu_frames_shared++;
            }
            f.ts_written = false;
        }
        query_collect_frame(*this, f);
        update_unfinished();
        run_garbage(sh);
    }

    void Context::wait_idle_all() {
        for (uint32_t i = 1; i <= frames_in_flight; i++)
            wait_frame(frames[(frame_index + i) % frames_in_flight]);
        {
            std::lock_guard<std::mutex> q(queue_mutex);
            vkQueueWaitIdle(queue);
        }
        update_unfinished();
        run_garbage(sh);
    }

    void Context::defer_destroy(std::function<void()> fn) {
        // after every frame any context has recorded or is recording now
        std::lock_guard<std::mutex> l(sh->garbage_mutex);
        sh->garbage.push_back({ sh->next_serial.load(), std::move(fn) });
    }

    void Context::begin_frame_if_needed() {
        if (recording)
            return;
        Frame& f = frame();
        wait_frame(f);
        vkResetFences(device, 1, &f.fence);
        f.arena.reset();
        for (VkDescriptorPool p : f.desc_pools)
            vkResetDescriptorPool(device, p, 0);
        f.desc_pool_current = 0;
        f.serial = serial;
        f.queries_used = 0;
        f.upload_used = false;

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(f.upload_cmd, 0);
        vkResetCommandBuffer(f.cmd, 0);
        vkBeginCommandBuffer(f.upload_cmd, &bi);
        vkBeginCommandBuffer(f.cmd, &bi);
        vkCmdResetQueryPool(f.cmd, f.query_pool, 0, 1024);
        if (f.ts_pool) {
            vkCmdResetQueryPool(f.cmd, f.ts_pool, 0, 2);
            vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.ts_pool, 0);
            f.ts_written = true;
        }
        recording = true;
        update_unfinished();
        bound_pipeline = VK_NULL_HANDLE;
        descriptors_dirty = true;
    }

    VkCommandBuffer Context::cmd() {
        begin_frame_if_needed();
        return frame().cmd;
    }

    VkCommandBuffer Context::upload_cmd() {
        begin_frame_if_needed();
        frame().upload_used = true;
        return frame().upload_cmd;
    }

    VkDescriptorSet Context::alloc_descriptor_set(VkDescriptorSetLayout layout) {
        Frame& f = frame();
        for (;;) {
            if (f.desc_pool_current >= f.desc_pools.size()) {
                VkDescriptorPoolSize sizes[] = {
                    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 },
                    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 },
                    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024 },
                };
                VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                dpci.maxSets = 2048;
                dpci.poolSizeCount = 3;
                dpci.pPoolSizes = sizes;
                VkDescriptorPool pool;
                DVK_CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
                f.desc_pools.push_back(pool);
            }
            VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = f.desc_pools[f.desc_pool_current];
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &layout;
            VkDescriptorSet set;
            VkResult r = vkAllocateDescriptorSets(device, &dsai, &set);
            if (r == VK_SUCCESS)
                return set;
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
                log("vkAllocateDescriptorSets failed: %d", (int)r);
                return VK_NULL_HANDLE;
            }
            f.desc_pool_current++;
        }
    }

    void Context::submit_frame(bool do_present) {
        if (!recording)
            begin_frame_if_needed();
        end_pass();
        flush_pending_clear();
        end_pass();

        Frame& f = frame();
        uint32_t image_index = ~0u;
        VkSemaphore acquire = VK_NULL_HANDLE;

        // In the headset the OpenXR thread shows the frames from its own staging images; the
        // window isn't seen. DIVAVK_WINDOW_PRESENT=0 skips its copy and its image wait then.
        if (window_present < 0) {
            const char* wp = env("DIVAVK_WINDOW_PRESENT");
            window_present = !(wp && !strcmp(wp, "0"));
            if (!window_present)
                log("Window present: off while OpenXR shows the frames (DIVAVK_WINDOW_PRESENT=0)");
        }
        if (!is_main())
            do_present = false;   // a shared context never shows anything itself
        if (do_present && swapchain && (window_present || !dvk_xr_active())) {
            acquire = acquire_sem[frame_index];
            DVK_STAGE("getting a window image (vkAcquireNextImageKHR)");
            // Bounded wait: a window system that stops returning images must not freeze the game.
            VkResult r = vkAcquireNextImageKHR(device, swapchain, 250000000ull, acquire, VK_NULL_HANDLE, &image_index);
            DVK_STAGE("ending the frame");
            if (r == VK_TIMEOUT || r == VK_NOT_READY) {
                image_index = ~0u;
                acquire = VK_NULL_HANDLE;
                if (++acquire_timeouts == 3 && present_mode == VK_PRESENT_MODE_FIFO_KHR && !fifo_stalled) {
                    log("The window stopped returning images with vsync (FIFO) presentation; switching to "
                        "mailbox/immediate presentation");
                    fifo_stalled = true;
                    swapchain_dirty = true;
                }
                else if (acquire_timeouts == 3 || acquire_timeouts % 600 == 0)
                    log("The window isn't returning images (%d timeouts); frames are being skipped", acquire_timeouts);
            }
            else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
                swapchain_dirty = true;
                image_index = ~0u;
                acquire = VK_NULL_HANDLE;
            }
            // VK_SUBOPTIMAL_KHR still presents fine; recreating on it makes some window systems
            // rebuild the swapchain every frame. Only OUT_OF_DATE (or a resize) recreates.
            else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                image_index = ~0u;
                acquire = VK_NULL_HANDLE;
            }
        }

        if (image_index != ~0u) {
            acquire_timeouts = 0;
            VkImage dst = swapchain_images[image_index];
            TextureObj* src = default_color.get();
            texture_transition(f.cmd, src, src->rest_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);

            // GL keeps row 0 at the bottom; flip while copying to the swapchain.
            VkImageBlit region = {};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffsets[0] = { 0, src->height, 0 };
            region.srcOffsets[1] = { src->width, 0, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstOffsets[0] = { 0, 0, 0 };
            region.dstOffsets[1] = { (int32_t)swapchain_extent.width, (int32_t)swapchain_extent.height, 1 };
            vkCmdBlitImage(f.cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
            texture_transition(f.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src->rest_layout);
        }

        if (do_present)
            xr_record_frame(*this, f.cmd);

        // Hoisted uploads must be visible to everything recorded in the main buffer.
        if (f.upload_used) {
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(f.upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
        }
        vkEndCommandBuffer(f.upload_cmd);
        if (f.ts_written)
            vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.ts_pool, 1);
        vkEndCommandBuffer(f.cmd);

        VkCommandBuffer cbs[2];
        uint32_t ncb = 0;
        if (f.upload_used)
            cbs[ncb++] = f.upload_cmd;
        cbs[ncb++] = f.cmd;

        // Waits: the window image, and other contexts' work this frame uses (dvk_ticket_use:
        // only ever work that has already finished, so the GPU doesn't actually wait).
        std::vector<VkSemaphore> wsems;
        std::vector<uint64_t> wvals;
        std::vector<VkPipelineStageFlags> wstages;
        std::vector<VkSemaphore> ssems;
        std::vector<uint64_t> svals;
        if (acquire && image_index != ~0u) {
            wsems.push_back(acquire);
            wvals.push_back(0);
            wstages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
            ssems.push_back(present_sem[image_index]);
            svals.push_back(0);
        }
        for (auto& w : submit_waits) {
            wsems.push_back(w.first);
            wvals.push_back(w.second);
            wstages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        submit_waits.clear();
        if (timeline) {
            ssems.push_back(timeline);
            svals.push_back(serial);
        }
        VkTimelineSemaphoreSubmitInfoKHR tsi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR };
        tsi.waitSemaphoreValueCount = (uint32_t)wvals.size();
        tsi.pWaitSemaphoreValues = wvals.data();
        tsi.signalSemaphoreValueCount = (uint32_t)svals.size();
        tsi.pSignalSemaphoreValues = svals.data();
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        if (sh->has_timeline && (timeline || wvals.size() > (acquire && image_index != ~0u ? 1u : 0u)))
            si.pNext = &tsi;
        si.commandBufferCount = ncb;
        si.pCommandBuffers = cbs;
        si.waitSemaphoreCount = (uint32_t)wsems.size();
        si.pWaitSemaphores = wsems.data();
        si.pWaitDstStageMask = wstages.data();
        si.signalSemaphoreCount = (uint32_t)ssems.size();
        si.pSignalSemaphores = ssems.data();
        DVK_STAGE("submitting the frame (vkQueueSubmit)");
        std::unique_lock<std::mutex> queue_lock(queue_mutex);
        VkResult r = vkQueueSubmit(queue, 1, &si, f.fence);
        if (r != VK_SUCCESS)
            log("vkQueueSubmit failed: %d", (int)r);
        f.pending = true;
        recording = false;

        if (image_index != ~0u) {
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &present_sem[image_index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &image_index;
            DVK_STAGE("showing the frame in the window (vkQueuePresentKHR)");
            VkResult pr = vkQueuePresentKHR(queue, &pi);
            if (pr == VK_ERROR_OUT_OF_DATE_KHR)
                swapchain_dirty = true;
        }
        queue_lock.unlock();
        DVK_STAGE("running the game's GL calls");
        if (do_present)
            xr_frame_submitted();

        last_submitted.store(serial);
        frame_index = (frame_index + 1) % frames_in_flight;
        serial = sh->next_serial.fetch_add(1);
        update_unfinished();

        if (swapchain_dirty) {
            wait_idle_all();
            create_swapchain();
        }
    }

    void Context::present() {
        dump_frame_end();
        submit_frame(true);
        // GPU time: what the GPU spent on the frames completed meanwhile, per presented frame
        if (timestamp_ns > 0.0) {
            const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!gpu_log_start_ns)
                gpu_log_start_ns = now;
            gpu_presents++;
            if (now - gpu_log_start_ns >= 10000000000ull) {
                log("GPU: %.1f ms per frame on average (%u frames in %.1f s)",
                    gpu_ms_total / gpu_presents, gpu_presents, (now - gpu_log_start_ns) / 1e9);
                gpu_ms_total = 0.0;
                gpu_presents = 0;
                gpu_log_start_ns = now;
            }
        }
        dump_frame_begin();
        // Persist new pipelines now and then; the game may never shut down cleanly.
        if ((serial % 1800) == 0 && stats.pipelines_created != pipelines_at_last_save)
            save_pipeline_cache();
    }

    // ------------------------------------------------------------ swapchain

    void Context::destroy_swapchain() {
        for (VkSemaphore& s : acquire_sem)
            if (s) {
                vkDestroySemaphore(device, s, nullptr);
                s = VK_NULL_HANDLE;
            }
        for (VkSemaphore s : present_sem)
            vkDestroySemaphore(device, s, nullptr);
        present_sem.clear();
        if (swapchain) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
        swapchain_images.clear();
    }

    bool Context::create_swapchain() {
        swapchain_dirty = false;
        if (!surface)
            return false;

        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, formats.data());
        if (formats.empty())
            return false;

        VkSurfaceFormatKHR chosen = formats[0];
        for (const VkSurfaceFormatKHR& f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
                && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }

        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &count, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync || fifo_stalled)
            for (VkPresentModeKHR m : modes)
                if (m == VK_PRESENT_MODE_MAILBOX_KHR || m == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                    mode = m;
                    if (m == VK_PRESENT_MODE_MAILBOX_KHR)
                        break;
                }
        // DIVAVK_PRESENT_MODE=fifo|mailbox|immediate forces a mode when the surface offers it.
        if (const char* forced = env("DIVAVK_PRESENT_MODE")) {
            VkPresentModeKHR want = !strcmp(forced, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
                : !strcmp(forced, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
            for (VkPresentModeKHR m : modes)
                if (m == want)
                    mode = want;
        }
        std::string mode_list;
        for (VkPresentModeKHR m : modes)
            mode_list += m == VK_PRESENT_MODE_FIFO_KHR ? " fifo" : m == VK_PRESENT_MODE_MAILBOX_KHR ? " mailbox"
                : m == VK_PRESENT_MODE_IMMEDIATE_KHR ? " immediate" : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? " fifo-relaxed" : " other";

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFF) {
            extent.width = (uint32_t)default_color->width;
            extent.height = (uint32_t)default_color->height;
        }
        if (!extent.width || !extent.height)
            return false;

        uint32_t images = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount)
            images = std::min(images, caps.maxImageCount);

        // Replace an old swapchain by destroying it first instead of handing it over through
        // oldSwapchain. Some window systems (WinlatorXR's X server) still hold the old images'
        // pixmaps while the new ones are created, and fail with BadIDChoice.
        const bool recreated = swapchain != VK_NULL_HANDLE;
        if (recreated) {
            {
                std::lock_guard<std::mutex> q(queue_mutex);
                vkDeviceWaitIdle(device);
            }
            destroy_swapchain();
        }
        VkSwapchainKHR old = VK_NULL_HANDLE;
        VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sci.surface = surface;
        sci.minImageCount = images;
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
        sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        sci.presentMode = mode;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = old;
        VkSwapchainKHR sc;
        DVK_STAGE("creating the window's swapchain (vkCreateSwapchainKHR)");
        VkResult scr = vkCreateSwapchainKHR(device, &sci, nullptr, &sc);
        DVK_STAGE("running the game's GL calls");
        if (scr != VK_SUCCESS) {
            log("vkCreateSwapchainKHR %ux%u failed: %d", extent.width, extent.height, (int)scr);
            return false;
        }
        swapchain = sc;
        swapchain_format = chosen.format;
        swapchain_extent = extent;

        vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        static const char* mode_names[] = { "immediate", "mailbox", "fifo", "fifo relaxed" };
        log("Swapchain %s: %ux%u, %u images, %s (offered:%s)", recreated ? "recreated" : "created", extent.width,
            extent.height, count, (unsigned)mode < 4 ? mode_names[mode] : "other", mode_list.c_str());
        present_mode = mode;
        swapchain_images.resize(count);
        vkGetSwapchainImagesKHR(device, swapchain, &count, swapchain_images.data());

        VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (VkSemaphore& s : acquire_sem)
            vkCreateSemaphore(device, &semci, nullptr, &s);
        present_sem.resize(count);
        for (VkSemaphore& s : present_sem)
            vkCreateSemaphore(device, &semci, nullptr, &s);
        return true;
    }
}
