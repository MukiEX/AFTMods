/*
    DivaVK - GLSL programs: GL-flavoured GLSL -> SPIR-V via glslang, reflection, layouts.

    Shaders arrive exactly as they would for OpenGL 4.3 (DivaGL strips the Vulkan-style
    "set = N" qualifiers before calling glShaderSource). They are compiled with glslang's
    "Vulkan rules relaxed" mode, which accepts GL-style GLSL, and binding namespaces are
    separated with binding shifts (see context.hpp). The vertex stage gets an epilogue
    that converts clip-space depth from GL's [-w, w] to Vulkan's [0, w].
*/

#include "context.hpp"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

namespace dvk {
    void program_global_init() {
        glslang::InitializeProcess();
    }

    void program_global_shutdown() {
        glslang::FinalizeProcess();
    }

    static EShLanguage stage_for(GLenum type) {
        switch (type) {
        case GL_VERTEX_SHADER: return EShLangVertex;
        case GL_FRAGMENT_SHADER: return EShLangFragment;
        case GL_GEOMETRY_SHADER: return EShLangGeometry;
        case GL_COMPUTE_SHADER: return EShLangCompute;
        default: return EShLangCount;
        }
    }

    // Applies DivaVK's source-level adjustments.
    static std::string prepare_source(const std::string& src, GLenum type) {
        std::string s = src;
        size_t insert = 0;
        size_t ver = s.find("#version");
        if (ver != std::string::npos) {
            size_t eol = s.find('\n', ver);
            insert = eol == std::string::npos ? s.size() : eol + 1;
        }
        else
            s = "#version 430 core\n" + s, insert = 18;

        std::string pre;
        if (type == GL_VERTEX_SHADER) {
            pre += "#define main dvk_main_\n";
            s += "\n#undef main\nvoid main() {\n    dvk_main_();\n"
                "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
        }
        s.insert(insert, pre);
        return s;
    }

    static void configure(glslang::TShader& sh, EShLanguage stage) {
        sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        sh.setEnvInputVulkanRulesRelaxed();
        sh.setAutoMapBindings(true);
        sh.setAutoMapLocations(true);
        sh.setShiftBinding(glslang::EResUbo, UBO_BINDING_BASE);
        sh.setShiftBinding(glslang::EResSsbo, SSBO_BINDING_BASE);
        sh.setGlobalUniformSet(0);
        sh.setGlobalUniformBinding(63);
    }

    static const EShMessages MESSAGES = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

    bool Context::compile_shader(ShaderObj* s) {
        s->compiled = false;
        s->info_log.clear();
        EShLanguage stage = stage_for(s->type);
        if (stage == EShLangCount || (stage != EShLangVertex && stage != EShLangFragment)) {
            s->info_log = "DivaVK: only vertex and fragment shaders are supported";
            return false;
        }

        std::string src = prepare_source(s->source, s->type);
        const char* strs[] = { src.c_str() };
        glslang::TShader sh(stage);
        sh.setStrings(strs, 1);
        configure(sh, stage);
        if (!sh.parse(GetDefaultResources(), 430, ECoreProfile, false, false, MESSAGES)) {
            s->info_log = sh.getInfoLog();
            log("Shader compile failed:\n%s", s->info_log.c_str());
            return false;
        }
        s->compiled = true;
        return true;
    }

    static VkShaderModule make_module(VkDevice dev, const std::vector<uint32_t>& code) {
        VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smci.codeSize = code.size() * 4;
        smci.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        DVK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &m));
        return m;
    }

    void Context::program_destroy(ProgramObj* p) {
        VkDevice dev = device;
        VkShaderModule vs = p->vs, fs = p->fs;
        VkDescriptorSetLayout sl = p->set_layout;
        VkPipelineLayout pl = p->layout;
        if (vs || fs || sl || pl)
            defer_destroy([dev, vs, fs, sl, pl]() {
                if (vs) vkDestroyShaderModule(dev, vs, nullptr);
                if (fs) vkDestroyShaderModule(dev, fs, nullptr);
                if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
                if (sl) vkDestroyDescriptorSetLayout(dev, sl, nullptr);
            });
        p->vs = p->fs = VK_NULL_HANDLE;
        p->set_layout = VK_NULL_HANDLE;
        p->layout = VK_NULL_HANDLE;
        p->linked = false;
    }

    // Builds modules, reflection and layouts from SPIR-V. Shared by link and glProgramBinary.
    bool program_from_spirv(Context& ctx, ProgramObj* p, const std::vector<uint32_t>& vs, const std::vector<uint32_t>& fs) {
        if (!reflect_spirv(vs, p->vs_refl) || !reflect_spirv(fs, p->fs_refl)) {
            p->info_log = "DivaVK: SPIR-V reflection failed";
            return false;
        }
        p->vs = make_module(ctx.device, vs);
        p->fs = make_module(ctx.device, fs);
        p->vs_spirv = vs;
        p->fs_spirv = fs;

        p->sampler_bindings = p->cube_bindings = p->ubo_bindings = p->ssbo_bindings = 0;
        p->sampler_names.clear();
        p->ubo_names.clear();
        p->uniform_locations.clear();
        for (uint32_t i = 0; i < MAX_TEXTURE_UNITS; i++)
            p->sampler_unit[i] = (uint8_t)i;
        for (uint32_t i = 0; i < MAX_UBO_BINDINGS; i++)
            p->ubo_point[i] = (uint8_t)i;

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        auto add = [&](const ReflectedBinding& b, VkShaderStageFlags stage) {
            if (b.set != 0) {
                log_once("set_nonzero", "Shader uses descriptor set %u; only set 0 is supported", b.set);
                return;
            }
            for (VkDescriptorSetLayoutBinding& e : bindings)
                if (e.binding == b.binding) {
                    e.stageFlags |= stage;
                    return;
                }
            VkDescriptorSetLayoutBinding e = {};
            e.binding = b.binding;
            e.descriptorCount = b.count;
            e.stageFlags = stage;
            switch (b.kind) {
            case BINDING_SAMPLER:
                if (b.binding >= MAX_TEXTURE_UNITS) {
                    log_once("sampler_range", "Sampler binding %u out of range", b.binding);
                    return;
                }
                e.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                p->sampler_bindings |= 1u << b.binding;
                if (b.cube)
                    p->cube_bindings |= 1u << b.binding;
                p->sampler_names.push_back({ b.name, b.binding });
                break;
            case BINDING_UBO:
                if (b.binding < UBO_BINDING_BASE || b.binding >= UBO_BINDING_BASE + MAX_UBO_BINDINGS) {
                    log_once("ubo_range", "UBO binding %u out of range", b.binding);
                    return;
                }
                e.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                p->ubo_bindings |= 1u << (b.binding - UBO_BINDING_BASE);
                p->ubo_names.push_back({ b.name, b.binding - UBO_BINDING_BASE });
                break;
            case BINDING_SSBO:
                if (b.binding < SSBO_BINDING_BASE || b.binding >= SSBO_BINDING_BASE + MAX_SSBO_BINDINGS) {
                    log_once("ssbo_range", "SSBO binding %u out of range", b.binding);
                    return;
                }
                e.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                p->ssbo_bindings |= 1u << (b.binding - SSBO_BINDING_BASE);
                break;
            }
            bindings.push_back(e);
        };
        for (const ReflectedBinding& b : p->vs_refl.bindings)
            add(b, VK_SHADER_STAGE_VERTEX_BIT);
        for (const ReflectedBinding& b : p->fs_refl.bindings)
            add(b, VK_SHADER_STAGE_FRAGMENT_BIT);

        p->vertex_inputs = p->vs_refl.inputs_mask;
        memcpy(p->vertex_input_base, p->vs_refl.input_base, sizeof(p->vertex_input_base));
        p->num_frag_outputs = 0;
        for (uint32_t i = 0; i < 32; i++)
            if (p->fs_refl.outputs_mask & (1u << i))
                p->num_frag_outputs = i + 1;

        VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dslci.bindingCount = (uint32_t)bindings.size();
        dslci.pBindings = bindings.data();
        DVK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &dslci, nullptr, &p->set_layout));
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &p->set_layout;
        DVK_CHECK(vkCreatePipelineLayout(ctx.device, &plci, nullptr, &p->layout));

        p->uid = ctx.next_program_uid++;
        p->linked = true;
        return true;
    }

    bool Context::link_program(ProgramObj* p) {
        program_destroy(p);
        p->info_log.clear();

        ShaderObj* vso = nullptr;
        ShaderObj* fso = nullptr;
        for (GLuint n : p->shaders) {
            ShaderObj* s = shaders.get(n);
            if (!s)
                continue;
            if (s->type == GL_VERTEX_SHADER)
                vso = s;
            else if (s->type == GL_FRAGMENT_SHADER)
                fso = s;
        }
        if (!vso || !fso || !vso->compiled || !fso->compiled) {
            p->info_log = "DivaVK: program needs a compiled vertex and fragment shader";
            return false;
        }

        std::string vsrc = prepare_source(vso->source, GL_VERTEX_SHADER);
        std::string fsrc = prepare_source(fso->source, GL_FRAGMENT_SHADER);
        const char* vstr[] = { vsrc.c_str() };
        const char* fstr[] = { fsrc.c_str() };

        DVK_STAGE("translating a shader program to SPIR-V (glslang)");
        stats.programs_linked++;
        struct StageReset { ~StageReset() { DVK_STAGE("running the game's GL calls"); } } stage_reset;
        glslang::TShader vsh(EShLangVertex), fsh(EShLangFragment);
        vsh.setStrings(vstr, 1);
        fsh.setStrings(fstr, 1);
        configure(vsh, EShLangVertex);
        configure(fsh, EShLangFragment);
        if (!vsh.parse(GetDefaultResources(), 430, ECoreProfile, false, false, MESSAGES)
            || !fsh.parse(GetDefaultResources(), 430, ECoreProfile, false, false, MESSAGES)) {
            p->info_log = std::string(vsh.getInfoLog()) + fsh.getInfoLog();
            return false;
        }

        glslang::TProgram prog;
        prog.addShader(&vsh);
        prog.addShader(&fsh);
        if (!prog.link(MESSAGES) || !prog.mapIO()) {
            p->info_log = prog.getInfoLog();
            log("Program link failed:\n%s", p->info_log.c_str());
            return false;
        }

        std::vector<uint32_t> vs, fs;
        glslang::SpvOptions opts;
        opts.generateDebugInfo = false;
        opts.disableOptimizer = true;
        glslang::GlslangToSpv(*prog.getIntermediate(EShLangVertex), vs, &opts);
        glslang::GlslangToSpv(*prog.getIntermediate(EShLangFragment), fs, &opts);
        return program_from_spirv(*this, p, vs, fs);
    }
}
