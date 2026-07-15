#include "Script.h"
#include "ScriptExternal.h"
#include "ScriptProperty.h"
#include "ScriptEventBus.h"
#include "ScriptState.h"
#include "Entity.h"
#include "Transform.h"

#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_set>

#define USE_TRANSFORM_CACHE_MAP 1

namespace primal::script {
	namespace {
		utl::vector<detail::script_ptr>				entity_scripts;
		utl::vector<script_id>							dense_script_ids;
		utl::vector<id::id_type>						id_mapping;
		utl::vector<script_id>							entity_to_script;

		utl::vector<id::generation_type>			generations;
		utl::deque<script_id>						free_ids;

		utl::vector<transform::component_cache>		transform_cache;
#if USE_TRANSFORM_CACHE_MAP
		std::unordered_map<id::id_type, u32>		cache_map;
#endif

		// === Phase 1 Task 7: deferred reload queue ===
		// reload(eid) 把 entity_id 推入这里,frame_tick 末尾 process_deferred_reloads_impl
		// 统一处理(去重 + 按序 reload)。设计见 spec §4.5 + §5.2。
		utl::vector<game_entity::entity_id>		deferred_reloads_;

		using script_registry = std::unordered_map<size_t, detail::script_creator>;

		script_registry& registry()
		{
			// NOTE: we put this static variable in a function beacuse of
			//		the initialization order of static data. This way, we can
			//		be certain that the data is initialized before accessing it.
			static script_registry reg;
			return reg;
		}
#ifdef USE_WITH_EDITOR
		utl::vector<std::string>& script_names()
		{
			// NOTE: we put this static variable in a function beacuse of
			//		the initialization order of static data. This way, we can
			//		be certain that the data is initialized before accessing it.
			static utl::vector<std::string> names;
			return names;
		}
#endif

		bool exists(script_id id)
		{
			assert(id::is_valid(id));
			const id::id_type index{ id::index(id) };
			assert(index < generations.size() && id_mapping[index] < entity_scripts.size());
			assert(generations[index] == id::generation(id));
			return (generations[index] == id::generation(id)) &&
				entity_scripts[id_mapping[index]] &&
				entity_scripts[id_mapping[index]]->is_valid();
		}

		// === Phase 1 Task 6: register_external C ABI adapter ===
		//
		// external_script bridges C callback function pointers into the
		// entity_script virtual interface. One instance is constructed per
		// script_create_external() call.
		//
		// LIFETIME INVARIANT: cbs_ is stored BY VALUE (copy), not by pointer.
		// The plan's original suggestion (storing &info.callbacks) was a
		// dangling-pointer bug: when external_types_ grows via push_back,
		// all prior element addresses invalidate. Copying 7 pointers per
		// instance is trivially cheap and eliminates the hazard entirely.
		//
		// Every override null-checks its callback before dispatching (null
		// callbacks are valid — test_null_callbacks_are_noop covers this).
		// try/catch wraps each dispatch because Script.cpp is compiled with
		// -fexceptions (per-file override from Task 2) — a thrown exception
		// from external code is logged to stderr and swallowed, matching
		// the pattern used by script::update() / begin_play / destroy.
		class external_script : public entity_script {
			// === Phase 2b.1: dual user_data channels ===
			// type_user_data_     — from script_register_external (shared across
			//                      all instances of this type; may be NULL).
			// instance_user_data_ — from script_create_external (per-instance;
			//                      NULL falls back to type-level at dispatch time).
			void* type_user_data_;
			void* instance_user_data_;
			script_external_callbacks cbs_;  // BY VALUE
		public:
			external_script(void* type_user_data, void* instance_user_data,
			                const script_external_callbacks& cbs,
			                game_entity::entity e)
				: entity_script(e),
				  type_user_data_(type_user_data),
				  instance_user_data_(instance_user_data),
				  cbs_(cbs) {}

			// Instance-level wins; NULL falls back to type-level. All 7
			// callbacks dispatch through this helper.
			void* effective_user_data() const {
				return instance_user_data_ ? instance_user_data_ : type_user_data_;
			}

			// === Reload support ===
			// process_deferred_reloads_impl reconstructs the instance from
			// these accessors. Both user_data channels must propagate so the
			// new instance behaves like the original.
			void* type_user_data_for_reload() const { return type_user_data_; }
			void* instance_user_data_for_reload() const { return instance_user_data_; }
			const script_external_callbacks& callbacks_for_reload() const { return cbs_; }

			void* reload_state_for_external() const { return _reload_state; }

			void begin_play() override {
				if (!cbs_.begin_play) return;
				try { cbs_.begin_play(effective_user_data()); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "external_script::begin_play: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "external_script::begin_play unknown exception\n");
				}
			}

			void update(float dt) override {
				if (!cbs_.update) return;
				try { cbs_.update(effective_user_data(), dt); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "external_script::update: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "external_script::update unknown exception\n");
				}
			}

			void fixed_update(float dt) override {
				if (!cbs_.fixed_update) return;
				try { cbs_.fixed_update(effective_user_data(), dt); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "external_script::fixed_update: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "external_script::fixed_update unknown exception\n");
				}
			}

			void late_update(float dt) override {
				if (!cbs_.late_update) return;
				try { cbs_.late_update(effective_user_data(), dt); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "external_script::late_update: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "external_script::late_update unknown exception\n");
				}
			}

			void destroy() override {
				if (!cbs_.destroy) return;
				try { cbs_.destroy(effective_user_data()); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "external_script::destroy: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "external_script::destroy unknown exception\n");
				}
			}

			void on_reload(void* old_state) override {
				if (!cbs_.on_reload) return;
				try { cbs_.on_reload(effective_user_data(), old_state); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "external_script::on_reload: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "external_script::on_reload unknown exception\n");
				}
			}

			void reflect(property_reflector& r) override {
				if (!cbs_.reflect) return;
				property_visitor_c visitor;
				visitor.state = &r;
				visitor.property = [](void* state, const char* name, int type, u32 offset) {
					auto* pr = static_cast<property_reflector*>(state);
					pr->property(name, (property_type)type, offset);
				};
				visitor.property_enum = [](void* state, const char* name, u32 offset,
				                            u32 count, const char** names) {
					auto* pr = static_cast<property_reflector*>(state);
					pr->property_enum(name, offset, count, names);
				};
				visitor.property_with_accessor = [](void* state, const char* name, int type,
				                                     void(*g)(void*, void*),
				                                     void(*s)(void*, const void*)) {
					auto* pr = static_cast<property_reflector*>(state);
					pr->property_with_accessor(name, (property_type)type, g, s);
				};
				try { cbs_.reflect(effective_user_data(), visitor); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "external_script::reflect: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "external_script::reflect unknown exception\n");
				}
			}
		};

		// Registered external types. type_id is the index into this vector.
		// The vector only grows (push_back in script_register_external);
		// elements are never moved or removed in Phase 1.
		struct external_type_info {
			const char* name;     // not owned — caller must keep alive
			void* user_data;      // opaque, not owned by engine
			script_external_callbacks callbacks;
		};
		utl::vector<external_type_info> external_types_;

		// === Phase 1 Task 8: MPSC queue state ===
		// Lock-free intrusive linked-list MPSC queue (Bryce Lelbach pattern).
		// Producers atomically swap their node into mpsc_head_ (LIFO chain).
		// Consumer (main thread) exchanges head to nullptr, reverses the chain
		// to restore FIFO order, then executes each callback.
		//
		// in_post_callback_ is thread_local: only the main thread's copy
		// matters (drain only runs on main). Producer threads' copies stay
		// false — they don't accidentally skip their own posts. The assert
		// inside post_to_main_thread guards against a callback itself
		// calling post_to_main_thread re-entrantly on the main thread
		// during drain, which would be a logic error (the callback would
		// be appended to a chain that's being iterated).
		struct mpsc_node {
			mpsc_node* next = nullptr;
			std::function<void()> callback;
		};

		std::atomic<mpsc_node*> mpsc_head_{nullptr};
		thread_local bool in_post_callback_ = false;

		// === Phase 2b.6 Task 2: frame-delayed queue ===
		// DelayedEntry holds both wall-clock and frame-accumulator fire times.
		// Task 2 uses only frame_fire_time (compared against frame_elapsed_time_).
		// wall_fire_time is reserved for Task 3's wall-clock queue.
		struct DelayedEntry {
			std::chrono::steady_clock::time_point wall_fire_time;   // used by wall queue (Task 3)
			float                                 frame_fire_time;   // compared against frame_elapsed_time_
			std::function<void()>                 callback;
		};

		// priority_queue is a MAX-heap by default; we want MIN-fire-time at top.
		struct FrameDelayedCmp {
			bool operator()(const DelayedEntry& a, const DelayedEntry& b) const {
				return a.frame_fire_time > b.frame_fire_time;
			}
		};

		std::priority_queue<DelayedEntry, std::vector<DelayedEntry>, FrameDelayedCmp> frame_delayed_queue_;
		std::mutex                                                                    delayed_mutex_;
		float                                                                         frame_elapsed_time_ = 0.0f;
		u64                                                                           frame_count_      = 0;
		float                                                                         last_frame_dt_    = 0.0f;

		// === Phase 2b.6 Task 3: wall-delayed queue ===
		// Same DelayedEntry struct; wall_fire_time is the active comparator field here.
		struct WallDelayedCmp {
			bool operator()(const DelayedEntry& a, const DelayedEntry& b) const {
				return a.wall_fire_time > b.wall_fire_time;
			}
		};

		std::priority_queue<DelayedEntry, std::vector<DelayedEntry>, WallDelayedCmp> wall_delayed_queue_;

#if USE_TRANSFORM_CACHE_MAP
		transform::component_cache *const get_cache_ptr(const game_entity::entity *const entity)
		{
			assert(game_entity::is_alive((*entity).get_id()));
			const transform::transform_id id{ (*entity).transform().get_id() };

			u32 index{ u32_invalid_id };
			auto pair = cache_map.try_emplace(id, id::invalid_id);

			//  cache_map didn't have an entry for this id, new entity inserted
			if (pair.second)
			{
				index = (u32)transform_cache.size();
				transform_cache.emplace_back();
				transform_cache.back().id = id;
				cache_map[id] = index;
			}
			else
			{
				index = cache_map[id];
			}

			assert(index < transform_cache.size());
			return &transform_cache[index];
		}
#else
		transform::component_cache *const get_cache_ptr(const game_entity::entity *const entity)
		{
			assert(game_entity::is_alive((*entity).get_id()));
			const transform::transform_id id{ (*entity).transform().get_id() };

			for (auto& cache : transform_cache)
			{
				if (cache.id == id)
				{
					return &cache;
				}

				transform_cache.emplace_back();
				transform_cache.back().id = id;

				return &transform_cache.back();
			}
		}
#endif

		// === Phase 1 Task 3 占位:后续 task 填充 ===
		// 这些 impl 函数在 frame_tick 内部按 spec §5.1 顺序被调用。
		// Task 5 已接入 drain_events_impl → script_event_bus::drain()。
		// drain_callbacks_impl 仍为空(Task 8 填充)。
		//
		// drain_events_impl: 把 queued events 投递给匹配订阅者。
		// bus 内部已处理 re-emit isolation + depth cap + deferred subscribe/unsubscribe,
		// 所以 apply_deferred_subscriptions_impl 不需要再做任何事 — drain() 返回后
		// deferred 已被 apply。这里保留 apply_deferred_subscriptions_impl 占位是为
		// 了 spec §5.1 调用顺序的语义清晰(后续 Task 若需要 frame 级 deferred 语义
		// 可在这里加逻辑)。
		void drain_events_impl()              { script_event_bus::instance().drain(); }

		// === Phase 1 Task 8: MPSC drain ===
		// Take the chain atomically, reverse it (producers pushed LIFO via
		// exchange, drain must execute FIFO), then invoke each callback in order.
		// Re-entrancy guard: if drain_callbacks_impl is called from inside a
		// post_to_main_thread callback (main thread during drain), bail out.
		// The callback chain is iterated while in_post_callback_ is true, so
		// a re-entrant post would append to mpsc_head_ — but the drain loop
		// wouldn't see it until the next frame_tick. The assert in
		// post_to_main_thread catches this programming error.
		//
		// Recursive drain (frame_tick called from inside a callback) is also
		// a silent no-op here, NOT an assert. This is intentional: a nested
		// event loop might legitimately re-enter frame_tick (e.g., modal
		// dialog pump). If we asserted here, we'd break that use case.
		// Asymmetric with post's assert — by design.
		void drain_callbacks_impl() {
			if (in_post_callback_) return;

			// Atomically take the entire chain.
			mpsc_node* head = mpsc_head_.exchange(nullptr, std::memory_order_acq_rel);

			// Reverse the chain (pushes were LIFO, drain must be FIFO).
			mpsc_node* reversed = nullptr;
			while (head) {
				mpsc_node* next = head->next;
				head->next = reversed;
				reversed = head;
				head = next;
			}

			// Execute in FIFO order.
			in_post_callback_ = true;
			while (reversed) {
				mpsc_node* next = reversed->next;
				try {
					if (reversed->callback) reversed->callback();
				} catch (const std::exception& e) {
					std::fprintf(stderr, "post_to_main_thread callback exception: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "post_to_main_thread callback unknown exception\n");
				}
				delete reversed;
				reversed = next;
			}

			// === Phase 2b.6 Task 2: drain frame-delayed queue ===
			// Pop entries whose frame_fire_time has been reached. Mutex is
			// released per-iteration so a long callback doesn't block producers.
			// in_post_callback_ stays true — delayed callbacks execute under the
			// same re-entrancy guard as immediate callbacks.
			while (true) {
				std::function<void()> cb;
				{
					std::lock_guard<std::mutex> lock(delayed_mutex_);
					if (frame_delayed_queue_.empty()) break;
					if (frame_delayed_queue_.top().frame_fire_time > frame_elapsed_time_) break;
					// top() returns const&; const_cast is the standard workaround
					// for moving a member out of a priority_queue entry. Safe
					// because the comparator ignores `callback` and we pop()
					// immediately after.
					cb = std::move(const_cast<DelayedEntry&>(frame_delayed_queue_.top()).callback);
					frame_delayed_queue_.pop();
				}
				try { cb(); } catch (const std::exception& e) {
					std::fprintf(stderr, "Exception in delayed callback: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "Unknown exception in delayed callback\n");
				}
			}

			// === Phase 2b.6 Task 3: drain wall-delayed queue ===
			// Pop entries whose wall_fire_time has been reached. Mutex is
			// released per-iteration so a long callback doesn't block producers.
			while (true) {
				std::function<void()> cb;
				{
					std::lock_guard<std::mutex> lock(delayed_mutex_);
					if (wall_delayed_queue_.empty()) break;
					if (wall_delayed_queue_.top().wall_fire_time > std::chrono::steady_clock::now()) break;
					cb = std::move(const_cast<DelayedEntry&>(wall_delayed_queue_.top()).callback);
					wall_delayed_queue_.pop();
				}
				try { cb(); } catch (const std::exception& e) {
					std::fprintf(stderr, "Exception in wall-delayed callback: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "Unknown exception in wall-delayed callback\n");
				}
			}

			in_post_callback_ = false;
		}
		void apply_deferred_subscriptions_impl() { /* Task 5: drain() 内部已处理 deferred */ }

		// === Phase 1 Task 7: deferred reload 实现 ===
		//
		// reload(eid) 推 entity_id 到 deferred_reloads_,frame_tick 末尾调本函数。
		// 流程(spec §4.5 + §5.2):
		//   1. 去重(同帧多次 reload 同 entity 只处理一次,保留首次出现顺序)
		//   2. lookup entity_to_script → script_id → generation 校验 → entity_scripts[index]
		//   3. 只支持 external_script reload(Phase 1 限制,native C++ reload = Phase 2)
		//   4. destroy() 旧实例 — 可调 set_reload_state() 留状态(Phase 1 外部脚本不
		//      能通过 C ABI 设状态,on_reload 收 nullptr)
		//   5. 构造新 external_script(user_data + callbacks 从旧实例复制),swap-in
		//   6. begin_play() 新实例
		//   7. on_reload(old_state) 新实例
		// script_id 不变(spec §4.5 invariant:caller 持有的 component handle 仍 valid)。
		//
		// 同帧 reload + remove:remove 在 frame_tick 之前(由调用方触发)或
		// remove_for_entity 直接调 remove()→swap-erase→清 entity_to_script。
		// 本函数后续 lookup 时 entity_to_script[entity_index] 已无效,自然 skip。
		void process_deferred_reloads_impl() {
			if (deferred_reloads_.empty()) return;

			// Deduplicate preserving first-occurrence order.
			std::unordered_set<u64> seen;
			utl::vector<game_entity::entity_id> unique;
			unique.reserve(deferred_reloads_.size());
			for (auto eid : deferred_reloads_) {
				u64 key = (u64)(id::id_type)eid;
				if (seen.insert(key).second) {
					unique.push_back(eid);
				}
			}
			deferred_reloads_.clear();

			for (auto eid : unique) {
				const id::id_type entity_index = id::index(eid);
				if (entity_index >= entity_to_script.size()) continue;
				const script_id sid = entity_to_script[entity_index];
				if (!id::is_valid(sid)) {
					std::fprintf(stderr,
						"script::reload: entity %u has no script (removed or never created)\n",
						(u32)entity_index);
					continue;
				}

				const id::id_type gen_index = id::index(sid);
				if (gen_index >= generations.size()) continue;
				if (generations[gen_index] != id::generation(sid)) continue;

				const id::id_type scripts_index = id_mapping[gen_index];
				if (scripts_index >= entity_scripts.size()) continue;
				auto& slot = entity_scripts[scripts_index];
				if (!slot) continue;

				// Phase 1: reload only external_script. Native C++ reload = Phase 2.
				auto* ext = dynamic_cast<external_script*>(slot.get());
				if (!ext) {
					std::fprintf(stderr,
						"script::reload: native C++ script reload not supported in Phase 1 (entity_id=%u)\n",
						(u32)entity_index);
					continue;
				}

				// === Phase 2b.3: capture/destroy/recreate flow (fixes UAF) ===
				// Read type-level info + callbacks BEFORE destroy (ext stays
				// valid until slot = std::move(new_ptr) below).
				void* type_ud = ext->type_user_data_for_reload();
				const script_external_callbacks cbs = ext->callbacks_for_reload();
				const u64 entity_id_raw = (u64)eid;
				const u64 script_id_raw = (u64)sid;

				// 1. capture_state_for_reload — backend snapshots pre-destroy state.
				void* captured_state = nullptr;
				if (cbs.capture_state_for_reload) {
					try { captured_state = cbs.capture_state_for_reload(ext->effective_user_data()); }
					catch (const std::exception& e) {
						std::fprintf(stderr, "script::reload capture_state exception: %s\n", e.what());
					} catch (...) {
						std::fprintf(stderr, "script::reload capture_state unknown exception\n");
					}
				}

				// 2. destroy — backend frees old instance_user_data. UAF-safe
				//    because we never read instance_user_data again.
				try { slot->destroy(); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "script::reload destroy exception: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "script::reload destroy unknown exception\n");
				}

				// 3. recreate — backend allocates FRESH instance_user_data
				//    (old pointer is dangling). If hook is NULL, new_inst_ud
				//    stays nullptr (Self-Test Backend path).
				void* new_inst_ud = nullptr;
				if (cbs.recreate_instance_user_data_for_reload) {
					try {
						new_inst_ud = cbs.recreate_instance_user_data_for_reload(
							type_ud, entity_id_raw, script_id_raw);
					}
					catch (const std::exception& e) {
						std::fprintf(stderr, "script::reload recreate exception: %s\n", e.what());
					} catch (...) {
						std::fprintf(stderr, "script::reload recreate unknown exception\n");
					}
				}

				// 4. Construct new external_script. script_id unchanged (spec §4.5).
				auto new_ptr = std::make_unique<external_script>(
					type_ud, new_inst_ud, cbs, game_entity::entity{eid});
				slot = std::move(new_ptr);  // old external_script destructed here

				// 5. begin_play on new instance (spec §5.2: begin_play precedes on_reload)
				try { slot->begin_play(); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "script::reload begin_play exception: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "script::reload begin_play unknown exception\n");
				}

				// 6. on_reload(captured_state) — backend may migrate fields.
				try { slot->on_reload(captured_state); }
				catch (const std::exception& e) {
					std::fprintf(stderr, "script::reload on_reload exception: %s\n", e.what());
				} catch (...) {
					std::fprintf(stderr, "script::reload on_reload unknown exception\n");
				}

				// 7. Free captured state (after on_reload consumed it).
				if (captured_state && cbs.delete_captured_state) {
					try { cbs.delete_captured_state(captured_state); }
					catch (const std::exception& e) {
						std::fprintf(stderr, "script::reload delete_state exception: %s\n", e.what());
					} catch (...) {
						std::fprintf(stderr, "script::reload delete_state unknown exception\n");
					}
				}
			}
		}

		// === Phase 1 Task 6 refactor: shared create() helper ===
		//
		// Both create() overloads funnel through this after constructing the
		// script instance. Handles: id allocation, generation bump, id_mapping,
		// entity_to_script, dense_script_ids, emplace_back, begin_play try/catch.
		component create_script_instance(detail::script_ptr instance,
		                                game_entity::entity entity)
		{
			assert(instance);

			script_id id{};
			if (free_ids.size() > id::min_deleted_elements)
			{
				id = free_ids.front();
				assert(!exists(id));
				free_ids.pop_front();
				id = script_id{ id::new_generation(id) };
				++generations[id::index(id)];
			}
			else
			{
				id = script_id{ (id::id_type)id_mapping.size() };
				id_mapping.emplace_back();
				generations.push_back(0);
			}

			assert(id::is_valid(id));
			const id::id_type index{ (id::id_type)entity_scripts.size() };
			entity_scripts.emplace_back(std::move(instance));
			dense_script_ids.emplace_back(id);
			assert(entity_scripts.back()->get_id() == entity.get_id());
			id_mapping[id::index(id)] = index;

			const id::id_type entity_index{ id::index(entity.get_id()) };
			if (entity_to_script.size() <= entity_index)
			{
				entity_to_script.resize(entity_index + 1);
			}
			entity_to_script[entity_index] = id;

			// === Phase 1 Task 2: begin_play hook ===
			// 必须在 entity_to_script 已建立映射之后调用——脚本可能在 begin_play 里通过
			// entity.script() 反查自己的 component(Task 5 事件总线会用到)。
			// 异常处理:Script.cpp 用 -fexceptions per-file override(见 Engine/CMakeLists.txt),
			// begin_play 抛异常时 catch + log + continue。script 已 emplace 且映射已建,
			// 不因为 hook 失败回滚——返回 valid component,后续 remove() 会触发 destroy。
			try {
				entity_scripts.back()->begin_play();
			} catch (const std::exception& e) {
				std::fprintf(stderr, "script::create begin_play exception: %s\n", e.what());
			} catch (...) {
				std::fprintf(stderr, "script::create begin_play unknown exception\n");
			}

			return component{ id };
		}
	} // anonymous namespace

	namespace detail {
		// 主线程检查基础设施。initialize() 设置 g_main_thread_id 并把 g_initialized 置 true,
		// 之后所有 script API 在 check_main_thread() 中无条件断言当前线程 == g_main_thread_id。
		// 注意:断言是无条件的——任何调用方必须在 script::initialize() 之后才能调用 update 等 API。
		std::thread::id g_main_thread_id{};
		bool g_initialized{ false };

		void check_main_thread() {
			assert(g_initialized);
			assert(std::this_thread::get_id() == g_main_thread_id &&
				   "script API called from non-main thread");
		}

		u8 register_script(size_t tag, script_creator func)
		{
			bool result{ registry().insert(script_registry::value_type{tag, func}).second };
			assert(result);
			return result;
		}

		script_creator get_script_creator(size_t tag)
		{
			auto script = primal::script::registry().find(tag);
			assert(script != primal::script::registry().end() && script->first == tag);
			return script->second;
		}

#ifdef USE_WITH_EDITOR
		u8 add_script_name(const char* name)
		{
			script_names().emplace_back(name);
			return true;
		}
#endif // USE_WITH_EDITOR

	} // namespace detail

	component create(init_info info, game_entity::entity entity)
	{
		detail::check_main_thread();
		assert(entity.is_valid());
		assert(info.script_creator);

		detail::script_ptr instance{ info.script_creator(entity) };
		return create_script_instance(std::move(instance), entity);
	}

	// === Phase 1 Task 6 新增 ===
	// Alternative create() that takes a pre-constructed script instance.
	// Used by script_create_external (C ABI) to inject external_script adapters
	// that carry captured user_data + callbacks. Cannot go through
	// init_info::script_creator because that's a captureless function pointer.
	component create(std::unique_ptr<entity_script> instance,
	                 game_entity::entity entity)
	{
		detail::check_main_thread();
		assert(entity.is_valid());
		assert(instance);

		return create_script_instance(std::move(instance), entity);
	}

	void remove(component c)
	{
		detail::check_main_thread();
		assert(c.is_valid() && exists(c.get_id()));
		const script_id id{ c.get_id() };
		const id::id_type index{ id_mapping[id::index(id)] };

		// === Phase 1 Task 2 新增:destroy hook 在 swap-erase 前调用 ===
		// 给 script 一次清理资源(纹理、句柄、外部 allocations)的机会。
		// 异常处理:Script.cpp 用 -fexceptions,destroy 抛异常时 catch + log,
		// 然后继续 swap-erase,确保 component 总被回收。
		try {
			entity_scripts[index]->destroy();
		} catch (const std::exception& e) {
			std::fprintf(stderr, "script::remove destroy exception: %s\n", e.what());
		} catch (...) {
			std::fprintf(stderr, "script::remove destroy unknown exception\n");
		}

		// I1: Unsubscribe this script from the event bus BEFORE swap-erase.
		// Ordering matters: unsubscribe must happen while the entity_script*
		// is still at entity_scripts[index] (the pointer is valid here).
		// After erase_unordered, the pointer may be swapped to a different
		// index or freed entirely. Without this call, queued deferred events
		// targeting this script would be dispatched to a dead pointer on next
		// drain — use-after-free. See spec §event bus: "emit 中途 destroy —
		// 事件仍在 queue, drain 时 subscriber 已 dead, skip".
		script_event_bus::instance().unsubscribe_all(entity_scripts[index].get());

		const game_entity::entity_id removed_entity{ entity_scripts[index]->get_id() };
		const script_id last_id{ dense_script_ids.back() };
		utl::erase_unordered(entity_scripts, index);
		utl::erase_unordered(dense_script_ids, index);
		id_mapping[id::index(last_id)] = index;
		id_mapping[id::index(id)] = id::invalid_id;

		const id::id_type entity_index{ id::index(removed_entity) };
		if (entity_index < entity_to_script.size())
		{
			entity_to_script[entity_index] = {};
		}
	}

	component get_component_for_entity(game_entity::entity_id eid)
	{
		const id::id_type entity_index{ id::index(eid) };
		if (entity_index < entity_to_script.size())
		{
			return component{ entity_to_script[entity_index] };
		}
		return component{};
	}

	void remove_for_entity(game_entity::entity_id eid)
	{
		const id::id_type entity_index{ id::index(eid) };
		if (entity_index < entity_to_script.size() && id::is_valid(entity_to_script[entity_index]))
		{
			remove(component{ entity_to_script[entity_index] });
		}
	}

	void update(f32 dt)
	{
		detail::check_main_thread();
		for (auto& ptr : entity_scripts)
		{
			try {
				ptr->update(dt);
			} catch (const std::exception& e) {
				std::fprintf(stderr, "script::update exception: %s\n", e.what());
			} catch (...) {
				std::fprintf(stderr, "script::update unknown exception\n");
			}
		}

		if (transform_cache.size())
		{
			transform::update(transform_cache.data(), (u32)transform_cache.size());
			transform_cache.clear();
#if USE_TRANSFORM_CACHE_MAP
			cache_map.clear();
#endif
		}
	}

	// === Phase 1 Task 2 新增 ===
	// fixed_update:物理步 dispatch。不消费 transform_cache(物理步通常独立调度)。
	// 异常处理:Script.cpp 用 -fexceptions,每个 hook 单独 try/catch + log,
	// 一个脚本抛异常不阻止后续脚本被调用。
	void fixed_update(f32 dt)
	{
		detail::check_main_thread();
		for (auto& ptr : entity_scripts)
		{
			try {
				ptr->fixed_update(dt);
			} catch (const std::exception& e) {
				std::fprintf(stderr, "script::fixed_update exception: %s\n", e.what());
			} catch (...) {
				std::fprintf(stderr, "script::fixed_update unknown exception\n");
			}
		}
	}

	// late_update:update() 全部完成后再调(相机跟随、后处理逻辑)。
	void late_update(f32 dt)
	{
		detail::check_main_thread();
		for (auto& ptr : entity_scripts)
		{
			try {
				ptr->late_update(dt);
			} catch (const std::exception& e) {
				std::fprintf(stderr, "script::late_update exception: %s\n", e.what());
			} catch (...) {
				std::fprintf(stderr, "script::late_update unknown exception\n");
			}
		}
	}

	// === Phase 1 Task 3: frame_tick 推荐入口 ===
	// 内部按 spec §5.1 顺序调用颗粒化 API:
	//   drain_callbacks → fixed_update → update → late_update → drain_events
	//   → process_deferred_reloads → apply_deferred_subscriptions
	// 颗粒化 API 的 try/catch 各自独立——一个 hook 抛异常不阻止同帧后续阶段执行。
	// drain_*_impl 当前为空实现(Task 5/7/8 填充),但 frame_tick 仍然按顺序调用它们,
	// 保证后续 task 接入时调用顺序立即可用,无需改 frame_tick 自身。
	void frame_tick(f32 dt)
	{
		detail::check_main_thread();
		{
			std::lock_guard<std::mutex> lock(delayed_mutex_);
			frame_elapsed_time_ += dt;   // advance BEFORE drain so delayed callbacks see accumulated time
			frame_count_ += 1;
			last_frame_dt_ = dt;
		}
		drain_callbacks_impl();
		fixed_update(dt);
		update(dt);
		late_update(dt);
		drain_events_impl();
		process_deferred_reloads_impl();
		apply_deferred_subscriptions_impl();
	}

	// drain_events / drain_callbacks 是颗粒化 API,允许调用方单独触发
	// (例如调试或编辑器模式)。frame_tick 内部也会调用它们。
	// 当前指向空 stub,Task 5/8 接入。
	void drain_events()    { detail::check_main_thread(); drain_events_impl(); }
	void drain_callbacks() { detail::check_main_thread(); drain_callbacks_impl(); }

	// === Phase 1 Task 8: post_to_main_thread ===
	// The ONLY script API callable from any thread (spec §6.2).
	// Does NOT call check_main_thread — that would defeat the purpose.
	// Asserts against re-entrant posting from inside a drain callback
	// (main thread during drain): a callback that posts another callback
	// would append to a chain being iterated, which wouldn't be seen
	// until next frame_tick. The assert catches this logic error.
	void post_to_main_thread(std::function<void()> callback)
	{
		assert(!in_post_callback_ && "post_to_main_thread called from inside drain callback");

		auto* node = new mpsc_node{};
		node->callback = std::move(callback);
		node->next = mpsc_head_.exchange(node, std::memory_order_acq_rel);
	}

	// === Phase 2b.6 Task 2: frame-delayed post ===
	// Enqueues a callback that fires when frame_elapsed_time_ (accumulated at
	// the start of each frame_tick) reaches or exceeds delay_seconds. Drain
	// happens inside drain_callbacks_impl after the immediate MPSC queue.
	void post_to_main_thread_delayed(float delay_seconds, std::function<void()> callback)
	{
		assert(!in_post_callback_);
		// Lock must precede reading frame_elapsed_time_ to avoid racing with frame_tick's write.
		std::lock_guard<std::mutex> lock(delayed_mutex_);
		DelayedEntry entry{};
		entry.frame_fire_time = frame_elapsed_time_ + delay_seconds;
		entry.callback        = std::move(callback);
		frame_delayed_queue_.push(std::move(entry));
	}

	// === Phase 2b.6 Task 3: wall-delayed post ===
	// Enqueues a callback that fires when steady_clock (wall time) elapses
	// delay_seconds. Immune to system clock adjustments. Drain happens inside
	// drain_callbacks_impl after the frame-delayed queue.
	void post_to_main_thread_delayed_wall(float delay_seconds, std::function<void()> callback)
	{
		assert(!in_post_callback_);
		std::lock_guard<std::mutex> lock(delayed_mutex_);
		DelayedEntry entry{};
		entry.wall_fire_time = std::chrono::steady_clock::now()
		                     + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
		                           std::chrono::duration<float>(delay_seconds));
		entry.callback       = std::move(callback);
		wall_delayed_queue_.push(std::move(entry));
	}

	// === Phase 1 Task 7: 热重载单个 entity 的 script ===
	// 把 entity_id 推入 deferred_reloads_ 队列,frame_tick 末尾的
	// process_deferred_reloads_impl 统一处理(去重 + 按序)。设计见 spec §4.5。
	void reload(game_entity::entity_id eid)
	{
		detail::check_main_thread();
		deferred_reloads_.push_back(eid);
	}

	void initialize()
	{
		// Phase 2b.8: initialize the shared script state store.
		ScriptState::instance().initialize();
		// 允许重复 initialize:覆盖主线程 id(通常不变)并重新打开 g_initialized。
		// 若已初始化,这是幂等操作。
		detail::g_main_thread_id = std::this_thread::get_id();
		detail::g_initialized = true;

		// Phase 2b.8 Task 3: register built-in C++-owned state.engine.* getters.
		// These read file-scope anonymous-namespace globals only (no ScriptState re-entrancy).
		auto& s = ScriptState::instance();
		s.register_engine_getter("frame", []() {
			return StateValue::make_number(static_cast<double>(frame_count_));
		});
		s.register_engine_getter("time", []() {
			return StateValue::make_number(static_cast<double>(frame_elapsed_time_));
		});
		s.register_engine_getter("dt", []() {
			return StateValue::make_number(static_cast<double>(last_frame_dt_));
		});
	}

	void shutdown()
	{
		// Phase 2b.8: shut down the shared script state store before clearing
		// g_initialized so any script code running during teardown can still
		// safely access ScriptState (which has its own initialized flag).
		ScriptState::instance().shutdown();
		// 关闭后 check_main_thread 的 g_initialized 断言会失败——任何 shutdown 后的
		// script API 调用都被视为编程错误。event_bus / 注册表等子系统清理在后续 task 加入。
		detail::g_initialized = false;

		// Clear external_types_ because it stores caller-owned raw pointers
		// (name, user_data). Unlike entity_scripts / id_mapping (internal
		// engine state), these point into the embedder's memory. After
		// shutdown() → initialize(), stale pointers would dangle. Embedders
		// that reinitialize (editors, test harnesses, hot-reload) must
		// re-register all external types.
		external_types_.clear();

		// Clear deferred reloads so a shutdown→initialize cycle doesn't carry
		// over stale entity_ids from a prior session (tests rely on this).
		deferred_reloads_.clear();

		// Clear script instance state. Without this, a shutdown→initialize
		// cycle leaves stale external_script entries in entity_scripts whose
		// instance_user_data points to backend-freed memory (e.g. LuaBackend
		// drops its LuaScriptInstance unique_ptrs in its own shutdown). The
		// next session's frame_tick() would dispatch update() on those stale
		// entries and dereference dangling pointers. Clearing here matches
		// external_types_ semantics above: embedders that reinitialize
		// (editors, test harnesses, hot-reload) must re-register everything.
		entity_scripts.clear();
		dense_script_ids.clear();
		id_mapping.clear();
		entity_to_script.clear();
		generations.clear();

		// === Phase 1 Task 8: drop pending MPSC callbacks ===
		// Take the chain, reverse it, and delete every node WITHOUT executing
		// the callback. Shutdown may be called during teardown when script
		// state is no longer safe to access — executing callbacks here would
		// risk use-after-free on captured state. Callers that need callbacks
		// to run must drain before shutdown (call frame_tick(0) once).
		mpsc_node* head = mpsc_head_.exchange(nullptr, std::memory_order_acq_rel);
		while (head) {
			mpsc_node* next = head->next;
			delete head;
			head = next;
		}
		// Detect producer race: if a producer thread was between `new mpsc_node`
		// and `mpsc_head_.exchange(...)` during the shutdown walk above, its
		// node is now in mpsc_head_ but won't be drained (we already walked).
		// Log + clean up best-effort.
		mpsc_node* raced = mpsc_head_.exchange(nullptr, std::memory_order_acq_rel);
		if (raced) {
			std::fprintf(stderr,
				"script::shutdown: producer thread posted after shutdown began; "
				"callbacks dropped (caller must join producers before shutdown)\n");
			while (raced) {
				mpsc_node* n = raced->next;
				delete raced;
				raced = n;
			}
		}

		// === Phase 2b.6 Task 2: clear frame-delayed queue ===
		// Drop pending delayed callbacks without executing (same safety rationale
		// as MPSC cleanup above). Reset accumulator so next session starts clean.
		{
			std::lock_guard<std::mutex> lock(delayed_mutex_);
			while (!frame_delayed_queue_.empty()) frame_delayed_queue_.pop();
			while (!wall_delayed_queue_.empty())  wall_delayed_queue_.pop();
		}
		frame_elapsed_time_ = 0.0f;
		frame_count_ = 0;
		last_frame_dt_ = 0.0f;
	}

	bool is_initialized()
	{
		// No check_main_thread() — safety-net callers (LuaBackend::shutdown)
		// may invoke this during partial teardown when the main-thread
		// invariant no longer holds.
		return detail::g_initialized;
	}

	void entity_script::set_rotation(const game_entity::entity *const entity, math::v4 rotation_quaternion)
	{
		transform::component_cache& cache{ *get_cache_ptr(entity) };
		cache.flags |= transform::component_flags::rotation;
		cache.rotation = rotation_quaternion;
	}

	void entity_script::set_orientation(const game_entity::entity *const entity, math::v3 orientation_vector)
	{
		transform::component_cache& cache{ *get_cache_ptr(entity) };
		cache.flags |= transform::component_flags::orientation;
		cache.orientation = orientation_vector;
	}

	void entity_script::set_position(const game_entity::entity *const entity, math::v3 position)
	{
		transform::component_cache& cache{ *get_cache_ptr(entity) };
		cache.flags |= transform::component_flags::position;
		cache.position = position;
	}

	void entity_script::set_scale(const game_entity::entity *const entity, math::v3 scale)
	{
		transform::component_cache& cache{ *get_cache_ptr(entity) };
		cache.flags |= transform::component_flags::scale;
		cache.scale = scale;
	}

} // namespace primal::script

#ifdef USE_WITH_EDITOR
#include <atlsafe.h>

extern "C" __declspec(dllexport)
LPSAFEARRAY get_script_names()
{
	const u32 size{ (u32)primal::script::script_names().size() };
	if (!size) return nullptr;
	CComSafeArray<BSTR> names(size);
	for (u32 i{ 0 }; i < size; ++i)
	{
		names.SetAt(i, A2BSTR_EX(primal::script::script_names()[i].c_str()), false);
	}
	return names.Detach();
}
#endif // USE_WITH_EDITOR

// === Phase 1 Task 6: register_external C ABI ===
//
// extern "C" implementations. These are file-scope functions (not in
// primal::script namespace) because the C ABI requires unmangled names.
//
// They access anonymous-namespace state (external_types_, external_script)
// via fully-qualified path primal::script::external_types_ — anonymous
// namespace members are accessible through the enclosing namespace within
// the same translation unit.
//
// main-thread check: both functions call detail::check_main_thread() after
// basic arg validation. This catches accidental off-main-thread calls in
// debug builds (assert). In release builds the check is a no-op — the
// caller is responsible for thread safety.

extern "C" {

u64 script_register_external(
    const char* type_name,
    void* user_data,
    const script_external_callbacks* callbacks)
{
    // Arg validation before main-thread check so a NULL callbacks pointer
    // or invalid type_name fails cleanly even if script::initialize() was
    // never called.
    if (!type_name || !type_name[0] || !callbacks) {
        std::fprintf(stderr, "script_register_external: invalid args\n");
        return u64_invalid_id;
    }
    primal::script::detail::check_main_thread();

    const u64 type_id = primal::script::external_types_.size();
    primal::script::external_type_info info{};
    info.name = type_name;
    info.user_data = user_data;
    info.callbacks = *callbacks;  // copy struct by value
    primal::script::external_types_.push_back(info);

    return type_id;
}

u64 script_create_external(u64 type_id, u64 entity_id, void* instance_user_data)
{
    // Arg validation first — same ordering as script_register_external.
    // A bogus type_id fails cleanly even if script::initialize() was never called.
    if (type_id >= primal::script::external_types_.size()) {
        std::fprintf(stderr, "script_create_external: type_id out of range\n");
        return u64_invalid_id;
    }
    primal::script::detail::check_main_thread();

    // Validate entity
    const primal::game_entity::entity_id eid{ static_cast<primal::id::id_type>(entity_id) };
    if (!primal::game_entity::is_alive(eid)) {
        std::fprintf(stderr, "script_create_external: entity %llu is not alive\n",
                     (unsigned long long)entity_id);
        return u64_invalid_id;
    }

    const auto& info = primal::script::external_types_[type_id];
    primal::game_entity::entity entity{ eid };

    // Construct the adapter. Both user_data channels propagated: type-level
    // from registration, instance-level from this call. Callbacks dispatch
    // via external_script::effective_user_data() (instance wins, NULL→type).
    auto instance = std::make_unique<primal::script::external_script>(
        info.user_data, instance_user_data, info.callbacks, entity);

    primal::script::component sc = primal::script::create(std::move(instance), entity);
    return static_cast<u64>(sc.get_id());
}

// === Phase 2b.2: Event Bus C ABI implementations ===
// Bridge C ABI calls to script_event_bus singleton. Each handler is wrapped
// in a std::function that forwards to the C function pointer.

u64 script_event_subscribe(
    const char* event_name,
    void* user_data,
    script_event_handler_t handler)
{
    if (!event_name || !event_name[0] || !handler) {
        std::fprintf(stderr, "script_event_subscribe: invalid args\n");
        return 0;
    }
    primal::script::detail::check_main_thread();

    auto dispatcher = [handler](void* ud, const void* payload, u64 size) {
        handler(ud, payload, size);
    };
    return primal::script::script_event_bus::instance().subscribe_by_name(
        event_name, user_data, std::move(dispatcher));
}

void script_event_unsubscribe(u64 subscription_id) {
    if (subscription_id == 0) return;
    primal::script::detail::check_main_thread();
    primal::script::script_event_bus::instance().unsubscribe(subscription_id);
}

void script_event_unsubscribe_all(void* user_data) {
    primal::script::detail::check_main_thread();
    primal::script::script_event_bus::instance().unsubscribe_all_by_data(user_data);
}

void script_event_emit(
    const char* event_name,
    void* payload,
    u64 payload_size,
    void (*deleter)(void*))
{
    if (!event_name || !event_name[0]) {
        std::fprintf(stderr, "script_event_emit: invalid event_name\n");
        if (deleter && payload) deleter(payload);
        return;
    }
    primal::script::detail::check_main_thread();
    primal::script::script_event_bus::instance().emit_by_name(
        event_name, payload, payload_size, deleter);
}

} // extern "C"
