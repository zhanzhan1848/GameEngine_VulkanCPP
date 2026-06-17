// Unit tests for ParticleMaterialAPI (P2.8 C API wrapper).
// Tests the 14 C ABI functions exposed by EngineDLL/ParticleMaterialAPI.cpp.
// particle_material::create does NOT require subsystem init — it uses an internal
// id->material cache with default-initialized ParticleMaterial objects.

#include "TestFramework.h"
#include "CommonHeaders.h"
#include <iostream>

// --- extern "C" prototypes mirroring EDITOR_INTERFACE exports ---

extern "C" {
    // Lifecycle
    u32  CreateParticleMaterial();
    void DestroyParticleMaterial(u32 pm_id);

    // Blend
    u32  GetParticleMaterialBlendMode(u32 pm_id);
    void SetParticleMaterialBlendMode(u32 pm_id, u32 mode);

    // Base texture
    u64  GetParticleMaterialBaseTexture(u32 pm_id);
    void SetParticleMaterialBaseTexture(u32 pm_id, u64 tex_handle);

    // Texture sheet
    void GetParticleMaterialTextureSheet(u32 pm_id, u32* fx, u32* fy, f32* fps);
    void SetParticleMaterialTextureSheet(u32 pm_id, u32 fx, u32 fy, f32 fps);

    // Depth
    u32  IsParticleMaterialDepthWriteEnabled(u32 pm_id);
    void SetParticleMaterialDepthWriteEnabled(u32 pm_id, u32 enable);

    // Shadows
    u32  ParticleMaterialReceivesShadows(u32 pm_id);
    void SetParticleMaterialReceiveShadows(u32 pm_id, u32 enable);

    // Soft particles
    u32  ParticleMaterialUsesSoftParticles(u32 pm_id);
    void SetParticleMaterialSoftParticles(u32 pm_id, u32 enable, f32 distance);
}

using namespace Engine::Test;

// --- Test 1: Create + Destroy balance (no leak/crash) ---

TestResult TestCreateDestroy() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "CreateParticleMaterial should return non-zero id");
    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

// --- Test 2: BlendMode round-trip ---

TestResult TestBlendMode() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "Create failed");

    // Default should be additive (0) per emitter_config default
    u32 initial = GetParticleMaterialBlendMode(pm);
    TEST_ASSERT_EQ(0u, initial, "Default blend mode should be additive (0)");

    SetParticleMaterialBlendMode(pm, 1); // alpha
    u32 got = GetParticleMaterialBlendMode(pm);
    TEST_ASSERT_EQ(1u, got, "Blend mode should be alpha (1) after set");

    SetParticleMaterialBlendMode(pm, 2); // multiply
    got = GetParticleMaterialBlendMode(pm);
    TEST_ASSERT_EQ(2u, got, "Blend mode should be multiply (2)");

    SetParticleMaterialBlendMode(pm, 3); // premultiplied
    got = GetParticleMaterialBlendMode(pm);
    TEST_ASSERT_EQ(3u, got, "Blend mode should be premultiplied (3)");

    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

// --- Test 3: Base texture round-trip ---

TestResult TestBaseTexture() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "Create failed");

    SetParticleMaterialBaseTexture(pm, 42);
    u64 got = GetParticleMaterialBaseTexture(pm);
    TEST_ASSERT_EQ(42ull, got, "Base texture should be 42 after set");

    SetParticleMaterialBaseTexture(pm, 0); // clear
    got = GetParticleMaterialBaseTexture(pm);
    TEST_ASSERT_EQ(0ull, got, "Base texture should be 0 after clear");

    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

// --- Test 4: Texture sheet round-trip ---

TestResult TestTextureSheet() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "Create failed");

    SetParticleMaterialTextureSheet(pm, 4, 4, 30.0f);

    u32 fx = 0, fy = 0;
    f32 fps = 0.0f;
    GetParticleMaterialTextureSheet(pm, &fx, &fy, &fps);

    TEST_ASSERT_EQ(4u, fx, "Texture frames_x should be 4");
    TEST_ASSERT_EQ(4u, fy, "Texture frames_y should be 4");
    TEST_ASSERT_FLOAT_EQ(30.0f, fps, 0.001f, "Texture frame_rate should be 30.0");

    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

// --- Test 5: Depth write round-trip ---

TestResult TestDepthWrite() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "Create failed");

    // Default is false per emitter_config
    u32 initial = IsParticleMaterialDepthWriteEnabled(pm);
    TEST_ASSERT_EQ(0u, initial, "Default depth write should be disabled (0)");

    SetParticleMaterialDepthWriteEnabled(pm, 1);
    u32 got = IsParticleMaterialDepthWriteEnabled(pm);
    TEST_ASSERT_EQ(1u, got, "Depth write should be enabled (1)");

    SetParticleMaterialDepthWriteEnabled(pm, 0);
    got = IsParticleMaterialDepthWriteEnabled(pm);
    TEST_ASSERT_EQ(0u, got, "Depth write should be disabled (0)");

    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

// --- Test 6: Receive shadows round-trip ---

TestResult TestReceiveShadows() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "Create failed");

    // Default is false per emitter_config
    u32 initial = ParticleMaterialReceivesShadows(pm);
    TEST_ASSERT_EQ(0u, initial, "Default receive shadows should be false (0)");

    SetParticleMaterialReceiveShadows(pm, 1);
    u32 got = ParticleMaterialReceivesShadows(pm);
    TEST_ASSERT_EQ(1u, got, "Receive shadows should be true (1)");

    SetParticleMaterialReceiveShadows(pm, 0);
    got = ParticleMaterialReceivesShadows(pm);
    TEST_ASSERT_EQ(0u, got, "Receive shadows should be false (0)");

    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

// --- Test 7: Soft particles round-trip ---

TestResult TestSoftParticles() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "Create failed");

    // Default is false
    u32 initial = ParticleMaterialUsesSoftParticles(pm);
    TEST_ASSERT_EQ(0u, initial, "Default soft particles should be false (0)");

    SetParticleMaterialSoftParticles(pm, 1, 2.5f);
    u32 got = ParticleMaterialUsesSoftParticles(pm);
    TEST_ASSERT_EQ(1u, got, "Soft particles should be true (1)");

    SetParticleMaterialSoftParticles(pm, 0, 1.0f);
    got = ParticleMaterialUsesSoftParticles(pm);
    TEST_ASSERT_EQ(0u, got, "Soft particles should be false (0)");

    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

// --- Test 8: DestroyParticleMaterial(0) is safe no-op ---

TestResult TestDestroyZero() {
    // Should not crash
    DestroyParticleMaterial(0);
    return TestResult::Passed;
}

// --- Test 9: Get* on pm_id=0 returns safe defaults ---

TestResult TestInvalidId() {
    const u32 invalid = 0;

    TEST_ASSERT_EQ(0u, GetParticleMaterialBlendMode(invalid), "GetBlendMode(0) should return 0");
    TEST_ASSERT_EQ(0ull, GetParticleMaterialBaseTexture(invalid), "GetBaseTexture(0) should return 0");

    u32 fx = 99, fy = 99;
    f32 fps = 99.0f;
    GetParticleMaterialTextureSheet(invalid, &fx, &fy, &fps);
    TEST_ASSERT_EQ(0u, fx, "GetTextureSheet(0).fx should be 0");
    TEST_ASSERT_EQ(0u, fy, "GetTextureSheet(0).fy should be 0");
    TEST_ASSERT_FLOAT_EQ(0.0f, fps, 0.001f, "GetTextureSheet(0).fps should be 0.0");

    TEST_ASSERT_EQ(0u, IsParticleMaterialDepthWriteEnabled(invalid), "IsDepthWrite(0) should return 0");
    TEST_ASSERT_EQ(0u, ParticleMaterialReceivesShadows(invalid), "ReceivesShadows(0) should return 0");
    TEST_ASSERT_EQ(0u, ParticleMaterialUsesSoftParticles(invalid), "UsesSoftParticles(0) should return 0");

    // Void setters should also be safe no-ops
    SetParticleMaterialBlendMode(invalid, 1);
    SetParticleMaterialBaseTexture(invalid, 42);
    SetParticleMaterialTextureSheet(invalid, 2, 2, 15.0f);
    SetParticleMaterialDepthWriteEnabled(invalid, 1);
    SetParticleMaterialReceiveShadows(invalid, 1);
    SetParticleMaterialSoftParticles(invalid, 1, 3.0f);

    return TestResult::Passed;
}

// --- Test 10: Multiple materials coexist ---

TestResult TestMultipleMaterials() {
    u32 pm1 = CreateParticleMaterial();
    u32 pm2 = CreateParticleMaterial();
    u32 pm3 = CreateParticleMaterial();
    TEST_ASSERT(pm1 != 0 && pm2 != 0 && pm3 != 0, "All creates should succeed");
    TEST_ASSERT(pm1 != pm2 && pm2 != pm3 && pm1 != pm3, "Ids should be unique");

    // Set different blend modes
    SetParticleMaterialBlendMode(pm1, 0);
    SetParticleMaterialBlendMode(pm2, 1);
    SetParticleMaterialBlendMode(pm3, 2);

    // Verify independence
    TEST_ASSERT_EQ(0u, GetParticleMaterialBlendMode(pm1), "pm1 blend should be 0");
    TEST_ASSERT_EQ(1u, GetParticleMaterialBlendMode(pm2), "pm2 blend should be 1");
    TEST_ASSERT_EQ(2u, GetParticleMaterialBlendMode(pm3), "pm3 blend should be 2");

    DestroyParticleMaterial(pm1);
    DestroyParticleMaterial(pm2);
    DestroyParticleMaterial(pm3);
    return TestResult::Passed;
}

// --- Test 11: Invalid out-param safety (null pointers in GetTextureSheet) ---

TestResult TestNullOutParams() {
    u32 pm = CreateParticleMaterial();
    TEST_ASSERT(pm != 0, "Create failed");

    SetParticleMaterialTextureSheet(pm, 8, 4, 60.0f);

    // Passing null out-params should not crash
    GetParticleMaterialTextureSheet(pm, nullptr, nullptr, nullptr);

    // Partial nulls also safe
    u32 fy = 0;
    GetParticleMaterialTextureSheet(pm, nullptr, &fy, nullptr);
    TEST_ASSERT_EQ(4u, fy, "fy should be 4");

    DestroyParticleMaterial(pm);
    return TestResult::Passed;
}

int main() {
    TestSuite suite("ParticleMaterialAPI");

    TEST_CASE(suite, "CreateDestroy",   TestCreateDestroy);
    TEST_CASE(suite, "BlendMode",       TestBlendMode);
    TEST_CASE(suite, "BaseTexture",     TestBaseTexture);
    TEST_CASE(suite, "TextureSheet",    TestTextureSheet);
    TEST_CASE(suite, "DepthWrite",      TestDepthWrite);
    TEST_CASE(suite, "ReceiveShadows",  TestReceiveShadows);
    TEST_CASE(suite, "SoftParticles",   TestSoftParticles);
    TEST_CASE(suite, "DestroyZero",     TestDestroyZero);
    TEST_CASE(suite, "InvalidId",       TestInvalidId);
    TEST_CASE(suite, "Multiple",        TestMultipleMaterials);
    TEST_CASE(suite, "NullOutParams",   TestNullOutParams);

    TestStats stats = suite.RunAllTests();
    return (stats.failedTests == 0) ? 0 : 1;
}
