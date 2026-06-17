#pragma once
#include "CommonHeaders.h"
#include "Graphics/ShaderIR/ShaderIRFunction.h"

namespace primal::graphics::shader_ir {

class ShaderIRBuilder {
public:
    explicit ShaderIRBuilder(ShaderIRFunction& func) : func_(func) {}

    // Constants
    u16 ConstFloat(f32 value) {
        u16 reg = AllocReg();
        u16 cidx = func_.AddConstant(value);
        func_.Emit(ShaderIROpcode::CONST_FLOAT, 0, reg, cidx);
        return reg;
    }

    u16 ConstFloat2(f32 x, f32 y) {
        u16 reg = AllocReg();
        u16 cidx = func_.AddConstant2(x, y);
        func_.Emit(ShaderIROpcode::CONST_FLOAT2, 1, reg, cidx);
        return reg;
    }

    u16 ConstFloat3(f32 x, f32 y, f32 z) {
        u16 reg = AllocReg();
        u16 cidx = func_.AddConstant3(x, y, z);
        func_.Emit(ShaderIROpcode::CONST_FLOAT3, 2, reg, cidx);
        return reg;
    }

    u16 ConstFloat4(f32 x, f32 y, f32 z, f32 w) {
        u16 reg = AllocReg();
        u16 cidx = func_.AddConstant4(x, y, z, w);
        func_.Emit(ShaderIROpcode::CONST_FLOAT4, 3, reg, cidx);
        return reg;
    }

    // Inputs
    u16 LoadTime() {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::LOAD_TIME, 0, reg);
        return reg;
    }

    u16 LoadUV() {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::LOAD_UV, 1, reg);
        return reg;
    }

    // Arithmetic
    u16 Add(u8 type, u16 a, u16 b) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::ADD, type, reg, a, b);
        return reg;
    }

    u16 Mul(u8 type, u16 a, u16 b) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::MUL, type, reg, a, b);
        return reg;
    }

    u16 Lerp(u8 type, u16 a, u16 b, u16 alpha) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::LERP, type, reg, a, b, alpha);
        return reg;
    }

    u16 Clamp(u8 type, u16 val, u16 min_val, u16 max_val) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::CLAMP, type, reg, val, min_val, max_val);
        return reg;
    }

    u16 Pow(u16 base, u16 exp) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::POW, 0, reg, base, exp);
        return reg;
    }

    u16 Saturate(u16 val) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::SATURATE, 0, reg, val);
        return reg;
    }

    u16 Dot(u8 type, u16 a, u16 b) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::DOT, type, reg, a, b);
        return reg;
    }

    u16 Select(u8 type, u16 cond, u16 true_val, u16 false_val) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::SELECT, type, reg, cond, true_val, false_val);
        return reg;
    }

    u16 Remap(u16 val, f32 in_min, f32 in_max, f32 out_min, f32 out_max) {
        u16 reg = AllocReg();
        u16 cidx = func_.AddConstant4(in_min, in_max, out_min, out_max);
        func_.Emit(ShaderIROpcode::REMAP, 0, reg, val, 0, 0, cidx);
        return reg;
    }

    // Texture
    u16 TextureHandle(u16 binding_slot) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::TEXTURE_HANDLE, 4, reg, 0, 0, 0, binding_slot);
        return reg;
    }

    u16 Sample(u16 tex_reg, u16 uv_reg) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::SAMPLE, 3, reg, tex_reg, uv_reg);
        return reg;
    }

    // Utility
    u16 Fresnel(u16 n_reg, u16 v_reg, f32 power) {
        u16 reg = AllocReg();
        u16 power_fixed = (u16)(power * 10.0f + 0.5f);
        func_.Emit(ShaderIROpcode::FRESNEL, 0, reg, n_reg, v_reg, 0, power_fixed);
        return reg;
    }

    u16 Normalize(u8 type, u16 val) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::NORMALIZE, type, reg, val);
        return reg;
    }

    u16 NormalBlend(u16 base, u16 detail) {
        u16 reg = AllocReg();
        func_.Emit(ShaderIROpcode::NORMAL_BLEND, 2, reg, base, detail);
        return reg;
    }

    // Control
    u16 CurveEval(u16 x_reg, const f32* points, u32 point_count) {
        u16 reg = AllocReg();
        u16 cidx = func_.AddConstant(point_count);
        for (u32 i = 0; i < point_count; i++) {
            func_.AddConstant(points[i * 2]);
            func_.AddConstant(points[i * 2 + 1]);
        }
        func_.Emit(ShaderIROpcode::CURVE_EVAL, 0, reg, x_reg, 0, 0, cidx);
        return reg;
    }

    u16 Swizzle(u16 src, u8 x, u8 y, u8 z, u8 w) {
        u16 reg = AllocReg();
        u16 swizzle_flags = (u16)((x) | (y << 2) | (z << 4) | (w << 6));
        func_.Emit(ShaderIROpcode::SWIZZLE, 3, reg, src, 0, 0, swizzle_flags);
        return reg;
    }

    void StoreOutput(u16 src, u8 pin_index) {
        func_.Emit(ShaderIROpcode::STORE_OUTPUT, 0, 0, src, 0, 0, (u16)pin_index);
    }

private:
    ShaderIRFunction& func_;
    u16 AllocReg() { return func_.AllocRegister(); }
};

} // namespace primal::graphics::shader_ir
