#include "client.hpp"
#include "content.hpp"
#include "arcapi.hpp"
#include "ui_font.hpp"

#include <godot_cpp/classes/button.hpp>
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
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/core/memory.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace godot;

namespace {
static constexpr float W = 1920, H = 1080;
static constexpr const char *ROOT = "res://assets/resources/img/";

static Vector2 at(float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    return Vector2(W / 2 + x - w * ax, H / 2 - y - h * (1 - ay));
}

static Ref<Texture2D> load(const String &path) {
    return ResourceLoader::get_singleton()->load(ROOT + path);
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
        const String &hover, float x, float y, float w, float h) {
    auto *node = memnew(TextureButton);
    node->set_name(name);
    parent->add_child(node);
    node->set_texture_normal(load(normal));
    if (!hover.is_empty()) node->set_texture_hover(load(hover));
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
    music_hit->connect("mouse_entered", Callable(this, "menu_hover").bind(1, 1));
    music_hit->connect("mouse_exited", Callable(this, "menu_hover").bind(1, 0));
    art_button(stage, "TutorialHit", "mainmenu/btn-replaytutorial.png", "mainmenu/btn-replaytutorial-hover.png",
        628.5, 381.6, 340, 75);
    TextureButton *timeline_hit = art_button(stage, "TimelineHit", "mainmenu/btn-default.png", "mainmenu/btn-hover.png",
        680.6, -35.2, 280, 90);
    timeline_hit->connect("pressed", Callable(this, "on_menu_action").bind(2));
    timeline_hit->connect("mouse_entered", Callable(this, "menu_hover").bind(2, 1));
    timeline_hit->connect("mouse_exited", Callable(this, "menu_hover").bind(2, 0));
    art_button(stage, "SettingsHit", "mainmenu/button-menu.png", "mainmenu/button-menu-hover.png",
        624.7, 501.7, 89.5, 70);

    const Color cyan(218.f / 255, 254.f / 255, 254.f / 255);
    const Color cyan_title(217.f / 255, 254.f / 255, 254.f / 255);
    const Color ink(15.f / 255, 25.f / 255, 36.f / 255);
    const Color teal(66.f / 255, 199.f / 255, 198.f / 255);
    caption(stage, "PlayerTag", U"玩家", -833.5, 435, 54, 11, ink, true, true);
    caption(stage, "MusicTitleShadow", U"音乐游玩", 36.119, 379.97, 300, 26, Color(cyan_title.r, cyan_title.g, cyan_title.b, 66.f / 255), false, true);
    caption(stage, "MusicTitle", U"音乐游玩", 37.111, 380.957, 300, 26, cyan_title, false, true);
    caption(stage, "DifficultyShadow", U"进化", 85.131, 256.696, 220, 15, Color(teal.r, teal.g, teal.b, 57.f / 255), false, true);
    caption(stage, "Difficulty", U"进化", 86.098, 257.664, 220, 15, teal, false, true);
    caption(stage, "Unlocked", U"已解锁", 110.855, 230.555, 110, 11, cyan, false, true);
    caption(stage, "UnlockedValue", "05/234", 134.786, 209.934, 120, 19, cyan, false, true);
    caption(stage, "Cleared", U"已完成", 245.83, 230.745, 100, 11, cyan, false, true);
    caption(stage, "ClearedValue", "05/234", 270.423, 209.148, 120, 19, cyan, false, true);
    caption(stage, "SelectSong", U"选择歌曲", 679.154, 226.761, 250, 23, cyan, true, true);
    caption(stage, "ReplayTutorial", U"重播教程", 659.884, 379.375, 280, 20, Color(193.f / 255, 224.f / 255, 227.f / 255), true, true);
    caption(stage, "ScenarioTitleShadow", U"剧情", 116.16, 110.196, 260, 26, Color(cyan_title.r, cyan_title.g, cyan_title.b, 66.f / 255), false, true);
    caption(stage, "ScenarioTitle", U"剧情", 117.158, 111.206, 260, 26, cyan_title, false, true);
    caption(stage, "ViewTimeline", U"重播剧情", 679.876, -44.1, 250, 23, cyan, true, true);
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
    menu_fetch_user();
}

void ClientScreen::build_konzetsu() {
    const float scale = .67f * 1.5f;
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
        add_hub(prefix + String("Panel"), "node/node-panel.png",
            positions[i].x * 1.5f, positions[i].y * 1.5f);
        const String path = "node/node-number" + String::num_int64(i + 1) + ".png";
        add_hub(prefix + String("Number"), path,
            positions[i].x * 1.5f + 3, positions[i].y * 1.5f + 9);
    }
    Ref<Texture2D> back_texture = load("konzetsu/hub/back.png");
    const float back_width = back_texture.is_valid() ? back_texture->get_width() * scale : 0;
    const float back_height = back_texture.is_valid() ? back_texture->get_height() * scale : 0;
    auto *back = art_button(hub, "HubBack", "konzetsu/hub/back.png", "konzetsu/hub/back-pressed.png",
        0, H / 2 - back_height / 2, back_width, back_height);
    if (back) {
        back->set_position(Vector2((W - back_width) / 2, 0));
        back->set_pivot_offset(Vector2(back_width / 2, back_height));
    }
    back->set_visible(false);
    back->connect("pressed", Callable(this, "return_to_menu"));
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
    stage->hide();
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
    Ref<Texture2D> normal_border = load("characterselect/partner-border.png");
    Ref<Texture2D> selected_border = load("characterselect/partner-border-sel.png");
    Ref<Texture2D> lock_texture = load("characterselect/cell-lock2.png");
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
            frame->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            frame->set_stretch_mode(TextureRect::STRETCH_SCALE);
            frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            const float width = (id == selected_character ? 234.f : 194.f) * .9f * 1.005f;
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
    name->add_theme_constant_override("outline_size", 5);
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
    credit->add_theme_constant_override("outline_size", 3);
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
    skill->add_theme_constant_override("outline_size", 3);
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
        label->add_theme_constant_override("outline_size", 4);
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
    build_partner_chrome();
    apply_partner_aside(selected_character);
    layout();
}

void ClientScreen::apply_partner_aside(int id) {
    if (!character_dialog) return;
    const bool open = content::unlocked(id);
    dialog_locked_preview = !open;
    for (const char *name : {"AsideLv", "AsideLevel", "AsideLevelBar", "AsideLevelBarFill"}) {
        if (CanvasItem *node = Object::cast_to<CanvasItem>(character_dialog->find_child(name, true, false)))
            node->set_visible(open);
    }
    if (Label *level = Object::cast_to<Label>(character_dialog->find_child("AsideLevel", true, false))) {
        const int value = content::level_of(id);
        level->set_text(value < 0 ? String() : String::num_int64(value));
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
    if (CanvasItem *tag = Object::cast_to<CanvasItem>(character_dialog->find_child("AsideOfflineTag", true, false)))
        tag->set_visible(!open);
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
    Ref<Texture2D> portrait = content::image(String("char/1080/") + String::num_int64(id) + ".png");
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
        border->set_texture(load(selected ? "characterselect/partner-border-sel.png" : "characterselect/partner-border.png"));
        border->set_material(selected ? selected_material : Ref<ShaderMaterial>());
        const float width = (selected ? 234.f : 194.f) * .9f * 1.005f;
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
    // Keep the same ultrawide compensation used by layout(). The default
    // portrait is placed there during layout, while a newly selected
    // portrait reaches its target through this animation path.
    const Vector2 rest = at(-330, -225, size.x, size.y) - Vector2(design_extra * .5f, 0);
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
    if (!slide) {
        node->set_position(rest);
        node->set_modulate(Color(1, 1, 1, 1));
        return;
    }
    node->set_position(rest + Vector2(225, 0));
    node->set_modulate(Color(1, 1, 1, 0));
    Ref<Tween> tween = node->create_tween();
    tween->set_parallel(true);
    tween->tween_property(node, "position", rest, .45)->set_trans(Tween::TRANS_CUBIC)->set_ease(Tween::EASE_OUT);
    tween->tween_property(node, "modulate:a", 1.f, .45)->set_trans(Tween::TRANS_CUBIC)->set_ease(Tween::EASE_OUT);
}

void ClientScreen::menu_hover(int which, int hovered) {
    Label *label = Object::cast_to<Label>(stage->find_child(which == 1 ? "SelectSong" : "ViewTimeline", true, false));
    if (!label) return;
    label->add_theme_color_override("font_color", hovered
        ? Color(31.f / 255, 34.f / 255, 48.f / 255)
        : Color(218.f / 255, 254.f / 255, 254.f / 255));
}

void ClientScreen::menu_fetch_user() {
    if (!menu_http || menu_http_kind != 0) return;
    const String token = menu_token();
    const String base = menu_base_url();
    if (token.is_empty() || base.is_empty()) return;
    PackedStringArray headers;
    headers.push_back("Authorization: Bearer " + token.trim_prefix("Bearer ").trim_prefix("bearer "));
    headers.push_back("AppVersion: 1.0.0");
    headers.push_back("Accept: application/json");
    menu_http_kind = 1;
    if (menu_http->request(base.trim_suffix("/") + "/user/online", headers) != OK) {
        menu_http_kind = 0;
        show_partner_message(U"无法连接至服务器");
    }
}

void ClientScreen::menu_commit_character(int id) {
    if (!menu_http || menu_http_kind != 0) return;
    const String token = menu_token();
    const String base = menu_base_url();
    if (token.is_empty() || base.is_empty()) return;
    PackedStringArray headers;
    headers.push_back("Authorization: Bearer " + token.trim_prefix("Bearer ").trim_prefix("bearer "));
    headers.push_back("AppVersion: 1.0.0");
    headers.push_back("Accept: application/json");
    headers.push_back("Content-Type: application/x-www-form-urlencoded");
    menu_commit_id = id;
    menu_http_kind = 2;
    const String body = "character=" + String::num_int64(id) + "&skill_sealed=false";
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
    if (kind == 4) {
        if (result != HTTPRequest::RESULT_SUCCESS) fail_partner_purchase_check(U"无法连接至服务器");
        else if (response_code != 200) fail_partner_purchase_check(U"发生了未知错误");
        else partner_purchase_score_result(body, true);
        return;
    }
    if ((kind == 2 || kind == 3) && partner_purchase_busy && partner_purchase_busy->is_visible())
        end_partner_purchase_activity(false);
    if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) {
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
                    show_partner_message(U"发生了未知错误");
                    return;
                }
            }
        }
    }
    if (kind == 1) {
        const String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).strip_edges();
        if (!text.begins_with("{") && !text.begins_with("[")) return;
        int id = -1;
        const Variant parsed = JSON::parse_string(text);
        content::remember_online(parsed);
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
        if (!find_int(parsed, "character", id) || id < 0 || id == selected_character) return;
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
