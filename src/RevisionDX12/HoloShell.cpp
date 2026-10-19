#include "Pch.h"
#include "HoloShell.h"
#include "Log.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// The solid pass's own vertex shader for these triangles, and its colour for a mesh no light map lights: the texture
// times the engine's vertex light times the lamps' strength. The depth's shader has the texture's holes alone.
static const char* GHoloShellShader =
"cbuffer Frame : register(b0)\n"
"{\n"
"    float2 InvHalfSize;   // two over the engine's width and height\n"
"    float2 Jitter;        // the world's jitter, in clip units\n"
"    float  LightBoost;    // the lamps' strength, as the solid pass lays it on every mesh\n"
"    float  MipBias;       // the world's textures' mip bias\n"
"    float  NoSmooth;      // the run's texture unfiltered\n"
"    float  Layers;        // how many of the game's translucent layers the surface is laid as\n"
"};\n"
"Texture2D    Tex       : register(t0);\n"
"SamplerState Samp      : register(s0);\n"
"SamplerState SampPoint : register(s1);\n"
"struct VSIn  { float3 Pos : POSITION; float2 Uv : TEXCOORD0; float3 Colour : COLOR; };\n"
"struct VSOut { float4 Pos : SV_Position; float2 Uv : TEXCOORD0; float3 Colour : COLOR; };\n"
"\n"
"// The corners come projected to the engine's pixels, with one over the view's depth: the divide is undone and done\n"
"// again, so the texture runs across the triangle as it does in the solid pass - and the depth is the same number.\n"
"VSOut ShellVS(VSIn In)\n"
"{\n"
"    VSOut Out;\n"
"    const float W = 1.0f / max(In.Pos.z, 1e-6f);\n"
"    Out.Pos.x  = (In.Pos.x * InvHalfSize.x - 1.0f + Jitter.x) * W;\n"
"    Out.Pos.y  = (1.0f - In.Pos.y * InvHalfSize.y + Jitter.y) * W;\n"
"    Out.Pos.z  = 1.0f;\n"
"    Out.Pos.w  = W;\n"
"    Out.Uv     = In.Uv;\n"
"    Out.Colour = In.Colour;\n"
"    return Out;\n"
"}\n"
"\n"
"float4 Texel(VSOut In)\n"
"{\n"
"    if (NoSmooth > 0.5f)\n"
"        return Tex.Sample(SampPoint, In.Uv);\n"
"    return Tex.SampleBias(Samp, In.Uv, MipBias);\n"
"}\n"
"\n"
"// The depth alone, with the holes a masked texture has.\n"
"void ShellDepthPS(VSOut In)\n"
"{\n"
"    clip(Texel(In).a - 0.5f);\n"
"}\n"
"\n"
"float4 ShellPS(VSOut In) : SV_Target\n"
"{\n"
"    const float4 T = Texel(In);\n"
"    clip(T.a - 0.5f);\n"
"    const float3 One = T.rgb * In.Colour * LightBoost;\n"
"    // Layers of it laid one over the other, each as the blend lays one: what is left of the target is one less\n"
"    // the layer, that many times over.\n"
"    if (abs(Layers - 1.0f) < 1e-3f)\n"
"        return float4(One, 1.0f);\n"
"    return float4(1.0f - pow(saturate(1.0f - One), Layers), 1.0f);\n"
"}\n";

UBOOL FHoloShell::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (IsReady())
        return 1;

    D3D12_DESCRIPTOR_RANGE R;
    appMemzero(&R, sizeof(R));
    R.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    R.NumDescriptors     = 1;
    R.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER P[2];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 8;
    P[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[1].DescriptorTable.NumDescriptorRanges = 1;
    P[1].DescriptorTable.pDescriptorRanges   = &R;
    P[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // The solid pass's two samplers: the textures as it reads them.
    D3D12_STATIC_SAMPLER_DESC S[2];
    appMemzero(S, sizeof(S));
    S[0].Filter           = D3D12_FILTER_ANISOTROPIC;
    S[0].MaxAnisotropy    = 16;
    S[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    S[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    S[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    S[0].MaxLOD           = D3D12_FLOAT32_MAX;
    S[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    S[0].ShaderRegister   = 0;
    S[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    S[1]                  = S[0];
    S[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    S[1].ShaderRegister   = 1;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 2;
    Desc.pParameters       = P;
    Desc.NumStaticSamplers = 2;
    Desc.pStaticSamplers   = S;
    Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("hologram shell: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Shutdown();
        return 0;
    }

    const size_t Len = strlen(GHoloShellShader);
    if (FAILED(RdxCompile(GHoloShellShader, Len, "HoloShell", nullptr, nullptr, "ShellVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GHoloShellShader, Len, "HoloShell", nullptr, nullptr, "ShellDepthPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, PsDepth.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GHoloShellShader, Len, "HoloShell", nullptr, nullptr, "ShellPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, PsColour.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("hologram shell: the shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        Shutdown();
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    Hd.NumDescriptors = 1;
    UBOOL bMade = !RdxForcedFailure(TEXT("hologram shell heap")) &&
                  SUCCEEDED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), DsvHeap.VoidSlot()));
    for (INT i = 0; i < FRAMES && bMade; i++)
        bMade = HoldCorners(Device, i, FIRST_CORNERS);
    if (!bMade)
    {
        RdxLog(TEXT("hologram shell: its depth view's heap or its lists of corners could not be made"));
        Shutdown();
        return 0;
    }
    return 1;
}

// A frame slot's list made long enough for Wanted corners, twice its room at least. False where the device refuses;
// the list is then as it was.
UBOOL FHoloShell::HoldCorners(ID3D12Device* Device, INT Slot, INT Wanted)
{
    if (Wanted <= Room[Slot])
        return 1;
    if (!Device)
        return 0;
    const INT Larger = Max(Wanted, Room[Slot] * 2);
    D3D12_HEAP_PROPERTIES Up;
    appMemzero(&Up, sizeof(Up));
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = (UINT64)Larger * sizeof(FVertex);
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    TComRef<ID3D12Resource> Made;
    void*                   MadeAt = nullptr;
    D3D12_RANGE             None   = { 0, 0 };
    if (RdxForcedFailure(TEXT("hologram shell corners")) ||
        FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), Made.VoidSlot())) ||
        FAILED(Made->Map(0, &None, &MadeAt)) || !MadeAt)
    {
        RdxLog(TEXT("hologram shell: the list of corners could not be made for %i (it holds %i)"), Larger, Room[Slot]);
        return 0;
    }
    // The shorter one is let go: the slot's last frame is done on the card by the time a frame records into it.
    if (Upload[Slot] && Mapped[Slot])
        Upload[Slot]->Unmap(0, nullptr);
    Upload[Slot].Release();
    *Upload[Slot].Slot() = Made.Get();
    Made->AddRef();
    Mapped[Slot] = MadeAt;
    Room[Slot]   = Larger;
    return 1;
}

UBOOL FHoloShell::EnsurePso(ID3D12Device* Device, DXGI_FORMAT SceneFormat, DXGI_FORMAT DsvFormat)
{
    if (PsoDepth && PsoColour && PsoScene == SceneFormat && PsoDsv == DsvFormat)
        return 1;
    PsoDepth.Release();
    PsoColour.Release();
    PsoScene = PsoDsv = DXGI_FORMAT_UNKNOWN;
    if (!Device || !Root || !Vs || !PsDepth || !PsColour || SceneFormat == DXGI_FORMAT_UNKNOWN ||
        DsvFormat == DXGI_FORMAT_UNKNOWN)
        return 0;
    D3D12_INPUT_ELEMENT_DESC Elements[3];
    appMemzero(Elements, sizeof(Elements));
    Elements[0].SemanticName      = "POSITION";
    Elements[0].Format            = DXGI_FORMAT_R32G32B32_FLOAT;
    Elements[0].AlignedByteOffset = 0;
    Elements[1].SemanticName      = "TEXCOORD";
    Elements[1].Format            = DXGI_FORMAT_R32G32_FLOAT;
    Elements[1].AlignedByteOffset = 12;
    Elements[2].SemanticName      = "COLOR";
    Elements[2].Format            = DXGI_FORMAT_R32G32B32_FLOAT;
    Elements[2].AlignedByteOffset = 20;
    for (INT i = 0; i < 3; i++)
        Elements[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC G;
    appMemzero(&G, sizeof(G));
    G.pRootSignature        = Root.Get();
    G.VS.pShaderBytecode    = Vs->GetBufferPointer();
    G.VS.BytecodeLength     = Vs->GetBufferSize();
    G.InputLayout.pInputElementDescs = Elements;
    G.InputLayout.NumElements        = 3;
    G.SampleMask            = UINT_MAX;
    G.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    G.SampleDesc.Count      = 1;
    G.DSVFormat             = DsvFormat;
    G.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    G.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    G.RasterizerState.DepthClipEnable = TRUE;
    G.DepthStencilState.StencilEnable = FALSE;
    G.DepthStencilState.DepthEnable   = TRUE;

    // The depth alone: nearer wins, as in the solid pass (the depth is reversed), and is kept.
    G.PS.pShaderBytecode               = PsDepth->GetBufferPointer();
    G.PS.BytecodeLength                = PsDepth->GetBufferSize();
    G.NumRenderTargets                 = 0;
    G.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    G.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    if (FAILED(Device->CreateGraphicsPipelineState(&G, __uuidof(ID3D12PipelineState), PsoDepth.VoidSlot())))
    {
        RdxLog(TEXT("hologram shell: the depth's pipeline could not be made"));
        return 0;
    }

    // The colour: only where the triangle is the one the depth kept, laid as the engine lays a translucent surface -
    // its light + target * (1 - its light).
    G.PS.pShaderBytecode               = PsColour->GetBufferPointer();
    G.PS.BytecodeLength                = PsColour->GetBufferSize();
    G.NumRenderTargets                 = 1;
    G.RTVFormats[0]                    = SceneFormat;
    G.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_EQUAL;
    G.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    D3D12_RENDER_TARGET_BLEND_DESC& B = G.BlendState.RenderTarget[0];
    B.BlendEnable           = TRUE;
    B.SrcBlend              = D3D12_BLEND_ONE;
    B.DestBlend             = D3D12_BLEND_INV_SRC_COLOR;
    B.BlendOp               = D3D12_BLEND_OP_ADD;
    B.SrcBlendAlpha         = D3D12_BLEND_ZERO;
    B.DestBlendAlpha        = D3D12_BLEND_ONE;
    B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                              D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(Device->CreateGraphicsPipelineState(&G, __uuidof(ID3D12PipelineState), PsoColour.VoidSlot())))
    {
        RdxLog(TEXT("hologram shell: the colour's pipeline could not be made"));
        PsoDepth.Release();
        return 0;
    }
    PsoScene = SceneFormat;
    PsoDsv   = DsvFormat;
    return 1;
}

// A depth target of the world's depth copy's own kind and size, with a view of it. Kept in the state a copy goes
// into, between frames.
UBOOL FHoloShell::EnsureDepth(ID3D12Device* Device, ID3D12Resource* Like, DXGI_FORMAT DsvFormat)
{
    if (!Device || !Like || !DsvHeap)
        return 0;
    const D3D12_RESOURCE_DESC Desc = Like->GetDesc();
    if (Depth && DepthWidth == Desc.Width && DepthHeight == Desc.Height && DepthFormat == Desc.Format &&
        DepthDsv == DsvFormat)
        return 1;
    Depth.Release();
    D3D12_HEAP_PROPERTIES Card;
    appMemzero(&Card, sizeof(Card));
    Card.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (RdxForcedFailure(TEXT("hologram shell depth")) ||
        FAILED(Device->CreateCommittedResource(&Card, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, __uuidof(ID3D12Resource), Depth.VoidSlot())))
    {
        RdxLog(TEXT("hologram shell: its depth target could not be made (%ix%i)"), (INT)Desc.Width, (INT)Desc.Height);
        Depth.Release();
        return 0;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC Dv;
    appMemzero(&Dv, sizeof(Dv));
    Dv.Format        = DsvFormat;
    Dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    Device->CreateDepthStencilView(Depth.Get(), &Dv, DsvHeap->GetCPUDescriptorHandleForHeapStart());
    DepthWidth  = Desc.Width;
    DepthHeight = Desc.Height;
    DepthFormat = Desc.Format;
    DepthDsv    = DsvFormat;
    return 1;
}

void FHoloShell::Shutdown()
{
    for (INT i = 0; i < FRAMES; i++)
    {
        if (Upload[i] && Mapped[i])
            Upload[i]->Unmap(0, nullptr);
        Mapped[i] = nullptr;
        Upload[i].Release();
        Room[i] = 0;
    }
    Depth.Release();
    DepthWidth  = 0;
    DepthHeight = 0;
    DepthFormat = DepthDsv = DXGI_FORMAT_UNKNOWN;
    PsoDepth.Release();
    PsoColour.Release();
    PsoScene = PsoDsv = DXGI_FORMAT_UNKNOWN;
    Vs.Release();
    PsDepth.Release();
    PsColour.Release();
    DsvHeap.Release();
    Root.Release();
}

UBOOL FHoloShell::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
                         D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv, ID3D12Resource* WorldDepth, DXGI_FORMAT DsvFormat,
                         ID3D12DescriptorHeap* TextureHeap, const FFrame& Frame, const FVertex* Verts, INT VertCount,
                         const FRun* Runs, INT RunCount, INT FrameIndex)
{
    if (!Cmd || !Device || !IsReady() || !WorldDepth || !TextureHeap || !Verts || VertCount < 3 || !Runs ||
        RunCount <= 0)
        return 0;
    const INT Slot = ((FrameIndex % FRAMES) + FRAMES) % FRAMES;
    if (!EnsurePso(Device, SceneFormat, DsvFormat) || !EnsureDepth(Device, WorldDepth, DsvFormat) ||
        !HoldCorners(Device, Slot, VertCount))
        return 0;
    appMemcpy(Mapped[Slot], Verts, (SIZE_T)VertCount * sizeof(FVertex));

    // The world's depth into the pass's own target, which then takes the triangles' depth.
    Cmd->CopyResource(Depth.Get(), WorldDepth);
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Depth.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    Cmd->ResourceBarrier(1, &B);

    FLOAT C[8];
    appMemzero(C, sizeof(C));
    C[0] = Frame.InvHalf[0]; C[1] = Frame.InvHalf[1]; C[2] = Frame.Jitter[0]; C[3] = Frame.Jitter[1];
    C[4] = Frame.LightBoost; C[5] = Frame.MipBias; C[7] = Frame.Layers;

    D3D12_VERTEX_BUFFER_VIEW View;
    View.BufferLocation = Upload[Slot]->GetGPUVirtualAddress();
    View.SizeInBytes    = (UINT)VertCount * sizeof(FVertex);
    View.StrideInBytes  = sizeof(FVertex);

    ID3D12DescriptorHeap* Heaps[1] = { TextureHeap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 8, C, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->IASetVertexBuffers(0, 1, &View);

    const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (INT Go = 0; Go < 2; Go++)
    {
        // First the depth alone, then the colour over the scene with that depth to be held against.
        if (Go == 0)
            Cmd->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
        else
            Cmd->OMSetRenderTargets(1, &SceneRtv, FALSE, &Dsv);
        Cmd->SetPipelineState(Go ? PsoColour.Get() : PsoDepth.Get());
        for (INT r = 0; r < RunCount; r++)
        {
            const FRun& R = Runs[r];
            if (R.Count < 3 || R.First < 0 || R.First + R.Count > VertCount)
                continue;
            const FLOAT NoSmooth = R.bNoSmooth ? 1.0f : 0.0f;
            Cmd->SetGraphicsRoot32BitConstants(0, 1, &NoSmooth, 6);
            Cmd->SetGraphicsRootDescriptorTable(1, R.Texture);
            Cmd->DrawInstanced((UINT)R.Count, 1, (UINT)R.First, 0);
        }
    }

    B.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    Cmd->ResourceBarrier(1, &B);
    return 1;
}
