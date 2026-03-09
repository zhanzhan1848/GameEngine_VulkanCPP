#include "Cluster.h"
#include "Entity.h"
#include "Graphics/Nanite/NaniteResourceManager.h"
#include <iostream>

// Debug logging for reference counting operations
// Enable with -DCLUSTER_DEBUG_LOGGING compiler flag
#ifdef CLUSTER_DEBUG_LOGGING
#define CLUSTER_LOG(msg) std::cout << "[Cluster] " << msg << std::endl
#else
#define CLUSTER_LOG(msg) ((void)0)
#endif

namespace primal::cluster {

    namespace {
        utl::vector<u8>                    exists_flags;
        utl::vector<id::id_type>           geometry_ids;
        utl::vector<f32>                   lod_biases;
        utl::vector<s32>                   forced_lods;
        utl::vector<u32>                   visibility_flags;
        utl::vector<u32>                   cluster_counts;
        utl::vector<u32>                   state_flags;

        component_cache* component_caches{ nullptr };

        void allocate_cache() {
            assert(!component_caches);
            constexpr size_t initial_capacity{ 1024 };
            component_caches = new component_cache[initial_capacity];
        }

        void free_cache() {
            delete[] component_caches;
            component_caches = nullptr;
        }

        component_cache& get_cache(component c) {
            if (c >= exists_flags.size()) {
                exists_flags.resize(c + 1);
                geometry_ids.resize(c + 1);
                lod_biases.resize(c + 1);
                forced_lods.resize(c + 1);
                visibility_flags.resize(c + 1);
                cluster_counts.resize(c + 1);
                state_flags.resize(c + 1);
                if (component_caches) {
                    auto* new_caches = new component_cache[c + 1];
                    memcpy(new_caches, component_caches, sizeof(component_cache) * c);
                    delete[] component_caches;
                    component_caches = new_caches;
                }
            }
            return component_caches[c];
        }

        bool is_valid(component c) {
            return c != id::invalid_id && c < exists_flags.size() && exists_flags[c];
        }
    }

    component create(init_info info, game_entity::entity entity) {
        assert(entity.is_valid());
        assert(info.geometry_content_id != id::invalid_id);

        if (!component_caches) {
            allocate_cache();
        }

        const game_entity::entity_id id{ entity.get_id() };
        component c{ id };

        if (is_valid(c)) {
            return c;
        }

        if (info.geometry_content_id == id::invalid_id) {
            return id::invalid_id;
        }

        graphics::nanite::NaniteResourceManager::Get().AddGeometryRef(info.geometry_content_id);
        CLUSTER_LOG("create(): Added ref for geometry_id=" << info.geometry_content_id);

        if (exists_flags.size() <= id) {
            exists_flags.resize(id + 1);
            geometry_ids.resize(id + 1);
            lod_biases.resize(id + 1);
            forced_lods.resize(id + 1);
            visibility_flags.resize(id + 1);
            cluster_counts.resize(id + 1);
            state_flags.resize(id + 1);
        }

        exists_flags[id] = true;
        geometry_ids[id] = info.geometry_content_id;
        lod_biases[id] = info.lod_bias;
        forced_lods[id] = info.forced_lod;
        visibility_flags[id] = info.visibility_flags;

        auto& cache = get_cache(c);
        cache.geometry_content_id = info.geometry_content_id;
        cache.lod_bias = info.lod_bias;
        cache.forced_lod = info.forced_lod;
        cache.visibility_flags = info.visibility_flags;
        cache.cluster_count = 0;
        cache.flags = 0;
        cache.exists = true;

        return c;
    }

    void remove(component c) {
        if (!is_valid(c)) {
            return;
        }

        const id::id_type geometry_id{ geometry_ids[c] };

        if (geometry_id != id::invalid_id) {
            graphics::nanite::NaniteResourceManager::Get().ReleaseGeometryRef(geometry_id);
            CLUSTER_LOG("remove(): Released ref for geometry_id=" << geometry_id);
        }

        exists_flags[c] = false;
        geometry_ids[c] = id::invalid_id;

        auto& cache = get_cache(c);
        cache.exists = false;
        cache.geometry_content_id = id::invalid_id;
    }

    const component_cache* get(component c) {
        if (!is_valid(c)) {
            return nullptr;
        }

        return &get_cache(c);
    }

    void set_geometry(component c, id::id_type new_geometry_content_id) {
        if (!is_valid(c)) {
            return;
        }

        const id::id_type old_geometry_id{ geometry_ids[c] };

        if (old_geometry_id == new_geometry_content_id) {
            return;
        }

        if (old_geometry_id != id::invalid_id) {
            graphics::nanite::NaniteResourceManager::Get().ReleaseGeometryRef(old_geometry_id);
            CLUSTER_LOG("set_geometry(): Released ref for old geometry_id=" << old_geometry_id);
        }

        if (new_geometry_content_id != id::invalid_id) {
            graphics::nanite::NaniteResourceManager::Get().AddGeometryRef(new_geometry_content_id);
            CLUSTER_LOG("set_geometry(): Added ref for new geometry_id=" << new_geometry_content_id);
        }

        geometry_ids[c] = new_geometry_content_id;

        auto& cache = get_cache(c);
        cache.geometry_content_id = new_geometry_content_id;
        cache.flags |= 0x01;
    }

    void set_lod_policy(component c, f32 lod_bias, s32 forced_lod) {
        if (!is_valid(c)) {
            return;
        }

        lod_biases[c] = lod_bias;
        forced_lods[c] = forced_lod;

        auto& cache = get_cache(c);
        cache.lod_bias = lod_bias;
        cache.forced_lod = forced_lod;
        cache.flags |= 0x02;
    }

    void set_visibility_flags(component c, u32 flags) {
        if (!is_valid(c)) {
            return;
        }

        visibility_flags[c] = flags;

        auto& cache = get_cache(c);
        cache.visibility_flags = flags;
    }

    void update(const component_cache* caches, u32 count) {
        assert(caches && count);

        for (u32 i{ 0 }; i < count; ++i) {
            const auto& cache{ caches[i] };
            const component c{ cache.id };

            if (!is_valid(c)) {
                continue;
            }

            if (cache.flags & 0x01) {
                set_geometry(c, cache.geometry_content_id);
            }

            if (cache.flags & 0x02) {
                set_lod_policy(c, cache.lod_bias, cache.forced_lod);
            }
        }
    }

}
