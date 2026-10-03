#pragma once

#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot::content {
String path(const String &relative);
Ref<Texture2D> image(const String &relative);
Array characters();
Dictionary character(const Array &catalog, int id);
int selected();
void save_selected(int id);
void remember_online(const Variant &parsed);
bool unlocked(int id);
int level_of(int id);
double exp_into_level(int id);
bool uncapped(int id);
int ticket();
void set_ticket(int value);
int base_frag(int id);
int max_frag(int id);
bool uncappable(int id);
int frag_of(int id);
void grant_purchased(int id);
String random_jacket();
String song_title(const String &id);
Ref<ShaderMaterial> hue_material(const Dictionary &row, bool strip = false);
void warmup_partner_assets_begin();
bool warmup_partner_assets_step(int batch = 2);
int warmup_partner_assets_total();
int warmup_partner_assets_ready();
void release_cached_images();
}
