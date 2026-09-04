// =============================================================================
// ParticleEmitterAPI — 粒子发射器编辑通道（plan §6.3 B4 P1，2026-09-03）
//
// C ABI 消费者纪律同 ParticleMaterialAPI：EDITOR_INTERFACE 导出面、函数声明
// 镜像在 C# 侧。语义要点（勘察报告 §2-§6）：
//   - particles::initialize() 引擎正式路径无人调用——本 API 的
//     ParticleEnsureSystems 幂等补齐（含 preset_system + 内置 8 预设）；
//   - emitter_config 含 6 个堆曲线指针（curves，96B）——跨 ABI blob 一律
//     置空并把 use_* 关掉（曲线编辑是 P3；P1 只走标量场）；
//   - 运行时改 config 走 get_emitter→set_config，无需重建发射器；
//   - 无 job 队列间接，任意线程可调（内部各自有锁）。
// =============================================================================

#include "Common.h"
#include "CommonHeaders.h"
#include "Components/Particle.h"
#include "Components/Entity.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "Particles/ParticlePreset.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleSystem.h"
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_MSC_VER)
#pragma comment(lib, "Engine.lib")
#endif

using namespace primal;

namespace {
game_entity::entity entity_from_id(id::id_type id)
{
	return game_entity::entity{ game_entity::entity_id{id} };
}

// 跨 ABI 安全化：曲线指针置空 + use_* 关闭（P1 不支持曲线；P3 落地时改为
// 序列化曲线数据在引擎侧重建对象）。
void SanitizeConfig(particles::emitter_config& config)
{
	config.curves.clear();
}

// =============================================================================
// B4 P3 曲线编辑通道（plan §6.4，2026-09-04）
//
// emitter_config.curves 的 6 个字段是引擎堆对象指针，不能跨 ABI 拷贝——本节
// 以"槽位 + 自含 wire 格式"传递曲线数据，所有权登记在本 DLL 内：
//   slot: 0=scale(f32) 1=alpha(f32) 2=color_gradient 3=velocity(f32)
//         4=force(v3) 5=rotation(f32)
//   wire: u32 interpolation(0=constant 1=linear 2=smooth 3=bezier)
//         + u32 key_count + keys
//     float 槽位: { f32 time, value, in_tangent, out_tangent }  ×16B
//     color 槽位: { f32 time, r, g, b, a }                      ×20B（无插值字段）
//     vector 槽位: { f32 time, x, y, z }                        ×16B（无切线/插值字段）
//
// 所有权纪律：SetCurveData 创建的曲线对象登记进 g_owned_curves，替换/组件
// 移除/整体 SetConfig（SanitizeConfig 把发射器 config 的曲线指针清掉之后）
// 时统一释放——engine 的 emitter 析构不拥有曲线（~particle_emitter = default），
// 预设路径也只值拷标量，因此登记表是这些对象唯一的拥有者，无共享无二次释放。
// =============================================================================

namespace {

constexpr u32 kCurveSlotCount = 6;
constexpr u32 kCurveWireHeaderBytes = 8; // interpolation + key_count

std::mutex g_owned_curve_mutex;
// emitter_id → [scale, alpha, color, velocity, force, rotation] 堆指针。
std::unordered_map<u32, std::array<void*, kCurveSlotCount>>& owned_curves()
{
	static std::unordered_map<u32, std::array<void*, kCurveSlotCount>> map;
	return map;
}

void* owned_curve(u32 emitter_id, u32 slot)
{
	std::lock_guard lock{ g_owned_curve_mutex };
	auto it = owned_curves().find(emitter_id);
	return (it == owned_curves().end()) ? nullptr : it->second[slot];
}

void set_owned_curve(u32 emitter_id, u32 slot, void* ptr)
{
	std::lock_guard lock{ g_owned_curve_mutex };
	owned_curves()[emitter_id][slot] = ptr;
}

// 释放某个发射器名下的全部自有曲线对象（析构前调用方须保证发射器 config
// 已不再引用它们——EmitterRemoveFromEntity 连组件一起销毁、EmitterSetConfig
// 先写入 SanitizeConfig 后的干净 config）。
void free_owned_curves(u32 emitter_id)
{
	std::lock_guard lock{ g_owned_curve_mutex };
	auto it = owned_curves().find(emitter_id);
	if (it == owned_curves().end()) return;
	for (u32 slot = 0; slot < kCurveSlotCount; ++slot)
	{
		void* p = it->second[slot];
		if (!p) continue;
		if (slot == 2) delete static_cast<particles::color_gradient*>(p);
		else if (slot == 4) delete static_cast<particles::vector_curve*>(p);
		else delete static_cast<particles::float_curve*>(p);
		it->second[slot] = nullptr;
	}
	owned_curves().erase(it);
}

// 重新装配某槽位：删旧自拥有曲线 → 用 wire 数据建新对象 → 写回发射器 config。
u32 replace_curve_from_wire(u32 emitter_id, u32 slot, const void* data, u32 size)
{
	particles::particle_emitter* emitter = particles::get_emitter(emitter_id);
	if (!emitter) return 0;
	if (size < kCurveWireHeaderBytes) return 0;

	u32 interpolation = 0, key_count = 0;
	std::memcpy(&interpolation, data, sizeof(u32));
	std::memcpy(&key_count, static_cast<const u8*>(data) + 4, sizeof(u32));
	const u8* keys = static_cast<const u8*>(data) + kCurveWireHeaderBytes;

	if (slot == 2)
	{
		constexpr u32 kKeyBytes = 20;
		if (key_count > 4096 || size < kCurveWireHeaderBytes + key_count * kKeyBytes) return 0;
		auto* curve = new particles::color_gradient();
		for (u32 i = 0; i < key_count; ++i)
		{
			f32 v[5];
			std::memcpy(v, keys + i * kKeyBytes, sizeof(v));
			curve->add_color(v[0], v[1], v[2], v[3], v[4]);
		}
		void* old = owned_curve(emitter_id, slot);
		delete static_cast<particles::color_gradient*>(old);
		set_owned_curve(emitter_id, slot, curve);
		particles::emitter_config cfg = emitter->get_config();
		cfg.curves.color_gradient_curve = curve;
		cfg.curves.use_color_gradient = true;
		emitter->set_config(cfg);
	}
	else if (slot == 4)
	{
		constexpr u32 kKeyBytes = 16;
		if (key_count > 4096 || size < kCurveWireHeaderBytes + key_count * kKeyBytes) return 0;
		auto* curve = new particles::vector_curve();
		curve->set_interpolation(static_cast<particles::curve_interpolation>(interpolation));
		for (u32 i = 0; i < key_count; ++i)
		{
			f32 v[4];
			std::memcpy(v, keys + i * kKeyBytes, sizeof(v));
			curve->add_key(v[0], v[1], v[2], v[3]);
		}
		void* old = owned_curve(emitter_id, slot);
		delete static_cast<particles::vector_curve*>(old);
		set_owned_curve(emitter_id, slot, curve);
		particles::emitter_config cfg = emitter->get_config();
		cfg.curves.force_curve = curve;
		cfg.curves.use_force_curve = true;
		emitter->set_config(cfg);
	}
	else
	{
		constexpr u32 kKeyBytes = 16;
		if (slot >= kCurveSlotCount) return 0;
		if (key_count > 4096 || size < kCurveWireHeaderBytes + key_count * kKeyBytes) return 0;
		auto* curve = new particles::float_curve();
		curve->set_interpolation(static_cast<particles::curve_interpolation>(interpolation));
		for (u32 i = 0; i < key_count; ++i)
		{
			f32 v[4];
			std::memcpy(v, keys + i * kKeyBytes, sizeof(v));
			curve->add_keyframe(v[0], v[1], v[2], v[3]);
		}
		void* old = owned_curve(emitter_id, slot);
		delete static_cast<particles::float_curve*>(old);
		set_owned_curve(emitter_id, slot, curve);
		particles::emitter_config cfg = emitter->get_config();
		switch (slot)
		{
		case 0: cfg.curves.scale_curve = curve;    cfg.curves.use_scale_curve = true;    break;
		case 1: cfg.curves.alpha_curve = curve;    cfg.curves.use_alpha_curve = true;    break;
		case 3: cfg.curves.velocity_curve = curve; cfg.curves.use_velocity_curve = true; break;
		case 5: cfg.curves.rotation_curve = curve; cfg.curves.use_rotation_curve = true; break;
		}
		emitter->set_config(cfg);
	}
	return 1;
}

// 序列化某槽位当前曲线到 wire 格式。无曲线（未启用或空指针）→ out_size=0。
u32 read_curve_to_wire(u32 emitter_id, u32 slot, void* out, u32 cap, u32* out_size)
{
	*out_size = 0;
	particles::particle_emitter* emitter = particles::get_emitter(emitter_id);
	if (!emitter || slot >= kCurveSlotCount) return 0;
	const particles::emitter_curves& curves = emitter->get_config().curves;

	if (slot == 2)
	{
		if (!curves.use_color_gradient || !curves.color_gradient_curve) return 1;
		const auto* g = curves.color_gradient_curve;
		const u32 n = static_cast<u32>(g->key_count());
		const u32 bytes = kCurveWireHeaderBytes + n * 20;
		if (!out || cap < bytes) { *out_size = bytes; return 0; }
		u8* w = static_cast<u8*>(out);
		const u32 interp = 1; // color_gradient 无插值枚举，写 linear 占位
		std::memcpy(w, &interp, 4);
		std::memcpy(w + 4, &n, 4);
		for (u32 i = 0; i < n; ++i)
		{
			const auto& k = g->get_key(i);
			f32 v[5] = { k.time, k.color.x, k.color.y, k.color.z, k.color.w };
			std::memcpy(w + kCurveWireHeaderBytes + i * 20, v, sizeof(v));
		}
		*out_size = bytes;
		return 1;
	}

	if (slot == 4)
	{
		if (!curves.use_force_curve || !curves.force_curve) return 1;
		const auto* c = curves.force_curve;
		const u32 n = static_cast<u32>(c->key_count());
		const u32 bytes = kCurveWireHeaderBytes + n * 16;
		if (!out || cap < bytes) { *out_size = bytes; return 0; }
		u8* w = static_cast<u8*>(out);
		const u32 interp = static_cast<u32>(c->get_interpolation());
		std::memcpy(w, &interp, 4);
		std::memcpy(w + 4, &n, 4);
		for (u32 i = 0; i < n; ++i)
		{
			const auto& k = c->get_key(i);
			f32 v[4] = { k.time, k.value.x, k.value.y, k.value.z };
			std::memcpy(w + kCurveWireHeaderBytes + i * 16, v, sizeof(v));
		}
		*out_size = bytes;
		return 1;
	}

	const particles::float_curve* c = nullptr;
	if (slot == 0 && curves.use_scale_curve) c = curves.scale_curve;
	else if (slot == 1 && curves.use_alpha_curve) c = curves.alpha_curve;
	else if (slot == 3 && curves.use_velocity_curve) c = curves.velocity_curve;
	else if (slot == 5 && curves.use_rotation_curve) c = curves.rotation_curve;
	if (!c) return 1;
	const u32 n = static_cast<u32>(c->keyframe_count());
	const u32 bytes = kCurveWireHeaderBytes + n * 16;
	if (!out || cap < bytes) { *out_size = bytes; return 0; }
	u8* w = static_cast<u8*>(out);
	const u32 interp = static_cast<u32>(c->get_interpolation());
	std::memcpy(w, &interp, 4);
	std::memcpy(w + 4, &n, 4);
	for (u32 i = 0; i < n; ++i)
	{
		const auto& k = c->get_keyframe(i);
		f32 v[4] = { k.time, k.value, k.in_tangent, k.out_tangent };
		std::memcpy(w + kCurveWireHeaderBytes + i * 16, v, sizeof(v));
	}
	*out_size = bytes;
	return 1;
}

} // namespace (B4 P3 curve helpers)
} // anonymous namespace

extern "C" {

// 幂等补齐粒子系统 + 预设系统（含 8 个内置预设注册）。返回 1 = 就绪。
EDITOR_INTERFACE u32 ParticleEnsureSystems() {
	static bool ready = false;
	if (ready) return 1;
	if (!particles::initialize()) return 0;
	if (!particles::preset_system::is_initialized()) {
		if (!particles::preset_system::initialize()) return 0;
		particles::default_presets::register_defaults(*particles::preset_system::get());
	}
	ready = true;
	return 1;
}

// emitter_config 的 sizeof（C# 据此校验 blob 长度；布局含 curves 指针，见
// SanitizeConfig）。
EDITOR_INTERFACE u32 ParticleEmitterConfigSize() {
	return static_cast<u32>(sizeof(particles::emitter_config));
}

// 为实体挂 Particle 组件并创建发射器。config 大小必须与 sizeof 完全一致。
// 返回发射器 id（0 = 失败；emitter id 从 1 起，0xFFFFFFFF = 引擎哨兵）。
EDITOR_INTERFACE u32 EmitterCreateForEntity(id::id_type entity_id,
                                            const void* config, u32 size) {
	if (!config || size != sizeof(particles::emitter_config)) return 0;
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || e.Has<component::Particle>()) return 0;
	if (!ParticleEnsureSystems()) return 0;

	particles::emitter_config cfg{};
	std::memcpy(&cfg, config, sizeof(cfg));
	SanitizeConfig(cfg);

	particle::init_info info{};
	info.config = cfg;
	info.material_id = u32_invalid_id; // component::create 不消费；渲染走 ParticleMaterialAPI
	info.auto_activate = true;
	particle::component c = particle::create(info, e);
	return c.is_valid() ? particle::get_emitter_id(c) : 0;
}

// 移除实体的 Particle 组件（连带销毁发射器）。B4 P3: 先释放该发射器名下
// 登记的自有曲线对象（发射器随组件销毁，config 不再引用它们）。
EDITOR_INTERFACE u32 EmitterRemoveFromEntity(id::id_type entity_id) {
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || !e.Has<component::Particle>()) return 0;
	particle::component c = e.particle();
	if (!c.is_valid()) return 0;
	free_owned_curves(particle::get_emitter_id(c));
	particle::remove(c);
	return 1;
}

// 运行时整体替换 config（get_emitter→set_config，无需重建发射器）。
// B4 P3: SanitizeConfig 清掉发射器侧曲线指针后，登记表里的旧自有曲线
// 对象一并释放（发射器不再引用它们）。
EDITOR_INTERFACE u32 EmitterSetConfig(u32 emitter_id, const void* config, u32 size) {
	if (!config || size != sizeof(particles::emitter_config)) return 0;
	particles::particle_emitter* emitter = particles::get_emitter(emitter_id);
	if (!emitter) return 0;
	particles::emitter_config cfg{};
	std::memcpy(&cfg, config, sizeof(cfg));
	SanitizeConfig(cfg);
	emitter->set_config(cfg);
	free_owned_curves(emitter_id);
	return 1;
}

// 读回当前 config（曲线指针字段清零后拷出）。
EDITOR_INTERFACE u32 EmitterGetConfig(u32 emitter_id, void* out, u32 cap, u32* out_size) {
	if (!out_size) return 0;
	*out_size = static_cast<u32>(sizeof(particles::emitter_config));
	if (!out || cap < sizeof(particles::emitter_config)) return 0;
	particles::particle_emitter* emitter = particles::get_emitter(emitter_id);
	if (!emitter) return 0;
	particles::emitter_config cfg = emitter->get_config();
	SanitizeConfig(cfg);
	std::memcpy(out, &cfg, sizeof(cfg));
	return 1;
}

EDITOR_INTERFACE u32 EmitterSetActive(id::id_type entity_id, u32 active) {
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || !e.Has<component::Particle>()) return 0;
	particle::component c = e.particle();
	if (!c.is_valid()) return 0;
	c.set_active(active != 0);
	return 1;
}

EDITOR_INTERFACE u32 EmitterSetSpawnRate(id::id_type entity_id, f32 rate) {
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || !e.Has<component::Particle>()) return 0;
	particle::component c = e.particle();
	if (!c.is_valid()) return 0;
	c.set_spawn_rate(rate);
	return 1;
}

EDITOR_INTERFACE u32 EmitterBurst(id::id_type entity_id, u32 count) {
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || !e.Has<component::Particle>()) return 0;
	particle::component c = e.particle();
	if (!c.is_valid()) return 0;
	c.burst(count);
	return 1;
}

// 预设名列表，'\n' 分隔写入 out（需容量含末尾 0）。返回 1 = 已写出。
EDITOR_INTERFACE u32 ParticlePresetList(char* out, u32 cap, u32* out_len) {
	if (!out_len) return 0;
	*out_len = 0;
	if (!ParticleEnsureSystems()) return 0;
	std::string joined;
	for (const auto& name : particles::preset_system::get()->get_all_preset_names()) {
		if (!joined.empty()) joined += '\n';
		joined += name;
	}
	if (!out || cap <= joined.size()) {
		*out_len = static_cast<u32>(joined.size()) + 1;
		return 0;
	}
	std::memcpy(out, joined.c_str(), joined.size() + 1);
	*out_len = static_cast<u32>(joined.size()) + 1;
	return 1;
}

// 从 .particle.json 装载预设进管理器（追加语义）。成功后经 ParticlePresetList 可见。
EDITOR_INTERFACE u32 ParticlePresetLoadFromFile(const char* path) {
	if (!path || !ParticleEnsureSystems()) return 0;
	return particles::preset_system::get()->load_preset(path) ? 1u : 0u;
}

// 把管理器中的具名预设存为 .particle.json（plan §6.3 B4 P4）。
EDITOR_INTERFACE u32 ParticlePresetSaveToFile(const char* name, const char* path) {
	if (!name || !path || !ParticleEnsureSystems()) return 0;
	const particles::particle_preset* preset = particles::preset_system::get()->get_preset(name);
	if (!preset) return 0;
	return particles::preset_system::get()->save_preset(path, *preset) ? 1u : 0u;
}

// 把具名预设的标量场套进 config blob（曲线字段照 SanitizeConfig 置空——
// 预设曲线是引擎堆对象，跨 ABI 只传值拷贝标量）。
EDITOR_INTERFACE u32 ParticlePresetApplyToConfig(const char* name, void* config, u32 size) {
	if (!name || !config || size != sizeof(particles::emitter_config)) return 0;
	if (!ParticleEnsureSystems()) return 0;
	const particles::particle_preset* preset = particles::preset_system::get()->get_preset(name);
	if (!preset) return 0;
	particles::emitter_config cfg{};
	preset->apply_to(cfg);
	SanitizeConfig(cfg);
	std::memcpy(config, &cfg, sizeof(cfg));
	return 1;
}

// 读回某槽位曲线（wire 格式）。out_size=0 表示该槽位当前无曲线。
// 参数为实体 id（与 EmitterSetSpawnRate 等同款，C# 侧只持有实体 id）。
EDITOR_INTERFACE u32 EmitterGetCurveData(id::id_type entity_id, u32 slot, void* out, u32 cap, u32* out_size) {
	if (!out_size) return 0;
	*out_size = 0;
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || !e.Has<component::Particle>()) return 0;
	particle::component c = e.particle();
	if (!c.is_valid()) return 0;
	if (!ParticleEnsureSystems()) return 0;
	return read_curve_to_wire(particle::get_emitter_id(c), slot, out, cap, out_size);
}

// 替换某槽位曲线（自含 wire 数据；发射器必须在场）。参数为实体 id。
EDITOR_INTERFACE u32 EmitterSetCurveData(id::id_type entity_id, u32 slot, const void* data, u32 size) {
	if (!data || slot >= kCurveSlotCount) return 0;
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || !e.Has<component::Particle>()) return 0;
	particle::component c = e.particle();
	if (!c.is_valid()) return 0;
	if (!ParticleEnsureSystems()) return 0;
	return replace_curve_from_wire(particle::get_emitter_id(c), slot, data, size);
}

// 关闭某槽位（曲线对象释放、use_* 关闭），发射器回到标量场驱动。
EDITOR_INTERFACE u32 EmitterDisableCurve(id::id_type entity_id, u32 slot) {
	if (slot >= kCurveSlotCount) return 0;
	game_entity::entity e = entity_from_id(entity_id);
	if (!e.is_valid() || !e.Has<component::Particle>()) return 0;
	particle::component c = e.particle();
	if (!c.is_valid()) return 0;
	particles::particle_emitter* emitter = particles::get_emitter(particle::get_emitter_id(c));
	if (!emitter) return 0;
	const u32 emitter_id = particle::get_emitter_id(c);
	{
		std::lock_guard lock{ g_owned_curve_mutex };
		auto it = owned_curves().find(emitter_id);
		if (it != owned_curves().end())
		{
			void* p = it->second[slot];
			if (p)
			{
				if (slot == 2) delete static_cast<particles::color_gradient*>(p);
				else if (slot == 4) delete static_cast<particles::vector_curve*>(p);
				else delete static_cast<particles::float_curve*>(p);
				it->second[slot] = nullptr;
			}
		}
	}
	particles::emitter_config cfg = emitter->get_config();
	switch (slot)
	{
	case 0: cfg.curves.scale_curve = nullptr;    cfg.curves.use_scale_curve = false;    break;
	case 1: cfg.curves.alpha_curve = nullptr;    cfg.curves.use_alpha_curve = false;    break;
	case 2: cfg.curves.color_gradient_curve = nullptr; cfg.curves.use_color_gradient = false; break;
	case 3: cfg.curves.velocity_curve = nullptr; cfg.curves.use_velocity_curve = false; break;
	case 4: cfg.curves.force_curve = nullptr;    cfg.curves.use_force_curve = false;    break;
	case 5: cfg.curves.rotation_curve = nullptr; cfg.curves.use_rotation_curve = false; break;
	}
	emitter->set_config(cfg);
	return 1;
}

} // extern "C"
