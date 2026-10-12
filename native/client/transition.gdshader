shader_type canvas_item;

uniform sampler2D background : source_color, filter_linear;
uniform float progress = 0.0;
uniform float aspect = 1.7777778;
uniform int transition_type = 0;
uniform bool revealing = false;

// Hash function for pseudo-random offset per layer
float hash(float n) {
    return fract(sin(n) * 43758.5453123);
}

vec4 compute_layer(vec2 uv, float layer_scale, float layer_offset, float layer_alpha) {
    vec2 p = vec2(uv.x * layer_scale, uv.y * layer_scale / aspect);
    vec2 lattice = vec2(p.x - p.y / 0.8660254 * 0.5, p.y / 0.8660254);
    vec2 cell = floor(lattice);
    vec2 local = fract(lattice);
    bool first = local.x + local.y < 1.0;
    vec2 center = vec2(cell.x + cell.y * 0.5 + (first ? 0.5 : 1.0),
                       cell.y + (first ? 1.0 / 3.0 : 2.0 / 3.0));
    vec2 center_uv = vec2(center.x / layer_scale, center.y * 0.8660254 * aspect / layer_scale);
    
    float front = transition_type == 1 ? center_uv.x : transition_type == 2 ? 1.0 - center_uv.x
        : length(center_uv - vec2(0.5)) / 0.7071068;
    
    // Apply layer-specific time offset for staggered animation
    float layer_progress = clamp(progress + layer_offset, 0.0, 1.0);
    float scale = layer_progress <= 0.001 ? 0.0 : layer_progress >= 0.999 ? 1.0
        : smoothstep(front - 0.035, front + 0.035, layer_progress);
    if (revealing) scale = 1.0 - scale;
    
    vec3 bary = first ? vec3(1.0 - local.x - local.y, local.x, local.y)
        : vec3(1.0 - local.y, local.x + local.y - 1.0, 1.0 - local.x);
    
    float alpha = scale <= 0.001 ? 0.0 : scale >= 0.999 ? 1.0
        : smoothstep((1.0 - scale) / 3.0 - 0.015, (1.0 - scale) / 3.0 + 0.015,
                     min(min(bary.x, bary.y), bary.z));
    
    return vec4(texture(background, uv).rgb, alpha * layer_alpha);
}

void fragment() {
    // Based on binary analysis: CellDensity=15 found in sharedassets0.assets at 0x4904/0x4990/0x4A1C
    // Original Unity shader uses three identical Voronoi layers (all scale 15)
    vec4 layer1 = compute_layer(UV, 15.0, 0.0, 1.0);           // Front layer (15 from assets)
    vec4 layer2 = compute_layer(UV, 15.0, -0.08, 0.7);         // Mid layer (15 from assets)
    vec4 layer3 = compute_layer(UV, 15.0, -0.15, 0.4);         // Back layer (15 from assets)
    
    // Composite layers back-to-front with alpha blending
    vec4 result = layer3;
    result.rgb = mix(result.rgb, layer2.rgb, layer2.a);
    result.a = max(result.a, layer2.a);
    result.rgb = mix(result.rgb, layer1.rgb, layer1.a);
    result.a = max(result.a, layer1.a);
    
    COLOR = result;
}
