#include "client.hpp"
#include "arcapi.hpp"
#include "content.hpp"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/input_event_screen_drag.hpp>
#include <godot_cpp/classes/input_event_screen_touch.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/tween.hpp>
#include <godot_cpp/classes/property_tweener.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace godot;

static constexpr float W = 1920, H = 1080;
static constexpr const char *IMAGE = "res://assets/resources/img/";
static constexpr const char *AUDIO = "res://assets/resources/audio/";

#if defined(_WIN32)
static HWND aspect_window = nullptr;
static WNDPROC previous_window_proc = nullptr;
static bool lock_window_aspect = true;

static LRESULT CALLBACK aspect_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SIZING && lparam && lock_window_aspect) {
        RECT &rect = *reinterpret_cast<RECT *>(lparam);
        RECT client = {}, outer = {};
        GetClientRect(hwnd, &client);
        GetWindowRect(hwnd, &outer);
        const int frame_width = (outer.right - outer.left) - (client.right - client.left);
        const int frame_height = (outer.bottom - outer.top) - (client.bottom - client.top);
        const int width = rect.right - rect.left - frame_width;
        const int height = rect.bottom - rect.top - frame_height;
        if (width > 0 && height > 0) {
            const bool width_driven = width * 9 >= height * 16;
            const int client_width = width_driven ? int(std::lround(height * 16.0 / 9.0)) : width;
            const int client_height = width_driven ? height : int(std::lround(width * 9.0 / 16.0));
            const int target_width = client_width + frame_width;
            const int target_height = client_height + frame_height;
            const int edge = int(wparam);
            const bool left = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
            const bool top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
            if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
                const int center = (rect.top + rect.bottom) / 2;
                rect.top = center - target_height / 2; rect.bottom = rect.top + target_height;
            } else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
                const int center = (rect.left + rect.right) / 2;
                rect.left = center - target_width / 2; rect.right = rect.left + target_width;
            }
            if (left) rect.left = rect.right - target_width; else rect.right = rect.left + target_width;
            if (top) rect.top = rect.bottom - target_height; else rect.bottom = rect.top + target_height;
        }
        return TRUE;
    }
    return CallWindowProcW(previous_window_proc, hwnd, message, wparam, lparam);
}

static void install_aspect_window_proc() {
    DisplayServer *display = DisplayServer::get_singleton();
    if (!display || aspect_window) return;
    aspect_window = reinterpret_cast<HWND>(display->window_get_native_handle(DisplayServer::WINDOW_HANDLE));
    if (aspect_window)
        previous_window_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(aspect_window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(aspect_window_proc)));
}

static void uninstall_aspect_window_proc() {
    if (aspect_window && previous_window_proc)
        SetWindowLongPtrW(aspect_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_window_proc));
    aspect_window = nullptr;
    previous_window_proc = nullptr;
}
#endif


static Vector2 canvas_pos(float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    return Vector2(W / 2 + x - w * ax, H / 2 - y - h * (1 - ay));
}

static Ref<Texture2D> texture(const String &file) {
    const String path = IMAGE + file;
    if (!ResourceLoader::get_singleton()->exists(path)) {
        UtilityFunctions::push_warning("Missing scene artwork: ", path);
        return Ref<Texture2D>();
    }
    return ResourceLoader::get_singleton()->load(path);
}

static TextureRect *image(Control *parent, const char *name, const String &path,
        float x, float y, float w = 0, float h = 0, float ax = .5f, float ay = .5f) {
    Ref<Texture2D> asset = texture(path);
    if (!asset.is_valid()) return nullptr;
    if (w <= 0) w = asset->get_width();
    if (h <= 0) h = asset->get_height();
    TextureRect *node = memnew(TextureRect);
    node->set_name(name);
    parent->add_child(node);
    node->set_texture(asset);
    node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    node->set_stretch_mode(TextureRect::STRETCH_SCALE);
    node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    node->set_position(canvas_pos(x, y, w, h, ax, ay));
    node->set_size(Vector2(w, h));
    return node;
}

static void set_alpha(Control *node, float alpha) {
    if (!node) return;
    Color c = node->get_modulate();
    c.a = std::clamp(alpha, 0.f, 1.f);
    node->set_modulate(c);
}

void ClientScreen::prepare_scene_resources() {
    auto retain_directory = [&](auto &&self, const String &path) -> void {
        Ref<DirAccess> directory = DirAccess::open(path);
        if (!directory.is_valid()) return;
        for (const String &folder : directory->get_directories()) self(self, path + String("/") + folder);
        for (const String &entry : directory->get_files()) {
            const String file = entry.trim_suffix(".remap");
            if (file.ends_with(".import")) continue;
            const String resource = path + String("/") + file;
            if (!ResourceLoader::get_singleton()->exists(resource)) continue;
            Ref<Resource> loaded = ResourceLoader::get_singleton()->load(resource);
            if (loaded.is_valid()) menu_resources.push_back(loaded);
        }
    };
    if (scene_kind == 1) {
        // Main-menu assets load on demand as the scene is built. Avoid
        // synchronously retaining whole content folders and pre-rasterizing
        // every catalog glyph during the scene transition.
        content::song_title("");
    } else {
        retain_directory(retain_directory, "res://assets/resources/font/mainmenu");
        retain_directory(retain_directory, "res://assets/resources/img/transition");
        retain_directory(retain_directory, "res://assets/resources/audio/sfx");
    }
    if (scene_kind == 0) {
        retain_directory(retain_directory, "res://assets/resources/img/startup");
    } else if (scene_kind == 2) {
        retain_directory(retain_directory, "res://assets/resources/img/konzetsu");
        retain_directory(retain_directory, "res://assets/resources/audio/konzetsu");
    }
    const char *intro = scene_kind == 0 ? "title_full.ogg" : scene_kind == 1 ? "mainmenu_full.ogg" : "hub_konzetsu.ogg";
    const char *loop = scene_kind == 0 ? "title_loop.ogg" : scene_kind == 1 ? "mainmenu_loop.ogg" : "hub_konzetsu_loop.ogg";
    for (const char *file : {intro, loop})
        menu_resources.push_back(ResourceLoader::get_singleton()->load(String(AUDIO) + "bgm/" + file));
    sfx = memnew(AudioStreamPlayer);
    add_child(sfx);
}

void ClientScreen::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_scene_kind", "kind"), &ClientScreen::set_scene_kind);
    ClassDB::bind_method(D_METHOD("get_scene_kind"), &ClientScreen::get_scene_kind);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "scene_kind", PROPERTY_HINT_ENUM, "Startup,MainMenu,Konzetsu"), "set_scene_kind", "get_scene_kind");
    ClassDB::bind_method(D_METHOD("set_native_mode", "mode"), &ClientScreen::set_native_mode);
    ClassDB::bind_method(D_METHOD("get_native_mode"), &ClientScreen::get_native_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "native_mode"), "set_native_mode", "get_native_mode");
    ClassDB::bind_method(D_METHOD("mark_content_ready"), &ClientScreen::mark_content_ready);
    ClassDB::bind_method(D_METHOD("on_music_finished"), &ClientScreen::on_music_finished);
    ClassDB::bind_method(D_METHOD("on_menu_action", "action"), &ClientScreen::on_menu_action);
    ClassDB::bind_method(D_METHOD("open_character_select"), &ClientScreen::open_character_select);
    ClassDB::bind_method(D_METHOD("position_hub_portrait", "slide_offset"), &ClientScreen::position_hub_portrait);
    ClassDB::bind_method(D_METHOD("close_character_select"), &ClientScreen::close_character_select);
    ClassDB::bind_method(D_METHOD("menu_http_completed", "result", "response_code", "headers", "body"), &ClientScreen::menu_http_completed);
    ClassDB::bind_method(D_METHOD("konzetsu_tasks_completed", "result", "response_code", "headers", "body"), &ClientScreen::konzetsu_tasks_completed);
    ClassDB::bind_method(D_METHOD("menu_hover", "which", "hovered"), &ClientScreen::menu_hover);
    ClassDB::bind_method(D_METHOD("select_character", "id"), &ClientScreen::select_character);
    ClassDB::bind_method(D_METHOD("open_partner_details"), &ClientScreen::open_partner_details);
    ClassDB::bind_method(D_METHOD("close_partner_details"), &ClientScreen::close_partner_details);
    ClassDB::bind_method(D_METHOD("partner_details_back"), &ClientScreen::partner_details_back);
    ClassDB::bind_method(D_METHOD("present_partner_details"), &ClientScreen::present_partner_details);
    ClassDB::bind_method(D_METHOD("restore_partner_aside"), &ClientScreen::restore_partner_aside);
    ClassDB::bind_method(D_METHOD("finish_partner_detail_transition", "open"), &ClientScreen::finish_partner_detail_transition);
    ClassDB::bind_method(D_METHOD("preview_partner_level", "level"), &ClientScreen::preview_partner_level);
    ClassDB::bind_method(D_METHOD("cycle_partner_preview"), &ClientScreen::cycle_partner_preview);
    ClassDB::bind_method(D_METHOD("open_partner_purchase"), &ClientScreen::open_partner_purchase);
    ClassDB::bind_method(D_METHOD("close_partner_modal"), &ClientScreen::close_partner_modal);
    ClassDB::bind_method(D_METHOD("confirm_partner_purchase"), &ClientScreen::confirm_partner_purchase);
    ClassDB::bind_method(D_METHOD("partner_caption_color", "path", "color"), &ClientScreen::partner_caption_color);
    ClassDB::bind_method(D_METHOD("filter_characters", "query"), &ClientScreen::filter_characters);
    ClassDB::bind_method(D_METHOD("toggle_all_characters"), &ClientScreen::toggle_all_characters);
    ClassDB::bind_method(D_METHOD("return_to_menu"), &ClientScreen::return_to_menu);
    ClassDB::bind_method(D_METHOD("open_konzetsu_challenge", "id"), &ClientScreen::open_konzetsu_challenge);
    ClassDB::bind_method(D_METHOD("close_konzetsu_challenge"), &ClientScreen::close_konzetsu_challenge);
    ClassDB::bind_method(D_METHOD("confirm_konzetsu_challenge"), &ClientScreen::confirm_konzetsu_challenge);
    ClassDB::bind_method(D_METHOD("set_konzetsu_progress", "completed_songs"), &ClientScreen::set_konzetsu_progress);
    ADD_SIGNAL(MethodInfo("konzetsu_challenge_requested", PropertyInfo(Variant::INT, "challenge_index")));
    ClassDB::bind_method(D_METHOD("startup_request_completed", "result", "response_code", "headers", "body"), &ClientScreen::startup_request_completed);
    ClassDB::bind_method(D_METHOD("startup_download_completed", "result", "response_code", "headers", "body", "slot"), &ClientScreen::startup_download_completed);
    ClassDB::bind_method(D_METHOD("startup_submit_login"), &ClientScreen::startup_submit_login);
    ClassDB::bind_method(D_METHOD("startup_confirm_content"), &ClientScreen::startup_confirm_content);
    ClassDB::bind_method(D_METHOD("startup_cancel_content"), &ClientScreen::startup_cancel_content);
    ClassDB::bind_method(D_METHOD("startup_start_content_check"), &ClientScreen::startup_start_content_check);
}

void ClientScreen::_ready() {
    switching = false;  // Reset scene transition flag
#if defined(_WIN32)
    install_aspect_window_proc();
#endif
    constrain_window_aspect();
    stage = memnew(Control);
    stage->set_name("DesignCanvas");
    add_child(stage);
    stage->set_z_index(1);
    stage->set_size(Vector2(W, H));
    stage->set_mouse_filter(Control::MOUSE_FILTER_PASS);
    prepare_scene_resources();
    if (scene_kind == 0) {
        startup_http = memnew(HTTPRequest);
        startup_http->set_name("StartupHTTP");
        startup_http->set_timeout(15.0);
        startup_http->set_use_threads(true);
        add_child(startup_http);
        startup_http->connect("request_completed", Callable(this, "startup_request_completed"));
    }
    if (scene_kind == 2 && get_tree()->has_meta("konzetsu_pending_clear_mask")) {
        konzetsu_pending_clear_mask = int(get_tree()->get_meta("konzetsu_pending_clear_mask"));
        get_tree()->remove_meta("konzetsu_pending_clear_mask");
    }
    if (scene_kind == 2 && get_tree()->has_meta("konzetsu_daily_tasks")) {
        const Variant tasks = get_tree()->get_meta("konzetsu_daily_tasks");
        if (tasks.get_type() == Variant::ARRAY) set_konzetsu_tasks(Array(tasks));
    }
    if (scene_kind == 0) build_startup();
    else if (scene_kind == 1) build_menu();
    else build_konzetsu();
    if (scene_kind == 1 || scene_kind == 2) ClientTransition::get_or_create(get_tree());
    layout();
    if (scene_kind == 1) {
        prepare_menu_dialogs();
        layout();
        // Keep the scene transition gated only on constructing the menu, not
        // on decoding every optional character portrait in the background.
        scene_assets_ready = true;
    }
    if (scene_kind == 2) scene_assets_ready = true;
}

void ClientScreen::_notification(int what) {
    if (what == NOTIFICATION_WM_GO_BACK_REQUEST) {
        // Ignore Android's system back request. It must not quit the game or
        // change the current startup/menu scene.
        return;
    }
    if (what == NOTIFICATION_RESIZED && stage) {
        constrain_window_aspect();
        layout();
    }
}

void ClientScreen::constrain_window_aspect() {
#if defined(_WIN32)
    lock_window_aspect = scene_kind == 1;
    install_aspect_window_proc();
#endif
}

static void attach_startup_video(ClientScreen *screen, ClientVideo *node) {
    if (!screen || !node) return;
    Control *canvas = screen->startup_stage();
    if (!canvas) return;
    if (node->get_parent() && node->get_parent() != canvas) node->get_parent()->remove_child(node);
    if (node->get_parent() != canvas) {
        canvas->add_child(node);
        canvas->move_child(node, 0);
    }
    const Vector2 viewport = screen->get_size();
    const float stage_scale = std::max(.001f, canvas->get_scale().x);
    const float visible_width = viewport.x / stage_scale;
    const float visible_height = viewport.y / stage_scale;
    const float cover_scale = std::max(visible_width / W, visible_height / H);
    const float width = W * cover_scale;
    const float height = H * cover_scale;
    node->set_scale(Vector2(1, 1));
    node->set_size(Vector2(width, height));
    node->set_position(Vector2((W - width) * .5f, (visible_height - height) * .5f));
    node->set_z_index(0);
}

static int wide_role(const String &name) {
    // The hub portrait is positioned explicitly in layout() from its Cocos
    // Creator design coordinate. Do not run it through cached wide placement.
    if (name == "PartnerIllustration") return 0;
    if (name == "Background" || name == "TopBar" || name == "BottomBar" || name == "StartupBlack") return 3;
    if (name == "Grid" || name == "OuterBracketsRight") return name == "Grid" ? 0 : 2;
    if (name == "OuterBracketsLeft") return 1;
    if (name.begins_with("Music") || name.begins_with("Scenario") || name.begins_with("Tutorial")
            || name.begins_with("Crafting") || name == "TimelineHit" || name == "SettingsHit"
            || name == "SelectSong" || name == "ReplayTutorial" || name == "ViewTimeline"
            || name.begins_with("Difficulty") || name.begins_with("Unlocked") || name.begins_with("Cleared"))
        return 2;
    if (name.begins_with("Character") || name.begins_with("Partner") || name.begins_with("Skill")
            || name.begins_with("Player") || name == "CharacterSelectHit" || name.begins_with("Top")
            || name.begins_with("HubButton") || name.begins_with("Primary") || name.begins_with("Secondary"))
        return 1;
    return 0;
}

static void place_adaptive(Control *node, float dx, float dy, float grow_x, float grow_y) {
    if (!node) return;
    if (!node->has_meta("home_pos")) {
        node->set_meta("home_pos", node->get_position());
        node->set_meta("home_size", node->get_size());
    }
    const Vector2 home = node->get_meta("home_pos");
    Vector2 box = node->get_meta("home_size");
    box += Vector2(grow_x, grow_y);
    node->set_position(home + Vector2(dx, dy));
    node->set_size(box);
}

static void place_wide(Control *node, float dx, float grow) {
    place_adaptive(node, dx, 0, grow, 0);
}

static void place_vertical(Control *node, float dy) {
    if (!node) return;
    if (!node->has_meta("home_pos")) node->set_meta("home_pos", node->get_position());
    const Vector2 home = node->get_meta("home_pos");
    node->set_position(Vector2(node->get_position().x, home.y + dy));
}

static int aside_pin(const String &name) {
    if (name == "PartnerGlow" || name == "SkillContainer" || name == "SkillContainerStats"
            || name == "SkillNameStats" || name == "AsideLv" || name == "AsideLevel"
            || name == "AsideLevelBar" || name == "AsideLevelBarFill" || name == "CharacterFragMultiplier"
            || name == "AsideOfflineTag")
        return 1;
    if (name == "NameBacking" || name == "PartnerName" || name == "PartnerShape"
            || name == "PartnerIllustrator" || name == "DetailsButton")
        return 2;
    if (name == "LeftShadow" || name == "PartnerSelectListView" || name == "SearchBar" || name == "SearchField"
            || name == "Divider" || name == "DiamondsBacking" || name == "ViewAllButton" || name == "ViewAllCaption"
            || name == "OverlayClose")
        return -1;
    if (name == "PartnerDetails") return 2;
    return 0;
}

static void adapt_partner_select(Node *node, float extra, float extra_y) {
    if (!node) return;
    for (int i = 0; i < node->get_child_count(); ++i) {
        Node *child = node->get_child(i);
        const String name = child->get_name();
        if (name == "ViewAllLayer") continue;
        if (Control *control = Object::cast_to<Control>(child)) {
            const int pin = aside_pin(name);
            const float dx = pin == 1 ? extra * .17f
                : pin == 2 ? extra * .5f
                : pin == -1 ? -extra * .5f : 0;
            if (name == "PartnerDetails") {
                place_adaptive(control, -extra * .5f, -extra_y * .5f, extra, extra_y);
            } else if (name == "PartnerSelectListView") {
                place_adaptive(control, dx, -extra_y * .5f, 0, extra_y);
                if (Control *content = Object::cast_to<Control>(control->get_node_or_null("Content"))) {
                    if (!content->has_meta("home_min_size"))
                        content->set_meta("home_min_size", content->get_custom_minimum_size());
                    const Vector2 base = content->get_meta("home_min_size");
                    const Vector2 minimum(base.x, std::max(base.y, control->get_size().y));
                    content->set_custom_minimum_size(minimum);
                    content->set_size(minimum);
                }
            } else if (name == "LeftShadow") {
                place_adaptive(control, dx, -extra_y * .5f, 0, extra_y);
            } else {
                if (pin) place_wide(control, dx, 0);
                const float inset = extra_y * .5f;
                if (name == "OverlayClose" || name == "Divider" || name == "SearchBar"
                        || name == "SearchField" || name == "NameBacking" || name == "PartnerName"
                        || name == "PartnerShape" || name == "PartnerIllustrator")
                    place_vertical(control, -inset);
                else if (name == "LeftShadow" || name == "ViewAllButton" || name == "ViewAllCaption" || name == "PartnerGlow"
                        || name == "SkillContainer" || name == "SkillContainerStats" || name == "SkillNameStats"
                        || name == "AsideLv" || name == "AsideLevel" || name == "AsideLevelBar"
                        || name == "AsideLevelBarFill" || name == "AsideOfflineTag"
                        || name == "CharacterFragMultiplier" || name == "DetailsButton")
                    place_vertical(control, inset);
                else if (name == "PartnerIllustration")
                    place_vertical(control, -inset * .5f);
                else if (name == "DiamondsBacking") {
                    if (!control->has_meta("home_pos")) {
                        control->set_meta("home_pos", control->get_position());
                        control->set_meta("home_size", control->get_size());
                    }
                    const Vector2 home = control->get_meta("home_pos");
                    const Vector2 base = control->get_meta("home_size");
                    const float height = std::max(base.y, H + extra_y);
                    control->set_position(Vector2(home.x + dx, H + extra_y - height - inset));
                    control->set_size(Vector2(base.x, height));
                }
            }
        }
        if (name != "PartnerDetails" && name != "PartnerSelectListView")
            adapt_partner_select(child, extra, extra_y);
    }
}

static void cover_window(Node *node, float extra_x, float extra_y) {
    if (!node) return;
    const String name = node->get_name();
    if (Control *control = Object::cast_to<Control>(node)) {
        if (name == "Dimmer" || name == "HubDimmer" || name == "InputCatcher" || name == "StartupBlack"
                || name == "BusyBlocker" || name == "Shadow" || name == "WhiteFlash")
            place_adaptive(control, -extra_x * .5f, -extra_y * .5f, extra_x, extra_y);
        else if (name == "Background" || name == "HubBg" || name == "LoadingBg" || name == "BgGlow")
            place_adaptive(control, -extra_x * .5f, -extra_y * .5f, extra_x, extra_y);
    }
    if (name == "ViewAllLayer" || name == "PartnerSelectListView") return;
    for (int i = 0; i < node->get_child_count(); ++i) cover_window(node->get_child(i), extra_x, extra_y);
}

void ClientScreen::layout() {
    Vector2 size = get_size();
    const bool startup_canvas = scene_kind == 0;
    // The original Cocos project uses a 1920x1080 design resolution with
    // fitHeight=true: narrow viewports crop the sides, while wide ones extend.
    float scale = startup_canvas
        ? size.y / H
        : std::min(size.x / W, size.y / H);
    if (scale <= 0) return;
    stage->set_scale(Vector2(scale, scale));
    stage->set_size(Vector2(W, H));
    stage->set_position(startup_canvas
        ? Vector2((size.x - W * scale) * .5f, 0)
        : (size - Vector2(W, H) * scale) / 2);
    const float extra_x = startup_canvas
        ? size.x / scale - W
        : std::max(0.f, size.x / scale - W);
    const float extra_y = startup_canvas ? 0.f : std::max(0.f, size.y / scale - H);
    design_extra = extra_x;
    design_extra_y = extra_y;
    const float shift = extra_x * .5f;
    if (scene_kind == 1) {
        for (int i = 0; i < stage->get_child_count(); ++i) {
            Control *node = Object::cast_to<Control>(stage->get_child(i));
            if (!node) continue;
            if (node == character_dialog) continue;
            const String name = node->get_name();
            const int role = wide_role(name);
            if (role == 1) place_wide(node, -shift, 0);
            else if (role == 2) place_wide(node, shift, 0);
            else if (role == 3) place_wide(node, -shift, extra_x);
            if (name == "TopBar" || name == "TopTitleBacking" || name == "HubButton"
                    || name == "PrimaryTab" || name == "SecondaryTab")
                place_vertical(node, -extra_y * .5f);
            else if (name == "BottomBar")
                place_vertical(node, extra_y * .5f);
        }
        position_hub_portrait(hub_portrait_slide_offset);
        if (Control *background = Object::cast_to<Control>(stage->get_node_or_null(NodePath("Background"))))
            if (TextureRect *sprite = Object::cast_to<TextureRect>(background))
                sprite->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
    } else if (scene_kind == 2) {
        // The stage remains centered: backgrounds fill the visible rect,
        // while the hub and rotating loading artwork keep their proportions.
        place_vertical(art["HubBack"], -extra_y * .5f);
        place_vertical(art["BottomDiamond"], extra_y * .5f);
    }
    if (character_dialog) {
        adapt_partner_select(character_dialog, extra_x, extra_y);
        if (dialog_all) {
            place_adaptive(dialog_all, -shift, -extra_y * .5f, extra_x, extra_y);
            if (dialog_all_list) place_adaptive(dialog_all_list, 0, 0, extra_x, extra_y);
            if (Control *left = Object::cast_to<Control>(dialog_all->get_node_or_null(NodePath("LeftShadow"))))
                place_adaptive(left, 0, 0, 0, extra_y);
            if (Control *right = Object::cast_to<Control>(dialog_all->get_node_or_null(NodePath("RightShadow"))))
                place_adaptive(right, extra_x, 0, 0, extra_y);
        }
        if (dialog_view_all && dialog_chrome)
            if (LineEdit *search = Object::cast_to<LineEdit>(dialog_chrome->get_node_or_null(NodePath("SearchField"))))
                filter_characters(search->get_text());
    }
    cover_window(stage, extra_x, extra_y);
    if (scene_kind == 2) {
        const Vector2 visible_size(W + extra_x, H + extra_y);
        const Vector2 visible_origin(-extra_x * .5f, -extra_y * .5f);
        for (const char *key : {"HubBg", "LoadingBg", "BgGlow"}) {
            if (TextureRect *sprite = Object::cast_to<TextureRect>(art[key])) {
                sprite->set_position(visible_origin);
                sprite->set_size(visible_size);
                sprite->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
            }
        }
    }
    if (startup_canvas && startup_activity && !startup_login_panel) {
        // Port StartupConfig.activityIconPose() from the Cocos viewport into
        // the scaled Godot stage, then into the icon's actual parent space.
        const float cocos_fit = size.y / (H / 1.5f);
        const Vector2 screen_point(size.x - 47.f * cocos_fit, size.y - 70.f * cocos_fit);
        const Vector2 stage_point = (screen_point - stage->get_position()) / scale;
        Vector2 parent_point = stage_point;
        if (Control *parent = Object::cast_to<Control>(startup_activity->get_parent()); parent && parent != stage) {
            const Vector2 parent_scale = parent->get_scale();
            parent_point = (stage_point - parent->get_position()) / Vector2(
                std::max(.001f, parent_scale.x), std::max(.001f, parent_scale.y));
        }
        startup_activity->set_position(parent_point - startup_activity->get_size() * .5f);
    }
    if (video) attach_startup_video(this, video);
    if (title_video) attach_startup_video(this, title_video);
    if (flash) {
        // WhiteFlash must cover the real viewport, not the 1920x1080 design
        // canvas. Keep it outside the scaled startup stage so no aspect ratio
        // can expose uncovered strips during the title transition.
        flash->set_position(Vector2(0, 0));
        flash->set_size(size);
    }
}

void ClientScreen::play_music(const String &intro, const String &repeat) {
    if (!music) {
        music = memnew(AudioStreamPlayer);
        music->set_name("Music");
        add_child(music);
    }
    music->stop();
    if (music->is_connected("finished", Callable(this, "on_music_finished")))
        music->disconnect("finished", Callable(this, "on_music_finished"));
    Ref<AudioStream> first = ResourceLoader::get_singleton()->load(AUDIO + intro);
    if (!first.is_valid()) return;
    music->set_meta("loop_path", AUDIO + repeat);
    music->set_stream(first);
    music->set_volume_linear(1);
    music->connect("finished", Callable(this, "on_music_finished"));
    music->play();
}

void ClientScreen::on_music_finished() {
    if (!music || !music->has_meta("loop_path")) return;
    const String path = music->get_meta("loop_path");
    Ref<AudioStream> repeat = ResourceLoader::get_singleton()->load(path);
    if (repeat.is_valid()) {
        music->set_stream(repeat);
        music->play();
    }
}

void ClientScreen::stop_music() {
    if (!music) return;
    music->stop();
    music->set_stream(Ref<AudioStream>());
}

void ClientScreen::play_sfx(const String &path) {
    Ref<AudioStream> clip = ResourceLoader::get_singleton()->load(AUDIO + path);
    if (!clip.is_valid()) return;
    if (!sfx) {
        sfx = memnew(AudioStreamPlayer);
        add_child(sfx);
    }
    sfx->set_stream(clip);
    sfx->play();
}

void ClientScreen::build_startup() {
    auto *background = memnew(ColorRect);
    background->set_name("StartupBlack");
    stage->add_child(background);
    background->set_color(Color(0, 0, 0));
    background->set_size(Vector2(W, H));
    background->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    const bool core = native_mode >= 6 && native_mode <= 8;
    const float fit = 1.5f;
    wreath = image(stage, "Wreath", core ? "startup/start_wreath_core.png" : "startup/start_wreath.png",
        (core ? 8.f : 0.f) * fit, (core ? -85.f : -30.f) * fit);
    icon = image(stage, "Icon", core ? "startup/start_icon_core.png" : "startup/start_icon.png", 0, 0);
    for (TextureRect *sprite : {wreath, icon}) {
        if (!sprite) continue;
        sprite->set_pivot_offset(sprite->get_size() / 2);
        sprite->set_scale(Vector2(.5f * fit, .5f * fit));
        set_alpha(sprite, 0);
    }
    content::warmup_partner_assets_begin();
    startup_begin_local_sweep();
    startup_warmup_done = false;
    startup_media_prepared = false;
    startup_content_started = false;
    startup_content_waiting = false;
    startup_loading_fading = false;
    startup_loading_fade_seconds = 0;
    startup_base_url = String(ProjectSettings::get_singleton()->get_setting("unfalsus/api_base_url", "")).strip_edges();
    const String environment = OS::get_singleton()->get_environment("UNFALSUS_API_BASE_URL");
    if (!environment.is_empty()) startup_base_url = environment.strip_edges();
    startup_base_url = arcapi::resolve_base_url(startup_base_url);
    if (!startup_base_url.begins_with("http://") && !startup_base_url.begins_with("https://"))
        UtilityFunctions::push_error("Startup Arcapi host could not be resolved");
    video = memnew(ClientVideo);
    video->set_name("IntroVideo");
    video->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    attach_startup_video(this, video);
    video->hide();
    title_video = memnew(ClientVideo);
    title_video->set_name("PreparedTitleVideo");
    title_video->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    attach_startup_video(this, title_video);
    title_video->set_visible(false);
    startup_media_prepared = false;
    startup_preload_index = 0;
    ClientTransition::get_or_create(get_tree());
}

void ClientScreen::begin_intro() {
    if (startup_state != 0) return;
    startup_state = 1;
    clock = 0;
    const bool intro_ready = video->play("res://assets/resources/video/startup/intro.mp4", false, true);
    if (!intro_ready) {
        startup_state = 0;
        startup_media_prepared = false;
        startup_warmup_done = false;
        startup_loading_fading = false;
        UtilityFunctions::push_error("Startup intro could not start; title transition cancelled");
        return;
    }
    if (Node *black = stage->find_child("StartupBlack", false, false))
        if (auto *item = Object::cast_to<Control>(black)) item->set_visible(false);
    video->show();
    // Sync music start with video start for better A/V sync
    if (music == nullptr || !music->is_playing()) {
        play_music("bgm/title_full.ogg", "bgm/title_loop.ogg");
    }
    video->start();
    // Ensure clock resets after both media start for accurate sync
    clock = 0;
}

void ClientScreen::begin_title() {
    if (startup_state != 1) return;
    startup_state = 2;
    clock = 0;
    if (!title_video) return;
    if (OS::get_singleton()->get_name() == "Windows" && !title_video->is_prepared()) {
        if (!title_video->prepare("res://assets/resources/video/startup/title.mp4", true)) {
            UtilityFunctions::push_error("Title video could not be prepared");
            return;
        }
    }
    if (!title_art) {
        // Match StartupVisual.layoutTitleLike(): csbY=124 on the 720-high
        // design canvas, using the centered Godot canvas coordinates.
        title_art = image(stage, "TitleArtwork", "startup/1080/title.png", 0, -354);
        if (title_art) {
            title_art->set_z_index(1);
            title_art->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            title_art->hide();
        }
    }
    const float peak = native_mode == 1 ? 1.f : 124.f / 255.f;
    flash = memnew(ColorRect);
    flash->set_name("WhiteFlash");
    flash->set_z_index(100);
    flash->set_color(Color(1, 1, 1));
    flash->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    add_child(flash);
    flash->set_size(get_size());
    set_alpha(flash, 0);
    layout();
    video->set_native_white(0);
}

void ClientScreen::mark_content_ready() { content_ready = true; }

void ClientScreen::_input(const Ref<InputEvent> &event) {
    // Handle intro video skip on click/touch
    if (startup_state == 1 && event.is_valid()) {
        bool should_skip = false;
        Ref<InputEventMouseButton> mouse = event;
        Ref<InputEventScreenTouch> touch = event;
        if (mouse.is_valid() && mouse->is_pressed() && mouse->get_button_index() == MOUSE_BUTTON_LEFT) {
            should_skip = true;
        } else if (touch.is_valid() && touch->is_pressed()) {
            should_skip = true;
        }
        if (should_skip) {
            // Skip to title video and seek music to 9.9s
            if (music && music->is_playing()) {
                music->seek(9.9);
            }
            begin_title();
            return;
        }
    }
    
    ScrollContainer *scroll = dialog_view_all ? dialog_all_list : dialog_list;
    if (!dialog_active || dialog_closing || !scroll || !event.is_valid()) {
        partner_scroll_pointer_active = false;
        partner_scroll_velocity = 0;
        partner_scroll_inertia_list = nullptr;
        partner_scroll_overscroll = 0;
        partner_scroll_bounce_start = 0;
        return;
    }

    Vector2 position;
    bool pressed = false;
    bool released = false;
    bool motion = false;
    Ref<InputEventMouseButton> mouse_button = event;
    Ref<InputEventMouseMotion> mouse_motion = event;
    Ref<InputEventScreenTouch> screen_touch = event;
    Ref<InputEventScreenDrag> screen_drag = event;

    if (mouse_button.is_valid() && mouse_button->get_button_index() == MOUSE_BUTTON_LEFT) {
        position = mouse_button->get_position();
        pressed = mouse_button->is_pressed();
        released = !pressed;
    } else if (mouse_motion.is_valid() && partner_scroll_pointer_active && !partner_scroll_touch) {
        position = mouse_motion->get_position();
        motion = true;
    } else if (screen_touch.is_valid()) {
        position = screen_touch->get_position();
        pressed = screen_touch->is_pressed();
        released = !pressed;
    } else if (screen_drag.is_valid() && partner_scroll_pointer_active && partner_scroll_touch) {
        position = screen_drag->get_position();
        motion = true;
    } else {
        return;
    }

    if (pressed) {
        if (partner_scroll_pointer_active) return;
        if (!scroll->get_global_rect().has_point(position)) return;
        partner_scroll_velocity = 0;
        partner_scroll_inertia_list = nullptr;
        partner_scroll_bounce_start = 0;
        partner_scroll_bounce_elapsed = 0;
        partner_scroll_samples.clear();
        partner_scroll_pointer_active = true;
        partner_scroll_dragging = false;
        partner_scroll_touch = screen_touch.is_valid();
        partner_scroll_pointer_start = position;
        partner_scroll_pointer_last = position;
        partner_scroll_last_motion = Time::get_singleton()->get_ticks_usec() / 1000000.0;
        return;
    }
    if (released) {
        if (!partner_scroll_pointer_active || partner_scroll_touch != screen_touch.is_valid()) return;
        if (partner_scroll_dragging) {
            partner_scroll_suppress_click = true;
            partner_scroll_clear_suppress = 2;
            const double now = Time::get_singleton()->get_ticks_usec() / 1000000.0;
            if (std::abs(partner_scroll_overscroll) > .01f) {
                partner_scroll_bounce_start = partner_scroll_overscroll;
                partner_scroll_bounce_elapsed = 0;
                partner_scroll_inertia_list = scroll;
            } else if (now - partner_scroll_last_motion < .12 && !partner_scroll_samples.empty()) {
                float distance = 0, duration = 0;
                for (const auto &sample : partner_scroll_samples) {
                    distance += sample.first;
                    duration += sample.second;
                }
                if (duration > 0 && duration < .5f) {
                    // Cocos 3.8.8 ScrollView defaults: brake=.5, movement factor=.7, quint-out.
                    partner_scroll_velocity = distance * .5f / duration;
                    const float travel = float(std::max(0.0,
                        scroll->get_v_scroll_bar()->get_max() - scroll->get_v_scroll_bar()->get_page()));
                    const float attenuation = .5f / (1.f + travel * .000014f + travel * travel * .000000008f);
                    const float projected = std::copysign(travel * attenuation * .5f, partner_scroll_velocity);
                    const float initial = partner_scroll_velocity * .7f;
                    if (std::abs(initial) >= 1.f) {
                        float factor = std::abs(projected / initial);
                        partner_scroll_distance = factor > 7.f
                            ? initial * (std::sqrt(factor) + 1.f) : projected + initial;
                        partner_scroll_duration = std::sqrt(std::sqrt(std::abs(partner_scroll_velocity) / 5.f));
                        if (factor > 3.f) partner_scroll_duration *= 3.f;
                        partner_scroll_start = scroll->get_v_scroll();
                        partner_scroll_elapsed = 0;
                        partner_scroll_inertia_list = scroll;
                    }
                }
            }
        }
        partner_scroll_pointer_active = false;
        partner_scroll_dragging = false;
        return;
    }
    if (!motion || !partner_scroll_pointer_active) return;

    if (!partner_scroll_dragging && partner_scroll_pointer_start.distance_to(position) >= 12.f)
        partner_scroll_dragging = true;
    if (partner_scroll_dragging) {
        const float scale = std::max(.001f, scroll->get_global_transform().get_scale().y);
        float movement = -(position.y - partner_scroll_pointer_last.y) / scale;
        if (partner_scroll_overscroll * movement > 0) {
            const float recovery = std::copysign(std::min(std::abs(movement),
                std::abs(partner_scroll_overscroll) * 2.f), movement);
            partner_scroll_overscroll -= recovery * .5f;
            movement -= recovery;
        }
        const int before = scroll->get_v_scroll();
        scroll->set_v_scroll(before + int(std::round(movement)));
        const float overflow = before + movement - scroll->get_v_scroll();
        if (std::abs(overflow) >= 1.f)
            partner_scroll_overscroll = std::clamp(partner_scroll_overscroll - overflow * .5f, -100.f, 100.f);
        if (dialog_visual_content) dialog_visual_content->set_position(Vector2(0, partner_scroll_overscroll));
        const double now = Time::get_singleton()->get_ticks_usec() / 1000000.0;
        if (scroll->get_v_scroll() != before && std::abs(movement) >= .01f) {
            const float dt = std::max(float(now - partner_scroll_last_motion), 1.f / 240.f);
            partner_scroll_samples.emplace_back(movement, dt);
            if (partner_scroll_samples.size() > 5) partner_scroll_samples.erase(partner_scroll_samples.begin());
        } else {
            partner_scroll_samples.clear();
        }
        partner_scroll_last_motion = now;
        get_viewport()->set_input_as_handled();
    }
    partner_scroll_pointer_last = position;
}

void ClientScreen::_unhandled_input(const Ref<InputEvent> &event) {
    Ref<InputEventKey> key = event;
    if (key.is_valid() && key->is_pressed() && !key->is_echo()
            && key->get_keycode() == KEY_ESCAPE) {
        // Android's system back button is delivered as Escape. Consume it so
        // Godot does not fall through to the default application quit action.
        get_viewport()->set_input_as_handled();
        return;
    }
    if (scene_kind != 0 || startup_state != 3 || title_time < 2 || switching) return;
    if (!content_ready) {
        if (startup_content_waiting) {
            startup_content_started = false;
            startup_start_content_check();
        }
        return;
    }
    Ref<InputEventMouseButton> mouse = event;
    Ref<InputEventScreenTouch> touch = event;
    if ((mouse.is_valid() && mouse->is_pressed() && mouse->get_button_index() == MOUSE_BUTTON_LEFT)
        || (touch.is_valid() && touch->is_pressed()) || event->is_action_pressed("ui_accept")) {
        switching = true;
        ClientTransition::get_or_create(get_tree())->switch_to("res://Godot/Scenes/MainMenu.tscn");
    }
}

void ClientScreen::on_menu_action(int action) {
    if (switching) return;
    if (action == 1) {
        play_sfx("sfx/06_LesserButtonClickV2.ogg");
        begin_konzetsu_tasks_request();
    } else if (action == 2) {
        play_sfx("sfx/08_TimelineNodeClick.ogg");
    }
}

void ClientScreen::return_to_menu() {
    // Remove clock restriction for offline preview mode
    if (switching || konzetsu_pending_clear_mask || (konzetsu_dialog && konzetsu_dialog->is_visible())) return;
    switching = true;
    play_sfx("konzetsu/ui_close.wav");
    ClientTransition::get_or_create(get_tree())->switch_to("res://Godot/Scenes/MainMenu.tscn");
}

void ClientScreen::begin_konzetsu_tasks_request() {
    if (switching || konzetsu_task_request_pending || !konzetsu_http || !konzetsu_task_busy) return;
    konzetsu_previous_tasks.clear();
    const Variant previous = get_tree()->get_meta("konzetsu_daily_tasks", Variant());
    if (previous.get_type() == Variant::ARRAY) konzetsu_previous_tasks = Array(previous).duplicate(true);
    konzetsu_task_error = "";
    konzetsu_task_busy->show();
    stage->move_child(konzetsu_task_busy, stage->get_child_count() - 1);
    konzetsu_task_busy->get_node<Control>("Dimmer")->set_modulate(Color(1, 1, 1, 0));
    konzetsu_task_activity.begin(konzetsu_task_busy->get_node<Control>("ActivityIcon"));
    konzetsu_task_finishing = false;
    const String base = arcapi::resolve_base_url(
        String(ProjectSettings::get_singleton()->get_setting("unfalsus/api_base_url", "")).strip_edges());
    String request_base = base;
    const String environment = OS::get_singleton()->get_environment("UNFALSUS_API_BASE_URL");
    if (!environment.is_empty()) request_base = arcapi::resolve_base_url(environment.strip_edges());
    Ref<ConfigFile> config;
    config.instantiate();
    if (config->load("user://startup.cfg") != OK || request_base.is_empty()) {
        konzetsu_task_error = U"无法连接至服务器";
        konzetsu_task_activity.end(konzetsu_task_busy->get_node<Control>("ActivityIcon"));
        konzetsu_task_finishing = true;
        return;
    }
    const String token = String(config->get_value("auth", "token", "")).trim_prefix("Bearer ").trim_prefix("bearer ");
    if (token.is_empty()) {
        konzetsu_task_error = U"请先登录";
        konzetsu_task_activity.end(konzetsu_task_busy->get_node<Control>("ActivityIcon"));
        konzetsu_task_finishing = true;
        return;
    }
    PackedStringArray headers;
    headers.push_back("Authorization: Bearer " + token);
    headers.push_back("AppVersion: " + String(ProjectSettings::get_singleton()->get_setting("application/config/version", "1.0.0")));
    headers.push_back("Accept: application/json");
    konzetsu_task_request_pending = true;
    const Error err = konzetsu_http->request(request_base.trim_suffix("/") + "/user/task", headers);
    if (err != OK) {
        konzetsu_task_request_pending = false;
        konzetsu_task_error = U"无法连接至服务器";
        konzetsu_task_activity.end(konzetsu_task_busy->get_node<Control>("ActivityIcon"));
        konzetsu_task_finishing = true;
    }
}

void ClientScreen::set_konzetsu_tasks(const Array &tasks) {
    if (tasks.size() != 5) return;
    konzetsu_tasks = tasks;
    konzetsu_tasks_loaded = true;
    refresh_konzetsu_hub_visuals();
}

void ClientScreen::step_konzetsu_task_activity(double delta) {
    if (!konzetsu_task_busy || !konzetsu_task_busy->is_visible()) return;
    Control *icon = konzetsu_task_busy->get_node<Control>("ActivityIcon");
    const bool done = konzetsu_task_activity.step(icon, delta);
    const float alpha = std::clamp(float(konzetsu_task_finishing
        ? konzetsu_task_dim_from * (1.0 - konzetsu_task_activity.fade_seconds / .2)
        : .5 * std::sin(std::clamp(konzetsu_task_activity.seconds / .7, 0.0, 1.0) * 1.570796327)), 0.f, 1.f);
    konzetsu_task_busy->get_node<Control>("Dimmer")->set_modulate(Color(1, 1, 1, alpha));
    if (!konzetsu_task_finishing || !done) return;
    konzetsu_task_finishing = false;
    konzetsu_task_busy->hide();
    if (!konzetsu_task_error.is_empty()) {
        show_partner_message(konzetsu_task_error);
        konzetsu_task_error = "";
    }
}

void ClientScreen::konzetsu_tasks_completed(int result, int response_code, const PackedStringArray &, const PackedByteArray &body) {
    konzetsu_task_request_pending = false;
    const auto fail = [this](const String &message) {
        konzetsu_task_error = message;
        konzetsu_task_dim_from = konzetsu_task_busy->get_node<Control>("Dimmer")->get_modulate().a;
        konzetsu_task_activity.end(konzetsu_task_busy->get_node<Control>("ActivityIcon"));
        konzetsu_task_finishing = true;
    };
    if (result != HTTPRequest::RESULT_SUCCESS) {
        fail(String(U"任务请求连接失败（网络结果：") + String::num_int64(result) + U"）");
        return;
    }
    const String text = body.is_empty() ? String()
        : String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).trim_prefix(String(U"\uFEFF")).strip_edges();
    Ref<JSON> json;
    json.instantiate();
    const Error parse_error = json->parse(text);
    const Variant parsed = parse_error == OK ? json->get_data() : Variant();
    if (response_code < 200 || response_code >= 300) {
        String message = String(U"任务请求失败（HTTP ") + String::num_int64(response_code) + U"）";
        if (parsed.get_type() == Variant::DICTIONARY) {
            const Dictionary response = parsed;
            if (response.has("error_code")) message += String(U"\n服务端 error_code：") + String(response["error_code"]);
        }
        message += String(U"\n请求版本：") + String(ProjectSettings::get_singleton()->get_setting("application/config/version", "1.0.0"));
        fail(message);
        return;
    }
    if (parse_error != OK) {
        fail(String(U"任务响应 JSON 解析失败，第 ") + String::num_int64(json->get_error_line())
            + String(U" 行：") + json->get_error_message());
        return;
    }
    if (parsed.get_type() != Variant::DICTIONARY) {
        fail(U"任务响应格式错误：JSON 顶层应为对象");
        return;
    }
    const Dictionary root = parsed;
    if (root.has("success") && root["success"] == Variant(false)) {
        fail(String(U"服务端返回任务失败，error_code：") + String(root.get("error_code", U"未提供")));
        return;
    }
    const Variant value = root.get("value", Variant());
    if (value.get_type() != Variant::DICTIONARY) {
        fail(U"任务响应格式错误：缺少 value 对象");
        return;
    }
    const Variant tasks = Dictionary(value).get("tasks", Variant());
    if (tasks.get_type() != Variant::ARRAY) {
        fail(U"任务响应格式错误：缺少 value.tasks 数组");
        return;
    }
    if (Array(tasks).size() != 5) {
        fail(String(U"任务响应数量错误：预期 5 项，实际 ") + String::num_int64(Array(tasks).size()) + U" 项");
        return;
    }
    for (int i = 0; i < 5; ++i) {
        if (Array(tasks)[i].get_type() != Variant::DICTIONARY) {
            fail(String(U"任务响应格式错误：第 ") + String::num_int64(i + 1) + U" 项应为对象");
            return;
        }
    }
    get_tree()->set_meta("konzetsu_pending_clear_mask", konzetsu_new_clear_mask(konzetsu_previous_tasks, Array(tasks)));
    get_tree()->set_meta("konzetsu_daily_tasks", Array(tasks).duplicate(true));
    switching = true;
    ClientTransition::get_or_create(get_tree())->switch_to("res://Godot/Scenes/Konzetsu.tscn");
}

void ClientScreen::animate_character_select(double delta) {
    if (menu_dimmer) {
        dialog_dim_seconds += delta;
        const float t = std::clamp(float(dialog_dim_seconds / .5), 0.f, 1.f);
        set_alpha(menu_dimmer, dialog_dim_start + (dialog_dim_target - dialog_dim_start) * std::sin(t * 1.570796327f));
    }
    if (!character_dialog || !dialog_active) return;
    dialog_seconds += delta;
    if (dialog_all) {
        dialog_layout->set_visible(!dialog_view_all || dialog_layout->get_modulate().a > .001f);
        dialog_all->set_visible(dialog_view_all || dialog_all->get_modulate().a > .001f);
    }
    if (dialog_closing) {
        const float t = std::clamp(float(dialog_seconds / (20. / 60.)), 0.f, 1.f);
        const float chrome = 1 - std::sin(t * 1.570796327f);
        set_alpha(dialog_back, chrome);
        set_alpha(dialog_chrome, chrome);
        set_alpha(Object::cast_to<Control>(character_dialog->get_node_or_null("OverlayClose")), chrome);
        const float menu_fade = std::clamp(float(dialog_seconds / .3), 0.f, 1.f);
        set_alpha(art["PartnerIllustration"], std::sin(menu_fade * 1.570796327f));
        set_alpha(art["SkillBacking"], menu_fade);
        const float u = std::clamp(float(dialog_seconds / .15), 0.f, 1.f);
        const float eased = 1 - std::pow(1 - u, 3);
        for (size_t i = 0; i < dialog_cells.size(); ++i) {
            const float delay = std::min(float(i) * .05f, 1.f);
            const float travel = (25.f + delay / .05f * 3.f) * 1.5f;
            dialog_cells[i]->set_position(dialog_rest[i] + Vector2(0, travel * eased));
            set_alpha(dialog_cells[i], 1 - eased);
        }
        if (dialog_seconds >= 20. / 60.) {
            character_dialog->hide();
            dialog_active = false;
            dialog_view_tween.unref();
            dialog_portrait_seconds = 0;
            dialog_closing = false;
            partner_modal = nullptr;
            partner_details_open = false;
            partner_detail_busy = false;
            partner_preview_level = -1;
            partner_shown_id = -1;
            partner_buy_id = -1;
            partner_stat_id = -2;
            partner_stat_seconds = 1;
            dialog_pending_character = -1;
            dialog_portrait_slide = true;
        }
        return;
    }
    const float t = std::clamp(float(dialog_seconds / (20. / 60.)), 0.f, 1.f);
    const float chrome = std::sin(t * 1.570796327f);
    set_alpha(dialog_back, chrome);
    set_alpha(dialog_chrome, chrome);
    set_alpha(Object::cast_to<Control>(character_dialog->get_node_or_null("OverlayClose")), chrome);
    const float hub_fade = std::clamp(float(dialog_seconds / .3), 0.f, 1.f);
    set_alpha(art["PartnerIllustration"], 1 - std::sin(hub_fade * 1.570796327f));
    set_alpha(art["SkillBacking"], 1 - hub_fade);

    if (dialog_portrait && dialog_portrait_slide && !partner_details_open) {
        dialog_portrait_seconds += delta;
        const float u = std::clamp(float(dialog_portrait_seconds / .45), 0.f, 1.f);
        const float eased = 1 - std::pow(1 - u, 3);
        const Vector2 size = dialog_portrait->get_size();
        const float target = content::unlocked(dialog_preview_character) ? 1.f : 153.f / 255.f;
        dialog_portrait->set_position(canvas_pos(350 + 225 * (1 - eased), -225, size.x, size.y));
        set_alpha(dialog_portrait, eased * target);
    }
    tick_partner_stats(delta);
    for (size_t i = 0; i < dialog_cells.size(); ++i) {
        Control *cell = dialog_cells[i];
        const float delay = std::min(float(i) * .05f, 1.f);
        const float total = delay + .15f;
        const float progress = std::clamp(float(dialog_seconds) / total, 0.f, 1.f);
        const float inner = (1 - std::pow(1 - progress, 3)) * total;
        const float visible = std::clamp((inner - delay) / .15f, 0.f, 1.f);
        const float travel = (25.f + delay / .05f * 3.f) * 1.5f;
        cell->set_position(dialog_rest[i] + Vector2(0, travel * (1 - visible)));
        set_alpha(cell, visible);
    }
}

void ClientScreen::_exit_tree() {
#if defined(_WIN32)
    uninstall_aspect_window_proc();
#endif
    if (video) video->stop();
    if (title_video) title_video->stop();
    stop_music();
    if (sfx) {
        sfx->stop();
        sfx->set_stream(Ref<AudioStream>());
    }
    startup_clear_overlay();
    if (startup_http) startup_http->cancel_request();
    startup_abort_seals();
}

void ClientScreen::_process(double delta) {
    if (scene_kind == 2 && konzetsu_preload_image_index < konzetsu_preload_images.size()) {
        const String &relative = konzetsu_preload_images[konzetsu_preload_image_index++];
        if (!content::cached_image(relative).is_valid()) content::image(relative);
    }
    if (konzetsu_gallery_strip && konzetsu_gallery_cycle_width > 0) {
        Vector2 position = konzetsu_gallery_strip->get_position();
        position.x -= float(delta) * konzetsu_gallery_cycle_width / 45.f;
        if (position.x <= -konzetsu_gallery_cycle_width) position.x += konzetsu_gallery_cycle_width;
        konzetsu_gallery_strip->set_position(position);
    }
    if (konzetsu_gallery_strip) {
        bool loaded_one_gallery_image = false;
        for (int i = 0; i < konzetsu_gallery_strip->get_child_count(); ++i) {
            auto *tile = Object::cast_to<TextureRect>(konzetsu_gallery_strip->get_child(i));
            if (!tile || tile->get_texture().is_valid()) continue;
            const String path = String(tile->get_meta("gallery_path", ""));
            Ref<Texture2D> texture = content::cached_image(path);
            if (!texture.is_valid() && !loaded_one_gallery_image
                    && !bool(tile->get_meta("gallery_attempted", false))) {
                UtilityFunctions::print(String("[gallery] attempt slot=")
                    + String::num_int64(int(tile->get_meta("gallery_slot", 0)))
                    + String(" path=") + path);
                tile->set_meta("gallery_attempted", true);
                loaded_one_gallery_image = true;
                texture = content::image(path);
            }
            if (texture.is_valid() && !bool(tile->get_meta("gallery_loaded", false))) {
                const int slot = int(tile->get_meta("gallery_slot", 0));
                int copies = 0;
                for (int j = 0; j < konzetsu_gallery_strip->get_child_count(); ++j) {
                    auto *copy = Object::cast_to<TextureRect>(konzetsu_gallery_strip->get_child(j));
                    if (copy && String(copy->get_meta("gallery_path", "")) == path) {
                        ++copies;
                        copy->set_texture(texture);
                        copy->set_meta("gallery_loaded", true);
                        copy->set_modulate(Color(1, 1, 1, 0));
                        Ref<Tween> fade = copy->create_tween();
                        fade->tween_property(copy, "modulate:a", 1.f, .35)
                            ->set_delay(float(slot) * .1f);
                    }
                }
                UtilityFunctions::print(String("[gallery] loaded slot=") + String::num_int64(slot)
                    + String(" path=") + path + String(" copies=") + String::num_int64(copies));
            }
            else if (!texture.is_valid() && (content::image_failed(path)
                    || bool(tile->get_meta("gallery_attempted", false)))
                    && !konzetsu_gallery_unused.is_empty()) {
                const String replacement_song = konzetsu_gallery_unused[0];
                konzetsu_gallery_unused.remove_at(0);
                const String replacement = String("songs/dl_") + replacement_song + "/1080_base.jpg";
                UtilityFunctions::printerr(String("[gallery] replace slot=")
                    + String::num_int64(int(tile->get_meta("gallery_slot", 0)))
                    + String(" failed=") + path + String(" replacement=") + replacement);
                const int slot = int(tile->get_meta("gallery_slot", 0));
                for (int j = 0; j < konzetsu_gallery_strip->get_child_count(); ++j) {
                    auto *copy = Object::cast_to<TextureRect>(konzetsu_gallery_strip->get_child(j));
                    if (!copy || int(copy->get_meta("gallery_slot", -1)) != slot
                            || String(copy->get_meta("gallery_path", "")) != path) continue;
                    copy->set_meta("gallery_path", replacement);
                    copy->set_meta("gallery_loaded", false);
                    copy->set_meta("gallery_attempted", false);
                    copy->set_meta("gallery_replacement_requested", false);
                }
            }
        }
    }
    if (scene_kind == 1 && hub_portrait_slide_seconds < .45) {
        hub_portrait_slide_seconds = std::min(.45, hub_portrait_slide_seconds + delta);
        const float remaining = 1.f - float(hub_portrait_slide_seconds / .45);
        position_hub_portrait(225.f * remaining * remaining * remaining);
    }
    step_partner_purchase_activity(delta);
    step_konzetsu_task_activity(delta);
    if (switching && music && music->is_playing()) {
        const float remaining = std::max(0.f, music->get_volume_linear() - float(delta / .85));
        music->set_volume_linear(remaining);
    }
    if (scene_kind == 1 && !menu_assets_ready) {
        menu_assets_ready = content::warmup_partner_assets_step(1);
        if (menu_assets_ready) scene_assets_ready = true;
    }
    if (partner_scroll_clear_suppress > 0 && --partner_scroll_clear_suppress == 0)
        partner_scroll_suppress_click = false;
    ScrollContainer *active_scroll = dialog_view_all ? dialog_all_list : dialog_list;
    if (!character_dialog || dialog_closing
            || (!partner_scroll_pointer_active && active_scroll != partner_scroll_inertia_list)) {
        partner_scroll_velocity = 0;
        partner_scroll_inertia_list = nullptr;
        partner_scroll_overscroll = 0;
        partner_scroll_bounce_start = 0;
        partner_scroll_bounce_elapsed = 0;
        if (dialog_visual_content) dialog_visual_content->set_position(Vector2());
    } else if (!partner_scroll_pointer_active && std::abs(partner_scroll_bounce_start) > .01f) {
        partner_scroll_bounce_elapsed = std::min(partner_scroll_bounce_elapsed + float(delta), 1.f);
        const float remaining = 1.f - partner_scroll_bounce_elapsed;
        partner_scroll_overscroll = partner_scroll_bounce_start * remaining * remaining * remaining * remaining * remaining;
        if (dialog_visual_content) dialog_visual_content->set_position(Vector2(0, partner_scroll_overscroll));
        if (partner_scroll_bounce_elapsed >= 1.f) {
            partner_scroll_bounce_start = 0;
            partner_scroll_overscroll = 0;
            partner_scroll_inertia_list = nullptr;
        }
    } else if (!partner_scroll_pointer_active && partner_scroll_duration > 0) {
        partner_scroll_elapsed = std::min(partner_scroll_elapsed + float(delta), partner_scroll_duration);
        const float remaining = 1.f - partner_scroll_elapsed / partner_scroll_duration;
        const float eased = 1.f - remaining * remaining * remaining * remaining * remaining;
        const int next = int(std::round(partner_scroll_start + partner_scroll_distance * eased));
        active_scroll->set_v_scroll(next);
        if (active_scroll->get_v_scroll() != next) {
            partner_scroll_overscroll = std::clamp(float(active_scroll->get_v_scroll() - next) * .5f, -100.f, 100.f);
            partner_scroll_bounce_start = partner_scroll_overscroll;
            partner_scroll_bounce_elapsed = 0;
            if (dialog_visual_content) dialog_visual_content->set_position(Vector2(0, partner_scroll_overscroll));
            partner_scroll_duration = 0;
        } else if (partner_scroll_elapsed >= partner_scroll_duration) {
            partner_scroll_duration = 0;
            partner_scroll_inertia_list = nullptr;
        }
    }
    if (scene_kind == 0) {
        clock += delta;
        startup_animate(delta);
        startup_step_warmup();
        scene_assets_ready = startup_warmup_done;
        startup_step_activity(delta);
        startup_step_downloads(delta);
        startup_step_local_sweep();
        startup_step_manifest_diff();
        if (startup_busy && startup_request_kind == 0 && startup_manifest_index >= startup_missing.size()
                && startup_verify_index < startup_manifest.size()) startup_verify_next();
        if (startup_state == 0) {
            if (!startup_loading_fading && startup_warmup_done && startup_sweep_done && startup_media_prepared) {
                startup_loading_fading = true;
                startup_loading_fade_seconds = std::max(1.0, std::ceil(clock));
            }
            const float fade_in = std::clamp(float(clock / .5), 0.f, 1.f);
            const float fade_out = startup_loading_fading
                ? 1 - std::clamp(float((clock - startup_loading_fade_seconds) / .3), 0.f, 1.f) : 1.f;
            const float fade = fade_in * fade_out;
            set_alpha(icon, fade);
            set_alpha(wreath, fade);
            if (icon) {
                const float fit = 1.5f;
                const float rest = .5f * fit;
                const float squash = .01f * fit;
                const float phase = float(std::fmod(clock, 1.0));
                const float step = phase < .5f ? phase * 2.f : (phase - .5f) * 2.f;
                const float eased = phase < .5f ? (step <= 0.f ? 0.f : std::pow(2.f, 10.f * (step - 1.f)))
                    : (step >= 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * step));
                const float width = phase < .5f ? rest - (rest - squash) * eased : squash + (rest - squash) * eased;
                icon->set_scale(Vector2(width, rest));
            }
            if (startup_loading_fading && clock >= startup_loading_fade_seconds) {
                if (!music || !music->is_playing()) play_music("bgm/title_full.ogg", "bgm/title_loop.ogg");
            }
            if (startup_loading_fading && clock >= startup_loading_fade_seconds + .3) begin_intro();
        } else if (startup_state == 1) {
            if (clock >= 6 && video && video->duration() > .5
                    && video->position() >= video->duration() - 7. / 60.) begin_title();
            else if (clock >= 20 || (video && video->has_finished())) {
                UtilityFunctions::push_error("Startup video did not report completion before timeout");
                begin_title();
            }
        } else if (startup_state == 2) {
            if (title_video && (OS::get_singleton()->get_name() != "Windows" || title_video->is_prepared())) {
                if (video) {
                    video->stop();
                    video->queue_free();
                }
                video = title_video;
                title_video = nullptr;
                if (!video) {
                    video = memnew(ClientVideo);
                    video->set_name("TitleVideo");
                    video->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    attach_startup_video(this, video);
                } else {
                    video->set_name("TitleVideo");
                    video->set_visible(true);
                    attach_startup_video(this, video);
                    if (OS::get_singleton()->get_name() == "iOS")
                        video->play("res://assets/resources/video/startup/title.mp4", true);
                    else if (video->is_prepared())
                        video->start();
                    else
                        video->play("res://assets/resources/video/startup/title.mp4", true);
                }
                if (title_art) title_art->show();
                startup_state = 3;
                clock = 0;
                content_ready = false;
                startup_start_content_check();
            }
        } else if (startup_state == 3) {
            title_time += delta;
            if (flash && clock <= 47. / 60.) {
                const float peak = native_mode == 1 ? 1.f : 124.f / 255.f;
                const float alpha = clock < 7. / 60. ? float(clock / (7. / 60.))
                    : 1.f - std::clamp(float((clock - 7. / 60.) / (40. / 60.)), 0.f, 1.f);
                set_alpha(flash, peak * alpha);
                if (video) video->set_native_white(flash->get_modulate().a);
            } else if (flash) {
                flash->queue_free();
                flash = nullptr;
            }
        }
        return;
    }

    if (scene_kind == 1) {
        animate_character_select(delta);
        if (!hub_music_started && !ClientTransition::covered()) {
            hub_music_started = true;
            play_music("bgm/mainmenu_full.ogg", "bgm/mainmenu_loop.ogg");
        }
        return;
    }
    if (scene_kind != 2) return;
    clock += delta;
    if (!show_loading) {
        show_loading = true;
        play_sfx("konzetsu/loading.wav");
    }
    const float first = std::clamp(float((clock - .65) / .4), 0.f, 1.f);
    const float second = std::clamp(float((clock - 1.05) / .4), 0.f, 1.f);
    const float hide = 1.f - std::clamp(float((clock - 3.35) / .8), 0.f, 1.f);
    const float hide_late = 1.f - std::clamp(float((clock - 3.35) / 1.), 0.f, 1.f);
    for (const char *name : {"CenterVortex", "BottomDiamond", "RingLarge", "RingMedium", "RingSmall", "CenterFrame"})
        set_alpha(art[name], first * hide);
    const float pulse_time = float(std::fmod(std::max(0.0, clock - 1.05), 1.2));
    const float pulse = 1.f - .4f * (pulse_time <= .6f ? pulse_time / .6f : (1.2f - pulse_time) / .6f);
    const float glow_at_fade = 14.f / 15; // Pulse opacity at the 3.35s fade boundary.
    set_alpha(art["BgGlow"], first * (clock < 3.35 ? pulse : glow_at_fade) * hide_late);
    for (const char *name : {"GaugeDeco", "GaugeBg", "LoadingRune", "GaugeFill"}) set_alpha(art[name], second * hide_late);
    set_alpha(art["LoadingBg"], hide);
    // Fade the button itself so revealing it never exposes opaque artwork.
    const float back_progress = 1.f - hide_late;
    set_alpha(art["HubBack"], back_progress * back_progress * (3.f - 2.f * back_progress));
    if (art["HubBack"]) art["HubBack"]->set_visible(clock >= 3.35);
    if (auto *clip = art["GaugeClip"]) {
        const float u = std::clamp(float((clock - 1.45) / 1.2), 0.f, 1.f);
        clip->set_size(Vector2(float(clip->get_meta("full_width")) * (1 - std::pow(1 - u, 3)), clip->get_size().y));
    }
    for (auto [name, period, clockwise] : {
            std::tuple<const char *, float, float>{"RingLarge", 90, 1}, {"RingMedium", 80, -1},
            {"RingSmall", 70, 1}, {"HubVortex1", 100, -1}, {"HubVortex2", 100, -1}}) {
        Control *node = art[name];
        if (node) node->set_rotation(node->get_rotation() + float(delta * clockwise * 6.283185307179586 / period));
    }
    if (clock >= 3.35 && !hub_music_started) {
        hub_music_started = true;
        play_music("bgm/hub_konzetsu.ogg", "bgm/hub_konzetsu_loop.ogg");
    }
    if (clock >= 4.35) {
        for (const char *name : {"LoadingBg", "CenterVortex", "BottomDiamond", "BgGlow", "RingLarge",
                "RingMedium", "RingSmall", "CenterFrame", "GaugeDeco", "GaugeBg", "GaugeFill", "LoadingRune"}) {
            if (art[name]) art[name]->set_visible(false);
        }
        if (TextureButton *back = Object::cast_to<TextureButton>(art["HubBack"]))
            back->set_disabled(false);
        if (!ClientTransition::covered() && konzetsu_preload_image_index >= konzetsu_preload_images.size())
            step_konzetsu_clear_animation(delta);
    }
}
