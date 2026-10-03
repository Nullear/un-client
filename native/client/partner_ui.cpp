#include "client.hpp"
#include "content.hpp"
#include "arcapi.hpp"
#include "ui_font.hpp"

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/callback_tweener.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/property_tweener.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/texture_button.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {
static constexpr float W = 1920, H = 1080;
static const Color INK(12.f / 255, 12.f / 255, 16.f / 255);
static const Color PALE(218.f / 255, 254.f / 255, 254.f / 255);
static const Color MODAL_INK(187.f / 255, 220.f / 255, 221.f / 255);
static const Color MODAL_DIM(140.f / 255, 168.f / 255, 170.f / 255);
static const Color NEGATIVE(232.f / 255, 96.f / 255, 104.f / 255);
static const Color PRESSED_INK(31.f / 255, 34.f / 255, 48.f / 255);

static Vector2 at(float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    return Vector2(W / 2 + x - w * ax, H / 2 - y - h * (1 - ay));
}

static Vector2 csb_at(float x, float y, float w, float h, float ax = .5f, float ay = .5f) {
    return at((x - 640.f) * 1.5f, (y - 360.f) * 1.5f, w, h, ax, ay);
}

static Ref<Texture2D> load_tex(const String &path) {
    return ResourceLoader::get_singleton()->load(String("res://assets/resources/img/") + path);
}

static Ref<Font> font_file(const char *file) {
    return unfalsus_ui::prepare_font(file);
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

static int required_song_difficulty(const String &id) {
    if (id.is_empty()) return 0;
    const char32_t suffix = id[id.length() - 1];
    return suffix >= '2' && suffix <= '4' ? int(suffix - '0') : 0;
}

static String required_song_id(const String &id) {
    return required_song_difficulty(id) ? id.substr(0, id.length() - 1) : id;
}

static String purchase_requirement_text(const String &id) {
    String text = String(U"「") + content::song_title(required_song_id(id)) + String(U"」");
    const int difficulty = required_song_difficulty(id);
    if (difficulty == 2) text += "[ULT]";
    else if (difficulty == 3 || difficulty == 4) text += "[FBD]";
    return text;
}

static int level_need(int level) {
    if (level >= 1 && level <= 4) return 50;
    if (level >= 5 && level <= 9) return 50 * (level - 3);
    if (level >= 10 && level <= 17) return 100 * (level - 6);
    if (level == 18) return 1300;
    if (level >= 19 && level <= 29) return 1500;
    return 0;
}

static float frag_bar_x(float value) {
    const float t = std::clamp(value / 400.f, 0.f, 1.f);
    const float remain = 1.f - t;
    return -275.f * remain * remain * remain;
}

static float frag_line_w(float max_frag, bool uncappable) {
    const float t = std::clamp((max_frag + (uncappable ? 10.f : 0.f)) / 400.f, 0.f, 1.f);
    const float remain = 1.f - t;
    const float eased = 1.f - remain * remain * remain;
    return 73.f + (348.f - 73.f) * eased;
}

struct PreviewSpec {
    const char *name;
    const char *asset;
    const char *selected;
    float x, w, h, sw, sh;
    int level;
    bool uncap;
};

static const PreviewSpec PREVIEWS[] = {
    {"DetailsPreviewA1", "preview-btn-left", "preview-btn-left-active", 0, 245, 120, 255, 120, 1, true},
    {"DetailsPreviewA20", "preview-btn-mid", "preview-btn-mid-active", 150, 255, 120, 255, 120, 20, true},
    {"DetailsPreviewA30", "preview-btn-right", "preview-btn-right-selected", 310, 244, 120, 202, 75, 30, true},
    {"DetailsPreviewB1", "preview-btn-l-left", "preview-btn-l-left-selected", 0, 325, 120, 283, 75, 1, false},
    {"DetailsPreviewB20", "preview-btn-right-l", "preview-btn-right-l-selected", 230, 322, 120, 280, 75, 20, false},
};

static TextureRect *sprite(Control *parent, const String &name, const String &file, Vector2 pos, Vector2 size) {
    Ref<Texture2D> tex = load_tex(file);
    if (!tex.is_valid()) return nullptr;
    auto *node = memnew(TextureRect);
    node->set_name(name);
    parent->add_child(node);
    node->set_texture(tex);
    node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    node->set_stretch_mode(TextureRect::STRETCH_SCALE);
    node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    node->set_position(pos);
    node->set_size(size);
    return node;
}

static Label *inked(Control *parent, const String &name, const String &text, int size, Ref<Font> font,
        const Color &color, const Color &outline, int outline_px) {
    auto *label = memnew(Label);
    label->set_name(name);
    parent->add_child(label);
    label->set_text(text);
    label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    label->set_clip_text(false);
    label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
    label->add_theme_font_size_override("font_size", size);
    label->add_theme_color_override("font_color", color);
    if (outline_px > 0) {
        label->add_theme_color_override("font_outline_color", outline);
        label->add_theme_constant_override("outline_size", outline_px + 1);
    }
    if (font.is_valid()) label->add_theme_font_override("font", font);
    return label;
}

static void place_panel(Control *node, float panel_w, float panel_h, float x, float y, float w, float h) {
    node->set_size(Vector2(w, h));
    node->set_position(Vector2(panel_w * .5f + x - w * .5f, panel_h * .5f - y - h * .5f));
}

static TextureButton *corner_button(Control *parent, const String &name, const String &normal_path,
        const String &pressed_path, Vector2 pos, float ax, float ay) {
    Ref<Texture2D> normal = load_tex(normal_path);
    Ref<Texture2D> pressed = load_tex(pressed_path);
    const float width = (normal.is_valid() ? normal->get_width() : 132.f) * .67f * 1.5f;
    const float height = (normal.is_valid() ? normal->get_height() : 71.f) * .67f * 1.5f;
    auto *button = memnew(TextureButton);
    button->set_name(name);
    parent->add_child(button);
    button->set_texture_normal(normal);
    button->set_texture_pressed(pressed);
    button->set_ignore_texture_size(true);
    button->set_stretch_mode(TextureButton::STRETCH_SCALE);
    button->set_focus_mode(Control::FOCUS_NONE);
    button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
    button->set_size(Vector2(width, height));
    button->set_position(pos - Vector2(width * ax, height * (1.f - ay)));
    return button;
}
}

void ClientScreen::build_partner_chrome() {
    if (!character_dialog || !dialog_chrome || partner_details) return;
    const Ref<Font> regular = font_file("VivoSans-Regular-Full.ttf");
    const Ref<Font> jamjuree = font_file("Bai-Jamjuree.ttf");
    for (const char *name : {"AsideLv", "AsideLevel"}) {
        if (Label *label = Object::cast_to<Label>(dialog_chrome->get_node_or_null(name)))
            if (jamjuree.is_valid()) label->add_theme_font_override("font", jamjuree);
    }
    const float tag_w = 182.f * .67f * 1.5f, tag_h = 37.f * .67f * 1.5f;
    auto *tag = memnew(TextureButton);
    tag->set_name("AsideOfflineTag");
    dialog_chrome->add_child(tag);
    tag->set_texture_normal(load_tex("characterselect/preview-tag.png"));
    tag->set_ignore_texture_size(true);
    tag->set_stretch_mode(TextureButton::STRETCH_SCALE);
    tag->set_focus_mode(Control::FOCUS_NONE);
    tag->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
    tag->set_size(Vector2(tag_w, tag_h));
    tag->set_position(csb_at(963.333f, 149.333f, tag_w, tag_h, .5f, 0));
    tag->set_visible(false);
    Label *buy = inked(tag, "AsideOffline", U"购买", 24, regular, Color(1, 1, 1), INK, 0);
    unfalsus_ui::place_ccc(buy, Vector2(tag_w * .5f, tag_h - 18.f), .5f, .5f, tag_w, 24, 0);
    tag->connect("pressed", Callable(this, "open_partner_purchase"));

    TextureButton *details = corner_button(dialog_chrome, "DetailsButton",
        "story/actselect/corner-btn-right.png", "story/actselect/corner-btn-right-pressed.png",
        csb_at(1280, 0, 0, 0), 1, 0);
    Label *detail_caption = inked(details, "Caption", U"详细", 30, regular, Color(1, 1, 1), INK, 0);
    const Vector2 detail_size = details->get_size();
    unfalsus_ui::place_ccc(detail_caption, Vector2(detail_size.x - 143.f, detail_size.y - 67.f), .5f, .5f, 200, 30, 0);
    details->connect("pressed", Callable(this, "open_partner_details"));

    auto *multiplier = inked(dialog_back, "CharacterFragMultiplier", "", 33, regular, Color(1, 1, 1), Color(0, 0, 0), 2);
    unfalsus_ui::place_ccc(multiplier, at(326.4f, -457, 0, 0), .5f, .5f, 540, 33, 2);

    partner_details = memnew(Control);
    partner_details->set_name("PartnerDetails");
    character_dialog->add_child(partner_details);
    partner_details->set_size(Vector2(W, H));
    partner_details->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    partner_details->set_visible(false);
    partner_details->set_modulate(Color(1, 1, 1, 0));
    if (Node *close = character_dialog->get_node_or_null("OverlayClose"))
        character_dialog->move_child(close, character_dialog->get_child_count() - 1);

    TextureButton *back = corner_button(partner_details, "DetailsBackButton",
        "story/actselect/corner-btn.png", "story/actselect/corner-btn-pressed.png",
        csb_at(0, 0, 0, 0), 0, 0);
    Label *back_caption = inked(back, "Caption", U"返回", 30, regular, Color(1, 1, 1), INK, 0);
    unfalsus_ui::place_ccc(back_caption, Vector2(143.f, back->get_size().y - 67.f), .5f, .5f, 200, 30, 0);
    back->connect("pressed", Callable(this, "partner_details_back"));

    const float panel_w = 652.f * .67f * 1.5f, panel_h = 269.f * .67f * 1.5f;
    if (TextureRect *panel = sprite(partner_details, "DetailsStatsPanel", "characterselect/stats-backing.png",
            csb_at(0, 583, 0, 0), Vector2(panel_w, panel_h)))
        panel->set_scale(Vector2(1, 1.f / 3.f));
    const Vector2 backing(388.f * .67f * 1.5f, 56.f * .67f * 1.5f);
    auto *frag_backing = sprite(partner_details, "DetailsFragBarBacking", "characterselect/stats-bar-backing.png",
        csb_at(150, 553, backing.x, backing.y, 0, .5f), backing);
    const Vector2 icon(75.f * .8f * 1.5f, 52.f * .8f * 1.5f);
    sprite(partner_details, "DetailsFragIcon", "characterselect/frag-icon-text.png",
        csb_at(95, 550, icon.x, icon.y, 0, .5f), icon);

    const float line_h = 2.f * .754f * 1.5f;
    auto *line = sprite(partner_details, "DetailsFragLine", "characterselect/stats-bar-frag-line.png",
        csb_at(164.07f, 553, 348, line_h, 0, .5f), Vector2(348, line_h));
    if (line) line->set_stretch_mode(TextureRect::STRETCH_SCALE);

    const Vector2 bar_size(367.f * .67f * 1.5f, 39.f * .67f * 1.5f);
    auto *bar_clip = memnew(Control);
    bar_clip->set_name("DetailsFragBarClip");
    partner_details->add_child(bar_clip);
    bar_clip->set_clip_contents(true);
    bar_clip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    bar_clip->set_size(bar_size);
    bar_clip->set_position(csb_at(150, 553, bar_size.x, bar_size.y, 0, .5f));
    if (TextureRect *bar = sprite(bar_clip, "DetailsFragBar", "characterselect/stats-bar-frag.png", Vector2(0, 0), bar_size)) {
        bar->set_stretch_mode(TextureRect::STRETCH_SCALE);
        Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://native/client/frag_bar_mask.gdshader");
        if (shader.is_valid()) {
            Ref<ShaderMaterial> mask;
            mask.instantiate();
            mask->set_shader(shader);
            mask->set_shader_parameter("mask_texture", bar->get_texture());
            mask->set_shader_parameter("bar_width", bar_size.x);
            bar->set_material(mask);
        }
    }

    const Vector2 dia_max(21.f * .67f * 1.5f, 20.f * .67f * 1.5f);
    const Vector2 dia_awaken(20.f * .67f * 1.5f, 20.f * .67f * 1.5f);
    auto *dia_max_node = sprite(partner_details, "DetailsFragDiaMax", "characterselect/frag-dia-max.png", Vector2(0, 0), dia_max);
    auto *dia_awaken_node = sprite(partner_details, "DetailsFragDiaAwaken", "characterselect/frag-dia-awaken.png", Vector2(0, 0), dia_awaken);
    if (line && bar_clip && dia_max_node && dia_awaken_node) {
        partner_details->move_child(bar_clip, line->get_index() + 1);
        partner_details->move_child(dia_max_node, bar_clip->get_index() + 1);
        partner_details->move_child(dia_awaken_node, dia_max_node->get_index() + 1);
    }
    Label *frag_value = inked(partner_details, "DetailsFragValue", "", 27, jamjuree, Color(1, 1, 1),
        Color(0x1f / 255.f, 0x77 / 255.f, 0x7e / 255.f), 3);
    frag_value->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
    frag_value->set_size(Vector2(120, 32));

    const Vector2 level_bar(496.f * .57f * 1.5f, 11.f * .57f * 1.5f);
    auto *level_backing = sprite(partner_details, "DetailsLevelBar", "characterselect/lvl-bar.png",
        csb_at(90, 623, level_bar.x, level_bar.y, 0, 0), level_bar);
    const float full = 464.f * .57f * 1.5f, fill_h = 9.f * .57f * 1.5f;
    auto *fill = memnew(Control);
    fill->set_name("DetailsLevelBarFill");
    partner_details->add_child(fill);
    fill->set_clip_contents(true);
    fill->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    fill->set_meta("full_width", full);
    fill->set_size(Vector2(full, fill_h));
    fill->set_position(csb_at(99, 623.5f, full, fill_h, 0, 0));
    sprite(fill, "Fill", "characterselect/lvl-bar-fill.png", Vector2(0, 0), Vector2(full, fill_h));
    Label *lv = inked(partner_details, "DetailsLv", "lv.", 24, jamjuree, Color(1, 1, 1), INK, 3);
    unfalsus_ui::place_ccc(lv, csb_at(95, 628, 0, 0), 0.f, 0.f, 120, 24, 3);
    const float prefix = jamjuree.is_valid() ? jamjuree->get_string_size("lv.", HORIZONTAL_ALIGNMENT_LEFT, -1, 24).x : 36.f;
    Label *level = inked(partner_details, "DetailsLevel", "", 45, jamjuree, Color(1, 1, 1), INK, 3);
    unfalsus_ui::place_ccc(level, csb_at(95.f + prefix / 1.5f + 4.f, 624.666f, 0, 0), 0.f, 0.f, 160, 45, 3);
    Label *exp = inked(partner_details, "DetailsLevelExp", "", 27, jamjuree, Color(0xa8 / 255.f, 0xa8 / 255.f, 0xa8 / 255.f), INK, 2);
    unfalsus_ui::place_ccc(exp, csb_at(369, 628, 0, 0), 1.f, 0.f, 220, 27, 2);
    Label *preview_tag = inked(partner_details, "DetailsPreviewTag", U"预览", 21, jamjuree, Color(1, 1, 1), INK, 3);
    unfalsus_ui::place_ccc(preview_tag, csb_at(231.333f, 628, 0, 0), .5f, 0.f, 90, 21, 3);
    preview_tag->set_visible(false);

    const float preview_left = 46.f, preview_y = 602.666f;
    for (const PreviewSpec &spec : PREVIEWS) {
        const float bw = spec.w * .67f * 1.5f, bh = spec.h * .67f * 1.5f;
        const float sw = spec.sw * .67f * 1.5f, sh = spec.sh * .67f * 1.5f;
        auto *button = memnew(TextureButton);
        button->set_name(spec.name);
        partner_details->add_child(button);
        button->set_texture_normal(load_tex(String("characterselect/") + spec.asset + ".png"));
        button->set_ignore_texture_size(true);
        button->set_stretch_mode(TextureButton::STRETCH_SCALE);
        button->set_focus_mode(Control::FOCUS_NONE);
        button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
        button->set_size(Vector2(bw, bh));
        button->set_position(csb_at(preview_left + spec.x * .67f, preview_y, bw, bh, 0, .5f));
        button->set_meta("preview_level", spec.level);
        button->set_meta("preview_uncap", spec.uncap);
        button->connect("pressed", Callable(this, "preview_partner_level").bind(spec.level));
        if (TextureRect *selected = sprite(button, "Selected", String("characterselect/") + spec.selected + ".png",
                Vector2((bw - sw) * .5f, (bh - sh) * .5f), Vector2(sw, sh)))
            selected->set_visible(false);
        if (spec.level >= 30) {
            const Vector2 awaken(71.f * .67f * 1.5f, 46.f * .67f * 1.5f);
            sprite(button, "Awaken", "characterselect/awaken-icon.png", (Vector2(bw, bh) - awaken) * .5f, awaken);
        } else {
            Label *caption = inked(button, "Caption", spec.level >= 20 ? U"最大值" : U"初始", 21, jamjuree, Color(1, 1, 1), INK, 2);
            unfalsus_ui::place_ccc(caption, Vector2(bw * .5f, bh * .5f), .5f, .5f, bw, 21, 2);
        }
    }
    auto bar_hit = [&](const char *name, TextureRect *backing, float pad_y, bool bottom_anchor) {
        if (!backing) return;
        auto *button = memnew(Button);
        button->set_name(name);
        partner_details->add_child(button);
        button->set_flat(true);
        button->set_focus_mode(Control::FOCUS_NONE);
        button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
        button->set_position(backing->get_position() - Vector2(0, bottom_anchor ? pad_y : pad_y * .5f));
        button->set_size(backing->get_size() + Vector2(0, pad_y));
        button->connect("pressed", Callable(this, "cycle_partner_preview"));
    };
    bar_hit("DetailsLevelBarHit", level_backing, 54.f, true);
    bar_hit("DetailsFragBarHit", frag_backing, 18.f, false);
}

void ClientScreen::open_partner_details() {
    if (!partner_details || partner_details_open || partner_detail_busy || !dialog_layout || !character_dialog) return;
    play_sfx("sfx/06_LesserButtonClickV2.ogg");
    dialog_portrait_slide = false;
    partner_detail_busy = true;
    if (LineEdit *search = Object::cast_to<LineEdit>(dialog_chrome->get_node_or_null("SearchField"))) {
        search->set_text("");
        search->release_focus();
    }
    Ref<Tween> tween = character_dialog->create_tween();
    tween->set_parallel(true);
    tween->tween_property(dialog_layout, "modulate:a", 0.f, .22)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_IN);
    if (dialog_portrait) {
        const Vector2 size = dialog_portrait->get_size();
        tween->tween_property(dialog_portrait, "position", at(350.f - 225.f, -225.f, size.x, size.y), .22)
            ->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_IN);
        tween->tween_property(dialog_portrait, "modulate:a", 0.f, .22)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_IN);
    }
    tween->chain()->tween_callback(Callable(this, "present_partner_details"));
}

void ClientScreen::partner_details_back() {
    play_sfx("sfx/06_LesserButtonClickV2.ogg");
    close_partner_details();
}

void ClientScreen::close_partner_details() {
    if (!partner_details_open || partner_detail_busy || !partner_details) return;
    partner_detail_busy = true;
    Ref<Tween> tween = partner_details->create_tween();
    tween->set_parallel(true);
    tween->tween_property(partner_details, "modulate:a", 0.f, .22)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_IN);
    if (dialog_portrait && dialog_portrait->get_parent() == partner_details) {
        const Vector2 size = dialog_portrait->get_size();
        tween->tween_property(dialog_portrait, "position", at(-225, -225, size.x, size.y), .22)
            ->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_IN);
        tween->tween_property(dialog_portrait, "modulate:a", 0.f, .22)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_IN);
    }
    tween->chain()->tween_callback(Callable(this, "restore_partner_aside"));
}

void ClientScreen::present_partner_details() {
    if (!character_dialog || !partner_details || !dialog_layout) {
        partner_detail_busy = false;
        return;
    }
    dialog_layout->set_visible(false);
    partner_details->set_visible(true);
    partner_details->set_modulate(Color(1, 1, 1, 0));
    partner_preview_level = content::unlocked(dialog_preview_character) ? -1 : 1;
    refresh_partner_details();
    const float alpha = content::unlocked(dialog_preview_character) ? 1.f : 153.f / 255.f;
    if (dialog_portrait) {
        if (dialog_portrait->get_parent() != partner_details) dialog_portrait->reparent(partner_details, false);
        partner_details->move_child(dialog_portrait, 0);
        const Vector2 size = dialog_portrait->get_size();
        dialog_portrait->set_position(at(225, -225, size.x, size.y));
        dialog_portrait->set_modulate(Color(1, 1, 1, 0));
        Ref<Tween> tween = partner_details->create_tween();
        tween->set_parallel(true);
        tween->tween_property(partner_details, "modulate:a", 1.f, .22)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_OUT);
        tween->tween_property(dialog_portrait, "position", at(0, -225, size.x, size.y), .22)
            ->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_OUT);
        tween->tween_property(dialog_portrait, "modulate:a", alpha, .22)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_OUT);
        tween->chain()->tween_callback(Callable(this, "finish_partner_detail_transition").bind(true));
        return;
    }
    partner_details->set_modulate(Color(1, 1, 1, 1));
    finish_partner_detail_transition(true);
}

void ClientScreen::restore_partner_aside() {
    if (!character_dialog) {
        partner_detail_busy = false;
        partner_details_open = false;
        return;
    }
    if (dialog_portrait && dialog_back) {
        dialog_portrait->reparent(dialog_back, false);
        if (Node *skill = dialog_back->get_node_or_null("SkillContainer"))
            dialog_back->move_child(dialog_portrait, skill->get_index());
        const Vector2 size = dialog_portrait->get_size();
        dialog_portrait->set_position(at(575, -225, size.x, size.y));
        dialog_portrait->set_modulate(Color(1, 1, 1, 0));
        dialog_portrait_seconds = 0;
        dialog_portrait_slide = true;
    }
    partner_details_open = false;
    if (partner_details) partner_details->set_visible(false);
    if (!dialog_layout) {
        finish_partner_detail_transition(false);
        return;
    }
    dialog_layout->set_visible(true);
    dialog_layout->set_modulate(Color(1, 1, 1, 0));
    Ref<Tween> tween = dialog_layout->create_tween();
    tween->tween_property(dialog_layout, "modulate:a", 1.f, .22)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_OUT);
    tween->tween_callback(Callable(this, "finish_partner_detail_transition").bind(false));
}

void ClientScreen::finish_partner_detail_transition(bool open) {
    partner_detail_busy = false;
    partner_details_open = open;
}

void ClientScreen::refresh_partner_details() {
    if (!partner_details) return;
    const int id = dialog_preview_character;
    const bool uncap = content::uncappable(id);
    const int live_level = content::level_of(id);
    const int shown_level = partner_preview_level >= 0 ? partner_preview_level : live_level;
    float frag = 0;
    if (partner_preview_level >= 30) frag = float(std::max(0, content::max_frag(id)) + 10);
    else if (partner_preview_level >= 20) frag = float(std::max(0, content::max_frag(id)));
    else if (partner_preview_level >= 1) frag = float(std::max(0, content::base_frag(id)));
    else {
        const int value = content::frag_of(id);
        frag = value < 0 ? 0.f : float(value);
    }
    float fill_to = 0;
    if (partner_preview_level < 0) {
        const double gained = content::exp_into_level(id);
        const int cap = content::uncapped(id) ? 30 : 20;
        const int need = live_level > 0 ? level_need(live_level) : 0;
        if (live_level >= cap && live_level > 0) fill_to = 1;
        else if (need > 0 && gained >= 0) fill_to = std::clamp(float(gained / need), 0.f, 1.f);
    } else fill_to = partner_preview_level <= 1 ? 0.f : 1.f;
    const float level_to = shown_level < 0 ? 0.f : float(shown_level);
    const bool snap = !partner_details_open || partner_stat_id != id;
    partner_stat_id = id;
    if (snap) {
        partner_frag_from = partner_frag_to = frag;
        partner_level_from = partner_level_to = level_to;
        partner_fill_from = partner_fill_to = fill_to;
        partner_stat_seconds = 1;
    } else {
        partner_frag_from = partner_frag_shown;
        partner_level_from = partner_level_shown;
        partner_fill_from = partner_fill_shown;
        partner_frag_to = frag;
        partner_level_to = level_to;
        partner_fill_to = fill_to;
        partner_stat_seconds = 0;
    }
    String exp_text;
    if (partner_preview_level < 0 && live_level > 0) {
        const int cap = content::uncapped(id) ? 30 : 20;
        const int need = level_need(live_level);
        const double gained = content::exp_into_level(id);
        if (live_level < cap && need > 0 && gained >= 0)
            exp_text = String::num_int64(int(gained)) + "/" + String::num_int64(need);
    }
    auto fade_caption = [&](CanvasItem *node, bool show) {
        if (!node) return;
        if (snap) {
            node->set_visible(show);
            Color color = node->get_modulate();
            color.a = show ? 1.f : 0.f;
            node->set_modulate(color);
            return;
        }
        if (show) node->set_visible(true);
        Ref<Tween> tween = node->create_tween();
        tween->tween_property(node, "modulate:a", show ? 1.f : 0.f, .33)
            ->set_trans(Tween::TRANS_SINE)->set_ease(show ? Tween::EASE_OUT : Tween::EASE_IN);
        if (!show) tween->tween_callback(Callable(node, "hide"));
    };
    if (Label *label = Object::cast_to<Label>(partner_details->get_node_or_null("DetailsLevelExp"))) {
        label->set_text(exp_text);
        fade_caption(label, !exp_text.is_empty() && partner_preview_level < 0);
    }
    fade_caption(Object::cast_to<CanvasItem>(partner_details->get_node_or_null("DetailsPreviewTag")), partner_preview_level >= 0);
    if (TextureRect *line = Object::cast_to<TextureRect>(partner_details->get_node_or_null("DetailsFragLine"))) {
        const float width = frag_line_w(float(std::max(0, content::max_frag(id))), uncap);
        line->set_size(Vector2(width, line->get_size().y));
        auto place_dia = [&](const char *name, float mark, bool visible) {
            if (TextureRect *dia = Object::cast_to<TextureRect>(partner_details->get_node_or_null(name))) {
                dia->set_visible(visible);
                dia->set_position(Vector2(line->get_position().x + mark - dia->get_size().x,
                    line->get_position().y + (line->get_size().y - dia->get_size().y) * .5f));
            }
        };
        const int marker_level = partner_preview_level >= 0 ? partner_preview_level : live_level;
        const int max_frag = std::max(0, content::max_frag(id));
        place_dia("DetailsFragDiaMax", frag_line_w(float(max_frag), false), marker_level < 0 || marker_level < 20);
        place_dia("DetailsFragDiaAwaken", frag_line_w(float(max_frag), true), uncap && (marker_level < 0 || marker_level < 30));
    }
    tick_partner_stats(0);
    for (const PreviewSpec &spec : PREVIEWS) {
        if (Control *button = Object::cast_to<Control>(partner_details->get_node_or_null(spec.name))) {
            const bool visible = spec.uncap == uncap;
            button->set_visible(visible);
            if (CanvasItem *selected = Object::cast_to<CanvasItem>(button->get_node_or_null("Selected")))
                selected->set_visible(visible && spec.level == partner_preview_level);
        }
    }
}

void ClientScreen::tick_partner_stats(double delta) {
    if (!partner_details) return;
    if (partner_stat_seconds < .33) partner_stat_seconds += delta;
    const float t = partner_stat_seconds >= .33 ? 1.f : std::clamp(float(partner_stat_seconds / .33), 0.f, 1.f);
    const float eased = 1.f - std::pow(1.f - t, 3.f);
    partner_frag_shown = partner_frag_from + (partner_frag_to - partner_frag_from) * eased;
    partner_level_shown = partner_level_from + (partner_level_to - partner_level_from) * eased;
    partner_fill_shown = partner_fill_from + (partner_fill_to - partner_fill_from) * eased;
    if (Label *label = Object::cast_to<Label>(partner_details->get_node_or_null("DetailsLevel"))) {
        const bool blank = partner_preview_level < 0 && partner_level_to <= 0 && t >= 1.f;
        label->set_text(blank ? String() : String::num_int64(int(std::lround(partner_level_shown))));
    }
    if (Control *fill = Object::cast_to<Control>(partner_details->get_node_or_null("DetailsLevelBarFill"))) {
        const float full = float(fill->get_meta("full_width", fill->get_size().x));
        fill->set_size(Vector2(full * std::clamp(partner_fill_shown, 0.f, 1.f), fill->get_size().y));
    }
    if (Control *clip = Object::cast_to<Control>(partner_details->get_node_or_null("DetailsFragBarClip"))) {
        if (Control *bar = Object::cast_to<Control>(clip->get_node_or_null("DetailsFragBar"))) {
            const float offset = frag_bar_x(partner_frag_shown);
            bar->set_position(Vector2(offset, 0));
            Ref<ShaderMaterial> mask = bar->get_material();
            if (mask.is_valid()) mask->set_shader_parameter("scroll_offset", offset);
            if (Label *value = Object::cast_to<Label>(partner_details->get_node_or_null("DetailsFragValue"))) {
                value->set_text(String::num_int64(int(std::lround(partner_frag_shown))));
                const float right = clip->get_position().x + bar->get_position().x + bar->get_size().x;
                unfalsus_ui::place_ccc(value, Vector2(right - 32.f, at(0, 289.5f, 0, 0).y), 1.f, .5f, 160, 27, 3);
            }
        }
    }
}

void ClientScreen::preview_partner_level(int level) {
    if (!partner_details_open || partner_detail_busy) return;
    play_sfx("sfx/08_TimelineNodeClick.ogg");
    if (partner_preview_level == level) {
        if (content::unlocked(dialog_preview_character)) partner_preview_level = -1;
    } else partner_preview_level = level;
    refresh_partner_details();
}

void ClientScreen::cycle_partner_preview() {
    if (!partner_details_open || partner_detail_busy || !partner_details) return;
    play_sfx("sfx/08_TimelineNodeClick.ogg");
    const int id = dialog_preview_character;
    if (partner_preview_level < 0) partner_preview_level = 1;
    else if (partner_preview_level == 1) partner_preview_level = 20;
    else if (partner_preview_level == 20 && content::uncappable(id)) partner_preview_level = 30;
    else partner_preview_level = content::unlocked(id) ? -1 : 1;
    refresh_partner_details();
}

void ClientScreen::partner_caption_color(const NodePath &path, const Color &color) {
    if (Label *label = Object::cast_to<Label>(get_node_or_null(path)))
        label->add_theme_color_override("font_color", color);
}

void ClientScreen::close_partner_modal() {
    if (!partner_modal) return;
    play_sfx("sfx/06_LesserButtonClickV2.ogg");
    Control *modal = partner_modal;
    partner_modal = nullptr;
    if (menu_http_kind == 4) {
        menu_http_kind = 0;
        menu_http->cancel_request();
    }
    partner_purchase_checking = false;
    if (partner_purchase_busy && partner_purchase_busy->is_visible()) end_partner_purchase_activity(false);
    partner_purchase_confirm = nullptr;
    partner_purchase_confirm_caption = nullptr;
    partner_purchase_requirement_label = nullptr;
    Ref<Tween> &animation = modal == partner_message_cache ? partner_message_tween : partner_purchase_tween;
    if (animation.is_valid()) animation->kill();
    Ref<Tween> tween = modal->create_tween();
    animation = tween;
    tween->set_parallel(true);
    if (Control *panel = Object::cast_to<Control>(modal->get_node_or_null("Panel")))
        tween->tween_property(panel, "modulate:a", 0.f, .12);
    if (CanvasItem *dim = Object::cast_to<CanvasItem>(modal->get_node_or_null("Dimmer")))
        tween->tween_property(dim, "modulate:a", 0.f, .12);
    tween->chain()->tween_callback(Callable(modal, "hide"));
}

void ClientScreen::animate_partner_modal(Control *root) {
    Ref<Tween> &animation = root == partner_message_cache ? partner_message_tween : partner_purchase_tween;
    if (animation.is_valid()) animation->kill();
    root->show();
    character_dialog->move_child(root, character_dialog->get_child_count() - 1);
    auto *panel = root->get_node<Control>("Panel");
    auto *dim = root->get_node<Control>("Dimmer");
    panel->set_modulate(Color(1, 1, 1, 0));
    dim->set_modulate(Color(1, 1, 1, 0));
    if (menu_preparing) return;
    animation = root->create_tween();
    animation->set_parallel(true);
    animation->tween_property(dim, "modulate:a", 128.f / 255.f, .7)->set_trans(Tween::TRANS_SINE)->set_ease(Tween::EASE_OUT);
    animation->tween_property(panel, "modulate:a", 1.f, .12);
}

void ClientScreen::begin_partner_purchase_check() {
    if (menu_preparing || partner_purchase_requirements.empty()) {
        if (!menu_preparing) play_sfx("sfx/25_VNIMText.ogg");
        animate_partner_modal(partner_purchase_cache);
        return;
    }
    if (partner_purchase_tween.is_valid()) partner_purchase_tween->kill();
    partner_purchase_cache->hide();
    partner_purchase_checking = true;
    begin_center_activity();
    partner_purchase_check_next();
}

void ClientScreen::begin_center_activity() {
    partner_activity_finishing = false;
    partner_purchase_busy->get_node<Control>("Dimmer")->set_modulate(Color(1, 1, 1, 0));
    partner_purchase_busy->show();
    character_dialog->move_child(partner_purchase_busy, character_dialog->get_child_count() - 1);
    if (Control *activity = Object::cast_to<Control>(partner_purchase_busy->get_node_or_null("ActivityIcon"))) {
        partner_activity_animation.begin(activity);
    }
}

void ClientScreen::end_partner_purchase_activity(bool reveal, const String &error) {
    partner_activity_finishing = true;
    partner_activity_dim_from = partner_purchase_busy->get_node<Control>("Dimmer")->get_modulate().a;
    partner_activity_animation.end(partner_purchase_busy->get_node<Control>("ActivityIcon"));
    if (!error.is_empty()) {
        partner_modal = nullptr;
        show_partner_message(error);
        character_dialog->move_child(partner_message_cache, character_dialog->get_child_count() - 1);
    } else if (reveal && partner_modal == partner_purchase_cache) {
        character_dialog->move_child(partner_purchase_cache, character_dialog->get_child_count() - 1);
        play_sfx("sfx/25_VNIMText.ogg");
        animate_partner_modal(partner_purchase_cache);
    }
}

void ClientScreen::step_partner_purchase_activity(double delta) {
    if (!partner_purchase_busy || !partner_purchase_busy->is_visible()) return;
    const bool done = partner_activity_animation.step(partner_purchase_busy->get_node<Control>("ActivityIcon"), delta);
    const float u = std::clamp(float((partner_activity_finishing ? partner_activity_animation.fade_seconds
        : partner_activity_animation.seconds) / .7), 0.f, 1.f);
    const float k = std::sin(u * 1.570796327f);
    partner_purchase_busy->get_node<Control>("Dimmer")->set_modulate(Color(1, 1, 1,
        partner_activity_finishing ? partner_activity_dim_from * (1.f - k) : k));
    if (!partner_activity_finishing || !done) return;
    partner_activity_finishing = false;
    partner_purchase_busy->hide();
}

void ClientScreen::fail_partner_purchase_check(const String &message) {
    if (!partner_purchase_checking || partner_modal != partner_purchase_cache) return;
    end_partner_purchase_activity(false, message);
    partner_purchase_checking = false;
    partner_purchase_confirm = nullptr;
    partner_purchase_confirm_caption = nullptr;
    partner_purchase_requirement_label = nullptr;
    partner_purchase_cache->hide();
    if (menu_http_kind == 4) {
        menu_http_kind = 0;
        if (menu_http) menu_http->cancel_request();
    }
}

void ClientScreen::show_partner_message(const String &message) {
    if (!character_dialog || partner_modal) return;
    if (!menu_preparing) play_sfx("sfx/25_VNIMText.ogg");
    if (partner_message_cache) {
        partner_modal = partner_message_cache;
        partner_modal->get_node<Label>("Panel/Message")->set_text(message);
        animate_partner_modal(partner_modal);
        layout();
        return;
    }
    const Ref<Font> regular = font_file("VivoSans-Regular-Full.ttf");
    constexpr float panel_w = 780, panel_h = 390;
    auto *root = memnew(Control);
    root->set_name("InFalsusMessageBox");
    character_dialog->add_child(root);
    root->set_size(Vector2(W, H));
    root->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    partner_modal = root;
    partner_message_cache = root;
    auto *dim = memnew(ColorRect);
    dim->set_name("Dimmer");
    root->add_child(dim);
    dim->set_color(Color(0, 0, 0));
    dim->set_size(Vector2(W, H));
    dim->set_modulate(Color(1, 1, 1, 0));
    dim->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *panel = memnew(Control);
    panel->set_name("Panel");
    root->add_child(panel);
    panel->set_size(Vector2(panel_w, panel_h));
    panel->set_pivot_offset(Vector2(panel_w, panel_h) * .5f);
    panel->set_position(Vector2((W - panel_w) * .5f, (H - panel_h) * .5f));
    panel->set_scale(Vector2(1.25f, 1.25f));
    panel->set_modulate(Color(1, 1, 1, 0));
    panel->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    sprite(panel, "Backing", "infalsus/modal-backing.png", Vector2(0, 0), Vector2(panel_w, panel_h));
    if (TextureRect *brackets = sprite(panel, "Brackets", "infalsus/modal-brackets.png", Vector2(0, 0), Vector2(panel_w, panel_h))) {
        brackets->set_pivot_offset(Vector2(panel_w, panel_h) * .5f);
        brackets->set_scale(Vector2(1.05f, 1.1f));
    }
    Label *body = inked(panel, "Message", message, 28, regular, MODAL_INK, Color(0, 0, 0, 0), 0);
    body->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    place_panel(body, panel_w, panel_h, 0, 54, 640, 150);
    auto *confirm = memnew(TextureButton);
    confirm->set_name("Confirm");
    panel->add_child(confirm);
    confirm->set_texture_normal(load_tex("infalsus/button-basic.png"));
    confirm->set_texture_hover(load_tex("infalsus/button-basic-selected.png"));
    confirm->set_texture_pressed(load_tex("infalsus/button-basic-selected.png"));
    confirm->set_ignore_texture_size(true);
    confirm->set_stretch_mode(TextureButton::STRETCH_SCALE);
    confirm->set_focus_mode(Control::FOCUS_NONE);
    confirm->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
    place_panel(confirm, panel_w, panel_h, 0, -116, 256, 64);
    Label *caption = inked(confirm, "Caption", U"确定", 24, regular, PALE, Color(0, 0, 0, 0), 0);
    caption->set_size(Vector2(256, 64));

    confirm->connect("mouse_entered", Callable(this, "partner_caption_color").bind(caption->get_path(), PRESSED_INK));
    confirm->connect("mouse_exited", Callable(this, "partner_caption_color").bind(caption->get_path(), PALE));
    confirm->connect("pressed", Callable(this, "close_partner_modal"));
    animate_partner_modal(root);
    layout();
}

void ClientScreen::open_partner_purchase() {
    if (!character_dialog || partner_modal || partner_details_open
            || (partner_purchase_busy && partner_purchase_busy->is_visible())) return;
    const int id = dialog_preview_character;
    if (content::unlocked(id) && !menu_preparing) return;
    if (!menu_preparing) {
        play_sfx("sfx/06_LesserButtonClickV2.ogg");
    }
    partner_buy_id = id;
    const String name = String(content::character(catalog, id).get("name", ""));
    const int particles = content::ticket();
    const int price = content::base_frag(id);
    const bool priced = particles >= 0 && price >= 0;
    const int after = priced ? particles - price : 0;
    const Dictionary row = content::character(catalog, id);
    partner_purchase_requirements.clear();
    partner_purchase_required_count = 0;
    const Variant required = row.get("purchase_require", Variant());
    if (required.get_type() == Variant::STRING && !String(required).strip_edges().is_empty()) {
        partner_purchase_requirements.push_back({String(required).strip_edges(), false});
        ++partner_purchase_required_count;
    }
    const Variant any = row.get("purchase_requireany", Variant());
    if (any.get_type() == Variant::ARRAY) {
        const Array ids = any;
        for (int i = 0; i < ids.size(); ++i) {
            const String song = String(ids[i]).strip_edges();
            if (!song.is_empty()) partner_purchase_requirements.push_back({song, true});
        }
    }
    partner_purchase_candidate = 0;
    partner_purchase_difficulty_index = 0;
    partner_purchase_any_met = false;
    partner_purchase_checking = false;
    partner_purchase_confirm = nullptr;
    partner_purchase_confirm_caption = nullptr;
    partner_purchase_requirement_label = nullptr;
    const bool has_requirements = !partner_purchase_requirements.empty();
    const bool can_buy = priced && after >= 0 && !has_requirements;
    if (partner_purchase_cache) {
        partner_modal = partner_purchase_cache;
        auto *panel = partner_modal->get_node<Control>("Panel");
        panel->get_node<Label>("Description")->set_text(name.is_empty() ? String(U"解锁该搭档？") : String(U"解锁「") + name + String(U"」？"));
        panel->get_node<Label>("ParticleValue")->set_text(particles < 0 ? String(U"—") : String::num_int64(particles));
        auto *remain = panel->get_node<Label>("RemainValue");
        remain->set_text(priced ? String::num_int64(after) : String(U"—"));
        remain->add_theme_color_override("font_color", priced && after < 0 ? NEGATIVE : PALE);
        partner_purchase_confirm = panel->get_node<TextureButton>("Confirm");
        partner_purchase_confirm_caption = panel->get_node<Label>("Confirm/Caption");
        partner_purchase_requirement_label = panel->get_node<Label>("PurchaseRequirement");
        partner_purchase_requirement_label->set_text("");
        partner_purchase_requirement_label->hide();
        partner_purchase_set_eligible(can_buy);
        begin_partner_purchase_check();
        layout();
        return;
    }
    const Ref<Font> regular = font_file("VivoSans-Regular-Full.ttf");
    const Ref<Font> semibold = font_file("VivoSans-Semibold-Full.ttf");
    const Ref<Font> jamjuree = font_file("Bai-Jamjuree.ttf");
    constexpr float panel_w = 960, panel_h = 480;
    auto *root = memnew(Control);
    root->set_name("PartnerPurchaseDialog");
    partner_purchase_cache = root;
    character_dialog->add_child(root);
    root->set_size(Vector2(W, H));
    root->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    partner_modal = root;
    auto *dim = memnew(ColorRect);
    dim->set_name("Dimmer");
    root->add_child(dim);
    dim->set_color(Color(0, 0, 0));
    dim->set_size(Vector2(W, H));
    dim->set_modulate(Color(1, 1, 1, 0));
    dim->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    auto *panel = memnew(Control);
    panel->set_name("Panel");
    root->add_child(panel);
    panel->set_size(Vector2(panel_w, panel_h));
    panel->set_pivot_offset(Vector2(panel_w, panel_h) * .5f);
    panel->set_position(Vector2((W - panel_w) * .5f, (H - panel_h) * .5f));
    panel->set_scale(Vector2(1.25f, 1.25f));
    panel->set_modulate(Color(1, 1, 1, 0));
    panel->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    sprite(panel, "Backing", "infalsus/modal-backing.png", Vector2(0, 0), Vector2(panel_w, panel_h));
    if (TextureRect *brackets = sprite(panel, "Brackets", "infalsus/modal-brackets.png", Vector2(0, 0), Vector2(panel_w, panel_h))) {
        brackets->set_pivot_offset(Vector2(panel_w, panel_h) * .5f);
        brackets->set_scale(Vector2(1.05f, 1.1f));
    }
    sprite(panel, "TopBar", "infalsus/modal-topbar.png", Vector2((panel_w - 640) * .5f, panel_h * .5f - 148.f - 20.f), Vector2(640, 40));
    Label *title = inked(panel, "Title", U"购买", 28, semibold, PALE, Color(0, 0, 0, 0), 0);
    place_panel(title, panel_w, panel_h, 0, 188, 280, 36);
    Label *description = inked(panel, "Description", name.is_empty() ? String(U"解锁该搭档？") : String(U"解锁「") + name + String(U"」？"),
        26, regular, MODAL_INK, Color(0, 0, 0, 0), 0);
    place_panel(description, panel_w, panel_h, 0, 70, 680, 36);
    partner_purchase_requirement_label = inked(panel, "PurchaseRequirement", "", 20, regular, NEGATIVE, Color(0, 0, 0, 0), 0);
    place_panel(partner_purchase_requirement_label, panel_w, panel_h, 0, 126, 820, 32);
    partner_purchase_requirement_label->set_visible(false);
    Label *particle_label = inked(panel, "ParticleLabel", U"当前粒子", 18, regular, MODAL_DIM, Color(0, 0, 0, 0), 0);
    place_panel(particle_label, panel_w, panel_h, -190, 8, 200, 26);
    Label *remain_label = inked(panel, "RemainLabel", U"剩余", 18, regular, MODAL_DIM, Color(0, 0, 0, 0), 0);
    place_panel(remain_label, panel_w, panel_h, 190, 8, 200, 26);
    Label *particle_value = inked(panel, "ParticleValue", particles < 0 ? String(U"—") : String::num_int64(particles),
        28, jamjuree.is_valid() ? jamjuree : regular, PALE, Color(0, 0, 0, 0), 0);
    place_panel(particle_value, panel_w, panel_h, -190, -32, 200, 36);
    Label *remain_value = inked(panel, "RemainValue", priced ? String::num_int64(after) : String(U"—"),
        28, jamjuree.is_valid() ? jamjuree : regular, priced && after < 0 ? NEGATIVE : PALE, Color(0, 0, 0, 0), 0);
    place_panel(remain_value, panel_w, panel_h, 190, -32, 200, 36);
    Label *arrow = inked(panel, "Arrow", ">>", 36, semibold, MODAL_INK, Color(0, 0, 0, 0), 0);
    place_panel(arrow, panel_w, panel_h, 0, -28, 80, 40);
    arrow->set_scale(Vector2(.7f, 1));
    auto add_plate = [&](const char *name, const String &text, float x, bool enabled) {
        auto *button = memnew(TextureButton);
        button->set_name(name);
        panel->add_child(button);
        button->set_texture_normal(load_tex("infalsus/button-basic.png"));
        button->set_texture_pressed(load_tex("infalsus/button-basic-selected.png"));
        button->set_ignore_texture_size(true);
        button->set_stretch_mode(TextureButton::STRETCH_SCALE);
        button->set_focus_mode(Control::FOCUS_NONE);
        place_panel(button, panel_w, panel_h, x, -176, 220, 56);
        Label *caption = inked(button, "Caption", text, 24, regular, enabled ? PALE : Color(150.f / 255, 154.f / 255, 162.f / 255), Color(0, 0, 0, 0), 0);
        caption->set_size(Vector2(220, 56));
        if (String(name) == "Confirm") {
            partner_purchase_confirm = button;
            partner_purchase_confirm_caption = caption;
        }

        if (!enabled) {
            button->set_disabled(true);
            button->set_modulate(Color(120.f / 255, 124.f / 255, 132.f / 255));
            return;
        }
        button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
        button->connect("button_down", Callable(this, "partner_caption_color").bind(caption->get_path(), PRESSED_INK));
        button->connect("button_up", Callable(this, "partner_caption_color").bind(caption->get_path(), PALE));
    };
    add_plate("Confirm", U"确定", -170, can_buy);
    add_plate("Cancel", U"取消", 170, true);
    if (TextureButton *confirm = Object::cast_to<TextureButton>(panel->get_node_or_null("Confirm")))
        if (!confirm->is_disabled()) confirm->connect("pressed", Callable(this, "confirm_partner_purchase"));
    if (TextureButton *cancel = Object::cast_to<TextureButton>(panel->get_node_or_null("Cancel")))
        cancel->connect("pressed", Callable(this, "close_partner_modal"));
    begin_partner_purchase_check();
    layout();
}

void ClientScreen::partner_purchase_set_eligible(bool eligible, const String &message) {
    const bool reveal = partner_purchase_checking && partner_modal == partner_purchase_cache;
    partner_purchase_checking = false;
    if (partner_purchase_requirement_label) {
        partner_purchase_requirement_label->set_text(message);
        partner_purchase_requirement_label->set_visible(!message.is_empty());
    }
    if (partner_purchase_confirm) {
        const int balance = content::ticket();
        const int price = content::base_frag(partner_buy_id);
        const bool affordable = balance >= 0 && price >= 0 && balance >= price;
        const bool enabled = eligible && affordable;
        partner_purchase_confirm->set_disabled(!enabled);
        partner_purchase_confirm->set_modulate(enabled ? Color(1, 1, 1) : Color(120.f / 255, 124.f / 255, 132.f / 255));
        if (partner_purchase_confirm_caption)
            partner_purchase_confirm_caption->add_theme_color_override("font_color", enabled ? PALE : Color(150.f / 255, 154.f / 255, 162.f / 255));
        if (enabled) {
            partner_purchase_confirm->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
            const Callable confirm = Callable(this, "confirm_partner_purchase");
            if (!partner_purchase_confirm->is_connected("pressed", confirm))
                partner_purchase_confirm->connect("pressed", confirm);
            const Callable down = Callable(this, "partner_caption_color")
                .bind(partner_purchase_confirm_caption->get_path(), PRESSED_INK);
            if (!partner_purchase_confirm->is_connected("button_down", down))
                partner_purchase_confirm->connect("button_down", down);
            const Callable up = Callable(this, "partner_caption_color")
                .bind(partner_purchase_confirm_caption->get_path(), PALE);
            if (!partner_purchase_confirm->is_connected("button_up", up))
                partner_purchase_confirm->connect("button_up", up);
        }
    }
    if (reveal) {
        end_partner_purchase_activity(true);
    }
}

void ClientScreen::partner_purchase_check_next() {
    if (!partner_modal || partner_purchase_candidate >= int(partner_purchase_requirements.size())) {
        partner_purchase_set_eligible(partner_purchase_any_met || partner_purchase_requirements.empty());
        return;
    }
    if (!partner_purchase_checking) {
        partner_purchase_checking = true;
        if (partner_purchase_confirm) partner_purchase_confirm->set_disabled(true);
    }
    const String token = menu_token();
    const String base = menu_base_url();
    if (!menu_http || token.is_empty() || base.is_empty()) {
        fail_partner_purchase_check(U"无法连接至服务器");
        return;
    }
    const PurchaseSongRequirement &requirement = partner_purchase_requirements[partner_purchase_candidate];
    const int fixed_difficulty = required_song_difficulty(requirement.id);
    const bool fixed = fixed_difficulty != 0;
    const int difficulty = fixed ? fixed_difficulty : 2 + partner_purchase_difficulty_index;
    const String song_id = required_song_id(requirement.id);
    PackedStringArray headers;
    headers.push_back("Authorization: Bearer " + token.trim_prefix("Bearer ").trim_prefix("bearer "));
    headers.push_back("AppVersion: 1.0.0");
    headers.push_back("Accept: application/json");
    menu_http_kind = 4;
    const String url = base.trim_suffix("/") + "/score/song/online?song-id=" + song_id.uri_encode()
        + "&difficulty=" + String::num_int64(difficulty);
    if (menu_http->request(url, headers) == OK) return;
    menu_http_kind = 0;
    fail_partner_purchase_check(U"无法连接至服务器");
}

void ClientScreen::partner_purchase_finish_song(bool cleared) {
    const PurchaseSongRequirement requirement = partner_purchase_requirements[partner_purchase_candidate];
    if (!requirement.any_group && !cleared) {
        const String message = String(U"需通关") + purchase_requirement_text(requirement.id);
        partner_purchase_set_eligible(false, message);
        return;
    }
    if (requirement.any_group && cleared) {
        partner_purchase_any_met = true;
        partner_purchase_set_eligible(true);
        return;
    }
    ++partner_purchase_candidate;
    partner_purchase_difficulty_index = 0;
    if (partner_purchase_candidate == partner_purchase_required_count
            && partner_purchase_required_count < int(partner_purchase_requirements.size())) {
        partner_purchase_check_next();
        return;
    }
    if (partner_purchase_candidate >= int(partner_purchase_requirements.size())) {
        if (partner_purchase_required_count == int(partner_purchase_requirements.size())) {
            partner_purchase_set_eligible(true);
            return;
        }
        String message;
        if (!partner_purchase_any_met && partner_purchase_required_count < int(partner_purchase_requirements.size())) {
            for (int i = partner_purchase_required_count; i < int(partner_purchase_requirements.size()); ++i) {
                if (message.is_empty()) message = U"需通关";
                else message += U"或";
                message += purchase_requirement_text(partner_purchase_requirements[i].id);
            }
        }
        partner_purchase_set_eligible(message.is_empty(), message);
        return;
    }
    partner_purchase_check_next();
}

void ClientScreen::partner_purchase_score_result(const PackedByteArray &body, bool response_ok) {
    if (!partner_purchase_checking || partner_modal != partner_purchase_cache) return;
    if (!response_ok) {
        fail_partner_purchase_check(U"无法连接至服务器");
        return;
    }
    bool cleared = false;
    const String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size()).strip_edges();
    const Variant parsed = JSON::parse_string(text);
    if (parsed.get_type() != Variant::DICTIONARY) {
        fail_partner_purchase_check(U"发生了未知错误");
        return;
    }
    const Dictionary root = parsed;
    const Variant success = root.get("success", false);
    const bool ok = success == Variant(true) || int(success) == 1 || String(success).to_lower() == "true";
    if (!ok) {
        fail_partner_purchase_check(U"发生了未知错误");
        return;
    }
    const Variant values = root.get("value", Variant());
    if (values.get_type() != Variant::ARRAY) {
        fail_partner_purchase_check(U"发生了未知错误");
        return;
    }
    if (values.get_type() == Variant::ARRAY) {
        const Array entries = values;
        for (int i = 0; i < entries.size(); ++i) {
            if (entries[i].get_type() != Variant::DICTIONARY) continue;
            const Variant clear_type = Dictionary(entries[i]).get("clear_type", 0);
            if (int(clear_type) != 0 || String(clear_type).is_valid_int() && String(clear_type).to_int() != 0) {
                cleared = true;
                break;
            }
        }
    }
    if (cleared) {
        partner_purchase_finish_song(true);
        return;
    }
    const String &id = partner_purchase_requirements[partner_purchase_candidate].id;
    if (required_song_difficulty(id) || ++partner_purchase_difficulty_index >= 3)
        partner_purchase_finish_song(false);
    else
        partner_purchase_check_next();
}

void ClientScreen::confirm_partner_purchase() {
    if (!partner_purchase_confirm || partner_purchase_confirm->is_disabled() || partner_purchase_checking) return;
    const int id = partner_buy_id;
    close_partner_modal();
    const String token = menu_token();
    const String base = menu_base_url();
    if (!menu_http || menu_http_kind != 0 || token.is_empty() || base.is_empty() || id < 0) {
        show_partner_message(U"发生了未知错误");
        return;
    }
    PackedStringArray headers;
    headers.push_back("Authorization: Bearer " + token.trim_prefix("Bearer ").trim_prefix("bearer "));
    headers.push_back("AppVersion: 1.0.0");
    headers.push_back("Accept: application/json");
    headers.push_back("Content-Type: application/x-www-form-urlencoded");
    menu_commit_id = id;
    menu_http_kind = 3;
    const String body = "character=" + String::num_int64(id);
    if (menu_http->request(base.trim_suffix("/") + "/user/online/buychar", headers, HTTPClient::METHOD_POST, body) != OK) {
        menu_http_kind = 0;
        show_partner_message(U"无法连接至服务器");
    } else {
        begin_center_activity();
    }
}

void ClientScreen::present_purchased_partner(int id) {
    apply_partner_aside(id);
    play_sfx("sfx/75_unlock.ogg");
    for (Control *cell : dialog_cells) {
        if (int(cell->get_meta("character_id", -1)) != id) continue;
        Control *lock = Object::cast_to<Control>(cell->get_node_or_null("Lock"));
        if (!lock) continue;
        lock->set_visible(true);
        lock->set_modulate(Color(1, 1, 1, 1));
        lock->set_scale(Vector2(1, 1));
        lock->set_pivot_offset(lock->get_size() * .5f);
        Ref<Tween> tween = lock->create_tween();
        tween->set_parallel(true);
        tween->tween_property(lock, "scale", Vector2(2, 2), .33)->set_trans(Tween::TRANS_CUBIC)->set_ease(Tween::EASE_OUT);
        tween->tween_property(lock, "modulate:a", 0.f, .33)->set_trans(Tween::TRANS_CUBIC)->set_ease(Tween::EASE_OUT);
        tween->chain()->tween_callback(Callable(lock, "hide"));
    }
}
