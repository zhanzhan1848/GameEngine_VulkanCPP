// 负例 1：旧 simd 写法 m.columns —— 门禁必须报数学相关诊断。
#include "mini_contract.h"
using namespace primal::math;
float bad(m4x4& m) {
    return m.columns[0][0];  // XMFLOAT4X4A 无 columns 成员
}
