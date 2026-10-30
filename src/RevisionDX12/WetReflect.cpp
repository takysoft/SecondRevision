#include "Pch.h"
#include "RenderMath.h"
#include "WetReflect.h"
#include "Log.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"
// WetReflect.hlsl's text, put into the DLL by the build (scripts/embed-text.ps1).
#include "WetReflectText.h"

FWetReflect::FWetReflect()
{
}

FWetReflect::~FWetReflect()
{
    Shutdown();
}

UBOOL FWetReflect::Init(ID3D12Device* Device, DXGI_FORMAT Picture, DXGI_FORMAT LightFormat, DXGI_FORMAT DepthFormat,
                        INT Samples)
{
    if (Root)
        return 1;
    if (!Device)
        return 0;
    // The numbers; the copies of the picture and the depth and the mark (t0..t2), each a view of the texture heap; the
    // lamps (b1), a constant buffer of the frame's slot; and the rain's cover (t3), a view of the texture heap.
    D3D12_DESCRIPTOR_RANGE Ranges[4];
    appMemzero(Ranges, sizeof(Ranges));
    D3D12_ROOT_PARAMETER Params[7];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[0].Constants.Num32BitValues = CONSTANTS;
    Params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    for (INT k = 0; k < 3; k++)
    {
        Ranges[k].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Ranges[k].NumDescriptors     = 1;
        Ranges[k].BaseShaderRegister = (UINT)k;
        Params[1 + k].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[1 + k].DescriptorTable.NumDescriptorRanges = 1;
        Params[1 + k].DescriptorTable.pDescriptorRanges   = &Ranges[k];
        Params[1 + k].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    Params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    Params[4].Descriptor.ShaderRegister = 1;
    Params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    Ranges[3].RangeType                           = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[3].NumDescriptors                      = 1;
    Ranges[3].BaseShaderRegister                  = 3;
    Params[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[5].DescriptorTable.NumDescriptorRanges = 1;
    Params[5].DescriptorTable.pDescriptorRanges   = &Ranges[3];
    Params[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    // The own lamps (b2), a constant block of the frame's.
    Params[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    Params[6].Descriptor.ShaderRegister = 2;
    Params[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    // The image read between its pixels, never past the picture's edge.
    D3D12_STATIC_SAMPLER_DESC Samp;
    appMemzero(&Samp, sizeof(Samp));
    Samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samp.MaxLOD           = D3D12_FLOAT32_MAX;
    Samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    Samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 7;
    Desc.pParameters       = Params;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &Samp;
    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("wet floors: the reflection pass's root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    const SIZE_T Len = sizeof(GWetReflectText) - 1;
    TComRef<ID3DBlob> Vs;
    if (FAILED(RdxCompile(GWetReflectText, Len, "WetReflect", nullptr, nullptr, "VSFull", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("wet floors: the reflection pass's vertex shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Root.Release();
        return 0;
    }
    // The picture alone, and the picture with the shadows' light and added targets: the light target untouched, the
    // added one taking what the picture takes.
    D3D_SHADER_MACRO Three[2];
    Three[0].Name       = "RDX_LIGHT_TARGET";
    Three[0].Definition = "1";
    Three[1].Name       = nullptr;
    Three[1].Definition = nullptr;
    UBOOL bAny = 0;
    for (INT q = 0; q < 4; q++)
    {
        const INT         e = q / 2, t = q % 2;
        TComRef<ID3DBlob> Ps;
        if (FAILED(RdxCompile(GWetReflectText, Len, "WetReflect", t ? Three : nullptr, nullptr,
                              e ? "PSWetFlames" : "PSWet", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
        {
            RdxLog(TEXT("wet floors: the reflection pass's pixel shader failed: %s"),
                   Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
            continue;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
        appMemzero(&Pso, sizeof(Pso));
        Pso.pRootSignature        = Root.Get();
        Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
        Pso.VS.BytecodeLength     = Vs->GetBufferSize();
        Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
        Pso.PS.BytecodeLength     = Ps->GetBufferSize();
        Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Pso.NumRenderTargets      = t ? 3 : 1;
        Pso.RTVFormats[0]         = Picture;
        if (t)
        {
            Pso.RTVFormats[1] = LightFormat;
            Pso.RTVFormats[2] = Picture;
        }
        // Not tested against the depth - the pixels are the marked ones - but drawn with the scene's bound; the flames'
        // draw comes after the volumetric walk, over the picture alone and with no depth bound, as the walk's own is.
        Pso.DSVFormat         = e ? DXGI_FORMAT_UNKNOWN : DepthFormat;
        Pso.SampleMask        = UINT_MAX;
        Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        Pso.RasterizerState.DepthClipEnable = TRUE;
        Pso.BlendState.IndependentBlendEnable = TRUE;
        for (INT k = 0; k < 3; k++)
        {
            D3D12_RENDER_TARGET_BLEND_DESC& B = Pso.BlendState.RenderTarget[k];
            B.BlendEnable           = (k != 1) ? TRUE : FALSE;
            B.SrcBlend              = D3D12_BLEND_ONE;
            B.DestBlend             = D3D12_BLEND_SRC_ALPHA;
            B.BlendOp               = D3D12_BLEND_OP_ADD;
            B.SrcBlendAlpha         = D3D12_BLEND_ZERO;
            B.DestBlendAlpha        = D3D12_BLEND_ONE;
            B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            B.RenderTargetWriteMask = (k != 1) ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
        }
        for (INT s = 0; s < 2; s++)
        {
            if (s && Samples <= 1)
                break;
            Pso.SampleDesc.Count = s ? (UINT)Samples : 1u;
            if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState),
                                                           Pipelines[e][t][s].VoidSlot())))
                RdxLog(TEXT("wet floors: the reflection pass's pipeline (%i targets, %i samples) was refused"),
                       t ? 3 : 1, (INT)Pso.SampleDesc.Count);
            else
                bAny = 1;
        }
    }
    if (!bAny)
    {
        Root.Release();
        return 0;
    }
    RdxLog(TEXT("wet floors: the late reflection pass made - the world given back over the wet floors"));
    return 1;
}

void FWetReflect::Shutdown()
{
    for (INT e = 0; e < 2; e++)
        for (INT t = 0; t < 2; t++)
            for (INT s = 0; s < 2; s++)
                Pipelines[e][t][s].Release();
    Root.Release();
}

void FWetReflect::Draw(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, const FLOAT* Constants,
                       D3D12_GPU_DESCRIPTOR_HANDLE Picture, D3D12_GPU_DESCRIPTOR_HANDLE Depth,
                       D3D12_GPU_DESCRIPTOR_HANDLE Mark, D3D12_GPU_DESCRIPTOR_HANDLE Cover, D3D12_GPU_VIRTUAL_ADDRESS Lamps,
                       D3D12_GPU_VIRTUAL_ADDRESS Spots, UBOOL bThree, UBOOL bMsaa, UBOOL bFlames)
{
    ID3D12PipelineState* const Pso = Pipelines[bFlames ? 1 : 0][bThree ? 1 : 0][bMsaa ? 1 : 0].Get();
    if (!Cmd || !Heap || !Constants || !Root || !Pso || !Lamps || !Spots)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Heap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetPipelineState(Pso);
    Cmd->SetGraphicsRoot32BitConstants(0, CONSTANTS, Constants, 0);
    Cmd->SetGraphicsRootDescriptorTable(1, Picture);
    Cmd->SetGraphicsRootDescriptorTable(2, Depth);
    Cmd->SetGraphicsRootDescriptorTable(3, Mark);
    Cmd->SetGraphicsRootConstantBufferView(4, Lamps);
    Cmd->SetGraphicsRootDescriptorTable(5, Cover);
    Cmd->SetGraphicsRootConstantBufferView(6, Spots);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}
