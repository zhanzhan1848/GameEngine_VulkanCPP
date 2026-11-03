/**
 * @file TestD3D12RootSigBudget.cpp
 * @brief Root signature 64-DWORD 预算器单测
 * @details D3D12 M0（F0-5 验收）：典型布局计数值 + 64-DWORD 超限显式失败
 *          + RHI DescriptorSetLayoutDesc 换算。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/D3D12/D3D12RootSignatureBudget.h"

using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::d3d12;
using namespace Engine::Test;

TestResult TestRootParameterCosts() {
    // 表=1；root descriptor=2；root constants 向上取整
    D3D12RootParameterDesc table;
    table.kind = D3D12RootParameterDesc::Kind::DescriptorTable;
    table.descriptorCount = 1000;  // 表内容不占 root space
    TEST_ASSERT(RootParameterCostDwords(table) == 1, "descriptor table = 1 DWORD regardless of size");

    D3D12RootParameterDesc cbv;
    cbv.kind = D3D12RootParameterDesc::Kind::RootCBV;
    TEST_ASSERT(RootParameterCostDwords(cbv) == 2, "root CBV = 2");
    D3D12RootParameterDesc srv;
    srv.kind = D3D12RootParameterDesc::Kind::RootSRV;
    TEST_ASSERT(RootParameterCostDwords(srv) == 2, "root SRV = 2");
    D3D12RootParameterDesc uav;
    uav.kind = D3D12RootParameterDesc::Kind::RootUAV;
    TEST_ASSERT(RootParameterCostDwords(uav) == 2, "root UAV = 2");

    D3D12RootParameterDesc c1, c4, c5;
    c1.kind = D3D12RootParameterDesc::Kind::Constants32Bit; c1.constantCount = 1;
    c4.kind = D3D12RootParameterDesc::Kind::Constants32Bit; c4.constantCount = 4;
    c5.kind = D3D12RootParameterDesc::Kind::Constants32Bit; c5.constantCount = 5;
    TEST_ASSERT(RootParameterCostDwords(c1) == 1, "1 constant = 1 DWORD (ceil)");
    TEST_ASSERT(RootParameterCostDwords(c4) == 1, "4 constants = 1 DWORD (exact)");
    TEST_ASSERT(RootParameterCostDwords(c5) == 2, "5 constants = 2 DWORDs (ceil)");
    return TestResult::Passed;
}

TestResult TestTypicalLayoutsAndLimit() {
    // 典型布局：4 个 set（表）+ 1 个 CBV + 16 个 root constants
    primal::utl::vector<D3D12RootParameterDesc> params;
    for (int i = 0; i < 4; ++i) {
        D3D12RootParameterDesc table;
        table.kind = D3D12RootParameterDesc::Kind::DescriptorTable;
        table.descriptorCount = 64;
        params.push_back(table);
    }
    D3D12RootParameterDesc cbv;
    cbv.kind = D3D12RootParameterDesc::Kind::RootCBV;
    params.push_back(cbv);
    D3D12RootParameterDesc constants;
    constants.kind = D3D12RootParameterDesc::Kind::Constants32Bit;
    constants.constantCount = 16;
    params.push_back(constants);

    auto budget = EvaluateRootSignature(params);
    TEST_ASSERT(budget.ok, "typical layout fits");
    TEST_ASSERT(budget.dwords == 4 * 1 + 2 + 4, "4*1 + 2 + 16/4 = 10 DWORDs");
    TEST_ASSERT(budget.parameterCount == 6, "6 parameters");
    TEST_ASSERT(budget.descriptorCount == 256, "diagnostic descriptor count");

    // 60 DWORD root constants（=15 个 4-const 参数? 用单参数 240 常量 = 60 DWORD）+ 3 表 + 1 CBV = 65 > 64
    primal::utl::vector<D3D12RootParameterDesc> over;
    for (int i = 0; i < 3; ++i) {
        D3D12RootParameterDesc table;
        table.kind = D3D12RootParameterDesc::Kind::DescriptorTable;
        over.push_back(table);
    }
    D3D12RootParameterDesc rootDesc;
    rootDesc.kind = D3D12RootParameterDesc::Kind::RootCBV;
    over.push_back(rootDesc);
    D3D12RootParameterDesc big;
    big.kind = D3D12RootParameterDesc::Kind::Constants32Bit;
    big.constantCount = 240;  // 60 DWORD -> total 3+2+60=65
    over.push_back(big);
    auto overBudget = EvaluateRootSignature(over);
    TEST_ASSERT(!overBudget.ok, "65 DWORDs exceeds limit (explicit failure)");
    TEST_ASSERT(overBudget.dwords == 65, "reported over-limit count");
    TEST_ASSERT(std::string(overBudget.reason) == "exceeds_64_dword_limit", "failure reason");

    // 恰好 64：合法边界（边界条件——M0 记忆条：恰好占满是常见翻车点）
    primal::utl::vector<D3D12RootParameterDesc> exact;
    for (int i = 0; i < 32; ++i) {
        D3D12RootParameterDesc rootDescriptor;
        rootDescriptor.kind = D3D12RootParameterDesc::Kind::RootSRV;
        exact.push_back(rootDescriptor);
    }
    auto exactBudget = EvaluateRootSignature(exact);
    TEST_ASSERT(exactBudget.ok && exactBudget.dwords == 64, "exactly 64 DWORDs fits");

    // 65：一超即败
    primal::utl::vector<D3D12RootParameterDesc> oneOver = exact;
    D3D12RootParameterDesc table;
    table.kind = D3D12RootParameterDesc::Kind::DescriptorTable;
    oneOver.push_back(table);
    auto oneOverBudget = EvaluateRootSignature(oneOver);
    TEST_ASSERT(!oneOverBudget.ok && oneOverBudget.dwords == 65, "64+1 fails");

    // 空参数：0 DWORD 合法
    auto emptyBudget = EvaluateRootSignature(nullptr, 0);
    TEST_ASSERT(emptyBudget.ok && emptyBudget.dwords == 0, "empty root signature valid");

    // null 参数带非零 count：显式失败
    auto nullBudget = EvaluateRootSignature(nullptr, 3);
    TEST_ASSERT(!nullBudget.ok, "null params with count>0 rejected");
    return TestResult::Passed;
}

TestResult TestEstimateFromRhiLayouts() {
    // 2 个 set -> 2 张表 = 2 DWORD
    DescriptorSetLayoutBinding bindingsA[2] = {};
    bindingsA[0].binding = 0; bindingsA[0].descriptorType = DescriptorType::UniformBuffer; bindingsA[0].descriptorCount = 2;
    bindingsA[1].binding = 1; bindingsA[1].descriptorType = DescriptorType::StorageBuffer; bindingsA[1].descriptorCount = 3;
    DescriptorSetLayoutBinding bindingsB[1] = {};
    bindingsB[0].binding = 0; bindingsB[0].descriptorType = DescriptorType::CombinedImageSampler; bindingsB[0].descriptorCount = 4;
    DescriptorSetLayoutDesc layouts[2];
    layouts[0].bindingCount = 2; layouts[0].bindings = bindingsA;
    layouts[1].bindingCount = 1; layouts[1].bindings = bindingsB;

    auto budget = EstimateRootSignatureForLayouts(layouts, 2);
    TEST_ASSERT(budget.ok && budget.dwords == 2, "2 sets -> 2 table DWORDs");
    // CombinedImageSampler = SRV + SAMPLER 双堆位：set A 2 UBO + 3 SSBO = 5；
    // set B 4 组合采样器 = 8 → 诊断计数 13
    TEST_ASSERT(budget.descriptorCount == 13, "combined samplers counted double (heap slots)");

    // + push constants 96B = 24 constants = 6 DWORD -> 总 8
    auto withPush = EstimateRootSignatureForLayouts(layouts, 2, 96);
    TEST_ASSERT(withPush.ok && withPush.dwords == 8, "push constants append ceil(96/4/4)=6 DWORDs");
    TEST_ASSERT(withPush.parameterCount == 3, "3 parameters after push constants");

    // 上限压测：62 个 set + 96B push = 62 + 6 = 68 > 64
    primal::utl::vector<DescriptorSetLayoutDesc> many(62, DescriptorSetLayoutDesc{});
    auto over = EstimateRootSignatureForLayouts(many.data(), static_cast<u32>(many.size()), 96);
    TEST_ASSERT(!over.ok, "62 sets + 96B push exceeds 64");
    return TestResult::Passed;
}

void RegisterD3D12RootSigBudgetTests() {
    auto suite = std::make_shared<TestSuite>("D3D12RootSigBudgetTests");
    suite->AddTestCase(TestCase("RootParameterCosts", TestRootParameterCosts));
    suite->AddTestCase(TestCase("TypicalLayoutsAndLimit", TestTypicalLayoutsAndLimit));
    suite->AddTestCase(TestCase("EstimateFromRhiLayouts", TestEstimateFromRhiLayouts));
    TestRunner::RegisterTestSuite(suite);
}

#ifndef UNIT_TEST_LIB
int main() {
    RegisterD3D12RootSigBudgetTests();
    TestRunner::RunAllSuites();
    return 0;
}
#endif
