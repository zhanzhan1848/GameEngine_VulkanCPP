// === Phase 1 Task 9: Self-Test Backend (Pure C) ===
//
// This file is the keystone validation of the script C ABI. It MUST compile
// with a plain C compiler (cc -x c, no C++ frontend). If it does, the entire
// Phase 1 C ABI story holds: any language that can produce C-compatible
// symbols (Lua FFI, Python ctypes, C# P/Invoke, Rust, mods) can drive the
// engine's script subsystem end-to-end.
//
// What this file proves:
//   1. ScriptExternal.h is includable from pure C (no C++ constructs leak).
//   2. PrimitiveTypes.h C branch (u64 = uint64_t) is usable.
//   3. script_register_external + script_create_external are callable from C.
//   4. All 7 lifecycle/reflection callbacks have C-compatible signatures.
//   5. property_visitor_c can be constructed and forwarded from C.
//
// Layout:
//   - 7 file-static counters (one per callback)
//   - last_user_data capture (to verify the engine forwards the pointer)
//   - C accessor functions (the C++ test driver reads these)
//   - self_test_register_and_create: one-shot C entry point used by the driver
//   - self_test_invoke_reflect: exposes the internal reflect callback so the
//     C++ driver can verify declared properties without a public reflect() API
//   - self_test_reset_counters: called at the start of each test for isolation
//
// No C++ constructs are used. No // C++ comments contain C++ code. The file
// is plain C99.

#include "Components/ScriptExternal.h"

#include <stdint.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// File-static counters. The C++ driver reads these via the accessors below.
// They accumulate across tests within a single binary run; each test calls
// self_test_reset_counters() at start to get a clean slate.
// ---------------------------------------------------------------------------
static int begin_play_count    = 0;
static int update_count        = 0;
static int fixed_update_count  = 0;
static int late_update_count   = 0;
static int destroy_count       = 0;
static int on_reload_count     = 0;
static int reflect_count       = 0;

// user_data pointer captured by every callback. Used to verify the engine
// forwards the same pointer that was passed to script_register_external.
static void* last_user_data    = NULL;

// Marker value that the C++ driver will recognize. static so the address
// remains stable for the lifetime of the binary.
static int user_data_marker    = 42;

// ---------------------------------------------------------------------------
// Accessors — the C++ driver imports these via extern "C".
// ---------------------------------------------------------------------------
int self_test_get_begin_play_count(void)    { return begin_play_count; }
int self_test_get_update_count(void)        { return update_count; }
int self_test_get_fixed_update_count(void)  { return fixed_update_count; }
int self_test_get_late_update_count(void)   { return late_update_count; }
int self_test_get_destroy_count(void)       { return destroy_count; }
int self_test_get_on_reload_count(void)     { return on_reload_count; }
int self_test_get_reflect_count(void)       { return reflect_count; }
void* self_test_get_last_user_data(void)    { return last_user_data; }

// Returns the address of the internal marker so the C++ driver can compare
// it against last_user_data (pointer-identity check).
void* self_test_get_marker_address(void)    { return (void*)&user_data_marker; }

// Resets all counters. Called by the C++ driver at the start of each test
// so the file-static state doesn't leak across tests.
void self_test_reset_counters(void) {
    begin_play_count   = 0;
    update_count       = 0;
    fixed_update_count = 0;
    late_update_count  = 0;
    destroy_count      = 0;
    on_reload_count    = 0;
    reflect_count      = 0;
    last_user_data     = NULL;
}

// ---------------------------------------------------------------------------
// Lifecycle callbacks. Each increments its counter and captures user_data.
// ---------------------------------------------------------------------------

static void my_begin_play(void* user_data) {
    ++begin_play_count;
    last_user_data = user_data;
}

static void my_update(void* user_data, float dt) {
    (void)dt;  // dt verified by the C++ driver via counters, not value check
    ++update_count;
    last_user_data = user_data;
}

static void my_fixed_update(void* user_data, float dt) {
    (void)dt;
    ++fixed_update_count;
    last_user_data = user_data;
}

static void my_late_update(void* user_data, float dt) {
    (void)dt;
    ++late_update_count;
    last_user_data = user_data;
}

static void my_destroy(void* user_data) {
    ++destroy_count;
    last_user_data = user_data;
}

static void my_on_reload(void* user_data, void* old_state) {
    (void)old_state;  // Phase 1 external backend has no per-instance state
    ++on_reload_count;
    last_user_data = user_data;
}

// Reflect callback: declares 2 properties via the C visitor.
//   - "speed": float32 at byte offset 0
//   - "count": int32 at byte offset 4
// The C++ driver builds a property_visitor_c that forwards into a
// property_collector, then calls self_test_invoke_reflect(visitor) to
// drive this callback and verifies the collector received both descriptors.
static void my_reflect(void* user_data, property_visitor_c visitor) {
    (void)user_data;  // no per-type state for this self-test
    ++reflect_count;

    if (visitor.property) {
        visitor.property(visitor.state, "speed", SCRIPT_PROPERTY_TYPE_FLOAT32, 0);
        visitor.property(visitor.state, "count", SCRIPT_PROPERTY_TYPE_INT32, 4);
    }
}

// Expose the internal my_reflect so the C++ driver can invoke it directly
// with a manually-constructed property_visitor_c. my_reflect itself is
// static, so this wrapper is the only way for the driver to reach it.
void self_test_invoke_reflect(property_visitor_c visitor) {
    my_reflect(NULL, visitor);
}

// ---------------------------------------------------------------------------
// One-shot entry point: register the self-test type and create an instance
// bound to the given entity.
//
// Returns 1 on success, 0 on failure. On success, writes the type_id and
// script_handle via the out-params. Using a separate int return avoids the
// ambiguity that type_id == 0 is a legitimate success value
// (script_register_external uses [0, external_type_count) as the valid range).
// ---------------------------------------------------------------------------
int self_test_register_and_create(u64 entity_id_raw,
                                  u64* type_id_out,
                                  u64* script_handle_out) {
    script_external_callbacks cbs = {0};
    cbs.begin_play   = &my_begin_play;
    cbs.update       = &my_update;
    cbs.fixed_update = &my_fixed_update;
    cbs.late_update  = &my_late_update;
    cbs.destroy      = &my_destroy;
    cbs.on_reload    = &my_on_reload;
    cbs.reflect      = &my_reflect;

    u64 type_id = script_register_external("self_test_c",
                                           (void*)&user_data_marker, &cbs);
    if (type_id == UINT64_MAX) {
        return 0;  // register failed
    }

    u64 script_handle = script_create_external(type_id, entity_id_raw);
    if (script_handle == UINT64_MAX) {
        return 0;  // create failed
    }

    if (type_id_out) {
        *type_id_out = type_id;
    }
    if (script_handle_out) {
        *script_handle_out = script_handle;
    }
    return 1;  // success
}
