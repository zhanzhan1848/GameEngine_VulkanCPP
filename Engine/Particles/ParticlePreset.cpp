#include "ParticlePreset.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>

namespace primal::particles {

// -----------------------------------------------------------------------------
// particle_preset Implementation
// -----------------------------------------------------------------------------

particle_preset::particle_preset(const std::string& preset_name, const emitter_config& cfg)
    : name(preset_name)
    , config(cfg)
{
    extract_curves_from(cfg);
}

void particle_preset::apply_to(emitter_config& cfg) const {
    cfg = config;
    
    // Note: Curve objects need to be created externally since we can't
    // store objects in the preset (only serialized data)
    // The caller should create curve objects from the serialized data
}

void particle_preset::extract_curves_from(const emitter_config& cfg) {
    use_scale_curve = cfg.curves.use_scale_curve;
    use_alpha_curve = cfg.curves.use_alpha_curve;
    use_color_gradient = cfg.curves.use_color_gradient;
    use_velocity_curve = cfg.curves.use_velocity_curve;
    use_force_curve = cfg.curves.use_force_curve;
    use_rotation_curve = cfg.curves.use_rotation_curve;
    
    // Extract curve data if curves exist
    // Note: This would require iterating through curve keyframes
    // For now, we store the config and flags
}

// -----------------------------------------------------------------------------
// JSON Serialization (Simple implementation without external library)
// -----------------------------------------------------------------------------

namespace preset_json {

// Simple JSON escaping
static std::string escape_json(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

// Simple JSON unescaping
static std::string unescape_json(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            switch (str[i + 1]) {
                case '"': result += '"'; ++i; break;
                case '\\': result += '\\'; ++i; break;
                case 'n': result += '\n'; ++i; break;
                case 'r': result += '\r'; ++i; break;
                case 't': result += '\t'; ++i; break;
                default: result += str[i]; break;
            }
        } else {
            result += str[i];
        }
    }
    return result;
}

// Simple JSON value parser
class simple_json_parser {
public:
    simple_json_parser(const std::string& str) : str_(str), pos_(0) {}
    
    void skip_whitespace() {
        while (pos_ < str_.size() && std::isspace(str_[pos_])) {
            ++pos_;
        }
    }
    
    bool expect(char c) {
        skip_whitespace();
        if (pos_ < str_.size() && str_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    
    std::string parse_string() {
        skip_whitespace();
        if (!expect('"')) return "";
        
        std::string result;
        while (pos_ < str_.size() && str_[pos_] != '"') {
            if (str_[pos_] == '\\' && pos_ + 1 < str_.size()) {
                ++pos_;
                switch (str_[pos_]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    default: result += str_[pos_]; break;
                }
            } else {
                result += str_[pos_];
            }
            ++pos_;
        }
        expect('"');
        return result;
    }
    
    f32 parse_number() {
        skip_whitespace();
        size_t start = pos_;
        if (pos_ < str_.size() && (str_[pos_] == '-' || str_[pos_] == '+')) {
            ++pos_;
        }
        while (pos_ < str_.size() && (std::isdigit(str_[pos_]) || str_[pos_] == '.')) {
            ++pos_;
        }
        return std::stof(str_.substr(start, pos_ - start));
    }
    
    std::string parse_value() {
        skip_whitespace();
        if (pos_ >= str_.size()) return "";
        
        if (str_[pos_] == '"') {
            return "\"" + parse_string() + "\"";
        } else if (str_[pos_] == '{') {
            return parse_object();
        } else if (str_[pos_] == '[') {
            return parse_array();
        } else {
            // Number or boolean
            size_t start = pos_;
            while (pos_ < str_.size() && !std::isspace(str_[pos_]) && 
                   str_[pos_] != ',' && str_[pos_] != '}' && str_[pos_] != ']') {
                ++pos_;
            }
            return str_.substr(start, pos_ - start);
        }
    }
    
    std::string parse_object() {
        skip_whitespace();
        if (!expect('{')) return "";
        
        std::string result = "{";
        bool first = true;
        
        while (pos_ < str_.size() && str_[pos_] != '}') {
            if (!first) {
                skip_whitespace();
                if (str_[pos_] == ',') {
                    result += ",";
                    ++pos_;
                }
            }
            first = false;
            
            skip_whitespace();
            std::string key = parse_string();
            skip_whitespace();
            expect(':');
            std::string value = parse_value();
            
            result += "\"" + key + "\":" + value;
        }
        expect('}');
        return result + "}";
    }
    
    std::string parse_array() {
        skip_whitespace();
        if (!expect('[')) return "";
        
        std::string result = "[";
        bool first = true;
        
        while (pos_ < str_.size() && str_[pos_] != ']') {
            if (!first) {
                skip_whitespace();
                if (str_[pos_] == ',') {
                    result += ",";
                    ++pos_;
                }
            }
            first = false;
            result += parse_value();
        }
        expect(']');
        return result + "]";
    }
    
private:
    const std::string& str_;
    size_t pos_;
};

// Simple JSON writer helper
class json_writer {
public:
    void write_key(const std::string& key) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << escape_json(key) << "\":";
        first_ = false;
    }
    
    void write_string(const std::string& value) {
        ss_ << "\"" << escape_json(value) << "\"";
    }
    
    void write_number(f32 value) {
        ss_ << value;
    }
    
    void write_int(u32 value) {
        ss_ << value;
    }
    
    void write_bool(bool value) {
        ss_ << (value ? "true" : "false");
    }
    
    void begin_object() {
        ss_ << "{";
        first_ = true;
    }
    
    void end_object() {
        ss_ << "}";
        first_ = false;
    }
    
    void begin_array() {
        ss_ << "[";
        first_ = true;
    }
    
    void end_array() {
        ss_ << "]";
        first_ = false;
    }
    
    std::string str() const { return ss_.str(); }
    
private:
    std::ostringstream ss_;
    bool first_ = false;
};

std::string to_json(const particle_preset& preset) {
    json_writer w;
    w.begin_object();
    
    // Metadata
    w.write_key("name"); w.write_string(preset.name);
    w.write_key("category"); w.write_string(preset.category);
    w.write_key("description"); w.write_string(preset.description);
    w.write_key("author"); w.write_string(preset.author);
    w.write_key("version"); w.write_int(preset.version);
    
    // Emitter config
    w.write_key("config");
    w.begin_object();
    
    // Basic settings
    w.write_key("max_particles"); w.write_int(preset.config.max_particles);
    w.write_key("spawn_rate"); w.write_number(preset.config.spawn_rate);
    w.write_key("burst_count"); w.write_int(preset.config.burst_count);
    w.write_key("ring_buffer_mode"); w.write_bool(preset.config.ring_buffer_mode);
    w.write_key("mode"); w.write_int(static_cast<u32>(preset.config.mode));
    
    // Lifetime
    w.write_key("lifetime_min"); w.write_number(preset.config.lifetime_min);
    w.write_key("lifetime_max"); w.write_number(preset.config.lifetime_max);
    
    // Velocity
    w.write_key("velocity_min");
    w.begin_array();
    w.write_number(preset.config.velocity_min.x);
    w.write_number(preset.config.velocity_min.y);
    w.write_number(preset.config.velocity_min.z);
    w.end_array();
    
    w.write_key("velocity_max");
    w.begin_array();
    w.write_number(preset.config.velocity_max.x);
    w.write_number(preset.config.velocity_max.y);
    w.write_number(preset.config.velocity_max.z);
    w.end_array();
    
    // Color
    w.write_key("color_start");
    w.begin_array();
    w.write_number(preset.config.color_start.x);
    w.write_number(preset.config.color_start.y);
    w.write_number(preset.config.color_start.z);
    w.write_number(preset.config.color_start.w);
    w.end_array();
    
    w.write_key("color_end");
    w.begin_array();
    w.write_number(preset.config.color_end.x);
    w.write_number(preset.config.color_end.y);
    w.write_number(preset.config.color_end.z);
    w.write_number(preset.config.color_end.w);
    w.end_array();
    
    // Scale
    w.write_key("scale_min");
    w.begin_array();
    w.write_number(preset.config.scale_min.x);
    w.write_number(preset.config.scale_min.y);
    w.end_array();
    
    w.write_key("scale_max");
    w.begin_array();
    w.write_number(preset.config.scale_max.x);
    w.write_number(preset.config.scale_max.y);
    w.end_array();
    
    w.write_key("scale_multiplier"); w.write_number(preset.config.scale_multiplier);
    
    // Forces
    w.write_key("gravity");
    w.begin_array();
    w.write_number(preset.config.gravity.x);
    w.write_number(preset.config.gravity.y);
    w.write_number(preset.config.gravity.z);
    w.end_array();
    
    w.write_key("wind");
    w.begin_array();
    w.write_number(preset.config.wind.x);
    w.write_number(preset.config.wind.y);
    w.write_number(preset.config.wind.z);
    w.end_array();
    
    w.write_key("drag"); w.write_number(preset.config.drag);
    
    // Rendering
    w.write_key("blend_mode"); w.write_int(static_cast<u32>(preset.config.blending));
    w.write_key("depth_write"); w.write_bool(preset.config.depth_write);
    
    // Texture
    w.write_key("texture_atlas_columns"); w.write_int(preset.config.texture_atlas_columns);
    w.write_key("texture_atlas_rows"); w.write_int(preset.config.texture_atlas_rows);
    w.write_key("texture_random_frame"); w.write_bool(preset.config.texture_random_frame);
    w.write_key("texture_frame_count"); w.write_int(preset.config.texture_frame_count);
    
    w.end_object(); // config
    
    // Curve flags
    w.write_key("use_scale_curve"); w.write_bool(preset.use_scale_curve);
    w.write_key("use_alpha_curve"); w.write_bool(preset.use_alpha_curve);
    w.write_key("use_color_gradient"); w.write_bool(preset.use_color_gradient);
    w.write_key("use_velocity_curve"); w.write_bool(preset.use_velocity_curve);
    w.write_key("use_force_curve"); w.write_bool(preset.use_force_curve);
    w.write_key("use_rotation_curve"); w.write_bool(preset.use_rotation_curve);
    
    w.end_object();
    
    return w.str();
}

bool from_json(const std::string& json_str, particle_preset& preset) {
    // Simple manual parsing (production would use a proper JSON library)
    // This is a minimal implementation for demonstration
    
    auto get_value = [&json_str](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":";
        size_t pos = json_str.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        
        // Skip whitespace
        while (pos < json_str.size() && std::isspace(json_str[pos])) ++pos;
        
        if (pos >= json_str.size()) return "";
        
        if (json_str[pos] == '"') {
            // String value
            ++pos;
            size_t end = json_str.find('"', pos);
            return json_str.substr(pos, end - pos);
        } else if (json_str[pos] == '[') {
            // Array value
            size_t end = json_str.find(']', pos);
            return json_str.substr(pos, end - pos + 1);
        } else {
            // Number or boolean
            size_t end = pos;
            while (end < json_str.size() && !std::isspace(json_str[end]) && 
                   json_str[end] != ',' && json_str[end] != '}') {
                ++end;
            }
            return json_str.substr(pos, end - pos);
        }
    };
    
    auto parse_vec3 = [](const std::string& str) -> math::v3 {
        math::v3 result{ 0, 0, 0 };
        // Simple parsing: [x, y, z]
        size_t start = str.find('[');
        size_t end = str.find(']');
        if (start == std::string::npos || end == std::string::npos) return result;
        
        std::string content = str.substr(start + 1, end - start - 1);
        size_t pos = 0;
        for (int i = 0; i < 3 && pos < content.size(); ++i) {
            size_t next = content.find(',', pos);
            if (next == std::string::npos) next = content.size();
            std::string num = content.substr(pos, next - pos);
            // Trim whitespace
            size_t first = num.find_first_not_of(" \t");
            if (first != std::string::npos) {
                num = num.substr(first);
            }
            result[i] = std::stof(num);
            pos = next + 1;
        }
        return result;
    };
    
    auto parse_vec4 = [](const std::string& str) -> math::v4 {
        math::v4 result{ 0, 0, 0, 1 };
        size_t start = str.find('[');
        size_t end = str.find(']');
        if (start == std::string::npos || end == std::string::npos) return result;
        
        std::string content = str.substr(start + 1, end - start - 1);
        size_t pos = 0;
        for (int i = 0; i < 4 && pos < content.size(); ++i) {
            size_t next = content.find(',', pos);
            if (next == std::string::npos) next = content.size();
            std::string num = content.substr(pos, next - pos);
            size_t first = num.find_first_not_of(" \t");
            if (first != std::string::npos) {
                num = num.substr(first);
            }
            result[i] = std::stof(num);
            pos = next + 1;
        }
        return result;
    };
    
    auto parse_vec2 = [](const std::string& str) -> math::v2 {
        math::v2 result{ 0, 0 };
        size_t start = str.find('[');
        size_t end = str.find(']');
        if (start == std::string::npos || end == std::string::npos) return result;
        
        std::string content = str.substr(start + 1, end - start - 1);
        size_t pos = 0;
        for (int i = 0; i < 2 && pos < content.size(); ++i) {
            size_t next = content.find(',', pos);
            if (next == std::string::npos) next = content.size();
            std::string num = content.substr(pos, next - pos);
            size_t first = num.find_first_not_of(" \t");
            if (first != std::string::npos) {
                num = num.substr(first);
            }
            result[i] = std::stof(num);
            pos = next + 1;
        }
        return result;
    };
    
    // Parse metadata
    preset.name = unescape_json(get_value("name"));
    preset.category = unescape_json(get_value("category"));
    preset.description = unescape_json(get_value("description"));
    preset.author = unescape_json(get_value("author"));
    
    std::string version_str = get_value("version");
    if (!version_str.empty()) preset.version = std::stoul(version_str);
    
    // Parse config
    std::string val;
    
    val = get_value("max_particles");
    if (!val.empty()) preset.config.max_particles = std::stoul(val);
    
    val = get_value("spawn_rate");
    if (!val.empty()) preset.config.spawn_rate = std::stof(val);
    
    val = get_value("burst_count");
    if (!val.empty()) preset.config.burst_count = std::stoul(val);
    
    val = get_value("ring_buffer_mode");
    preset.config.ring_buffer_mode = (val == "true");
    
    val = get_value("mode");
    if (!val.empty()) preset.config.mode = static_cast<emission_mode>(std::stoul(val));
    
    val = get_value("lifetime_min");
    if (!val.empty()) preset.config.lifetime_min = std::stof(val);
    
    val = get_value("lifetime_max");
    if (!val.empty()) preset.config.lifetime_max = std::stof(val);
    
    val = get_value("velocity_min");
    if (!val.empty()) preset.config.velocity_min = parse_vec3(val);
    
    val = get_value("velocity_max");
    if (!val.empty()) preset.config.velocity_max = parse_vec3(val);
    
    val = get_value("color_start");
    if (!val.empty()) preset.config.color_start = parse_vec4(val);
    
    val = get_value("color_end");
    if (!val.empty()) preset.config.color_end = parse_vec4(val);
    
    val = get_value("scale_min");
    if (!val.empty()) preset.config.scale_min = parse_vec2(val);
    
    val = get_value("scale_max");
    if (!val.empty()) preset.config.scale_max = parse_vec2(val);
    
    val = get_value("scale_multiplier");
    if (!val.empty()) preset.config.scale_multiplier = std::stof(val);
    
    val = get_value("gravity");
    if (!val.empty()) preset.config.gravity = parse_vec3(val);
    
    val = get_value("wind");
    if (!val.empty()) preset.config.wind = parse_vec3(val);
    
    val = get_value("drag");
    if (!val.empty()) preset.config.drag = std::stof(val);
    
    val = get_value("blend_mode");
    if (!val.empty()) preset.config.blending = static_cast<blend_mode>(std::stoul(val));
    
    val = get_value("depth_write");
    preset.config.depth_write = (val == "true");
    
    val = get_value("texture_atlas_columns");
    if (!val.empty()) preset.config.texture_atlas_columns = std::stoul(val);
    
    val = get_value("texture_atlas_rows");
    if (!val.empty()) preset.config.texture_atlas_rows = std::stoul(val);
    
    val = get_value("texture_random_frame");
    preset.config.texture_random_frame = (val == "true");
    
    val = get_value("texture_frame_count");
    if (!val.empty()) preset.config.texture_frame_count = std::stoul(val);
    
    // Parse curve flags
    val = get_value("use_scale_curve");
    preset.use_scale_curve = (val == "true");
    
    val = get_value("use_alpha_curve");
    preset.use_alpha_curve = (val == "true");
    
    val = get_value("use_color_gradient");
    preset.use_color_gradient = (val == "true");
    
    val = get_value("use_velocity_curve");
    preset.use_velocity_curve = (val == "true");
    
    val = get_value("use_force_curve");
    preset.use_force_curve = (val == "true");
    
    val = get_value("use_rotation_curve");
    preset.use_rotation_curve = (val == "true");
    
    return true;
}

bool save_to_file(const std::string& filepath, const particle_preset& preset) {
    std::string json = to_json(preset);
    
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[ParticlePreset] Failed to open file for writing: " << filepath << std::endl;
        return false;
    }
    
    file << json;
    return true;
}

bool load_from_file(const std::string& filepath, particle_preset& preset) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[ParticlePreset] Failed to open file for reading: " << filepath << std::endl;
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();
    
    return from_json(json, preset);
}

} // namespace preset_json

// -----------------------------------------------------------------------------
// preset_manager Implementation
// -----------------------------------------------------------------------------

bool preset_manager::load_preset(const std::string& filepath) {
    particle_preset preset;
    if (!preset_json::load_from_file(filepath, preset)) {
        return false;
    }
    
    return add_preset(preset);
}

bool preset_manager::save_preset(const std::string& filepath, const particle_preset& preset) {
    return preset_json::save_to_file(filepath, preset);
}

bool preset_manager::add_preset(const particle_preset& preset) {
    if (preset.name.empty()) {
        return false;
    }
    
    presets_[preset.name] = preset;
    
    // Add to category
    if (!preset.category.empty()) {
        categories_[preset.category].push_back(preset.name);
    }
    
    if (on_preset_added_) {
        on_preset_added_(preset.name);
    }
    
    return true;
}

bool preset_manager::remove_preset(const std::string& name) {
    auto it = presets_.find(name);
    if (it == presets_.end()) {
        return false;
    }
    
    // Remove from category
    const std::string& category = it->second.category;
    if (!category.empty()) {
        auto& cat_list = categories_[category];
        cat_list.erase(std::remove(cat_list.begin(), cat_list.end(), name), cat_list.end());
    }
    
    presets_.erase(it);
    
    if (on_preset_removed_) {
        on_preset_removed_(name);
    }
    
    return true;
}

const particle_preset* preset_manager::get_preset(const std::string& name) const {
    auto it = presets_.find(name);
    if (it == presets_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<std::string> preset_manager::get_categories() const {
    std::vector<std::string> result;
    result.reserve(categories_.size());
    for (const auto& pair : categories_) {
        result.push_back(pair.first);
    }
    return result;
}

std::vector<std::string> preset_manager::get_presets_in_category(const std::string& category) const {
    auto it = categories_.find(category);
    if (it == categories_.end()) {
        return {};
    }
    return it->second;
}

std::vector<std::string> preset_manager::get_all_preset_names() const {
    std::vector<std::string> result;
    result.reserve(presets_.size());
    for (const auto& pair : presets_) {
        result.push_back(pair.first);
    }
    return result;
}

u32 preset_manager::load_presets_from_directory(const std::string& directory) {
    // This would use filesystem to iterate .json files
    // For now, return 0 as placeholder
    (void)directory;
    return 0;
}

bool preset_manager::save_all_presets_to_directory(const std::string& directory) {
    bool success = true;
    for (const auto& pair : presets_) {
        std::string filepath = directory + "/" + pair.first + ".json";
        if (!save_preset(filepath, pair.second)) {
            success = false;
        }
    }
    return success;
}

bool preset_manager::has_preset(const std::string& name) const {
    return presets_.find(name) != presets_.end();
}

// -----------------------------------------------------------------------------
// Default Presets Implementation
// -----------------------------------------------------------------------------

namespace default_presets {

particle_preset fire() {
    particle_preset preset;
    preset.name = "Fire";
    preset.category = "Effects";
    preset.description = "Basic fire effect with orange-red color";
    
    preset.config.spawn_rate = 50.0f;
    preset.config.lifetime_min = 0.5f;
    preset.config.lifetime_max = 1.5f;
    
    preset.config.velocity_min = { -0.5f, 2.0f, -0.5f };
    preset.config.velocity_max = { 0.5f, 5.0f, 0.5f };
    
    preset.config.color_start = { 1.0f, 0.8f, 0.2f, 1.0f }; // Yellow-orange
    preset.config.color_end = { 1.0f, 0.2f, 0.0f, 0.0f };  // Red, transparent
    
    preset.config.scale_min = { 0.2f, 0.2f };
    preset.config.scale_max = { 0.5f, 0.5f };
    
    preset.config.gravity = { 0.0f, 0.5f, 0.0f }; // Slight upward buoyancy
    
    preset.config.blending = blend_mode::additive;
    
    preset.use_alpha_curve = true;
    preset.alpha_curve.keyframes = { {0.0f, 1.0f}, {0.7f, 1.0f}, {1.0f, 0.0f} };
    
    return preset;
}

particle_preset smoke() {
    particle_preset preset;
    preset.name = "Smoke";
    preset.category = "Effects";
    preset.description = "Rising smoke effect";
    
    preset.config.spawn_rate = 20.0f;
    preset.config.lifetime_min = 2.0f;
    preset.config.lifetime_max = 4.0f;
    
    preset.config.velocity_min = { -0.3f, 0.5f, -0.3f };
    preset.config.velocity_max = { 0.3f, 1.5f, 0.3f };
    
    preset.config.color_start = { 0.5f, 0.5f, 0.5f, 0.8f };
    preset.config.color_end = { 0.3f, 0.3f, 0.3f, 0.0f };
    
    preset.config.scale_min = { 0.5f, 0.5f };
    preset.config.scale_max = { 1.0f, 1.0f };
    
    preset.config.gravity = { 0.0f, -0.2f, 0.0f };
    preset.config.drag = 0.3f;
    
    preset.config.blending = blend_mode::alpha;
    
    preset.use_scale_curve = true;
    preset.scale_curve.keyframes = { {0.0f, 0.5f}, {1.0f, 2.0f} };
    
    return preset;
}

particle_preset explosion() {
    particle_preset preset;
    preset.name = "Explosion";
    preset.category = "Effects";
    preset.description = "Explosion burst effect";
    
    preset.config.mode = emission_mode::burst;
    preset.config.burst_count = 100;
    preset.config.lifetime_min = 0.3f;
    preset.config.lifetime_max = 1.0f;
    
    preset.config.velocity_min = { -10.0f, -10.0f, -10.0f };
    preset.config.velocity_max = { 10.0f, 10.0f, 10.0f };
    
    preset.config.color_start = { 1.0f, 1.0f, 0.5f, 1.0f };
    preset.config.color_end = { 1.0f, 0.3f, 0.0f, 0.0f };
    
    preset.config.scale_min = { 0.3f, 0.3f };
    preset.config.scale_max = { 0.6f, 0.6f };
    
    preset.config.drag = 2.0f;
    
    preset.config.blending = blend_mode::additive;
    
    return preset;
}

particle_preset magic_sparkle() {
    particle_preset preset;
    preset.name = "Magic Sparkle";
    preset.category = "Effects";
    preset.description = "Magical sparkle effect";
    
    preset.config.spawn_rate = 30.0f;
    preset.config.lifetime_min = 0.5f;
    preset.config.lifetime_max = 1.5f;
    
    preset.config.velocity_min = { -1.0f, -1.0f, -1.0f };
    preset.config.velocity_max = { 1.0f, 1.0f, 1.0f };
    
    preset.config.color_start = { 0.5f, 0.3f, 1.0f, 1.0f }; // Purple
    preset.config.color_end = { 0.3f, 0.8f, 1.0f, 0.0f };  // Cyan
    
    preset.config.scale_min = { 0.1f, 0.1f };
    preset.config.scale_max = { 0.2f, 0.2f };
    
    preset.config.blending = blend_mode::additive;
    
    return preset;
}

particle_preset rain() {
    particle_preset preset;
    preset.name = "Rain";
    preset.category = "Weather";
    preset.description = "Falling rain effect";
    
    preset.config.spawn_rate = 200.0f;
    preset.config.lifetime_min = 1.0f;
    preset.config.lifetime_max = 2.0f;
    
    preset.config.velocity_min = { -0.5f, -15.0f, -0.5f };
    preset.config.velocity_max = { 0.5f, -10.0f, 0.5f };
    
    preset.config.color_start = { 0.7f, 0.8f, 1.0f, 0.6f };
    preset.config.color_end = { 0.7f, 0.8f, 1.0f, 0.3f };
    
    preset.config.scale_min = { 0.02f, 0.2f };
    preset.config.scale_max = { 0.05f, 0.4f };
    
    preset.config.blending = blend_mode::alpha;
    
    return preset;
}

particle_preset snow() {
    particle_preset preset;
    preset.name = "Snow";
    preset.category = "Weather";
    preset.description = "Falling snow effect";
    
    preset.config.spawn_rate = 50.0f;
    preset.config.lifetime_min = 5.0f;
    preset.config.lifetime_max = 10.0f;
    
    preset.config.velocity_min = { -0.5f, -1.0f, -0.5f };
    preset.config.velocity_max = { 0.5f, -2.0f, 0.5f };
    
    preset.config.color_start = { 1.0f, 1.0f, 1.0f, 0.9f };
    preset.config.color_end = { 0.9f, 0.95f, 1.0f, 0.5f };
    
    preset.config.scale_min = { 0.05f, 0.05f };
    preset.config.scale_max = { 0.15f, 0.15f };
    
    preset.config.drag = 0.5f;
    
    preset.config.blending = blend_mode::alpha;
    
    return preset;
}

particle_preset dust() {
    particle_preset preset;
    preset.name = "Dust";
    preset.category = "Effects";
    preset.description = "Floating dust particles";
    
    preset.config.spawn_rate = 10.0f;
    preset.config.lifetime_min = 3.0f;
    preset.config.lifetime_max = 8.0f;
    
    preset.config.velocity_min = { -0.2f, -0.1f, -0.2f };
    preset.config.velocity_max = { 0.2f, 0.1f, 0.2f };
    
    preset.config.color_start = { 0.8f, 0.7f, 0.6f, 0.5f };
    preset.config.color_end = { 0.6f, 0.5f, 0.4f, 0.0f };
    
    preset.config.scale_min = { 0.02f, 0.02f };
    preset.config.scale_max = { 0.1f, 0.1f };
    
    preset.config.drag = 1.0f;
    
    preset.config.blending = blend_mode::alpha;
    
    return preset;
}

particle_preset bubbles() {
    particle_preset preset;
    preset.name = "Bubbles";
    preset.category = "Effects";
    preset.description = "Rising bubbles effect";
    
    preset.config.spawn_rate = 15.0f;
    preset.config.lifetime_min = 2.0f;
    preset.config.lifetime_max = 5.0f;
    
    preset.config.velocity_min = { -0.2f, 0.5f, -0.2f };
    preset.config.velocity_max = { 0.2f, 1.5f, 0.2f };
    
    preset.config.color_start = { 0.8f, 0.9f, 1.0f, 0.6f };
    preset.config.color_end = { 0.6f, 0.8f, 1.0f, 0.3f };
    
    preset.config.scale_min = { 0.1f, 0.1f };
    preset.config.scale_max = { 0.3f, 0.3f };
    
    preset.config.blending = blend_mode::alpha;
    
    preset.use_scale_curve = true;
    preset.scale_curve.keyframes = { {0.0f, 0.5f}, {1.0f, 1.2f} };
    
    return preset;
}

void register_defaults(preset_manager& manager) {
    manager.add_preset(fire());
    manager.add_preset(smoke());
    manager.add_preset(explosion());
    manager.add_preset(magic_sparkle());
    manager.add_preset(rain());
    manager.add_preset(snow());
    manager.add_preset(dust());
    manager.add_preset(bubbles());
}

} // namespace default_presets

// -----------------------------------------------------------------------------
// Global Preset System
// -----------------------------------------------------------------------------

namespace {
    preset_manager* g_preset_manager = nullptr;
}

namespace preset_system {

bool initialize() {
    if (g_preset_manager) {
        return true;
    }
    
    g_preset_manager = new preset_manager();
    
    // Register default presets
    default_presets::register_defaults(*g_preset_manager);
    
    std::cout << "[PresetSystem] Initialized with " 
              << g_preset_manager->preset_count() << " default presets" << std::endl;
    
    return true;
}

void shutdown() {
    if (g_preset_manager) {
        delete g_preset_manager;
        g_preset_manager = nullptr;
    }
}

preset_manager* get() {
    return g_preset_manager;
}

bool is_initialized() {
    return g_preset_manager != nullptr;
}

} // namespace preset_system

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
