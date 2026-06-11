// 文件说明: Mesh 组件实现。管理实体到几何/材质的绑定、渲染项创建与生命周期、LOD策略。
// 性能策略: 稳定索引数组按实体索引存储、渲染项双缓冲替换、尽量减少分配次数与拷贝。

#include "Mesh.h"
#include "Entity.h"
#include "Graphics/Renderer.h"

namespace primal::mesh {

    namespace {
        // 每个实体索引的 Mesh 组件是否存在
        utl::vector<u8>                    exists_flags;
        // 每个实体索引的几何资源 id（geometry_content_id）
        utl::vector<id::id_type>           geometry_ids;
        // 每个实体索引的渲染项 id
        utl::vector<id::id_type>           render_item_ids;
        // 每个实体索引的材质数组（与 submesh 顺序一致）
        utl::vector<utl::vector<id::id_type>> materials;
        // LOD 策略参数
        utl::vector<f32>                   lod_biases;
        utl::vector<s32>                   forced_lods;

        // 内部函数: 为实体创建渲染项
        static id::id_type create_render_item(id::id_type entity_id,
                                              id::id_type geometry_content_id,
                                              const utl::vector<id::id_type>& mtl_ids)
        {
            assert(id::is_valid(entity_id));
            assert(id::is_valid(geometry_content_id));
            const u32 count = (u32)mtl_ids.size();
            assert(count);
            return graphics::add_render_item(entity_id, geometry_content_id, count, mtl_ids.data());
        }

        // 内部函数: 移除渲染项（安全检查）
        static void remove_render_item_if_exists(id::id_type render_item_id)
        {
            if (id::is_valid(render_item_id)) {
                graphics::remove_render_item(render_item_id);
            }
        }
    } // anonymous namespace

    // 函数说明: 获取 Mesh 组件的几何资源 id
    id::id_type get_geometry_id(component c)
    {
        assert(c.is_valid());
        const id::id_type eindex{ id::index(c.get_id()) };
        assert(eindex < geometry_ids.size());
        return geometry_ids[eindex];
    }

    // 函数说明: 创建 Mesh 组件并绑定渲染项
    mesh::component create(init_info info, game_entity::entity entity)
    {
        assert(entity.is_valid());
        const id::id_type eindex{ id::index(entity.get_id()) };

        // 扩容至实体索引
        if (geometry_ids.size() <= eindex) {
            geometry_ids.resize(eindex + 1, id::invalid_id);
            render_item_ids.resize(eindex + 1, id::invalid_id);
            materials.resize(eindex + 1);
            exists_flags.resize(eindex + 1, (u8)0);
            lod_biases.resize(eindex + 1, 0.f);
            forced_lods.resize(eindex + 1, -1);
        }

        // 写入 LOD 策略
        lod_biases[eindex] = info.lod_bias;
        forced_lods[eindex] = info.forced_lod;

        // 材质复制
        materials[eindex].resize(info.material_count);
        if (info.material_count && info.material_ids) {
            memcpy(materials[eindex].data(), info.material_ids, sizeof(id::id_type) * info.material_count);
        }

        // 如果有几何，则创建渲染项
        if (id::is_valid(info.geometry_content_id) && info.material_count) {
            geometry_ids[eindex] = info.geometry_content_id;
            render_item_ids[eindex] = create_render_item(id::id_type(entity.get_id()), info.geometry_content_id, materials[eindex]);
        }

        exists_flags[eindex] = 1;
        // Mesh 组件 id 与实体 id 对齐（与 Transform 一致）
        return mesh::component{ mesh::mesh_id{ entity.get_id() } };
    }

    // 函数说明: 移除 Mesh 组件并释放渲染项
    void remove(mesh::component c)
    {
        assert(c.is_valid());
        const id::id_type eindex{ id::index(c.get_id()) };
        if (eindex < exists_flags.size() && exists_flags[eindex]) {
            remove_render_item_if_exists(render_item_ids[eindex]);
            render_item_ids[eindex] = id::invalid_id;
            geometry_ids[eindex] = id::invalid_id;
            materials[eindex].clear();
            exists_flags[eindex] = 0;
        }
    }

    // 函数说明: 用新的几何+材质重新绑定渲染项（支持 CPU→GPU 替换）
    void set_geometry(mesh::component c, id::id_type geometry_content_id,
                      const id::id_type* material_ids, u32 material_count)
    {
        assert(c.is_valid());
        const id::id_type eindex{ id::index(c.get_id()) };
        assert(eindex < exists_flags.size() && exists_flags[eindex]);

        // 更新材质
        materials[eindex].resize(material_count);
        if (material_count && material_ids) {
            memcpy(materials[eindex].data(), material_ids, sizeof(id::id_type) * material_count);
        }

        // 替换渲染项
        remove_render_item_if_exists(render_item_ids[eindex]);
        geometry_ids[eindex] = geometry_content_id;
        render_item_ids[eindex] = create_render_item(id::id_type(c.get_id()), geometry_content_id, materials[eindex]);
    }

    // 函数说明: 设置 LOD 策略参数
    void set_lod_policy(mesh::component c, f32 lod_bias, s32 forced_lod)
    {
        assert(c.is_valid());
        const id::id_type eindex{ id::index(c.get_id()) };
        assert(eindex < exists_flags.size() && exists_flags[eindex]);
        lod_biases[eindex] = lod_bias;
        forced_lods[eindex] = forced_lod;
    }

    // 函数说明: 批量更新 Mesh 组件缓存（便于系统层统一刷新）
    void update(const component_cache* caches, u32 count)
    {
        assert(caches && count);
        for (u32 i = 0; i < count; ++i) {
            const component_cache& cc = caches[i];
            assert(id::is_valid(cc.id));
            const id::id_type eindex{ id::index(cc.id) };
            if (!(eindex < exists_flags.size() && exists_flags[eindex])) continue;

            if (cc.flags & 0x1) {
                set_geometry(mesh::component{ cc.id }, cc.geometry_content_id, cc.material_ids, cc.material_count);
            }
            if (cc.flags & 0x2) {
                set_lod_policy(mesh::component{ cc.id }, cc.lod_bias, cc.forced_lod);
            }
        }
    }
}