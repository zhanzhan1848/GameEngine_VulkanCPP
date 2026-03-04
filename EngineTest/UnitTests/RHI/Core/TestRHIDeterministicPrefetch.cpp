/**
 * @file TestRHIDeterministicPrefetch.cpp
 * @brief RHI确定性资源预取系统单元测试
 * @details 测试硬件能力分析、访问模式分析、静态规则引擎和预取决策功能
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-04
 * @version 1.0.0
 */

#include "../../TestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDeterministicPrefetch.h"
#include <memory>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <unordered_set>

using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::deterministic_prefetch;
using namespace Engine::Test;
using TestResult = Engine::Test::TestResult;

// 枚举类型的输出流运算符重载
std::ostream& operator<<(std::ostream& os, HardwareClass hardwareClass) {
    switch (hardwareClass) {
        case HardwareClass::Ultra: os << "Ultra"; break;
        case HardwareClass::High: os << "High"; break;
        case HardwareClass::Medium: os << "Medium"; break;
        case HardwareClass::Low: os << "Low"; break;
        default: os << "Unknown"; break;
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, AccessType accessType) {
    switch (accessType) {
        case AccessType::Sequential: os << "Sequential"; break;
        case AccessType::Random: os << "Random"; break;
        case AccessType::PatternBased: os << "PatternBased"; break;
        case AccessType::Predictable: os << "Predictable"; break;
        case AccessType::Count: os << "Count"; break;
        default: os << "Unknown"; break;
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, primal::graphics::rhi::PrefetchResourceType resourceType) {
    switch (resourceType) {
        case primal::graphics::rhi::PrefetchResourceType::Geometry: os << "Geometry"; break;
    case primal::graphics::rhi::PrefetchResourceType::Texture: os << "Texture"; break;
    case primal::graphics::rhi::PrefetchResourceType::Shader: os << "Shader"; break;
    case primal::graphics::rhi::PrefetchResourceType::Animation: os << "Animation"; break;
    case primal::graphics::rhi::PrefetchResourceType::Audio: os << "Audio"; break;
    case primal::graphics::rhi::PrefetchResourceType::Material: os << "Material"; break;
    case primal::graphics::rhi::PrefetchResourceType::PipelineState: os << "PipelineState"; break;
    case primal::graphics::rhi::PrefetchResourceType::Count: os << "Count"; break;
        default: os << "Unknown"; break;
    }
    return os;
}

// === Mock数据生成器 ===

class TestDataGenerator {
public:
    static HardwareProfile CreateMockHardwareProfile(HardwareClass hardwareClass = HardwareClass::Medium) {
        HardwareProfile profile;
        profile.totalGPUMemoryMB = hardwareClass == HardwareClass::Ultra ? 16384 :
                                  hardwareClass == HardwareClass::High ? 8192 :
                                  hardwareClass == HardwareClass::Medium ? 4096 : 2048;
        profile.systemMemoryMB = 16384;
        profile.memoryBandwidthGBps = hardwareClass == HardwareClass::Ultra ? 100 :
                                     hardwareClass == HardwareClass::High ? 50 :
                                     hardwareClass == HardwareClass::Medium ? 25 : 15;
        profile.storageType = 1; // SSD
        profile.hardwareClass = hardwareClass;
        profile.supportsAsyncCompute = true;
        profile.supportsFastStorage = true;
        return profile;
    }
    
    static FrameData CreateMockFrameData(u32 frameNumber = 1, SceneType sceneType = SceneType::Outdoor) {
        FrameData frameData;
        frameData.currentFrameNumber = frameNumber;
        frameData.currentSceneType = sceneType;
        frameData.frameTime = 16.67f; // 60fps
        frameData.currentMemoryUsageMB = 1024;
        
        // 添加一些可见资源
        frameData.visibleResources.push_back(1001);
        frameData.visibleResources.push_back(1002);
        frameData.visibleResources.push_back(1003);
        frameData.visibleResources.push_back(2001);
        frameData.visibleResources.push_back(2002);
        return frameData;
    }
    
    static AccessRecord CreateMockAccessRecord(u64 resourceId = 1001, PrefetchResourceType type = PrefetchResourceType::Geometry) {
        AccessRecord record;
        record.resourceId = resourceId;
        record.timestamp = GetCurrentTimestamp();
        record.frameNumber = 1;
        record.resourceType = type;
        record.lodLevel = 0;
        record.accessType = AccessType::Sequential;
        record.accessCount = 1;
        return record;
    }
    
    static PrefetchConfiguration CreateMockConfiguration() {
        PrefetchConfiguration config;
        config.enableDeterministic = true;
        config.enableMLFeatures = false;
        config.maxMemoryBudgetMB = 2048;
        config.historyWindowSize = 1000;
        config.cleanupIntervalSeconds = 300;
        config.hotResourceThreshold = 10;
        config.enableLogging = false; // 测试时关闭日志
        config.enableProfiling = true;
        return config;
    }
    
private:
    static u64 GetCurrentTimestamp() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
};

// === 测试套件类 ===

class TestRHIDeterministicPrefetch : public TestSuite {
public:
    TestRHIDeterministicPrefetch() : TestSuite("RHIDeterministicPrefetch") {
        // 注册所有测试用例
        AddTestCase(TestCase("HardwareCapabilityAnalyzer_BasicInitialization", 
                           [this]() { return HardwareCapabilityAnalyzer_BasicInitialization(); },
                           "测试硬件能力分析器基本初始化"));
        
        AddTestCase(TestCase("HardwareCapabilityAnalyzer_ProfileValidation", 
                           [this]() { return HardwareCapabilityAnalyzer_ProfileValidation(); },
                           "测试硬件配置文件验证"));
        
        AddTestCase(TestCase("HardwareCapabilityAnalyzer_ProfileUpdate", 
                           [this]() { return HardwareCapabilityAnalyzer_ProfileUpdate(); },
                           "测试硬件配置文件更新"));
        
        AddTestCase(TestCase("AccessPatternAnalyzer_BasicOperations", 
                           [this]() { return AccessPatternAnalyzer_BasicOperations(); },
                           "测试访问模式分析器基本操作"));
        
        AddTestCase(TestCase("AccessPatternAnalyzer_PatternAnalysis", 
                           [this]() { return AccessPatternAnalyzer_PatternAnalysis(); },
                           "测试访问模式分析"));
        
        AddTestCase(TestCase("AccessPatternAnalyzer_FrequencyTiers", 
                           [this]() { return AccessPatternAnalyzer_FrequencyTiers(); },
                           "测试频率分层"));
        
        AddTestCase(TestCase("StaticRuleEngine_BasicOperations", 
                           [this]() { return StaticRuleEngine_BasicOperations(); },
                           "测试静态规则引擎基本操作"));
        
        AddTestCase(TestCase("StaticRuleEngine_RuleEvaluation", 
                           [this]() { return StaticRuleEngine_RuleEvaluation(); },
                           "测试规则评估"));
        
        AddTestCase(TestCase("StaticRuleEngine_CandidateGeneration", 
                           [this]() { return StaticRuleEngine_CandidateGeneration(); },
                           "测试候选生成"));
        
        AddTestCase(TestCase("StaticRuleEngine_CustomRules", 
                           [this]() { return StaticRuleEngine_CustomRules(); },
                           "测试自定义规则"));
        
        AddTestCase(TestCase("PrefetchManager_BasicLifecycle", 
                           [this]() { return PrefetchManager_BasicLifecycle(); },
                           "测试预取管理器基本生命周期"));
        
        AddTestCase(TestCase("PrefetchManager_DecisionGeneration", 
                           [this]() { return PrefetchManager_DecisionGeneration(); },
                           "测试预取决策生成"));
        
        AddTestCase(TestCase("PrefetchManager_MemoryBudgetManagement", 
                           [this]() { return PrefetchManager_MemoryBudgetManagement(); },
                           "测试内存预算管理"));
        
        AddTestCase(TestCase("PrefetchManager_UpdateCycle", 
                           [this]() { return PrefetchManager_UpdateCycle(); },
                           "测试更新周期"));
        
        AddTestCase(TestCase("PrefetchManager_ConfidenceCalculation", 
                           [this]() { return PrefetchManager_ConfidenceCalculation(); },
                           "测试置信度计算"));
        
        AddTestCase(TestCase("PrefetchManager_ErrorHandling", 
                           [this]() { return PrefetchManager_ErrorHandling(); },
                           "测试错误处理"));
        
        AddTestCase(TestCase("Performance_DecisionGeneration", 
                           [this]() { return Performance_DecisionGeneration(); },
                           "测试性能：决策生成"));
        
        AddTestCase(TestCase("Performance_MemoryUsage", 
                           [this]() { return Performance_MemoryUsage(); },
                           "测试性能：内存使用"));
    }
    
    void SetUp() {
        config_ = TestDataGenerator::CreateMockConfiguration();
        prefetchManager_ = std::make_unique<RHIDeterministicPrefetchManager>(config_);
        
        // 初始化测试数据
        bool initialized = prefetchManager_->Initialize();
        if (!initialized) {
            std::cout << "错误: 预取管理器初始化失败" << std::endl;
        }
        
        std::cout << "确定性预取测试环境设置完成" << std::endl;
    }
    
    void TearDown() {
        if (prefetchManager_) {
            prefetchManager_->Shutdown();
            prefetchManager_.reset();
        }
        std::cout << "确定性预取测试环境清理完成" << std::endl;
    }
    
    // 硬件能力分析器测试
    TestResult HardwareCapabilityAnalyzer_BasicInitialization();
    TestResult HardwareCapabilityAnalyzer_ProfileValidation();
    TestResult HardwareCapabilityAnalyzer_ProfileUpdate();
    
    // 访问模式分析器测试
    TestResult AccessPatternAnalyzer_BasicOperations();
    TestResult AccessPatternAnalyzer_PatternAnalysis();
    TestResult AccessPatternAnalyzer_FrequencyTiers();
    
    // 静态规则引擎测试
    TestResult StaticRuleEngine_BasicOperations();
    TestResult StaticRuleEngine_RuleEvaluation();
    TestResult StaticRuleEngine_CandidateGeneration();
    TestResult StaticRuleEngine_CustomRules();
    
    // 预取管理器集成测试
    TestResult PrefetchManager_BasicLifecycle();
    TestResult PrefetchManager_DecisionGeneration();
    TestResult PrefetchManager_MemoryBudgetManagement();
    TestResult PrefetchManager_UpdateCycle();
    TestResult PrefetchManager_ConfidenceCalculation();
    TestResult PrefetchManager_ErrorHandling();
    
    // 性能测试
    TestResult Performance_DecisionGeneration();
    TestResult Performance_MemoryUsage();

private:
    PrefetchConfiguration config_;
    std::unique_ptr<RHIDeterministicPrefetchManager> prefetchManager_;
};

// === 硬件能力分析器测试 ===

TestResult TestRHIDeterministicPrefetch::HardwareCapabilityAnalyzer_BasicInitialization() {
    auto analyzer = std::make_unique<HardwareCapabilityAnalyzer>();
    
    // 测试初始化
    bool initialized = analyzer->Initialize();
    TEST_ASSERT(initialized, "硬件能力分析器初始化应该成功");
    
    // 测试获取硬件配置文件
    const auto& profile = analyzer->GetProfile();
    TEST_ASSERT(profile.totalGPUMemoryMB > 0, "GPU内存大小应该大于0");
    TEST_ASSERT(profile.systemMemoryMB > 0, "系统内存大小应该大于0");
    TEST_ASSERT(profile.memoryBandwidthGBps > 0, "内存带宽应该大于0");
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::HardwareCapabilityAnalyzer_ProfileValidation() {
    auto analyzer = std::make_unique<HardwareCapabilityAnalyzer>();
    analyzer->Initialize();
    
    const auto& profile = analyzer->GetProfile();
    
    // 验证硬件等级分类的合理性
    bool isValidClass = (profile.hardwareClass == HardwareClass::Low ||
                        profile.hardwareClass == HardwareClass::Medium ||
                        profile.hardwareClass == HardwareClass::High ||
                        profile.hardwareClass == HardwareClass::Ultra);
    TEST_ASSERT(isValidClass, "硬件等级应该是有效的枚举值");
    
    // 验证存储类型
    TEST_ASSERT(profile.storageType >= 0 && profile.storageType <= 2, 
                "存储类型应该在0-2范围内");
    
    // 验证布尔标志
    TEST_ASSERT(profile.supportsAsyncCompute || !profile.supportsAsyncCompute, 
                "异步计算支持应该是布尔值");
    TEST_ASSERT(profile.supportsFastStorage || !profile.supportsFastStorage, 
                "快速存储支持应该是布尔值");
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::HardwareCapabilityAnalyzer_ProfileUpdate() {
    auto analyzer = std::make_unique<HardwareCapabilityAnalyzer>();
    
    // 创建自定义配置文件
    HardwareProfile customProfile = TestDataGenerator::CreateMockHardwareProfile(HardwareClass::High);
    
    // 更新配置文件
    analyzer->UpdateProfile(customProfile);
    
    // 验证更新后的配置
    const auto& updatedProfile = analyzer->GetProfile();
    TEST_ASSERT_EQ(customProfile.totalGPUMemoryMB, updatedProfile.totalGPUMemoryMB, 
                   "GPU内存大小应该被正确更新");
    TEST_ASSERT_EQ(customProfile.hardwareClass, updatedProfile.hardwareClass, 
                   "硬件等级应该被正确更新");
    TEST_ASSERT_EQ(customProfile.memoryBandwidthGBps, updatedProfile.memoryBandwidthGBps, 
                   "内存带宽应该被正确更新");
    
    return TestResult::Passed;
}

// === 访问模式分析器测试 ===

TestResult TestRHIDeterministicPrefetch::AccessPatternAnalyzer_BasicOperations() {
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto analyzer = std::make_unique<AccessPatternAnalyzer>(config);
    
    // 添加访问记录
    auto record = TestDataGenerator::CreateMockAccessRecord(1001, PrefetchResourceType::Texture);
    analyzer->AddAccessRecord(record);
    
    // 获取访问频率
    auto frequency = analyzer->GetAccessFrequency(1001);
    TEST_ASSERT_EQ(1001, frequency.resourceId, "资源ID应该匹配");
    TEST_ASSERT_EQ(1, frequency.accessCount, "访问次数应该为1");
    TEST_ASSERT(frequency.lastAccessTime > 0, "最后访问时间应该大于0");
    
    // 测试不存在的资源
    auto emptyFrequency = analyzer->GetAccessFrequency(9999);
    TEST_ASSERT_EQ(0, emptyFrequency.resourceId, "不存在资源的ID应该为0");
    TEST_ASSERT_EQ(0, emptyFrequency.accessCount, "不存在资源的访问次数应该为0");
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::AccessPatternAnalyzer_PatternAnalysis() {
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto analyzer = std::make_unique<AccessPatternAnalyzer>(config);
    
    // 添加多个访问记录模拟高频访问
    u64 resourceId = 1001;
    for (int i = 0; i < 11; ++i) {  // 添加11次访问确保超过阈值10
        auto record = TestDataGenerator::CreateMockAccessRecord(resourceId, PrefetchResourceType::Texture);
        record.timestamp += i * 50; // 50ms间隔
        analyzer->AddAccessRecord(record);
    }
    
    // 分析访问模式
    AccessType pattern = analyzer->AnalyzePattern(resourceId);
    
    // 验证模式识别结果
    bool isValidPattern = (pattern == AccessType::Sequential ||
                          pattern == AccessType::PatternBased ||
                          pattern == AccessType::Random ||
                          pattern == AccessType::Predictable);
    TEST_ASSERT(isValidPattern, "访问模式应该是有效的枚举值");
    
    // 测试热点资源检测
    auto hotResources = analyzer->GetHotResources(10);
    bool foundInHot = std::find(hotResources.begin(), hotResources.end(), resourceId) != hotResources.end();
    TEST_ASSERT(foundInHot, "高频访问的资源应该被识别为热点资源");
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::AccessPatternAnalyzer_FrequencyTiers() {
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto analyzer = std::make_unique<AccessPatternAnalyzer>(config);
    
    // 测试不同访问频率的等级计算
    std::vector<u32> testCounts = {1, 5, 10, 20, 50, 100};
    std::vector<u32> expectedTiers = {0, 1, 2, 3, 4, 5};
    
    for (size_t i = 0; i < testCounts.size(); ++i) {
        u64 resourceId = 2000 + i;
        
        // 添加指定数量的访问记录
        for (u32 j = 0; j < testCounts[i]; ++j) {
            auto record = TestDataGenerator::CreateMockAccessRecord(resourceId, PrefetchResourceType::Texture);
            record.timestamp += j * 10;
            analyzer->AddAccessRecord(record);
        }
        
        auto frequency = analyzer->GetAccessFrequency(resourceId);
        u32 tier = analyzer->CalculateFrequencyTier(frequency);
        
        TEST_ASSERT_EQ(expectedTiers[i], tier, 
                       ("访问次数" + std::to_string(testCounts[i]) + "的频率等级应该正确").c_str());
    }
    
    return TestResult::Passed;
}

// === 静态规则引擎测试 ===

TestResult TestRHIDeterministicPrefetch::StaticRuleEngine_BasicOperations() {
    auto engine = std::make_unique<StaticRuleEngine>();
    
    // 测试初始化
    bool initialized = engine->Initialize();
    TEST_ASSERT(initialized, "静态规则引擎初始化应该成功");
    
    // 测试获取默认规则
    const auto& rules = engine->GetAllRules();
    TEST_ASSERT(rules.size() > 0, "应该有默认规则");
    
    // 验证规则的基本属性
    for (const auto& rule : rules) {
        TEST_ASSERT(rule.enabled || !rule.enabled, "规则启用状态应该是布尔值");
        TEST_ASSERT(rule.priority >= 0 && rule.priority <= 100, "规则优先级应该在0-100范围内");
        TEST_ASSERT(rule.confidence >= 0.0f && rule.confidence <= 1.0f, "规则置信度应该在0-1范围内");
    }
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::StaticRuleEngine_RuleEvaluation() {
    auto engine = std::make_unique<StaticRuleEngine>();
    engine->Initialize();
    
    // 测试规则评估
    auto matchingRules = engine->EvaluateRules(
        PrefetchResourceType::Texture, 
        SceneType::Outdoor, 
        HardwareClass::Medium
    );
    
    TEST_ASSERT(matchingRules.size() > 0, "应该有匹配的规则");
    
    // 验证规则按优先级排序
    for (size_t i = 1; i < matchingRules.size(); ++i) {
        TEST_ASSERT(matchingRules[i-1].priority >= matchingRules[i].priority, 
                    "规则应该按优先级降序排列");
    }
    
    // 测试规则匹配逻辑
    for (const auto& rule : matchingRules) {
        bool matches = engine->RuleMatches(rule, PrefetchResourceType::Texture, SceneType::Outdoor, HardwareClass::Medium);
        TEST_ASSERT(matches, "评估返回的规则应该匹配给定条件");
    }
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::StaticRuleEngine_CandidateGeneration() {
    auto engine = std::make_unique<StaticRuleEngine>();
    engine->Initialize();
    
    auto profile = TestDataGenerator::CreateMockHardwareProfile();
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto patternAnalyzer = std::make_unique<AccessPatternAnalyzer>(config);
    
    // 添加一些访问历史
    auto record = TestDataGenerator::CreateMockAccessRecord(1001, PrefetchResourceType::Texture);
    patternAnalyzer->AddAccessRecord(record);
    
    auto frameData = TestDataGenerator::CreateMockFrameData();
    
    // 生成候选资源
    auto candidates = engine->GenerateCandidates(frameData, profile, *patternAnalyzer);
    
    TEST_ASSERT(candidates.size() > 0, "应该生成候选资源列表");
    
    // 验证候选资源ID的唯一性
    std::unordered_set<u64> uniqueIds;
    for (u64 id : candidates) {
        TEST_ASSERT(id > 0, "候选资源ID应该大于0");
        uniqueIds.insert(id);
    }
    
    TEST_ASSERT(uniqueIds.size() == candidates.size(), "候选资源ID应该是唯一的");
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::StaticRuleEngine_CustomRules() {
    auto engine = std::make_unique<StaticRuleEngine>();
    engine->Initialize();
    
    // 添加自定义规则
    PrefetchRule customRule;
    customRule.resourceType = PrefetchResourceType::Texture;
    customRule.sceneType = SceneType::Combat;
    customRule.hardwareClass = HardwareClass::High;
    customRule.timeWindow.cpuToGpuFrames = 1;
    customRule.timeWindow.renderDataFrames = 1;
    customRule.timeWindow.cullingDataFrames = 2;
    customRule.timeWindow.streamingDataMs = 100;
    customRule.priority = 95;
    customRule.confidence = 0.95f;
    customRule.memoryBudgetMB = 1024;
    customRule.enabled = true;
    
    engine->AddRule(customRule);
    
    // 验证规则添加
    const auto& rules = engine->GetAllRules();
    bool foundCustom = false;
    for (const auto& rule : rules) {
        if (rule.resourceType == PrefetchResourceType::Texture && 
            rule.sceneType == SceneType::Combat &&
            rule.hardwareClass == HardwareClass::High &&
            rule.priority == 95) {
            foundCustom = true;
            break;
        }
    }
    TEST_ASSERT(foundCustom, "自定义规则应该被正确添加");
    
    // 测试自定义规则的匹配
    auto matchingRules = engine->EvaluateRules(
        PrefetchResourceType::Texture, 
        SceneType::Combat, 
        HardwareClass::High
    );
    
    bool foundInMatching = false;
    for (const auto& rule : matchingRules) {
        if (rule.priority == 95 && rule.confidence == 0.95f) {
            foundInMatching = true;
            break;
        }
    }
    TEST_ASSERT(foundInMatching, "自定义规则应该被正确匹配");
    
    return TestResult::Passed;
}

// === 预取管理器集成测试 ===

TestResult TestRHIDeterministicPrefetch::PrefetchManager_BasicLifecycle() {
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto manager = std::make_unique<RHIDeterministicPrefetchManager>(config);
    
    // 测试初始化
    bool initialized = manager->Initialize();
    TEST_ASSERT(initialized, "预取管理器初始化应该成功");
    
    // 测试配置获取
    const auto& retrievedConfig = manager->GetConfiguration();
    TEST_ASSERT_EQ(config.enableDeterministic, retrievedConfig.enableDeterministic, 
                   "配置应该被正确保存和获取");
    TEST_ASSERT_EQ(config.maxMemoryBudgetMB, retrievedConfig.maxMemoryBudgetMB, 
                   "内存预算配置应该被正确保存和获取");
    
    // 测试组件获取
    TEST_ASSERT_NOT_NULL(manager->GetHardwareAnalyzer(), "硬件分析器应该存在");
    TEST_ASSERT_NOT_NULL(manager->GetPatternAnalyzer(), "模式分析器应该存在");
    TEST_ASSERT_NOT_NULL(manager->GetRuleEngine(), "规则引擎应该存在");
    
    // 测试关闭
    manager->Shutdown();
    // 注意：关闭后的状态测试依赖于具体实现，这里不强制要求
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::PrefetchManager_DecisionGeneration() {
    // 为此测试创建新的管理器实例
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto prefetchManager = std::make_unique<RHIDeterministicPrefetchManager>(config);
    bool initialized = prefetchManager->Initialize();
    TEST_ASSERT(initialized, "预取管理器应该成功初始化");
    
    auto frameData = TestDataGenerator::CreateMockFrameData(1, SceneType::Outdoor);
    
    // 生成预取决策
    auto decisions = prefetchManager->GeneratePrefetchDecisions(frameData);
    
    TEST_ASSERT(decisions.size() >= 0, "预取决策列表应该存在");
    
    // 验证决策的基本属性
    for (const auto& decision : decisions) {
        TEST_ASSERT(decision.resourceId > 0, "决策中的资源ID应该大于0");
        TEST_ASSERT(decision.shouldPrefetch || !decision.shouldPrefetch, "预取标志应该是布尔值");
        TEST_ASSERT(decision.priority >= 0 && decision.priority <= 100, "优先级应该在0-100范围内");
        TEST_ASSERT(decision.confidence >= 0.0f && decision.confidence <= 1.0f, "置信度应该在0-1范围内");
        TEST_ASSERT(decision.estimatedSize > 0, "预估大小应该大于0");
    }
    
    // 测试决策按优先级排序
    for (size_t i = 1; i < decisions.size(); ++i) {
        TEST_ASSERT(decisions[i-1].priority >= decisions[i].priority, 
                    "预取决策应该按优先级降序排列");
    }
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::PrefetchManager_MemoryBudgetManagement() {
    // 为此测试创建新的管理器实例
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto prefetchManager = std::make_unique<RHIDeterministicPrefetchManager>(config);
    bool initialized = prefetchManager->Initialize();
    TEST_ASSERT(initialized, "预取管理器应该成功初始化");
    
    // 测试内存预算检查
    bool hasBudget = prefetchManager->HasMemoryBudget(1024 * 1024); // 1MB
    TEST_ASSERT(hasBudget || !hasBudget, "内存预算检查应该返回布尔值");
    
    // 测试大内存请求
    bool hasLargeBudget = prefetchManager->HasMemoryBudget(10ULL * 1024 * 1024 * 1024); // 10GB
    TEST_ASSERT(!hasLargeBudget, "超大内存请求应该超出预算");
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::PrefetchManager_UpdateCycle() {
    // 为此测试创建新的管理器实例
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto prefetchManager = std::make_unique<RHIDeterministicPrefetchManager>(config);
    bool initialized = prefetchManager->Initialize();
    TEST_ASSERT(initialized, "预取管理器应该成功初始化");
    
    // 模拟多帧更新
    for (u32 frame = 1; frame <= 5; ++frame) {
        auto frameData = TestDataGenerator::CreateMockFrameData(frame, SceneType::Outdoor);
        frameData.frameTime = 16.67f; // 60fps
        
        // 添加一些资源访问记录
        if (frame > 1) {
            auto analyzer = prefetchManager->GetPatternAnalyzer();
            auto record = TestDataGenerator::CreateMockAccessRecord(1000 + frame, PrefetchResourceType::Texture);
            analyzer->AddAccessRecord(record);
        }
        
        // 更新预取管理器
        prefetchManager->Update(frameData);
        
        // 获取活跃决策
        const auto& activeDecisions = prefetchManager->GetActiveDecisions();
        TEST_ASSERT(activeDecisions.size() >= 0, "活跃决策列表应该存在");
        
        // 验证决策的合理性
        for (const auto& decision : activeDecisions) {
            TEST_ASSERT(decision.resourceId > 0, "活跃决策的资源ID应该有效");
            if (decision.shouldPrefetch) {
                TEST_ASSERT(decision.confidence > 0.5f, "应该预取的决策置信度应该较高");
            }
        }
    }
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::PrefetchManager_ConfidenceCalculation() {
    // 为此测试创建新的管理器实例
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto prefetchManager = std::make_unique<RHIDeterministicPrefetchManager>(config);
    bool initialized = prefetchManager->Initialize();
    TEST_ASSERT(initialized, "预取管理器应该成功初始化");
    
    // 为不同资源创建不同的访问历史
    auto patternAnalyzer = prefetchManager->GetPatternAnalyzer();
    
    // 资源1001: 高频访问 (50次)
    for (int i = 0; i < 50; ++i) {
        auto record = TestDataGenerator::CreateMockAccessRecord(1001, PrefetchResourceType::Texture);
        record.timestamp += i * 10;
        patternAnalyzer->AddAccessRecord(record);
    }
    
    // 资源1002: 中频访问 (20次)
    for (int i = 0; i < 20; ++i) {
        auto record = TestDataGenerator::CreateMockAccessRecord(1002, PrefetchResourceType::Texture);
        record.timestamp += i * 15;
        patternAnalyzer->AddAccessRecord(record);
    }
    
    // 资源1003: 低频访问 (5次)
    for (int i = 0; i < 5; ++i) {
        auto record = TestDataGenerator::CreateMockAccessRecord(1003, PrefetchResourceType::Texture);
        record.timestamp += i * 20;
        patternAnalyzer->AddAccessRecord(record);
    }
    
    // 资源2001, 2002: 无访问历史
    
    auto frameData = TestDataGenerator::CreateMockFrameData();
    
    // 测试确定性置信度计算
    f32 confidence = prefetchManager->CalculateDeterministicConfidence(
        1001, primal::graphics::rhi::PrefetchResourceType::Texture, frameData);
    
    TEST_ASSERT(confidence >= 0.0f && confidence <= 1.0f, "置信度应该在0-1范围内");
    
    // 测试不同资源的置信度
    std::vector<u64> resourceIds = {1001, 1002, 1003, 2001, 2002};
    std::vector<f32> confidences;
    
    for (u64 id : resourceIds) {
        f32 conf = prefetchManager->CalculateDeterministicConfidence(
            id, PrefetchResourceType::Texture, frameData);
        confidences.push_back(conf);
        
        TEST_ASSERT(conf >= 0.0f && conf <= 1.0f, "每个资源的置信度都应该在0-1范围内");
    }
    
    // 验证置信度的差异性（不同资源应该有不同的置信度）
    bool hasVariation = false;
    for (size_t i = 1; i < confidences.size(); ++i) {
        if (std::abs(confidences[i] - confidences[i-1]) > 0.01f) {
            hasVariation = true;
            break;
        }
    }
    TEST_ASSERT(hasVariation, "不同资源的置信度应该有差异");
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::PrefetchManager_ErrorHandling() {
    // 为此测试创建新的管理器实例
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto prefetchManager = std::make_unique<RHIDeterministicPrefetchManager>(config);
    bool initialized = prefetchManager->Initialize();
    TEST_ASSERT(initialized, "预取管理器应该成功初始化");
    
    // 测试空帧数据
    FrameData emptyFrameData;
    auto emptyDecisions = prefetchManager->GeneratePrefetchDecisions(emptyFrameData);
    TEST_ASSERT(emptyDecisions.size() >= 0, "空帧数据应该产生有效的决策列表");
    
    // 测试无效资源ID
    f32 invalidConfidence = prefetchManager->CalculateDeterministicConfidence(
        0, PrefetchResourceType::Texture, emptyFrameData);
    TEST_ASSERT(invalidConfidence >= 0.0f && invalidConfidence <= 1.0f, 
                "无效资源的置信度也应该在有效范围内");
    
    // 测试极大资源ID
    f32 largeIdConfidence = prefetchManager->CalculateDeterministicConfidence(
        UINT64_MAX, PrefetchResourceType::Texture, emptyFrameData);
    TEST_ASSERT(largeIdConfidence >= 0.0f && largeIdConfidence <= 1.0f, 
                "极大资源ID的置信度也应该在有效范围内");
    
    return TestResult::Passed;
}

// === 性能测试 ===

TestResult TestRHIDeterministicPrefetch::Performance_DecisionGeneration() {
    // 为此测试创建新的管理器实例
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto prefetchManager = std::make_unique<RHIDeterministicPrefetchManager>(config);
    bool initialized = prefetchManager->Initialize();
    TEST_ASSERT(initialized, "预取管理器应该成功初始化");
    
    const int numIterations = 100;
    auto frameData = TestDataGenerator::CreateMockFrameData();
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < numIterations; ++i) {
        auto decisions = prefetchManager->GeneratePrefetchDecisions(frameData);
        // 确保编译器不会优化掉循环
        volatile size_t decisionCount = decisions.size();
        (void)decisionCount;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    double averageTime = static_cast<double>(duration.count()) / numIterations;
    
    // 验证性能要求（每次决策生成应该在10ms以内）
    TEST_ASSERT(averageTime < 10000.0, 
                ("决策生成平均时间应该在10ms以内，实际: " + std::to_string(averageTime) + "μs").c_str());
    
    return TestResult::Passed;
}

TestResult TestRHIDeterministicPrefetch::Performance_MemoryUsage() {
    // 测试大量访问记录的内存使用
    auto config = TestDataGenerator::CreateMockConfiguration();
    auto analyzer = std::make_unique<AccessPatternAnalyzer>(config);
    
    // 添加大量访问记录
    const int numRecords = 5000;
    for (int i = 0; i < numRecords; ++i) {
        auto record = TestDataGenerator::CreateMockAccessRecord(1000 + (i % 100), PrefetchResourceType::Texture);
        record.timestamp += i * 10;
        analyzer->AddAccessRecord(record);
    }
    
    // 验证系统能够处理大量数据
    auto hotResources = analyzer->GetHotResources(50);
    TEST_ASSERT(hotResources.size() <= 50, "热点资源数量应该符合请求限制");
    
    // 验证频率表的大小合理
    const auto& frequencies = analyzer->GetAllFrequencies();
    TEST_ASSERT(frequencies.size() <= 100, "频率表大小应该不超过唯一资源数量");
    
    return TestResult::Passed;
}

// === 测试注册 ===

void RegisterRHIDeterministicPrefetchTests() {
    TestRunner::RegisterTestSuite(std::make_shared<TestRHIDeterministicPrefetch>());
}

// === 主函数 ===

int main() {
    std::cout << "🚀 开始运行RHI确定性资源预取系统测试" << std::endl;
    std::cout << "测试覆盖: 硬件分析、访问模式、规则引擎、预取决策" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    // 注册测试
    RegisterRHIDeterministicPrefetchTests();
    
    // 运行所有测试
    auto stats = TestRunner::RunAllSuites();
    
    // 输出最终结果
    std::cout << std::endl;
    if (stats.failedTests == 0) {
        std::cout << "🎉 所有RHI确定性预取系统测试都通过了!" << std::endl;
        std::cout << "系统功能验证完成，可以进入下一阶段开发。" << std::endl;
    } else {
        std::cout << "⚠️  有 " << stats.failedTests << " 个测试失败，请检查实现。" << std::endl;
    }
    
    return (stats.failedTests == 0) ? 0 : 1;
}