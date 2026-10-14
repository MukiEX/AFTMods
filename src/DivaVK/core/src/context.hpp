/*
    DivaVK - internal object model and context.

    Conventions (see README "Coordinate conventions"):
    - Images keep OpenGL's memory layout: row 0 is the bottom row. Rendering uses a normal
      (positive-height) Vulkan viewport, which produces exactly that layout, so uploads,
      readbacks, blits and gl_FragCoord all match GL without any flipping. Only two
      things differ: triangle winding (GL_CCW == VK_FRONT_FACE_CLOCKWISE) and the final
      present, which flips vertically.
    - Clip-space depth is converted from GL's [-w, w] to Vulkan's [0, w] by a vertex
      shader epilogue, so depth buffer contents are identical to GL's.
    - All GL resource bindings live in descriptor set 0:
        texture unit N  -> binding N        (0..31)
        UBO binding N   -> binding 32 + N   (0..15)
        SSBO binding N  -> binding 48 + N   (0..15)
*/

#pragma once

#include "common.hpp"
#include "formats.hpp"
#include "spirv_reflect.hpp"
#include "../include/divavk/divavk.h"

namespace dvk {
    enum : uint32_t {
        MAX_TEXTURE_UNITS = 32,
        MAX_UBO_BINDINGS = 16,
        MAX_SSBO_BINDINGS = 16,
        UBO_BINDING_BASE = 32,
        SSBO_BINDING_BASE = 48,
        MAX_VERTEX_ATTRIBS = 16,
        MAX_COLOR_ATTACHMENTS = 8,
        FRAMES_IN_FLIGHT = 2,
    };

    struct Context;

    // ---------------------------------------------------------------- Per-frame resources

    struct UploadArena {
        struct Chunk {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation alloc = VK_NULL_HANDLE;
            uint8_t* ptr = nullptr;
            VkDeviceSize size = 0;
            VkDeviceSize used = 0;
        };

        struct Alloc {
            VkBuffer buffer;
            VkDeviceSize offset;
            uint8_t* ptr;
        };

        std::vector<Chunk> chunks;
        size_t current = 0;

        Alloc alloc(Context& ctx, VkDeviceSize size, VkDeviceSize align);
        void reset();
        void destroy(Context& ctx);
    };

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer upload_cmd = VK_NULL_HANDLE; // copies hoisted ahead of the frame
        VkCommandBuffer cmd = VK_NULL_HANDLE;        // everything else, in GL order
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;  // submitted and not yet waited on
        uint64_t serial = 0;
        bool upload_used = false;

        UploadArena arena;
        std::vector<VkDescriptorPool> desc_pools;
        size_t desc_pool_current = 0;
        std::vector<std::function<void()>> garbage;

        VkQueryPool query_pool = VK_NULL_HANDLE;
        uint32_t queries_used = 0;
    };

    // ---------------------------------------------------------------- GL objects

    struct BufferObj {
        GLuint name = 0;
        VkDeviceSize size = 0;
        bool dynamic = false;   // CPU shadow + per-frame versions (UBOs, streaming data)
        bool created = false;

        // Static storage (device local)
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        uint64_t last_used_serial = 0;

        // Dynamic storage
        std::vector<uint8_t> shadow;
        VkBuffer ver_buffer = VK_NULL_HANDLE;
        VkDeviceSize ver_offset = 0;
        uint8_t* ver_ptr = nullptr;
        uint64_t ver_serial = 0;

        // Mapping
        uint8_t* map_ptr = nullptr;
        GLbitfield map_access = 0;
        GLintptr map_offset = 0;
        GLsizeiptr map_length = 0;
        std::vector<uint8_t> map_temp;
    };

    struct SamplerState {
        GLenum min_filter = GL_NEAREST_MIPMAP_LINEAR;
        GLenum mag_filter = GL_LINEAR;
        GLenum wrap_s = GL_REPEAT;
        GLenum wrap_t = GL_REPEAT;
        GLenum wrap_r = GL_REPEAT;
        GLenum compare_mode = GL_NONE;
        GLenum compare_func = GL_LEQUAL;
        float min_lod = -1000.0f;
        float max_lod = 1000.0f;
        float lod_bias = 0.0f;
        float max_anisotropy = 1.0f;
        float border_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        bool set_param(GLenum pname, const float* fv, const GLint* iv);
        uint64_t hash() const;
    };

    struct SamplerObj {
        GLuint name = 0;
        SamplerState state;
    };

    struct TextureObj {
        GLuint name = 0;
        GLenum target = 0;            // GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_RENDERBUFFER
        GLenum internal_format = 0;
        const FormatInfo* format = nullptr;
        int32_t width = 0;
        int32_t height = 0;
        uint32_t layers = 1;
        uint32_t levels = 0;          // allocated
        uint32_t defined_levels = 0;  // bitmask of levels with defined contents
        uint32_t samples = 1;
        bool renderable = false;

        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageLayout rest_layout = VK_IMAGE_LAYOUT_UNDEFINED; // layout between operations
        VkImageView view = VK_NULL_HANDLE;                     // sampling view
        bool view_dirty = true;
        std::unordered_map<uint32_t, VkImageView> attach_views; // (level << 8) | layer
        uint64_t last_used_serial = 0;

        SamplerState sampler;
        GLint swizzle[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
        GLint base_level = 0;
        GLint max_level = 1000;
    };

    struct Attachment {
        GLuint texture = 0;   // texture or renderbuffer name
        bool renderbuffer = false;
        GLint level = 0;
        GLint layer = 0;
    };

    struct FramebufferObj {
        GLuint name = 0;
        Attachment color[MAX_COLOR_ATTACHMENTS];
        Attachment depth;
        Attachment stencil;
        GLenum draw_buffers[MAX_COLOR_ATTACHMENTS] = { GL_COLOR_ATTACHMENT0 };
        uint32_t num_draw_buffers = 1;
        GLenum read_buffer = GL_COLOR_ATTACHMENT0;
        uint64_t generation = 0; // bumps whenever attachments change
    };

    struct VertexAttrib {
        bool enabled = false;
        GLint size = 4;
        GLenum type = GL_FLOAT;
        bool normalized = false;
        bool integer = false;
        GLsizei stride = 0;
        uint64_t offset = 0;
        GLuint buffer = 0;
        GLuint divisor = 0;
    };

    struct VertexArrayObj {
        GLuint name = 0;
        VertexAttrib attribs[MAX_VERTEX_ATTRIBS];
        GLuint element_buffer = 0;
    };

    struct ShaderObj {
        GLuint name = 0;
        GLenum type = 0;
        std::string source;
        bool compiled = false;
        std::string info_log;
        std::vector<uint32_t> spirv;
    };

    struct ProgramObj {
        GLuint name = 0;
        uint64_t uid = 0; // unique across re-links, used in pipeline keys
        std::vector<GLuint> shaders;
        bool linked = false;
        std::string info_log;

        VkShaderModule vs = VK_NULL_HANDLE;
        VkShaderModule fs = VK_NULL_HANDLE;
        ShaderReflection vs_refl;
        ShaderReflection fs_refl;

        uint32_t sampler_bindings = 0;   // bitmask, texture units used
        uint32_t cube_bindings = 0;      // bitmask, units that are samplerCube
        uint32_t ubo_bindings = 0;       // bitmask
        uint32_t ssbo_bindings = 0;      // bitmask
        uint32_t vertex_inputs = 0;      // bitmask of input locations
        uint8_t vertex_input_base[MAX_VERTEX_ATTRIBS] = {}; // 0 float, 1 int, 2 uint
        uint32_t num_frag_outputs = 1;

        std::vector<uint32_t> vs_spirv;  // kept for glGetProgramBinary
        std::vector<uint32_t> fs_spirv;

        // glUniform1i / glUniformBlockBinding remaps (GL-style programs)
        uint8_t sampler_unit[MAX_TEXTURE_UNITS] = {};  // shader binding -> texture unit
        uint8_t ubo_point[MAX_UBO_BINDINGS] = {};      // shader block binding -> GL binding point
        std::unordered_map<std::string, GLint> uniform_locations;
        std::vector<std::pair<std::string, uint32_t>> sampler_names;   // name -> binding
        std::vector<std::pair<std::string, uint32_t>> ubo_names;       // name -> binding

        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
    };

    struct QueryObj {
        GLuint name = 0;
        GLenum target = 0;
        // Result location of the last completed begin/end pair
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t index = 0;
        uint64_t serial = 0;    // frame serial the query was recorded in
        bool issued = false;    // vkCmdBeginQuery recorded for the current pair
        bool has_result = false;
        uint64_t result = 0;
    };

    template <typename T>
    struct NameTable {
        std::unordered_map<GLuint, std::unique_ptr<T>> objects;
        GLuint next = 1;

        GLuint gen() {
            while (objects.count(next))
                next++;
            GLuint n = next++;
            objects[n] = std::make_unique<T>();
            objects[n]->name = n;
            return n;
        }

        T* get(GLuint n) {
            auto it = objects.find(n);
            return it != objects.end() ? it->second.get() : nullptr;
        }

        // GL allows binding a name that was never generated (legacy); create on demand.
        T* get_or_create(GLuint n) {
            if (!n)
                return nullptr;
            auto& p = objects[n];
            if (!p) {
                p = std::make_unique<T>();
                p->name = n;
            }
            return p.get();
        }

        std::unique_ptr<T> take(GLuint n) {
            auto it = objects.find(n);
            if (it == objects.end())
                return nullptr;
            std::unique_ptr<T> p = std::move(it->second);
            objects.erase(it);
            return p;
        }
    };

    // ---------------------------------------------------------------- Fixed-function state

    struct RasterState {
        bool blend = false;
        GLenum blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
        GLenum blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
        GLenum blend_eq_rgb = GL_FUNC_ADD, blend_eq_alpha = GL_FUNC_ADD;
        float blend_color[4] = {};
        bool color_mask[4] = { true, true, true, true };

        bool cull = false;
        GLenum cull_mode = GL_BACK;
        GLenum front_face = GL_CCW;
        GLenum polygon_mode = GL_FILL;

        bool depth_test = false;
        bool depth_write = true;
        GLenum depth_func = GL_LESS;

        bool stencil_test = false;
        GLenum stencil_func = GL_ALWAYS;
        GLint stencil_ref = 0;
        GLuint stencil_read_mask = ~0u;
        GLuint stencil_write_mask = ~0u;
        GLenum stencil_fail = GL_KEEP, stencil_zfail = GL_KEEP, stencil_zpass = GL_KEEP;

        bool scissor_test = false;
        GLint scissor[4] = {};
        GLint viewport[4] = {};
        float depth_range[2] = { 0.0f, 1.0f };
        float line_width = 1.0f;

        bool primitive_restart = false;
        GLuint primitive_restart_index = 0xFFFFFFFF;
        bool multisample = true;
    };

    struct BufferBinding {
        GLuint buffer = 0;
        GLintptr offset = 0;
        GLsizeiptr size = 0; // 0 = whole buffer
    };

    // ---------------------------------------------------------------- Render pass tracking

    struct PassKeyAttachment {
        VkFormat format;
        uint8_t samples;
        uint8_t load_op;  // VkAttachmentLoadOp
        uint8_t layout;   // index into rest layout table (see framebuffer.cpp)
        uint8_t read_only; // depth: subpass uses DEPTH_STENCIL_READ_ONLY (depth sampled while bound)
    };

    struct PassKey {
        PassKeyAttachment color[MAX_COLOR_ATTACHMENTS];
        PassKeyAttachment depth;
        uint32_t num_color;
        uint32_t has_depth;
        bool operator==(const PassKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
    };

    struct PassTarget {
        TextureObj* color[MAX_COLOR_ATTACHMENTS] = {};
        uint32_t color_level[MAX_COLOR_ATTACHMENTS] = {};
        uint32_t color_layer[MAX_COLOR_ATTACHMENTS] = {};
        TextureObj* depth = nullptr;
        uint32_t depth_level = 0;
        uint32_t num_color = 0;   // highest used draw buffer + 1
        int32_t width = 0;
        int32_t height = 0;
        uint32_t samples = 1;
    };

    struct PendingClear {
        bool color[MAX_COLOR_ATTACHMENTS] = {};
        VkClearColorValue color_value[MAX_COLOR_ATTACHMENTS] = {};
        bool depth = false;
        bool stencil = false;
        float depth_value = 1.0f;
        uint32_t stencil_value = 0;
        bool any() const;
    };

    // ---------------------------------------------------------------- Context

    struct Context {
        // Device
        dvk_create_info info = {};
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queue_family = 0;
        VmaAllocator vma = VK_NULL_HANDLE;
        VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props = {};
        VkPhysicalDeviceFeatures features = {};
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        bool has_debug_utils = false;
        VkFormat depth_stencil_format = VK_FORMAT_UNDEFINED; // best match for D24S8

        Frame frames[FRAMES_IN_FLIGHT];
        uint32_t frame_index = 0;
        uint64_t serial = 1;  // serial of the frame being recorded
        bool recording = false;

        // Objects
        NameTable<BufferObj> buffers;
        NameTable<TextureObj> textures;       // includes renderbuffers
        NameTable<SamplerObj> samplers;
        NameTable<FramebufferObj> framebuffers;
        NameTable<VertexArrayObj> vertex_arrays;
        NameTable<ShaderObj> shaders;          // shaders and programs share a namespace in GL
        NameTable<ProgramObj> programs;
        NameTable<QueryObj> queries;
        GLuint next_shader_program_name = 1;
        uint64_t next_program_uid = 1;

        // Default objects
        VertexArrayObj default_vao;
        FramebufferObj default_fbo;
        std::unique_ptr<TextureObj> default_color;
        std::unique_ptr<TextureObj> default_depth;
        std::unique_ptr<TextureObj> dummy_2d;
        std::unique_ptr<TextureObj> dummy_cube;
        VkBuffer null_vertex_buffer = VK_NULL_HANDLE;
        VmaAllocation null_vertex_alloc = VK_NULL_HANDLE;
        VkBuffer dummy_buffer = VK_NULL_HANDLE;       // bound for missing UBO/SSBO bindings
        VmaAllocation dummy_buffer_alloc = VK_NULL_HANDLE;

        // Bindings
        GLuint active_texture = 0;
        GLuint tex_2d[MAX_TEXTURE_UNITS] = {};
        GLuint tex_cube[MAX_TEXTURE_UNITS] = {};
        GLuint sampler_units[MAX_TEXTURE_UNITS] = {};
        GLuint array_buffer = 0;
        GLuint copy_read_buffer = 0, copy_write_buffer = 0;
        GLuint pixel_pack_buffer = 0, pixel_unpack_buffer = 0;
        GLuint generic_uniform_buffer = 0, generic_storage_buffer = 0;
        BufferBinding ubo[MAX_UBO_BINDINGS];
        BufferBinding ssbo[MAX_SSBO_BINDINGS];
        GLuint vertex_array = 0;
        GLuint draw_fbo = 0, read_fbo = 0;
        GLuint renderbuffer = 0;
        GLuint program = 0;
        GLint unpack_alignment = 4, pack_alignment = 4, unpack_row_length = 0;
        GLenum error = GL_NO_ERROR;
        GLuint active_queries_samples = 0; // query name active for GL_SAMPLES_PASSED/ANY_SAMPLES_PASSED
        uint32_t debug_depth = 0;          // open debug labels in the current command buffer

        RasterState rs;
        float clear_color_value[4] = {};
        float clear_depth_value = 1.0f;
        GLint clear_stencil_value = 0;

        // Render pass tracking
        bool pass_active = false;
        bool pass_depth_ro = false;     // current pass has its depth attachment read-only
        GLuint pass_fbo = ~0u;
        uint64_t pass_fbo_generation = 0;
        PassTarget pass_target;
        PassKey pass_key = {};
        PendingClear pending_clear;
        GLuint pending_clear_fbo = ~0u;
        uint64_t pending_clear_generation = 0;
        std::unordered_map<uint64_t, VkRenderPass> render_passes;
        std::unordered_map<uint64_t, VkFramebuffer> vk_framebuffers;
        std::unordered_map<uint64_t, VkPipeline> pipelines;
        std::unordered_map<uint64_t, VkSampler> vk_samplers;

        // Per-draw bound-state caches
        VkPipeline bound_pipeline = VK_NULL_HANDLE;
        bool descriptors_dirty = true;

        // Statistics
        struct Stats {
            uint64_t draws = 0;
            uint64_t passes = 0;
            uint64_t pass_breaks = 0;
            uint64_t pipelines_created = 0;
            uint64_t hoisted_uploads = 0;
            uint64_t inline_uploads = 0;
        } stats;

        // Presentation
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> swapchain_images;
        VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
        VkExtent2D swapchain_extent = {};
        VkSemaphore acquire_sem[FRAMES_IN_FLIGHT] = {};
        std::vector<VkSemaphore> present_sem; // one per swapchain image
        bool swapchain_dirty = false;
        bool vsync = true;

        // ------------------------------------------------ device.cpp
        bool init(const dvk_create_info& ci);
        void shutdown();
        Frame& frame() { return frames[frame_index]; }
        VkCommandBuffer cmd();             // main command buffer, begun lazily
        VkCommandBuffer upload_cmd();      // hoisted transfer command buffer
        void begin_frame_if_needed();
        void submit_frame(bool present);   // ends recording and submits
        void wait_frame(Frame& f);
        void wait_idle_all();
        void defer_destroy(std::function<void()> fn);
        VkDescriptorSet alloc_descriptor_set(VkDescriptorSetLayout layout);
        bool create_swapchain();
        void destroy_swapchain();
        void save_pipeline_cache();
        uint64_t pipelines_at_last_save = 0;

        // ------------------------------------------------ buffer.cpp
        BufferObj* buffer_for_target(GLenum target);
        void buffer_storage(BufferObj* b, GLsizeiptr size, const void* data, bool dynamic);
        void buffer_write(BufferObj* b, GLintptr offset, GLsizeiptr size, const void* data);
        bool buffer_resolve(BufferObj* b, VkBuffer& out_buffer, VkDeviceSize& out_offset);
        void buffer_destroy(BufferObj* b);
        void* buffer_map(BufferObj* b, GLintptr offset, GLsizeiptr length, GLbitfield access);
        bool buffer_unmap(BufferObj* b);
        void buffer_read(BufferObj* b, GLintptr offset, GLsizeiptr size, void* out);

        // ------------------------------------------------ texture.cpp
        TextureObj* texture_for_target(GLenum target);
        bool texture_alloc(TextureObj* t, GLenum target, GLenum internal_format,
            int32_t width, int32_t height, uint32_t levels, uint32_t samples);
        void texture_image(TextureObj* t, GLenum target, GLint level, GLenum internal_format,
            int32_t width, int32_t height, GLenum format, GLenum type, const void* pixels);
        void texture_sub_image(TextureObj* t, GLenum target, GLint level, GLint x, GLint y,
            int32_t width, int32_t height, GLenum format, GLenum type, const void* pixels);
        void texture_compressed_image(TextureObj* t, GLenum target, GLint level, GLenum internal_format,
            int32_t width, int32_t height, GLsizei size, const void* data, bool sub, GLint x, GLint y);
        void texture_generate_mipmap(TextureObj* t);
        void texture_destroy(TextureObj* t);
        VkImageView texture_view(TextureObj* t);
        VkImageView texture_attach_view(TextureObj* t, uint32_t level, uint32_t layer);
        void texture_transition(VkCommandBuffer cb, TextureObj* t, VkImageLayout from, VkImageLayout to,
            uint32_t base_level = 0, uint32_t level_count = VK_REMAINING_MIP_LEVELS);
        VkCommandBuffer transfer_cmd_for(TextureObj* t); // hoisted if unused this frame
        void texture_upload_region(TextureObj* t, uint32_t level, uint32_t layer,
            int32_t x, int32_t y, int32_t w, int32_t h, const void* data, size_t size, size_t row_pitch_texels);
        VkSampler get_sampler(const SamplerState& s, const TextureObj* t);
        void texture_read(TextureObj* t, uint32_t level, uint32_t layer, int32_t x, int32_t y, int32_t w, int32_t h,
            GLenum format, GLenum type, void* out, GLint pack_alignment);

        // ------------------------------------------------ framebuffer.cpp
        FramebufferObj* fbo(GLuint name);
        bool resolve_target(GLuint fbo_name, PassTarget& out, bool for_read);
        void end_pass();
        // depth_mode: -1 keep the current pass's mode, 0 writable depth, 1 read-only depth
        bool ensure_pass(int depth_mode = -1);
        void clear(GLbitfield mask);
        void clear_buffer(GLenum buffer, GLint drawbuffer, const float* fv, const GLint* iv);
        void blit_framebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
            GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter);
        void copy_tex_sub_image(TextureObj* t, GLenum target, GLint level, GLint xoff, GLint yoff,
            GLint x, GLint y, GLsizei w, GLsizei h);
        void copy_image_sub_data(TextureObj* src, GLint src_level, GLint sx, GLint sy, GLint sz,
            TextureObj* dst, GLint dst_level, GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei d);
        void read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* out);
        void flush_pending_clear();
        void on_texture_destroyed(TextureObj* t);

        // ------------------------------------------------ program.cpp
        bool compile_shader(ShaderObj* s);
        bool link_program(ProgramObj* p);
        void program_destroy(ProgramObj* p);

        // ------------------------------------------------ draw.cpp
        void draw(GLenum mode, GLint first, GLsizei count, GLsizei instances,
            bool indexed, GLenum index_type, const void* indices, GLint base_vertex);
        VkPipeline get_pipeline(ProgramObj* p, VkPrimitiveTopology topology, VertexArrayObj* vao);
        VkRenderPass get_render_pass(const PassKey& key);

        // ------------------------------------------------ query.cpp
        void begin_query(GLenum target, GLuint id);
        void end_query(GLenum target);
        void query_on_draw();
        void query_on_pass_end();
        bool query_result(QueryObj* q, bool wait, uint64_t& out);

        // ------------------------------------------------ present
        void present();
        void set_error(GLenum e) { if (error == GL_NO_ERROR) error = e; }
    };

    extern Context* g_ctx;
}
