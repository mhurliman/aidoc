
#ifndef FFTCOMMON_HLSLI
#define FFTCOMMON_HLSLI

#define ROOTSIG "RootFlags(0),"\
    "CBV(b0)," \
    "CBV(b1)," \
    "DescriptorTable(" \
    "    SRV(t0, numDescriptors = 3)," \
    "    UAV(u0, numDescriptors = 3))"

// Mip-chain downsample: one source-mip SRV, one dest-mip UAV, small params cbuffer.
#define DOWNSAMPLE_ROOTSIG "RootFlags(0)," \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors = 1))," \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"

static const float Pi = 3.1415926;
static const float g = 9.81f;

struct FFTParameters
{
    uint TextureSize;
    float TileSize;
    float RcpTileSize;
};

float Square(float x)
{
    return x * x;
}

float Omega(float2 k)
{
    return sqrt(length(k) * g);
}

float2 ComplexMult(float2 a, float2 b)
{
    return float2(a.r * b.r - a.g * b.g, a.r * b.g + a.g * b.r);
}

float2 ComplexConj(float2 A)
{
    return float2(A.x, -A.y);
}

float2 ComplexExp(float2 a)
{
    return float2(cos(a.y), sin(a.y)) * exp(a.x);
}

#endif