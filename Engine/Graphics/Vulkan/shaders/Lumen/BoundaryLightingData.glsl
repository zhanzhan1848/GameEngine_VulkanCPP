#ifndef SC_BOUNDARY_LIGHT_DATA
#define SC_BOUNDARY_LIGHT_DATA
struct SCBoundaryLightPoint {
    vec4 worldFootprint;
    vec4 normal;
    vec4 albedo;
    uvec4 identity; // card, original TriangleID, allocation epoch, frame
};
struct SCBoundaryLightResult {
    vec4 radiance; // A: finite diagnostic evaluation, not production cache validity
    vec4 irradiance; // RGB: bootstrap-scaled DDGI; A: in-volume
    vec4 directVisibility;
    uvec4 identity;
};
bool scBoundaryLightFinite(vec3 v){return !any(isnan(v)) && !any(isinf(v));}
#endif
