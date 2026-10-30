#include "Pch.h"
#include "MirrorBody.h"
#include "Log.h"
#include "RenderMath.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"
// MirrorBody.hlsl's text, put into the DLL by the build (scripts/embed-text.ps1).
#include "MirrorBodyText.h"

#define RDX_BODY_STRINGIFY2(x) #x
#define RDX_BODY_STRINGIFY(x)  RDX_BODY_STRINGIFY2(x)

FMirrorBody::FMirrorBody()
{
    PictureState = D3D12_RESOURCE_STATE_COMMON;
    Device       = nullptr;
    Width        = 0;
    Height       = 0;
}

FMirrorBody::~FMirrorBody()
{
    Shutdown();
}

UBOOL FMirrorBody::Init(ID3D12Device* InDevice)
{
    if (Pipeline)
        return 1;
    if (!InDevice)
        return 0;
    Device = InDevice;
    // The constants; the body's vertices and texture places (t0, t1), buffers read by the vertex's number; and the
    // texture heap from its first view on (t0 of space 1), where the skins are.
    D3D12_DESCRIPTOR_RANGE Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Heap.NumDescriptors     = (UINT)-1;
    Heap.BaseShaderRegister = 0;
    Heap.RegisterSpace      = 1;
    D3D12_ROOT_PARAMETER Params[4];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[0].Constants.Num32BitValues = CONSTANTS;
    Params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    Params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_SRV;
    Params[1].Descriptor.ShaderRegister = 0;
    Params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
    Params[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_SRV;
    Params[2].Descriptor.ShaderRegister = 1;
    Params[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
    Params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[3].DescriptorTable.NumDescriptorRanges = 1;
    Params[3].DescriptorTable.pDescriptorRanges   = &Heap;
    Params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    // The skins as the geometry pass samples them: smooth, sixteen times, wrapping.
    D3D12_STATIC_SAMPLER_DESC Samp;
    appMemzero(&Samp, sizeof(Samp));
    Samp.Filter           = D3D12_FILTER_ANISOTROPIC;
    Samp.MaxAnisotropy    = 16;
    Samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samp.MaxLOD           = D3D12_FLOAT32_MAX;
    Samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    Samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 4;
    Desc.pParameters       = Params;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &Samp;
    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("mirror body: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    D3D_SHADER_MACRO Macros[2];
    Macros[0].Name       = "RDX_RT_NORMAL_STEPS";
    Macros[0].Definition = RDX_BODY_STRINGIFY(RDX_RT_NORMAL_STEPS);
    Macros[1].Name       = nullptr;
    Macros[1].Definition = nullptr;
    TComRef<ID3DBlob> Vs, Ps;
    const SIZE_T Len = sizeof(GMirrorBodyText) - 1;
    if (FAILED(RdxCompile(GMirrorBodyText, Len, "MirrorBody", Macros, nullptr, "VSBody", "vs_5_1",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GMirrorBodyText, Len, "MirrorBody", Macros, nullptr, "PSBody", "ps_5_1",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0,
                          Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("mirror body: the shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Root.Release();
        return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature        = Root.Get();
    Pso.VS.pShaderBytecode    = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength     = Vs->GetBufferSize();
    Pso.PS.pShaderBytecode    = Ps->GetBufferPointer();
    Pso.PS.BytecodeLength     = Ps->GetBufferSize();
    Pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets      = 1;
    Pso.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    Pso.SampleDesc.Count      = 1;
    Pso.SampleMask            = UINT_MAX;
    // Mirrored, a triangle's corners go round the other way: both sides are drawn.
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // Reversed depth (z/w = 1/W), as the scene: nearer is greater.
    Pso.DepthStencilState.DepthEnable    = TRUE;
    Pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    Pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER;
    if (FAILED(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), Pipeline.VoidSlot())))
    {
        RdxLog(TEXT("mirror body: the pipeline could not be made"));
        Root.Release();
        return 0;
    }
    RdxLog(TEXT("mirror body: made - the own body for the panes, where no rays run"));
    return 1;
}

void FMirrorBody::Shutdown()
{
    Pipeline.Release();
    Root.Release();
    Picture.Release();
    Depth.Release();
    RtvHeap.Release();
    DsvHeap.Release();
    PictureState = D3D12_RESOURCE_STATE_COMMON;
    Width = Height = 0;
}

UBOOL FMirrorBody::EnsureTargets(INT InWidth, INT InHeight, UBOOL& bMade)
{
    bMade = 0;
    if (!Device || InWidth <= 0 || InHeight <= 0)
        return 0;
    if (Picture && Depth && Width == InWidth && Height == InHeight)
        return 1;
    bMade = 1;
    Picture.Release();
    Depth.Release();
    if (!RtvHeap || !DsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC H;
        appMemzero(&H, sizeof(H));
        H.NumDescriptors = 1;
        H.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(Device->CreateDescriptorHeap(&H, __uuidof(ID3D12DescriptorHeap), RtvHeap.VoidSlot())))
            return 0;
        H.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(Device->CreateDescriptorHeap(&H, __uuidof(ID3D12DescriptorHeap), DsvHeap.VoidSlot())))
            return 0;
    }
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)InWidth;
    Desc.Height           = (UINT)InHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format = Desc.Format;
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               &Clear, __uuidof(ID3D12Resource), Picture.VoidSlot())))
    {
        RdxLog(TEXT("mirror body: no memory for its picture (%i x %i) - the panes show no body"), InWidth, InHeight);
        return 0;
    }
    PictureState  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Desc.Format   = DXGI_FORMAT_D32_FLOAT;
    Desc.Flags    = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    Clear.Format  = Desc.Format;
    Clear.DepthStencil.Depth = 0.0f;
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                               &Clear, __uuidof(ID3D12Resource), Depth.VoidSlot())))
    {
        Picture.Release();
        RdxLog(TEXT("mirror body: no memory for its depth (%i x %i) - the panes show no body"), InWidth, InHeight);
        return 0;
    }
    Device->CreateRenderTargetView(Picture.Get(), nullptr, RtvHeap->GetCPUDescriptorHandleForHeapStart());
    Device->CreateDepthStencilView(Depth.Get(), nullptr, DsvHeap->GetCPUDescriptorHandleForHeapStart());
    Width  = InWidth;
    Height = InHeight;
    return 1;
}

void FMirrorBody::WriteView(ID3D12Device* Device, ID3D12Resource* Picture, D3D12_CPU_DESCRIPTOR_HANDLE At)
{
    if (!Device)
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC Srv;
    appMemzero(&Srv, sizeof(Srv));
    Srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Srv.Texture2D.MipLevels     = 1;
    Device->CreateShaderResourceView(Picture, &Srv, At);
}

void FMirrorBody::Draw(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* TextureHeap, const FBody* Body,
                       const FLOAT* Plane, const FLOAT* Eye, const FLOAT* Axes, FLOAT TanX, FLOAT TanY,
                       const FLOAT* Jitter, FLOAT Boost)
{
    if (!Cmd || !Pipeline || !Picture || !Depth)
        return;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Picture.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = PictureState;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (PictureState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        Cmd->ResourceBarrier(1, &B);
    const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap->GetCPUDescriptorHandleForHeapStart();
    static const FLOAT Nothing[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Cmd->ClearRenderTargetView(Rtv, Nothing, 0, nullptr);
    Cmd->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    if (Body && Body->Count >= 3 && Body->Positions && Body->Places && Plane && Eye && Axes && Jitter && TextureHeap)
    {
        DWORD  C[CONSTANTS];
        FLOAT* const F = (FLOAT*)C;
        appMemzero(C, sizeof(C));
        for (INT a = 0; a < 3; a++)
            for (INT k = 0; k < 3; k++)
                F[a * 4 + k] = Axes[a * 3 + k];
        F[3]  = TanX;
        F[7]  = TanY;
        F[11] = Jitter[0];
        F[12] = Eye[0];
        F[13] = Eye[1];
        F[14] = Eye[2];
        F[15] = Jitter[1];
        for (INT k = 0; k < 4; k++)
            F[16 + k] = Plane[k];
        F[20] = Boost;
        C[21] = Body->bEngineLit ? 1u : 0u;
        C[22] = Body->Average;
        for (INT w = 0; w < 6; w++)
            C[24 + w] = Body->Cube[w];
        for (INT k = 0; k < 15; k++)
            C[32 + k] = Body->Skins[k];
        const D3D12_VIEWPORT View = { 0.0f, 0.0f, (FLOAT)Width, (FLOAT)Height, 0.0f, 1.0f };
        const D3D12_RECT     All  = { 0, 0, Width, Height };
        ID3D12DescriptorHeap* Heaps[1] = { TextureHeap };
        Cmd->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);
        Cmd->RSSetViewports(1, &View);
        Cmd->RSSetScissorRects(1, &All);
        Cmd->SetDescriptorHeaps(1, Heaps);
        Cmd->SetGraphicsRootSignature(Root.Get());
        Cmd->SetPipelineState(Pipeline.Get());
        Cmd->SetGraphicsRoot32BitConstants(0, CONSTANTS, C, 0);
        Cmd->SetGraphicsRootShaderResourceView(1, Body->Positions);
        Cmd->SetGraphicsRootShaderResourceView(2, Body->Places);
        Cmd->SetGraphicsRootDescriptorTable(3, TextureHeap->GetGPUDescriptorHandleForHeapStart());
        Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Cmd->DrawInstanced((UINT)Body->Count, 1, (UINT)Body->First, 0);
    }
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);
    PictureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
