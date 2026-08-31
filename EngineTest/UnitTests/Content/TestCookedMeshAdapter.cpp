#include "Engine/Content/CookedMeshAdapter.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
    template<typename T>
    void write(std::vector<std::uint8_t>& blob, const T& value)
    {
        const auto offset = blob.size();
        blob.resize(offset + sizeof(T));
        std::memcpy(blob.data() + offset, &value, sizeof(T));
    }

    void write_string(std::vector<std::uint8_t>& blob, const char* value)
    {
        const auto size = static_cast<std::uint32_t>(std::strlen(value));
        write(blob, size);
        blob.insert(blob.end(), value, value + size);
    }

    void write_mesh(std::vector<std::uint8_t>& blob, std::uint32_t lod_id, float threshold)
    {
        write_string(blob, "triangle");
        write(blob, lod_id);
        write(blob, std::uint32_t{ 0 }); // material
        write(blob, std::uint32_t{ 0 }); // element size
        write(blob, std::uint32_t{ 0 }); // element type
        write(blob, std::uint32_t{ 3 }); // vertices
        write(blob, std::uint32_t{ 2 }); // index size
        write(blob, std::uint32_t{ 3 }); // indices
        write(blob, threshold);
        blob.resize(blob.size() + 36, 0); // positions
        blob.resize(blob.size() + 6, 0);  // indices
        write(blob, std::uint32_t{ 0x4C48534D });
        write(blob, std::uint32_t{ 0 }); // meshlets
        write(blob, std::uint32_t{ 0 }); // meshlet vertices
        write(blob, std::uint32_t{ 0 }); // meshlet triangles
        write(blob, std::uint32_t{ 0x20464453 });
        blob.resize(blob.size() + 12 + 24, 0); // SDF resolution + bounds
        write(blob, std::uint32_t{ 0 });
        write(blob, std::uint32_t{ 0 });
        write(blob, std::uint32_t{ 0 });
    }

    std::vector<std::uint8_t> make_scene()
    {
        std::vector<std::uint8_t> blob;
        write_string(blob, ""); // Empty scene name reproduced the editor crash.
        write(blob, std::uint32_t{ 0 }); // materials
        write(blob, std::uint32_t{ 1 }); // source LOD groups
        write_string(blob, "");
        write(blob, std::uint32_t{ 1 });
        write_mesh(blob, 0, 0.0f);
        return blob;
    }

    std::uint32_t read_u32(const std::vector<std::uint8_t>& blob, std::size_t offset)
    {
        std::uint32_t value{};
        std::memcpy(&value, blob.data() + offset, sizeof(value));
        return value;
    }
}

int main(int argc, char** argv)
{
    auto scene = make_scene();
    std::vector<std::uint8_t> geometry;
    std::string error;

    assert(primal::content::normalize_cooked_mesh_scene(
        scene.data(), scene.size(), geometry, &error));
    assert(error.empty());
    assert(read_u32(geometry, 0) == 1);  // normalized lod_count
    assert(read_u32(geometry, 8) == 1);  // submesh_count
    assert(!primal::content::normalize_cooked_mesh_scene(
        scene.data(), scene.size() - 1, geometry, &error));
    assert(geometry.empty());

    if (argc == 2)
    {
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        assert(file);
        const auto size = file.tellg();
        assert(size > 0);
        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> real_blob(static_cast<std::size_t>(size));
        assert(file.read(reinterpret_cast<char*>(real_blob.data()), size));
        if (!primal::content::normalize_cooked_mesh_scene(
            real_blob.data(), real_blob.size(), geometry, &error))
        {
            std::cerr << "Real cooked mesh rejected: " << error << '\n';
            return 2;
        }
        assert(!geometry.empty());
    }

    std::cout << "CookedMeshAdapter tests passed\n";
    return 0;
}
