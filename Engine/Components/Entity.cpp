#include "Entity.h"
#include "Transform.h"
#include "Script.h"
#include "Mesh.h"
#include "Particle.h"
#include "Cluster.h"
#include "Geometry.h"
#include "CommandBuffer.h"

namespace primal::game_entity {

	namespace {

		utl::vector<id::generation_type>			generations;
		utl::deque<entity_id>						free_ids;
		utl::vector<component_mask>				component_masks;

	}// anonymous namespace


	entity create()
	{
		entity_id id;

		if (free_ids.size() > id::min_deleted_elements)
		{
			id = free_ids.front();
			free_ids.pop_front();
			id = entity_id{ id::new_generation(id) };
			++generations[id::index(id)];
			component_masks[id::index(id)] = 0;
		}
		else
		{
			id = entity_id{ (id::id_type)generations.size() };
			generations.push_back(0);
			component_masks.emplace_back(0);
		}

		return entity{ id };
	}

	entity create(entity_info info)
	{
		entity ent{ create() };

		if (info.transform)
		{
			transform::component tc = transform::create(*info.transform, ent);
			assert(tc.is_valid());
			set_component_bit(ent.get_id(), static_cast<u8>(component_bit::Transform));
		}

		if (info.script && info.script->script_creator)
		{
			script::component sc = script::create(*info.script, ent);
			assert(sc.is_valid());
			set_component_bit(ent.get_id(), static_cast<u8>(component_bit::Script));
		}

		if (info.mesh && info.mesh->material_count)
		{
			mesh::component mc = mesh::create(*info.mesh, ent);
			assert(mc.is_valid());
			set_component_bit(ent.get_id(), static_cast<u8>(component_bit::Mesh));
		}

		if (info.particle)
		{
			particle::component pc = particle::create(*info.particle, ent);
			set_component_bit(ent.get_id(), static_cast<u8>(component_bit::Particle));
		}

		if (info.cluster && info.cluster->geometry_content_id != id::invalid_id)
		{
			cluster::component cc = cluster::create(*info.cluster, ent);
			assert(cc != id::invalid_id);
			set_component_bit(ent.get_id(), static_cast<u8>(component_bit::Cluster));
		}

		if (info.geometry && info.geometry->handle.is_valid())
		{
			geometry::component::geometry gc = geometry::component::create(*info.geometry, ent);
			assert(gc.is_valid());
			set_component_bit(ent.get_id(), static_cast<u8>(component_bit::Geometry));
		}

		return ent;
	}

	void remove(entity_id id)
	{
		const id::id_type index{ id::index(id) };
		assert(is_alive(id));
		const component_mask mask{ component_masks[index] };

		if (mask & bit_mask(component_bit::Script))
		{
			script::remove_for_entity(id);
		}
		if (mask & bit_mask(component_bit::Mesh))
		{
			mesh::component mc{ mesh::mesh_id{id} };
			mesh::remove(mc);
		}
		if (mask & bit_mask(component_bit::Particle))
		{
			particle::remove_for_entity(id);
		}
		if (mask & bit_mask(component_bit::Cluster))
		{
			cluster::component cc{ id };
			cluster::remove(cc);
		}
		if (mask & bit_mask(component_bit::Geometry))
		{
			geometry::component::remove(id);
		}
		// Transform is always removed last since other components may reference it
		if (mask & bit_mask(component_bit::Transform))
		{
			transform::component tc{ transform::transform_id{id} };
			transform::remove(tc);
		}

		component_masks[index] = 0;
		free_ids.push_back(id);
	}

	bool is_alive(entity_id id)
	{
		assert(id::is_valid(id));
		const id::id_type index{ id::index(id) };
		assert(index < generations.size());
		return generations[index] == id::generation(id);
	}

	component_mask get_component_mask(entity_id id)
	{
		assert(is_alive(id));
		return component_masks[id::index(id)];
	}

	void set_component_bit(entity_id id, u8 bit)
	{
		assert(is_alive(id));
		component_masks[id::index(id)] |= (component_mask{1} << bit);
	}

	void clear_component_bit(entity_id id, u8 bit)
	{
		assert(is_alive(id));
		component_masks[id::index(id)] &= ~(component_mask{1} << bit);
	}

	// Legacy entity accessors — delegate to component systems
	transform::component entity::transform() const
	{
		assert(is_alive(_id));
		return transform::component{ transform::transform_id{_id} };
	}

	script::component entity::script() const
	{
		assert(is_alive(_id));
		return script::get_component_for_entity(_id);
	}

	mesh::component entity::mesh() const
	{
		assert(is_alive(_id));
		return mesh::component{ mesh::mesh_id{_id} };
	}

	particle::component entity::particle() const
	{
		assert(is_alive(_id));
		return particle::get_component_for_entity(_id);
	}

	u32 entity_count()
	{
		return static_cast<u32>(generations.size());
	}
}
