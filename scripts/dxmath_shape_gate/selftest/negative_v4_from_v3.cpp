// 负例 3：v4{v3, w} 组合构造 —— XMFLOAT4A 无此构造，门禁必须报数学相关诊断。
#include "mini_contract.h"
using namespace primal::math;
v4 bad(const v3& v) {
    return v4{ v, 1.f };
}
