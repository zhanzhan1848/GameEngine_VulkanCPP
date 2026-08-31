#pragma once

#include <cstdint>
#include <vector>

namespace primal::graphics
{
    class EditorShaderManager
    {
    public:
        static EditorShaderManager& Get();

        // Compile the editor cube shader sources to a metallib, read the
        // bytes back, and cache them. Returns true on success; on failure
        // the metallib cache stays empty and callers should fall back to
        // the inline source path in EditorShaders.h.
        bool Initialize();

        const void* GetMetallibBytes() const;
        std::uint64_t GetMetallibSize() const;

        void Shutdown();

    private:
        EditorShaderManager() = default;
        ~EditorShaderManager() = default;
        EditorShaderManager(const EditorShaderManager&) = delete;
        EditorShaderManager& operator=(const EditorShaderManager&) = delete;

        std::vector<unsigned char> metallib_;
    };
}
