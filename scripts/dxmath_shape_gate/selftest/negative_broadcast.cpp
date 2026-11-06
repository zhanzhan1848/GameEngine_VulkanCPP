// 负例 4：标量广播构造 v3{1.f} —— XMFLOAT3A 无单标量构造，门禁必须报数学相关诊断。
#include "mini_contract.h"
using namespace primal::math;
v3 bad() {
    return v3{ 1.f };
}
