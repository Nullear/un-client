shader_type canvas_item;

uniform sampler2D y_data;
uniform sampler2D u_data;
uniform sampler2D v_data;
uniform sampler2D a_data;
uniform vec2 resolution;
uniform vec4 color_profile;
uniform bool full_color = false;
uniform int interlaced = 0;
uniform float rotation = 0.0;

varying vec2 tex_uv;
varying vec4 vertex_color;

const vec3 LIMITED_Y_OFFSET = vec3(16.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0);
const vec3 LIMITED_SCALE = vec3(255.0 / 219.0, 255.0 / 224.0, 255.0 / 224.0);

void vertex() {
    float c = cos(rotation);
    float s = sin(rotation);
    mat2 rot_mat = mat2(vec2(c, s), vec2(-s, c));
    tex_uv = rot_mat * (UV - 0.5) + 0.5;
    vertex_color = COLOR;
}

void fragment() {
    if (tex_uv.x < 0.0 || tex_uv.x > 1.0 || tex_uv.y < 0.0 || tex_uv.y > 1.0) {
        COLOR = vec4(0.0);
    } else {
        float y = texture(y_data, tex_uv).r;
        if (interlaced > 0) {
            float direction = interlaced == 1 ? -1.0 : 1.0;
            float neighbor = texture(y_data, clamp(tex_uv + vec2(0.0, direction / resolution.y), 0.0, 1.0)).r;
            y = mix(y, neighbor, 0.5);
        }
        vec3 yuv = vec3(y, texture(u_data, tex_uv).r, texture(v_data, tex_uv).r);
        if (full_color) yuv.yz -= 0.5;
        else yuv = (yuv - LIMITED_Y_OFFSET) * LIMITED_SCALE;
        COLOR = vec4(
            yuv.x + yuv.z * color_profile.x,
            yuv.x - yuv.y * color_profile.y - yuv.z * color_profile.z,
            yuv.x + yuv.y * color_profile.w,
            texture(a_data, tex_uv).r) * vertex_color;
    }
}
