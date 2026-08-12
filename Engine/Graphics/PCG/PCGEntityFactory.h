#pragma once

#include "Graphics/PCG/PCGTypes.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Material.h"
#include "Graphics/Material/ShaderTechnique.h"
#include "EngineAPI/GameEntity.h"
#include <vector>

namespace primal::graphics::pcg {

// Converts a PCGPointSet into ECS Entities (Transform only).
// Each PCG point becomes a game_entity with Transform component.
// The mesh_slot_index maps to ForwardSceneRenderer's mesh_infos_ array for rendering.
//
// Data flow:
//   PCGPointSet → CreateEntities() → Entity IDs + mesh_slot_indices
//       Entity IDs are used by StandardRenderPipeline to sync RenderProxies to RenderScene
//       mesh_slot_indices map each Entity to a loaded mesh in ForwardSceneRenderer
//
// Usage:
//   auto result = PCGEntityFactory::CreateEntities(pointSet);
//   // result.entity_ids.size() == pointSet.count
//   // result.mesh_slot_indices[i] == MeshIndex attribute of point i
class PCGEntityFactory {
public:
    struct CreateResult {
        std::vector<id::id_type> entity_ids;
        std::vector<u32> mesh_slot_indices;
    };

    static CreateResult CreateEntities(const PCGPointSet& points) {
        CreateResult result;
        result.entity_ids.reserve(points.count);
        result.mesh_slot_indices.reserve(points.count);

        for (u32 i = 0; i < points.count; ++i) {
            f32 sx = points.GetAttr(i, PCGAttr::ScaleX);
            f32 sy = points.GetAttr(i, PCGAttr::ScaleY);
            f32 sz = points.GetAttr(i, PCGAttr::ScaleZ);
            if (sx == 0.0f) sx = 1.0f;
            if (sy == 0.0f) sy = 1.0f;
            if (sz == 0.0f) sz = 1.0f;

            f32 angle = points.GetAttr(i, PCGAttr::RotationY);
            const auto& pos = points.positions[i];

            // Euler → quaternion (RotationY only)
            f32 half_angle = angle * 0.5f;
            f32 cos_ha = std::cos(half_angle);
            f32 sin_ha = std::sin(half_angle);

            // Create Entity with Transform
            transform::init_info transform_info{};
            transform_info.position[0] = pos.x;
            transform_info.position[1] = pos.y;
            transform_info.position[2] = pos.z;
            transform_info.rotation[0] = 0.0f;         // x
            transform_info.rotation[1] = sin_ha;        // y
            transform_info.rotation[2] = 0.0f;         // z
            transform_info.rotation[3] = cos_ha;        // w
            transform_info.scale[0] = sx;
            transform_info.scale[1] = sy;
            transform_info.scale[2] = sz;

            game_entity::entity_info entity_info{};
            entity_info.transform = &transform_info;

            material::init_info mat_info{}; // default Opaque
            u32 tech_val = static_cast<u32>(points.GetAttr(i, PCGAttr::TechniqueIndex));
            mat_info.technique = static_cast<graphics::ShaderTechnique>(
                std::min(tech_val, static_cast<u32>(graphics::ShaderTechnique::Count) - 1));

            // Per-point tint. WFCOutput writes linear-RGB into BaseColorR/G/B
            // for ruins tiles; primitives leave the slots at 0 (init default),
            // which we map to 1.0 so the GBuffer shader's albedo multiplier
            // stays neutral white for untinted points.
            f32 cr = points.GetAttr(i, PCGAttr::BaseColorR);
            f32 cg = points.GetAttr(i, PCGAttr::BaseColorG);
            f32 cb = points.GetAttr(i, PCGAttr::BaseColorB);
            if (cr == 0.0f) cr = 1.0f;
            if (cg == 0.0f) cg = 1.0f;
            if (cb == 0.0f) cb = 1.0f;
            mat_info.base_color[0] = cr;
            mat_info.base_color[1] = cg;
            mat_info.base_color[2] = cb;
            mat_info.base_color[3] = 1.0f;
            entity_info.material = &mat_info;

            game_entity::entity entity = game_entity::create(entity_info);
            result.entity_ids.push_back(entity.get_id());
            result.mesh_slot_indices.push_back(
                static_cast<u32>(points.GetAttr(i, PCGAttr::MeshIndex)));
        }

        return result;
    }

    static void DestroyEntities(const std::vector<id::id_type>& ids) {
        for (auto id : ids) {
            if (game_entity::is_alive(game_entity::entity_id{id})) {
                game_entity::remove(game_entity::entity_id{id});
            }
        }
    }
};

} // namespace primal::graphics::pcg
