#include "ContentLoader.h"
#include "Graphics/Renderer.h"


#if !defined(SHIPPING) && defined(__APPLE__)

#include <fstream>
#include <filesystem>
#include <mach-o/dyld.h>


namespace primal::content
{
	namespace
	{
		bool read_file(std::filesystem::path path, std::unique_ptr<u8[]>& data, u64& size)
		{
			if (!std::filesystem::exists(path)) return false;

			size = std::filesystem::file_size(path);
			assert(size);
			if (!size) return false;
			data = std::make_unique<u8[]>(size);
			std::ifstream file{ path, std::ios::in | std::ios::binary };
			if (!file || !file.read((char*)data.get(), size))
			{
				file.close();
				return false;
			}
			file.close();
			return true;
		}
	} // anonymous namespace

	bool load_game()
	{
		return true;
	}
	void unload_game()
	{
		
	}

	bool load_engine_shaders(std::unique_ptr<u8[]>& shaders, u64& size)
	{
		// 1) cwd 相对路径——测试可执行文件从 Darwin/<Config>/ 运行时的惯例。
		if (read_file(graphics::get_engine_shaders_path(), shaders, size)) return true;

		// 2) 可执行文件相对路径——Avalonia 编辑器经 LaunchServices (`open`)
		// 启动时 cwd 为 "/"，从仓库根直接启动时 cwd 为仓库根，两种情况下
		// cwd 相对路径都解析不到随 .app 打包的 shaders/metal/shaders.metallib，
		// graphics::initialize(metal) 随之失败（症状：World Editor 视口全黑、
		// 只有 C# overlay 可见）。回退到 bundle 内的副本。
		uint32_t exe_buf_size = 0;
		_NSGetExecutablePath(nullptr, &exe_buf_size);
		if (exe_buf_size > 0)
		{
			std::string exe_path(exe_buf_size, '\0');
			if (_NSGetExecutablePath(exe_path.data(), &exe_buf_size) == 0)
			{
				const std::filesystem::path exe_dir =
					std::filesystem::path(exe_path.c_str()).parent_path();
				for (const char* rel : { "shaders/metal/shaders.metallib",
										 "../Resources/shaders/metal/shaders.metallib" })
				{
					if (read_file(exe_dir / rel, shaders, size)) return true;
				}
			}
		}
		return false;
	}
}
#endif // !defined(SHIPPING)