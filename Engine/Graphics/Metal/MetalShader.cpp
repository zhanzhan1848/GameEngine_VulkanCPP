#include "MetalShader.h"

#include "Content/ContentLoader.h"
#include "Content/ContentToEngine.h"
#include "MetalCore.h"
#include "EngineConfig.h"

#include <fstream>
#include <cstring>
#include <cstdio>
#include <string>
#include <unistd.h>

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
			if (!content::load_engine_shaders(engine_shaders_blob, size) ||
				!engine_shaders_blob || size == 0) return false;

			// All 9 entries in the packed blob contain the SAME metallib
			// (build_engine_shader_blob.py packs the same compiled
			// .metallib 9 times, once per engine_shader::count slot). The
			// legacy load path was: iterate 9 times, build a dispatch_data_t
			// from each entry, call device->newLibrary(data) for each, then
			// call newFunction(shader_names[i]). That's wasteful but on
			// macOS 26 / Apple Silicon it's also CRASHING: Metal's
			// newLibraryWithData runs CC_SHA256_Update via the
			// NEON-accelerated AccelerateCrypto_SHA256_compress, and any
			// dispatch_data_t whose backing buffer is page-aligned and
			// has a non-multiple-of-64 size will SIGBUS during that
			// 64-byte SHA256 read (the read crosses a page boundary into
			// a commpage / kernel-mapped region). The exception lands
			// in the editor process before it can show a window.
			//
			// Fix: load the metallib ONCE (the first entry is the same
			// metallib) and get all 9 named functions from that single
			// library. This sidesteps both the per-entry dispatch_data_t
			// (which is what triggered the SIGBUS) and the redundant
			// 9x library compilation.
			content::compiled_shader_ptr first_shader =
				reinterpret_cast<content::compiled_shader_ptr>(&engine_shaders_blob[0]);
			engine_shaders[0] = first_shader;  // legacy slot 0 still points at the entry

			// To dodge the dispatch_data_t SIGBUS path, write the metallib
			// to a tmp file once and load via newLibraryWithURL. This
			// path is well-tested and avoids the cross-page SHA256
			// read because Metal's URL loader does the file → buffer
			// copy itself with proper alignment guarantees.
			NS::String* tmp_path = [&]() -> NS::String* {
				std::string s{ "/tmp/primal_engine_shaders_XXXXXX.metallib" };
				int fd = mkstemps(s.data(), 9);  // 9 = strlen(".metallib")
				if (fd < 0) return nullptr;
				ssize_t written = ::write(fd, first_shader->byte_code(),
					static_cast<size_t>(first_shader->byte_code_size()));
				::close(fd);
				if (written != static_cast<ssize_t>(first_shader->byte_code_size())) {
					::unlink(s.c_str());
					return nullptr;
				}
				return NS::String::string(s.c_str(), NS::UTF8StringEncoding);
			}();
			if (!tmp_path) return false;

			NS::URL* url = NS::URL::fileURLWithPath(tmp_path);
			MTL::Library* engine_library = device->newLibrary(url, &pError);
			::unlink(tmp_path->utf8String());
			// tmp_path was allocated by NS::String::string() (autoreleased);
			// do NOT call ->release() here — the autorelease pool owns it.
			// Over-releasing an autoreleased object causes the next autorelease
			// pool drain to dereference a recycled slot pointer, which is the
			// documented root cause of the objc_release SIGSEGV in the timer
			// callback's per-callout ARP.
			if (!engine_library || pError) {
				if (pError) {
					std::fprintf(stderr, "[MetalShader] newLibraryWithURL failed: %s\n",
						pError->localizedDescription()->utf8String());
					// pError is an NS::Error** out-parameter; Metal owns the
					// NSError* it writes. Calling ->release() here would be
					// an over-release of an unowned pointer.
				}
				return false;
			}
			engine_libraries.emplace_back(engine_library);

			// Resolve all 9 named functions from the single library.
			for (u32 i = 0; i < engine_shader::count; ++i) {
				// functionName is autoreleased (NS::String::string factory).
				// newFunction() does not retain it, but that's fine — the
				// autorelease pool owns it until this scope ends. Releasing
				// here was a double-release.
				NS::String* functionName = NS::String::string(shader_names[i], NS::UTF8StringEncoding);
				MTL::Function* function = engine_library->newFunction(functionName);
				engine_functions.emplace_back(function);
			}

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
		// If load_engine_shaders never produced any library (e.g. Avalonia editor
		// without a precompiled shaders blob), return an empty function. Callers
		// null-check via .get() and skip their dependent pass.
		if (id >= engine_shader::count || engine_libraries.empty() || engine_functions.empty()) {
			return engine_metal_shader_function(nullptr, nullptr);
		}
		// 检查函数指针是否有效
		if (!engine_functions[id]) {
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