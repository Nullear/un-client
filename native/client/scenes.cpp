#include "client.hpp"
#include "content.hpp"
#include "arcapi.hpp"
#include "ui_font.hpp"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/callback_tweener.hpp>
#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/atlas_texture.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/nine_patch_rect.hpp>
#include <godot_cpp/classes/property_tweener.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace godot;

namespace {
static constexpr float W = 1920, H = 1080;
static bool menu_user_logged_in = false;
static bool menu_online_available = false;
static int64_t task_integer(const Dictionary &task, const Dictionary &state, const char *key, int64_t fallback = 0) {
    const Variant value = task.has(key) ? task[key] : state.get(key, Variant());
    return value.get_type() == Variant::NIL ? fallback : int64_t(value);
}

static Variant konzetsu_task_field(const Dictionary &task, const char *key) {
    if (task.has(key)) return task[key];
    const Dictionary req = task.get("req", Dictionary());
    return req.get(key, Variant());
}

static String konzetsu_task_song(const Dictionary &task) {
    return String(konzetsu_task_field(task, "song"));
}

static int konzetsu_task_difficulty(const Dictionary &task) {
    Variant value = konzetsu_task_field(task, "difficulty");
    if (value.get_type() == Variant::NIL) value = konzetsu_task_field(task, "diff");
    return int(value);
}

static bool konzetsu_daily_task_complete(const Dictionary &task) {
    const Dictionary state = task.get("state", Dictionary());
    if (bool(state.get("done", false))) return true;
    const String id = task.get("id", "");
    const int64_t current = task_integer(task, state, "current");
    const Dictionary req = task.get("req", Dictionary());
    const int64_t amount = task_integer(task, state, "amount",
        req.has("amount") ? int64_t(req["amount"]) : 0);
    const int64_t progress = task_integer(task, state, "progress",
        req.has("value") ? int64_t(req["value"]) : 0);
    if (id == "target_chart_perfect_or_clear_twice" || id == "target_chart_link_or_clear_twice"
            || id == "any_song_perfect_or_clear_twice") return current == 2;
    if (id == "target_chart_dual_difficulty_clear") return current == 3;
    if (id == "shiny_pure_total" || id == "daily_near_total" || id == "daily_exact_total"
            || id == "daily_break_total") return amount > 0 && current >= amount;
    if (id == "target_chart_score" || id == "target_chart_judgment_limit"
            || id == "daily_unplayed_song_clear") return current == 1;
    if (id == "challenge_16_clear")
        return current == 1 || bool(state.get("cleared", state.get("clear", false)));
    if (id == "daily_full_link_total" || id == "high_const_clear_total" || id == "daily_score_total")
        return progress > 0 && current >= progress;
    return false;
}

static bool konzetsu_all_daily_tasks_complete(const Array &tasks) {
    if (tasks.size() < 5) return false;
    for (int i = 0; i < 5; ++i) {
        if (tasks[i].get_type() != Variant::DICTIONARY
                || !konzetsu_daily_task_complete(Dictionary(tasks[i]))) return false;
    }
    return true;
}

static int konzetsu_saved_completion_count() {
    Ref<ConfigFile> snapshot;
    snapshot.instantiate();
    if (snapshot->load("user://konzetsu_tasks.cfg") != OK) return 0;
    const Variant value = snapshot->get_value("tasks", "snapshot", Variant());
    if (value.get_type() != Variant::ARRAY) return 0;
    const Array tasks = value;
    if (tasks.size() != 6 || tasks[5].get_type() != Variant::DICTIONARY
            || String(Dictionary(tasks[5]).get("id", "")) != "challenge_16_clear") return 0;
    int completed = 0;
    for (int i = 0; i < tasks.size(); ++i)
        if (tasks[i].get_type() == Variant::DICTIONARY
                && konzetsu_daily_task_complete(Dictionary(tasks[i]))) ++completed;
    return completed;
}

static Vector2 at(float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    return Vector2(W / 2 + x - w * ax, H / 2 - y - h * (1 - ay));
}

static Ref<Texture2D> load(const String &path) {
    return content::image(path);
}

static TextureRect *place(Control *parent, const String &name, const String &path,
        float x, float y, float w = 0, float h = 0, float ax = .5f, float ay = .5f) {
    Ref<Texture2D> tex = load(path);
    if (!tex.is_valid()) return nullptr;
    if (w <= 0) w = tex->get_width();
    if (h <= 0) h = tex->get_height();
    auto *node = memnew(TextureRect);
    node->set_name(name);
    parent->add_child(node);
    node->set_texture(tex);
    node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    node->set_stretch_mode(TextureRect::STRETCH_SCALE);
    node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    node->set_position(at(x, y, w, h, ax, ay));
    node->set_size(Vector2(w, h));
    return node;
}

static Label *caption(Control *parent, const String &name, const String &value,
        float x, float y, float w, int size, const Color &color, bool centered = false, bool baseline = false) {
    auto *label = memnew(Label);
    label->set_name(name);
    parent->add_child(label);
    const float lifted = y + (baseline ? size * .34f : 0);
    label->set_text(value);
    label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    label->add_theme_font_size_override("font_size", size);
    label->add_theme_color_override("font_color", color);
    Ref<Font> font = unfalsus_ui::prepare_font("VivoSans-Semibold-Full.ttf");
    if (font.is_valid()) label->add_theme_font_override("font", font);
    unfalsus_ui::place_ccc(label, Vector2(W / 2 + x, H / 2 - lifted), centered ? .5f : 0.f, .5f, w, size, 0);
    return label;
}

static TextureButton *art_button(Control *parent, const String &name, const String &normal,
        const String &pressed, float x, float y, float w, float h) {
    auto *node = memnew(TextureButton);
    node->set_name(name);
    parent->add_child(node);
    node->set_texture_normal(load(normal));
    if (!pressed.is_empty()) node->set_texture_pressed(load(pressed));
    node->set_ignore_texture_size(true);
    node->set_stretch_mode(TextureButton::STRETCH_SCALE);
    node->set_focus_mode(Control::FOCUS_NONE);
    node->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
    node->set_position(at(x, y, w, h));
    node->set_size(Vector2(w, h));
    return node;
}

struct Spec {
    const char *name;
    const char *file;
    float x, y, w, h;
    float depth;
    float ax = .5f, ay = .5f;
};

static Vector2 partner_cell_position(int index, bool all, float span) {
    constexpr float w = 169 * 1.005f, h = 170 * 1.005f, gap = 18;
    const int short_cols = all ? 8 : 2;
    const int long_cols = short_cols + 1;
    const int pair = short_cols + long_cols;
    const int remainder = index % pair;
    const bool long_row = remainder >= short_cols;
    const int column = long_row ? remainder - short_cols : remainder;
    const float pitch = w + 2 * gap;
    const float row = (long_row ? long_cols : short_cols) * w
        + ((long_row ? long_cols : short_cols) - 1) * gap * 2;
    const float x = all ? (span - row) / 2 + column * pitch
        : long_row ? 150.f + column * pitch : 150.f + gap + w * .5f + column * pitch;
    return Vector2(x, (all ? 90.f : 30.f) + (index / pair) * (h + 2 * gap)
        + (long_row ? h * .5f + gap : 0));
}
} // namespace

void ClientScreen::build_menu() {
    // These positions and sizes are the 3840x2160 HubScene values divided by two.
    catalog = content::characters();
    selected_character = content::selected();
    Dictionary selected_row = content::character(catalog, selected_character);
    const Spec sprites[] = {
        {"Background", "bg-home-regular-lights", 0, 0, 1920, 1080, 0},
        {"Grid", "grid", 0, 0, 1740, 910, -.01},
        {"OuterBracketsLeft", "background-outerbrackets", -459.625, -8, 906.25, 975.5, -.02},
        {"OuterBracketsRight", "background-outerbrackets", 446.625, -8, 906.25, 975.5, -.02},
        {"PlayerTagBacking", "backing-playertag", -833.5, 435, 54, 20, -.03},
        {"BottomBar", "background-bottombar", 0, -540, 1920, 17, -.05, .5, 0},
        {"CharacterColorStrip", "colorstrip-nia", -398.304, 0, 1099.5, 1080, -.12},
        {"CharacterSquares", "characterbacking-squares", -450.5, -95.5, 762.5, 651.5, -.11},
        {"PartnerIllustration", "", -330, -225, 0, 0, -.20},
        {"SkillBacking", "backing-skillicons", -410, -179, 814, 351.5, -.16},
        {"CharacterBracketBL", "characterframe-brackets-nia-bl", -777.5, -312.886, 218.5, 333, -.13},
        {"CharacterBracketTR", "characterframe-brackets-nia-tr", -37.403, 96.653, 136, 207.5, -.13},
        {"CharacterSwapIcon", "hub_character_swap_icon", -762.522, 39.719, 48, 25, -.25},
        {"MusicBacking", "backing-musicplay", 415, 295, 850, 250, -.3},
        {"MusicDropBacking", "musicplay-dropbacking", 416.42, 254.7, 801, 161.5, -.31},
        {"MusicDifficulty", "musicplay-diffback-evo", 199, 235, 360, 110, -.34},
        {"MusicEdge", "title-edgebar", .38, 381.925, 32.5, 55.5, -.36},
        {"MusicClearIcon", "icon-clear", 250.377, 215.305, 15, 16, -.4},
        {"MusicUnlockIcon", "icon-clear", 114.664, 215.305, 15, 16, -.4},
        {"ScenarioBacking", "backing-scenario", 455, 25, 770, 250, -.3},
        {"ScenarioArtwork", "packartwork_02", 460, -.5, 720, 160, -.3},
        {"ScenarioDropBacking", "scenario-dropbacking", 455.98, -16.05, 721, 161.5, -.3},
        {"ScenarioCoverEffectsNoTitle", "scenario-covereffects-notitle", 460, -.5, 720, 160, -.32},
        {"ScenarioEdge", "title-edgebar", 80.263, 112.188, 32.5, 55.5, -.47},
        {"CraftingLocked", "hidden-section", 492.358, -274.672, 696, 310, -.3},
        {"TopBar", "topbar-backing", 0, 540, 1920, 90, -6.01, .5, 1},
        {"TopTitleBacking", "topbar-titlebanner-backing", -393.5, 524.5, 350.5, 40, -6.01, .5, 1},
        {"HubButton", "btn-hub-disabled", -621.214, 509.904, 134, 54, -6.02},
        {"PrimaryTab", "tab-first-deselected", -478, 496, 191, 55.5, -6.09},
        {"SecondaryTab", "tab-following-deselected", -306.5, 496, 199.5, 55.5, -6.09},
    };
    std::vector<Spec> sorted(std::begin(sprites), std::end(sprites));
    std::stable_sort(sorted.begin(), sorted.end(), [](const Spec &a, const Spec &b) { return a.depth > b.depth; });
    Ref<Texture2D> bracket_texture = load("mainmenu/background-outerbrackets.png");
    for (const Spec &spec : sorted) {
        TextureRect *node = nullptr;
        if (String(spec.name) == "PartnerIllustration") {
            Ref<Texture2D> portrait = content::image(String("char/1080/") + String::num_int64(selected_character) + ".png");
            if (portrait.is_valid()) {
                node = memnew(TextureRect);
                node->set_name(spec.name);
                stage->add_child(node);
                node->set_texture(portrait);
                node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
                 node->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
                node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                const float w = portrait->get_width();
                const float h = portrait->get_height();
                node->set_position(at(spec.x, spec.y, w, h, spec.ax, spec.ay));
                node->set_size(Vector2(w, h));
            }
        } else {
            node = place(stage, spec.name, String("mainmenu/") + spec.file + ".png",
                spec.x, spec.y, spec.w, spec.h, spec.ax, spec.ay);
            if (node && bracket_texture.is_valid() && String(spec.name).begins_with("OuterBrackets")) {
                Ref<AtlasTexture> half;
                half.instantiate();
                half->set_atlas(bracket_texture);
                const float middle = std::floor(bracket_texture->get_width() / 2.f);
                half->set_region(String(spec.name) == "OuterBracketsLeft"
                    ? Rect2(0, 0, middle, bracket_texture->get_height())
                    : Rect2(middle, 0, bracket_texture->get_width() - middle, bracket_texture->get_height()));
                node->set_texture(half);
            }
        }
            if (node) {
                node->set_meta("depth", spec.depth);
                art[spec.name] = node;
            }
        if (String(spec.name) == "Background" && node) node->set_modulate(Color(.498f, .498f, .498f));
    }
    Ref<ShaderMaterial> chrome_material = content::hue_material(selected_row);
    for (const char *key : {"CharacterBracketBL", "CharacterBracketTR"})
        if (art.count(key)) art.at(key)->set_material(chrome_material);
    if (art.count("CharacterColorStrip")) {
        art.at("CharacterColorStrip")->set_visible(!bool(selected_row.get("is_grey", false)));
        art.at("CharacterColorStrip")->set_material(content::hue_material(selected_row, true));
    }
    const String jacket_path = content::random_jacket();
    Ref<Texture2D> jacket = jacket_path.is_empty() ? Ref<Texture2D>() : content::image(jacket_path);
    if (jacket.is_valid()) {
        auto *jacket_node = memnew(TextureRect);
        jacket_node->set_name("MusicJacket");
        stage->add_child(jacket_node);
        jacket_node->set_texture(jacket);
        jacket_node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        jacket_node->set_stretch_mode(TextureRect::STRETCH_SCALE);
        jacket_node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        jacket_node->set_position(at(419.829, 269.947, 800, 160));
        jacket_node->set_size(Vector2(800, 160));
        Ref<ShaderMaterial> jacket_material;
        jacket_material.instantiate();
        jacket_material->set_shader(ResourceLoader::get_singleton()->load("res://native/client/jacket.gdshader"));
        jacket_material->set_shader_parameter("mask_texture", load("mainmenu/musicplay-covereffects.png"));
        jacket_node->set_material(jacket_material);
        jacket_node->set_meta("depth", -0.32f);
        int index = stage->get_child_count() - 1;
        for (int i = 0; i < stage->get_child_count(); ++i) {
            Node *child = stage->get_child(i);
            if (child != jacket_node && child->has_meta("depth") && float(child->get_meta("depth")) < -0.32f) {
                index = i;
                break;
            }
        }
        stage->move_child(jacket_node, index);
        art["MusicJacket"] = jacket_node;
    }

    auto *character_button = art_button(stage, "CharacterSelectHit", "mainmenu/btn-character-Nia.png",
        "mainmenu/btn-character-Nia-Hover01.png", -613.427, 38.5, 451.17, 110);
    character_button->set_material(chrome_material);
    character_button->connect("pressed", Callable(this, "open_character_select"));
    stage->move_child(art["CharacterSwapIcon"], character_button->get_index() + 1);
    TextureButton *music_hit = art_button(stage, "MusicPlayHit", "mainmenu/btn-default.png", "mainmenu/btn-hover.png",
        680.3, 235.3, 280, 90);
    music_hit->connect("pressed", Callable(this, "on_menu_action").bind(1));
    art_button(stage, "TutorialHit", "mainmenu/btn-replaytutorial.png", "mainmenu/btn-replaytutorial-hover.png",
        628.5, 381.6, 340, 75);
    TextureButton *timeline_hit = art_button(stage, "TimelineHit", "mainmenu/btn-default.png", "mainmenu/btn-hover.png",
        680.6, -35.2, 280, 90);
    timeline_hit->connect("pressed", Callable(this, "on_menu_action").bind(2));
    art_button(stage, "SettingsHit", "mainmenu/button-menu.png", "mainmenu/button-menu-hover.png",
        624.7, 501.7, 89.5, 70);

    const Color cyan(218.f / 255, 254.f / 255, 254.f / 255);
    const Color cyan_title(217.f / 255, 254.f / 255, 254.f / 255);
    const Color ink(15.f / 255, 25.f / 255, 36.f / 255);
    const Color teal(66.f / 255, 199.f / 255, 198.f / 255);
    const Color cyan_pressed(160.f / 255, 200.f / 255, 200.f / 255); // Pressed state color for cyan text
    caption(stage, "PlayerTag", U"玩家", -833.5, 435, 54, 11, ink, true, true);
    caption(stage, "MusicTitleShadow", U"探索", 36.119, 379.97, 300, 26, Color(cyan_title.r, cyan_title.g, cyan_title.b, 66.f / 255), false, true);
    caption(stage, "MusicTitle", U"探索", 37.111, 380.957, 300, 26, cyan_title, false, true);
    caption(stage, "DifficultyShadow", U"进化", 85.131, 256.696, 220, 15, Color(teal.r, teal.g, teal.b, 57.f / 255), false, true);
    caption(stage, "Difficulty", U"进化", 86.098, 257.664, 220, 15, teal, false, true);
    caption(stage, "Unlocked", U"日期", 110.855, 230.555, 110, 11, cyan, false, true);
    const Dictionary date = Time::get_singleton()->get_date_dict_from_system(false);
    const int month = int(date.get("month", 1));
    const int day = int(date.get("day", 1));
    const String date_text = (month < 10 ? String("0") : String()) + String::num_int64(month)
        + "/" + (day < 10 ? String("0") : String()) + String::num_int64(day);
    caption(stage, "UnlockedValue", date_text, 134.786, 209.934, 120, 19, cyan, false, true);
    caption(stage, "Cleared", U"已完成", 245.83, 230.745, 100, 11, cyan, false, true);
    caption(stage, "ClearedValue", String::num_int64(konzetsu_saved_completion_count()) + "/6",
        270.423, 209.148, 120, 19, cyan, false, true);
    Label *select_song = caption(stage, "SelectSong", U"每日任务", 679.154, 226.761, 250, 23, cyan, true, true);
    caption(stage, "ReplayTutorial", U"重播教程", 659.884, 379.375, 280, 20, Color(193.f / 255, 224.f / 255, 227.f / 255), true, true);
    caption(stage, "ScenarioTitleShadow", U"剧情", 116.16, 110.196, 260, 26, Color(cyan_title.r, cyan_title.g, cyan_title.b, 66.f / 255), false, true);
    caption(stage, "ScenarioTitle", U"剧情", 117.158, 111.206, 260, 26, cyan_title, false, true);
    Label *view_timeline = caption(stage, "ViewTimeline", U"重播剧情", 679.876, -44.1, 250, 23, cyan, true, true);
    
    // Add button press color changes for main menu buttons
    if (select_song) {
        music_hit->connect("button_down", Callable(this, "partner_caption_color").bind(select_song->get_path(), cyan_pressed));
        music_hit->connect("button_up", Callable(this, "partner_caption_color").bind(select_song->get_path(), cyan));
    }
    if (view_timeline) {
        timeline_hit->connect("button_down", Callable(this, "partner_caption_color").bind(view_timeline->get_path(), cyan_pressed));
        timeline_hit->connect("button_up", Callable(this, "partner_caption_color").bind(view_timeline->get_path(), cyan));
    }
    
    caption(stage, "PrimaryTabText", U"主界面", -478, 491.013, 150, 15, Color(70.f / 255, 80.f / 255, 87.f / 255), true, true);
    caption(stage, "SecondaryTabText", U"总览", -306.5, 489.548, 175, 15, Color(70.f / 255, 80.f / 255, 87.f / 255), true, true);
    const String name = selected_row.get("name", "");
    caption(stage, "CharacterNameShadow", name, -585.918, 27.234, 235, 35, Color(ink.r, ink.g, ink.b, 66.f / 255), true, true);
    caption(stage, "CharacterName", name, -584.881, 28.205, 235, 35, ink, true, true);
    menu_http = memnew(HTTPRequest);
    menu_http->set_name("MenuHTTP");
    menu_http->set_timeout(15);
    menu_http->set_use_threads(true);
    add_child(menu_http);
    menu_http->connect("request_completed", Callable(this, "menu_http_completed"));
    konzetsu_http = memnew(HTTPRequest);
    konzetsu_http->set_name("KonzetsuTasksHTTP");
    konzetsu_http->set_timeout(15);
    konzetsu_http->set_use_threads(true);
    add_child(konzetsu_http);
    konzetsu_http->connect("request_completed", Callable(this, "konzetsu_tasks_completed"));
    konzetsu_task_busy = memnew(Control);
    konzetsu_task_busy->set_name("KonzetsuTaskBusy");
    stage->add_child(konzetsu_task_busy);
    konzetsu_task_busy->set_size(Vector2(W, H));
    konzetsu_task_busy->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *task_dim = memnew(ColorRect);
    task_dim->set_name("Dimmer");
    konzetsu_task_busy->add_child(task_dim);
    task_dim->set_color(Color(0, 0, 0, 1));
    task_dim->set_size(Vector2(W, H));
    task_dim->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *task_icon = place(konzetsu_task_busy, "ActivityIcon", "startup/activity_icon.png", 0, 0, 97, 146);
    if (task_icon) task_icon->set_pivot_offset(task_icon->get_size() * .5f);
    konzetsu_task_busy->hide();
    menu_fetch_user();
}

void ClientScreen::build_konzetsu() {
    const float scale = .67f * 1.5f;
    konzetsu_preload_images.clear();
    konzetsu_preload_image_index = 0;
    // Preload task images (user:// paths loaded with content::image)
    for (int i = 0; i < konzetsu_tasks.size(); ++i) {
        if (konzetsu_tasks[i].get_type() != Variant::DICTIONARY) continue;
        const Dictionary task = konzetsu_tasks[i];
        const String task_id = task.get("id", "");
        String image_path;
        if (task_id == "shiny_pure_total") image_path = "img/hit_max.png";
        else if (task_id == "daily_near_total") image_path = "img/hit_far.png";
        else if (task_id == "daily_exact_total") image_path = "img/hit_pure.png";
        else if (task_id == "daily_break_total") image_path = "img/hit_lost.png";
        else if (task_id == "daily_full_link_total") image_path = "img/clear_fl.png";
        else if (task_id == "daily_unplayed_song_clear") image_path = "img/new_badge.png";
        else if (task_id == "any_song_perfect_or_clear_twice") image_path = "img/clear_pd.png";
        else if (!konzetsu_task_song(task).is_empty())
            image_path = String("songs/dl_") + konzetsu_task_song(task) + "/1080_base.jpg";
        if (!image_path.is_empty() && std::find(konzetsu_preload_images.begin(),
                konzetsu_preload_images.end(), image_path) == konzetsu_preload_images.end())
            konzetsu_preload_images.push_back(image_path);
    }
    auto *hub = memnew(Control);
    hub->set_name("Hub");
    hub->set_size(Vector2(W, H));
    stage->add_child(hub);
    Ref<Texture2D> bg = load("konzetsu/hub/bg.jpg");
    const float cover = bg.is_valid() ? W / bg->get_width() : 1;
    if (TextureRect *hub_bg = place(hub, "HubBg", "konzetsu/hub/bg.jpg", 0, 0,
            bg.is_valid() ? bg->get_width() * cover : W, bg.is_valid() ? bg->get_height() * cover : H))
        art["HubBg"] = hub_bg;
    hub_content = memnew(Control);
    hub_content->set_name("HubContent");
    hub->add_child(hub_content);
    hub_content->set_position(Vector2(W / 2, H / 2));
    hub_content->set_scale(Vector2(.85f, .85f));
    auto add_hub = [&](const String &name, const String &file, float x = 0, float y = 0,
                       float ax = .5f, float ay = .5f, bool flipped = false) {
        Ref<Texture2D> tex = load(String("konzetsu/hub/") + file);
        if (!tex.is_valid()) return;
        auto *node = memnew(TextureRect);
        node->set_name(name);
        hub_content->add_child(node);
        node->set_texture(tex);
        node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        node->set_stretch_mode(TextureRect::STRETCH_SCALE);
        node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        const float width = tex->get_width() * scale;
        const float height = tex->get_height() * scale;
        node->set_size(Vector2(width, height));
        node->set_position(Vector2(x - width * (flipped ? 1 - ax : ax), -y - height * (1 - ay)));
        node->set_pivot_offset(Vector2(width * ax, height * (1 - ay)));
        node->set_flip_h(flipped);
        art[name.utf8().get_data()] = node;
    };
    add_hub("HubVortex1", "vortex1.png");
    add_hub("HubVortex2", "vortex2.png");
    add_hub("HubTopChainLeft", "centerpiece/top-chain.png", 58 * 1.5, 445 * 1.5, 1, 0);
    add_hub("HubCenterChainLeft", "centerpiece/center-chain.png", 0, 180 * 1.5, 1, 0);
    add_hub("HubBottomChainLeft", "centerpiece/bottom-chain.png", 58 * 1.5, 72 * 1.5, 1, 0);
    add_hub("HubTopChainRight", "centerpiece/top-chain.png", 156 * 1.5, 445 * 1.5, 1, 0, true);
    add_hub("HubCenterChainRight", "centerpiece/center-chain.png", 214 * 1.5, 180 * 1.5, 1, 0, true);
    add_hub("HubBottomChainRight", "centerpiece/bottom-chain.png", 156 * 1.5, 72 * 1.5, 1, 0, true);
    add_hub("HubCenterPanel", "centerpiece/center-node-panel.png");
    add_hub("HubCenterEye", "centerpiece/center-node-eye.png");
    if (art["HubCenterEye"]) art["HubCenterEye"]->set_modulate(Color(1, 1, 1, 76.f / 255));
    add_hub("HubCenterDecoTop", "centerpiece/center-node-deco-top.png", 0, 12 * 1.5);
    add_hub("HubCenterDecoBottom", "centerpiece/center-node-deco-bottom.png", 0, 12 * 1.5);
    const Vector2 positions[] = {{-220, 280}, {220, 280}, {-416, 0}, {416, 0}, {-220, -280}, {220, -280}};
    for (int i = 0; i < 6; ++i) {
        const String prefix = "HubNode" + String::num_int64(i + 1);
        const float x = positions[i].x * 1.5f;
        const float y = positions[i].y * 1.5f;
        add_hub(prefix + String("Swirl"), "node/node-swirl-inactive.png", x, y);
        add_hub(prefix + String("Panel"), "node/node-panel.png",
            positions[i].x * 1.5f, positions[i].y * 1.5f);
        Control *panel = art[(prefix + String("Panel")).utf8().get_data()];
        if (panel) {
            panel->hide();
            auto *hit = memnew(TextureButton);
            hit->set_name(prefix + String("Hit"));
            hub_content->add_child(hit);
            hit->set_texture_normal(load("konzetsu/hub/node/node-panel.png"));
            hit->set_texture_pressed(load("konzetsu/hub/node/node-pressed.png"));
            hit->set_ignore_texture_size(true);
            hit->set_stretch_mode(TextureButton::STRETCH_SCALE);
            hit->set_position(panel->get_position());
            hit->set_size(panel->get_size());
            art[(prefix + String("Hit")).utf8().get_data()] = hit;
            hit->set_focus_mode(Control::FOCUS_NONE);
            hit->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
            hit->set_disabled(i == 5 && !konzetsu_all_daily_tasks_complete(konzetsu_tasks));
            hit->connect("pressed", Callable(this, "open_konzetsu_challenge").bind(i + 1));
        }
        const String path = "node/node-number" + String::num_int64(i + 1) + ".png";
        add_hub(prefix + String("Number"), path,
            positions[i].x * 1.5f + 3, positions[i].y * 1.5f + 9);
        add_hub(prefix + String("Dim"), "node/node-dim-inactive.png", x, y);
        add_hub(prefix + String("Ripple"), "node/node-clear-ripple.png", x, y);
        add_hub(prefix + String("Badge"), "node/node-clear-badge" + String::num_int64(i + 1) + ".png", x + 6, y);
        add_hub(prefix + String("Chain"), "node/node-clear-chain.png", x, y - 48);
        add_hub(prefix + String("BadgeInactive"), "node/node-clear-badge" + String::num_int64(i + 1) + "-inactive.png", x + 6, y);
        add_hub(prefix + String("RippleInactive"), "node/node-clear-ripple-inactive.png", x, y);
        add_hub(prefix + String("ChainInactive"), "node/node-clear-chain-inactive.png", x, y - 48);
        if (i == 5) {
            add_hub("HubNode6Locked", "node/node-locked.png",
                positions[i].x * 1.5f, positions[i].y * 1.5f);
            if (Control *lock = art["HubNode6Locked"]) {
                lock->set_position(lock->get_position() + Vector2(10.f * 1.5f, -6.f * 1.5f));
                lock->set_visible(!konzetsu_all_daily_tasks_complete(konzetsu_tasks));
            }
        }
    }
    refresh_konzetsu_hub_visuals();
    Ref<Texture2D> back_texture = load("konzetsu/hub/back.png");
    const float back_width = back_texture.is_valid() ? back_texture->get_width() * scale : 0;
    const float back_height = back_texture.is_valid() ? back_texture->get_height() * scale : 0;
    auto *back = art_button(hub, "HubBack", "konzetsu/hub/back.png", "konzetsu/hub/back-pressed.png",
        0, H / 2 - back_height / 2, back_width, back_height);
    if (back) {
        back->set_position(Vector2((W - back_width) / 2, 0));
        back->set_pivot_offset(Vector2(back_width / 2, back_height));
        back->set_modulate(Color(1, 1, 1, 0));
        back->set_visible(false);
        back->set_disabled(true);
        back->connect("pressed", Callable(this, "return_to_menu"));
    }
    art["HubBack"] = back;

    Ref<Texture2D> loading_bg = load("konzetsu/loadtransition/loading-bg.png");
    const float loading_scale = loading_bg.is_valid() ? W / loading_bg->get_width() : 1;
    const Spec pieces[] = {
        {"LoadingBg", "loading-bg.png", 0, 0, 0, 0, 0},
        {"CenterVortex", "center-vortex.png", 0, 0, 0, 0, 0},
        {"BottomDiamond", "bottom-diamond-panel.png", 0, -H / 2, 0, 0, 0, .5, 0},
        {"BgGlow", "bg-glow.png", 0, -H / 2, 0, 0, 0, .5, 0},
        {"RingLarge", "ring-large.png", 0, 0, 0, 0, 0},
        {"RingMedium", "ring-medium.png", 0, 0, 0, 0, 0},
        {"RingSmall", "ring-small.png", 0, 0, 0, 0, 0},
        {"CenterFrame", "center-frame.png", 0, 0, 0, 0, 0},
        {"GaugeDeco", "gauge-deco.png", 0, 48, 0, 0, 0},
        {"GaugeBg", "gauge-bg.png", 0, 0, 0, 0, 0},
        {"LoadingRune", "loading-rune-text.png", 0, -48, 0, 0, 0},
    };
    for (const Spec &spec : pieces) {
        const String path = String("konzetsu/loadtransition/") + spec.file;
        Ref<Texture2D> tex = load(path);
        if (!tex.is_valid()) continue;
        const float factor = String(spec.name) == "LoadingBg" || String(spec.name) == "CenterFrame"
            || String(spec.name) == "BgGlow" ? loading_scale : scale;
        auto *node = place(stage, spec.name, path, spec.x, spec.y,
            tex->get_width() * factor, tex->get_height() * factor, spec.ax, spec.ay);
        art[spec.name] = node;
        node->set_pivot_offset(node->get_size() / 2);
        if (String(spec.name) != "LoadingBg") node->set_modulate(Color(1, 1, 1, 0));
    }
    Ref<Texture2D> fill = load("konzetsu/loadtransition/gauge-fill.png");
    if (fill.is_valid()) {
        const Vector2 size(fill->get_width() * scale, fill->get_height() * scale);
        auto *clip = memnew(Control);
        clip->set_name("GaugeClip");
        stage->add_child(clip);
        clip->set_position(at(0, 0, size.x, size.y));
        clip->set_size(Vector2(0, size.y));
        clip->set_clip_contents(true);
        clip->set_meta("full_width", size.x);
        art["GaugeClip"] = clip;
        auto *stripe = memnew(TextureRect);
        stripe->set_name("GaugeFill");
        clip->add_child(stripe);
        stripe->set_texture(fill);
        stripe->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        stripe->set_size(size);
        stripe->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        stripe->set_modulate(Color(1, 1, 1, 0));
        art["GaugeFill"] = stripe;
    }
}

void ClientScreen::refresh_konzetsu_hub_visuals() {
    if (!hub_content) return;
    const bool all_complete = !(konzetsu_pending_clear_mask & 31) && konzetsu_all_daily_tasks_complete(konzetsu_tasks);
    const char *center_chains[] = {"HubTopChainLeft", "HubTopChainRight", "HubCenterChainLeft",
        "HubCenterChainRight", "HubBottomChainLeft", "HubBottomChainRight"};
    for (int i = 0; i < 6; ++i) {
        const bool cleared = konzetsu_tasks.size() > i
            && !(konzetsu_pending_clear_mask & (1 << i))
            && konzetsu_tasks[i].get_type() == Variant::DICTIONARY
            && konzetsu_daily_task_complete(Dictionary(konzetsu_tasks[i]));
        const bool completed = i < 5 ? all_complete : cleared;
        const bool center_chain_cleared = cleared
            || (i == 5 && !String(konzetsu_completed_songs.get(6, "")).is_empty());
        if (Control *chain = art[center_chains[i]])
            chain->set_modulate(Color(1, 1, 1, center_chain_cleared ? 0.f : 1.f));
        const String prefix = "HubNode" + String::num_int64(i + 1);
        auto set_visible = [&](const String &name, bool visible) {
            if (Control *node = art[name.utf8().get_data()]) node->set_visible(visible);
        };
        auto set_alpha = [&](const String &name, float alpha) {
            if (Control *node = art[name.utf8().get_data()]) node->set_modulate(Color(1, 1, 1, alpha));
        };
        set_visible(prefix + String("Panel"), false);
        for (const char *suffix : {"Number", "Swirl", "Dim", "Badge", "Ripple", "Chain",
                "BadgeInactive", "RippleInactive", "ChainInactive"})
            set_alpha(prefix + String(suffix), 1.f);
        set_alpha(prefix + String("Hit"), cleared ? 0.f : 1.f);
        set_visible(prefix + String("Number"), !cleared);
        set_visible(prefix + String("Swirl"), cleared);
        set_visible(prefix + String("Dim"), cleared);
        set_visible(prefix + String("Badge"), cleared && !completed);
        set_visible(prefix + String("Ripple"), cleared && !completed);
        set_visible(prefix + String("Chain"), cleared && !completed);
        for (const char *suffix : {"BadgeInactive", "RippleInactive", "ChainInactive"})
            set_visible(prefix + String(suffix), completed);
        if (i == 5) {
            if (Control *lock = art["HubNode6Locked"]) {
                lock->set_visible(!all_complete);
                lock->set_modulate(Color(1, 1, 1, 1));
            }
            if (BaseButton *hit = Object::cast_to<BaseButton>(hub_content->get_node_or_null("HubNode6Hit")))
                hit->set_disabled(!all_complete);
        }
    }
}

int ClientScreen::konzetsu_new_clear_mask(const Array &previous, const Array &tasks) {
    if (previous.size() != 6 || tasks.size() != 6) return 0;
    int mask = 0;
    for (int i = 0; i < 6; ++i) {
        if (previous[i].get_type() != Variant::DICTIONARY || tasks[i].get_type() != Variant::DICTIONARY) continue;
        const Dictionary before = previous[i], after = tasks[i];
        // A replacement daily task has no previously observed completion transition.
        bool same_task = true;
        for (const char *key : {"id", "song", "difficulty"})
            if (before.get(key, Variant()) != after.get(key, Variant())) same_task = false;
        if (same_task && !konzetsu_daily_task_complete(before) && konzetsu_daily_task_complete(after)) mask |= 1 << i;
    }
    return mask;
}

void ClientScreen::step_konzetsu_clear_animation(double delta) {
    if (!konzetsu_pending_clear_mask || !hub_content) return;
    if (konzetsu_clear_seconds < 0) konzetsu_clear_seconds = 0;
    const double previous = konzetsu_clear_seconds;
    konzetsu_clear_seconds += delta;
    // setupIntroAnimations schedules the original clear callback after 0.6s.
    if (konzetsu_clear_seconds < .6) return;
    if (previous < .6) play_sfx("konzetsu/challenge_chain_break.wav");
    const double elapsed = konzetsu_clear_seconds - .6;
    const auto ease_out = [](double time, double duration) {
        const float u = std::clamp(float(time / duration), 0.f, 1.f);
        return 1.f - std::pow(1.f - u, 3.f);
    };
    const float fade = ease_out(elapsed, .7);
    const float chain_fade = ease_out(elapsed, .3);
    // The original wraps the entire FadeOut/CallFunc/FadeIn sequence in
    // EaseCubicActionOut, so ease its combined clock rather than each half.
    const float inactive_time = ease_out(elapsed, .6) * .6f;
    const float inactive_out = std::clamp(inactive_time / .3f, 0.f, 1.f);
    const float inactive_in = std::clamp((inactive_time - .3f) / .3f, 0.f, 1.f);
    const bool unlock = (konzetsu_pending_clear_mask & 31)
        && konzetsu_all_daily_tasks_complete(konzetsu_tasks);
    const bool deactivate_sixth = (konzetsu_pending_clear_mask & 32) != 0;
    const char *center_chains[] = {"HubTopChainLeft", "HubTopChainRight", "HubCenterChainLeft",
        "HubCenterChainRight", "HubBottomChainLeft", "HubBottomChainRight"};
    for (int i = 0; i < 6; ++i) {
        const String prefix = "HubNode" + String::num_int64(i + 1);
        // transitionToCleared(false) establishes the clear state before
        // transitionToInactive(true) fades out, swaps images, then fades in.
        if ((i < 5 && unlock) || (i == 5 && deactivate_sixth)) {
            for (const char *suffix : {"Hit", "Number"})
                if (Control *node = art[(prefix + String(suffix)).utf8().get_data()])
                    node->set_modulate(Color(1, 1, 1, 0));
            for (const char *suffix : {"Swirl", "Dim"})
                if (Control *node = art[(prefix + String(suffix)).utf8().get_data()]) {
                    node->show();
                    node->set_modulate(Color(1, 1, 1, 1));
                }
            for (const char *suffix : {"Badge", "Ripple", "Chain"}) {
                if (Control *node = art[(prefix + String(suffix)).utf8().get_data()]) {
                    node->set_visible(inactive_time < .3f);
                    node->set_modulate(Color(1, 1, 1, 1.f - inactive_out));
                }
                if (Control *node = art[(prefix + String(suffix) + String("Inactive")).utf8().get_data()]) {
                    node->set_visible(inactive_time >= .3f);
                    node->set_modulate(Color(1, 1, 1, inactive_in));
                }
            }
        } else if (konzetsu_pending_clear_mask & (1 << i)) {
            for (const char *suffix : {"Hit", "Number", "Swirl", "Dim", "Badge", "Ripple", "Chain"}) {
                if (Control *node = art[(prefix + String(suffix)).utf8().get_data()]) {
                    node->show();
                    const bool disappearing = String(suffix) == "Hit" || String(suffix) == "Number";
                    node->set_modulate(Color(1, 1, 1, disappearing ? 1.f - fade : fade));
                }
            }
        }
        if (konzetsu_pending_clear_mask & (1 << i))
            if (Control *chain = art[center_chains[i]]) chain->set_modulate(Color(1, 1, 1, 1.f - chain_fade));
    }
    if (unlock)
        if (Control *lock = art["HubNode6Locked"])
            lock->set_modulate(Color(1, 1, 1, 1.f - chain_fade));
    const double duration = unlock || (konzetsu_pending_clear_mask == 32) ? .6 : .7;
    if (elapsed >= duration) {
        konzetsu_pending_clear_mask = 0;
        konzetsu_clear_seconds = -1;
        refresh_konzetsu_hub_visuals();
        save_konzetsu_task_snapshot();
    }
}

void ClientScreen::set_konzetsu_progress(const Dictionary &completed_songs) {
    // Keys are challenge IDs (1..6); values are the completed song IDs.
    konzetsu_completed_songs = completed_songs.duplicate(true);
    refresh_konzetsu_hub_visuals();
    if (konzetsu_dialog && konzetsu_dialog->is_visible()) close_konzetsu_challenge();
}

void ClientScreen::open_konzetsu_challenge(int id) {
    // Remove clock restriction for offline preview mode
    if (scene_kind != 2 || switching || clock < 4.35 || konzetsu_pending_clear_mask || id < 1 || id > 6) return;
    if (konzetsu_dialog && konzetsu_dialog->is_visible()) return;
    const char *titles_zh[] = {"I. 晦涩之道", "II. 扭曲之道", "III. 无极之道",
        "IV. 残缺之道", "V. 无形之道", "VI."};
    const char *task_title_prefixes[] = {"I.", "II.", "III.", "IV.", "V.", "VI."};
    const char *hints_zh[] = {"培养对不可察觉之物的感知。", "挣脱时间的桎梏。",
        "重构世界。", "稍歇一会，便再度迈步吧。",
        "伸手，触碰那不可能之物。", "开拓。"};
    const char **titles = titles_zh;
    const char **hints = hints_zh;
    const char *stories_zh[] = {
        "// 比起细腻柔软的布料，这个世界更像是难以捉摸的大海，\n组成Arcaea的东西无时无刻不在以令人难以理解的方式变化。\n很多人也许会用描述情感的词来形容这种变化：冷静的，\n或是愤怒的；阴暗的，或是柔和的。\n\n潮水般，时涨，时落…… //",
        "// 七彩的光芒透过破碎的玻璃喷涌而出，连带着时间也停止了下来。\n这种情况先前肯定也发生过。\n\n这种抗拒的方式，这种似曾相识的感觉——\n时光仍处于冰封之中，她的心情也低沉到了冰点。\n现在的她，只想让自己就此打住。//",
        "// 新的路径已经完工，她自然刻不容缓地登上了阶梯。\n那把浮空座椅近距离陪伴在她身旁，随时准备接住也许会不慎失足的她。\n当女孩往上迈进时，她更是抽走了位于最下端的梯级，接续在整体结构的最上方。\n没过多久，她便意识到自己是在攀爬一段永无止境地延伸的螺旋长梯。\n穿过层层云雾，直至抵达最高点。//",
        "// 她将自己的衣服捂得更紧了点，思量着要不要闭上眼睛。\n最后她没有那么做，而是哆嗦着死死盯着她前方上空中碎片的变化。\n即使是自己都快要给吹跑了，雪没过小腿，没过膝盖了，\n她也丝毫没有移开视线。她那厚厚的长袍此时显得特别单薄，\n而她的呼吸也逐渐急促了起来。 //",
        "// 每踏出一步，场景就转换一次。她的脚步能改变脚下的地面，\n并改造空间。她走向织锦旁，却发现布料并未好好缝上。\n玻璃静静地滑过她的身边，然后突然四散，像是受到了惊吓。\n她周遭的世界已变得漆黑而不再洁白。繁星洒落在空中，\n脚下的路却是如此支离破碎…… //",
        "// 她是生命的摇篮孕育的花。\n矛尖的殷红不断坠落，她的双眼却再次睁开。\n凝视那神明的骸骨吧，少女，然后命令祂——\n给予祂一切。//"};
    const char **stories = stories_zh;
    String task_title;
    String task_hint;
    String task_story;
    String task_song_id;
    String task_art_path;
    bool target_chart_perfect = false;
    bool target_chart_link = false;
    bool daily_score_total = false;
    bool any_song_perfect = false;
    bool daily_full_link = false;
    bool daily_rating_14 = false;
    bool hard_skill_partner_clear = false;
    int64_t task_current = 0;
    String current_score;
    String target_score;
    if (konzetsu_tasks_loaded && konzetsu_tasks.size() >= id) {
        const Dictionary task = konzetsu_tasks[id - 1];
        const Dictionary req = task.get("req", Dictionary());
        const Dictionary state = task.get("state", Dictionary());
        const String task_id = String(task.get("id", ""));
        task_title = String(task.get("name", U"每日任务"));
        const int64_t raw_current = task_integer(task, state, "current");
        const int64_t progress = task_integer(task, state, "progress",
            req.has("value") ? int64_t(req["value"]) : 0);
        const int64_t amount = task_integer(task, state, "amount",
            req.has("amount") ? int64_t(req["amount"]) : 0);
        const int64_t current = std::max<int64_t>(0, raw_current);
        task_current = current;
        const int64_t capped_current = progress > 0 ? std::min(current, progress) : current;
        const String current_text = String::num_int64(capped_current);
        const String progress_text = String::num_int64(progress);
        task_hint = String(U"进度 ") + String::num_int64(current)
            + (progress > 0 ? String(U" / ") + progress_text : String());
        if (req.has("song")) task_story += String(U"指定歌曲：") + String(req.get("song", ""));
        if (req.has("diff")) task_story += String(U"\n难度：") + String(req.get("diff", ""));
        task_story += String(U"\n完成条件由服务端每日生成。奖励：15 Ticket。//");
        if (String(task.get("id", "")) == "target_chart_score") {
            task_title = U"登临之道";
            task_story = "";
            const String song_id = konzetsu_task_song(task);
            task_song_id = song_id;
            const int difficulty = konzetsu_task_difficulty(task);
            const String difficulty_name = difficulty == 2 ? "ULT"
                : difficulty == 3 || difficulty == 4 ? "FBD" : String::num_int64(difficulty);
            String score_w = String::num(double(task.get("progress", 0)) / 10000.0, 2);
            if (score_w.contains(".")) {
                while (score_w.ends_with("0")) score_w = score_w.left(score_w.length() - 1);
                if (score_w.ends_with(".")) score_w = score_w.left(score_w.length() - 1);
            }
            task_hint = String(U"以 ") + score_w + String(U"w 或以上成绩通关「")
                + content::song_title(song_id) + String(U"」[") + difficulty_name + String(U"]");
        }
        if (String(task.get("id", "")) == "target_chart_judgment_limit") {
            task_title = U"无痕之道";
            task_song_id = konzetsu_task_song(task);
            const int difficulty = konzetsu_task_difficulty(task);
            const String difficulty_name = difficulty == 2 ? "ULT"
                : difficulty == 3 || difficulty == 4 ? "FBD" : String::num_int64(difficulty);
            task_hint = String(U"通关「")
                + content::song_title(task_song_id) + String(U"」[")
                + difficulty_name + String(U"] 时 NEAR + BREAK 不大于 ")
                + String::num_int64(int64_t(task.get("progress", 0)));
        }
        if (String(task.get("id", "")) == "target_chart_dual_difficulty_clear") {
            task_title = U"双生之道";
            task_song_id = konzetsu_task_song(task);
            const String both_charts = String(U"通关「") + content::song_title(task_song_id)
                + String(U"」[ULT] [FBD] 各 1 次");
            task_hint = current == 1 ? both_charts.replace("[ULT] ", "")
                : current == 2 ? both_charts.replace("[FBD] ", "") : both_charts;
        }
        if (String(task.get("id", "")) == "target_chart_perfect_or_clear_twice") {
            target_chart_perfect = true;
            task_title = U"无暇之道";
            task_song_id = konzetsu_task_song(task);
            task_hint = String(U"PERFECT DIVE 通关「") + content::song_title(task_song_id) + String(U"」");
        }
        if (String(task.get("id", "")) == "target_chart_link_or_clear_twice") {
            target_chart_link = true;
            task_title = U"连绵之道";
            task_song_id = konzetsu_task_song(task);
            task_hint = String(U"FULL LINK 通关「") + content::song_title(task_song_id) + String(U"」");
        }
        if (task_id == "shiny_pure_total") {
            task_title = U"辉耀之道";
            task_art_path = "img/hit_max.png";
        } else if (task_id == "daily_near_total") {
            task_title = U"偏离之道";
            task_art_path = "img/hit_far.png";
        } else if (task_id == "daily_exact_total") {
            task_title = U"秩序之道";
            task_art_path = "img/hit_pure.png";
        } else if (task_id == "daily_break_total") {
            task_title = U"残缺之道";
            task_art_path = "img/hit_lost.png";
        }
        if (!task_art_path.is_empty()) {
            const int64_t capped_amount = amount > 0 ? std::min(current, amount) : current;
            task_hint = String(U"累计获得 ") + String::num_int64(capped_amount) + "/"
                + String::num_int64(amount) + String(U" 个此判定");
        }
        if (task_id == "daily_score_total") {
            daily_score_total = true;
            task_title = U"无际之道";
            current_score = current_text + String("/");
            target_score = progress_text;
            task_hint = U"累计获得一定分数";
        } else if (task_id == "daily_full_link_total") {
            daily_full_link = true;
            task_title = U"不绝之道";
            task_art_path = "img/clear_fl.png";
            task_hint = String(U"获得 FULL LINK ")
                + current_text + "/" + progress_text + String(U" 次");
        } else if (task_id == "daily_unplayed_song_clear") {
            task_title = U"未知之道";
            task_art_path = "img/new_badge.png";
            task_hint = U"通关任意一首未曾游玩过的曲目";
        } else if (task_id == "high_const_clear_total") {
            daily_rating_14 = true;
            task_title = U"晦涩之道";
            task_hint = String(U"通关 ") + current_text + "/" + progress_text
                + String(U" 首定级为 14 的曲目");
        } else if (task_id == "any_song_perfect_or_clear_twice") {
            any_song_perfect = true;
            task_title = U"无形之道";
            task_art_path = "img/clear_pd.png";
            task_hint = U"获得 PERFECT DIVE";
        } else if (task_id == "challenge_16_clear") {
            hard_skill_partner_clear = true;
            task_title = String(task.get("name", U"困难之道"));
            task_song_id = konzetsu_task_song(task);
            const int difficulty = konzetsu_task_difficulty(task);
            const String difficulty_name = difficulty == 2 ? "ULT"
                : difficulty == 3 || difficulty == 4 ? "FBD" : String::num_int64(difficulty);
            task_hint = String(U"使用包含「困难」技能的搭档通关「")
                + content::song_title(task_song_id) + String(U"」[") + difficulty_name + String(U"]");
        }
    }
    const bool distinct = konzetsu_all_daily_tasks_complete(konzetsu_tasks);
    konzetsu_challenge = id;
    konzetsu_dialog_closing = false;
    const String completed = konzetsu_completed_songs.get(id, "");
    konzetsu_dialog_mode = !completed.is_empty() ? 1 : id == 6 && !distinct ? 2 : 0;
    play_sfx("konzetsu/ui_tap.wav");
    if (konzetsu_dialog) {
        konzetsu_gallery_strip = nullptr;
        konzetsu_gallery_cycle_width = 0;
        konzetsu_gallery_unused.clear();
        konzetsu_gallery_decode_attempted = false;
        stage->remove_child(konzetsu_dialog);
        konzetsu_dialog->queue_free();
    }
    konzetsu_dialog = memnew(Control);
    konzetsu_dialog->set_name("KonzetsuChallengeDialog");
    stage->add_child(konzetsu_dialog);
    konzetsu_dialog->set_size(Vector2(W, H));
    konzetsu_dialog->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *shade = memnew(ColorRect);
    shade->set_name("Dimmer");
    konzetsu_dialog->add_child(shade);
    shade->set_size(Vector2(W, H));
    shade->set_color(Color(0, 16.f / 255, 12.f / 255, 148.f / 255));
    shade->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *visuals = memnew(Control);
    visuals->set_name("PanelVisuals");
    konzetsu_dialog->add_child(visuals);
    visuals->set_size(Vector2(W, H));
    visuals->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    const String base = "konzetsu/hub/challengedialog/";
    auto dialog_art = [&](const char *name, const String &file, float y = 0.f) {
        Ref<Texture2D> texture = load(base + file);
        if (texture.is_valid()) {
            auto *rect = memnew(TextureRect);
            rect->set_name(name);
            visuals->add_child(rect);
            rect->set_texture(texture);
            rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            rect->set_stretch_mode(TextureRect::STRETCH_SCALE);
            rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            const float width = texture->get_width() * 1.005f;
            const float height = texture->get_height() * 1.005f;
            rect->set_size(Vector2(width, height));
            rect->set_position(at(0, y, width, height, .5f, 1.f));
            rect->set_pivot_offset(Vector2(width * .5f, height));
        }
    };
    dialog_art("Backing", konzetsu_dialog_mode == 2 ? "popup-bg-locked.png" : "popup-bg.png", 540.f);
    dialog_art("Spear", "popup-spear.png", 540.f);

    auto *content = memnew(Control);
    content->set_name("DialogContent");
    if (konzetsu_dialog_mode == 2) {
        Ref<Texture2D> rune_texture = content::image(base + String("popup-text-rune.png"));
        if (rune_texture.is_valid()) {
            auto *rune = memnew(TextureRect);
            rune->set_name("Rune");
            visuals->add_child(rune);
            rune->set_texture(rune_texture);
            rune->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            rune->set_stretch_mode(TextureRect::STRETCH_SCALE);
            rune->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            const float width = rune_texture->get_width() * 1.005f;
            const float height = rune_texture->get_height() * 1.005f;
            rune->set_size(Vector2(width, height));
            rune->set_position(Vector2((W - width) * .5f, 150 - height * .5f));
        }
        dialog_art("LockedOverlay", "popup-bg-locked-overlay.png", 540.f);
    }
    visuals->add_child(content);
    constexpr float cocos_to_canvas = 1.5f;
    constexpr float content_width = 1085.f * cocos_to_canvas;
    constexpr float content_height = 480.f * cocos_to_canvas;
    content->set_size(Vector2(content_width, content_height));
    content->set_position(Vector2((W - content_width) * .5f,
        H * .5f - 80.f * cocos_to_canvas - content_height * .5f));
    auto set_local_text_rect = [](Label *label, float x, float y, float width, float height) {
        label->set_anchor(SIDE_LEFT, 0);
        label->set_anchor(SIDE_TOP, 0);
        label->set_anchor(SIDE_RIGHT, 0);
        label->set_anchor(SIDE_BOTTOM, 0);
        label->set_size(Vector2(width, height));
        label->set_position(Vector2(x, y));
    };
    const String display_title = task_title.is_empty() ? String::utf8(titles[id - 1])
        : String(task_title_prefixes[id - 1]) + " " + task_title;
    auto *title = caption(content, "Title", display_title, 0, 135, content_width, 42,
        Color(217.f / 255, 207.f / 255, 173.f / 255), true);
    if (!task_title.is_empty()) {
        Ref<Font> task_title_font = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
        if (task_title_font.is_valid()) title->add_theme_font_override("font", task_title_font);
    }
    set_local_text_rect(title, 0, (480.f - 370.f) * cocos_to_canvas - title->get_size().y * .5f,
        content_width, title->get_size().y);
    Label *story = caption(content, "Story", "", 0, -128, content_width, 24,
        Color(225.f / 255, 217.f / 255, 191.f / 255), true);
    Ref<Font> task_body_font = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
    if (task_body_font.is_valid()) story->add_theme_font_override("font", task_body_font);
    story->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    story->set_clip_text(true);
    story->set_text("");
    constexpr float story_height = 120.f * cocos_to_canvas;
    const float story_y = (480.f - 250.f) * cocos_to_canvas - story_height * .5f;
    set_local_text_rect(story, 0, story_y,
        content_width, story_height);
    story->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
    if (daily_score_total && task_body_font.is_valid()) {
        constexpr int current_size = 54;
        constexpr int target_size = 36;
        const float current_width = task_body_font->get_string_size(current_score, HORIZONTAL_ALIGNMENT_LEFT, -1, current_size).x;
        const float target_width = task_body_font->get_string_size(target_score, HORIZONTAL_ALIGNMENT_LEFT, -1, target_size).x;
        const float left = (content_width - current_width - target_width) * .5f;
        const float baseline = story_y + story_height * .5f + task_body_font->get_ascent(current_size) * .5f;
        auto *current = caption(content, "CurrentScore", current_score, 0, 0, current_width, current_size,
            Color(225.f / 255, 217.f / 255, 191.f / 255));
        auto *target = caption(content, "TargetScore", target_score, 0, 0, target_width, target_size,
            Color(225.f / 255, 217.f / 255, 191.f / 255));
        for (Label *label : {current, target}) {
            label->add_theme_font_override("font", task_body_font);
            label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
            label->set_vertical_alignment(VERTICAL_ALIGNMENT_TOP);
        }
        set_local_text_rect(current, left, baseline - task_body_font->get_ascent(current_size), current_width,
            task_body_font->get_height(current_size));
        set_local_text_rect(target, left + current_width, baseline - task_body_font->get_ascent(target_size), target_width,
            task_body_font->get_height(target_size));
    }
    if (task_art_path.is_empty() && !task_song_id.is_empty())
        task_art_path = String("songs/dl_") + task_song_id + String("/1080_base.jpg");
    if (daily_rating_14) {
        const PackedStringArray candidates = content::songs_with_rating_at_least(14);
        if (!candidates.is_empty()) {
            PackedStringArray selected;
            PackedStringArray available = candidates;
            while (selected.size() < 6 && !available.is_empty()) {
                const int index = UtilityFunctions::randi_range(0, available.size() - 1);
                selected.push_back(available[index]);
                available.remove_at(index);
            }
            konzetsu_gallery_unused = available;
            auto *viewport = memnew(Control);
            viewport->set_name("TaskSongGallery");
            content->add_child(viewport);
            viewport->set_clip_contents(true);
            const float gallery_width = 1180.f;
            const float gallery_height = story_height + 54.f;
            viewport->set_position(Vector2((content_width - gallery_width) * .5f, story_y - 15.f));
            viewport->set_size(Vector2(gallery_width, gallery_height));
            auto *strip = memnew(Control);
            viewport->add_child(strip);
            konzetsu_gallery_strip = strip;
            const float tile_size = gallery_height;
            const float gap = 18.f;
            for (int copy = 0; copy < 2; ++copy) {
                for (int i = 0; i < selected.size(); ++i) {
                    const String gallery_path = String("songs/dl_") + selected[i] + "/1080_base.jpg";
                    auto *tile = memnew(TextureRect);
                    strip->add_child(tile);
                    tile->set_meta("gallery_path", gallery_path);
                    tile->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
                    tile->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
                    tile->set_position(Vector2((copy * selected.size() + i) * (tile_size + gap), 0));
                    tile->set_size(Vector2(tile_size, tile_size));
                    tile->set_modulate(Color(1, 1, 1, 0));
                    tile->set_meta("gallery_slot", i);
                    tile->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                }
            }
            const float cycle_width = selected.size() * (tile_size + gap);
            konzetsu_gallery_cycle_width = cycle_width;
            strip->set_size(Vector2(cycle_width * 2.f, gallery_height));
            auto *fade_mask = memnew(ColorRect);
            content->add_child(fade_mask);
            fade_mask->set_name("TaskGalleryFadeMask");
            fade_mask->set_position(viewport->get_position());
            fade_mask->set_size(Vector2(gallery_width, gallery_height));
            fade_mask->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            Ref<Shader> fade_shader;
            fade_shader.instantiate();
            fade_shader->set_code(R"shader(shader_type canvas_item;
                void fragment() {
                    const float edge = 0.14;
                    float alpha = max((edge - UV.x) / edge, (UV.x - (1.0 - edge)) / edge);
                    COLOR = vec4(0.0, 16.0 / 255.0, 12.0 / 255.0, clamp(alpha, 0.0, 1.0));
                }
            )shader");
            Ref<ShaderMaterial> fade_material;
            fade_material.instantiate();
            fade_material->set_shader(fade_shader);
            fade_mask->set_material(fade_material);
        }
    } else if (!task_art_path.is_empty()) {
        const Ref<Texture2D> song_art = content::image(task_art_path);
        if (song_art.is_valid()) {
            auto *art = memnew(TextureRect);
            art->set_name("TaskSongArtwork");
            content->add_child(art);
            art->set_texture(song_art);
            art->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            art->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
            const float art_scale = any_song_perfect || daily_full_link ? .75f
                : task_art_path == "img/new_badge.png" ? .8f : 1.f;
            const float art_height = (story_height + 54.f) * art_scale;
            const float art_width = content_width * art_scale;
            art->set_position(Vector2((content_width - art_width) * .5f,
                story_y - 15.f + (story_height + 54.f - art_height) * .5f));
            art->set_size(Vector2(art_width, art_height));
            art->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        }
    }
    const String display_hint = task_hint.is_empty() ? String::utf8(hints[id - 1]) : task_hint;
    auto *hint = caption(content, "Hint", display_hint, 0, 16, content_width, 30,
        Color(225.f / 255, 217.f / 255, 191.f / 255), true);
    Ref<Font> task_hint_font = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
    if (task_hint_font.is_valid()) hint->add_theme_font_override("font", task_hint_font);
    set_local_text_rect(hint, 0, (480.f - 160.f) * cocos_to_canvas - hint->get_size().y * .5f + 42.f,
        content_width, hint->get_size().y);
    if (target_chart_perfect || target_chart_link || any_song_perfect) {
        const Ref<Font> font = hint->get_theme_font("font");
        const String target_phrase = target_chart_link ? "FULL LINK" : "PERFECT DIVE";
        const float phrase_width = font->get_string_size(target_phrase, HORIZONTAL_ALIGNMENT_LEFT, -1, 30).x;
        const float hint_width = font->get_string_size(display_hint, HORIZONTAL_ALIGNMENT_LEFT, -1, 30).x;
        const int64_t clear_count = task_current == 1 ? 1 : 2;
        const String note_text = String(U"或 DIVE CLEARED ") + String::num_int64(clear_count) + String(U" 次");
        const float note_width = font->get_string_size(note_text, HORIZONTAL_ALIGNMENT_LEFT, -1, 18).x;
        auto *note = caption(content, "PerfectDiveNote", note_text, 0, 0, note_width, 18,
            Color(225.f / 255, 217.f / 255, 191.f / 255), true);
        note->add_theme_font_override("font", font);
        const float prefix_width = any_song_perfect
            ? font->get_string_size(U"获得 ", HORIZONTAL_ALIGNMENT_LEFT, -1, 30).x : 0.f;
        set_local_text_rect(note, (content_width - hint_width + phrase_width - note_width) * .5f + prefix_width,
            hint->get_position().y - note->get_size().y, note_width, note->get_size().y);
    }
    if (hard_skill_partner_clear && task_hint_font.is_valid()) {
        constexpr int hint_size = 30;
        constexpr int note_size = 18;
        const float hint_width = task_hint_font->get_string_size(display_hint,
            HORIZONTAL_ALIGNMENT_LEFT, -1, hint_size).x;
        const float prefix_width = task_hint_font->get_string_size(U"使用包含「困难」技能的搭档",
            HORIZONTAL_ALIGNMENT_LEFT, -1, hint_size).x;
        const float clear_width = task_hint_font->get_string_size(U"通关",
            HORIZONTAL_ALIGNMENT_LEFT, -1, hint_size).x;
        const String note_text = U"或 FULL LINK";
        const float note_width = task_hint_font->get_string_size(note_text,
            HORIZONTAL_ALIGNMENT_LEFT, -1, note_size).x;
        Label *note = caption(content, "HardSkillFullLinkNote", note_text, 0, 0,
            note_width, note_size, Color(225.f / 255, 217.f / 255, 191.f / 255), true);
        note->add_theme_font_override("font", task_hint_font);
        set_local_text_rect(note,
            (content_width - hint_width) * .5f + prefix_width + (clear_width - note_width) * .5f,
            hint->get_position().y - note->get_size().y * .8f,
            note_width, note->get_size().y);
    }
    const String action = konzetsu_dialog_mode == 2 ? "return" : konzetsu_dialog_mode == 1 ? "retrace" : "commence";
    const String normal = base + String("popup-btn-") + action + String(".png");
    const String pressed = base + String("popup-btn-") + action + String("-pressed.png");
    Ref<Texture2D> texture = load(normal);
    auto *confirm = art_button(content, "Confirm", normal,
         pressed, 0, -60,
        texture.is_valid() ? texture->get_width() * 1.005f : 450,
        texture.is_valid() ? texture->get_height() * 1.005f : 135);
    confirm->set_position(Vector2((content_width - confirm->get_size().x) * .5f,
        (480.f - 60.f) * cocos_to_canvas - confirm->get_size().y * .5f));
    confirm->connect("pressed", Callable(this, "confirm_konzetsu_challenge"));
    confirm->set_texture_pressed(load(pressed));
    confirm->set_disabled(konzetsu_dialog_mode != 2 && get_signal_connection_list("konzetsu_challenge_requested").is_empty());
    auto *close = art_button(visuals, "Close", base + String("popup-btn-x.png"), "", 745, 345, 40, 40);
    close->connect("pressed", Callable(this, "close_konzetsu_challenge"));
    visuals->set_pivot_offset(Vector2(W / 2, H / 2));
    visuals->set_scale(Vector2(1.1f, 1.1f));
    konzetsu_dialog->set_modulate(Color(1, 1, 1, 0));
    konzetsu_dialog_tween = create_tween();
    konzetsu_dialog_tween->set_parallel(true);
    konzetsu_dialog_tween->tween_property(visuals, "scale", Vector2(1, 1), .3)
        ->set_trans(Tween::TRANS_ELASTIC)->set_ease(Tween::EASE_OUT);
    konzetsu_dialog_tween->tween_property(konzetsu_dialog, "modulate:a", 1.f, .3);
    layout();
}

void ClientScreen::close_konzetsu_challenge() {
    if (!konzetsu_dialog || !konzetsu_dialog->is_visible() || konzetsu_dialog_closing) return;
    konzetsu_dialog_closing = true;
    if (konzetsu_dialog_tween.is_valid()) konzetsu_dialog_tween->kill();
    play_sfx("konzetsu/ui_close.wav");
    konzetsu_dialog_tween = create_tween();
    konzetsu_dialog_tween->set_parallel(true);
    konzetsu_dialog_tween->tween_property(konzetsu_dialog->get_node<Control>("PanelVisuals"), "scale", Vector2(1.1f, 1.1f), .3);
    konzetsu_dialog_tween->tween_property(konzetsu_dialog, "modulate:a", 0.f, .3);
    konzetsu_dialog_tween->chain()->tween_callback(Callable(konzetsu_dialog, "hide"));
}

void ClientScreen::confirm_konzetsu_challenge() {
    if (!konzetsu_dialog || !konzetsu_dialog->is_visible() || switching || konzetsu_dialog_closing) return;
    if (konzetsu_dialog_mode == 2) {
        close_konzetsu_challenge();
        return;
    }
    if (get_signal_connection_list("konzetsu_challenge_requested").is_empty()) return;
    const int index = konzetsu_challenge - 1;
    close_konzetsu_challenge();
    if (ResourceLoader::get_singleton()->exists("res://assets/resources/audio/konzetsu/start_challenge.wav"))
        play_sfx("konzetsu/start_challenge.wav");
    emit_signal("konzetsu_challenge_requested", index);
}

void ClientScreen::prepare_menu_dialogs() {
    menu_preparing = true;
    for (int i = 0; i < catalog.size(); ++i) {
        const Dictionary row = catalog[i];
        content::hue_material(row);
        content::hue_material(row, true);
    }
    build_character_select();
    partner_purchase_busy = memnew(Control);
    partner_purchase_busy->set_name("PurchaseRequireBusy");
    character_dialog->add_child(partner_purchase_busy);
    partner_purchase_busy->set_size(Vector2(W, H));
    partner_purchase_busy->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *busy_dim = memnew(ColorRect);
    busy_dim->set_name("Dimmer");
    partner_purchase_busy->add_child(busy_dim);
    busy_dim->set_color(Color(0, 0, 0, 128.f / 255.f));
    busy_dim->set_size(Vector2(W, H));
    busy_dim->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *activity = place(partner_purchase_busy, "ActivityIcon", "startup/activity_icon.png", 0, 0, 97, 146);
    if (activity) activity->set_pivot_offset(activity->get_size() * .5f);
    partner_purchase_busy->hide();
    toggle_all_characters();
    if (dialog_view_tween.is_valid()) dialog_view_tween->kill();
    dialog_view_all = false;
    dialog_content->reparent(dialog_list, false);
    dialog_all->hide();
    dialog_layout->set_modulate(Color(1, 1, 1));
    filter_characters("");
    show_partner_message("");
    partner_message_cache->hide();
    partner_modal = nullptr;
    const int selected = dialog_preview_character;
    open_partner_purchase();
    if (partner_purchase_cache) partner_purchase_cache->hide();
    partner_modal = nullptr;
    dialog_preview_character = selected;
    character_dialog->hide();
    dialog_dim_start = dialog_dim_target = 0;
    menu_preparing = false;
    for (const char *file : {"glow-ez.png", "glow-hard.png", "glow-normal.png"})
        menu_resources.push_back(load(String("characterselect/") + file));
    for (const char *file : {"06_LesserButtonClickV2.ogg", "08_TimelineNodeClick.ogg",
            "10_TimelineSpecialNodeClick.ogg", "25_VNIMText.ogg", "75_unlock.ogg"})
        menu_resources.push_back(ResourceLoader::get_singleton()->load(String("res://assets/resources/audio/sfx/") + file));
    content::warmup_partner_assets_begin();
}

void ClientScreen::open_character_select() {
    if (dialog_active || scene_kind != 1 || switching || !menu_assets_ready) return;
    play_sfx("sfx/06_LesserButtonClickV2.ogg");
    dialog_active = true;
    dialog_closing = false;
    dialog_pending_character = -1;
    character_dialog->show();
    dialog_layout->show();
    dialog_layout->set_modulate(Color(1, 1, 1));
    partner_details->hide();
    dialog_back->set_modulate(Color(1, 1, 1, 0));
    dialog_chrome->set_modulate(Color(1, 1, 1, 0));
    dialog_list->set_v_scroll(0);
    dialog_visual_content->set_position(Vector2());
    if (dialog_all) dialog_all->hide();
    dialog_chrome->get_node<LineEdit>("SearchField")->set_text("");
    filter_characters("");
    show_partner_preview(selected_character);
    dialog_portrait_seconds = 0;
    dialog_seconds = 0;
    dialog_dim_start = menu_dimmer->get_modulate().a;
    dialog_dim_target = .5f;
    dialog_dim_seconds = 0;
    layout();
}

void ClientScreen::build_character_select() {
    dialog_seconds = 0;
    dialog_portrait_seconds = 0;
    dialog_closing = false;
    dialog_view_all = false;
    dialog_preview_character = selected_character;
    dialog_pending_character = -1;
    dialog_locked_preview = false;
    dialog_portrait_slide = true;
    dialog_cells.clear();
    dialog_rest.clear();
    if (!menu_dimmer) {
        menu_dimmer = memnew(ColorRect);
        menu_dimmer->set_name("HubDimmer");
        stage->add_child(menu_dimmer);
        menu_dimmer->set_color(Color(0, 0, 0));
        menu_dimmer->set_size(Vector2(W, H));
        menu_dimmer->set_modulate(Color(1, 1, 1, 0));
        menu_dimmer->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    }
    dialog_dim_start = menu_dimmer->get_modulate().a;
    dialog_dim_target = .5f;
    dialog_dim_seconds = 0;
    character_dialog = memnew(Control);
    character_dialog->set_name("PartnerSelectDialog");
    stage->add_child(character_dialog);
    character_dialog->set_size(Vector2(W, H));
    character_dialog->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *shade = memnew(ColorRect);
    shade->set_name("InputCatcher");
    shade->set_color(Color(0, 0, 0, 0));
    shade->set_size(Vector2(W, H));
    shade->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    character_dialog->add_child(shade);
    dialog_layout = memnew(Control);
    dialog_layout->set_name("AsideLayout");
    character_dialog->add_child(dialog_layout);
    dialog_layout->set_size(Vector2(W, H));
    dialog_layout->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    dialog_back = memnew(Control);
    dialog_back->set_name("ChromeBack");
    dialog_layout->add_child(dialog_back);
    dialog_back->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    dialog_chrome = memnew(Control);
    dialog_chrome->set_name("Chrome");
    dialog_layout->add_child(dialog_chrome);
    dialog_chrome->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);

    // Original A-side is authored in a 1280x720, bottom-left CSB coordinate system.
    auto csb = [&](const char *name, const String &file, float x, float y, float source_w,
                   float source_h, float ax = .5f, float ay = .5f) -> TextureRect * {
        Control *parent = String(name) == "SearchBar" || String(name) == "NameBacking" || String(name) == "Divider"
            ? dialog_chrome : dialog_back;
        return place(parent, name, String("characterselect/") + file + String(".png"),
            x * 1.5f - W / 2, y * 1.5f - H / 2,
            source_w * 1.005f, source_h * 1.005f, ax, ay);
    };
    Dictionary selected_row = content::character(catalog, selected_character);
    const int char_type = int(selected_row.get("char_type", 0));
    csb("PartnerGlow", char_type == 1 ? "glow-ez" : char_type == 2 ? "glow-hard" : "glow-normal",
        857.6f, 0, 1620, 557, .5f, 0);
    csb("DiamondsBacking", "partner-diamonds-backing", 309, 0, 882, 1255, .5f, 0);
    csb("SkillContainer", "skill-container-2", 857.6f, 26, 690, 203, .5f, 0);
    csb("SkillContainerStats", "skill-container-stats-2", 857.6f, 24, 690, 203, .5f, 0);

    Ref<Texture2D> side_shadow = load("characterselect/partner-select-left-shadow.png");
    if (side_shadow.is_valid()) place(dialog_back, "LeftShadow", "characterselect/partner-select-left-shadow.png",
        -W / 2, -H / 2, side_shadow->get_width() * 1.005f, H, 0, 0);

    auto *list = memnew(ScrollContainer);
    list->set_name("PartnerSelectListView");
    dialog_layout->add_child(list);
    dialog_layout->move_child(list, dialog_chrome->get_index());
    list->set_position(Vector2(0, 180));
    list->set_size(Vector2(817.5f, 900));
    list->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
    list->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_SHOW_NEVER);
    list->set_deadzone(20);
    dialog_list = list;
    auto *cells = memnew(Control);
    cells->set_name("Content");
    list->add_child(cells);
    cells->set_mouse_filter(Control::MOUSE_FILTER_PASS);
    dialog_content = cells;
    auto *visual = memnew(Control);
    visual->set_name("VisualContent");
    cells->add_child(visual);
    visual->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    dialog_visual_content = visual;
    constexpr float cell_w = 169 * 1.005f, cell_h = 170 * 1.005f;
    Ref<DirAccess> directory = DirAccess::open(content::path("char"));
    std::vector<int> ids;
    if (directory.is_valid()) {
        for (const String &entry : directory->get_files()) {
            const String file = entry.trim_suffix(".remap");
            if (!file.ends_with("_icon.png")) continue;
            const String number = file.trim_suffix("_icon.png");
            if (!number.is_valid_int()) continue;
            int id = number.to_int();
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end(), [](int a, int b) {
        return (a == 5 ? 55.5f : float(a)) < (b == 5 ? 55.5f : float(b));
    });
    ids.erase(std::remove_if(ids.begin(), ids.end(), [](int id) {
        // Only these three partners are absent until unlocked. Other locked
        // partners remain visible as dimmed previews.
        return (id == 5 || id == 55 || id == 72) && !content::unlocked(id);
    }), ids.end());
    if (!ids.empty() && std::find(ids.begin(), ids.end(), selected_character) == ids.end()) {
        selected_character = ids.front();
        content::save_selected(selected_character);
    }
    const float gap = 12 * 1.5f;
    const int last = std::max(0, int(ids.size()) - 1);
    const float last_row = (last / 5) * (cell_h + 2 * gap)
        + (last % 5 >= 2 ? cell_h * .5f + gap : 0);
    cells->set_custom_minimum_size(Vector2(817.5f, std::max(900.f, 30.f + last_row + cell_h + 150.f)));
    cells->set_size(cells->get_custom_minimum_size());
    Ref<Texture2D> normal_border = content::image("characterselect/partner-border.png");
    Ref<Texture2D> selected_border = content::image("characterselect/partner-border-sel.png");
    Ref<Texture2D> lock_texture = load("characterselect/cell-lock2.png");
    Ref<Texture2D> max_border = content::image("characterselect/partner-border-max.png");
    Ref<Texture2D> awaken_border = content::image("characterselect/partner-border-awaken.png");
    for (int i = 0; i < int(ids.size()); ++i) {
        const int id = ids[i];
        Ref<Texture2D> icon = content::image("char/" + String::num_int64(id) + "_icon.png");
        if (!icon.is_valid()) continue;
        auto *cell = memnew(Control);
        cell->set_name("PartnerCell_" + String::num_int64(id));
        visual->add_child(cell);
        cell->set_position(partner_cell_position(i, false, W));
        cell->set_size(Vector2(cell_w, cell_h));
        cell->set_meta("character_id", id);
        cell->set_mouse_filter(Control::MOUSE_FILTER_PASS);
        auto *button = memnew(TextureButton);
        button->set_name("Icon");
        cell->add_child(button);
        button->set_texture_normal(icon);
        button->set_ignore_texture_size(true);
        button->set_stretch_mode(TextureButton::STRETCH_SCALE);
        button->set_size(Vector2(cell_w, cell_h));
        button->set_focus_mode(Control::FOCUS_NONE);
        button->connect("pressed", Callable(this, "select_character").bind(id));
        Ref<Texture2D> border = id == selected_character ? selected_border : normal_border;
        if (border.is_valid()) {
            auto *frame = memnew(TextureRect);
            frame->set_name("Border");
            cell->add_child(frame);
            frame->set_texture(border);
            frame->set_meta("normal_border", normal_border);
            frame->set_meta("max_border", max_border);
            frame->set_meta("awaken_border", awaken_border);
            frame->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            frame->set_stretch_mode(TextureRect::STRETCH_SCALE);
            frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            // Different border textures have different sizes:
            // normal: 194x194, sel: 234x234, max/awaken: 248x246
            const int cell_level = content::level_of(id);
            float base_size = id == selected_character ? 234.f : (cell_level >= 20 ? 248.f : 194.f);
            const float width = base_size * .9f * 1.005f;
            frame->set_size(Vector2(width, width));
            frame->set_position((Vector2(cell_w, cell_h) - frame->get_size()) / 2);
            if (id == selected_character) frame->set_material(content::hue_material(content::character(catalog, id)));
        }
        if (lock_texture.is_valid()) {
            auto *lock = memnew(TextureRect);
            lock->set_name("Lock");
            cell->add_child(lock);
            lock->set_texture(lock_texture);
            lock->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            lock->set_stretch_mode(TextureRect::STRETCH_SCALE);
            lock->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            const Vector2 size(198.f * .9f * 1.005f, 198.f * .9f * 1.005f);
            lock->set_size(size);
            lock->set_position((cell->get_size() - size) * .5f);
            lock->set_visible(!content::unlocked(id));
        }
        dialog_cells.push_back(cell);
        dialog_rest.push_back(cell->get_position());
        const float delay = std::min(float(dialog_cells.size() - 1) * .05f, 1.f);
        const float travel = (25.f + delay / .05f * 3.f) * 1.5f;
        cell->set_position(cell->get_position() + Vector2(0, travel));
        cell->set_modulate(Color(1, 1, 1, 0));
    }

    Ref<Texture2D> portrait_texture = content::image(String("char/1080/") + String::num_int64(selected_character) + ".png");
    if (portrait_texture.is_valid()) {
        auto *portrait = memnew(TextureRect);
        portrait->set_name("PartnerIllustration");
        dialog_back->add_child(portrait);
        portrait->set_texture(portrait_texture);
        portrait->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        portrait->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
        const Vector2 size(portrait_texture->get_width(), portrait_texture->get_height());
        portrait->set_position(at(575, -225, size.x, size.y));
        portrait->set_size(size);
        portrait->set_modulate(Color(1, 1, 1, 0));
        portrait->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        dialog_portrait = portrait;
        dialog_back->move_child(portrait, dialog_back->get_node<Node>("SkillContainer")->get_index());
    }

    const float search_width = (375.f + 105.f * .67f - 130.f) * 1.5f;
    Ref<Texture2D> search_texture = load("characterselect/search-bar-s.png");
    auto *search_bar = memnew(NinePatchRect);
    search_bar->set_name("SearchBar");
    dialog_chrome->add_child(search_bar);
    search_bar->set_texture(search_texture);
    search_bar->set_position(Vector2(195, 115.5f - 66.f * 1.005f / 2));
    search_bar->set_size(Vector2(search_width, 66 * 1.005f));
    search_bar->set_patch_margin(SIDE_LEFT, 145);
    search_bar->set_patch_margin(SIDE_RIGHT, 58);
    search_bar->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    auto *search = memnew(LineEdit);
    search->set_name("SearchField");
    dialog_chrome->add_child(search);
    search->set_position(Vector2((130.f + 77.05f) * 1.5f, 115.5f - 44.89f * 1.005f / 2));
    search->set_size(Vector2((375.f + 105.f * .67f - 130.f - 77.05f - 18.f) * 1.5f, 44.89f * 1.005f));
    search->set_placeholder(U"搜索");
    search->set_max_length(32);
    search->add_theme_font_size_override("font_size", 30);
    search->add_theme_color_override("font_color", Color(1, 1, 1));
    search->add_theme_color_override("font_placeholder_color", Color(1, 1, 1));
    Ref<StyleBoxEmpty> empty;
    empty.instantiate();
    for (const char *state : {"normal", "focus", "read_only"}) search->add_theme_stylebox_override(state, empty);
    search->connect("text_changed", Callable(this, "filter_characters"));
    csb("NameBacking", "name-backing", 1280, 635, 488, 131, 1, .5f);
    csb("Divider", "partners-divider", 276, 605, 512, 36, .5f, 1);
    art_button(dialog_chrome, "ViewAllButton", "characterselect/view-all-btn.png",
        "characterselect/view-all-btn-pressed.png", -510, -489, 441 * 1.005f, 102 * 1.005f)
        ->connect("pressed", Callable(this, "toggle_all_characters"));
    caption(dialog_chrome, "ViewAllCaption", U"查看全部", -510, -471, 450, 30, Color(1, 1, 1), true);
    auto *name = memnew(Label);
    name->set_name("PartnerName");
    dialog_chrome->add_child(name);
    name->set_text(String(selected_row.get("name", "")));
    name->set_position(Vector2(1884 - 480, 540 - 412 - 40));
    name->set_size(Vector2(480, 80));
    name->add_theme_font_size_override("font_size", 68);
    name->add_theme_color_override("font_color", Color(1, 1, 1));
    Ref<Font> regular = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
    if (regular.is_valid()) name->add_theme_font_override("font", regular);
    name->add_theme_color_override("font_outline_color", Color(12.f / 255, 12.f / 255, 16.f / 255));
    name->add_theme_constant_override("outline_size", 8);
    name->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    unfalsus_ui::place_ccc(name, Vector2(1884, 127.5f), 1.f, .5f, 520, 68, 3);
    if (Label *view = Object::cast_to<Label>(dialog_chrome->get_node_or_null("ViewAllCaption"))) {
        if (regular.is_valid()) view->add_theme_font_override("font", regular);
        unfalsus_ui::place_ccc(view, Vector2(W / 2 - 510, H / 2 - -471), .5f, .5f, 450, 30, 0);
    }
    const String illustrator = String(selected_row.get("illust", ""));
    auto *credit = caption(dialog_layout, "PartnerIllustrator",
        illustrator.is_empty() ? String() : String("illust: ") + illustrator,
        800, 470, 440, 24, Color(1, 1, 1));
    if (regular.is_valid()) credit->add_theme_font_override("font", regular);
    credit->add_theme_color_override("font_outline_color", Color(12.f / 255, 12.f / 255, 16.f / 255));
    credit->add_theme_constant_override("outline_size", 5);
    unfalsus_ui::place_ccc(credit, Vector2(1884, 42), 1.f, .5f, 500, 24, 2);

    const String skill_text = String(selected_row.get("skill_name", ""));
    auto *skill = memnew(Label);
    skill->set_name("SkillNameStats");
    dialog_back->add_child(skill);
    skill->set_text(skill_text);
    skill->set_position(at((857.6f - 640.f) * 1.5f, (108.f - 360.f) * 1.5f,
        1280.f * .67f * 1.5f, 108.f * 1.5f, .5f, .5f));
    skill->set_size(Vector2(1280.f * .67f * 1.5f, 108.f * 1.5f));
    skill->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    skill->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    skill->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
    skill->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    skill->add_theme_font_size_override("font_size", 24);
    skill->add_theme_color_override("font_color", Color(1, 1, 1));
    skill->add_theme_color_override("font_outline_color", Color(12.f / 255, 12.f / 255, 16.f / 255));
    skill->add_theme_constant_override("outline_size", 5);
    if (regular.is_valid()) skill->add_theme_font_override("font", regular);
    skill->set_clip_text(false);
    if (regular.is_valid()) search->add_theme_font_override("font", regular);
    auto *close = memnew(TextureButton);
    close->set_name("OverlayClose");
    character_dialog->add_child(close);
    close->set_texture_normal(load("characterselect/overlay-close.png"));
    close->set_ignore_texture_size(true);
    close->set_stretch_mode(TextureButton::STRETCH_SCALE);
    close->set_position(Vector2(65 * 1.5f - 24, 77 * 1.5f - 24));
    close->set_size(Vector2(48, 48));
    close->set_modulate(Color(1, 1, 1, 0));
    close->connect("pressed", Callable(this, "close_character_select"));
    dialog_back->set_modulate(Color(1, 1, 1, 0));
    dialog_chrome->set_modulate(Color(1, 1, 1, 0));
    const float skill_x = 1280.f * .67f;
    auto aside_label = [&](const char *name, const String &text, int size) {
        auto *label = memnew(Label);
        label->set_name(name);
        dialog_chrome->add_child(label);
        label->set_text(text);
        label->add_theme_font_size_override("font_size", int(std::round(size * 1.5f)));
        label->add_theme_color_override("font_color", Color(1, 1, 1));
        label->add_theme_color_override("font_outline_color", Color(12.f / 255, 12.f / 255, 16.f / 255));
        label->add_theme_constant_override("outline_size", 6);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        if (regular.is_valid()) label->add_theme_font_override("font", regular);
        label->set_clip_text(false);
        label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
        return label;
    };
    Ref<Font> jamjuree = unfalsus_ui::prepare_font("Bai-Jamjuree.ttf");
    Label *lv = aside_label("AsideLv", "lv.", 16);
    if (jamjuree.is_valid()) lv->add_theme_font_override("font", jamjuree);
    unfalsus_ui::place_ccc(lv, Vector2(W / 2 + (skill_x - 165.f - 640.f) * 1.5f, H / 2 - (155.f - 360.f) * 1.5f), 0.f, 0.f, 120, 24, 3);
    Label *level = aside_label("AsideLevel", "", 36);
    if (jamjuree.is_valid()) level->add_theme_font_override("font", jamjuree);
    level->add_theme_color_override("font_color", Color(1, 1, 1));
    level->add_theme_color_override("font_outline_color", Color(12.f / 255, 12.f / 255, 16.f / 255));
    level->add_theme_constant_override("outline_size", 6);
    unfalsus_ui::place_ccc(level, Vector2(W / 2 + (skill_x - 143.f - 640.f) * 1.5f, H / 2 - (150.f - 360.f) * 1.5f), 0.f, 0.f, 180, 54, 3);
    place(dialog_chrome, "AsideLevelBar", "characterselect/lvl-bar.png",
        (skill_x - 165.f - 640.f) * 1.5f, (150.f - 360.f) * 1.5f, 496.f * .67f * 1.5f, 11.f * .67f * 1.5f, 0, 0);
    const float full = 464.f * .67f * 1.5f;
    const float fill_h = 9.f * .67f * 1.5f;
    auto *clip = memnew(Control);
    clip->set_name("AsideLevelBarFill");
    dialog_chrome->add_child(clip);
    clip->set_clip_contents(true);
    clip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    clip->set_meta("full_width", full);
    clip->set_size(Vector2(full, fill_h));
    clip->set_position(at((skill_x - 154.f - 640.f) * 1.5f, (150.5f - 360.f) * 1.5f, full, fill_h, 0, 0));
    auto *stripe = memnew(TextureRect);
    clip->add_child(stripe);
    stripe->set_texture(load("characterselect/lvl-bar-fill.png"));
    stripe->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    stripe->set_stretch_mode(TextureRect::STRETCH_SCALE);
    stripe->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    stripe->set_size(Vector2(full, fill_h));
    TextureButton *uncap_toggle = memnew(TextureButton);
    uncap_toggle->set_name("AsideUncapToggle");
    dialog_chrome->add_child(uncap_toggle);
    const Ref<Texture2D> uncap_button = content::image("characterselect/skill-side-btn.png");
    uncap_toggle->set_texture_normal(uncap_button);
    uncap_toggle->set_texture_pressed(content::image("characterselect/skill-side-btn-pressed.png"));
    uncap_toggle->set_ignore_texture_size(true);
    uncap_toggle->set_stretch_mode(TextureButton::STRETCH_SCALE);
    uncap_toggle->set_focus_mode(Control::FOCUS_NONE);
    uncap_toggle->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
    const Vector2 uncap_button_size = uncap_button.is_valid()
        ? Vector2(uncap_button->get_width(), uncap_button->get_height()) * 1.5f
        : Vector2(120, 78);
    uncap_toggle->set_size(uncap_button_size);
    uncap_toggle->set_pivot_offset(uncap_button_size * .5f);
    uncap_toggle->set_scale(Vector2(.8f, .8f));
    TextureButton *skill_toggle = memnew(TextureButton);
    skill_toggle->set_name("AsideSkillToggle");
    dialog_chrome->add_child(skill_toggle);
    skill_toggle->set_texture_normal(uncap_button);
    skill_toggle->set_texture_pressed(content::image("characterselect/skill-side-btn-pressed.png"));
    skill_toggle->set_ignore_texture_size(true);
    skill_toggle->set_stretch_mode(TextureButton::STRETCH_SCALE);
    skill_toggle->set_focus_mode(Control::FOCUS_NONE);
    skill_toggle->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
    skill_toggle->set_size(uncap_button_size);
    skill_toggle->set_pivot_offset(uncap_button_size * .5f);
    skill_toggle->set_scale(Vector2(.8f, .8f));
    Vector2 awaken_button_cocos(602.6f, 118.f);
    if (Control *container = Object::cast_to<Control>(dialog_back->get_node_or_null("SkillContainer"))) {
        const Vector2 size = container->get_size();
        const float source_x = 857.6f;
        const float source_y = 26.f;
        const float source_h = size.y / 1.005f;
        const float source_scale = 0.67f;
        awaken_button_cocos = Vector2((source_x + 255.f),
            source_y + source_h * source_scale * .5f + 24.f);
    }
    uncap_toggle->set_position(at((awaken_button_cocos.x - 640.f) * 1.5f,
        (awaken_button_cocos.y - 360.f) * 1.5f, uncap_button_size.x, uncap_button_size.y));
    uncap_toggle->set_visible(false);
    uncap_toggle->connect("pressed", Callable(this, "toggle_partner_uncap"));
    const Ref<Texture2D> awaken_texture = content::image("characterselect/awaken-icon.png");
    auto *awaken = memnew(TextureRect);
    awaken->set_name("AwakenIcon");
    uncap_toggle->add_child(awaken);
    awaken->set_texture(awaken_texture);
    awaken->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    awaken->set_stretch_mode(TextureRect::STRETCH_SCALE);
    awaken->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    const Vector2 awaken_size = awaken_texture.is_valid()
        ? Vector2(awaken_texture->get_width(), awaken_texture->get_height()) * 1.5f
        : uncap_button_size * .42f;
    awaken->set_size(awaken_size);
    awaken->set_pivot_offset(awaken_size * .5f);
    awaken->set_scale(Vector2(.8f, .8f));
    awaken->set_position((uncap_button_size - awaken_size) * .5f);
    const Vector2 skill_button_cocos(awaken_button_cocos.x - 510.f, awaken_button_cocos.y);
    skill_toggle->set_position(at((skill_button_cocos.x - 640.f) * 1.5f,
        (skill_button_cocos.y - 360.f) * 1.5f, uncap_button_size.x, uncap_button_size.y));
    skill_toggle->set_visible(false);
    skill_toggle->connect("pressed", Callable(this, "toggle_partner_skill_seal"));
    TextureRect *skill_icon = memnew(TextureRect);
    skill_icon->set_name("SkillActiveIcon");
    dialog_chrome->add_child(skill_icon);
    const Ref<Texture2D> skill_active_texture = content::image("characterselect/skillactive-icon.png");
    skill_icon->set_texture(skill_active_texture);
    skill_icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    skill_icon->set_stretch_mode(TextureRect::STRETCH_SCALE);
    skill_icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    const Vector2 skill_icon_size = skill_active_texture.is_valid()
        ? Vector2(skill_active_texture->get_width(), skill_active_texture->get_height()) * 1.5f
        : uncap_button_size * .42f;
    skill_icon->set_size(skill_icon_size);
    skill_icon->set_pivot_offset(skill_icon_size * .5f);
    skill_icon->set_scale(Vector2(.8f, .8f));
    skill_icon->set_position(at((skill_button_cocos.x - 640.f) * 1.5f,
        (skill_button_cocos.y - 360.f) * 1.5f, skill_icon_size.x, skill_icon_size.y));
    Label *skill_state_label = memnew(Label);
    skill_state_label->set_name("SkillActiveText");
    dialog_chrome->add_child(skill_state_label);
    skill_state_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    skill_state_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
    const Vector2 skill_label_size(uncap_button_size.x, 36.f);
    skill_state_label->set_position(at((skill_button_cocos.x - 640.f) * 1.5f,
        (skill_button_cocos.y + 20.f - 360.f) * 1.5f, skill_label_size.x, skill_label_size.y));
    skill_state_label->set_size(skill_label_size);
    skill_state_label->add_theme_font_size_override("font_size", 21);
    Ref<Font> skill_font = unfalsus_ui::prepare_font("VivoSans-Regular-Full.ttf");
    if (skill_font.is_valid()) skill_state_label->add_theme_font_override("font", skill_font);
    skill_state_label->add_theme_color_override("font_color", Color(.745f, .725f, .796f));
    skill_state_label->add_theme_color_override("font_outline_color", Color(79.f / 255, 61.f / 255, 85.f / 255));
    skill_state_label->add_theme_constant_override("outline_size", 2);
    build_partner_chrome();
    apply_partner_aside(selected_character);
    layout();
}

void ClientScreen::apply_partner_aside(int id) {
    if (!character_dialog) return;
    const bool open = content::unlocked(id);
    dialog_locked_preview = !open;
    if (TextureButton *toggle = Object::cast_to<TextureButton>(dialog_chrome->get_node_or_null("AsideSkillToggle"))) {
        toggle->set_visible(open);
        toggle->set_disabled(menu_http_kind != 0 || partner_uncap_toggle_id >= 0);
        const bool sealed = content::skill_sealed();
        if (TextureRect *icon = Object::cast_to<TextureRect>(dialog_chrome->get_node_or_null("SkillActiveIcon")))
        {
            icon->set_visible(open);
            icon->set_texture(content::image(sealed
                ? "characterselect/lock-icon.png"
                : "characterselect/skillactive-icon.png"));
        }
        if (Label *text = Object::cast_to<Label>(dialog_chrome->get_node_or_null("SkillActiveText"))) {
            text->set_visible(open);
            text->set_text(sealed ? String(U"技能不可用") : String(U"技能生效中"));
        }
    }
    if (TextureButton *toggle = Object::cast_to<TextureButton>(dialog_chrome->get_node_or_null("AsideUncapToggle"))) {
        toggle->set_visible(open && content::uncapped(id));
        toggle->set_disabled(menu_http_kind != 0 || partner_uncap_toggle_id >= 0);
        if (TextureRect *icon = Object::cast_to<TextureRect>(toggle->get_node_or_null("AwakenIcon"))) {
            const String icon_path = content::uncapped_override(id)
                ? "characterselect/awaken-icon.png"
                : "characterselect/awaken-icon-closed.png";
            icon->set_texture(content::image(icon_path));
        }
    }
    const String id_text = String::num_int64(id);
    const bool use_u = content::uncapped_override(id);
    for (Control *cell : dialog_cells) {
        const int cell_id = int(cell->get_meta("character_id", -1));
        if (cell_id == id) {
            if (TextureButton *icon = Object::cast_to<TextureButton>(cell->get_node_or_null("Icon"))) {
                Ref<Texture2D> texture = use_u ? content::image("char/" + id_text + "u_icon.png") : Ref<Texture2D>();
                if (!texture.is_valid()) texture = content::image("char/" + id_text + "_icon.png");
                if (texture.is_valid()) icon->set_texture_normal(texture);
            }
        }
        const int cell_level = content::level_of(cell_id);
        const bool cell_selected = cell_id == dialog_preview_character;
        if (TextureRect *frame = Object::cast_to<TextureRect>(cell->get_node_or_null("Border"))) {
            if (!cell_selected) {
                const String key = cell_level == 30 ? "awaken_border"
                    : cell_level >= 20 ? "max_border" : "normal_border";
                Ref<Texture2D> frame_texture = frame->get_meta(key);
                if (frame_texture.is_valid()) frame->set_texture(frame_texture);
                // Update size based on border type
                float base_size = cell_level >= 20 ? 248.f : 194.f;
                const float width = base_size * .9f * 1.005f;
                frame->set_size(Vector2(width, width));
                frame->set_position((cell->get_size() - frame->get_size()) / 2);
            }
        }
    }
    const bool shows_level = open && menu_user_logged_in && menu_online_available;
    for (const char *name : {"AsideLv", "AsideLevel", "AsideLevelBar", "AsideLevelBarFill"}) {
        if (CanvasItem *node = Object::cast_to<CanvasItem>(character_dialog->find_child(name, true, false)))
            node->set_visible(shows_level);
    }
    if (Label *level = Object::cast_to<Label>(character_dialog->find_child("AsideLevel", true, false))) {
        const int value = content::level_of(id);
        const bool unknown_insight_level = content::insight_level_unknown(id, menu_user_logged_in);
        level->set_text(unknown_insight_level ? String("???")
            : value >= 0 ? String::num_int64(value)
            : !open && menu_user_logged_in && menu_online_available ? String("20") : String());
        Color text_color(1, 1, 1);
        Color outline_color(12.f / 255, 12.f / 255, 16.f / 255);
        if (value >= 20) {
            text_color = value == 30 ? Color(210.f / 255, 200.f / 255, 252.f / 255)
                : Color(183.f / 255, 247.f / 255, 1.f);
            outline_color = value == 30 ? Color(62.f / 255, 52.f / 255, 102.f / 255)
                : Color(14.f / 255, 50.f / 255, 54.f / 255);
        }
        level->add_theme_color_override("font_color", text_color);
        level->add_theme_color_override("font_outline_color", outline_color);
    }
    if (Control *fill = Object::cast_to<Control>(character_dialog->find_child("AsideLevelBarFill", true, false))) {
        const int value = content::level_of(id);
        const double gained = content::exp_into_level(id);
        const int limit = content::uncapped(id) ? 30 : 20;
        int need = 0;
        if (value >= 1 && value <= 4) need = 50;
        else if (value >= 5 && value <= 9) need = 50 * (value - 3);
        else if (value >= 10 && value <= 17) need = 100 * (value - 6);
        else if (value == 18) need = 1300;
        else if (value >= 19 && value <= 29) need = 1500;
        float fraction = 0;
        if (value >= limit) fraction = 1;
        else if (need > 0 && gained >= 0) fraction = std::clamp(float(gained / need), 0.f, 1.f);
        const float full = float(fill->get_meta("full_width", fill->get_size().x));
        fill->set_size(Vector2(full * fraction, fill->get_size().y));
    }
    if (dialog_portrait && !partner_details_open && !partner_detail_busy && dialog_portrait_seconds >= .45)
        dialog_portrait->set_modulate(Color(1, 1, 1, open ? 1.f : 153.f / 255.f));
    if (dialog_portrait && !partner_details_open && !partner_detail_busy) {
        const String portrait_path = "char/1080/" + id_text + (use_u ? "u.png" : ".png");
        Ref<Texture2D> portrait = content::image(portrait_path);
        if (!portrait.is_valid() && use_u) portrait = content::image("char/1080/" + id_text + ".png");
        if (portrait.is_valid()) {
            if (TextureRect *portrait_node = Object::cast_to<TextureRect>(dialog_portrait)) {
                const bool changed = portrait_node->get_texture() != portrait;
                portrait_node->set_texture(portrait);
                const Vector2 size(portrait->get_width(), portrait->get_height());
                dialog_portrait->set_size(size);
                if (changed) {
                    dialog_portrait->set_position(at(575, -225, size.x, size.y));
                    dialog_portrait->set_modulate(Color(1, 1, 1, 0));
                    dialog_portrait_seconds = 0;
                    dialog_portrait_slide = true;
                } else {
                    dialog_portrait->set_position(at(350, -225, size.x, size.y));
                }
            }
        }
    }
    if (CanvasItem *tag = Object::cast_to<CanvasItem>(character_dialog->find_child("AsideOfflineTag", true, false)))
        tag->set_visible(!open);
    
    if (TextureButton *level_tag = Object::cast_to<TextureButton>(character_dialog->find_child("AsideLevelUpTag", true, false))) {
        const int level = content::level_of(id);
        const bool online_session = menu_user_logged_in && menu_online_available;
        const bool awaken = id != 71 && level == 20 && content::uncappable(id) && !content::uncapped(id);
        const int cap = content::uncapped(id) ? 30 : 20;
        const int next_exp = level > 0 && level < cap ? content::next_level_exp(level) : -1;
        const double current_exp = content::exp_into_level(id);
        const int core_exp = content::core_exp_value();
        const int exp_needed = next_exp > 0 && current_exp >= 0 ? std::max(0, next_exp - int(current_exp)) : 0;
        const int cores_needed = core_exp > 0 ? (exp_needed + core_exp - 1) / core_exp : 0;
        const bool affordable = awaken || (cores_needed > 0 && content::ether_drop_count() >= cores_needed);
        const bool can_act = level > 0 && (awaken || level < cap) && affordable;
        const bool active = online_session && can_act;
        const String normal_asset = active
            ? (awaken ? String("level-awaken-available.png") : String("level-available.png"))
            : (awaken ? String("level-awaken.png") : String("level-tag.png"));
        const String pressed_asset = active
            ? (awaken ? String("level-awaken-available-pressed.png") : String("level-tag-available-pressed.png"))
            : (awaken ? String("level-awaken-pressed.png") : String("level-tag-pressed.png"));
        level_tag->set_texture_normal(content::image(String("characterselect/") + normal_asset));
        level_tag->set_texture_pressed(content::image(String("characterselect/") + pressed_asset));
        level_tag->set_texture_disabled(content::image(String("characterselect/") + (awaken
            ? "level-awaken.png" : "level-tag.png")));
        level_tag->set_visible(false);
        level_tag->set_disabled(!active || menu_http_kind != 0 || partner_uncap_toggle_id >= 0);
        if (Label *caption = Object::cast_to<Label>(level_tag->get_node_or_null("AsideLevelUp")))
            caption->set_text(awaken ? String(U"觉醒") : String(U"升级"));
    }
    
    if (Label *multiplier = Object::cast_to<Label>(character_dialog->find_child("CharacterFragMultiplier", true, false))) {
        const int frag = content::frag_of(id);
        if (frag < 0) {
            multiplier->set_text("");
            multiplier->set_visible(false);
        } else {
            const double value = std::floor((frag / 50.0) * 100.0) / 100.0;
            multiplier->set_text(String(U"比特收集倍率×") + String::num(value, 2));
            multiplier->set_visible(true);
        }
    }
    if (partner_shown_id != id) {
        partner_shown_id = id;
        partner_preview_level = open ? -1 : 1;
    }
    refresh_partner_details();
}

void ClientScreen::select_character(int id) {
    if (partner_scroll_suppress_click) return;
    if (!dialog_active || dialog_closing) return;
    play_sfx("sfx/08_TimelineNodeClick.ogg");
    if (content::character(catalog, id).is_empty()) return;
    if (content::unlocked(id)) dialog_pending_character = id;
    show_partner_preview(id);
}

void ClientScreen::show_partner_preview(int id) {
    if (!character_dialog || dialog_closing) return;
    Dictionary row = content::character(catalog, id);
    if (row.is_empty()) return;
    const int previous_id = dialog_preview_character;
    const bool changed = dialog_preview_character != id;
    dialog_preview_character = id;
    auto *name = Object::cast_to<Label>(dialog_chrome->get_node_or_null("PartnerName"));
    if (name) name->set_text(String(row.get("name", "")));
    auto *credit = Object::cast_to<Label>(dialog_layout->get_node_or_null("PartnerIllustrator"));
    const String illustrator = String(row.get("illust", ""));
    if (credit) credit->set_text(illustrator.is_empty() ? String() : String("illust: ") + illustrator);
    auto *skill = Object::cast_to<Label>(dialog_back->get_node_or_null("SkillNameStats"));
    if (skill) skill->set_text(String(row.get("skill_name", "")));
    const int char_type = int(row.get("char_type", 0));
    auto *glow = dialog_back->get_node<TextureRect>("PartnerGlow");
    glow->set_texture(load(char_type == 1 ? "characterselect/glow-ez.png"
        : char_type == 2 ? "characterselect/glow-hard.png" : "characterselect/glow-normal.png"));
    auto *preview = Object::cast_to<TextureRect>(dialog_portrait);
    const String portrait_base = "char/1080/" + String::num_int64(id);
    const bool use_u = content::uncapped_override(id);
    Ref<Texture2D> portrait = use_u ? content::image(portrait_base + String("u.png")) : Ref<Texture2D>();
    if (!portrait.is_valid()) portrait = content::image(portrait_base + String(".png"));
    if (preview && portrait.is_valid()) {
        preview->set_texture(portrait);
        const Vector2 size(portrait->get_width(), portrait->get_height());
        preview->set_size(size);
        const float alpha = content::unlocked(id) ? 1.f : 153.f / 255.f;
        dialog_portrait_slide = true;
        if (changed) {
            preview->set_position(at(575, -225, size.x, size.y));
            preview->set_modulate(Color(1, 1, 1, 0));
            dialog_portrait_seconds = 0;
        } else {
            preview->set_position(at(350, -225, size.x, size.y));
            preview->set_modulate(Color(1, 1, 1, alpha));
            dialog_portrait_seconds = .45;
        }
    }
    const Ref<ShaderMaterial> selected_material = content::hue_material(row);
    for (Control *cell : dialog_cells) {
        const int cell_id = int(cell->get_meta("character_id"));
        if (cell_id != id && cell_id != previous_id) continue;
        auto *border = Object::cast_to<TextureRect>(cell->get_node_or_null("Border"));
        if (!border) continue;
        const bool selected = cell_id == id;
        const int cell_level = content::level_of(cell_id);
        const String border_path = selected ? "characterselect/partner-border-sel.png"
            : cell_level == 30 ? "characterselect/partner-border-awaken.png"
            : cell_level >= 20 ? "characterselect/partner-border-max.png"
            : "characterselect/partner-border.png";
        border->set_texture(content::image(border_path));
        border->set_material(selected ? selected_material : Ref<ShaderMaterial>());
        // Different border textures have different sizes:
        // normal: 194x194, sel: 234x234, max/awaken: 248x246
        // Use 0.9 scale like original
        float base_size = selected ? 234.f : (cell_level >= 20 ? 248.f : 194.f);
        const float width = base_size * .9f * 1.005f;
        border->set_size(Vector2(width, width));
        border->set_position((cell->get_size() - border->get_size()) / 2);
    }
    if (dialog_view_all) {
        toggle_all_characters();
        for (size_t i = 0; i < dialog_cells.size(); ++i) {
            if (int(dialog_cells[i]->get_meta("character_id")) == id) {
                dialog_list->set_v_scroll(std::max(0, int(dialog_rest[i].y - (900 - dialog_cells[i]->get_size().y) / 2)));
                break;
            }
        }
    }
    apply_partner_aside(id);
}

void ClientScreen::filter_characters(const String &raw) {
    if (!character_dialog || dialog_closing || !dialog_content) return;
    const String query = raw.strip_edges().to_lower();
    int visible = 0;
    float bottom = 0;
    for (size_t i = 0; i < dialog_cells.size(); ++i) {
        Control *cell = dialog_cells[i];
        const Dictionary row = content::character(catalog, int(cell->get_meta("character_id")));
        const bool matched = query.is_empty() || String(row.get("name", "")).to_lower().contains(query)
            || String(row.get("shape_name", "")).to_lower().contains(query);
        cell->set_visible(matched);
        if (!matched) continue;
        const Vector2 position = partner_cell_position(visible++, dialog_view_all,
            dialog_view_all ? W + design_extra : W);
        dialog_rest[i] = position;
        cell->set_position(position);
        cell->set_modulate(Color(1, 1, 1));
        bottom = std::max(bottom, position.y + cell->get_size().y);
    }
    const Vector2 minimum(dialog_view_all ? W + design_extra : 817.5f, std::max(dialog_view_all ? H : 900.f, bottom + 150));
    dialog_content->set_custom_minimum_size(minimum);
    dialog_content->set_size(minimum);
    if (dialog_visual_content) dialog_visual_content->set_size(minimum);
    (dialog_view_all ? dialog_all_list : dialog_list)->set_v_scroll(0);
    dialog_seconds = std::max(dialog_seconds, 1.15);
}

void ClientScreen::toggle_all_characters() {
    if (!character_dialog || dialog_closing) return;
    partner_scroll_velocity = 0;
    partner_scroll_inertia_list = nullptr;
    partner_scroll_pointer_active = false;
    partner_scroll_overscroll = 0;
    partner_scroll_bounce_start = 0;
    partner_scroll_bounce_elapsed = 0;
    if (dialog_visual_content) dialog_visual_content->set_position(Vector2());
    if (!menu_preparing) play_sfx("sfx/06_LesserButtonClickV2.ogg");
    if (!dialog_all) {
        dialog_all = memnew(Control);
        dialog_all->set_name("ViewAllLayer");
        character_dialog->add_child(dialog_all);
        dialog_all->set_size(Vector2(W, H));
        dialog_all->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        place(dialog_all, "LeftShadow", "characterselect/partner-select-left-shadow.png",
            -W / 2, -H / 2, 849 * 1.005f, H, 0, 0);
        auto *right = place(dialog_all, "RightShadow", "characterselect/partner-select-left-shadow.png",
            W / 2, -H / 2, 849 * 1.005f, H, 1, 0);
        if (right) right->set_flip_h(true);
        dialog_all_list = memnew(ScrollContainer);
        dialog_all_list->set_name("ViewAllViewport");
        dialog_all->add_child(dialog_all_list);
        dialog_all_list->set_size(Vector2(W, H));
        dialog_all_list->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
        dialog_all_list->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_SHOW_NEVER);
        dialog_all_list->set_deadzone(20);
        character_dialog->move_child(character_dialog->get_node<Node>("OverlayClose"), character_dialog->get_child_count() - 1);
    }
    dialog_view_all = !dialog_view_all;
    auto *search = dialog_chrome->get_node<LineEdit>("SearchField");
    search->set_text("");
    search->release_focus();
    dialog_content->reparent(dialog_view_all ? dialog_all_list : dialog_list, false);
    if (dialog_view_tween.is_valid()) dialog_view_tween->kill();
    dialog_layout->set_visible(true);
    dialog_all->set_visible(true);
    // Keep both layers alive for the duration of the cross-fade.
    if (dialog_view_all) {
        dialog_layout->set_modulate(Color(1, 1, 1, 1));
        dialog_all->set_modulate(Color(1, 1, 1, 0));
    } else {
        dialog_layout->set_modulate(Color(1, 1, 1, 0));
        dialog_all->set_modulate(Color(1, 1, 1, 1));
    }
    dialog_view_tween = character_dialog->create_tween();
    dialog_view_tween->set_parallel(true);
    dialog_view_tween->tween_property(dialog_layout, "modulate:a", dialog_view_all ? 0.f : 1.f, 20. / 60.)
        ->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_OUT);
    dialog_view_tween->tween_property(dialog_all, "modulate:a", dialog_view_all ? 1.f : 0.f, .15)
        ->set_trans(Tween::TRANS_CUBIC)->set_ease(Tween::EASE_OUT);
    filter_characters("");
    layout();
}

static bool find_int(const Variant &value, const String &key, int &out) {
    if (value.get_type() == Variant::ARRAY) {
        const Array list = value;
        for (int i = 0; i < list.size(); ++i)
            if (find_int(list[i], key, out)) return true;
        return false;
    }
    if (value.get_type() != Variant::DICTIONARY) return false;
    const Dictionary object = value;
    const Array keys = object.keys();
    for (int i = 0; i < keys.size(); ++i) {
        if (String(keys[i]).to_lower() != key) continue;
        const Variant child = object[keys[i]];
        if (child.get_type() == Variant::INT || child.get_type() == Variant::FLOAT) { out = int(child); return true; }
        if (child.get_type() == Variant::STRING && String(child).is_valid_int()) { out = String(child).to_int(); return true; }
    }
    for (int i = 0; i < keys.size(); ++i)
        if (find_int(object[keys[i]], key, out)) return true;
    return false;
}

static String menu_base_url() {
    String url = String(ProjectSettings::get_singleton()->get_setting("unfalsus/api_base_url", "")).strip_edges();
    const String environment = OS::get_singleton()->get_environment("UNFALSUS_API_BASE_URL");
    if (!environment.is_empty()) url = environment.strip_edges();
    return arcapi::resolve_base_url(url);
}

static String menu_token() {
    Ref<ConfigFile> config;
    config.instantiate();
    if (config->load("user://startup.cfg") != OK) return "";
    return String(config->get_value("auth", "token", ""));
}

void ClientScreen::position_hub_portrait(float slide_offset) {
    hub_portrait_slide_offset = slide_offset;
    if (!stage) return;
    Control *portrait = Object::cast_to<Control>(stage->get_node_or_null("PartnerIllustration"));
    if (!portrait) return;
    const Vector2 viewport_size = get_size();
    const float scale = std::min(viewport_size.x / W, viewport_size.y / H);
    const float extra = scale > 0 ? std::max(0.f, viewport_size.x / scale - W) : 0.f;
    const Vector2 size = portrait->get_size();
    portrait->set_position(at(-330, -225, size.x, size.y) - Vector2(extra * .5f, 0)
        + Vector2(slide_offset, 0));
}

void ClientScreen::apply_hub_partner(int id, bool slide) {
    const Dictionary row = content::character(catalog, id);
    if (row.is_empty()) return;
    selected_character = id;
    content::save_selected(id);
    const String partner_name = row.get("name", "");
    if (Label *label = Object::cast_to<Label>(stage->find_child("CharacterName", true, false))) label->set_text(partner_name);
    if (Label *label = Object::cast_to<Label>(stage->find_child("CharacterNameShadow", true, false))) label->set_text(partner_name);
    const Ref<ShaderMaterial> chrome = content::hue_material(row);
    for (const char *key : {"CharacterBracketBL", "CharacterBracketTR"})
        if (art.count(key)) art[key]->set_material(chrome);
    if (TextureButton *button = Object::cast_to<TextureButton>(stage->find_child("CharacterSelectHit", true, false)))
        button->set_material(chrome);
    if (art.count("CharacterColorStrip")) {
        art["CharacterColorStrip"]->set_visible(!bool(row.get("is_grey", false)));
        art["CharacterColorStrip"]->set_material(content::hue_material(row, true));
    }
    const Ref<Texture2D> portrait = content::image(String("char/1080/") + String::num_int64(id) + ".png");
    if (!portrait.is_valid()) return;
    TextureRect *node = art.count("PartnerIllustration") ? Object::cast_to<TextureRect>(art["PartnerIllustration"]) : nullptr;
    const Vector2 size(portrait->get_width(), portrait->get_height());
    if (!node) {
        node = memnew(TextureRect);
        node->set_name("PartnerIllustration");
        stage->add_child(node);
        node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
         node->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
        node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        art["PartnerIllustration"] = node;
    }
    node->set_texture(portrait);
    node->set_size(size);
    if (hub_portrait_tween.is_valid()) hub_portrait_tween->kill();
    if (!slide) {
        hub_portrait_slide_seconds = .45;
        position_hub_portrait(0);
        node->set_modulate(Color(1, 1, 1, 1));
        return;
    }
    hub_portrait_slide_seconds = 0;
    position_hub_portrait(225);
    node->set_modulate(Color(1, 1, 1, 0));
    Ref<Tween> tween = node->create_tween();
    hub_portrait_tween = tween;
    tween->set_parallel(true);
    tween->tween_property(node, "modulate:a", 1.f, .45)->set_trans(Tween::TRANS_CUBIC)->set_ease(Tween::EASE_OUT);
}

void ClientScreen::menu_fetch_user() {
    if (!menu_http || menu_http_kind != 0) return;
    const String token = menu_token();
    const String base = menu_base_url();
    menu_user_logged_in = !token.is_empty();
    if (token.is_empty() || base.is_empty()) {
        menu_online_available = false;
        if (character_dialog) apply_partner_aside(dialog_preview_character);
        return;
    }
    PackedStringArray headers;
    headers.push_back("Authorization: Bearer " + token.trim_prefix("Bearer ").trim_prefix("bearer "));
    headers.push_back("AppVersion: 1.1");
    headers.push_back("Accept: application/json");
    menu_http_kind = 1;
    if (menu_http->request(base.trim_suffix("/") + "/user/online", headers) != OK) {
        menu_http_kind = 0;
        menu_online_available = false;
        if (character_dialog) apply_partner_aside(dialog_preview_character);
        show_partner_message(U"无法连接至服务器");
    }
}

void ClientScreen::menu_fetch_game_info() {
    if (!menu_http || menu_http_kind != 0) return;
    const String base = menu_base_url();
    if (base.is_empty()) return;
    PackedStringArray headers;
    headers.push_back("Accept: application/json");
    menu_http_kind = 7; // game/info request
    if (menu_http->request(base.trim_suffix("/") + "/game/info", headers) != OK) {
        menu_http_kind = 0;
    }
}

void ClientScreen::menu_commit_character(int id) {
    if (!menu_http || menu_http_kind != 0) return;
    const String token = menu_token();
    const String base = menu_base_url();
    if (token.is_empty() || base.is_empty()) return;
    PackedStringArray headers;
    headers.push_back("Authorization: Bearer " + token.trim_prefix("Bearer ").trim_prefix("bearer "));
    headers.push_back("AppVersion: 1.1");
    headers.push_back("Accept: application/json");
    headers.push_back("Content-Type: application/x-www-form-urlencoded");
    menu_commit_id = id;
    menu_http_kind = 2;
    const String body = "character=" + String::num_int64(id) + "&skill_sealed=" + (content::skill_sealed() ? "true" : "false");
    if (menu_http->request(base.trim_suffix("/") + "/user/online/character", headers, HTTPClient::METHOD_POST, body) != OK) {
        menu_http_kind = 0;
        show_partner_message(U"无法连接至服务器");
    } else {
        begin_center_activity();
    }
}

void ClientScreen::menu_http_completed(int result, int response_code, const PackedStringArray &, const PackedByteArray &body) {
    const int kind = menu_http_kind;
    const int commit = menu_commit_id;
    menu_http_kind = 0;
    if (kind == 1) menu_online_available = false;
    if (kind == 7) {
        if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) return;
        const String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).strip_edges();
        const Variant parsed = JSON::parse_string(text);
        if (parsed.get_type() == Variant::DICTIONARY) {
            const Dictionary response = parsed;
            if (response.has("value") && response["value"].get_type() == Variant::DICTIONARY) {
                const Dictionary game_info = response["value"];
                content::remember_game_info(game_info);
        }
    }
    if (character_dialog) apply_partner_aside(dialog_preview_character);
        return;
    }
    if (kind == 6) {
        // Level-up response
        partner_uncap_toggle_id = -1;
        if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) {
            end_partner_purchase_activity(false);
            if (character_dialog) show_partner_message(result != HTTPRequest::RESULT_SUCCESS ? String(U"无法连接至服务器") : String(U"发生了未知错误"));
            return;
        }
        const String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).strip_edges();
        const Variant parsed = JSON::parse_string(text);
        bool success = response_code >= 200 && response_code < 300;
        if (parsed.get_type() == Variant::DICTIONARY) {
            const Dictionary response = parsed;
            if (response.has("success")) {
                const Variant flag = response["success"];
                success = flag == Variant(true) || int(flag) == 1 || String(flag).to_lower() == "true";
            }
        }
        if (!success) {
            end_partner_purchase_activity(false);
            if (character_dialog) show_partner_message(U"升级失败");
            return;
        }
        // Refresh user data to get updated character stats
        partner_uncap_refresh_pending = true;
        menu_fetch_user();
        if (menu_http_kind != 1) {
            partner_uncap_refresh_pending = false;
            end_partner_purchase_activity(false);
        }
        return;
    }
    if (kind == 5) {
        partner_uncap_toggle_id = -1;
        if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) {
            end_partner_purchase_activity(false);
            if (character_dialog) apply_partner_aside(dialog_preview_character);
            if (character_dialog) show_partner_message(result != HTTPRequest::RESULT_SUCCESS ? String(U"无法连接至服务器") : String(U"发生了未知错误"));
            return;
        }
        const String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).strip_edges();
        const Variant parsed = JSON::parse_string(text);
        bool success = response_code >= 200 && response_code < 300;
        if (parsed.get_type() == Variant::DICTIONARY) {
            const Dictionary response = parsed;
            if (response.has("success")) {
                const Variant flag = response["success"];
                success = flag == Variant(true) || int(flag) == 1 || String(flag).to_lower() == "true";
            }
        }
        if (!success) {
            end_partner_purchase_activity(false);
            if (character_dialog) apply_partner_aside(dialog_preview_character);
            if (character_dialog) show_partner_message(U"发生了未知错误");
            return;
        }
        partner_uncap_refresh_pending = true;
        menu_fetch_user();
        if (menu_http_kind != 1) {
            partner_uncap_refresh_pending = false;
            end_partner_purchase_activity(false);
        }
        return;
    }
    if (kind == 4) {
        if (result != HTTPRequest::RESULT_SUCCESS) fail_partner_purchase_check(U"无法连接至服务器");
        else if (response_code != 200) fail_partner_purchase_check(U"发生了未知错误");
        else partner_purchase_score_result(body, true);
        return;
    }
    if ((kind == 2 || kind == 3) && partner_purchase_busy && partner_purchase_busy->is_visible())
        end_partner_purchase_activity(false);
    const bool uncap_refresh = kind == 1 && partner_uncap_refresh_pending;
    if (uncap_refresh) {
        partner_uncap_refresh_pending = false;
        end_partner_purchase_activity(false);
    }
    if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) {
        if (kind == 1 && character_dialog) apply_partner_aside(dialog_preview_character);
        if (partner_details_open) refresh_partner_details();
        if ((kind == 1 || kind == 2 || kind == 3) && character_dialog)
            show_partner_message(result != HTTPRequest::RESULT_SUCCESS ? String(U"无法连接至服务器") : String(U"发生了未知错误"));
        return;
    }
    if (kind == 1 || kind == 2 || kind == 3) {
        const Variant parsed = JSON::parse_string(String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()));
        if (parsed.get_type() == Variant::DICTIONARY) {
            const Dictionary object = parsed;
            if (object.has("success")) {
                const Variant flag = object["success"];
                if (!(flag == Variant(true) || int(flag) == 1 || String(flag).to_lower() == "true")) {
                    if (kind == 1 && character_dialog) apply_partner_aside(dialog_preview_character);
                    show_partner_message(U"发生了未知错误");
                    return;
                }
            }
        }
    }
    if (kind == 1) {
        const String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).strip_edges();
        if (!text.begins_with("{") && !text.begins_with("[")) {
            if (character_dialog) apply_partner_aside(dialog_preview_character);
            if (uncap_refresh) {
                if (character_dialog) show_partner_message(U"发生了未知错误");
            }
            return;
        }
        int id = -1;
        const Variant parsed = JSON::parse_string(text);
        if (uncap_refresh && parsed.get_type() != Variant::DICTIONARY && parsed.get_type() != Variant::ARRAY) {
            apply_partner_aside(dialog_preview_character);
            if (character_dialog) show_partner_message(U"发生了未知错误");
            return;
        }
        if (parsed.get_type() != Variant::DICTIONARY && parsed.get_type() != Variant::ARRAY) {
            if (character_dialog) apply_partner_aside(dialog_preview_character);
            return;
        }
        menu_online_available = true;
        content::remember_online(parsed);
        if (content::level_exp_table().is_empty()) menu_fetch_game_info();
        find_int(parsed, "character", id);
        if (uncap_refresh || id == selected_character) apply_hub_partner(selected_character, false);
        if (partner_details_open) refresh_partner_details();
        for (Control *cell : dialog_cells) {
            if (Control *lock = Object::cast_to<Control>(cell->get_node_or_null("Lock")))
                lock->set_visible(!content::unlocked(int(cell->get_meta("character_id"))));
        }
        if (partner_grant_id >= 0) {
            const int bought = partner_grant_id;
            partner_grant_id = -1;
            if (!content::unlocked(bought)) content::grant_purchased(bought);
        }
        if (character_dialog) apply_partner_aside(dialog_preview_character);
        if (id < 0 || id == selected_character) return;
        apply_hub_partner(id, true);
        return;
    }
    if (kind == 3) {
        const String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).strip_edges();
        bool bought = false;
        if (text.begins_with("{")) {
            const Variant parsed = JSON::parse_string(text);
            if (parsed.get_type() == Variant::DICTIONARY) {
                const Variant flag = Dictionary(parsed).get("success", false);
                bought = flag == Variant(true) || int(flag) == 1 || String(flag).to_lower() == "true";
            }
        }
        if (!bought) {
            if (character_dialog) show_partner_message(U"发生了未知错误");
            return;
        }
        content::grant_purchased(commit);
        const int price = content::base_frag(commit);
        const int balance = content::ticket();
        if (price >= 0 && balance >= price) content::set_ticket(balance - price);
        partner_grant_id = commit;
        if (character_dialog) present_purchased_partner(commit);
        menu_fetch_user();
        if (menu_http_kind != 1) partner_grant_id = -1;
        return;
    }
    if (kind == 2) {
        apply_hub_partner(commit, true);
        play_sfx("sfx/10_TimelineSpecialNodeClick.ogg");
        begin_close_character_select();
    }
}

void ClientScreen::close_character_select() {
    if (dialog_active && !dialog_closing && menu_http_kind == 4 && partner_modal) {
        close_partner_modal();
        return;
    }
    if (!dialog_active || dialog_closing || menu_http_kind != 0) return;
    if (partner_modal) {
        close_partner_modal();
        return;
    }
    if (partner_details_open || partner_detail_busy) {
        if (!partner_detail_busy) play_sfx("sfx/08_TimelineNodeClick.ogg");
        close_partner_details();
        return;
    }
    if (dialog_view_all) {
        play_sfx("sfx/08_TimelineNodeClick.ogg");
        toggle_all_characters();
        return;
    }
    if (dialog_locked_preview) {
        play_sfx("sfx/08_TimelineNodeClick.ogg");
        dialog_pending_character = -1;
        show_partner_preview(selected_character);
        dialog_locked_preview = false;
        return;
    }
    if (dialog_pending_character >= 0 && dialog_pending_character != selected_character) {
        if (menu_token().is_empty()) {
            apply_hub_partner(dialog_pending_character, true);
            play_sfx("sfx/10_TimelineSpecialNodeClick.ogg");
            begin_close_character_select();
        } else menu_commit_character(dialog_pending_character);
        return;
    }
    play_sfx("sfx/08_TimelineNodeClick.ogg");
    begin_close_character_select();
}

void ClientScreen::begin_close_character_select() {
    if (!character_dialog || dialog_closing) return;
    if (dialog_view_tween.is_valid()) dialog_view_tween->kill();
    dialog_dim_start = menu_dimmer->get_modulate().a;
    dialog_dim_target = 0;
    dialog_dim_seconds = 0;
    dialog_closing = true;
    dialog_seconds = 0;
}
