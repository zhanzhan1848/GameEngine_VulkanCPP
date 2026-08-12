#pragma once
#include "CommonHeaders.h"

namespace primal::graphics::shader_ir {

enum class ShaderIROpcode : u8 {
    NOP          = 0x00,
    CONST_FLOAT  = 0x01,
    CONST_FLOAT3 = 0x02,
    CONST_FLOAT4 = 0x03,
    LOAD_TIME    = 0x04,
    LOAD_UV      = 0x05,
    CONST_FLOAT2 = 0x06,

    ADD      = 0x10,
    MUL      = 0x11,
    LERP     = 0x12,
    CLAMP    = 0x13,
    POW      = 0x14,
    SATURATE = 0x15,
    DOT      = 0x16,
    SELECT   = 0x17,
    REMAP    = 0x18,

    SAMPLE         = 0x20,
    TEXTURE_HANDLE = 0x21,

    FRESNEL      = 0x30,
    NORMALIZE    = 0x31,
    NORMAL_BLEND = 0x32,

    SWIZZLE      = 0x40,
    CURVE_EVAL   = 0x50,
    STORE_OUTPUT = 0xFF,
};

struct ShaderIRInstruction {
    ShaderIROpcode opcode;
    u8  result_type;
    u16 dst;
    u16 src0;
    u16 src1;
    u16 src2;
    u16 flags;
};

static_assert(sizeof(ShaderIRInstruction) == 12, "ShaderIRInstruction must be 12 bytes");

} // namespace primal::graphics::shader_ir
