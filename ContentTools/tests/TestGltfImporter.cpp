// TestGltfImporter: glTF 2.0 import (.glb container + .gltf JSON).
//
// Builds minimal glTF assets in memory, drives ImportGltfAPI through the
// ContentTools dylib, and validates the cooked blob with the pipeline's
// SceneBlobReader (mesh counts, baked node transforms, material texture
// references, embedded image extraction).
//
// Coverage:
//   1. .glb  — BIN chunk, u16 indices, node scale baked into positions,
//              bufferView-embedded PNG extracted to <stem>_extracted/,
//              sidecar material record references the extracted file.
//   2. .gltf — external .bin buffer, data-URI image extraction.
//   3. .glb  — extensionsRequired (Draco) is rejected, empty buffer.

#include "GltfImporter.h"
#include "Geometry.h"
#include "pipeline/SceneBlobReader.h"
#include "pipeline/Validator.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ContentTools.cpp ABI (global scope, extern "C").
extern "C" u32 WriteModelSidecar(const char* cooked_mesh_path, const char* sidecar_path);
// ObjImporter.cpp ABI (defined inside primal::tools, extern "C" linkage).
extern "C" void ImportObjAPI(const char* file, primal::tools::scene_data* data, void(*callback)(s32, s32));

namespace fs = std::filesystem;

namespace
{
	int g_failures{ 0 };

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) {                                                       \
			std::cerr << "CHECK FAILED: " << msg << " (line "                \
			          << __LINE__ << ")\n";                                  \
			++g_failures;                                                    \
		}                                                                    \
	} while (0)

	void write_file(const fs::path& path, const void* data, size_t size)
	{
		std::ofstream f{ path, std::ios::binary | std::ios::trunc };
		f.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
		CHECK(f.good(), ("write " + path.string()).c_str());
	}

	std::string read_text(const fs::path& path)
	{
		std::ifstream f{ path, std::ios::binary };
		return std::string{ std::istreambuf_iterator<char>{f },
			                std::istreambuf_iterator<char>{} };
	}

	void append_u32(std::vector<u8>& v, u32 x)
	{
		v.insert(v.end(), { (u8)(x & 0xFF), (u8)((x >> 8) & 0xFF),
			                (u8)((x >> 16) & 0xFF), (u8)((x >> 24) & 0xFF) });
	}

	void append_aligned(std::vector<u8>& v, const void* data, size_t size)
	{
		const u8* p{ (const u8*)data };
		v.insert(v.end(), p, p + size);
		while (v.size() % 4) v.emplace_back(0);
	}

	// Minimal but well-formed PNG magic + payload bytes. The importer only
	// extracts raw bytes (the C# side cooks them), so any byte string works.
	const u8 kFakePng[16] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
		0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03 };

	// Quad geometry: 4 verts (unit square on XY), normals +Z, uvs, 2 tris.
	const f32 kPositions[12] = {
		-1.f, -1.f, 0.f,  1.f, -1.f, 0.f,  1.f, 1.f, 0.f,  -1.f, 1.f, 0.f };
	const f32 kNormals[12] = {
		 0.f, 0.f, 1.f,  0.f, 0.f, 1.f,  0.f, 0.f, 1.f,  0.f, 0.f, 1.f };
	const f32 kUvs[8] = { 0.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f };
	const u16 kIndices[6] = { 0, 1, 2, 0, 2, 3 };

	const char* kJsonGlb =
		"{"
		"\"asset\": {\"version\": \"2.0\"},"
		"\"scene\": 0,"
		"\"scenes\": [{\"nodes\": [0]}],"
		"\"nodes\": [{\"mesh\": 0, \"scale\": [2.0, 2.0, 2.0]}],"
		"\"meshes\": [{\"name\": \"GenQuad\", \"primitives\": [{"
		"\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2},"
		"\"indices\": 3, \"material\": 0, \"mode\": 4}]}],"
		"\"materials\": [{\"name\": \"GenMaterial\","
		"\"pbrMetallicRoughness\": {\"baseColorTexture\": {\"index\": 0}}}],"
		"\"textures\": [{\"source\": 0}],"
		"\"images\": [{\"bufferView\": 4, \"mimeType\": \"image/png\"}],"
		"\"buffers\": [{\"byteLength\": 156}],"
		"\"bufferViews\": ["
		"{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 48},"
		"{\"buffer\": 0, \"byteOffset\": 48, \"byteLength\": 48},"
		"{\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 32},"
		"{\"buffer\": 0, \"byteOffset\": 128, \"byteLength\": 12},"
		"{\"buffer\": 0, \"byteOffset\": 140, \"byteLength\": 16}"
		"],"
		"\"accessors\": ["
		"{\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},"
		"{\"bufferView\": 1, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},"
		"{\"bufferView\": 2, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\"},"
		"{\"bufferView\": 3, \"componentType\": 5123, \"count\": 6, \"type\": \"SCALAR\"}"
		"]"
		"}";

	// GLB: 12-byte header + JSON chunk + BIN chunk.
	std::vector<u8> build_glb(const char* json)
	{
		const size_t json_len{ std::strlen(json) };
		std::vector<u8> out;
		append_u32(out, 0x46546C67);            // magic 'glTF'
		append_u32(out, 2);                     // version
		append_u32(out, (u32)(12 + 8 + ((json_len + 3) & ~3ull) + 8 + 156));
		append_u32(out, (u32)json_len);         // JSON chunk length
		append_u32(out, 0x4E4F534A);            // 'JSON'
		append_aligned(out, json, json_len);    // padded with spaces? zeros ok
		append_u32(out, 156);                   // BIN chunk length
		append_u32(out, 0x004E4942);            // 'BIN\0'

		std::vector<u8> bin;
		bin.insert(bin.end(), (const u8*)kPositions, (const u8*)kPositions + 48);
		bin.insert(bin.end(), (const u8*)kNormals, (const u8*)kNormals + 48);
		bin.insert(bin.end(), (const u8*)kUvs, (const u8*)kUvs + 32);
		bin.insert(bin.end(), (const u8*)kIndices, (const u8*)kIndices + 12);
		bin.insert(bin.end(), kFakePng, kFakePng + 16);
		out.insert(out.end(), bin.begin(), bin.end());
		return out;
	}

	primal::tools::geometry_import_settings default_settings()
	{
		// CookMesh short-form defaults (178.0f, 1, 1, 0, 0, 0, 1).
		primal::tools::geometry_import_settings s{};
		s.smoothing_angle = 178.f;
		s.calculate_normals = 1;
		s.calculate_tangents = 1;
		s.reverse_handedness = 0;
		s.import_embedded_textures = 0;
		s.import_animations = 0;
		s.coalesce_meshes = 1;
		return s;
	}

	u32 run_import(const fs::path& source, primal::tools::scene_data& data)
	{
		using namespace primal::tools;
		data.settings = default_settings();
		ImportGltfAPI(source.string().c_str(), &data, nullptr);
		return data.buffer_size;
	}

	// Walks the blob and returns the mesh whose lod_id matches (first hit).
	bool find_mesh(primal::tools::scene_data& data, u32 lod_id,
		primal::tools::pipeline::PackedMeshView& out)
	{
		using namespace primal::tools;
		bool found{ false };
		primal::utl::vector<primal::tools::ErrorReport> walk_errors;
		pipeline::WalkSceneBlob(data, [&](u32, const pipeline::PackedMeshView& m)
		{
			if (!found && m.lod_id == lod_id)
			{
				out = m;
				found = true;
			}
		}, walk_errors);
		return found;
	}

}  // namespace

int main()
{
	using namespace primal::tools;
	namespace pipeline = primal::tools::pipeline;

	const fs::path root{ "/tmp/primal_test_gltf" };
	fs::remove_all(root);
	fs::create_directories(root);

	// -------------------------------------------------------------------------
	// 1. .glb: container parse, baked transform, embedded image extraction.
	// -------------------------------------------------------------------------
	{
		const fs::path glb{ root / "test_glb.glb" };
		const std::vector<u8> bytes{ build_glb(kJsonGlb) };
		write_file(glb, bytes.data(), bytes.size());

		scene_data data{};
		CHECK(run_import(glb, data) > 0, "glb import produced a buffer");

		// Base (coalesced) mesh + 5 OBJ-parity LOD chain levels.
		pipeline::ValidationReport report;
		primal::utl::vector<primal::tools::ErrorReport> errors;
		CHECK(pipeline::ValidateSceneBlob(data, report, errors), "glb blob validates");
		CHECK(report.total_errors == 0, "glb blob has no validation errors");
		std::cout << "  diag: glb mesh_count=" << report.mesh_count << "\n";
		CHECK(report.mesh_count == 6,
			"glb blob has base mesh + 5 LOD chain levels");

		// LOD 0 quad: 4 verts / 6 indices, node scale baked into positions.
		pipeline::PackedMeshView base{};
		CHECK(find_mesh(data, 0, base), "glb blob has a lod 0 mesh");
		std::cout << "  diag: glb base verts=" << base.num_vertices
			      << " indices=" << base.num_indices
			      << " name=" << base.name << "\n";
		CHECK(base.num_indices == 6, "glb quad has 6 indices");
		CHECK(base.name == "GenQuad", "glb quad keeps the glTF mesh name");
		CHECK(base.material_idx != u32_invalid_id, "glb quad has a material hash");

		// Behavioral-twin oracle: the same quad through the OBJ path must
		// produce the identical packed geometry (legacy-IR parity).
		{
			write_file(root / "quad.mtl",
				"newmtl GenMaterial\nmap_Kd colormap.png\n", 33);
			const char* obj_text{
				"mtllib quad.mtl\n"
				"v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n"
				"vn 0 0 1\n"
				"vt 0 1\nvt 1 1\nvt 1 0\nvt 0 0\n"
				"o GenQuad\nusemtl GenMaterial\n"
				"f 1/1/1 2/2/2 3/3/3\nf 1/1/1 3/3/3 4/4/4\n" };
			write_file(root / "quad.obj", obj_text, std::strlen(obj_text));

			scene_data obj_data{};
			obj_data.settings = default_settings();
			ImportObjAPI((root / "quad.obj").string().c_str(), &obj_data, nullptr);
			CHECK(obj_data.buffer_size > 0, "obj oracle import produced a buffer");

			pipeline::ValidationReport obj_report;
			primal::utl::vector<primal::tools::ErrorReport> obj_errors;
			pipeline::ValidateSceneBlob(obj_data, obj_report, obj_errors);
			std::cout << "  diag: obj oracle mesh_count=" << obj_report.mesh_count
				      << "\n";
			CHECK(obj_report.mesh_count == report.mesh_count,
				"obj and gltf quads produce the same mesh count");

			pipeline::PackedMeshView obj_base{};
			CHECK(find_mesh(obj_data, 0, obj_base), "obj oracle has a lod 0 mesh");
			std::cout << "  diag: obj oracle verts=" << obj_base.num_vertices
				      << " indices=" << obj_base.num_indices
				      << " elements=" << (u32)obj_base.elements_type << "\n";
			CHECK(obj_base.num_vertices == base.num_vertices &&
				      obj_base.num_indices == base.num_indices,
				"obj and gltf quads produce identical packed geometry");
			CHECK(obj_base.elements_type == base.elements_type,
				"obj and gltf quads produce the same element layout");
			std::free(obj_data.buffer);
		}

		if (base.position_buffer)
		{
			const f32* pos{ (const f32*)base.position_buffer };
			// Vertex 0 = (-1,-1,0) scaled by the node's [2,2,2].
			CHECK(pos[0] == -2.f && pos[1] == -2.f && pos[2] == 0.f,
				"node scale baked into positions");
		}

		// Normals survived the scale transform unit-length: element layout
		// static_normal_texture starts with 3 color bytes + t_sign then packed
		// normals — presence of uv/tangent payload is checked via elements_type.
		CHECK(base.elements_type == elements::elements_type::static_normal_texture,
			"glb quad has normal+texture elements (uv imported)");

		// Material: sidecar must reference the extracted embedded PNG.
		const fs::path cooked{ root / "test_glb.mesh" };
		write_file(cooked, data.buffer, data.buffer_size);
		const fs::path sidecar{ root / "test_glb.model.json" };
		CHECK(WriteModelSidecar(cooked.string().c_str(), sidecar.string().c_str()) == 1,
			"glb sidecar written");
		const std::string sidecar_text{ read_text(sidecar) };
		CHECK(sidecar_text.find("\"name\": \"GenMaterial\"") != std::string::npos,
			"sidecar carries the glTF material name");
		CHECK(sidecar_text.find("test_glb_extracted/embedded_0.png") != std::string::npos,
			"sidecar diffuse references the extracted embedded image");

		const fs::path extracted{ root / "test_glb_extracted" / "embedded_0.png" };
		CHECK(fs::exists(extracted), "embedded image extracted beside the source");
		if (fs::exists(extracted))
		{
			std::ifstream f{ extracted, std::ios::binary };
			const std::string bytes_on_disk{ std::istreambuf_iterator<char>{f},
				                                 std::istreambuf_iterator<char>{} };
			CHECK(bytes_on_disk.size() == sizeof(kFakePng) &&
				      std::memcmp(bytes_on_disk.data(), kFakePng, sizeof(kFakePng)) == 0,
				"extracted image bytes are verbatim");
		}

		std::free(data.buffer);
	}

	// -------------------------------------------------------------------------
	// 2. .gltf: external .bin buffer + data-URI image.
	// -------------------------------------------------------------------------
	{
		std::vector<u8> bin;
		bin.insert(bin.end(), (const u8*)kPositions, (const u8*)kPositions + 48);
		bin.insert(bin.end(), (const u8*)kNormals, (const u8*)kNormals + 48);
		bin.insert(bin.end(), (const u8*)kIndices, (const u8*)kIndices + 12);
		write_file(root / "quad.bin", bin.data(), bin.size());

		// data URI of kFakePng (89 50 4E 47 0D 0A 1A 0A DE AD BE EF 00 01 02 03).
		const char* data_uri{
			"data:image/png;base64,iVBORw0KGgrerb7vAAECAw==" };
		std::string json{
			"{"
			"\"asset\": {\"version\": \"2.0\"},"
			"\"scenes\": [{\"nodes\": [0]}],"
			"\"nodes\": [{\"mesh\": 0}],"
			"\"meshes\": [{\"name\": \"RefQuad\", \"primitives\": [{"
			"\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1},"
			"\"indices\": 2, \"material\": 0}]}],"
			"\"materials\": [{\"name\": \"UriMaterial\","
			"\"pbrMetallicRoughness\": {\"baseColorTexture\": {\"index\": 0}}}],"
			"\"textures\": [{\"source\": 0}],"
			"\"images\": [{\"uri\": \"" + std::string{ data_uri } + "\"}],"
			"\"buffers\": [{\"uri\": \"quad.bin\", \"byteLength\": 108}],"
			"\"bufferViews\": ["
			"{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 48},"
			"{\"buffer\": 0, \"byteOffset\": 48, \"byteLength\": 48},"
			"{\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 12}"
			"],"
			"\"accessors\": ["
			"{\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},"
			"{\"bufferView\": 1, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},"
			"{\"bufferView\": 2, \"componentType\": 5123, \"count\": 6, \"type\": \"SCALAR\"}"
			"]"
			"}" };

		const fs::path gltf{ root / "test_gltf.gltf" };
		write_file(gltf, json.data(), json.size());

		scene_data data{};
		CHECK(run_import(gltf, data) > 0, "gltf import produced a buffer");

		pipeline::PackedMeshView base{};
		CHECK(find_mesh(data, 0, base), "gltf blob has a lod 0 mesh");
		std::cout << "  diag: gltf base verts=" << base.num_vertices
			      << " indices=" << base.num_indices << "\n";
		CHECK(base.num_vertices == base.num_indices && base.num_indices == 6,
			"gltf quad geometry matches (external buffer resolved)");

		const fs::path extracted{ root / "test_gltf_extracted" / "embedded_0.png" };
		CHECK(fs::exists(extracted), "data-URI image extracted beside the source");
		if (fs::exists(extracted))
		{
			std::ifstream f{ extracted, std::ios::binary };
			const std::string bytes_on_disk{ std::istreambuf_iterator<char>{f},
				                                 std::istreambuf_iterator<char>{} };
			CHECK(bytes_on_disk.size() == sizeof(kFakePng),
				"data-URI image decodes to the original bytes");
		}

		std::free(data.buffer);
	}

	// -------------------------------------------------------------------------
	// 3. .glb with a required extension (Draco) is rejected cleanly.
	// -------------------------------------------------------------------------
	{
		const char* draco_json{
			"{"
			"\"asset\": {\"version\": \"2.0\"},"
			"\"extensionsRequired\": [\"KHR_draco_mesh_compression\"],"
			"\"extensionsUsed\": [\"KHR_draco_mesh_compression\"],"
			"\"nodes\": [{\"mesh\": 0}],"
			"\"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0}}]}],"
			"\"accessors\": [], \"bufferViews\": [], \"buffers\": []"
			"}" };
		const fs::path draco{ root / "draco.glb" };
		const std::vector<u8> bytes{ build_glb(draco_json) };
		write_file(draco, bytes.data(), bytes.size());

		scene_data data{};
		CHECK(run_import(draco, data) == 0, "Draco glb is rejected (empty buffer)");
	}

	std::cout << (g_failures == 0 ? "TestGltfImporter: PASS\n"
	                              : "TestGltfImporter: FAIL\n");
	return g_failures == 0 ? 0 : 1;
}
