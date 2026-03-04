#include <fstream>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>
#include "ShaderCompilation.h"

#if defined(_MSC_VER)
#include "Graphics/Direct3D12/D3D12Core.h"
#include "Graphics/Direct3D12/D3D12Shaders.h"

#include "../packages/DirectXShaderCompiler/inc/dxcapi.h"
#include "../packages/DirectXShaderCompiler/inc/d3d12shader.h"
#include "Content/ContentToEngine.h"
#include "Utilities/IOStream.h"



using namespace primal;
using namespace primal::graphics::d3d12::shaders;
using namespace Microsoft::WRL;

// NOTE: we wouldn't need to do this if DXC had a NuGet package
#pragma comment(lib, "../packages/DirectXShaderCompiler/lib/x64/dxcompiler.lib")

namespace 
{
	constexpr const char* shaders_source_path{ "../Engine/Graphics/Direct3D12/Shaders/" };

	struct engine_shader_info
	{
		engine_shader::id		id;
		shader_file_info		info;
	};

	constexpr engine_shader_info engine_shader_files[]
	{
		{ engine_shader::fullscreen_triangle_vs,		{ "FullScreenTriangle.hlsl", "FullScreenTriangleVS", shader_type::vertex } },
		{ engine_shader::fill_color_ps,					{ "FillColor.hlsl", "FillColorPS", shader_type::pixel } },
		{ engine_shader::post_process_ps,				{ "PostProcess.hlsl", "PostProcessPS", shader_type::pixel } },
		{ engine_shader::grid_frustums_cs,				{ "GridFrustums.hlsl", "ComputeGridFrustumsCS", shader_type::compute } },
		{ engine_shader::light_culling_cs,				{ "CullingLights.hlsl", "CullLightsCS", shader_type::compute } }
	};

	static_assert(_countof(engine_shader_files) == engine_shader::count);

	struct dxc_compiled_shader
	{
		ComPtr<IDxcBlob>		byte_code;
		ComPtr<IDxcBlobUtf8>	disassembly;
		DxcShaderHash			hash;
	};

	decltype(auto) get_engine_shaders_path() { return std::filesystem::path{ graphics::get_engine_shaders_path(graphics::graphics_platform::direct3d12) }; }

	std::wstring to_wstring(const char* c)
	{
		std::string s{ c };
		return { s.begin(), s.end() };
	}

	class shader_compiler
	{
	public:
		shader_compiler()
		{
			HRESULT hr{ S_OK };
			DXCall(hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&_compiler)));
			if (FAILED(hr)) return;
			DXCall(hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&_utils)));
			if (FAILED(hr)) return;
			DXCall(hr = _utils->CreateDefaultIncludeHandler(&_include_handle));
			if (FAILED(hr)) return;
		}
		DISABLE_COPY_AND_MOVE(shader_compiler);

		dxc_compiled_shader compile(shader_file_info info, std::filesystem::path full_path, primal::utl::vector<std::wstring>& extra_args)
		{
			assert(_compiler && _utils && _include_handle);
			HRESULT hr{ S_OK };

			// Load the source file Utils interface.
			ComPtr<IDxcBlobEncoding> source_blob{ nullptr };
			DXCall(hr = _utils->LoadFile(full_path.c_str(), nullptr, &source_blob));
			if (FAILED(hr)) return {};
			assert(source_blob && source_blob->GetBufferSize());

			OutputDebugStringA("Compiling ");
			OutputDebugStringA(info.file_name);
			OutputDebugStringA(" : ");
			OutputDebugStringA(info.function);
			OutputDebugStringA("\n");

			return compile(source_blob.Get(), get_args(info, extra_args));
		}

		dxc_compiled_shader compile(IDxcBlobEncoding* source_blob, primal::utl::vector<std::wstring> compiler_args)
		{
			DxcBuffer buffer{};
			buffer.Encoding = DXC_CP_ACP; // auto-detect text format, I guss?
			buffer.Ptr = source_blob->GetBufferPointer();
			buffer.Size = source_blob->GetBufferSize();

			utl::vector<LPCWSTR> args;
			for (const auto& arg : compiler_args)
			{
				args.emplace_back(arg.c_str());
			}

			HRESULT hr{ S_OK };
			ComPtr<IDxcResult> results{ nullptr };
			DXCall(hr = _compiler->Compile(&buffer, args.data(), (u32)args.size(), _include_handle.Get(), IID_PPV_ARGS(&results)));
			if (FAILED(hr)) return {};

			ComPtr<IDxcBlobUtf8> errors{ nullptr };
			DXCall(hr = results->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr));
			if (FAILED(hr)) return {};

			if (errors && errors->GetStringLength())
			{
				OutputDebugStringA("\nShader compilation error: \n");
				OutputDebugStringA(errors->GetStringPointer());
			}
			else
			{
				OutputDebugStringA(" [ Succeeded ]");
			}
			OutputDebugStringA("\n");

			HRESULT status{ S_OK };
			DXCall(hr = results->GetStatus(&status));
			if (FAILED(hr) || FAILED(status)) return {};

			ComPtr<IDxcBlob> hash{ nullptr };
			DXCall(hr = results->GetOutput(DXC_OUT_SHADER_HASH, IID_PPV_ARGS(&hash), nullptr));
			if (FAILED(hr)) return {};
			DxcShaderHash *const hash_buffer{ (DxcShaderHash *const)hash->GetBufferPointer() };
			// different source code could result in the same byte code, so we only care about byte code hash.
			assert(!(hash_buffer->Flags & DXC_HASHFLAG_INCLUDES_SOURCE));
			OutputDebugStringA("Shader hash: ");
			for (u32 i{ 0 }; i < _countof(hash_buffer->HashDigest); ++i)
			{
				char hash_bytes[3]{}; // 2 chars for hex value plus termination 0.
				sprintf_s(hash_bytes, "%02x", (u32)hash_buffer->HashDigest[i]);
				OutputDebugStringA(hash_bytes);
				OutputDebugStringA(" ");
			}
			OutputDebugStringA("\n");

			ComPtr<IDxcBlob> shader{ nullptr };
			DXCall(hr = results->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr));
			if (FAILED(hr)) return {};
			buffer.Ptr = shader->GetBufferPointer();
			buffer.Size = shader->GetBufferSize();

			ComPtr<IDxcResult> disasm_results{ nullptr };
			DXCall(hr = _compiler->Disassemble(&buffer, IID_PPV_ARGS(&disasm_results)));

			ComPtr<IDxcBlobUtf8> disassembly{ nullptr };
			DXCall(hr = disasm_results->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&disassembly), nullptr));

			dxc_compiled_shader result{ shader.Detach(), disassembly.Detach() };
			memcpy(&result.hash.HashDigest[0], &hash_buffer->HashDigest[0], _countof(hash_buffer->HashDigest));

			return result;
		}

	private:

		utl::vector<std::wstring> get_args(const shader_file_info& info, utl::vector<std::wstring>& extra_args)
		{
			utl::vector<std::wstring>	args{};
			args.emplace_back(to_wstring(info.file_name));						// Optional shader source file name for error reporting
			args.emplace_back(L"-E");											
			args.emplace_back(to_wstring(info.function));						// Entry function
			args.emplace_back(L"-T");
			args.emplace_back(to_wstring(_profile_strings[(u32)info.type]));	// Target profile
			args.emplace_back(L"-I");
			args.emplace_back(to_wstring(shaders_source_path));					// Include path
			args.emplace_back(L"-enable-16bit-types");
			args.emplace_back(DXC_ARG_ALL_RESOURCES_BOUND);
#if _DEBUG
			args.emplace_back(DXC_ARG_DEBUG);
			args.emplace_back(DXC_ARG_SKIP_OPTIMIZATIONS);
#else
			args.emplace_back(DXC_ARG_OPTIMIZATION_LEVEL3);
#endif
			args.emplace_back(DXC_ARG_WARNINGS_ARE_ERRORS);
			args.emplace_back(L"-Qstrip_reflect");								// Strip reflections into a separate blob
			args.emplace_back(L"-QStrip_debug");								// Strip debug information into a separate blob

			for (const auto& arg : extra_args)
			{
				args.emplace_back(arg.c_str());
			}

			return args;
		}
		// NOTE: Shader Model 6.x can also be used (AS and MS are only supported from SM6.5 on).
		constexpr static const char*	_profile_strings[]{ "vs_6_6", "hs_6_6", "ds_6_6", "gs_6_6", "ps_6_6", "cs_6_6", "as_6_6", "ms_6_6"};
		static_assert(_countof(_profile_strings) == shader_type::count);

		ComPtr<IDxcCompiler3>			_compiler{ nullptr };
		ComPtr<IDxcUtils>				_utils{ nullptr };
		ComPtr<IDxcIncludeHandler>		_include_handle{ nullptr };
	};

	bool compiled_shaders_are_up_to_date()
	{
		// get the path to the compiled shaders binary file
		auto engine_shaders_path = get_engine_shaders_path();
		auto path1 = std::filesystem::absolute(engine_shaders_path);
		if (!std::filesystem::exists(engine_shaders_path)) return false;
		auto shaders_compilation_time = std::filesystem::last_write_time(engine_shaders_path);

		//std::filesystem::path full_path{};

		//// Check if either of engine shader source files is newer than the compiled shader file.
		//// In that case, we need to recompile.
		//for(u32 i{ 0 }; i < engine_shader::count; ++i)
		//{
		//	auto& file = engine_shader_files[i];

		//	full_path = shaders_source_path;
		//	full_path += file.info.file_name;
		//	if (!std::filesystem::exists(full_path)) return false;

		//	auto shader_file_time = std::filesystem::last_write_time(full_path);
		//	if (shader_file_time > shaders_compilation_time)
		//	{
		//		return false;
		//	}
		//}
		for (const auto& entry : std::filesystem::directory_iterator{ shaders_source_path })
		{
			if (entry.last_write_time() > shaders_compilation_time)
			{
				return false;
			}
		}
		return true;
	}

	bool save_compiled_shaders(utl::vector<dxc_compiled_shader>& shaders)
	{
		auto engine_shaders_path = get_engine_shaders_path();
		std::filesystem::create_directories(engine_shaders_path.parent_path());
		std::ofstream file(engine_shaders_path, std::ios::out | std::ios::binary);
		if (!file || !std::filesystem::exists(engine_shaders_path))
		{
			file.close();
			return false;
		}

		for (const auto& shader : shaders)
		{
			const D3D12_SHADER_BYTECODE byte_code{ shader.byte_code->GetBufferPointer(), shader.byte_code->GetBufferSize() };
			file.write((char*)&byte_code.BytecodeLength, sizeof(byte_code.BytecodeLength));
			file.write((char*)&shader.hash.HashDigest[0], _countof(shader.hash.HashDigest));
			file.write((char*)byte_code.pShaderBytecode, byte_code.BytecodeLength);
		}
		file.close();
		return true;
	}
} // anonymous napespace

std::unique_ptr<u8[]> compile_shader(shader_file_info info, const char* file_path, primal::utl::vector<std::wstring>& extra_args)
{
	std::filesystem::path full_path{ file_path };
	full_path += info.file_name;
	if (!std::filesystem::exists(full_path)) return {};

	// NOTE: according to marcelolr (https://github.com/Microsorf/DirectXShaderCompiler/issues/79)
	//		 "...creating colpiler instances is petty cheap, so it's probably not worth the hassle of caching / sharing them."
	shader_compiler compiler{};
	dxc_compiled_shader compiled_shader{ compiler.compile(info, full_path, extra_args) };

	if (compiled_shader.byte_code && compiled_shader.byte_code->GetBufferPointer() && compiled_shader.byte_code->GetBufferSize())
	{
		static_assert(content::compiled_shader::hash_length == _countof(DxcShaderHash::HashDigest));
		const u64 buffer_size{ sizeof(u64) + content::compiled_shader::hash_length + compiled_shader.byte_code->GetBufferSize() };
		std::unique_ptr<u8[]> buffer{ std::make_unique<u8[]>(buffer_size) };
		utl::blob_stream_writer blob{ buffer.get(), buffer_size };
		blob.write(compiled_shader.byte_code->GetBufferSize());
		blob.write(compiled_shader.hash.HashDigest, content::compiled_shader::hash_length);
		blob.write((u8*)compiled_shader.byte_code->GetBufferPointer(), compiled_shader.byte_code->GetBufferSize());

		assert(blob.offset() == buffer_size);
		return buffer;
	}

	return {};
}

bool compile_shaders()
{
	if (compiled_shaders_are_up_to_date()) return true;

	shader_compiler compiler{};
	utl::vector<dxc_compiled_shader> shaders;
	std::filesystem::path full_path{};

	// compile shaders and them together in a buffer in the same order of compilation.
	for (u32 i{ 0 }; i < engine_shader::count; ++i)
	{
		auto& file = engine_shader_files[i];

		full_path = shaders_source_path;
		full_path += file.info.file_name;
		auto p1 = std::filesystem::absolute(full_path);
		if (!std::filesystem::exists(full_path)) return false;
		utl::vector<std::wstring> extra_args{};

		if (file.id == engine_shader::grid_frustums_cs ||
			file.id == engine_shader::light_culling_cs)
		{
			// TODO: get TILE_SIZE valuye from d3d12
			extra_args.emplace_back(L"-D");
			extra_args.emplace_back(L"TILE_SIZE=32");
		}

		dxc_compiled_shader compiled_shader{ compiler.compile(file.info, full_path, extra_args) };
		if (compiled_shader.byte_code && compiled_shader.byte_code->GetBufferPointer() && compiled_shader.byte_code->GetBufferSize())
		{
			shaders.emplace_back(std::move(compiled_shader));
		}
		else
		{
			return false;
		}
	}

	return save_compiled_shaders(shaders);
}
#elif defined(__clang__)
#include "Graphics/Metal/MetalCore.h"
#include "Graphics/Metal/MetalShader.h"

#include "Content/ContentToEngine.h"
#include "Utilities/IOStream.h"

#include <functional>
#include <iostream>
#include <thread>

using namespace primal;
using namespace primal::graphics::metal::shader;

	namespace
	{
		// constexpr const char* shaders_source_path{ "../../Engine/Graphics/Metal/shaders/" };
		constexpr const char* shaders_source_path{ "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/" };

		struct engine_shader_info
		{
			engine_shader::id		id;
			shader_file_info		info;
		};

		constexpr engine_shader_info engine_shader_files[]
		{
			{ engine_shader::fullscreen_triangle_vs,		{ "FullScreenTriangle.metal", "fullscreen_triangle_vs", shader_type::vertex } },
			// { engine_shader::fill_color_ps,					{ "FillColor.hlsl", "FillColorPS", shader_type::pixel } },
			{ engine_shader::post_process_ps,				{ "PostProcess.metal", "post_process_ps", shader_type::pixel } },
			{ engine_shader::shadow_mapping_vs,				{ "DepthPassShader.metal", "shadow_mapping_vs", shader_type::vertex } },
			{ engine_shader::ssao_calculate,				{ "SSAOShader.metal", "ssao_pass", shader_type::compute } },
			{ engine_shader::ssao_blur,						{ "SSAOShader.metal", "ssao_blur", shader_type::compute } },
			{ engine_shader::ssgi_pass,						{ "SSGIShader.metal", "ssgi_pass", shader_type::compute } },
			{ engine_shader::ssgi_blur,						{ "SSGIShader.metal", "ssgi_blur", shader_type::compute } },
			{ engine_shader::taa_pass,						{ "TAAShader.metal", "taa_pass", shader_type::pixel } },
			{ engine_shader::compose_pass,					{ "Compose.metal", "compose_pass", shader_type::pixel } },
			// { engine_shader::grid_frustums_cs,				{ "GridFrustums.hlsl", "ComputeGridFrustumsCS", shader_type::compute } },
			// { engine_shader::light_culling_cs,				{ "CullingLights.hlsl", "CullLightsCS", shader_type::compute } }
		};

		static_assert(_countof(engine_shader_files) == engine_shader::count);

		decltype(auto) get_engine_shaders_path() { return std::filesystem::path{ graphics::get_engine_shaders_path(graphics::graphics_platform::metal) }; }

		[[maybe_unused]] std::wstring to_wstring(const char* c)
		{
			std::string s{ c };
			return { s.begin(), s.end() };
		}

		struct metal_compiled_shader
		{
			utl::vector<u8> byte_code{}; 											// 编译后的 Metal 着色器字节码
			std::array<u8, content::compiled_shader::hash_length> hash{};         	// 哈希值，与 compiled_shader 的 hash_length 一致
		};

		class shader_compiler
		{
		public:
			shader_compiler() = default;
			DISABLE_COPY_AND_MOVE(shader_compiler);

			metal_compiled_shader compile(shader_file_info info, std::filesystem::path full_path, primal::utl::vector<std::wstring>& extra_args)
			{
				// 读取着色器源文件
				std::ifstream file(full_path, std::ios::in);
				if (!file.is_open()) return {};
				
				std::stringstream buffer;
				buffer << file.rdbuf();
				std::string source = buffer.str();
				file.close();
				
				if (source.empty()) return {};
			
				std::cout << "Compiling " << info.file_name << " : " << info.function << "\n";
				
				return compile(source, get_args(info, extra_args));
			}

			metal_compiled_shader compile(const std::string& source, primal::utl::vector<std::string> compiler_args)
			{
				metal_compiled_shader result{};
				
				// 生成随机文件名后缀
				std::random_device rd;
				std::mt19937 gen(rd());
				std::uniform_int_distribution<> dis(100000, 999999);
				std::stringstream ss;
				ss << std::hex << dis(gen);
				std::string random_suffix = ss.str();
				
				// 创建临时源文件
				std::string temp_source_file = "/tmp/shader_temp_" + random_suffix + ".metal";
				std::ofstream source_file(temp_source_file);
				if (!source_file.is_open()) return result;
				source_file << source;
				source_file.close();
				
				// 创建临时IR文件和metallib文件
				std::string temp_ir_file = "/tmp/shader_temp_" + random_suffix + ".ir";
				std::string temp_metalar_file = "/tmp/shader_temp_" + random_suffix + ".metalar";
				std::string temp_metallib_file = "/tmp/shader_temp_" + random_suffix + ".metallib";
				
				// 步骤1: 使用metal命令编译为IR
				std::string compile_cmd = "xcrun -sdk macosx metal -o " + temp_ir_file + " -c " + temp_source_file;
				
				// 添加编译参数
				for (const auto& arg : compiler_args)
				{
					compile_cmd += " " + arg;
				}
				
				// 执行编译命令并捕获输出
				std::string output;
				FILE* pipe = popen((compile_cmd + " 2>&1").c_str(), "r");
				if (!pipe) {
					std::cout << "无法执行Metal编译命令" << std::endl;
					std::remove(temp_source_file.c_str());
					return result;
				}
				
				char buffer[512];
				while (!feof(pipe)) {
					if (fgets(buffer, 512, pipe) != nullptr)
						output += buffer;
				}
				int compile_result = pclose(pipe);
				
				if (compile_result != 0) {
					std::cout << "\nShader compilation error: \n" << output << std::endl;
					std::cout << "Compilation command: " << compile_cmd << std::endl;
					std::remove(temp_source_file.c_str());
					return result;
				}
				else {
					std::cout << " [ IR编译成功 ]\n";
				}
				
				// 步骤2: 使用metal-ar创建metalar文件
				std::string ar_cmd = "xcrun -sdk macosx metal-ar -q " + temp_metalar_file + " " + temp_ir_file;
				pipe = popen((ar_cmd + " 2>&1").c_str(), "r");
				if (!pipe) {
					std::cout << "无法执行Metal-ar命令" << std::endl;
					std::remove(temp_source_file.c_str());
					std::remove(temp_ir_file.c_str());
					return result;
				}
				
				output.clear();
				while (!feof(pipe)) {
					if (fgets(buffer, 512, pipe) != nullptr)
						output += buffer;
				}
				int ar_result = pclose(pipe);
				
				if (ar_result != 0) {
					std::cout << "\nMetal-ar error: \n" << output << std::endl;
					std::cout << "AR command: " << ar_cmd << std::endl;
					std::remove(temp_source_file.c_str());
					std::remove(temp_ir_file.c_str());
					return result;
				}
				else {
					std::cout << " [ Metal-ar成功 ]\n";
				}
				
				// 步骤3: 使用metallib创建最终的metallib文件
				std::string lib_cmd = "xcrun -sdk macosx metallib -o " + temp_metallib_file + " " + temp_metalar_file;
				pipe = popen((lib_cmd + " 2>&1").c_str(), "r");
				if (!pipe) {
					std::cout << "无法执行Metallib命令" << std::endl;
					std::remove(temp_source_file.c_str());
					std::remove(temp_ir_file.c_str());
					std::remove(temp_metalar_file.c_str());
					return result;
				}
				
				output.clear();
				while (!feof(pipe)) {
					if (fgets(buffer, 512, pipe) != nullptr)
						output += buffer;
				}
				int lib_result = pclose(pipe);
				
				if (lib_result != 0) {
					std::cout << "\nMetallib error: \n" << output << std::endl;
					std::cout << "Lib command: " << lib_cmd << std::endl;
				}
				else {
					std::cout << " [ Metallib成功 ]\n";
				}
				
				// 如果编译成功，读取metallib文件
				if (lib_result == 0) {
					std::ifstream metallib_file(temp_metallib_file, std::ios::binary);
					if (metallib_file.is_open()) {
						metallib_file.seekg(0, std::ios::end);
						size_t size = metallib_file.tellg();
						metallib_file.seekg(0, std::ios::beg);
						
						result.byte_code.resize(size);
						metallib_file.read(reinterpret_cast<char*>(result.byte_code.data()), size);
						metallib_file.close();
						
						// 计算哈希值
						std::hash<std::string> hasher;
						u64 hash_value = hasher(std::string(result.byte_code.begin(), result.byte_code.end()));
						
						std::cout << "Shader hash: ";
						for (u64 i{ 0 }; i < content::compiled_shader::hash_length && i < sizeof(hash_value); ++i) {
							result.hash[i] = (hash_value >> (i * 8)) & 0xFF;
							char hash_bytes[3]{};
							sprintf(hash_bytes, "%02x", (u32)result.hash[i]);
							std::cout << hash_bytes << " ";
						}
						std::cout << "\n";
					}
				}
				
				// 清理临时文件
				std::remove(temp_source_file.c_str());
				std::remove(temp_ir_file.c_str());
				std::remove(temp_metalar_file.c_str());
				std::remove(temp_metallib_file.c_str());
				
				return result;
			}

		private:
			utl::vector<std::string> get_args([[maybe_unused]] const shader_file_info& info, utl::vector<std::wstring>& extra_args)
			{
				utl::vector<std::string> args{};
				
				// Metal编译器基本参数
				// args.emplace_back("-std=macos-metal2.4");     // 使用Metal 2.4标准
				args.emplace_back("-Wall");                   // 启用所有警告
				args.emplace_back("-Wno-unused-variable");    // 禁用未使用变量警告
				args.emplace_back("-I " + std::string(shaders_source_path)); // Include路径
				
			#if _DEBUG
				args.emplace_back("-gline-tables-only");      // 添加调试信息
				args.emplace_back("-frecord-sources");        // 记录源文件信息
				args.emplace_back("-O0");                     // 禁用优化
			#else
				args.emplace_back("-O3");                     // 最高优化级别
			#endif
				
				// 添加额外的编译参数
				for (const auto& arg : extra_args)
				{
					args.emplace_back(std::string(arg.begin(), arg.end()));
				}
				
				return args;
			}
		};

		bool compiled_shaders_are_up_to_date()
		{
			// get the path to the compiled shaders binary file
			auto engine_shaders_path = get_engine_shaders_path();
			auto path1 = std::filesystem::absolute(engine_shaders_path);
			if (!std::filesystem::exists(path1)) return false;
			auto shaders_compilation_time = std::filesystem::last_write_time(engine_shaders_path);

			std::filesystem::path full_path{};

			//// Check if either of engine shader source files is newer than the compiled shader file.
			//// In that case, we need to recompile.
			for(u32 i{ 0 }; i < engine_shader::count; ++i)
			{
				auto& file = engine_shader_files[i];
				full_path = shaders_source_path;
				full_path += file.info.file_name;
				if (!std::filesystem::exists(full_path)) return false;
				auto shader_file_time = std::filesystem::last_write_time(full_path);
				if (shader_file_time > shaders_compilation_time)
				{
					return false;
				}
			}
			for (const auto& entry : std::filesystem::directory_iterator{ shaders_source_path })
			{
				if (entry.last_write_time() > shaders_compilation_time)
				{
					return false;
				}
			}
			return true;
		}
		

		bool save_compiled_shaders(utl::vector<metal_compiled_shader>& shaders)
		{
			auto engine_shaders_path = get_engine_shaders_path();
			std::filesystem::create_directories(engine_shaders_path.parent_path());
			std::ofstream file(engine_shaders_path, std::ios::out | std::ios::binary);
			if (!file || !std::filesystem::exists(engine_shaders_path))
			{
				file.close();
				return false;
			}

			for (const auto& shader : shaders)
			{
				// 写入 compiled_shader 格式的数据
				u64 byte_code_size = shader.byte_code.size();
				file.write(reinterpret_cast<const char*>(&byte_code_size), sizeof(u64)); // _byte_code_size
				file.write(reinterpret_cast<const char*>(shader.hash.data()), content::compiled_shader::hash_length); // _hash
				file.write(reinterpret_cast<const char*>(shader.byte_code.data()), byte_code_size); // _byte_code
			}
			file.close();
			return true;
		}
	} // anonymous namespace

	std::unique_ptr<u8[]> compile_shader(shader_file_info info, const char* file_path, primal::utl::vector<std::wstring>& extra_args)
	{
		std::filesystem::path full_path{ file_path };
		full_path += info.file_name;
		std::filesystem::path absolute_path{ std::filesystem::absolute(full_path) };
		if (!std::filesystem::exists(full_path)) return {};

		shader_compiler compiler{};
		metal_compiled_shader compiled_shader{ compiler.compile(info, full_path, extra_args) };

		if (!compiled_shader.byte_code.empty())
		{
			const u64 buffer_size{ sizeof(u64) + content::compiled_shader::hash_length + compiled_shader.byte_code.size() };
			std::unique_ptr<u8[]> buffer{ std::make_unique<u8[]>(buffer_size) };
			utl::blob_stream_writer blob{ buffer.get(), buffer_size };
			
			blob.write(compiled_shader.byte_code.size()); // _byte_code_size
        	blob.write(compiled_shader.hash.data(), content::compiled_shader::hash_length); // _hash
        	blob.write(compiled_shader.byte_code.data(), compiled_shader.byte_code.size()); // _byte_code

			assert(blob.offset() == buffer_size);
			return buffer;
		}

		return {};
	}

	bool compile_shaders()
	{
		if (compiled_shaders_are_up_to_date()) return true;

		shader_compiler compiler{};
		utl::vector<metal_compiled_shader> shaders{};
		std::filesystem::path full_path{};
		
		for (u32 i{ 0 }; i < engine_shader::count; ++i)
		{
			auto& file = engine_shader_files[i];
	
			full_path = shaders_source_path;
			full_path += file.info.file_name;
			auto p1 = std::filesystem::absolute(full_path);
			if (!std::filesystem::exists(full_path)) return false;
			utl::vector<std::wstring> extra_args{};
	
			metal_compiled_shader compiled_shader{ compiler.compile(file.info, full_path, extra_args) };
			if (!compiled_shader.byte_code.empty())
			{
				shaders.emplace_back(std::move(compiled_shader));
			}
			else
			{
				return false;
			}
		}

		return save_compiled_shaders(shaders);
	}
#endif