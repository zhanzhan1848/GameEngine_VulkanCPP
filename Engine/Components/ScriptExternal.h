// === Phase 1 Task 6: register_external C ABI ===
//
// Pure C header. Allows external backends (Lua/Python/C#, mods, plugins) to
// register script types and create instances without any C++ knowledge.
//
// Compileable as both C (-x c) and C++ (extern "C").
// The adapter that bridges these callbacks to entity_script lives in
// Script.cpp (anonymous namespace external_script).

#pragma once

// PrimitiveTypes.h defines u32 / u64 as uint32_t / uint64_t via <cstdint>.
// It is safe to include from C because it only uses typedef aliases —
// no C++ constructs.
#include "Common/PrimitiveTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Property type constants — must match ScriptProperty.h's property_type enum.
//   boolean    = 0, int32 = 1, float32 = 2, float3 = 3, float4 = 4,
//   quaternion = 5, string = 6, asset_ref = 7, enum_ = 8, custom = 9
// Keep in sync with primal::script::property_type.
// ---------------------------------------------------------------------------
#define SCRIPT_PROPERTY_TYPE_BOOLEAN    0
#define SCRIPT_PROPERTY_TYPE_INT32      1
#define SCRIPT_PROPERTY_TYPE_FLOAT32    2
#define SCRIPT_PROPERTY_TYPE_FLOAT3     3
#define SCRIPT_PROPERTY_TYPE_FLOAT4     4
#define SCRIPT_PROPERTY_TYPE_QUATERNION 5
#define SCRIPT_PROPERTY_TYPE_STRING     6
#define SCRIPT_PROPERTY_TYPE_ASSET_REF  7
#define SCRIPT_PROPERTY_TYPE_ENUM       8
#define SCRIPT_PROPERTY_TYPE_CUSTOM     9

// ---------------------------------------------------------------------------
// C visitor for property reflection.
//
// The external backend's reflect callback receives this struct and calls
// one of its function-pointer members per declared property. The function
// pointers forward into the engine's property_reflector (C++ side, see
// Script.cpp external_script::reflect).
//
// `state` is an opaque pointer set by the engine before invoking reflect.
// External code must pass it back unmodified as the first argument to each
// function pointer.
// ---------------------------------------------------------------------------
typedef struct property_visitor_c {
    void* state;

    // Declare a simple property at a known byte offset within user_data.
    void (*property)(void* state, const char* name, int type, u32 offset);

    // Declare an enum property: offset + array of enum value names.
    void (*property_enum)(void* state, const char* name, u32 offset,
                          u32 count, const char** names);

    // Declare a property accessed via getter/setter function pointers
    // instead of byte offset (e.g. computed properties).
    void (*property_with_accessor)(void* state, const char* name, int type,
                                   void(*getter)(void* instance, void* out),
                                   void(*setter)(void* instance, const void* in));
} property_visitor_c;

// ---------------------------------------------------------------------------
// Lifecycle + reflection callbacks for an external script type.
//
// Every field is optional (may be NULL). The adapter null-checks before
// each dispatch. user_data is passed verbatim to every callback — it is
// the external backend's responsibility to manage its lifetime.
//
// reflect receives a property_visitor_c by value. This is 4 pointers (32
// bytes on 64-bit) — cheap to copy, avoids requiring the backend to keep
// a stable visitor address.
// ---------------------------------------------------------------------------
typedef struct script_external_callbacks {
    void (*begin_play)(void* user_data);
    void (*update)(void* user_data, float dt);
    void (*fixed_update)(void* user_data, float dt);
    void (*late_update)(void* user_data, float dt);
    void (*destroy)(void* user_data);
    void (*on_reload)(void* user_data, void* old_state);
    void (*reflect)(void* user_data, property_visitor_c visitor);
} script_external_callbacks;

// ---------------------------------------------------------------------------
// Register an external script type.
//
// type_name:  human-readable name (for debugging/editor). Need not be unique
//             in Phase 1, but should be for future hot-reload.
// user_data:  opaque pointer passed to every callback. May be NULL if the
//             callbacks don't need per-type state.
// callbacks:  must be non-NULL. Individual function pointers inside may be NULL.
//
// Returns type_id in range [0, external_type_count) on success.
// Returns (u64)-1 == UINT64_MAX on failure
// (callbacks is NULL, or called from non-main thread in debug builds).
// ---------------------------------------------------------------------------
u64 script_register_external(
    const char* type_name,
    void* user_data,
    const script_external_callbacks* callbacks
);

// ---------------------------------------------------------------------------
// Create an instance of a previously-registered external type, attached to
// the given entity_id.
//
// type_id:             value returned by script_register_external.
// entity_id:           the game_entity::entity_id (u64) the script should bind to.
// instance_user_data:  opaque pointer stored on this instance. Dispatch prefers
//                      this over the type-level user_data when non-NULL. May be
//                      NULL — callbacks then receive the type-level pointer
//                      (or NULL if that is also NULL).
//
// Returns script_id as u64 on success. Returns (u64)-1 on failure
// (invalid type_id, invalid entity, or non-main thread in debug builds).
// ---------------------------------------------------------------------------
u64 script_create_external(u64 type_id, u64 entity_id, void* instance_user_data);

// ---------------------------------------------------------------------------
// Phase 2b.2: String-keyed Event Bus C ABI
//
// Allows external backends (Lua/Python/C#) to subscribe/emit events through
// the engine's script_event_bus. Events are keyed by string name (FNV-1a
// hashed internally); payload is opaque bytes the engine does not interpret.
//
// Each subscription has a `user_data` pointer that serves two roles:
//   1. Passed back to handler on each dispatch (handler state / capture)
//   2. Identity for script_event_unsubscribe_all bulk cleanup
//
// Templated C++ subscribe<E>/emit<E> API (ScriptEventBus.h) is unaffected —
// both channels share the same drain queue (FIFO + re-emit isolation +
// depth cap 8) but use separate subscriber buckets.
// ---------------------------------------------------------------------------

typedef void (*script_event_handler_t)(
    void* user_data,
    const void* payload,
    u64 payload_size
);

// Subscribe to a string-keyed event. Returns subscription_id (never 0 on
// success; 0 on failure — null event_name or null handler).
u64 script_event_subscribe(
    const char* event_name,
    void* user_data,
    script_event_handler_t handler
);

// Unsubscribe by id. No-op if unknown id (allows double-unsubscribe).
void script_event_unsubscribe(u64 subscription_id);

// Remove all subscriptions whose user_data matches. Searches only the
// string-keyed channel. O(N) over all buckets.
void script_event_unsubscribe_all(void* user_data);

// Emit a string-keyed event. Engine takes ownership of payload; deleter is
// called after dispatch in drain(). payload_size bytes are not interpreted.
// May be called during drain (event goes to re_emitted_ queue).
void script_event_emit(
    const char* event_name,
    void* payload,
    u64 payload_size,
    void (*deleter)(void* payload)
);

#ifdef __cplusplus
} // extern "C"
#endif
