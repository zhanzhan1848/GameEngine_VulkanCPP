// The vertex stream stores normal.xy as two 16-bit UNORM values.
// The sign in the separate packed tangent/color attribute is handled by the pass.
float3 UnpackNormal(uint2 packedXY)
{
    float2 xy = float2(packedXY) * (2.0 / 65535.0) - 1.0;
    float xyLength = dot(xy, xy);
    if (xyLength > 1.0) return float3(0, 0, 1);
    return float3(xy, sqrt(max(0.0, 1.0 - xyLength)));
}
