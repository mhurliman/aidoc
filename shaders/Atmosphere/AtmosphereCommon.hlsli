#ifndef ATMOSPHERECOMMON_HLSLI
#define ATMOSPHERECOMMON_HLSLI

#define ATM_PI 3.14159265358979

// All distances in kilometres.
// Planet centre at origin. Camera position = (0, bottomRadius + altKm, 0).

// Root signature shared by all atmosphere compute passes.
// b0: AtmosphereParameters  b1: per-pass constants
// t0: input SRV (transmittance LUT or null)
// u0: output UAV
// s0: static linear-clamp sampler
#define ATM_COMPUTE_ROOTSIG \
    "RootFlags(0), " \
    "CBV(b0), " \
    "CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors = 1)), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1)), " \
    "StaticSampler(s0, " \
    "    filter = FILTER_MIN_MAG_MIP_LINEAR, " \
    "    addressU = TEXTURE_ADDRESS_CLAMP, " \
    "    addressV = TEXTURE_ADDRESS_CLAMP, " \
    "    addressW = TEXTURE_ADDRESS_CLAMP)"

// Root signature for the env-cubemap compute pass.
// b0: SkyRenderConstants  b1: (unused)
// t0: SkyViewLUT SRV  t1: TransmittanceLUT SRV
// u0: RWTexture2DArray (cubemap array)
// s0: static linear-clamp sampler
#define ATM_ENVMAP_ROOTSIG \
    "RootFlags(0), " \
    "CBV(b0), " \
    "CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors = 2)), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1)), " \
    "StaticSampler(s0, " \
    "    filter = FILTER_MIN_MAG_MIP_LINEAR, " \
    "    addressU = TEXTURE_ADDRESS_CLAMP, " \
    "    addressV = TEXTURE_ADDRESS_CLAMP, " \
    "    addressW = TEXTURE_ADDRESS_CLAMP)"

// Root signature for the fullscreen sky composite pass.
// b0: SkyRenderConstants
// t0: SkyViewLUT SRV   t1: TransmittanceLUT SRV
// s0: static linear-clamp sampler
#define ATM_RENDER_ROOTSIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 2)), " \
    "StaticSampler(s0, " \
    "    filter = FILTER_MIN_MAG_MIP_LINEAR, " \
    "    addressU = TEXTURE_ADDRESS_CLAMP, " \
    "    addressV = TEXTURE_ADDRESS_CLAMP, " \
    "    addressW = TEXTURE_ADDRESS_CLAMP)"

// Matches C++ AtmosphereParameters (80 bytes, naturally float4-aligned).
struct AtmosphereParameters
{
    float3 RayleighBeta;        float RayleighScaleHeight;
    float3 MieBeta;             float MieScaleHeight;
    float3 MieBetaExt;          float MieG;
    float3 OzoneBetaAbs;        float OzoneCenterHeight;
    float  OzoneWidth;          float BottomRadius;
    float  TopRadius;           float SunIntensity;
};

// Per-frame data for the SkyViewLUT compute pass.
struct SkyViewConstants
{
    float3 CameraPositionAtm;   // (0, bottomRadius + altKm, 0)
    float  _pad0;
    float3 SunDirection;        // normalised, points toward sun
    float  _pad1;
};

// Constants for the sky composite pixel shader.
struct SkyRenderConstants
{
    float4x4 InvViewProj;
    float3   CameraPositionAtm;  // km atmosphere space
    float    _pad0;
    float3   SunDirection;
    float    SunIntensity;
    float    BottomRadius;
    float    TopRadius;
    float2   _pad1;
};

// --- Phase functions ---

float RayleighPhase(float cosTheta)
{
    return (3.0 / (16.0 * ATM_PI)) * (1.0 + cosTheta * cosTheta);
}

float MiePhase(float cosTheta, float g)
{
    float g2  = g * g;
    float den = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (3.0 * (1.0 - g2) * (1.0 + cosTheta * cosTheta))
         / ((8.0 * ATM_PI) * (2.0 + g2) * pow(den, 1.5));
}

// --- Density profiles ---

float RayleighDensity(float altitude, float scaleHeight)
{
    return exp(-altitude / scaleHeight);
}

float MieDensity(float altitude, float scaleHeight)
{
    return exp(-altitude / scaleHeight);
}

float OzoneDensity(float altitude, float centerHeight, float width)
{
    return max(0.0, 1.0 - abs(altitude - centerHeight) / width);
}

// --- Ray-sphere intersection ---
// Returns (near, far) distances; negative values mean no intersection or behind ray.
float2 RaySphereIntersect(float3 rayOrigin, float3 rayDir, float3 sphereCenter, float radius)
{
    float3 oc = (rayOrigin - sphereCenter) / radius;
    float  b  = dot(oc, rayDir);
    float  c  = dot(oc, oc) - 1.0;
    float  d  = b * b - c;
    if (d < -1e-6) return float2(-1.0, -1.0);
    float sq = sqrt(max(d, 0.0));
    return float2((-b - sq) * radius, (-b + sq) * radius);
}

// --- Transmittance LUT helpers ---
// Simple linear parameterisation (same as reference).
// u = (mu + 1) / 2,  v = (radius - bottom) / (top - bottom)

float2 TransmittanceLUTUv(AtmosphereParameters atm, float3 samplePt, float3 lightDir)
{
    float3 up  = normalize(samplePt);
    float  mu  = dot(up, lightDir);
    float  r   = length(samplePt);
    float  u   = mu * 0.5 + 0.5;
    float  v   = clamp((r - atm.BottomRadius) / max(atm.TopRadius - atm.BottomRadius, 1e-5),
                       0.0, 1.0);
    return float2(u, v);
}

float3 SampleTransmittanceLUT(Texture2D<float4> lut, SamplerState samp,
                               AtmosphereParameters atm,
                               float3 samplePt, float3 lightDir)
{
    return lut.SampleLevel(samp, TransmittanceLUTUv(atm, samplePt, lightDir), 0).rgb;
}

// --- Sky-View LUT helpers ---
// Azimuth: [-PI, PI] → [0, 1];  Elevation: [-PI/2, PI/2] → [0, 1] with sqrt compress.

float3 SkyViewForward(float3 up, float3 sunDir)
{
    float3 proj = sunDir - up * dot(sunDir, up);
    float  lenSq = dot(proj, proj);
    if (lenSq < 1e-5)
        proj = abs(up.y) > 0.99 ? float3(1, 0, 0) : float3(0, 1, 0);
    return normalize(proj);
}

float3 SkyViewUvToDir(float2 uv, float3 up, float3 sunDir)
{
    float3 fwd   = SkyViewForward(up, sunDir);
    float3 right = normalize(cross(fwd, up));

    float azimuth   = (uv.x * 2.0 - 1.0) * ATM_PI;
    float elevation = (uv.y * uv.y - 0.5) * ATM_PI;

    float cosEl = cos(elevation);
    float3 horiz = cos(azimuth) * fwd + sin(azimuth) * right;
    return normalize(horiz * cosEl + up * sin(elevation));
}

float2 SkyViewDirToUv(float3 rayDir, float3 up, float3 sunDir)
{
    float3 fwd   = SkyViewForward(up, sunDir);
    float3 right = normalize(cross(fwd, up));

    float  vert    = clamp(dot(rayDir, up), -1.0, 1.0);
    float3 horiz   = rayDir - up * vert;
    float  azimuth = atan2(dot(horiz, right), dot(horiz, fwd));
    float  elev    = asin(vert);
    float  elev01  = clamp(elev / ATM_PI + 0.5, 0.0, 1.0);

    return float2(azimuth / (2.0 * ATM_PI) + 0.5, sqrt(elev01));
}

// --- Tonemapping ---
float3 ACESFilm(float3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

#endif // ATMOSPHERECOMMON_HLSLI
