#include "client.hpp"
#include "content.hpp"
#include "asset_cipher.hpp"
#include "ui_font.hpp"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/tls_options.hpp>
#include <godot_cpp/classes/java_class.hpp>
#include <godot_cpp/classes/java_class_wrapper.hpp>
#include <godot_cpp/classes/java_object.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/progress_bar.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

using namespace godot;

namespace {
static constexpr float W = 1920, H = 1080;
static constexpr float STARTUP_CANVAS_H = 1440;
static constexpr const char *IMAGE = "res://assets/resources/img/";
static constexpr const char *ASSETS = "user://assets";
enum RequestKind { NONE = 0, AUTH_REFRESH, LOGIN, MANIFEST, DOWNLOAD };
enum ContentAction { DISMISS = 0, RETRY = 1, OPEN_SITE = 2 };

static Vector2 pos(float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    return Vector2(W / 2 + x - w * ax, H / 2 - y - h * (1 - ay));
}

static Ref<Texture2D> tex(const String &name) {
    return ResourceLoader::get_singleton()->load(IMAGE + name);
}

static TextureRect *sprite(Control *parent, const String &name, const String &file,
        float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    Ref<Texture2D> texture = tex(file);
    if (!texture.is_valid()) return nullptr;
    auto *node = memnew(TextureRect);
    node->set_name(name);
    parent->add_child(node);
    node->set_texture(texture);
    node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    node->set_stretch_mode(TextureRect::STRETCH_SCALE);
    node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    node->set_position(pos(x, y, w, h, ax, ay));
    node->set_size(Vector2(w, h));
    return node;
}

static Label *text(Control *parent, const String &name, const String &value,
        float x, float y, float w, int size, const Color &color, bool centered = false) {
    auto *label = memnew(Label);
    label->set_name(name);
    parent->add_child(label);
    label->set_position(pos(x, y, w, size + 12, centered ? .5f : 0));
    label->set_size(Vector2(w, size + 12));
    label->set_text(value);
    label->set_horizontal_alignment(centered ? HORIZONTAL_ALIGNMENT_CENTER : HORIZONTAL_ALIGNMENT_LEFT);
    label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
    label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    label->set_clip_text(false);
    label->add_theme_font_size_override("font_size", size);
    label->add_theme_color_override("font_color", color);
    Ref<Font> font = unfalsus_ui::prepare_font("VivoSans-Semibold-Full.ttf");
    if (font.is_valid()) label->add_theme_font_override("font", font);
    return label;
}

static float side_fit() { return 1440.f / 964.f; }
static float css_ease_out(float t) {
    float u = std::clamp(t, 0.f, 1.f);
    for (int i = 0; i < 5; ++i) {
        const float omt = 1.f - u;
        const float x = 1.74f * omt * u * u + u * u * u;
        const float dx = 3.48f * omt * u + 1.26f * u * u;
        if (dx < .0001f) break;
        u = std::clamp(u - (x - t) / dx, 0.f, 1.f);
    }
    const float omt = 1.f - u;
    return 3.f * omt * u * u + u * u * u;
}
static Vector2 csb_pos(float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    const float fit = side_fit();
    return Vector2(x * fit - w * ax, (720 - y) * fit - h * (1 - ay));
}

static TextureRect *csb_sprite(Control *parent, const char *name, const char *file,
        float x, float y, float w, float h, float asset_scale = 1, float ax = .5f, float ay = .5f) {
    const float width = w * asset_scale * side_fit();
    const float height = h * asset_scale * side_fit();
    auto *node = sprite(parent, name, String("startup/login/") + String(file) + String(".png"), 0, 0, width, height);
    if (node) node->set_position(csb_pos(x, y, width, height, ax, ay));
    return node;
}

static Label *csb_text(Control *parent, const char *name, const String &value,
        float x, float y, float width, float height, float font_size, const Color &fill,
        float ax = .5f, float ay = .5f, HorizontalAlignment align = HORIZONTAL_ALIGNMENT_CENTER) {
    auto *label = text(parent, name, value, 0, 0, width * side_fit(),
        int(font_size * side_fit()), fill);
    label->set_position(csb_pos(x, y, width * side_fit(), height * side_fit(), ax, ay));
    label->set_size(Vector2(width * side_fit(), height * side_fit()));
    label->set_horizontal_alignment(align);
    label->set_clip_text(false);
    return label;
}

static TextureButton *image_button(Control *parent, const char *name, const String &normal,
        const String &pressed, float x, float y, float width, float height) {
    auto *button = memnew(TextureButton);
    button->set_name(name);
    parent->add_child(button);
    button->set_texture_normal(tex(normal));
    button->set_texture_pressed(tex(pressed));
    button->set_ignore_texture_size(true);
    button->set_stretch_mode(TextureButton::STRETCH_SCALE);
    button->set_focus_mode(Control::FOCUS_NONE);
    button->set_size(Vector2(width, height));
    button->set_position(pos(x, y, width, height));
    return button;
}

static String setting_string(const String &name, const String &fallback = "") {
    return String(ProjectSettings::get_singleton()->get_setting(name, fallback)).strip_edges();
}

static Dictionary load_auth() {
    Ref<ConfigFile> config;
    config.instantiate();
    if (config->load("user://startup.cfg") != OK) return Dictionary();
    Dictionary result;
    result["token"] = config->get_value("auth", "token", "");
    result["user"] = config->get_value("auth", "user", "");
    result["password"] = config->get_value("auth", "password", "");
    return result;
}

static void save_auth(const String &token, const String &user, const String &password) {
    Ref<ConfigFile> config;
    config.instantiate();
    config->load("user://startup.cfg");
    config->set_value("auth", "token", token);
    config->set_value("auth", "user", user);
    config->set_value("auth", "password", password);
    config->save("user://startup.cfg");
}

static String basic_auth(const String &user, const String &password) {
    const PackedByteArray bytes = (user + String(":") + password).to_utf8_buffer();
    return String("Basic ") + Marshalls::get_singleton()->raw_to_base64(bytes);
}

static String bearer_auth(const String &token) {
    return token.begins_with("Bearer ") || token.begins_with("bearer ") ? token : String("Bearer ") + token;
}

static uint32_t crc32(const PackedByteArray &bytes) {
    static uint32_t table[256] = {};
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        ready = true;
    }
    uint32_t result = 0xFFFFFFFFu;
    for (int i = 0; i < bytes.size(); ++i) result = table[(result ^ bytes[i]) & 0xFF] ^ (result >> 8);
    return result ^ 0xFFFFFFFFu;
}

static String crc_hex(uint32_t value) { return String("%08x") % int64_t(value); }
static String join_path(const String &base, const String &part) { return base.trim_suffix("/") + "/" + part.trim_prefix("/"); }
static String body_text(const PackedByteArray &body) { return String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()); }

static bool json_text(const String &text) {
    const String trimmed = text.strip_edges();
    return trimmed.begins_with("{") || trimmed.begins_with("[");
}

static String asset_path(const Dictionary &item) {
    const String path = String(item.get("path", "")).replace("\\", "/").trim_prefix("/");
    if (path.is_empty() || path.begins_with("/") || path.contains(":") || path == ".."
            || path.begins_with("../") || path.contains("/../") || path.ends_with("/..")) return "";
    return path;
}

static int64_t file_size(const String &path) {
    if (!FileAccess::file_exists(path)) return -1;
    Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (!file.is_valid()) return -1;
    return file->get_length();
}

static Array manifest_file(const String &file) {
    if (!FileAccess::file_exists(file)) return Array();
    const String text = FileAccess::get_file_as_string(file);
    if (!json_text(text)) return Array();
    Variant parsed = JSON::parse_string(text);
    if (parsed.get_type() != Variant::DICTIONARY) return Array();
    Dictionary object = parsed;
    return object.get("assets", Array());
}

static void hide_ios_startup_video(ClientVideo *video) {
    // iOS video is now a Godot TextureRect, so startup overlays can render
    // above it without stopping the title background.
    (void)video;
}

static void remove_asset(const String &path) {
    if (FileAccess::file_exists(path))
        DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(path));
}

static bool android_companion_present() {
    if (OS::get_singleton()->get_name() != "Android") return true;
    JavaClassWrapper *wrapper = JavaClassWrapper::get_singleton();
    if (!wrapper) return false;
    Ref<JavaClass> thread = wrapper->wrap("android.app.ActivityThread");
    if (!thread.is_valid()) return false;
    Ref<JavaObject> app = thread->call("currentApplication");
    if (!app.is_valid() || wrapper->get_exception().is_valid()) return false;
    Ref<JavaObject> manager = app->call("getPackageManager");
    if (!manager.is_valid() || wrapper->get_exception().is_valid()) return false;
    for (const char *name : {"moe.ncp.unf", "moe.ncp.inf"}) {
        // PackageManager throws NameNotFoundException for a missing package.
        // Consume that exception before and after every individual lookup;
        // otherwise a failed first candidate can poison the second lookup.
        wrapper->get_exception();
        Ref<JavaObject> info = manager->call("getPackageInfo", String(name), 0);
        const bool failed = wrapper->get_exception().is_valid();
        UtilityFunctions::print("Android companion lookup ", String(name), ": ",
            info.is_valid() && !failed ? "installed" : "not found");
        if (info.is_valid() && !failed) return true;
    }
    UtilityFunctions::push_error("Android companion package not found: moe.ncp.unf or moe.ncp.inf");
    return false;
}

static String device_id() {
    Ref<ConfigFile> config;
    config.instantiate();
    config->load("user://startup.cfg");
    String id = String(config->get_value("device", "id", ""));
    if (!id.is_empty()) return id;
    Ref<Crypto> crypto;
    crypto.instantiate();
    PackedByteArray bytes = crypto->generate_random_bytes(16);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    id = "";
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) id += "-";
        id += String("%02x") % int64_t(bytes[i]);
    }
    config->set_value("device", "id", id);
    config->save("user://startup.cfg");
    return id;
}

static bool has_cjk(const String &text) {
    for (int i = 0; i < text.length(); ++i) {
        const char32_t code = text.unicode_at(i);
        if (code >= 0x3400 && code <= 0x9FFF) return true;
    }
    return false;
}

static String login_error_text(const Dictionary &object, int status, int code) {
    if (code == 104 && (status == 200 || status == 403)) return U"用户名或密码错误";
    const String message = String(object.get("message", object.get("error", object.get("error_msg", "")))).strip_edges();
    if (has_cjk(message)) return message;
    if (code == 2) return U"网络目前正在维护中。";
    if (code == 5) return U"请将 Online 更新至最新版本。";
    if (code == 9) return U"此版本正在准备发布。\n请几分钟后再查看。";
    if (status == 400 || status == 401) return U"用户名或密码错误";
    if (status == 403) return U"无权访问 Online";
    if (status == 404 || status >= 500) return U"Online 服务暂时不可用";
    return status ? String(U"登录失败（") + String::num_int64(status) + String(U"）") : String(U"登录失败");
}

static bool cellular_network() {
    const String os = OS::get_singleton()->get_name();
    if (os == "iOS") {
        Engine *engine = Engine::get_singleton();
        if (!engine->has_singleton("UnFalsusVideo")) return false;
        Object *plugin = engine->get_singleton("UnFalsusVideo");
        return plugin && bool(plugin->call("is_cellular"));
    }
    if (os != "Android") return false;
    JavaClassWrapper *wrapper = JavaClassWrapper::get_singleton();
    if (!wrapper) return false;
    Ref<JavaClass> thread = wrapper->wrap("android.app.ActivityThread");
    if (!thread.is_valid()) return false;
    Ref<JavaObject> app = thread->call("currentApplication");
    if (!app.is_valid() || wrapper->get_exception().is_valid()) return false;
    Ref<JavaObject> manager = app->call("getSystemService", String("connectivity"));
    if (!manager.is_valid() || wrapper->get_exception().is_valid()) return false;
    Ref<JavaObject> info = manager->call("getActiveNetworkInfo");
    if (!info.is_valid() || wrapper->get_exception().is_valid()) return false;
    return int(info->call("getType")) == 0;
}

static int transport_tries = 0;

static Dictionary &catalog_same() {
    static Dictionary value;
    return value;
}

static Dictionary json_dictionary(const PackedByteArray &body) {
    const String text = body_text(body);
    if (!json_text(text)) return Dictionary();
    const Variant parsed = JSON::parse_string(text);
    return parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
}

static String traffic_text(int64_t bytes) {
    const double kb = double(std::max<int64_t>(0, bytes)) / 1024.0;
    if (kb < 1024.0) return String::num(kb, 2) + String(" KB");
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return String::num(mb, 2) + String(" MB");
    return String::num(mb / 1024.0, 2) + String(" GB");
}
}

void ClientScreen::startup_step_warmup() {
    if (startup_preload_index == 0 && video) video->prepare("res://assets/resources/video/startup/intro.mp4", false);
    else if (startup_preload_index == 1 && title_video) {
        const bool intro_ready = video && video->is_prepared();
        const bool title_ready = title_video->prepare("res://assets/resources/video/startup/title.mp4", true);
        startup_media_prepared = intro_ready;
        if (!intro_ready)
            UtilityFunctions::push_error("Startup intro hardware decoder could not be prepared");
        if (!title_ready)
            UtilityFunctions::push_warning("Startup title hardware decoder could not be prepared; it will retry at transition");
    } else if (startup_preload_index < 6) {
        const char *files[] = {
            "res://assets/resources/img/startup/1080/title.png",
            "res://assets/resources/img/startup/activity_icon.png",
            "res://assets/resources/audio/bgm/title_full.ogg",
            "res://assets/resources/audio/bgm/title_loop.ogg",
        };
        ResourceLoader::get_singleton()->load(files[startup_preload_index - 2]);
    }
    if (startup_preload_index < 6) ++startup_preload_index;
    const bool images = content::warmup_partner_assets_step();
    if (!startup_warmup_done) startup_warmup_done = images && startup_preload_index >= 6 && startup_media_prepared;
}

void ClientScreen::startup_begin_local_sweep() {
    startup_local_sweep.clear();
    startup_local_sweep_index = 0;
    startup_sweep_done = false;
    const Array entries = manifest_file(join_path(ASSETS, "meta.assets"));
    for (int i = 0; i < entries.size(); ++i) startup_local_sweep.push_back(entries[i]);
    remove_asset(join_path(ASSETS, "meta_new.assets"));
    if (startup_local_sweep.is_empty()) startup_sweep_done = true;
}

void ClientScreen::startup_step_local_sweep() {
    if (startup_sweep_done) return;
    const int end = std::min(startup_local_sweep_index + 64, int(startup_local_sweep.size()));
    for (; startup_local_sweep_index < end; ++startup_local_sweep_index) {
        const Dictionary item = startup_local_sweep[startup_local_sweep_index];
        const String path = asset_path(item);
        if (path.is_empty()) continue;
        const String file = join_path(ASSETS, path);
        const int64_t expected = int64_t(item.get("length", 0));
        const int64_t actual = file_size(file);
        if (actual >= 0 && expected > 0 && actual != expected) remove_asset(file);
    }
    if (startup_local_sweep_index >= startup_local_sweep.size()) {
        startup_local_sweep.clear();
        startup_sweep_done = true;
    }
}

void ClientScreen::startup_start_content_check() {
    if (startup_content_started || scene_kind != 0) return;
    startup_content_started = true;
    startup_content_waiting = false;
    if (!android_companion_present()) {
        startup_show_error(-12, 0);
        return;
    }
    if (startup_base_url.is_empty()) {
        content_ready = true;
        return;
    }
    const Dictionary auth = load_auth();
    startup_auth_token = String(auth.get("token", ""));
    const String user = String(auth.get("user", ""));
    const String password = String(auth.get("password", ""));
    if (user.is_empty() || password.is_empty()) {
        startup_show_login();
        return;
    }
    transport_tries = 0;
    startup_request_kind = AUTH_REFRESH;
    PackedStringArray headers;
    headers.push_back("AppVersion: 1.0.0");
    headers.push_back("Authorization: " + basic_auth(user, password));
    headers.push_back("Content-Type: application/x-www-form-urlencoded");
    headers.push_back("grant_type: password");
    if (startup_http->request(join_path(startup_base_url, "auth/online"), headers, HTTPClient::METHOD_POST,
            "username=" + user.uri_encode() + "&password=" + password.uri_encode()) != OK) {
        startup_request_kind = NONE;
        startup_set_busy(false);
        if (startup_error_label) startup_error_label->set_text(U"无法连接至服务器");
    }
}

void ClientScreen::startup_fetch_manifest() {
    startup_request_kind = MANIFEST;
    startup_http->set_download_file("");
    startup_http->set_timeout(15);
    PackedStringArray headers;
    headers.push_back("AppVersion: 1.0.0");
    headers.push_back("DeviceId: " + device_id());
    startup_activity_wait = true;
    startup_activity_delay = 0;
    if (!startup_auth_token.is_empty()) headers.push_back("Authorization: " + bearer_auth(startup_auth_token));
    headers.push_back("Accept: application/json");
    headers.push_back("Cache-Control: no-cache");
    headers.push_back("Pragma: no-cache");
    const Error started = startup_http->request(join_path(startup_base_url, "game/current_assets?_="
        + String::num_int64(Time::get_singleton()->get_ticks_msec())), headers);
    if (started != OK) {
        startup_request_kind = NONE;
        startup_show_error(-30, 0);
    }
}

void ClientScreen::startup_request_completed(int result, int response_code,
        const PackedStringArray &, const PackedByteArray &body) {
    const int request = startup_request_kind;
    startup_request_kind = NONE;
    startup_activity_wait = false;
    if (request == MANIFEST && startup_busy && !startup_login_panel) startup_set_busy(false);
    if (scene_kind != 0 || request == NONE) return;
    if (result != HTTPRequest::RESULT_SUCCESS || response_code < 200 || response_code >= 300) {
        if (result != HTTPRequest::RESULT_SUCCESS && transport_tries < 3 && startup_http
                && (request == MANIFEST || request == LOGIN || request == AUTH_REFRESH)) {
            ++transport_tries;
            if (request == MANIFEST) {
                startup_fetch_manifest();
                return;
            }
            String user;
            String password;
            if (request == LOGIN && startup_user_field && startup_pass_field) {
                user = startup_user_field->get_text().strip_edges();
                password = startup_pass_field->get_text();
            } else {
                const Dictionary auth = load_auth();
                user = String(auth.get("user", ""));
                password = String(auth.get("password", ""));
            }
            if (!user.is_empty() && !password.is_empty()) {
                startup_request_kind = request;
                PackedStringArray headers;
                headers.push_back("AppVersion: 1.0.0");
                headers.push_back("Authorization: " + basic_auth(user, password));
                headers.push_back("Content-Type: application/x-www-form-urlencoded");
                headers.push_back("grant_type: password");
                if (startup_http->request(join_path(startup_base_url, "auth/online"), headers, HTTPClient::METHOD_POST,
                        "username=" + user.uri_encode() + "&password=" + password.uri_encode()) != OK) {
                    startup_request_kind = NONE;
                    startup_set_busy(false);
                    startup_show_error(-30, 0);
                }
                return;
            }
        }
        transport_tries = 0;
        if (result != HTTPRequest::RESULT_SUCCESS) {
            startup_set_busy(false);
            if (request == LOGIN) {
                if (startup_error_label) startup_error_label->set_text(U"无法连接至服务器");
            } else startup_show_error(-30, 0);
            return;
        }
        const Dictionary object = json_dictionary(body);
        if (request == AUTH_REFRESH) startup_show_login();
        else if (request == LOGIN) {
            startup_set_busy(false);
            const int code = int(object.get("error_code", -3));
            if (startup_error_label) startup_error_label->set_text(login_error_text(object, response_code, code));
        } else startup_show_error(int(object.get("error_code", -3)), response_code);
        return;
    }
    transport_tries = 0;
    const Dictionary object = json_dictionary(body);
    const Variant parsed = object;
    if (object.has("success") && !bool(object["success"])) {
        startup_set_busy(false);
        if (request == LOGIN) {
            if (startup_error_label) startup_error_label->set_text(
                login_error_text(object, response_code, int(object.get("error_code", -3))));
        } else startup_show_error(-31, response_code);
        return;
    }
    if (request == AUTH_REFRESH || request == LOGIN) {
        String token = String(object.get("access_token", object.get("token", object.get("accessToken", ""))));
        if (token.is_empty() && object.get("value", Variant()).get_type() == Variant::DICTIONARY) {
            const Dictionary nested = object.get("value", Dictionary());
            token = String(nested.get("access_token", nested.get("token", nested.get("accessToken", ""))));
        }
        if (token.is_empty() || !bool(object.get("success", true))) {
            if (request == AUTH_REFRESH) startup_show_login();
            else {
                startup_set_busy(false);
                if (startup_error_label) startup_error_label->set_text(
                    login_error_text(object, response_code, int(object.get("error_code", -3))));
            }
            return;
        }
        startup_auth_token = token.trim_prefix("Bearer ").trim_prefix("bearer ");
        if (request == AUTH_REFRESH) {
            const Dictionary auth = load_auth();
            save_auth(startup_auth_token, String(auth.get("user", "")), String(auth.get("password", "")));
        }
        if (request == LOGIN) {
            save_auth(startup_auth_token, startup_user_field->get_text().strip_edges(), startup_pass_field->get_text());
            play_sfx("sfx/10_TimelineSpecialNodeClick.ogg");
            startup_set_busy(false);
            startup_content_phase = 1;
            startup_overlay_closing = true;
            startup_overlay_seconds = 0;
            return;
        }
        startup_fetch_manifest();
        return;
    }
    if (parsed.get_type() != Variant::DICTIONARY || !object.has("assets") || !bool(object.get("success", true))) {
        startup_show_error(int(object.get("error_code", -3)), response_code);
        return;
    }
    startup_manifest_text = body_text(body);
    const Variant list = object.get("assets", Variant());
    if (list.get_type() != Variant::ARRAY) { startup_show_error(-3, response_code); return; }
    startup_manifest = Array(list);
    startup_missing.clear();
    const String assets_dir = ASSETS;
    const String global_assets = ProjectSettings::get_singleton()->globalize_path(assets_dir);
    DirAccess::make_dir_recursive_absolute(global_assets);
    startup_committed = manifest_file(join_path(assets_dir, "meta.assets"));
    Ref<FileAccess> saved = FileAccess::open(join_path(assets_dir, "meta_new.assets"), FileAccess::WRITE);
    if (saved.is_valid()) saved->store_string(startup_manifest_text);
    startup_next_by_path.clear();
    for (int i = 0; i < startup_manifest.size(); ++i) {
        const Dictionary item = startup_manifest[i];
        const String path = asset_path(item);
        if (!path.is_empty()) startup_next_by_path[path] = item;
    }
    startup_manifest_bytes_total = 0;
    catalog_same().clear();
    startup_diff_index = 0;
    startup_diff_phase = startup_committed.is_empty() ? 2 : 1;
}

void ClientScreen::startup_step_manifest_diff() {
    if (startup_diff_phase == 0) return;
    const String assets_dir = ASSETS;
    const int batch = 64;
    if (startup_diff_phase == 1) {
        const int end = std::min(startup_diff_index + batch, int(startup_committed.size()));
        for (; startup_diff_index < end; ++startup_diff_index) {
            const Dictionary old = startup_committed[startup_diff_index];
            const String previous = asset_path(old);
            if (previous.is_empty()) continue;
            bool unchanged = false;
            const Variant found = startup_next_by_path.get(previous, Variant());
            if (found.get_type() == Variant::DICTIONARY) {
                const Dictionary next = found;
                unchanged = old.get("length", 0) == next.get("length", 0)
                    && String(old.get("crc32", "")) == String(next.get("crc32", ""));
            }
            if (unchanged) catalog_same()[previous] = true;
        }
        if (startup_diff_index < startup_committed.size()) return;
        startup_diff_phase = 2;
        startup_diff_index = 0;
    }
    if (startup_diff_phase == 2) {
        const int end = std::min(startup_diff_index + batch, int(startup_manifest.size()));
        for (; startup_diff_index < end; ++startup_diff_index) {
            const Dictionary item = startup_manifest[startup_diff_index];
            const String path = asset_path(item);
            if (path.is_empty()) continue;
            const String file = join_path(assets_dir, path);
            const bool same = bool(catalog_same().get(path, false));
            if (!same || !FileAccess::file_exists(file)) {
                startup_missing.push_back(item);
                startup_manifest_bytes_total += std::max<int64_t>(0, int64_t(item.get("length", 0)));
            }
        }
        if (startup_diff_index < startup_manifest.size()) return;
        startup_diff_phase = 3;
    }
    startup_diff_phase = 0;
    startup_committed.clear();
    startup_next_by_path.clear();
    startup_manifest_total = startup_missing.size();
    startup_manifest_index = 0;
    startup_manifest_bytes_done = 0;
    startup_verify_round = 0;
    if (startup_missing.is_empty()) {
        startup_verify_index = startup_manifest.size();
        startup_clear_overlay();
        content_ready = true;
    } else if (startup_manifest_bytes_total >= 10 * 1024 * 1024) {
        const String size_line = (cellular_network() ? String(U"使用移动数据下载预计消耗流量：")
            : String(U"预计下载大小：")) + traffic_text(startup_manifest_bytes_total);
        startup_show_content_dialog(true, String(U" 有新资源需要更新！\n更新数量：")
            + String::num_int64(startup_manifest_total) + String("\n") + size_line);
    } else {
        startup_start_downloads();
    }
}

void ClientScreen::startup_show_error(int code, int status) {
    startup_error_code = code;
    startup_error_status = status;
    startup_error_action = RETRY;
    String body;
    if (code == -30) body = U"无法连接至服务器";
    else if (code == -31) body = U"发生了未知错误";
    else if (code == -12) { startup_error_action = DISMISS; body = U" Online 需搭配 Un Falsus 使用。\n 请先安装 Un Falsus 后再使用 Online。"; }
    else if (code == 2) { startup_error_action = DISMISS; body = U"网络目前正在维护中。"; }
    else if (code == 5) { startup_error_action = OPEN_SITE; body = U"请将 Online 更新至最新版本。"; }
    else if (code == 9) { startup_error_action = DISMISS; body = U"此版本正在准备发布。\n请几分钟后再查看。"; }
    else if (status == 401) body = String(U"登录状态已过期。\n请重新登录。错误代码：") + String::num_int64(code);
    else if (status == 403) { startup_error_action = DISMISS; body = String(U"无权访问最新内容。\n错误代码：") + String::num_int64(code); }
    else if (status >= 500 && status < 600) body = String(U"内容服务暂时不可用。\n请稍后再试。错误代码：") + String::num_int64(code);
    else body = String(U"获取最新内容信息失败。\n请检查网络连接。错误代码：") + String::num_int64(code);
    startup_show_content_dialog(false, body);
}

void ClientScreen::startup_show_login() {
    hide_ios_startup_video(video);
    startup_clear_overlay();
    startup_overlay = memnew(Control);
    startup_overlay->set_name("StartupLoginOverlay");
    stage->add_child(startup_overlay);
    startup_overlay->set_z_index(20);
    startup_overlay->set_size(Vector2(W, 1440));
    startup_overlay->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *dim = memnew(ColorRect);
    dim->set_name("Dimmer");
    startup_overlay->add_child(dim);
    dim->set_color(Color(0, 0, 0));
    dim->set_modulate(Color(1, 1, 1, 0));
    dim->set_size(Vector2(W, 1440));
    dim->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    startup_login_panel = memnew(Control);
    startup_login_panel->set_name("LoginPanel");
    startup_overlay->add_child(startup_login_panel);
    startup_login_panel->set_size(Vector2(W, 1440));
    startup_login_panel->set_position(Vector2(-100 * side_fit() - design_extra * .5f, 0));
    startup_login_panel->set_modulate(Color(1, 1, 1, 0));
    csb_sprite(startup_login_panel, "SideBottom", "sidedialog-bottom", 605.331f, 720, 219, 541, 1, .5f, 1);
    csb_sprite(startup_login_panel, "SideTop", "sidedialog-top", 0, 720, 756, 964, 1, 0, 1);
    csb_text(startup_login_panel, "LoginTitle", U"Login", 402.5f, 551.798f, 120, 61, 42,
        Color(1, 1, 1), 1, .5f, HORIZONTAL_ALIGNMENT_RIGHT);
    csb_sprite(startup_login_panel, "TitleDash", "title-float", 402.5f, 522.56f, 351, 3, 2.f / 3, 1, .5f);
    csb_text(startup_login_panel, "UserLabel", U"用户名", 430.52f, 462.99f, 96, 28, 18,
        Color(.95f, .95f, .95f), 1, 0, HORIZONTAL_ALIGNMENT_RIGHT);
    csb_sprite(startup_login_panel, "UserUnderline", "oauth-title-underline", 311.9f, 461.9f, 351, 2, 2.f / 3);
    csb_sprite(startup_login_panel, "UserBox", "oauth-textbox", 248.68f, 437.92f, 565, 52, 2.f / 3);
    csb_text(startup_login_panel, "PassLabel", U"密码", 432.34f, 380.45f, 91, 28, 18,
        Color(.95f, .95f, .95f), 1, 0, HORIZONTAL_ALIGNMENT_RIGHT);
    csb_sprite(startup_login_panel, "PassUnderline", "oauth-title-underline", 313.72f, 379.36f, 351, 2, 2.f / 3);
    csb_sprite(startup_login_panel, "PassBox", "oauth-textbox", 248.68f, 356.66f, 565, 52, 2.f / 3);
    auto field = [&](const char *name, const String &placeholder, float x, float y, float width, bool secret) {
        auto *edit = memnew(LineEdit);
        edit->set_name(name);
        startup_login_panel->add_child(edit);
        edit->set_position(csb_pos(x, y + 1.5f, width * side_fit(), 35 * side_fit(), 0, .5f));
        edit->set_size(Vector2(width * side_fit(), 35 * side_fit()));
        edit->set_placeholder(placeholder);
        edit->set_secret(secret);
        edit->set_max_length(64);
        edit->add_theme_font_size_override("font_size", int(20 * side_fit()));
        edit->add_theme_color_override("font_color", Color(1, 1, 1));
        edit->add_theme_color_override("font_placeholder_color", Color(180.f / 255.f, 180.f / 255.f, 180.f / 255.f));
        edit->add_theme_color_override("caret_color", Color(1, 1, 1));
        Ref<StyleBoxEmpty> empty;
        empty.instantiate();
        edit->add_theme_stylebox_override("normal", empty);
        edit->add_theme_stylebox_override("focus", empty);
        edit->add_theme_stylebox_override("read_only", empty);
        Ref<Font> regular = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
        if (regular.is_valid()) edit->add_theme_font_override("font", regular);
        return edit;
    };
    const Dictionary auth = load_auth();
    startup_user_field = field("UserField", "Email, ID or Username", 86.14f, 435, 320, false);
    startup_pass_field = field("PassField", "Password", 86.14f, 354, 300, true);
    startup_user_field->set_text(String(auth.get("user", "")));
    startup_pass_field->set_text(String(auth.get("password", "")));
    startup_dimmer_to = -1;
    if (auto *user_label = Object::cast_to<Label>(startup_login_panel->get_node_or_null(NodePath("UserLabel"))))
        user_label->set_vertical_alignment(VERTICAL_ALIGNMENT_BOTTOM);
    if (auto *pass_label = Object::cast_to<Label>(startup_login_panel->get_node_or_null(NodePath("PassLabel"))))
        pass_label->set_vertical_alignment(VERTICAL_ALIGNMENT_BOTTOM);
    startup_error_label = csb_text(startup_login_panel, "Error", "", 69.035f, 335.33f, 380, 28, 18,
        Color(1.f, .18f, .18f), 0, 1, HORIZONTAL_ALIGNMENT_LEFT);
    startup_error_label->add_theme_color_override("font_color", Color(1.f, .18f, .18f));
    startup_error_label->set_vertical_alignment(VERTICAL_ALIGNMENT_TOP);
    const float button_w = 492 * .6f * side_fit(), button_h = 150 * .6f * side_fit();
    auto *button = image_button(startup_login_panel, "LoginButton", "startup/login/main-button.png",
        "startup/login/main-button-pressed.png", 0, 0, button_w, button_h);
    button->set_texture_disabled(tex("startup/login/main-button-disabled.png"));
    button->set_position(csb_pos(243.116f, 255.654f, button_w, button_h));
    startup_login_button = button;
    auto *caption = text(button, "Caption", U"登录", 0, 0, button_w, int(50 * .6f * side_fit()), Color(1, 1, 1), true);
    caption->set_position(Vector2(0, (button_h - caption->get_size().y) / 2));
    startup_login_button->connect("pressed", Callable(this, "startup_submit_login"));
    startup_overlay_seconds = 0;
    startup_content_phase = 0;
    layout();
    play_sfx("sfx/25_VNIMText.ogg");
}

void ClientScreen::startup_submit_login() {
    if (startup_busy || !startup_user_field || !startup_pass_field) return;
    transport_tries = 0;
    const String user = startup_user_field->get_text().strip_edges();
    const String password = startup_pass_field->get_text();
    play_sfx("sfx/06_LesserButtonClickV2.ogg");
    if (user.is_empty() || password.is_empty()) {
        if (startup_error_label) startup_error_label->set_text(U"请输入用户名和密码");
        return;
    }
    startup_set_busy(true);
    startup_request_kind = LOGIN;
    PackedStringArray headers;
    headers.push_back("AppVersion: 1.0.0");
    headers.push_back("Authorization: " + basic_auth(user, password));
    headers.push_back("Content-Type: application/x-www-form-urlencoded");
    headers.push_back("grant_type: password");
    if (startup_http->request(join_path(startup_base_url, "auth/online"), headers, HTTPClient::METHOD_POST,
            "username=" + user.uri_encode() + "&password=" + password.uri_encode()) != OK) {
        startup_request_kind = NONE;
        startup_set_busy(false);
        startup_show_error(-30, 0);
    }
}

void ClientScreen::startup_show_content_dialog(bool download, const String &body) {
    hide_ios_startup_video(video);
    startup_dialog_download = download;
    startup_clear_overlay();
    startup_overlay = memnew(Control);
    startup_overlay->set_name("StartupContentDialog");
    stage->add_child(startup_overlay);
    startup_overlay->set_size(Vector2(W, H));
    startup_overlay->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *dim = memnew(ColorRect);
    dim->set_name("Dimmer");
    startup_overlay->add_child(dim);
    dim->set_color(Color(0, 0, 0, 0));
    dim->set_size(Vector2(W, H));
    auto *panel = memnew(Control);
    startup_overlay->add_child(panel);
    panel->set_name("DialogPanel");
    panel->set_size(Vector2(W, H));
    panel->set_pivot_offset(Vector2(W / 2, H / 2));
    panel->set_scale(Vector2(1.1f, 1.1f));
    const float fit = 1.5f;
    sprite(panel, "Backing", "startup/download/1080/download-backing.png", 0, 0, 2560, 663);
    text(panel, "Title", download ? String(U"文件更新") : String(U"错误"),
        0, 108 * fit, 900 * fit, int(24 * fit), Color(0xEB / 255.f, 0xEB / 255.f, 0xEB / 255.f), true);
    startup_status_label = text(panel, "Body", body, 0, 0, 820 * fit, int(24 * fit),
        Color(0xF2 / 255.f, 0xF2 / 255.f, 0xF2 / 255.f), true);
    startup_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    startup_status_label->set_size(Vector2(820 * fit, 140 * fit));
    startup_status_label->set_position(pos(0, 0, 820 * fit, 140 * fit));
    startup_status_label->add_theme_constant_override("outline_size", 2);
    startup_status_label->add_theme_color_override("font_outline_color", Color(0x25 / 255.f, 0x2B / 255.f, 0x32 / 255.f));
    Ref<Font> regular = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
    if (regular.is_valid()) startup_status_label->add_theme_font_override("font", regular);
    const float button_y = -100 * fit;
    auto *confirm = image_button(panel, "Confirm", "startup/download/1080/confirm-button.png",
        "startup/download/1080/confirm-button-pressed.png", download ? -250 * fit : 0, button_y, 404, 108);
    startup_confirm_button = confirm;
    auto *confirm_caption = text(confirm, "Caption", download ? String(U"下载")
        : startup_error_action == OPEN_SITE ? String(U"打开网站") : String(U"确定"),
        0, 0, 404, int(24 * fit), Color(1, 1, 1), true);
    confirm_caption->set_position(Vector2(0, 0));
    confirm_caption->set_size(Vector2(404, 108));
    confirm_caption->add_theme_constant_override("outline_size", 1);
    confirm_caption->add_theme_color_override("font_outline_color", Color(0x46 / 255.f, 0x46 / 255.f, 0x46 / 255.f));
    startup_confirm_button->connect("pressed", Callable(this, "startup_confirm_content"));
    if (download) {
        auto *cancel = image_button(panel, "Cancel", "startup/download/1080/cancel-button.png",
            "startup/download/1080/cancel-button-pressed.png", 250 * fit, button_y, 385, 108);
        startup_cancel_button = cancel;
        auto *cancel_caption = text(cancel, "Caption", U"取消", 0, 0, 385, int(24 * fit), Color(1, 1, 1), true);
        cancel_caption->set_position(Vector2(0, 0));
        cancel_caption->set_size(Vector2(385, 108));
        cancel_caption->add_theme_constant_override("outline_size", 1);
        cancel_caption->add_theme_color_override("font_outline_color", Color(0x46 / 255.f, 0x46 / 255.f, 0x46 / 255.f));
        startup_cancel_button->connect("pressed", Callable(this, "startup_cancel_content"));
    }
    startup_overlay_seconds = 0;
    startup_content_phase = 0;
    startup_overlay->set_modulate(Color(1, 1, 1, 0));
    layout();
}

void ClientScreen::startup_confirm_content() {
    if (startup_busy || startup_overlay_closing) return;
    startup_content_phase = 2;
    startup_overlay_closing = true;
    startup_overlay_seconds = 0;
}

void ClientScreen::startup_cancel_content() {
    if (startup_overlay_closing) return;
    startup_content_phase = 3;
    startup_overlay_closing = true;
    startup_overlay_seconds = 0;
}

void ClientScreen::startup_start_downloads() {
    hide_ios_startup_video(video);
    startup_dialog_download = false;
    startup_clear_overlay();
    startup_manifest_index = 0;
    startup_manifest_done = 0;
    startup_manifest_bytes_done = 0;
    startup_manifest_bytes_total = 0;
    for (int i = 0; i < startup_missing.size(); ++i)
        startup_manifest_bytes_total += std::max<int64_t>(0, int64_t(Dictionary(startup_missing[i]).get("length", 0)));
    startup_overlay = memnew(Control);
    startup_overlay->set_name("StartupDownloadOverlay");
    stage->add_child(startup_overlay);
    startup_overlay->set_size(Vector2(W, H));
    startup_overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    auto *catcher = memnew(ColorRect);
    catcher->set_name("InputCatcher");
    startup_overlay->add_child(catcher);
    catcher->set_color(Color(0, 0, 0, 0));
    catcher->set_size(Vector2(W, H));
    catcher->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    const float w = 1673, y = 37.5f, edge = (W - w) / 2;
    auto *shadow = sprite(startup_overlay, "Shadow", "startup/download/1080/bar-shadow.png",
        0, H / 2 - 441 / 2, W, 441);
    if (shadow) shadow->set_position(Vector2(0, 0));
    auto bar = [&](const char *name, const char *file, float cx, float cy, float width, float height) {
        auto *node = sprite(startup_overlay, name, String("startup/download/1080/") + file + ".png",
            0, 0, width, height);
        if (node) node->set_position(Vector2(cx - width / 2, cy - height / 2));
        return node;
    };
    bar("Empty", "bar-empty", W / 2, y, w, 11);
    auto *clip = memnew(Control);
    clip->set_name("FullClip");
    startup_overlay->add_child(clip);
    clip->set_position(Vector2(edge, y - 11 / 2));
    clip->set_size(Vector2(0, 11));
    clip->set_clip_contents(true);
    auto *full = sprite(clip, "Full", "startup/download/1080/bar-full.png", 0, 0, w, 11);
    if (full) full->set_position(Vector2(0, 0));
    startup_progress = clip;
    startup_download_fill = clip;
    startup_download_diamond = bar("Diamond", "bar-progress-diamond", edge, y, 34, 34);
    auto *tail_clip = memnew(Control);
    tail_clip->set_name("TailClip");
    startup_overlay->add_child(tail_clip);
    tail_clip->set_clip_contents(true);
    tail_clip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    tail_clip->set_visible(false);
    auto *tail = memnew(TextureRect);
    tail_clip->add_child(tail);
    tail->set_texture(tex("startup/download/1080/bar-progress-tail.png"));
    tail->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    tail->set_stretch_mode(TextureRect::STRETCH_SCALE);
    tail->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    tail->set_size(Vector2(116, 11));
    startup_download_tail = tail_clip;
    bar("AccentL", "bar-accent", edge - 31.5f, y, 33, 33);
    bar("AccentR", "bar-accent", W - edge + 31.5f, y, 33, 33);
    bar("UnderAccent", "bar-under-accent", W / 2, 82.5f, 477, 106);
    Ref<Font> bar_font = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
    const Color bar_outline(140.f / 255.f, 140.f / 255.f, 140.f / 255.f, 100.f / 255.f);
    startup_size_label = text(startup_overlay, "Size", "", 0, 0, 600, 24, Color(1, 1, 1));
    startup_size_label->set_position(Vector2(edge - 7.5f, 33));
    startup_size_label->set_size(Vector2(600, 54));
    startup_count_label = text(startup_overlay, "Count", "0/" + String::num_int64(startup_manifest_total),
        0, 0, 420, 24, Color(1, 1, 1));
    startup_count_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
    startup_count_label->set_position(Vector2(W - edge + 7.5f - 420, 33));
    startup_count_label->set_size(Vector2(420, 54));
    for (Label *label : {startup_size_label, startup_count_label}) {
        if (bar_font.is_valid()) label->add_theme_font_override("font", bar_font);
        label->set_clip_text(false);
        label->add_theme_constant_override("outline_size", 1);
        label->add_theme_color_override("font_outline_color", bar_outline);
    }
    startup_overlay->set_modulate(Color(1, 1, 1, 0));
    startup_overlay_seconds = 0;
    startup_status_label = nullptr;
    startup_set_busy(true);
    startup_download_attempt = 0;
    layout();
    startup_download_next();
}

namespace {
struct SealJob {
    int index = -1;
    int attempt = 0;
    int64_t length = 0;
    String crc;
    String temp;
    String dest;
    bool ok = false;
    bool storage = false;
    std::atomic<bool> done{false};
    int64_t task = -1;
};

struct DownloadRetry {
    int index = 0;
    int attempt = 0;
};

std::vector<std::unique_ptr<SealJob>> seal_pending;
std::vector<std::unique_ptr<SealJob>> seal_live;
std::vector<DownloadRetry> download_retries;
int64_t seal_bytes = 0;
int64_t shown_bytes = 0;
bool slot_ignore[16] = {};
bool committed_loaded = false;

Array &committed_assets() {
    static Array value;
    return value;
}

void remember_asset(const Dictionary &item) {
    const String path = asset_path(item);
    if (path.is_empty()) return;
    if (!committed_loaded) {
        committed_assets() = manifest_file(join_path(ASSETS, "meta.assets"));
        committed_loaded = true;
    }
    bool replaced = false;
    for (int i = 0; i < committed_assets().size(); ++i) {
        if (asset_path(Dictionary(committed_assets()[i])) == path) {
            committed_assets()[i] = item;
            replaced = true;
            break;
        }
    }
    if (!replaced) committed_assets().push_back(item);
    Dictionary root;
    root["assets"] = committed_assets();
    Ref<FileAccess> out = FileAccess::open(join_path(ASSETS, "meta.assets"), FileAccess::WRITE);
    if (out.is_valid()) out->store_string(JSON::stringify(root));
}

void seal_task(void *userdata) {
    SealJob *job = static_cast<SealJob *>(userdata);
    PackedByteArray bytes = FileAccess::get_file_as_bytes(job->temp);
    bool ok = !bytes.is_empty();
    bool storage = false;
    if (ok) {
        asset_cipher::apply(bytes);
        if (crc_hex(crc32(bytes)) != job->crc) ok = false;
        else {
            Ref<FileAccess> out = FileAccess::open(job->dest, FileAccess::WRITE);
            if (!out.is_valid() || !out->store_buffer(bytes)) {
                ok = false;
                storage = true;
            }
        }
    }
    job->ok = ok;
    job->storage = storage;
    job->done.store(true, std::memory_order_release);
}

struct FetchJob {
    std::string host;
    std::string path;
    std::string temp;
    std::vector<std::string> headers;
    int port = 80;
    bool tls = false;
    double timeout = 60;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<int64_t> got{0};
    int result = HTTPRequest::RESULT_CANT_CONNECT;
    int code = 0;
    std::thread thread;
};

std::unique_ptr<FetchJob> fetches[16];

static std::string to_utf8(const String &text) {
    const CharString utf = text.utf8();
    return std::string(utf.get_data(), static_cast<size_t>(utf.length()));
}

static bool split_download_url(const String &url, FetchJob &job) {
    job.tls = url.begins_with("https://");
    String rest = url.trim_prefix("https://").trim_prefix("http://");
    const int slash = rest.find("/");
    String host = slash >= 0 ? rest.substr(0, slash) : rest;
    job.path = to_utf8(slash >= 0 ? rest.substr(slash) : String("/"));
    job.port = job.tls ? 443 : 80;
    const int colon = host.rfind(":");
    if (colon > 0) {
        job.port = host.substr(colon + 1).to_int();
        host = host.substr(0, colon);
    }
    job.host = to_utf8(host);
    return !job.host.empty() && !job.path.empty();
}

#ifdef _WIN32
static std::wstring widen(const std::string &text) {
    if (text.empty()) return std::wstring();
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

struct HttpPool {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    std::string host;
    int port = 0;
    std::mutex mu;
};

HttpPool &http_pool() {
    static HttpPool pool;
    return pool;
}

static HINTERNET shared_connect(const FetchJob *job) {
    HttpPool &pool = http_pool();
    std::lock_guard<std::mutex> lock(pool.mu);
    if (!pool.session) {
        pool.session = WinHttpOpen(L"UnFalsus", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!pool.session) return nullptr;
        DWORD conns = 32;
        WinHttpSetOption(pool.session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &conns, sizeof(conns));
        WinHttpSetOption(pool.session, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &conns, sizeof(conns));
        WinHttpSetTimeouts(pool.session, 8000, 8000, 30000, 15000);
    }
    if (pool.connect && (pool.host != job->host || pool.port != job->port)) {
        WinHttpCloseHandle(pool.connect);
        pool.connect = nullptr;
    }
    if (!pool.connect) {
        pool.connect = WinHttpConnect(pool.session, widen(job->host).c_str(), static_cast<INTERNET_PORT>(job->port), 0);
        pool.host = job->host;
        pool.port = job->port;
    }
    return pool.connect;
}

static void fetch_body(FetchJob *job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(job->timeout * 1000.0));
    HINTERNET connect = shared_connect(job);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", widen(job->path).c_str(), L"HTTP/1.1", WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, job->tls ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    std::wstring headers;
    for (const std::string &header : job->headers) headers += widen(header) + L"\r\n";
    bool sent = request && WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                        headers.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(request, nullptr);
    if (!sent || job->cancel.load(std::memory_order_acquire)) {
        if (request) WinHttpCloseHandle(request);
        job->done.store(true, std::memory_order_release);
        return;
    }
    DWORD code = 0;
    DWORD code_size = sizeof(code);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_size, WINHTTP_NO_HEADER_INDEX);
    job->code = static_cast<int>(code);
    if (code != 200) {
        WinHttpCloseHandle(request);
        job->result = HTTPRequest::RESULT_SUCCESS;
        job->done.store(true, std::memory_order_release);
        return;
    }
    int64_t expect = -1;
    wchar_t length_text[32] = {};
    DWORD length_size = sizeof(length_text);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size, WINHTTP_NO_HEADER_INDEX))
        expect = _wtoi64(length_text);
    FILE *out = _wfopen(widen(job->temp).c_str(), L"wb");
    if (!out) {
        WinHttpCloseHandle(request);
        job->done.store(true, std::memory_order_release);
        return;
    }
    std::vector<unsigned char> file_buffer(1024 * 1024);
    setvbuf(out, reinterpret_cast<char *>(file_buffer.data()), _IOFBF, file_buffer.size());
    std::vector<unsigned char> buffer(32 * 1024);
    int64_t total = 0;
    bool failed = false;
    while (!job->cancel.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            if (GetLastError() == ERROR_WINHTTP_TIMEOUT) continue;
            failed = true;
            break;
        }
        if (read == 0) break;
        if (std::fwrite(buffer.data(), 1, read, out) != read) { failed = true; break; }
        total += read;
        job->got.store(total, std::memory_order_relaxed);
        if (expect >= 0 && total >= expect) break;
    }
    std::fclose(out);
    WinHttpCloseHandle(request);
    if (job->cancel.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= deadline) failed = true;
    if (expect >= 0 && total != expect) failed = true;
    if (!failed && total > 0) job->result = HTTPRequest::RESULT_SUCCESS;
    job->done.store(true, std::memory_order_release);
}
#else
static void fetch_body(FetchJob *job) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(static_cast<int>(job->timeout * 1000.0));
    Ref<HTTPClient> client;
    client.instantiate();
    client->set_read_chunk_size(1024 * 1024);
    Ref<TLSOptions> tls;
    if (job->tls) tls = TLSOptions::client();
    if (client->connect_to_host(String::utf8(job->host.c_str()), job->port, tls) != OK) {
        job->done.store(true, std::memory_order_release);
        return;
    }
    while (!job->cancel.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        client->poll();
        const int status = client->get_status();
        if (status == HTTPClient::STATUS_CONNECTED) break;
        if (status == HTTPClient::STATUS_CONNECTION_ERROR || status == HTTPClient::STATUS_TLS_HANDSHAKE_ERROR
                || status == HTTPClient::STATUS_CANT_CONNECT || status == HTTPClient::STATUS_CANT_RESOLVE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (client->get_status() != HTTPClient::STATUS_CONNECTED || job->cancel.load(std::memory_order_acquire)) {
        client->close();
        job->done.store(true, std::memory_order_release);
        return;
    }
    PackedStringArray headers;
    for (const std::string &header : job->headers) headers.push_back(String::utf8(header.c_str()));
    if (client->request(HTTPClient::METHOD_GET, String::utf8(job->path.c_str()), headers) != OK) {
        client->close();
        job->done.store(true, std::memory_order_release);
        return;
    }
    while (!job->cancel.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        client->poll();
        const int status = client->get_status();
        if (status == HTTPClient::STATUS_BODY || client->has_response()) break;
        if (status == HTTPClient::STATUS_CONNECTION_ERROR || status == HTTPClient::STATUS_TLS_HANDSHAKE_ERROR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!client->has_response() || job->cancel.load(std::memory_order_acquire)) {
        client->close();
        job->done.store(true, std::memory_order_release);
        return;
    }
    job->code = client->get_response_code();
    if (job->code != 200) {
        client->close();
        job->result = HTTPRequest::RESULT_SUCCESS;
        job->done.store(true, std::memory_order_release);
        return;
    }
    Ref<FileAccess> out = FileAccess::open(String::utf8(job->temp.c_str()), FileAccess::WRITE);
    if (!out.is_valid()) {
        client->close();
        job->done.store(true, std::memory_order_release);
        return;
    }
    const int64_t body_len = client->get_response_body_length();
    int64_t total = 0;
    bool failed = false;
    while (!job->cancel.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        client->poll();
        const PackedByteArray chunk = client->read_response_body_chunk();
        if (!chunk.is_empty()) {
            if (!out->store_buffer(chunk)) { failed = true; break; }
            total += chunk.size();
            job->got.store(total, std::memory_order_relaxed);
            if (body_len >= 0 && total >= body_len) break;
            continue;
        }
        const int status = client->get_status();
        if (status == HTTPClient::STATUS_BODY || status == HTTPClient::STATUS_CONNECTED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (status == HTTPClient::STATUS_CONNECTION_ERROR || status == HTTPClient::STATUS_TLS_HANDSHAKE_ERROR) failed = true;
        break;
    }
    out.unref();
    client->close();
    if (!failed && !job->cancel.load(std::memory_order_acquire) && total > 0) job->result = HTTPRequest::RESULT_SUCCESS;
    job->done.store(true, std::memory_order_release);
}
#endif

static void stop_fetches() {
    for (int i = 0; i < 16; ++i) if (fetches[i]) fetches[i]->cancel.store(true, std::memory_order_release);
    for (int i = 0; i < 16; ++i) {
        if (!fetches[i]) continue;
        if (fetches[i]->thread.joinable()) fetches[i]->thread.join();
        fetches[i].reset();
    }
}
}

void ClientScreen::startup_abort_seals() {
    for (int i = 0; i < 16; ++i) slot_ignore[i] = true;
    stop_fetches();
    for (int i = 0; i < 16; ++i) {
        if (startup_slot_http[i]) startup_slot_http[i]->cancel_request();
        startup_slot_item[i] = -1;
    }
    startup_download_inflight = 0;
    WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
    for (const std::unique_ptr<SealJob> &job : seal_live) {
        if (pool && job->task >= 0 && !job->done.load(std::memory_order_acquire))
            pool->wait_for_task_completion(job->task);
    }
    seal_live.clear();
    seal_pending.clear();
    download_retries.clear();
    seal_bytes = 0;
    shown_bytes = 0;
    committed_assets().clear();
    committed_loaded = false;
    for (int i = 0; i < 16; ++i) slot_ignore[i] = false;
}

bool ClientScreen::startup_launch_slot(int slot, int index, int attempt) {
    const Dictionary item = startup_missing[index];
    const String path = asset_path(item);
    if (path.is_empty()) {
        startup_storage_failed = true;
        startup_download_failed = true;
        return true;
    }
    const String dest = join_path(ASSETS, path);
    const String global = ProjectSettings::get_singleton()->globalize_path(dest);
    const int slash = global.rfind("/");
    if (slash > 0) DirAccess::make_dir_recursive_absolute(global.substr(0, slash));
    startup_slot_item[slot] = index;
    startup_slot_attempt[slot] = attempt;
    startup_slot_stall[slot] = 0;
    startup_slot_seen[slot] = 0;
    startup_slot_temp[slot] = dest + String(".part");
    startup_slot_dest[slot] = dest;
    auto fetch = std::make_unique<FetchJob>();
    const String url = join_path(startup_base_url, "assets_download/" + path);
    if (!split_download_url(url, *fetch)) {
        startup_slot_item[slot] = -1;
        startup_download_failed = true;
        return false;
    }
#ifdef _WIN32
    fetch->temp = to_utf8(ProjectSettings::get_singleton()->globalize_path(startup_slot_temp[slot]));
#else
    fetch->temp = to_utf8(startup_slot_temp[slot]);
#endif
    fetch->timeout = std::clamp(30.0 + double(item.get("length", 0)) / 32.0 / 1000.0, 60.0, 600.0);
    fetch->headers.push_back(to_utf8("Authorization: " + bearer_auth(startup_auth_token)));
    fetch->headers.push_back("AppVersion: 1.0.0");
    FetchJob *raw = fetch.get();
    try {
        raw->thread = std::thread(fetch_body, raw);
    } catch (...) {
        startup_slot_item[slot] = -1;
        if (attempt + 1 < 8) download_retries.push_back(DownloadRetry{index, attempt + 1});
        else startup_download_failed = true;
        return false;
    }
    fetches[slot] = std::move(fetch);
    ++startup_download_inflight;
    return true;
}

void ClientScreen::startup_pump_slots() {
    while (startup_download_inflight < startup_download_limit) {
        int slot = -1;
        for (int i = 0; i < 16; ++i) {
            if (startup_slot_item[i] < 0 && !fetches[i]) { slot = i; break; }
        }
        if (slot < 0) return;
        int index = -1;
        int attempt = 0;
        if (!download_retries.empty()) {
            index = download_retries.front().index;
            attempt = download_retries.front().attempt;
            download_retries.erase(download_retries.begin());
        } else if (startup_download_cursor < startup_missing.size()) {
            index = startup_download_cursor++;
        } else return;
        if (!startup_launch_slot(slot, index, attempt)) return;
    }
}

void ClientScreen::startup_finish_downloads() {
    if (startup_request_kind != DOWNLOAD) return;
    if (startup_download_inflight > 0 || startup_download_cursor < startup_missing.size()) return;
    // A network response is only the first half of an update. Every .part
    // file must pass the CRC/cipher check and be committed before the
    // progress bar can finish or the startup overlay can close.
    if (startup_manifest_done < startup_missing.size()
            || !download_retries.empty() || !seal_pending.empty() || !seal_live.empty()) return;
    startup_request_kind = NONE;
    if (startup_download_failed) {
        startup_set_busy(false);
        startup_error_action = DISMISS;
        startup_show_content_dialog(false, startup_storage_failed
            ? String(U"下载数据时出现问题，请检查设备存储空间。\n错误代码：9805")
            : String(U"发生网络错误，请重试。\n错误代码：9804"));
        return;
    }
    startup_manifest_index = startup_missing.size();
    startup_verify_index = startup_manifest.size();
    startup_set_busy(false);
    startup_clear_overlay();
    content_ready = true;
}

void ClientScreen::startup_collect_seals() {
    WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
    int limit = OS::get_singleton()->get_processor_count();
    if (limit < 2) limit = 2;
    if (limit > 4) limit = 4;
    while (int(seal_live.size()) < limit && !seal_pending.empty()) {
        std::unique_ptr<SealJob> job = std::move(seal_pending.back());
        seal_pending.pop_back();
        SealJob *raw = job.get();
        raw->task = pool ? pool->add_native_task(&seal_task, raw, false, "Seal asset") : -1;
        if (raw->task < 0) seal_task(raw);
        seal_live.push_back(std::move(job));
    }
    for (int i = 0; i < int(seal_live.size());) {
        SealJob *job = seal_live[i].get();
        if (!job->done.load(std::memory_order_acquire)) { ++i; continue; }
        const int64_t n = std::max<int64_t>(0, job->length);
        seal_bytes -= n;
        if (seal_bytes < 0) seal_bytes = 0;
        remove_asset(job->temp);
        if (!job->ok) {
            remove_asset(job->dest);
            if (job->storage) {
                startup_storage_failed = true;
                startup_download_failed = true;
            } else if (job->attempt + 1 < 4) {
                download_retries.push_back(DownloadRetry{job->index, job->attempt + 1});
            } else startup_download_failed = true;
        } else {
            ++startup_manifest_done;
            startup_manifest_bytes_done += n;
            if (job->index >= 0 && job->index < startup_missing.size())
                remember_asset(startup_missing[job->index]);
        }
        seal_live.erase(seal_live.begin() + i);
    }
}

void ClientScreen::startup_download_next() {
    startup_abort_seals();
    crc32(PackedByteArray());
    startup_download_limit = 8;
    shown_bytes = 0;
    startup_download_cursor = 0;
    startup_download_inflight = 0;
    startup_download_failed = false;
    startup_storage_failed = false;
    startup_manifest_index = 0;
    startup_manifest_done = 0;
    startup_manifest_bytes_done = 0;
    startup_request_kind = DOWNLOAD;
    for (int i = 0; i < 16; ++i) {
        startup_slot_item[i] = -1;
        startup_slot_attempt[i] = 0;
        startup_slot_stall[i] = 0;
        startup_slot_seen[i] = 0;
    }
    startup_pump_slots();
    startup_finish_downloads();
}

void ClientScreen::startup_download_completed(int result, int response_code, const PackedStringArray &, const PackedByteArray &, int slot) {
    if (slot < 0 || slot >= 16 || slot_ignore[slot] || startup_slot_item[slot] < 0) return;
    const int index = startup_slot_item[slot];
    const int attempt = startup_slot_attempt[slot];
    const String temp = startup_slot_temp[slot];
    const String dest = startup_slot_dest[slot];
    const Dictionary item = startup_missing[index];
    startup_slot_item[slot] = -1;
    if (startup_download_inflight > 0) --startup_download_inflight;
    if (result == HTTPRequest::RESULT_SUCCESS && response_code == 200) {
        auto job = std::make_unique<SealJob>();
        job->index = index;
        job->attempt = attempt;
        job->length = int64_t(item.get("length", 0));
        job->crc = String(item.get("crc32", "")).to_lower();
        job->temp = temp;
        job->dest = dest;
        seal_bytes += std::max<int64_t>(0, job->length);
        seal_pending.push_back(std::move(job));
    } else {
        remove_asset(temp);
        if (result != HTTPRequest::RESULT_SUCCESS) {
            if (startup_download_inflight > 0) download_retries.push_back(DownloadRetry{index, attempt});
            else if (attempt + 1 < 8) download_retries.push_back(DownloadRetry{index, attempt + 1});
            else startup_download_failed = true;
        } else if (attempt + 1 < 2) download_retries.push_back(DownloadRetry{index, attempt + 1});
        else startup_download_failed = true;
    }
    startup_collect_seals();
}

void ClientScreen::startup_step_downloads(double delta) {
    if (startup_request_kind != DOWNLOAD) return;
    startup_collect_seals();
    startup_pump_slots();
    const double step = delta > 1.0 ? 1.0 : delta;
    for (int i = 0; i < 16; ++i) {
        if (startup_slot_item[i] < 0 || !fetches[i]) continue;
        if (fetches[i]->done.load(std::memory_order_acquire)) {
            const int result = fetches[i]->result;
            const int code = fetches[i]->code;
            if (fetches[i]->thread.joinable()) fetches[i]->thread.join();
            fetches[i].reset();
            startup_download_completed(result, code, PackedStringArray(), PackedByteArray(), i);
            continue;
        }
        const int64_t seen = fetches[i]->got.load(std::memory_order_relaxed);
        if (seen != startup_slot_seen[i]) {
            startup_slot_seen[i] = seen;
            startup_slot_stall[i] = 0;
        } else if ((startup_slot_stall[i] += step) >= 45.0) {
            startup_slot_stall[i] = 0;
            fetches[i]->cancel.store(true, std::memory_order_release);
        }
    }
    startup_finish_downloads();
}

void ClientScreen::startup_step_activity(double delta) {
    if (!startup_activity_wait || startup_request_kind != MANIFEST) return;
    startup_activity_delay += delta;
    if (startup_activity_delay < 0.28) return;
    startup_activity_wait = false;
    startup_set_busy(true);
}

int64_t ClientScreen::startup_live_download_bytes() const {
    int64_t live = startup_manifest_bytes_done + seal_bytes;
    if (startup_request_kind == DOWNLOAD) {
        for (int i = 0; i < 16; ++i) {
            if (startup_slot_item[i] < 0 || !fetches[i]) continue;
            int64_t got = fetches[i]->got.load(std::memory_order_relaxed);
            const int64_t expected = int64_t(Dictionary(startup_missing[startup_slot_item[i]]).get("length", 0));
            if (expected > 0 && got > expected) got = expected;
            live += got;
        }
    }
    if (live < shown_bytes) return shown_bytes;
    shown_bytes = live;
    return live;
}

void ClientScreen::startup_verify_next() {
    startup_verify_index = startup_manifest.size();
}

void ClientScreen::startup_set_busy(bool busy) {
    startup_busy = busy;
    startup_busy_seconds = 0;
    if (startup_login_button) startup_login_button->set_disabled(busy);
    if (startup_confirm_button) startup_confirm_button->set_disabled(busy);
    Node *host = startup_overlay ? static_cast<Node *>(startup_overlay) : static_cast<Node *>(stage);
    if (!host) return;
    if (!busy && startup_activity && startup_activity->get_parent() == stage) {
        startup_activity->queue_free();
        startup_activity = nullptr;
        startup_busy_blocker = nullptr;
        return;
    }
    if (!startup_overlay && !busy) return;
    if (busy && startup_overlay && !startup_busy_blocker) {
        auto *blocker = memnew(Control);
        blocker->set_name("BusyBlocker");
        host->add_child(blocker);
        blocker->set_position(Vector2(0, 0));
        blocker->set_size(Vector2(W, H));
        blocker->set_mouse_filter(Control::MOUSE_FILTER_STOP);
        blocker->set_visible(false);
        startup_busy_blocker = blocker;
    }
    if (startup_busy_blocker) {
        startup_busy_blocker->set_visible(busy);
        startup_busy_blocker->set_mouse_filter(busy ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
    }
    if (busy && !startup_activity) {
        Ref<Texture2D> icon = tex("startup/activity_icon.png");
        if (icon.is_valid()) {
            const bool login = startup_login_panel != nullptr;
            const float rest = login ? 1.f : .75f;
            auto *activity = memnew(TextureRect);
            activity->set_name("ActivityIcon");
            host->add_child(activity);
            activity->set_texture(icon);
            activity->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            activity->set_stretch_mode(TextureRect::STRETCH_SCALE);
            activity->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            activity->set_size(Vector2(97, 146));
            const bool full_canvas = get_size().y > 0 && get_size().x / get_size().y <= W / H;
            const float canvas_h = login ? H : (full_canvas ? STARTUP_CANVAS_H : H);
            const Vector2 activity_position = login
                ? Vector2((W - 97) / 2, (H - 146) / 2)
                : pos(W / 2 - 47 * 1.5f, -H / 2 + 70 * 1.5f, 97, 146);
            activity->set_position(login ? activity_position
                : Vector2(activity_position.x - 97.f / 2,
                    canvas_h - 146 - (H - 902.f - 146.f)));
            activity->set_pivot_offset(Vector2(97, 146) / 2);
            activity->set_scale(Vector2(rest, rest));
            activity->set_modulate(Color(1, 1, 1, 0));
            startup_activity = activity;
            layout();
        }
    }
    if (startup_activity && startup_login_panel) {
        if (busy) {
            startup_activity_animation.begin(startup_activity);
        } else {
            startup_activity_animation.end(startup_activity);
        }
    } else if (startup_activity) {
        startup_activity->set_visible(busy);
    }
    if (startup_login_panel && busy) {
        if (auto *dim = startup_overlay->get_node_or_null(NodePath("Dimmer")))
            startup_overlay->move_child(dim, startup_overlay->get_child_count() - 1);
        if (startup_busy_blocker) startup_overlay->move_child(startup_busy_blocker, startup_overlay->get_child_count() - 1);
        if (startup_activity) startup_overlay->move_child(startup_activity, startup_overlay->get_child_count() - 1);
    }
}

void ClientScreen::startup_animate(double delta) {
    if (!startup_overlay) {
        if (!(startup_activity && startup_busy)) return;
        startup_busy_seconds += delta;
        const float alpha = std::clamp(float(startup_busy_seconds / .5), 0.f, 1.f);
        startup_activity->set_modulate(Color(1, 1, 1, alpha));
        startup_activity->set_visible(true);
        const float rest = .75f;
        const float squash = rest * (.01f / .5f);
        const float phase = float(std::fmod(startup_busy_seconds, 1.0));
        const float u = phase < .5f ? phase / .5f : (phase - .5f) / .5f;
        const float k = phase < .5f ? (u <= 0.f ? 0.f : std::pow(2.f, 10.f * (u - 1.f)))
            : (u >= 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * u));
        const float from = phase < .5f ? rest : squash;
        const float to = phase < .5f ? squash : rest;
        startup_activity->set_scale(Vector2(from + (to - from) * k, rest));
        return;
    }
    startup_overlay_seconds += delta;
    const bool login = startup_login_panel != nullptr;
    const bool dialog = startup_overlay->get_name() == StringName("StartupContentDialog");
    const double duration = login ? 20. / 60. : .2;
    const float t = std::clamp(float(startup_overlay_seconds / duration), 0.f, 1.f);
    const float easing = std::sin(t * 1.570796327f);
    if (login) {
        auto *dim = Object::cast_to<ColorRect>(startup_overlay->get_node_or_null(NodePath("Dimmer")));
        if (dim) {
            const float target = (startup_overlay_closing ? 0.f : startup_busy ? 128.f : 64.f) / 255.f;
            if (startup_dimmer_to < 0.f || std::abs(startup_dimmer_to - target) > 0.001f) {
                startup_dimmer_from = dim->get_modulate().a;
                const bool to_busy = !startup_overlay_closing && (startup_busy || startup_dimmer_from > 70.f / 255.f);
                startup_dimmer_to = target;
                startup_dimmer_time = 0;
                startup_dimmer_duration = to_busy ? .2f : float(20. / 60.);
            }
            startup_dimmer_time += delta;
            const float u = std::clamp(float(startup_dimmer_time / std::max(.001f, startup_dimmer_duration)), 0.f, 1.f);
            const float k = std::sin(u * 1.570796327f);
            const float alpha = startup_dimmer_from + (startup_dimmer_to - startup_dimmer_from) * k;
            dim->set_modulate(Color(1, 1, 1, alpha));
            if (!startup_busy && !startup_overlay_closing && u >= 1.f && alpha <= 64.f / 255.f + .01f)
                startup_overlay->move_child(dim, 0);
        }
    }
    if (startup_overlay_closing) {
        if (login) {
            startup_login_panel->set_position(Vector2(-100 * side_fit() * easing - design_extra * .5f, 0));
            startup_login_panel->set_modulate(Color(1, 1, 1, 1 - easing));
        } else if (dialog) {
            startup_overlay->set_modulate(Color(1, 1, 1, 1 - t));
            auto *panel = startup_overlay->get_node<Control>("DialogPanel");
            const float ease = css_ease_out(t);
            panel->set_scale(Vector2(1 + .1f * ease, 1 + .1f * ease));
        }
        if (t < 1) return;
        const int phase = startup_content_phase;
        const bool download = startup_dialog_download;
        const int action = startup_error_action;
        startup_clear_overlay();
        if (phase == 1) { startup_fetch_manifest(); return; }
        if (phase == 2 && download) { startup_start_downloads(); return; }
        if (phase == 2 && action == OPEN_SITE) {
            const String url = setting_string("unfalsus/update_url");
            if (!url.is_empty()) OS::get_singleton()->shell_open(url);
        }
        if (phase == 2 && action == RETRY) { startup_fetch_manifest(); return; }
        startup_content_started = false;
        startup_content_waiting = true;
        return;
    }
    if (login) {
        startup_login_panel->set_position(Vector2(-100 * side_fit() * (1 - easing) - design_extra * .5f, 0));
        startup_login_panel->set_modulate(Color(1, 1, 1, easing));
    } else if (dialog) {
        startup_overlay->set_modulate(Color(1, 1, 1, t));
        const float ease = css_ease_out(t);
        startup_overlay->get_node<Control>("DialogPanel")->set_scale(Vector2(1.1f - .1f * ease, 1.1f - .1f * ease));
    } else {
        startup_overlay->set_modulate(Color(1, 1, 1, std::clamp(float(startup_overlay_seconds / .2), 0.f, 1.f)));
        if (startup_download_fill) {
            const int64_t current = startup_live_download_bytes();
            const float ratio = startup_manifest_bytes_total > 0
                ? std::clamp(float(current) / startup_manifest_bytes_total, 0.f, 1.f)
                : float(startup_manifest_done) / std::max(1, startup_manifest_total);
            const float full_w = 1673 * ratio;
            startup_download_fill->set_size(Vector2(full_w, 11));
            if (startup_download_diamond) startup_download_diamond->set_position(Vector2((W - 1673) / 2 + full_w - 17, 37.5f - 17));
            if (startup_download_tail) {
                const float vis = std::min(116.f, full_w);
                startup_download_tail->set_visible(full_w > 15);
                startup_download_tail->set_size(Vector2(vis, 11));
                startup_download_tail->set_position(Vector2((W - 1673) / 2 + full_w - vis, 37.5f - 5.5f));
                if (auto *img = Object::cast_to<Control>(startup_download_tail->get_child(0)))
                    img->set_position(Vector2(vis - 116.f, 0));
            }
            if (startup_count_label) {
                const bool validating = startup_download_cursor >= startup_missing.size()
                    && startup_download_inflight == 0
                    && (startup_manifest_done < startup_missing.size()
                        || !download_retries.empty() || !seal_pending.empty() || !seal_live.empty());
                if (validating) startup_count_label->set_text(U"文件校验中...");
                else if (startup_manifest_index < startup_missing.size())
                    startup_count_label->set_text(String::num_int64(startup_manifest_done) + "/"
                        + String::num_int64(startup_manifest_total));
            }
            if (startup_size_label && startup_manifest_bytes_total >= 102400)
                startup_size_label->set_text(String::num(double(current) / 1048576., 1) + "MB / "
                    + String::num(double(startup_manifest_bytes_total) / 1048576., 1) + "MB");
        }
    }
    if (startup_activity && (startup_busy || startup_login_panel)) {
        if (startup_login_panel) {
            startup_activity_animation.step(startup_activity, delta);
            return;
        }
        if (startup_busy) startup_busy_seconds += delta;
        const float alpha = std::clamp(float(startup_busy_seconds / .5), 0.f, 1.f);
        startup_activity->set_modulate(Color(1, 1, 1, alpha));
        startup_activity->set_visible(alpha > .001f);
        if (!startup_busy) {
            startup_activity->set_scale(Vector2(.75f, .75f));
        } else {
            startup_activity->set_pivot_offset(startup_activity->get_size() / 2);
            const float rest = .75f;
            const float squash = rest * (.01f / .5f);
            const float phase = float(std::fmod(startup_busy_seconds, 1.0));
            const float u = phase < .5f ? phase / .5f : (phase - .5f) / .5f;
            const float k = phase < .5f ? (u <= 0.f ? 0.f : std::pow(2.f, 10.f * (u - 1.f)))
                : (u >= 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * u));
            const float from = phase < .5f ? rest : squash;
            const float to = phase < .5f ? squash : rest;
            startup_activity->set_scale(Vector2(from + (to - from) * k, rest));
        }
    }
}

void ClientScreen::startup_clear_overlay() {
    startup_busy = false;
    startup_login_panel = nullptr;
    startup_user_field = nullptr;
    startup_pass_field = nullptr;
    startup_error_label = nullptr;
    startup_status_label = nullptr;
    startup_login_button = nullptr;
    startup_confirm_button = nullptr;
    startup_cancel_button = nullptr;
    startup_progress = nullptr;
    startup_count_label = nullptr;
    startup_size_label = nullptr;
    startup_download_fill = nullptr;
    startup_download_diamond = nullptr;
    startup_download_tail = nullptr;
    startup_activity = nullptr;
    startup_busy_blocker = nullptr;
    startup_overlay_closing = false;
    startup_content_phase = 0;
    if (startup_overlay) {
        startup_overlay->queue_free();
        startup_overlay = nullptr;
    }
}
