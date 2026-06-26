#pragma once
#include <cstdint>

using u32 = uint32_t;
using f32 = float;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PipelineLightDesc {
    u32   type;          // 0=directional, 1=point, 2=spot
    f32   color[3];
    f32   intensity;
    f32   position[3];
    f32   direction[3];  // directional/spot; point ignores
    f32   range;         // point/spot; directional ignores
    f32   umbra;         // spot only, radians [0, pi)
    f32   penumbra;      // spot only, radians [umbra, pi)
    u32   is_enabled;
} PipelineLightDesc;

#ifdef __cplusplus
}
#endif
