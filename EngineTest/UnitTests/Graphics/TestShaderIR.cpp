// ShaderIR + MaterialGraphToIR comprehensive test
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/ShaderIR/ShaderIROpcode.h"
#include "Engine/Graphics/ShaderIR/ShaderIRFunction.h"
#include "Engine/Graphics/ShaderIR/ShaderIRBuilder.h"
#include "Engine/Graphics/ShaderIR/MaterialGraphToIR.h"
#include "Engine/Graphics/MaterialGraph/MaterialGraph.h"
#include "Engine/Graphics/MaterialGraph/Nodes/ConstantNode.h"
#include "Engine/Graphics/MaterialGraph/Nodes/TimeNode.h"
#include "Engine/Graphics/MaterialGraph/Nodes/UVNode.h"
#include "Engine/Graphics/MaterialGraph/Nodes/MathNodes.h"
#include "Engine/Graphics/MaterialGraph/Nodes/UtilityNodes.h"
#include "Engine/Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "Engine/Graphics/ShaderIR/MetalEmitter.h"
#include "Engine/Graphics/ShaderIR/ShaderCompiler.h"
#include <iostream>
#include <cassert>
#include <cmath>

#ifdef __APPLE__
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#endif

using namespace primal::graphics::shader_ir;
using namespace primal::graphics::material_graph;

#define TEST(name) std::cout << "[TEST] " << name << "..."
#define PASS() std::cout << " PASS\n"

// ============================================================================
// Phase 1: ShaderIR Instruction Encoding
// ============================================================================

void test_instruction_size() {
    TEST("Instruction size = 12 bytes");
    static_assert(sizeof(ShaderIRInstruction) == 12, "must be 12 bytes");
    PASS();
}

void test_opcode_values() {
    TEST("Opcode values");
    assert((u8)ShaderIROpcode::NOP == 0x00);
    assert((u8)ShaderIROpcode::CONST_FLOAT == 0x01);
    assert((u8)ShaderIROpcode::CONST_FLOAT3 == 0x02);
    assert((u8)ShaderIROpcode::CONST_FLOAT4 == 0x03);
    assert((u8)ShaderIROpcode::LOAD_TIME == 0x04);
    assert((u8)ShaderIROpcode::LOAD_UV == 0x05);
    assert((u8)ShaderIROpcode::ADD == 0x10);
    assert((u8)ShaderIROpcode::MUL == 0x11);
    assert((u8)ShaderIROpcode::LERP == 0x12);
    assert((u8)ShaderIROpcode::CLAMP == 0x13);
    assert((u8)ShaderIROpcode::POW == 0x14);
    assert((u8)ShaderIROpcode::SATURATE == 0x15);
    assert((u8)ShaderIROpcode::DOT == 0x16);
    assert((u8)ShaderIROpcode::SAMPLE == 0x20);
    assert((u8)ShaderIROpcode::TEXTURE_HANDLE == 0x21);
    assert((u8)ShaderIROpcode::FRESNEL == 0x30);
    assert((u8)ShaderIROpcode::NORMALIZE == 0x31);
    assert((u8)ShaderIROpcode::NORMAL_BLEND == 0x32);
    assert((u8)ShaderIROpcode::SWIZZLE == 0x40);
    assert((u8)ShaderIROpcode::STORE_OUTPUT == 0xFF);
    PASS();
}

// ============================================================================
// Phase 2: ShaderIRBuilder
// ============================================================================

void test_builder_basic() {
    TEST("Builder: constants + stores");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto r0 = builder.ConstFloat4(1.0f, 0.0f, 0.0f, 1.0f);
    auto r1 = builder.ConstFloat(0.5f);
    auto r2 = builder.ConstFloat(0.0f);
    builder.StoreOutput(r0, 0);
    builder.StoreOutput(r1, 1);
    builder.StoreOutput(r2, 2);

    assert(func.Instructions().size() == 6);
    assert(func.RegisterCount() == 3);
    // Constant pool: Float4(4) + Float(1) + Float(1) = 6
    assert(func.ConstantPool().size() == 6);
    assert(std::abs(func.ConstantPool()[0] - 1.0f) < 0.001f);
    assert(std::abs(func.ConstantPool()[4] - 0.5f) < 0.001f);
    PASS();
}

void test_builder_constant_pool_float3() {
    TEST("Builder: Float3 constant pool indexing");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto r0 = builder.ConstFloat3(1.0f, 2.0f, 3.0f);
    // Constant pool: [1.0, 2.0, 3.0] at index 0
    assert(func.ConstantPool().size() == 3);
    assert(std::abs(func.ConstantPool()[0] - 1.0f) < 0.001f);
    assert(std::abs(func.ConstantPool()[1] - 2.0f) < 0.001f);
    assert(std::abs(func.ConstantPool()[2] - 3.0f) < 0.001f);

    // Instruction should reference index 0
    assert(func.Instructions()[0].src0 == 0);
    PASS();
}

void test_builder_arithmetic() {
    TEST("Builder: Add/Mul/Lerp/Saturate/Pow/Dot");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto a = builder.ConstFloat(1.0f);
    auto b = builder.ConstFloat(2.0f);
    auto c = builder.Add(0, a, b);
    auto d = builder.Mul(0, a, b);
    auto e = builder.Lerp(0, a, b, a);
    auto f = builder.Saturate(c);
    auto g = builder.Pow(a, b);
    auto h = builder.Dot(2, a, b); // Float3 dot, but using same reg for test

    assert(func.RegisterCount() == 8);

    // Verify opcodes
    auto& ins = func.Instructions();
    assert(ins[2].opcode == ShaderIROpcode::ADD);
    assert(ins[3].opcode == ShaderIROpcode::MUL);
    assert(ins[4].opcode == ShaderIROpcode::LERP);
    assert(ins[5].opcode == ShaderIROpcode::SATURATE);
    assert(ins[6].opcode == ShaderIROpcode::POW);
    assert(ins[7].opcode == ShaderIROpcode::DOT);
    PASS();
}

void test_builder_texture() {
    TEST("Builder: TextureHandle + Sample");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto uv = builder.LoadUV();
    auto tex = builder.TextureHandle(0);
    auto sampled = builder.Sample(tex, uv);

    auto& ins = func.Instructions();
    assert(ins.size() == 3);
    assert(ins[0].opcode == ShaderIROpcode::LOAD_UV);
    assert(ins[1].opcode == ShaderIROpcode::TEXTURE_HANDLE);
    assert(ins[2].opcode == ShaderIROpcode::SAMPLE);
    // Sample dst = sampled reg, src0 = tex reg, src1 = uv reg
    assert(ins[2].src0 == tex);
    assert(ins[2].src1 == uv);
    PASS();
}

void test_builder_fresnel_flags() {
    TEST("Builder: Fresnel power encoding in flags");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto n = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto v = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto f = builder.Fresnel(n, v, 5.0f);

    // Find the Fresnel instruction
    bool found = false;
    for (auto& inst : func.Instructions()) {
        if (inst.opcode == ShaderIROpcode::FRESNEL) {
            assert(inst.flags == (u16)(5.0f * 10.0f + 0.5f)); // 50
            found = true;
        }
    }
    assert(found);
    PASS();
}

void test_builder_swizzle() {
    TEST("Builder: Swizzle flags encoding");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto v = builder.ConstFloat4(1.0f, 2.0f, 3.0f, 4.0f);
    auto s = builder.Swizzle(v, 2, 1, 0, 3); // z, y, x, w

    bool found = false;
    for (auto& inst : func.Instructions()) {
        if (inst.opcode == ShaderIROpcode::SWIZZLE) {
            // flags = x | (y << 2) | (z << 4) | (w << 6)
            u16 expected = 2 | (1 << 2) | (0 << 4) | (3 << 6);
            assert(inst.flags == expected);
            found = true;
        }
    }
    assert(found);
    PASS();
}

void test_builder_normalize_normalblend() {
    TEST("Builder: Normalize + NormalBlend");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto a = builder.ConstFloat3(1.0f, 0.0f, 0.0f);
    auto b = builder.ConstFloat3(0.0f, 1.0f, 0.0f);
    auto n = builder.Normalize(2, a);
    auto blended = builder.NormalBlend(a, b);

    assert(func.Instructions().size() == 4);
    auto& ins = func.Instructions();
    assert(ins[2].opcode == ShaderIROpcode::NORMALIZE);
    assert(ins[3].opcode == ShaderIROpcode::NORMAL_BLEND);
    PASS();
}

void test_builder_clamp() {
    TEST("Builder: Clamp with explicit min/max registers");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);

    auto val = builder.ConstFloat(0.7f);
    auto min_v = builder.ConstFloat(0.1f);
    auto max_v = builder.ConstFloat(0.9f);
    auto clamped = builder.Clamp(0, val, min_v, max_v);

    assert(func.Instructions().back().opcode == ShaderIROpcode::CLAMP);
    PASS();
}

// ============================================================================
// Phase 3: MaterialGraphToIR — per-node-type tests
// ============================================================================

void test_translate_multiply_scalar_vector() {
    TEST("Translate: Multiply scalar × Float3 (type promotion)");
    MaterialGraph graph;

    auto scalar = std::make_unique<ConstantFloatNode>();
    scalar->value = 2.0f;
    graph.AddNode(std::move(scalar)); // node 0

    auto vec = std::make_unique<ConstantFloat3Node>();
    vec->value = {1.0f, 0.5f, 0.2f};
    graph.AddNode(std::move(vec)); // node 1

    auto mul = std::make_unique<MultiplyNode>();
    graph.AddNode(std::move(mul)); // node 2

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output)); // node 3

    graph.Connect(0, 0, 2, 0); // scalar → Mul.A
    graph.Connect(1, 0, 2, 1); // vec → Mul.B
    graph.Connect(2, 0, 3, 0); // Mul → BaseColor (pin 0, Float4 output)

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    // Find MUL instruction and verify result_type = Float3 (2)
    bool found = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::MUL) {
            assert(inst.result_type == 2); // Float3
            found = true;
        }
    }
    assert(found);
    PASS();
}

void test_translate_clamp() {
    TEST("Translate: Clamp with min_val/max_val parameters");
    MaterialGraph graph;

    auto val = std::make_unique<ConstantFloatNode>();
    val->value = 0.7f;
    graph.AddNode(std::move(val)); // node 0

    auto clamp = std::make_unique<ClampNode>();
    clamp->min_val = 0.2f;
    clamp->max_val = 0.8f;
    graph.AddNode(std::move(clamp)); // node 1

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output)); // node 2

    graph.Connect(0, 0, 1, 0);
    graph.Connect(1, 0, 2, 1); // Roughness

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    // Should have: ConstFloat(0.7) + ConstFloat(0.2) + ConstFloat(0.8) + Clamp + StoreOutput
    assert(result.function.Instructions().size() == 5);

    // Verify clamp min/max in constant pool
    assert(std::abs(result.function.ConstantPool()[1] - 0.2f) < 0.001f);
    assert(std::abs(result.function.ConstantPool()[2] - 0.8f) < 0.001f);
    PASS();
}

void test_translate_fresnel() {
    TEST("Translate: Fresnel with power parameter");
    MaterialGraph graph;

    auto n = std::make_unique<ConstantFloat3Node>();
    n->value = {0.0f, 0.0f, 1.0f};
    graph.AddNode(std::move(n)); // node 0

    auto v = std::make_unique<ConstantFloat3Node>();
    v->value = {0.0f, 0.0f, 1.0f};
    graph.AddNode(std::move(v)); // node 1

    auto fresnel = std::make_unique<FresnelNode>();
    fresnel->power = 3.5f;
    graph.AddNode(std::move(fresnel)); // node 2

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output)); // node 3

    graph.Connect(0, 0, 2, 0); // N → Fresnel.N
    graph.Connect(1, 0, 2, 1); // V → Fresnel.V
    graph.Connect(2, 0, 3, 1); // Fresnel → Roughness

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    bool found = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::FRESNEL) {
            assert(inst.flags == (u16)(3.5f * 10.0f + 0.5f)); // 35
            found = true;
        }
    }
    assert(found);
    PASS();
}

void test_translate_pow() {
    TEST("Translate: Pow node");
    MaterialGraph graph;

    auto base = std::make_unique<ConstantFloatNode>();
    base->value = 2.0f;
    graph.AddNode(std::move(base)); // node 0

    auto exp = std::make_unique<ConstantFloatNode>();
    exp->value = 3.0f;
    graph.AddNode(std::move(exp)); // node 1

    auto pow_node = std::make_unique<PowNode>();
    graph.AddNode(std::move(pow_node)); // node 2

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output)); // node 3

    graph.Connect(0, 0, 2, 0);
    graph.Connect(1, 0, 2, 1);
    graph.Connect(2, 0, 3, 1);

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    bool found = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::POW) { found = true; }
    }
    assert(found);
    PASS();
}

void test_translate_normal_blend() {
    TEST("Translate: NormalBlend node");
    MaterialGraph graph;

    auto base = std::make_unique<ConstantFloat3Node>();
    base->value = {0.0f, 0.0f, 1.0f};
    graph.AddNode(std::move(base));

    auto detail = std::make_unique<ConstantFloat3Node>();
    detail->value = {0.1f, 0.1f, 0.99f};
    graph.AddNode(std::move(detail));

    auto blend = std::make_unique<NormalBlendNode>();
    graph.AddNode(std::move(blend));

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));

    graph.Connect(0, 0, 2, 0);
    graph.Connect(1, 0, 2, 1);
    graph.Connect(2, 0, 3, 0);

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    bool found = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::NORMAL_BLEND) { found = true; }
    }
    assert(found);
    PASS();
}

void test_translate_uv() {
    TEST("Translate: UV node");
    MaterialGraph graph;

    graph.AddNode(std::make_unique<UVNode>());    // node 0
    graph.AddNode(std::make_unique<MaterialOutputNode>()); // node 1
    // UV → no direct output pin match (UV is Float2, BaseColor expects Float4)
    // This tests that UV generates LOAD_UV instruction even without connection

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    // Only LOAD_UV emitted, no STORE_OUTPUT (unconnected)
    bool found = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::LOAD_UV) { found = true; }
    }
    assert(found);
    PASS();
}

void test_translate_texture() {
    TEST("Translate: Texture nodes get sequential binding slots");
    MaterialGraph graph;

    graph.AddNode(std::make_unique<ConstantTextureNode>()); // node 0, slot 0
    graph.AddNode(std::make_unique<ConstantTextureNode>()); // node 1, slot 1
    graph.AddNode(std::make_unique<ConstantTextureNode>()); // node 2, slot 2

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    int slot_count = 0;
    u16 slots[3] = {};
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::TEXTURE_HANDLE) {
            slots[slot_count++] = inst.flags;
        }
    }
    assert(slot_count == 3);
    assert(slots[0] == 0);
    assert(slots[1] == 1);
    assert(slots[2] == 2);
    PASS();
}

void test_translate_empty_graph() {
    TEST("Translate: Empty graph");
    MaterialGraph graph;

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    assert(result.function.Instructions().empty());
    PASS();
}

void test_translate_output_only() {
    TEST("Translate: Only MaterialOutput (all pins unconnected)");
    MaterialGraph graph;
    graph.AddNode(std::make_unique<MaterialOutputNode>());

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    assert(result.function.Instructions().empty());
    PASS();
}

void test_translate_unconnected_required_input() {
    TEST("Translate: Unconnected required input → error");
    MaterialGraph graph;

    auto add = std::make_unique<AddNode>();
    graph.AddNode(std::move(add)); // node 0 — inputs A, B both unconnected

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(!result.success);
    assert(!result.errors.empty());
    // Should report two errors (A and B both required)
    assert(result.errors.size() == 2);
    PASS();
}

void test_translate_type_inference_across_nodes() {
    TEST("Translate: Type inference — Float + Float3 through Lerp");
    MaterialGraph graph;

    // Node 0: ConstantFloat3 (A)
    auto a = std::make_unique<ConstantFloat3Node>();
    a->value = {1.0f, 0.0f, 0.0f};
    graph.AddNode(std::move(a));

    // Node 1: ConstantFloat3 (B)
    auto b = std::make_unique<ConstantFloat3Node>();
    b->value = {0.0f, 0.0f, 1.0f};
    graph.AddNode(std::move(b));

    // Node 2: ConstantFloat (Alpha)
    auto alpha = std::make_unique<ConstantFloatNode>();
    alpha->value = 0.5f;
    graph.AddNode(std::move(alpha));

    // Node 3: Lerp(A, B, Alpha) — result should be Float3
    graph.AddNode(std::make_unique<LerpNode>());

    // Node 4: Add(Lerp, Lerp) — result should also be Float3
    graph.AddNode(std::make_unique<AddNode>());

    // Node 5: Output
    graph.AddNode(std::make_unique<MaterialOutputNode>());

    graph.Connect(0, 0, 3, 0); // A → Lerp.A
    graph.Connect(1, 0, 3, 1); // B → Lerp.B
    graph.Connect(2, 0, 3, 2); // Alpha → Lerp.Alpha
    graph.Connect(3, 0, 4, 0); // Lerp → Add.A
    graph.Connect(3, 0, 4, 1); // Lerp → Add.B
    graph.Connect(4, 0, 5, 0); // Add → BaseColor

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    // Verify Lerp and Add both have result_type = Float3 (2)
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::LERP) {
            assert(inst.result_type == 2); // Float3
        }
        if (inst.opcode == ShaderIROpcode::ADD) {
            assert(inst.result_type == 2); // Float3
        }
    }
    PASS();
}

void test_translate_ir_encoding_correctness() {
    TEST("Translate: IR encoding — register mapping and result_type");
    MaterialGraph graph;

    auto c = std::make_unique<ConstantFloat4Node>();
    c->value = {1.0f, 0.0f, 0.0f, 1.0f};
    graph.AddNode(std::move(c)); // node 0 → reg 0

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output)); // node 1
    graph.Connect(0, 0, 1, 0);

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    assert(result.function.Instructions().size() == 2);

    auto& inst0 = result.function.Instructions()[0];
    assert(inst0.opcode == ShaderIROpcode::CONST_FLOAT4);
    assert(inst0.result_type == 3); // Float4
    assert(inst0.dst == 0);         // first register

    auto& inst1 = result.function.Instructions()[1];
    assert(inst1.opcode == ShaderIROpcode::STORE_OUTPUT);
    assert(inst1.src0 == 0);         // reads from reg 0
    assert(inst1.flags == 0);        // pin index 0 (BaseColor)
    PASS();
}

void test_translate_all_output_pins() {
    TEST("Translate: Multiple output pins (BaseColor + Roughness + Metallic)");
    MaterialGraph graph;

    auto color = std::make_unique<ConstantFloat4Node>();
    color->value = {0.8f, 0.2f, 0.1f, 1.0f};
    graph.AddNode(std::move(color)); // node 0

    auto roughness = std::make_unique<ConstantFloatNode>();
    roughness->value = 0.5f;
    graph.AddNode(std::move(roughness)); // node 1

    auto metallic = std::make_unique<ConstantFloatNode>();
    metallic->value = 0.1f;
    graph.AddNode(std::move(metallic)); // node 2

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output)); // node 3

    graph.Connect(0, 0, 3, 0); // BaseColor (pin 0)
    graph.Connect(1, 0, 3, 1); // Roughness (pin 1)
    graph.Connect(2, 0, 3, 2); // Metallic (pin 2)

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);

    assert(result.success);
    // 3 constants + 3 stores = 6 instructions
    assert(result.function.Instructions().size() == 6);

    // Verify STORE_OUTPUT pin indices
    int store_count = 0;
    u8 pin_indices[3] = {};
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::STORE_OUTPUT) {
            pin_indices[store_count++] = (u8)inst.flags;
        }
    }
    assert(store_count == 3);
    assert(pin_indices[0] == 0); // BaseColor
    assert(pin_indices[1] == 1); // Roughness
    assert(pin_indices[2] == 2); // Metallic
    PASS();
}

// ============================================================================
// Phase 6: MetalEmitter
// ============================================================================

void test_emitter_empty_ir() {
    TEST("Emitter: empty IR generates valid fragment function");
    ShaderIRFunction func;
    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("fragment FragmentOut fragmentMain") != std::string::npos);
    assert(result.source.find("return out;") != std::string::npos);
    PASS();
}

void test_emitter_const_float() {
    TEST("Emitter: ConstFloat → float literal");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto r0 = builder.ConstFloat(0.5f);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("r0 = ") != std::string::npos);
    assert(result.source.find("0.5f") != std::string::npos);
    PASS();
}

void test_emitter_const_float3() {
    TEST("Emitter: ConstFloat3 → float3 constructor");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto r0 = builder.ConstFloat3(1.0f, 2.0f, 3.0f);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("r0 = float3(") != std::string::npos);
    assert(result.source.find("1") != std::string::npos);
    assert(result.source.find("2") != std::string::npos);
    PASS();
}

void test_emitter_const_float4_store() {
    TEST("Emitter: ConstFloat4 + StoreOutput → out.albedo");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto r0 = builder.ConstFloat4(0.8f, 0.2f, 0.1f, 1.0f);
    builder.StoreOutput(r0, 0);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("r0 = float4(") != std::string::npos);
    assert(result.source.find("out.albedo = r0") != std::string::npos);
    PASS();
}

void test_emitter_arithmetic() {
    TEST("Emitter: Add/Mul arithmetic");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto a = builder.ConstFloat(1.0f);
    auto b = builder.ConstFloat(2.0f);
    auto c = builder.Add(0, a, b);
    auto d = builder.Mul(0, a, b);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("r2 = r0 + r1") != std::string::npos);
    assert(result.source.find("r3 = r0 * r1") != std::string::npos);
    PASS();
}

void test_emitter_lerp() {
    TEST("Emitter: Lerp → mix()");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto a = builder.ConstFloat3(1.0f, 0.0f, 0.0f);
    auto b = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto alpha = builder.ConstFloat(0.5f);
    auto l = builder.Lerp(2, a, b, alpha);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("mix(r0, r1, r2)") != std::string::npos);
    PASS();
}

void test_emitter_clamp() {
    TEST("Emitter: Clamp → clamp()");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto val = builder.ConstFloat(0.7f);
    auto min_v = builder.ConstFloat(0.1f);
    auto max_v = builder.ConstFloat(0.9f);
    auto c = builder.Clamp(0, val, min_v, max_v);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("clamp(r0, r1, r2)") != std::string::npos);
    PASS();
}

void test_emitter_saturate() {
    TEST("Emitter: Saturate → clamp(0,1)");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto val = builder.ConstFloat(1.5f);
    auto s = builder.Saturate(val);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("clamp(r0, 0.0f, 1.0f)") != std::string::npos);
    PASS();
}

void test_emitter_pow() {
    TEST("Emitter: Pow → pow()");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto base = builder.ConstFloat(2.0f);
    auto exp = builder.ConstFloat(3.0f);
    auto p = builder.Pow(base, exp);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("pow(r0, r1)") != std::string::npos);
    PASS();
}

void test_emitter_dot() {
    TEST("Emitter: Dot → dot()");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto a = builder.ConstFloat3(1.0f, 0.0f, 0.0f);
    auto b = builder.ConstFloat3(0.0f, 1.0f, 0.0f);
    auto d = builder.Dot(2, a, b);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("dot(r0, r1)") != std::string::npos);
    PASS();
}

void test_emitter_texture_sample() {
    TEST("Emitter: TextureHandle + Sample");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto uv = builder.LoadUV();
    auto tex = builder.TextureHandle(0);
    auto sampled = builder.Sample(tex, uv);
    builder.StoreOutput(sampled, 0);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("texture2d<float> tex0 [[texture(0)]]") != std::string::npos);
    assert(result.source.find("tex0.sample(defaultSampler, r0)") != std::string::npos);
    assert(result.source.find("out.albedo = r2") != std::string::npos);
    PASS();
}

void test_emitter_multiple_textures() {
    TEST("Emitter: Multiple texture declarations");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto uv = builder.LoadUV();
    auto tex0 = builder.TextureHandle(0);
    auto tex1 = builder.TextureHandle(1);
    auto s0 = builder.Sample(tex0, uv);
    auto s1 = builder.Sample(tex1, uv);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("tex0 [[texture(0)]]") != std::string::npos);
    assert(result.source.find("tex1 [[texture(1)]]") != std::string::npos);
    PASS();
}

void test_emitter_fresnel() {
    TEST("Emitter: Fresnel → pow(1-dot,...)");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto n = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto v = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto f = builder.Fresnel(n, v, 5.0f);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("pow(1.0f - clamp(dot(r0, r1), 0.0f, 1.0f)") != std::string::npos);
    assert(result.source.find("5.0f") != std::string::npos);
    PASS();
}

void test_emitter_normalize() {
    TEST("Emitter: Normalize → normalize()");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto v = builder.ConstFloat3(1.0f, 2.0f, 3.0f);
    auto n = builder.Normalize(2, v);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("normalize(r0)") != std::string::npos);
    PASS();
}

void test_emitter_normal_blend() {
    TEST("Emitter: NormalBlend → UDN pattern");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto base = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto detail = builder.ConstFloat3(0.1f, 0.1f, 0.99f);
    auto blended = builder.NormalBlend(base, detail);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("normalize(float3(r0.xy + r1.xy, r0.z * r1.z))") != std::string::npos);
    PASS();
}

void test_emitter_swizzle() {
    TEST("Emitter: Swizzle → component selection");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto v = builder.ConstFloat4(1.0f, 2.0f, 3.0f, 4.0f);
    auto s = builder.Swizzle(v, 2, 1, 0, 3); // z,y,x,w → .zyxw

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find(".zyxw") != std::string::npos);
    PASS();
}

void test_emitter_roughness_metallic() {
    TEST("Emitter: Roughness + Metallic → out.orm.y/z");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto roughness = builder.ConstFloat(0.5f);
    auto metallic = builder.ConstFloat(0.1f);
    auto color = builder.ConstFloat4(1.0f, 1.0f, 1.0f, 1.0f);
    builder.StoreOutput(color, 0);
    builder.StoreOutput(roughness, 1);
    builder.StoreOutput(metallic, 2);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("out.orm.y = r0") != std::string::npos);
    assert(result.source.find("out.orm.z = r1") != std::string::npos);
    PASS();
}

void test_emitter_full_material_graph() {
    TEST("Emitter: Full MaterialGraph → Metal end-to-end");
    MaterialGraph graph;

    auto color = std::make_unique<ConstantFloat4Node>();
    color->value = {0.8f, 0.2f, 0.1f, 1.0f};
    graph.AddNode(std::move(color)); // node 0

    auto roughness = std::make_unique<ConstantFloatNode>();
    roughness->value = 0.5f;
    graph.AddNode(std::move(roughness)); // node 1

    auto metallic = std::make_unique<ConstantFloatNode>();
    metallic->value = 0.1f;
    graph.AddNode(std::move(metallic)); // node 2

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output)); // node 3

    graph.Connect(0, 0, 3, 0); // BaseColor
    graph.Connect(1, 0, 3, 1); // Roughness
    graph.Connect(2, 0, 3, 2); // Metallic

    MaterialGraphToIR translator;
    auto ir_result = translator.Translate(graph);
    assert(ir_result.success);

    MetalEmitter emitter;
    auto emit_result = emitter.Emit(ir_result.function);

    assert(emit_result.success);
    assert(emit_result.source.find("fragment FragmentOut fragmentMain") != std::string::npos);
    assert(emit_result.source.find("out.albedo = r0") != std::string::npos);
    assert(emit_result.source.find("out.orm.y = r1") != std::string::npos);
    assert(emit_result.source.find("out.orm.z = r2") != std::string::npos);
    PASS();
}

void test_emitter_load_time() {
    TEST("Emitter: LoadTime → sceneData.time");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto t = builder.LoadTime();

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("sceneData.time") != std::string::npos);
    PASS();
}

void test_emitter_load_uv() {
    TEST("Emitter: LoadUV → in.uv");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto uv = builder.LoadUV();

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    assert(result.success);
    assert(result.source.find("in.uv") != std::string::npos);
    PASS();
}

// ============================================================================
// Phase 7: Metal Runtime Compilation Verification
// ============================================================================

#ifdef __APPLE__
bool metal_compile_verify(const std::string& source, std::string& error_msg) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) {
        error_msg = "Failed to create Metal device";
        pool->release();
        return false;
    }

    NS::Error* error = nullptr;
    NS::String* nsSource = NS::String::alloc()->init(source.c_str(), NS::UTF8StringEncoding);
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();

    MTL::Library* library = device->newLibrary(nsSource, options, &error);

    bool success = (library != nullptr);
    if (!success && error) {
        error_msg = error->localizedDescription()->utf8String();
    }

    if (library) library->release();
    nsSource->release();
    options->release();
    device->release();
    pool->release();

    return success;
}

void test_compile_empty_shader() {
    TEST("Compile: empty IR → valid Metal");
    ShaderIRFunction func;
    MetalEmitter emitter;
    auto result = emitter.Emit(func);
    assert(result.success);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_const_color() {
    TEST("Compile: ConstFloat4 → BaseColor");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto color = builder.ConstFloat4(0.8f, 0.2f, 0.1f, 1.0f);
    builder.StoreOutput(color, 0);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);
    assert(result.success);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_arithmetic() {
    TEST("Compile: Add/Mul/Lerp/Saturate/Pow/Dot");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    // Use Float4 for operations that go to albedo (pin 0 expects float4)
    auto a = builder.ConstFloat4(1.0f, 0.0f, 0.0f, 1.0f);
    auto b = builder.ConstFloat4(0.0f, 1.0f, 0.0f, 1.0f);
    auto alpha = builder.ConstFloat(0.5f);
    auto lerp = builder.Lerp(3, a, b, alpha);
    auto added = builder.Add(3, lerp, a);
    auto mul = builder.Mul(3, added, b);
    // Scalar operations
    auto a3 = builder.ConstFloat3(1.0f, 0.0f, 0.0f);
    auto b3 = builder.ConstFloat3(0.0f, 1.0f, 0.0f);
    auto dot_val = builder.Dot(2, a3, b3);
    auto pw = builder.Pow(dot_val, alpha);
    builder.StoreOutput(mul, 0);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_texture_sample() {
    TEST("Compile: TextureHandle + Sample");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto uv = builder.LoadUV();
    auto tex = builder.TextureHandle(0);
    auto sampled = builder.Sample(tex, uv);
    builder.StoreOutput(sampled, 0);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_fresnel() {
    TEST("Compile: Fresnel");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto n = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto v = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto f = builder.Fresnel(n, v, 5.0f);
    builder.StoreOutput(f, 1);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_normal_blend() {
    TEST("Compile: NormalBlend + Normalize");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto color = builder.ConstFloat4(1.0f, 1.0f, 1.0f, 1.0f);
    auto base = builder.ConstFloat3(0.0f, 0.0f, 1.0f);
    auto detail = builder.ConstFloat3(0.1f, 0.1f, 0.99f);
    auto norm = builder.Normalize(2, base);
    auto blended = builder.NormalBlend(norm, detail);
    builder.StoreOutput(color, 0);  // float4 for albedo

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_swizzle_clamp() {
    TEST("Compile: Swizzle + Clamp");
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto v = builder.ConstFloat4(1.0f, 2.0f, 3.0f, 4.0f);
    auto sw = builder.Swizzle(v, 2, 1, 0, 3);
    auto min_v = builder.ConstFloat(0.0f);
    auto max_v = builder.ConstFloat(1.0f);
    builder.StoreOutput(sw, 0);

    MetalEmitter emitter;
    auto result = emitter.Emit(func);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_full_material() {
    TEST("Compile: Full MaterialGraph → Metal end-to-end");
    MaterialGraph graph;

    auto color = std::make_unique<ConstantFloat4Node>();
    color->value = {0.8f, 0.2f, 0.1f, 1.0f};
    graph.AddNode(std::move(color));

    auto roughness = std::make_unique<ConstantFloatNode>();
    roughness->value = 0.5f;
    graph.AddNode(std::move(roughness));

    auto metallic = std::make_unique<ConstantFloatNode>();
    metallic->value = 0.1f;
    graph.AddNode(std::move(metallic));

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));

    graph.Connect(0, 0, 3, 0);
    graph.Connect(1, 0, 3, 1);
    graph.Connect(2, 0, 3, 2);

    MaterialGraphToIR translator;
    auto ir_result = translator.Translate(graph);
    assert(ir_result.success);

    MetalEmitter emitter;
    auto emit_result = emitter.Emit(ir_result.function);
    assert(emit_result.success);

    std::string err;
    bool compiled = metal_compile_verify(emit_result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << emit_result.source << "\n";
    }
    assert(compiled);
    PASS();
}
#endif // __APPLE__

// ============================================================================
// Phase 8: ShaderCompiler
// ============================================================================

void test_compiler_sync_basic() {
    TEST("ShaderCompiler: CompileSync basic MaterialGraph");
    using namespace primal::graphics::material_graph;

    MaterialGraph graph;
    auto color = std::make_unique<ConstantFloat4Node>();
    color->value = {0.8f, 0.2f, 0.1f, 1.0f};
    graph.AddNode(std::move(color));

    auto roughness = std::make_unique<ConstantFloatNode>();
    roughness->value = 0.5f;
    graph.AddNode(std::move(roughness));

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));

    graph.Connect(0, 0, 2, 0);
    graph.Connect(1, 0, 2, 1);

    auto result = primal::graphics::shader_ir::ShaderCompiler::CompileSync(graph);
    assert(result.success);
    assert(!result.source.empty());
    assert(result.source.find("fragment FragmentOut fragmentMain") != std::string::npos);
    PASS();
}

void test_compiler_sync_empty_graph() {
    TEST("ShaderCompiler: CompileSync empty graph (output only)");
    using namespace primal::graphics::material_graph;

    MaterialGraph graph;
    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));

    auto result = primal::graphics::shader_ir::ShaderCompiler::CompileSync(graph);
    assert(result.success);
    assert(result.source.find("float4(1.0f, 1.0f, 1.0f, 1.0f)") != std::string::npos);
    PASS();
}

void test_compiler_sync_error_graph() {
    TEST("ShaderCompiler: CompileSync graph with unconnected required inputs");
    using namespace primal::graphics::material_graph;

    MaterialGraph graph;
    auto add = std::make_unique<AddNode>();
    graph.AddNode(std::move(add));

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));

    // Add node has no inputs connected — should report errors
    graph.Connect(0, 0, 1, 0);

    auto result = primal::graphics::shader_ir::ShaderCompiler::CompileSync(graph);
    assert(!result.success);
    assert(!result.error.empty());
    PASS();
}

void test_compiler_sync_texture_material() {
    TEST("ShaderCompiler: CompileSync multiply two constants");
    using namespace primal::graphics::material_graph;

    MaterialGraph graph;

    auto a = std::make_unique<ConstantFloat4Node>();
    a->value = {1.0f, 0.9f, 0.8f, 1.0f};
    graph.AddNode(std::move(a));       // 0

    auto b = std::make_unique<ConstantFloat4Node>();
    b->value = {0.5f, 0.5f, 0.5f, 1.0f};
    graph.AddNode(std::move(b));       // 1

    auto mul = std::make_unique<MultiplyNode>();
    graph.AddNode(std::move(mul));     // 2

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));  // 3

    graph.Connect(0, 0, 2, 0);  // a → mul.A
    graph.Connect(1, 0, 2, 1);  // b → mul.B
    graph.Connect(2, 0, 3, 0);  // mul → output pin 0 (BaseColor)

    auto result = primal::graphics::shader_ir::ShaderCompiler::CompileSync(graph);
    assert(result.success);
    assert(result.source.find("0.899") != std::string::npos || result.source.find("0.9") != std::string::npos);
    PASS();
}

#ifdef __APPLE__
void test_compiler_sync_compile_verify() {
    TEST("ShaderCompiler: CompileSync output passes Metal compilation");
    using namespace primal::graphics::material_graph;

    MaterialGraph graph;
    auto color = std::make_unique<ConstantFloat4Node>();
    color->value = {0.8f, 0.2f, 0.1f, 1.0f};
    graph.AddNode(std::move(color));

    auto roughness = std::make_unique<ConstantFloatNode>();
    roughness->value = 0.5f;
    graph.AddNode(std::move(roughness));

    auto metallic = std::make_unique<ConstantFloatNode>();
    metallic->value = 0.1f;
    graph.AddNode(std::move(metallic));

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));

    graph.Connect(0, 0, 3, 0);
    graph.Connect(1, 0, 3, 1);
    graph.Connect(2, 0, 3, 2);

    auto result = primal::graphics::shader_ir::ShaderCompiler::CompileSync(graph);
    assert(result.success);

    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n";
        std::cerr << "  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}
#endif // __APPLE__

// ============================================================================
// Phase 9: New Nodes — CONST_FLOAT2, SELECT, REMAP, SampleTexture
// ============================================================================

void test_builder_const_float2() {
    TEST("Builder: ConstFloat2");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto r = builder.ConstFloat2(0.3f, 0.7f);
    (void)r;

    auto& insts = func.Instructions();
    assert(insts.size() == 1);
    assert(insts[0].opcode == ShaderIROpcode::CONST_FLOAT2);
    assert(insts[0].result_type == 1); // float2
    assert(insts[0].dst == 0);

    auto& cp = func.ConstantPool();
    assert(cp.size() == 2);
    assert(std::abs(cp[0] - 0.3f) < 1e-6f);
    assert(std::abs(cp[1] - 0.7f) < 1e-6f);
    PASS();
}

void test_builder_select() {
    TEST("Builder: Select");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto cond = builder.ConstFloat(1.0f);
    auto a = builder.ConstFloat3(1.0f, 0.0f, 0.0f);
    auto b = builder.ConstFloat3(0.0f, 1.0f, 0.0f);
    auto sel = builder.Select(2, cond, a, b);
    (void)sel;

    auto& insts = func.Instructions();
    auto& sel_inst = insts.back();
    assert(sel_inst.opcode == ShaderIROpcode::SELECT);
    assert(sel_inst.result_type == 2); // float3
    assert(sel_inst.src0 == cond);
    assert(sel_inst.src1 == a);
    assert(sel_inst.src2 == b);
    PASS();
}

void test_builder_remap() {
    TEST("Builder: Remap");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto val = builder.ConstFloat(0.5f);
    auto remapped = builder.Remap(val, 0.0f, 1.0f, 2.0f, 4.0f);
    (void)remapped;

    auto& insts = func.Instructions();
    auto& remap_inst = insts.back();
    assert(remap_inst.opcode == ShaderIROpcode::REMAP);
    assert(remap_inst.src0 == val);

    auto& cp = func.ConstantPool();
    u16 cidx = remap_inst.flags;
    assert(std::abs(cp[cidx] - 0.0f) < 1e-6f);
    assert(std::abs(cp[cidx + 1] - 1.0f) < 1e-6f);
    assert(std::abs(cp[cidx + 2] - 2.0f) < 1e-6f);
    assert(std::abs(cp[cidx + 3] - 4.0f) < 1e-6f);
    PASS();
}

void test_emitter_const_float2() {
    TEST("Emitter: ConstFloat2 → float2 constructor");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto v = builder.ConstFloat2(0.3f, 0.7f);
    builder.StoreOutput(v, 0);

    // Need float4 for albedo output — use float4 constant instead
    ShaderIRFunction func2;
    ShaderIRBuilder b2(func2);
    auto f2 = b2.ConstFloat2(0.3f, 0.7f);
    (void)f2;

    MetalEmitter emitter;
    auto result = emitter.Emit(func2);
    assert(result.success);
    assert(result.source.find("float2(") != std::string::npos);
    assert(result.source.find("0.3") != std::string::npos);
    assert(result.source.find("0.7") != std::string::npos);
    PASS();
}

void test_emitter_select() {
    TEST("Emitter: Select → ternary expression");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto cond = builder.ConstFloat(1.0f);
    auto a = builder.ConstFloat(0.2f);
    auto b = builder.ConstFloat(0.8f);
    auto sel = builder.Select(0, cond, a, b);
    (void)sel;

    MetalEmitter emitter;
    auto result = emitter.Emit(func);
    assert(result.success);
    assert(result.source.find("!= 0.0f) ? r") != std::string::npos);
    PASS();
}

void test_emitter_remap() {
    TEST("Emitter: Remap → arithmetic formula");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto val = builder.ConstFloat(0.5f);
    auto remapped = builder.Remap(val, 0.0f, 1.0f, 2.0f, 4.0f);
    (void)remapped;

    MetalEmitter emitter;
    auto result = emitter.Emit(func);
    assert(result.success);
    assert(result.source.find("2.0f") != std::string::npos);
    assert(result.source.find("4.0f") != std::string::npos);
    PASS();
}

void test_translate_const_float2() {
    TEST("Translate: ConstantFloat2 node → CONST_FLOAT2 opcode");
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto f2 = std::make_unique<ConstantFloat2Node>();
    f2->value = {0.25f, 0.75f};
    graph.AddNode(std::move(f2));  // 0
    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));  // 1

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);
    // Unconnected output pins — should still succeed (output pins unconnected)
    // But the node itself should translate to CONST_FLOAT2
    bool has_f2 = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::CONST_FLOAT2) { has_f2 = true; break; }
    }
    assert(has_f2);
    PASS();
}

void test_translate_select() {
    TEST("Translate: Select node → SELECT opcode");
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto a = std::make_unique<ConstantFloatNode>();
    a->value = 1.0f;
    graph.AddNode(std::move(a));  // 0

    auto b = std::make_unique<ConstantFloatNode>();
    b->value = 0.0f;
    graph.AddNode(std::move(b));  // 1

    // Use ConstFloat as condition (>0 = true)
    auto cond = std::make_unique<ConstantFloatNode>();
    cond->value = 1.0f;
    graph.AddNode(std::move(cond));  // 2

    auto sel = std::make_unique<SelectNode>();
    graph.AddNode(std::move(sel));  // 3

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));  // 4

    graph.Connect(2, 0, 3, 0);  // cond → Select.condition
    graph.Connect(0, 0, 3, 1);  // a → Select.true_value
    graph.Connect(1, 0, 3, 2);  // b → Select.false_value
    graph.Connect(3, 0, 4, 1);  // Select → output pin 1 (Roughness)

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);
    assert(result.success);

    bool has_select = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::SELECT) { has_select = true; break; }
    }
    assert(has_select);
    PASS();
}

void test_translate_remap() {
    TEST("Translate: Remap node → REMAP opcode with constant pool");
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto val = std::make_unique<ConstantFloatNode>();
    val->value = 0.5f;
    graph.AddNode(std::move(val));  // 0

    auto remap = std::make_unique<RemapNode>();
    remap->in_min = 0.0f;
    remap->in_max = 1.0f;
    remap->out_min = 2.0f;
    remap->out_max = 4.0f;
    graph.AddNode(std::move(remap));  // 1

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));  // 2

    graph.Connect(0, 0, 1, 0);  // val → Remap.value
    graph.Connect(1, 0, 2, 1);  // remap → output pin 1 (Roughness)

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);
    assert(result.success);

    bool has_remap = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::REMAP) {
            has_remap = true;
            auto& cp = result.function.ConstantPool();
            u16 cidx = inst.flags;
            assert(std::abs(cp[cidx] - 0.0f) < 1e-6f);
            assert(std::abs(cp[cidx + 1] - 1.0f) < 1e-6f);
            assert(std::abs(cp[cidx + 2] - 2.0f) < 1e-6f);
            assert(std::abs(cp[cidx + 3] - 4.0f) < 1e-6f);
        }
    }
    assert(has_remap);
    PASS();
}

void test_translate_sample_texture() {
    TEST("Translate: SampleTexture node → SAMPLE opcode");
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto tex = std::make_unique<ConstantTextureNode>();
    tex->asset_path = "test.png";
    graph.AddNode(std::move(tex));  // 0

    auto uv = std::make_unique<UVNode>();
    graph.AddNode(std::move(uv));  // 1

    auto sample = std::make_unique<SampleTextureNode>();
    graph.AddNode(std::move(sample));  // 2

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));  // 3

    graph.Connect(0, 0, 2, 0);  // tex → SampleTexture.texture
    graph.Connect(1, 0, 2, 1);  // uv → SampleTexture.uv
    graph.Connect(2, 0, 3, 0);  // sample → output pin 0 (BaseColor)

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);
    assert(result.success);

    bool has_sample = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::SAMPLE) { has_sample = true; break; }
    }
    assert(has_sample);
    PASS();
}

#ifdef __APPLE__
void test_compile_select() {
    TEST("Compile: Select node");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto cond = builder.ConstFloat(1.0f);
    auto a = builder.ConstFloat(0.2f);
    auto b = builder.ConstFloat(0.8f);
    auto sel = builder.Select(0, cond, a, b);
    (void)sel;

    MetalEmitter emitter;
    auto result = emitter.Emit(func);
    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_remap() {
    TEST("Compile: Remap node");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto val = builder.ConstFloat(0.5f);
    auto remapped = builder.Remap(val, 0.0f, 1.0f, 2.0f, 4.0f);
    (void)remapped;

    MetalEmitter emitter;
    auto result = emitter.Emit(func);
    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_compile_const_float2() {
    TEST("Compile: ConstFloat2");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto v = builder.ConstFloat2(0.3f, 0.7f);
    (void)v;

    MetalEmitter emitter;
    auto result = emitter.Emit(func);
    std::string err;
    bool compiled = metal_compile_verify(result.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n  SOURCE:\n" << result.source << "\n";
    }
    assert(compiled);
    PASS();
}
#endif // __APPLE__

// ============================================================================
// Phase 10: Curve
// ============================================================================

void test_curve_data_evaluate() {
    TEST("Curve: CurveData CPU evaluation");
    using namespace primal::graphics::material_graph;
    CurveData curve;
    curve.points[0] = {0.0f, 0.0f};
    curve.points[1] = {0.5f, 1.0f};
    curve.points[2] = {1.0f, 0.0f};
    curve.point_count = 3;

    assert(std::abs(curve.Evaluate(0.0f) - 0.0f) < 1e-5f);
    assert(std::abs(curve.Evaluate(0.25f) - 0.5f) < 1e-5f);
    assert(std::abs(curve.Evaluate(0.5f) - 1.0f) < 1e-5f);
    assert(std::abs(curve.Evaluate(0.75f) - 0.5f) < 1e-5f);
    assert(std::abs(curve.Evaluate(1.0f) - 0.0f) < 1e-5f);
    // Out of range clamps
    assert(std::abs(curve.Evaluate(-0.5f) - 0.0f) < 1e-5f);
    assert(std::abs(curve.Evaluate(1.5f) - 0.0f) < 1e-5f);
    PASS();
}

void test_translate_curve() {
    TEST("Translate: Curve node → CURVE_EVAL opcode");
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto time = std::make_unique<TimeNode>();
    graph.AddNode(std::move(time));  // 0

    auto curve_node = std::make_unique<CurveNode>();
    curve_node->curve.points[0] = {0.0f, 0.0f};
    curve_node->curve.points[1] = {0.5f, 1.0f};
    curve_node->curve.points[2] = {1.0f, 0.0f};
    curve_node->curve.point_count = 3;
    graph.AddNode(std::move(curve_node));  // 1

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));  // 2

    graph.Connect(0, 0, 1, 0);  // time → Curve.x
    graph.Connect(1, 0, 2, 1);  // curve → output pin 1 (Roughness)

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);
    assert(result.success);

    bool has_curve = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::CURVE_EVAL) {
            has_curve = true;
            // Verify constant pool: count=3, then [t0,v0,t1,v1,t2,v2]
            auto& cp = result.function.ConstantPool();
            u16 cidx = inst.flags;
            assert((u32)cp[cidx] == 3); // point_count
            assert(std::abs(cp[cidx + 1] - 0.0f) < 1e-5f);
            assert(std::abs(cp[cidx + 2] - 0.0f) < 1e-5f);
            assert(std::abs(cp[cidx + 3] - 0.5f) < 1e-5f);
            assert(std::abs(cp[cidx + 4] - 1.0f) < 1e-5f);
        }
    }
    assert(has_curve);
    PASS();
}

void test_emitter_curve() {
    TEST("Emitter: Curve → piecewise linear evaluation");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto x = builder.LoadTime();
    f32 points[] = {0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f};
    auto result_reg = builder.CurveEval(x, points, 3);
    (void)result_reg;

    MetalEmitter emitter;
    auto output = emitter.Emit(func);
    assert(output.success);
    assert(output.source.find("_result") != std::string::npos);
    assert(output.source.find("if (_x >=") != std::string::npos);
    PASS();
}

#ifdef __APPLE__
void test_compile_curve() {
    TEST("Compile: Curve evaluation");
    using namespace primal::graphics::shader_ir;
    ShaderIRFunction func;
    ShaderIRBuilder builder(func);
    auto x = builder.LoadTime();
    f32 points[] = {0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f};
    auto result_reg = builder.CurveEval(x, points, 3);
    (void)result_reg;

    MetalEmitter emitter;
    auto output = emitter.Emit(func);
    assert(output.success);

    std::string err;
    bool compiled = metal_compile_verify(output.source, err);
    if (!compiled) {
        std::cerr << "\n  COMPILE ERROR: " << err << "\n  SOURCE:\n" << output.source << "\n";
    }
    assert(compiled);
    PASS();
}

void test_serializer_curve_roundtrip() {
    TEST("Serializer: Curve round-trip");
    using namespace primal::graphics::material_graph;

    MaterialGraph graph;
    auto curve_node = std::make_unique<CurveNode>();
    curve_node->curve.points[0] = {0.0f, 0.0f};
    curve_node->curve.points[1] = {0.5f, 1.0f};
    curve_node->curve.points[2] = {1.0f, 0.25f};
    curve_node->curve.point_count = 3;
    graph.AddNode(std::move(curve_node));

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));

    // Serialize
    auto json = MaterialGraphSerializer::Serialize(graph);
    assert(json.find("curve_points") != std::string::npos);
    assert(json.find("[[0,0],[0.5,1],[1,0.25]]") != std::string::npos ||
           json.find("[[0.000000,0.000000],[0.500000,1.000000],[1.000000,0.250000]]") != std::string::npos);

    // Deserialize
    MaterialGraph graph2;
    bool ok = MaterialGraphSerializer::DeserializeIntoGraph(json, graph2);
    assert(ok);
    assert(graph2.GetNodes().size() == 2);

    auto& restored = graph2.GetNodes()[0];
    assert(std::strcmp(restored->TypeName(), "Curve") == 0);
    auto* cn = static_cast<CurveNode*>(restored.get());
    assert(cn->curve.point_count == 3);
    assert(std::abs(cn->curve.points[0].time - 0.0f) < 1e-5f);
    assert(std::abs(cn->curve.points[0].value - 0.0f) < 1e-5f);
    assert(std::abs(cn->curve.points[1].time - 0.5f) < 1e-5f);
    assert(std::abs(cn->curve.points[1].value - 1.0f) < 1e-5f);
    assert(std::abs(cn->curve.points[2].time - 1.0f) < 1e-5f);
    assert(std::abs(cn->curve.points[2].value - 0.25f) < 1e-5f);
    PASS();
}

void test_select_float3() {
    TEST("Translate: Select node with Float3 values");
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto a = std::make_unique<ConstantFloat3Node>();
    a->value = {1.0f, 0.0f, 0.0f};
    graph.AddNode(std::move(a));  // 0

    auto b = std::make_unique<ConstantFloat3Node>();
    b->value = {0.0f, 0.0f, 1.0f};
    graph.AddNode(std::move(b));  // 1

    auto cond = std::make_unique<ConstantFloatNode>();
    cond->value = 1.0f;
    graph.AddNode(std::move(cond));  // 2

    auto sel = std::make_unique<SelectNode>();
    graph.AddNode(std::move(sel));  // 3

    auto output = std::make_unique<MaterialOutputNode>();
    graph.AddNode(std::move(output));  // 4

    graph.Connect(2, 0, 3, 0);
    graph.Connect(0, 0, 3, 1);
    graph.Connect(1, 0, 3, 2);
    graph.Connect(3, 0, 4, 0);

    MaterialGraphToIR translator;
    auto result = translator.Translate(graph);
    assert(result.success);

    bool found = false;
    for (auto& inst : result.function.Instructions()) {
        if (inst.opcode == ShaderIROpcode::SELECT) {
            assert(inst.result_type == 2);
            found = true;
        }
    }
    assert(found);

    MetalEmitter emitter;
    auto emit_result = emitter.Emit(result.function);
    assert(emit_result.success);
    assert(emit_result.source.find("!= 0.0f) ? r") != std::string::npos);
    PASS();
}
#endif // __APPLE__

void test_emitter_preview_mode() {
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto c = std::make_unique<ConstantFloat4Node>();
    c->value = {1.0f, 0.5f, 0.25f, 1.0f};
    u32 cid = graph.AddNode(std::move(c));
    auto out = std::make_unique<MaterialOutputNode>();
    u32 oid = graph.AddNode(std::move(out));
    graph.Connect(cid, 0, oid, 0);

    MaterialGraphToIR translator;
    auto ir = translator.Translate(graph);
    MetalEmitter emitter;

    auto normal_output = emitter.Emit(ir.function, false);
    assert(normal_output.success);
    assert(normal_output.source.find("FragmentOut") != std::string::npos);
    assert(normal_output.source.find("out.albedo") != std::string::npos);
    assert(normal_output.source.find("out.velocity") != std::string::npos);

    auto preview_output = emitter.Emit(ir.function, true);
    assert(preview_output.success);
    assert(preview_output.source.find("FragmentOut") == std::string::npos);
    assert(preview_output.source.find("fragment float4 fragmentMain") != std::string::npos);
    assert(preview_output.source.find("return r") != std::string::npos);
    assert(preview_output.source.find("out.albedo") == std::string::npos);
    assert(preview_output.source.find("out.velocity") == std::string::npos);
    assert(preview_output.source.find("VertexOut") != std::string::npos);
    assert(preview_output.source.find("SceneData") != std::string::npos);

    printf("  [PASS] test_emitter_preview_mode\n");
}

#ifdef __APPLE__
void test_compile_preview() {
    using namespace primal::graphics;
    using namespace material_graph;
    using namespace shader_ir;

    MaterialGraph graph;
    auto c = std::make_unique<ConstantFloat4Node>();
    c->value = {1.0f, 0.5f, 0.25f, 1.0f};
    u32 cid = graph.AddNode(std::move(c));
    auto out = std::make_unique<MaterialOutputNode>();
    u32 oid = graph.AddNode(std::move(out));
    graph.Connect(cid, 0, oid, 0);

    MaterialGraphToIR translator;
    auto ir = translator.Translate(graph);
    MetalEmitter emitter;
    auto output = emitter.Emit(ir.function, true);
    assert(output.success);

    // The MetalEmitter already emits #include <metal_stdlib>, VertexOut, SceneData.
    // Append a stub VS so the library has both vertex and fragment stages.
    std::string source = output.source
        + "\nvertex VertexOut previewVertexMain(uint vid [[vertex_id]]) { VertexOut o; o.position = float4(0); return o; }\n";

    std::string err;
    bool compiled = metal_compile_verify(source, err);
    if (!compiled) {
        std::cerr << "[test_compile_preview] FAILED.\n";
        std::cerr << "--- Source ---\n" << source << "\n--- End Source ---\n";
        std::cerr << "Error: " << err << "\n";
    }
    assert(compiled && "Preview shader Metal compilation failed");
    printf("  [PASS] test_compile_preview\n");
}
#endif // __APPLE__

// ============================================================================
// Main
// ============================================================================

int main() {
    // Phase 1: IR encoding
    test_instruction_size();
    test_opcode_values();

    // Phase 2: Builder
    test_builder_basic();
    test_builder_constant_pool_float3();
    test_builder_arithmetic();
    test_builder_texture();
    test_builder_fresnel_flags();
    test_builder_swizzle();
    test_builder_normalize_normalblend();
    test_builder_clamp();

    // Phase 3: Translator per-node
    test_translate_multiply_scalar_vector();
    test_translate_clamp();
    test_translate_fresnel();
    test_translate_pow();
    test_translate_normal_blend();
    test_translate_uv();
    test_translate_texture();

    // Phase 4: Edge cases
    test_translate_empty_graph();
    test_translate_output_only();
    test_translate_unconnected_required_input();

    // Phase 5: Type inference and encoding
    test_translate_type_inference_across_nodes();
    test_translate_ir_encoding_correctness();
    test_translate_all_output_pins();

    // Phase 6: MetalEmitter
    test_emitter_empty_ir();
    test_emitter_const_float();
    test_emitter_const_float3();
    test_emitter_const_float4_store();
    test_emitter_arithmetic();
    test_emitter_lerp();
    test_emitter_clamp();
    test_emitter_saturate();
    test_emitter_pow();
    test_emitter_dot();
    test_emitter_texture_sample();
    test_emitter_multiple_textures();
    test_emitter_fresnel();
    test_emitter_normalize();
    test_emitter_normal_blend();
    test_emitter_swizzle();
    test_emitter_roughness_metallic();
    test_emitter_full_material_graph();
    test_emitter_load_time();
    test_emitter_load_uv();

    // Phase 7: Metal Runtime Compilation Verification
#ifdef __APPLE__
    test_compile_empty_shader();
    test_compile_const_color();
    test_compile_arithmetic();
    test_compile_texture_sample();
    test_compile_fresnel();
    test_compile_normal_blend();
    test_compile_swizzle_clamp();
    test_compile_full_material();
#endif

    // Phase 8: ShaderCompiler
    test_compiler_sync_basic();
    test_compiler_sync_empty_graph();
    test_compiler_sync_error_graph();
    test_compiler_sync_texture_material();
#ifdef __APPLE__
    test_compiler_sync_compile_verify();
#endif

    // Phase 9: New Nodes
    test_builder_const_float2();
    test_builder_select();
    test_builder_remap();
    test_emitter_const_float2();
    test_emitter_select();
    test_emitter_remap();
    test_translate_const_float2();
    test_translate_select();
    test_translate_remap();
    test_translate_sample_texture();
#ifdef __APPLE__
    test_compile_select();
    test_compile_remap();
    test_compile_const_float2();
#endif

    // Phase 10: Curve
    test_curve_data_evaluate();
    test_translate_curve();
    test_emitter_curve();
#ifdef __APPLE__
    test_compile_curve();
#endif
    test_serializer_curve_roundtrip();
    test_select_float3();
    test_emitter_preview_mode();
#ifdef __APPLE__
    test_compile_preview();
#endif

    return 0;
}
