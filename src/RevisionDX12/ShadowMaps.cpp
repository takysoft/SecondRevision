#include "Pch.h"
#include "Backend.h"
#include "Log.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// The faces are drawn with RdxMath::CubeFaceMatrix's rows as four rows of root constants - no matrix packing convention to
// get wrong. Depth only, no render target. The level's maps with no pixel shader (VSMain); the actors' with their texture
// places (VSActor) and a pixel shader that leaves out what a masked texture's mask leaves out (PSActor) - the masks' slot 0 is
// solid, so every face without a mask passes.
static const char* GShadowMapShader =
"cbuffer Face : register(b0) { float4 R0; float4 R1; float4 R2; float4 R3; };\n"
"Texture2DArray<float> Masks : register(t0);\n"
"SamplerState MaskSampler : register(s0);\n"
"\n"
"float4 VSMain(float3 P : POSITION) : SV_Position\n"
"{\n"
"    float4 Q = float4(P, 1.0f);\n"
"    return float4(dot(R0, Q), dot(R1, Q), dot(R2, Q), dot(R3, Q));\n"
"}\n"
"\n"
"struct ActorOut { float4 Pos : SV_Position; float3 Uvw : TEXCOORD0; };\n"
"ActorOut VSActor(float3 P : POSITION, float3 Uvw : TEXCOORD0)\n"
"{\n"
"    ActorOut Out;\n"
"    float4 Q = float4(P, 1.0f);\n"
"    Out.Pos = float4(dot(R0, Q), dot(R1, Q), dot(R2, Q), dot(R3, Q));\n"
"    Out.Uvw = Uvw;\n"
"    return Out;\n"
"}\n"
"\n"
"void PSActor(ActorOut In)\n"
"{\n"
"    clip(Masks.SampleLevel(MaskSampler, In.Uvw, 0.0f) - 0.5f);\n"
"}\n";

static UBOOL ShadowCheckResult(HRESULT Result, const TCHAR* What)
{
    if (SUCCEEDED(Result))
        return 1;
    RdxLog(TEXT("shadow map: %s failed, hr=0x%08X"), What, (DWORD)Result);
    return 0;
}
// The cube arrays are the biggest single thing this renderer asks a card for - 600 MB at High - so they are the
// most likely to be refused, and the path they take when refused is worth walking (RdxForcedFailure). Where a failure
// is forced the call is not made at all: a refusal leaves nothing behind, and a forced one has to leave the same.
#define ShadowCheck(Call, What) (RdxForcedFailure(What) ? 0 : ShadowCheckResult((Call), (What)))

static void Transition(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Resource, D3D12_RESOURCE_STATES& State,
                       D3D12_RESOURCE_STATES To)
{
    if (!Resource || State == To)
        return;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Resource;
    B.Transition.StateBefore = State;
    B.Transition.StateAfter  = To;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Cmd->ResourceBarrier(1, &B);
    State = To;
}

static UBOOL CreateUploadBuffer(ID3D12Device* Device, UINT64 Bytes, TComRef<ID3D12Resource>& Out, const TCHAR* What)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = Bytes;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return ShadowCheck(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr, __uuidof(ID3D12Resource), Out.VoidSlot()), What);
}

static void SetSquareViewport(ID3D12GraphicsCommandList* Cmd, INT Size)
{
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)Size, (FLOAT)Size, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, Size, Size };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
}

// The cells a sphere's box covers along one axis of the grid, clamped to the grid.
static inline void CellRange(FLOAT Centre, FLOAT Radius, FLOAT Min, FLOAT Size, INT Cells, INT& Lo, INT& Hi)
{
    Lo = Clamp(appFloor((Centre - Radius - Min) / Size), 0, Cells - 1);
    Hi = Clamp(appFloor((Centre + Radius - Min) / Size), 0, Cells - 1);
}

FShadowMaps::FShadowMaps()
{
    appMemzero(&Preset, sizeof(Preset));
    MaxLights           = 0;
    bReady              = 0;
    ActorState          = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    WorldState          = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    WorldVertexCount    = 0;
    RetiredAge          = 0;
    WorldSpheres        = nullptr;
    WorldTriangles      = 0;
    CellStart           = nullptr;
    CellTris            = nullptr;
    BigTris             = nullptr;
    BigCount            = 0;
    GridX = GridY = GridZ = GridCells = 0;
    GridMin[0] = GridMin[1] = GridMin[2] = 0.0f;
    CellSize            = 512.0f;
    Stamp               = nullptr;
    StampClock          = 0;
    Candidates          = nullptr;
    FaceMasks           = nullptr;
    ActorVertexCount    = 0;
    WorldIbUsed         = 0;
    FrameSlot           = 0;
    CasterCount         = 0;
    LightCount          = 0;
    UseClock            = 0;
    StatWorldRedraws    = 0;
    StatActorDraws      = 0;
    TotalWorldRedraws   = 0;
    TotalDeferred       = 0;
    StatStaleUsed       = 0;
    TotalStaleUsed      = 0;
    TotalWorldTriangles = 0;
    for (INT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        ActorMapped[i]    = nullptr;
        ActorUvwMapped[i] = nullptr;
        WorldIbMapped[i]  = nullptr;
        LookupMapped[i]   = nullptr;
    }
    MaskState       = D3D12_RESOURCE_STATE_COPY_DEST;
    MaskMapped      = nullptr;
    MaskCount       = 0;
    UnmaskableCount = 0;
    ArenaMapped     = nullptr;
    ArenaUvwMapped  = nullptr;
    ArenaUsed       = 0;
    appMemzero(MaskKeys, sizeof(MaskKeys));
    appMemzero(MaskDirty, sizeof(MaskDirty));
    appMemzero(Unmaskable, sizeof(Unmaskable));
    Dev           = nullptr;
    bListsRefused = 0;
    Casters       = nullptr;
    CasterRoom    = 0;
    NearCasters   = nullptr;
    NearFaces     = nullptr;
    LightBits     = nullptr;
    LightBitsRoom = 0;
    LightWords    = 0;
    ActorAsked    = 0;
    ActorWanted   = 0;
    appMemzero(ActorRoom, sizeof(ActorRoom));
    appMemzero(Lights, sizeof(Lights));
    appMemzero(LightSlot, sizeof(LightSlot));
    appMemzero(LightReady, sizeof(LightReady));
    appMemzero(LightState, sizeof(LightState));
    appMemzero(Slots, sizeof(Slots));
    for (INT s = 0; s < RdxMath::ShadowMaxLights; s++)
        Slots[s].ActorFaceUsed = ALL_FACES;
}

FShadowMaps::~FShadowMaps()
{
    Shutdown();
}

void FShadowMaps::RefuseLists(const TCHAR* Which, INT Wanted)
{
    if (!bListsRefused)
        RdxLog(TEXT("shadow maps: no memory for %s (%i wanted) - the shadows are left out from here"), Which, Wanted);
    bListsRefused = 1;
}

UBOOL FShadowMaps::HoldCasters(INT Wanted)
{
    if (Wanted <= CasterRoom)
        return 1;
    INT Room = Max(CasterRoom, (INT)FIRST_CASTERS);
    while (Room < Wanted)
        Room += Room / 2;
    FCaster* const NewCasters = RdxForcedFailure(TEXT("shadow lists")) ? nullptr
        : (FCaster*)appRealloc(Casters, (DWORD)Room * sizeof(FCaster), TEXT("RdxShadowCasters"));
    if (NewCasters)
        Casters = NewCasters;
    INT* const NewNear = NewCasters ? (INT*)appRealloc(NearCasters, (DWORD)Room * sizeof(INT), TEXT("RdxShadowNear"))
                                    : nullptr;
    if (NewNear)
        NearCasters = NewNear;
    BYTE* const NewFaces = NewNear ? (BYTE*)appRealloc(NearFaces, (DWORD)Room, TEXT("RdxShadowFaces")) : nullptr;
    if (NewFaces)
        NearFaces = NewFaces;
    if (!NewFaces)
    {
        RefuseLists(TEXT("the casters' list"), Wanted);
        return 0;
    }
    CasterRoom = Room;
    return 1;
}

UBOOL FShadowMaps::MakeActorBuffers(INT Slot, INT Vertices)
{
    if (!Dev || Slot < 0 || Slot >= BACKEND_FRAME_COUNT || Vertices <= 0)
        return 0;
    const UINT64 Bytes = (UINT64)Vertices * 3 * sizeof(FLOAT);
    TComRef<ID3D12Resource> Xyz, Uvw;
    FLOAT* XyzMapped = nullptr;
    FLOAT* UvwMapped = nullptr;
    D3D12_RANGE NoRead = { 0, 0 };
    if (RdxForcedFailure(TEXT("shadow lists")) ||
        !CreateUploadBuffer(Dev, Bytes, Xyz, TEXT("actor vertex buffer")) ||
        !ShadowCheck(Xyz->Map(0, &NoRead, (void**)&XyzMapped), TEXT("actor vertex buffer Map")) ||
        !CreateUploadBuffer(Dev, Bytes, Uvw, TEXT("actor texture-slot buffer")) ||
        !ShadowCheck(Uvw->Map(0, &NoRead, (void**)&UvwMapped), TEXT("actor texture-slot buffer Map")))
        return 0;
    // The new ones into the old ones' places: a reference each for the members, the locals let go of their own.
    Xyz->AddRef();
    *ActorVb[Slot].Slot() = Xyz.Get();
    Uvw->AddRef();
    *ActorUvwVb[Slot].Slot() = Uvw.Get();
    ActorMapped[Slot]    = XyzMapped;
    ActorUvwMapped[Slot] = UvwMapped;
    ActorRoom[Slot]      = Vertices;
    return 1;
}

UBOOL FShadowMaps::CreatePipeline(ID3D12Device* Device)
{
    // Root: the face's matrix (vertex shader), and a table with the masks' view (the actors' pixel shader), point-sampled.
    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = 1;
    Range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER Params[2];
    appMemzero(Params, sizeof(Params));
    Params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[0].Constants.ShaderRegister = 0;
    Params[0].Constants.Num32BitValues = 16;
    Params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
    Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[1].DescriptorTable.NumDescriptorRanges = 1;
    Params[1].DescriptorTable.pDescriptorRanges   = &Range;
    Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC Sampler;
    appMemzero(&Sampler, sizeof(Sampler));
    Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    Sampler.ShaderRegister   = 0;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 2;
    Desc.pParameters       = Params;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &Sampler;
    Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        !ShadowCheck(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                                 __uuidof(ID3D12RootSignature), Root.VoidSlot()),
                     TEXT("root signature")))
        return 0;

    TComRef<ID3DBlob> Vs, VsActor, PsActor;
    const SIZE_T Len = strlen(GShadowMapShader);
    if (FAILED(RdxCompile(GShadowMapShader, Len, "ShadowMap", nullptr, nullptr, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GShadowMapShader, Len, "ShadowMap", nullptr, nullptr, "VSActor", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, VsActor.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GShadowMapShader, Len, "ShadowMap", nullptr, nullptr, "PSActor", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, PsActor.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("shadow map: shader error: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    D3D12_INPUT_ELEMENT_DESC Elements[2];
    appMemzero(Elements, sizeof(Elements));
    Elements[0].SemanticName   = "POSITION";
    Elements[0].Format         = DXGI_FORMAT_R32G32B32_FLOAT;
    Elements[0].InputSlot      = 0;
    Elements[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    Elements[1].SemanticName   = "TEXCOORD";
    Elements[1].Format         = DXGI_FORMAT_R32G32B32_FLOAT;
    Elements[1].InputSlot      = 1;
    Elements[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature                  = Root.Get();
    Pso.VS.pShaderBytecode              = Vs->GetBufferPointer();
    Pso.VS.BytecodeLength               = Vs->GetBufferSize();
    Pso.InputLayout.pInputElementDescs  = Elements;
    Pso.InputLayout.NumElements         = 1;
    Pso.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets                = 0;
    Pso.DSVFormat                       = DXGI_FORMAT_D32_FLOAT;
    Pso.SampleDesc.Count                = 1;
    Pso.SampleMask                      = UINT_MAX;
    Pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    // Both sides: the level's polygons face the rooms, and a light may stand on either side of a mesh's face.
    Pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable = TRUE;
    // Reversed depth (RdxMath::CubeFaceMatrix): the nearest occluder has the largest value.
    Pso.DepthStencilState.DepthEnable    = TRUE;
    Pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    Pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER;
    if (!ShadowCheck(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), Pipeline.VoidSlot()),
                     TEXT("pipeline")))
        return 0;
    // The actors': their texture places from slot 1, and the masks' pixel shader.
    Pso.VS.pShaderBytecode     = VsActor->GetBufferPointer();
    Pso.VS.BytecodeLength      = VsActor->GetBufferSize();
    Pso.PS.pShaderBytecode     = PsActor->GetBufferPointer();
    Pso.PS.BytecodeLength      = PsActor->GetBufferSize();
    Pso.InputLayout.NumElements = 2;
    return ShadowCheck(Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState), ActorPipeline.VoidSlot()),
                       TEXT("actor pipeline"));
}

UBOOL FShadowMaps::CreateCubes(ID3D12Device* Device, INT Size, TComRef<ID3D12Resource>& Out, const TCHAR* What)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)Size;
    Desc.Height           = (UINT)Size;
    Desc.DepthOrArraySize = (UINT16)(6 * MaxLights);
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R32_TYPELESS;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format             = DXGI_FORMAT_D32_FLOAT;
    Clear.DepthStencil.Depth = 0.0f;
    return ShadowCheck(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                       &Clear, __uuidof(ID3D12Resource), Out.VoidSlot()), What);
}

UBOOL FShadowMaps::Init(ID3D12Device* Device, const RdxMath::FShadowPreset& InPreset)
{
    // ReleasePreset and NOT Shutdown: the level's geometry has to survive a change of preset. It arrives once, when
    // the level loads (SetWorld), and nothing hands it over again - so dropping it here left the world maps being
    // redrawn from NOTHING. Every one of them then came back empty, which reads as "no wall blocks this light", and
    // the level cast no shadows at all for the rest of the level. Measured: after one change in the renderer's menu
    // the level maps kept being drawn and the triangle count stopped dead at the number it had reached, the shadow
    // factor went white, and the compensation followed it down from +50% to +1%.
    ReleasePreset();
    Preset    = InPreset;
    MaxLights = (Preset.MaxLights < 0) ? 0 : ((Preset.MaxLights > RdxMath::ShadowMaxLights) ? (INT)RdxMath::ShadowMaxLights
                                                                                             : Preset.MaxLights);
    if (!Device || MaxLights <= 0 || Preset.ActorMapSize <= 0 || Preset.WorldMapSize <= 0)
        return 0;
    Dev           = Device;
    bListsRefused = 0;
    // A failure part way must leave nothing behind: the cube arrays alone are some 300 MB at High, and the mapped
    // buffers come out of a 32-bit process's address space.
    struct FUndo
    {
        FShadowMaps* Maps;
        // ReleasePreset, like Init's first line: a failed attempt must leave none of the preset behind - the cube
        // arrays alone are some 300 MB - but the level's geometry is what the caller's retry with the OLD preset
        // needs, and it is not ours to throw away here.
        ~FUndo() { if (Maps) Maps->ReleasePreset(); }
    } Undo = { this };

    if (!CreatePipeline(Device))
        return 0;
    if (!CreateCubes(Device, Preset.ActorMapSize, ActorCubes, TEXT("actor cubes")) ||
        !CreateCubes(Device, Preset.WorldMapSize, WorldCubes, TEXT("level cubes")))
        return 0;
    RdxViewsChanged();
    ActorState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    WorldState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HeapDesc.NumDescriptors = (UINT)(12 * MaxLights);
    if (!ShadowCheck(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), DsvHeap.VoidSlot()),
                     TEXT("DSV heap")))
        return 0;
    const UINT DsvInc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (INT Array = 0; Array < 2; Array++)
        for (INT Slice = 0; Slice < 6 * MaxLights; Slice++)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC View;
            appMemzero(&View, sizeof(View));
            View.Format                         = DXGI_FORMAT_D32_FLOAT;
            View.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            View.Texture2DArray.MipSlice        = 0;
            View.Texture2DArray.FirstArraySlice = (UINT)Slice;
            View.Texture2DArray.ArraySize       = 1;
            Device->CreateDepthStencilView((Array == 0) ? ActorCubes.Get() : WorldCubes.Get(), &View, Dsv);
            Dsv.ptr += DsvInc;
        }

    D3D12_RANGE NoRead = { 0, 0 };
    // The frame's vertex buffers at what the frames have asked for so far - a change of preset in a running game
    // keeps that - or at what they begin with.
    const INT ActorVertices = Max(ActorWanted + ActorWanted / 2, (INT)FIRST_ACTOR_VERTICES);
    for (INT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        if (!MakeActorBuffers(i, ActorVertices))
            return 0;
        if (!CreateUploadBuffer(Device, (UINT64)WORLD_INDICES * sizeof(UINT), WorldIb[i], TEXT("level index buffer")) ||
            !ShadowCheck(WorldIb[i]->Map(0, &NoRead, (void**)&WorldIbMapped[i]), TEXT("level index buffer Map")))
            return 0;
        if (!CreateUploadBuffer(Device, LOOKUP_CONSTANT_BYTES, LookupCb[i], TEXT("constant buffer")) ||
            !ShadowCheck(LookupCb[i]->Map(0, &NoRead, (void**)&LookupMapped[i]), TEXT("constant buffer Map")))
            return 0;
    }
    if (!HoldCasters(FIRST_CASTERS))
        return 0;
    if (!CreateMaskArray(Device))
        return 0;
    if (!CreateUploadBuffer(Device, (UINT64)ARENA_VERTICES * 3 * sizeof(FLOAT), ArenaVb, TEXT("shape arena")) ||
        !ShadowCheck(ArenaVb->Map(0, &NoRead, (void**)&ArenaMapped), TEXT("shape arena Map")) ||
        !CreateUploadBuffer(Device, (UINT64)ARENA_VERTICES * 3 * sizeof(FLOAT), ArenaUvwVb, TEXT("shape arena texture slots")) ||
        !ShadowCheck(ArenaUvwVb->Map(0, &NoRead, (void**)&ArenaUvwMapped), TEXT("shape arena texture slots Map")))
        return 0;
    ArenaUsed = 0;

    const DOUBLE MB = 4.0 * 6.0 * MaxLights *
                      ((DOUBLE)Preset.ActorMapSize * Preset.ActorMapSize + (DOUBLE)Preset.WorldMapSize * Preset.WorldMapSize) /
                      (1024.0 * 1024.0);
    RdxLog(TEXT("shadow maps: up - %i lights, actor cube %i, level cube %i (%.0f MB), room for %i casters' vertices a "
                "frame to begin with, %i level indices per frame, at most %i new level maps per frame"),
           MaxLights, Preset.ActorMapSize, Preset.WorldMapSize, MB, ActorVertices, (INT)WORLD_INDICES,
           (INT)WORLD_REDRAWS_PER_FRAME);
    Undo.Maps = nullptr;
    bReady    = 1;
    return 1;
}

// Everything the PRESET owns: the cube arrays sized by it, the pipelines drawn with it, the per-frame buffers and
// every slot's claim on a cube slice. Not the level's geometry - see ForgetWorld.
void FShadowMaps::ReleasePreset()
{
    bReady = 0;
    Dev    = nullptr;
    for (INT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        ActorVb[i].Release();
        ActorMapped[i] = nullptr;
        WorldIb[i].Release();
        WorldIbMapped[i] = nullptr;
        LookupCb[i].Release();
        LookupMapped[i] = nullptr;
        ActorUvwVb[i].Release();
        ActorUvwMapped[i] = nullptr;
        ActorRoom[i]      = 0;
    }
    MaskUpload.Release();
    MaskMapped = nullptr;
    MaskArray.Release();
    MaskState       = D3D12_RESOURCE_STATE_COPY_DEST;
    MaskCount       = 0;
    UnmaskableCount = 0;
    ActorPipeline.Release();
    ArenaVb.Release();
    ArenaUvwVb.Release();
    ArenaMapped    = nullptr;
    ArenaUvwMapped = nullptr;
    ArenaUsed      = 0;
    DsvHeap.Release();
    ActorCubes.Release();
    WorldCubes.Release();
    SpotMaps.Release();
    SpotDsvHeap.Release();
    bSpotsRefused = 0;
    RdxViewsChanged();          // the passes' views of the cubes go with them (ViewGeneration.h)
    Pipeline.Release();
    Root.Release();
    ActorVertexCount = 0;
    WorldIbUsed      = 0;
    CasterCount      = 0;
    LightCount       = 0;
    // The cube arrays are new, so no slot holds a drawn map any more - every light's world map has to be drawn
    // again, and bWorldValid going to zero with them is what makes that happen.
    appMemzero(Slots, sizeof(Slots));
    // And nothing has cleared the new cubes' faces yet (see FSlot::ActorFaceUsed).
    for (INT s = 0; s < RdxMath::ShadowMaxLights; s++)
        Slots[s].ActorFaceUsed = ALL_FACES;
    appMemzero(LightReady, sizeof(LightReady));
}

// The LEVEL's geometry: the vertices the world maps are drawn from and the grid that culls them. It belongs to the
// level, not to the preset, and the only things that may drop it are a new level (SetWorld) and a real teardown.
void FShadowMaps::ForgetWorld()
{
    WorldVb.Release();
    RetiredWorldVb.Release();
    WorldVertexCount = 0;
    RetiredAge       = 0;
    FreeWorldGrid();
}

void FShadowMaps::Shutdown()
{
    ReleasePreset();
    ForgetWorld();
    if (Casters)
        appFree(Casters);
    if (NearCasters)
        appFree(NearCasters);
    if (NearFaces)
        appFree(NearFaces);
    if (LightBits)
        appFree(LightBits);
    Casters       = nullptr;
    NearCasters   = nullptr;
    NearFaces     = nullptr;
    LightBits     = nullptr;
    CasterRoom    = 0;
    LightBitsRoom = 0;
    LightWords    = 0;
    ActorWanted   = 0;
}

void FShadowMaps::NameResources()
{
    if (ActorCubes)
        ActorCubes->SetName(L"ShadowActorCubes");
    if (WorldCubes)
        WorldCubes->SetName(L"ShadowWorldCubes");
    if (MaskArray)
        MaskArray->SetName(L"ShadowMasks");
}

void FShadowMaps::DescribeCubeView(D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const
{
    appMemzero(&Desc, sizeof(Desc));
    Desc.Format                               = DXGI_FORMAT_R32_FLOAT;
    Desc.ViewDimension                        = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    Desc.Shader4ComponentMapping              = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Desc.TextureCubeArray.MostDetailedMip     = 0;
    Desc.TextureCubeArray.MipLevels           = 1;
    Desc.TextureCubeArray.First2DArrayFace    = 0;
    Desc.TextureCubeArray.NumCubes            = (UINT)((MaxLights > 0) ? MaxLights : 1);
    Desc.TextureCubeArray.ResourceMinLODClamp = 0.0f;
}

void FShadowMaps::DescribeMaskView(D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const
{
    appMemzero(&Desc, sizeof(Desc));
    Desc.Format                         = DXGI_FORMAT_R8_UNORM;
    Desc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    Desc.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Desc.Texture2DArray.MostDetailedMip = 0;
    Desc.Texture2DArray.MipLevels       = 1;
    Desc.Texture2DArray.FirstArraySlice = 0;
    Desc.Texture2DArray.ArraySize       = (UINT)RdxMath::ShadowMaxMasks;
}

// The masks' array on the card, one byte a texel, and the upload buffer they reach it through - mapped for good. Slot 0 is
// written solid now and copied at the first Render.
UBOOL FShadowMaps::CreateMaskArray(ID3D12Device* Device)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)RdxMath::ShadowMaskSize;
    Desc.Height           = (UINT)RdxMath::ShadowMaskSize;
    Desc.DepthOrArraySize = (UINT16)RdxMath::ShadowMaxMasks;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R8_UNORM;
    Desc.SampleDesc.Count = 1;
    if (!ShadowCheck(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, __uuidof(ID3D12Resource), MaskArray.VoidSlot()),
                     TEXT("mask array")))
        return 0;
    MaskState = D3D12_RESOURCE_STATE_COPY_DEST;
    const INT    Texels = RdxMath::ShadowMaskSize * RdxMath::ShadowMaskSize;
    D3D12_RANGE  NoRead = { 0, 0 };
    if (!CreateUploadBuffer(Device, (UINT64)Texels * RdxMath::ShadowMaxMasks, MaskUpload, TEXT("mask upload")) ||
        !ShadowCheck(MaskUpload->Map(0, &NoRead, (void**)&MaskMapped), TEXT("mask upload Map")))
        return 0;
    ForgetMasks();
    appMemset(MaskMapped, 255, Texels);
    return 1;
}

void FShadowMaps::ForgetMasks()
{
    for (INT m = 0; m < RdxMath::ShadowMaxMasks; m++)
    {
        MaskKeys[m]  = nullptr;
        MaskDirty[m] = 0;
    }
    for (INT k = 0; k < MAX_UNMASKABLE; k++)
        Unmaskable[k] = nullptr;
    UnmaskableCount = 0;
    MaskCount       = 1;          // slot 0: solid
    MaskDirty[0]    = 1;
}

UBOOL FShadowMaps::GetCasterGeometry(INT Index, D3D12_VERTEX_BUFFER_VIEW& View, INT& First, INT& Count) const
{
    appMemzero(&View, sizeof(View));
    First = Count = 0;
    if (Index < 0 || Index >= CasterCount)
        return 0;
    const FCaster& C = Casters[Index];
    ID3D12Resource* const Vb = C.bArena ? ArenaVb.Get() : ActorVb[FrameSlot].Get();
    if (!Vb || C.Count < 3)
        return 0;
    View.BufferLocation = Vb->GetGPUVirtualAddress();
    View.SizeInBytes    = (UINT)((C.bArena ? (INT)ARENA_VERTICES : ActorRoom[FrameSlot]) * 3 * sizeof(FLOAT));
    View.StrideInBytes  = 3 * sizeof(FLOAT);
    First = C.First;
    Count = C.Count;
    return 1;
}

const FLOAT* FShadowMaps::GetCasterPositions(INT Index) const
{
    if (Index < 0 || Index >= CasterCount)
        return nullptr;
    const FCaster&     C    = Casters[Index];
    const FLOAT* const Base = C.bArena ? ArenaMapped : ActorMapped[FrameSlot];
    return Base ? Base + (SIZE_T)C.First * 3 : nullptr;
}

void FShadowMaps::GetCasterSphere(INT Index, FLOAT* Out) const
{
    Out[0] = Out[1] = Out[2] = Out[3] = 0.0f;
    if (Index < 0 || Index >= CasterCount)
        return;
    Out[0] = Casters[Index].X;
    Out[1] = Casters[Index].Y;
    Out[2] = Casters[Index].Z;
    Out[3] = Casters[Index].R;
}

INT FShadowMaps::FindMask(void* Key) const
{
    if (!Key)
        return -2;
    for (INT m = 1; m < MaskCount; m++)
        if (MaskKeys[m] == Key)
            return m;
    for (INT k = 0; k < UnmaskableCount; k++)
        if (Unmaskable[k] == Key)
            return -2;
    return -1;
}

INT FShadowMaps::AddMask(void* Key, const BYTE* Coverage)
{
    if (!Key || !MaskMapped)
        return -2;
    if (!Coverage || MaskCount >= RdxMath::ShadowMaxMasks)
    {
        if (UnmaskableCount < MAX_UNMASKABLE)
            Unmaskable[UnmaskableCount++] = Key;
        return -2;
    }
    const INT M = MaskCount++;
    const INT Texels = RdxMath::ShadowMaskSize * RdxMath::ShadowMaskSize;
    appMemcpy(MaskMapped + (SIZE_T)M * Texels, Coverage, Texels);
    MaskKeys[M]  = Key;
    MaskDirty[M] = 1;
    return M;
}

void FShadowMaps::FreeWorldGrid()
{
    if (WorldSpheres) { appFree(WorldSpheres); WorldSpheres = nullptr; }
    if (CellStart)    { appFree(CellStart);    CellStart    = nullptr; }
    if (CellTris)     { appFree(CellTris);     CellTris     = nullptr; }
    if (BigTris)      { appFree(BigTris);      BigTris      = nullptr; }
    if (Stamp)        { appFree(Stamp);        Stamp        = nullptr; }
    if (Candidates)   { appFree(Candidates);   Candidates   = nullptr; }
    if (FaceMasks)    { appFree(FaceMasks);    FaceMasks    = nullptr; }
    WorldTriangles = 0;
    BigCount       = 0;
    GridX = GridY = GridZ = GridCells = 0;
    StampClock     = 0;
}

// The grid: cells of 512 units over the level's box - doubled until there are at most 65536 of them. A triangle is listed in
// every cell its sphere's box covers; one covering more than 64 cells goes to BigTris instead.
UBOOL FShadowMaps::BuildWorldGrid(const FLOAT* Xyz, INT Triangles)
{
    FreeWorldGrid();
    if (!Xyz || Triangles <= 0)
        return 1;
    WorldSpheres = (FLOAT*)appMalloc(Triangles * 4 * sizeof(FLOAT), TEXT("RdxShadowSpheres"));
    Stamp        = (INT*)appMalloc(Triangles * sizeof(INT), TEXT("RdxShadowStamp"));
    Candidates   = (INT*)appMalloc(Triangles * sizeof(INT), TEXT("RdxShadowCandidates"));
    FaceMasks    = (BYTE*)appMalloc(Triangles * sizeof(BYTE), TEXT("RdxShadowFaceMasks"));
    if (!WorldSpheres || !Stamp || !Candidates || !FaceMasks)
    {
        FreeWorldGrid();
        return 0;
    }
    appMemzero(Stamp, Triangles * sizeof(INT));
    WorldTriangles = Triangles;

    FLOAT BoxMin[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
    FLOAT BoxMax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
    for (INT t = 0; t < Triangles; t++)
    {
        const FLOAT* const V = Xyz + t * 9;
        const FLOAT C[3] = { (V[0] + V[3] + V[6]) / 3.0f, (V[1] + V[4] + V[7]) / 3.0f, (V[2] + V[5] + V[8]) / 3.0f };
        FLOAT R2 = 0.0f;
        for (INT k = 0; k < 3; k++)
        {
            const FLOAT DX = V[k * 3] - C[0], DY = V[k * 3 + 1] - C[1], DZ = V[k * 3 + 2] - C[2];
            const FLOAT D2 = DX * DX + DY * DY + DZ * DZ;
            if (D2 > R2)
                R2 = D2;
        }
        FLOAT* const S = WorldSpheres + t * 4;
        S[0] = C[0];
        S[1] = C[1];
        S[2] = C[2];
        S[3] = appSqrt(R2) + 0.5f;
        for (INT a = 0; a < 3; a++)
        {
            if (C[a] - S[3] < BoxMin[a]) BoxMin[a] = C[a] - S[3];
            if (C[a] + S[3] > BoxMax[a]) BoxMax[a] = C[a] + S[3];
        }
    }

    CellSize = 512.0f;
    for (;;)
    {
        GridX = Max(1, appCeil((BoxMax[0] - BoxMin[0]) / CellSize));
        GridY = Max(1, appCeil((BoxMax[1] - BoxMin[1]) / CellSize));
        GridZ = Max(1, appCeil((BoxMax[2] - BoxMin[2]) / CellSize));
        if ((SQWORD)GridX * GridY * GridZ <= 65536)
            break;
        CellSize *= 2.0f;
    }
    GridCells  = GridX * GridY * GridZ;
    GridMin[0] = BoxMin[0];
    GridMin[1] = BoxMin[1];
    GridMin[2] = BoxMin[2];

    CellStart = (INT*)appMalloc((GridCells + 1) * sizeof(INT), TEXT("RdxShadowCells"));
    INT* const Fill = (INT*)appMalloc(GridCells * sizeof(INT), TEXT("RdxShadowFill"));
    if (!CellStart || !Fill)
    {
        if (Fill)
            appFree(Fill);
        FreeWorldGrid();
        return 0;
    }
    appMemzero(CellStart, (GridCells + 1) * sizeof(INT));
    for (INT t = 0; t < Triangles; t++)
    {
        const FLOAT* const S = WorldSpheres + t * 4;
        INT X0, X1, Y0, Y1, Z0, Z1;
        CellRange(S[0], S[3], GridMin[0], CellSize, GridX, X0, X1);
        CellRange(S[1], S[3], GridMin[1], CellSize, GridY, Y0, Y1);
        CellRange(S[2], S[3], GridMin[2], CellSize, GridZ, Z0, Z1);
        if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > 64)
        {
            BigCount++;
            continue;
        }
        for (INT z = Z0; z <= Z1; z++)
            for (INT y = Y0; y <= Y1; y++)
                for (INT x = X0; x <= X1; x++)
                    CellStart[(z * GridY + y) * GridX + x + 1]++;
    }
    for (INT c = 0; c < GridCells; c++)
        CellStart[c + 1] += CellStart[c];
    const INT Refs = CellStart[GridCells];
    CellTris = (INT*)appMalloc(Max(Refs, 1) * sizeof(INT), TEXT("RdxShadowCellTris"));
    BigTris  = (INT*)appMalloc(Max(BigCount, 1) * sizeof(INT), TEXT("RdxShadowBigTris"));
    if (!CellTris || !BigTris)
    {
        appFree(Fill);
        FreeWorldGrid();
        return 0;
    }
    appMemcpy(Fill, CellStart, GridCells * sizeof(INT));
    INT Big = 0;
    for (INT t = 0; t < Triangles; t++)
    {
        const FLOAT* const S = WorldSpheres + t * 4;
        INT X0, X1, Y0, Y1, Z0, Z1;
        CellRange(S[0], S[3], GridMin[0], CellSize, GridX, X0, X1);
        CellRange(S[1], S[3], GridMin[1], CellSize, GridY, Y0, Y1);
        CellRange(S[2], S[3], GridMin[2], CellSize, GridZ, Z0, Z1);
        if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > 64)
        {
            BigTris[Big++] = t;
            continue;
        }
        for (INT z = Z0; z <= Z1; z++)
            for (INT y = Y0; y <= Y1; y++)
                for (INT x = X0; x <= X1; x++)
                    CellTris[Fill[(z * GridY + y) * GridX + x]++] = t;
    }
    appFree(Fill);
    RdxLog(TEXT("shadow maps: the level grid is %ix%ix%i, cell %.0f units, %i references, %i large triangles (%.1f MB)"),
           GridX, GridY, GridZ, CellSize, Refs, BigCount,
           ((DOUBLE)Triangles * 3 * sizeof(INT) + (DOUBLE)Triangles * 4 * sizeof(FLOAT) + (DOUBLE)(GridCells + 1 + Refs + BigCount) *
            sizeof(INT)) / (1024.0 * 1024.0));
    return 1;
}

INT FShadowMaps::CollectWorldTriangles(const RdxMath::FVec3& Light, FLOAT Radius)
{
    if (!WorldSpheres || !CellStart || !CellTris)
        return 0;
    if (++StampClock >= 0x7FFFFFF0)
    {
        appMemzero(Stamp, WorldTriangles * sizeof(INT));
        StampClock = 1;
    }
    INT N = 0;
    INT X0, X1, Y0, Y1, Z0, Z1;
    CellRange(Light.X, Radius, GridMin[0], CellSize, GridX, X0, X1);
    CellRange(Light.Y, Radius, GridMin[1], CellSize, GridY, Y0, Y1);
    CellRange(Light.Z, Radius, GridMin[2], CellSize, GridZ, Z0, Z1);
    for (INT z = Z0; z <= Z1; z++)
        for (INT y = Y0; y <= Y1; y++)
            for (INT x = X0; x <= X1; x++)
            {
                const INT c = (z * GridY + y) * GridX + x;
                for (INT k = CellStart[c]; k < CellStart[c + 1]; k++)
                {
                    const INT t = CellTris[k];
                    if (Stamp[t] == StampClock)
                        continue;
                    Stamp[t] = StampClock;
                    const FLOAT* const S = WorldSpheres + t * 4;
                    const FLOAT DX = S[0] - Light.X, DY = S[1] - Light.Y, DZ = S[2] - Light.Z, R = Radius + S[3];
                    if (DX * DX + DY * DY + DZ * DZ < R * R)
                        Candidates[N++] = t;
                }
            }
    for (INT b = 0; b < BigCount; b++)
    {
        const INT t = BigTris[b];
        const FLOAT* const S = WorldSpheres + t * 4;
        const FLOAT DX = S[0] - Light.X, DY = S[1] - Light.Y, DZ = S[2] - Light.Z, R = Radius + S[3];
        if (Stamp[t] != StampClock && DX * DX + DY * DY + DZ * DZ < R * R)
        {
            Stamp[t] = StampClock;
            Candidates[N++] = t;
        }
    }
    return N;
}

UBOOL FShadowMaps::SetWorld(ID3D12Device* Device, const FLOAT* Xyz, INT VertexCount)
{
    // The GPU may still be drawing a frame with the old buffer: it goes to the retired slot, released a few frames later.
    if (WorldVb)
    {
        RetiredWorldVb.Release();
        ID3D12Resource* Old = WorldVb.Get();
        Old->AddRef();
        WorldVb.Release();
        *RetiredWorldVb.Slot() = Old;
        RetiredAge = 0;
    }
    WorldVertexCount    = 0;
    TotalWorldRedraws   = 0;
    TotalDeferred       = 0;
    TotalStaleUsed      = 0;
    TotalWorldTriangles = 0;
    FreeWorldGrid();
    for (INT s = 0; s < RdxMath::ShadowMaxLights; s++)
    {
        Slots[s].Key           = nullptr;
        Slots[s].bWorldValid   = 0;
        // The cubes' contents belong to the level being forgotten: no face may count as clean any more, or the first
        // frame of the next level would skip a clear it needs (see FSlot::ActorFaceUsed).
        Slots[s].ActorFaceUsed = ALL_FACES;
    }
    if (MaskMapped)
        ForgetMasks();
    ArenaUsed = 0;
    if (!Device || !Xyz || VertexCount <= 0 || !IsReady())
        return 1;

    const UINT64 Bytes = (UINT64)VertexCount * 3 * sizeof(FLOAT);
    if (!CreateUploadBuffer(Device, Bytes, WorldVb, TEXT("level vertex buffer")))
        return 0;
    D3D12_RANGE NoRead = { 0, 0 };
    BYTE* Dest = nullptr;
    if (!ShadowCheck(WorldVb->Map(0, &NoRead, (void**)&Dest), TEXT("level vertex buffer Map")))
    {
        WorldVb.Release();
        return 0;
    }
    appMemcpy(Dest, Xyz, (INT)Bytes);
    WorldVb->Unmap(0, nullptr);
    WorldVertexCount = VertexCount;
    // Without the grid every world map is drawn with the whole level, as before.
    if (!BuildWorldGrid(Xyz, VertexCount / 3))
        RdxLog(TEXT("shadow maps: the level grid could NOT be built - level maps will be drawn from the whole level"));
    return 1;
}

void FShadowMaps::BeginFrame(INT FrameIndex)
{
    FrameSlot        = (FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? FrameIndex : 0;
    CasterCount      = 0;
    ActorVertexCount = 0;
    WorldIbUsed      = 0;
    LightCount       = 0;
    // What the frame before asked for, and the slot's buffers made to hold the most a frame has: half as much again,
    // so that a scene growing a caster at a time does not make them anew at every one.
    ActorWanted   = Max(ActorWanted, ActorAsked);
    ActorAsked    = 0;
    bListsRefused = 0;
    if (bReady && ActorWanted > ActorRoom[FrameSlot])
    {
        const INT Was = ActorRoom[FrameSlot];
        if (MakeActorBuffers(FrameSlot, ActorWanted + ActorWanted / 2))
            RdxLog(TEXT("shadow maps: a frame's casters asked for %i vertices - frame slot %i now holds %i (it held "
                        "%i)"), ActorWanted, FrameSlot, ActorRoom[FrameSlot], Was);
        else
        {
            // Asked again only when a frame asks for more again: not every frame while the shadows are left out.
            RefuseLists(TEXT("the frame's vertex buffers"), ActorWanted);
            ActorWanted = ActorRoom[FrameSlot];
        }
    }
    // BACKEND_FRAME_COUNT frames are in flight at most: a frame after that the old level's buffer is nobody's.
    if (RetiredWorldVb && ++RetiredAge >= BACKEND_FRAME_COUNT + 1)
        RetiredWorldVb.Release();
}

FLOAT* FShadowMaps::AddCaster(INT VertexCount, FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, INT& Index)
{
    Index = -1;
    const FLOAT* const Buffer = ActorMapped[FrameSlot];
    if (!Buffer || VertexCount <= 0)
        return nullptr;
    ActorAsked += VertexCount;
    if (ActorVertexCount + VertexCount > ActorRoom[FrameSlot] || !HoldCasters(CasterCount + 1))
        return nullptr;
    FCaster& C = Casters[CasterCount];
    C.First = ActorVertexCount;
    C.Count = VertexCount;
    C.X     = X;
    C.Y     = Y;
    C.Z     = Z;
    C.R     = Radius;
    C.bArena = 0;
    C.bMasked = 0;
    C.bOwnBody = 0;
    appMemzero(&C.Rt, sizeof(C.Rt));
    C.Rt.Colour = 0x808080u;
    Index = CasterCount++;
    FLOAT* const Out = ActorMapped[FrameSlot] + ActorVertexCount * 3;
    ActorVertexCount += VertexCount;
    return Out;
}

FLOAT* FShadowMaps::CasterUvw(INT Index)
{
    if (Index < 0 || Index >= CasterCount || !ActorUvwMapped[FrameSlot] || Casters[Index].bArena)
        return nullptr;
    return ActorUvwMapped[FrameSlot] + Casters[Index].First * 3;
}

UBOOL FShadowMaps::ArenaAlloc(INT VertexCount, INT& Offset, FLOAT*& Xyz, FLOAT*& Uvw)
{
    Offset = -1;
    Xyz    = nullptr;
    Uvw    = nullptr;
    if (!ArenaMapped || !ArenaUvwMapped || VertexCount <= 0 || ArenaUsed + VertexCount > ARENA_VERTICES)
        return 0;
    Offset     = ArenaUsed;
    Xyz        = ArenaMapped + ArenaUsed * 3;
    Uvw        = ArenaUvwMapped + ArenaUsed * 3;
    ArenaUsed += VertexCount;
    return 1;
}

UBOOL FShadowMaps::AddArenaCaster(INT Offset, INT VertexCount, FLOAT X, FLOAT Y, FLOAT Z, FLOAT Radius, INT& Index)
{
    Index = -1;
    if (!ArenaMapped || VertexCount <= 0 || Offset < 0 || Offset + VertexCount > ArenaUsed ||
        !HoldCasters(CasterCount + 1))
        return 0;
    FCaster& C = Casters[CasterCount];
    C.First  = Offset;
    C.Count  = VertexCount;
    C.X      = X;
    C.Y      = Y;
    C.Z      = Z;
    C.R      = Radius;
    C.bArena = 1;
    C.bMasked = 0;
    C.bOwnBody = 0;
    appMemzero(&C.Rt, sizeof(C.Rt));
    C.Rt.Colour = 0x808080u;
    Index = CasterCount++;
    return 1;
}

D3D12_GPU_VIRTUAL_ADDRESS FShadowMaps::GetCasterUvw(UBOOL bArena) const
{
    ID3D12Resource* const Vb = bArena ? ArenaUvwVb.Get() : ActorUvwVb[FrameSlot].Get();
    return Vb ? Vb->GetGPUVirtualAddress() : 0;
}

ID3D12Resource* FShadowMaps::GetCasterBuffer(UBOOL bArena, UBOOL bUvw) const
{
    if (bArena)
        return bUvw ? ArenaUvwVb.Get() : ArenaVb.Get();
    return bUvw ? ActorUvwVb[FrameSlot].Get() : ActorVb[FrameSlot].Get();
}

ID3D12Resource* FShadowMaps::GetLookupBuffer(INT FrameIndex) const
{
    return LookupCb[(FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? FrameIndex : 0].Get();
}

void FShadowMaps::CopyMasks(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To)
{
    if (!Cmd || !To || !MaskArray)
        return;
    const D3D12_RESOURCE_STATES Was = MaskState;
    Transition(Cmd, MaskArray.Get(), MaskState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Cmd->CopyResource(To, MaskArray.Get());
    Transition(Cmd, MaskArray.Get(), MaskState, Was);
}

void FShadowMaps::SetLights(const FShadowLightFrame* In, INT Count)
{
    LightCount = (Count < 0 || !In) ? 0 : ((Count > MaxLights) ? MaxLights : Count);
    UseClock++;
    UBOOL Taken[RdxMath::ShadowMaxLights];
    appMemzero(Taken, sizeof(Taken));
    appMemzero(LightReady, sizeof(LightReady));

    // The lights' bits, copied: two rows a light, as long as this frame's casters ask. No room for them: no light
    // this frame, and the caller is told (ListsRefused).
    LightWords = GetCasterWords();
    const INT BitsWanted = LightCount * 2 * LightWords;
    if (BitsWanted > LightBitsRoom)
    {
        const INT    Room  = BitsWanted + BitsWanted / 2;
        DWORD* const Grown = RdxForcedFailure(TEXT("shadow lists")) ? nullptr
            : (DWORD*)appRealloc(LightBits, (DWORD)Room * sizeof(DWORD), TEXT("RdxShadowBits"));
        if (!Grown)
        {
            RefuseLists(TEXT("the lights' lists of casters"), BitsWanted);
            LightCount = 0;
            return;
        }
        LightBits     = Grown;
        LightBitsRoom = Room;
    }

    // A light chosen before keeps its slot - and its world map.
    for (INT i = 0; i < LightCount; i++)
    {
        Lights[i]    = In[i];
        DWORD* const Rows = LightBits + i * 2 * LightWords;
        for (INT w = 0; w < LightWords; w++)
        {
            Rows[w]              = In[i].Exclude ? In[i].Exclude[w] : 0u;
            Rows[LightWords + w] = In[i].Own ? In[i].Own[w] : 0u;
        }
        Lights[i].Exclude = Rows;
        Lights[i].Own     = Rows + LightWords;
        LightSlot[i] = -1;
        for (INT s = 0; s < MaxLights; s++)
            if (!Taken[s] && Slots[s].Key && Slots[s].Key == In[i].Key)
            {
                LightSlot[i] = s;
                Taken[s]     = 1;
                break;
            }
    }
    // A new one takes the free slot unused the longest. There are as many slots as lights at most.
    for (INT i = 0; i < LightCount; i++)
    {
        if (LightSlot[i] >= 0)
            continue;
        INT Best = -1;
        for (INT s = 0; s < MaxLights; s++)
            if (!Taken[s] && (Best < 0 || Slots[s].LastUsed < Slots[Best].LastUsed))
                Best = s;
        LightSlot[i]            = Best;
        Taken[Best]             = 1;
        Slots[Best].Key         = In[i].Key;
        Slots[Best].bWorldValid = 0;
        // The slot has changed hands: whatever the previous light left in the actor cube's faces is not this light's,
        // and the face-skipping in Render would take the old light's faces for clean ones (see FSlot::ActorFaceUsed).
        Slots[Best].ActorFaceUsed = ALL_FACES;
    }
    // How each light's world map stands (RdxMath::ClassifyWorldMap): measured against where the map was actually DRAWN
    // from - which Render records - and not against the previous frame. Comparing frame to frame, a light drifting less
    // than the tolerance each frame never counts as having moved, however far it ends up.
    for (INT i = 0; i < LightCount; i++)
    {
        FSlot&                   S = Slots[LightSlot[i]];
        const FShadowLightFrame& L = Lights[i];
        RdxMath::FVec3 DrawnAt, Now;
        DrawnAt.X = S.X;
        DrawnAt.Y = S.Y;
        DrawnAt.Z = S.Z;
        Now.X     = L.World[0];
        Now.Y     = L.World[1];
        Now.Z     = L.World[2];
        LightState[i] = RdxMath::ClassifyWorldMap(S.bWorldValid != 0, DrawnAt, S.R, Now, L.Radius);
        S.LastUsed    = UseClock;
    }
}

void FShadowMaps::SetFace(ID3D12GraphicsCommandList* Cmd, INT Face, const RdxMath::FVec3& Light) const
{
    FLOAT M[16];
    RdxMath::CubeFaceMatrix(Face, Light, GetNear(), M);
    Cmd->SetGraphicsRoot32BitConstants(0, 16, M, 0);
}

UBOOL FShadowMaps::EnsureSpots(ID3D12Device* Device)
{
    if (SpotMaps && SpotDsvHeap)
        return 1;
    if (bSpotsRefused || !Device || !bReady)
        return 0;
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)RdxMath::OwnLampMapSize;
    Desc.Height           = (UINT)RdxMath::OwnLampMapSize;
    Desc.DepthOrArraySize = (UINT16)RdxMath::OwnLampMost;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R32_TYPELESS;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE Clear;
    appMemzero(&Clear, sizeof(Clear));
    Clear.Format             = DXGI_FORMAT_D32_FLOAT;
    Clear.DepthStencil.Depth = 0.0f;
    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
    appMemzero(&HeapDesc, sizeof(HeapDesc));
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HeapDesc.NumDescriptors = (UINT)RdxMath::OwnLampMost;
    if (!ShadowCheck(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                     D3D12_RESOURCE_STATE_DEPTH_WRITE, &Clear, __uuidof(ID3D12Resource),
                                                     SpotMaps.VoidSlot()), TEXT("the own lamps' maps")) ||
        !ShadowCheck(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), SpotDsvHeap.VoidSlot()),
                     TEXT("the own lamps' DSV heap")))
    {
        SpotMaps.Release();
        SpotDsvHeap.Release();
        bSpotsRefused = 1;
        RdxLog(TEXT("own lamps: the card refused their maps - no weapon lamp lights anything (Flashlight runs as "
                    "Fixed)"));
        return 0;
    }
    SpotState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    const UINT DsvInc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv = SpotDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (INT Slice = 0; Slice < (INT)RdxMath::OwnLampMost; Slice++)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC View;
        appMemzero(&View, sizeof(View));
        View.Format                         = DXGI_FORMAT_D32_FLOAT;
        View.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        View.Texture2DArray.FirstArraySlice = (UINT)Slice;
        View.Texture2DArray.ArraySize       = 1;
        Device->CreateDepthStencilView(SpotMaps.Get(), &View, Dsv);
        Dsv.ptr += DsvInc;
    }
    SpotsMadeCount++;
    RdxViewsChanged();
    RdxLog(TEXT("own lamps: %i maps of %i texels a side (%.0f MB)"), (INT)RdxMath::OwnLampMost,
           (INT)RdxMath::OwnLampMapSize,
           4.0 * RdxMath::OwnLampMost * (DOUBLE)RdxMath::OwnLampMapSize * RdxMath::OwnLampMapSize / (1024.0 * 1024.0));
    return 1;
}

void FShadowMaps::DescribeSpotView(D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const
{
    appMemzero(&Desc, sizeof(Desc));
    Desc.Format                         = DXGI_FORMAT_R32_FLOAT;
    Desc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    Desc.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Desc.Texture2DArray.MipLevels       = 1;
    Desc.Texture2DArray.FirstArraySlice = 0;
    Desc.Texture2DArray.ArraySize       = (UINT)RdxMath::OwnLampMost;
}

void FShadowMaps::RenderSpots(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* SrvHeap,
                              D3D12_GPU_DESCRIPTOR_HANDLE MaskSrv, const FSpot* Spots, INT Count)
{
    if (!IsReady() || !Cmd || !Spots || Count <= 0 || !SpotMaps || !SpotDsvHeap || !SrvHeap || !MaskArray ||
        !MaskUpload || !Dev)
        return;
    Count = Min(Count, (INT)RdxMath::OwnLampMost);
    UploadMasks(Cmd);
    Transition(Cmd, SpotMaps.Get(), SpotState, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    ID3D12DescriptorHeap* Heaps[1] = { SrvHeap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetGraphicsRootDescriptorTable(1, MaskSrv);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    SetSquareViewport(Cmd, (INT)RdxMath::OwnLampMapSize);
    const UINT DsvInc = Dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_VERTEX_BUFFER_VIEW WorldView;
    appMemzero(&WorldView, sizeof(WorldView));
    if (WorldVb)
    {
        WorldView.BufferLocation = WorldVb->GetGPUVirtualAddress();
        WorldView.SizeInBytes    = (UINT)(WorldVertexCount * 3 * sizeof(FLOAT));
        WorldView.StrideInBytes  = 3 * sizeof(FLOAT);
    }
    D3D12_INDEX_BUFFER_VIEW IndexView;
    appMemzero(&IndexView, sizeof(IndexView));
    if (WorldIb[FrameSlot])
    {
        IndexView.BufferLocation = WorldIb[FrameSlot]->GetGPUVirtualAddress();
        IndexView.SizeInBytes    = (UINT)(WORLD_INDICES * sizeof(UINT));
        IndexView.Format         = DXGI_FORMAT_R32_UINT;
    }
    D3D12_VERTEX_BUFFER_VIEW ActorView;
    appMemzero(&ActorView, sizeof(ActorView));
    ActorView.BufferLocation = ActorVb[FrameSlot]->GetGPUVirtualAddress();
    ActorView.SizeInBytes    = (UINT)(ActorRoom[FrameSlot] * 3 * sizeof(FLOAT));
    ActorView.StrideInBytes  = 3 * sizeof(FLOAT);
    D3D12_VERTEX_BUFFER_VIEW UvwView = ActorView;
    UvwView.BufferLocation = ActorUvwVb[FrameSlot]->GetGPUVirtualAddress();
    const D3D12_VERTEX_BUFFER_VIEW ActorViews[2] = { ActorView, UvwView };
    D3D12_VERTEX_BUFFER_VIEW ArenaView = ActorView;
    ArenaView.BufferLocation = ArenaVb ? ArenaVb->GetGPUVirtualAddress() : 0;
    ArenaView.SizeInBytes    = (UINT)(ARENA_VERTICES * 3 * sizeof(FLOAT));
    D3D12_VERTEX_BUFFER_VIEW ArenaUvwView = ArenaView;
    ArenaUvwView.BufferLocation = ArenaUvwVb ? ArenaUvwVb->GetGPUVirtualAddress() : 0;
    const D3D12_VERTEX_BUFFER_VIEW ArenaViews[2] = { ArenaView, ArenaUvwView };

    // A map sees a square of OwnLampMapTan to either side: what reaches into the cone round its corners may be in it.
    const FLOAT Diag = RdxMath::OwnLampMapTan * 1.41421356f;
    const FLOAT Cos  = 1.0f / RdxMath::Sqrt(1.0f + Diag * Diag);
    const FLOAT Sin  = Diag * Cos;
    for (INT i = 0; i < Count; i++)
    {
        const FSpot&   S   = Spots[i];
        RdxMath::FVec3 Pos = { S.At[0], S.At[1], S.At[2] };
        RdxMath::FVec3 Aim = { S.Aim[0], S.Aim[1], S.Aim[2] };
        D3D12_CPU_DESCRIPTOR_HANDLE Dsv = SpotDsvHeap->GetCPUDescriptorHandleForHeapStart();
        Dsv.ptr += (SIZE_T)i * DsvInc;
        Cmd->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
        Cmd->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        Cmd->SetGraphicsRoot32BitConstants(0, 16, S.Matrix, 0);

        // The level: its triangles within the reach whose spheres reach into the map, into this frame's index buffer
        // after what is there; where they do not fit, the whole level.
        if (WorldVertexCount > 0 && WorldVb)
        {
            Cmd->SetPipelineState(Pipeline.Get());
            Cmd->IASetVertexBuffers(0, 1, &WorldView);
            const INT First = WorldIbUsed;
            INT       Taken = 0;
            UBOOL     bIndexed = 0;
            if (WorldIbMapped[FrameSlot] && WorldSpheres && CellStart)
            {
                const INT N = CollectWorldTriangles(Pos, RdxMath::OwnLampReach);
                if (First + N * 3 <= WORLD_INDICES)
                {
                    UINT* const Out = WorldIbMapped[FrameSlot] + First;
                    for (INT n = 0; n < N; n++)
                    {
                        const FLOAT* const Sp = WorldSpheres + Candidates[n] * 4;
                        const RdxMath::FVec3 C = { Sp[0], Sp[1], Sp[2] };
                        if (!RdxMath::SphereTouchesSpot(Pos, Aim, Cos, Sin, RdxMath::OwnLampReach, C, Sp[3]))
                            continue;
                        const UINT Base = (UINT)(Candidates[n] * 3);
                        Out[Taken++] = Base;
                        Out[Taken++] = Base + 1;
                        Out[Taken++] = Base + 2;
                    }
                    WorldIbUsed += Taken;
                    bIndexed = 1;
                }
            }
            if (bIndexed)
            {
                if (Taken > 0)
                {
                    Cmd->IASetIndexBuffer(&IndexView);
                    Cmd->DrawIndexedInstanced((UINT)Taken, 1, (UINT)First, 0, 0);
                    TotalWorldTriangles += Taken / 3;
                }
            }
            else
            {
                Cmd->DrawInstanced((UINT)WorldVertexCount, 1, 0, 0);
                TotalWorldTriangles += WorldVertexCount / 3;
            }
        }

        // The casters in it, but none round the lamp itself.
        Cmd->SetPipelineState(ActorPipeline.Get());
        INT BoundArena = -1;
        for (INT c = 0; c < CasterCount; c++)
        {
            const FCaster& K = Casters[c];
            const FLOAT DX = K.X - Pos.X, DY = K.Y - Pos.Y, DZ = K.Z - Pos.Z;
            if (DX * DX + DY * DY + DZ * DZ <= K.R * K.R)
                continue;
            const RdxMath::FVec3 C = { K.X, K.Y, K.Z };
            if (!RdxMath::SphereTouchesSpot(Pos, Aim, Cos, Sin, RdxMath::OwnLampReach, C, K.R))
                continue;
            if (BoundArena != (K.bArena ? 1 : 0))
            {
                BoundArena = K.bArena ? 1 : 0;
                Cmd->IASetVertexBuffers(0, 2, K.bArena ? ArenaViews : ActorViews);
            }
            Cmd->DrawInstanced((UINT)K.Count, 1, (UINT)K.First, 0);
            StatActorDraws++;
        }
    }
    // Read by the world's pixel shaders and by the walk's compute shader (the weapon lamps' beams in the air).
    Transition(Cmd, SpotMaps.Get(), SpotState,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void FShadowMaps::UploadMasks(ID3D12GraphicsCommandList* Cmd)
{
    if (!Cmd || !MaskArray || !MaskUpload)
        return;
    // The masks new since the last upload (AddMask): each slot's bytes from the upload buffer.
    for (INT m = 0; m < MaskCount; m++)
    {
        if (!MaskDirty[m])
            continue;
        Transition(Cmd, MaskArray.Get(), MaskState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION Dst;
        appMemzero(&Dst, sizeof(Dst));
        Dst.pResource        = MaskArray.Get();
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.SubresourceIndex = (UINT)m;
        D3D12_TEXTURE_COPY_LOCATION Src;
        appMemzero(&Src, sizeof(Src));
        Src.pResource                          = MaskUpload.Get();
        Src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint.Offset             = (UINT64)m * RdxMath::ShadowMaskSize * RdxMath::ShadowMaskSize;
        Src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
        Src.PlacedFootprint.Footprint.Width    = (UINT)RdxMath::ShadowMaskSize;
        Src.PlacedFootprint.Footprint.Height   = (UINT)RdxMath::ShadowMaskSize;
        Src.PlacedFootprint.Footprint.Depth    = 1;
        Src.PlacedFootprint.Footprint.RowPitch = (UINT)RdxMath::ShadowMaskSize;
        Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        MaskDirty[m] = 0;
    }
    Transition(Cmd, MaskArray.Get(), MaskState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FShadowMaps::Render(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* SrvHeap, D3D12_GPU_DESCRIPTOR_HANDLE MaskSrv)
{
    StatWorldRedraws = 0;
    StatActorDraws   = 0;
    StatStaleUsed    = 0;
    if (!IsReady() || !Cmd || LightCount <= 0 || !DsvHeap || !SrvHeap || !MaskArray || !MaskUpload)
        return;
    const DOUBLE CpuStart = appSeconds();
    DOUBLE       CpuWorld = 0.0;

    UploadMasks(Cmd);

    // The world maps this frame draws (RdxMath::WorldRedrawOrder).
    INT   Ages[RdxMath::ShadowMaxLights];
    INT   Order[RdxMath::ShadowMaxLights];
    UBOOL bRedraw[RdxMath::ShadowMaxLights];
    for (INT i = 0; i < LightCount; i++)
    {
        Ages[i]    = UseClock - Slots[LightSlot[i]].DrawnClock;
        bRedraw[i] = 0;
    }
    const INT RedrawCount = RdxMath::WorldRedrawOrder(LightState, Ages, LightCount, (INT)WORLD_REDRAWS_PER_FRAME, Order);
    for (INT k = 0; k < RedrawCount; k++)
        bRedraw[Order[k]] = 1;
    Transition(Cmd, ActorCubes.Get(), ActorState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (RedrawCount > 0)
        Transition(Cmd, WorldCubes.Get(), WorldState, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    ID3D12DescriptorHeap* Heaps[1] = { SrvHeap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(Root.Get());
    Cmd->SetGraphicsRootDescriptorTable(1, MaskSrv);
    Cmd->SetPipelineState(Pipeline.Get());
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12Device* Device = nullptr;
    if (FAILED(Cmd->GetDevice(__uuidof(ID3D12Device), (void**)&Device)) || !Device)
        return;
    const UINT DsvInc = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    Device->Release();
    const D3D12_CPU_DESCRIPTOR_HANDLE DsvStart = DsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_VERTEX_BUFFER_VIEW WorldView;
    appMemzero(&WorldView, sizeof(WorldView));
    if (WorldVb)
    {
        WorldView.BufferLocation = WorldVb->GetGPUVirtualAddress();
        WorldView.SizeInBytes    = (UINT)(WorldVertexCount * 3 * sizeof(FLOAT));
        WorldView.StrideInBytes  = 3 * sizeof(FLOAT);
    }
    D3D12_INDEX_BUFFER_VIEW IndexView;
    appMemzero(&IndexView, sizeof(IndexView));
    if (WorldIb[FrameSlot])
    {
        IndexView.BufferLocation = WorldIb[FrameSlot]->GetGPUVirtualAddress();
        IndexView.SizeInBytes    = (UINT)(WORLD_INDICES * sizeof(UINT));
        IndexView.Format         = DXGI_FORMAT_R32_UINT;
    }
    D3D12_VERTEX_BUFFER_VIEW ActorView;
    appMemzero(&ActorView, sizeof(ActorView));
    ActorView.BufferLocation = ActorVb[FrameSlot]->GetGPUVirtualAddress();
    ActorView.SizeInBytes    = (UINT)(ActorRoom[FrameSlot] * 3 * sizeof(FLOAT));
    ActorView.StrideInBytes  = 3 * sizeof(FLOAT);
    D3D12_VERTEX_BUFFER_VIEW UvwView = ActorView;
    UvwView.BufferLocation = ActorUvwVb[FrameSlot]->GetGPUVirtualAddress();
    const D3D12_VERTEX_BUFFER_VIEW ActorViews[2] = { ActorView, UvwView };
    D3D12_VERTEX_BUFFER_VIEW ArenaView = ActorView;
    ArenaView.BufferLocation = ArenaVb ? ArenaVb->GetGPUVirtualAddress() : 0;
    ArenaView.SizeInBytes    = (UINT)(ARENA_VERTICES * 3 * sizeof(FLOAT));
    D3D12_VERTEX_BUFFER_VIEW ArenaUvwView = ArenaView;
    ArenaUvwView.BufferLocation = ArenaUvwVb ? ArenaUvwVb->GetGPUVirtualAddress() : 0;
    const D3D12_VERTEX_BUFFER_VIEW ArenaViews[2] = { ArenaView, ArenaUvwView };

    for (INT i = 0; i < LightCount; i++)
    {
        const FShadowLightFrame& L    = Lights[i];
        const INT                Slot = LightSlot[i];
        FSlot&                   S    = Slots[Slot];
        RdxMath::FVec3 Pos;
        Pos.X = L.World[0];
        Pos.Y = L.World[1];
        Pos.Z = L.World[2];

        // Not this frame's turn: a missing or unusable map waits, left out of the lookup; a stale one serves as it is.
        if (!bRedraw[i])
        {
            if (LightState[i] == RdxMath::WorldMap_Missing || LightState[i] == RdxMath::WorldMap_Unusable)
            {
                TotalDeferred++;
                continue;
            }
            if (LightState[i] == RdxMath::WorldMap_Stale)
            {
                StatStaleUsed++;
                TotalStaleUsed++;
            }
        }
        else
        {
            SetSquareViewport(Cmd, Preset.WorldMapSize);
            Cmd->SetPipelineState(Pipeline.Get());

            // The triangles within the light's reach, face by face into this frame's index buffer: each candidate's faces
            // in one pass (RdxMath::CubeFacesTouched), then the faces' ranges, a face's triangles in the candidates' order.
            // If they do not fit, or there is no grid, the whole level.
            INT   FaceStart[6] = { 0, 0, 0, 0, 0, 0 };
            INT   FaceCount[6] = { 0, 0, 0, 0, 0, 0 };
            UBOOL bIndexed     = 0;
            const DOUBLE CpuWorldStart = appSeconds();
            if (WorldVertexCount > 0 && WorldIbMapped[FrameSlot] && WorldSpheres && FaceMasks)
            {
                const INT   N   = CollectWorldTriangles(Pos, L.Radius);
                UINT* const Out = WorldIbMapped[FrameSlot];
                INT Count[6] = { 0, 0, 0, 0, 0, 0 };
                for (INT n = 0; n < N; n++)
                {
                    const FLOAT* const Sp = WorldSpheres + Candidates[n] * 4;
                    RdxMath::FVec3 V;
                    V.X = Sp[0] - Pos.X;
                    V.Y = Sp[1] - Pos.Y;
                    V.Z = Sp[2] - Pos.Z;
                    const unsigned Mask = RdxMath::CubeFacesTouched(V, Sp[3]);
                    FaceMasks[n] = (BYTE)Mask;
                    for (INT f = 0; f < 6; f++)
                        Count[f] += (INT)((Mask >> f) & 1u);
                }
                INT Total = 0;
                for (INT f = 0; f < 6; f++)
                    Total += Count[f];
                if (WorldIbUsed + Total * 3 <= WORLD_INDICES)
                {
                    bIndexed = 1;
                    INT Fill[6];
                    INT At = WorldIbUsed;
                    for (INT f = 0; f < 6; f++)
                    {
                        FaceStart[f] = At;
                        FaceCount[f] = Count[f] * 3;
                        Fill[f]      = At;
                        At          += FaceCount[f];
                    }
                    for (INT n = 0; n < N; n++)
                    {
                        const unsigned Mask = FaceMasks[n];
                        if (Mask == 0)
                            continue;
                        const UINT Base = (UINT)(Candidates[n] * 3);
                        for (INT f = 0; f < 6; f++)
                            if (Mask & (1u << f))
                            {
                                UINT* const Dst = Out + Fill[f];
                                Dst[0] = Base;
                                Dst[1] = Base + 1;
                                Dst[2] = Base + 2;
                                Fill[f] += 3;
                            }
                    }
                    WorldIbUsed = At;
                }
            }
            CpuWorld += appSeconds() - CpuWorldStart;
            if (WorldVertexCount > 0)
            {
                Cmd->IASetVertexBuffers(0, 1, &WorldView);
                if (bIndexed)
                    Cmd->IASetIndexBuffer(&IndexView);
            }
            for (INT f = 0; f < 6; f++)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvStart;
                Dsv.ptr += (SIZE_T)((6 * MaxLights + Slot * 6 + f) * DsvInc);
                Cmd->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
                Cmd->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                if (WorldVertexCount <= 0)
                    continue;
                if (bIndexed)
                {
                    if (FaceCount[f] <= 0)
                        continue;
                    SetFace(Cmd, f, Pos);
                    Cmd->DrawIndexedInstanced((UINT)FaceCount[f], 1, (UINT)FaceStart[f], 0, 0);
                    TotalWorldTriangles += FaceCount[f] / 3;
                }
                else
                {
                    SetFace(Cmd, f, Pos);
                    Cmd->DrawInstanced((UINT)WorldVertexCount, 1, 0, 0);
                    TotalWorldTriangles += WorldVertexCount / 3;
                }
            }
            S.bWorldValid = 1;
            S.X = Pos.X;
            S.Y = Pos.Y;
            S.Z = Pos.Z;
            S.R = L.Radius;
            S.DrawnClock = UseClock;
            StatWorldRedraws++;
            TotalWorldRedraws++;
        }
        LightReady[i] = 1;

        // The casters within this light's reach, less those it must not see.
        INT* const Near = NearCasters;
        INT NearCount = 0;
        for (INT c = 0; c < CasterCount; c++)
        {
            if (L.Exclude && (L.Exclude[c >> 5] & (1u << (c & 31))))
                continue;
            const FCaster& C  = Casters[c];
            const FLOAT    DX = C.X - Pos.X, DY = C.Y - Pos.Y, DZ = C.Z - Pos.Z;
            const FLOAT    R  = L.Radius + C.R;
            if (DX * DX + DY * DY + DZ * DZ < R * R)
                Near[NearCount++] = c;
        }

        // Which faces have a caster in them at all, in one pass: CubeFacesTouched is SphereTouchesCubeFace's comparisons
        // for all six at once, and the tests hold the two against each other. Knowing this BEFORE the loop is what lets
        // an untouched face be skipped whole - clear and all (see FSlot::ActorFaceUsed).
        // Each caster's own mask is KEPT, not just folded into FaceUsed: the per-face loop below needs exactly the same
        // answer, and asking SphereTouchesCubeFace again for every (face, caster) pair is the same comparisons six more
        // times over. Bit f of the mask is face f - a test holds the two functions against each other on every face.
        unsigned FaceUsed = 0;
        for (INT n = 0; n < NearCount; n++)
        {
            const FCaster& C = Casters[Near[n]];
            RdxMath::FVec3 V;
            V.X = C.X - Pos.X;
            V.Y = C.Y - Pos.Y;
            V.Z = C.Z - Pos.Z;
            const unsigned Mask = RdxMath::CubeFacesTouched(V, C.R);
            NearFaces[n] = (BYTE)Mask;
            FaceUsed |= Mask;
        }
        // What the skipping is worth, measured against clearing all six faces every time (FaceUsed forced to 0x3F):
        // 0.52 ms of the 11.0 ms the maps take, and 0.48 ms of the frame, with the draw count unchanged. Modest, and
        // above the method's 0.1-1.1% noise floor. It also prices the clears - about half a millisecond for all 300 -
        // which is how we know the maps' 10.5 ms is the DRAWS and not the clearing.
        const unsigned WasUsed = S.ActorFaceUsed;
        S.ActorFaceUsed        = (BYTE)FaceUsed;

        SetSquareViewport(Cmd, Preset.ActorMapSize);
        Cmd->SetPipelineState(ActorPipeline.Get());
        // The frame's buffers or the arena's, as the casters come; -1: none bound yet for this light.
        INT BoundArena = -1;
        for (INT f = 0; f < 6; f++)
        {
            const unsigned Bit = 1u << f;
            // Empty now and empty last time: the face already says "nothing in the way", so leave it entirely alone.
            if (!(FaceUsed & Bit) && !(WasUsed & Bit))
                continue;
            D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvStart;
            Dsv.ptr += (SIZE_T)((Slot * 6 + f) * DsvInc);
            Cmd->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
            Cmd->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            // It had something in it before and has nothing now: cleared, and that is all it needs - without the clear
            // the caster that has moved away would go on casting its shadow.
            if (!(FaceUsed & Bit))
                continue;
            SetFace(Cmd, f, Pos);
            for (INT n = 0; n < NearCount; n++)
            {
                if (!(NearFaces[n] & Bit))
                    continue;
                const FCaster& C = Casters[Near[n]];
                if (BoundArena != (C.bArena ? 1 : 0))
                {
                    BoundArena = C.bArena ? 1 : 0;
                    Cmd->IASetVertexBuffers(0, 2, C.bArena ? ArenaViews : ActorViews);
                }
                Cmd->DrawInstanced((UINT)C.Count, 1, (UINT)C.First, 0);
                StatActorDraws++;
            }
        }
    }

    Transition(Cmd, ActorCubes.Get(), ActorState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(Cmd, WorldCubes.Get(), WorldState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const DOUBLE CpuAll = appSeconds() - CpuStart;
    CpuRenderSum += CpuAll;
    CpuWorldSum  += CpuWorld;
    CpuRenderMax  = Max(CpuRenderMax, CpuAll);
    CpuWorldMax   = Max(CpuWorldMax, CpuWorld);
    CpuFrames++;
}

BYTE* FShadowMaps::GetLookupConstants(INT FrameIndex, D3D12_GPU_VIRTUAL_ADDRESS& Address)
{
    const INT Slot = (FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? FrameIndex : 0;
    Address = LookupCb[Slot] ? LookupCb[Slot]->GetGPUVirtualAddress() : 0;
    return LookupMapped[Slot];
}
