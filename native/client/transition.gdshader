shader_type canvas_item;

uniform sampler2D background : source_color, filter_linear;
uniform float progress = 0.0;
uniform float aspect = 1.7777778;
uniform int transition_type = 0;
uniform bool revealing = false;

void fragment() {
    vec2 p = vec2(UV.x * 18.0, UV.y * 18.0 / aspect);
    vec2 lattice = vec2(p.x - p.y / 0.8660254 * 0.5, p.y / 0.8660254);
    vec2 cell = floor(lattice);
    vec2 local = fract(lattice);
    bool first = local.x + local.y < 1.0;
    vec2 center = vec2(cell.x + cell.y * 0.5 + (first ? 0.5 : 1.0),
                       cell.y + (first ? 1.0 / 3.0 : 2.0 / 3.0));
    vec2 center_uv = vec2(center.x / 18.0, center.y * 0.8660254 * aspect / 18.0);
    float front = transition_type == 1 ? center_uv.x : transition_type == 2 ? 1.0 - center_uv.x
        : length(center_uv - vec2(0.5)) / 0.7071068;
    float scale = progress <= 0.001 ? 0.0 : progress >= 0.999 ? 1.0
        : smoothstep(front - 0.022, front + 0.022, progress);
    if (revealing) scale = 1.0 - scale;
    vec3 bary = first ? vec3(1.0 - local.x - local.y, local.x, local.y)
        : vec3(1.0 - local.y, local.x + local.y - 1.0, 1.0 - local.x);
    float alpha = scale <= 0.001 ? 0.0 : scale >= 0.999 ? 1.0
        : smoothstep((1.0 - scale) / 3.0 - 0.008, (1.0 - scale) / 3.0 + 0.008,
                     min(min(bary.x, bary.y), bary.z));
    COLOR = vec4(texture(background, UV).rgb, alpha);
}
