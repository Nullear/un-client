#pragma once

#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot::content {
String path(const String &relative);
Ref<Texture2D> image(const String &relative);
Ref<Texture2D> cached_image(const String &relative);
void invalidate_image(const String &relative);
bool image_failed(const String &relative);
void begin_gallery_images(const PackedStringArray &relative_paths);
void step_gallery_images();
Array characters();
PackedStringArray songs_with_rating_at_least(int rating);
Dictionary character(const Array &catalog, int id);
int selected();
void save_selected(int id);
void remember_online(const Variant &parsed);
void remember_game_info(const Variant &parsed);
bool unlocked(int id);
int level_of(int id);
bool insight_level_unknown(int id, bool logged_in);
double exp_into_level(int id);
bool uncapped(int id);
bool uncapped_override(int id);
bool skill_sealed();
void set_skill_sealed(bool sealed);
int ticket();
void set_ticket(int value);
int base_frag(int id);
int max_frag(int id);
bool uncappable(int id);
int frag_of(int id);
void grant_purchased(int id);
int ether_drop_count();
int core_exp_value();
Dictionary level_exp_table();
int next_level_exp(int current_level);
String random_jacket();
String song_title(const String &id);
Ref<ShaderMaterial> hue_material(const Dictionary &row, bool strip = false);
void warmup_partner_assets_begin();
bool warmup_partner_assets_step(int batch = 2);
int warmup_partner_assets_total();
int warmup_partner_assets_ready();
void release_cached_images();
}
