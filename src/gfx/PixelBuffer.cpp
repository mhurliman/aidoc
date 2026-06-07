#include "PixelBuffer.h"
#include "util/Assert.h"

void PixelBuffer::Create(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height,
                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = format;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = flags;

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    ASSERT_SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                     initialState, nullptr,
                                                     IID_PPV_ARGS(&m_resource)));

    m_currentState = initialState;
    m_format       = format;
    m_width        = width;
    m_height       = height;
}
