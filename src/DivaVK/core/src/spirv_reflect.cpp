/*
    DivaVK - minimal SPIR-V reflection.
*/

#include "spirv_reflect.hpp"

namespace dvk {
    namespace {
        enum : uint32_t {
            OpName = 5,
            OpEntryPoint = 15,
            OpTypeInt = 21,
            OpTypeFloat = 22,
            OpTypeVector = 23,
            OpTypeMatrix = 24,
            OpTypeImage = 25,
            OpTypeSampler = 26,
            OpTypeSampledImage = 27,
            OpTypeArray = 28,
            OpTypeRuntimeArray = 29,
            OpTypeStruct = 30,
            OpTypePointer = 32,
            OpConstant = 43,
            OpVariable = 59,
            OpDecorate = 71,

            DecorationBlock = 2,
            DecorationBufferBlock = 3,
            DecorationBuiltIn = 11,
            DecorationLocation = 30,
            DecorationBinding = 33,
            DecorationDescriptorSet = 34,

            StorageUniformConstant = 0,
            StorageInput = 1,
            StorageUniform = 2,
            StorageOutput = 3,
            StorageStorageBuffer = 12,

            DimCube = 3,
        };

        struct Id {
            uint32_t op = 0;
            uint32_t type = 0;          // result type (variables, constants) or element type
            uint32_t storage = 0;       // pointers / variables
            uint32_t width = 0;         // ints/floats
            uint32_t signedness = 0;
            uint32_t dim = 0;           // images
            uint32_t depth = 0;
            uint32_t length_id = 0;     // arrays
            uint32_t constant = 0;      // constants (low word)
            bool block = false;
            bool buffer_block = false;
            bool builtin = false;
            int32_t location = -1;
            int32_t binding = -1;
            int32_t set = -1;
            std::string name;
        };
    }

    static std::string read_string(const uint32_t* w, size_t n) {
        const char* s = (const char*)w;
        size_t max = n * 4;
        size_t len = 0;
        while (len < max && s[len])
            len++;
        return std::string(s, len);
    }

    bool reflect_spirv(const std::vector<uint32_t>& spirv, ShaderReflection& out) {
        out = ShaderReflection();
        if (spirv.size() < 5 || spirv[0] != 0x07230203)
            return false;

        const uint32_t bound = spirv[3];
        std::vector<Id> ids(bound);
        uint32_t exec_model = ~0u;

        size_t i = 5;
        while (i < spirv.size()) {
            const uint32_t word = spirv[i];
            const uint32_t op = word & 0xFFFF;
            const uint32_t count = word >> 16;
            if (!count || i + count > spirv.size())
                return false;
            const uint32_t* a = &spirv[i + 1];

            switch (op) {
            case OpEntryPoint:
                if (exec_model == ~0u)
                    exec_model = a[0];
                break;
            case OpName:
                if (a[0] < bound)
                    ids[a[0]].name = read_string(a + 1, count - 2);
                break;
            case OpTypeInt:
                ids[a[0]].op = op;
                ids[a[0]].width = a[1];
                ids[a[0]].signedness = a[2];
                break;
            case OpTypeFloat:
                ids[a[0]].op = op;
                ids[a[0]].width = a[1];
                break;
            case OpTypeVector:
            case OpTypeMatrix:
                ids[a[0]].op = op;
                ids[a[0]].type = a[1];
                break;
            case OpTypeImage:
                ids[a[0]].op = op;
                ids[a[0]].type = a[1];
                ids[a[0]].dim = a[2];
                ids[a[0]].depth = a[3];
                break;
            case OpTypeSampler:
                ids[a[0]].op = op;
                break;
            case OpTypeSampledImage:
                ids[a[0]].op = op;
                ids[a[0]].type = a[1];
                break;
            case OpTypeArray:
                ids[a[0]].op = op;
                ids[a[0]].type = a[1];
                ids[a[0]].length_id = a[2];
                break;
            case OpTypeRuntimeArray:
                ids[a[0]].op = op;
                ids[a[0]].type = a[1];
                break;
            case OpTypeStruct:
                ids[a[0]].op = op;
                break;
            case OpTypePointer:
                ids[a[0]].op = op;
                ids[a[0]].storage = a[1];
                ids[a[0]].type = a[2];
                break;
            case OpConstant:
                ids[a[1]].op = op;
                ids[a[1]].type = a[0];
                ids[a[1]].constant = a[2];
                break;
            case OpVariable:
                ids[a[1]].op = op;
                ids[a[1]].type = a[0];
                ids[a[1]].storage = a[2];
                break;
            case OpDecorate: {
                Id& t = ids[a[0]];
                switch (a[1]) {
                case DecorationBlock: t.block = true; break;
                case DecorationBufferBlock: t.buffer_block = true; break;
                case DecorationBuiltIn: t.builtin = true; break;
                case DecorationLocation: t.location = (int32_t)a[2]; break;
                case DecorationBinding: t.binding = (int32_t)a[2]; break;
                case DecorationDescriptorSet: t.set = (int32_t)a[2]; break;
                }
            } break;
            }
            i += count;
        }

        // Strip pointer/array wrappers, tracking array length
        auto unwrap = [&](uint32_t type, uint32_t& array_len) {
            array_len = 1;
            while (type < bound) {
                const Id& t = ids[type];
                if (t.op == OpTypeArray) {
                    uint32_t len = t.length_id < bound ? ids[t.length_id].constant : 1;
                    array_len *= len ? len : 1;
                    type = t.type;
                }
                else if (t.op == OpTypeRuntimeArray)
                    type = t.type;
                else
                    break;
            }
            return type;
        };

        auto scalar_base = [&](uint32_t type) -> uint8_t {
            while (type < bound && (ids[type].op == OpTypeVector || ids[type].op == OpTypeMatrix))
                type = ids[type].type;
            if (type < bound && ids[type].op == OpTypeInt)
                return ids[type].signedness ? 1 : 2;
            return 0;
        };

        for (uint32_t id = 0; id < bound; id++) {
            const Id& v = ids[id];
            if (v.op != OpVariable || v.type >= bound)
                continue;
            const Id& ptr = ids[v.type];
            if (ptr.op != OpTypePointer)
                continue;

            uint32_t array_len = 1;
            uint32_t inner = unwrap(ptr.type, array_len);
            if (inner >= bound)
                continue;
            const Id& t = ids[inner];

            switch (v.storage) {
            case StorageUniformConstant: {
                uint32_t img = inner;
                if (t.op == OpTypeSampledImage)
                    img = t.type;
                else if (t.op != OpTypeImage)
                    break; // separate samplers aren't used by DivaGL shaders
                ReflectedBinding b;
                b.set = v.set < 0 ? 0 : (uint32_t)v.set;
                b.binding = v.binding < 0 ? 0 : (uint32_t)v.binding;
                b.kind = BINDING_SAMPLER;
                b.cube = img < bound && ids[img].dim == DimCube;
                b.shadow = img < bound && ids[img].depth == 1;
                b.count = array_len;
                b.name = v.name;
                out.bindings.push_back(b);
            } break;
            case StorageUniform:
            case StorageStorageBuffer: {
                if (t.op != OpTypeStruct)
                    break;
                ReflectedBinding b;
                b.set = v.set < 0 ? 0 : (uint32_t)v.set;
                b.binding = v.binding < 0 ? 0 : (uint32_t)v.binding;
                b.kind = (v.storage == StorageStorageBuffer || t.buffer_block) ? BINDING_SSBO : BINDING_UBO;
                b.count = array_len;
                b.name = t.name;
                b.inst_name = v.name;
                out.bindings.push_back(b);
            } break;
            case StorageInput:
                if (!v.builtin && v.location >= 0 && v.location < 16 && t.op != OpTypeStruct) {
                    // Matrices and arrays occupy consecutive locations
                    uint32_t slots = array_len;
                    if (t.op == OpTypeMatrix)
                        slots *= 4;
                    for (uint32_t s = 0; s < slots && v.location + s < 16; s++) {
                        out.inputs_mask |= 1u << (v.location + s);
                        out.input_base[v.location + s] = scalar_base(inner);
                    }
                }
                break;
            case StorageOutput:
                if (!v.builtin && v.location >= 0 && v.location < 32)
                    out.outputs_mask |= 1u << v.location;
                break;
            }
        }

        (void)exec_model;
        return true;
    }
}
