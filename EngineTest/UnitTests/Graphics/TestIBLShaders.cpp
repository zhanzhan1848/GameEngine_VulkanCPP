#include "TestFramework.h"
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>
#include <cstdlib>

namespace Engine::Test {

namespace fs = std::filesystem;

class IBLShaderTest {
public:
    static TestResult VerifyShaderExistence() {
        // Path relative to project root
        std::vector<std::string> shaders = {
            "Engine/Graphics/RHI/Shaders/IBL_Hammersley.metal",
            "Engine/Graphics/RHI/Shaders/IBL_IrradianceConvolution.metal",
            "Engine/Graphics/RHI/Shaders/IBL_SpecularPrefilter.metal",
            "Engine/Graphics/RHI/Shaders/IBL_BRDFIntegration.metal"
        };
        
#ifdef PROJECT_ROOT
        fs::path rootPath = PROJECT_ROOT;
#else
        // Try to find project root
        fs::path rootPath = fs::current_path();
        // Heuristic: walk up until we see 'Engine' directory
        int maxDepth = 10;
        while(maxDepth-- > 0 && !fs::exists(rootPath / "Engine")) {
            if(rootPath.has_parent_path())
                rootPath = rootPath.parent_path();
            else
                break;
        }
        
        if (!fs::exists(rootPath / "Engine")) {
            // Hardcoded fallback for this environment
            rootPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP";
        }
#endif

        bool allExist = true;
        for (const auto& shader : shaders) {
            fs::path fullPath = rootPath / shader;
            if (!fs::exists(fullPath)) {
                std::cerr << "Shader not found: " << fullPath << std::endl;
                allExist = false;
            }
        }
        
        return allExist ? TestResult::Passed : TestResult::Failed;
    }

    static TestResult VerifyShaderCompilation() {
#ifdef PROJECT_ROOT
        fs::path rootPath = PROJECT_ROOT;
#else
         // Find project root
        fs::path rootPath = fs::current_path();
        int maxDepth = 10;
        while(maxDepth-- > 0 && !fs::exists(rootPath / "Engine")) {
             if(rootPath.has_parent_path())
                rootPath = rootPath.parent_path();
            else
                break;
        }
         if (!fs::exists(rootPath / "Engine")) {
            rootPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP";
        }
#endif

        std::vector<std::string> shadersToCompile = {
            "Engine/Graphics/RHI/Shaders/IBL_IrradianceConvolution.metal",
            "Engine/Graphics/RHI/Shaders/IBL_SpecularPrefilter.metal",
            "Engine/Graphics/RHI/Shaders/IBL_BRDFIntegration.metal",
            "Engine/Graphics/RHI/Shaders/StandardPBR.metal"
        };
        
        bool allCompiled = true;
        for (const auto& shader : shadersToCompile) {
            fs::path sourcePath = rootPath / shader;
            std::string includePath = (rootPath / "Engine/Graphics/RHI/Shaders").string();
            
            // Construct metal command
            // xcrun -sdk macosx metal -c <file> -I <include_dir> -o /dev/null
            // Redirect stderr to stdout to see errors
            std::string cmd = "xcrun -sdk macosx metal -c \"" + sourcePath.string() + "\" -I \"" + includePath + "\" -o /dev/null 2>&1";
            
            std::cout << "Compiling: " << shader << "..." << std::endl;
            // Capture output
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                std::cerr << "Failed to run metal compiler" << std::endl;
                return TestResult::Failed;
            }
            char buffer[128];
            std::string result = "";
            while (!feof(pipe)) {
                if (fgets(buffer, 128, pipe) != NULL)
                    result += buffer;
            }
            int ret = pclose(pipe);
            
            if (ret != 0) {
                std::cerr << "Failed to compile: " << shader << "\nErrors:\n" << result << std::endl;
                allCompiled = false;
            } else {
                std::cout << "Success." << std::endl;
            }
        }
        
        return allCompiled ? TestResult::Passed : TestResult::Failed;
    }
};

void RegisterIBLTests(TestSuite& suite) {
    suite.AddTestCase(TestCase("IBL Shader Existence", IBLShaderTest::VerifyShaderExistence));
    suite.AddTestCase(TestCase("IBL Shader Compilation", IBLShaderTest::VerifyShaderCompilation));
}

} // namespace Engine::Test

#ifdef STANDALONE_TEST
int main() {
    Engine::Test::TestSuite suite("IBL Generation Tests");
    Engine::Test::RegisterIBLTests(suite);
    
    auto stats = suite.RunAllTests();
    
    return stats.failedTests > 0 ? 1 : 0;
}
#endif
