
#include "FFTCommon.hlsli"

ConstantBuffer<FFTParameters> FFT : register(b0);

cbuffer Globals : register(b1)
{
    float WindTheta;
    float WindSpeed;
    float SmallWaveLengthCutoff;
    float Amplitude;
}

Texture2D<float2> Noise : register(t0);

RWTexture2D<float4> H0 : register(u0);

float PhillipsSpectrum2D(float2 k)
{
    if (length(k) < 1e-4)
        return 0;

    float2 WindDir = float2(cos(WindTheta), sin(WindTheta));
    
    float L = Square(WindSpeed) / g;
    float k2 = dot(k, k);
    float k4 = Square(k2);
    float kL2 = k2 * Square(L);

    float Cutoff = exp(-k2 * Square(SmallWaveLengthCutoff));
    float kw2 = Square(dot(normalize(k), normalize(WindDir)));

    return Amplitude * exp(-1.0 / kL2) * kw2 * Cutoff / k4;
}

[RootSignature(ROOTSIG)]
[NumThreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    float DeltaK = 2 * Pi * FFT.RcpTileSize;
    float HalfTexSize = float(FFT.TextureSize) * 0.5;

    float Nx = float(DTid.x) - HalfTexSize;
    float Nz = float(DTid.y) - HalfTexSize;
    float2 k = float2(Nx, Nz) * DeltaK;

    float2 NoiseK = Noise[DTid.xy].xy;
    float2 H0K = NoiseK * sqrt(PhillipsSpectrum2D(k) * 0.5);

    uint2 SamplePoint = -DTid.xy + FFT.TextureSize;
    float2 NoiseMinusK = Noise[SamplePoint];

    float2 H0MinusK     = NoiseMinusK * sqrt(PhillipsSpectrum2D(-k) * 0.5);
    float2 H0MinusKConj = ComplexConj(H0MinusK);

    H0[DTid.xy] = float4(H0K, H0MinusKConj);
}
