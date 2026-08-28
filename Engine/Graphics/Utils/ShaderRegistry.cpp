#include "Graphics/Utils/ShaderRegistry.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace primal::graphics::utils {

namespace {

std::filesystem::path GetExecutableDir() {
#if defined(_WIN32)
    char buffer[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, buffer, MAX_PATH) > 0)
        return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return std::filesystem::path(buffer).parent_path();
#else
    char buffer[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return {};
}

// Check dir/relPath, walking up through ancestors until the filesystem root.
// Returns the first existing match, or an empty path.
std::filesystem::path FindFromAnchor(const std::filesystem::path& anchor,
                                     const std::filesystem::path& relPath) {
    std::error_code ec;
    std::filesystem::path dir = anchor.empty() ? std::filesystem::current_path(ec) : anchor;
    for (int i = 0; i < 16 && !dir.empty(); ++i) {
        std::filesystem::path candidate = dir / relPath;
        if (std::filesystem::exists(candidate, ec)) return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

} // namespace

std::string ShaderRegistry::ResolveShaderPath(const std::string& relPath) {
    const std::filesystem::path rel(relPath);
    std::error_code ec;

    if (const char* root = std::getenv("PRIMAL_SHADER_ROOT")) {
        std::filesystem::path candidate = std::filesystem::path(root) / rel;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }

    // CWD-relative (and ancestors, so launching from a build subdirectory of
    // the repo still resolves).
    if (auto found = FindFromAnchor(std::filesystem::current_path(ec), rel); !found.empty())
        return found.string();

    if (auto found = FindFromAnchor(GetExecutableDir(), rel); !found.empty())
        return found.string();

    return relPath;
}

} // namespace primal::graphics::utils
