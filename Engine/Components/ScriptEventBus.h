// === Phase 1 Task 5: Cross-Script Event Bus ===
//
// script_event_bus singleton — type-erased pub/sub for inter-script communication.
//
// 设计契约(见 Docs/superpowers/specs/2026-07-07-script-core-capabilities-design.md §event bus):
//   1) FIFO dispatch — events drained in emission order.
//   2) Re-emit isolation — events emitted from within a handler go to a separate
//      `re_emitted_` queue, drained in a subsequent internal pass within the same
//      drain() call. depth_cap=8 prevents infinite emit loops.
//   3) Deferred subscribe/unsubscribe — mutations during drain are queued and
//      applied after drain completes, so the iteration bucket is stable.
//
// 线程安全契约:
//   - instance() 初始化是线程安全的(Meyers singleton, C++11 保证)。
//   - 所有 runtime access (subscribe/emit/unsubscribe/drain) 必须在主线程。
//     调用方(frame_tick → drain_events_impl)已通过 detail::check_main_thread() 强制。
//
// 事件类型约束:
//   E 必须是 trivially copyable(POD-like)。static_assert 在 subscribe/emit 模板实例化点
//   检查。禁止 std::string、虚函数类等。事件通过 `new E(event)` heap-copy 投递,
//   drain 后 delete。这避免了对象生命周期问题,代价是每次 emit 一次 heap alloc。
//
// 实现说明:
//   - 用 typeid(E).name() 的 FNV-1a hash 作为 event_id_type,跨 TU 一致
//     (static const 局部变量,每个 TU 实例化一次,值相同)。
//   - handler 通过 std::function<void(entity_script*, const void*)> 类型擦除存储。
//     subscribe 时构造一个 lambda,内部 const void* cast 回 const E* 后 forward
//     给原 handler。这避免了 void*/void(*)() round-trip UB(Bug 3),
//     同时支持 capturing lambdas、std::function、functor 等任意 callable。
//   - 不在 header 内 try/catch — 异常传播到 Script.cpp 的 drain_events_impl
//     调用点(frame_tick 已有 try/catch 边界)。但 drain() 本身没有 try/catch,
//     所以脚本 handler 抛异常会中断 drain。这是有意为之 — 异常应在 handler 内部
//     catch,不应该泄漏到 bus。Task 10 会加 exception-path 测试。
//
// 重要修正(相对于 plan 草稿 Docs/.../2026-07-07-script-core-capabilities-phase1.md):
//   Bug 1 修复:detail::subscription 加 event_id_type type 字段,deferred apply
//     时按 type 入对应桶(不再调用不存在的 event_type_id_void())。
//   Bug 2 修复:dispatcher 内移除 s.type != type 检查 — dispatcher 只被同桶调用,
//     所有 sub 已匹配 type。
//   Bug 3 修复:handler 不再用 void* 或 void(*)() 存储 function pointer — 改用
//     std::function<void(entity_script*, const void*)> 类型擦除。避免了 function
//     pointer round-trip UB,同时支持 capturing lambdas(比 plan 草稿的函数指针
//     API 更通用)。subscribe 模板用 Handler&& + std::is_invocable 检查。

#pragma once
#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <type_traits>
#include <cassert>
#include <cstdio>
#include <typeinfo>
#include <algorithm>

namespace primal::script {

// Event type identifier — FNV-1a hash of typeid(E).name().
// 64-bit hash collision 概率极低(~2^-64),且事件类型数量有限(几十个),
// 不需要 collision 处理。如未来需要,可加二次 string compare 验证。
using event_id_type = u64;

// Subscription id returned to caller. 0 是 invalid sentinel
// (next_sub_id_ 从 1 开始递增,永远不会返回 0)。
using subscription_id_t = u64;

namespace detail {

// FNV-1a hash of typeid(E).name() — stable across TUs for same E.
// 用 lambda + static const 局部变量保证每个 E 只 hash 一次。
template<typename E>
event_id_type event_type_id() {
    static const event_id_type id = []{
        const char* name = typeid(E).name();
        event_id_type h = 14695981039346656037ULL;
        for (const char* p = name; *p; ++p) {
            h ^= static_cast<u8>(*p);
            h *= 1099511628211ULL;
        }
        return h;
    }();
    return id;
}

// Type-erased subscription record.
//
// Type erasure 设计:
//   - handler_store: std::function<void(entity_script*, const void*)> — 已类型擦除
//     的 callable。const void* 在 trampoline 内 cast 回 const E*。
//   - 这种设计支持任意 callable(函数指针、lambda、std::function、functor),
//     包括捕获 lambda。std::function 内部会 SBO 小对象优化或 heap-alloc capture。
//
// 关于 Bug 3(plan 提到的 void* 存 function pointer UB):
//   我们不再用 void* 存函数指针 — 直接用 std::function 类型擦除。这避免了
//   function pointer round-trip UB,同时支持 capturing lambdas(更通用)。
//   代价是每次 subscribe 一次 std::function 构造(可能 heap alloc),
//   但 subscribe 不在 hot path(每帧不会 subscribe),完全可接受。
//
// Dead subscription 标记:
//   用 `dead` bool flag 而不是 `subscriber == nullptr`。原因:owner-less 订阅
//   (subscriber=nullptr)是合法的系统级广播,不应该被当作 dead。
//   unsubscribe() 把 dead=true,dispatcher 跳过 dead 的 sub。
struct subscription {
    subscription_id_t id;
    event_id_type type;             // Bug 1 fix: 记录 type,deferred apply 时按 type 入桶
    entity_script* subscriber;       // 可为 nullptr(owner-less 系统级广播)
    bool dead = false;               // true = 已 unsubscribe,dispatcher 跳过
    std::function<void(entity_script*, const void*)> handler_store;
};

// Queued event with type-erased payload + dispatcher.
// dispatcher 只接收 matching-type bucket(subscribers_[eid] 的 vector),
// 不需要再 check type(见 Bug 2 fix)。
struct queued_event {
    event_id_type type;
    void* payload;                  // heap-allocated copy of E
    void(*deleter)(void*);
    void(*dispatcher)(void* payload,
                      std::vector<subscription>& subs);
};

} // namespace detail

class script_event_bus {
public:
    static script_event_bus& instance() {
        // Meyers singleton — C++11 guarantees thread-safe initialization.
        // 但所有 runtime access 必须在主线程(由调用方 check_main_thread 保证)。
        static script_event_bus inst;
        return inst;
    }

    // Subscribe a handler for event type E.
    //
    // self: 订阅者 entity_script 指针,可为 nullptr(系统级广播)。
    //   用于 unsubscribe_all(owner) 批量清理。
    // handler: 任意 callable,签名 void(entity_script*, const E&)。
    //   支持函数指针、lambda(含捕获)、std::function、functor。
    //   内部存为 std::function 类型擦除 — subscribe 不在 hot path,构造代价可接受。
    //
    // 返回 subscription_id_t,用于 unsubscribe。0 表示无效(永远不会返回)。
    //
    // 线程契约:必须在主线程调用。drain 进行中调用会被 defer 到 drain 结束。
    template<typename E, typename Handler>
    subscription_id_t subscribe(entity_script* self, Handler&& handler) {
        static_assert(std::is_trivially_copyable_v<E>,
                      "Event type E must be trivially copyable (POD-like). "
                      "No std::string, no virtual classes.");
        // Handler 必须可调用为 void(entity_script*, const E&)
        static_assert(std::is_invocable_v<Handler, entity_script*, const E&>,
                      "Handler must be callable as void(entity_script*, const E&)");

        const auto eid = detail::event_type_id<E>();

        // 类型擦除:把 Handler 包装成 std::function<void(entity_script*, const void*)>,
        // 内部 const void* cast 回 const E* 再 forward 给原 handler。
        // Bug 3 fix:不再用 void*/void(*)() round-trip,直接用 std::function 类型擦除。
        std::function<void(entity_script*, const void*)> erased =
            [h = std::forward<Handler>(handler)](entity_script* self_, const void* ev) mutable {
                h(self_, *static_cast<const E*>(ev));
            };

        detail::subscription s;
        s.id = next_sub_id_++;
        s.type = eid;                                    // Bug 1 fix: capture type
        s.subscriber = self;
        s.handler_store = std::move(erased);

        const subscription_id_t returned_id = s.id;  // save before move

        if (draining_) {
            deferred_subscriptions_.push_back(std::move(s));
        } else {
            subscribers_[eid].push_back(std::move(s));
        }
        return returned_id;
    }

    // Unsubscribe by id. O(N) over all subscribers — fine for hot path
    // (subscription count 通常 < 几百)。dead subscription 标记 dead=true,
    // dispatcher 跳过。不 swap-remove(会打乱 FIFO 顺序,且 deferred 路径复杂)。
    // 生产实现可以定期 compact,但 Phase 1 不做。
    void unsubscribe(subscription_id_t subscription_id) {
        if (draining_) {
            deferred_unsubscribes_.push_back(subscription_id);
            return;
        }
        // Linear search — bucket count 有限,可接受。
        for (auto& [type, subs] : subscribers_) {
            for (auto& s : subs) {
                if (s.id == subscription_id && !s.dead) {
                    s.dead = true;
                    return;
                }
            }
        }
        // 不存在的 id — silently ignore(允许 double-unsubscribe)
    }

    // Unsubscribe all subscriptions owned by `owner`. Used in entity_script::destroy
    // to prevent dangling pointers. 同样 O(N) over all subscribers。
    // owner=nullptr 时 noop(系统级广播没有 owner 可批量清理)。
    void unsubscribe_all(entity_script* owner) {
        if (owner == nullptr) return;
        if (draining_) {
            deferred_unsubscribe_owners_.push_back(owner);
            return;
        }
        for (auto& [type, subs] : subscribers_) {
            for (auto& s : subs) {
                if (s.subscriber == owner) s.dead = true;
            }
        }
    }

    // Emit event to all matching subscribers.
    //
    // 线程契约:主线程。drain 进行中调用会进 re_emitted_ 队列,
    // 在当前 drain 的下一 pass 投递。
    template<typename E>
    void emit(const E& event) {
        static_assert(std::is_trivially_copyable_v<E>,
                      "Event type E must be trivially copyable (POD-like).");

        const auto eid = detail::event_type_id<E>();
        auto* payload = new E(event);  // heap-copy;freed in drain after dispatch

        detail::queued_event qe;
        qe.type = eid;
        qe.payload = payload;
        qe.deleter = [](void* p) { delete static_cast<E*>(p); };
        // Bug 2 fix: dispatcher 不再 check s.type != type — 调用方保证 bucket 匹配。
        qe.dispatcher = [](void* payload_raw,
                           std::vector<detail::subscription>& subs) {
            const E* ev = static_cast<const E*>(payload_raw);
            for (auto& s : subs) {
                if (s.dead) continue;  // unsubscribed/dead
                // handler_store 已类型擦除,const void* cast 回 const E* 由
                // subscribe 时构造的 lambda 负责。
                s.handler_store(s.subscriber, static_cast<const void*>(ev));
            }
        };

        if (draining_) {
            re_emitted_.push_back(qe);
        } else {
            queue_.push_back(qe);
        }
    }

    // Drain all queued events. Called from drain_events_impl (Script.cpp).
    //
    // 算法:
    //   1) Mark draining_=true,使后续 emit 进 re_emitted_ 而非 queue_。
    //   2) Loop:从 queue_ 取所有事件,逐个 dispatch 到 matching bucket。
    //   3) dispatch 完后 delete payload。
    //   4) 若 re_emitted_ 非空,移到 queue_ 并重复(depth++)。
    //   5) depth >= depth_cap (8) → assert fail(Debug 模式 abort)。
    //   6) 结束后处理 deferred_subscriptions_ / deferred_unsubscribes_。
    //
    // 重要:trampoline 内 handler 可能抛异常。drain() 没有 try/catch,
    // 异常会传播到 frame_tick 的 try/catch 边界(Script.cpp 已有)。
    // 但这会中断整个 drain — 剩余事件不投递。这是有意为之:handler 异常是
    // 脚本 bug,应由脚本自己 catch。Task 10 会加 exception-path 测试验证。
    void drain() {
        if (draining_) {
            // 防重入 — frame_tick 不会重入 drain,但保护一下
            return;
        }
        draining_ = true;

        constexpr int depth_cap = 8;
        int depth = 0;

        while (!queue_.empty()) {
            if (depth >= depth_cap) {
                // Depth cap: log warning when exceeded, assert in Debug builds (hard abort
                // under NDEBUG-off). Release builds log only and stop re-emitting, which
                // prevents stack overflow but does not abort. Remaining re_emitted_ events
                // are discarded to break the emit loop.
                std::fprintf(stderr,
                    "script_event_bus::drain depth cap %d exceeded — "
                    "likely emit loop in handler. Discarding remaining %zu re-emitted events.\n",
                    depth_cap, re_emitted_.size());
                re_emitted_.clear();
                break;
            }

            // Snapshot 当前 queue,清空让 emit 进 re_emitted_。
            auto current = std::move(queue_);
            queue_.clear();

            for (auto& qe : current) {
                auto it = subscribers_.find(qe.type);
                if (it != subscribers_.end()) {
                    qe.dispatcher(qe.payload, it->second);
                }
                qe.deleter(qe.payload);
            }

            if (!re_emitted_.empty()) {
                queue_ = std::move(re_emitted_);
                re_emitted_.clear();
                ++depth;
            } else {
                break;
            }
        }

        assert(depth < depth_cap && "event bus drain depth cap exceeded (likely emit loop)");

        draining_ = false;

        // Apply deferred subscribe/unsubscribe after drain completes.
        // Bug 1 fix:用 subscription.type 字段定位 bucket,不再调用不存在的 event_type_id_void()。
        for (auto& s : deferred_subscriptions_) {
            subscribers_[s.type].push_back(s);
        }
        deferred_subscriptions_.clear();

        for (subscription_id_t id : deferred_unsubscribes_) {
            unsubscribe(id);  // draining_ 已 false,走快路径
        }
        deferred_unsubscribes_.clear();

        for (entity_script* owner : deferred_unsubscribe_owners_) {
            unsubscribe_all(owner);
        }
        deferred_unsubscribe_owners_.clear();

        // I5: Auto-compact dead subscriptions. drain() is already O(n) per pass,
        // so one additional O(n) erase-remove pass is negligible. This prevents
        // dead slots from accumulating in long-running sessions (editor, runtime)
        // and slowing down every subsequent drain's dispatch loop.
        compact();
    }

    // I5: Erase all dead subscription slots in-place.
    //
    // Contract: called automatically at the end of every drain(). Also safe to
    // call manually from outside drain (e.g., after bulk unsubscribe_all during
    // entity removal). Does nothing if no dead subscriptions exist.
    //
    // Implementation: std::remove_if + erase idiom per event-type bucket.
    // O(n) total across all buckets — same complexity as one drain dispatch pass.
    void compact() {
        for (auto& [type, subs] : subscribers_) {
            subs.erase(
                std::remove_if(subs.begin(), subs.end(),
                               [](const detail::subscription& s) { return s.dead; }),
                subs.end()
            );
        }
    }

    // Test-only:reset 所有 bus state。用于测试之间清空,避免 singleton 跨测试污染。
    // 生产代码不应调用 — bus 生命周期跟 script::initialize()/shutdown() 解耦,
    // 不在 shutdown 时 reset(订阅者需要手动 unsubscribe,否则下次 initialize 会泄漏)。
    void reset() {
        // 释放所有 pending events 防止内存泄漏
        for (auto& qe : queue_) qe.deleter(qe.payload);
        for (auto& qe : re_emitted_) qe.deleter(qe.payload);
        queue_.clear();
        re_emitted_.clear();
        subscribers_.clear();
        deferred_subscriptions_.clear();
        deferred_unsubscribes_.clear();
        deferred_unsubscribe_owners_.clear();
        draining_ = false;
        next_sub_id_ = 1;
    }

private:
    script_event_bus() = default;
    ~script_event_bus() {
        // 防止未 drain 的 events 泄漏(程序退出时)
        for (auto& qe : queue_) qe.deleter(qe.payload);
        for (auto& qe : re_emitted_) qe.deleter(qe.payload);
    }
    script_event_bus(const script_event_bus&) = delete;
    script_event_bus& operator=(const script_event_bus&) = delete;

    std::unordered_map<event_id_type, std::vector<detail::subscription>> subscribers_;
    std::vector<detail::queued_event> queue_;
    std::vector<detail::queued_event> re_emitted_;
    std::vector<detail::subscription> deferred_subscriptions_;
    std::vector<subscription_id_t> deferred_unsubscribes_;
    std::vector<entity_script*> deferred_unsubscribe_owners_;
    bool draining_ = false;
    subscription_id_t next_sub_id_ = 1;  // 0 是 invalid sentinel
};

} // namespace primal::script
