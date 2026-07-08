#pragma once
#include "ComponentsCommon.h"
#include <functional>
#include <memory>

namespace primal::script {

	struct init_info
	{
		detail::script_creator script_creator;
	};

	component create(init_info info, game_entity::entity entity);

	// === Phase 1 Task 6 新增 ===
	// Alternative create() overload that takes a already-constructed script
	// instance (as unique_ptr<entity_script>). This bypasses the
	// init_info::script_creator function pointer (which cannot capture state)
	// and is used by the register_external C ABI adapter to inject
	// external_script instances that carry per-instance user_data + callbacks.
	component create(std::unique_ptr<entity_script> instance,
	                 game_entity::entity entity);
	void remove(component c);
	component get_component_for_entity(game_entity::entity_id eid);
	void remove_for_entity(game_entity::entity_id eid);
	void update(f32 dt);

	// === Phase 1 Task 2 新增 ===
	// 物理步 dispatch:遍历所有 entity_script 调 fixed_update。
	// 与 update() 不同:fixed_update 不消费 transform_cache(物理步通常独立调度)。
	void fixed_update(f32 dt);
	// update() 全部完成后再调 late_update(相机跟随、后处理逻辑)。
	void late_update(f32 dt);

	// === Phase 1 Task 3: 推荐入口 ===
	// frame_tick 是单帧推荐入口,内部按 spec §5.1 顺序调用颗粒化 API:
	//   drain_callbacks → fixed_update → update → late_update → drain_events
	//   → process_deferred_reloads → apply_deferred_subscriptions
	// 调用方可以只用 frame_tick,也可以分别调 fixed_update/update/late_update
	// (例如编辑器模式不调物理步)。所有颗粒化 API 都各自 check_main_thread,
	// frame_tick 也独立 check 一次(冗余但明确)。
	void frame_tick(f32 dt);

	// === Phase 1 后续 task 占位声明(本 task 仅声明,实现见各 task)===
	void drain_events();                       // Task 5:事件总线派发
	void drain_callbacks();                    // Task 8:跨线程 post_to_main_thread 回调
	void reload(game_entity::entity_id eid);   // Task 7:热重载单个 entity 的 script

	// === Phase 1 Task 8: 跨线程回调提交 ===
	// post_to_main_thread 把 callback 推入 lock-free MPSC queue。
	// 任意线程可调(唯一不需要主线程的 script API)。
	// drain 在 frame_tick 第 1 步执行,callback 在主线程上同步运行。
	//
	// 调用方契约:shutdown() 前必须 join 所有 producer 线程。
	// shutdown 会清理 queue 中 pending callbacks,但无法看到 producer
	// 正在 push(尚未 exchange 到 head)的节点 — 此时该节点泄漏。
	// 这是 MPSC shutdown 的标准限制,不是 bug。
	void post_to_main_thread(std::function<void()> callback);

	// 初始化/关闭脚本子系统。initialize() 必须在主线程上调用一次,
	// 之后所有 script API(包括 update)只能在主线程上调用(由 check_main_thread 强制)。
	// shutdown() 后系统回到未初始化状态,可再次 initialize。
	void initialize();
	void shutdown();
}