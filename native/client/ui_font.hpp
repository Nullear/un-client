#pragma once

#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/text_server.hpp>

#include <algorithm>

namespace unfalsus_ui {

inline godot::Ref<godot::Font> prepare_font(const char *file) {
	using namespace godot;
	Ref<FontFile> font = ResourceLoader::get_singleton()->load(
		String("res://assets/resources/font/mainmenu/") + file);
	if (!font.is_valid()) return Ref<Font>();
	if (String(file) == "Bai-Jamjuree.ttf") {
		Ref<FontFile> vivo = ResourceLoader::get_singleton()->load(
			"res://assets/resources/font/mainmenu/VivoSans-Regular-Full.ttf");
		TypedArray<Font> fallbacks;
		if (vivo.is_valid()) fallbacks.push_back(vivo);
		font->set_fallbacks(fallbacks);
	}
	return font;
}

// Creator overflow NONE sizes one line to (1 + 0.26) * fontSize, plus outline on each side.
// The node position is the anchor. The baseline sits fontSize + outline below the box top.
inline void place_ccc(godot::Label *label, godot::Vector2 anchor, float ax, float ay, float width, int size, int outline) {
	using namespace godot;
	if (!label) return;
	const float s = float(std::max(size, 1));
	const float o = float(std::max(outline, 0));
	const float h = (1.f + 0.26f) * s + o * 2.f;
	Ref<Font> font = label->get_theme_font("font");
	float ascent = s * 0.928f;
	float descent = s * 0.244f;
	if (font.is_valid()) {
		ascent = font->get_ascent(size);
		descent = font->get_descent(size);
	}
	const float shift = (s + o) - (h * 0.5f + (ascent - descent) * 0.5f);
	label->set_clip_text(false);
	label->set_text_overrun_behavior(TextServer::OVERRUN_NO_TRIMMING);
	label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	label->set_horizontal_alignment(ax >= 0.75f ? HORIZONTAL_ALIGNMENT_RIGHT
		: ax <= 0.25f ? HORIZONTAL_ALIGNMENT_LEFT : HORIZONTAL_ALIGNMENT_CENTER);
	label->set_size(Vector2(std::max(width, 1.f), h));
	label->set_position(Vector2(anchor.x - width * ax, anchor.y - h * (1.f - ay) + shift));
}

}
