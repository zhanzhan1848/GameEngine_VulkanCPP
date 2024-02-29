#pragma once

#include "ToolsCommon.h"

namespace primal::tools
{
    struct Vertex
    {
        math::v3 pos;
        math::v3 color;
        math::v3 texCoord;
        math::v3 normal;
        math::v3 tangent;

        bool operator==(Vertex& v)
        {
            const f32 epsilon = 1.192e-07f;
            return abs(this->pos.x - v.pos.x) <= epsilon && abs(this->pos.y - v.pos.y) <= epsilon && abs(this->pos.z - v.pos.z) <= epsilon &&
                abs(this->color.x - v.color.x) <= epsilon && abs(this->color.y - v.color.y) <= epsilon && abs(this->color.z - v.color.z) <= epsilon &&
                abs(this->texCoord.x - v.texCoord.x) <= epsilon && abs(this->texCoord.y - v.texCoord.y) <= epsilon && abs(this->texCoord.z - v.texCoord.z) <= epsilon &&
                abs(this->normal.x - v.normal.x) <= epsilon && abs(this->normal.y - v.normal.y) <= epsilon && abs(this->normal.z - v.normal.z) <= epsilon;
        }
    };

    struct geometry_config
    {
        u32						vertex_size;
        u32						vertex_count;
        utl::vector<Vertex>		vertices;

        u32						index_size;
        u32						index_count;
        utl::vector<u32>		indices;

        math::v3				center;
        math::v3				min_extents;
        math::v3				max_extents;

        char					name[256];
        char					material_name[256];
        std::string             ambient_map;
        std::string             diffuse_map;
        std::string             specular_map;
        std::string             alpha_map;
        std::string             normal_map;

        void clear()
        {
            this->vertex_size = 0;
            this->vertex_count = 0;
            this->vertices.clear();
            this->index_size = 0;
            this->index_count = 0;
            this->indices.clear();
            this->center = math::v3{ 0, 0, 0 };
            this->min_extents = math::v3{ 0, 0, 0 };
            this->max_extents = math::v3{ 0, 0, 0 };
            memset(this->name, 0, 256);
            memset(this->material_name, 0, 256);
            this->ambient_map.clear();
            this->diffuse_map.clear();
            this->specular_map.clear();
            this->alpha_map.clear();
            this->normal_map.clear();
        }
    };

}