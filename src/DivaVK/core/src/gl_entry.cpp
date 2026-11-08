/*
    DivaVK - GL entry points.

    Each function below implements one GL call on top of the Context. Everything listed in
    gl_names.inc without an implementation here resolves to a logging stub, so a run of the
    game reports exactly which calls still need work.
*/

#include "context.hpp"
#include <utility>

#ifdef _WIN32
#define DVK_APIENTRY __stdcall
#else
#define DVK_APIENTRY
#endif

#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

using namespace dvk;

#define CTX Context& c = *current_ctx()

namespace dvk {
    bool program_from_spirv(Context& ctx, ProgramObj* p, const std::vector<uint32_t>& vs, const std::vector<uint32_t>& fs);
}

static const GLenum DVK_PROGRAM_BINARY_FORMAT = 0x44564B31; // 'DVK1'

// ------------------------------------------------------------------ errors / strings / state queries

static GLenum DVK_APIENTRY dvk_glGetError() {
    CTX;
    GLenum e = c.error;
    c.error = GL_NO_ERROR;
    return e;
}

static const GLubyte* DVK_APIENTRY dvk_glGetString(GLenum name) {
    static std::string renderer;
    switch (name) {
    case GL_VENDOR: return (const GLubyte*)"DivaVK";
    case GL_RENDERER:
        renderer = std::string("DivaVK on ") + g_ctx->props.deviceName;
        return (const GLubyte*)renderer.c_str();
    case GL_VERSION: return (const GLubyte*)"4.6.0 DivaVK";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"4.60 DivaVK";
    case GL_EXTENSIONS: return (const GLubyte*)"GL_ARB_texture_compression_rgtc GL_EXT_texture_compression_s3tc "
        "GL_EXT_texture_filter_anisotropic GL_ARB_get_program_binary";
    default: return nullptr;
    }
}

static const char* s_extensions[] = {
    "GL_ARB_texture_compression_rgtc", "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_filter_anisotropic", "GL_ARB_get_program_binary",
};

static const GLubyte* DVK_APIENTRY dvk_glGetStringi(GLenum name, GLuint index) {
    if (name == GL_EXTENSIONS && index < sizeof(s_extensions) / sizeof(s_extensions[0]))
        return (const GLubyte*)s_extensions[index];
    return nullptr;
}

// Writes up to 4 values for pname; returns count (0 = unknown pname).
static int get_state(GLenum pname, double* v) {
    CTX;
    const RasterState& rs = c.rs;
    VertexArrayObj* vao = c.vertex_array ? c.vertex_arrays.get(c.vertex_array) : &c.default_vao;
    switch (pname) {
    case GL_MAJOR_VERSION: v[0] = 4; return 1;
    case GL_MINOR_VERSION: v[0] = 6; return 1;
    case GL_NUM_EXTENSIONS: v[0] = sizeof(s_extensions) / sizeof(s_extensions[0]); return 1;
    case GL_MAX_TEXTURE_SIZE: v[0] = c.props.limits.maxImageDimension2D; return 1;
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: v[0] = c.props.limits.maxImageDimensionCube; return 1;
    case GL_MAX_RENDERBUFFER_SIZE: v[0] = c.props.limits.maxFramebufferWidth; return 1;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: v[0] = c.features.samplerAnisotropy ? c.props.limits.maxSamplerAnisotropy : 1.0; return 1;
    case GL_MAX_UNIFORM_BLOCK_SIZE: v[0] = std::min<uint32_t>(c.props.limits.maxUniformBufferRange, 65536); return 1;
    case GL_MAX_SHADER_STORAGE_BLOCK_SIZE: v[0] = std::min<uint32_t>(c.props.limits.maxStorageBufferRange, 0x8000000); return 1;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: v[0] = (double)c.props.limits.minUniformBufferOffsetAlignment; return 1;
    case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT: v[0] = (double)c.props.limits.minStorageBufferOffsetAlignment; return 1;
    case GL_MAX_UNIFORM_BUFFER_BINDINGS: v[0] = MAX_UBO_BINDINGS; return 1;
    case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS: v[0] = MAX_SSBO_BINDINGS; return 1;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: case GL_MAX_TEXTURE_IMAGE_UNITS: v[0] = MAX_TEXTURE_UNITS; return 1;
    case GL_MAX_VERTEX_ATTRIBS: v[0] = MAX_VERTEX_ATTRIBS; return 1;
    case GL_MAX_DRAW_BUFFERS: case GL_MAX_COLOR_ATTACHMENTS: v[0] = MAX_COLOR_ATTACHMENTS; return 1;
    case GL_MAX_SAMPLES: v[0] = 8; return 1;
    case GL_NUM_PROGRAM_BINARY_FORMATS: v[0] = 1; return 1;
    case GL_PROGRAM_BINARY_FORMATS: v[0] = DVK_PROGRAM_BINARY_FORMAT; return 1;
    case GL_CURRENT_PROGRAM: v[0] = c.program; return 1;
    case GL_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + c.active_texture; return 1;
    case GL_TEXTURE_BINDING_2D: v[0] = c.tex_2d[c.active_texture]; return 1;
    case GL_TEXTURE_BINDING_CUBE_MAP: v[0] = c.tex_cube[c.active_texture]; return 1;
    case GL_SAMPLER_BINDING: v[0] = c.sampler_units[c.active_texture]; return 1;
    case GL_ARRAY_BUFFER_BINDING: v[0] = c.array_buffer; return 1;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = vao ? vao->element_buffer : 0; return 1;
    case GL_UNIFORM_BUFFER_BINDING: v[0] = c.generic_uniform_buffer; return 1;
    case GL_SHADER_STORAGE_BUFFER_BINDING: v[0] = c.generic_storage_buffer; return 1;
    case GL_PIXEL_PACK_BUFFER_BINDING: v[0] = c.pixel_pack_buffer; return 1;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: v[0] = c.pixel_unpack_buffer; return 1;
    case GL_VERTEX_ARRAY_BINDING: v[0] = c.vertex_array; return 1;
    case GL_DRAW_FRAMEBUFFER_BINDING: v[0] = c.draw_fbo; return 1;
    case GL_READ_FRAMEBUFFER_BINDING: v[0] = c.read_fbo; return 1;
    case GL_RENDERBUFFER_BINDING: v[0] = c.renderbuffer; return 1;
    case GL_VIEWPORT: for (int i = 0; i < 4; i++) v[i] = rs.viewport[i]; return 4;
    case GL_SCISSOR_BOX: for (int i = 0; i < 4; i++) v[i] = rs.scissor[i]; return 4;
    case GL_SCISSOR_TEST: v[0] = rs.scissor_test; return 1;
    case GL_BLEND: v[0] = rs.blend; return 1;
    case GL_BLEND_SRC_RGB: v[0] = rs.blend_src_rgb; return 1;
    case GL_BLEND_DST_RGB: v[0] = rs.blend_dst_rgb; return 1;
    case GL_BLEND_SRC_ALPHA: v[0] = rs.blend_src_alpha; return 1;
    case GL_BLEND_DST_ALPHA: v[0] = rs.blend_dst_alpha; return 1;
    case GL_BLEND_EQUATION_RGB: v[0] = rs.blend_eq_rgb; return 1;
    case GL_BLEND_EQUATION_ALPHA: v[0] = rs.blend_eq_alpha; return 1;
    case GL_COLOR_WRITEMASK: for (int i = 0; i < 4; i++) v[i] = rs.color_mask[i]; return 4;
    case GL_COLOR_CLEAR_VALUE: for (int i = 0; i < 4; i++) v[i] = c.clear_color_value[i]; return 4;
    case GL_DEPTH_CLEAR_VALUE: v[0] = c.clear_depth_value; return 1;
    case GL_STENCIL_CLEAR_VALUE: v[0] = c.clear_stencil_value; return 1;
    case GL_CULL_FACE: v[0] = rs.cull; return 1;
    case GL_CULL_FACE_MODE: v[0] = rs.cull_mode; return 1;
    case GL_FRONT_FACE: v[0] = rs.front_face; return 1;
    case GL_DEPTH_TEST: v[0] = rs.depth_test; return 1;
    case GL_DEPTH_WRITEMASK: v[0] = rs.depth_write; return 1;
    case GL_DEPTH_FUNC: v[0] = rs.depth_func; return 1;
    case GL_STENCIL_TEST: v[0] = rs.stencil_test; return 1;
    case GL_STENCIL_FUNC: v[0] = rs.stencil_func; return 1;
    case GL_STENCIL_REF: v[0] = rs.stencil_ref; return 1;
    case GL_STENCIL_VALUE_MASK: v[0] = rs.stencil_read_mask; return 1;
    case GL_STENCIL_WRITEMASK: v[0] = rs.stencil_write_mask; return 1;
    case GL_STENCIL_FAIL: v[0] = rs.stencil_fail; return 1;
    case GL_STENCIL_PASS_DEPTH_FAIL: v[0] = rs.stencil_zfail; return 1;
    case GL_STENCIL_PASS_DEPTH_PASS: v[0] = rs.stencil_zpass; return 1;
    case GL_PRIMITIVE_RESTART: v[0] = rs.primitive_restart; return 1;
    case GL_PRIMITIVE_RESTART_INDEX: v[0] = rs.primitive_restart_index; return 1;
    case GL_MULTISAMPLE: v[0] = rs.multisample; return 1;
    case GL_LINE_WIDTH: v[0] = rs.line_width; return 1;
    case GL_UNPACK_ALIGNMENT: v[0] = c.unpack_alignment; return 1;
    case GL_PACK_ALIGNMENT: v[0] = c.pack_alignment; return 1;
    case GL_UNPACK_ROW_LENGTH: v[0] = c.unpack_row_length; return 1;
    default:
        log_once(("getparam" + std::to_string(pname)).c_str(), "glGet: unknown pname 0x%04X", pname);
        return 0;
    }
}

static void DVK_APIENTRY dvk_glGetIntegerv(GLenum pname, GLint* data) {
    double v[4] = {};
    int n = get_state(pname, v);
    for (int i = 0; i < n; i++)
        data[i] = (pname == GL_COLOR_CLEAR_VALUE || pname == GL_DEPTH_CLEAR_VALUE)
            ? (GLint)(v[i] * 2147483647.0) : (GLint)(int64_t)v[i];
}

static void DVK_APIENTRY dvk_glGetFloatv(GLenum pname, GLfloat* data) {
    double v[4] = {};
    int n = get_state(pname, v);
    for (int i = 0; i < n; i++)
        data[i] = (GLfloat)v[i];
}

static void DVK_APIENTRY dvk_glGetBooleanv(GLenum pname, GLboolean* data) {
    double v[4] = {};
    int n = get_state(pname, v);
    for (int i = 0; i < n; i++)
        data[i] = v[i] != 0.0 ? GL_TRUE : GL_FALSE;
}

// Indexed state (glGetIntegeri_v / glGetInteger64i_v / glGetBooleani_v). DivaGL's
// gl_rend_state_rect saves and restores per-unit texture, sampler, UBO and SSBO bindings with
// these; leaving them unimplemented left those values uninitialised, and restoring them bound
// garbage buffer ranges (a crash at the first song).
static bool get_state_indexed(GLenum pname, GLuint index, int64_t* out) {
    CTX;
    switch (pname) {
    case GL_UNIFORM_BUFFER_BINDING:
    case GL_UNIFORM_BUFFER_START:
    case GL_UNIFORM_BUFFER_SIZE: {
        if (index >= MAX_UBO_BINDINGS) { *out = 0; return true; }
        const BufferBinding& b = c.ubo[index];
        *out = pname == GL_UNIFORM_BUFFER_BINDING ? (int64_t)b.buffer
             : pname == GL_UNIFORM_BUFFER_START ? (int64_t)b.offset : (int64_t)b.size;
        return true;
    }
    case GL_SHADER_STORAGE_BUFFER_BINDING:
    case GL_SHADER_STORAGE_BUFFER_START:
    case GL_SHADER_STORAGE_BUFFER_SIZE: {
        if (index >= MAX_SSBO_BINDINGS) { *out = 0; return true; }
        const BufferBinding& b = c.ssbo[index];
        *out = pname == GL_SHADER_STORAGE_BUFFER_BINDING ? (int64_t)b.buffer
             : pname == GL_SHADER_STORAGE_BUFFER_START ? (int64_t)b.offset : (int64_t)b.size;
        return true;
    }
    case GL_TEXTURE_BINDING_2D:
        *out = index < MAX_TEXTURE_UNITS ? c.tex_2d[index] : 0;
        return true;
    case GL_TEXTURE_BINDING_CUBE_MAP:
        *out = index < MAX_TEXTURE_UNITS ? c.tex_cube[index] : 0;
        return true;
    case GL_SAMPLER_BINDING:
        *out = index < MAX_TEXTURE_UNITS ? c.sampler_units[index] : 0;
        return true;
    case GL_COLOR_WRITEMASK:
        // Per-draw-buffer masks aren't tracked separately; report the global one.
        *out = 0;
        return false;
    default:
        log_once(("getparami" + std::to_string(pname)).c_str(), "glGet*i_v: unknown pname 0x%04X", pname);
        *out = 0;
        return true;
    }
}

static void DVK_APIENTRY dvk_glGetIntegeri_v(GLenum pname, GLuint index, GLint* data) {
    if (!data) return;
    if (pname == GL_COLOR_WRITEMASK) { dvk_glGetIntegerv(pname, data); return; }
    int64_t v = 0;
    get_state_indexed(pname, index, &v);
    data[0] = (GLint)v;
}

static void DVK_APIENTRY dvk_glGetInteger64i_v(GLenum pname, GLuint index, GLint64* data) {
    if (!data) return;
    if (pname == GL_COLOR_WRITEMASK) {
        GLint m[4] = {};
        dvk_glGetIntegerv(pname, m);
        for (int i = 0; i < 4; i++) data[i] = m[i];
        return;
    }
    int64_t v = 0;
    get_state_indexed(pname, index, &v);
    data[0] = (GLint64)v;
}

static void DVK_APIENTRY dvk_glGetBooleani_v(GLenum pname, GLuint index, GLboolean* data) {
    if (!data) return;
    if (pname == GL_COLOR_WRITEMASK) { dvk_glGetBooleanv(pname, data); return; }
    int64_t v = 0;
    get_state_indexed(pname, index, &v);
    data[0] = v ? GL_TRUE : GL_FALSE;
}

static void DVK_APIENTRY dvk_glFinish() {
    CTX;
    if (c.recording) {
        Frame& f = c.frame();
        c.submit_frame(false);
        c.wait_frame(f);
    }
}

static void DVK_APIENTRY dvk_glFlush() {}

// ------------------------------------------------------------------ capabilities

static void set_cap(GLenum cap, bool on) {
    CTX;
    RasterState& rs = c.rs;
    switch (cap) {
    case GL_BLEND: rs.blend = on; break;
    case GL_CULL_FACE: rs.cull = on; break;
    case GL_DEPTH_TEST: rs.depth_test = on; break;
    case GL_STENCIL_TEST: rs.stencil_test = on; break;
    case GL_SCISSOR_TEST: rs.scissor_test = on; break;
    case GL_PRIMITIVE_RESTART: case GL_PRIMITIVE_RESTART_FIXED_INDEX: rs.primitive_restart = on; break;
    case GL_MULTISAMPLE: rs.multisample = on; break;
    case GL_DITHER: case GL_DEBUG_OUTPUT: case GL_DEBUG_OUTPUT_SYNCHRONOUS:
    case GL_TEXTURE_CUBE_MAP_SEAMLESS: case GL_FRAMEBUFFER_SRGB: case GL_LINE_SMOOTH:
        break;
    default:
        log_once(("cap" + std::to_string(cap)).c_str(), "glEnable/glDisable: unhandled cap 0x%04X", cap);
        break;
    }
}

static void DVK_APIENTRY dvk_glEnable(GLenum cap) { set_cap(cap, true); }
static void DVK_APIENTRY dvk_glDisable(GLenum cap) { set_cap(cap, false); }

static GLboolean DVK_APIENTRY dvk_glIsEnabled(GLenum cap) {
    double v[4] = {};
    return get_state(cap, v) && v[0] != 0.0;
}

// ------------------------------------------------------------------ fixed function state

static void DVK_APIENTRY dvk_glBlendFunc(GLenum s, GLenum d) {
    CTX;
    c.rs.blend_src_rgb = c.rs.blend_src_alpha = s;
    c.rs.blend_dst_rgb = c.rs.blend_dst_alpha = d;
}
static void DVK_APIENTRY dvk_glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    CTX;
    c.rs.blend_src_rgb = sr; c.rs.blend_dst_rgb = dr;
    c.rs.blend_src_alpha = sa; c.rs.blend_dst_alpha = da;
}
static void DVK_APIENTRY dvk_glBlendEquation(GLenum m) { CTX; c.rs.blend_eq_rgb = c.rs.blend_eq_alpha = m; }
static void DVK_APIENTRY dvk_glBlendEquationSeparate(GLenum r, GLenum a) { CTX; c.rs.blend_eq_rgb = r; c.rs.blend_eq_alpha = a; }
static void DVK_APIENTRY dvk_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    CTX; c.rs.blend_color[0] = r; c.rs.blend_color[1] = g; c.rs.blend_color[2] = b; c.rs.blend_color[3] = a;
}
static void DVK_APIENTRY dvk_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    CTX; c.rs.color_mask[0] = r; c.rs.color_mask[1] = g; c.rs.color_mask[2] = b; c.rs.color_mask[3] = a;
}
static void DVK_APIENTRY dvk_glCullFace(GLenum m) { CTX; c.rs.cull_mode = m; }
static void DVK_APIENTRY dvk_glFrontFace(GLenum m) { CTX; c.rs.front_face = m; }
static void DVK_APIENTRY dvk_glPolygonMode(GLenum face, GLenum mode) { (void)face; CTX; c.rs.polygon_mode = mode; }
static void DVK_APIENTRY dvk_glDepthFunc(GLenum f) { CTX; c.rs.depth_func = f; }
static void DVK_APIENTRY dvk_glDepthMask(GLboolean m) { CTX; c.rs.depth_write = m; }
static void DVK_APIENTRY dvk_glDepthRange(GLdouble n, GLdouble f) { CTX; c.rs.depth_range[0] = (float)n; c.rs.depth_range[1] = (float)f; }
static void DVK_APIENTRY dvk_glDepthRangef(GLfloat n, GLfloat f) { CTX; c.rs.depth_range[0] = n; c.rs.depth_range[1] = f; }
static void DVK_APIENTRY dvk_glStencilFunc(GLenum f, GLint ref, GLuint mask) {
    CTX; c.rs.stencil_func = f; c.rs.stencil_ref = ref; c.rs.stencil_read_mask = mask;
}
static void DVK_APIENTRY dvk_glStencilFuncSeparate(GLenum face, GLenum f, GLint ref, GLuint mask) {
    (void)face; dvk_glStencilFunc(f, ref, mask);
}
static void DVK_APIENTRY dvk_glStencilOp(GLenum f, GLenum zf, GLenum zp) {
    CTX; c.rs.stencil_fail = f; c.rs.stencil_zfail = zf; c.rs.stencil_zpass = zp;
}
static void DVK_APIENTRY dvk_glStencilOpSeparate(GLenum face, GLenum f, GLenum zf, GLenum zp) {
    (void)face; dvk_glStencilOp(f, zf, zp);
}
static void DVK_APIENTRY dvk_glStencilMask(GLuint m) { CTX; c.rs.stencil_write_mask = m; }
static void DVK_APIENTRY dvk_glStencilMaskSeparate(GLenum face, GLuint m) { (void)face; dvk_glStencilMask(m); }
static void DVK_APIENTRY dvk_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    CTX; c.rs.scissor[0] = x; c.rs.scissor[1] = y; c.rs.scissor[2] = w; c.rs.scissor[3] = h;
}
static void DVK_APIENTRY dvk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    CTX; c.rs.viewport[0] = x; c.rs.viewport[1] = y; c.rs.viewport[2] = w; c.rs.viewport[3] = h;
}
static void DVK_APIENTRY dvk_glLineWidth(GLfloat w) { CTX; c.rs.line_width = w; }
static void DVK_APIENTRY dvk_glPrimitiveRestartIndex(GLuint i) { CTX; c.rs.primitive_restart_index = i; }
static void DVK_APIENTRY dvk_glPixelStorei(GLenum pname, GLint v) {
    CTX;
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: c.unpack_alignment = v; break;
    case GL_PACK_ALIGNMENT: c.pack_alignment = v; break;
    case GL_UNPACK_ROW_LENGTH: c.unpack_row_length = v; break;
    default: log_once(("pstore" + std::to_string(pname)).c_str(), "glPixelStorei 0x%04X ignored", pname); break;
    }
}
static void DVK_APIENTRY dvk_glNoop0() {}
static void DVK_APIENTRY dvk_glShadeModel(GLenum) {}
static void DVK_APIENTRY dvk_glClampColor(GLenum, GLenum) {}
static void DVK_APIENTRY dvk_glPointSize(GLfloat) {}
static void DVK_APIENTRY dvk_glHint(GLenum, GLenum) {}

// ------------------------------------------------------------------ buffers

static void DVK_APIENTRY dvk_glGenBuffers(GLsizei n, GLuint* out) {
    CTX;
    for (GLsizei i = 0; i < n; i++)
        out[i] = c.buffers.gen();
}

static void DVK_APIENTRY dvk_glDeleteBuffers(GLsizei n, const GLuint* names) {
    CTX;
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = names[i];
        if (!name)
            continue;
        if (auto b = c.buffers.take(name))
            c.buffer_destroy(b.get());
        if (c.array_buffer == name) c.array_buffer = 0;
        if (c.generic_uniform_buffer == name) c.generic_uniform_buffer = 0;
        if (c.generic_storage_buffer == name) c.generic_storage_buffer = 0;
        if (c.pixel_pack_buffer == name) c.pixel_pack_buffer = 0;
        if (c.pixel_unpack_buffer == name) c.pixel_unpack_buffer = 0;
        for (BufferBinding& bb : c.ubo) if (bb.buffer == name) bb = BufferBinding();
        for (BufferBinding& bb : c.ssbo) if (bb.buffer == name) bb = BufferBinding();
    }
}

static void DVK_APIENTRY dvk_glBindBuffer(GLenum target, GLuint name) {
    CTX;
    if (name)
        c.buffers.get_or_create(name);
    switch (target) {
    case GL_ARRAY_BUFFER: c.array_buffer = name; break;
    case GL_ELEMENT_ARRAY_BUFFER: {
        VertexArrayObj* vao = c.vertex_array ? c.vertex_arrays.get(c.vertex_array) : &c.default_vao;
        if (vao)
            vao->element_buffer = name;
    } break;
    case GL_UNIFORM_BUFFER: c.generic_uniform_buffer = name; break;
    case GL_SHADER_STORAGE_BUFFER: c.generic_storage_buffer = name; break;
    case GL_COPY_READ_BUFFER: c.copy_read_buffer = name; break;
    case GL_COPY_WRITE_BUFFER: c.copy_write_buffer = name; break;
    case GL_PIXEL_PACK_BUFFER: c.pixel_pack_buffer = name; break;
    case GL_PIXEL_UNPACK_BUFFER: c.pixel_unpack_buffer = name; break;
    default: log_once(("bt" + std::to_string(target)).c_str(), "glBindBuffer target 0x%04X ignored", target); break;
    }
}

static void DVK_APIENTRY dvk_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    CTX;
    if (buffer)
        c.buffers.get_or_create(buffer);
    if (target == GL_UNIFORM_BUFFER && index < MAX_UBO_BINDINGS) {
        c.ubo[index] = { buffer, offset, size };
        c.generic_uniform_buffer = buffer;
    }
    else if (target == GL_SHADER_STORAGE_BUFFER && index < MAX_SSBO_BINDINGS) {
        c.ssbo[index] = { buffer, offset, size };
        c.generic_storage_buffer = buffer;
    }
    else
        log_once("bbr_target", "glBindBufferRange/Base: unsupported target or index");
}

static void DVK_APIENTRY dvk_glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    dvk_glBindBufferRange(target, index, buffer, 0, 0);
}

static bool usage_is_dynamic(GLenum usage) {
    return usage != GL_STATIC_DRAW && usage != GL_STATIC_COPY;
}

static void DVK_APIENTRY dvk_glNamedBufferData(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage) {
    CTX;
    BufferObj* b = c.buffers.get_or_create(buffer);
    if (b)
        c.buffer_storage(b, size, data, usage_is_dynamic(usage));
}

static void DVK_APIENTRY dvk_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    CTX;
    if (BufferObj* b = c.buffer_for_target(target))
        c.buffer_storage(b, size, data, usage_is_dynamic(usage) || target == GL_PIXEL_PACK_BUFFER);
}

static void DVK_APIENTRY dvk_glNamedBufferStorage(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags) {
    CTX;
    BufferObj* b = c.buffers.get_or_create(buffer);
    if (b)
        c.buffer_storage(b, size, data, (flags & (GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_READ_BIT)) != 0);
}

static void DVK_APIENTRY dvk_glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    CTX;
    if (BufferObj* b = c.buffer_for_target(target))
        c.buffer_storage(b, size, data, (flags & (GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_READ_BIT)) != 0);
}

static void DVK_APIENTRY dvk_glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    CTX;
    c.buffer_write(c.buffers.get(buffer), offset, size, data);
}

static void DVK_APIENTRY dvk_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    CTX;
    c.buffer_write(c.buffer_for_target(target), offset, size, data);
}

static void DVK_APIENTRY dvk_glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
    CTX;
    BufferObj* b = c.buffer_for_target(target);
    if (b && b->created && offset >= 0 && (VkDeviceSize)(offset + size) <= b->size)
        c.buffer_read(b, offset, size, data);
}

static GLbitfield access_bits(GLenum access) {
    switch (access) {
    case GL_READ_ONLY: return GL_MAP_READ_BIT;
    case GL_WRITE_ONLY: return GL_MAP_WRITE_BIT;
    default: return GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
    }
}

static void* DVK_APIENTRY dvk_glMapBuffer(GLenum target, GLenum access) {
    CTX;
    BufferObj* b = c.buffer_for_target(target);
    return b ? c.buffer_map(b, 0, (GLsizeiptr)b->size, access_bits(access)) : nullptr;
}

static void* DVK_APIENTRY dvk_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    CTX;
    return c.buffer_map(c.buffer_for_target(target), offset, length, access);
}

static void* DVK_APIENTRY dvk_glMapNamedBuffer(GLuint buffer, GLenum access) {
    CTX;
    BufferObj* b = c.buffers.get(buffer);
    return b ? c.buffer_map(b, 0, (GLsizeiptr)b->size, access_bits(access)) : nullptr;
}

static void* DVK_APIENTRY dvk_glMapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    CTX;
    return c.buffer_map(c.buffers.get(buffer), offset, length, access);
}

static GLboolean DVK_APIENTRY dvk_glUnmapBuffer(GLenum target) {
    CTX;
    return c.buffer_unmap(c.buffer_for_target(target)) ? GL_TRUE : GL_FALSE;
}

static GLboolean DVK_APIENTRY dvk_glUnmapNamedBuffer(GLuint buffer) {
    CTX;
    return c.buffer_unmap(c.buffers.get(buffer)) ? GL_TRUE : GL_FALSE;
}

static void buffer_param(BufferObj* b, GLenum pname, GLint* out) {
    if (!b) {
        *out = 0;
        return;
    }
    switch (pname) {
    case GL_BUFFER_SIZE: *out = (GLint)b->size; break;
    case GL_BUFFER_MAPPED: *out = b->map_ptr ? 1 : 0; break;
    case GL_BUFFER_IMMUTABLE_STORAGE: *out = 0; break;
    default: *out = 0; break;
    }
}

static void DVK_APIENTRY dvk_glGetBufferParameteriv(GLenum target, GLenum pname, GLint* out) {
    CTX;
    buffer_param(c.buffer_for_target(target), pname, out);
}

static void DVK_APIENTRY dvk_glGetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint* out) {
    CTX;
    buffer_param(c.buffers.get(buffer), pname, out);
}

static GLboolean DVK_APIENTRY dvk_glIsBuffer(GLuint name) {
    CTX;
    return c.buffers.get(name) ? GL_TRUE : GL_FALSE;
}

// ------------------------------------------------------------------ vertex arrays

static void DVK_APIENTRY dvk_glGenVertexArrays(GLsizei n, GLuint* out) {
    CTX;
    for (GLsizei i = 0; i < n; i++)
        out[i] = c.vertex_arrays.gen();
}

static void DVK_APIENTRY dvk_glDeleteVertexArrays(GLsizei n, const GLuint* names) {
    CTX;
    for (GLsizei i = 0; i < n; i++) {
        c.vertex_arrays.take(names[i]);
        if (c.vertex_array == names[i])
            c.vertex_array = 0;
    }
}

static void DVK_APIENTRY dvk_glBindVertexArray(GLuint name) {
    CTX;
    if (name)
        c.vertex_arrays.get_or_create(name);
    c.vertex_array = name;
}

static GLboolean DVK_APIENTRY dvk_glIsVertexArray(GLuint name) {
    CTX;
    return c.vertex_arrays.get(name) ? GL_TRUE : GL_FALSE;
}

static VertexAttrib* current_attrib(GLuint index) {
    CTX;
    if (index >= MAX_VERTEX_ATTRIBS)
        return nullptr;
    VertexArrayObj* vao = c.vertex_array ? c.vertex_arrays.get(c.vertex_array) : &c.default_vao;
    return vao ? &vao->attribs[index] : nullptr;
}

static void DVK_APIENTRY dvk_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void* pointer) {
    CTX;
    if (VertexAttrib* a = current_attrib(index)) {
        a->size = size;
        a->type = type;
        a->normalized = normalized != 0;
        a->integer = false;
        a->stride = stride;
        a->offset = (uint64_t)(uintptr_t)pointer;
        a->buffer = c.array_buffer;
    }
}

static void DVK_APIENTRY dvk_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    CTX;
    if (VertexAttrib* a = current_attrib(index)) {
        a->size = size;
        a->type = type;
        a->normalized = false;
        a->integer = true;
        a->stride = stride;
        a->offset = (uint64_t)(uintptr_t)pointer;
        a->buffer = c.array_buffer;
    }
}

static void DVK_APIENTRY dvk_glEnableVertexAttribArray(GLuint index) {
    if (VertexAttrib* a = current_attrib(index))
        a->enabled = true;
}

static void DVK_APIENTRY dvk_glDisableVertexAttribArray(GLuint index) {
    if (VertexAttrib* a = current_attrib(index))
        a->enabled = false;
}

static void DVK_APIENTRY dvk_glVertexAttribDivisor(GLuint index, GLuint divisor) {
    if (VertexAttrib* a = current_attrib(index))
        a->divisor = divisor;
}

// ------------------------------------------------------------------ textures

static void DVK_APIENTRY dvk_glGenTextures(GLsizei n, GLuint* out) {
    CTX;
    for (GLsizei i = 0; i < n; i++)
        out[i] = c.textures.gen();
}

static void DVK_APIENTRY dvk_glDeleteTextures(GLsizei n, const GLuint* names) {
    CTX;
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = names[i];
        if (!name)
            continue;
        if (auto t = c.textures.take(name))
            c.texture_destroy(t.get());
        for (uint32_t u = 0; u < MAX_TEXTURE_UNITS; u++) {
            if (c.tex_2d[u] == name) c.tex_2d[u] = 0;
            if (c.tex_cube[u] == name) c.tex_cube[u] = 0;
        }
    }
}

static void DVK_APIENTRY dvk_glActiveTexture(GLenum unit) {
    CTX;
    GLuint u = unit - GL_TEXTURE0;
    if (u < MAX_TEXTURE_UNITS)
        c.active_texture = u;
}

static void bind_texture_unit(GLuint unit, GLenum target, GLuint name) {
    CTX;
    if (unit >= MAX_TEXTURE_UNITS)
        return;
    TextureObj* t = name ? c.textures.get_or_create(name) : nullptr;
    if (t && !t->target)
        t->target = target;
    if (target == GL_TEXTURE_CUBE_MAP)
        c.tex_cube[unit] = name;
    else if (target == GL_TEXTURE_2D)
        c.tex_2d[unit] = name;
    else
        log_once(("bindtex" + std::to_string(target)).c_str(), "glBindTexture target 0x%04X ignored", target);
}

static void DVK_APIENTRY dvk_glBindTexture(GLenum target, GLuint name) {
    CTX;
    bind_texture_unit(c.active_texture, target, name);
}

static void DVK_APIENTRY dvk_glBindTextureUnit(GLuint unit, GLuint name) {
    CTX;
    TextureObj* t = c.textures.get(name);
    if (!name) {
        if (unit < MAX_TEXTURE_UNITS)
            c.tex_2d[unit] = c.tex_cube[unit] = 0;
        return;
    }
    bind_texture_unit(unit, t && t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, name);
}

static void DVK_APIENTRY dvk_glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei w, GLsizei h,
    GLint border, GLenum format, GLenum type, const void* pixels) {
    (void)border;
    CTX;
    c.texture_image(c.texture_for_target(target), target, level, (GLenum)internal_format, w, h, format, type, pixels);
}

static void DVK_APIENTRY dvk_glTexStorage2D(GLenum target, GLsizei levels, GLenum internal_format, GLsizei w, GLsizei h) {
    CTX;
    TextureObj* t = c.texture_for_target(target);
    if (t && c.texture_alloc(t, target, internal_format, w, h, (uint32_t)levels, 1)) {
        t->defined_levels = (1u << levels) - 1;
        t->view_dirty = true;
    }
}

static void DVK_APIENTRY dvk_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
    GLenum format, GLenum type, const void* pixels) {
    CTX;
    c.texture_sub_image(c.texture_for_target(target), target, level, x, y, w, h, format, type, pixels);
}

static void DVK_APIENTRY dvk_glTextureSubImage2D(GLuint texture, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
    GLenum format, GLenum type, const void* pixels) {
    CTX;
    TextureObj* t = c.textures.get(texture);
    c.texture_sub_image(t, t ? t->target : GL_TEXTURE_2D, level, x, y, w, h, format, type, pixels);
}

static void DVK_APIENTRY dvk_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal_format,
    GLsizei w, GLsizei h, GLint border, GLsizei size, const void* data) {
    (void)border;
    CTX;
    c.texture_compressed_image(c.texture_for_target(target), target, level, internal_format, w, h, size, data, false, 0, 0);
}

static void DVK_APIENTRY dvk_glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
    GLsizei w, GLsizei h, GLenum format, GLsizei size, const void* data) {
    CTX;
    c.texture_compressed_image(c.texture_for_target(target), target, level, format, w, h, size, data, true, x, y);
}

static void tex_param(TextureObj* t, GLenum pname, const GLfloat* fv, const GLint* iv) {
    if (!t)
        return;
    switch (pname) {
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
        t->swizzle[pname - GL_TEXTURE_SWIZZLE_R] = iv ? iv[0] : (GLint)fv[0];
        t->view_dirty = true;
        return;
    case GL_TEXTURE_SWIZZLE_RGBA:
        for (int i = 0; i < 4; i++)
            t->swizzle[i] = iv ? iv[i] : (GLint)fv[i];
        t->view_dirty = true;
        return;
    case GL_TEXTURE_BASE_LEVEL:
        t->base_level = iv ? iv[0] : (GLint)fv[0];
        t->view_dirty = true;
        return;
    case GL_TEXTURE_MAX_LEVEL:
        t->max_level = iv ? iv[0] : (GLint)fv[0];
        t->view_dirty = true;
        return;
    default:
        if (!t->sampler.set_param(pname, fv, iv))
            log_once(("texparam" + std::to_string(pname)).c_str(), "glTexParameter 0x%04X ignored", pname);
    }
}

static void DVK_APIENTRY dvk_glTexParameteri(GLenum target, GLenum pname, GLint v) { CTX; tex_param(c.texture_for_target(target), pname, nullptr, &v); }
static void DVK_APIENTRY dvk_glTexParameteriv(GLenum target, GLenum pname, const GLint* v) { CTX; tex_param(c.texture_for_target(target), pname, nullptr, v); }
static void DVK_APIENTRY dvk_glTexParameterf(GLenum target, GLenum pname, GLfloat v) { CTX; tex_param(c.texture_for_target(target), pname, &v, nullptr); }
static void DVK_APIENTRY dvk_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* v) { CTX; tex_param(c.texture_for_target(target), pname, v, nullptr); }

static void DVK_APIENTRY dvk_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* out) {
    CTX;
    TextureObj* t = c.texture_for_target(target);
    *out = 0;
    if (!t || !t->image)
        return;
    switch (pname) {
    case GL_TEXTURE_WIDTH: *out = std::max(t->width >> level, 1); break;
    case GL_TEXTURE_HEIGHT: *out = std::max(t->height >> level, 1); break;
    case GL_TEXTURE_INTERNAL_FORMAT: *out = (GLint)t->internal_format; break;
    case GL_TEXTURE_COMPRESSED: *out = t->format->compressed; break;
    case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
        *out = (GLint)image_size(t->format, std::max(t->width >> level, 1), std::max(t->height >> level, 1));
        break;
    default: break;
    }
}

static void DVK_APIENTRY dvk_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
    CTX;
    TextureObj* t = c.texture_for_target(target);
    if (!t || !t->image)
        return;
    uint32_t layer = (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
        ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
    c.texture_read(t, (uint32_t)level, layer, 0, 0, std::max(t->width >> level, 1), std::max(t->height >> level, 1),
        format, type, pixels, c.pack_alignment);
}

static void DVK_APIENTRY dvk_glGetCompressedTexImage(GLenum target, GLint level, void* pixels) {
    CTX;
    TextureObj* t = c.texture_for_target(target);
    if (!t || !t->image || !t->format->compressed)
        return;
    uint32_t layer = (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
        ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
    c.texture_read(t, (uint32_t)level, layer, 0, 0, std::max(t->width >> level, 1), std::max(t->height >> level, 1),
        0, 0, pixels, 1);
}

static void DVK_APIENTRY dvk_glGenerateMipmap(GLenum target) {
    CTX;
    c.texture_generate_mipmap(c.texture_for_target(target));
}

static void DVK_APIENTRY dvk_glGenerateTextureMipmap(GLuint texture) {
    CTX;
    c.texture_generate_mipmap(c.textures.get(texture));
}

static GLboolean DVK_APIENTRY dvk_glIsTexture(GLuint name) {
    CTX;
    return c.textures.get(name) ? GL_TRUE : GL_FALSE;
}

// ------------------------------------------------------------------ samplers

static void DVK_APIENTRY dvk_glGenSamplers(GLsizei n, GLuint* out) {
    CTX;
    for (GLsizei i = 0; i < n; i++)
        out[i] = c.samplers.gen();
}

static void DVK_APIENTRY dvk_glDeleteSamplers(GLsizei n, const GLuint* names) {
    CTX;
    for (GLsizei i = 0; i < n; i++) {
        c.samplers.take(names[i]);
        for (GLuint& s : c.sampler_units)
            if (s == names[i])
                s = 0;
    }
}

static void DVK_APIENTRY dvk_glBindSampler(GLuint unit, GLuint sampler) {
    CTX;
    if (unit < MAX_TEXTURE_UNITS)
        c.sampler_units[unit] = sampler;
}

static void sampler_param(GLuint sampler, GLenum pname, const GLfloat* fv, const GLint* iv) {
    CTX;
    SamplerObj* s = c.samplers.get_or_create(sampler);
    if (s && !s->state.set_param(pname, fv, iv))
        log_once(("smpparam" + std::to_string(pname)).c_str(), "glSamplerParameter 0x%04X ignored", pname);
}

static void DVK_APIENTRY dvk_glSamplerParameteri(GLuint s, GLenum p, GLint v) { sampler_param(s, p, nullptr, &v); }
static void DVK_APIENTRY dvk_glSamplerParameteriv(GLuint s, GLenum p, const GLint* v) { sampler_param(s, p, nullptr, v); }
static void DVK_APIENTRY dvk_glSamplerParameterf(GLuint s, GLenum p, GLfloat v) { sampler_param(s, p, &v, nullptr); }
static void DVK_APIENTRY dvk_glSamplerParameterfv(GLuint s, GLenum p, const GLfloat* v) { sampler_param(s, p, v, nullptr); }

// ------------------------------------------------------------------ framebuffers / renderbuffers

static void DVK_APIENTRY dvk_glGenFramebuffers(GLsizei n, GLuint* out) {
    CTX;
    for (GLsizei i = 0; i < n; i++)
        out[i] = c.framebuffers.gen();
}

static void DVK_APIENTRY dvk_glDeleteFramebuffers(GLsizei n, const GLuint* names) {
    CTX;
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = names[i];
        if (!name)
            continue;
        if (c.pass_fbo == name)
            c.end_pass();
        if (c.pending_clear_fbo == name)
            c.flush_pending_clear();
        c.framebuffers.take(name);
        if (c.draw_fbo == name) c.draw_fbo = 0;
        if (c.read_fbo == name) c.read_fbo = 0;
    }
}

static void DVK_APIENTRY dvk_glBindFramebuffer(GLenum target, GLuint name) {
    CTX;
    if (name)
        c.framebuffers.get_or_create(name);
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
        c.draw_fbo = name;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
        c.read_fbo = name;
}

static GLboolean DVK_APIENTRY dvk_glIsFramebuffer(GLuint name) {
    CTX;
    return c.framebuffers.get(name) ? GL_TRUE : GL_FALSE;
}

static FramebufferObj* fbo_for_target(GLenum target) {
    CTX;
    GLuint name = target == GL_READ_FRAMEBUFFER ? c.read_fbo : c.draw_fbo;
    return name ? c.framebuffers.get(name) : nullptr;
}

static void attach(GLenum target, GLenum attachment, GLuint texture, bool renderbuffer, GLint level, GLint layer) {
    CTX;
    FramebufferObj* f = fbo_for_target(target);
    if (!f) {
        c.set_error(GL_INVALID_OPERATION);
        return;
    }
    Attachment a = { texture, renderbuffer, level, layer };
    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + MAX_COLOR_ATTACHMENTS)
        f->color[attachment - GL_COLOR_ATTACHMENT0] = a;
    else if (attachment == GL_DEPTH_ATTACHMENT)
        f->depth = a;
    else if (attachment == GL_STENCIL_ATTACHMENT)
        f->stencil = a;
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT)
        f->depth = f->stencil = a;
    else
        return;
    f->generation++;
    if (c.pass_fbo == f->name)
        c.end_pass();
}

static void DVK_APIENTRY dvk_glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level) {
    attach(target, attachment, texture, false, level, 0);
}

static void DVK_APIENTRY dvk_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    GLint layer = (textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
        ? (GLint)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    attach(target, attachment, texture, false, level, layer);
}

static void DVK_APIENTRY dvk_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    (void)rbtarget;
    attach(target, attachment, rb, true, 0, 0);
}

static GLenum DVK_APIENTRY dvk_glCheckFramebufferStatus(GLenum target) {
    CTX;
    GLuint name = target == GL_READ_FRAMEBUFFER ? c.read_fbo : c.draw_fbo;
    if (!name)
        return GL_FRAMEBUFFER_COMPLETE;
    PassTarget t;
    return c.resolve_target(name, t, target == GL_READ_FRAMEBUFFER) ? GL_FRAMEBUFFER_COMPLETE
        : GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}

static void DVK_APIENTRY dvk_glDrawBuffers(GLsizei n, const GLenum* bufs) {
    CTX;
    FramebufferObj* f = c.draw_fbo ? c.framebuffers.get(c.draw_fbo) : nullptr;
    if (!f)
        return;
    f->num_draw_buffers = (uint32_t)std::min<GLsizei>(n, MAX_COLOR_ATTACHMENTS);
    for (uint32_t i = 0; i < f->num_draw_buffers; i++)
        f->draw_buffers[i] = bufs[i];
    f->generation++;
}

static void DVK_APIENTRY dvk_glDrawBuffer(GLenum buf) {
    CTX;
    if (!c.draw_fbo)
        return; // default framebuffer only has one color buffer
    dvk_glDrawBuffers(1, &buf);
}

static void DVK_APIENTRY dvk_glReadBuffer(GLenum buf) {
    CTX;
    FramebufferObj* f = c.read_fbo ? c.framebuffers.get(c.read_fbo) : nullptr;
    if (f)
        f->read_buffer = buf;
}

static void DVK_APIENTRY dvk_glGenRenderbuffers(GLsizei n, GLuint* out) {
    CTX;
    for (GLsizei i = 0; i < n; i++) {
        out[i] = c.textures.gen();
        c.textures.get(out[i])->target = GL_RENDERBUFFER;
    }
}

static void DVK_APIENTRY dvk_glDeleteRenderbuffers(GLsizei n, const GLuint* names) {
    dvk_glDeleteTextures(n, names);
    CTX;
    for (GLsizei i = 0; i < n; i++)
        if (c.renderbuffer == names[i])
            c.renderbuffer = 0;
}

static void DVK_APIENTRY dvk_glBindRenderbuffer(GLenum, GLuint rb) {
    CTX;
    if (rb)
        c.textures.get_or_create(rb);
    c.renderbuffer = rb;
}

static void DVK_APIENTRY dvk_glRenderbufferStorageMultisample(GLenum, GLsizei samples, GLenum internal_format, GLsizei w, GLsizei h) {
    CTX;
    TextureObj* t = c.textures.get(c.renderbuffer);
    if (t && c.texture_alloc(t, GL_RENDERBUFFER, internal_format, w, h, 1, (uint32_t)std::max(samples, 1)))
        t->defined_levels = 1;
}

static void DVK_APIENTRY dvk_glRenderbufferStorage(GLenum target, GLenum internal_format, GLsizei w, GLsizei h) {
    dvk_glRenderbufferStorageMultisample(target, 1, internal_format, w, h);
}

static void DVK_APIENTRY dvk_glClear(GLbitfield mask) { CTX; c.clear(mask); }
static void DVK_APIENTRY dvk_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    CTX; c.clear_color_value[0] = r; c.clear_color_value[1] = g; c.clear_color_value[2] = b; c.clear_color_value[3] = a;
}
static void DVK_APIENTRY dvk_glClearDepth(GLdouble d) { CTX; c.clear_depth_value = (float)d; }
static void DVK_APIENTRY dvk_glClearDepthf(GLfloat d) { CTX; c.clear_depth_value = d; }
static void DVK_APIENTRY dvk_glClearStencil(GLint s) { CTX; c.clear_stencil_value = s; }
static void DVK_APIENTRY dvk_glClearBufferfv(GLenum buf, GLint db, const GLfloat* v) { CTX; c.clear_buffer(buf, db, v, nullptr); }
static void DVK_APIENTRY dvk_glClearBufferiv(GLenum buf, GLint db, const GLint* v) { CTX; c.clear_buffer(buf, db, nullptr, v); }
static void DVK_APIENTRY dvk_glClearBufferfi(GLenum buf, GLint db, GLfloat d, GLint s) { CTX; c.clear_buffer(buf, db, &d, &s); }

static bool blit_color_by_draw(Context& c, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
    GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter);

static void DVK_APIENTRY dvk_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
    GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    CTX;
    if ((mask & GL_COLOR_BUFFER_BIT) && blit_color_by_draw(c, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, filter))
        mask &= ~GL_COLOR_BUFFER_BIT;
    if (mask)
        c.blit_framebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

static void DVK_APIENTRY dvk_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* out) {
    CTX;
    c.read_pixels(x, y, w, h, format, type, out);
}

static void DVK_APIENTRY dvk_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
    GLint x, GLint y, GLsizei w, GLsizei h) {
    CTX;
    c.copy_tex_sub_image(c.texture_for_target(target), target, level, xoff, yoff, x, y, w, h);
}

static void DVK_APIENTRY dvk_glCopyImageSubData(GLuint src, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz,
    GLuint dst, GLenum dt, GLint dl, GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei d) {
    (void)st; (void)dt;
    CTX;
    c.copy_image_sub_data(c.textures.get(src), sl, sx, sy, sz, c.textures.get(dst), dl, dx, dy, dz, w, h, d);
}

// ------------------------------------------------------------------ shaders / programs

static GLuint DVK_APIENTRY dvk_glCreateShader(GLenum type) {
    CTX;
    GLuint n = c.next_shader_program_name++;
    ShaderObj* s = c.shaders.get_or_create(n);
    s->type = type;
    return n;
}

static void DVK_APIENTRY dvk_glDeleteShader(GLuint s) {
    CTX;
    c.shaders.take(s);
}

static void DVK_APIENTRY dvk_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strs, const GLint* lens) {
    CTX;
    ShaderObj* s = c.shaders.get(shader);
    if (!s)
        return;
    s->source.clear();
    for (GLsizei i = 0; i < count; i++)
        if (strs[i])
            s->source.append(strs[i], lens && lens[i] >= 0 ? (size_t)lens[i] : strlen(strs[i]));
}

static void DVK_APIENTRY dvk_glCompileShader(GLuint shader) {
    CTX;
    if (ShaderObj* s = c.shaders.get(shader))
        c.compile_shader(s);
}

static void DVK_APIENTRY dvk_glGetShaderiv(GLuint shader, GLenum pname, GLint* out) {
    CTX;
    ShaderObj* s = c.shaders.get(shader);
    *out = 0;
    if (!s)
        return;
    switch (pname) {
    case GL_COMPILE_STATUS: *out = s->compiled; break;
    case GL_INFO_LOG_LENGTH: *out = s->info_log.empty() ? 0 : (GLint)s->info_log.size() + 1; break;
    case GL_SHADER_TYPE: *out = (GLint)s->type; break;
    case GL_SHADER_SOURCE_LENGTH: *out = (GLint)s->source.size() + 1; break;
    default: break;
    }
}

static void copy_log(const std::string& log, GLsizei max, GLsizei* len, GLchar* out) {
    GLsizei n = max > 0 ? std::min<GLsizei>((GLsizei)log.size(), max - 1) : 0;
    if (out && max > 0) {
        memcpy(out, log.data(), (size_t)n);
        out[n] = 0;
    }
    if (len)
        *len = n;
}

static void DVK_APIENTRY dvk_glGetShaderInfoLog(GLuint shader, GLsizei max, GLsizei* len, GLchar* out) {
    CTX;
    ShaderObj* s = c.shaders.get(shader);
    copy_log(s ? s->info_log : std::string(), max, len, out);
}

static GLuint DVK_APIENTRY dvk_glCreateProgram() {
    CTX;
    GLuint n = c.next_shader_program_name++;
    c.programs.get_or_create(n);
    return n;
}

static void DVK_APIENTRY dvk_glDeleteProgram(GLuint p) {
    CTX;
    if (auto obj = c.programs.take(p))
        c.program_destroy(obj.get());
    if (c.program == p)
        c.program = 0;
}

static GLboolean DVK_APIENTRY dvk_glIsProgram(GLuint p) {
    CTX;
    return c.programs.get(p) ? GL_TRUE : GL_FALSE;
}

static void DVK_APIENTRY dvk_glAttachShader(GLuint program, GLuint shader) {
    CTX;
    if (ProgramObj* p = c.programs.get(program))
        p->shaders.push_back(shader);
}

static void DVK_APIENTRY dvk_glDetachShader(GLuint program, GLuint shader) {
    CTX;
    if (ProgramObj* p = c.programs.get(program))
        p->shaders.erase(std::remove(p->shaders.begin(), p->shaders.end(), shader), p->shaders.end());
}

static void DVK_APIENTRY dvk_glLinkProgram(GLuint program) {
    CTX;
    if (ProgramObj* p = c.programs.get(program))
        c.link_program(p);
}

static void DVK_APIENTRY dvk_glProgramParameteri(GLuint, GLenum, GLint) {}

static void DVK_APIENTRY dvk_glGetProgramiv(GLuint program, GLenum pname, GLint* out) {
    CTX;
    ProgramObj* p = c.programs.get(program);
    *out = 0;
    if (!p)
        return;
    switch (pname) {
    case GL_LINK_STATUS: *out = p->linked; break;
    case GL_VALIDATE_STATUS: *out = p->linked; break;
    case GL_INFO_LOG_LENGTH: *out = p->info_log.empty() ? 0 : (GLint)p->info_log.size() + 1; break;
    case GL_ATTACHED_SHADERS: *out = (GLint)p->shaders.size(); break;
    case GL_PROGRAM_BINARY_LENGTH:
        *out = p->linked ? (GLint)(12 + 4 * (p->vs_spirv.size() + p->fs_spirv.size())) : 0;
        break;
    default: break;
    }
}

static void DVK_APIENTRY dvk_glGetProgramInfoLog(GLuint program, GLsizei max, GLsizei* len, GLchar* out) {
    CTX;
    ProgramObj* p = c.programs.get(program);
    copy_log(p ? p->info_log : std::string(), max, len, out);
}

// Program binaries: 'DVK1' magic + VS/FS SPIR-V word counts + SPIR-V. Lets DivaGL's
// program cache skip glslang entirely on later runs.
static void DVK_APIENTRY dvk_glGetProgramBinary(GLuint program, GLsizei max, GLsizei* len, GLenum* format, void* out) {
    CTX;
    ProgramObj* p = c.programs.get(program);
    if (len)
        *len = 0;
    if (!p || !p->linked)
        return;
    size_t size = 12 + 4 * (p->vs_spirv.size() + p->fs_spirv.size());
    if ((size_t)max < size) {
        c.set_error(GL_INVALID_OPERATION);
        return;
    }
    uint32_t* w = (uint32_t*)out;
    w[0] = DVK_PROGRAM_BINARY_FORMAT;
    w[1] = (uint32_t)p->vs_spirv.size();
    w[2] = (uint32_t)p->fs_spirv.size();
    memcpy(w + 3, p->vs_spirv.data(), p->vs_spirv.size() * 4);
    memcpy(w + 3 + p->vs_spirv.size(), p->fs_spirv.data(), p->fs_spirv.size() * 4);
    if (len)
        *len = (GLsizei)size;
    if (format)
        *format = DVK_PROGRAM_BINARY_FORMAT;
}

static void DVK_APIENTRY dvk_glProgramBinary(GLuint program, GLenum format, const void* data, GLsizei length) {
    CTX;
    ProgramObj* p = c.programs.get(program);
    if (!p)
        return;
    c.program_destroy(p);
    const uint32_t* w = (const uint32_t*)data;
    if (format != DVK_PROGRAM_BINARY_FORMAT || length < 12 || w[0] != DVK_PROGRAM_BINARY_FORMAT
        || (size_t)length != 12 + 4 * ((size_t)w[1] + w[2])) {
        p->info_log = "DivaVK: incompatible program binary";
        return; // GL semantics: link status false, app recompiles from source
    }
    std::vector<uint32_t> vs(w + 3, w + 3 + w[1]);
    std::vector<uint32_t> fs(w + 3 + w[1], w + 3 + w[1] + w[2]);
    program_from_spirv(c, p, vs, fs);
}

static void DVK_APIENTRY dvk_glUseProgram(GLuint program) {
    CTX;
    c.program = program;
}

// Uniform locations: samplers get 1000 + binding; glUniform1i remaps them to texture units.
static GLint DVK_APIENTRY dvk_glGetUniformLocation(GLuint program, const GLchar* name) {
    CTX;
    ProgramObj* p = c.programs.get(program);
    if (!p || !name)
        return -1;
    for (auto& s : p->sampler_names)
        if (s.first == name)
            return 1000 + (GLint)s.second;
    return -1;
}

static void DVK_APIENTRY dvk_glUniform1i(GLint location, GLint v) {
    CTX;
    ProgramObj* p = c.programs.get(c.program);
    if (!p || location < 1000 || location >= 1000 + (GLint)MAX_TEXTURE_UNITS)
        return;
    p->sampler_unit[location - 1000] = (uint8_t)std::min<GLint>(std::max(v, 0), MAX_TEXTURE_UNITS - 1);
}

static GLuint DVK_APIENTRY dvk_glGetUniformBlockIndex(GLuint program, const GLchar* name) {
    CTX;
    ProgramObj* p = c.programs.get(program);
    if (!p || !name)
        return GL_INVALID_INDEX;
    for (auto& u : p->ubo_names)
        if (u.first == name)
            return u.second;
    return GL_INVALID_INDEX;
}

static void DVK_APIENTRY dvk_glUniformBlockBinding(GLuint program, GLuint index, GLuint binding) {
    CTX;
    ProgramObj* p = c.programs.get(program);
    if (p && index < MAX_UBO_BINDINGS && binding < MAX_UBO_BINDINGS)
        p->ubo_point[index] = (uint8_t)binding;
}

// ------------------------------------------------------------------ draws

static void DVK_APIENTRY dvk_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    CTX; c.draw(mode, first, count, 1, false, 0, nullptr, 0);
}
static void DVK_APIENTRY dvk_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei inst) {
    CTX; c.draw(mode, first, count, inst, false, 0, nullptr, 0);
}
static void DVK_APIENTRY dvk_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    CTX; c.draw(mode, 0, count, 1, true, type, indices, 0);
}
static void DVK_APIENTRY dvk_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei inst) {
    CTX; c.draw(mode, 0, count, inst, true, type, indices, 0);
}
static void DVK_APIENTRY dvk_glDrawRangeElements(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const void* indices) {
    CTX; c.draw(mode, 0, count, 1, true, type, indices, 0);
}
static void DVK_APIENTRY dvk_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint bv) {
    CTX; c.draw(mode, 0, count, 1, true, type, indices, bv);
}

// ------------------------------------------------------------------ queries

static void DVK_APIENTRY dvk_glGenQueries(GLsizei n, GLuint* out) {
    CTX;
    for (GLsizei i = 0; i < n; i++)
        out[i] = c.queries.gen();
}

static void DVK_APIENTRY dvk_glDeleteQueries(GLsizei n, const GLuint* names) {
    CTX;
    for (GLsizei i = 0; i < n; i++)
        c.queries.take(names[i]);
}

static GLboolean DVK_APIENTRY dvk_glIsQuery(GLuint q) {
    CTX;
    return c.queries.get(q) ? GL_TRUE : GL_FALSE;
}

static void DVK_APIENTRY dvk_glBeginQuery(GLenum target, GLuint id) { CTX; c.begin_query(target, id); }
static void DVK_APIENTRY dvk_glEndQuery(GLenum target) { CTX; c.end_query(target); }

static bool query_object(GLuint id, GLenum pname, uint64_t& out) {
    CTX;
    QueryObj* q = c.queries.get(id);
    if (!q)
        return false;
    uint64_t v = 0;
    switch (pname) {
    case GL_QUERY_RESULT_AVAILABLE:
        out = c.query_result(q, false, v) ? 1 : 0;
        return true;
    case GL_QUERY_RESULT:
        c.query_result(q, true, v);
        out = v;
        return true;
    case GL_QUERY_RESULT_NO_WAIT:
        if (c.query_result(q, false, v))
            out = v;
        return true;
    default:
        return false;
    }
}

static void DVK_APIENTRY dvk_glGetQueryObjectiv(GLuint id, GLenum pname, GLint* out) {
    uint64_t v = 0;
    if (query_object(id, pname, v))
        *out = (GLint)std::min<uint64_t>(v, 0x7FFFFFFF);
}
static void DVK_APIENTRY dvk_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* out) {
    uint64_t v = 0;
    if (query_object(id, pname, v))
        *out = (GLuint)std::min<uint64_t>(v, 0xFFFFFFFF);
}
static void DVK_APIENTRY dvk_glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* out) {
    uint64_t v = 0;
    if (query_object(id, pname, v))
        *out = v;
}

// ------------------------------------------------------------------ debug

static void DVK_APIENTRY dvk_glPushDebugGroup(GLenum, GLuint, GLsizei length, const GLchar* message) {
    CTX;
    if (!c.has_debug_utils || !vkCmdBeginDebugUtilsLabelEXT || !message)
        return;
    std::string m = length < 0 ? std::string(message) : std::string(message, (size_t)length);
    VkDebugUtilsLabelEXT l = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    l.pLabelName = m.c_str();
    vkCmdBeginDebugUtilsLabelEXT(c.cmd(), &l);
    c.debug_depth++;
}

static void DVK_APIENTRY dvk_glPopDebugGroup() {
    CTX;
    if (!c.has_debug_utils || !vkCmdEndDebugUtilsLabelEXT || !c.debug_depth || !c.recording)
        return;
    vkCmdEndDebugUtilsLabelEXT(c.cmd());
    c.debug_depth--;
}

static void DVK_APIENTRY dvk_glPushGroupMarkerEXT(GLsizei length, const GLchar* marker) {
    dvk_glPushDebugGroup(0, 0, length ? length : -1, marker);
}

static void DVK_APIENTRY dvk_glDebugMessageCallback(const void*, const void*) {}
static void DVK_APIENTRY dvk_glDebugMessageControl(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean) {}

// ------------------------------------------------------------------ proc table

struct ProcEntry {
    const char* name;
    void* fn;
};

#define E(n) { #n, (void*)&dvk_##n }
#define A(n, impl) { #n, (void*)&dvk_##impl }
static const ProcEntry s_procs[] = {
    E(glGetError), E(glGetString), E(glGetStringi), E(glGetIntegerv), E(glGetFloatv), E(glGetBooleanv),
    E(glGetIntegeri_v), E(glGetInteger64i_v), E(glGetBooleani_v),
    E(glFinish), E(glFlush), E(glEnable), E(glDisable), E(glIsEnabled),
    E(glBlendFunc), E(glBlendFuncSeparate), A(glBlendFuncSeparateEXT, glBlendFuncSeparate),
    E(glBlendEquation), A(glBlendEquationEXT, glBlendEquation), E(glBlendEquationSeparate),
    A(glBlendEquationSeparateEXT, glBlendEquationSeparate), E(glBlendColor), E(glColorMask),
    E(glCullFace), E(glFrontFace), E(glPolygonMode), E(glDepthFunc), E(glDepthMask), E(glDepthRange), E(glDepthRangef),
    E(glStencilFunc), E(glStencilFuncSeparate), E(glStencilOp), E(glStencilOpSeparate), E(glStencilMask),
    E(glStencilMaskSeparate), E(glScissor), E(glViewport), E(glLineWidth), E(glPrimitiveRestartIndex),
    E(glPixelStorei), E(glShadeModel), A(glClampColorARB, glClampColor), A(glClampColor, glClampColor),
    E(glPointSize), E(glHint),
    E(glGenBuffers), A(glGenBuffersARB, glGenBuffers), A(glCreateBuffers, glGenBuffers), E(glDeleteBuffers),
    A(glDeleteBuffersARB, glDeleteBuffers), E(glBindBuffer), A(glBindBufferARB, glBindBuffer),
    E(glBindBufferBase), E(glBindBufferRange), E(glBufferData), A(glBufferDataARB, glBufferData),
    E(glBufferSubData), A(glBufferSubDataARB, glBufferSubData), E(glBufferStorage), E(glNamedBufferData),
    E(glNamedBufferSubData), E(glNamedBufferStorage), E(glGetBufferSubData), E(glMapBuffer), A(glMapBufferARB, glMapBuffer),
    E(glMapBufferRange), E(glMapNamedBuffer), E(glMapNamedBufferRange), E(glUnmapBuffer), A(glUnmapBufferARB, glUnmapBuffer),
    E(glUnmapNamedBuffer), E(glGetBufferParameteriv), E(glGetNamedBufferParameteriv), E(glIsBuffer),
    E(glGenVertexArrays), E(glDeleteVertexArrays), E(glBindVertexArray), E(glIsVertexArray),
    E(glVertexAttribPointer), A(glVertexAttribPointerARB, glVertexAttribPointer), E(glVertexAttribIPointer),
    E(glEnableVertexAttribArray), A(glEnableVertexAttribArrayARB, glEnableVertexAttribArray),
    E(glDisableVertexAttribArray), A(glDisableVertexAttribArrayARB, glDisableVertexAttribArray), E(glVertexAttribDivisor),
    E(glGenTextures), E(glDeleteTextures), E(glActiveTexture), A(glActiveTextureARB, glActiveTexture),
    E(glBindTexture), E(glBindTextureUnit), E(glTexImage2D), E(glTexStorage2D), E(glTexSubImage2D), E(glTextureSubImage2D),
    E(glCompressedTexImage2D), A(glCompressedTexImage2DARB, glCompressedTexImage2D), E(glCompressedTexSubImage2D),
    A(glCompressedTexSubImage2DARB, glCompressedTexSubImage2D), E(glTexParameteri), E(glTexParameteriv),
    E(glTexParameterf), E(glTexParameterfv), E(glGetTexLevelParameteriv), E(glGetTexImage), E(glGetCompressedTexImage),
    A(glGetCompressedTexImageARB, glGetCompressedTexImage), E(glGenerateMipmap), A(glGenerateMipmapEXT, glGenerateMipmap),
    E(glGenerateTextureMipmap), E(glIsTexture),
    E(glGenSamplers), E(glDeleteSamplers), E(glBindSampler), E(glSamplerParameteri), E(glSamplerParameteriv),
    E(glSamplerParameterf), E(glSamplerParameterfv),
    E(glGenFramebuffers), A(glGenFramebuffersEXT, glGenFramebuffers), E(glDeleteFramebuffers),
    A(glDeleteFramebuffersEXT, glDeleteFramebuffers), E(glBindFramebuffer), A(glBindFramebufferEXT, glBindFramebuffer),
    E(glIsFramebuffer), E(glFramebufferTexture), A(glFramebufferTextureARB, glFramebufferTexture),
    E(glFramebufferTexture2D), A(glFramebufferTexture2DEXT, glFramebufferTexture2D), E(glFramebufferRenderbuffer),
    A(glFramebufferRenderbufferEXT, glFramebufferRenderbuffer), E(glCheckFramebufferStatus),
    A(glCheckFramebufferStatusEXT, glCheckFramebufferStatus), E(glDrawBuffers), A(glDrawBuffersARB, glDrawBuffers),
    E(glDrawBuffer), E(glReadBuffer), E(glGenRenderbuffers), A(glGenRenderbuffersEXT, glGenRenderbuffers),
    E(glDeleteRenderbuffers), A(glDeleteRenderbuffersEXT, glDeleteRenderbuffers), E(glBindRenderbuffer),
    A(glBindRenderbufferEXT, glBindRenderbuffer), E(glRenderbufferStorage), A(glRenderbufferStorageEXT, glRenderbufferStorage),
    E(glRenderbufferStorageMultisample), A(glRenderbufferStorageMultisampleEXT, glRenderbufferStorageMultisample),
    E(glClear), E(glClearColor), E(glClearDepth), E(glClearDepthf), E(glClearStencil), E(glClearBufferfv),
    E(glClearBufferiv), E(glClearBufferfi), E(glBlitFramebuffer), A(glBlitFramebufferEXT, glBlitFramebuffer),
    E(glReadPixels), E(glCopyTexSubImage2D), E(glCopyImageSubData),
    E(glCreateShader), E(glDeleteShader), E(glShaderSource), E(glCompileShader), E(glGetShaderiv), E(glGetShaderInfoLog),
    E(glCreateProgram), E(glDeleteProgram), E(glIsProgram), E(glAttachShader), E(glDetachShader), E(glLinkProgram),
    E(glProgramParameteri), E(glGetProgramiv), E(glGetProgramInfoLog), E(glGetProgramBinary), E(glProgramBinary),
    E(glUseProgram), E(glGetUniformLocation), E(glUniform1i), E(glGetUniformBlockIndex), E(glUniformBlockBinding),
    E(glDrawArrays), A(glDrawArraysEXT, glDrawArrays), E(glDrawArraysInstanced), A(glDrawArraysInstancedARB, glDrawArraysInstanced),
    E(glDrawElements), E(glDrawElementsInstanced), A(glDrawElementsInstancedARB, glDrawElementsInstanced),
    E(glDrawRangeElements), A(glDrawRangeElementsEXT, glDrawRangeElements), E(glDrawElementsBaseVertex),
    E(glGenQueries), A(glGenQueriesARB, glGenQueries), E(glDeleteQueries), A(glDeleteQueriesARB, glDeleteQueries),
    E(glIsQuery), E(glBeginQuery), A(glBeginQueryARB, glBeginQuery), E(glEndQuery), A(glEndQueryARB, glEndQuery),
    E(glGetQueryObjectiv), A(glGetQueryObjectivARB, glGetQueryObjectiv), E(glGetQueryObjectuiv),
    A(glGetQueryObjectuivARB, glGetQueryObjectuiv), E(glGetQueryObjectui64v),
    E(glPushDebugGroup), E(glPopDebugGroup), E(glPushGroupMarkerEXT), A(glPopGroupMarkerEXT, glPopDebugGroup),
    E(glDebugMessageCallback), E(glDebugMessageControl),
};
#undef E
#undef A

// ------------------------------------------------------------------ logging stubs

/* Colour blits as draws: a quad over the destination rectangle sampling the read framebuffer's colour
   texture into the draw framebuffer, through the ordinary GL path. On an Odin 3 (Adreno 830)
   a transfer that read an image the previous transfer had written (the game's copy into the
   window's framebuffer, then the present blit; the PV picture's slot, then the copy under the
   HUD) got only the first of the two triangles the driver draws it with, whatever the barriers
   between them. A render pass in between (the leek pointer, when it's showing) avoided it.
   Used when the source is a single-sample, non-integer colour texture at level 0 (renderbuffers,
   multisample resolves, depth and stencil keep the transfer path). DIVAVK_BLIT_DRAW=0: off. */
static std::mutex s_blit_m;
static GLuint s_blit_prog = 0, s_blit_prog_same = 0, s_blit_samp[2] = {};
static int s_blit_state = 0;   // 0 untried, 1 ready, -1 off or failed
static bool s_blit_same_off = false;   // DIVAVK_BLIT_SAME=0: same-rectangle copies use the general program
static thread_local GLuint s_blit_ubo = 0;

static bool blit_draw_ready() {
    std::lock_guard<std::mutex> l(s_blit_m);
    if (s_blit_state)
        return s_blit_state > 0;
    s_blit_state = -1;
    if (const char* e = env("DIVAVK_BLIT_DRAW"))
        if (!strcmp(e, "0")) {
            log("Colour blits: transfers (DIVAVK_BLIT_DRAW=0)");
            return false;
        }
    static const char* VS =
        "#version 430 core\n"
        "void main() {\n"
        "    // two triangles exactly covering the viewport (0,1,2 and 2,1,3 of its corners), not\n"
        "    // one oversized triangle: that one is clipped to the viewport by the GPU, and the\n"
        "    // Odin 3 (Adreno 830) showed a picture copied with it only on one side of the\n"
        "    // viewport's diagonal (the right eye's PV picture in SBS)\n"
        "    const int c[6] = int[6](0, 1, 2, 2, 1, 3);\n"
        "    int k = c[gl_VertexIndex];\n"
        "    vec2 p = vec2(float(k & 1), float(k >> 1));\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char* FS =
        "#version 430 core\n"
        "layout(std140, binding = 15) uniform DvkBlit { vec4 dst; vec4 src; vec4 size; } u;\n"
        "layout(binding = 0) uniform sampler2D g_src;\n"
        "layout(location = 0) out vec4 result;\n"
        "void main() {\n"
        "    vec2 t = (gl_FragCoord.xy - u.dst.xy) / (u.dst.zw - u.dst.xy);\n"
        "    vec2 s = u.src.xy + t * (u.src.zw - u.src.xy);\n"
        "    if (u.size.z > 0.5) result = textureLod(g_src, s / u.size.xy, 0.0);\n"
        "    else result = texelFetch(g_src, clamp(ivec2(floor(s)), ivec2(0), ivec2(u.size.xy) - 1), 0);\n"
        "}\n";
    // the same rectangle in and out (the PV picture's save and its copy under the HUD): no
    // uniform block at all. The Odin 3 (Adreno 830) drew the right eye's copy with the block
    // read as zeros in one of the quad's two triangles: that half showed texel (0,0), flat.
    static const char* FS_SAME =
        "#version 430 core\n"
        "layout(binding = 0) uniform sampler2D g_src;\n"
        "layout(location = 0) out vec4 result;\n"
        "void main() {\n"
        "    result = texelFetch(g_src, ivec2(gl_FragCoord.xy), 0);\n"
        "}\n";
    auto build = [](const char* vs, const char* fs) -> GLuint {
        GLuint sh[2] = { dvk_glCreateShader(GL_VERTEX_SHADER), dvk_glCreateShader(GL_FRAGMENT_SHADER) };
        const char* src[2] = { vs, fs };
        GLint ok = 1;
        for (int i = 0; i < 2; i++) {
            dvk_glShaderSource(sh[i], 1, &src[i], nullptr);
            dvk_glCompileShader(sh[i]);
            GLint st = 0;
            dvk_glGetShaderiv(sh[i], GL_COMPILE_STATUS, &st);
            ok &= st;
        }
        GLuint prog = 0;
        if (ok) {
            prog = dvk_glCreateProgram();
            dvk_glAttachShader(prog, sh[0]);
            dvk_glAttachShader(prog, sh[1]);
            dvk_glLinkProgram(prog);
            dvk_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        }
        dvk_glDeleteShader(sh[0]);
        dvk_glDeleteShader(sh[1]);
        return ok ? prog : 0;
    };
    GLuint prog = build(VS, FS);
    s_blit_prog_same = build(VS, FS_SAME);
    const GLint ok = prog && s_blit_prog_same;
    if (!ok) {
        log("Colour blits: the draw program failed; using transfers");
        return false;
    }
    dvk_glGenSamplers(2, s_blit_samp);
    for (int i = 0; i < 2; i++) {
        GLint f = i ? GL_LINEAR : GL_NEAREST;
        dvk_glSamplerParameteri(s_blit_samp[i], GL_TEXTURE_MIN_FILTER, f);
        dvk_glSamplerParameteri(s_blit_samp[i], GL_TEXTURE_MAG_FILTER, f);
        dvk_glSamplerParameteri(s_blit_samp[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        dvk_glSamplerParameteri(s_blit_samp[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    s_blit_prog = prog;
    s_blit_state = 1;
    if (const char* e = env("DIVAVK_BLIT_SAME"))
        s_blit_same_off = !strcmp(e, "0");
    log("Colour blits: drawn (a render pass, not a transfer; DIVAVK_BLIT_DRAW=0 for transfers)");
    return true;
}

static bool blit_color_by_draw(Context& c, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
    GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter) {
    // once per reason: which blits still go to the transfer path (and so could meet the Adreno bug)
    auto transfer = [&](const char* key, const char* why) {
        log_once(key, "Colour blit as a transfer (%s): framebuffer %u -> %u, %dx%d -> %dx%d", why,
            c.read_fbo, c.draw_fbo, std::abs(sx1 - sx0), std::abs(sy1 - sy0), std::abs(dx1 - dx0), std::abs(dy1 - dy0));
        return false;
    };
    if (sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1)
        return false;
    if (!c.read_fbo)
        return transfer("blitx_win", "from the window");
    FramebufferObj* rf = c.framebuffers.get(c.read_fbo);
    if (!rf)
        return transfer("blitx_nofb", "unknown read framebuffer");
    uint32_t ri = rf->read_buffer >= GL_COLOR_ATTACHMENT0 ? rf->read_buffer - GL_COLOR_ATTACHMENT0 : 0;
    if (ri >= MAX_COLOR_ATTACHMENTS)
        return transfer("blitx_rb", "read buffer");
    const Attachment& a = rf->color[ri];
    if (!a.texture)
        return transfer("blitx_none", "nothing attached");
    if (!a.renderbuffer && (a.level != 0 || a.layer != 0))
        return transfer("blitx_level", "a mip level or layer");
    TextureObj* t = c.textures.get(a.texture);
    if (!t || !t->image || !t->format)
        return transfer("blitx_tex", "no texture image");
    if (t->samples != 1)
        return transfer("blitx_ms", "multisample");
    if (t->target != GL_TEXTURE_2D && t->target != GL_RENDERBUFFER)
        return transfer("blitx_2d", "not a 2D texture");
    // a renderbuffer is an image like a 2D texture's (same name table), sampled the same way when
    // its format allows sampling (its resting layout then is the sampling one)
    if (t->rest_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return transfer("blitx_nosample", "a format that can't be sampled");
    if (t->format->depth || t->format->stencil || t->format->is_int)
        return transfer("blitx_fmt", "depth, stencil or integer");
    if (t->base_level != 0)
        return transfer("blitx_base", "base level not 0");
    if (c.draw_fbo == c.read_fbo)
        return transfer("blitx_same", "within one framebuffer");   // a feedback loop as a draw
    if (c.draw_fbo) {
        FramebufferObj* df = c.framebuffers.get(c.draw_fbo);
        if (!df)
            return transfer("blitx_nodfb", "unknown draw framebuffer");
        for (const Attachment& d : df->color)
            if (d.texture == a.texture)   // textures and renderbuffers share names
                return transfer("blitx_loop", "the source texture is also the target");
    }
    if (!blit_draw_ready())
        return false;

    const bool scaled = std::abs(sx1 - sx0) != std::abs(dx1 - dx0) || std::abs(sy1 - sy0) != std::abs(dy1 - dy0);
    const bool linear = filter == GL_LINEAR && scaled;
    const float block[12] = {
        (float)dx0, (float)dy0, (float)dx1, (float)dy1,
        (float)sx0, (float)sy0, (float)sx1, (float)sy1,
        (float)t->width, (float)t->height, linear ? 1.0f : 0.0f, 0.0f,
    };
    dvk_push_gl_state();   // everything below is undone by the pop
    const GLuint read = c.read_fbo, draw = c.draw_fbo;
    if (!s_blit_ubo)
        dvk_glGenBuffers(1, &s_blit_ubo);
    dvk_glBindBuffer(GL_UNIFORM_BUFFER, s_blit_ubo);
    dvk_glBufferData(GL_UNIFORM_BUFFER, sizeof(block), block, GL_DYNAMIC_DRAW);
    dvk_glBindBufferBase(GL_UNIFORM_BUFFER, 15, s_blit_ubo);
    dvk_glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    dvk_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    dvk_glDisable(GL_BLEND);
    dvk_glDisable(GL_CULL_FACE);
    dvk_glDisable(GL_DEPTH_TEST);
    dvk_glDisable(GL_STENCIL_TEST);
    dvk_glDisable(GL_SCISSOR_TEST);
    dvk_glDisable(GL_PRIMITIVE_RESTART);
    dvk_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    dvk_glDepthMask(GL_FALSE);
    dvk_glActiveTexture(GL_TEXTURE0);
    dvk_glBindTexture(GL_TEXTURE_2D, a.texture);
    dvk_glBindSampler(0, s_blit_samp[linear ? 1 : 0]);
    // the same rectangle in and out, inside the source: texel = pixel, no uniform block
    const bool same = sx0 == dx0 && sy0 == dy0 && sx1 == dx1 && sy1 == dy1 && sx1 > sx0 && sy1 > sy0
        && sx0 >= 0 && sy0 >= 0 && sx1 <= t->width && sy1 <= t->height;
    dvk_glUseProgram(same && !s_blit_same_off ? s_blit_prog_same : s_blit_prog);
    dvk_glBindVertexArray(0);
    dvk_glViewport(std::min(dx0, dx1), std::min(dy0, dy1), std::abs(dx1 - dx0), std::abs(dy1 - dy0));
    dvk_glDrawArrays(GL_TRIANGLES, 0, 6);
    dvk_pop_gl_state();
    return true;
}

static const char* s_stub_names[] = {
#include "gl_names.inc"
};
static const size_t STUB_COUNT = sizeof(s_stub_names) / sizeof(s_stub_names[0]);
static const size_t STUB_SLOTS = 1300;  // room for the generated list
static const size_t EXTRA_STUBS = 64;   // for names outside the generated list
static_assert(sizeof(s_stub_names) / sizeof(s_stub_names[0]) <= STUB_SLOTS, "grow STUB_SLOTS");
static std::vector<std::string> s_extra_names;

template <size_t I>
static uintptr_t DVK_APIENTRY stub_fn() {
    const char* name = I < STUB_COUNT ? s_stub_names[I]
        : (I >= STUB_SLOTS && I - STUB_SLOTS < s_extra_names.size() ? s_extra_names[I - STUB_SLOTS].c_str() : "?");
    log_once(name, "Unimplemented GL call: %s", name);
    return 0;
}

template <size_t... I>
static void fill_stubs(void** out, std::index_sequence<I...>) {
    void* fns[] = { (void*)&stub_fn<I>... };
    memcpy(out, fns, sizeof(fns));
}

static void* s_stub_table[STUB_SLOTS + EXTRA_STUBS];

// Only functions DivaVK really implements (no logging stubs): for callers that test whether
// an entry point exists, such as GetProcAddress(opengl32.dll, ...) probing for extensions.
extern "C" void* dvk_get_implemented_proc(const char* name) {
    if (!name)
        return nullptr;
    static std::unordered_map<std::string, void*> table;
    if (table.empty())
        for (const ProcEntry& e : s_procs)
            table[e.name] = e.fn;
    auto it = table.find(name);
    return it != table.end() ? it->second : nullptr;
}

extern "C" void* dvk_get_proc_address(const char* name) {
    if (!name)
        return nullptr;
    static std::unordered_map<std::string, void*> table;
    if (table.empty()) {
        fill_stubs(s_stub_table, std::make_index_sequence<STUB_SLOTS + EXTRA_STUBS>());
        for (size_t i = 0; i < STUB_COUNT; i++)
            table[s_stub_names[i]] = s_stub_table[i];
        for (const ProcEntry& e : s_procs)
            table[e.name] = e.fn;
    }
    auto it = table.find(name);
    if (it != table.end())
        return it->second;
    if ((name[0] == 'g' && name[1] == 'l') || !strncmp(name, "wgl", 3)) {
        if (s_extra_names.size() < EXTRA_STUBS) {
            s_extra_names.push_back(name);
            void* fn = s_stub_table[STUB_SLOTS + s_extra_names.size() - 1];
            table[name] = fn;
            return fn;
        }
    }
    return nullptr;
}
