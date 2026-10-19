#include "Pch.h"
#include "BeamBounce.h"
#include "Log.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// Keep: the mirror's polygons, in the world's view's space, drawn as that view sees them; each pixel takes the depth
// the mirror's view left there, and the mirror's number. Glare: the lamp's own image in the mirror - a disc and the
// glare about it - added over the mirror's pixels, where the image's place is the mirror's and nothing stands
// nearer than the image there.
static const char* GBeamBounceShader =
"cbuffer C : register(b0)\n"
"{\n"
"    float4 V0;    // the view's centre and its projection, in the map's pixels; the mirror's number\n"
"    float4 V1;    // the map's size; how deep the lamp's image lies; how much nearer than it hides it\n"
"    float4 V2;    // the lamp's image on the screen; its disc's radius and its glare's, in pixels\n"
"    float4 V3;    // the lamp's colour; how strongly it shines this way\n"
"};\n"
"Texture2D<float>  Depth : register(t0);\n"
"Texture2D<float2> Kept  : register(t1);\n"
"\n"
"float4 KeepVS(float3 P : POSITION) : SV_Position\n"
"{\n"
"    // The view's own projection, with the view depth for w: the card cuts what lies at the eye or behind it.\n"
"    return float4((V0.x * P.z + V0.z * P.x) * (2.0f / V1.x) - P.z,\n"
"                  P.z - (V0.y * P.z + V0.z * P.y) * (2.0f / V1.y), 0.5f * P.z, P.z);\n"
"}\n"
"\n"
"float2 KeepPS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    return float2(Depth.Load(int3(Pos.xy, 0)), V0.w);\n"
"}\n"
"\n"
"float4 GlareVS(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 T = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(T * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
"}\n"
"\n"
"float4 GlarePS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    if (abs(Kept.Load(int3(Pos.xy, 0)).y - V0.w) > 0.5f)\n"
"        discard;\n"
"    static const int2 Off[5] = { int2(0, 0), int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2) };\n"
"    float Seen = 0.0f;\n"
"    for (int i = 0; i < 5; i++)\n"
"    {\n"
"        const int2 At = int2(V2.xy) + Off[i];\n"
"        if (At.x < 0 || At.y < 0 || At.x >= (int)V1.x || At.y >= (int)V1.y)\n"
"            continue;\n"
"        const float2 K = Kept.Load(int3(At, 0));\n"
"        if (abs(K.y - V0.w) > 0.5f)\n"
"            continue;\n"
"        const float Z = (K.x > 1e-9f) ? 1.0f / K.x : 1e9f;\n"
"        Seen += 0.2f * (1.0f - smoothstep(V1.w, V1.w * 1.5f, V1.z - Z));\n"
"    }\n"
"    const float R    = length(Pos.xy - V2.xy);\n"
"    const float Core = 1.0f - smoothstep(V2.z, V2.z + 1.5f, R);\n"
"    const float Q    = R / max(V2.w, 1.0f);\n"
"    const float Halo = 1.0f / ((1.0f + Q * Q) * (1.0f + Q * Q));\n"
"    return float4(V3.rgb * (V3.w * Seen * max(Core, 0.85f * Halo)), 0.0f);\n"
"}\n"
"\n"
"// A lamp the world's view sees (LampGlare): the same disc and glare, seen where the world's own depth\n"
"// holds nothing nearer than the lamp about its place - five looks, as the mirror's.\n"
"float4 LampGlarePS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    static const int2 Off[5] = { int2(0, 0), int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2) };\n"
"    float Seen = 0.0f;\n"
"    for (int i = 0; i < 5; i++)\n"
"    {\n"
"        const int2 At = int2(V2.xy) + Off[i];\n"
"        if (At.x < 0 || At.y < 0 || At.x >= (int)V1.x || At.y >= (int)V1.y)\n"
"            continue;\n"
"        const float D = Depth.Load(int3(At, 0));\n"
"        const float Z = (D > 1e-9f) ? 1.0f / D : 1e9f;\n"
"        Seen += 0.2f * (1.0f - smoothstep(V1.w, V1.w * 1.5f, V1.z - Z));\n"
"    }\n"
"    const float R    = length(Pos.xy - V2.xy);\n"
"    const float Core = 1.0f - smoothstep(V2.z, V2.z + 1.5f, R);\n"
"    const float Q    = R / max(V2.w, 1.0f);\n"
"    const float Halo = 1.0f / ((1.0f + Q * Q) * (1.0f + Q * Q));\n"
"    return float4(V3.rgb * (V3.w * Seen * max(Core, 0.85f * Halo)), 0.0f);\n"
"}";

UBOOL FBeamBounce::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (IsReady())
        return 1;

    D3D12_DESCRIPTOR_RANGE R[2];
    appMemzero(R, sizeof(R));
    R[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    R[0].NumDescriptors     = 1;
    R[0].BaseShaderRegister = 0;        // t0: the depth a mirror's view left
    R[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    R[1].NumDescriptors     = 1;
    R[1].BaseShaderRegister = 1;        // t1: the map

    D3D12_ROOT_PARAMETER P[3];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 16;
    P[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    for (INT i = 0; i < 2; i++)
    {
        P[1 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        P[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        P[1 + i].DescriptorTable.pDescriptorRanges   = &R[i];
        P[1 + i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 3;
    Desc.pParameters   = P;
    Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("flashlight off mirrors: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    const size_t Len = strlen(GBeamBounceShader);
    if (FAILED(RdxCompile(GBeamBounceShader, Len, "BeamBounce", nullptr, nullptr, "KeepVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, KeepVs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GBeamBounceShader, Len, "BeamBounce", nullptr, nullptr, "KeepPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, KeepPs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GBeamBounceShader, Len, "BeamBounce", nullptr, nullptr, "GlareVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, GlareVs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GBeamBounceShader, Len, "BeamBounce", nullptr, nullptr, "GlarePS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, GlarePs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GBeamBounceShader, Len, "BeamBounce", nullptr, nullptr, "LampGlarePS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, LampGlarePs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("flashlight off mirrors: the shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = 3;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC Rd;
    appMemzero(&Rd, sizeof(Rd));
    Rd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Rd.NumDescriptors = 1;
    if (FAILED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot())) ||
        FAILED(Device->CreateDescriptorHeap(&Rd, __uuidof(ID3D12DescriptorHeap), RtvHeap.VoidSlot())))
    {
        RdxLog(TEXT("flashlight off mirrors: its view heaps could not be made"));
        Shutdown();
        return 0;
    }
    HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Views of nothing to begin with: a table always points at a view.
    D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
    appMemzero(&Sd, sizeof(Sd));
    Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Sd.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
    for (INT i = 0; i < 3; i++)
    {
        Sd.Format = (i < 2) ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32_FLOAT;
        Device->CreateShaderResourceView(nullptr, &Sd, At);
        At.ptr += HeapStep;
    }
    for (INT i = 0; i < FRAMES; i++)
        if (!HoldVertices(Device, i, FIRST_VERTICES))
        {
            Shutdown();
            return 0;
        }

    static const D3D12_INPUT_ELEMENT_DESC Layout[1] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC G;
    appMemzero(&G, sizeof(G));
    G.pRootSignature        = Root.Get();
    G.VS.pShaderBytecode    = KeepVs->GetBufferPointer();
    G.VS.BytecodeLength     = KeepVs->GetBufferSize();
    G.PS.pShaderBytecode    = KeepPs->GetBufferPointer();
    G.PS.BytecodeLength     = KeepPs->GetBufferSize();
    G.InputLayout.pInputElementDescs = Layout;
    G.InputLayout.NumElements        = 1;
    G.SampleMask            = UINT_MAX;
    G.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    G.NumRenderTargets      = 1;
    G.RTVFormats[0]         = DXGI_FORMAT_R32G32_FLOAT;
    G.SampleDesc.Count      = 1;
    G.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    G.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    G.RasterizerState.DepthClipEnable = TRUE;
    G.DepthStencilState.DepthEnable   = FALSE;
    G.DepthStencilState.StencilEnable = FALSE;
    G.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(Device->CreateGraphicsPipelineState(&G, __uuidof(ID3D12PipelineState), KeepPso.VoidSlot())))
    {
        RdxLog(TEXT("flashlight off mirrors: the map's pipeline could not be made"));
        Shutdown();
        return 0;
    }
    return 1;
}

UBOOL FBeamBounce::EnsureGlare(ID3D12Device* Device, DXGI_FORMAT SceneFormat)
{
    if (GlarePso && LampGlarePso && GlareFormat == SceneFormat)
        return 1;
    GlarePso.Release();
    LampGlarePso.Release();
    GlareFormat = DXGI_FORMAT_UNKNOWN;
    if (!Device || !Root || !GlareVs || !GlarePs || !LampGlarePs || SceneFormat == DXGI_FORMAT_UNKNOWN)
        return 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC G;
    appMemzero(&G, sizeof(G));
    G.pRootSignature        = Root.Get();
    G.VS.pShaderBytecode    = GlareVs->GetBufferPointer();
    G.VS.BytecodeLength     = GlareVs->GetBufferSize();
    G.PS.pShaderBytecode    = GlarePs->GetBufferPointer();
    G.PS.BytecodeLength     = GlarePs->GetBufferSize();
    G.SampleMask            = UINT_MAX;
    G.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    G.NumRenderTargets      = 1;
    G.RTVFormats[0]         = SceneFormat;
    G.SampleDesc.Count      = 1;
    G.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    G.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    G.RasterizerState.DepthClipEnable = FALSE;
    G.DepthStencilState.DepthEnable   = FALSE;
    G.DepthStencilState.StencilEnable = FALSE;
    // Light: added to what the mirror's view holds.
    D3D12_RENDER_TARGET_BLEND_DESC& B = G.BlendState.RenderTarget[0];
    B.BlendEnable           = TRUE;
    B.SrcBlend              = D3D12_BLEND_ONE;
    B.DestBlend             = D3D12_BLEND_ONE;
    B.BlendOp               = D3D12_BLEND_OP_ADD;
    B.SrcBlendAlpha         = D3D12_BLEND_ZERO;
    B.DestBlendAlpha        = D3D12_BLEND_ONE;
    B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                              D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(Device->CreateGraphicsPipelineState(&G, __uuidof(ID3D12PipelineState), GlarePso.VoidSlot())))
    {
        RdxLog(TEXT("flashlight off mirrors: the glare's pipeline could not be made"));
        return 0;
    }
    G.PS.pShaderBytecode = LampGlarePs->GetBufferPointer();
    G.PS.BytecodeLength  = LampGlarePs->GetBufferSize();
    if (FAILED(Device->CreateGraphicsPipelineState(&G, __uuidof(ID3D12PipelineState), LampGlarePso.VoidSlot())))
    {
        GlarePso.Release();
        RdxLog(TEXT("flashlight off mirrors: the lamps' glare pipeline could not be made"));
        return 0;
    }
    GlareFormat = SceneFormat;
    return 1;
}

INT FBeamBounce::DepthSlot(ID3D12Device* Device, ID3D12Resource* Depth, DXGI_FORMAT DepthRead)
{
    if (ViewsAt != RdxViewGeneration())
    {
        ViewsAt      = RdxViewGeneration();
        DepthSeen[0] = DepthSeen[1] = nullptr;
    }
    INT S = (DepthSeen[0] == Depth && DepthFormat[0] == DepthRead) ? 0
          : ((DepthSeen[1] == Depth && DepthFormat[1] == DepthRead) ? 1 : -1);
    if (S < 0)
    {
        S = !DepthSeen[0] ? 0 : 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DepthRead;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += S * (UINT64)HeapStep;
        Device->CreateShaderResourceView(Depth, &Sd, At);
        DepthSeen[S]   = Depth;
        DepthFormat[S] = DepthRead;
    }
    return S;
}

void FBeamBounce::LampGlare(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
                            D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv, ID3D12Resource* Depth, DXGI_FORMAT DepthRead,
                            const FLamp& Lamp, const FView& View)
{
    if (!Cmd || !Device || !IsReady() || !Depth || Lamp.Strength <= 0.0f || View.W <= 0 || View.H <= 0 ||
        !EnsureGlare(Device, SceneFormat))
        return;
    const FLOAT Reach = Max(Lamp.Halo * 6.0f, Lamp.Core + 2.0f);
    D3D12_RECT  Sc;
    Sc.left   = Max((INT)(Lamp.Centre[0] - Reach), 0);
    Sc.top    = Max((INT)(Lamp.Centre[1] - Reach), 0);
    Sc.right  = Min((INT)(Lamp.Centre[0] + Reach) + 1, View.W);
    Sc.bottom = Min((INT)(Lamp.Centre[1] + Reach) + 1, View.H);
    if (Sc.right <= Sc.left || Sc.bottom <= Sc.top)
        return;
    const INT S = DepthSlot(Device, Depth, DepthRead);

    UINT   C[16];
    FLOAT* F = (FLOAT*)C;
    appMemzero(C, sizeof(C));
    F[0] = View.Screen[0];   F[1] = View.Screen[1];   F[2] = View.Focal;  F[3] = 0.0f;
    F[4] = (FLOAT)View.W;    F[5] = (FLOAT)View.H;    F[6] = Lamp.Depth;  F[7] = 12.0f;
    F[8] = Lamp.Centre[0];   F[9] = Lamp.Centre[1];   F[10] = Lamp.Core;  F[11] = Lamp.Halo;
    F[12] = Lamp.Colour[0];  F[13] = Lamp.Colour[1];  F[14] = Lamp.Colour[2]; F[15] = Lamp.Strength;

    Cmd->OMSetRenderTargets(1, &SceneRtv, FALSE, nullptr);
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)View.W, (FLOAT)View.H, 0.0f, 1.0f };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetPipelineState(LampGlarePso.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 16, C, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE Table = Heap->GetGPUDescriptorHandleForHeapStart();
    Table.ptr += (UINT64)S * HeapStep;
    Cmd->SetGraphicsRootDescriptorTable(1, Table);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}

UBOOL FBeamBounce::HoldVertices(ID3D12Device* Device, INT Slot, INT Wanted)
{
    if (Wanted <= VerticesRoom[Slot])
        return 1;
    if (!Device)
        return 0;
    const INT Larger = Max(Wanted, VerticesRoom[Slot] * 2);
    D3D12_HEAP_PROPERTIES Up;
    appMemzero(&Up, sizeof(Up));
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = (UINT64)Larger * 3 * sizeof(FLOAT);
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* Made   = nullptr;
    void*           MadeAt = nullptr;
    D3D12_RANGE     None   = { 0, 0 };
    if (RdxForcedFailure(TEXT("mirror vertices")) ||
        FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), (void**)&Made)) ||
        FAILED(Made->Map(0, &None, &MadeAt)) || !MadeAt)
    {
        if (Made)
            Made->Release();
        RdxLog(TEXT("flashlight off mirrors: the mirrors' vertices could not be held for %i (it holds %i)"), Larger,
               VerticesRoom[Slot]);
        return 0;
    }
    // The shorter one is let go: the slot's last frame is done on the card by the time a frame begins in it.
    if (Vertices[Slot])
        Vertices[Slot]->Unmap(0, nullptr);
    Vertices[Slot].Release();
    *Vertices[Slot].Slot() = Made;
    VerticesMapped[Slot] = MadeAt;
    VerticesRoom[Slot]   = Larger;
    return 1;
}

void FBeamBounce::Shutdown()
{
    ReleaseTarget();
    for (INT i = 0; i < FRAMES; i++)
    {
        if (Vertices[i] && VerticesMapped[i])
            Vertices[i]->Unmap(0, nullptr);
        VerticesMapped[i] = nullptr;
        Vertices[i].Release();
        VerticesRoom[i] = VerticesUsed[i] = VerticesWant[i] = 0;
    }
    GlarePso.Release();
    LampGlarePso.Release();
    GlareFormat = DXGI_FORMAT_UNKNOWN;
    KeepPso.Release();
    KeepVs.Release();
    KeepPs.Release();
    GlareVs.Release();
    GlarePs.Release();
    LampGlarePs.Release();
    Heap.Release();
    RtvHeap.Release();
    Root.Release();
    DepthSeen[0] = DepthSeen[1] = nullptr;
}

void FBeamBounce::ReleaseTarget()
{
    Map.Release();
    MapW = MapH = 0;
}

UBOOL FBeamBounce::EnsureTarget(ID3D12Device* Device, INT W, INT H)
{
    if (!Device || !IsReady() || W <= 1 || H <= 1)
        return 0;
    if (Map && MapW == W && MapH == H)
        return 1;
    ReleaseTarget();

    D3D12_HEAP_PROPERTIES Hp;
    appMemzero(&Hp, sizeof(Hp));
    Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Td;
    appMemzero(&Td, sizeof(Td));
    Td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Td.Width            = (UINT64)W;
    Td.Height           = (UINT)H;
    Td.DepthOrArraySize = 1;
    Td.MipLevels        = 1;
    Td.Format           = DXGI_FORMAT_R32G32_FLOAT;
    Td.SampleDesc.Count = 1;
    Td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE Cv;
    appMemzero(&Cv, sizeof(Cv));
    Cv.Format = DXGI_FORMAT_R32G32_FLOAT;
    if (RdxForcedFailure(TEXT("mirror map")) ||
        FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Cv,
                                               __uuidof(ID3D12Resource), Map.VoidSlot())))
    {
        RdxLog(TEXT("flashlight off mirrors: the %ix%i map could not be made - no light off the mirrors"), W, H);
        ReleaseTarget();
        return 0;
    }
    Device->CreateRenderTargetView(Map.Get(), nullptr, RtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
    appMemzero(&Sd, sizeof(Sd));
    Sd.Format                  = DXGI_FORMAT_R32G32_FLOAT;
    Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Sd.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
    At.ptr += 2 * (UINT64)HeapStep;
    Device->CreateShaderResourceView(Map.Get(), &Sd, At);
    MapW = W;
    MapH = H;
    MadeCount++;
    RdxLog(TEXT("flashlight off mirrors: the map of the mirrors' views' depth is %ix%i"), W, H);
    return 1;
}

void FBeamBounce::Turn(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To)
{
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Map.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = From;
    B.Transition.StateAfter  = To;
    Cmd->ResourceBarrier(1, &B);
}

void FBeamBounce::Begin(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, INT FrameIndex)
{
    if (!Cmd || !Map)
        return;
    const INT Slot = ((FrameIndex % FRAMES) + FRAMES) % FRAMES;
    // What the slot's frame before asked for and had no room for: held from here on.
    if (VerticesWant[Slot] > VerticesRoom[Slot])
        HoldVertices(Device, Slot, VerticesWant[Slot]);
    VerticesUsed[Slot] = 0;
    VerticesWant[Slot] = 0;
    const FLOAT Nothing[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Turn(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Cmd->ClearRenderTargetView(RtvHeap->GetCPUDescriptorHandleForHeapStart(), Nothing, 0, nullptr);
    Turn(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

UBOOL FBeamBounce::BeginDraw(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !Map)
        return 0;
    Turn(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)MapW, (FLOAT)MapH, 0.0f, 1.0f };
    const D3D12_RECT     Sc = { 0, 0, MapW, MapH };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    return 1;
}

void FBeamBounce::EndDraw(ID3D12GraphicsCommandList* Cmd)
{
    if (Cmd && Map)
        Turn(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

UBOOL FBeamBounce::Keep(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Depth,
                        DXGI_FORMAT DepthRead, INT FrameIndex, const FLOAT* Xyz, INT Count, INT Number,
                        const FView& View)
{
    if (!Cmd || !Device || !IsReady() || !Map || !Depth || !Xyz || Count < 3)
        return 0;
    const INT Slot = ((FrameIndex % FRAMES) + FRAMES) % FRAMES;
    // No room made in the middle of a frame - the frame's earlier mirrors are drawn from the buffer as it is: this
    // mirror goes without for the frame, and the slot holds enough from its next one on.
    VerticesWant[Slot] = Max(VerticesWant[Slot], VerticesUsed[Slot] + Count);
    if (VerticesUsed[Slot] + Count > VerticesRoom[Slot])
        return 0;
    // The depth's view: one for each of the two depth buffers, written when a buffer is first seen or made again.
    const INT S = DepthSlot(Device, Depth, DepthRead);
    appMemcpy((FLOAT*)VerticesMapped[Slot] + VerticesUsed[Slot] * 3, Xyz, (SIZE_T)Count * 3 * sizeof(FLOAT));

    UINT   C[16];
    FLOAT* F = (FLOAT*)C;
    appMemzero(C, sizeof(C));
    F[0] = View.Screen[0]; F[1] = View.Screen[1]; F[2] = View.Focal; F[3] = (FLOAT)Number;
    F[4] = (FLOAT)MapW;    F[5] = (FLOAT)MapH;

    Turn(Cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)MapW, (FLOAT)MapH, 0.0f, 1.0f };
    const D3D12_RECT     Sc = { 0, 0, MapW, MapH };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetPipelineState(KeepPso.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 16, C, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE Table = Heap->GetGPUDescriptorHandleForHeapStart();
    Table.ptr += S * (UINT64)HeapStep;
    Cmd->SetGraphicsRootDescriptorTable(1, Table);
    D3D12_VERTEX_BUFFER_VIEW Vb;
    Vb.BufferLocation = Vertices[Slot]->GetGPUVirtualAddress() + (UINT64)VerticesUsed[Slot] * 3 * sizeof(FLOAT);
    Vb.SizeInBytes    = (UINT)(Count * 3 * sizeof(FLOAT));
    Vb.StrideInBytes  = 3 * sizeof(FLOAT);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->IASetVertexBuffers(0, 1, &Vb);
    Cmd->DrawInstanced((UINT)Count, 1, 0, 0);
    Turn(Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    VerticesUsed[Slot] += Count;
    return 1;
}

void FBeamBounce::Glare(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
                        D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv, INT Number, const FLamp& Lamp, const FView& View)
{
    if (!Cmd || !Device || !IsReady() || !Map || Lamp.Strength <= 0.0f || !EnsureGlare(Device, SceneFormat))
        return;
    // As far out as the glare still adds a step of the picture's.
    const FLOAT Reach = Max(Lamp.Halo * 6.0f, Lamp.Core + 2.0f);
    D3D12_RECT  Sc;
    Sc.left   = Max((INT)(Lamp.Centre[0] - Reach), 0);
    Sc.top    = Max((INT)(Lamp.Centre[1] - Reach), 0);
    Sc.right  = Min((INT)(Lamp.Centre[0] + Reach) + 1, MapW);
    Sc.bottom = Min((INT)(Lamp.Centre[1] + Reach) + 1, MapH);
    if (Sc.right <= Sc.left || Sc.bottom <= Sc.top)
        return;

    UINT   C[16];
    FLOAT* F = (FLOAT*)C;
    appMemzero(C, sizeof(C));
    F[0] = View.Screen[0];   F[1] = View.Screen[1];   F[2] = View.Focal;  F[3] = (FLOAT)Number;
    F[4] = (FLOAT)MapW;      F[5] = (FLOAT)MapH;      F[6] = Lamp.Depth;  F[7] = 12.0f;
    F[8] = Lamp.Centre[0];   F[9] = Lamp.Centre[1];   F[10] = Lamp.Core;  F[11] = Lamp.Halo;
    F[12] = Lamp.Colour[0];  F[13] = Lamp.Colour[1];  F[14] = Lamp.Colour[2]; F[15] = Lamp.Strength;

    Cmd->OMSetRenderTargets(1, &SceneRtv, FALSE, nullptr);
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)MapW, (FLOAT)MapH, 0.0f, 1.0f };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetPipelineState(GlarePso.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 16, C, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE Table = Heap->GetGPUDescriptorHandleForHeapStart();
    Table.ptr += 2 * (UINT64)HeapStep;
    Cmd->SetGraphicsRootDescriptorTable(2, Table);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
}
