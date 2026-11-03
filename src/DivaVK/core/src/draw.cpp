/*
    DivaVK - draws: pipeline cache, descriptor sets, vertex input.
*/

#include "context.hpp"

namespace dvk {
    VkCompareOp gl_compare_op(GLenum f);

    // ------------------------------------------------------------ enum translation

    static VkBlendFactor blend_factor(GLenum f) {
        switch (f) {
        case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
        case GL_ONE: return VK_BLEND_FACTOR_ONE;
        case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
        }
    }

    static VkBlendOp blend_op(GLenum e) {
        switch (e) {
        case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case GL_MIN: return VK_BLEND_OP_MIN;
        case GL_MAX: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
        }
    }

    static VkStencilOp stencil_op(GLenum o) {
        switch (o) {
        case GL_ZERO: return VK_STENCIL_OP_ZERO;
        case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
        case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case GL_INVERT: return VK_STENCIL_OP_INVERT;
        case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
        }
    }

    static bool topology_for(GLenum mode, VkPrimitiveTopology& out) {
        switch (mode) {
        case GL_POINTS: out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
        case GL_LINES: out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
        case GL_LINE_STRIP: out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
        case GL_TRIANGLES: out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
        case GL_TRIANGLE_STRIP: out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
        case GL_TRIANGLE_FAN: out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
        default: return false;
        }
    }

    // Vertex attribute format. `base` is what the shader declares (0 float, 1 int, 2 uint).
    static VkFormat attrib_format(const VertexAttrib& a, uint8_t base) {
        int n = a.size == GL_BGRA ? 4 : std::min(std::max(a.size, 1), 4);
        if (a.size == GL_BGRA)
            return VK_FORMAT_B8G8R8A8_UNORM;

        auto pick = [n](VkFormat f1, VkFormat f2, VkFormat f3, VkFormat f4) {
            return n == 1 ? f1 : n == 2 ? f2 : n == 3 ? f3 : f4;
        };

        const bool integer = a.integer || base != 0;
        switch (a.type) {
        case GL_FLOAT:
            return pick(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT);
        case GL_HALF_FLOAT:
            return pick(VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT);
        case GL_UNSIGNED_BYTE:
            if (integer)
                return pick(VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT);
            return a.normalized ? pick(VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)
                : pick(VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED);
        case GL_BYTE:
            if (integer)
                return pick(VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT);
            return a.normalized ? pick(VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM)
                : pick(VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED);
        case GL_UNSIGNED_SHORT:
            if (integer)
                return pick(VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT);
            return a.normalized ? pick(VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM)
                : pick(VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED);
        case GL_SHORT:
            if (integer)
                return pick(VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT);
            return a.normalized ? pick(VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM)
                : pick(VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED);
        case GL_UNSIGNED_INT:
            return pick(VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT);
        case GL_INT:
            return pick(VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT);
        case GL_INT_2_10_10_10_REV:
            return a.normalized ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            return a.normalized ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    // 3-component 8/16-bit formats are rarely supported as vertex formats; widen them.
    static VkFormat supported_vertex_format(Context& ctx, VkFormat f) {
        static std::unordered_map<int, VkFormat> cache;
        auto it = cache.find((int)f);
        if (it != cache.end())
            return it->second;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(ctx.phys, f, &fp);
        VkFormat out = f;
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            switch (f) {
            case VK_FORMAT_R8G8B8_UNORM: out = VK_FORMAT_R8G8B8A8_UNORM; break;
            case VK_FORMAT_R8G8B8_SNORM: out = VK_FORMAT_R8G8B8A8_SNORM; break;
            case VK_FORMAT_R8G8B8_UINT: out = VK_FORMAT_R8G8B8A8_UINT; break;
            case VK_FORMAT_R8G8B8_SINT: out = VK_FORMAT_R8G8B8A8_SINT; break;
            case VK_FORMAT_R16G16B16_UNORM: out = VK_FORMAT_R16G16B16A16_UNORM; break;
            case VK_FORMAT_R16G16B16_SNORM: out = VK_FORMAT_R16G16B16A16_SNORM; break;
            case VK_FORMAT_R16G16B16_SFLOAT: out = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            case VK_FORMAT_R16G16B16_UINT: out = VK_FORMAT_R16G16B16A16_UINT; break;
            case VK_FORMAT_R16G16B16_SINT: out = VK_FORMAT_R16G16B16A16_SINT; break;
            default: break;
            }
            log_once(("vfmt" + std::to_string(f)).c_str(),
                "Vertex format %d unsupported, using %d (4th component reads padding)", (int)f, (int)out);
        }
        cache[(int)f] = out;
        return out;
    }

    // ------------------------------------------------------------ pipelines

    struct PipelineKey {
        uint64_t program_uid;
        uint64_t render_pass;
        uint32_t topology;
        uint8_t restart, cull, front_cw, polygon_line;
        uint8_t depth_test, depth_write, depth_func, stencil_test;
        uint8_t stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
        uint8_t blend, blend_src_rgb, blend_dst_rgb, blend_src_a;
        uint8_t blend_dst_a, blend_eq_rgb, blend_eq_a, color_mask;
        uint8_t samples, num_color, pad0, pad1;
        uint32_t attr_format[MAX_VERTEX_ATTRIBS];
        uint32_t attr_stride[MAX_VERTEX_ATTRIBS];
        uint8_t attr_instanced[MAX_VERTEX_ATTRIBS];
    };

    VkPipeline Context::get_pipeline(ProgramObj* p, VkPrimitiveTopology topology, VertexArrayObj* vao) {
        PipelineKey k;
        memset(&k, 0, sizeof(k));
        k.program_uid = p->uid;
        k.render_pass = (uint64_t)(uintptr_t)get_render_pass(pass_key);
        k.topology = (uint32_t)topology;
        k.restart = rs.primitive_restart && (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
            || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
        k.cull = rs.cull ? (rs.cull_mode == GL_FRONT ? 1 : rs.cull_mode == GL_BACK ? 2 : 3) : 0;
        k.front_cw = rs.front_face == GL_CCW; // see context.hpp: GL CCW == Vulkan CW here
        k.polygon_line = rs.polygon_mode == GL_LINE && features.fillModeNonSolid;
        k.depth_test = rs.depth_test && pass_target.depth;
        k.depth_write = rs.depth_test && rs.depth_write && pass_target.depth;
        k.depth_func = (uint8_t)gl_compare_op(rs.depth_func);
        k.stencil_test = rs.stencil_test && pass_target.depth && pass_target.depth->format->stencil;
        if (k.stencil_test) {
            k.stencil_func = (uint8_t)gl_compare_op(rs.stencil_func);
            k.stencil_fail = (uint8_t)stencil_op(rs.stencil_fail);
            k.stencil_zfail = (uint8_t)stencil_op(rs.stencil_zfail);
            k.stencil_zpass = (uint8_t)stencil_op(rs.stencil_zpass);
        }
        k.blend = rs.blend;
        if (k.blend) {
            k.blend_src_rgb = (uint8_t)blend_factor(rs.blend_src_rgb);
            k.blend_dst_rgb = (uint8_t)blend_factor(rs.blend_dst_rgb);
            k.blend_src_a = (uint8_t)blend_factor(rs.blend_src_alpha);
            k.blend_dst_a = (uint8_t)blend_factor(rs.blend_dst_alpha);
            k.blend_eq_rgb = (uint8_t)blend_op(rs.blend_eq_rgb);
            k.blend_eq_a = (uint8_t)blend_op(rs.blend_eq_alpha);
        }
        k.color_mask = (rs.color_mask[0] ? 1 : 0) | (rs.color_mask[1] ? 2 : 0)
            | (rs.color_mask[2] ? 4 : 0) | (rs.color_mask[3] ? 8 : 0);
        k.samples = (uint8_t)pass_target.samples;
        k.num_color = (uint8_t)pass_key.num_color;

        for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
            if (!(p->vertex_inputs & (1u << i)))
                continue;
            const VertexAttrib& a = vao->attribs[i];
            if (a.enabled && a.buffer) {
                k.attr_format[i] = (uint32_t)supported_vertex_format(*this, attrib_format(a, p->vertex_input_base[i]));
                GLsizei stride = a.stride;
                if (!stride) {
                    // Tightly packed
                    size_t comp = a.type == GL_FLOAT || a.type == GL_INT || a.type == GL_UNSIGNED_INT ? 4
                        : a.type == GL_HALF_FLOAT || a.type == GL_SHORT || a.type == GL_UNSIGNED_SHORT ? 2
                        : a.type == GL_INT_2_10_10_10_REV || a.type == GL_UNSIGNED_INT_2_10_10_10_REV ? 0 : 1;
                    stride = comp ? (GLsizei)(comp * (a.size == GL_BGRA ? 4 : a.size)) : 4;
                }
                k.attr_stride[i] = (uint32_t)stride;
                k.attr_instanced[i] = a.divisor ? 1 : 0;
                if (a.divisor > 1)
                    log_once("divisor", "Vertex attrib divisor > 1 not supported (treated as 1)");
            }
            else {
                // Unbound input reads the constant from the null buffer with stride 0
                uint8_t base = p->vertex_input_base[i];
                k.attr_format[i] = base == 1 ? VK_FORMAT_R32G32B32A32_SINT
                    : base == 2 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
                k.attr_stride[i] = 0;
                k.attr_instanced[i] = 0;
            }
        }

        uint64_t h = hash_bytes(&k, sizeof(k));
        auto it = pipelines.find(h);
        if (it != pipelines.end())
            return it->second;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = p->vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = p->fs;
        stages[1].pName = "main";

        VkVertexInputBindingDescription vbind[MAX_VERTEX_ATTRIBS];
        VkVertexInputAttributeDescription vattr[MAX_VERTEX_ATTRIBS];
        uint32_t nv = 0;
        for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
            if (!(p->vertex_inputs & (1u << i)))
                continue;
            vbind[nv] = { i, k.attr_stride[i], k.attr_instanced[i] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX };
            vattr[nv] = { i, i, (VkFormat)k.attr_format[i], 0 };
            nv++;
        }
        VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vis.vertexBindingDescriptionCount = nv;
        vis.pVertexBindingDescriptions = vbind;
        vis.vertexAttributeDescriptionCount = nv;
        vis.pVertexAttributeDescriptions = vattr;

        VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ias.topology = topology;
        ias.primitiveRestartEnable = k.restart;

        VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1;
        vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rst = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rst.polygonMode = k.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rst.cullMode = k.cull == 1 ? VK_CULL_MODE_FRONT_BIT : k.cull == 2 ? VK_CULL_MODE_BACK_BIT
            : k.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
        rst.frontFace = k.front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rst.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo mss = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        mss.rasterizationSamples = (VkSampleCountFlagBits)std::max<uint32_t>(k.samples, 1);

        VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = k.depth_test;
        dss.depthWriteEnable = k.depth_write;
        dss.depthCompareOp = k.depth_test ? (VkCompareOp)k.depth_func : VK_COMPARE_OP_ALWAYS;
        dss.stencilTestEnable = k.stencil_test;
        dss.front.failOp = (VkStencilOp)k.stencil_fail;
        dss.front.depthFailOp = (VkStencilOp)k.stencil_zfail;
        dss.front.passOp = (VkStencilOp)k.stencil_zpass;
        dss.front.compareOp = (VkCompareOp)k.stencil_func;
        dss.back = dss.front;

        VkPipelineColorBlendAttachmentState cba[MAX_COLOR_ATTACHMENTS] = {};
        for (uint32_t i = 0; i < k.num_color; i++) {
            cba[i].blendEnable = k.blend;
            cba[i].srcColorBlendFactor = (VkBlendFactor)k.blend_src_rgb;
            cba[i].dstColorBlendFactor = (VkBlendFactor)k.blend_dst_rgb;
            cba[i].colorBlendOp = (VkBlendOp)k.blend_eq_rgb;
            cba[i].srcAlphaBlendFactor = (VkBlendFactor)k.blend_src_a;
            cba[i].dstAlphaBlendFactor = (VkBlendFactor)k.blend_dst_a;
            cba[i].alphaBlendOp = (VkBlendOp)k.blend_eq_a;
            cba[i].colorWriteMask = k.color_mask;
        }
        VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = k.num_color;
        cbs.pAttachments = cba;

        VkDynamicState dyn[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]);
        ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vis;
        gpci.pInputAssemblyState = &ias;
        gpci.pViewportState = &vps;
        gpci.pRasterizationState = &rst;
        gpci.pMultisampleState = &mss;
        gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cbs;
        gpci.pDynamicState = &ds;
        gpci.layout = p->layout;
        gpci.renderPass = (VkRenderPass)(uintptr_t)k.render_pass;
        gpci.subpass = 0;

        VkPipeline pipe = VK_NULL_HANDLE;
        DVK_STAGE("the GPU driver compiling a pipeline (vkCreateGraphicsPipelines)");
        VkResult pipe_result = vkCreateGraphicsPipelines(device, pipeline_cache, 1, &gpci, nullptr, &pipe);
        DVK_STAGE("running the game's GL calls");
        if (pipe_result != VK_SUCCESS) {
            log("vkCreateGraphicsPipelines failed (program %u)", p->name);
            pipe = VK_NULL_HANDLE;
        }
        pipelines[h] = pipe;
        stats.pipelines_created++;
        return pipe;
    }

    // ------------------------------------------------------------ descriptors

    // Sampling a color attachment of the current pass is a feedback loop; sampling the depth
    // attachment is fine as long as the pass has it read-only (see Context::draw).
    static bool texture_in_pass(Context& ctx, TextureObj* t) {
        if (!ctx.pass_active)
            return false;
        if (ctx.pass_target.depth == t)
            return !ctx.pass_depth_ro;
        for (uint32_t i = 0; i < ctx.pass_target.num_color; i++)
            if (ctx.pass_target.color[i] == t)
                return true;
        return false;
    }

    // Whether the program samples `t` through any of its sampler bindings with the current units.
    static bool program_samples(Context& ctx, ProgramObj* p, TextureObj* t) {
        if (!t)
            return false;
        for (uint32_t b = 0; b < MAX_TEXTURE_UNITS; b++) {
            if (!(p->sampler_bindings & (1u << b)))
                continue;
            uint32_t unit = p->sampler_unit[b] < MAX_TEXTURE_UNITS ? p->sampler_unit[b] : 0;
            GLuint name = ((p->cube_bindings >> b) & 1) ? ctx.tex_cube[unit] : ctx.tex_2d[unit];
            if (name == t->name)
                return true;
        }
        return false;
    }

    static bool bind_descriptors(Context& ctx, ProgramObj* p, VkCommandBuffer cb) {
        VkDescriptorImageInfo images[MAX_TEXTURE_UNITS];
        VkDescriptorBufferInfo ubos[MAX_UBO_BINDINGS];
        VkDescriptorBufferInfo ssbos[MAX_SSBO_BINDINGS];
        VkWriteDescriptorSet writes[MAX_TEXTURE_UNITS + MAX_UBO_BINDINGS + MAX_SSBO_BINDINGS];
        uint32_t nw = 0;
        uint64_t h = p->uid;

        for (uint32_t b = 0; b < MAX_TEXTURE_UNITS; b++) {
            if (!(p->sampler_bindings & (1u << b)))
                continue;
            uint32_t unit = p->sampler_unit[b] < MAX_TEXTURE_UNITS ? p->sampler_unit[b] : 0;
            bool cube = (p->cube_bindings >> b) & 1;
            TextureObj* t = ctx.textures.get(cube ? ctx.tex_cube[unit] : ctx.tex_2d[unit]);
            if (t && (!t->image || t->samples > 1 || (t->target == GL_TEXTURE_CUBE_MAP) != cube))
                t = nullptr;
            if (t && texture_in_pass(ctx, t)) {
                log_once("feedback", "Texture %u is sampled while bound as a render target; using a dummy", t->name);
                t = nullptr;
            }
            if (!t)
                t = cube ? ctx.dummy_cube.get() : ctx.dummy_2d.get();
            t->last_used_serial = ctx.serial;

            const SamplerState* ss = &t->sampler;
            if (ctx.sampler_units[unit])
                if (SamplerObj* so = ctx.samplers.get(ctx.sampler_units[unit]))
                    ss = &so->state;
            VkImageLayout layout = (ctx.pass_depth_ro && t == ctx.pass_target.depth)
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            images[nw] = { ctx.get_sampler(*ss, t), ctx.texture_view(t), layout };
            hash_combine(h, (uint64_t)layout);
            VkWriteDescriptorSet& w = writes[nw];
            w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[nw];
            hash_combine(h, (uint64_t)(uintptr_t)images[nw].imageView);
            hash_combine(h, (uint64_t)(uintptr_t)images[nw].sampler);
            nw++;
        }

        auto buffer_info = [&](const BufferBinding& bb, VkDeviceSize max_range, VkDescriptorBufferInfo& out) {
            BufferObj* b = ctx.buffers.get(bb.buffer);
            VkBuffer vb;
            VkDeviceSize base;
            if (b && ctx.buffer_resolve(b, vb, base) && (VkDeviceSize)bb.offset < b->size) {
                VkDeviceSize range = bb.size ? (VkDeviceSize)bb.size : b->size - (VkDeviceSize)bb.offset;
                range = std::min(range, b->size - (VkDeviceSize)bb.offset);
                out = { vb, base + (VkDeviceSize)bb.offset, std::min(range, max_range) };
            }
            else
                out = { ctx.dummy_buffer, 0, std::min<VkDeviceSize>(65536, max_range) };
        };

        uint32_t nu = 0;
        for (uint32_t i = 0; i < MAX_UBO_BINDINGS; i++) {
            if (!(p->ubo_bindings & (1u << i)))
                continue;
            uint32_t point = p->ubo_point[i] < MAX_UBO_BINDINGS ? p->ubo_point[i] : 0;
            buffer_info(ctx.ubo[point], ctx.props.limits.maxUniformBufferRange, ubos[nu]);
            VkWriteDescriptorSet& w = writes[nw++];
            w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstBinding = UBO_BINDING_BASE + i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &ubos[nu];
            hash_combine(h, (uint64_t)(uintptr_t)ubos[nu].buffer);
            hash_combine(h, ubos[nu].offset ^ (ubos[nu].range << 40));
            nu++;
        }
        uint32_t ns = 0;
        for (uint32_t i = 0; i < MAX_SSBO_BINDINGS; i++) {
            if (!(p->ssbo_bindings & (1u << i)))
                continue;
            buffer_info(ctx.ssbo[i], ctx.props.limits.maxStorageBufferRange, ssbos[ns]);
            VkWriteDescriptorSet& w = writes[nw++];
            w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstBinding = SSBO_BINDING_BASE + i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &ssbos[ns];
            hash_combine(h, (uint64_t)(uintptr_t)ssbos[ns].buffer);
            hash_combine(h, ssbos[ns].offset ^ (ssbos[ns].range << 40));
            ns++;
        }

        // Reuse the previous set when nothing changed (common for consecutive draws).
        static uint64_t last_hash = 0;
        static uint64_t last_serial = 0;
        static VkDescriptorSet last_set = VK_NULL_HANDLE;
        static VkPipelineLayout last_layout = VK_NULL_HANDLE;
        if (last_hash == h && last_serial == ctx.serial && last_set && last_layout == p->layout && !ctx.descriptors_dirty)
            return true;

        VkDescriptorSet set = ctx.alloc_descriptor_set(p->set_layout);
        if (!set)
            return false;
        for (uint32_t i = 0; i < nw; i++)
            writes[i].dstSet = set;
        if (nw)
            vkUpdateDescriptorSets(ctx.device, nw, writes, 0, nullptr);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &set, 0, nullptr);
        last_hash = h;
        last_serial = ctx.serial;
        last_set = set;
        last_layout = p->layout;
        ctx.descriptors_dirty = false;
        return true;
    }

    // ------------------------------------------------------------ draw

    void Context::draw(GLenum mode, GLint first, GLsizei count, GLsizei instances,
        bool indexed, GLenum index_type, const void* indices, GLint base_vertex) {
        if (count <= 0 || instances <= 0)
            return;
        ProgramObj* p = programs.get(program);
        if (!p || !p->linked) {
            log_once("draw_noprog", "Draw without a linked program (fixed function isn't supported)");
            return;
        }
        VkPrimitiveTopology topology;
        if (!topology_for(mode, topology)) {
            log_once(("mode" + std::to_string(mode)).c_str(), "Unsupported primitive mode 0x%04X", mode);
            return;
        }
        VertexArrayObj* vao = vertex_array ? vertex_arrays.get(vertex_array) : &default_vao;
        if (!vao)
            return;

        // Resolve the index source first: nothing below may submit the frame.
        VkIndexType index_vk = VK_INDEX_TYPE_UINT16;
        const void* client_indices = nullptr;
        BufferObj* eb = nullptr;
        size_t isz = 0;
        if (indexed) {
            switch (index_type) {
            case GL_UNSIGNED_SHORT: index_vk = VK_INDEX_TYPE_UINT16; isz = 2; break;
            case GL_UNSIGNED_INT: index_vk = VK_INDEX_TYPE_UINT32; isz = 4; break;
            case GL_UNSIGNED_BYTE: index_vk = VK_INDEX_TYPE_UINT16; isz = 1; break;
            default: return;
            }
            if (rs.primitive_restart) {
                GLuint expect = isz == 2 ? 0xFFFF : isz == 4 ? 0xFFFFFFFF : 0xFF;
                if (rs.primitive_restart_index != expect)
                    log_once("restart_idx", "Primitive restart index 0x%X isn't the all-ones value; Vulkan requires it",
                        rs.primitive_restart_index);
            }
            eb = buffers.get(vao->element_buffer);
            if (eb && isz == 1) {
                if (!eb->dynamic) {
                    log_once("u8_static", "8-bit indices from a static element buffer aren't supported");
                    return;
                }
                client_indices = eb->shadow.data() + (size_t)(uintptr_t)indices;
                eb = nullptr;
            }
            else if (!eb) {
                if (!indices)
                    return;
                client_indices = indices;
            }
        }

        if (!ensure_pass())
            return;

        // Depth sampled while bound as the depth attachment (TAA camera blur, NPR contours,
        // soft particles): legal when this draw doesn't write depth/stencil, by switching the
        // pass to a read-only depth layout. Switch back when a draw needs to write again.
        if (pass_target.depth) {
            bool samples_depth = program_samples(*this, p, pass_target.depth);
            bool writes_depth = (rs.depth_test && rs.depth_write)
                || (rs.stencil_test && rs.stencil_write_mask && pass_target.depth->format->stencil);
            if (samples_depth && writes_depth)
                log_once("depth_feedback_write", "Draw samples the depth buffer it writes to; sampling a dummy instead");
            int mode = samples_depth && !writes_depth ? 1 : (writes_depth ? 0 : -1);
            if (mode >= 0 && (mode == 1) != pass_depth_ro) {
                stats.pass_breaks++;
                end_pass();
                if (!ensure_pass(mode))
                    return;
            }
        }
        VkCommandBuffer cb = cmd();

        VkPipeline pipe = get_pipeline(p, topology, vao);
        if (!pipe)
            return;
        if (pipe != bound_pipeline) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            bound_pipeline = pipe;
            descriptors_dirty = true;
        }

        // Dynamic state. Images keep GL's row order, so the viewport maps 1:1 (no flip).
        VkViewport vp = { (float)rs.viewport[0], (float)rs.viewport[1], (float)std::max(rs.viewport[2], 1),
            (float)std::max(rs.viewport[3], 1), rs.depth_range[0], rs.depth_range[1] };
        vkCmdSetViewport(cb, 0, 1, &vp);
        int32_t x0 = 0, y0 = 0, x1 = pass_target.width, y1 = pass_target.height;
        if (rs.scissor_test) {
            x0 = std::max(rs.scissor[0], 0);
            y0 = std::max(rs.scissor[1], 0);
            x1 = std::min(rs.scissor[0] + rs.scissor[2], x1);
            y1 = std::min(rs.scissor[1] + rs.scissor[3], y1);
        }
        VkRect2D sc = { { x0, y0 }, { (uint32_t)std::max(x1 - x0, 0), (uint32_t)std::max(y1 - y0, 0) } };
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdSetLineWidth(cb, features.wideLines ? std::max(rs.line_width, 1.0f) : 1.0f);
        vkCmdSetBlendConstants(cb, rs.blend_color);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, rs.stencil_read_mask);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, rs.stencil_write_mask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)rs.stencil_ref);

        if (!bind_descriptors(*this, p, cb))
            return;

        // Vertex buffers: binding i == location i
        VkBuffer vbufs[MAX_VERTEX_ATTRIBS];
        VkDeviceSize voffs[MAX_VERTEX_ATTRIBS];
        uint32_t max_loc = 0;
        for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
            vbufs[i] = null_vertex_buffer;
            voffs[i] = p->vertex_input_base[i] ? 16 : 0;
            if (!(p->vertex_inputs & (1u << i)))
                continue;
            max_loc = i + 1;
            const VertexAttrib& a = vao->attribs[i];
            if (!a.enabled)
                continue;
            if (!a.buffer) {
                log_once("client_arrays", "Client-side vertex arrays aren't supported");
                continue;
            }
            BufferObj* b = buffers.get(a.buffer);
            VkBuffer vb;
            VkDeviceSize base;
            if (b && buffer_resolve(b, vb, base)) {
                vbufs[i] = vb;
                voffs[i] = base + a.offset;
            }
        }
        if (max_loc)
            vkCmdBindVertexBuffers(cb, 0, max_loc, vbufs, voffs);

        query_on_draw();

        if (indexed) {
            VkBuffer ib = VK_NULL_HANDLE;
            VkDeviceSize ioff = 0;
            if (client_indices) {
                // Client-side or 8-bit indices: copy (and widen) into the arena
                size_t out_sz = isz == 1 ? 2 : isz;
                UploadArena::Alloc a = frame().arena.alloc(*this, (VkDeviceSize)count * out_sz, 4);
                if (isz == 1) {
                    const uint8_t* src = (const uint8_t*)client_indices;
                    for (GLsizei i = 0; i < count; i++)
                        ((uint16_t*)a.ptr)[i] = src[i] == 0xFF && rs.primitive_restart ? 0xFFFF : src[i];
                }
                else
                    memcpy(a.ptr, client_indices, (size_t)count * isz);
                ib = a.buffer;
                ioff = a.offset;
            }
            else {
                VkDeviceSize base;
                if (!buffer_resolve(eb, ib, base))
                    return;
                ioff = base + (VkDeviceSize)(uintptr_t)indices;
            }
            vkCmdBindIndexBuffer(cb, ib, ioff, index_vk);
            vkCmdDrawIndexed(cb, (uint32_t)count, (uint32_t)instances, 0, base_vertex, 0);
        }
        else
            vkCmdDraw(cb, (uint32_t)count, (uint32_t)instances, (uint32_t)first, 0);
        stats.draws++;
    }
}
