#include "Material.h"
#include "Texture.h"
#include "../helpers/GraphicsContext.h"
#include "../helpers/LinearAllocator.h"
#include "../helpers/DescriptorHeap.h"
#include "../helpers/Assert.h"
#include <cassert>
#include <cstring>
#include <fstream>

// Resolve a relative path against the directory containing the executable.
static std::string ResolveExePath(const std::string& relativePath)
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
    {
        dir = dir.substr(0, pos + 1);
    }
    return dir + relativePath;
}

// Static member definitions
ID3D12RootSignature* Material::s_rootSignature = nullptr;
std::vector<BYTE> Material::s_pbrVsBlob;
std::vector<BYTE> Material::s_pbrPsBlob;
std::vector<BYTE> Material::s_phongVsBlob;
std::vector<BYTE> Material::s_phongPsBlob;

std::vector<BYTE> Material::LoadShaderBlob(const std::string& filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    assert(file.is_open() && "Failed to open shader file");
    auto size = file.tellg();
    std::vector<BYTE> blob(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(blob.data()), size);
    return blob;
}

void Material::InitSharedState(ID3D12Device* device, ID3D12RootSignature* rootSignature)
{
    s_rootSignature = rootSignature;
    s_pbrVsBlob = LoadShaderBlob(ResolveExePath("shaders/pbr_vs.cso"));
    s_pbrPsBlob = LoadShaderBlob(ResolveExePath("shaders/pbr_ps.cso"));
    s_phongVsBlob = LoadShaderBlob(ResolveExePath("shaders/phong_vs.cso"));
    s_phongPsBlob = LoadShaderBlob(ResolveExePath("shaders/phong_ps.cso"));
}

void Material::CreatePSO(ID3D12Device* device)
{
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = s_rootSignature;
    if (shadingModel == ShadingModel::Phong)
    {
        psoDesc.VS = {s_phongVsBlob.data(), s_phongVsBlob.size()};
        psoDesc.PS = {s_phongPsBlob.data(), s_phongPsBlob.size()};
    }
    else
    {
        psoDesc.VS = {s_pbrVsBlob.data(), s_pbrVsBlob.size()};
        psoDesc.PS = {s_pbrPsBlob.data(), s_pbrPsBlob.size()};
    }
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = doubleSided ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    auto& rt0 = psoDesc.BlendState.RenderTarget[0];
    if (alphaBlend)
    {
        rt0.BlendEnable = TRUE;
        rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt0.BlendOp = D3D12_BLEND_OP_ADD;
        rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    else
    {
        rt0.BlendEnable = FALSE;
    }
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ASSERT_SUCCEEDED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));
}

void Material::Bind(GraphicsContext& ctx, LinearAllocator& allocator,
                    TransientDescriptorHeap& transientHeap, ID3D12Device* device,
                    D3D12_CPU_DESCRIPTOR_HANDLE nullSrvHandle)
{
    // Set this material's PSO
    ctx.SetPipelineState(m_pso.Get());

    // Fill and bind material constant buffer at root param 1
    MaterialConstants constants = {};
    constants.baseColorFactor = baseColorFactor;
    constants.emissiveFactor = emissiveFactor;
    constants.roughness = roughness;
    constants.metallic = metallic;
    constants.hasTexture = HasBaseColorTexture() ? 1 : 0;
    constants.hasEmissiveTexture = HasEmissiveTexture() ? 1 : 0;

    auto alloc = allocator.Allocate(sizeof(MaterialConstants));
    memcpy(alloc.cpuAddress, &constants, sizeof(MaterialConstants));
    ctx.SetConstantBufferView(1, alloc.gpuAddress);

    // Copy texture descriptors from persistent -> transient heap
    D3D12_CPU_DESCRIPTOR_HANDLE srcHandles[2];
    srcHandles[0] = baseColorTexture ? baseColorTexture->GetSRVHandle() : nullSrvHandle;
    srcHandles[1] = emissiveTexture ? emissiveTexture->GetSRVHandle() : nullSrvHandle;

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = transientHeap.CopyDescriptors(device, srcHandles, 2);
    ctx.SetDescriptorTable(2, gpuHandle);
}
