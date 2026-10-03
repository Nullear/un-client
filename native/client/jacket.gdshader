shader_type canvas_item;
uniform sampler2D mask_texture : source_color, filter_linear;

varying vec4 vertex_color;

void vertex() {
    vertex_color = COLOR;
}

void fragment() {
    vec2 jacket_uv = vec2(UV.x, 0.5 + (UV.y - 0.5) * 0.2);
    vec4 jacket = texture(TEXTURE, jacket_uv) * vertex_color;
    jacket.a *= texture(mask_texture, UV).a;
    COLOR = jacket;
}
