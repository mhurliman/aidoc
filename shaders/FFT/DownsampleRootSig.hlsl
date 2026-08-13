#include "FFTCommon.hlsli"

#undef ROOTSIG
#define ROOTSIG DOWNSAMPLE_ROOTSIG

[RootSignature(ROOTSIG)]
void main() {}
