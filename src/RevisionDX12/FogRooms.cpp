#include "Pch.h"
#include "FogRooms.h"
#include "Log.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// One triangle over the whole target; for each pixel the point it shows, its zone, and the fog of every light whose
// zone the point or the eye is in. The constants' integers ride in float places, read with asuint.
static const char* GFogRoomsShader =
"cbuffer View : register(b0)\n"
"{\n"
"    float4 V0;    // the engine's screen centre, its projection, the eye's zone\n"
"    float4 V1;    // engine pixels to one of the target's, the number of lights, the number of nodes\n"
"    float4 V2;    // the eye in the world; in the w's from here to V5 a mirror's plane, in the view's space\n"
"    float4 V3;    // the view's X axis in the world\n"
"    float4 V4;    // its Y\n"
"    float4 V5;    // its Z\n"
"};\n"
"struct FLight { float4 PosRadius; float4 ColourAlpha; uint4 Zone; };\n"
"// Every light of the frame's fog, V1.z of them: a list as long as the frame needs.\n"
"StructuredBuffer<FLight> L : register(t3);\n"
"Texture2D<float> Depth : register(t0);\n"
"Texture2D<float> Water : register(t2);    // the see-through water's own depth; 0 where there is none\n"
"struct FNode { float4 Plane; int Back; int Front; uint Zones; uint Pad; };\n"
"StructuredBuffer<FNode> Nodes : register(t1);\n"
"\n"
"float4 FogVS(uint Id : SV_VertexID) : SV_Position\n"
"{\n"
"    float2 T = float2((Id << 1) & 2, Id & 2);\n"
"    return float4(T * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
"}\n"
"\n"
"// UModel::PointRegion: down the tree, in front where the plane says so, to the node with no child on that side.\n"
"uint ZoneAt(float3 P, uint Count)\n"
"{\n"
"    int  I = 0, Parent = 0;\n"
"    uint Front = 0;\n"
"    [loop] for (uint K = 0; K < 1024 && I >= 0 && (uint)I < Count; K++)\n"
"    {\n"
"        Parent = I;\n"
"        FNode N = Nodes[I];\n"
"        Front = (dot(N.Plane.xyz, P) - N.Plane.w >= 0.0f) ? 1u : 0u;\n"
"        I = Front ? N.Front : N.Back;\n"
"    }\n"
"    uint Z = Nodes[Parent].Zones;\n"
"    return Front ? ((Z >> 8) & 255u) : (Z & 255u);\n"
"}\n"
"\n"
"// The integral of 1 - r^2/R^2 along the ray from the eye, over the part of it inside the sphere, From on and before\n"
"// the point, divided by R: what the engine's fog of the light is a multiple of.\n"
"float Tau(float3 E, float3 D, float From, float Len, float3 C, float R)\n"
"{\n"
"    float3 EC = C - E;\n"
"    float  t0 = dot(EC, D);\n"
"    float  h2 = dot(EC, EC) - t0 * t0;\n"
"    float  R2 = R * R;\n"
"    if (h2 >= R2)\n"
"        return 0.0f;\n"
"    float w = sqrt(R2 - h2);\n"
"    float a = max(t0 - w, From), b = min(t0 + w, Len);\n"
"    if (b <= a)\n"
"        return 0.0f;\n"
"    float da = a - t0, db = b - t0;\n"
"    return ((b - a) * (1.0f - h2 / R2) - (db * db * db - da * da * da) / (3.0f * R2)) / R;\n"
"}\n"
"\n"
"float4 FogPS(float4 Pos : SV_Position) : SV_Target\n"
"{\n"
"    // The nearer of the world and the water's surface: the fog stops at the water, not at the bed under it.\n"
"    float d = max(Depth.Load(int3(Pos.xy, 0)), Water.Load(int3(Pos.xy, 0)));\n"
"    // Nothing drawn here: the sky. The fog of the zone the eye is in lies before it as well, as far as that zone's\n"
"    // lights reach - a point far off along the pixel's ray stands in for the surface the fog would lie on.\n"
"    bool bSky = d <= 1e-6f;\n"
"    d = max(d, 1e-6f);\n"
"    float2 Eng   = Pos.xy * V1.xy;                    // Pos.xy is already the pixel's centre\n"
"    float3 View  = float3((Eng - V0.xy) / max(V0.z, 1e-3f), 1.0f) / d;\n"
"    float3 World = V2.xyz + V3.xyz * View.x + V4.xyz * View.y + V5.xyz * View.z;\n"
"    float3 Ray   = World - V2.xyz;\n"
"    float  Len   = length(Ray);\n"
"    if (Len < 1.0f)\n"
"        return 0.0f;\n"
"    float3 Dir = Ray / Len;\n"
"    // The zone of the surface the pixel shows: a step back towards the eye, off the surface's own plane.\n"
"    uint Zone = ZoneAt(World - Dir * 2.0f, asuint(V1.w));\n"
"    uint Eye  = asuint(V0.w);\n"
"    // A mirror's view shows the world from behind the mirror (its plane: the w's of V2 to V5; none where its normal\n"
"    // is nought): the fog from the mirror on, by the zone the way comes out into there.\n"
"    float4 Mirror = float4(V2.w, V3.w, V4.w, V5.w);\n"
"    float  From   = 0.0f;\n"
"    if (dot(Mirror.xyz, Mirror.xyz) > 0.25f)\n"
"    {\n"
"        float Along = dot(Mirror.xyz, View) / Len;\n"
"        From = (abs(Along) > 1e-5f) ? clamp(Mirror.w / Along, 0.0f, Len) : 0.0f;\n"
"        Eye  = ZoneAt(V2.xyz + Dir * min(From + 2.0f, Len), asuint(V1.w));\n"
"    }\n"
"    // The sky is in no zone of the level: the far point's own would be anybody's.\n"
"    if (bSky)\n"
"        Zone = Eye;\n"
"    float3 C = 0.0f;\n"
"    float  A = 0.0f;\n"
"    uint   N = asuint(V1.z);\n"
"    [loop] for (uint i = 0; i < N; i++)\n"
"    {\n"
"        if (L[i].Zone.x != Zone && L[i].Zone.x != Eye)\n"
"            continue;\n"
"        float t = Tau(V2.xyz, Dir, From, Len, L[i].PosRadius.xyz, L[i].PosRadius.w);\n"
"        C += L[i].ColourAlpha.rgb * t;\n"
"        A += L[i].ColourAlpha.a * t;\n"
"    }\n"
"    return float4(saturate(C), saturate(A));\n"
"}\n";

UBOOL FFogRooms::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (IsReady())
        return 1;

    D3D12_DESCRIPTOR_RANGE R;
    appMemzero(&R, sizeof(R));
    R.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    R.NumDescriptors     = 3;
    R.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER P[3];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 24;
    P[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    P[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_SRV;
    P[1].Descriptor.ShaderRegister = 3;             // t3: the lights, straight from the root signature
    P[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    P[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[2].DescriptorTable.NumDescriptorRanges = 1;
    P[2].DescriptorTable.pDescriptorRanges   = &R;
    P[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 3;
    Desc.pParameters   = P;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("fog rooms: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    const size_t Len = strlen(GFogRoomsShader);
    if (FAILED(RdxCompile(GFogRoomsShader, Len, "FogRooms", nullptr, nullptr, "FogVS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GFogRoomsShader, Len, "FogRooms", nullptr, nullptr, "FogPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("fog rooms: the shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = 3 * VIEWS;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_HEAP_PROPERTIES Up;
    appMemzero(&Up, sizeof(Up));
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = (UINT64)FIRST_LIGHTS * sizeof(FFogLight);
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE None = { 0, 0 };
    UBOOL bMade = SUCCEEDED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot()));
    for (INT i = 0; i < SLOTS && bMade; i++)
    {
        bMade = SUCCEEDED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          __uuidof(ID3D12Resource), LightUpload[i].VoidSlot())) &&
                SUCCEEDED(LightUpload[i]->Map(0, &None, &LightMapped[i])) && LightMapped[i];
        LightRoom[i] = bMade ? (INT)FIRST_LIGHTS : 0;
    }
    if (!bMade)
    {
        RdxLog(TEXT("fog rooms: its heap or its lights' lists could not be made"));
        Shutdown();
        return 0;
    }
    HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return 1;
}

UBOOL FFogRooms::HoldLights(ID3D12Device* Device, INT Slot, INT Wanted)
{
    if (Wanted <= LightRoom[Slot])
        return 1;
    if (!Device || !LightUpload[Slot])
        return 0;
    const INT Larger = Max(Wanted, LightRoom[Slot] * 2);
    D3D12_HEAP_PROPERTIES Up;
    appMemzero(&Up, sizeof(Up));
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = (UINT64)Larger * sizeof(FFogLight);
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* Made   = nullptr;
    void*           MadeAt = nullptr;
    D3D12_RANGE     None   = { 0, 0 };
    if (RdxForcedFailure(TEXT("fog lights")) ||
        FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), (void**)&Made)) ||
        FAILED(Made->Map(0, &None, &MadeAt)) || !MadeAt)
    {
        if (Made)
            Made->Release();
        RdxLog(TEXT("fog rooms: the lights' list could not be made for %i (it holds %i)"), Larger, LightRoom[Slot]);
        return 0;
    }
    // The shorter one is let go: the slot's last frame is done on the card by the time a frame records into it.
    LightUpload[Slot]->Unmap(0, nullptr);
    *LightUpload[Slot].Slot() = Made;
    LightMapped[Slot] = MadeAt;
    RdxLog(TEXT("fog rooms: the lights' list now holds %i (it held %i)"), Larger, LightRoom[Slot]);
    LightRoom[Slot] = Larger;
    return 1;
}

UBOOL FFogRooms::EnsurePso(ID3D12Device* Device, DXGI_FORMAT SceneFormat)
{
    if (Pso && PsoFormat == SceneFormat)
        return 1;
    Pso.Release();
    PsoFormat = DXGI_FORMAT_UNKNOWN;
    if (!Device || !Root || !Vs || !Ps || SceneFormat == DXGI_FORMAT_UNKNOWN)
        return 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC G;
    appMemzero(&G, sizeof(G));
    G.pRootSignature        = Root.Get();
    G.VS.pShaderBytecode    = Vs->GetBufferPointer();
    G.VS.BytecodeLength     = Vs->GetBufferSize();
    G.PS.pShaderBytecode    = Ps->GetBufferPointer();
    G.PS.BytecodeLength     = Ps->GetBufferSize();
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
    // The fog's colour is premultiplied, as the engine's is: fog + target * (1 - alpha).
    D3D12_RENDER_TARGET_BLEND_DESC& B = G.BlendState.RenderTarget[0];
    B.BlendEnable           = TRUE;
    B.SrcBlend              = D3D12_BLEND_ONE;
    B.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    B.BlendOp               = D3D12_BLEND_OP_ADD;
    B.SrcBlendAlpha         = D3D12_BLEND_ZERO;
    B.DestBlendAlpha        = D3D12_BLEND_ONE;
    B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                              D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(Device->CreateGraphicsPipelineState(&G, __uuidof(ID3D12PipelineState), Pso.VoidSlot())))
    {
        RdxLog(TEXT("fog rooms: the pipeline could not be made"));
        return 0;
    }

    PsoFormat = SceneFormat;
    return 1;
}

void FFogRooms::Shutdown()
{
    for (INT i = 0; i < SLOTS; i++)
    {
        if (LightUpload[i] && LightMapped[i])
            LightUpload[i]->Unmap(0, nullptr);
        LightMapped[i] = nullptr;
        LightUpload[i].Release();
        LightRoom[i] = 0;
    }
    bLightsRefused = 0;
    Nodes.Release();
    NodesUpload.Release();
    for (INT i = 0; i < 2; i++)
    {
        OldNodes[i].Release();
        OldUpload[i].Release();
    }
    NodesCount   = 0;
    bNodesToCopy = 0;
    Heap.Release();
    Pso.Release();
    PsoFormat = DXGI_FORMAT_UNKNOWN;
    Vs.Release();
    Ps.Release();
    Root.Release();
    for (INT i = 0; i < VIEWS; i++)
    {
        DepthSeen[i]   = nullptr;
        DepthFormat[i] = DXGI_FORMAT_UNKNOWN;
    }
    WaterSeen   = nullptr;
    bWaterSeen  = 0;
}

// From's reference handed to To, whose own one goes; From is left empty.
static void MoveRef(TComRef<ID3D12Resource>& To, TComRef<ID3D12Resource>& From)
{
    ID3D12Resource* const P = From.Get();
    if (P)
        P->AddRef();
    *To.Slot() = P;
    From.Release();
}

UBOOL FFogRooms::SetNodes(ID3D12Device* Device, const FFogNode* List, INT Count)
{
    MoveRef(OldNodes[1], OldNodes[0]);
    MoveRef(OldUpload[1], OldUpload[0]);
    MoveRef(OldNodes[0], Nodes);
    MoveRef(OldUpload[0], NodesUpload);
    NodesCount   = 0;
    bNodesToCopy = 0;
    if (!Device || !Heap || !List || Count <= 0)
        return 0;

    D3D12_HEAP_PROPERTIES Hp;
    appMemzero(&Hp, sizeof(Hp));
    D3D12_RESOURCE_DESC Bd;
    appMemzero(&Bd, sizeof(Bd));
    Bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Bd.Width            = (UINT64)Count * sizeof(FFogNode);
    Bd.Height           = 1;
    Bd.DepthOrArraySize = 1;
    Bd.MipLevels        = 1;
    Bd.SampleDesc.Count = 1;
    Bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // A buffer starts out COMMON whatever it is asked to: the copy promotes it to COPY_DEST, where the barrier after
    // the copy takes it from.
    Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               __uuidof(ID3D12Resource), Nodes.VoidSlot())))
        return 0;
    Hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    void* Mapped = nullptr;
    D3D12_RANGE None = { 0, 0 };
    if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), NodesUpload.VoidSlot())) ||
        FAILED(NodesUpload->Map(0, &None, &Mapped)))
    {
        Nodes.Release();
        NodesUpload.Release();
        return 0;
    }
    appMemcpy(Mapped, List, (SIZE_T)Count * sizeof(FFogNode));
    NodesUpload->Unmap(0, nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
    appMemzero(&Sd, sizeof(Sd));
    Sd.Format                     = DXGI_FORMAT_UNKNOWN;
    Sd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    Sd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Sd.Buffer.NumElements         = (UINT)Count;
    Sd.Buffer.StructureByteStride = sizeof(FFogNode);
    // Into both tables of views: the world's view's and a mirror's.
    D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
    At.ptr += HeapStep;
    Device->CreateShaderResourceView(Nodes.Get(), &Sd, At);
    At.ptr += 3 * (UINT64)HeapStep;
    Device->CreateShaderResourceView(Nodes.Get(), &Sd, At);
    NodesCount   = Count;
    bNodesToCopy = 1;
    return 1;
}

UBOOL FFogRooms::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
                        ID3D12Resource* Depth, DXGI_FORMAT DepthRead, ID3D12Resource* Water, const FView& View,
                        const FFogLight* Lights, INT Count, INT FrameIndex, const FLOAT* Mirror)
{
    if (!Cmd || !Device || !IsReady() || !Nodes || !Depth || !Lights || Count <= 0 || !EnsurePso(Device, SceneFormat))
        return 0;
    if (bNodesToCopy)
    {
        Cmd->CopyBufferRegion(Nodes.Get(), 0, NodesUpload.Get(), 0, (UINT64)NodesCount * sizeof(FFogNode));
        D3D12_RESOURCE_BARRIER B;
        appMemzero(&B, sizeof(B));
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Nodes.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Cmd->ResourceBarrier(1, &B);
        bNodesToCopy = 0;
    }
    // The world's view's table of views, or a mirror's: each holds its own depth.
    const INT W = Mirror ? 1 : 0;
    // A texture whose view the heap holds was made again since: the view anew (ViewGeneration.h).
    if (ViewsAt[W] != RdxViewGeneration() || DepthSeen[W] != Depth || DepthFormat[W] != DepthRead)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = DepthRead;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += 3 * W * (UINT64)HeapStep;
        Device->CreateShaderResourceView(Depth, &Sd, At);
        ViewsAt[W]     = RdxViewGeneration();
        DepthSeen[W]   = Depth;
        DepthFormat[W] = DepthRead;
        if (W)
        {
            // A mirror's view has no water's depth of its own: a view of nothing, which reads 0.
            Sd.Format = DXGI_FORMAT_R32_FLOAT;
            At.ptr += 2 * (UINT64)HeapStep;
            Device->CreateShaderResourceView(nullptr, &Sd, At);
        }
        else
            bWaterSeen = 0;
    }
    // The water's view, or a view of nothing - which reads 0, the far plane: no water in the way.
    if (!W && (!bWaterSeen || WaterSeen != Water))
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Wd;
        appMemzero(&Wd, sizeof(Wd));
        Wd.Format                  = DXGI_FORMAT_R32_FLOAT;
        Wd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Wd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Wd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += 2 * (UINT64)HeapStep;
        Device->CreateShaderResourceView(Water, &Wd, At);
        WaterSeen  = Water;
        bWaterSeen = 1;
    }

    // Every light the frame has: the slot's list is made as long as that. Where it cannot be, none of the fog is
    // laid - the nearest few alone would leave the others' fog out, to come as the eye walked up to them.
    const INT N    = Count;
    const INT Slot = ((FrameIndex % FRAMES) + FRAMES) % FRAMES + W * FRAMES;
    if (!HoldLights(Device, Slot, N))
    {
        bLightsRefused = 1;
        return 0;
    }
    appMemcpy(LightMapped[Slot], Lights, (SIZE_T)N * sizeof(FFogLight));

    UINT   C[24];
    FLOAT* F = (FLOAT*)C;
    appMemzero(C, sizeof(C));
    F[0] = View.Screen[0]; F[1] = View.Screen[1]; F[2] = View.Focal; C[3] = View.CameraZone;
    F[4] = View.EngPerPixel[0]; F[5] = View.EngPerPixel[1]; C[6] = (UINT)N; C[7] = (UINT)NodesCount;
    for (INT k = 0; k < 3; k++)
    {
        F[8 + k]  = View.Eye[k];
        F[12 + k] = View.AxisX[k];
        F[16 + k] = View.AxisY[k];
        F[20 + k] = View.AxisZ[k];
    }
    // A mirror's plane, in the places the eye and the axes leave free (the shader's Mirror).
    for (INT k = 0; k < 4 && Mirror; k++)
        F[11 + 4 * k] = Mirror[k];

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetPipelineState(Pso.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 24, C, 0);
    Cmd->SetGraphicsRootShaderResourceView(1, LightUpload[Slot]->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE Table = Heap->GetGPUDescriptorHandleForHeapStart();
    Table.ptr += 3 * W * (UINT64)HeapStep;
    Cmd->SetGraphicsRootDescriptorTable(2, Table);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->DrawInstanced(3, 1, 0, 0);
    return 1;
}
