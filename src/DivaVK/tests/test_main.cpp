/*
    DivaVK headless tests.

    Drives DivaVK purely through GL entry points obtained from dvk_get_proc_address, the
    same way the PD Loader plugin will, and checks pixels read back with glReadPixels.
    Runs on any Vulkan driver, including SwiftShader (VK_ICD_FILENAMES=...).
*/

#include <divavk/divavk.h>
#include <GL/glcorearb.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include "movie_yuv.h"

#ifndef DIVAVK_SHADER_DIR
#define DIVAVK_SHADER_DIR "res/ft_shaders"
#endif

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif

// ------------------------------------------------------------------ GL loading

#define GLFN(type, name) static type name;
#define GL_FUNCS(X) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARPROC, glClear) X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLBUFFERSTORAGEPROC, glBufferStorage) \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLCULLFACEPROC, glCullFace) X(PFNGLFRONTFACEPROC, glFrontFace) X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLCLEARDEPTHPROC, glClearDepth) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D) X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLDRAWBUFFERSPROC, glDrawBuffers) X(PFNGLREADBUFFERPROC, glReadBuffer) \
    X(PFNGLCOPYTEXSUBIMAGE2DPROC, glCopyTexSubImage2D) X(PFNGLGETTEXIMAGEPROC, glGetTexImage) \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLBEGINQUERYPROC, glBeginQuery) X(PFNGLENDQUERYPROC, glEndQuery) \
    X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv) X(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary) \
    X(PFNGLPROGRAMBINARYPROC, glProgramBinary) X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv) X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLBLENDFUNCPROC, glBlendFunc) X(PFNGLCOLORMASKPROC, glColorMask) X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv) X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers) X(PFNGLBINDSAMPLERPROC, glBindSampler) \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) X(PFNGLFINISHPROC, glFinish) \
    X(PFNGLTEXPARAMETERIVPROC, glTexParameteriv) X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)

GL_FUNCS(GLFN)

static void load_gl() {
#define LOAD(type, name) name = (type)dvk_get_proc_address(#name); \
    if (!name) { fprintf(stderr, "missing %s\n", #name); exit(2); }
    GL_FUNCS(LOAD)
#undef LOAD
}

// ------------------------------------------------------------------ helpers

static int g_failures = 0;
static int g_checks = 0;
static const int W = 128, H = 96;

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

struct RGBA { int r, g, b, a; };

static RGBA pixel(int x, int y) {
    uint8_t p[4] = {};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return { p[0], p[1], p[2], p[3] };
}

static bool near(RGBA p, int r, int g, int b, int a = 255, int tol = 3) {
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol && std::abs(p.a - a) <= tol;
}

#define CHECK_PIXEL(X_, Y_, R_, G_, B_, A_) do { RGBA _p = pixel(X_, Y_); \
    CHECK(near(_p, R_, G_, B_, A_), "pixel (%d,%d) = %d,%d,%d,%d expected %d,%d,%d,%d", X_, Y_, _p.r, _p.g, _p.b, _p.a, R_, G_, B_, A_); } while (0)

static GLuint compile(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        printf("  shader compile failed:\n%s\n", log);
    }
    return s;
}

static GLuint program(const std::string& vs, const std::string& fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        printf("  link failed:\n%s\n", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return ok ? p : 0;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Mirrors DivaGL's shader preprocessing: #include, permutation defines, "set = N, " removal.
static std::string divagl_preprocess(const std::string& file, int defines, int value = 0) {
    std::string s = read_file(std::string(DIVAVK_SHADER_DIR) + "/" + file);
    for (int guard = 0; guard < 32; guard++) {
        size_t pos = s.find("#include \"");
        if (pos == std::string::npos)
            break;
        size_t end = s.find('"', pos + 10);
        std::string inc = s.substr(pos + 10, end - pos - 10);
        s.replace(pos, end + 1 - pos, read_file(std::string(DIVAVK_SHADER_DIR) + "/" + inc));
    }
    size_t def = s.find("//DEF\n");
    if (def != std::string::npos) {
        std::string d;
        for (int i = 0; i < defines; i++)
            d += "#define _" + std::to_string(i) + " " + std::to_string(value) + "\n";
        s.replace(def, 6, d);
    }
    size_t off = 0;
    for (;;) {
        size_t ps = s.find("set = ", off);
        if (ps == std::string::npos)
            break;
        size_t pb = s.find("binding = ", ps);
        if (pb == std::string::npos || pb - ps > 10) {
            off = ps + 6;
            continue;
        }
        s.erase(ps, pb - ps);
        off = ps;
    }
    return s;
}

static const char* VS_POS2 = R"(#version 430 core
layout(location = 0) in vec2 a_pos;
layout(binding = 0) uniform Params { vec4 u_color; vec4 u_offset; };
layout(location = 0) out vec4 v_color;
void main() {
    gl_Position = vec4(a_pos + u_offset.xy, u_offset.z, 1.0);
    v_color = u_color;
}
)";

static const char* FS_COLOR = R"(#version 430 core
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() { o_color = v_color; }
)";

struct Params {
    float color[4];
    float offset[4];
};

static GLuint g_ubo, g_vao, g_vbo, g_prog_color;

static void set_params(float r, float g, float b, float a, float ox = 0, float oy = 0, float z = 0) {
    Params p = { { r, g, b, a }, { ox, oy, z, 0 } };
    glBindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(p), &p);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_ubo);
}

static void draw_tris(const float* verts, int n) {
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * 2 * sizeof(float), verts);
    glUseProgram(g_prog_color);
    glDrawArrays(GL_TRIANGLES, 0, n);
}

static void reset_state() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
}

// ------------------------------------------------------------------ tests

static void test_clear() {
    printf("clear\n");
    reset_state();
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK_PIXEL(0, 0, 64, 128, 191, 255);
    CHECK_PIXEL(W - 1, H - 1, 64, 128, 191, 255);

    // Scissored clear only touches the scissor rect
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 10, 10);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    CHECK_PIXEL(5, 5, 255, 0, 0, 255);
    CHECK_PIXEL(20, 20, 64, 128, 191, 255);
}

static void test_orientation_and_winding() {
    printf("orientation and winding\n");
    reset_state();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // Counter-clockwise triangle in the lower-left corner (GL window coords, y up)
    const float ccw[] = { -1, -1, 0, -1, -1, 0 };
    set_params(1, 0, 0, 1);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    draw_tris(ccw, 3);
    CHECK_PIXEL(2, 2, 255, 0, 0, 255);           // bottom-left is row 0 in GL
    CHECK_PIXEL(W - 3, H - 3, 0, 0, 0, 255);     // top-right untouched

    // Same triangle wound clockwise must be culled
    const float cw[] = { 1, 1, 1, 0, 0, 1 };
    set_params(0, 1, 0, 1);
    draw_tris(cw, 3);
    CHECK_PIXEL(W - 3, H - 3, 0, 0, 0, 255);
    glFrontFace(GL_CW);
    draw_tris(cw, 3);
    CHECK_PIXEL(W - 3, H - 3, 0, 255, 0, 255);
    glFrontFace(GL_CCW);
}

static void test_depth() {
    printf("depth (GL [-1,1] clip depth)\n");
    reset_state();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    const float quad[] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    set_params(1, 0, 0, 1, 0, 0, -0.5f); // near
    draw_tris(quad, 6);
    set_params(0, 0, 1, 1, 0, 0, 0.5f);  // far: must lose
    draw_tris(quad, 6);
    CHECK_PIXEL(W / 2, H / 2, 255, 0, 0, 255);
    set_params(0, 1, 0, 1, 0, 0, -0.9f); // nearer: must win
    draw_tris(quad, 6);
    CHECK_PIXEL(W / 2, H / 2, 0, 255, 0, 255);
    // z = -1 (GL near plane) must still be visible, not clipped
    glClear(GL_DEPTH_BUFFER_BIT);
    set_params(1, 1, 0, 1, 0, 0, -0.999f);
    draw_tris(quad, 6);
    CHECK_PIXEL(W / 2, H / 2, 255, 255, 0, 255);
}

static void test_ubo_ordering() {
    printf("dynamic UBO updated between draws\n");
    reset_state();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const float left[] = { -1, -1, 0, -1, 0, 1, -1, -1, 0, 1, -1, 1 };
    set_params(1, 0, 0, 1);
    draw_tris(left, 6);
    set_params(0, 0, 1, 1, 1.0f, 0); // same buffer, new contents, shifted right
    draw_tris(left, 6);
    CHECK_PIXEL(10, H / 2, 255, 0, 0, 255);
    CHECK_PIXEL(W - 10, H / 2, 0, 0, 255, 255);
}

static void test_blend() {
    printf("blending\n");
    reset_state();
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const float quad[] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    set_params(1, 0, 0, 0.5f);
    draw_tris(quad, 6);
    CHECK_PIXEL(W / 2, H / 2, 128, 0, 128, 191);
    glDisable(GL_BLEND);
}

static GLuint make_texture(int w, int h, const uint8_t* rgba) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

// Real DivaGL shader: copy.vert/copy.frag (gl_VertexID strip, Scene + Batch UBOs, sampler 0)
static GLuint g_copy_prog, g_copy_scene, g_copy_batch, g_empty_vao;

static void setup_copy() {
    g_copy_prog = program(divagl_preprocess("copy.vert", 0), divagl_preprocess("copy.frag", 0));
    float scene[8] = { 1, 1, 0, 0, 1, 1, 0, 0 };
    float batch[12] = { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0 };
    glGenBuffers(1, &g_copy_scene);
    glBindBuffer(GL_UNIFORM_BUFFER, g_copy_scene);
    glBufferStorage(GL_UNIFORM_BUFFER, sizeof(scene), scene, GL_DYNAMIC_STORAGE_BIT);
    glGenBuffers(1, &g_copy_batch);
    glBindBuffer(GL_UNIFORM_BUFFER, g_copy_batch);
    glBufferStorage(GL_UNIFORM_BUFFER, sizeof(batch), batch, GL_DYNAMIC_STORAGE_BIT);
    glGenVertexArrays(1, &g_empty_vao);
}

static void draw_copy(GLuint tex, float scale = 1.0f) {
    float batch[12] = { 0, 0, 0, 0, scale, scale, scale, scale, 0, 0, 0, 0 };
    glBindBuffer(GL_UNIFORM_BUFFER, g_copy_batch);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(batch), batch);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_copy_scene);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_copy_batch);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUseProgram(g_copy_prog);
    glBindVertexArray(g_empty_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void test_divagl_copy_shader() {
    printf("DivaGL copy shader + texture orientation\n");
    CHECK(g_copy_prog != 0, "copy program failed to link");
    if (!g_copy_prog)
        return;
    reset_state();
    // 2x2 texture: row 0 (bottom) red|green, row 1 (top) blue|white
    const uint8_t texels[] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    GLuint t = make_texture(2, 2, texels);
    draw_copy(t);
    CHECK_PIXEL(5, 5, 255, 0, 0, 255);
    CHECK_PIXEL(W - 5, 5, 0, 255, 0, 255);
    CHECK_PIXEL(5, H - 5, 0, 0, 255, 255);
    CHECK_PIXEL(W - 5, H - 5, 255, 255, 255, 255);
    draw_copy(t, 0.5f);
    CHECK_PIXEL(5, 5, 128, 0, 0, 128);
}

// DivaGL's tone-map LUT: RG16F, filled with GL_RG/GL_FLOAT data (via the 4.5 DSA call),
// sampled through a RRRG swizzle.
static void test_tone_map_lut() {
    printf("Tone map LUT (RG16F, float upload, RRRG swizzle)\n");
    reset_state();
    const int N = 64;
    float data[N * 2];
    for (int i = 0; i < N; i++) {
        data[i * 2 + 0] = (float)i / (N - 1);
        data[i * 2 + 1] = 0.25f;
    }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, N, 1, 0, GL_RG, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const GLint swizzle[] = { GL_RED, GL_RED, GL_RED, GL_GREEN };
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    glTextureSubImage2D(t, 0, 0, 0, N, 1, GL_RG, GL_FLOAT, data);
    draw_copy(t);
    for (int x : { 1, W / 4, W / 2, 3 * W / 4, W - 2 }) {
        int i = (int)((x + 0.5f) / W * N);
        int v = (int)((float)i / (N - 1) * 255.0f + 0.5f);
        CHECK_PIXEL(x, H / 2, v, v, v, 64);
    }
}

static void test_fbo_and_blit() {
    printf("FBO render-to-texture, MSAA resolve, blit, copy\n");
    reset_state();
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint depth;
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 64, 64, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "fbo incomplete");
    glViewport(0, 0, 64, 64);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const float tri[] = { -1, -1, 1, -1, -1, 1 };
    set_params(1, 0, 1, 1);
    draw_tris(tri, 3);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    CHECK_PIXEL(2, 2, 255, 0, 255, 255);
    CHECK_PIXEL(62, 62, 0, 255, 0, 255);

    // Sample the FBO texture into the default framebuffer (texture now in the sampled layout)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    draw_copy(tex);
    CHECK_PIXEL(2, 2, 255, 0, 255, 255);
    CHECK_PIXEL(W - 2, H - 2, 0, 255, 0, 255);

    // Vertically flipped blit FBO -> default
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 64, 64, 0, H, W, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    CHECK_PIXEL(2, H - 3, 255, 0, 255, 255);
    CHECK_PIXEL(W - 3, 2, 0, 255, 0, 255);

    // MSAA renderbuffer -> resolve blit -> texture
    GLuint rb, msfbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 64, 64);
    glGenFramebuffers(1, &msfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, msfbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, 64, 64);
    glClearColor(1, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msfbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    CHECK_PIXEL(32, 32, 255, 255, 0, 255);

    // glCopyTexSubImage2D from the bound framebuffer into a texture
    GLuint dst = make_texture(64, 64, nullptr);
    glBindTexture(GL_TEXTURE_2D, dst);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 64, 64);
    std::vector<uint8_t> px(64 * 64 * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    CHECK(px[0] == 255 && px[1] == 255 && px[2] == 0, "copytexsubimage got %d,%d,%d", px[0], px[1], px[2]);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
}

static void test_formats() {
    printf("texture formats: RGB8, luminance/alpha swizzles, BC1, mipmaps\n");
    reset_state();
    // RGB8 (expanded to RGBA8 with alpha forced to 1)
    const uint8_t rgb[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    draw_copy(t);
    CHECK_PIXEL(2, 2, 10, 20, 30, 255);
    CHECK_PIXEL(W - 2, H - 2, 100, 110, 120, 255);

    // GL_LUMINANCE8_ALPHA8 -> (L, L, L, A)
    const uint8_t la[] = { 200, 100 };
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8045 /*GL_LUMINANCE8_ALPHA8*/, 1, 1, 0, 0x190A /*GL_LUMINANCE_ALPHA*/, GL_UNSIGNED_BYTE, la);
    draw_copy(t);
    CHECK_PIXEL(W / 2, H / 2, 200, 200, 200, 100);

    // GL_ALPHA8 -> (0, 0, 0, A)
    const uint8_t a8[] = { 77 };
    glTexImage2D(GL_TEXTURE_2D, 0, 0x803C /*GL_ALPHA8*/, 1, 1, 0, GL_ALPHA, GL_UNSIGNED_BYTE, a8);
    draw_copy(t);
    CHECK_PIXEL(W / 2, H / 2, 0, 0, 0, 77);

    // Texture swizzle on top of a format swizzle
    const uint8_t rgba[] = { 1, 2, 3, 4 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    draw_copy(t);
    CHECK_PIXEL(W / 2, H / 2, 3, 2, 1, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);

    // BC1 (DXT1) 4x4 block: color0 = red (0xF800), color1 = blue (0x001F), all indices 0 -> red
    GLint bc_ok = 1;
    const uint8_t bc1[8] = { 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0 };
    GLuint tc;
    glGenTextures(1, &tc);
    glBindTexture(GL_TEXTURE_2D, tc);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 4, 4, 0, 8, bc1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    (void)bc_ok;
    draw_copy(tc);
    CHECK_PIXEL(W / 2, H / 2, 255, 0, 0, 255);

    // Mipmap generation: 4x4 checker of black/white averages to grey at level 2
    std::vector<uint8_t> chk(16 * 4);
    for (int i = 0; i < 16; i++) {
        uint8_t v = ((i % 4) + (i / 4)) % 2 ? 255 : 0;
        chk[i * 4 + 0] = chk[i * 4 + 1] = chk[i * 4 + 2] = v;
        chk[i * 4 + 3] = 255;
    }
    GLuint tm = make_texture(4, 4, chk.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    std::vector<uint8_t> lvl(4);
    glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, lvl.data());
    CHECK(std::abs(lvl[0] - 128) <= 3, "mip level 2 = %d expected ~128", lvl[0]);
    (void)tm;
}

static void test_mrt_and_clearbuffer() {
    printf("multiple render targets + glClearBufferfv\n");
    reset_state();
    GLuint t0 = make_texture(32, 32, nullptr), t1 = make_texture(32, 32, nullptr);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t1, 0);
    const GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, bufs);
    const float c0[] = { 1, 0, 0, 1 }, c1[] = { 0, 0, 1, 1 };
    glClearBufferfv(GL_COLOR, 0, c0);
    glClearBufferfv(GL_COLOR, 1, c1);

    GLuint p = program(R"(#version 430 core
void main() { vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID >> 1) * 4.0 - 1.0); gl_Position = vec4(p, 0, 1); })",
        R"(#version 430 core
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main() { o0 = vec4(0, 1, 0, 1); if (gl_FragCoord.x < 16.0) o1 = vec4(1, 1, 0, 1); else o1 = vec4(0, 1, 1, 1); })");
    CHECK(p != 0, "mrt program");
    glViewport(0, 0, 32, 32);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    CHECK_PIXEL(5, 5, 255, 0, 0, 255);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    CHECK_PIXEL(5, 5, 0, 0, 255, 255);
    glUseProgram(p);
    glBindVertexArray(g_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    CHECK_PIXEL(5, 5, 0, 255, 0, 255);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    CHECK_PIXEL(5, 5, 255, 255, 0, 255);
    CHECK_PIXEL(25, 5, 0, 255, 255, 255);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void test_query_and_binary() {
    printf("occlusion query + program binary round trip\n");
    reset_state();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint q;
    glGenQueries(1, &q);
    const float quad[] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    set_params(1, 1, 1, 1);
    glBeginQuery(GL_SAMPLES_PASSED, q);
    draw_tris(quad, 6);
    glEndQuery(GL_SAMPLES_PASSED);
    dvk_swap_buffers();
    GLuint samples = 0;
    glGetQueryObjectuiv(q, GL_QUERY_RESULT, &samples);
    CHECK(samples == (GLuint)(W * H), "samples passed %u expected %d", samples, W * H);

    GLint len = 0;
    glGetProgramiv(g_copy_prog, GL_PROGRAM_BINARY_LENGTH, &len);
    std::vector<uint8_t> bin((size_t)len);
    GLenum fmt = 0;
    glGetProgramBinary(g_copy_prog, len, &len, &fmt, bin.data());
    GLuint p2 = glCreateProgram();
    glProgramBinary(p2, fmt, bin.data(), len);
    GLint ok = 0;
    glGetProgramiv(p2, GL_LINK_STATUS, &ok);
    CHECK(ok, "program binary reload failed");
    GLuint keep = g_copy_prog;
    g_copy_prog = p2;
    const uint8_t white[] = { 255, 255, 255, 255 };
    draw_copy(make_texture(1, 1, white));
    CHECK_PIXEL(W / 2, H / 2, 255, 255, 255, 255);
    g_copy_prog = keep;
}

static void test_all_divagl_shaders() {
    printf("compile + link every DivaGL shader pair (all flags 0, then all flags 1)\n");
    DIR* d = opendir(DIVAVK_SHADER_DIR);
    if (!d) {
        CHECK(false, "cannot open %s", DIVAVK_SHADER_DIR);
        return;
    }
    std::vector<std::string> verts;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".vert")
            verts.push_back(n.substr(0, n.size() - 5));
    }
    closedir(d);
    // Every program also gets one draw into a small RGBA16F target, so a graphics pipeline
    // is created for it (shader interface, descriptor layout, vertex input all checked).
    GLuint target = 0, fbo = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 16, 16, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glViewport(0, 0, 16, 16);
    dvk_stats before;
    dvk_get_stats(&before);

    int linked = 0, total = 0;
    for (int value = 0; value <= 1; value++)
    for (const std::string& base : verts) {
        std::string fs_path = std::string(DIVAVK_SHADER_DIR) + "/" + base + ".frag";
        if (read_file(fs_path).empty())
            continue;
        total++;
        GLuint p = program(divagl_preprocess(base + ".vert", 16, value), divagl_preprocess(base + ".frag", 16, value));
        if (p) {
            linked++;
            glUseProgram(p);
            glBindVertexArray(g_empty_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDeleteProgram(p);
        }
        else
            printf("  %s (flags=%d) failed\n", base.c_str(), value);
    }
    dvk_swap_buffers();
    dvk_stats after;
    dvk_get_stats(&after);
    printf("  %llu pipelines created for them\n", (unsigned long long)(after.pipelines_created - before.pipelines_created));
    CHECK(after.pipelines_created - before.pipelines_created == (uint64_t)linked, "pipeline creation failed for some programs");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    printf("  %d/%d programs linked\n", linked, total);
    CHECK(linked == total, "%d programs failed", total - linked);
}

static void test_mid_frame_updates() {
    printf("static buffer + texture updated after use in the same frame\n");
    reset_state();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Static vertex buffer: draw, overwrite, draw again
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const float left[] = { -1, -1, 0, -1, 0, 1, -1, -1, 0, 1, -1, 1 };
    const float right[] = { 0, -1, 1, -1, 1, 1, 0, -1, 1, 1, 0, 1 };
    glBufferData(GL_ARRAY_BUFFER, sizeof(left), left, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glUseProgram(g_prog_color);
    set_params(1, 0, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(right), right);
    set_params(0, 1, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    CHECK_PIXEL(10, H / 2, 255, 0, 0, 255);
    CHECK_PIXEL(W - 10, H / 2, 0, 255, 0, 255);

    // Texture: sample, re-upload, sample again
    const uint8_t red[] = { 255, 0, 0, 255 }, blue[] = { 0, 0, 255, 255 };
    GLuint t = make_texture(1, 1, red);
    glViewport(0, 0, W / 2, H);
    draw_copy(t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
    glViewport(W / 2, 0, W / 2, H);
    draw_copy(t);
    glViewport(0, 0, W, H);
    CHECK_PIXEL(10, H / 2, 255, 0, 0, 255);
    CHECK_PIXEL(W - 10, H / 2, 0, 0, 255, 255);
}

// DivaGL's IBL cube maps: RGBA16F half-float faces uploaded one mip level at a time
// (all six faces of level 0, then level 1, ...), sampled with textureLod.
static uint16_t to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    if (e == 0) return (uint16_t)sign;
    int ne = (int)e - 127 + 15;
    if (ne <= 0) return (uint16_t)sign;
    if (ne >= 31) return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | (ne << 10) | (m >> 13));
}

static void test_ibl_cube_mips() {
    printf("IBL-style cube map: RGBA16F, per-level face uploads, textureLod\n");
    reset_state();
    GLuint cube;
    glGenTextures(1, &cube);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const float level_color[3][4] = { { 0.5f, 0, 0, 1 }, { 0, 0.5f, 0, 1 }, { 0, 0, 0.5f, 1 } };
    for (int level = 0; level < 3; level++) {
        int size = 4 >> level;
        std::vector<uint16_t> px((size_t)size * size * 4);
        for (size_t i = 0; i < px.size(); i++)
            px[i] = to_half(level_color[level][i % 4]);
        for (int f = 0; f < 6; f++)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, level, GL_RGBA16F, size, size, 0, GL_RGBA, GL_HALF_FLOAT, px.data());
    }
    GLuint p = program(R"(#version 430 core
void main() { vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID >> 1) * 4.0 - 1.0); gl_Position = vec4(p, 0, 1); })",
        R"(#version 430 core
layout(binding = 3) uniform samplerCube u_cube;
layout(location = 0) out vec4 o;
void main() {
    float lod = gl_FragCoord.x < 42.0 ? 0.0 : gl_FragCoord.x < 84.0 ? 1.0 : 2.0;
    o = textureLod(u_cube, vec3(0.3, 1.0, 0.2), lod);
})");
    CHECK(p != 0, "ibl cube program");
    glUseProgram(p);
    glBindVertexArray(g_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK_PIXEL(10, 10, 128, 0, 0, 255);
    CHECK_PIXEL(60, 10, 0, 128, 0, 255);
    CHECK_PIXEL(110, 10, 0, 0, 128, 255);
    glActiveTexture(GL_TEXTURE0);
}

static void test_cube_and_sampler_objects() {
    printf("cube map sampling + sampler objects\n");
    reset_state();
    GLuint cube;
    glGenTextures(1, &cube);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    const uint8_t faces[6][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
        { 255, 255, 0, 255 }, { 0, 255, 255, 255 }, { 255, 0, 255, 255 } };
    for (int i = 0; i < 6; i++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, faces[i]);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLuint p = program(R"(#version 430 core
void main() { vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID >> 1) * 4.0 - 1.0); gl_Position = vec4(p, 0, 1); })",
        R"(#version 430 core
layout(binding = 3) uniform samplerCube u_cube;
layout(location = 0) out vec4 o;
void main() { o = gl_FragCoord.x < 64.0 ? texture(u_cube, vec3(1, 0, 0)) : texture(u_cube, vec3(0, 0, -1)); })");
    CHECK(p != 0, "cube program");
    glUseProgram(p);
    glBindVertexArray(g_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK_PIXEL(10, 10, 255, 0, 0, 255);      // +X
    CHECK_PIXEL(W - 10, 10, 255, 0, 255, 255); // -Z

    // Sampler object overrides texture parameters (nearest vs linear on a 2x1 texture)
    const uint8_t two[] = { 0, 0, 0, 255, 255, 255, 255, 255 };
    glActiveTexture(GL_TEXTURE0);
    GLuint t = make_texture(2, 1, two);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLuint smp;
    glGenSamplers(1, &smp);
    glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glBindSampler(0, smp);
    draw_copy(t);
    CHECK_PIXEL(W / 2 - 4, H / 2, 0, 0, 0, 255);
    CHECK_PIXEL(W / 2 + 4, H / 2, 255, 255, 255, 255);
    glBindSampler(0, 0);
    glActiveTexture(GL_TEXTURE0);
}

static void test_depth_feedback() {
    printf("sampling the bound depth buffer (read-only depth pass)\n");
    reset_state();
    GLuint color = make_texture(32, 32, nullptr);
    GLuint depth;
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 32, 32, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    glViewport(0, 0, 32, 32);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    const float quad[] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    set_params(0, 0, 0, 1, 0, 0, 0.0f); // GL NDC z 0 -> window depth 0.5
    draw_tris(quad, 6);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, 32, 32);
    draw_copy(depth);               // samples the depth attachment of the bound FBO
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    CHECK_PIXEL(16, 16, 128, 0, 0, 255);
    // Back to depth writes in the same framebuffer
    glEnable(GL_DEPTH_TEST);
    set_params(0, 1, 0, 1, 0, 0, -0.5f);
    draw_tris(quad, 6);
    CHECK_PIXEL(16, 16, 0, 255, 0, 255);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
}

// ------------------------------------------------------------------ headless presentation

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

static void test_present_loop() {
    printf("present loop (headless swapchain)\n");
    reset_state();
    for (int i = 0; i < 10; i++) {
        glClearColor(i / 10.0f, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        const float tri[] = { -1, -1, 1, -1, -1, 1 };
        set_params(0, 1, 0, 1);
        draw_tris(tri, 3);
        dvk_swap_buffers();
    }
    CHECK_PIXEL(2, 2, 0, 255, 0, 255);

    // One-frame render target dump: an FBO pass plus the final image
    const char* dir = "/tmp/divavk_dump_test";
    std::string cmd = std::string("rm -rf ") + dir + " && mkdir -p " + dir;
    CHECK(system(cmd.c_str()) == 0, "mkdir");
    dvk_request_frame_dump(dir);
    dvk_swap_buffers(); // dump starts with the next frame
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 32, 32, 0, GL_RGBA, GL_FLOAT, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 32, 32);
    glClearColor(2.0f, 0.5f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const float tri[] = { -1, -1, 1, -1, -1, 1 };
    set_params(0, 0, 1, 1);
    draw_tris(tri, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    draw_copy(tex);
    dvk_swap_buffers();
    std::string idx = read_file(std::string(dir) + "/index.txt");
    printf("%s", idx.c_str());
    CHECK(idx.find("RGBA16F") != std::string::npos && idx.find("final") != std::string::npos, "dump index incomplete");
}

// DivaLinux's GPU colour conversion for movies (src/DivaLinux/movie/movie_yuv.h): colours match
// a CPU reference, and the GL state the game set up is the same afterwards.
static void test_movie_yuv() {
    printf("movie yuv conversion\n");
    const int MW = 32, MH = 32, CW = 16, CH = 16;
    GLuint target;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MW, MH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Luma ramp along x with padding at the end of each row; chroma constant per quadrant.
    const int YS = MW + 8, CS = CW + 4;
    std::vector<uint8_t> yp(YS * MH, 0), up(CS * CH, 0), vp(CS * CH, 0);
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++)
            yp[y * YS + x] = (uint8_t)(32 + x * 6 + (y < MH / 2 ? 0 : 20));
    const uint8_t QU[4] = { 128, 60, 200, 100 }, QV[4] = { 128, 200, 90, 40 };
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            int q = (y >= CH / 2) * 2 + (x >= CW / 2);
            up[y * CS + x] = QU[q];
            vp[y * CS + x] = QV[q];
        }

    // Game-like state that must survive
    GLuint other_tex;
    glGenTextures(1, &other_tex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, other_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, other_tex);
    glUseProgram(g_prog_color);
    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glViewport(3, 4, 50, 60);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(g_vao);
    GLint unit0 = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0);
    glActiveTexture(GL_TEXTURE1);
    while (glGetError() != GL_NO_ERROR) {}

    static MovieYuv::Converter conv;
    for (int full = 0; full < 2; full++)
        for (int m = 0; m < 2; m++) {
            MovieYuv::Planes p = {};
            p.data[0] = yp.data(); p.data[1] = up.data(); p.data[2] = vp.data();
            p.stride[0] = YS; p.stride[1] = CS; p.stride[2] = CS;
            p.width = MW; p.height = MH; p.chromaWidth = CW; p.chromaHeight = CH;
            p.matrix = m ? MovieYuv::BT709 : MovieYuv::BT601;
            p.fullRange = full != 0;
            bool ok = conv.Convert(target, MW, MH, p);
            CHECK(ok, "Convert failed: %s", conv.error.c_str());
            if (!ok)
                return;

            std::vector<uint8_t> out(MW * MH * 4);
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, target);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE1);

            int bad = 0, worst = 0;
            for (int y = 0; y < MH; y++)
                for (int x = 0; x < MW; x++) {
                    if (x % 16 == 0 || x % 16 == 15 || y % 16 == 0 || y % 16 == 15)
                        continue; // chroma is interpolated across the quadrant edges
                    int q = (y >= MH / 2) * 2 + (x >= MW / 2);
                    double Y = yp[y * YS + x] / 255.0, U = QU[q] / 255.0 - 128.0 / 255.0, V = QV[q] / 255.0 - 128.0 / 255.0;
                    if (!full) { Y = (Y - 16.0 / 255.0) * 255.0 / 219.0; U *= 255.0 / 224.0; V *= 255.0 / 224.0; }
                    double r, g, b;
                    if (m) { r = Y + 1.5748 * V; g = Y - 0.1873 * U - 0.4681 * V; b = Y + 1.8556 * U; }
                    else { r = Y + 1.402 * V; g = Y - 0.344136 * U - 0.714136 * V; b = Y + 1.772 * U; }
                    double ref[3] = { r, g, b };
                    for (int c = 0; c < 3; c++) {
                        int e = (int)lround(std::min(std::max(ref[c], 0.0), 1.0) * 255.0);
                        int d = abs(out[(y * MW + x) * 4 + c] - e);
                        worst = std::max(worst, d);
                        if (d > 2)
                            bad++;
                    }
                    if (out[(y * MW + x) * 4 + 3] != 255)
                        bad++;
                }
            CHECK(bad == 0, "matrix %d full %d: %d channels off (worst %d)", m, full, bad, worst);
        }

    GLint v[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, v);
    CHECK((GLuint)v[0] == g_prog_color, "program changed to %d", v[0]);
    glGetIntegerv(GL_ACTIVE_TEXTURE, v);
    CHECK(v[0] == GL_TEXTURE1, "active texture changed to 0x%04X", v[0]);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, v);
    CHECK((GLuint)v[0] == other_tex, "unit 1 binding changed to %d", v[0]);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, v);
    glActiveTexture(GL_TEXTURE1);
    CHECK(v[0] == unit0, "unit 0 binding changed from %d to %d", unit0, v[0]);
    glGetIntegerv(GL_VIEWPORT, v);
    CHECK(v[0] == 3 && v[1] == 4 && v[2] == 50 && v[3] == 60, "viewport changed to %d,%d,%d,%d", v[0], v[1], v[2], v[3]);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, v);
    CHECK(v[0] == 8, "unpack alignment changed to %d", v[0]);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, v);
    CHECK(v[0] == 0, "framebuffer changed to %d", v[0]);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, v);
    CHECK((GLuint)v[0] == g_vao, "vertex array changed to %d", v[0]);
    GLint blend = 0, depth = 0;
    glGetIntegerv(GL_BLEND, &blend);
    glGetIntegerv(GL_DEPTH_TEST, &depth);
    CHECK(blend == 1 && depth == 1, "blend %d / depth test %d changed", blend, depth);
    CHECK(glGetError() == GL_NO_ERROR, "GL error after the conversion");

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glActiveTexture(GL_TEXTURE0);
}

int main(int argc, char** argv) {
    bool validation = argc > 1 && !strcmp(argv[1], "--validate");
    const char* exts[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    dvk_create_info ci = {};
    ci.app_name = "divavk_tests";
    ci.width = W;
    ci.height = H;
    ci.enable_validation = validation;
    ci.instance_extensions = exts;
    ci.instance_extension_count = 2;
    ci.create_surface = create_headless_surface;
    if (!dvk_create(&ci)) {
        printf("dvk_create failed\n");
        return 1;
    }
    load_gl();
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    g_prog_color = program(VS_POS2, FS_COLOR);
    glGenBuffers(1, &g_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    glBufferStorage(GL_UNIFORM_BUFFER, sizeof(Params), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, 64 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    setup_copy();

    test_clear();
    test_orientation_and_winding();
    test_depth();
    test_ubo_ordering();
    test_blend();
    test_divagl_copy_shader();
    test_tone_map_lut();
    test_fbo_and_blit();
    test_formats();
    test_mrt_and_clearbuffer();
    test_query_and_binary();
    test_mid_frame_updates();
    test_cube_and_sampler_objects();
    test_ibl_cube_mips();
    test_depth_feedback();
    test_movie_yuv();
    test_present_loop();
    test_all_divagl_shaders();

    GLenum err = glGetError();
    CHECK(err == GL_NO_ERROR, "glGetError = 0x%04X", err);

    dvk_stats st;
    dvk_get_stats(&st);
    printf("stats: %llu draws, %llu passes (%llu breaks), %llu pipelines, %llu hoisted / %llu inline uploads\n",
        (unsigned long long)st.draws, (unsigned long long)st.render_passes, (unsigned long long)st.render_pass_breaks,
        (unsigned long long)st.pipelines_created, (unsigned long long)st.hoisted_uploads, (unsigned long long)st.inline_uploads);
    dvk_destroy();
    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
