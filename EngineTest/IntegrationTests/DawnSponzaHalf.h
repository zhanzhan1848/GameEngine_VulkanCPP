#pragma once

namespace primal::tests::dawn_sponza {
#ifdef __EMSCRIPTEN__
// WASM Clang provides binary16 storage and conversion through __fp16.
using HalfStorage = __fp16;
#else
using HalfStorage = _Float16;
#endif
static_assert(sizeof(HalfStorage) == 2);
}
