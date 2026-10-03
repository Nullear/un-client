shader_type canvas_item;

uniform sampler2D mask_texture : source_color, filter_linear;
uniform float scroll_offset = 0.0;
uniform float bar_width = 367.0;

void fragment() {
    vec2 mask_uv = UV + vec2(scroll_offset / bar_width, 0.0);
    COLOR.a *= texture(mask_texture, mask_uv).a;
}
