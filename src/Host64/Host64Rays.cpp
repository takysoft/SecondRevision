// SecondRevisionHost64 - the rays. See Host64Rays.h.

#include "Host64Rays.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The light arrays' length in the factor's constants: the renderer's own number.
#include "../RevisionDX12/RenderLimits.h"

// The passes' shaders, built by dxc (Host64.vcxproj).
#include "RayListCS.h"
#include "ShadowFactorRtVS.h"
#include "ShadowFactorRtPS.h"
#include "ShadowFactorRtAoPS.h"

namespace
{
    template <typename T>
    void SafeRelease(T*& P)
    {
        if (P)
        {
            P->Release();
            P = nullptr;
        }
    }

    // FRtPass's numbers, which the passes' shaders go by: an instance's place in the scene is its InstanceID - 0 the
    // level, 1 the bodies, 2 the sky's walls, 3 the see-through surfaces - and its mask is what a ray names to meet it.
    const UINT GInstanceMask[4] = { 1u, 2u, 4u, 8u };

    // The factor's root parameters - the renderer's own signature for the same shader (FinalPass.cpp,
    // BuildShadowFactorRoot): six views at t0-t5, the constants at b0, the scene at t6, the rays' constants at b1, the
    // masked bodies' texture places at t7 and t8, the masks at t9, the geometries' table at t10.
    enum { FACTOR_VIEWS, FACTOR_CONSTS, FACTOR_SCENE, FACTOR_RAYS, FACTOR_FRAME_UVW, FACTOR_ARENA_UVW, FACTOR_MASKS,
           FACTOR_GEO, FACTOR_PARAMS };
    // The list's (RayList.hlsl), a compute pass: its constants at b0, the scene at t6, the requests at t0, the answers
    // at u0, and the tables where the factor has them; then what it traces on by - the mirror rays' table and the
    // level's surfaces, views at t11 and t12 - and the see-through surfaces it writes, at u1.
    enum { LIST_CONSTS, LIST_SCENE, LIST_REQUESTS, LIST_ANSWERS, LIST_FRAME_UVW, LIST_ARENA_UVW, LIST_MASKS, LIST_GEO,
           LIST_MORE, LIST_PASSED, LIST_PARAMS };
    // The views' places in the heap: the factor's six in a row, then the masks, then the list's two tables in a row.
    // The water surface's factor reads a table of the factor's six of its own: the water's pack for the world's depth,
    // nothing drawn after the engine's ClearZ, no surface kinds (views of nothing: the level's), the pack's second
    // float for the light, and the cubes' views of nothing.
    enum { SRV_WORLD_DEPTH, SRV_ON_TOP, SRV_STENCIL, SRV_LIGHT, SRV_WORLD_CUBES, SRV_ACTOR_CUBES, SRV_MASKS, SRV_WATER,
           SRV_MATERIAL, SRV_SURFACE_DEPTH, SRV_SURFACE_ON_TOP, SRV_SURFACE_STENCIL, SRV_SURFACE_LIGHT,
           SRV_SURFACE_WORLD_CUBES, SRV_SURFACE_ACTOR_CUBES };
    enum { RTV_FACTOR, RTV_OCCLUSION, RTV_SURFACE_FACTOR };

    // What a pass reads of its constants, from where the frame's list says they begin: the tables' buffer has to hold
    // that much from there, or the pass would read past its end.
    const UINT64 GFactorConstsBytes = 192u + 48u * (UINT64)RDX_SHADOW_MAX_LIGHTS;
    const UINT64 GRayConstsBytes    = 48u;
    const UINT64 GListConstsBytes   = 16u;

    // More bodies than this in a frame's list is not a list: the mapping's size is worked out from it.
    const uint32_t GBodyRoomMax = 1u << 20;

    void Transition(ID3D12GraphicsCommandList* List, ID3D12Resource* R, D3D12_RESOURCE_STATES From,
                    D3D12_RESOURCE_STATES To)
    {
        D3D12_RESOURCE_BARRIER B = {};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = R;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = From;
        B.Transition.StateAfter  = To;
        List->ResourceBarrier(1, &B);
    }

    // Orders one structure's making before whatever reads it next.
    void StructureBarrier(ID3D12GraphicsCommandList* List, ID3D12Resource* R)
    {
        D3D12_RESOURCE_BARRIER B = {};
        B.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        B.UAV.pResource = R;
        List->ResourceBarrier(1, &B);
    }

    // The state a pass's pixel shader reads a shared texture in: a depth-stencil buffer stays one while it is read.
    D3D12_RESOURCE_STATES ReadState(ID3D12Resource* R)
    {
        return (R->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
                   ? (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                   : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // The format a depth buffer's depth is read through, and its stencil plane's (UNKNOWN: it has none). Anything that
    // is not a depth buffer's storage is read as it is.
    DXGI_FORMAT DepthView(DXGI_FORMAT F)
    {
        switch (F)
        {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_UNORM;
        default:                               return F;
        }
    }

    DXGI_FORMAT StencilView(DXGI_FORMAT F)
    {
        switch (F)
        {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        default:                               return DXGI_FORMAT_UNKNOWN;
        }
    }

    // A view of a texture's first level - or, where there is no texture, of nothing: a pass's table has to be whole.
    void View2D(ID3D12Device* Device, ID3D12Resource* R, DXGI_FORMAT Format, UINT Plane, D3D12_CPU_DESCRIPTOR_HANDLE At)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC V = {};
        V.Format                  = Format;
        V.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        V.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        V.Texture2D.MipLevels     = 1;
        V.Texture2D.PlaneSlice    = R ? Plane : 0;
        Device->CreateShaderResourceView(R, &V, At);
    }

    bool MakeRoot(ID3D12Device* Device, const D3D12_ROOT_PARAMETER* Params, UINT Count,
                  const D3D12_STATIC_SAMPLER_DESC* Samplers, UINT SamplerCount, ID3D12RootSignature** Out)
    {
        D3D12_ROOT_SIGNATURE_DESC D = {};
        D.NumParameters     = Count;
        D.pParameters       = Params;
        D.NumStaticSamplers = SamplerCount;
        D.pStaticSamplers   = Samplers;
        ID3DBlob* Blob  = nullptr;
        ID3DBlob* Error = nullptr;
        const bool bOk = SUCCEEDED(::D3D12SerializeRootSignature(&D, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Error)) &&
                         SUCCEEDED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                                               IID_PPV_ARGS(Out)));
        SafeRelease(Blob);
        SafeRelease(Error);
        return bOk;
    }

    // A pass's pipeline: three corners that cover the target, no depth, one target of the format given. How long the
    // driver's own compiler took over it comes back in Ms.
    bool MakeState(ID3D12Device* Device, ID3D12RootSignature* Root, const void* Vs, size_t VsBytes, const void* Ps,
                   size_t PsBytes, DXGI_FORMAT Target, ID3D12PipelineState** Out, double& Ms)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC P = {};
        P.pRootSignature        = Root;
        P.VS.pShaderBytecode    = Vs;
        P.VS.BytecodeLength     = VsBytes;
        P.PS.pShaderBytecode    = Ps;
        P.PS.BytecodeLength     = PsBytes;
        P.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        P.NumRenderTargets      = 1;
        P.RTVFormats[0]         = Target;
        P.SampleDesc.Count      = 1;
        P.SampleMask            = UINT_MAX;
        P.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        P.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        P.RasterizerState.DepthClipEnable = TRUE;
        P.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        LARGE_INTEGER T0, T1, Freq;
        ::QueryPerformanceFrequency(&Freq);
        ::QueryPerformanceCounter(&T0);
        const bool bOk = SUCCEEDED(Device->CreateGraphicsPipelineState(&P, IID_PPV_ARGS(Out)));
        ::QueryPerformanceCounter(&T1);
        Ms = 1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart;
        return bOk;
    }

    // A root parameter of a pass drawn by a pixel shader - or, for the list's compute pass, of any stage.
    D3D12_ROOT_PARAMETER RootView(D3D12_ROOT_PARAMETER_TYPE Type, UINT Register,
                                  D3D12_SHADER_VISIBILITY Stage = D3D12_SHADER_VISIBILITY_PIXEL)
    {
        D3D12_ROOT_PARAMETER P = {};
        P.ParameterType             = Type;
        P.Descriptor.ShaderRegister = Register;
        P.ShaderVisibility          = Stage;
        return P;
    }

    D3D12_ROOT_PARAMETER RootTable(const D3D12_DESCRIPTOR_RANGE* Range,
                                   D3D12_SHADER_VISIBILITY Stage = D3D12_SHADER_VISIBILITY_PIXEL)
    {
        D3D12_ROOT_PARAMETER P = {};
        P.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        P.DescriptorTable.NumDescriptorRanges = 1;
        P.DescriptorTable.pDescriptorRanges   = Range;
        P.ShaderVisibility                    = Stage;
        return P;
    }

    D3D12_DESCRIPTOR_RANGE ViewRange(UINT First, UINT Count)
    {
        D3D12_DESCRIPTOR_RANGE R = {};
        R.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        R.NumDescriptors     = Count;
        R.BaseShaderRegister = First;
        return R;
    }
}

void FHostRays::Say(const char* Format, ...)
{
    if (!Log)
        return;
    char    Line[512];
    va_list Args;
    va_start(Args, Format);
    vsnprintf(Line, sizeof(Line), Format, Args);
    va_end(Args);
    Log(Line);
}

bool FHostRays::Load(ID3D12Device* InDevice, FLogFunc InLog, char* Why, size_t WhyChars)
{
    Log = InLog;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 O5 = {};
    const bool bAsked = SUCCEEDED(InDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &O5, sizeof(O5)));
    const int  Tier   = bAsked ? (int)O5.RaytracingTier : 0;
    // Inline ray tracing - a ray query in an ordinary shader - is tier 1.1.
    if (Tier < (int)D3D12_RAYTRACING_TIER_1_1)
    {
        sprintf_s(Why, WhyChars, "rays: this card and driver trace none the inline way (ray tracing tier %d.%d)",
                  Tier / 10, Tier % 10);
        return false;
    }
    if (FAILED(InDevice->QueryInterface(IID_PPV_ARGS(&Device))))
    {
        sprintf_s(Why, WhyChars, "rays: the device has no ray tracing interface");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC Hd = {};
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = SRV_COUNT;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC Rd = {};
    Rd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Rd.NumDescriptors = RTV_COUNT;
    D3D12_HEAP_PROPERTIES Up = {};
    Up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC Buf = {};
    Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buf.Width            = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * INSTANCES * Host64::FrameSlots;
    Buf.Height           = 1;
    Buf.DepthOrArraySize = 1;
    Buf.MipLevels        = 1;
    Buf.SampleDesc.Count = 1;
    Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(Device->CreateDescriptorHeap(&Hd, IID_PPV_ARGS(&SrvHeap))) ||
        FAILED(Device->CreateDescriptorHeap(&Rd, IID_PPV_ARGS(&RtvHeap))) ||
        FAILED(Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &Buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&Instances))) ||
        FAILED(Instances->Map(0, nullptr, (void**)&InstancesMapped)) || !InstancesMapped)
    {
        sprintf_s(Why, WhyChars, "rays: the passes' heaps could not be made");
        Shutdown();
        return false;
    }
    SrvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!MakePipelines())
    {
        sprintf_s(Why, WhyChars, "rays: none of the passes' pipelines came up on this card (tier %d.%d)", Tier / 10,
                  Tier % 10);
        Shutdown();
        return false;
    }
    Say("rays: this device traces them (ray tracing tier %d.%d); the passes that came up: 0x%x", Tier / 10, Tier % 10,
        Ready);
    return true;
}

bool FHostRays::MakePipelines()
{
    // The factor's two samplers, as the renderer's signature has them: the maps' comparison at s0 - the RT build of the
    // shader does not sample with it, but declares it - and the masks' own at s1: point, wrapping.
    D3D12_STATIC_SAMPLER_DESC Samplers[2] = {};
    Samplers[0].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    Samplers[0].ShaderRegister   = 0;
    Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    Samplers[1]                  = Samplers[0];
    Samplers[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    Samplers[1].ShaderRegister   = 1;

    const D3D12_DESCRIPTOR_RANGE Views = ViewRange(0, 6), Mask = ViewRange(9, 1), More = ViewRange(11, 2);
    D3D12_ROOT_PARAMETER F[FACTOR_PARAMS];
    F[FACTOR_VIEWS]     = RootTable(&Views);
    F[FACTOR_CONSTS]    = RootView(D3D12_ROOT_PARAMETER_TYPE_CBV, 0);
    F[FACTOR_SCENE]     = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 6);
    F[FACTOR_RAYS]      = RootView(D3D12_ROOT_PARAMETER_TYPE_CBV, 1);
    F[FACTOR_FRAME_UVW] = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 7);
    F[FACTOR_ARENA_UVW] = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 8);
    F[FACTOR_MASKS]     = RootTable(&Mask);
    F[FACTOR_GEO]       = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 10);
    const D3D12_SHADER_VISIBILITY Any = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_PARAMETER L[LIST_PARAMS];
    L[LIST_CONSTS]    = RootView(D3D12_ROOT_PARAMETER_TYPE_CBV, 0, Any);
    L[LIST_SCENE]     = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 6, Any);
    L[LIST_REQUESTS]  = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 0, Any);
    L[LIST_ANSWERS]   = RootView(D3D12_ROOT_PARAMETER_TYPE_UAV, 0, Any);
    L[LIST_FRAME_UVW] = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 7, Any);
    L[LIST_ARENA_UVW] = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 8, Any);
    L[LIST_MASKS]     = RootTable(&Mask, Any);
    L[LIST_GEO]       = RootView(D3D12_ROOT_PARAMETER_TYPE_SRV, 10, Any);
    L[LIST_MORE]      = RootTable(&More, Any);
    L[LIST_PASSED]    = RootView(D3D12_ROOT_PARAMETER_TYPE_UAV, 1, Any);
    D3D12_STATIC_SAMPLER_DESC ListSampler = Samplers[1];
    ListSampler.ShaderVisibility = Any;

    Ready = 0;
    double Ms = 0.0;
    if (!MakeRoot(Device, F, FACTOR_PARAMS, Samplers, 2, &FactorRoot))
        Say("rays: the factor's root signature failed");
    else
    {
        if (MakeState(Device, FactorRoot, GShadowFactorRtVS, sizeof(GShadowFactorRtVS), GShadowFactorRtPS,
                      sizeof(GShadowFactorRtPS), DXGI_FORMAT_R8_UNORM, &FactorPso, Ms))
            Ready |= Host64::RayPass_Shadow | Host64::RayPass_Surface;
        Say("rays: the shadows' factor (ShadowFactor.hlsl, PSMain): its pipeline %s in %.0f ms",
            FactorPso ? "made" : "FAILED", Ms);
        if (MakeState(Device, FactorRoot, GShadowFactorRtVS, sizeof(GShadowFactorRtVS), GShadowFactorRtAoPS,
                      sizeof(GShadowFactorRtAoPS), DXGI_FORMAT_R8_UNORM, &AoPso, Ms))
            Ready |= Host64::RayPass_Ao;
        Say("rays: the occlusion (ShadowFactor.hlsl, AoPS): its pipeline %s in %.0f ms", AoPso ? "made" : "FAILED", Ms);
    }
    if (!MakeRoot(Device, L, LIST_PARAMS, &ListSampler, 1, &ListRoot))
        Say("rays: the list's root signature failed");
    else
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC P = {};
        P.pRootSignature     = ListRoot;
        P.CS.pShaderBytecode = GRayListCS;
        P.CS.BytecodeLength  = sizeof(GRayListCS);
        LARGE_INTEGER T0, T1, Freq;
        ::QueryPerformanceFrequency(&Freq);
        ::QueryPerformanceCounter(&T0);
        if (SUCCEEDED(Device->CreateComputePipelineState(&P, IID_PPV_ARGS(&ListPso))))
            Ready |= Host64::RayPass_List;
        ::QueryPerformanceCounter(&T1);
        Say("rays: the list (RayList.hlsl): its pipeline %s in %.0f ms", ListPso ? "made" : "FAILED",
            1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart);
    }
    return Ready != 0;
}

UINT64 FHostRays::BufferBytes(int Resource) const
{
    if (!Shared[Resource])
        return 0;
    const D3D12_RESOURCE_DESC D = Shared[Resource]->GetDesc();
    return (D.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) ? D.Width : 0;
}

bool FHostRays::Take(const Host64::FRayShare& Share)
{
    Release();
    bool bOk = Device != nullptr;
    for (int r = 0; r < Host64::Ray_Count; r++)
    {
        const HANDLE Handle = (HANDLE)(uintptr_t)Share.Resources[r].Handle;
        if (!Handle)
            continue;
        if (Device && FAILED(Device->OpenSharedHandle(Handle, IID_PPV_ARGS(&Shared[r]))))
        {
            Say("rays: shared resource %d (handle 0x%llx) would not open", r, (unsigned long long)Share.Resources[r].Handle);
            bOk = false;
        }
        // The handle was duplicated into this process for this one use.
        ::CloseHandle(Handle);
    }
    if (const HANDLE Lists = (HANDLE)(uintptr_t)Share.Frames)
    {
        if (bOk && Share.BodyRoom <= GBodyRoomMax)
            Frames = ::MapViewOfFile(Lists, FILE_MAP_READ, 0, 0, Host64::RayFramesBytes(Share.BodyRoom));
        // The view holds the mapping from here.
        ::CloseHandle(Lists);
    }
    if (bOk && !Frames)
    {
        Say("rays: the frames' lists would not map (room for %u bodies)", Share.BodyRoom);
        bOk = false;
    }
    if (bOk && !BufferBytes(Host64::Ray_Tables))
    {
        Say("rays: a share without the passes' tables");
        bOk = false;
    }
    const uint32_t Asked[3] = { Share.LevelVertices, Share.SkyVertices, Share.ThinVertices };
    for (int k = 0; k < 3; k++)
    {
        StaticVertices[k] = Asked[k] / 3 * 3;
        const UINT64 Held = BufferBytes((int)Host64::Ray_Level + k);
        if ((UINT64)StaticVertices[k] * 12 > Held)
        {
            Say("rays: the share says %u corners of %s, and their buffer holds %llu bytes", Asked[k],
                (k == 0) ? "the level" : (k == 1) ? "the sky's walls" : "the see-through surfaces",
                (unsigned long long)Held);
            bOk = false;
        }
    }
    if (!bOk)
    {
        Release();
        return false;
    }
    BodyRoom     = Share.BodyRoom;
    bShare       = true;
    bStaticDirty = true;
    MakeViews();
    Say("rays: a share taken - the level %u triangles, the sky's walls %u, the see-through surfaces %u; room for %u "
        "bodies a frame", StaticVertices[0] / 3, StaticVertices[1] / 3, StaticVertices[2] / 3, BodyRoom);
    return true;
}

// The share's views, into the heaps: what a pass's table names and the share has not is a view of nothing.
bool FHostRays::MakeViews()
{
    static_assert(SRV_SURFACE_ACTOR_CUBES + 1 == SRV_COUNT, "the rays' heap holds every view");
    const auto Srv = [this](int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE H = SrvHeap->GetCPUDescriptorHandleForHeapStart();
        H.ptr += (SIZE_T)i * SrvStep;
        return H;
    };
    const auto Format = [this](int r) { return Shared[r] ? Shared[r]->GetDesc().Format : DXGI_FORMAT_UNKNOWN; };
    ID3D12Resource* const World = Shared[Host64::Ray_WorldDepth];
    ID3D12Resource* const OnTop = Shared[Host64::Ray_OnTopDepth];
    View2D(Device, World, World ? DepthView(Format(Host64::Ray_WorldDepth)) : DXGI_FORMAT_R32_FLOAT, 0,
           Srv(SRV_WORLD_DEPTH));
    View2D(Device, OnTop, OnTop ? DepthView(Format(Host64::Ray_OnTopDepth)) : DXGI_FORMAT_R32_FLOAT, 0, Srv(SRV_ON_TOP));
    // The surfaces' kinds: the world depth's stencil plane, where it was shared with one.
    const DXGI_FORMAT Stencil = World ? StencilView(Format(Host64::Ray_WorldDepth)) : DXGI_FORMAT_UNKNOWN;
    if (Stencil != DXGI_FORMAT_UNKNOWN)
        View2D(Device, World, Stencil, 1, Srv(SRV_STENCIL));
    else
        View2D(Device, nullptr, DXGI_FORMAT_R8G8_UINT, 0, Srv(SRV_STENCIL));
    View2D(Device, Shared[Host64::Ray_SceneLight],
           Shared[Host64::Ray_SceneLight] ? Format(Host64::Ray_SceneLight) : DXGI_FORMAT_R8_UNORM, 0, Srv(SRV_LIGHT));
    // The maps' cubes: the RT build of the factor declares them and reads neither.
    D3D12_SHADER_RESOURCE_VIEW_DESC Cubes = {};
    Cubes.Format                     = DXGI_FORMAT_R32_FLOAT;
    Cubes.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    Cubes.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Cubes.TextureCubeArray.MipLevels = 1;
    Cubes.TextureCubeArray.NumCubes  = 1;
    Device->CreateShaderResourceView(nullptr, &Cubes, Srv(SRV_WORLD_CUBES));
    Device->CreateShaderResourceView(nullptr, &Cubes, Srv(SRV_ACTOR_CUBES));
    // The water surface's table.
    View2D(Device, Shared[Host64::Ray_SurfacePack], DXGI_FORMAT_R32G32_FLOAT, 0, Srv(SRV_SURFACE_DEPTH));
    View2D(Device, nullptr, DXGI_FORMAT_R32_FLOAT, 0, Srv(SRV_SURFACE_ON_TOP));
    View2D(Device, nullptr, DXGI_FORMAT_R8G8_UINT, 0, Srv(SRV_SURFACE_STENCIL));
    D3D12_SHADER_RESOURCE_VIEW_DESC PackLight = {};
    PackLight.Format                  = DXGI_FORMAT_R32G32_FLOAT;
    PackLight.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    PackLight.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 1, 1, 1);
    PackLight.Texture2D.MipLevels     = 1;
    Device->CreateShaderResourceView(Shared[Host64::Ray_SurfacePack], &PackLight, Srv(SRV_SURFACE_LIGHT));
    Device->CreateShaderResourceView(nullptr, &Cubes, Srv(SRV_SURFACE_WORLD_CUBES));
    Device->CreateShaderResourceView(nullptr, &Cubes, Srv(SRV_SURFACE_ACTOR_CUBES));
    ID3D12Resource* const Masks = Shared[Host64::Ray_Masks];
    D3D12_SHADER_RESOURCE_VIEW_DESC Mv = {};
    Mv.Format                   = Masks ? Format(Host64::Ray_Masks) : DXGI_FORMAT_R8_UNORM;
    Mv.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    Mv.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Mv.Texture2DArray.MipLevels = Masks ? Masks->GetDesc().MipLevels : 1;
    Mv.Texture2DArray.ArraySize = Masks ? Masks->GetDesc().DepthOrArraySize : 1;
    Device->CreateShaderResourceView(Masks, &Mv, Srv(SRV_MASKS));
    // The two tables the list traces on by, each as long as its buffer: read past its end, a view gives nought - a
    // number of the renderer's that points nowhere must not take the pass anywhere.
    const int Tables[2] = { Host64::Ray_Water, Host64::Ray_Material };
    for (int t = 0; t < 2; t++)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Raw = {};
        Raw.Format                  = DXGI_FORMAT_R32_TYPELESS;
        Raw.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        Raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Raw.Buffer.NumElements      = Shared[Tables[t]] ? (UINT)(BufferBytes(Tables[t]) / 4) : 1;
        Raw.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
        Device->CreateShaderResourceView(Shared[Tables[t]], &Raw, Srv(SRV_WATER + t));
    }
    const int Targets[RTV_COUNT] = { Host64::Ray_Factor, Host64::Ray_Occlusion, Host64::Ray_SurfaceFactor };
    for (int t = 0; t < RTV_COUNT; t++)
        if (Shared[Targets[t]])
        {
            D3D12_CPU_DESCRIPTOR_HANDLE H = RtvHeap->GetCPUDescriptorHandleForHeapStart();
            H.ptr += (SIZE_T)t * RtvStep;
            Device->CreateRenderTargetView(Shared[Targets[t]], nullptr, H);
        }
    return true;
}

bool FHostRays::Ensure(FBuffer& Buffer, UINT64 Bytes, D3D12_RESOURCE_STATES State, const char* What, uint64_t Frame)
{
    if (Buffer.Res && Buffer.Bytes >= Bytes)
        return true;
    Retire(Buffer, Frame);
    // A little room over what was asked, so a frame with one more body does not make the buffer again.
    const UINT64 Want = (Bytes + Bytes / 4 + 65535) & ~(UINT64)65535;
    D3D12_HEAP_PROPERTIES Heap = {};
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = Want;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT Made = Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, State, nullptr,
                                                         IID_PPV_ARGS(&Buffer.Res));
    if (FAILED(Made))
    {
        Say("rays: %s could not be made (%.1f MB, 0x%08lx)", What, (double)Want / (1024.0 * 1024.0), (unsigned long)Made);
        Buffer.Res = nullptr;
        return false;
    }
    Buffer.Bytes = Want;
    return true;
}

void FHostRays::Retire(FBuffer& Buffer, uint64_t Frame)
{
    if (!Buffer.Res)
        return;
    if (RetiredCount == RetiredRoom)
    {
        const uint32_t  Room = RetiredRoom ? RetiredRoom * 2 : 16;
        FRetired* const More = (FRetired*)realloc(Retired, (size_t)Room * sizeof(FRetired));
        if (More)
        {
            Retired     = More;
            RetiredRoom = Room;
        }
    }
    // Without room for its record it is never let go: the one way left that cannot leave a command of a frame in flight
    // pointing at memory given back.
    if (RetiredCount < RetiredRoom)
    {
        Retired[RetiredCount].Res   = Buffer.Res;
        Retired[RetiredCount].Frame = Frame;
        RetiredCount++;
    }
    Buffer.Res   = nullptr;
    Buffer.Bytes = 0;
}

void FHostRays::Bury(uint64_t Frame, bool bAll)
{
    uint32_t Kept = 0;
    for (uint32_t i = 0; i < RetiredCount; i++)
    {
        if (bAll || Retired[i].Frame + Host64::FrameSlots <= Frame)
            Retired[i].Res->Release();
        else
            Retired[Kept++] = Retired[i];
    }
    RetiredCount = Kept;
}

bool FHostRays::BuildStatic(ID3D12GraphicsCommandList4* List, int Resource, uint32_t Vertices, FBuffer& Blas,
                            const char* What, uint64_t Frame)
{
    if (!Shared[Resource] || Vertices < 3)
        return false;
    D3D12_RAYTRACING_GEOMETRY_DESC Geo = {};
    Geo.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    Geo.Flags                                = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    Geo.Triangles.VertexBuffer.StartAddress  = Shared[Resource]->GetGPUVirtualAddress();
    Geo.Triangles.VertexBuffer.StrideInBytes = 12;
    Geo.Triangles.VertexCount                = Vertices;
    Geo.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    Geo.Triangles.IndexFormat                = DXGI_FORMAT_UNKNOWN;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS In = {};
    In.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    In.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    In.NumDescs       = 1;
    In.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    In.pGeometryDescs = &Geo;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info = {};
    Device->GetRaytracingAccelerationStructurePrebuildInfo(&In, &Info);
    // The room the making needs is this one build's: let go again once the frame is long done. (A buffer begins in
    // COMMON whatever is asked, and is taken for unordered access by the build itself.)
    FBuffer Scratch;
    if (Info.ResultDataMaxSizeInBytes == 0 ||
        !Ensure(Blas, Info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, What, Frame) ||
        !Ensure(Scratch, Info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, What, Frame))
        return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build = {};
    Build.DestAccelerationStructureData    = Blas.Res->GetGPUVirtualAddress();
    Build.Inputs                           = In;
    Build.ScratchAccelerationStructureData = Scratch.Res->GetGPUVirtualAddress();
    List->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);
    StructureBarrier(List, Blas.Res);
    Retire(Scratch, Frame);
    Say("rays: the structure of %s made - %u triangles, %.1f MB (and %.1f MB to make it)", What, Vertices / 3,
        (double)Info.ResultDataMaxSizeInBytes / (1024.0 * 1024.0),
        (double)Info.ScratchDataSizeInBytes / (1024.0 * 1024.0));
    return true;
}

// The frame's bodies: a geometry each, in the list's order - the passes' tables go by it. A body some pass has to be
// asked about (a mask, a lamp's own shade) is not opaque; every other triangle ends a ray by itself, the fast way.
bool FHostRays::BuildBodies(ID3D12GraphicsCommandList4* List, const Host64::FRayFrame& Head,
                            const Host64::FRayBody* Bodies, uint64_t Frame)
{
    bBodies = false;
    if (Head.Bodies == 0)
        return true;
    if (Head.Bodies > GeoRoom)
    {
        const uint32_t Room = Head.Bodies + Head.Bodies / 2 + 64;
        D3D12_RAYTRACING_GEOMETRY_DESC* const More =
            (D3D12_RAYTRACING_GEOMETRY_DESC*)realloc(Geos, (size_t)Room * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC));
        if (!More)
            return false;
        Geos    = More;
        GeoRoom = Room;
    }
    const UINT64 Bytes[2] = { BufferBytes(Host64::Ray_FrameXyz), BufferBytes(Host64::Ray_ArenaXyz) };
    for (uint32_t b = 0; b < Head.Bodies; b++)
    {
        // Copied out: the list is memory the renderer writes.
        const Host64::FRayBody B = Bodies[b];
        const int    Where    = (B.Flags & Host64::RayBody_Arena) ? 1 : 0;
        const UINT   Vertices = B.Vertices / 3 * 3;
        // A body its buffer does not hold whole would have the build read past the buffer's end. The frame is refused:
        // leaving the body out would move every later one's place in the passes' tables.
        if (Vertices < 3 || B.Stride < 12 || (B.Stride & 3) != 0 || (B.Offset & 3) != 0 ||
            (UINT64)B.Offset + (UINT64)(Vertices - 1) * B.Stride + 12 > Bytes[Where])
        {
            Say("rays: frame %llu - body %u of %u is not within its buffer (offset %u, stride %u, %u corners, %llu bytes)",
                (unsigned long long)Frame, b, Head.Bodies, B.Offset, B.Stride, B.Vertices,
                (unsigned long long)Bytes[Where]);
            return false;
        }
        D3D12_RAYTRACING_GEOMETRY_DESC& Geo = Geos[b];
        memset(&Geo, 0, sizeof(Geo));
        Geo.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        Geo.Flags = (B.Flags & Host64::RayBody_Asks) ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
                                                     : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        Geo.Triangles.VertexBuffer.StartAddress =
            Shared[Where ? Host64::Ray_ArenaXyz : Host64::Ray_FrameXyz]->GetGPUVirtualAddress() + B.Offset;
        Geo.Triangles.VertexBuffer.StrideInBytes = B.Stride;
        Geo.Triangles.VertexCount                = Vertices;
        Geo.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
        Geo.Triangles.IndexFormat                = DXGI_FORMAT_UNKNOWN;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS In = {};
    In.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    In.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    In.NumDescs       = Head.Bodies;
    In.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    In.pGeometryDescs = Geos;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info = {};
    Device->GetRaytracingAccelerationStructurePrebuildInfo(&In, &Info);
    if (Info.ResultDataMaxSizeInBytes == 0 ||
        !Ensure(BodyBlas, Info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                "the bodies' structure", Frame) ||
        !Ensure(BodyScratch, Info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, "the bodies' scratch", Frame))
        return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build = {};
    Build.DestAccelerationStructureData    = BodyBlas.Res->GetGPUVirtualAddress();
    Build.Inputs                           = In;
    Build.ScratchAccelerationStructureData = BodyScratch.Res->GetGPUVirtualAddress();
    List->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);
    StructureBarrier(List, BodyBlas.Res);
    bBodies = true;
    return true;
}

// The top level: the level, the bodies, the sky's walls and the see-through surfaces as they are - their triangles are
// in the level's space already - each with FRtPass's number and mask.
bool FHostRays::BuildScene(ID3D12GraphicsCommandList4* List, uint64_t Frame)
{
    const UINT64 First = (UINT64)INSTANCES * (Frame % Host64::FrameSlots);
    D3D12_RAYTRACING_INSTANCE_DESC* const Out = InstancesMapped + First;
    UINT Placed = 0;
    for (UINT k = 0; k < INSTANCES; k++)
    {
        ID3D12Resource* const Blas = (k == 0) ? (bStaticBuilt[0] ? StaticBlas[0].Res : nullptr)
                                   : (k == 1) ? (bBodies ? BodyBlas.Res : nullptr)
                                   : (k == 2) ? (bStaticBuilt[1] ? StaticBlas[1].Res : nullptr)
                                              : (bStaticBuilt[2] ? StaticBlas[2].Res : nullptr);
        if (!Blas)
            continue;
        D3D12_RAYTRACING_INSTANCE_DESC& I = Out[Placed++];
        memset(&I, 0, sizeof(I));
        I.Transform[0][0] = I.Transform[1][1] = I.Transform[2][2] = 1.0f;
        I.InstanceID      = k;
        I.InstanceMask    = GInstanceMask[k];
        // Both faces: a level polygon or a body blocks a ray from either side, as it blocks the light.
        I.Flags                 = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        I.AccelerationStructure = Blas->GetGPUVirtualAddress();
    }
    if (!Placed)
        return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Top = {};
    Top.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    Top.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    Top.NumDescs      = Placed;
    Top.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    Top.InstanceDescs = Instances->GetGPUVirtualAddress() + First * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info = {};
    Device->GetRaytracingAccelerationStructurePrebuildInfo(&Top, &Info);
    if (Info.ResultDataMaxSizeInBytes == 0 ||
        !Ensure(Tlas, Info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                "the top level", Frame) ||
        !Ensure(TlasScratch, Info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, "the top level's scratch", Frame))
        return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build = {};
    Build.DestAccelerationStructureData    = Tlas.Res->GetGPUVirtualAddress();
    Build.Inputs                           = Top;
    Build.ScratchAccelerationStructureData = TlasScratch.Res->GetGPUVirtualAddress();
    List->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);
    StructureBarrier(List, Tlas.Res);
    return true;
}

void FHostRays::DrawPass(ID3D12GraphicsCommandList* List, uint32_t Pass, const Host64::FRayFrame& Head)
{
    const bool bAo      = Pass == Host64::RayPass_Ao;
    const bool bSurface = Pass == Host64::RayPass_Surface;
    ID3D12Resource* const Target =
        Shared[bAo ? Host64::Ray_Occlusion : (bSurface ? Host64::Ray_SurfaceFactor : Host64::Ray_Factor)];
    const D3D12_GPU_VIRTUAL_ADDRESS Tables = Shared[Host64::Ray_Tables]->GetGPUVirtualAddress();
    // The texture places' buffers, where the share has them. Where it has not, no body of the frame has a mask and no
    // pass reads them - but a root view has to be an address, and the tables' own is one.
    const D3D12_GPU_VIRTUAL_ADDRESS FrameUvw =
        Shared[Host64::Ray_FrameUvw] ? Shared[Host64::Ray_FrameUvw]->GetGPUVirtualAddress() : Tables;
    const D3D12_GPU_VIRTUAL_ADDRESS ArenaUvw =
        Shared[Host64::Ray_ArenaUvw] ? Shared[Host64::Ray_ArenaUvw]->GetGPUVirtualAddress() : Tables;
    const auto Srv = [this](int i) {
        D3D12_GPU_DESCRIPTOR_HANDLE H = SrvHeap->GetGPUDescriptorHandleForHeapStart();
        H.ptr += (UINT64)i * SrvStep;
        return H;
    };
    Transition(List, Target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    Rtv.ptr += (SIZE_T)(bAo ? RTV_OCCLUSION : (bSurface ? RTV_SURFACE_FACTOR : RTV_FACTOR)) * RtvStep;
    List->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    const D3D12_RESOURCE_DESC Td = Target->GetDesc();
    const UINT W = (Head.Width && Head.Width < Td.Width) ? Head.Width : (UINT)Td.Width;
    const UINT H = (Head.Height && Head.Height < Td.Height) ? Head.Height : Td.Height;
    const D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f };
    const D3D12_RECT     Sc = { 0, 0, (LONG)W, (LONG)H };
    List->RSSetViewports(1, &Vp);
    List->RSSetScissorRects(1, &Sc);
    List->SetDescriptorHeaps(1, &SrvHeap);
    List->SetGraphicsRootSignature(FactorRoot);
    List->SetPipelineState(bAo ? AoPso : FactorPso);
    List->SetGraphicsRootDescriptorTable(FACTOR_VIEWS, Srv(bSurface ? SRV_SURFACE_DEPTH : SRV_WORLD_DEPTH));
    List->SetGraphicsRootConstantBufferView(FACTOR_CONSTS, Tables + Head.FactorConstsAt);
    List->SetGraphicsRootShaderResourceView(FACTOR_SCENE, Tlas.Res->GetGPUVirtualAddress());
    List->SetGraphicsRootConstantBufferView(FACTOR_RAYS, Tables + Head.RayConstsAt);
    List->SetGraphicsRootShaderResourceView(FACTOR_FRAME_UVW, FrameUvw);
    List->SetGraphicsRootShaderResourceView(FACTOR_ARENA_UVW, ArenaUvw);
    List->SetGraphicsRootDescriptorTable(FACTOR_MASKS, Srv(SRV_MASKS));
    List->SetGraphicsRootShaderResourceView(FACTOR_GEO, Tables + Head.GeoAt);
    List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    List->DrawInstanced(3, 1, 0, 0);
    Transition(List, Target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
}

// The list: every request the frame's pixel shaders wrote down, answered in its own slot. The pass reads the count of
// the slots taken out of the requests themselves, and leaves alone what lies past it. What it traces on by is bound
// whether the frame brought it or not - the pass's constants say whether it is read - and the see-through surfaces
// go into their own buffer; where the share has none, the answers' stands in its place, and nothing is written there.
void FHostRays::ListPass(ID3D12GraphicsCommandList* List, const Host64::FRayFrame& Head)
{
    ID3D12Resource* const Requests = Shared[Host64::Ray_Requests];
    ID3D12Resource* const Answers  = Shared[Host64::Ray_Answers];
    ID3D12Resource* const Passed   = Shared[Host64::Ray_Passed];
    ID3D12Resource* const More[2]  = { Shared[Host64::Ray_Water], Shared[Host64::Ray_Material] };
    const D3D12_GPU_VIRTUAL_ADDRESS Tables = Shared[Host64::Ray_Tables]->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS FrameUvw =
        Shared[Host64::Ray_FrameUvw] ? Shared[Host64::Ray_FrameUvw]->GetGPUVirtualAddress() : Tables;
    const D3D12_GPU_VIRTUAL_ADDRESS ArenaUvw =
        Shared[Host64::Ray_ArenaUvw] ? Shared[Host64::Ray_ArenaUvw]->GetGPUVirtualAddress() : Tables;
    D3D12_GPU_DESCRIPTOR_HANDLE Masks = SrvHeap->GetGPUDescriptorHandleForHeapStart();
    Masks.ptr += (UINT64)SRV_MASKS * SrvStep;
    D3D12_GPU_DESCRIPTOR_HANDLE MoreViews = SrvHeap->GetGPUDescriptorHandleForHeapStart();
    MoreViews.ptr += (UINT64)SRV_WATER * SrvStep;
    Transition(List, Requests, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(List, Answers, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (ID3D12Resource* R : More)
        if (R)
            Transition(List, R, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (Passed)
        Transition(List, Passed, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    List->SetDescriptorHeaps(1, &SrvHeap);
    List->SetComputeRootSignature(ListRoot);
    List->SetPipelineState(ListPso);
    List->SetComputeRootConstantBufferView(LIST_CONSTS, Tables + Head.ListConstsAt);
    List->SetComputeRootShaderResourceView(LIST_SCENE, Tlas.Res->GetGPUVirtualAddress());
    List->SetComputeRootShaderResourceView(LIST_REQUESTS, Requests->GetGPUVirtualAddress());
    List->SetComputeRootUnorderedAccessView(LIST_ANSWERS, Answers->GetGPUVirtualAddress());
    List->SetComputeRootShaderResourceView(LIST_FRAME_UVW, FrameUvw);
    List->SetComputeRootShaderResourceView(LIST_ARENA_UVW, ArenaUvw);
    List->SetComputeRootDescriptorTable(LIST_MASKS, Masks);
    List->SetComputeRootShaderResourceView(LIST_GEO, Tables + Head.GeoAt);
    List->SetComputeRootDescriptorTable(LIST_MORE, MoreViews);
    List->SetComputeRootUnorderedAccessView(LIST_PASSED, (Passed ? Passed : Answers)->GetGPUVirtualAddress());
    List->Dispatch((Head.ListRoom + 63u) / 64u, 1, 1);
    if (Passed)
        Transition(List, Passed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (ID3D12Resource* R : More)
        if (R)
            Transition(List, R, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(List, Answers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Transition(List, Requests, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
}

bool FHostRays::Trace(ID3D12GraphicsCommandList* List, uint64_t Frame)
{
    bTracedList = false;
    if (!bShare || !Device || !Frames)
        return false;
    Bury(Frame, false);
    // The frame's list, its head copied out: the list is memory the renderer writes, and what is checked here has to
    // be what is used below.
    const Host64::FRayFrame* const Slot = (const Host64::FRayFrame*)(
        (const char*)Frames + (size_t)(Frame % Host64::FrameSlots) * Host64::RayFrameBytes(BodyRoom));
    const Host64::FRayFrame Head = *Slot;
    const uint32_t Passes = Head.Passes & Ready;
    const bool bFactor  = (Passes & (Host64::RayPass_Shadow | Host64::RayPass_Ao)) != 0;
    const bool bSurface = (Passes & Host64::RayPass_Surface) != 0;
    const bool bList   = (Passes & Host64::RayPass_List) != 0;
    bTracedList = bList;
    const UINT64 ListBytes = (UINT64)Head.ListRoom * RDX_RAY_SLOT_BYTES;
    const UINT64 TableBytes = BufferBytes(Host64::Ray_Tables);
    const auto Within = [TableBytes](uint32_t At, UINT64 Bytes, uint32_t Step) {
        return (At % Step) == 0 && (UINT64)At + Bytes <= TableBytes;
    };
    // Refused before anything is recorded: a pass this card has no pipeline for, a pass whose textures the share has
    // not, constants that are not where a constant buffer can begin or not within the tables, a table to trace on by
    // that its buffer does not hold or that comes without the level's surfaces, more passed surfaces than their
    // buffer has slots.
    if (Passes != Head.Passes || Passes == 0 || Head.Bodies > BodyRoom ||
        ((Passes & Host64::RayPass_Shadow) && !Shared[Host64::Ray_Factor]) ||
        ((Passes & Host64::RayPass_Ao) && !Shared[Host64::Ray_Occlusion]) ||
        (bSurface && (!(Passes & Host64::RayPass_Shadow) || !Shared[Host64::Ray_SurfaceFactor] ||
                      !Shared[Host64::Ray_SurfacePack])) ||
        (bFactor && (!Shared[Host64::Ray_WorldDepth] || !Within(Head.FactorConstsAt, GFactorConstsBytes, 256) ||
                     !Within(Head.RayConstsAt, GRayConstsBytes, 256))) ||
        (bList && (Head.ListRoom < 2 || ListBytes > BufferBytes(Host64::Ray_Requests) ||
                   ListBytes > BufferBytes(Host64::Ray_Answers) ||
                   !Within(Head.ListConstsAt, GListConstsBytes, 256) ||
                   Head.WaterBytes > BufferBytes(Host64::Ray_Water) ||
                   (Head.WaterBytes != 0 && !Shared[Host64::Ray_Material]) ||
                   (UINT64)Head.PassedRoom * RDX_RAY_SLOT_BYTES > BufferBytes(Host64::Ray_Passed))) ||
        !Within(Head.GeoAt, 4, 4))
    {
        Say("rays: frame %llu refused - passes 0x%x asked (0x%x here), %u bodies (room for %u), tables at %u/%u/%u/%u "
            "of %llu bytes, %u slots of the list, %u bytes of the mirror rays' table, %u slots for what the rays pass",
            (unsigned long long)Frame, Head.Passes, Ready, Head.Bodies, BodyRoom, Head.FactorConstsAt, Head.RayConstsAt,
            Head.GeoAt, Head.ListConstsAt, (unsigned long long)TableBytes, Head.ListRoom, Head.WaterBytes,
            Head.PassedRoom);
        return false;
    }
    ID3D12GraphicsCommandList4* List4 = nullptr;
    if (FAILED(List->QueryInterface(IID_PPV_ARGS(&List4))))
        return false;
    // The level's structures, once a share. One that could not be made is not tried again: the rays go without it, and
    // the log has said so.
    if (bStaticDirty)
    {
        static const char* const Names[3] = { "the level", "the sky's walls", "the see-through surfaces" };
        for (int k = 0; k < 3; k++)
            bStaticBuilt[k] = BuildStatic(List4, (int)Host64::Ray_Level + k, StaticVertices[k], StaticBlas[k], Names[k],
                                          Frame);
        bStaticDirty = false;
    }
    // The bodies' structure and the top level - unless the frame says its scene is the one the submission before it was
    // traced against, and that one stands: then they are as they were left.
    const bool bSame = (Head.Flags & Host64::RayFrame_SameScene) != 0 && bSceneStands && SceneBodies == Head.Bodies;
    bSceneStands = bSame ||
                   (BuildBodies(List4, Head, (const Host64::FRayBody*)(Slot + 1), Frame) && BuildScene(List4, Frame));
    SceneBodies  = Head.Bodies;
    List4->Release();
    if (!bSceneStands)
        return false;

    // What the asked passes read, readable by their shaders while they run, and back in COMMON after. The masks are
    // read by the list's compute pass as well: a state that says so, or the read is one no stage may make.
    ID3D12Resource* const Read[5] = {
        bFactor ? Shared[Host64::Ray_WorldDepth] : nullptr, bFactor ? Shared[Host64::Ray_OnTopDepth] : nullptr,
        bFactor ? Shared[Host64::Ray_SceneLight] : nullptr, Shared[Host64::Ray_Masks],
        bSurface ? Shared[Host64::Ray_SurfacePack] : nullptr };
    const auto Reads = [this, bList](ID3D12Resource* R) {
        return (bList && R == Shared[Host64::Ray_Masks])
                   ? (ReadState(R) | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) : ReadState(R);
    };
    for (ID3D12Resource* R : Read)
        if (R)
            Transition(List, R, D3D12_RESOURCE_STATE_COMMON, Reads(R));
    if (Passes & Host64::RayPass_Shadow)
        DrawPass(List, Host64::RayPass_Shadow, Head);
    if (Passes & Host64::RayPass_Ao)
        DrawPass(List, Host64::RayPass_Ao, Head);
    if (bSurface)
        DrawPass(List, Host64::RayPass_Surface, Head);
    if (bList)
        ListPass(List, Head);
    for (ID3D12Resource* R : Read)
        if (R)
            Transition(List, R, Reads(R), D3D12_RESOURCE_STATE_COMMON);
    if (Traced++ % 1800 == 0)
        Say("rays: frame %llu traced - %u bodies, passes 0x%x, the level %s", (unsigned long long)Frame, Head.Bodies,
            Passes, bStaticBuilt[0] ? "in" : "missing");
    return true;
}

void FHostRays::Release()
{
    if (Frames)
        ::UnmapViewOfFile(Frames);
    Frames = nullptr;
    for (int r = 0; r < Host64::Ray_Count; r++)
        SafeRelease(Shared[r]);
    for (int k = 0; k < 3; k++)
    {
        SafeRelease(StaticBlas[k].Res);
        StaticBlas[k].Bytes = 0;
        bStaticBuilt[k]     = false;
        StaticVertices[k]   = 0;
    }
    Bury(0, true);
    BodyRoom     = 0;
    bShare       = false;
    bStaticDirty = false;
    bBodies      = false;
    bSceneStands = false;
}

void FHostRays::Shutdown()
{
    Release();
    FBuffer* const Own[4] = { &BodyBlas, &BodyScratch, &Tlas, &TlasScratch };
    for (FBuffer* B : Own)
    {
        SafeRelease(B->Res);
        B->Bytes = 0;
    }
    if (Instances && InstancesMapped)
        Instances->Unmap(0, nullptr);
    InstancesMapped = nullptr;
    SafeRelease(Instances);
    SafeRelease(ListPso);
    SafeRelease(ListRoot);
    SafeRelease(AoPso);
    SafeRelease(FactorPso);
    SafeRelease(FactorRoot);
    SafeRelease(RtvHeap);
    SafeRelease(SrvHeap);
    free(Geos);
    Geos    = nullptr;
    GeoRoom = 0;
    free(Retired);
    Retired     = nullptr;
    RetiredRoom = 0;
    Ready       = 0;
    SafeRelease(Device);
}
