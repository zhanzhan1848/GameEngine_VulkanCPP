#pragma once
#if !defined(__cplusplus)
#error Do not include this header directly in shader files. Only include this file via Common.hlsli
#endif

constant float PI = 3.1415926535897932384626433832795f;

// Light types
// NORE: these to be the same as primal::graphics::light::type enumeration!
constant uint LIGHT_TYPE_DIRECTIONAL_LIGHT = 0;
constant uint LIGHT_TYPE_POINT_LIGHT = 1;
constant uint LIGHT_TYPE_SPOT_LIGHT = 2;