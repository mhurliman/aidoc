
#include "FFTCommon.hlsli"

ConstantBuffer<FFTParameters> FFT : register(b0);

cbuffer Globals : register(b1)
{
    float WindTheta;
    float WindSpeed;
    float SmallWaveLengthCutoff;
    float Amplitude;
    float DirExponent;   // sharpens the directional lobe: (k̂·ŵ)^(2·DirExponent). 1 = classic cos²
    // Energy multiplier for components travelling AGAINST the wind. The directional term above is
    // squared, so it thins waves across the wind but cannot tell upwind from downwind - a pure
    // Phillips ocean runs backwards as readily as forwards. 1 keeps that symmetry; Tessendorf's
    // classic value is 0.07. It scales ENERGY, so amplitude falls as its square root: 0.07 of the
    // energy is about a quarter of the height.
    float UpwindAttenuation;
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
    float kdotw = dot(normalize(k), normalize(WindDir));
    float kw2 = pow(Square(kdotw), DirExponent);   // concentrate energy along the wind axis

    // The one place the spectrum stops being symmetric, and the sense is the opposite of the obvious
    // one. This transform's twiddle is exp(-2*pi*i*n/N), so the surface is summed with exp(-i k.x);
    // working the phases through, the wave that ENDS UP travelling along +d is the one drawn from
    // h0 at -d. Damping k.w > 0 therefore damps the waves that run UPWIND.
    //
    // Verified rather than argued: with this damping the crest pattern shifts +1.0 m downwind per
    // half second along the wind axis, and with the comparison the other way round it shifted the
    // same distance upwind. The phase-sign reasoning is easy to get backwards - it was, once.
    if (kdotw > 0.0)
        kw2 *= UpwindAttenuation;

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
