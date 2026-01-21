#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <vector>

namespace primal::graphics::utils {

class ShaderLoader {
public:
    static std::string LoadShaderSource(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

} // namespace primal::graphics::utils
