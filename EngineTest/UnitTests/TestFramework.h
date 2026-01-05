/**
 * @file TestFramework.h
 * @brief 游戏引擎测试框架
 * @details 提供简单的单元测试框架，用于测试引擎核心模块
 * 
 * @author Engine开发团队
 * @date 2025-12-29
 * @version 1.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <chrono>
#include <sstream>

namespace Engine {
namespace Test {

/**
 * @brief 测试结果状态
 */
enum class TestResult : uint8_t {
    Passed = 0,     ///< 测试通过
    Failed = 1,     ///< 测试失败
    Skipped = 2     ///< 测试跳过
};

/**
 * @brief 测试用例信息
 */
struct TestCase {
    std::string name;                    ///< 测试用例名称
    std::function<TestResult()> func;    ///< 测试函数
    std::string description;             ///< 测试描述
    
    TestCase(const std::string& testName, 
             std::function<TestResult()> testFunc,
             const std::string& testDesc = "")
        : name(testName), func(testFunc), description(testDesc) {}
};

/**
 * @brief 测试套件统计信息
 */
struct TestStats {
    uint32_t totalTests = 0;      ///< 总测试数
    uint32_t passedTests = 0;     ///< 通过测试数
    uint32_t failedTests = 0;     ///< 失败测试数
    uint32_t skippedTests = 0;    ///< 跳过测试数
    
    double totalTime = 0.0;      ///< 总耗时（毫秒）
    double averageTime = 0.0;     ///< 平均耗时（毫秒）
};

/**
 * @brief 测试套件类
 */
class TestSuite {
public:
    /**
     * @brief 构造函数
     * @param suiteName 测试套件名称
     */
    explicit TestSuite(const std::string& suiteName) 
        : suiteName_(suiteName) {}
    
    /**
     * @brief 析构函数
     */
    virtual ~TestSuite() = default;
    
    /**
     * @brief 添加测试用例
     * @param testCase 测试用例
     */
    void AddTestCase(const TestCase& testCase) {
        testCases_.push_back(testCase);
    }
    
    /**
     * @brief 运行所有测试
     * @return 测试统计信息
     */
    TestStats RunAllTests() {
        TestStats stats;
        stats.totalTests = static_cast<uint32_t>(testCases_.size());
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        std::cout << "\n=== 运行测试套件: " << suiteName_ << " ===" << std::endl;
        std::cout << "总测试数: " << stats.totalTests << std::endl;
        std::cout << std::string(50, '-') << std::endl;
        
        for (const auto& testCase : testCases_) {
            std::cout << "运行测试: " << testCase.name << "... ";
            
            auto testStartTime = std::chrono::high_resolution_clock::now();
            
            try {
                TestResult result = testCase.func();
                
                auto testEndTime = std::chrono::high_resolution_clock::now();
                auto testDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                    testEndTime - testStartTime);
                double testTime = static_cast<double>(testDuration.count()) / 1000.0;
                
                switch (result) {
                    case TestResult::Passed:
                        std::cout << "通过 (" << testTime << "ms)" << std::endl;
                        stats.passedTests++;
                        break;
                    case TestResult::Failed:
                        std::cout << "失败 (" << testTime << "ms)" << std::endl;
                        stats.failedTests++;
                        break;
                    case TestResult::Skipped:
                        std::cout << "跳过" << std::endl;
                        stats.skippedTests++;
                        break;
                }
                
                stats.totalTime += testTime;
            }
            catch (const std::exception& e) {
                std::cout << "异常: " << e.what() << std::endl;
                stats.failedTests++;
            }
            catch (...) {
                std::cout << "未知异常" << std::endl;
                stats.failedTests++;
            }
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime);
        stats.totalTime = static_cast<double>(totalDuration.count()) / 1000.0;
        
        if (stats.totalTests > 0) {
            stats.averageTime = stats.totalTime / stats.totalTests;
        }
        
        PrintResults(stats);
        return stats;
    }
    
    /**
     * @brief 获取测试套件名称
     * @return 测试套件名称
     */
    const std::string& GetSuiteName() const {
        return suiteName_;
    }
    
    /**
     * @brief 清空所有测试用例
     */
    void Clear() {
        testCases_.clear();
    }
    
private:
    std::string suiteName_;                    ///< 测试套件名称
    std::vector<TestCase> testCases_;          ///< 测试用例列表
    
    /**
     * @brief 打印测试结果
     * @param stats 测试统计信息
     */
    void PrintResults(const TestStats& stats) {
        std::cout << std::string(50, '-') << std::endl;
        std::cout << "测试结果统计:" << std::endl;
        std::cout << "  总数: " << stats.totalTests << std::endl;
        std::cout << "  通过: " << stats.passedTests << std::endl;
        std::cout << "  失败: " << stats.failedTests << std::endl;
        std::cout << "  跳过: " << stats.skippedTests << std::endl;
        std::cout << "  总耗时: " << stats.totalTime << "ms" << std::endl;
        std::cout << "  平均耗时: " << stats.averageTime << "ms" << std::endl;
        
        if (stats.failedTests == 0) {
            std::cout << "✅ 所有测试通过!" << std::endl;
        } else {
            std::cout << "❌ 有 " << stats.failedTests << " 个测试失败!" << std::endl;
        }
        std::cout << "=== 测试套件结束 ===" << std::endl;
    }
};

/**
 * @brief 测试断言宏
 */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cout << "断言失败: " << message << std::endl; \
            std::cout << "文件: " << __FILE__ << ", 行: " << __LINE__ << std::endl; \
            return Engine::Test::TestResult::Failed; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(expected, actual, message) \
    do { \
        if ((expected) != (actual)) { \
            std::cout << "断言失败: " << message << std::endl; \
            std::cout << "期望值: " << (expected) << ", 实际值: " << (actual) << std::endl; \
            std::cout << "文件: " << __FILE__ << ", 行: " << __LINE__ << std::endl; \
            return Engine::Test::TestResult::Failed; \
        } \
    } while(0)

#define TEST_ASSERT_NE(expected, actual, message) \
    do { \
        if ((expected) == (actual)) { \
            std::cout << "断言失败: " << message << std::endl; \
            std::cout << "值不应该相等: " << (expected) << std::endl; \
            std::cout << "文件: " << __FILE__ << ", 行: " << __LINE__ << std::endl; \
            return Engine::Test::TestResult::Failed; \
        } \
    } while(0)

#define TEST_ASSERT_STR_EQ(expected, actual, message) \
    do { \
        if (std::string(expected) != std::string(actual)) { \
            std::cout << "断言失败: " << message << std::endl; \
            std::cout << "期望值: " << (expected) << ", 实际值: " << (actual) << std::endl; \
            std::cout << "文件: " << __FILE__ << ", 行: " << __LINE__ << std::endl; \
            return Engine::Test::TestResult::Failed; \
        } \
    } while(0)

#define TEST_ASSERT_NULL(ptr, message) \
    do { \
        if ((ptr) != nullptr) { \
            std::cout << "断言失败: " << message << std::endl; \
            std::cout << "期望为null，实际为: " << (ptr) << std::endl; \
            std::cout << "文件: " << __FILE__ << ", 行: " << __LINE__ << std::endl; \
            return Engine::Test::TestResult::Failed; \
        } \
    } while(0)

#define TEST_ASSERT_NOT_NULL(ptr, message) \
    do { \
        if ((ptr) == nullptr) { \
            std::cout << "断言失败: " << message << std::endl; \
            std::cout << "期望不为null" << std::endl; \
            std::cout << "文件: " << __FILE__ << ", 行: " << __LINE__ << std::endl; \
            return Engine::Test::TestResult::Failed; \
        } \
    } while(0)

#define TEST_SKIP(message) \
    do { \
        std::cout << "测试跳过: " << message << std::endl; \
        return Engine::Test::TestResult::Skipped; \
    } while(0)

/**
 * @brief 创建测试用例宏
 */
#define TEST_CASE(suite, name, func) \
    suite.AddTestCase(Engine::Test::TestCase(name, func, #name))

/**
 * @brief 测试运行器
 */
class TestRunner {
public:
    /**
     * @brief 注册测试套件
     * @param testSuite 测试套件
     */
    static void RegisterTestSuite(std::shared_ptr<TestSuite> testSuite) {
        testSuites_.push_back(testSuite);
    }
    
    /**
     * @brief 运行所有注册的测试套件
     * @return 总体统计信息
     */
    static TestStats RunAllSuites() {
        TestStats totalStats;
        
        std::cout << "\n🚀 开始运行所有测试套件" << std::endl;
        std::cout << "注册的测试套件数量: " << testSuites_.size() << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        
        auto overallStartTime = std::chrono::high_resolution_clock::now();
        
        for (auto& testSuite : testSuites_) {
            TestStats suiteStats = testSuite->RunAllTests();
            
            totalStats.totalTests += suiteStats.totalTests;
            totalStats.passedTests += suiteStats.passedTests;
            totalStats.failedTests += suiteStats.failedTests;
            totalStats.skippedTests += suiteStats.skippedTests;
            totalStats.totalTime += suiteStats.totalTime;
        }
        
        auto overallEndTime = std::chrono::high_resolution_clock::now();
        auto overallDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            overallEndTime - overallStartTime);
        totalStats.totalTime = static_cast<double>(overallDuration.count()) / 1000.0;
        
        if (totalStats.totalTests > 0) {
            totalStats.averageTime = totalStats.totalTime / totalStats.totalTests;
        }
        
        PrintOverallResults(totalStats);
        return totalStats;
    }
    
    /**
     * @brief 清空所有测试套件
     */
    static void ClearAllSuites() {
        testSuites_.clear();
    }
    
private:
    static std::vector<std::shared_ptr<TestSuite>> testSuites_;
    
    /**
     * @brief 打印总体测试结果
     * @param stats 总体统计信息
     */
    static void PrintOverallResults(const TestStats& stats) {
        std::cout << std::string(60, '=') << std::endl;
        std::cout << "📊 总体测试结果:" << std::endl;
        std::cout << "  总测试数: " << stats.totalTests << std::endl;
        std::cout << "  通过: " << stats.passedTests << " (" 
                  << (stats.totalTests > 0 ? (stats.passedTests * 100.0 / stats.totalTests) : 0.0) 
                  << "%)" << std::endl;
        std::cout << "  失败: " << stats.failedTests << " (" 
                  << (stats.totalTests > 0 ? (stats.failedTests * 100.0 / stats.totalTests) : 0.0) 
                  << "%)" << std::endl;
        std::cout << "  跳过: " << stats.skippedTests << " (" 
                  << (stats.totalTests > 0 ? (stats.skippedTests * 100.0 / stats.totalTests) : 0.0) 
                  << "%)" << std::endl;
        std::cout << "  总耗时: " << stats.totalTime << "ms" << std::endl;
        std::cout << "  平均耗时: " << stats.averageTime << "ms" << std::endl;
        
        if (stats.failedTests == 0) {
            std::cout << "🎉 所有测试都通过了!" << std::endl;
        } else {
            std::cout << "⚠️  有 " << stats.failedTests << " 个测试失败!" << std::endl;
        }
        std::cout << std::string(60, '=') << std::endl;
    }
};

// 静态成员定义
std::vector<std::shared_ptr<TestSuite>> TestRunner::testSuites_;

} // namespace Test
} // namespace Engine