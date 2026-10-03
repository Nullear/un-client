#pragma once

#include <godot_cpp/variant/string.hpp>

namespace godot::arcapi {
String resolve_base_url(const String &override_url);
}
