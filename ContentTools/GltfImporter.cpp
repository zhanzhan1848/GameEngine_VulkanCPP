#include "GltfImporter.h"

#include "Geometry.h"
#include "../Engine/Utilities/Hash.h"
#include "../Engine/ParticleFX/core/PfxJson.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace primal::tools
{
	namespace
	{
		// ---------------------------------------------------------------------------
		// glTF 2.0 constants
		// ---------------------------------------------------------------------------
		constexpr u32 GLB_MAGIC{ 0x46546C67 };       // 'glTF'
		constexpr u32 CHUNK_TYPE_JSON{ 0x4E4F534A }; // 'JSON'
		constexpr u32 CHUNK_TYPE_BIN{ 0x004E4942 };  // 'BIN\0'

		u32 component_size(u32 component_type)
		{
			switch (component_type)
			{
			case 5120: return 1;  // BYTE
			case 5121: return 1;  // UNSIGNED_BYTE
			case 5122: return 2;  // SHORT
			case 5123: return 2;  // UNSIGNED_SHORT
			case 5125: return 4;  // UNSIGNED_INT
			case 5126: return 4;  // FLOAT
			default:   return 0;
			}
		}

		u32 components_per_element(const char* type)
		{
			if (!std::strcmp(type, "SCALAR")) return 1;
			if (!std::strcmp(type, "VEC2"))   return 2;
			if (!std::strcmp(type, "VEC3"))   return 3;
			if (!std::strcmp(type, "VEC4"))   return 4;
			if (!std::strcmp(type, "MAT4"))   return 16;
			return 0;
		}

		// ---------------------------------------------------------------------------
		// base64 (data URI payloads)
		// ---------------------------------------------------------------------------
		bool base64_decode(const std::string& in, std::vector<u8>& out)
		{
			auto value_of = [](char c) -> s32
			{
				if (c >= 'A' && c <= 'Z') return c - 'A';
				if (c >= 'a' && c <= 'z') return c - 'a' + 26;
				if (c >= '0' && c <= '9') return c - '0' + 52;
				if (c == '+') return 62;
				if (c == '/') return 63;
				return -1;
			};

			out.clear();
			out.reserve(in.size() / 4 * 3);

			u32 acc{ 0 };
			u32 bits{ 0 };
			for (char c : in)
			{
				if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
				const s32 v{ value_of(c) };
				if (v < 0) return false;
				acc = (acc << 6) | (u32)v;
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					out.emplace_back((u8)((acc >> bits) & 0xFF));
				}
			}
			return true;
		}

		bool read_file_bytes(const std::string& path, std::vector<u8>& out)
		{
			std::ifstream f{ path, std::ios::binary };
			if (!f.is_open()) return false;
			f.seekg(0, std::ios::end);
			const std::streamoff size{ f.tellg() };
			if (size <= 0) return false;
			f.seekg(0, std::ios::beg);
			out.resize((size_t)size);
			f.read(reinterpret_cast<char*>(out.data()), size);
			return (size_t)f.gcount() == (size_t)size;
		}

		bool write_file_bytes(const std::string& path, const u8* data, size_t size)
		{
			std::ofstream f{ path, std::ios::binary | std::ios::trunc };
			if (!f.is_open()) return false;
			f.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
			return f.good();
		}

		bool make_dir(const std::string& path)
		{
#if defined(_MSC_VER)
			return _mkdir(path.c_str()) == 0;
#else
			return ::mkdir(path.c_str(), 0755) == 0;
#endif
		}

		bool dir_exists(const std::string& path)
		{
#if defined(_MSC_VER)
			const DWORD attr{ GetFileAttributesA(path.c_str()) };
			return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
			struct stat s{};
			return ::stat(path.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
#endif
		}

		bool looks_absolute(const std::string& path)
		{
			return !path.empty() && (path[0] == '/' ||
				(path.size() > 1 && path[1] == ':'));  // drive letter (Windows)
		}

		// ---------------------------------------------------------------------------
		// Column-major 4x4 (glTF convention: m[0..3] is the first COLUMN).
		// ---------------------------------------------------------------------------
		struct m4x4
		{
			f32 m[16];

			static m4x4 identity()
			{
				m4x4 r{};
				r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
				return r;
			}
		};

		m4x4 mat_mul(const m4x4& a, const m4x4& b)
		{
			m4x4 out{};
			for (u32 col{ 0 }; col < 4; ++col)
				for (u32 row{ 0 }; row < 4; ++row)
				{
					f32 sum{ 0.f };
					for (u32 k{ 0 }; k < 4; ++k)
						sum += a.m[k * 4 + row] * b.m[col * 4 + k];
					out.m[col * 4 + row] = sum;
				}
			return out;
		}

		void transform_point(const m4x4& t, math::v3& p)
		{
			const f32 x{ p.x }, y{ p.y }, z{ p.z };
			p.x = t.m[0] * x + t.m[4] * y + t.m[8] * z + t.m[12];
			p.y = t.m[1] * x + t.m[5] * y + t.m[9] * z + t.m[13];
			p.z = t.m[2] * x + t.m[6] * y + t.m[10] * z + t.m[14];
		}

		// Normals use the inverse-transpose of the upper-left 3x3 (correct under
		// non-uniform scale). Degenerate matrices leave the normal untouched.
		void transform_normal(const m4x4& t, math::v3& n)
		{
			// A[row][col] = t.m[col * 4 + row]
			const f32 a00{ t.m[0] }, a01{ t.m[4] }, a02{ t.m[8] };
			const f32 a10{ t.m[1] }, a11{ t.m[5] }, a12{ t.m[9] };
			const f32 a20{ t.m[2] }, a21{ t.m[6] }, a22{ t.m[10] };

			const f32 det{ a00 * (a11 * a22 - a12 * a21)
						 - a01 * (a10 * a22 - a12 * a20)
						 + a02 * (a10 * a21 - a11 * a20) };
			if (std::fabs(det) < 1e-12f) return;

			const f32 i00{ (a11 * a22 - a12 * a21) / det };
			const f32 i01{ (a02 * a21 - a01 * a22) / det };  // inv[0][1]
			const f32 i02{ (a01 * a12 - a02 * a11) / det };  // inv[0][2]
			const f32 i10{ (a12 * a20 - a10 * a22) / det };  // inv[1][0]
			const f32 i11{ (a00 * a22 - a02 * a20) / det };
			const f32 i12{ (a02 * a10 - a00 * a12) / det };
			const f32 i20{ (a10 * a21 - a11 * a20) / det };  // inv[2][0]
			const f32 i21{ (a01 * a20 - a00 * a21) / det };
			const f32 i22{ (a00 * a11 - a01 * a10) / det };

			const f32 x{ n.x }, y{ n.y }, z{ n.z };
			// n' = (inv(A))^T · n
			math::v3 out{
				i00 * x + i10 * y + i20 * z,
				i01 * x + i11 * y + i21 * z,
				i02 * x + i12 * y + i22 * z
			};
			const f32 len{ std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z) };
			if (len > 1e-12f)
			{
				out.x /= len; out.y /= len; out.z /= len;
			}
			n = out;
		}

		// ---------------------------------------------------------------------------
		// Accessor reader: element-wise float / index access over a resolved buffer.
		// ---------------------------------------------------------------------------
		struct accessor_reader
		{
			const u8* base{ nullptr };
			size_t count{ 0 };
			u32 stride{ 0 };        // bytes between elements
			u32 ncomp{ 0 };
			u32 ctype{ 0 };         // glTF componentType
			bool normalized{ false };

			constexpr bool valid() const { return base != nullptr && ncomp > 0; }
		};

		f32 accessor_comp(const accessor_reader& a, size_t elem, u32 c)
		{
			const u32 csize{ component_size(a.ctype) };
			const u8* p{ a.base + elem * a.stride + c * csize };
			switch (a.ctype)
			{
			case 5126: { f32 v; std::memcpy(&v, p, 4); return v; }
			case 5125: { u32 v; std::memcpy(&v, p, 4); return (f32)v; }
			case 5123: { u16 v; std::memcpy(&v, p, 2); return a.normalized ? (f32)v / 65535.f : (f32)v; }
			case 5122: { s16 v; std::memcpy(&v, p, 2);
						 const f32 f{ (f32)v / 32767.f };
						 return a.normalized ? (f < -1.f ? -1.f : f) : (f32)v; }
			case 5121: { const u8 v{ *p }; return a.normalized ? (f32)v / 255.f : (f32)v; }
			case 5120: { const s8 v{ (s8)*p };
						 const f32 f{ (f32)v / 127.f };
						 return a.normalized ? (f < -1.f ? -1.f : f) : (f32)v; }
			default: return 0.f;
			}
		}

		u32 accessor_index(const accessor_reader& a, size_t elem)
		{
			return (u32)accessor_comp(a, elem, 0);
		}

		// ---------------------------------------------------------------------------
		// OBJ-style recursive LOD chain (behavioral twin of
		// ObjImporter.cpp obj_context::get_lod_group): each level simplifies to
		// 80% index count via meshopt_simplify, small meshes pass through.
		// ---------------------------------------------------------------------------
		void generate_lod_chain(const utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold,
			utl::vector<lod_group>& out)
		{
			if (lod_id > 5 || meshes.empty()) return;

			lod_group lod{};
			lod.name = meshes[0].name + "_LODS" + std::to_string(lod_id);
			for (const auto& m : meshes)
			{
				mesh submesh{};
				submesh.positions = m.positions;
				submesh.name = m.name;
				submesh.lod_id = lod_id;
				submesh.lod_threshold = lod_threshold;
				submesh.colors = m.colors;
				submesh.normals = m.normals;
				submesh.tangents = m.tangents;
				submesh.uv_sets = m.uv_sets;
				submesh.material_indices = m.material_indices;
				submesh.material_used = m.material_used;
				if (m.raw_indices.size() > 24)
				{
					u32 target_index_count{ (u32)std::floor(m.raw_indices.size() * 0.8f)
										  - (u32)std::floor(m.raw_indices.size() * 0.8f) % 3 };
					utl::vector<u32> dst_indices(m.raw_indices.size());
					f32 error{ 0.f };
					const u64 simplified{ meshopt_simplify(dst_indices.data(),
						m.raw_indices.data(),
						m.raw_indices.size(),
						reinterpret_cast<const f32*>(m.positions.data()),
						m.positions.size(),
						sizeof(math::v3),
						(u64)target_index_count,
						0.01f,
						0u,
						&error) };
					dst_indices.resize(simplified);
					submesh.raw_indices = dst_indices;
				}
				else
				{
					submesh.raw_indices = m.raw_indices;
				}
				lod.meshes.emplace_back(std::move(submesh));
			}
			if (lod.meshes.empty()) return;

			out.emplace_back(std::move(lod));
			// Recurse on the stored level (out may reallocate; the reference is
			// only evaluated for the call itself).
			generate_lod_chain(out.back().meshes, lod_id + 1, lod_threshold + 20.f, out);
		}

	}  // anonymous namespace

	// ---------------------------------------------------------------------------
	// gltf_context::impl
	// ---------------------------------------------------------------------------
	struct gltf_context::impl
	{
		scene* scene_ptr{ nullptr };
		scene_data* scene_data_ptr{ nullptr };
		progression* prog{ nullptr };

		std::string source_path;
		std::string source_dir;     // with trailing separator ("" if cwd)
		std::string stem;
		bool is_glb{ false };
		std::vector<u8> file_bytes;

		pfx::json::parse_result json;
		std::vector<u8> bin_chunk;              // GLB BIN chunk (buffer 0)
		std::vector<std::vector<u8>> buffers;   // resolved buffer payloads
		std::vector<bool> buffer_resolved;

		std::string extract_dir;                // where embedded images land
		bool extract_dir_ready{ false };
		std::map<u32, std::string> image_refs;  // image index → sidecar reference

		// --- JSON helpers -------------------------------------------------------
		const pfx::json::value* obj_at(const pfx::json::value& v, const char* key) const
		{
			return v.is_object() ? v.find(key) : nullptr;
		}

		u32 get_u32(const pfx::json::value& v, const char* key, u32 def) const
		{
			const pfx::json::value* f{ obj_at(v, key) };
			return (f && f->is_number()) ? f->as_u32() : def;
		}

		const pfx::json::value* array_elem(const pfx::json::value& v, const char* key, u32 idx) const
		{
			const pfx::json::value* arr{ obj_at(v, key) };
			if (!arr || !arr->is_array()) return nullptr;
			return arr->array_at(idx);
		}

		// Array-valued member (e.g. node "scale" / "rotation" / "translation").
		const pfx::json::value* array_member(const pfx::json::value& v, const char* key) const
		{
			const pfx::json::value* arr{ obj_at(v, key) };
			return (arr && arr->is_array()) ? arr : nullptr;
		}

		u32 array_count(const pfx::json::value& v, const char* key) const
		{
			const pfx::json::value* arr{ obj_at(v, key) };
			return (arr && arr->is_array()) ? (u32)arr->array_size() : 0;
		}

		// --- loading --------------------------------------------------------------
		bool load(const char* file)
		{
			source_path = file;

			const size_t slash{ source_path.find_last_of("/\\") };
			if (slash != std::string::npos)
			{
				source_dir = source_path.substr(0, slash + 1);
				stem = source_path.substr(slash + 1);
			}
			else
			{
				stem = source_path;
			}
			const size_t dot{ stem.find_last_of('.') };
			if (dot != std::string::npos) stem = stem.substr(0, dot);
			if (stem.empty()) stem = "gltf_scene";

			if (!read_file_bytes(source_path, file_bytes))
			{
				std::cerr << "GltfImporter: cannot read file '" << source_path << "'\n";
				return false;
			}

			const size_t ext_dot{ source_path.find_last_of('.') };
			const std::string ext{ ext_dot == std::string::npos
				? std::string{} : source_path.substr(ext_dot + 1) };
			is_glb = (ext == "glb" || ext == "GLB");

			const char* json_text{ nullptr };
			size_t json_size{ 0 };

			if (is_glb)
			{
				if (file_bytes.size() < 20)
				{
					std::cerr << "GltfImporter: GLB container too small\n";
					return false;
				}
				u32 magic{ 0 }, version{ 0 }, total{ 0 };
				std::memcpy(&magic, file_bytes.data(), 4);
				std::memcpy(&version, file_bytes.data() + 4, 4);
				std::memcpy(&total, file_bytes.data() + 8, 4);
				if (magic != GLB_MAGIC)
				{
					std::cerr << "GltfImporter: bad GLB magic\n";
					return false;
				}
				if (version != 2)
				{
					std::cerr << "GltfImporter: unsupported GLB version " << version << " (need 2)\n";
					return false;
				}

				size_t pos{ 12 };
				while (pos + 8 <= file_bytes.size() && pos + 8 <= total)
				{
					u32 len{ 0 }, type{ 0 };
					std::memcpy(&len, file_bytes.data() + pos, 4);
					std::memcpy(&type, file_bytes.data() + pos + 4, 4);
					pos += 8;
					if (pos + len > file_bytes.size()) break;

					if (type == CHUNK_TYPE_JSON && !json_text)
					{
						json_text = reinterpret_cast<const char*>(file_bytes.data() + pos);
						json_size = len;
					}
					else if (type == CHUNK_TYPE_BIN && bin_chunk.empty())
					{
						bin_chunk.assign(file_bytes.begin() + pos, file_bytes.begin() + pos + len);
					}
					// Chunks start on 4-byte boundaries: the length field
					// excludes the tail padding, so round pos up to the next
					// aligned offset before reading the next chunk header.
					pos = (pos + len + 3) & ~size_t(3);
				}
				if (!json_text)
				{
					std::cerr << "GltfImporter: GLB container has no JSON chunk\n";
					return false;
				}
			}
			else
			{
				json_text = reinterpret_cast<const char*>(file_bytes.data());
				json_size = file_bytes.size();
			}

			json = pfx::json::parse(json_text, json_size);
			if (!json.ok || !json.root)
			{
				std::cerr << "GltfImporter: JSON parse failed at line " << json.line
						  << ":" << json.column << " — " << json.error << "\n";
				return false;
			}

			// glTF 2.0 only; refuse required extensions we don't implement
			// (Draco compression, mesh quantization, BasisU textures…).
			// character-animation S1（2026-10-15）导入审计落地：skins/animations
			// 此前从未检查——含骨骼/动画的文件被静默按静态导入（与头文件声明的
			// "拒绝"矛盾）。现在按声明拒绝并点名特性：蒙皮/动画导入属
			// character-animation 后续阶段（蒙皮渲染待渲染专项协调）。
			if (array_count(*json.root, "skins") > 0)
			{
				std::cerr << "GltfImporter: skins are not supported yet — "
						  << "file contains " << array_count(*json.root, "skins")
						  << " skin(s); imported as static geometry would silently "
						  << "drop deformation (character-animation 阶段引入蒙皮)\n";
				return false;
			}
			if (array_count(*json.root, "animations") > 0)
			{
				std::cerr << "GltfImporter: animations are not supported yet — "
						  << "file contains " << array_count(*json.root, "animations")
						  << " animation(s); animation import is deferred with the "
						  << "character-animation module\n";
				return false;
			}
			if (array_count(*json.root, "extensionsRequired") > 0)
			{
				const pfx::json::value* first{ array_elem(*json.root, "extensionsRequired", 0) };
				std::cerr << "GltfImporter: unsupported required extension '"
						  << (first && first->is_string() ? first->as_string() : "?")
						  << "' (only plain glTF 2.0 is supported)\n";
				return false;
			}
			if (const pfx::json::value* asset{ obj_at(*json.root, "asset") })
			{
				if (const pfx::json::value* ver{ obj_at(*asset, "version") };
					ver && ver->is_string() && ver->as_string().compare(0, 1, "2") != 0)
				{
					std::cerr << "GltfImporter: unsupported glTF version '"
							  << ver->as_string() << "' (need 2.x)\n";
					return false;
				}
			}

			buffers.resize(array_count(*json.root, "buffers"));
			buffer_resolved.assign(buffers.size(), false);
			return true;
		}

		// --- buffers ----------------------------------------------------------------
		bool resolve_buffer(u32 index)
		{
			if (index >= buffers.size()) return false;
			if (buffer_resolved[index]) return true;

			const pfx::json::value* buf{ array_elem(*json.root, "buffers", index) };
			if (!buf) return false;

			const pfx::json::value* uri{ obj_at(*buf, "uri") };
			if (!uri || !uri->is_string())
			{
				// No uri → the GLB BIN chunk (buffer 0).
				if (is_glb && index == 0 && !bin_chunk.empty())
				{
					buffers[index] = bin_chunk;
					buffer_resolved[index] = true;
					return true;
				}
				std::cerr << "GltfImporter: buffer " << index << " has no uri (GLB BIN chunk missing?)\n";
				return false;
			}

			const std::string& u{ uri->as_string() };
			if (u.compare(0, 5, "data:") == 0)
			{
				const size_t comma{ u.find(',') };
				if (comma == std::string::npos)
				{
					std::cerr << "GltfImporter: buffer " << index << " has malformed data URI\n";
					return false;
				}
				std::vector<u8> decoded;
				if (!base64_decode(u.substr(comma + 1), decoded))
				{
					std::cerr << "GltfImporter: buffer " << index << " data URI base64 decode failed\n";
					return false;
				}
				buffers[index] = std::move(decoded);
				buffer_resolved[index] = true;
				return true;
			}

			std::string path{ source_dir };
			path.append(u);
			if (!read_file_bytes(path, buffers[index]))
			{
				std::cerr << "GltfImporter: cannot read buffer file '" << path << "'\n";
				return false;
			}
			buffer_resolved[index] = true;
			return true;
		}

		// Build a reader over accessors[index]. expect_ncomp: 0 = any.
		bool build_accessor(u32 index, u32 expect_ncomp, bool allow_normalized, accessor_reader& out)
		{
			const pfx::json::value* acc{ array_elem(*json.root, "accessors", index) };
			if (!acc)
			{
				std::cerr << "GltfImporter: accessor " << index << " out of range\n";
				return false;
			}
			if (obj_at(*acc, "sparse"))
			{
				std::cerr << "GltfImporter: sparse accessors are not supported (accessor " << index << ")\n";
				return false;
			}

			const u32 view_index{ get_u32(*acc, "bufferView", u32_invalid_id) };
			if (view_index == u32_invalid_id)
			{
				std::cerr << "GltfImporter: accessor " << index << " has no bufferView\n";
				return false;
			}
			const pfx::json::value* view{ array_elem(*json.root, "bufferViews", view_index) };
			if (!view) return false;

			const u32 buf_index{ get_u32(*view, "buffer", u32_invalid_id) };
			if (buf_index == u32_invalid_id || !resolve_buffer(buf_index)) return false;

			const u32 ctype{ get_u32(*acc, "componentType", 0) };
			const pfx::json::value* type{ obj_at(*acc, "type") };
			const u32 ncomp{ type && type->is_string() ? components_per_element(type->as_string().c_str()) : 0 };
			const u32 csize{ component_size(ctype) };
			if (!csize || !ncomp)
			{
				std::cerr << "GltfImporter: accessor " << index << " has bad componentType/type\n";
				return false;
			}
			if (expect_ncomp && ncomp != expect_ncomp)
			{
				std::cerr << "GltfImporter: accessor " << index << " expected " << expect_ncomp
						  << " components, got " << ncomp << "\n";
				return false;
			}

			const bool normalized{ get_u32(*acc, "normalized", 0) != 0 };
			if (normalized && !allow_normalized)
			{
				std::cerr << "GltfImporter: accessor " << index << " unexpectedly normalized\n";
				return false;
			}

			const u64 acc_offset{ get_u32(*acc, "byteOffset", 0) };
			const u64 view_offset{ get_u32(*view, "byteOffset", 0) };
			const u64 elem_size{ (u64)ncomp * csize };

			// Indices accessors must be tight (spec: byteStride shall not be set).
			// Vertex accessors may be interleaved via the view-level byteStride.
			u64 stride{ elem_size };
			if (ncomp > 1)
			{
				const u64 view_stride{ get_u32(*view, "byteStride", 0) };
				if (view_stride)
				{
					if (view_stride < elem_size)
					{
						std::cerr << "GltfImporter: bufferView " << view_index << " byteStride < element size\n";
						return false;
					}
					stride = view_stride;
				}
			}

			const u64 count{ get_u32(*acc, "count", 0) };
			if (count == 0)
			{
				std::cerr << "GltfImporter: accessor " << index << " is empty\n";
				return false;
			}
			const std::vector<u8>& payload{ buffers[buf_index] };
			const u64 start{ view_offset + acc_offset };
			const u64 needed{ start + (count - 1) * stride + elem_size };
			if (needed > payload.size())
			{
				std::cerr << "GltfImporter: accessor " << index << " exceeds buffer bounds ("
						  << needed << " > " << payload.size() << ")\n";
				return false;
			}

			out.base = payload.data() + start;
			out.count = (size_t)count;
			out.stride = (u32)stride;
			out.ncomp = ncomp;
			out.ctype = ctype;
			out.normalized = normalized;
			return true;
		}

		// --- embedded image extraction -------------------------------------------------
		const char* mime_ext(const std::string& mime) const
		{
			if (mime.find("png") != std::string::npos)  return ".png";
			if (mime.find("jpeg") != std::string::npos) return ".jpg";
			if (mime.find("jpg") != std::string::npos)  return ".jpg";
			return ".bin";
		}

		// Writability probe (tiny throwaway file) so unwritable source dirs
		// fall back to temp instead of failing per-image.
		bool write_probe(const std::string& dir) const
		{
			std::string probe{ dir };
			if (probe.back() != '/') probe.push_back('/');
			probe.append(".write_probe");
			const bool ok{ write_file_bytes(probe, (const u8*)"ok", 2) };
			if (ok) std::remove(probe.c_str());
			return ok;
		}

		// Extract raw image bytes so the C# importer can cook them. Preference:
		// "<source_dir>/<stem>_extracted/" (moves with the source asset). If the
		// source directory is not writable, fall back to a temp dir and record an
		// ABSOLUTE path — the C# texture resolver passes absolute references
		// through unchanged (Path.Combine semantics).
		std::string extract_image(u32 image_index, const std::string& mime, const u8* data, size_t size)
		{
			{
				const auto it{ image_refs.find(image_index) };
				if (it != image_refs.end()) return it->second;
			}

			if (!extract_dir_ready)
			{
				std::string preferred{ source_dir };
				preferred.append(stem).append("_extracted");
				if ((dir_exists(preferred) || make_dir(preferred)) && write_probe(preferred))
				{
					extract_dir = preferred;
				}
				else
				{
					const char* tmp{ std::getenv("TMPDIR") };
					extract_dir = (tmp && *tmp) ? std::string{ tmp } : std::string{ "/tmp" };
					if (extract_dir.back() != '/') extract_dir.push_back('/');
					extract_dir.append("primal_gltf_extract");
					make_dir(extract_dir);
				}
				extract_dir_ready = true;
			}

			const std::string filename{ "embedded_" + std::to_string(image_index) + mime_ext(mime) };
			std::string full{ extract_dir };
			if (full.back() != '/') full.push_back('/');
			full.append(filename);

			std::string ref;
			if (write_file_bytes(full, data, size))
			{
				if (looks_absolute(extract_dir))
				{
					ref = full;
				}
				else
				{
					// Relative to the source dir — the C# texture lookup BFS
					// searches the source tree by filename/path.
					ref = stem;
					ref.append("_extracted/").append(filename);
				}
			}
			else
			{
				std::cerr << "GltfImporter: failed to write embedded image '" << full << "'\n";
			}
			image_refs[image_index] = ref;
			return ref;
		}

		// Texture reference for the sidecar: external URIs pass through
		// (FBX-style relative filename convention); embedded images are
		// extracted to disk first.
		std::string resolve_texture(u32 texture_index)
		{
			const pfx::json::value* tex{ array_elem(*json.root, "textures", texture_index) };
			if (!tex) return {};
			const u32 source{ get_u32(*tex, "source", u32_invalid_id) };
			if (source == u32_invalid_id) return {};

			const pfx::json::value* image{ array_elem(*json.root, "images", source) };
			if (!image) return {};

			if (const pfx::json::value* uri{ obj_at(*image, "uri") })
			{
				if (!uri->is_string()) return {};
				const std::string& u{ uri->as_string() };
				if (u.compare(0, 5, "data:") == 0)
				{
					const size_t comma{ u.find(',') };
					if (comma == std::string::npos) return {};
					// "data:image/png;base64,...." — mime between ':' and ';'.
					const size_t semi{ u.find(';') };
					const std::string mime{ u.substr(5, semi == std::string::npos ? comma - 5 : semi - 5) };
					std::vector<u8> decoded;
					if (!base64_decode(u.substr(comma + 1), decoded) || decoded.empty()) return {};
					return extract_image(source, mime, decoded.data(), decoded.size());
				}
				return u;  // external file, relative to the source asset
			}

			const u32 bv{ get_u32(*image, "bufferView", u32_invalid_id) };
			if (bv == u32_invalid_id) return {};

			const pfx::json::value* view{ array_elem(*json.root, "bufferViews", bv) };
			if (!view) return {};
			const u32 buf_index{ get_u32(*view, "buffer", u32_invalid_id) };
			if (buf_index == u32_invalid_id || !resolve_buffer(buf_index)) return {};

			const u64 offset{ get_u32(*view, "byteOffset", 0) };
			const u64 length{ get_u32(*view, "byteLength", 0) };
			const std::vector<u8>& payload{ buffers[buf_index] };
			if (offset + length > payload.size())
			{
				std::cerr << "GltfImporter: image " << source << " bufferView out of bounds\n";
				return {};
			}

			std::string mime{ "image/png" };
			if (const pfx::json::value* mime_v{ obj_at(*image, "mimeType") };
				mime_v && mime_v->is_string())
			{
				mime = mime_v->as_string();
			}
			return extract_image(source, mime, payload.data() + offset, (size_t)length);
		}

		// --- materials -------------------------------------------------------------------
		void build_materials()
		{
			const u32 count{ array_count(*json.root, "materials") };
			scene_ptr->materials.reserve(count);
			for (u32 i{ 0 }; i < count; ++i)
			{
				const pfx::json::value* gm{ array_elem(*json.root, "materials", i) };
				if (!gm) continue;

				material m{};
				if (const pfx::json::value* name{ obj_at(*gm, "name") };
					name && name->is_string() && !name->as_string().empty())
				{
					m.name = name->as_string();
				}
				else
				{
					m.name = "material_" + std::to_string(i);
				}

				// diffuse ← pbrMetallicRoughness.baseColorTexture
				if (const pfx::json::value* pbr{ obj_at(*gm, "pbrMetallicRoughness") })
				{
					if (const pfx::json::value* bct{ obj_at(*pbr, "baseColorTexture") })
					{
						m.diffuse_texture = resolve_texture(get_u32(*bct, "index", u32_invalid_id));
					}
				}
				// normal ← material.normalTexture (top level, not under pbr)
				if (const pfx::json::value* nt{ obj_at(*gm, "normalTexture") })
				{
					m.normal_texture = resolve_texture(get_u32(*nt, "index", u32_invalid_id));
				}

				scene_ptr->materials.emplace_back(std::move(m));
			}
		}

		// --- scene graph -------------------------------------------------------------------
		m4x4 node_local_matrix(const pfx::json::value& node) const
		{
			if (const pfx::json::value* mat{ obj_at(node, "matrix") })
			{
				if (mat->is_array() && mat->array_size() == 16)
				{
					m4x4 r{};
					for (u32 i{ 0 }; i < 16; ++i)
					{
						const pfx::json::value* e{ mat->array_at(i) };
						r.m[i] = (e && e->is_number()) ? e->as_f32() : 0.f;
					}
					return r;
				}
			}

			// TRS decomposition (glTF defaults: T=0, R=identity, S=1).
			m4x4 r{ m4x4::identity() };

			if (const pfx::json::value* s{ array_member(node, "scale") })
			{
				if (s->is_array() && s->array_size() == 3)
				{
					for (u32 i{ 0 }; i < 3; ++i)
					{
						const pfx::json::value* e{ s->array_at(i) };
						r.m[i * 4 + i] = (e && e->is_number()) ? e->as_f32() : 1.f;
					}
				}
			}
			if (const pfx::json::value* q{ array_member(node, "rotation") })
			{
				if (q->is_array() && q->array_size() == 4)
				{
					const f32 x{ (f32)q->array_at(0)->as_number() };
					const f32 y{ (f32)q->array_at(1)->as_number() };
					const f32 z{ (f32)q->array_at(2)->as_number() };
					const f32 w{ (f32)q->array_at(3)->as_number() };

					const f32 xx{ 2.f * x * x }, yy{ 2.f * y * y }, zz{ 2.f * z * z };
					const f32 xy{ 2.f * x * y }, xz{ 2.f * x * z }, yz{ 2.f * y * z };
					const f32 wx{ 2.f * w * x }, wy{ 2.f * w * y }, wz{ 2.f * w * z };

					// R·S in one pass: scale sits on the diagonal from the step
					// above; the rotation columns get multiplied by it.
					m4x4 rot{ m4x4::identity() };
					rot.m[0] = (1.f - yy - zz) * r.m[0];
					rot.m[1] = (xy + wz) * r.m[0];
					rot.m[2] = (xz - wy) * r.m[0];
					rot.m[4] = (xy - wz) * r.m[5];
					rot.m[5] = (1.f - xx - zz) * r.m[5];
					rot.m[6] = (yz + wx) * r.m[5];
					rot.m[8] = (xz + wy) * r.m[10];
					rot.m[9] = (yz - wx) * r.m[10];
					rot.m[10] = (1.f - xx - yy) * r.m[10];
					r = rot;
				}
			}
			if (const pfx::json::value* t{ array_member(node, "translation") })
			{
				if (t->is_array() && t->array_size() == 3)
				{
					for (u32 i{ 0 }; i < 3; ++i)
					{
						const pfx::json::value* e{ t->array_at(i) };
						r.m[12 + i] = (e && e->is_number()) ? e->as_f32() : 0.f;
					}
				}
			}
			return r;
		}

		bool build_primitive_mesh(const pfx::json::value& prim, const m4x4& world,
			const std::string& mesh_name, u32 prim_index, u32 prim_total, mesh& m)
		{
			const u32 mode{ get_u32(prim, "mode", 4) };
			if (mode != 4)  // TRIANGLES only
			{
				std::cerr << "GltfImporter: primitive " << prim_index << " of '" << mesh_name
						  << "' skipped (mode " << mode << ", only TRIANGLES=4 supported)\n";
				return false;
			}

			const pfx::json::value* attributes{ obj_at(prim, "attributes") };
			if (!attributes) return false;

			// POSITION is the only mandatory attribute.
			const pfx::json::value* pos_acc{ obj_at(*attributes, "POSITION") };
			if (!pos_acc || !pos_acc->is_number()) return false;

			accessor_reader pos_a, nrm_a, uv_a, tan_a;
			if (!build_accessor(pos_acc->as_u32(), 3, false, pos_a)) return false;

			const bool has_normals{ [&]() -> bool
			{
				const pfx::json::value* a{ obj_at(*attributes, "NORMAL") };
				return (a && a->is_number()) ? build_accessor(a->as_u32(), 3, false, nrm_a) : false;
			}() };
			const bool has_uv{ [&]() -> bool
			{
				const pfx::json::value* a{ obj_at(*attributes, "TEXCOORD_0") };
				// u8/u16 UVs arrive normalized; f32 is the common path.
				return (a && a->is_number()) ? build_accessor(a->as_u32(), 2, true, uv_a) : false;
			}() };
			// OBJ parity: tangents are only imported when the user asked the
			// engine NOT to derive them (calculate_tangents=0).
			const bool want_tangents{ scene_data_ptr->settings.calculate_tangents == 0 };
			const bool has_tangents{ want_tangents && [&]() -> bool
			{
				const pfx::json::value* a{ obj_at(*attributes, "TANGENT") };
				return (a && a->is_number()) ? build_accessor(a->as_u32(), 4, false, tan_a) : false;
			}() };

			// Indices: accessor or implicit 0..n-1.
			accessor_reader idx_a;
			bool has_indices{ false };
			if (const pfx::json::value* ia{ obj_at(prim, "indices") };
				ia && ia->is_number())
			{
				has_indices = build_accessor(ia->as_u32(), 1, false, idx_a);
			}

			const u32 vertex_count{ (u32)pos_a.count };
			if (!has_indices && vertex_count < 3) return false;
			const u32 index_count{ has_indices ? (u32)idx_a.count : vertex_count };
			if (index_count < 3 || (index_count % 3) != 0)
			{
				std::cerr << "GltfImporter: primitive " << prim_index << " of '" << mesh_name
						  << "' skipped (index count " << index_count << " not a triangle list)\n";
				return false;
			}

			m.lod_id = 0;
			m.lod_threshold = -1.f;
			m.name = (prim_total > 1)
				? mesh_name + "_" + std::to_string(prim_index)
				: mesh_name;

			// Material hash (OBJ scheme: Murmur3 of the material name, seed 123;
			// 0 when the primitive has no material).
			u32 mtl_hash{ 0 };
			if (const pfx::json::value* mi{ obj_at(prim, "material") };
				mi && mi->is_number())
			{
				const u32 mat_idx{ mi->as_u32() };
				if (mat_idx < scene_ptr->materials.size())
				{
					const std::string& name{ scene_ptr->materials[mat_idx].name };
					utl::MurmurHash3_x86_32((void*)name.c_str(), (u32)name.size(), 123, &mtl_hash);
				}
			}
			m.material_indices.emplace_back(mtl_hash);
			if (std::find(m.material_used.begin(), m.material_used.end(), mtl_hash) == m.material_used.end())
			{
				m.material_used.emplace_back(mtl_hash);
			}

			// Dedupe by attribute-index tuple (OBJ convention (v, vn, vt) plus the
			// tangent slot): glTF splits/shares attribute streams independently,
			// so the tuple — not any single index — defines a unique vertex.
			struct vert_key
			{
				u32 p, n, uv, t;
				bool operator==(const vert_key& o) const { return p == o.p && n == o.n && uv == o.uv && t == o.t; }
			};
			struct vert_key_hash
			{
				size_t operator()(const vert_key& k) const
				{
					return (k.p * 73856093u) ^ (k.n * 19349663u) ^ (k.uv * 83492791u) ^ (k.t * 2654435761u);
				}
			};
			std::unordered_map<vert_key, u32, vert_key_hash> unique;

			m.uv_sets.resize(1);
			for (u32 i{ 0 }; i < index_count; ++i)
			{
				const u32 pi{ has_indices ? accessor_index(idx_a, i) : i };
				if (pi >= vertex_count)
				{
					std::cerr << "GltfImporter: '" << m.name << "' index " << pi
							  << " out of range (" << vertex_count << " verts); primitive skipped\n";
					return false;
				}
				const vert_key key{ pi,
					has_normals ? pi : u32_invalid_id,
					has_uv ? pi : u32_invalid_id,
					has_tangents ? pi : u32_invalid_id };

				auto it{ unique.find(key) };
				if (it == unique.end())
				{
					const u32 slot{ (u32)m.positions.size() };
					math::v3 p{
						accessor_comp(pos_a, pi, 0),
						accessor_comp(pos_a, pi, 1),
						accessor_comp(pos_a, pi, 2)
					};
					transform_point(world, p);
					m.positions.emplace_back(p);
					it = unique.emplace(key, slot).first;
				}
				m.raw_indices.emplace_back(it->second);

				// Legacy scene-IR invariant (ObjImporter parity): positions are
				// unique-vertex-aligned, but normals/tangents/uv_sets/colors are
				// INDEX-SLOT-aligned (one entry per raw_indices slot, sizes ==
				// raw_indices.size()). process_vertices/split_meshes_by_material
				// index them by index-buffer position, not by vertex.
				math::v3 n{ 0.f, 0.f, 1.f };
				if (has_normals)
				{
					n = math::v3{
						accessor_comp(nrm_a, pi, 0),
						accessor_comp(nrm_a, pi, 1),
						accessor_comp(nrm_a, pi, 2)
					};
					transform_normal(world, n);
				}
				m.normals.emplace_back(n);

				// OBJ fills white per slot; glTF COLOR_0 is ignored to keep the
				// element layout identical to FBX/OBJ imports.
				m.colors.emplace_back(math::v3{ 1.f, 1.f, 1.f });

				// glTF's UV origin is top-left, which is already the engine's
				// convention (the OBJ importer flips v into it) — import
				// as-is, no flip.
				m.uv_sets[0].emplace_back(has_uv
					? math::v2{ accessor_comp(uv_a, pi, 0), accessor_comp(uv_a, pi, 1) }
					: math::v2{ 0.f, 0.f });

				if (has_tangents)
				{
					m.tangents.emplace_back(math::v4{
						accessor_comp(tan_a, pi, 0),
						accessor_comp(tan_a, pi, 1),
						accessor_comp(tan_a, pi, 2),
						accessor_comp(tan_a, pi, 3) });
				}
			}

			return !m.raw_indices.empty();
		}

		void walk_node(u32 node_index, const m4x4& parent, u32 depth, utl::vector<mesh>& out)
		{
			constexpr u32 MAX_DEPTH{ 128 };
			if (depth > MAX_DEPTH)
			{
				std::cerr << "GltfImporter: node tree deeper than " << MAX_DEPTH << "; subtree skipped\n";
				return;
			}
			const pfx::json::value* node{ array_elem(*json.root, "nodes", node_index) };
			if (!node) return;

			const m4x4 world{ mat_mul(parent, node_local_matrix(*node)) };

			if (const pfx::json::value* mesh_idx{ obj_at(*node, "mesh") };
				mesh_idx && mesh_idx->is_number())
			{
				const pfx::json::value* gm{ array_elem(*json.root, "meshes", mesh_idx->as_u32()) };
				if (gm)
				{
					std::string mesh_name{ "mesh_" + std::to_string(node_index) };
					if (const pfx::json::value* name{ obj_at(*gm, "name") };
						name && name->is_string() && !name->as_string().empty())
					{
						mesh_name = name->as_string();
					}
					const u32 prim_total{ array_count(*gm, "primitives") };
					for (u32 p{ 0 }; p < prim_total; ++p)
					{
						const pfx::json::value* prim{ array_elem(*gm, "primitives", p) };
						if (!prim) continue;
						mesh m{};
						if (build_primitive_mesh(*prim, world, mesh_name, p, prim_total, m))
						{
							out.emplace_back(std::move(m));
							prog->callback(prog->value(), prog->max_value() + 1);
						}
					}
				}
			}

			const u32 child_count{ array_count(*node, "children") };
			for (u32 c{ 0 }; c < child_count; ++c)
			{
				const pfx::json::value* child{ array_elem(*node, "children", c) };
				if (child && child->is_number())
				{
					walk_node(child->as_u32(), world, depth + 1, out);
				}
			}
		}

		void build_base_meshes(utl::vector<mesh>& out)
		{
			build_materials();

			// Default scene: "scene" index if present, else scene 0, else all nodes.
			utl::vector<u32> roots;
			if (array_count(*json.root, "scenes") > 0)
			{
				const u32 default_scene{ get_u32(*json.root, "scene", 0) };
				const pfx::json::value* sc{ array_elem(*json.root, "scenes", default_scene) };
				if (!sc) sc = array_elem(*json.root, "scenes", 0);
				if (sc)
				{
					const u32 n{ array_count(*sc, "nodes") };
					for (u32 i{ 0 }; i < n; ++i)
					{
						const pfx::json::value* nd{ array_elem(*sc, "nodes", i) };
						if (nd && nd->is_number()) roots.emplace_back(nd->as_u32());
					}
				}
			}
			if (roots.empty())
			{
				const u32 n{ array_count(*json.root, "nodes") };
				for (u32 i{ 0 }; i < n; ++i) roots.emplace_back(i);
			}

			const m4x4 identity{ m4x4::identity() };
			for (u32 root : roots)
			{
				walk_node(root, identity, 0, out);
			}
		}
	};

	gltf_context::gltf_context(const char* file, scene* scene, scene_data* data, progression* const progression)
		: _scene{ scene }, _scene_data{ data }, _progression{ progression }
	{
		assert(file && _scene && _scene_data && _progression);
		_impl = std::make_unique<impl>();
		_impl->scene_ptr = scene;
		_impl->scene_data_ptr = data;
		_impl->prog = progression;
		if (!_impl->load(file))
		{
			_impl.reset();
		}
	}

	gltf_context::~gltf_context() = default;

	void gltf_context::get_scene()
	{
		if (!_impl) return;

		lod_group lod{};
		_impl->build_base_meshes(lod.meshes);

		if (_scene_data->settings.coalesce_meshes && lod.meshes.size())
		{
			mesh combined{};
			if (coalesce_meshes(lod, combined, _progression))
			{
				lod.meshes.clear();
				lod.meshes.emplace_back(std::move(combined));
			}
		}

		if (lod.meshes.size())
		{
			lod.name = lod.meshes[0].name;
			_scene->lod_groups.emplace_back(lod);
			generate_lod_chain(lod.meshes, 1, 20.f, _scene->lod_groups);
		}

		_scene->name = _impl->stem;
	}

	EDITOR_INTERFACE void ImportGltfAPI(const char* file, scene_data* data, progression::progress_callback callback)
	{
		assert(file && data);
		scene scene{};
		progression progression{ callback };
		gltf_context ctx{ file, &scene, data, &progression };
		ctx.get_scene();

		if (scene.lod_groups.empty())
		{
			// Caller (ProcessAIAsset) treats an empty buffer as the failure path.
			std::cerr << "GltfImporter: no geometry imported from '" << file << "'\n";
			return;
		}

		process_scene(scene, data->settings, &progression);
		pack_data(scene, *data);
	}
}
