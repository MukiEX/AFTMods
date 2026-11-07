/*
    DivaVK - OpenXR output.

    The game keeps rendering into the default framebuffer exactly as on a monitor. Each
    presented frame is also copied (flipped to top-down) into one of three staging images on
    the game's own command buffer. A frame thread, paced by the runtime's xrWaitFrame, copies
    the newest finished staging image into the OpenXR swapchain and shows it as quad layers:
    one for both eyes (mono), or one per eye taking the left and right halves of an SBS frame.

    Quad layers are composited by the runtime at the headset's own rate with the latest head
    pose, so the screen stays still in the room however fast or slowly the game renders.

    The runtime is used without the Khronos loader: its manifest is found the same way the
    loader would, and xrNegotiateLoaderRuntimeInterface gives us its xrGetInstanceProcAddr.
    Inside Wine that is the Windows-side runtime (Proton's wineopenxr, WinlatorXR's
    openxr_wxr.dll); in the tests, a mock runtime.
*/

#include "context.hpp"
#include "xr.hpp"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace dvk {

#define XR_FUNCS(X) \
    X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) \
    X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) X(xrPollEvent) \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrGetVulkanGraphicsRequirements2KHR) X(xrCreateVulkanInstanceKHR) \
    X(xrGetVulkanGraphicsDevice2KHR) X(xrCreateVulkanDeviceKHR)

    namespace {
        struct Staging {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation alloc = VK_NULL_HANDLE;
        };

        static const int STAGING_COUNT = 3;
        static const int CMD_COUNT = 6;   // two copies per headset frame (picture and HUD)

        // One image stream to the headset: staging images the game thread copies its frames
        // into, and the OpenXR swapchain the frame thread copies the newest one into.
        // Staging bookkeeping is guarded by XrState::m; the swapchain is the frame thread's.
        struct Channel {
            Staging staging[STAGING_COUNT];
            int32_t st_width = 0, st_height = 0;
            int published = -1;        // newest submitted copy
            int reading = -1;          // being copied by the frame thread
            int pending = -1;          // recorded this frame, not yet submitted
            uint64_t published_seq = 0, consumed_seq = 0;
            XrSwapchain swapchain = XR_NULL_HANDLE;
            std::vector<XrSwapchainImageVulkan2KHR> sc_images;
            int32_t sc_width = 0, sc_height = 0;
            bool sc_has_image = false;
        };

        struct XrState {
            // Runtime
            void* lib = nullptr;
            PFN_xrGetInstanceProcAddr gipa = nullptr;
#define DECL(name) PFN_##name name = nullptr;
            XR_FUNCS(DECL)
#undef DECL
            XrInstance instance = XR_NULL_HANDLE;
            XrSystemId system = XR_NULL_SYSTEM_ID;
            dvk_xr_info info = {};
            std::string app_name;
            bool enabled = false;      // setup succeeded

            // Session (frame thread, except creation)
            XrSession session = XR_NULL_HANDLE;
            XrSpace local = XR_NULL_HANDLE;
            XrSpace view = XR_NULL_HANDLE;
            VkFormat sc_format = VK_FORMAT_UNDEFINED;
            VkFormat staging_format = VK_FORMAT_UNDEFINED;
            XrPosef screen_pose = { { 0, 0, 0, 1 }, { 0, 0, -2 } };
            std::atomic<bool> session_running{ false };

            // The game's picture, and the HUD drawn apart from it (dvk_xr_set_hud_texture):
            // shown as its own alpha-blended layer just in front of the picture, so it updates
            // on its own and stands out from the 3D.
            Channel pv, hud;
            std::atomic<uint32_t> hud_tex{ 0 };   // GL texture name, 0 = no HUD layer
            float hud_depth = 0.10f;              // metres in front of the picture
            std::mutex m;
            std::condition_variable cv;

            // Frame thread
            Context* ctx = nullptr;
            std::thread thread;
            std::atomic<bool> quit{ false };
            std::atomic<bool> active{ false };
            std::atomic<bool> recenter{ true };
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cbs[CMD_COUNT] = {};
            VkFence fences[CMD_COUNT] = {};
            int cb_index = 0;
            uint64_t frames_shown = 0;
        };

        XrState* s_xr = nullptr;

        const char* result_name(XrResult r) {
            switch (r) {
            case XR_SUCCESS: return "XR_SUCCESS";
            case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE (headset not connected?)";
            case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
            case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
            case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
            case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
            case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
            case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
            case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
            case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
            case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
            case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
            case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
            case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
            case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
            case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
            case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
            default: return "";
            }
        }

#define XR_FAILED_LOG(call, what) ([&]() { XrResult _r = (call); \
            if (XR_FAILED(_r)) { log("OpenXR: %s failed: %d %s", what, (int)_r, result_name(_r)); return true; } \
            return false; }())

        // ---------------------------------------------------------------- runtime loading

        std::string read_text(const std::string& path) {
            std::string out;
            if (FILE* f = fopen(path.c_str(), "rb")) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    out.append(buf, n);
                fclose(f);
            }
            return out;
        }

        // The string value of "key" in a JSON manifest (enough for runtime manifests).
        std::string json_string(const std::string& text, const char* key) {
            std::string k = std::string("\"") + key + "\"";
            size_t p = text.find(k);
            if (p == std::string::npos)
                return "";
            p = text.find(':', p + k.size());
            if (p == std::string::npos)
                return "";
            p = text.find('"', p);
            if (p == std::string::npos)
                return "";
            std::string out;
            for (size_t i = p + 1; i < text.size() && text[i] != '"'; i++) {
                char c = text[i];
                if (c == '\\' && i + 1 < text.size()) {
                    c = text[++i];
                    if (c == 'n') c = '\n';
                    else if (c == 't') c = '\t';
                }
                out += c;
            }
            return out;
        }

        bool is_absolute(const std::string& p) {
#ifdef _WIN32
            return p.size() > 2 && (p[1] == ':' || (p[0] == '\\' && p[1] == '\\'));
#else
            return !p.empty() && p[0] == '/';
#endif
        }

        std::string active_runtime_manifest() {
            if (const char* env = getenv("XR_RUNTIME_JSON"))
                if (*env)
                    return env;
#ifdef _WIN32
            wchar_t value[1024] = {};
            DWORD size = sizeof(value);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                    RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
                char utf8[2048] = {};
                WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), nullptr, nullptr);
                return utf8;
            }
            return "";
#else
            std::vector<std::string> dirs;
            if (const char* x = getenv("XDG_CONFIG_HOME"))
                dirs.push_back(std::string(x) + "/openxr/1/active_runtime.json");
            if (const char* h = getenv("HOME"))
                dirs.push_back(std::string(h) + "/.config/openxr/1/active_runtime.json");
            dirs.push_back("/etc/xdg/openxr/1/active_runtime.json");
            dirs.push_back("/etc/openxr/1/active_runtime.json");
            for (const std::string& d : dirs)
                if (access(d.c_str(), R_OK) == 0)
                    return d;
            return "";
#endif
        }

        bool load_runtime(XrState& x, const char* manifest_override) {
            std::string manifest = manifest_override && *manifest_override ? manifest_override : active_runtime_manifest();
            if (manifest.empty()) {
                log("OpenXR: no runtime is installed or active (no ActiveRuntime / active_runtime.json, XR_RUNTIME_JSON unset)");
                return false;
            }
            std::string text = read_text(manifest);
            std::string lib = json_string(text, "library_path");
            if (lib.empty()) {
                log("OpenXR: can't read library_path from %s", manifest.c_str());
                return false;
            }
            if (!is_absolute(lib)) {
                size_t slash = manifest.find_last_of("/\\");
                lib = (slash == std::string::npos ? std::string() : manifest.substr(0, slash + 1)) + lib;
            }
            log("OpenXR: runtime %s (%s)", lib.c_str(), manifest.c_str());

            PFN_xrNegotiateLoaderRuntimeInterface negotiate = nullptr;
#ifdef _WIN32
            wchar_t wlib[1024] = {};
            MultiByteToWideChar(CP_UTF8, 0, lib.c_str(), -1, wlib, 1024);
            HMODULE h = LoadLibraryExW(wlib, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!h) {
                log("OpenXR: can't load %s (error %lu)", lib.c_str(), GetLastError());
                return false;
            }
            x.lib = h;
            negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(h, "xrNegotiateLoaderRuntimeInterface");
#else
            void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                log("OpenXR: can't load %s: %s", lib.c_str(), dlerror());
                return false;
            }
            x.lib = h;
            negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(h, "xrNegotiateLoaderRuntimeInterface");
#endif
            if (!negotiate) {
                log("OpenXR: %s has no xrNegotiateLoaderRuntimeInterface", lib.c_str());
                return false;
            }
            XrNegotiateLoaderInfo li = {};
            li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
            li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
            li.structSize = sizeof(li);
            li.minInterfaceVersion = 1;
            li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
            li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
            li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
            XrNegotiateRuntimeRequest rr = {};
            rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
            rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
            rr.structSize = sizeof(rr);
            if (XR_FAILED(negotiate(&li, &rr)) || !rr.getInstanceProcAddr) {
                log("OpenXR: runtime negotiation failed");
                return false;
            }
            x.gipa = rr.getInstanceProcAddr;
            return true;
        }

        void unload_runtime(XrState& x) {
            if (!x.lib)
                return;
#ifdef _WIN32
            FreeLibrary((HMODULE)x.lib);
#else
            dlclose(x.lib);
#endif
            x.lib = nullptr;
        }

        bool load_functions(XrState& x, XrInstance instance) {
            bool ok = true;
#define LOAD(name) if (XR_FAILED(x.gipa(instance, #name, (PFN_xrVoidFunction*)&x.name)) || !x.name) { \
                if (instance) { log("OpenXR: runtime lacks %s", #name); ok = false; } }
            XR_FUNCS(LOAD)
#undef LOAD
            return ok;
        }

        // ---------------------------------------------------------------- maths

        XrPosef screen_in_front_of(const XrPosef& head, const dvk_xr_info& info) {
            const XrQuaternionf& q = head.orientation;
            // Forward (-Z) rotated by the head orientation; keep only its heading.
            float fx = -2.0f * (q.x * q.z + q.w * q.y);
            float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            float yaw = (fx * fx + fz * fz) > 1e-6f ? atan2f(-fx, -fz) : 0.0f;
            XrPosef p;
            p.orientation = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
            p.position = { head.position.x - info.screen_distance * sinf(yaw),
                           head.position.y + info.screen_height,
                           head.position.z - info.screen_distance * cosf(yaw) };
            return p;
        }
    }

    // ---------------------------------------------------------------- setup

    bool xr_enabled() {
        return s_xr && s_xr->enabled;
    }

    static void xr_destroy_state() {
        if (!s_xr)
            return;
        if (s_xr->instance && s_xr->xrDestroyInstance)
            s_xr->xrDestroyInstance(s_xr->instance);
        unload_runtime(*s_xr);
        delete s_xr;
        s_xr = nullptr;
    }

    static bool xr_setup(const dvk_xr_info& info) {
        xr_destroy_state();
        s_xr = new XrState();
        XrState& x = *s_xr;
        x.info = info;
        x.app_name = info.app_name ? info.app_name : "DivaVK";
        x.info.app_name = x.app_name.c_str();
        x.info.runtime_json = nullptr;
        if (x.info.screen_width <= 0.1f) x.info.screen_width = 3.0f;
        if (x.info.screen_distance <= 0.1f) x.info.screen_distance = 2.5f;

        if (info.get_instance_proc_addr)
            x.gipa = (PFN_xrGetInstanceProcAddr)info.get_instance_proc_addr;
        else if (!load_runtime(x, info.runtime_json))
            return false;
        load_functions(x, XR_NULL_HANDLE);
        if (!x.xrEnumerateInstanceExtensionProperties || !x.xrCreateInstance) {
            log("OpenXR: runtime doesn't export the instance functions");
            return false;
        }

        uint32_t count = 0;
        x.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
        std::vector<XrExtensionProperties> exts(count, { XR_TYPE_EXTENSION_PROPERTIES });
        x.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
        bool has_vk2 = false;
        for (const XrExtensionProperties& e : exts)
            if (!strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
                has_vk2 = true;
        if (!has_vk2) {
            log("OpenXR: runtime doesn't support %s", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
            return false;
        }

        const char* enabled[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
        XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
        snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", x.app_name.c_str());
        ici.applicationInfo.applicationVersion = 1;
        snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "DivaVK");
        ici.applicationInfo.engineVersion = 1;
        ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = enabled;
        if (XR_FAILED_LOG(x.xrCreateInstance(&ici, &x.instance), "xrCreateInstance"))
            return false;
        if (!load_functions(x, x.instance))
            return false;

        XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(x.xrGetInstanceProperties(x.instance, &ip)))
            log("OpenXR: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

        XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (XR_FAILED_LOG(x.xrGetSystem(x.instance, &sgi, &x.system), "xrGetSystem"))
            return false;
        XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
        if (XR_SUCCEEDED(x.xrGetSystemProperties(x.instance, x.system, &sp)))
            log("OpenXR: headset %s", sp.systemName);

        XrGraphicsRequirementsVulkan2KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
        if (XR_FAILED_LOG(x.xrGetVulkanGraphicsRequirements2KHR(x.instance, x.system, &req),
                "xrGetVulkanGraphicsRequirements2KHR"))
            return false;
        x.enabled = true;
        return true;
    }

    bool xr_create_vk_instance(const VkInstanceCreateInfo& ci, VkInstance& out) {
        XrState& x = *s_xr;
        XrVulkanInstanceCreateInfoKHR xci = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
        xci.systemId = x.system;
        xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xci.vulkanCreateInfo = &ci;
        VkResult vr = VK_SUCCESS;
        if (XR_FAILED_LOG(x.xrCreateVulkanInstanceKHR(x.instance, &xci, &out, &vr), "xrCreateVulkanInstanceKHR"))
            return false;
        if (vr != VK_SUCCESS) {
            log("OpenXR: vkCreateInstance through the runtime failed: %d", (int)vr);
            return false;
        }
        return true;
    }

    VkPhysicalDevice xr_physical_device(VkInstance instance) {
        XrState& x = *s_xr;
        XrVulkanGraphicsDeviceGetInfoKHR gi = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
        gi.systemId = x.system;
        gi.vulkanInstance = instance;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        if (XR_FAILED_LOG(x.xrGetVulkanGraphicsDevice2KHR(x.instance, &gi, &phys), "xrGetVulkanGraphicsDevice2KHR"))
            return VK_NULL_HANDLE;
        return phys;
    }

    bool xr_create_vk_device(VkPhysicalDevice phys, const VkDeviceCreateInfo& ci, VkDevice& out) {
        XrState& x = *s_xr;
        XrVulkanDeviceCreateInfoKHR xci = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
        xci.systemId = x.system;
        xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xci.vulkanPhysicalDevice = phys;
        xci.vulkanCreateInfo = &ci;
        VkResult vr = VK_SUCCESS;
        if (XR_FAILED_LOG(x.xrCreateVulkanDeviceKHR(x.instance, &xci, &out, &vr), "xrCreateVulkanDeviceKHR"))
            return false;
        if (vr != VK_SUCCESS) {
            log("OpenXR: vkCreateDevice through the runtime failed: %d", (int)vr);
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- frame thread

    static void destroy_swapchain(XrState& x, Channel& ch) {
        if (ch.swapchain) {
            x.xrDestroySwapchain(ch.swapchain);
            ch.swapchain = XR_NULL_HANDLE;
        }
        ch.sc_images.clear();
        ch.sc_width = ch.sc_height = 0;
        ch.sc_has_image = false;
    }

    static bool ensure_swapchain(XrState& x, Channel& ch, int32_t w, int32_t h) {
        if (ch.swapchain && ch.sc_width == w && ch.sc_height == h)
            return true;
        if (ch.swapchain) {
            // Our copies into the old images must be finished first.
            vkWaitForFences(x.ctx->device, CMD_COUNT, x.fences, VK_TRUE, UINT64_MAX);
            destroy_swapchain(x, ch);
        }
        XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
            | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format = (int64_t)x.sc_format;
        sci.sampleCount = 1;
        sci.width = (uint32_t)w;
        sci.height = (uint32_t)h;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED_LOG(x.xrCreateSwapchain(x.session, &sci, &ch.swapchain), "xrCreateSwapchain"))
            return false;
        uint32_t count = 0;
        x.xrEnumerateSwapchainImages(ch.swapchain, 0, &count, nullptr);
        ch.sc_images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
        x.xrEnumerateSwapchainImages(ch.swapchain, count, &count, (XrSwapchainImageBaseHeader*)ch.sc_images.data());
        ch.sc_width = w;
        ch.sc_height = h;
        log("OpenXR: %s swapchain %dx%d, %u images", &ch == &x.hud ? "HUD" : "picture", w, h, count);
        return true;
    }

    static void handle_events(XrState& x) {
        for (;;) {
            XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
            if (x.xrPollEvent(x.instance, &ev) != XR_SUCCESS)
                break;
            switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& sc = *(XrEventDataSessionStateChanged*)&ev;
                static const char* names[] = { "unknown", "idle", "ready", "synchronized", "visible",
                    "focused", "stopping", "loss pending", "exiting" };
                log("OpenXR: session %s", sc.state < 9 ? names[sc.state] : "?");
                if (sc.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (!XR_FAILED_LOG(x.xrBeginSession(x.session, &bi), "xrBeginSession")) {
                        x.session_running = true;
                        x.recenter = true;
                    }
                }
                else if (sc.state == XR_SESSION_STATE_STOPPING) {
                    x.xrEndSession(x.session);
                    x.session_running = false;
                }
                else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING) {
                    x.session_running = false;
                    x.quit = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                x.recenter = true;   // the runtime recentred; follow it
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                log("OpenXR: instance loss pending");
                x.session_running = false;
                x.quit = true;
                break;
            default:
                break;
            }
        }
    }

    // Copies staging image idx into the next swapchain image. Returns false on failure.
    static bool copy_to_swapchain(XrState& x, Channel& ch, int idx, int32_t w, int32_t h) {
        Context& c = *x.ctx;
        if (!ensure_swapchain(x, ch, w, h))
            return false;

        uint32_t image = 0;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        {
            std::lock_guard<std::mutex> q(c.queue_mutex);
            if (XR_FAILED_LOG(x.xrAcquireSwapchainImage(ch.swapchain, &ai, &image), "xrAcquireSwapchainImage"))
                return false;
        }
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = 1000000000; // 1 s
        if (XR_FAILED_LOG(x.xrWaitSwapchainImage(ch.swapchain, &wi), "xrWaitSwapchainImage"))
            return false;

        int k = x.cb_index;
        x.cb_index = (x.cb_index + 1) % CMD_COUNT;
        vkWaitForFences(c.device, 1, &x.fences[k], VK_TRUE, UINT64_MAX);
        vkResetFences(c.device, 1, &x.fences[k]);
        VkCommandBuffer cb = x.cbs[k];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        VkImageMemoryBarrier b[2] = { { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER }, { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER } };
        // Staging: written by an earlier submission of the game's frame (already in
        // TRANSFER_SRC); the barrier makes that write visible here.
        b[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[0].image = ch.staging[idx].image;
        b[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        // Swapchain image: acquired in COLOR_ATTACHMENT_OPTIMAL; fully overwritten.
        b[1].srcAccessMask = 0;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].srcQueueFamilyIndex = b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[1].image = ch.sc_images[image].image;
        b[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);

        // Same-size formats (UNORM staging, possibly sRGB swapchain): the bytes are copied
        // as they are, so the game's already gamma-encoded colours stay as they were.
        VkImageCopy region = {};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImage(cb, ch.staging[idx].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            ch.sc_images[image].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b[1]);
        vkEndCommandBuffer(cb);

        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        std::lock_guard<std::mutex> q(c.queue_mutex);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VkResult r = vkQueueSubmit(c.queue, 1, &si, x.fences[k]);
        if (r != VK_SUCCESS)
            log("OpenXR: vkQueueSubmit failed: %d", (int)r);
        x.xrReleaseSwapchainImage(ch.swapchain, &ri);
        ch.sc_has_image = true;
        return r == VK_SUCCESS;
    }

    // Quad layers for the current swapchain contents.
    static uint32_t build_layers(XrState& x, XrCompositionLayerQuad* quads) {
        const dvk_xr_info& in = x.info;
        const int32_t w = x.pv.sc_width, h = x.pv.sc_height;
        auto quad = [&](XrCompositionLayerQuad& q, XrEyeVisibility eye, int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                float aspect) {
            q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
            q.layerFlags = 0;
            q.space = x.local;
            q.eyeVisibility = eye;
            q.subImage.swapchain = x.pv.swapchain;
            q.subImage.imageRect = { { rx, ry }, { rw, rh } };
            q.subImage.imageArrayIndex = 0;
            q.pose = x.screen_pose;
            q.size = { in.screen_width, in.screen_width / (aspect > 0.01f ? aspect : 16.0f / 9.0f) };
        };

        if (in.layout == DVK_XR_LAYOUT_MONO) {
            quad(quads[0], XR_EYE_VISIBILITY_BOTH, 0, 0, w, h,
                in.content_aspect > 0 ? in.content_aspect : (float)w / h);
            return 1;
        }

        const int32_t half = w / 2;
        int32_t rx = 0, ry = 0, rw = half, rh = h;
        float aspect = in.content_aspect > 0 ? in.content_aspect : (float)half / h;
        if (in.layout == DVK_XR_LAYOUT_SBS_HALF) {
            if (in.content_aspect <= 0)
                aspect = (float)w / h;   // a squeezed half shown at the frame's own shape
        }
        else if (in.content_aspect > 0) {
            // Full SBS: show only the picture, not the bars DivaGL put around it in its half.
            if ((float)half / h > aspect) {
                rw = (int32_t)(h * aspect + 0.5f);
                rx = (half - rw) / 2;
            }
            else {
                rh = (int32_t)(half / aspect + 0.5f);
                ry = (h - rh) / 2;
            }
        }
        const int32_t left_x = in.swap_eyes ? half : 0;
        const int32_t right_x = in.swap_eyes ? 0 : half;
        quad(quads[0], XR_EYE_VISIBILITY_LEFT, left_x + rx, ry, rw, rh, aspect);
        quad(quads[1], XR_EYE_VISIBILITY_RIGHT, right_x + rx, ry, rw, rh, aspect);
        return 2;
    }

    // The HUD layer: the whole HUD image, over the picture's quad, a little nearer the player
    // and shrunk by the same proportion so its edges still line up with the picture's.
    static void build_hud_layer(XrState& x, const XrCompositionLayerQuad& picture, XrCompositionLayerQuad& q) {
        q = picture;
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied alpha
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = x.hud.swapchain;
        q.subImage.imageRect = { { 0, 0 }, { x.hud.sc_width, x.hud.sc_height } };
        const XrQuaternionf& o = picture.pose.orientation;
        // the quad's +Z (towards the player) = rotate (0,0,1) by o
        const float nx = 2.0f * (o.x * o.z + o.w * o.y);
        const float ny = 2.0f * (o.y * o.z - o.w * o.x);
        const float nz = 1.0f - 2.0f * (o.x * o.x + o.y * o.y);
        const float d = x.hud_depth;
        q.pose.position.x += nx * d;
        q.pose.position.y += ny * d;
        q.pose.position.z += nz * d;
        const float dist = x.info.screen_distance > d + 0.05f ? x.info.screen_distance : d + 0.05f;
        const float k = (dist - d) / dist;
        q.size.width *= k;
        q.size.height *= k;
    }

    // Copies a channel's newest finished staging image into its swapchain, if there's a new one.
    static void update_channel(XrState& x, Channel& ch) {
        int idx = -1;
        int32_t w = 0, h = 0;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(x.m);
            if (ch.published >= 0 && ch.published_seq != ch.consumed_seq) {
                idx = ch.published;
                ch.reading = idx;
                seq = ch.published_seq;
                w = ch.st_width;
                h = ch.st_height;
            }
        }
        if (idx >= 0) {
            copy_to_swapchain(x, ch, idx, w, h);
            std::lock_guard<std::mutex> lock(x.m);
            ch.reading = -1;
            ch.consumed_seq = seq;
            x.cv.notify_all();
        }
    }

    static void frame_thread(XrState* xs) {
        XrState& x = *xs;
        while (!x.quit) {
            handle_events(x);
            if (!x.session_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
            XrFrameState fs = { XR_TYPE_FRAME_STATE };
            if (XR_FAILED_LOG(x.xrWaitFrame(x.session, &fwi, &fs), "xrWaitFrame")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            {
                std::lock_guard<std::mutex> q(x.ctx->queue_mutex);
                XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
                if (XR_FAILED_LOG(x.xrBeginFrame(x.session, &fbi), "xrBeginFrame"))
                    continue;
            }

            if (x.recenter.exchange(false)) {
                XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
                XrPosef head = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
                if (XR_SUCCEEDED(x.xrLocateSpace(x.view, x.local, fs.predictedDisplayTime, &loc))
                    && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
                    && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
                    head = loc.pose;
                x.screen_pose = screen_in_front_of(head, x.info);
            }

            XrCompositionLayerQuad quads[3];
            const XrCompositionLayerBaseHeader* layers[3];
            uint32_t layer_count = 0;
            if (fs.shouldRender) {
                update_channel(x, x.pv);
                const bool hud = x.hud_tex.load() != 0;
                if (hud)
                    update_channel(x, x.hud);
                if (x.pv.sc_has_image) {
                    layer_count = build_layers(x, quads);
                    if (hud && x.hud.sc_has_image) {
                        build_hud_layer(x, quads[0], quads[layer_count]);
                        layer_count++;
                    }
                    for (uint32_t i = 0; i < layer_count; i++)
                        layers[i] = (const XrCompositionLayerBaseHeader*)&quads[i];
                }
            }

            XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = layer_count;
            fei.layers = layer_count ? layers : nullptr;
            {
                std::lock_guard<std::mutex> q(x.ctx->queue_mutex);
                XR_FAILED_LOG(x.xrEndFrame(x.session, &fei), "xrEndFrame");
            }
            if (layer_count && x.frames_shown++ == 0)
                log("OpenXR: first frame shown in the headset");
        }
        x.active = false;
    }

    void xr_start(Context& c) {
        if (!xr_enabled())
            return;
        XrState& x = *s_xr;
        x.ctx = &c;

        XrGraphicsBindingVulkan2KHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
        binding.instance = c.instance;
        binding.physicalDevice = c.phys;
        binding.device = c.device;
        binding.queueFamilyIndex = c.queue_family;
        binding.queueIndex = 0;
        XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
        sci.next = &binding;
        sci.systemId = x.system;
        if (XR_FAILED_LOG(x.xrCreateSession(x.instance, &sci, &x.session), "xrCreateSession")) {
            x.enabled = false;
            return;
        }

        XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rci.poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (XR_FAILED_LOG(x.xrCreateReferenceSpace(x.session, &rci, &x.local), "xrCreateReferenceSpace(LOCAL)")) {
            x.enabled = false;
            return;
        }
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        XR_FAILED_LOG(x.xrCreateReferenceSpace(x.session, &rci, &x.view), "xrCreateReferenceSpace(VIEW)");

        // Prefer sRGB: the game's pixels are already gamma encoded, and an sRGB swapchain
        // tells the compositor so. The staging image is the UNORM twin (same bytes).
        uint32_t count = 0;
        x.xrEnumerateSwapchainFormats(x.session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        x.xrEnumerateSwapchainFormats(x.session, count, &count, formats.data());
        static const VkFormat wanted[][2] = {
            { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM },
            { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM },
            { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM },
            { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM },
        };
        for (const auto& wf : wanted) {
            for (int64_t f : formats)
                if (f == (int64_t)wf[0]) {
                    x.sc_format = wf[0];
                    x.staging_format = wf[1];
                    break;
                }
            if (x.sc_format)
                break;
        }
        if (!x.sc_format) {
            log("OpenXR: no 8-bit RGBA swapchain format offered");
            x.enabled = false;
            return;
        }
        if (x.sc_format == x.staging_format)
            log("OpenXR: no sRGB swapchain format; colours may look darker in the headset");

        VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = c.queue_family;
        vkCreateCommandPool(c.device, &cpci, nullptr, &x.pool);
        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = x.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = CMD_COUNT;
        vkAllocateCommandBuffers(c.device, &cbai, x.cbs);
        for (VkFence& f : x.fences) {
            VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(c.device, &fci, nullptr, &f);
        }

        static const char* layouts[] = { "mono", "half SBS", "full SBS" };
        log("OpenXR: %s screen %.2f m wide, %.2f m away", layouts[x.info.layout < 3 ? x.info.layout : 0],
            x.info.screen_width, x.info.screen_distance);
        x.active = true;
        x.thread = std::thread(frame_thread, s_xr);
    }

    // ---------------------------------------------------------------- game thread

    static void destroy_staging(Context& c, Channel& ch) {
        for (Staging& s : ch.staging)
            if (s.image) {
                vmaDestroyImage(c.vma, s.image, s.alloc);
                s = Staging();
            }
        ch.st_width = ch.st_height = 0;
        ch.published = -1;
    }

    // Copies src (a GL colour image, row 0 at the bottom) into a free staging image of ch, top-down.
    static void record_channel(Context& c, XrState& x, Channel& ch, TextureObj* src, VkCommandBuffer cb) {
        const int32_t w = src->width, h = src->height;

        int idx = -1;
        {
            std::unique_lock<std::mutex> lock(x.m);
            if (ch.st_width != w || ch.st_height != h) {
                // Resized: wait for the frame thread to let go, and for the GPU, then rebuild.
                x.cv.wait(lock, [&] { return ch.reading < 0; });
                {
                    std::lock_guard<std::mutex> q(c.queue_mutex);
                    vkQueueWaitIdle(c.queue);
                }
                destroy_staging(c, ch);
                for (Staging& s : ch.staging) {
                    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
                    ici.imageType = VK_IMAGE_TYPE_2D;
                    ici.format = x.staging_format;
                    ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
                    ici.mipLevels = 1;
                    ici.arrayLayers = 1;
                    ici.samples = VK_SAMPLE_COUNT_1_BIT;
                    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
                    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                    VmaAllocationCreateInfo aci = {};
                    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                    if (vmaCreateImage(c.vma, &ici, &aci, &s.image, &s.alloc, nullptr) != VK_SUCCESS) {
                        log("OpenXR: can't create %dx%d staging images", w, h);
                        destroy_staging(c, ch);
                        return;
                    }
                }
                ch.st_width = w;
                ch.st_height = h;
            }
            for (int i = 0; i < STAGING_COUNT; i++)
                if (i != ch.published && i != ch.reading) {
                    idx = i;
                    break;
                }
            ch.pending = idx;
        }
        if (idx < 0)
            return;

        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = ch.staging[idx].image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        // srcStage TRANSFER: earlier copies out of this image (frame thread) finish first.
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);

        c.texture_transition(cb, src, src->rest_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        // GL keeps row 0 at the bottom; OpenXR images are top-down.
        VkImageBlit region = {};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.srcOffsets[0] = { 0, h, 0 };
        region.srcOffsets[1] = { w, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstOffsets[0] = { 0, 0, 0 };
        region.dstOffsets[1] = { w, h, 1 };
        vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            ch.staging[idx].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        c.texture_transition(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src->rest_layout);

        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void xr_record_frame(Context& c, VkCommandBuffer cb) {
        if (!s_xr || !s_xr->active || !s_xr->session_running)
            return;
        XrState& x = *s_xr;
        record_channel(c, x, x.pv, c.default_color.get(), cb);
        if (const GLuint hud = x.hud_tex.load()) {
            TextureObj* t = c.textures.get(hud);
            if (t && t->image && t->width > 0 && t->height > 0)
                record_channel(c, x, x.hud, t, cb);
        }
    }

    void xr_frame_submitted() {
        if (!s_xr)
            return;
        std::lock_guard<std::mutex> lock(s_xr->m);
        for (Channel* ch : { &s_xr->pv, &s_xr->hud })
            if (ch->pending >= 0) {
                ch->published = ch->pending;
                ch->published_seq++;
                ch->pending = -1;
            }
    }

    void xr_shutdown(Context& c) {
        if (!s_xr)
            return;
        XrState& x = *s_xr;
        x.quit = true;
        if (x.thread.joinable())
            x.thread.join();
        if (c.device) {
            if (x.fences[0])
                vkWaitForFences(c.device, CMD_COUNT, x.fences, VK_TRUE, UINT64_MAX);
            {
                std::lock_guard<std::mutex> q(c.queue_mutex);
                vkQueueWaitIdle(c.queue);
            }
        }
        if (x.session) {
            destroy_swapchain(x, x.pv);
            destroy_swapchain(x, x.hud);
            if (x.view) x.xrDestroySpace(x.view);
            if (x.local) x.xrDestroySpace(x.local);
            x.xrDestroySession(x.session);
            x.session = XR_NULL_HANDLE;
        }
        if (c.device) {
            for (VkFence f : x.fences)
                if (f) vkDestroyFence(c.device, f, nullptr);
            if (x.pool)
                vkDestroyCommandPool(c.device, x.pool, nullptr);
            if (c.vma) {
                destroy_staging(c, x.pv);
                destroy_staging(c, x.hud);
            }
        }
        if (x.frames_shown)
            log("OpenXR: %llu frames shown", (unsigned long long)x.frames_shown);
        xr_destroy_state();
    }
}

using namespace dvk;

extern "C" bool dvk_xr_setup(const dvk_xr_info* info) {
    if (!info)
        return false;
    if (info->log)
        set_log_target(info->log, info->user);
    bool ok = xr_setup(*info);
    if (!ok)
        xr_destroy_state();
    return ok;
}

extern "C" bool dvk_xr_active(void) {
    return s_xr && s_xr->active && s_xr->session_running;
}

extern "C" void dvk_xr_set_hud_texture(uint32_t texture) {
    if (!s_xr)
        return;
    const uint32_t before = s_xr->hud_tex.exchange(texture);
    if (texture && !before) {
        // DIVAVK_HUD_DEPTH_CM: how far in front of the picture the HUD layer floats
        if (const char* e = getenv("DIVAVK_HUD_DEPTH_CM")) {
            const float cm = (float)atof(e);
            if (cm >= 0.0f && cm <= 200.0f)
                s_xr->hud_depth = cm / 100.0f;
        }
        log("OpenXR: HUD shown as its own layer, %.0f cm in front of the picture", s_xr->hud_depth * 100.0f);
    }
    else if (!texture && before)
        log("OpenXR: HUD back in the picture");
}

extern "C" void dvk_xr_recenter(void) {
    if (s_xr)
        s_xr->recenter = true;
}
