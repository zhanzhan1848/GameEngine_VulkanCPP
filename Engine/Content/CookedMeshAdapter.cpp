#include "CookedMeshAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace primal::content
{
    namespace
    {
        constexpr std::uint32_t meshlet_magic = 0x4C48534D; // "MSHL"
        constexpr std::uint32_t sdf_magic = 0x20464453;     // "SDF "
        // A9 Lightmap UV sidecar ("UVLM"), flagged by bit 0x10 in the mesh
        // header's elements_type (ContentTools Geometry.h
        // elements_type::lightmap_uv_flag).
        constexpr std::uint32_t lightmap_uv_flag = 0x10;
        constexpr std::uint32_t lightmap_uv_magic = 0x4D4C5655; // "UVLM"
        constexpr std::size_t serialized_meshlet_size = 60;
        constexpr std::uint32_t max_collection_count = 1u << 20;

        struct cursor
        {
            const std::uint8_t* data;
            std::size_t size;
            std::size_t offset{ 0 };

            bool read_u32(std::uint32_t& value)
            {
                if (size - offset < sizeof(value)) return false;
                std::memcpy(&value, data + offset, sizeof(value));
                offset += sizeof(value);
                return true;
            }

            bool read_f32(float& value)
            {
                if (size - offset < sizeof(value)) return false;
                std::memcpy(&value, data + offset, sizeof(value));
                offset += sizeof(value);
                return true;
            }

            bool skip(std::size_t bytes)
            {
                if (bytes > size - offset) return false;
                offset += bytes;
                return true;
            }

            bool skip_string()
            {
                std::uint32_t length{};
                return read_u32(length) && skip(length);
            }
        };

        struct mesh_record
        {
            std::size_t begin;
            std::size_t end;
            std::uint32_t group_index;
            std::uint32_t lod_id;
            float threshold;
        };

        bool checked_size(std::uint32_t count, std::size_t stride, std::size_t& result)
        {
            if (stride != 0 && count > std::numeric_limits<std::size_t>::max() / stride)
                return false;
            result = static_cast<std::size_t>(count) * stride;
            return true;
        }

        bool skip_counted_bytes(cursor& input, std::size_t stride)
        {
            std::uint32_t count{};
            std::size_t bytes{};
            return input.read_u32(count) &&
                count <= max_collection_count * 64u &&
                checked_size(count, stride, bytes) &&
                input.skip(bytes);
        }

        bool read_mesh(cursor& input, mesh_record& record)
        {
            record.begin = input.offset;
            if (!input.skip_string()) return false;

            std::uint32_t material_index{};
            std::uint32_t element_size{};
            std::uint32_t element_type{};
            std::uint32_t vertex_count{};
            std::uint32_t index_size{};
            std::uint32_t index_count{};
            if (!input.read_u32(record.lod_id) ||
                !input.read_u32(material_index) ||
                !input.read_u32(element_size) ||
                !input.read_u32(element_type) ||
                !input.read_u32(vertex_count) ||
                !input.read_u32(index_size) ||
                !input.read_u32(index_count) ||
                !input.read_f32(record.threshold))
                return false;

            (void)material_index;
            // element_type: low 4 bits are the canonical format; bit 0x10
            // flags the A9 lightmap UV sidecar section and is consumed below.
            if (element_size > 256 || vertex_count == 0 || vertex_count > 10000000 ||
                (index_size != 2 && index_size != 4) ||
                index_count == 0 || index_count > 30000000 ||
                !std::isfinite(record.threshold))
                return false;

            std::size_t position_bytes{};
            std::size_t element_bytes{};
            std::size_t index_bytes{};
            if (!checked_size(vertex_count, 12, position_bytes) ||
                !checked_size(vertex_count, element_size, element_bytes) ||
                !checked_size(index_count, index_size, index_bytes) ||
                !input.skip(position_bytes) ||
                !input.skip(element_bytes) ||
                !input.skip(index_bytes))
                return false;

            std::uint32_t magic{};
            std::uint32_t meshlet_count{};
            std::size_t meshlet_bytes{};
            if (!input.read_u32(magic) || magic != meshlet_magic ||
                !input.read_u32(meshlet_count) || meshlet_count > max_collection_count ||
                !checked_size(meshlet_count, serialized_meshlet_size, meshlet_bytes) ||
                !input.skip(meshlet_bytes) ||
                !skip_counted_bytes(input, sizeof(std::uint32_t)) ||
                !skip_counted_bytes(input, sizeof(std::uint8_t)))
                return false;

            if (!input.read_u32(magic) || magic != sdf_magic ||
                !input.skip(sizeof(std::uint32_t) * 3 + sizeof(float) * 6) ||
                !skip_counted_bytes(input, sizeof(std::uint16_t)) ||
                !skip_counted_bytes(input, sizeof(std::uint8_t)) ||
                !skip_counted_bytes(input, sizeof(std::uint16_t)))
                return false;

            // A9: lightmap UV sidecar, only when flagged in the header.
            // Full-coverage invariant: uv_count == vertex_count.
            if (element_type & lightmap_uv_flag)
            {
                std::uint32_t uv_magic{};
                std::uint32_t uv_count{};
                if (!input.read_u32(uv_magic) || uv_magic != lightmap_uv_magic ||
                    !input.read_u32(uv_count) || uv_count != vertex_count ||
                    !input.skip(static_cast<std::size_t>(uv_count) * 2 * sizeof(float)))
                    return false;
            }

            record.end = input.offset;
            return true;
        }

        bool validate_normalized_geometry(const std::uint8_t* data, std::size_t size)
        {
            cursor input{ data, size };
            std::uint32_t lod_count{};
            if (!input.read_u32(lod_count) || lod_count == 0 || lod_count >= (1u << 16))
                return false;

            float previous_threshold{};
            bool have_previous_threshold = false;
            for (std::uint32_t lod = 0; lod < lod_count; ++lod)
            {
                float threshold{};
                std::uint32_t mesh_count{};
                std::uint32_t declared_body_size{};
                if (!input.read_f32(threshold) || !std::isfinite(threshold) ||
                    !input.read_u32(mesh_count) || mesh_count == 0 ||
                    mesh_count > max_collection_count ||
                    !input.read_u32(declared_body_size))
                    return false;
                if (have_previous_threshold && threshold <= previous_threshold) return false;
                previous_threshold = threshold;
                have_previous_threshold = true;

                const auto bodies_begin = input.offset;
                for (std::uint32_t mesh = 0; mesh < mesh_count; ++mesh)
                {
                    mesh_record record{};
                    if (!read_mesh(input, record)) return false;
                }
                const auto actual_body_size = input.offset - bodies_begin;
                // Older cookers wrote zero here; newer cookers write the
                // exact byte count. Accept both, reject conflicting values.
                if (declared_body_size != 0 && declared_body_size != actual_body_size)
                    return false;
            }
            return input.offset == input.size;
        }

        template<typename T>
        void append_value(std::vector<std::uint8_t>& output, const T& value)
        {
            const auto begin = output.size();
            output.resize(begin + sizeof(T));
            std::memcpy(output.data() + begin, &value, sizeof(T));
        }

        void append_bytes(
            std::vector<std::uint8_t>& output,
            const std::uint8_t* data,
            std::size_t size)
        {
            const auto begin = output.size();
            output.resize(begin + size);
            std::memcpy(output.data() + begin, data, size);
        }

        void set_error(std::string* error, const char* message)
        {
            if (error) *error = message;
        }
    }

    bool normalize_cooked_mesh_scene(
        const void* data,
        std::size_t size,
        std::vector<std::uint8_t>& geometry_blob,
        std::string* error)
    {
        geometry_blob.clear();
        if (!data || size < sizeof(std::uint32_t) * 3)
        {
            set_error(error, "cooked mesh is empty or truncated");
            return false;
        }

        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (validate_normalized_geometry(bytes, size))
        {
            geometry_blob.assign(bytes, bytes + size);
            return true;
        }

        cursor input{ bytes, size };
        if (!input.skip_string())
        {
            set_error(error, "invalid scene name");
            return false;
        }

        std::uint32_t material_count{};
        if (!input.read_u32(material_count) || material_count > max_collection_count)
        {
            set_error(error, "invalid material count");
            return false;
        }
        for (std::uint32_t i = 0; i < material_count; ++i)
        {
            // Current ContentTools scene format stores name, diffuse and normal.
            if (!input.skip_string() || !input.skip_string() || !input.skip_string())
            {
                set_error(error, "invalid material table");
                return false;
            }
        }

        std::uint32_t source_lod_group_count{};
        if (!input.read_u32(source_lod_group_count) ||
            source_lod_group_count == 0 || source_lod_group_count > max_collection_count)
        {
            set_error(error, "invalid LOD group count");
            return false;
        }

        std::vector<mesh_record> meshes;
        for (std::uint32_t lod = 0; lod < source_lod_group_count; ++lod)
        {
            std::uint32_t mesh_count{};
            if (!input.skip_string() || !input.read_u32(mesh_count) ||
                mesh_count == 0 || mesh_count > max_collection_count)
            {
                set_error(error, "invalid mesh group");
                return false;
            }
            for (std::uint32_t mesh = 0; mesh < mesh_count; ++mesh)
            {
                mesh_record record{};
                if (!read_mesh(input, record))
                {
                    set_error(error, "invalid packed mesh body");
                    return false;
                }
                record.group_index = lod;
                meshes.push_back(record);
            }
        }

        if (meshes.empty() || input.offset != input.size)
        {
            set_error(error, meshes.empty() ? "scene contains no meshes" : "unexpected trailing scene data");
            return false;
        }

        if (source_lod_group_count >= (1u << 16))
        {
            set_error(error, "too many normalized LOD groups");
            return false;
        }

        float previous_threshold{};
        bool have_previous_threshold = false;
        for (std::uint32_t group_index = 0; group_index < source_lod_group_count; ++group_index)
        {
            const auto first = std::find_if(meshes.begin(), meshes.end(),
                [group_index](const mesh_record& mesh) { return mesh.group_index == group_index; });
            if (first == meshes.end()) return false;
            if (have_previous_threshold && first->threshold <= previous_threshold)
            {
                set_error(error, "LOD thresholds are not strictly increasing");
                return false;
            }
            previous_threshold = first->threshold;
            have_previous_threshold = true;
        }

        append_value(geometry_blob, source_lod_group_count);
        for (std::uint32_t group_index = 0; group_index < source_lod_group_count; ++group_index)
        {
            const auto first = std::find_if(meshes.begin(), meshes.end(),
                [group_index](const mesh_record& mesh) { return mesh.group_index == group_index; });
            const auto count = static_cast<std::uint32_t>(std::count_if(
                meshes.begin(), meshes.end(),
                [group_index](const mesh_record& mesh) { return mesh.group_index == group_index; }));

            std::size_t bodies_size = 0;
            for (const auto& mesh : meshes)
                if (mesh.group_index == group_index) bodies_size += mesh.end - mesh.begin;
            if (bodies_size > std::numeric_limits<std::uint32_t>::max())
            {
                set_error(error, "normalized mesh group is too large");
                geometry_blob.clear();
                return false;
            }

            append_value(geometry_blob, first->threshold);
            append_value(geometry_blob, count);
            append_value(geometry_blob, static_cast<std::uint32_t>(bodies_size));
            for (const auto& mesh : meshes)
            {
                if (mesh.group_index == group_index)
                    append_bytes(geometry_blob, input.data + mesh.begin, mesh.end - mesh.begin);
            }
        }
        return true;
    }
}
