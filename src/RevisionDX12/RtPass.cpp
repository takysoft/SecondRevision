#include "Pch.h"
#include "Backend.h"
#include "Log.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// The far probe's update (FRtPass::UpdateFar), compiled at load time like the other passes that need no shader model 6: a
// thread a texel of the cube. Its direction, per Direct3D's own cube faces, goes back onto the picture through the world
// view; where the picture shows something further off than FarMin - or nothing at all - the texel takes the picture's
// colour, and alpha 1 says it holds one. Reset: the texels this frame does not reach are cleared as well.
static const char* const GFarProbeShader =
"Texture2D        Picture : register(t0);\n"
"Texture2D<float> Depth   : register(t1);     // the view depth's reciprocal; 0 where nothing was drawn\n"
"RWTexture2DArray<float4> Far : register(u0);\n"
"cbuffer FarConsts : register(b0)\n"
"{\n"
"    float3 AxisX; float TanX;\n"
"    float3 AxisY; float TanY;\n"
"    float3 AxisZ; float FarMin;\n"
"    float4 Rect;\n"
"    uint   Face; uint Reset; uint Pad0; uint Pad1;\n"
"};\n"
"[numthreads(8, 8, 1)]\n"
"void FarCS(uint3 Id : SV_DispatchThreadID)\n"
"{\n"
"    if (Id.x >= Face || Id.y >= Face)\n"
"        return;\n"
"    const float2 F = (float2(Id.xy) + 0.5f) / float(Face) * 2.0f - 1.0f;\n"
"    float3 D;\n"
"    if (Id.z == 0u)      D = float3(1.0f, -F.y, -F.x);\n"
"    else if (Id.z == 1u) D = float3(-1.0f, -F.y, F.x);\n"
"    else if (Id.z == 2u) D = float3(F.x, 1.0f, F.y);\n"
"    else if (Id.z == 3u) D = float3(F.x, -1.0f, -F.y);\n"
"    else if (Id.z == 4u) D = float3(F.x, -F.y, 1.0f);\n"
"    else                 D = float3(-F.x, -F.y, -1.0f);\n"
"    D = normalize(D);\n"
"    const float3 C = float3(dot(D, AxisX), dot(D, AxisY), dot(D, AxisZ));\n"
"    float4 Out  = 0.0f;\n"
"    bool   Seen = false;\n"
"    if (C.z > 0.01f)\n"
"    {\n"
"        const float2 Ndc = float2(C.x / (C.z * TanX), C.y / (C.z * TanY));\n"
"        if (all(abs(Ndc) < 1.0f))\n"
"        {\n"
"            uint W, H;\n"
"            Depth.GetDimensions(W, H);\n"
"            const float2 Uv = Rect.xy + (Ndc * 0.5f + 0.5f) * Rect.zw;\n"
"            const int2   P  = min(int2(Uv * float2(W, H)), int2(int(W) - 1, int(H) - 1));\n"
"            const float  Z  = Depth.Load(int3(P, 0));\n"
"            if (Z <= 0.0f || 1.0f / Z > FarMin)\n"
"            {\n"
"                Out  = float4(Picture.Load(int3(P, 0)).rgb, 1.0f);\n"
"                Seen = true;\n"
"            }\n"
"        }\n"
"    }\n"
"    if (Seen || Reset != 0u)\n"
"        Far[Id] = Out;\n"
"}\n";

FRtPass::FRtPass()
{
    Device          = nullptr;
    bReady          = 0;
    bHosted         = 0;
    StaticSerial    = 0;
    LevelVertices   = 0;
    SkyVertices     = 0;
    LoggedFrames    = 0;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        GeoCaster[f]  = nullptr;
        GeoRoom[f]    = 0;
        GeoCount[f]   = 0;
        OwnBodyGeo[f] = -1;
        ShadowTable[f].Mapped = WaterTable[f].Mapped = nullptr;
        ShadowTable[f].Bytes  = WaterTable[f].Bytes  = 0;
    }
    Geos            = nullptr;
    OwnBits         = nullptr;
    SceneRoom       = 0;
    ExclusionMapped = nullptr;
    FarState        = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bFarEmpty       = 1;
    MaterialCpu     = nullptr;
    SurfLightId     = nullptr;
    SurfCount       = 0;
    PieceCount      = 0;
    MaterialState   = D3D12_RESOURCE_STATE_COPY_DEST;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        MaterialUploadMapped[f] = nullptr;
        MaterialUploadBytes[f]  = 0;
    }
    bMaterialDirty  = 0;
    bRecordsDirty   = 0;
    DirtyFirst      = 0;
    DirtyLast       = -1;
    bMaterialOnCard = 0;
}

FRtPass::~FRtPass()
{
    Shutdown();
}

// The RT shadows' exclusion constants (FillShadowExclusion): an upload buffer, a frame slot's worth each, mapped for as
// long as it lives.
UBOOL FRtPass::MakeExclusion(ID3D12Device5* InDevice)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Cb;
    appMemzero(&Cb, sizeof(Cb));
    Cb.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Cb.Width            = (UINT64)EXCLUSION_STRIDE * BACKEND_FRAME_COUNT;
    Cb.Height           = 1;
    Cb.DepthOrArraySize = 1;
    Cb.MipLevels        = 1;
    Cb.SampleDesc.Count = 1;
    Cb.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(InDevice->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Cb, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 __uuidof(ID3D12Resource), ExclusionCb.VoidSlot())) ||
        FAILED(ExclusionCb->Map(0, nullptr, (void**)&ExclusionMapped)) || !ExclusionMapped)
    {
        ExclusionCb.Release();
        ExclusionMapped = nullptr;
        return 0;
    }
    appMemzero(ExclusionMapped, (SIZE_T)EXCLUSION_STRIDE * BACKEND_FRAME_COUNT);
    return 1;
}

UBOOL FRtPass::InitHosted(ID3D12Device5* InDevice)
{
    Shutdown();
    if (!InDevice)
        return 0;
    if (!MakeExclusion(InDevice))
    {
        RdxLog(TEXT("ray tracing (the host's): the shadows' constant buffer could not be made - no rays through the host"));
        return 0;
    }
    // The far probe traces nothing - a compute pass over the picture, which the mirror layers that read the host's
    // answers read.
    if (!InitFar(InDevice))
        RdxLog(TEXT("ray tracing (the host's): the far probe could not be made - no mirror images through the host"));
    Device  = InDevice;
    bReady  = 1;
    bHosted = 1;
    RdxLog(TEXT("ray tracing (the host's): the scene's lists and the shadows' tables are kept here for the 64-bit ")
           TEXT("host, which makes the structures and runs the passes"));
    return 1;
}

void FRtPass::Shutdown()
{
    bHosted = 0;
    StaticSerial++;
    if (LampBlob)
        appFree(LampBlob);
    LampBlob  = nullptr;
    LampRoom  = LampBytes = LampCount = LampLarge = 0;
    LevelVb.Release();
    RetiredLevelVb.Release();
    LevelVertices = 0;
    SkyVb.Release();
    RetiredSkyVb.Release();
    SkyVertices = 0;
    ThinVb.Release();
    RetiredThinVb.Release();
    ThinVertices = 0;
    if (ThinSurfIds)
        appFree(ThinSurfIds);
    ThinSurfIds = nullptr;
    if (ExclusionCb && ExclusionMapped)
        ExclusionCb->Unmap(0, nullptr);
    ExclusionCb.Release();
    ExclusionMapped = nullptr;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        FTable* const Tables[2] = { &ShadowTable[f], &WaterTable[f] };
        for (INT t = 0; t < 2; t++)
        {
            if (Tables[t]->Res && Tables[t]->Mapped)
                Tables[t]->Res->Unmap(0, nullptr);
            Tables[t]->Res.Release();
            Tables[t]->Mapped = nullptr;
            Tables[t]->Bytes  = 0;
        }
        if (GeoCaster[f])
            appFree(GeoCaster[f]);
        GeoCaster[f] = nullptr;
        GeoRoom[f]   = 0;
    }
    if (Geos)
        appFree(Geos);
    if (OwnBits)
        appFree(OwnBits);
    Geos      = nullptr;
    OwnBits   = nullptr;
    SceneRoom = 0;
    ForgetMaterial();
    Far.Release();
    FarRoot.Release();
    FarPipeline.Release();
    FarState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bFarEmpty = 1;
    LevelMaterial.Release();
    RetiredMaterial.Release();
    MaterialState = D3D12_RESOURCE_STATE_COPY_DEST;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        if (MaterialUpload[f] && MaterialUploadMapped[f])
            MaterialUpload[f]->Unmap(0, nullptr);
        MaterialUpload[f].Release();
        MaterialUploadMapped[f] = nullptr;
        MaterialUploadBytes[f]  = 0;
    }
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        GeoCount[f]   = 0;
        OwnBodyGeo[f] = -1;
    }
    Device = nullptr;
    bReady = 0;
}

void FRtPass::RefuseLists(const TCHAR* Which, INT Wanted)
{
    RdxLog(TEXT("ray tracing: no memory for %s (%i wanted) - the RT settings run their fallbacks from here"), Which,
           Wanted);
    bReady = 0;
}

UBOOL FRtPass::HoldScene(INT FrameIndex, INT Casters)
{
    enum { FIRST_GEOS = 128 };
    if (Casters > SceneRoom)
    {
        INT Room = Max(SceneRoom, (INT)FIRST_GEOS);
        while (Room < Casters)
            Room += Room / 2;
        D3D12_RAYTRACING_GEOMETRY_DESC* const NewGeos = RdxForcedFailure(TEXT("rt lists")) ? nullptr
            : (D3D12_RAYTRACING_GEOMETRY_DESC*)appRealloc(Geos, (DWORD)Room * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC),
                                                          TEXT("RdxRtGeos"));
        if (NewGeos)
            Geos = NewGeos;
        DWORD* const NewBits = NewGeos ? (DWORD*)appRealloc(OwnBits, (DWORD)ShadowCasterWords(Room) * sizeof(DWORD),
                                                            TEXT("RdxRtOwnBits"))
                                       : nullptr;
        if (NewBits)
            OwnBits = NewBits;
        if (!NewBits)
            return 0;
        SceneRoom = Room;
    }
    if (Casters > GeoRoom[FrameIndex])
    {
        INT* const NewList = (INT*)appRealloc(GeoCaster[FrameIndex], (DWORD)SceneRoom * sizeof(INT),
                                              TEXT("RdxRtGeoCaster"));
        if (!NewList)
            return 0;
        GeoCaster[FrameIndex] = NewList;
        GeoRoom[FrameIndex]   = SceneRoom;
    }
    return 1;
}

UBOOL FRtPass::HoldTable(FTable& Table, UINT64 Bytes, const TCHAR* What)
{
    if (Table.Res && Table.Mapped && Table.Bytes >= Bytes)
        return 1;
    if (Table.Res && Table.Mapped)
        Table.Res->Unmap(0, nullptr);
    Table.Res.Release();
    Table.Mapped = nullptr;
    Table.Bytes  = 0;
    // Room over what was asked, so a frame with one more body does not make the table again.
    const UINT64 Want = (Bytes + Bytes / 2 + 4095) & ~(UINT64)4095;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = Want;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (RdxForcedFailure(TEXT("rt lists")) ||
        FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, __uuidof(ID3D12Resource), Table.Res.VoidSlot())) ||
        FAILED(Table.Res->Map(0, nullptr, (void**)&Table.Mapped)) || !Table.Mapped)
    {
        RdxLog(TEXT("ray tracing: %s could not be made (%i bytes)"), What, (INT)Want);
        Table.Res.Release();
        Table.Mapped = nullptr;
        return 0;
    }
    appMemzero(Table.Mapped, (SIZE_T)Want);
    Table.Bytes = Want;
    return 1;
}

// The frame's actors as the geometries of one structure - listed into Geos, the frame slot's own lists and tables held
// for them - for ListScene, which hands the list on to the host.
UBOOL FRtPass::GatherScene(INT FrameIndex, const FShadowMaps& Shadow, INT& Count, INT& Triangles, INT& NonOpaque)
{
    // The actors: every caster of the frame a geometry of its own, where the shadow maps hold its triangles. The casters
    // some light must not see (FShadowLightFrame::Own - a lamp's own mesh, its shade) go in non-opaque, so a ray asks the
    // shader about them (ShadowFactor.hlsl, Occluded); every other triangle ends a ray by itself, the fast way. Not the
    // maps' Exclude: its reach cull marks nearly every caster, and all of them non-opaque made the factor several times
    // slower (SHADOW10: 8.7 ms).
    const INT Casters = Shadow.GetCasterCount();
    const INT Words   = Shadow.GetCasterWords();
    Count = Triangles = NonOpaque = 0;
    GeoCount[FrameIndex]   = 0;
    OwnBodyGeo[FrameIndex] = -1;
    if (!HoldScene(FrameIndex, Max(Casters, 1)))
    {
        RefuseLists(TEXT("the scene's lists"), Casters);
        return 0;
    }
    DWORD* const Excluded = OwnBits;
    appMemzero(Excluded, (DWORD)Words * sizeof(DWORD));
    for (INT l = 0; l < Shadow.GetLightCount(); l++)
    {
        const FShadowLightFrame& L = Shadow.GetLight(l);
        for (INT w = 0; L.Own && w < Words; w++)
            Excluded[w] |= L.Own[w];
    }
    for (INT i = 0; i < Casters; i++)
    {
        D3D12_VERTEX_BUFFER_VIEW View;
        INT First = 0, Vertices = 0;
        if (!Shadow.GetCasterGeometry(i, View, First, Vertices) || Vertices < 3)
            continue;
        const INT G = Count++;
        GeoCaster[FrameIndex][G] = i;
        // Non-opaque too: a caster with a mask (a leaf, a fence), whose rays the shader lets through where it is clear,
        // and the player's own body, which the occlusion's rays pass (FillShadowExclusion's last knob).
        const UBOOL bOwnBody  = Shadow.IsCasterOwnBody(i);
        if (bOwnBody)
            OwnBodyGeo[FrameIndex] = G;
        const UBOOL bExcluded = (Excluded[i >> 5] & (1u << (i & 31))) != 0 || Shadow.IsCasterMasked(i) || bOwnBody;
        NonOpaque += bExcluded ? 1 : 0;
        D3D12_RAYTRACING_GEOMETRY_DESC& Geo = Geos[G];
        appMemzero(&Geo, sizeof(Geo));
        Geo.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        Geo.Flags                                = bExcluded ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
                                                             : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        Geo.Triangles.VertexBuffer.StartAddress  = View.BufferLocation + (UINT64)First * View.StrideInBytes;
        Geo.Triangles.VertexBuffer.StrideInBytes = View.StrideInBytes;
        Geo.Triangles.VertexCount                = (UINT)(Vertices / 3 * 3);
        Geo.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
        Geo.Triangles.IndexFormat                = DXGI_FORMAT_UNKNOWN;
        Triangles += Vertices / 3;
    }
    // The shaders' tables of this frame slot, with room for these geometries: the shadows' - a uint each, and a row
    // of bits a light - and the water's.
    const INT GeoWords = ShadowCasterWords(Count);
    if (!HoldTable(ShadowTable[FrameIndex], ((UINT64)Count + (UINT64)RdxMath::ShadowMaxLights * GeoWords + 4) * 4,
                   TEXT("the shadows' table")) ||
        !HoldTable(WaterTable[FrameIndex],
                   (UINT64)WATER_TABLE_HEAD + (UINT64)Count * RDX_RT_GEO_BYTES + (UINT64)LampBytes,
                   TEXT("the water's table")))
    {
        RefuseLists(TEXT("the scene's tables"), Count);
        return 0;
    }
    return 1;
}

INT FRtPass::ListScene(INT FrameIndex, const FShadowMaps& Shadow)
{
    if (!bHosted || !bReady || FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT)
        return -1;
    INT Count = 0, Triangles = 0, NonOpaque = 0;
    if (!GatherScene(FrameIndex, Shadow, Count, Triangles, NonOpaque))
        return -1;
    // With no structure made here the list is the scene: what FillShadowExclusion's table goes by.
    GeoCount[FrameIndex] = Count;
    if (LoggedFrames++ % 600 == 0)
        RdxLog(TEXT("ray tracing (the host's): the frame's scene listed - %i actors with %i triangles, %i of them masked, ")
               TEXT("some light's own or the player's own body; the level %i triangles"),
               Count, Triangles, NonOpaque, LevelVertices / 3);
    return Count;
}

ID3D12Resource* FRtPass::GetStaticBuffer(INT Which) const
{
    return (Which == 0) ? LevelVb.Get() : ((Which == 1) ? SkyVb.Get() : ((Which == 2) ? ThinVb.Get() : nullptr));
}

INT FRtPass::GetStaticVertices(INT Which) const
{
    return (Which == 0) ? LevelVertices : ((Which == 1) ? SkyVertices : ((Which == 2) ? ThinVertices : 0));
}

ID3D12Resource* FRtPass::GetShadowTableBuffer(INT FrameIndex) const
{
    return (FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? ShadowTable[FrameIndex].Res.Get() : nullptr;
}

UINT64 FRtPass::GetShadowTableBytes(INT FrameIndex) const
{
    if (FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT || !ShadowTable[FrameIndex].Res)
        return 0;
    const INT    Geometries = GeoCount[FrameIndex];
    const UINT64 Bytes = ((UINT64)Geometries + (UINT64)RdxMath::ShadowMaxLights * ShadowCasterWords(Geometries)) * 4;
    return Min(Bytes, ShadowTable[FrameIndex].Bytes);
}

ID3D12Resource* FRtPass::GetWaterTableBuffer(INT FrameIndex) const
{
    return (FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? WaterTable[FrameIndex].Res.Get() : nullptr;
}

UINT64 FRtPass::GetWaterTableBytes(INT Bodies) const
{
    return (UINT64)WATER_TABLE_HEAD + (UINT64)Max(Bodies, 0) * RDX_RT_GEO_BYTES + (UINT64)LampBytes;
}

UINT64 FRtPass::GetMaterialBytes() const
{
    return MaterialCpu ? ((UINT64)PieceCount + (UINT64)RdxMath::RtRecordUints * (UINT64)SurfCount) * sizeof(DWORD) : 0;
}

UBOOL FRtPass::CopyMaterial(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To) const
{
    const UINT64 Bytes = GetMaterialBytes();
    if (!Cmd || !To || !Bytes || !LevelMaterial || !bMaterialOnCard || To->GetDesc().Width < Bytes ||
        LevelMaterial->GetDesc().Width < Bytes)
        return 0;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = LevelMaterial.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = MaterialState;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Cmd->ResourceBarrier(1, &B);
    Cmd->CopyBufferRegion(To, 0, LevelMaterial.Get(), 0, Bytes);
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B.Transition.StateAfter  = MaterialState;
    Cmd->ResourceBarrier(1, &B);
    return 1;
}

// The level's pieces: at most this many triangles, or four times the level's own, whichever is more.
enum { RT_LEVEL_TRIANGLES = 400000 };

UBOOL FRtPass::InitFar(ID3D12Device5* InDevice)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = FAR_SIZE;
    Desc.Height           = FAR_SIZE;
    Desc.DepthOrArraySize = 6;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(InDevice->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 nullptr, __uuidof(ID3D12Resource), Far.VoidSlot())))
        return 0;
    FarState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bFarEmpty = 1;

    D3D12_DESCRIPTOR_RANGE Ranges[2];
    appMemzero(Ranges, sizeof(Ranges));
    Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors     = 2;
    Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors     = 1;
    D3D12_ROOT_PARAMETER Params[3];
    appMemzero(Params, sizeof(Params));
    for (INT k = 0; k < 2; k++)
    {
        Params[k].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[k].DescriptorTable.NumDescriptorRanges = 1;
        Params[k].DescriptorTable.pDescriptorRanges   = &Ranges[k];
        Params[k].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    }
    Params[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[2].Constants.Num32BitValues = 20;
    Params[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC Root;
    appMemzero(&Root, sizeof(Root));
    Root.NumParameters = 3;
    Root.pParameters   = Params;
    TComRef<ID3DBlob> Blob, Error, Cs;
    if (FAILED(D3D12SerializeRootSignature(&Root, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(InDevice->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                             __uuidof(ID3D12RootSignature), FarRoot.VoidSlot())))
    {
        Far.Release();
        return 0;
    }
    if (FAILED(RdxCompile(GFarProbeShader, strlen(GFarProbeShader), "FarProbe", nullptr, nullptr, "FarCS", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Cs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("ray tracing: the far probe's shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        FarRoot.Release();
        Far.Release();
        return 0;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature     = FarRoot.Get();
    Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
    Pso.CS.BytecodeLength  = Cs->GetBufferSize();
    if (FAILED(InDevice->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), FarPipeline.VoidSlot())))
    {
        FarRoot.Release();
        Far.Release();
        return 0;
    }
    RdxLog(TEXT("ray tracing: the far probe made - a cube of %i x %i a face (%.1f MB)"), (INT)FAR_SIZE, (INT)FAR_SIZE,
           (DOUBLE)FAR_SIZE * FAR_SIZE * 6 * 8 / (1024.0 * 1024.0));
    return 1;
}

void FRtPass::DescribeFarViews(D3D12_SHADER_RESOURCE_VIEW_DESC& Srv, D3D12_UNORDERED_ACCESS_VIEW_DESC& Uav) const
{
    appMemzero(&Srv, sizeof(Srv));
    Srv.Format                      = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Srv.ViewDimension               = D3D12_SRV_DIMENSION_TEXTURECUBE;
    Srv.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Srv.TextureCube.MipLevels       = 1;
    appMemzero(&Uav, sizeof(Uav));
    Uav.Format                      = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Uav.ViewDimension               = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    Uav.Texture2DArray.ArraySize    = 6;
}

void FRtPass::UpdateFar(ID3D12GraphicsCommandList* Cmd, D3D12_GPU_DESCRIPTOR_HANDLE Picture,
                        D3D12_GPU_DESCRIPTOR_HANDLE FarUav, const FLOAT* Axes, FLOAT TanX, FLOAT TanY, const FLOAT* Rect)
{
    if (!Cmd || !Far || !FarPipeline || !Axes || !Rect)
        return;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Far.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (FarState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        B.Transition.StateBefore = FarState;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Cmd->ResourceBarrier(1, &B);
    }
    DWORD C[20];
    FLOAT* const F = (FLOAT*)C;
    for (INT a = 0; a < 3; a++)
    {
        F[a * 4 + 0] = Axes[a * 3 + 0];
        F[a * 4 + 1] = Axes[a * 3 + 1];
        F[a * 4 + 2] = Axes[a * 3 + 2];
    }
    F[3]  = TanX;
    F[7]  = TanY;
    F[11] = (FLOAT)RdxMath::RtFarMin;
    for (INT k = 0; k < 4; k++)
        F[12 + k] = Rect[k];
    C[16] = (DWORD)FAR_SIZE;
    C[17] = bFarEmpty ? 1u : 0u;
    C[18] = C[19] = 0;
    Cmd->SetComputeRootSignature(FarRoot.Get());
    Cmd->SetPipelineState(FarPipeline.Get());
    Cmd->SetComputeRootDescriptorTable(0, Picture);
    Cmd->SetComputeRootDescriptorTable(1, FarUav);
    Cmd->SetComputeRoot32BitConstants(2, 20, C, 0);
    Cmd->Dispatch((UINT)FAR_SIZE / 8, (UINT)FAR_SIZE / 8, 6);
    bFarEmpty = 0;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);
    FarState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void FRtPass::ForgetMaterial()
{
    if (MaterialCpu)
        appFree(MaterialCpu);
    if (SurfLightId)
        appFree(SurfLightId);
    MaterialCpu     = nullptr;
    SurfLightId     = nullptr;
    SurfCount       = 0;
    PieceCount      = 0;
    bMaterialDirty  = 0;
    bRecordsDirty   = 0;
    DirtyFirst      = 0;
    DirtyLast       = -1;
    bMaterialOnCard = 0;
}

ID3D12Resource* FRtPass::MakeVertexBuffer(const FLOAT* Src, INT Floats, const TCHAR* What)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = (UINT64)Floats * sizeof(FLOAT);
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* Res = nullptr;
    FLOAT* Out = nullptr;
    D3D12_RANGE NoRead = { 0, 0 };
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&Res)) ||
        FAILED(Res->Map(0, &NoRead, (void**)&Out)) || !Out)
    {
        if (Res)
            Res->Release();
        RdxLog(TEXT("ray tracing: the %s could not be put on the card (%i floats)"), What, Floats);
        return nullptr;
    }
    appMemcpy(Out, Src, (SIZE_T)Floats * sizeof(FLOAT));
    Res->Unmap(0, nullptr);
    return Res;
}

void FRtPass::SetLevel(const FLOAT* Xyz, INT VertexCount, const DWORD* Surf, const DWORD* Albedo, INT InSurfCount,
                       const FLOAT* Sky, INT SkyVertexCount)
{
    if (!bReady || !Device)
        return;
    StaticSerial++;
    ForgetMaterial();
    bFarEmpty = 1;       // another level's sky
    // The sky walls: kept whole, as they are - a ray only has to find them.
    RetiredSkyVb.Release();
    if (SkyVb)
    {
        ID3D12Resource* const Old = SkyVb.Get();
        Old->AddRef();
        SkyVb.Release();
        *RetiredSkyVb.Slot() = Old;
    }
    SkyVertices = 0;
    if (Sky && SkyVertexCount >= 3)
    {
        ID3D12Resource* const Made = MakeVertexBuffer(Sky, SkyVertexCount / 3 * 9, TEXT("sky walls"));
        if (Made)
        {
            *SkyVb.Slot() = Made;
            SkyVertices   = SkyVertexCount / 3 * 3;
        }
    }
    RetiredLevelVb.Release();
    if (LevelVb)
    {
        ID3D12Resource* const Old = LevelVb.Get();
        Old->AddRef();
        LevelVb.Release();
        *RetiredLevelVb.Slot() = Old;
    }
    LevelVertices = 0;
    const INT Triangles = (Xyz && VertexCount >= 3) ? VertexCount / 3 : 0;
    if (Triangles <= 0)
        return;
    const DOUBLE Start = appSeconds();
    // What there is to split: the longest edge, and how many triangles have an edge over 1024 units.
    FLOAT Longest2 = 0.0f;
    INT   Over1024 = 0;
    for (INT t = 0; t < Triangles; t++)
    {
        const FLOAT* P = Xyz + t * 9;
        const FLOAT  E = Max(RdxMath::SplitDist2(P, P + 3), Max(RdxMath::SplitDist2(P + 3, P + 6),
                                                                RdxMath::SplitDist2(P + 6, P)));
        Longest2 = Max(Longest2, E);
        Over1024 += (E > 1024.0f * 1024.0f) ? 1 : 0;
    }
    const INT   Budget = Max(Triangles * 4, (INT)RT_LEVEL_TRIANGLES);
    const FLOAT Edge   = RdxMath::LevelSplitEdge(Xyz, Triangles, Budget);
    const FLOAT Edge2  = Edge * Edge;
    INT Pieces = Triangles;
    if (Edge > 0.0f)
    {
        Pieces = 0;
        for (INT t = 0; t < Triangles; t++)
            Pieces += RdxMath::SplitTriangleCount(Xyz + t * 9, Xyz + t * 9 + 3, Xyz + t * 9 + 6, Edge2,
                                                  RdxMath::LevelSplitDepth);
    }

    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = (UINT64)Pieces * 9 * sizeof(FLOAT);
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    FLOAT* Out = nullptr;
    D3D12_RANGE NoRead = { 0, 0 };
    if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               __uuidof(ID3D12Resource), LevelVb.VoidSlot())) ||
        FAILED(LevelVb->Map(0, &NoRead, (void**)&Out)) || !Out)
    {
        LevelVb.Release();
        RdxLog(TEXT("ray tracing: the level's triangles could not be put on the card (%i) - the rays see no level"), Pieces);
        return;
    }
    if (Edge > 0.0f)
    {
        FLOAT* At = Out;
        for (INT t = 0; t < Triangles; t++)
            At = RdxMath::SplitTriangle(Xyz + t * 9, Xyz + t * 9 + 3, Xyz + t * 9 + 6, Edge2, RdxMath::LevelSplitDepth, At);
    }
    else
        appMemcpy(Out, Xyz, Triangles * 9 * (INT)sizeof(FLOAT));
    LevelVb->Unmap(0, nullptr);
    LevelVertices = Pieces * 3;

    // The level's colours for the RT water: each piece its triangle's surface - SplitTriangle writes a triangle's pieces
    // one after another, as many as SplitTriangleCount says - then each surface's colour, with no light known yet.
    if (Surf && Albedo && InSurfCount > 0)
    {
        // The see-through surfaces' triangles after the level's own pieces: one list of surfaces for both.
        const INT ThinPieces = ThinSurfIds ? ThinVertices / 3 : 0;
        MaterialCpu = (DWORD*)appMalloc(((SIZE_T)Pieces + (SIZE_T)ThinPieces +
                                         (SIZE_T)RdxMath::RtRecordUints * (SIZE_T)InSurfCount) * sizeof(DWORD),
                                        TEXT("RdxRtMaterial"));
        SurfLightId = (QWORD*)appMalloc((SIZE_T)InSurfCount * sizeof(QWORD), TEXT("RdxRtSurfLight"));
        if (MaterialCpu && SurfLightId)
        {
            INT At = 0;
            for (INT t = 0; t < Triangles && At < Pieces; t++)
            {
                const INT N = (Edge > 0.0f) ? RdxMath::SplitTriangleCount(Xyz + t * 9, Xyz + t * 9 + 3, Xyz + t * 9 + 6, Edge2,
                                                                          RdxMath::LevelSplitDepth)
                                            : 1;
                for (INT k = 0; k < N && At < Pieces; k++)
                    MaterialCpu[At++] = Surf[t];
            }
            for (; At < Pieces; At++)
                MaterialCpu[At] = 0xFFFFFFFFu;
            for (INT k = 0; k < ThinPieces; k++)
                MaterialCpu[Pieces + k] = ThinSurfIds[k];
            DWORD* const Records = MaterialCpu + Pieces + ThinPieces;
            appMemzero(Records, (SIZE_T)RdxMath::RtRecordUints * (SIZE_T)InSurfCount * sizeof(DWORD));
            for (INT i = 0; i < InSurfCount; i++)
            {
                Records[i * RdxMath::RtRecordUints]     = Albedo[i];
                Records[i * RdxMath::RtRecordUints + 1] = RdxMath::RtLightUnknown;
                SurfLightId[i]                          = 0;
            }
            SurfCount      = InSurfCount;
            PieceCount     = Pieces + ThinPieces;
            ThinBase       = Pieces;
            bMaterialDirty = 1;
        }
        else
        {
            ForgetMaterial();
            RdxLog(TEXT("ray tracing: no memory for the level's colours - the RT water sees the level without them"));
        }
    }
    RdxLog(TEXT("ray tracing: the level for the scene - %i triangles (longest edge %.0f units, %i with an edge over 1024), ")
           TEXT("split at %.0f units into %i, in %.1f ms"),
           Triangles, appSqrt(Longest2), Over1024, Edge, Pieces, (appSeconds() - Start) * 1000.0);
}

void FRtPass::SetSurfaceLight(INT Surf, QWORD LightId, DWORD Light)
{
    if (Surf < 0 || Surf >= SurfCount || !MaterialCpu || !SurfLightId)
        return;
    SurfLightId[Surf] = LightId;
    DWORD& Rec = MaterialCpu[PieceCount + Surf * RdxMath::RtRecordUints + 1];
    if (Rec != Light)
    {
        Rec = Light;
        NoteRecordDirty(Surf);
    }
}

void FRtPass::NoteRecordDirty(INT Surf)
{
    DirtyFirst    = bRecordsDirty ? Min(DirtyFirst, Surf) : Surf;
    DirtyLast     = bRecordsDirty ? Max(DirtyLast, Surf) : Surf;
    bRecordsDirty = 1;
}

void FRtPass::SetSurfaceTexture(INT Surf, INT SrvIndex, const FLOAT* Rows, FLOAT Density, UBOOL bKeepRows)
{
    if (Surf < 0 || Surf >= SurfCount || !MaterialCpu || !Rows || SrvIndex < 0)
        return;
    DWORD* const Rec  = MaterialCpu + PieceCount + Surf * RdxMath::RtRecordUints;
    const DWORD  Want = (DWORD)SrvIndex + 1u;
    if (Rec[RdxMath::RtRecordTexture] == Want &&
        (bKeepRows || (appMemcmp(Rec + RdxMath::RtRecordRows, Rows, 8 * sizeof(FLOAT)) == 0 &&
                       appMemcmp(Rec + RdxMath::RtRecordDensity, &Density, sizeof(FLOAT)) == 0)))
        return;
    Rec[RdxMath::RtRecordTexture] = Want;
    appMemcpy(Rec + RdxMath::RtRecordRows, Rows, 8 * sizeof(FLOAT));
    appMemcpy(Rec + RdxMath::RtRecordDensity, &Density, sizeof(FLOAT));
    NoteRecordDirty(Surf);
}

void FRtPass::ClearSurfaceTexture(INT Surf)
{
    if (Surf < 0 || Surf >= SurfCount || !MaterialCpu)
        return;
    DWORD* const Rec = MaterialCpu + PieceCount + Surf * RdxMath::RtRecordUints;
    if (Rec[RdxMath::RtRecordTexture] == 0)
        return;
    Rec[RdxMath::RtRecordTexture] = 0;
    NoteRecordDirty(Surf);
}

void FRtPass::SetSurfaceLightMap(INT Surf, INT SrvIndex, const FLOAT* Rows, const FLOAT* Edge, FLOAT Gain)
{
    if (Surf < 0 || Surf >= SurfCount || !MaterialCpu || !Rows || !Edge || SrvIndex < 0)
        return;
    DWORD* const Rec  = MaterialCpu + PieceCount + Surf * RdxMath::RtRecordUints;
    const DWORD  Want = (DWORD)SrvIndex + 1u;
    if (Rec[RdxMath::RtRecordLightMap] == Want &&
        appMemcmp(Rec + RdxMath::RtRecordLightRows, Rows, 8 * sizeof(FLOAT)) == 0 &&
        appMemcmp(Rec + RdxMath::RtRecordLightEdge, Edge, 4 * sizeof(FLOAT)) == 0 &&
        appMemcmp(Rec + RdxMath::RtRecordGain, &Gain, sizeof(FLOAT)) == 0)
        return;
    Rec[RdxMath::RtRecordLightMap] = Want;
    appMemcpy(Rec + RdxMath::RtRecordLightRows, Rows, 8 * sizeof(FLOAT));
    appMemcpy(Rec + RdxMath::RtRecordLightEdge, Edge, 4 * sizeof(FLOAT));
    appMemcpy(Rec + RdxMath::RtRecordGain, &Gain, sizeof(FLOAT));
    NoteRecordDirty(Surf);
}

void FRtPass::SetThin(const FLOAT* Xyz, INT VertexCount, const DWORD* Surf)
{
    if (!bReady || !Device)
        return;
    StaticSerial++;
    RetiredThinVb.Release();
    if (ThinVb)
    {
        ID3D12Resource* const Old = ThinVb.Get();
        Old->AddRef();
        ThinVb.Release();
        *RetiredThinVb.Slot() = Old;
    }
    ThinVertices = 0;
    ThinBase     = 0;
    if (ThinSurfIds)
        appFree(ThinSurfIds);
    ThinSurfIds = nullptr;
    const INT Triangles = (Xyz && Surf && VertexCount >= 3) ? VertexCount / 3 : 0;
    if (Triangles <= 0)
        return;
    DWORD* const Ids = (DWORD*)appMalloc((SIZE_T)Triangles * sizeof(DWORD), TEXT("RdxRtThinSurf"));
    if (!Ids)
        return;
    ID3D12Resource* const Made = MakeVertexBuffer(Xyz, Triangles * 9, TEXT("see-through surfaces"));
    if (!Made)
    {
        appFree(Ids);
        return;
    }
    appMemcpy(Ids, Surf, (SIZE_T)Triangles * sizeof(DWORD));
    *ThinVb.Slot() = Made;
    ThinVertices   = Triangles * 3;
    ThinSurfIds    = Ids;
}

void FRtPass::SetSurfaceStatic(INT Surf, const FLOAT* Normal, DWORD Ambient, UBOOL bSpecial, DWORD Kind)
{
    if (Surf < 0 || Surf >= SurfCount || !MaterialCpu || !Normal)
        return;
    DWORD* const Rec = MaterialCpu + PieceCount + Surf * RdxMath::RtRecordUints;
    appMemcpy(Rec + RdxMath::RtRecordNormal, Normal, 3 * sizeof(FLOAT));
    Rec[RdxMath::RtRecordAmbient] = Ambient;
    Rec[RdxMath::RtRecordKind]    = Kind;
    if (bSpecial)
        Rec[0] |= RdxMath::RtAlbedoSpecialLit;
    NoteRecordDirty(Surf);
}

void FRtPass::SetLamps(const BYTE* Blob, INT Bytes, const DWORD* Part, INT Count, INT Large, FLOAT Cell, FLOAT Gain)
{
    LampBytes = LampCount = LampLarge = 0;
    LampGain  = Gain;
    LampCell  = (Cell > 1.0f) ? Cell : 1024.0f;
    if (!Blob || !Part || Bytes <= 0 || Count <= 0)
        return;
    if (Bytes > LampRoom)
    {
        BYTE* const Grown = (BYTE*)appRealloc(LampBlob, (DWORD)Bytes + 4096, TEXT("RdxRtLamps"));
        if (!Grown)
            return;
        LampBlob = Grown;
        LampRoom = Bytes + 4096;
    }
    appMemcpy(LampBlob, Blob, (DWORD)Bytes);
    for (INT k = 0; k < 4; k++)
        LampPart[k] = Part[k];
    LampBytes = Bytes;
    LampCount = Count;
    LampLarge = Large;
}

D3D12_GPU_VIRTUAL_ADDRESS FRtPass::GetLevelMaterial() const
{
    return (LevelMaterial && bMaterialOnCard) ? LevelMaterial->GetGPUVirtualAddress() : 0;
}

void FRtPass::UploadMaterial(ID3D12GraphicsCommandList* Cmd, INT FrameIndex)
{
    if (!MaterialCpu || (!bMaterialDirty && !bRecordsDirty))
        return;
    const UINT64 Whole   = ((UINT64)PieceCount + (UINT64)RdxMath::RtRecordUints * (UINT64)SurfCount) * sizeof(DWORD);
    // All of it after SetLevel; otherwise the records that changed, from the first of them to the last.
    const UBOOL  bSome   = (!bMaterialDirty && DirtyLast >= DirtyFirst && DirtyFirst >= 0 && DirtyLast < SurfCount)
                           ? 1 : 0;
    const UINT64 Skip    = bSome ? (UINT64)DirtyFirst * RdxMath::RtRecordUints : 0;
    const UINT64 From    = bMaterialDirty ? 0 : ((UINT64)PieceCount + Skip) * sizeof(DWORD);
    const UINT64 Bytes   = bSome ? (UINT64)(DirtyLast - DirtyFirst + 1) * RDX_RT_RECORD_BYTES : Whole - From;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // The card's buffer, new after SetLevel: the one before is kept - a frame in flight may still read it.
    if (bMaterialDirty)
    {
        RetiredMaterial.Release();
        if (LevelMaterial)
        {
            ID3D12Resource* const Old = LevelMaterial.Get();
            Old->AddRef();
            LevelMaterial.Release();
            *RetiredMaterial.Slot() = Old;
        }
        bMaterialOnCard = 0;
        Heap.Type  = D3D12_HEAP_TYPE_DEFAULT;
        Desc.Width = Whole;
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   __uuidof(ID3D12Resource), LevelMaterial.VoidSlot())))
        {
            RdxLog(TEXT("ray tracing: the level's colours could not be put on the card (%.1f MB)"),
                   (DOUBLE)Whole / (1024.0 * 1024.0));
            ForgetMaterial();
            return;
        }
        MaterialState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (!LevelMaterial)
        return;
    // This frame slot's upload buffer: the GPU is done with the slot's frame before, so it may be made again.
    if (MaterialUploadBytes[FrameIndex] < Whole)
    {
        if (MaterialUpload[FrameIndex] && MaterialUploadMapped[FrameIndex])
            MaterialUpload[FrameIndex]->Unmap(0, nullptr);
        MaterialUpload[FrameIndex].Release();
        MaterialUploadMapped[FrameIndex] = nullptr;
        MaterialUploadBytes[FrameIndex]  = 0;
        Heap.Type  = D3D12_HEAP_TYPE_UPLOAD;
        Desc.Width = Whole;
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, __uuidof(ID3D12Resource), MaterialUpload[FrameIndex].VoidSlot())) ||
            FAILED(MaterialUpload[FrameIndex]->Map(0, nullptr, (void**)&MaterialUploadMapped[FrameIndex])) ||
            !MaterialUploadMapped[FrameIndex])
        {
            MaterialUpload[FrameIndex].Release();
            MaterialUploadMapped[FrameIndex] = nullptr;
            RdxLog(TEXT("ray tracing: the level's colours' upload buffer could not be made"));
            return;
        }
        MaterialUploadBytes[FrameIndex] = Whole;
    }
    appMemcpy(MaterialUploadMapped[FrameIndex] + From, (const BYTE*)MaterialCpu + From, (SIZE_T)Bytes);
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = LevelMaterial.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (MaterialState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        B.Transition.StateBefore = MaterialState;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        Cmd->ResourceBarrier(1, &B);
    }
    Cmd->CopyBufferRegion(LevelMaterial.Get(), From, MaterialUpload[FrameIndex].Get(), From, Bytes);
    MaterialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    B.Transition.StateAfter  = MaterialState;
    Cmd->ResourceBarrier(1, &B);
    if (bMaterialDirty)
        RdxLog(TEXT("ray tracing: the level's colours on the card - %i triangle pieces, %i surfaces (%.1f MB)"),
               PieceCount, SurfCount, (DOUBLE)Whole / (1024.0 * 1024.0));
    bMaterialDirty  = 0;
    bRecordsDirty   = 0;
    DirtyFirst      = 0;
    DirtyLast       = -1;
    bMaterialOnCard = 1;
    MaterialSerial++;
}

D3D12_GPU_VIRTUAL_ADDRESS FRtPass::PrepareWater(ID3D12GraphicsCommandList* Cmd, INT FrameIndex, const FShadowMaps& Shadow,
                                                const FLOAT* Eye, INT View, UBOOL bHalf)
{
    if (!bReady || !Cmd || !Eye || FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT)
        return 0;
    // The table ListScene made room in: the head, then a row a geometry.
    const FTable& Table = WaterTable[FrameIndex];
    const INT     Geos  = GeoCount[FrameIndex];
    if (!Table.Mapped ||
        Table.Bytes < (UINT64)WATER_TABLE_HEAD + (UINT64)Geos * RDX_RT_GEO_BYTES + (UINT64)LampBytes)
        return 0;
    UploadMaterial(Cmd, FrameIndex);
    FLOAT* const E = (FLOAT*)Table.Mapped;
    E[0] = Eye[0];
    E[1] = Eye[1];
    E[2] = Eye[2];
    E[3] = bHalf ? 1.0f : 0.0f;
    DWORD* const L = (DWORD*)(E + 4);
    L[0] = (DWORD)PieceCount * (DWORD)sizeof(DWORD);
    L[1] = bMaterialOnCard ? (DWORD)SurfCount : 0u;
    L[2] = (DWORD)Max(View, 0);
    // Where the see-through surfaces' triangles start in the colours' list of surfaces (SetThin).
    L[3] = (DWORD)ThinBase;
    // The level's lamps after the rows, and where they are.
    const DWORD LampsAt = (DWORD)WATER_TABLE_HEAD + (DWORD)Geos * RDX_RT_GEO_BYTES;
    L[4]  = LampsAt + LampPart[0];
    L[5]  = (DWORD)LampCount;
    L[6]  = LampsAt + LampPart[1];
    L[7]  = LampsAt + LampPart[2];
    L[8]  = LampsAt + LampPart[3];
    L[9]  = (DWORD)LampLarge;
    appMemcpy(L + 10, &LampCell, sizeof(FLOAT));
    appMemcpy(L + 11, &LampGain, sizeof(FLOAT));
    if (LampBytes > 0 && LampBlob)
        appMemcpy(Table.Mapped + LampsAt, LampBlob, (DWORD)LampBytes);
    DWORD* const Rows = L + 12;
    for (INT g = 0; g < Geos; g++)
    {
        D3D12_VERTEX_BUFFER_VIEW Vb;
        INT First = 0, Vertices = 0;
        const INT    c   = GeoCaster[FrameIndex][g];
        DWORD* const Row = Rows + g * RdxMath::RtGeoUints;
        appMemzero(Row, RDX_RT_GEO_BYTES);
        Row[RdxMath::RtGeoFirst] = Shadow.GetCasterGeometry(c, Vb, First, Vertices)
            ? (((DWORD)First & 0x7FFFFFFFu) | (Shadow.IsCasterInArena(c) ? 0x80000000u : 0u)) : 0u;
        const FShadowMaps::FCasterRt* const Rt = Shadow.GetCasterRt(c);
        if (!Rt)
        {
            Row[RdxMath::RtGeoColour] = 0x808080u;
            continue;
        }
        Row[RdxMath::RtGeoColour] = Rt->Colour | ((g == OwnBodyGeo[FrameIndex]) ? RdxMath::RtAlbedoOwnBody : 0u);
        appMemcpy(Row + RdxMath::RtGeoDensity, &Rt->Density, sizeof(FLOAT));
        Row[RdxMath::RtGeoAmbient] = Rt->Ambient;
        appMemcpy(Row + RdxMath::RtGeoScale, &Rt->Scale, sizeof(FLOAT));
        for (INT w = 0; w < RdxMath::RtCubeWays; w++)
            Row[RdxMath::RtGeoCube + w] = Rt->Cube[w];
        for (INT k = 0; k < RdxMath::RtCasterKeys; k++)
            Row[RdxMath::RtGeoTextures + k] = Rt->Texture[k];
    }
    return Table.Res->GetGPUVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS FRtPass::FillShadowExclusion(INT FrameIndex, const FShadowMaps& Shadow, const INT* Lights,
                                                       INT Count, FLOAT AoRadius, FLOAT AoStrength, INT TracedLights,
                                                       INT EdgeSamples, INT AoInstances)
{
    if (!bReady || !ExclusionMapped || FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT)
        return 0;
    // The table ListScene made room in: a uint a geometry, then a row of Words uints a light.
    const FTable& Table = ShadowTable[FrameIndex];
    const INT     Geos  = GeoCount[FrameIndex];
    const INT     Words = ShadowCasterWords(Geos);
    const UINT64  Bytes = ((UINT64)Geos + (UINT64)RdxMath::ShadowMaxLights * Words) * sizeof(DWORD);
    if (!Table.Mapped || Table.Bytes < Bytes)
        return 0;
    appMemzero(Table.Mapped, (SIZE_T)Bytes);
    // Each geometry's first vertex in its buffer, bit 31 for the arena's - where the shader finds a masked caster's
    // texture places.
    DWORD* const Places = (DWORD*)Table.Mapped;
    for (INT g = 0; g < Geos; g++)
    {
        D3D12_VERTEX_BUFFER_VIEW View;
        INT First = 0, Vertices = 0;
        const INT c = GeoCaster[FrameIndex][g];
        if (Shadow.GetCasterGeometry(c, View, First, Vertices))
            Places[g] = ((DWORD)First & 0x7FFFFFFFu) | (Shadow.IsCasterInArena(c) ? 0x80000000u : 0u);
    }
    DWORD* const Rows = Places + Geos;
    for (INT n = 0; Lights && n < Count && n < RdxMath::ShadowMaxLights; n++)
    {
        if (Lights[n] < 0 || Lights[n] >= Shadow.GetLightCount())
            continue;
        const FShadowLightFrame& L = Shadow.GetLight(Lights[n]);
        for (INT g = 0; L.Own && g < Geos; g++)
        {
            const INT c = GeoCaster[FrameIndex][g];
            if (L.Own[c >> 5] & (1u << (c & 31)))
                Rows[n * Words + (g >> 5)] |= 1u << (g & 31);
        }
    }
    DWORD* const Out = (DWORD*)(ExclusionMapped + (SIZE_T)EXCLUSION_STRIDE * FrameIndex);
    appMemzero(Out, EXCLUSION_STRIDE);
    FLOAT* const Ao = (FLOAT*)Out;
    Ao[0] = AoRadius;
    Ao[1] = AoStrength;
    DWORD* const Knobs = Out + 4;
    Knobs[0] = (DWORD)Max(TracedLights, 0);
    Knobs[1] = (DWORD)Max(EdgeSamples, 0);
    Knobs[2] = (DWORD)Max(AoInstances, 0);
    Knobs[3] = (OwnBodyGeo[FrameIndex] >= 0 && OwnBodyGeo[FrameIndex] < Geos) ? (DWORD)OwnBodyGeo[FrameIndex] : 0xFFFFFFFFu;
    // Where the lights' rows start in the table, in uints, and how long one is.
    DWORD* const Where = Knobs + 4;
    Where[0] = (DWORD)Geos;
    Where[1] = (DWORD)Words;
    return ExclusionCb->GetGPUVirtualAddress() + (UINT64)EXCLUSION_STRIDE * FrameIndex;
}
