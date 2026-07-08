#pragma once


#ifndef __cplusplus
// C compilation: use stdint.h and typedef (C has no using-alias and no <cstdint>)
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define U64_INVALID_ID  ((uint64_t)0xffffffffffffffffull)
#define U32_INVALID_ID  ((uint32_t)0xffffffffu)
#define U16_INVALID_ID  ((uint16_t)0xffffu)
#define U8_INVALID_ID   ((uint8_t)0xffu)

typedef float f32;

#else // __cplusplus

#include <cstdint>

// unsigned intergers

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// signed intergers

using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

constexpr u64 u64_invalid_id{ 0xffff'ffff'ffff'ffff }; // = -1
constexpr u32 u32_invalid_id{ 0xffff'ffff }; // = -1
constexpr u16 u16_invalid_id{ 0xffff }; // = -1
constexpr u8 u8_invalid_id{ 0xff }; // = -1

using f32 = float;

#endif // __cplusplus