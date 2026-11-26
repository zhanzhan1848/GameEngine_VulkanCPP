uint packHalf2(float2 value)
{
    uint2 packed = f32tof16(value);
    return packed.x | (packed.y << 16);
}

float2 unpackHalf2(uint value)
{
    return f16tof32(uint2(value & 0xffffu, value >> 16));
}
