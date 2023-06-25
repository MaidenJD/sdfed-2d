
cbuffer uniforms : register(b0) { 
    uint   mode                  = 0;
    float  blur_amount           = 0.01;
    float  division_distance     = 0.25;
    float  division_thickness    = 0.005;
    int    num_subdivisions      = 4;
    float  subdivision_thickness = 0.001;
    float4 inside_color          = float4(0.7, 0.3, 0.5, 1.0);
    float4 outside_color         = float4(0.3, 0.7, 0.5, 1.0);
}

#define PreviewMode_Mask 1
#define PreviewMode_Contour 2

struct PS_OUTPUT {
    float4 color : SV_Target;
};

PS_OUTPUT main(float2 uv : TEXCOORD)
{
    float sdf = build_sdf(uv);

    float4 frag_color = float4(0.0, 0.0, 0.0, 1.0);

    if (mode == PreviewMode_Mask) {
        float alpha = 1.0 - smoothstep(0.0, blur_amount, sdf);
        frag_color = lerp(float4(0.0, 0.0, 0.0, 1.0), float4(1.0, 1.0, 1.0, 1.0), alpha);
    } else if (mode == PreviewMode_Contour) {
        float4 color = lerp(inside_color, outside_color, step(sdf, 0));

        float distance_change = fwidth(sdf) * 0.5;

        float distance_between_divisions = abs(frac(sdf / division_distance + 0.5) - 0.5) * division_distance;
        float divisions = smoothstep(division_thickness - distance_change, division_thickness + distance_change, distance_between_divisions);

        float distance_between_subdivisions = division_distance / float(num_subdivisions);
        float subdivision_distance = abs(frac(sdf / distance_between_subdivisions + 0.5) - 0.5) * distance_between_subdivisions;
        float subvisions = smoothstep(subdivision_thickness - distance_change, subdivision_thickness + distance_change, subdivision_distance);

        frag_color = color * divisions * subvisions;
    } else {
        float clamped_sdf = clamp(sdf, 0.0, 1.0);
        frag_color = float4(clamped_sdf, clamped_sdf, clamped_sdf, 1.0);
    }

    // Gamma
    // frag_color = float4(pow(abs(frag_color.rgb), float3(2.2, 2.2, 2.2)) * sign(frag_color.rgb), frag_color.a);

    PS_OUTPUT output;
    output.color = frag_color;

    return output;
}
