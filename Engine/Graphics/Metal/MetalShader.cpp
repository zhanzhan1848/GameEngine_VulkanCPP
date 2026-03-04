#include "MetalShader.h"

#include "Content/ContentLoader.h"
#include "Content/ContentToEngine.h"
#include "MetalCore.h"
#include "EngineConfig.h"

#include <fstream>

namespace primal::graphics::metal::shader 
{
    namespace 
    {
        // Each element in this array points to an offset withing the shaders blob.
        content::compiled_shader_ptr 	engine_shaders[engine_shader::count]{};

        // this is a chunk of memory that contains all comiled engine shaders.
		// The blob is an array of shader byte code consisting of a u64 size and
		// an array of bytes.
        std::unique_ptr<u8[]> 			engine_shaders_blob{};

		utl::vector<MTL::Library*> 		engine_libraries;
		utl::vector<MTL::Function*> 	engine_functions;

        bool load_engine_shaders()
        {
			MTL::Device* device{ core::get_device() };
			NS::Error* pError{ nullptr };
			static const char* shader_names[] = {
				"fullscreen_triangle_vs",
				"post_process_ps",
				"shadow_mapping_vs",
				"ssao_pass",
				"ssao_blur",
				"ssgi_pass",
				"ssgi_blur",
				"taa_pass",
				"compose_pass"
			};

			static_assert(sizeof(shader_names) / sizeof(shader_names[0]) == engine_shader::count, "shader_names array size must match engine_shader::count");

			assert(!engine_shaders_blob);

			u64 size{ 0 };
			bool result { content::load_engine_shaders(engine_shaders_blob, size) };
			assert(engine_shaders_blob && size);

			u64 offset { 0 };
			u32 index { 0 };
			while (offset < size && result)
			{
				assert(index < engine_shader::count);
				content::compiled_shader_ptr& shader{ engine_shaders[index] };
				assert(!shader);
				result &= index < engine_shader::count && !shader;
				if (!result) break;
				shader = reinterpret_cast<const content::compiled_shader_ptr>(&engine_shaders_blob[offset]);
				offset += shader->buffer_size();

				// 创建 dispatch_data_t
				dispatch_data_t data = dispatch_data_create(
					shader->byte_code(),    		// 数据指针
					shader->byte_code_size(),    	// 数据长度
					nullptr,          				// 队列（使用默认队列）
					^{
						// 释放回调（可选）
						// 这里 buffer 是 vector 管理的，dispatch_data_t 不会持有 buffer.data()，
						// 所以无需额外释放
					}
				);
				if (!data) {
					// 处理 dispatch_data_t 创建失败
					return false;
				}
				MTL::Library* engine_library{ device->newLibrary(data, &pError) };
				dispatch_release(data);
				if (pError) {
					// 处理错误
					// error->localizedDescription() 可用于获取错误信息
					pError->release();
					return false;
				}
				engine_libraries.emplace_back(engine_library);
				u32 id{ static_cast<u32>(engine_functions.size()) };
				NS::String* functionName = NS::String::string(shader_names[id], NS::UTF8StringEncoding);
				MTL::Function* function = engine_library->newFunction(functionName);
				engine_functions.emplace_back(function);
				functionName->release();

				++index;
			}
		
			assert(offset == size && index == engine_shader::count);

			return true;
        }
    } // anonymous namespace

	bool initialize() 
    {
		return load_engine_shaders();
	}
	void shutdown() 
    {
		// 释放所有MTL::Function对象
		for (auto* function : engine_functions)
		{
			if (function)
			{
				function->release();
			}
		}
		engine_functions.clear();
		
		// 释放所有MTL::Library对象
		for (auto* library : engine_libraries)
		{
			if (library)
			{
				library->release();
			}
		}
		engine_libraries.clear();
		
		// 清理着色器数据
		for (u32 i{ 0 }; i < engine_shader::count; ++i)
		{
			engine_shaders[i] = {};
		}
		engine_shaders_blob.reset();
	}

	engine_metal_shader_function get_engine_shader(engine_shader::id id)
	{
		assert(id < engine_shader::count);
		assert(!engine_libraries.empty());
		assert(!engine_functions.empty());
		
		// 检查函数指针是否有效
		if (id >= engine_shader::count || engine_functions.empty() || !engine_functions[id]) {
			return engine_metal_shader_function(nullptr, nullptr);
		}
		
		// 增加引用计数并返回，使用空删除器避免重复释放
		// 因为engine_functions中的对象由shutdown()统一管理释放
		MTL::Function* function = engine_functions[id];
		if (function) {
			function->retain(); // 增加引用计数
		}
		
		// 定义删除器函数，负责减少引用计数
		static auto deleter = [](MTL::Function* ptr) {
			if (ptr) {
				ptr->release(); // 减少引用计数
			}
		};
		return engine_metal_shader_function(function, deleter);
	}
}