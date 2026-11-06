// 负例 2：浮点向量下标 v[1] —— XMFLOAT3A 无 operator[]，门禁必须报数学相关诊断。
#include "mini_contract.h"
using namespace primal::math;
float bad(const v3& v) {
    return v[1];
}
