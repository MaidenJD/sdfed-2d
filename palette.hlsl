float sdf_circle(float2 coord, float2 position, float radius) {
    coord -= position;
    return distance(coord, float2(0.0, 0.0)) - radius;
}

float sdf_rect(float2 coord, float2 position, float2 size, float4 corner_radii)
{
    coord -= position;

    corner_radii.xy = (coord.x > 0.0) ? corner_radii.xy : corner_radii.zw;
    corner_radii.x  = (coord.y > 0.0) ? corner_radii.x  : corner_radii.y;
    corner_radii.x *= min(size.x, size.y);

    float2 corner = abs(coord) - size + corner_radii.x;
    return min(max(corner.x, corner.y), 0.0) + length(max(corner, 0.0)) - corner_radii.x;
}

float sdf_union(float a, float b, float blend) {
    float blend_alpha = clamp((b - a) * 0.5 / blend + 0.5, 0.0, 1.0);
    float base = lerp(b, a, blend_alpha);
    float blended_pow = blend_alpha * blend * (1.0 - blend_alpha);
    return base - blended_pow;
}

float sdf_intersect(float a, float b, float blend) {
    return max(a, b);
}

float sdf_subtract(float a, float b, float blend) {
    return sdf_intersect(a, -b, blend);
}