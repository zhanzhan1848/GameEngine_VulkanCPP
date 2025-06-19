// Checker
// p：二维位置 (float2)。dpdx 和 dpdy：位置关于 x 和 y 的导数 (float2)，用于抗锯齿。
float checkersGradBox(float2 p, float2 dpdx, float2 dpdy) {
    // 滤波核宽度
    float2 w = abs(dpdx) + abs(dpdy) + 0.001;

    // 解析积分（盒滤波）
    float2 i = 2.0 * (abs(fract((p - 0.5 * w) * 0.5) - 0.5) - 
                      abs(fract((p + 0.5 * w) * 0.5) - 0.5)) / w;

    // xor 模式
    return 0.5 - 0.5 * i.x * i.y;
}