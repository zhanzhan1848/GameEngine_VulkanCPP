#pragma once
#include "CommonHeaders.h"
#include "Graphics/ShaderIR/ShaderIROpcode.h"

namespace primal::graphics::shader_ir {

class ShaderIRFunction {
public:
    u16 AllocRegister() { return register_count_++; }

    u16 AddConstant(f32 value) {
        u16 idx = (u16)constant_pool_.size();
        constant_pool_.push_back(value);
        return idx;
    }

    u16 AddConstant2(f32 x, f32 y) {
        u16 idx = (u16)constant_pool_.size();
        constant_pool_.push_back(x);
        constant_pool_.push_back(y);
        return idx;
    }

    u16 AddConstant3(f32 x, f32 y, f32 z) {
        u16 idx = (u16)constant_pool_.size();
        constant_pool_.push_back(x);
        constant_pool_.push_back(y);
        constant_pool_.push_back(z);
        return idx;
    }

    u16 AddConstant4(f32 x, f32 y, f32 z, f32 w) {
        u16 idx = (u16)constant_pool_.size();
        constant_pool_.push_back(x);
        constant_pool_.push_back(y);
        constant_pool_.push_back(z);
        constant_pool_.push_back(w);
        return idx;
    }

    void Emit(ShaderIROpcode op, u8 result_type, u16 dst,
              u16 src0 = 0, u16 src1 = 0, u16 src2 = 0, u16 flags = 0) {
        instructions_.push_back({op, result_type, dst, src0, src1, src2, flags});
    }

    const utl::vector<ShaderIRInstruction>& Instructions() const { return instructions_; }
    const utl::vector<f32>& ConstantPool() const { return constant_pool_; }
    u16 RegisterCount() const { return register_count_; }

    void Clear() {
        instructions_.clear();
        constant_pool_.clear();
        register_count_ = 0;
    }

private:
    utl::vector<ShaderIRInstruction> instructions_;
    utl::vector<f32> constant_pool_;
    u16 register_count_{0};
};

} // namespace primal::graphics::shader_ir
