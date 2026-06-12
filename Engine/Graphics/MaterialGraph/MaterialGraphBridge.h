#pragma once

#include "Graphics/MaterialGraph/MaterialGraph.h"
#include "Graphics/MaterialGraph/Nodes/MaterialOutputNode.h"
#include "Graphics/Material/ShaderTechnique.h"
#include "Components/Material.h"
#include <algorithm>

namespace primal::graphics::material_graph {

struct MaterialGraphEvalResult {
    material::init_info info;
    bool valid{false};
    std::string error_message;
};

class MaterialGraphBridge {
public:
    static MaterialGraphEvalResult Evaluate(MaterialGraph& graph) {
        MaterialGraphEvalResult result;

        graph.ClearErrors();
        graph.Execute();

        if (!graph.GetErrors().empty()) {
            result.error_message = graph.GetErrors()[0].message;
            return result;
        }

        MaterialOutputNode* output = nullptr;
        for (auto& node : graph.GetNodes()) {
            if (std::strcmp(node->TypeName(), "MaterialOutput") == 0) {
                output = static_cast<MaterialOutputNode*>(node.get());
                break;
            }
        }
        if (!output) {
            result.error_message = "No MaterialOutput node found in graph";
            return result;
        }

        result.info = {};

        // Pin 0: BaseColor (Float4)
        auto* base_color = output->inputs[0].AsFloat4();
        if (base_color) {
            result.info.base_color[0] = base_color->value.x;
            result.info.base_color[1] = base_color->value.y;
            result.info.base_color[2] = base_color->value.z;
            result.info.base_color[3] = base_color->value.w;
        }

        // Pin 1: Roughness (Float)
        auto* roughness = output->inputs[1].AsFloat();
        if (roughness) result.info.roughness = roughness->value;

        // Pin 2: Metallic (Float)
        auto* metallic = output->inputs[2].AsFloat();
        if (metallic) result.info.metallic = metallic->value;

        // Pin 3: AlphaCutoff (Float)
        auto* alpha = output->inputs[3].AsFloat();
        if (alpha) result.info.alpha_cutoff = alpha->value;

        // Pin 4: AlbedoTex (Texture2D)
        auto* albedo_tex = output->inputs[4].AsTexture();
        if (albedo_tex) result.info.albedo_texture = albedo_tex->texture_id;

        // Pin 5: NormalTex (Texture2D)
        auto* normal_tex = output->inputs[5].AsTexture();
        if (normal_tex) result.info.normal_texture = normal_tex->texture_id;

        // Pin 6: ORMTex (Texture2D)
        auto* orm_tex = output->inputs[6].AsTexture();
        if (orm_tex) result.info.orm_texture = orm_tex->texture_id;

        // Pin 7: Technique (Float, cast to enum)
        auto* technique = output->inputs[7].AsFloat();
        if (technique) {
            u32 tech_val = static_cast<u32>(technique->value);
            u32 max_val = static_cast<u32>(ShaderTechnique::TechniqueCount) - 1;
            result.info.technique = static_cast<ShaderTechnique>(std::min(tech_val, max_val));
        }

        result.valid = true;
        return result;
    }
};

} // namespace primal::graphics::material_graph
