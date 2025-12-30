/**
 * @file Main.cpp
 * @brief 测试程序主入口点
 * @details 运行所有RHI核心模块的单元测试和性能测试
 * 
 * @author RHI开发团队
 * @date 2025-12-29
 */

#include "TestFramework.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <string>
#include <vector>

/**
 * @brief 测试程序主函数
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 程序退出代码
 */
int main(int argc, char* argv[])
{
    std::cout << "========================================\n";
    std::cout << "    RHI核心模块测试程序\n";
    std::cout << "========================================\n\n";
    
    // 创建测试运行器
    Engine::Test::TestRunner runner;
    
    // 记录开始时间
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // 运行所有测试
    bool allTestsPassed = runner.RunAllSuites();
    
    // 记录结束时间
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // 输出测试结果统计
    std::cout << "\n========================================\n";
    std::cout << "    测试结果统计\n";
    std::cout << "========================================\n";
    std::cout << "总测试用例数: " << runner.GetTotalTestCount() << "\n";
    std::cout << "通过测试数: " << runner.GetPassedTestCount() << "\n";
    std::cout << "失败测试数: " << runner.GetFailedTestCount() << "\n";
    std::cout << "执行总时间: " << duration.count() << " ms\n";
    
    if (allTestsPassed) {
        std::cout << "\n✅ 所有测试通过！\n";
    } else {
        std::cout << "\n❌ 部分测试失败！\n";
    }
    
    std::cout << "========================================\n";
    
    return allTestsPassed ? 0 : 1;
}