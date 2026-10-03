shader_type canvas_item;
uniform float hue = 193.0;
uniform bool grey = false;
uniform float bottom_fade = 0.0;

vec3 rgb2hsv(vec3 c) {
    vec4 k = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, k.wz), vec4(c.gb, k.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w-q.y)/(6.0*d+1e-10)), d/(q.x+1e-10), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0/3.0, 1.0/3.0))*6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p-1.0, 0.0, 1.0), c.y);
}

void fragment() {
    vec4 c = texture(TEXTURE, UV) * COLOR;
    vec3 hsv = rgb2hsv(c.rgb);
    hsv.x = fract(hue / 360.0);
    if (grey) { hsv.y = 0.0; hsv.z = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)); }
    c.rgb = hsv2rgb(hsv);
    if (bottom_fade > 0.0) c.a *= clamp((1.0-UV.y) / bottom_fade, 0.0, 1.0);
    COLOR = c;
}
