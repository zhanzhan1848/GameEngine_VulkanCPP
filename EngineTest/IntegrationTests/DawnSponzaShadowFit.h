#pragma once
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include <vector>

namespace primal::tests::dawn_sponza {
using namespace graphics::rhi;
namespace smath = graphics::rhi::math;
struct ShadowParameters {
    smath::m4x4 inverseViewProjection, lightVP[4];
    smath::v4 lightDirection;
    smath::v2 texelSize, depthTexelSize;
    u32 quality=2, width=0, height=0;
    float sunTanHalfAngle=0;
    smath::v4 cascade[4];
};
static_assert(sizeof(ShadowParameters)==432 && offsetof(ShadowParameters,lightDirection)==320);
static_assert(offsetof(ShadowParameters,width)==356 && offsetof(ShadowParameters,cascade)==368);
struct ShadowBounds { smath::v3 center; float radius; };

// Input direction is normalized by the scene host before the module fit.
void FitShadowCascades(ShadowParameters& p,const std::vector<ShadowBounds>& bounds,u32 mapSize);
}
