/*
    DivaVK OpenXR output tests, against tests/mock_xr_runtime.cpp.

    Round 1: full SBS, BGRA sRGB swapchain. The frame's left half must reach the left eye's
    quad and the right half the right eye's, top-down, and the screen must be placed in front
    of where the (mock) head looks.
    Round 2: mono, RGBA sRGB swapchain, and a resize in the middle (new staging images and a
    new swapchain).

    Run with VK_ICD_FILENAMES pointing at SwiftShader, like divavk_tests. --validate turns on
    the Khronos validation layer (needs it installed).
*/

#include <divavk/divavk.h>
#include <GL/glcorearb.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

#ifndef DIVAVK_MOCK_XR
#error DIVAVK_MOCK_XR must name the mock runtime library
#endif

struct MockXrReport {
    int frames_ended, layers_last, swapchains_created, errors;
    bool instance_alive, session_alive;
    XrCompositionLayerQuad quads[2];
    uint32_t pix_w, pix_h;
    int pix_format;
};
typedef void (*PFN_mock_xr_report)(MockXrReport*, uint8_t*, size_t);
static PFN_mock_xr_report mock_xr_report;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static PFNGLCLEARCOLORPROC glClearColor;
static PFNGLCLEARPROC glClear;
static PFNGLENABLEPROC glEnable;
static PFNGLDISABLEPROC glDisable;
static PFNGLSCISSORPROC glScissor;
static PFNGLVIEWPORTPROC glViewport;
static PFNGLGETERRORPROC glGetError;

static void load_gl() {
    glClearColor = (PFNGLCLEARCOLORPROC)dvk_get_proc_address("glClearColor");
    glClear = (PFNGLCLEARPROC)dvk_get_proc_address("glClear");
    glEnable = (PFNGLENABLEPROC)dvk_get_proc_address("glEnable");
    glDisable = (PFNGLDISABLEPROC)dvk_get_proc_address("glDisable");
    glScissor = (PFNGLSCISSORPROC)dvk_get_proc_address("glScissor");
    glViewport = (PFNGLVIEWPORTPROC)dvk_get_proc_address("glViewport");
    glGetError = (PFNGLGETERRORPROC)dvk_get_proc_address("glGetError");
}

static void fill(int x, int y, int w, int h, float r, float g, float b) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

static dvk_surface create_headless_surface(dvk_instance instance, void*) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    auto create = (PFN_vkCreateHeadlessSurfaceEXT)gipa((VkInstance)instance, "vkCreateHeadlessSurfaceEXT");
    if (!create)
        return nullptr;
    VkHeadlessSurfaceCreateInfoEXT ci = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    VkSurfaceKHR s = VK_NULL_HANDLE;
    create((VkInstance)instance, &ci, nullptr, &s);
    return (dvk_surface)s;
}

static void log_line(const char* msg, void*) {
    printf("  [log] %s\n", msg);
}

static bool start(int w, int h, int layout, bool validation) {
    dvk_xr_info xi = {};
    xi.app_name = "divavk_xr_tests";
    xi.layout = layout;
    xi.screen_width = 3.0f;
    xi.screen_distance = 2.5f;
    xi.log = log_line;
    if (!dvk_xr_setup(&xi)) {
        printf("dvk_xr_setup failed\n");
        return false;
    }
    const char* exts[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    dvk_create_info ci = {};
    ci.app_name = "divavk_xr_tests";
    ci.width = w;
    ci.height = h;
    ci.enable_validation = validation;
    ci.instance_extensions = exts;
    ci.instance_extension_count = 2;
    ci.create_surface = create_headless_surface;
    ci.log = log_line;
    if (!dvk_create(&ci)) {
        printf("dvk_create failed\n");
        return false;
    }
    load_gl();
    return true;
}

// Presents frames until the headset has shown one from the current size.
static void present_frames(int n) {
    for (int i = 0; i < n; i++) {
        dvk_swap_buffers();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

static void rgb_at(const std::vector<uint8_t>& px, int w, int x, int y, int out[3]) {
    const uint8_t* p = &px[((size_t)y * w + x) * 4];
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
}

#define CHECK_RGB(PX, W, X, Y, R, G, B) do { int _c[3]; rgb_at(PX, W, X, Y, _c); \
    CHECK(abs(_c[0] - (R)) < 4 && abs(_c[1] - (G)) < 4 && abs(_c[2] - (B)) < 4, \
        "headset pixel (%d,%d) = %d,%d,%d expected %d,%d,%d", X, Y, _c[0], _c[1], _c[2], R, G, B); } while (0)

static bool feq(float a, float b) { return fabsf(a - b) < 1e-3f; }

static void round_sbs(bool validation) {
    printf("full SBS, BGRA sRGB swapchain\n");
    setenv("MOCK_XR_FORMATS", "bgra", 1);
    const int W = 128, H = 64;
    if (!start(W, H, DVK_XR_LAYOUT_SBS_FULL, validation)) {
        g_failures++;
        return;
    }
    // Wait for the session to start running.
    for (int i = 0; i < 100 && !dvk_xr_active(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(dvk_xr_active(), "session never started");

    glViewport(0, 0, W, H);
    fill(0, 0, W / 2, H, 1, 0, 0);        // left eye: red
    fill(W / 2, 0, W / 2, H, 0, 1, 0);    // right eye: green
    fill(0, H - 16, W / 2, 16, 0, 0, 1);  // top of the left eye: blue (GL y is bottom-up)
    present_frames(6);

    MockXrReport r = {};
    std::vector<uint8_t> px(W * H * 4);
    mock_xr_report(&r, px.data(), px.size());
    CHECK(r.frames_ended > 5, "only %d XR frames", r.frames_ended);
    CHECK(r.layers_last == 2, "%d layers, expected 2", r.layers_last);
    CHECK(r.quads[0].eyeVisibility == XR_EYE_VISIBILITY_LEFT && r.quads[1].eyeVisibility == XR_EYE_VISIBILITY_RIGHT,
        "eye visibility %d/%d", r.quads[0].eyeVisibility, r.quads[1].eyeVisibility);
    const XrRect2Di& a = r.quads[0].subImage.imageRect;
    const XrRect2Di& b = r.quads[1].subImage.imageRect;
    CHECK(a.offset.x == 0 && a.offset.y == 0 && a.extent.width == 64 && a.extent.height == 64,
        "left rect %d,%d %dx%d", a.offset.x, a.offset.y, a.extent.width, a.extent.height);
    CHECK(b.offset.x == 64 && b.extent.width == 64 && b.extent.height == 64,
        "right rect %d,%d %dx%d", b.offset.x, b.offset.y, b.extent.width, b.extent.height);
    CHECK(feq(r.quads[0].size.width, 3.0f) && feq(r.quads[0].size.height, 3.0f),
        "quad size %.3f x %.3f", r.quads[0].size.width, r.quads[0].size.height);
    // The mock head looks down -X from 1.6 m: the screen goes 2.5 m that way, turned to face it.
    const XrPosef& p = r.quads[0].pose;
    CHECK(feq(p.position.x, -2.5f) && feq(p.position.y, 1.6f) && feq(p.position.z, 0.0f),
        "screen at %.3f,%.3f,%.3f", p.position.x, p.position.y, p.position.z);
    CHECK(feq(p.orientation.y, 0.70710678f) && feq(p.orientation.w, 0.70710678f),
        "screen orientation %.3f,%.3f,%.3f,%.3f", p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    CHECK(r.pix_w == W && r.pix_h == H, "swapchain %ux%u", r.pix_w, r.pix_h);
    CHECK(r.pix_format == VK_FORMAT_B8G8R8A8_SRGB, "swapchain format %d", r.pix_format);
    if (r.pix_w == W && r.pix_h == H) {
        CHECK_RGB(px, W, 4, 4, 0, 0, 255);     // top-left: blue, so the image is top-down
        CHECK_RGB(px, W, 4, 40, 255, 0, 0);
        CHECK_RGB(px, W, 100, 4, 0, 255, 0);
        CHECK_RGB(px, W, 100, 60, 0, 255, 0);
    }

    // New content reaches the headset
    fill(0, 0, W, H, 1, 1, 1);
    present_frames(4);
    mock_xr_report(&r, px.data(), px.size());
    CHECK_RGB(px, W, 4, 4, 255, 255, 255);

    CHECK(glGetError() == GL_NO_ERROR, "GL error");
    dvk_destroy();
    mock_xr_report(&r, nullptr, 0);
    CHECK(!r.session_alive && !r.instance_alive, "session/instance not destroyed");
    CHECK(r.errors == 0, "mock runtime saw %d errors", r.errors);
}

static void round_mono_resize(bool validation) {
    printf("mono, RGBA sRGB swapchain, resize\n");
    setenv("MOCK_XR_FORMATS", "rgba", 1);
    MockXrReport r = {};
    mock_xr_report(&r, nullptr, 0);
    const int swapchains_before = r.swapchains_created;
    if (!start(64, 64, DVK_XR_LAYOUT_MONO, validation)) {
        g_failures++;
        return;
    }
    for (int i = 0; i < 100 && !dvk_xr_active(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    fill(0, 0, 64, 64, 1, 0, 0);
    present_frames(4);

    dvk_resize(96, 48);
    glViewport(0, 0, 96, 48);
    fill(0, 0, 96, 48, 0, 0, 1);
    fill(0, 0, 8, 8, 1, 1, 0);   // bottom-left in GL: bottom-left in the headset too
    present_frames(6);

    std::vector<uint8_t> px(96 * 48 * 4);
    mock_xr_report(&r, px.data(), px.size());
    CHECK(r.swapchains_created - swapchains_before == 2, "%d swapchains created, expected 2",
        r.swapchains_created - swapchains_before);
    CHECK(r.layers_last == 1 && r.quads[0].eyeVisibility == XR_EYE_VISIBILITY_BOTH, "mono layer");
    CHECK(r.quads[0].subImage.imageRect.extent.width == 96 && r.quads[0].subImage.imageRect.extent.height == 48,
        "mono rect %dx%d", r.quads[0].subImage.imageRect.extent.width, r.quads[0].subImage.imageRect.extent.height);
    CHECK(feq(r.quads[0].size.height, 1.5f), "mono quad height %.3f", r.quads[0].size.height);
    CHECK(r.pix_format == VK_FORMAT_R8G8B8A8_SRGB, "swapchain format %d", r.pix_format);
    if (r.pix_w == 96 && r.pix_h == 48) {
        CHECK_RGB(px, 96, 2, 45, 255, 255, 0);
        CHECK_RGB(px, 96, 50, 10, 0, 0, 255);
    }
    else
        CHECK(false, "swapchain %ux%u after resize", r.pix_w, r.pix_h);

    dvk_destroy();
    mock_xr_report(&r, nullptr, 0);
    CHECK(r.errors == 0, "mock runtime saw %d errors", r.errors);
}

int main(int argc, char** argv) {
    bool validation = argc > 1 && !strcmp(argv[1], "--validate");
    void* mock = dlopen(DIVAVK_MOCK_XR, RTLD_NOW);
    if (!mock) {
        printf("can't load %s: %s\n", DIVAVK_MOCK_XR, dlerror());
        return 1;
    }
    mock_xr_report = (PFN_mock_xr_report)dlsym(mock, "mock_xr_report");

    // A runtime manifest, found through XR_RUNTIME_JSON like any real runtime.
    const char* manifest = "/tmp/divavk_mock_xr.json";
    if (FILE* f = fopen(manifest, "w")) {
        fprintf(f, "{\n  \"file_format_version\": \"1.0.0\",\n  \"runtime\": {\n"
                   "    \"name\": \"DivaVK mock\",\n    \"library_path\": \"%s\"\n  }\n}\n", DIVAVK_MOCK_XR);
        fclose(f);
    }
    setenv("XR_RUNTIME_JSON", manifest, 1);

    round_sbs(validation);
    round_mono_resize(validation);

    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
