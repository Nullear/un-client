#pragma once
#include "activity.hpp"

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/tween.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace godot {

class ScrollContainer;
class LineEdit;
class Label;
class Button;
class ProgressBar;
class BaseButton;
class TextureButton;

class ClientVideo : public Control {
    GDCLASS(ClientVideo, Control)
    Object *decoder = nullptr;
    Ref<RefCounted> decoder_ref;
    Object *ios = nullptr;
    bool ios_previous_transparent_background = false;
    void *hw = nullptr;
    TextureRect *picture = nullptr;
    Ref<ShaderMaterial> material;
    Ref<ImageTexture> planes[4];
    double frame_time = 0;
    double accumulator = 0;
    double hardware_duration = 0;
    double hardware_position = 0;
    String prepared_resource;
    int frame = 0;
    int frame_count = 0;
    bool has_alpha = false;
    bool loop = false;
    bool finished = false;
    bool initially_paused = false;

protected:
    static void _bind_methods() {}

public:
    bool prepare(const String &resource, bool repeat = false);
    bool play(const String &resource, bool repeat, bool paused = false);
    void start();
    void stop();
    bool start_hw(const char *path, bool repeat);
    bool hardware_finished() const;
    bool is_prepared() const { return hw != nullptr || prepared_resource.length() > 0; }
    double duration() const { return hardware_duration > 0 ? hardware_duration : frame_count * frame_time; }
    double position() const { return hardware_duration > 0 ? hardware_position : frame * frame_time; }
    void stop_hw();
    void publish_hw();
    void set_native_white(float alpha);
    void set_native_title(const String &resource);
    bool has_finished() const { return finished; }
    void _process(double delta) override;
    void _exit_tree() override { stop(); }
};

class ClientTransition : public CanvasLayer {
    GDCLASS(ClientTransition, CanvasLayer)
    static ClientTransition *instance;
    TextureRect *mask = nullptr;
    Ref<ShaderMaterial> material;
    AudioStreamPlayer *sfx = nullptr;
    String destination;
    Node *prepared_scene = nullptr;
    bool scene_preparation_attempted = false;
    double elapsed = 0;
    double hold = 0;
    int state = 0; // 0 idle, 1 cover, 2 load/hold, 3 reveal
    int transition_type = 0;

protected:
    static void _bind_methods();

public:
    static ClientTransition *get_or_create(SceneTree *tree);
    static bool covered() { return instance && instance->state == 2; }
    static bool running() { return instance && instance->state != 0; }
    void switch_to(const String &scene, int type = 0);
    void _ready() override;
    void _process(double delta) override;
    void _exit_tree() override;
};

class ClientScreen : public Control {
    GDCLASS(ClientScreen, Control)
    struct PurchaseSongRequirement {
        String id;
        bool any_group = false;
    };
    int scene_kind = 0;
    int native_mode = 0;
    Control *stage = nullptr;
    Control *hub_content = nullptr;
    ClientVideo *video = nullptr;
    ClientVideo *title_video = nullptr;
    TextureRect *title_art = nullptr;
    TextureRect *icon = nullptr;
    TextureRect *wreath = nullptr;
    ColorRect *flash = nullptr;
    AudioStreamPlayer *music = nullptr;
    AudioStreamPlayer *sfx = nullptr;
    std::map<std::string, Control *> art;
    Array catalog;
    Control *character_dialog = nullptr;
    bool dialog_active = false;
    bool menu_preparing = false;
    bool menu_assets_ready = false;
    bool scene_assets_ready = false;
    Control *partner_message_cache = nullptr;
    Control *partner_purchase_cache = nullptr;
    Control *partner_purchase_busy = nullptr;
    unfalsus_ui::CenteredActivityAnimation partner_activity_animation;
    unfalsus_ui::CenteredActivityAnimation startup_activity_animation;
    bool partner_activity_finishing = false;
    float partner_activity_dim_from = 0;
    Ref<Tween> partner_message_tween;
    Ref<Tween> partner_purchase_tween;
    std::vector<Ref<Resource>> menu_resources;
    Control *dialog_layout = nullptr;
    Control *dialog_back = nullptr;
    Control *dialog_chrome = nullptr;
    ColorRect *menu_dimmer = nullptr;
    Ref<Tween> dialog_view_tween;
    double dialog_dim_seconds = 0;
    float dialog_dim_start = 0;
    float dialog_dim_target = 0;
    Control *dialog_portrait = nullptr;
    ScrollContainer *dialog_list = nullptr;
    Control *dialog_content = nullptr;
    Control *dialog_visual_content = nullptr;
    Control *dialog_all = nullptr;
    ScrollContainer *dialog_all_list = nullptr;
    std::vector<Control *> dialog_cells;
    std::vector<Vector2> dialog_rest;
    double dialog_seconds = 0;
    double dialog_portrait_seconds = 0;
    bool dialog_closing = false;
    bool dialog_view_all = false;
    int dialog_preview_character = 0;
    int dialog_pending_character = -1;
    bool dialog_locked_preview = false;
    bool dialog_portrait_slide = true;
    Control *partner_details = nullptr;
    Control *partner_modal = nullptr;
    bool partner_details_open = false;
    bool partner_detail_busy = false;
    int partner_preview_level = -1;
    int partner_shown_id = -1;
    int partner_buy_id = -1;
    std::vector<PurchaseSongRequirement> partner_purchase_requirements;
    int partner_purchase_required_count = 0;
    int partner_purchase_candidate = 0;
    int partner_purchase_difficulty_index = 0;
    bool partner_purchase_any_met = false;
    bool partner_purchase_checking = false;
    TextureButton *partner_purchase_confirm = nullptr;
    Label *partner_purchase_confirm_caption = nullptr;
    Label *partner_purchase_requirement_label = nullptr;
    int partner_grant_id = -1;
    int partner_stat_id = -2;
    float partner_frag_shown = 0;
    float partner_level_shown = 0;
    float partner_fill_shown = 0;
    float partner_frag_from = 0;
    float partner_frag_to = 0;
    float partner_level_from = 0;
    float partner_level_to = 0;
    float partner_fill_from = 0;
    float partner_fill_to = 0;
    double partner_stat_seconds = 1;
    int selected_character = 0;
    double clock = 0;
    double title_time = 0;
    int startup_state = 0;
    bool content_ready = false;
    bool switching = false;
    bool show_loading = false;
    bool hub_music_started = false;
    HTTPRequest *menu_http = nullptr;
    int menu_http_kind = 0;
    int menu_commit_id = -1;

    HTTPRequest *startup_http = nullptr;
    Control *startup_overlay = nullptr;
    Control *startup_login_panel = nullptr;
    LineEdit *startup_user_field = nullptr;
    LineEdit *startup_pass_field = nullptr;
    Label *startup_error_label = nullptr;
    Label *startup_status_label = nullptr;
    BaseButton *startup_login_button = nullptr;
    BaseButton *startup_confirm_button = nullptr;
    BaseButton *startup_cancel_button = nullptr;
    Control *startup_progress = nullptr;
    Label *startup_count_label = nullptr;
    Label *startup_size_label = nullptr;
    Control *startup_download_fill = nullptr;
    Control *startup_download_diamond = nullptr;
    Control *startup_download_tail = nullptr;
    Control *startup_activity = nullptr;
    Control *startup_busy_blocker = nullptr;
    int startup_request_kind = 0;
    int startup_content_phase = 0; // 0 none; 1 close login; 2 accept dialog; 3 dismiss dialog
    int startup_manifest_index = 0;
    int startup_manifest_done = 0;
    int startup_manifest_total = 0;
    int64_t startup_manifest_bytes_done = 0;
    int64_t startup_manifest_bytes_total = 0;
    int startup_verify_index = 0;
    int startup_verify_round = 0;
    int startup_download_attempt = 0;
    HTTPRequest *startup_slot_http[16] = {};
    int startup_slot_item[16] = {};
    int startup_slot_attempt[16] = {};
    int64_t startup_slot_seen[16] = {};
    double startup_slot_stall[16] = {};
    String startup_slot_temp[16];
    String startup_slot_dest[16];
    int startup_download_cursor = 0;
    int startup_download_inflight = 0;
    int startup_download_limit = 8;
    bool startup_download_failed = false;
    bool startup_storage_failed = false;
    bool startup_activity_wait = false;
    double startup_activity_delay = 0;
    int startup_preload_index = 0;
    int startup_error_action = 0;
    int startup_error_code = 0;
    int startup_error_status = 0;
    bool startup_dialog_download = false;
    Array startup_verify_failed;
    Array startup_local_sweep;
    int startup_local_sweep_index = 0;
    bool startup_sweep_done = false;
    bool startup_warmup_done = false;
    bool startup_media_prepared = false;
    bool startup_content_started = false;
    bool startup_content_waiting = false;
    bool startup_busy = false;
    bool startup_overlay_closing = false;
    bool startup_loading_fading = false;
    double startup_loading_fade_seconds = 0;
    double startup_overlay_seconds = 0;
    double startup_busy_seconds = 0;
    float startup_dimmer_from = 0;
    float startup_dimmer_to = -1;
    double startup_dimmer_time = 0;
    float startup_dimmer_duration = 20.f / 60.f;
    String startup_base_url;
    String startup_auth_token;
    String startup_manifest_text;
    String startup_current_path;
    String startup_current_temp;
    Array startup_manifest;
    Array startup_missing;
    Array startup_committed;
    Dictionary startup_next_by_path;
    int startup_diff_phase = 0;
    int startup_diff_index = 0;
    float design_extra = 0;
    float design_extra_y = 0;
    Vector2i last_window_size;
    bool enforcing_window_aspect = false;
    bool partner_scroll_pointer_active = false;
    bool partner_scroll_dragging = false;
    bool partner_scroll_suppress_click = false;
    bool partner_scroll_touch = false;
    int partner_scroll_clear_suppress = 0;
    Vector2 partner_scroll_pointer_start;
    Vector2 partner_scroll_pointer_last;
    double partner_scroll_last_motion = 0;
    float partner_scroll_velocity = 0;
    float partner_scroll_start = 0;
    float partner_scroll_distance = 0;
    float partner_scroll_duration = 0;
    float partner_scroll_elapsed = 0;
    float partner_scroll_overscroll = 0;
    float partner_scroll_bounce_start = 0;
    float partner_scroll_bounce_elapsed = 0;
    std::vector<std::pair<float, float>> partner_scroll_samples;
    ScrollContainer *partner_scroll_inertia_list = nullptr;

    void layout();
    void constrain_window_aspect();
    void build_startup();
    void build_menu();
    void prepare_scene_resources();
    void prepare_menu_dialogs();
    void build_character_select();
    void animate_partner_modal(Control *root);
    void begin_partner_purchase_check();
    void begin_center_activity();
    void end_partner_purchase_activity(bool reveal, const String &error = String());
    void step_partner_purchase_activity(double delta);
    void fail_partner_purchase_check(const String &message);
    void build_konzetsu();
    void begin_intro();
    void begin_title();
    void play_music(const String &intro, const String &repeat);
    void stop_music();
    void on_music_finished();
    void play_sfx(const String &path);
    void on_menu_action(int action);
    void open_character_select();
    void close_character_select();
    void begin_close_character_select();
    void apply_hub_partner(int id, bool slide);
    void menu_fetch_user();
    void menu_commit_character(int id);
    void menu_http_completed(int result, int response_code, const PackedStringArray &headers, const PackedByteArray &body);
    void menu_hover(int which, int hovered);
    void select_character(int id);
    void show_partner_preview(int id);
    void apply_partner_aside(int id);
    void build_partner_chrome();
    void open_partner_details();
    void close_partner_details();
    void partner_details_back();
    void present_partner_details();
    void restore_partner_aside();
    void finish_partner_detail_transition(bool open);
    void refresh_partner_details();
    void preview_partner_level(int level);
    void cycle_partner_preview();
    void open_partner_purchase();
    void close_partner_modal();
    void confirm_partner_purchase();
    void partner_purchase_check_next();
    void partner_purchase_score_result(const PackedByteArray &body, bool response_ok);
    void partner_purchase_finish_song(bool cleared);
    void partner_purchase_set_eligible(bool eligible, const String &message = String());
    void show_partner_message(const String &message);
    void partner_caption_color(const NodePath &path, const Color &color);
    void present_purchased_partner(int id);
    void tick_partner_stats(double delta);
    void animate_character_select(double delta);
    void filter_characters(const String &query);
    void toggle_all_characters();
    void return_to_menu();
    void startup_step_warmup();
    void startup_begin_local_sweep();
    void startup_step_local_sweep();
    void startup_step_manifest_diff();
    void startup_start_content_check();
    void startup_request_completed(int result, int response_code, const PackedStringArray &headers, const PackedByteArray &body);
    void startup_show_login();
    void startup_submit_login();
    void startup_show_content_dialog(bool download, const String &body);
    void startup_show_error(int code, int status);
    void startup_fetch_manifest();
    void startup_confirm_content();
    void startup_cancel_content();
    void startup_start_downloads();
    void startup_download_next();
    bool startup_launch_slot(int slot, int index, int attempt);
    void startup_pump_slots();
    void startup_collect_seals();
    void startup_finish_downloads();
    void startup_abort_seals();
    void startup_download_completed(int result, int response_code, const PackedStringArray &headers, const PackedByteArray &body, int slot);
    void startup_step_downloads(double delta);
    void startup_step_activity(double delta);
    int64_t startup_live_download_bytes() const;
    void startup_verify_next();
    void startup_set_busy(bool busy);
    void startup_clear_overlay();
    void startup_animate(double delta);
    void set_scene_kind(int kind) { scene_kind = kind; }
    int get_scene_kind() const { return scene_kind; }
    void set_native_mode(int value) { native_mode = value; }
    int get_native_mode() const { return native_mode; }

protected:
    static void _bind_methods();

public:
    Control *startup_stage() const { return stage; }
    void mark_content_ready();
    bool is_scene_prepared() const { return scene_assets_ready; }
    void _ready() override;
    void _process(double delta) override;
    void _input(const Ref<InputEvent> &event) override;
    void _notification(int what);
    void _unhandled_input(const Ref<InputEvent> &event) override;
    void _exit_tree() override;
};

} // namespace godot
