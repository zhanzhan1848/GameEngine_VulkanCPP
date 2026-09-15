#pragma once

// ParticleFXEditorPreviewTests.h — 编辑器私有预览发射器的渲染隔离门禁：
// 带编辑器预览标记的发射器不进世界场景渲染（packed_frame_pool 桥跳过）。
// 背景：PFX 编辑器的内嵌预览发射器建在引擎全局注册表里、位于世界原点，
// 无标记时会被主视口 ParticlePass 画出——表现为"场景原点凭空出现粒子"。

#include "Engine/ParticleFX/core/EmitterProgram.h"
#include "Engine/ParticleFX/core/PfxSystem.h"

#include <cassert>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

inline void test_editor_preview_emitter_excluded_from_bridge() {
    std::cout << "Testing editor-preview emitter excluded from render bridge..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":16,"stages":{
            "spawn":[{"module":"SpawnRate","rate":240}],
            "init":[{"module":"SetLifetime","min":5,"max":5}],
            "update":[]}})json", 7);
    assert(id != invalid_emitter);

    u32 count = 999;
    u32 blends[4] = {};

    // 未打标：粒子正常进渲染桥（回归保护——普通场景发射器不受影响）
    emitter_get(id)->tick(1.f / 60.f);
    packed_frame_pool(0, &count, blends);
    assert(count > 0);

    // 打编辑器预览标记：同一发射器、粒子仍存活，但不再进渲染桥
    // （池空时按契约返回 nullptr，只断言 count）
    emitter_set_editor_preview(id, true);
    emitter_get(id)->tick(1.f / 60.f);
    assert(emitter_get(id)->alive_count() > 0);   // 模拟照常运行
    count = 999;
    packed_frame_pool(0, &count, blends);
    assert(count == 0);

    // 取消标记：恢复进渲染桥（标记可逆，供调试/未来场景化预览）
    emitter_set_editor_preview(id, false);
    emitter_get(id)->tick(1.f / 60.f);
    count = 999;
    packed_frame_pool(0, &count, blends);
    assert(count > 0);

    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: editor-preview emitter excluded from render bridge" << std::endl;
}

inline void run_all_editor_preview_tests() {
    test_editor_preview_emitter_excluded_from_bridge();
}

} // namespace primal::pfx::test

#endif // DISABLE_PARTICLE_FX
