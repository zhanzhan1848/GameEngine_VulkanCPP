#include "MetalCamera.h"

#include "EngineAPI/GameEntity.h"

#define M_PI 3.14159265358979323846

namespace primal::graphics::metal::camera
{
    namespace
    {
        utl::free_list<metal_camera>						cameras;

        //////////////////////////////////////// Math Functions /////////////////////////////////////
		// Metal 使用 左手坐标系
		//////
		math::m4x4 createLookToLH(
            const math::v3& eyePosition,
            const math::v3& eyeDirection,
            const math::v3& upDirection)
        {
            // 标准化方向向量（Z 轴指向摄像机前方，左手系中Z轴是正向的）
            math::v3 zAxis{ eyeDirection.normalized() };
        
            // 计算右向量（X 轴）
            math::v3 xAxis{ upDirection.cross(zAxis).normalized() };
            
            // 修正上向量（Y 轴）
            math::v3 yAxis{ zAxis.cross(xAxis) };
            
            // 构建旋转矩阵（3x3）
            math::m3x3 rotation;
            rotation << xAxis.x(), xAxis.y(), xAxis.z(),
                        yAxis.x(), yAxis.y(), yAxis.z(),
                        zAxis.x(), zAxis.y(), zAxis.z();
            
            // 构建 4x4 视图矩阵
            math::m4x4 viewMatrix{ Eigen::Matrix4f::Identity() };
            viewMatrix.block<3, 3>(0, 0) = rotation.transpose(); // 旋转部分的转置
            viewMatrix.block<3, 1>(0, 3) = -rotation.transpose() * eyePosition; // 平移部分
            
            return viewMatrix;
        }
    
        math::m4x4 createPerspectiveFovLH(float fovY, float aspectRatio, float nearZ, float farZ)
        {
            float tanHalfFovY = std::tan(fovY * 0.5f);
            float f = 1.0f / tanHalfFovY;  // 焦距缩放因子
    
            math::m4x4 proj{ Eigen::Matrix4f::Zero() };
            proj(0, 0) = f / aspectRatio;  // X 缩放
            proj(1, 1) = f;                // Y 缩放
            proj(2, 2) = farZ / (farZ - nearZ);  // Z 缩放（左手系）
            proj(3, 2) = 1.0f;                   // Z 透视分量（左手系为正）
            proj(2, 3) = -(nearZ * farZ) / (farZ - nearZ);  // 平移分量

			// 添加深度范围调整
			// math::m4x4 depthAdjust{ Eigen::Matrix4f::Identity() };
			// depthAdjust(2, 2) = 0.5f;
			// depthAdjust(2, 3) = 0.5f;
    
            return proj;
        }
    
        math::m4x4 createOrthographicLH(float width, float height, float nearZ, float farZ)
        {
            math::m4x4 proj{ Eigen::Matrix4f::Identity() };
            proj(0, 0) = 2.0f / width;              // X 缩放
            proj(1, 1) = 2.0f / height;             // Y 缩放
            proj(2, 2) = 1.0f / (farZ - nearZ);     // Z 缩放（左手系）
            proj(2, 3) = -nearZ / (farZ - nearZ);   // Z 平移
    
            return proj;
        }

        //////////////////////////////////////// Math Functions /////////////////////////////////////

		void set_up_vector(metal_camera& camera, const void *const data, [[maybe_unused]] u32 size)
		{
			math::v3 up_vector{ *(math::v3*)data };
			assert(sizeof(up_vector) == size);
			camera.up(up_vector);
		}

		constexpr void set_field_of_view(metal_camera& camera, const void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::perspective);
			f32 fov{ *(f32*)data };
			assert(sizeof(fov) == size);
			camera.field_of_view(fov);
		}

		constexpr void set_aspect_ratio(metal_camera& camera, const void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::perspective);
			f32 aspect_ratio{ *(f32*)data };
			assert(sizeof(aspect_ratio) == size);
			camera.aspect_ratio(aspect_ratio);
		}

		constexpr void set_view_width(metal_camera& camera, const void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::orthographic);
			f32 view_width{ *(f32*)data };
			assert(sizeof(view_width) == size);
			camera.view_width(view_width);
		}

		constexpr void set_view_height(metal_camera& camera, const void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::orthographic);
			f32 view_height{ *(f32*)data };
			assert(sizeof(view_height) == size);
			camera.view_height(view_height);
		}

		constexpr void set_near_z(metal_camera& camera, const void *const data, [[maybe_unused]] u32 size)
		{
			f32 near_z{ *(f32*)data };
			assert(sizeof(near_z) == size);
			camera.near_z(near_z);
		}

		constexpr void set_far_z(metal_camera& camera, const void *const data, [[maybe_unused]] u32 size)
		{
			f32 far_z{ *(f32*)data };
			assert(sizeof(far_z) == size);
			camera.far_z(far_z);
		}

		void get_view(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			math::m4x4 *const matrix{ (math::m4x4 *const)data };
			assert(sizeof(math::m4x4) == size);
            *matrix = camera.view();
		}

		void get_projection(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			math::m4x4 *const matrix{ (math::m4x4 *const)data };
			assert(sizeof(math::m4x4) == size);
			*matrix = camera.projection();
		}

		void get_inverse_projection(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			math::m4x4 *const matrix{ (math::m4x4 *const)data };
			assert(sizeof(math::m4x4) == size);
			*matrix = camera.inverse_projection();
		}

		void get_view_projection(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			math::m4x4 *const matrix{ (math::m4x4 *const)data };
			assert(sizeof(math::m4x4) == size);
			*matrix = camera.view_projection();
		}

		void get_inverse_view_projection(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			math::m4x4 *const matrix{ (math::m4x4 *const)data };
			assert(sizeof(math::m4x4) == size);
			*matrix = camera.inverse_view_projection();
		}

		void get_up_vector(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			math::v3 *const up_vector{ (math::v3 *const)data };
			assert(sizeof(math::v3) == size);
			*up_vector = camera.up();
		}

		constexpr void get_field_of_view(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::perspective);
			f32 *const fov{ (f32 *const)data };
			assert(sizeof(f32) == size);
			*fov = camera.field_of_view();
		}

		constexpr void get_aspect_ratio(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::perspective);
			f32 *const aspect_ratio{ (f32 *const)data };
			assert(sizeof(f32) == size);
			*aspect_ratio = camera.aspect_ratio();
		}

		constexpr void get_view_width(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::orthographic);
			f32 *const view_width{ (f32 *const)data };
			assert(sizeof(f32) == size);
			*view_width = camera.view_width();
		}

		constexpr void get_view_height(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			assert(camera.projection_type() == graphics::camera::orthographic);
			f32 *const view_height{ (f32 *const)data };
			assert(sizeof(f32) == size);
			*view_height = camera.view_height();
		}

		constexpr void get_near_z(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			f32 *const near_z{ (f32 *const)data };
			assert(sizeof(f32) == size);
			*near_z = camera.near_z();
		}

		constexpr void get_far_z(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			f32 *const far_z{ (f32 *const)data };
			assert(sizeof(f32) == size);
			*far_z = camera.far_z();
		}

		constexpr void get_project_type(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			graphics::camera::type *const type{ (graphics::camera::type *const)data };
			assert(sizeof(graphics::camera::type) == size);
			*type = camera.projection_type();
		}

		constexpr void get_entity_id(const metal_camera& camera, void *const data, [[maybe_unused]] u32 size)
		{
			id::id_type *const entity_id{ (id::id_type *const)data };
			assert(sizeof(id::id_type) == size);
			*entity_id = camera.entity_id();
		}

		constexpr void dummy_set(metal_camera&, const void *const, u32) {}

		using set_function = void(*)(metal_camera&, const void *const, u32);
		using get_function = void(*)(const metal_camera&, void *const, u32);
		constexpr set_function set_functions[]
		{
			set_up_vector,
			set_field_of_view,
			set_aspect_ratio,
			set_view_width,
			set_view_height,
			set_near_z,
			set_far_z,
			dummy_set,
			dummy_set,
			dummy_set,
			dummy_set,
			dummy_set,
			dummy_set,
			dummy_set,
		};

		static_assert(_countof(set_functions) == camera_parameter::count);

		constexpr get_function get_functions[]
		{
			get_up_vector,
			get_field_of_view,
			get_aspect_ratio,
			get_view_width,
			get_view_height,
			get_near_z,
			get_far_z,
			get_view,
			get_projection,
			get_inverse_projection,
			get_view_projection,
			get_inverse_view_projection,
			get_project_type,
			get_entity_id,
		};

		static_assert(_countof(get_functions) == camera_parameter::count);
    } // anonymous namespace

    metal_camera::metal_camera(camera_init_info info)
		: _up{ info.up },
		_near_z{ info.near_z }, _far_z{ info.far_z },
		_field_of_view{ info.field_of_view }, _aspect_ratio{ info.aspect_ratio },
		_projection_type{ info.type }, _entity_id{ info.entity_id }, _is_dirty{ true }
	{
		assert(id::is_valid(_entity_id));
		update();
	}

	void metal_camera::update()
	{
		game_entity::entity entity{ game_entity::entity_id{ _entity_id} };
		math::v3 pos{ entity.transform().position() };
		math::v3 dir{ entity.transform().orientation() };
		_position = entity.transform().position();
		_direction = entity.transform().orientation();
		_view = createLookToLH(_position, _direction, _up);

		if (_is_dirty)
		{
			_projection = (_projection_type == graphics::camera::perspective) ?
				createPerspectiveFovLH(_field_of_view * M_PI, _aspect_ratio, _near_z, _far_z) :
				createOrthographicLH(_view_width, _view_height, _near_z, _far_z);
			_inverse_projection = _projection.inverse();
			_is_dirty = false;
		}

		_view_projection = _projection * _view;
		_inverse_view_projection = _view_projection.inverse();
	}

	void metal_camera::up(math::v3 up) 
	{
		_up = up;
	}

	constexpr void metal_camera::field_of_view(f32 fov)
	{
		assert(_projection_type == graphics::camera::perspective);
		_field_of_view = fov;
		_is_dirty = true;
	}

	constexpr void metal_camera::aspect_ratio(f32 aspect_ratio)
	{
		assert(_projection_type == graphics::camera::perspective);
		_aspect_ratio = aspect_ratio;
		_is_dirty = true;
	}

	constexpr void metal_camera::view_width(f32 width)
	{
		assert(width);
		assert(_projection_type == graphics::camera::orthographic);
		_view_height = width;
		_is_dirty = true;
	}

	constexpr void metal_camera::view_height(f32 height)
	{
		assert(height);
		assert(_projection_type == graphics::camera::orthographic);
		_view_height = height;
		_is_dirty = true;
	}

	constexpr void metal_camera::near_z(f32 near_z)
	{
		_near_z = near_z;
		_is_dirty = true;
	}

	constexpr void metal_camera::far_z(f32 far_z)
	{
		_far_z = far_z;
		_is_dirty = true;
	}

	graphics::camera create(camera_init_info info)
	{
		return graphics::camera{ camera_id{ cameras.add(info) } };
	}

	void remove(camera_id id)
	{
		assert(id::is_valid(id));
		cameras.remove(id);
	}

	void set_parameter(camera_id id, camera_parameter::parameter parameter, const void * const data, u32 data_size)
	{
		assert(data && data_size);
		assert(parameter < camera_parameter::count);
		metal_camera& camera{ get(id) };
		set_functions[parameter](camera, data, data_size);
	}

	void get_parameter(camera_id id, camera_parameter::parameter parameter, void * const data, u32 data_size)
	{
		assert(data && data_size);
		assert(parameter < camera_parameter::count);
		metal_camera& camera{ get(id) };
		get_functions[parameter](camera, data, data_size);
	}

	metal_camera & get(camera_id id)
	{
		assert(id::is_valid(id));
		return cameras[id];
	}
    
}