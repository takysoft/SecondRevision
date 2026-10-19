#include "Pch.h"
#include "FilmLook.h"
#include "Log.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// The three, in the order a camera does them: the lens splits the colours, the lens darkens the corners, and then
// the film is exposed and has grain of its own.
static const char* GFilmLookShader =
"Texture2D<float4>   Picture : register(t0);\n"
"Texture2D<float4>   Halo    : register(t1);\n"
"Texture2D<float4>   Ghosts  : register(t2);\n"
"Texture2D<float4>   Coronas : register(t3);\n"
"RWTexture2D<float4> Out     : register(u0);\n"
"SamplerState        Smooth  : register(s0);\n"
"cbuffer C : register(b0)\n"
"{\n"
"    uint2 Size; uint Frame; uint Pad0;\n"
"    float Aberration; float Vignette; float Grain; float Bloom;\n"
"    float Flare; float BloomCoronas; float Pad2; float Pad3;\n"
"};\n"
"\n"
"float Speckle(uint2 P, uint F)\n"
"{\n"
"    uint n = P.x * 1973u + P.y * 9277u + F * 26699u;\n"
"    n = (n ^ 61u) ^ (n >> 16);\n"
"    n *= 9u;\n"
"    n = n ^ (n >> 4);\n"
"    n *= 0x27d4eb2du;\n"
"    n = n ^ (n >> 15);\n"
"    return (float)(n & 0xFFFFFFu) / 16777215.0f;\n"
"}\n"
"\n"
"// The grain at a size of its own, not the pixel's: GRAIN_ROWS grains from the top of the picture to its foot whatever\n"
"// its resolution - one pixel apiece at 540 lines, four at 4K - so the film looks the same at every size. Each grain a\n"
"// value of its own, blended smoothly into its neighbours, afresh every frame. The blend narrows how far the values\n"
"// spread, and GRAIN_SPREAD gives that back, so Grain means what it meant for one value a pixel.\n"
"#define GRAIN_ROWS   540.0f\n"
"#define GRAIN_SPREAD 1.7f\n"
"float GrainAt(float2 P, uint F)\n"
"{\n"
"    float2 I = floor(P);\n"
"    float2 f = P - I;\n"
"    f = f * f * (3.0f - 2.0f * f);\n"
"    uint2  i = uint2(I);\n"
"    float  a = Speckle(i, F), b = Speckle(i + uint2(1u, 0u), F);\n"
"    float  c = Speckle(i + uint2(0u, 1u), F), d = Speckle(i + uint2(1u, 1u), F);\n"
"    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);\n"
"}\n"
"\n"
"[numthreads(8, 8, 1)]\n"
"void LookCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= Size.x || t.y >= Size.y) return;\n"
"    float2 Half = float2(Size) * 0.5f;\n"
"    // How far out this pixel is, 0 in the middle and 1 at a corner. Everything a LENS does grows with this, and\n"
"    // nothing a lens does happens in the middle.\n"
"    float2 Off  = (float2(t.xy) + 0.5f - Half) / Half;\n"
"    float  Out2 = saturate(length(Off) * 0.70710678f);\n"
"\n"
"    float4 C;\n"
"    if (Aberration > 0.0f)\n"
"    {\n"
"        // The lens brings the colours to a focus at slightly different places, so towards the edge they draw apart\n"
"        // ALONG THE LINE OUT FROM THE MIDDLE - never sideways, which is what a plain three-way offset would do and\n"
"        // what makes a picture look merely broken.\n"
"        float2 Step = normalize(Off + 1e-6f) * (Aberration * Out2 * Out2);\n"
"        int2   R    = clamp(int2(float2(t.xy) + Step), int2(0, 0), int2(Size) - 1);\n"
"        int2   B    = clamp(int2(float2(t.xy) - Step), int2(0, 0), int2(Size) - 1);\n"
"        C.r = Picture.Load(int3(R, 0)).r;\n"
"        C.g = Picture.Load(int3(t.xy, 0)).g;\n"
"        C.b = Picture.Load(int3(B, 0)).b;\n"
"        C.a = 1.0f;\n"
"    }\n"
"    else\n"
"        C = Picture.Load(int3(t.xy, 0));\n"
"\n"
"    // The halos of what is bright - the picture's, and the coronas' own, wider one - made small and blurred\n"
"    // elsewhere (FBloom) and read back with a filter: a halo has no detail to lose. ADDED and not mixed in: light\n"
"    // adds to light, it does not replace it.\n"
"    if (Bloom > 0.0f)\n"
"    {\n"
"        float2 Uv = (float2(t.xy) + 0.5f) / float2(Size);\n"
"        C.rgb += Halo.SampleLevel(Smooth, Uv, 0.0f).rgb * Bloom;\n"
"    }\n"
"    if (BloomCoronas > 0.0f)\n"
"    {\n"
"        float2 Uv = (float2(t.xy) + 0.5f) / float2(Size);\n"
"        C.rgb += Coronas.SampleLevel(Smooth, Uv, 0.0f).rgb * BloomCoronas;\n"
"    }\n"
"\n"
"    // The lens's ghosts. A bright thing throws a row of images of itself ACROSS THE MIDDLE of the picture,\n"
"    // because that is what a stack of glass elements does with a strong light. So each is read from the\n"
"    // ghosts' own small picture at the MIRRORED place, stepped towards the middle, and each is weaker than the\n"
"    // last. What throws a ghost is a corona's bright middle (FBloom), so a lamp the engine shows no corona for\n"
"    // throws none.\n"
"    if (Flare > 0.0f)\n"
"    {\n"
"        float2 Uv   = (float2(t.xy) + 0.5f) / float2(Size);\n"
"        float2 Flip = 1.0f - Uv;\n"
"        float2 Walk = 0.5f - Flip;\n"
"        float3 G    = 0.0f;\n"
"        [unroll] for (int i = 0; i < 3; i++)\n"
"        {\n"
"            float  Step = 0.34f * (float)(i + 1);\n"
"            float2 At   = Flip + Walk * Step;\n"
"            // The glass does not bring the three colours to the same place, so each is taken from its own.\n"
"            float2 Part = Walk * (0.005f * (float)(i + 1));\n"
"            float3 One;\n"
"            One.r = Ghosts.SampleLevel(Smooth, At + Part, 0.0f).r;\n"
"            One.g = Ghosts.SampleLevel(Smooth, At, 0.0f).g;\n"
"            One.b = Ghosts.SampleLevel(Smooth, At - Part, 0.0f).b;\n"
"            // A ghost is a disc: it has to fade before the edge of the picture, or it reads as a bright\n"
"            // rectangle sliding about.\n"
"            float Soft = 1.0f - saturate(length(At - 0.5f) * 1.9f);\n"
"            G += One * (Soft * Soft) / (float)(i + 1);\n"
"        }\n"
"        // And the ring the lens throws around the middle. Its width is what keeps it a RING and not a wash.\n"
"        float2 Mid  = 0.5f - Uv;\n"
"        float  Away = length(Mid);\n"
"        float2 Ring = Uv + normalize(Mid + 1e-6f) * 0.30f;\n"
"        float  Band = 1.0f - saturate(abs(Away - 0.24f) * 5.0f);\n"
"        G += Ghosts.SampleLevel(Smooth, Ring, 0.0f).rgb * (Band * Band * 0.6f);\n"
"        C.rgb += G * Flare;\n"
"    }\n"
"\n"
"    // The corners darker, smoothly and from about half way out - a vignette with a hard start is a black ring.\n"
"    if (Vignette > 0.0f)\n"
"        C.rgb *= 1.0f - Vignette * smoothstep(0.45f, 1.0f, Out2);\n"
"\n"
"    // And the grain. Multiplied rather than added, because grain is the film's own unevenness in how much light it\n"
"    // caught - added grain shows up in the black of a dark corridor, where film has none.\n"
"    if (Grain > 0.0f)\n"
"    {\n"
"        float2 P = (float2(t.xy) + 0.5f) * (GRAIN_ROWS / (float)Size.y);\n"
"        float  g = (GrainAt(P, Frame) - 0.5f) * 2.0f * GRAIN_SPREAD;\n"
"        C.rgb *= 1.0f + g * Grain;\n"
"    }\n"
"    Out[t.xy] = C;\n"
"}\n";

UBOOL FFilmLook::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (Look)
        return 1;

    D3D12_DESCRIPTOR_RANGE Ranges[5];
    appMemzero(Ranges, sizeof(Ranges));
    Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors     = 1;
    Ranges[0].BaseShaderRegister = 0;
    Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors     = 1;
    Ranges[1].BaseShaderRegister = 0;
    Ranges[1].OffsetInDescriptorsFromTableStart = 1;
    Ranges[2].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[2].NumDescriptors     = 1;
    Ranges[2].BaseShaderRegister = 1;                 // t1: the halo
    Ranges[2].OffsetInDescriptorsFromTableStart = 2;
    Ranges[3].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[3].NumDescriptors     = 1;
    Ranges[3].BaseShaderRegister = 2;                 // t2: the ghosts' own
    Ranges[3].OffsetInDescriptorsFromTableStart = 3;
    Ranges[4].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[4].NumDescriptors     = 1;
    Ranges[4].BaseShaderRegister = 3;                 // t3: the coronas' halo
    Ranges[4].OffsetInDescriptorsFromTableStart = 4;

    D3D12_ROOT_PARAMETER P[2];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 12;
    P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[1].DescriptorTable.NumDescriptorRanges = 5;
    P[1].DescriptorTable.pDescriptorRanges   = Ranges;

    // The halo is a quarter of the size, so it is READ WITH A FILTER rather than point sampled: point sampling
    // a quarter-size halo puts its own blockiness into a picture that has none.
    D3D12_STATIC_SAMPLER_DESC Smp;
    appMemzero(&Smp, sizeof(Smp));
    Smp.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Smp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Smp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Smp.MaxLOD   = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = 2;
    Desc.pParameters       = P;
    Desc.NumStaticSamplers = 1;
    Desc.pStaticSamplers   = &Smp;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("film look: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    TComRef<ID3DBlob> Cs;
    if (FAILED(RdxCompile(GFilmLookShader, strlen(GFilmLookShader), "FilmLook", nullptr, nullptr,
                          "LookCS", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Cs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("film look: the shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature     = Root.Get();
    Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
    Pso.CS.BytecodeLength  = Cs->GetBufferSize();
    if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Look.VoidSlot())))
    {
        RdxLog(TEXT("film look: the pipeline could not be made"));
        return 0;
    }
    return 1;
}

void FFilmLook::Shutdown()
{
    ReleaseTargets();
    Look.Release();
    Root.Release();
}

void FFilmLook::ReleaseTargets()
{
    Work.Release();
    Heap.Release();
    TargetW = TargetH = 0;
    TargetFormat = DXGI_FORMAT_UNKNOWN;
    WriteFormat  = DXGI_FORMAT_UNKNOWN;
    PictureSeen = nullptr;
    HaloSeen    = nullptr;
    GhostSeen   = nullptr;
    CoronaSeen  = nullptr;
}

UBOOL FFilmLook::EnsureTargets(ID3D12Device* Device, INT W, INT H, DXGI_FORMAT Storage, DXGI_FORMAT Write)
{
    const DXGI_FORMAT Format = Storage;
    if (!Device || W <= 1 || H <= 1 || Format == DXGI_FORMAT_UNKNOWN || Write == DXGI_FORMAT_UNKNOWN)
        return 0;
    if (Work && TargetW == W && TargetH == H && TargetFormat == Format && WriteFormat == Write)
        return 1;
    ReleaseTargets();

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
    Td.Format           = Format;
    Td.SampleDesc.Count = 1;
    Td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (RdxForcedFailure(TEXT("film look")) ||
        FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               __uuidof(ID3D12Resource), Work.VoidSlot())))
    {
        RdxLog(TEXT("film look: the %ix%i working picture could not be made - the pass is skipped"), W, H);
        ReleaseTargets();
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = 5;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot())))
    {
        RdxLog(TEXT("film look: its view heap could not be made - the pass is skipped"));
        ReleaseTargets();
        return 0;
    }
    HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE Uav = Heap->GetCPUDescriptorHandleForHeapStart();
    Uav.ptr += HeapStep;
    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = Write;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(Work.Get(), nullptr, &Ud, Uav);

    TargetW = W;
    TargetH = H;
    TargetFormat = Format;
    WriteFormat = Write;
    RdxLog(TEXT("film look: its working picture is %ix%i"), W, H);
    return 1;
}

void FFilmLook::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Picture,
                       DXGI_FORMAT Read, ID3D12Resource* Halo, ID3D12Resource* CoronaHalo, ID3D12Resource* Ghosts,
                       INT W, INT H, const FLook& L)
{
    if (!Cmd || !Device || !Look || !Work || !Heap || !Picture)
        return;
    // With no halo to add, the view is pointed at the picture instead: a table whose descriptor was never
    // written is undefined behaviour, and the shader will not read it anyway.
    ID3D12Resource* const Use = Halo ? Halo : Picture;
    // A texture whose view the heap holds was made again since: every view anew (ViewGeneration.h).
    if (ViewsAt != RdxViewGeneration())
    {
        ViewsAt     = RdxViewGeneration();
        PictureSeen = nullptr;
        HaloSeen    = nullptr;
        GhostSeen   = nullptr;
        CoronaSeen  = nullptr;
    }
    if (HaloSeen != Use)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Hd;
        appMemzero(&Hd, sizeof(Hd));
        Hd.Format                  = Halo ? DXGI_FORMAT_R16G16B16A16_FLOAT : Read;
        Hd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Hd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Hd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += 2 * HeapStep;
        Device->CreateShaderResourceView(Use, &Hd, At);
        HaloSeen = Use;
    }
    ID3D12Resource* const Ghost = Ghosts ? Ghosts : Picture;
    if (GhostSeen != Ghost)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Gd;
        appMemzero(&Gd, sizeof(Gd));
        Gd.Format                  = Ghosts ? DXGI_FORMAT_R16G16B16A16_FLOAT : Read;
        Gd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Gd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Gd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += 3 * HeapStep;
        Device->CreateShaderResourceView(Ghost, &Gd, At);
        GhostSeen = Ghost;
    }
    ID3D12Resource* const Corona = CoronaHalo ? CoronaHalo : Picture;
    if (CoronaSeen != Corona)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Kd;
        appMemzero(&Kd, sizeof(Kd));
        Kd.Format                  = CoronaHalo ? DXGI_FORMAT_R16G16B16A16_FLOAT : Read;
        Kd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Kd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Kd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE At = Heap->GetCPUDescriptorHandleForHeapStart();
        At.ptr += 4 * HeapStep;
        Device->CreateShaderResourceView(Corona, &Kd, At);
        CoronaSeen = Corona;
    }
    if (PictureSeen != Picture)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
        appMemzero(&Sd, sizeof(Sd));
        Sd.Format                  = Read;
        Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Sd.Texture2D.MipLevels     = 1;
        Device->CreateShaderResourceView(Picture, &Sd, Heap->GetCPUDescriptorHandleForHeapStart());
        PictureSeen = Picture;
    }

    UINT C[12];
    appMemzero(C, sizeof(C));
    FLOAT* F = (FLOAT*)C;
    C[0] = (UINT)W; C[1] = (UINT)H; C[2] = (UINT)(L.Frame & 0xFFFF); C[3] = 0;
    F[4] = L.Aberration; F[5] = L.Vignette; F[6] = L.Grain; F[7] = Halo ? L.Bloom : 0.0f;
    F[8] = Ghosts ? L.Flare : 0.0f;
    F[9] = CoronaHalo ? L.BloomCoronas : 0.0f;

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetComputeRootSignature(Root.Get());
    Cmd->SetPipelineState(Look.Get());
    Cmd->SetComputeRoot32BitConstants(0, 12, C, 0);
    Cmd->SetComputeRootDescriptorTable(1, Heap->GetGPUDescriptorHandleForHeapStart());
    Cmd->Dispatch((UINT)((W + 7) / 8), (UINT)((H + 7) / 8), 1);

    D3D12_RESOURCE_BARRIER U;
    appMemzero(&U, sizeof(U));
    U.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    U.UAV.pResource = Work.Get();
    Cmd->ResourceBarrier(1, &U);
}
