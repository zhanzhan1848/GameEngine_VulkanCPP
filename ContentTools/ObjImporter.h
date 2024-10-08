#pragma once

#include "ToolsCommon.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "Content/tiny_obj_loader.h"

namespace primal::tools
{
    struct scene_data;
    struct scene;
    struct mesh;
    struct lod_group;
    struct geeometry_import_settings;

    class obj_context
    {
    public:
        obj_context(const char* file, scene* scene, scene_data* data, progression *const progression)
            : _scene{ scene }, _scene_data{ data }, _progression{ progression }
        {
            assert(file && _scene && _scene_data && _progression);
            load_obj_file(file);
        }

        ~obj_context()
        {
            ZeroMemory(this, sizeof(obj_context));
        }

        void get_scene();

        constexpr f32 scene_scale() const { return _scene_scale; }
        constexpr progression* get_progression() const { return _progression; }

    private:

        void load_obj_file(const char* file);
        void get_meshes(utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold);
        void get_mesh(utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold);
        void get_lod_group(const utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold);
        bool get_mesh_data(tinyobj::shape_t* shape, mesh& m);

        scene*							        _scene{ nullptr };
        scene_data*						        _scene_data{ nullptr };
        progression*					        _progression{ nullptr };
        f32								        _scene_scale{ 1.0f };
        tinyobj::attrib_t                       _attribute;
        std::vector<tinyobj::shape_t>           _shapes;
        std::vector<tinyobj::material_t>        _materials;
    };

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