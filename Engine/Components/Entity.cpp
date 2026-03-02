#include "Entity.h"
#include "Transform.h"
#include "Script.h"
#include "Mesh.h"
#include "Particle.h"

namespace primal::game_entity {

	namespace {

		utl::vector<transform::component>			transforms;
		utl::vector<script::component>			scripts;
		utl::vector<mesh::component>			meshes;
		utl::vector<particle::component>			particles;

		utl::vector<id::generation_type>			generations;
		utl::deque<entity_id>						free_ids;

	}// anonymous namespace


	entity create(entity_info info)
	{
		assert(info.transform); //All game entities must have a transform component
		if (!info.transform) return entity{};

		entity_id id;

		if (free_ids.size() > id::min_deleted_elements)
		{
			id = free_ids.front();
			assert(!is_alive(id ));
			free_ids.pop_front();
			id = entity_id{ id::new_generation(id) };
			++generations[id::index(id)];
		}
		else
		{
			id = entity_id{ (id::id_type)generations.size() };
			generations.push_back(0);

			// Resize component
			// NOTE: we don't call resize(), so the number of memory allocations stays low
			transforms.emplace_back();
			scripts.emplace_back();
			meshes.emplace_back();
			particles.emplace_back();
		}

		const entity new_entity{ id };
		const id::id_type index{ id::index(id) };

		//Create transform component
		assert(!transforms[index].is_valid());
		transforms[index] = transform::create(*info.transform, new_entity);
		if (!transforms[index].is_valid()) return {};

		//Create Script component
		if (info.script && info.script->script_creator)
		{
			assert(!scripts[index].is_valid());
			scripts[index] = script::create(*info.script, new_entity);
			assert(scripts[index].is_valid());
		}

		//Create Mesh component
		if (info.mesh && info.mesh->material_count)
		{
			assert(!meshes[index].is_valid());
			meshes[index] = mesh::create(*info.mesh, new_entity);
			assert(meshes[index].is_valid());
		}

		//Create Particle component
		if (info.particle)
		{
			assert(!particles[index].is_valid());
			particles[index] = particle::create(*info.particle, new_entity);
		}

		return new_entity;

		return new_entity;
	}

	void remove(entity_id id)
	{
		const id::id_type index{ id::index(id) };
		assert(is_alive(id));

		if (scripts[index].is_valid())
		{
			script::remove(scripts[index]);
			scripts[index] = {};
		}
		if (meshes[index].is_valid())
		{
			mesh::remove(meshes[index]);
			meshes[index] = {};
		}
		if (particles[index].is_valid())
		{
			particle::remove(particles[index]);
			particles[index] = {};
		}
		transform::remove(transforms[index]);
		transforms[index] = {};
	}

	bool is_alive(entity_id id)
	{
		assert(id::is_valid(id));
		const id::id_type index{ id::index(id) };
		assert(index < generations.size());
		return (generations[index] == id::generation(id) && transforms[index].is_valid());
	}

	transform::component entity::transform() const
	{
		assert(is_alive(_id));
		const id::id_type index{ id::index(_id) };
		return transforms[index];
	}
	
	script::component entity::script() const
	{
		assert(is_alive(_id));
		const id::id_type index{ id::index(_id) };
		return scripts[index];
	}

	mesh::component entity::mesh() const
	{
		assert(is_alive(_id));
		const id::id_type index{ id::index(_id) };
		return meshes[index];
	}

	particle::component entity::particle() const
	{
		assert(is_alive(_id));
		const id::id_type index{ id::index(_id) };
		return particles[index];
	}
}