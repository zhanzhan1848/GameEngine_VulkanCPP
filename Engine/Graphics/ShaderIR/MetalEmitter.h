#pragma once

#include "CommonHeaders.h"
#include "Graphics/ShaderIR/ShaderIRFunction.h"
#include <sstream>
#include <string>
#include <unordered_map>

namespace primal::graphics::shader_ir {

struct EmitterOutput {
    std::string source;
    std::string log;
    bool success{false};
};

class MetalEmitter {
public:
    EmitterOutput Emit(const ShaderIRFunction& ir, bool preview_mode = false) {
        EmitterOutput output;
        const auto& instructions = ir.Instructions();
        const auto& constants = ir.ConstantPool();
        u16 reg_count = ir.RegisterCount();

        // --- Pass 1: Collect metadata ---
        utl::vector<u8> reg_types(reg_count, 0);
        std::unordered_map<u16, std::string> tex_param_names;
        u16 max_tex_slot = 0;
        bool has_output[8] = {};
        u16 output_src[8] = {};
        bool wrote_albedo = false;
        bool wrote_orm_component = false;

        for (auto& inst : instructions) {
            if (inst.opcode == ShaderIROpcode::NOP) continue;
            if (inst.opcode == ShaderIROpcode::STORE_OUTPUT) {
                u8 pin = (u8)inst.flags;
                if (pin < 8) {
                    has_output[pin] = true;
                    output_src[pin] = inst.src0;
                }
                continue;
            }
            if (inst.opcode == ShaderIROpcode::TEXTURE_HANDLE) {
                reg_types[inst.dst] = inst.result_type;
                tex_param_names[inst.dst] = "tex" + std::to_string(inst.flags);
                if (inst.flags > max_tex_slot) max_tex_slot = inst.flags;
                continue;
            }
            if (inst.dst < reg_count) {
                // Force scalar result for opcodes that always return float
                u8 actual_type = inst.result_type;
                if (inst.opcode == ShaderIROpcode::DOT ||
                    inst.opcode == ShaderIROpcode::SATURATE ||
                    inst.opcode == ShaderIROpcode::POW ||
                    inst.opcode == ShaderIROpcode::FRESNEL) {
                    actual_type = 0; // float
                }
                reg_types[inst.dst] = actual_type;
            }
        }

        bool has_textures = !tex_param_names.empty();

        // --- Pass 2: Preamble + function signature ---
        std::ostringstream ss;
        ss << "#include <metal_stdlib>\n";
        ss << "using namespace metal;\n\n";

        // VertexOut matching ForwardPBR vertex shader output
        ss << "struct VertexOut {\n";
        ss << "    float4 position [[position]];\n";
        ss << "    float3 worldPos;\n";
        ss << "    float3 worldNormal;\n";
        ss << "    float2 uv;\n";
        ss << "    float4 shadowPos0;\n";
        ss << "    float4 shadowPos1;\n";
        ss << "};\n\n";

        if (preview_mode) {
            ss << "struct SceneData {\n";
            ss << "    float time;\n";
            ss << "};\n\n";
        } else {
            ss << "struct FragmentOut {\n";
            ss << "    float4 albedo [[color(0)]];\n";
            ss << "    float4 normal [[color(1)]];\n";
            ss << "    float4 orm [[color(2)]];\n";
            ss << "    float2 velocity [[color(3)]];\n";
            ss << "};\n\n";

            ss << "struct SceneData {\n";
            ss << "    float time;\n";
            ss << "};\n\n";
        }

        // Function signature
        if (preview_mode) {
            ss << "fragment float4 fragmentMain(\n";
        } else {
            ss << "fragment FragmentOut fragmentMain(\n";
        }
        ss << "    VertexOut in [[stage_in]],\n";
        ss << "    constant SceneData& sceneData [[buffer(1)]]";
        if (has_textures) {
            for (u16 slot = 0; slot <= max_tex_slot; slot++) {
                ss << ",\n    texture2d<float> tex" << slot
                   << " [[texture(" << slot << ")]]";
            }
            ss << ",\n    sampler defaultSampler [[sampler(0)]]";
        }
        ss << "\n) {\n";
        if (preview_mode) {
            // No output struct needed — direct return
        } else {
            ss << "    FragmentOut out;\n";
        }

        // --- Register declarations ---
        for (u16 i = 0; i < reg_count; i++) {
            if (reg_types[i] == 4) continue; // texture handle
            ss << "    " << TypeToMetal(reg_types[i])
               << " r" << i << " = " << DefaultValue(reg_types[i]) << ";\n";
        }

        // --- Pass 3: Emit instructions ---
        for (auto& inst : instructions) {
            EmitInstruction(ss, inst, constants, reg_types, tex_param_names);
        }

        // --- Pass 4: Output assignments ---
        if (preview_mode) {
            // Preview mode: output single float4 color (pin 0 = BaseColor)
            if (has_output[0]) {
                ss << "    return " << RegRef(output_src[0], tex_param_names) << ";\n";
            } else {
                ss << "    return float4(1.0f, 1.0f, 1.0f, 1.0f);\n";
            }
        } else {
            for (u32 pin = 0; pin < 8; pin++) {
                if (!has_output[pin]) continue;
                std::string reg_ref = RegRef(output_src[pin], tex_param_names);
                switch (pin) {
                    case 0: // BaseColor
                        ss << "    out.albedo = " << reg_ref << ";\n";
                        wrote_albedo = true;
                        break;
                    case 1: // Roughness
                        ss << "    out.orm.y = " << reg_ref << ";\n";
                        wrote_orm_component = true;
                        break;
                    case 2: // Metallic
                        ss << "    out.orm.z = " << reg_ref << ";\n";
                        wrote_orm_component = true;
                        break;
                    case 3: // AlphaCutoff - future use
                        break;
                    case 4: case 5: case 6: // Texture pins - handled by SAMPLE
                        break;
                    case 7: // Technique - future use
                        break;
                }
            }

            // Default values for unwritten outputs
            if (!wrote_albedo) {
                ss << "    out.albedo = float4(1.0f, 1.0f, 1.0f, 1.0f);\n";
            }
            if (!wrote_orm_component && !has_output[0]) {
                // Only set default ORM if nothing was written at all
                ss << "    out.orm = float4(1.0f, 0.5f, 0.0f, 1.0f);\n";
            } else if (!wrote_orm_component) {
                ss << "    out.orm = float4(1.0f, 0.5f, 0.0f, 1.0f);\n";
            } else {
                // Fill in unset ORM components
                if (!has_output[1]) ss << "    out.orm.x = 1.0f;\n"; // AO default
                if (!has_output[2]) ss << "    out.orm.a = 1.0f;\n"; // Alpha
            }
            ss << "    out.velocity = float2(0.0f);\n";

            ss << "    return out;\n";
        }
        ss << "}\n";

        output.source = ss.str();
        output.success = true;
        return output;
    }

    const char* BackendName() const { return "Metal"; }

private:
    static const char* TypeToMetal(u8 type) {
        switch (type) {
            case 0: return "float";
            case 1: return "float2";
            case 2: return "float3";
            case 3: return "float4";
            default: return "float";
        }
    }

    static const char* DefaultValue(u8 type) {
        switch (type) {
            case 0: return "0.0f";
            case 1: return "float2(0.0f)";
            case 2: return "float3(0.0f)";
            case 3: return "float4(0.0f)";
            default: return "0.0f";
        }
    }

    static std::string FormatFloat(f32 v) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.7gf", v);
        std::string s(buf);
        // %g may omit decimal point for integers (e.g., "5f" instead of "5.0f")
        size_t f_pos = s.rfind('f');
        if (f_pos != std::string::npos && s.find('.') == std::string::npos) {
            s.insert(f_pos, ".0");
        }
        return s;
    }

    static std::string RegRef(u16 reg, const std::unordered_map<u16, std::string>& tex_names) {
        auto it = tex_names.find(reg);
        if (it != tex_names.end()) return it->second;
        return "r" + std::to_string(reg);
    }

    void EmitInstruction(std::ostringstream& ss,
                         const ShaderIRInstruction& inst,
                         const utl::vector<f32>& constants,
                         const utl::vector<u8>& reg_types,
                         const std::unordered_map<u16, std::string>& tex_names) {
        switch (inst.opcode) {
            case ShaderIROpcode::NOP:
            case ShaderIROpcode::STORE_OUTPUT:
                break;

            case ShaderIROpcode::CONST_FLOAT:
                ss << "    r" << inst.dst << " = "
                   << FormatFloat(constants[inst.src0]) << ";\n";
                break;

            case ShaderIROpcode::CONST_FLOAT2:
                ss << "    r" << inst.dst << " = float2("
                   << FormatFloat(constants[inst.src0]) << ", "
                   << FormatFloat(constants[inst.src0 + 1]) << ");\n";
                break;

            case ShaderIROpcode::CONST_FLOAT3:
                ss << "    r" << inst.dst << " = float3("
                   << FormatFloat(constants[inst.src0]) << ", "
                   << FormatFloat(constants[inst.src0 + 1]) << ", "
                   << FormatFloat(constants[inst.src0 + 2]) << ");\n";
                break;

            case ShaderIROpcode::CONST_FLOAT4:
                ss << "    r" << inst.dst << " = float4("
                   << FormatFloat(constants[inst.src0]) << ", "
                   << FormatFloat(constants[inst.src0 + 1]) << ", "
                   << FormatFloat(constants[inst.src0 + 2]) << ", "
                   << FormatFloat(constants[inst.src0 + 3]) << ");\n";
                break;

            case ShaderIROpcode::LOAD_TIME:
                ss << "    r" << inst.dst << " = sceneData.time;\n";
                break;

            case ShaderIROpcode::LOAD_UV:
                ss << "    r" << inst.dst << " = in.uv;\n";
                break;

            case ShaderIROpcode::ADD:
                ss << "    r" << inst.dst
                   << " = r" << inst.src0 << " + r" << inst.src1 << ";\n";
                break;

            case ShaderIROpcode::MUL:
                ss << "    r" << inst.dst
                   << " = r" << inst.src0 << " * r" << inst.src1 << ";\n";
                break;

            case ShaderIROpcode::LERP:
                ss << "    r" << inst.dst
                   << " = mix(r" << inst.src0 << ", r" << inst.src1
                   << ", r" << inst.src2 << ");\n";
                break;

            case ShaderIROpcode::CLAMP:
                ss << "    r" << inst.dst
                   << " = clamp(r" << inst.src0 << ", r" << inst.src1
                   << ", r" << inst.src2 << ");\n";
                break;

            case ShaderIROpcode::POW:
                ss << "    r" << inst.dst << " = pow(r"
                   << inst.src0 << ", r" << inst.src1 << ");\n";
                break;

            case ShaderIROpcode::SATURATE:
                ss << "    r" << inst.dst << " = clamp(r"
                   << inst.src0 << ", 0.0f, 1.0f);\n";
                break;

            case ShaderIROpcode::DOT:
                ss << "    r" << inst.dst << " = dot(r"
                   << inst.src0 << ", r" << inst.src1 << ");\n";
                break;

            case ShaderIROpcode::SELECT:
                ss << "    r" << inst.dst
                   << " = (r" << inst.src0 << " != 0.0f) ? r"
                   << inst.src1 << " : r" << inst.src2 << ";\n";
                break;

            case ShaderIROpcode::REMAP: {
                f32 in_min = constants[inst.flags];
                f32 in_max = constants[inst.flags + 1];
                f32 out_min = constants[inst.flags + 2];
                f32 out_max = constants[inst.flags + 3];
                ss << "    r" << inst.dst << " = " << FormatFloat(out_min)
                   << " + (r" << inst.src0 << " - " << FormatFloat(in_min)
                   << ") / (" << FormatFloat(in_max) << " - " << FormatFloat(in_min)
                   << ") * (" << FormatFloat(out_max) << " - " << FormatFloat(out_min)
                   << ");\n";
                break;
            }

            case ShaderIROpcode::TEXTURE_HANDLE:
                // No local variable — tracked in tex_param_names
                break;

            case ShaderIROpcode::SAMPLE: {
                std::string tex_name = RegRef(inst.src0, tex_names);
                ss << "    r" << inst.dst << " = " << tex_name
                   << ".sample(defaultSampler, r" << inst.src1 << ");\n";
                break;
            }

            case ShaderIROpcode::FRESNEL: {
                f32 power = inst.flags / 10.0f;
                ss << "    r" << inst.dst << " = pow(1.0f - clamp(dot(r"
                   << inst.src0 << ", r" << inst.src1
                   << "), 0.0f, 1.0f), " << FormatFloat(power) << ");\n";
                break;
            }

            case ShaderIROpcode::NORMALIZE:
                ss << "    r" << inst.dst
                   << " = normalize(r" << inst.src0 << ");\n";
                break;

            case ShaderIROpcode::NORMAL_BLEND:
                ss << "    r" << inst.dst << " = normalize(float3(r"
                   << inst.src0 << ".xy + r" << inst.src1 << ".xy, r"
                   << inst.src0 << ".z * r" << inst.src1 << ".z));\n";
                break;

            case ShaderIROpcode::CURVE_EVAL: {
                u32 point_count = (u32)constants[inst.flags];
                ss << "    {\n";
                ss << "        float _x = r" << inst.src0 << ";\n";
                ss << "        float _result = " << FormatFloat(constants[inst.flags + 1 + (point_count - 1) * 2 + 1]) << ";\n";
                for (u32 i = 0; i < point_count - 1; i++) {
                    f32 t0 = constants[inst.flags + 1 + i * 2];
                    f32 v0 = constants[inst.flags + 1 + i * 2 + 1];
                    f32 t1 = constants[inst.flags + 1 + (i + 1) * 2];
                    f32 v1 = constants[inst.flags + 1 + (i + 1) * 2 + 1];
                    ss << "        if (_x >= " << FormatFloat(t0) << " && _x <= " << FormatFloat(t1) << ") {\n";
                    ss << "            float _t = (_x - " << FormatFloat(t0) << ") / (" << FormatFloat(t1) << " - " << FormatFloat(t0) << ");\n";
                    ss << "            _result = " << FormatFloat(v0) << " + _t * (" << FormatFloat(v1) << " - " << FormatFloat(v0) << ");\n";
                    ss << "        }\n";
                }
                ss << "        r" << inst.dst << " = _result;\n";
                ss << "    }\n";
                break;
            }

            case ShaderIROpcode::SWIZZLE: {
                const char comps[] = {'x', 'y', 'z', 'w'};
                u8 cx = inst.flags & 0x3;
                u8 cy = (inst.flags >> 2) & 0x3;
                u8 cz = (inst.flags >> 4) & 0x3;
                u8 cw = (inst.flags >> 6) & 0x3;
                ss << "    r" << inst.dst
                   << " = r" << inst.src0
                   << "." << comps[cx] << comps[cy] << comps[cz] << comps[cw] << ";\n";
                break;
            }
        }
    }
};

} // namespace primal::graphics::shader_ir
