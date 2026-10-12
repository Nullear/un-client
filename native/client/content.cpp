#include "content.hpp"
#include "asset_cipher.hpp"

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <wincodec.h>
#endif

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
std::map<int, PackedStringArray> song_rating_candidates;
std::map<String, bool> failed_images;
std::mutex jpeg_decode_mutex;

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
std::vector<std::unique_ptr<WarmJob>> gallery_live;

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

bool valid_jpeg(const PackedByteArray &bytes) {
    if (bytes.size() < 4 || uint8_t(bytes[0]) != 0xff || uint8_t(bytes[1]) != 0xd8) return false;
    bool has_frame = false;
    bool has_scan = false;
    int offset = 2;
    while (offset + 1 < bytes.size()) {
        if (uint8_t(bytes[offset++]) != 0xff) return false;
        while (offset < bytes.size() && uint8_t(bytes[offset]) == 0xff) ++offset;
        if (offset >= bytes.size()) return false;
        const uint8_t marker = uint8_t(bytes[offset++]);
        if (marker == 0xd9) return has_frame && has_scan;
        if (marker == 0xda) has_scan = true;
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) continue;
        if (offset + 1 >= bytes.size()) return false;
        const int length = (int(uint8_t(bytes[offset])) << 8) | uint8_t(bytes[offset + 1]);
        if (length < 2 || offset + length > bytes.size()) return false;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7)
            || (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) has_frame = true;
        offset += length;
        if (has_scan) {
            while (offset + 1 < bytes.size()) {
                if (uint8_t(bytes[offset++]) != 0xff) continue;
                const uint8_t scan_marker = uint8_t(bytes[offset++]);
                if (scan_marker == 0x00 || (scan_marker >= 0xd0 && scan_marker <= 0xd7)) continue;
                if (scan_marker == 0xd9) return has_frame;
                return false;
            }
            return false;
        }
    }
    return false;
}

String jpeg_summary(const PackedByteArray &bytes) {
    String result = String("size=") + String::num_int64(bytes.size()) + String(" header=");
    constexpr char hex[] = "0123456789abcdef";
    const int header_length = int(std::min<int64_t>(12, bytes.size()));
    for (int i = 0; i < header_length; ++i) {
        if (i) result += " ";
        const uint8_t value = uint8_t(bytes[i]);
        char pair[] = {hex[value >> 4], hex[value & 0x0f], '\0'};
        result += String::utf8(pair);
    }
    result += " tail=";
    const int tail_start = int(std::max<int64_t>(0, bytes.size() - 4));
    for (int i = tail_start; i < bytes.size(); ++i) {
        if (i > tail_start) result += " ";
        const uint8_t value = uint8_t(bytes[i]);
        char pair[] = {hex[value >> 4], hex[value & 0x0f], '\0'};
        result += String::utf8(pair);
    }
    int offset = 2;
    while (offset + 3 < bytes.size()) {
        if (uint8_t(bytes[offset++]) != 0xff) break;
        while (offset < bytes.size() && uint8_t(bytes[offset]) == 0xff) ++offset;
        if (offset >= bytes.size()) break;
        const uint8_t marker = uint8_t(bytes[offset++]);
        if (marker == 0xda || marker == 0xd9) break;
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) continue;
        if (offset + 1 >= bytes.size()) break;
        const int length = (int(uint8_t(bytes[offset])) << 8) | uint8_t(bytes[offset + 1]);
        if (length < 2 || offset + length > bytes.size()) break;
        const bool is_frame = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7)
            || (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
        if (is_frame && length >= 7) {
            const int height = (int(uint8_t(bytes[offset + 3])) << 8) | uint8_t(bytes[offset + 4]);
            const int width = (int(uint8_t(bytes[offset + 5])) << 8) | uint8_t(bytes[offset + 6]);
            result += String(" sof=0x") + String::num_uint64(marker, 16)
                + String(" dimensions=") + String::num_int64(width) + "x" + String::num_int64(height);
            break;
        }
        offset += length;
    }
    return result;
}

PackedByteArray jpeg_without_comments(const PackedByteArray &bytes) {
    if (bytes.size() < 4 || uint8_t(bytes[0]) != 0xff || uint8_t(bytes[1]) != 0xd8)
        return PackedByteArray();
    std::vector<uint8_t> filtered;
    filtered.reserve(size_t(bytes.size()));
    filtered.push_back(0xff);
    filtered.push_back(0xd8);
    int offset = 2;
    bool removed_comment = false;
    while (offset + 1 < bytes.size()) {
        const int segment_start = offset;
        if (uint8_t(bytes[offset++]) != 0xff) return PackedByteArray();
        while (offset < bytes.size() && uint8_t(bytes[offset]) == 0xff) ++offset;
        if (offset >= bytes.size()) return PackedByteArray();
        const uint8_t marker = uint8_t(bytes[offset++]);
        if (marker == 0xda) {
            for (int i = segment_start; i < bytes.size(); ++i) filtered.push_back(uint8_t(bytes[i]));
            break;
        }
        if (marker == 0xd9) {
            for (int i = segment_start; i < offset; ++i) filtered.push_back(uint8_t(bytes[i]));
            break;
        }
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
            for (int i = segment_start; i < offset; ++i) filtered.push_back(uint8_t(bytes[i]));
            continue;
        }
        if (offset + 1 >= bytes.size()) return PackedByteArray();
        const int length = (int(uint8_t(bytes[offset])) << 8) | uint8_t(bytes[offset + 1]);
        const int segment_end = offset + length;
        if (length < 2 || segment_end > bytes.size()) return PackedByteArray();
        if (marker == 0xfe) {
            removed_comment = true;
        } else {
            for (int i = segment_start; i < segment_end; ++i) filtered.push_back(uint8_t(bytes[i]));
        }
        offset = segment_end;
    }
    if (!removed_comment || filtered.size() >= size_t(bytes.size())) return PackedByteArray();
    PackedByteArray result;
    result.resize(int64_t(filtered.size()));
    std::memcpy(result.ptrw(), filtered.data(), filtered.size());
    return result;
}

#if defined(_WIN32)
class ComApartment {
    bool initialized = false;
    bool usable = false;
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized = SUCCEEDED(result);
        usable = initialized || result == RPC_E_CHANGED_MODE;
    }
    ~ComApartment() { if (initialized) CoUninitialize(); }
    bool available() const { return usable; }
};

template <typename T>
class ComObject {
public:
    T *value = nullptr;
    ~ComObject() { if (value) value->Release(); }
    T **out() { return &value; }
    T *operator->() const { return value; }
};

Ref<Image> decode_jpeg_with_wic(const PackedByteArray &bytes, HRESULT &failure) {
    Ref<Image> result;
    if (bytes.is_empty() || uint64_t(bytes.size()) > MAXDWORD) {
        failure = E_INVALIDARG;
        return result;
    }
    ComApartment apartment;
    if (!apartment.available()) {
        failure = CO_E_NOTINITIALIZED;
        return result;
    }
    ComObject<IWICImagingFactory> factory;
    ComObject<IWICStream> stream;
    ComObject<IWICBitmapDecoder> decoder;
    ComObject<IWICBitmapFrameDecode> frame;
    ComObject<IWICFormatConverter> converter;
    failure = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory, reinterpret_cast<void **>(factory.out()));
    if (FAILED(failure)) return result;
    failure = factory->CreateStream(stream.out());
    if (FAILED(failure)) return result;
    failure = stream->InitializeFromMemory(const_cast<BYTE *>(bytes.ptr()), UINT(bytes.size()));
    if (FAILED(failure)) return result;
    failure = factory->CreateDecoderFromStream(stream.value, nullptr, WICDecodeMetadataCacheOnLoad, decoder.out());
    if (FAILED(failure)) return result;
    failure = decoder->GetFrame(0, frame.out());
    if (FAILED(failure)) return result;
    failure = factory->CreateFormatConverter(converter.out());
    if (FAILED(failure)) return result;
    failure = converter->Initialize(frame.value, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(failure)) return result;
    UINT width = 0, height = 0;
    failure = converter->GetSize(&width, &height);
    if (FAILED(failure) || width == 0 || height == 0 || uint64_t(width) * height > MAXDWORD / 4) {
        if (SUCCEEDED(failure)) failure = E_INVALIDARG;
        return result;
    }
    PackedByteArray pixels;
    const UINT stride = width * 4;
    const UINT pixel_bytes = stride * height;
    pixels.resize(pixel_bytes);
    failure = converter->CopyPixels(nullptr, stride, pixel_bytes, pixels.ptrw());
    if (FAILED(failure)) return result;
    result.instantiate();
    result->set_data(int(width), int(height), false, Image::FORMAT_RGBA8, pixels);
    failure = S_OK;
    return result;
}
#endif

void warm_decode(void *userdata) {
    WarmJob *job = static_cast<WarmJob *>(userdata);
    UtilityFunctions::print(String("[warm] request relative=") + job->relative
        + String(" file=") + job->file);
    PackedByteArray bytes = open_bytes(job->file);
    if (!bytes.is_empty()) {
        UtilityFunctions::print(String("[warm] bytes relative=") + job->relative
            + String(" size=") + String::num_int64(bytes.size()));
        if (job->jpg && !valid_jpeg(bytes)) {
            UtilityFunctions::printerr(String("[warm] invalid jpeg relative=") + job->relative
                + String(" size=") + String::num_int64(bytes.size()));
            job->done.store(true, std::memory_order_release);
            return;
        }
        Ref<Image> decoded;
        decoded.instantiate();
        Error err = OK;
        if (job->jpg) {
            std::lock_guard<std::mutex> lock(jpeg_decode_mutex);
            err = decoded->load_jpg_from_buffer(bytes);
        } else {
            err = decoded->load_png_from_buffer(bytes);
        }
        if (err == OK) {
            job->image = decoded;
            UtilityFunctions::print(String("[warm] decode ok relative=") + job->relative
                + String(" width=") + String::num_int64(decoded->get_width())
                + String(" height=") + String::num_int64(decoded->get_height()));
        } else {
            UtilityFunctions::printerr(String("[warm] decode failed relative=") + job->relative
                + String(" err=") + String::num_int64(err));
        }
    } else {
        UtilityFunctions::printerr(String("[warm] missing or empty relative=") + job->relative
            + String(" file=") + job->file);
    }
    job->done.store(true, std::memory_order_release);
}
}

String content::path(const String &relative) {
    return String("user://assets/") + relative;
}

Ref<Texture2D> content::image(const String &relative) {
    if (image_cache.count(relative)) {
        UtilityFunctions::print(String("[image] cache hit relative=") + relative);
        return image_cache.at(relative);
    }
    const String file = path(relative);
    UtilityFunctions::print(String("[image] request relative=") + relative
        + String(" file=") + ProjectSettings::get_singleton()->globalize_path(file));
    if (!FileAccess::file_exists(file)) {
        UtilityFunctions::printerr(String("[image] missing relative=") + relative + String(" file=") + file);
        failed_images[relative] = true;
        return Ref<Texture2D>();
    }
    Ref<Image> decoded;
    decoded.instantiate();
    PackedByteArray bytes = open_bytes(file);
    const bool jpg = relative.to_lower().ends_with(".jpg") || relative.to_lower().ends_with(".jpeg");
    UtilityFunctions::print(String("[image] bytes relative=") + relative
        + String(" size=") + String::num_int64(bytes.size())
        + String(" jpg=") + (jpg ? "true" : "false"));
    if (jpg && !valid_jpeg(bytes)) {
        UtilityFunctions::printerr(String("[image] invalid jpeg relative=") + relative
            + String(" ") + jpeg_summary(bytes));
        failed_images[relative] = true;
        return Ref<Texture2D>();
    }
    Error err = OK;
    UtilityFunctions::print(String("[image] decode begin relative=") + relative);
    if (jpg) {
        std::lock_guard<std::mutex> lock(jpeg_decode_mutex);
        err = decoded->load_jpg_from_buffer(bytes);
    } else {
        err = decoded->load_png_from_buffer(bytes);
    }
    if (err != OK) {
        if (jpg) {
#if defined(_WIN32)
            HRESULT wic_error = S_OK;
            Ref<Image> wic_image = decode_jpeg_with_wic(bytes, wic_error);
            if (wic_image.is_valid()) {
                decoded = wic_image;
                err = OK;
                UtilityFunctions::print(String("[image] WIC decode ok relative=") + relative
                    + String(" width=") + String::num_int64(decoded->get_width())
                    + String(" height=") + String::num_int64(decoded->get_height()));
            } else {
                UtilityFunctions::printerr(String("[image] WIC decode failed relative=") + relative
                    + String(" HRESULT=0x") + String::num_uint64(uint32_t(wic_error), 16));
            }
#endif
        }
        if (err != OK && jpg) {
            PackedByteArray compatible = jpeg_without_comments(bytes);
            if (!compatible.is_empty()) {
                UtilityFunctions::print(String("[image] retry without comments relative=") + relative
                    + String(" original_size=") + String::num_int64(bytes.size())
                    + String(" retry_size=") + String::num_int64(compatible.size()));
                Ref<Image> retry;
                retry.instantiate();
                Error retry_err = OK;
                {
                    std::lock_guard<std::mutex> lock(jpeg_decode_mutex);
                    retry_err = retry->load_jpg_from_buffer(compatible);
                }
                if (retry_err == OK) {
                    decoded = retry;
                    err = OK;
                    UtilityFunctions::print(String("[image] retry decode ok relative=") + relative
                        + String(" width=") + String::num_int64(decoded->get_width())
                        + String(" height=") + String::num_int64(decoded->get_height()));
                } else {
                    UtilityFunctions::printerr(String("[image] retry decode failed relative=") + relative
                        + String(" err=") + String::num_int64(retry_err));
                }
            }
        }
    }
    if (err != OK) {
        failed_images[relative] = true;
        UtilityFunctions::printerr(String("[image] decode failed relative=") + relative
            + String(" ") + (jpg ? jpeg_summary(bytes) : String("bytes=") + String::num_int64(bytes.size()))
            + String(" err=") + String::num_int64(err));
        return Ref<Texture2D>();
    }
    UtilityFunctions::print(String("[image] decode ok relative=") + relative
        + String(" width=") + String::num_int64(decoded->get_width())
        + String(" height=") + String::num_int64(decoded->get_height()));
    Ref<Texture2D> result = ImageTexture::create_from_image(decoded);
    if (result.is_valid()) {
        image_cache[relative] = result;
        UtilityFunctions::print(String("[image] texture ok relative=") + relative);
    } else {
        failed_images[relative] = true;
        UtilityFunctions::printerr(String("[image] texture creation failed relative=") + relative);
    }
    return result;
}

Ref<Texture2D> content::cached_image(const String &relative) {
    return image_cache.count(relative) ? image_cache.at(relative) : Ref<Texture2D>();
}

bool content::image_failed(const String &relative) {
    return failed_images.count(relative) && failed_images.at(relative);
}

void content::begin_gallery_images(const PackedStringArray &relative_paths) {
    for (const String &relative : relative_paths) {
        if (image_cache.count(relative) || image_failed(relative) || !FileAccess::file_exists(path(relative))) continue;
        bool already_queued = false;
        for (const auto &queued : gallery_live)
            if (queued->relative == relative) already_queued = true;
        if (already_queued) continue;
        auto job = std::make_unique<WarmJob>();
        job->relative = relative;
        job->file = path(relative);
        job->jpg = relative.ends_with(".jpg");
        WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
        if (!pool || pool->add_native_task(&warm_decode, job.get(), true, "Task gallery") < 0) continue;
        gallery_live.push_back(std::move(job));
    }
}

void content::step_gallery_images() {
    for (int i = int(gallery_live.size()) - 1; i >= 0; --i) {
        WarmJob *job = gallery_live[i].get();
        if (!job->done.load(std::memory_order_acquire)) continue;
        if (job->image.is_valid()) image_cache[job->relative] = ImageTexture::create_from_image(job->image);
        else failed_images[job->relative] = true;
        gallery_live.erase(gallery_live.begin() + i);
    }
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

PackedStringArray content::songs_with_rating_at_least(int rating) {
    const auto cached = song_rating_candidates.find(rating);
    if (cached != song_rating_candidates.end()) return cached->second;
    PackedStringArray result;
    const PackedByteArray bytes = open_bytes(path("songs/songlist"));
    const String text = bytes.is_empty() ? String() : String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size()).strip_edges();
    const Variant parsed = text.is_empty() ? Variant() : JSON::parse_string(text);
    Array songs;
    if (parsed.get_type() == Variant::DICTIONARY)
        songs = Dictionary(parsed).get("songs", Array());
    else if (parsed.get_type() == Variant::ARRAY)
        songs = parsed;
    else {
        song_rating_candidates[rating] = result;
        return result;
    }
    for (int i = 0; i < songs.size(); ++i) {
        if (songs[i].get_type() != Variant::DICTIONARY) continue;
        const Dictionary song = songs[i];
        bool eligible = false;
        std::function<void(const Variant &)> scan_rating = [&](const Variant &value) {
            if (eligible) return;
            if (value.get_type() == Variant::DICTIONARY) {
                const Dictionary dict = value;
                const Array keys = dict.keys();
                for (int key_index = 0; key_index < keys.size(); ++key_index) {
                    const Variant key = keys[key_index];
                    const String key_name = String(key).to_lower();
                    const Variant child = dict[key];
                    if (key_name == "rating" || key_name == "ratings" || key_name == "constant") {
                        if (child.get_type() == Variant::ARRAY) {
                            const Array entries = child;
                            for (int entry_index = 0; entry_index < entries.size(); ++entry_index)
                                scan_rating(entries[entry_index]);
                        } else if (child.get_type() == Variant::STRING) {
                            eligible = String(child).to_float() >= rating;
                        } else if (child.get_type() == Variant::INT || child.get_type() == Variant::FLOAT) {
                            eligible = double(child) >= rating;
                        }
                    } else if (child.get_type() == Variant::DICTIONARY || child.get_type() == Variant::ARRAY) {
                        scan_rating(child);
                    }
                    if (eligible) return;
                }
            } else if (value.get_type() == Variant::ARRAY) {
                const Array entries = value;
                for (int entry_index = 0; entry_index < entries.size(); ++entry_index)
                    scan_rating(entries[entry_index]);
            }
        };
        scan_rating(song);
        const String id = song.get("id", "");
        if (eligible && !id.is_empty()) result.push_back(id);
    }
    song_rating_candidates[rating] = result;
    return result;
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
