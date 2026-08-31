#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace primal::content
{
    // Convert ContentTools' cooked scene container into the geometry blob
    // consumed by create_resource(asset_type::mesh). The parser is fully
    // bounds checked so untrusted/truncated files never reach assert-driven
    // legacy blob readers.
    bool normalize_cooked_mesh_scene(
        const void* data,
        std::size_t size,
        std::vector<std::uint8_t>& geometry_blob,
        std::string* error = nullptr);
}
