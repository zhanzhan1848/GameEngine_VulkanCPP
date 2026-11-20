/**
 * @file TestEntityIdentityRegistry.cpp
 * @brief EntityIdentityRegistry 单测（2026-11-05 lightmap 稳定键轮；
 *        同日对接修正轮改全量替换语义）
 *
 * 覆盖（注册表契约，见 Engine/Graphics/EntityIdentityRegistry.h）：
 *   1. 全量替换语义：推表 A(2 条) 后推表 B(1 条) → A 独有键查不到、
 *      B 键命中；幂等；entity id 0（合法 ECS 首槽）可作键；
 *   2. 未命中回退：GetStableKey miss 不改 out；Clear 后全 miss；
 *      空快照（count==0）= 清空（合法态，回退自愈）；
 *   3. 键解析回退：ResolveLightmapInstanceKey 注册表命中用稳定键、
 *      未命中回退 mesh_<hierarchyId>_e<entityId> 老命名；
 *   4. 整批校验：空键/无 NUL 超长键整批拒绝且旧表保持不变、
 *      63 字符键可接受、null+count>0 拒绝；
 *   5. 线程安全冒烟：并发整表替换/读不崩溃不死锁，汇合后终态一致。
 *
 * 自包含，零 GPU / Engine-lib 依赖（registry cpp 直接编入，沿
 * TestLightmapBaking 注册惯例）。注册表为进程级单例：每个用例先
 * ClearEntityIdentities 复位，用例间零依赖。
 */
#include "Engine/Graphics/EntityIdentityRegistry.h"

#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace primal::graphics;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")"   \
                      << std::endl;                                         \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

// 契约键形态："e" + 32 小写 hex（总长 33，LMAP name[64] 容得下）。
EntityIdentity MakeId(u32 native, const char* key) {
    EntityIdentity e{};
    e.native_entity_id = native;
    std::strncpy(e.stable_key, key, sizeof(e.stable_key) - 1);
    return e;
}

const char* kKeyA = "e0123456789abcdef0123456789abcdef";  // 33 chars
const char* kKeyB = "e1123456789abcdef0123456789abcdef";
const char* kKeyC = "e2123456789abcdef0123456789abcdef";

void TestFullReplaceSemantics() {
    ClearEntityIdentities();

    // 表 A（2 条）→ 全部命中。
    const EntityIdentity tableA[] = {MakeId(1, kKeyA), MakeId(2, kKeyB)};
    CHECK(SetEntityIdentities(tableA, 2), "table A accepted");
    std::string out;
    CHECK(GetStableKey(1, out) && out == kKeyA, "table A id 1 resolvable");
    CHECK(GetStableKey(2, out) && out == kKeyB, "table A id 2 resolvable");

    // 表 B（1 条，C# 侧全量快照形态）→ 整表替换：A 独有键 1 失效，
    // B 键 2 命中（旧值被换掉）。残留旧键清零 = ECS id 复用不误挂。
    const EntityIdentity tableB[] = {MakeId(2, kKeyC)};
    CHECK(SetEntityIdentities(tableB, 1), "table B accepted");
    CHECK(!GetStableKey(1, out), "table-A-only id gone after full replace");
    CHECK(GetStableKey(2, out) && out == kKeyC, "table B id resolvable");

    // 幂等：同表重推结果不变。
    CHECK(SetEntityIdentities(tableB, 1), "idempotent re-push accepted");
    CHECK(GetStableKey(2, out) && out == kKeyC, "idempotent value stable");
    CHECK(!GetStableKey(1, out), "idempotent absence stable");

    // entity id 0 是合法 ECS id（首槽位；RenderPipelineAPI 同注），可作键。
    const EntityIdentity zero[] = {MakeId(0, kKeyB)};
    CHECK(SetEntityIdentities(zero, 1), "entity id 0 accepted");
    CHECK(GetStableKey(0, out) && out == kKeyB, "entity id 0 resolvable");
    CHECK(!GetStableKey(2, out), "full replace to {0} drops id 2");
}

void TestMissAndClear() {
    ClearEntityIdentities();

    std::string out = "SENTINEL";
    CHECK(!GetStableKey(1234, out), "unknown id misses");
    CHECK(out == "SENTINEL", "miss leaves out unmodified");

    const EntityIdentity ids[] = {MakeId(1234, kKeyA)};
    CHECK(SetEntityIdentities(ids, 1), "set accepted");
    ClearEntityIdentities();
    CHECK(!GetStableKey(1234, out), "cleared id misses");

    // 空快照（count==0）= 清空（全量替换语义；未推送/推空表为合法态，
    // 收集/渲染未命中走旧命名回退自愈）。
    CHECK(SetEntityIdentities(ids, 1), "re-set accepted");
    CHECK(SetEntityIdentities(nullptr, 0), "empty snapshot (null, 0) ok");
    CHECK(!GetStableKey(1234, out), "empty snapshot clears table");
    CHECK(!SetEntityIdentities(nullptr, 3), "null + count>0 rejected");
}

void TestFallbackResolveKey() {
    ClearEntityIdentities();

    // 未命中 → 老命名（与 2026-11-05 前行为逐字符一致，零回归）。
    CHECK(ResolveLightmapInstanceKey(7, 42) == "mesh_42_e7",
          "fallback legacy name");
    CHECK(ResolveLightmapInstanceKey(0, 0) == "mesh_0_e0",
          "fallback zero ids");

    // 命中 → 稳定键（entityId 查表，mesh hierarchy id 不参与）。
    const EntityIdentity ids[] = {MakeId(7, kKeyA)};
    CHECK(SetEntityIdentities(ids, 1), "set accepted");
    CHECK(ResolveLightmapInstanceKey(7, 42) == kKeyA,
          "stable key wins over legacy name");
    CHECK(ResolveLightmapInstanceKey(8, 42) == "mesh_42_e8",
          "unregistered sibling still falls back");
}

void TestBatchValidation() {
    ClearEntityIdentities();
    const EntityIdentity good[] = {MakeId(9, kKeyA)};
    CHECK(SetEntityIdentities(good, 1), "baseline set accepted");

    // 空键 → 整批拒绝，旧表保持不变（不部分应用、不清表）。
    EntityIdentity bad[2];
    bad[0] = MakeId(10, kKeyB);
    bad[1] = MakeId(11, "");
    CHECK(!SetEntityIdentities(bad, 2), "empty key rejects whole batch");
    std::string out;
    CHECK(!GetStableKey(10, out), "rejected batch applied nothing (10)");
    CHECK(!GetStableKey(11, out), "rejected batch applied nothing (11)");
    CHECK(GetStableKey(9, out) && out == kKeyA,
          "rejected batch keeps old table (entry + value)");

    // 63 字符键可接受（name[64] 内最长可 NUL 终止串）。接受即整表替换：
    // 旧键 9 失效、12 生效。
    EntityIdentity maxLen{};
    maxLen.native_entity_id = 12;
    std::memset(maxLen.stable_key, 'e', 63);
    maxLen.stable_key[63] = '\0';
    const EntityIdentity okLen[] = {maxLen};
    CHECK(SetEntityIdentities(okLen, 1), "63-char key accepted");
    CHECK(!GetStableKey(9, out), "accepted batch fully replaced old table");

    // 64 字符填满无 NUL → strnlen=64 → 整批拒绝，旧表（{12}）原样保留。
    EntityIdentity tooLong{};
    tooLong.native_entity_id = 13;
    std::memset(tooLong.stable_key, 'e', 64);
    const EntityIdentity badLen[] = {tooLong};
    CHECK(!SetEntityIdentities(badLen, 1), "untrimmed 64-char key rejected");
    CHECK(!GetStableKey(13, out), "overlong key applied nothing");
    CHECK(GetStableKey(12, out) && out.size() == 63,
          "rejected batch keeps old table across overlong reject");
}

void TestThreadSmoke() {
    ClearEntityIdentities();

    // 2 写线程（各自交替推单条整表快照）+ 2 读线程。断言：整表替换并发
    // 下不死锁不崩溃；汇合后共享 id 必为两写者之一最后一次快照的合法值；
    // 终态做一次确定性整表替换并验证（读侧一致）。
    constexpr int kIters = 400;
    auto writer = [](u32 base, const char* key) {
        for (int i = 0; i < kIters; ++i) {
            const EntityIdentity own[] = {MakeId(base + (u32)(i % 8), key)};
            SetEntityIdentities(own, 1);
            const EntityIdentity shared[] = {MakeId(999, key)};
            SetEntityIdentities(shared, 1);
        }
    };
    auto reader = []() {
        std::string out;
        for (int i = 0; i < kIters; ++i) {
            GetStableKey(999, out);  // 竞态下命中与否都合法（整表在换）
            GetStableKey(5000 + (u32)(i % 16), out);
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(writer, 100, kKeyA);
    threads.emplace_back(writer, 200, kKeyB);
    threads.emplace_back(reader);
    threads.emplace_back(reader);
    for (auto& t : threads) t.join();

    std::string out;
    CHECK(GetStableKey(999, out) && (out == kKeyA || out == kKeyB),
          "shared id holds one of the writer keys after join");

    const EntityIdentity fin[] = {MakeId(999, kKeyC)};
    CHECK(SetEntityIdentities(fin, 1), "final deterministic set accepted");
    CHECK(GetStableKey(999, out) && out == kKeyC, "final value consistent");
}

} // namespace

int main() {
    TestFullReplaceSemantics();
    TestMissAndClear();
    TestFallbackResolveKey();
    TestBatchValidation();
    TestThreadSmoke();
    if (g_failures == 0) {
        std::cout << "[TestEntityIdentityRegistry] ALL PASSED" << std::endl;
        return 0;
    }
    std::cout << "[TestEntityIdentityRegistry] FAILED: " << g_failures
              << " check(s)" << std::endl;
    return 1;
}
