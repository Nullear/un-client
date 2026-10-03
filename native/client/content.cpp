#include "content.hpp"
#include "asset_cipher.hpp"

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

using namespace godot;

namespace {
std::map<String, Ref<Texture2D>> image_cache;
std::map<String, String> song_titles;
bool song_titles_loaded = false;
std::vector<String> warmup_files;
int warmup_index = 0;
int warmup_ready = 0;
bool warmup_started = false;
Array &character_cache() { static Array value; return value; }
bool characters_loaded = false;
Ref<ConfigFile> partner_config;
Dictionary &partner_stats() { static Dictionary value; return value; }
Array &partner_access() { static Array value; return value; }
bool partner_access_known = false;
std::map<std::pair<int, bool>, Ref<ShaderMaterial>> hue_cache;

void load_partner_config() {
    if (partner_config.is_valid()) return;
    partner_config.instantiate();
    partner_config->load("user://client.cfg");
    const String stats = partner_config->get_value("partners", "stats", "");
    const Variant parsed_stats = stats.begins_with("{") ? JSON::parse_string(stats) : Variant();
    if (parsed_stats.get_type() == Variant::DICTIONARY) partner_stats() = parsed_stats;
    const String access = partner_config->get_value("partners", "access", "");
    const Variant parsed_access = access.begins_with("[") ? JSON::parse_string(access) : Variant();
    partner_access_known = parsed_access.get_type() == Variant::ARRAY;
    if (partner_access_known) partner_access() = parsed_access;
}

struct WarmJob {
    String relative;
    String file;
    bool jpg = false;
    Ref<Image> image;
    std::atomic<bool> done{false};
    int64_t task = -1;
};

std::vector<std::unique_ptr<WarmJob>> warm_live;

int warm_limit() {
    int count = OS::get_singleton()->get_processor_count();
    if (OS::get_singleton()->get_name() == "Android") return 2;
    if (count < 4) count = 4;
    if (count > 8) count = 8;
    return count;
}

PackedByteArray open_bytes(const String &file) {
    PackedByteArray bytes = FileAccess::get_file_as_bytes(file);
    if (!bytes.is_empty() && file.begins_with("user://")) asset_cipher::apply(bytes);
    return bytes;
}

void warm_decode(void *userdata) {
    WarmJob *job = static_cast<WarmJob *>(userdata);
    PackedByteArray bytes = open_bytes(job->file);
    if (!bytes.is_empty()) {
        Ref<Image> decoded;
        decoded.instantiate();
        const Error err = job->jpg ? decoded->load_jpg_from_buffer(bytes) : decoded->load_png_from_buffer(bytes);
        if (err == OK) job->image = decoded;
    }
    job->done.store(true, std::memory_order_release);
}
}

String content::path(const String &relative) {
    return String("user://assets/") + relative;
}

Ref<Texture2D> content::image(const String &relative) {
    if (image_cache.count(relative)) return image_cache.at(relative);
    const String file = path(relative);
    if (!FileAccess::file_exists(file)) return Ref<Texture2D>();
    Ref<Image> decoded;
    decoded.instantiate();
    PackedByteArray bytes = open_bytes(file);
    Error err = relative.ends_with(".jpg") ? decoded->load_jpg_from_buffer(bytes) : decoded->load_png_from_buffer(bytes);
    if (err != OK) return Ref<Texture2D>();
    Ref<Texture2D> result = ImageTexture::create_from_image(decoded);
    if (result.is_valid()) image_cache[relative] = result;
    return result;
}

Array content::characters() {
    if (characters_loaded) return character_cache();
    characters_loaded = true;
    const String file = path("char/characters.json");
    if (!FileAccess::file_exists(file)) return Array();
    const PackedByteArray bytes = open_bytes(file);
    const String text = String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size()).strip_edges();
    if (!text.begins_with("[") && !text.begins_with("{")) return Array();
    Variant result = JSON::parse_string(text);
    character_cache() = result.get_type() == Variant::ARRAY ? Array(result) : Array();
    return character_cache();
}

Dictionary content::character(const Array &catalog, int id) {
    for (int i = 0; i < catalog.size(); ++i) {
        Dictionary row = catalog[i];
        if (int(row.get("character_id", -1)) == id) return row;
    }
    return Dictionary();
}

int content::selected() {
    load_partner_config();
    return int(partner_config->get_value("preview", "character", 0));
}

void content::save_selected(int id) {
    load_partner_config();
    Ref<ConfigFile> config = partner_config;
    config->set_value("preview", "character", id);
    config->save("user://client.cfg");
}

static bool number_value(const Variant &value, double &out) {
    if (value.get_type() == Variant::INT || value.get_type() == Variant::FLOAT) { out = double(value); return true; }
    if (value.get_type() == Variant::STRING && String(value).is_valid_float()) { out = String(value).to_float(); return true; }
    return false;
}

static Array find_id_array(const Variant &value) {
    if (value.get_type() == Variant::ARRAY) {
        const Array list = value;
        Array ids;
        for (int i = 0; i < list.size(); ++i) {
            double number = 0;
            if (!number_value(list[i], number) || number < 0) return Array();
            ids.push_back(int(number));
        }
        return ids;
    }
    if (value.get_type() != Variant::DICTIONARY) return Array();
    const Dictionary object = value;
    const Array keys = object.keys();
    for (int i = 0; i < keys.size(); ++i) {
        if (String(keys[i]).to_lower() != "characters") continue;
        const Array ids = find_id_array(object[keys[i]]);
        if (!ids.is_empty()) return ids;
    }
    for (int i = 0; i < keys.size(); ++i) {
        const Variant child = object[keys[i]];
        if (child.get_type() != Variant::DICTIONARY) continue;
        const Array ids = find_id_array(child);
        if (!ids.is_empty()) return ids;
    }
    return Array();
}

static Dictionary find_stat_map(const Variant &value) {
    if (value.get_type() == Variant::ARRAY) {
        const Array list = value;
        Dictionary stats;
        for (int i = 0; i < list.size(); ++i) {
            if (list[i].get_type() != Variant::DICTIONARY) continue;
            const Dictionary row = list[i];
            double id = 0;
            if (!number_value(row.get("character_id", row.get("characterId", Variant())), id)) continue;
            Dictionary entry;
            double number = 0;
            if (number_value(row.get("frag", Variant()), number)) entry["frag"] = number;
            if (number_value(row.get("level", Variant()), number)) entry["level"] = int(number);
            if (number_value(row.get("exp", Variant()), number)) entry["exp"] = number;
            if (number_value(row.get("level_exp", row.get("levelExp", Variant())), number)) entry["level_exp"] = number;
            const Variant flag = row.get("uncapped", row.get("is_uncapped", false));
            entry["uncapped"] = flag == Variant(true) || int(flag) == 1 || String(flag).to_lower() == "true";
            if (!entry.has("frag") && !entry.has("level") && !entry.has("exp")) continue;
            stats[String::num_int64(int(id))] = entry;
        }
        return stats;
    }
    if (value.get_type() != Variant::DICTIONARY) return Dictionary();
    const Dictionary object = value;
    const Array keys = object.keys();
    for (int i = 0; i < keys.size(); ++i) {
        if (String(keys[i]).to_lower() != "character_stats") continue;
        const Dictionary stats = find_stat_map(object[keys[i]]);
        if (!stats.is_empty()) return stats;
    }
    for (int i = 0; i < keys.size(); ++i) {
        const Dictionary stats = find_stat_map(object[keys[i]]);
        if (!stats.is_empty()) return stats;
    }
    return Dictionary();
}

static bool find_named_number(const Variant &value, const String &key, double &out) {
    if (value.get_type() == Variant::ARRAY) {
        const Array list = value;
        for (int i = 0; i < list.size(); ++i)
            if (find_named_number(list[i], key, out)) return true;
        return false;
    }
    if (value.get_type() != Variant::DICTIONARY) return false;
    const Dictionary object = value;
    const Array keys = object.keys();
    for (int i = 0; i < keys.size(); ++i) {
        if (String(keys[i]).to_lower() != key) continue;
        if (number_value(object[keys[i]], out)) return true;
    }
    for (int i = 0; i < keys.size(); ++i)
        if (find_named_number(object[keys[i]], key, out)) return true;
    return false;
}

static Dictionary read_stats() {
    load_partner_config();
    return partner_stats();
}

void content::remember_online(const Variant &parsed) {
    load_partner_config();
    Ref<ConfigFile> config = partner_config;
    const Array ids = find_id_array(parsed);
    if (!ids.is_empty()) config->set_value("partners", "access", JSON::stringify(ids));
    const Dictionary stats = find_stat_map(parsed);
    partner_stats() = stats;
    if (!ids.is_empty()) {
        partner_access() = ids;
        partner_access_known = true;
    }
    config->set_value("partners", "stats", stats.is_empty() ? "" : JSON::stringify(stats));
    double ticket = 0;
    if (find_named_number(parsed, "ticket", ticket)) config->set_value("partners", "ticket", String::num_int64(int(ticket)));
    else config->set_value("partners", "ticket", "");
    config->save("user://client.cfg");
}

bool content::unlocked(int id) {
    load_partner_config();
    if (!partner_access_known) return true;
    const Array ids = partner_access();
    for (int i = 0; i < ids.size(); ++i)
        if (int(ids[i]) == id) return true;
    return false;
}

int content::level_of(int id) {
    const Dictionary stats = read_stats();
    if (!stats.has(String::num_int64(id))) return -1;
    const Dictionary entry = stats[String::num_int64(id)];
    return entry.has("level") ? int(entry["level"]) : -1;
}

double content::exp_into_level(int id) {
    const Dictionary stats = read_stats();
    if (!stats.has(String::num_int64(id))) return -1;
    const Dictionary entry = stats[String::num_int64(id)];
    if (!entry.has("exp") || !entry.has("level_exp")) return -1;
    return double(entry["exp"]) - double(entry["level_exp"]);
}

bool content::uncapped(int id) {
    const Dictionary stats = read_stats();
    if (!stats.has(String::num_int64(id))) return false;
    return bool(Dictionary(stats[String::num_int64(id)]).get("uncapped", false));
}

int content::ticket() {
    load_partner_config();
    Ref<ConfigFile> config = partner_config;
    const String text = String(config->get_value("partners", "ticket", ""));
    return text.is_valid_int() ? text.to_int() : -1;
}

void content::set_ticket(int value) {
    load_partner_config();
    Ref<ConfigFile> config = partner_config;
    config->set_value("partners", "ticket", String::num_int64(value));
    config->save("user://client.cfg");
}

static int catalog_int(int id, const char *key) {
    double number = 0;
    const Dictionary row = content::character(content::characters(), id);
    return number_value(row.get(key, Variant()), number) ? int(number) : -1;
}

int content::base_frag(int id) { return catalog_int(id, "base_frag"); }

int content::max_frag(int id) { return catalog_int(id, "max_frag"); }

bool content::uncappable(int id) {
    const Variant flag = content::character(content::characters(), id).get("is_uncappable", false);
    return flag == Variant(true) || int(flag) == 1 || String(flag).to_lower() == "true";
}

int content::frag_of(int id) {
    const Dictionary stats = read_stats();
    const String key = String::num_int64(id);
    if (stats.has(key)) {
        double number = 0;
        if (number_value(Dictionary(stats[key]).get("frag", Variant()), number)) return int(number);
    }
    return catalog_int(id, "base_frag");
}

void content::grant_purchased(int id) {
    load_partner_config();
    Ref<ConfigFile> config = partner_config;
    Array ids = partner_access();
    bool found = false;
    for (int i = 0; i < ids.size(); ++i)
        if (int(ids[i]) == id) found = true;
    if (!found) ids.push_back(id);
    partner_access() = ids;
    partner_access_known = true;
    config->set_value("partners", "access", JSON::stringify(ids));
    Dictionary stats = read_stats();
    const String key = String::num_int64(id);
    Dictionary entry;
    if (stats.has(key) && stats[key].get_type() == Variant::DICTIONARY) entry = stats[key];
    entry["level"] = 1;
    stats[key] = entry;
    config->set_value("partners", "stats", JSON::stringify(stats));
    config->save("user://client.cfg");
}

String content::random_jacket() {
    Ref<DirAccess> directory = DirAccess::open(path("songs"));
    if (!directory.is_valid()) return "";
    const PackedStringArray folders = directory->get_directories();
    if (folders.is_empty()) return "";
    const int start = UtilityFunctions::randi_range(0, folders.size() - 1);
    for (int i = 0; i < folders.size(); ++i) {
        const String relative = String("songs/") + folders[(start + i) % folders.size()] + String("/1080_base.jpg");
        const String file = path(relative);
        if (FileAccess::file_exists(file)) return relative;
    }
    return "";
}

String content::song_title(const String &id) {
    if (!song_titles_loaded) {
        song_titles_loaded = true;
        const PackedByteArray bytes = open_bytes(path("songs/songlist"));
        const String text = bytes.is_empty() ? String() : String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size()).strip_edges();
        const Variant parsed = text.is_empty() ? Variant() : JSON::parse_string(text);
        if (parsed.get_type() == Variant::DICTIONARY) {
            const Array songs = Dictionary(parsed).get("songs", Array());
            for (int i = 0; i < songs.size(); ++i) {
                if (songs[i].get_type() != Variant::DICTIONARY) continue;
                const Dictionary song = songs[i];
                const String song_id = song.get("id", "");
                const Dictionary localized = song.get("title_localized", Dictionary());
                const String title = localized.get("en", "");
                if (!song_id.is_empty() && !title.is_empty()) song_titles[song_id] = title;
            }
        }
    }
    return song_titles.count(id) ? song_titles.at(id) : id;
}

Ref<ShaderMaterial> content::hue_material(const Dictionary &row, bool strip) {
    const std::pair<int, bool> key(int(row.get("hue", 193)), !strip && bool(row.get("is_grey", false)));
    const auto cached = hue_cache.find(key);
    if (cached != hue_cache.end()) return cached->second;
    Ref<ShaderMaterial> material;
    material.instantiate();
    Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://native/client/hue.gdshader");
    material->set_shader(shader);
    material->set_shader_parameter("hue", float(row.get("hue", 193)));
    material->set_shader_parameter("grey", !strip && bool(row.get("is_grey", false)));
    hue_cache[key] = material;
    return material;
}

void content::warmup_partner_assets_begin() {
    if (warmup_started) return;
    warmup_started = true;
    warmup_index = 0;
    warmup_ready = 0;
    warmup_files.clear();
    Ref<DirAccess> directory = DirAccess::open(path("char"));
    if (!directory.is_valid()) return;
    PackedStringArray files = directory->get_files();
    for (const String &entry : files) {
        const String file = entry.trim_suffix(".remap");
        if (file.ends_with("_icon.png")) {
            warmup_files.push_back(String("char/") + file);
        }
    }
    Ref<DirAccess> portraits = DirAccess::open(path("char/1080"));
    if (portraits.is_valid()) {
        const PackedStringArray portrait_files = portraits->get_files();
        for (const String &entry : portrait_files) {
            const String file = entry.trim_suffix(".remap");
            if (file.ends_with(".png")) warmup_files.push_back(String("char/1080/") + file);
        }
    }
}

bool content::warmup_partner_assets_step(int) {
    if (!warmup_started) warmup_partner_assets_begin();
    int index = 0;
    int uploaded = 0;
    while (index < int(warm_live.size())) {
        WarmJob *job = warm_live[index].get();
        if (!job->done.load(std::memory_order_acquire)) {
            ++index;
            continue;
        }
        if (uploaded >= 1) break;
        if (job->image.is_valid()) {
            Ref<Texture2D> texture = ImageTexture::create_from_image(job->image);
            if (texture.is_valid()) {
                image_cache[job->relative] = texture;
                ++warmup_ready;
            }
        }
        warm_live.erase(warm_live.begin() + index);
        ++uploaded;
    }
    const int limit = warm_limit();
    WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
    while (warmup_index < int(warmup_files.size()) && int(warm_live.size()) < limit) {
        auto job = std::make_unique<WarmJob>();
        job->relative = warmup_files[warmup_index++];
        if (image_cache.count(job->relative)) {
            ++warmup_ready;
            continue;
        }
        job->file = path(job->relative);
        job->jpg = job->relative.ends_with(".jpg");
        WarmJob *raw = job.get();
        const int64_t task = pool ? pool->add_native_task(&warm_decode, raw, true, "Warm partner") : -1;
        if (task < 0) {
            if (content::image(raw->relative).is_valid()) ++warmup_ready;
            continue;
        }
        raw->task = task;
        warm_live.push_back(std::move(job));
    }
    return warmup_index >= int(warmup_files.size()) && warm_live.empty();
}

int content::warmup_partner_assets_total() { return warmup_files.size(); }
int content::warmup_partner_assets_ready() { return warmup_ready; }
void content::release_cached_images() {
    characters_loaded = false;
    character_cache().clear();
    hue_cache.clear();
    WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
    for (const std::unique_ptr<WarmJob> &job : warm_live) {
        if (pool && job->task >= 0 && !job->done.load(std::memory_order_acquire))
            pool->wait_for_task_completion(job->task);
    }
    warm_live.clear();
    image_cache.clear();
    warmup_files.clear();
    warmup_started = false;
    warmup_index = 0;
    warmup_ready = 0;
}
