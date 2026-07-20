#ifndef COMMON_HLSLI
#define COMMON_HLSLI

// Root signature for the PBR/Phong material rendering pipeline.
// Matches Renderer::m_rootSignature built in Renderer.cpp.
#define ROOTSIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
    "CBV(b0), " \
    "CBV(b1, visibility=SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL), " \
    "SRV(t2, visibility=SHADER_VISIBILITY_PIXEL), " \
    "CBV(b2, visibility=SHADER_VISIBILITY_VERTEX), " \
    "CBV(b3, visibility=SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t3, numDescriptors=1), visibility=SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s0, " \
        "filter=FILTER_MIN_MAG_MIP_LINEAR, " \
        "addressU=TEXTURE_ADDRESS_WRAP, " \
        "addressV=TEXTURE_ADDRESS_WRAP, " \
        "addressW=TEXTURE_ADDRESS_WRAP, " \
        "visibility=SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s1, " \
        "filter=FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, " \
        "comparisonFunc=COMPARISON_LESS_EQUAL, " \
        "addressU=TEXTURE_ADDRESS_CLAMP, " \
        "addressV=TEXTURE_ADDRESS_CLAMP, " \
        "addressW=TEXTURE_ADDRESS_CLAMP, " \
        "visibility=SHADER_VISIBILITY_PIXEL)"

#endif
