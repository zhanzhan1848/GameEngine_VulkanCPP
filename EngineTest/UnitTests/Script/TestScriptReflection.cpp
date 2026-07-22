// === Phase 1 Task 4: Property Reflection 测试 ===
//
// 验证 entity_script::reflect(property_reflector&) 协议:
//   1) 子类重写 reflect() 通过 r.property(name, type, offset) 声明属性
//   2) property_collector 收集所有 property_descriptor,顺序与声明一致
//   3) 同类型不同实例 reflect() 输出 schema 一致(可缓存性 — offset 是类布局属性,
//      不依赖实例)
//
// 这是纯单元测试 — 不走 script::initialize() / dispatch 机制。
// reflect() 是直接虚函数调用,不需要 entity 子系统参与。
//
// 设计说明:
//   - test subclass 必须把 entity_script 的 protected constructor 暴露为 public,
//     否则不能在测试函数里 stack-allocate。
//   - offsetof 对 non-standard-layout 子类在技术上 UB,但主流编译器(clang/gcc/MSVC)
//     行为正确,且这是引擎属性反射的标准做法。若编译器告警可加 pragma 抑制。

#include "../TestFramework.h"
#include "Components/ScriptProperty.h"
#include "EngineAPI/GameEntity.h"

#include <cstddef>
#include <string>

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;

// reflect_test_script inherits a virtual destructor from entity_script → non-standard-layout.
// offsetof() on such types is technically UB per [support.types.layout] but works correctly
// on clang/gcc/MSVC and is the standard idiom for property reflection. Silence the warning
// so the test output stays clean; the offsets are correct.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

namespace {

// 测试脚本:声明 3 个属性(float / int / bool)
class reflect_test_script : public primal::script::entity_script {
public:
    float speed = 5.0f;
    int count = 3;
    bool active = true;

    void reflect(primal::script::property_reflector& r) override {
        r.property("speed", primal::script::property_type::float32,
                   static_cast<u32>(offsetof(reflect_test_script, speed)));
        r.property("count", primal::script::property_type::int32,
                   static_cast<u32>(offsetof(reflect_test_script, count)));
        r.property("active", primal::script::property_type::boolean,
                   static_cast<u32>(offsetof(reflect_test_script, active)));
    }

    explicit reflect_test_script(primal::game_entity::entity e)
        : primal::script::entity_script(e) {}
};

// 主测试:reflect() 收集 3 个属性,顺序与声明一致,offset 正确。
TestResult test_reflect_collects_all_properties() {
    reflect_test_script s{primal::game_entity::entity{}};
    primal::script::property_collector collector;
    s.reflect(collector);

    const auto& descs = collector.descriptors();
    if (descs.size() != 3) return TestResult::Failed;

    // 顺序应与 reflect() 声明一致
    if (std::string(descs[0].name) != "speed") return TestResult::Failed;
    if (descs[0].type != primal::script::property_type::float32) return TestResult::Failed;
    if (descs[0].offset != static_cast<u32>(offsetof(reflect_test_script, speed)))
        return TestResult::Failed;

    if (std::string(descs[1].name) != "count") return TestResult::Failed;
    if (descs[1].type != primal::script::property_type::int32) return TestResult::Failed;

    if (std::string(descs[2].name) != "active") return TestResult::Failed;
    if (descs[2].type != primal::script::property_type::boolean) return TestResult::Failed;

    return TestResult::Passed;
}

// 同类型不同实例 reflect() 输出应一致 — 这是 schema 可缓存性的前提。
// reflect() 描述的是类的内存布局(offset / type),与实例的值无关。
TestResult test_reflect_same_for_same_type() {
    reflect_test_script a{primal::game_entity::entity{}};
    reflect_test_script b{primal::game_entity::entity{}};
    a.speed = 1.0f; b.speed = 99.0f;  // 实例值不同 — 不应影响 schema

    primal::script::property_collector ca, cb;
    a.reflect(ca);
    b.reflect(cb);

    if (ca.descriptors().size() != cb.descriptors().size()) return TestResult::Failed;
    for (size_t i = 0; i < ca.descriptors().size(); ++i) {
        if (ca.descriptors()[i].offset != cb.descriptors()[i].offset) return TestResult::Failed;
        if (ca.descriptors()[i].type != cb.descriptors()[i].type) return TestResult::Failed;
        if (std::string(ca.descriptors()[i].name) != std::string(cb.descriptors()[i].name))
            return TestResult::Failed;
    }
    return TestResult::Passed;
}

// 测试脚本:通过 property_enum() 声明一个 enum 风格属性。
class enum_test_script : public primal::script::entity_script {
public:
    int state = 0;  // 0=Idle, 1=Active, 2=Paused

    static inline constexpr const char* state_names[3] = {"Idle", "Active", "Paused"};

    void reflect(primal::script::property_reflector& r) override {
        r.property_enum("state", static_cast<u32>(offsetof(enum_test_script, state)),
                        3, const_cast<const char**>(state_names));
    }

    explicit enum_test_script(primal::game_entity::entity e)
        : primal::script::entity_script(e) {}
};

// property_enum() 应设置 type=enum_, enum_count, enum_names, offset。
TestResult test_reflect_enum_property() {
    enum_test_script s{primal::game_entity::entity{}};
    primal::script::property_collector collector;
    s.reflect(collector);

    const auto& descs = collector.descriptors();
    if (descs.size() != 1) return TestResult::Failed;
    if (descs[0].type != primal::script::property_type::enum_) return TestResult::Failed;
    if (descs[0].enum_count != 3) return TestResult::Failed;
    if (std::string(descs[0].enum_names[0]) != "Idle") return TestResult::Failed;
    if (std::string(descs[0].enum_names[2]) != "Paused") return TestResult::Failed;
    if (descs[0].offset != static_cast<u32>(offsetof(enum_test_script, state)))
        return TestResult::Failed;
    if (std::string(descs[0].name) != "state") return TestResult::Failed;
    return TestResult::Passed;
}

// 测试脚本:通过 property_with_accessor() 声明带 getter/setter 的属性。
class accessor_test_script : public primal::script::entity_script {
public:
    float hp = 100.0f;  // backing field

    static void get_hp(void* instance, void* out) {
        auto* self = static_cast<accessor_test_script*>(instance);
        *static_cast<float*>(out) = self->hp;
    }
    static void set_hp(void* instance, const void* in) {
        auto* self = static_cast<accessor_test_script*>(instance);
        self->hp = *static_cast<const float*>(in);
    }

    void reflect(primal::script::property_reflector& r) override {
        r.property_with_accessor("hp", primal::script::property_type::float32,
                                 &accessor_test_script::get_hp,
                                 &accessor_test_script::set_hp);
    }

    explicit accessor_test_script(primal::game_entity::entity e)
        : primal::script::entity_script(e) {}
};

// property_with_accessor() 应设置 getter/setter,offset 留为默认 0(invariant)。
TestResult test_reflect_with_accessor() {
    accessor_test_script s{primal::game_entity::entity{}};
    primal::script::property_collector collector;
    s.reflect(collector);

    const auto& descs = collector.descriptors();
    if (descs.size() != 1) return TestResult::Failed;
    if (descs[0].type != primal::script::property_type::float32) return TestResult::Failed;
    if (descs[0].getter == nullptr) return TestResult::Failed;
    if (descs[0].setter == nullptr) return TestResult::Failed;
    if (descs[0].offset != 0) return TestResult::Failed;  // accessor path leaves offset unused
    if (std::string(descs[0].name) != "hp") return TestResult::Failed;
    // 调用 getter 验证函数指针可正确访问实例数据
    float out = 0.0f;
    descs[0].getter(&s, &out);
    if (out != 100.0f) return TestResult::Failed;
    return TestResult::Passed;
}

} // namespace

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void RunScriptReflectionTests() {
    TestSuite suite("Script.Reflection");
    suite.AddTestCase(TestCase("collects_all_properties",
                               test_reflect_collects_all_properties,
                               "property_collector captures all 3 declared properties "
                               "in declaration order with correct name/type/offset"));
    suite.AddTestCase(TestCase("same_output_for_same_type",
                               test_reflect_same_for_same_type,
                               "two instances of the same script type produce identical "
                               "schema (offsets are class-layout, not instance state)"));
    suite.AddTestCase(TestCase("reflect_enum_property",
                               test_reflect_enum_property,
                               "property_enum() sets type=enum_, enum_count, enum_names, "
                               "and offset; enum_names[0] / [2] resolve correctly"));
    suite.AddTestCase(TestCase("reflect_with_accessor",
                               test_reflect_with_accessor,
                               "property_with_accessor() sets getter/setter non-null and "
                               "leaves offset at default 0; getter invocation reads backing field"));
    suite.RunAllTests();
}

int main() {
    RunScriptReflectionTests();
    return 0;
}
