/*
    DivaVK - minimal SPIR-V reflection (descriptor bindings, vertex inputs, fragment outputs).
*/

#pragma once

#include "common.hpp"

namespace dvk {
    enum BindingKind : uint32_t {
        BINDING_SAMPLER = 0,
        BINDING_UBO,
        BINDING_SSBO,
    };

    struct ReflectedBinding {
        uint32_t set = 0;
        uint32_t binding = 0;
        BindingKind kind = BINDING_SAMPLER;
        bool cube = false;
        bool shadow = false;
        uint32_t count = 1;
        std::string name;       // variable name (samplers) or block name (buffers)
        std::string inst_name;  // instance name for blocks
    };

    struct ShaderReflection {
        std::vector<ReflectedBinding> bindings;
        uint32_t inputs_mask = 0;
        uint8_t input_base[16] = {};   // 0 float, 1 int, 2 uint
        uint32_t outputs_mask = 0;
    };

    bool reflect_spirv(const std::vector<uint32_t>& spirv, ShaderReflection& out);
}
