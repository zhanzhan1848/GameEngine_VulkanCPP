#ifndef DDGI_FIRST_STRICT_MATH_GLSL
#define DDGI_FIRST_STRICT_MATH_GLSL
// Private first-record diagnostic only. The strict SPIR-V build decorates
// every core floating arithmetic result NoContraction and requests RTE32 +
// SignedZeroInfNanPreserve32. Keep these scalar evaluation trees explicit;
// the independent reader models core RTE and InverseSqrt's 2 ULP bound.
float ddgiStrictDot(vec3 a, vec3 b) {
    float p0 = a.x * b.x;
    float p1 = a.y * b.y;
    float p2 = a.z * b.z;
    float s0 = p0 + p1;
    return s0 + p2;
}
vec3 ddgiStrictCross(vec3 a, vec3 b) {
    return vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}
float ddgiStrictSqrt(float x) {
    if (x == 0.0) return 0.0;
    return x * inversesqrt(x);
}
float ddgiStrictLength(vec3 x) {
    return ddgiStrictSqrt(ddgiStrictDot(x, x));
}
vec3 ddgiStrictNormalize(vec3 x) {
    float squared = ddgiStrictDot(x, x);
    float inv = inversesqrt(squared);
    return x * inv;
}
#endif
