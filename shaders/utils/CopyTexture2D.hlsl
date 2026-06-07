
cbuffer Constants : register(b0)
{
    uint2 SrcOffset;
    uint2 SrcSize;

    uint2 DestOffset;
    uint2 DestSize;

    uint2 CopyDims;
}

Texture2D<float2> Src : register(t0);
RWTexture2D<float2> Dest : register(u0);

[NumThreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= CopyDims))
    {
        return;
    }

    uint2 SrcPixel = SrcOffset + DTid.xy;
    uint2 DestPixel = DestOffset + DTid.xy;

    if (any(SrcPixel >= SrcSize) || any(DestPixel >= DestSize))
    {
        return;
    }

    Dest[DestPixel] = Src[SrcPixel];
}