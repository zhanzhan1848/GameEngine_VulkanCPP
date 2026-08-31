#include "EditorShaderManager.h"
#include "EditorShaders.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace primal::graphics
{
    namespace
    {
        std::filesystem::path g_work_dir;
        std::filesystem::path g_metallib_path;
        bool g_artifacts_alive = false;

        int run_command(const std::string& cmd)
        {
            std::fprintf(stderr, "[ESM] %s\n", cmd.c_str());
            return std::system(cmd.c_str());
        }

        bool write_source_file(const std::filesystem::path& path, const char* source)
        {
            std::ofstream f(path, std::ios::binary);
            if (!f.is_open()) {
                std::fprintf(stderr, "[ESM] failed to open %s for write\n", path.string().c_str());
                return false;
            }
            f.write(source, std::strlen(source));
            f.close();
            return f.good();
        }

        bool read_binary_file(const std::filesystem::path& path, std::vector<unsigned char>& out)
        {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f.is_open()) {
                std::fprintf(stderr, "[ESM] failed to open %s for read\n", path.string().c_str());
                return false;
            }
            const std::streamsize size = f.tellg();
            if (size <= 0) {
                std::fprintf(stderr, "[ESM] metallib %s is empty\n", path.string().c_str());
                return false;
            }
            out.resize(static_cast<std::size_t>(size));
            f.seekg(0, std::ios::beg);
            f.read(reinterpret_cast<char*>(out.data()), size);
            return f.good();
        }
    }

    EditorShaderManager& EditorShaderManager::Get()
    {
        static EditorShaderManager instance;
        return instance;
    }

    bool EditorShaderManager::Initialize()
    {
        if (!metallib_.empty()) return true;
        if (g_artifacts_alive && !g_metallib_path.empty() && std::filesystem::exists(g_metallib_path)) {
            return read_binary_file(g_metallib_path, metallib_);
        }

        const auto base = std::filesystem::temp_directory_path() / "primal_editor_shaders";
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base, ec);
        if (ec) {
            std::fprintf(stderr, "[ESM] failed to create work dir %s: %s\n",
                         base.string().c_str(), ec.message().c_str());
            return false;
        }
        g_work_dir = base;

        const auto metal_path = base / "editor_cube.metal";
        const auto air_path = base / "editor_cube.air";
        g_metallib_path = base / "editor_cube.metallib";

        if (!write_source_file(metal_path, primal::editor_shaders::kEditorCubeShaders)) {
            return false;
        }

        std::string compile_cmd = "xcrun -sdk macosx metal -c \"";
        compile_cmd += metal_path.string();
        compile_cmd += "\" -o \"";
        compile_cmd += air_path.string();
        compile_cmd += "\" -Wno-unused-variable 2>&1";
        if (run_command(compile_cmd) != 0) {
            std::fprintf(stderr, "[ESM] xcrun metal -c failed; fallback to inline MSL\n");
            return false;
        }

        std::string lib_cmd = "xcrun -sdk macosx metallib -o \"";
        lib_cmd += g_metallib_path.string();
        lib_cmd += "\" \"";
        lib_cmd += air_path.string();
        lib_cmd += "\" 2>&1";
        if (run_command(lib_cmd) != 0) {
            std::fprintf(stderr, "[ESM] xcrun metallib failed; fallback to inline MSL\n");
            return false;
        }

        if (!read_binary_file(g_metallib_path, metallib_)) {
            return false;
        }
        g_artifacts_alive = true;
        std::fprintf(stderr, "[ESM] EditorShaderManager initialized (%zu bytes at %s)\n",
                     metallib_.size(), g_metallib_path.string().c_str());
        return true;
    }

    const void* EditorShaderManager::GetMetallibBytes() const
    {
        return metallib_.empty() ? nullptr : metallib_.data();
    }

    std::uint64_t EditorShaderManager::GetMetallibSize() const
    {
        return metallib_.size();
    }

    void EditorShaderManager::Shutdown()
    {
        metallib_.clear();
        metallib_.shrink_to_fit();
        if (g_artifacts_alive && !g_work_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(g_work_dir, ec);
            g_artifacts_alive = false;
            g_work_dir.clear();
            g_metallib_path.clear();
        }
    }
}
