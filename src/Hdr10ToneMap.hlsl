cbuffer Hdr10Display : register(b0) {
    float SourcePeakNits;
    float DisplayPeakNits;
    float KneeNits;
    float EnableCompression;
};

Texture2D<float> Hdr10Y : register(t0);
Texture2D<float2> Hdr10UV : register(t1);
SamplerState HdrSampler : register(s0);

float3 PqToNits(float3 encoded) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(encoded), 1.0 / m2);
    return 10000.0 * pow(max((p - c1) / max(c2 - c3 * p, 0.000001), 0.0), 1.0 / m1);
}

float MappedNits(float nits) {
    if (EnableCompression < 0.5 || nits <= KneeNits) return nits;
    if (nits >= SourcePeakNits) return DisplayPeakNits;
    float room = DisplayPeakNits - KneeNits;
    float available = SourcePeakNits - KneeNits;
    if (room <= 0.0 || available <= room) return min(nits, DisplayPeakNits);
    float curve = (available - room) / (available * room);
    float distance = nits - KneeNits;
    return KneeNits + distance / (1.0 + curve * distance);
}

float4 Hdr10P010ToScRgb(float2 uv : TEXCOORD0) : SV_Target {
    const float rawY = Hdr10Y.Sample(HdrSampler, uv) * (65535.0 / 64.0);
    const float2 rawUV = Hdr10UV.Sample(HdrSampler, uv) * (65535.0 / 64.0);
    const float y = (rawY - 64.0) / 876.0;
    const float cb = (rawUV.x - 512.0) / 896.0;
    const float cr = (rawUV.y - 512.0) / 896.0;
    float3 nonlinear2020;
    nonlinear2020.r = y + 1.4746 * cr;
    nonlinear2020.g = y - 0.164553 * cb - 0.571353 * cr;
    nonlinear2020.b = y + 1.8814 * cb;
    float3 rgb2020 = PqToNits(nonlinear2020);
    float luma = max(dot(rgb2020, float3(0.2627, 0.6780, 0.0593)), 0.0);
    float scale = luma > 0.000001 ? MappedNits(luma) / luma : 1.0;
    rgb2020 *= scale;
    float3 rgb709;
    rgb709.r =  1.660491 * rgb2020.r - 0.587641 * rgb2020.g - 0.072850 * rgb2020.b;
    rgb709.g = -0.124550 * rgb2020.r + 1.132900 * rgb2020.g - 0.008349 * rgb2020.b;
    rgb709.b = -0.018151 * rgb2020.r - 0.100579 * rgb2020.g + 1.118729 * rgb2020.b;
    return float4(rgb709 / 80.0, 1.0);
}
