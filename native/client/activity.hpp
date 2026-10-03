#pragma once

#include <godot_cpp/classes/control.hpp>
#include <algorithm>
#include <cmath>

namespace unfalsus_ui {

struct CenteredActivityAnimation {
    // Calibrated from the user's observed rendered sizes: parameter 1.0 ->
    // visual 0.67 and parameter 1.5 -> visual 1.5, so visual 1.0 is ~1.2.
    static constexpr float kCenterScale = 1.2f;
    bool active = false;
    double seconds = 0;
    double fade_seconds = 0;
    float fade_from = 0;

    void begin(godot::Control *icon) {
        active = true;
        seconds = fade_seconds = 0;
        icon->set_pivot_offset(icon->get_size() * .5f);
        icon->set_scale(godot::Vector2(kCenterScale, kCenterScale));
        icon->set_modulate(godot::Color(1, 1, 1, 0));
        icon->show();
    }

    void end(godot::Control *icon) {
        active = false;
        fade_seconds = 0;
        fade_from = icon->get_modulate().a;
    }

    bool step(godot::Control *icon, double delta) {
        float alpha;
        if (active) {
            seconds += delta;
            alpha = std::clamp(float(seconds / .5), 0.f, 1.f);
            const float phase = float(std::fmod(seconds, 1.0));
            const float u = phase < .5f ? phase * 2.f : (phase - .5f) * 2.f;
            const float remaining = 1.f - u;
            // Match the original EaseExponentialIn squeeze and
            // EaseExponentialOut recovery, including exact endpoints.
            const float k = phase < .5f
                ? (u >= 1.f ? 1.f : std::pow(2.f, 10.f * (u - 1.f)))
                : (u <= 0.f ? 0.f : 1.f - std::pow(2.f, -10.f * u));
            const float from = phase < .5f ? kCenterScale : kCenterScale * .01f;
            const float to = phase < .5f ? kCenterScale * .01f : kCenterScale;
            icon->set_scale(godot::Vector2(from + (to - from) * k, kCenterScale));
        } else {
            fade_seconds += delta;
            const float u = std::clamp(float(fade_seconds / .2), 0.f, 1.f);
            alpha = fade_from * (1.f - std::sin(u * 1.570796327f));
        }
        icon->set_modulate(godot::Color(1, 1, 1, alpha));
        icon->set_visible(alpha > .001f);
        return !active && fade_seconds >= .2;
    }
};

}
