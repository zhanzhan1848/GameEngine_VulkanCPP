
// Basic
float length2(float3 v) {
    return dot(v, v);  // 返回向量的长度平方
}

float dot2(float2 v) {
    return length_squared(v);  // 推荐使用内置函数
    // 或 return dot(v, v);     // 等价替代
}

// dot2 for float3
float dot2(float3 v) {
    return length_squared(v);  // 推荐使用内置函数
    // 或 return dot(v, v);     // 等价替代
}

// ndot for float2
float ndot(float2 a, float2 b) {
    return a.x * b.x - a.y * b.y;  // 无内置替代，直接实现
}

float2 OPU( float2 d1, float2 d2 )
{
	return (d1.x < d2.x) ? d1 : d2;
}


// SDF
