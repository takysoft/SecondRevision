#include "Pch.h"
#include "Bloom.h"
#include "Log.h"
#include "ViewGeneration.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"

// Two steps. The first takes what is bright, at a quarter of the width and height. The second blurs it, once across
// and once down - a blur that can be done in two passes instead of one costs its width twice instead of its width
// squared, and for a halo that is the whole difference between affordable and not.
static const char* GBloomShader =
"Texture2D<float4>   Src : register(t0);\n"
"RWTexture2D<float4> Out : register(u0);\n"
"cbuffer C : register(b0)\n"
"{\n"
"    uint2 Size;    uint2 From;      // this pass's own size, and the size of what it reads\n"
"    float Threshold; float Spread; float2 Way;   // Way: (1,0) across or (0,1) down\n"
"};\n"
"\n"
"// What is bright, shrunk four times. The four texels of each quarter are averaged FIRST and thresholded after:\n"
"// thresholding each texel on its own makes a halo that crawls, because a texel crossing the line on one frame and\n"
"// back on the next flickers the whole quarter.\n"
"[numthreads(8, 8, 1)]\n"
"void BrightCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= Size.x || t.y >= Size.y) return;\n"
"    int2 At = int2(t.xy) * 4;\n"
"    float3 Sum = float3(0.0f, 0.0f, 0.0f);\n"
"    [unroll] for (int y = 0; y < 4; y++)\n"
"    [unroll] for (int x = 0; x < 4; x++)\n"
"    {\n"
"        int2 P = clamp(At + int2(x, y), int2(0, 0), int2(From) - 1);\n"
"        Sum += Src.Load(int3(P, 0)).rgb;\n"
"    }\n"
"    Sum /= 16.0f;\n"
"    // Above the line only, and what is left is how far above it went - so a light just over the line adds almost\n"
"    // nothing and grows from there, instead of appearing all at once.\n"
"    float L = max(max(Sum.r, Sum.g), Sum.b);\n"
"    float Keep = max(L - Threshold, 0.0f);\n"
"    Out[t.xy] = float4(Sum * (Keep / max(L, 1e-4f)), 1.0f);\n"
"}\n"
"\n"
"// Nine taps along one way. The weights are a bell, and they are written out rather than worked out: nine constants\n"
"// cost nothing and a loop that computes them costs every pixel.\n"
"[numthreads(8, 8, 1)]\n"
"void BlurCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= Size.x || t.y >= Size.y) return;\n"
"    const float W[5] = { 0.2270270f, 0.1945946f, 0.1216216f, 0.0540541f, 0.0162162f };\n"
"    float3 Sum = Src.Load(int3(t.xy, 0)).rgb * W[0];\n"
"    [unroll] for (int k = 1; k < 5; k++)\n"
"    {\n"
"        float2 D = Way * ((float)k * Spread);\n"
"        int2 A = clamp(int2(float2(t.xy) + D), int2(0, 0), int2(Size) - 1);\n"
"        int2 B = clamp(int2(float2(t.xy) - D), int2(0, 0), int2(Size) - 1);\n"
"        Sum += (Src.Load(int3(A, 0)).rgb + Src.Load(int3(B, 0)).rgb) * W[k];\n"
"    }\n"
"    Out[t.xy] = float4(Sum, 1.0f);\n"
"}\n"
"\n"
"// The coronas' own picture, already small, for their halo and the ghosts: taken over its line texel by texel,\n"
"// the way BrightCS takes the picture's quarters. A corona is a soft glow from its rim to its middle, and only its\n"
"// bright middle should throw a halo or a ghost.\n"
"[numthreads(8, 8, 1)]\n"
"void CoronaTakeCS(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= Size.x || t.y >= Size.y) return;\n"
"    float3 C = Src.Load(int3(t.xy, 0)).rgb;\n"
"    float L = max(max(C.r, C.g), C.b);\n"
"    float Keep = max(L - Threshold, 0.0f);\n"
"    Out[t.xy] = float4(C * (Keep / max(L, 1e-4f)), 1.0f);\n"
"}\n";

// The coronas drawn again, small, for their own halo (FBloom::DrawCoronas): a strip of four corners made from the
// vertex's number - no vertex buffer - with the corona's texture and colour, added up over black.
static const char* GCoronaShader =
"cbuffer Q : register(b0)\n"
"{\n"
"    float4 Rect;    // the corners in clip space: left, top, right, bottom\n"
"    float4 Uv;      // the texture's part: left, top, right, bottom\n"
"    float4 Tint;    // the corona's own colour\n"
"};\n"
"Texture2D<float4> Tex    : register(t0);\n"
"SamplerState      Smooth : register(s0);\n"
"struct V2P { float4 Pos : SV_Position; float2 Uv : TEXCOORD0; };\n"
"V2P CoronaVS(uint Id : SV_VertexID)\n"
"{\n"
"    float2 K = float2((float)(Id & 1), (float)(Id >> 1));\n"
"    V2P O;\n"
"    O.Pos = float4(lerp(Rect.xy, Rect.zw, K), 0.0f, 1.0f);\n"
"    O.Uv  = lerp(Uv.xy, Uv.zw, K);\n"
"    return O;\n"
"}\n"
"float4 CoronaPS(V2P In) : SV_Target\n"
"{\n"
"    return float4(Tex.Sample(Smooth, In.Uv).rgb * Tint.rgb, 1.0f);\n"
"}\n";

// The own lamps' glows (FBloom::Record's Lamps): a strip of four corners from the vertex's number, each
// corner's place across the quad -1..1, and a glow that falls away from the middle as the lamp's glare does.
static const char* GLampShader =
"cbuffer Q : register(b0)\n"
"{\n"
"    float4 Rect;    // the corners in clip space: left, top, right, bottom\n"
"    float4 Hue;     // the lamp's colour at its brightest one; w the line the ghosts are taken over\n"
"    float4 Shine;   // x how strongly it shines this way, 0..1\n"
"};\n"
"struct V2P { float4 Pos : SV_Position; float2 Across : TEXCOORD0; };\n"
"V2P LampVS(uint Id : SV_VertexID)\n"
"{\n"
"    float2 K = float2((float)(Id & 1), (float)(Id >> 1));\n"
"    V2P O;\n"
"    O.Pos    = float4(lerp(Rect.xy, Rect.zw, K), 0.0f, 1.0f);\n"
"    O.Across = K * 2.0f - 1.0f;\n"
"    return O;\n"
"}\n"
"// The glow stands on a pedestal at the line, over the whole of its disc: what the ghosts take of it is the glow\n"
"// alone, by the lamp's strength all the way down and always of the same shape - a pane's few hundredths make\n"
"// faint ghosts, not none. Kept by the most (the pipeline's blend): two pedestals over each other stay one.\n"
"float4 LampPS(V2P In) : SV_Target\n"
"{\n"
"    const float R = length(In.Across);\n"
"    if (R >= 1.0f)\n"
"        return float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
"    const float Q = 3.0f * R;\n"
"    const float G = (1.0f - smoothstep(0.75f, 1.0f, R)) / ((1.0f + Q * Q) * (1.0f + Q * Q));\n"
"    return float4(Hue.rgb * (Hue.w + Shine.x * (1.0f - Hue.w) * G), 1.0f);\n"
"}\n";

UBOOL FBloom::Init(ID3D12Device* Device)
{
    if (!Device)
        return 0;
    if (Bright && Blur && CoronaTake && CoronaDraw)
        return 1;

    D3D12_DESCRIPTOR_RANGE Ranges[2];
    appMemzero(Ranges, sizeof(Ranges));
    Ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors     = 1;
    Ranges[0].BaseShaderRegister = 0;
    Ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors     = 1;
    Ranges[1].BaseShaderRegister = 0;

    // Two tables, so a step can be pointed at any pair of views without the heap having to hold them side by side.
    D3D12_ROOT_PARAMETER P[3];
    appMemzero(P, sizeof(P));
    P[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    P[0].Constants.ShaderRegister = 0;
    P[0].Constants.Num32BitValues = 8;
    P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[1].DescriptorTable.NumDescriptorRanges = 1;
    P[1].DescriptorTable.pDescriptorRanges   = &Ranges[0];
    P[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    P[2].DescriptorTable.NumDescriptorRanges = 1;
    P[2].DescriptorTable.pDescriptorRanges   = &Ranges[1];

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters = 3;
    Desc.pParameters   = P;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
        FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Root.VoidSlot())))
    {
        RdxLog(TEXT("bloom: the root signature failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    const size_t Len = strlen(GBloomShader);
    struct { const char* Entry; TComRef<ID3D12PipelineState>* Slot; } Steps[3] =
    {
        { "BrightCS", &Bright }, { "BlurCS", &Blur }, { "CoronaTakeCS", &CoronaTake }
    };
    for (INT i = 0; i < 3; i++)
    {
        TComRef<ID3DBlob> Cs;
        if (FAILED(RdxCompile(GBloomShader, Len, "Bloom", nullptr, nullptr, Steps[i].Entry, "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Cs.Slot(), Error.Slot())))
        {
            RdxLog(TEXT("bloom: %s failed: %s"), appFromAnsi(Steps[i].Entry),
                   Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
            return 0;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC Pso;
        appMemzero(&Pso, sizeof(Pso));
        Pso.pRootSignature     = Root.Get();
        Pso.CS.pShaderBytecode = Cs->GetBufferPointer();
        Pso.CS.BytecodeLength  = Cs->GetBufferSize();
        if (FAILED(Device->CreateComputePipelineState(&Pso, __uuidof(ID3D12PipelineState), Steps[i].Slot->VoidSlot())))
        {
            RdxLog(TEXT("bloom: the %s pipeline could not be made"), appFromAnsi(Steps[i].Entry));
            return 0;
        }
    }

    // The coronas' draw (DrawCoronas): twelve numbers, the texture's view in the TEXTURE heap - where the coronas'
    // textures already are - and one smooth sampler.
    {
        D3D12_DESCRIPTOR_RANGE TexRange;
        appMemzero(&TexRange, sizeof(TexRange));
        TexRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        TexRange.NumDescriptors     = 1;
        TexRange.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER G[2];
        appMemzero(G, sizeof(G));
        G[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        G[0].Constants.ShaderRegister            = 0;
        G[0].Constants.Num32BitValues            = 12;
        G[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        G[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        G[1].DescriptorTable.NumDescriptorRanges = 1;
        G[1].DescriptorTable.pDescriptorRanges   = &TexRange;
        G[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC Smp;
        appMemzero(&Smp, sizeof(Smp));
        Smp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Smp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Smp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Smp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Smp.MaxLOD           = D3D12_FLOAT32_MAX;
        Smp.ShaderRegister   = 0;
        Smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC Gd;
        appMemzero(&Gd, sizeof(Gd));
        Gd.NumParameters     = 2;
        Gd.pParameters       = G;
        Gd.NumStaticSamplers = 1;
        Gd.pStaticSamplers   = &Smp;
        TComRef<ID3DBlob> GBlob, GError;
        if (FAILED(D3D12SerializeRootSignature(&Gd, D3D_ROOT_SIGNATURE_VERSION_1, GBlob.Slot(), GError.Slot())) ||
            FAILED(Device->CreateRootSignature(0, GBlob->GetBufferPointer(), GBlob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), CoronaRoot.VoidSlot())))
        {
            RdxLog(TEXT("bloom: the coronas' root signature failed: %s"),
                   GError ? appFromAnsi((const ANSICHAR*)GError->GetBufferPointer()) : TEXT("(no message)"));
            return 0;
        }

        const size_t CLen = strlen(GCoronaShader);
        TComRef<ID3DBlob> Vs, Ps;
        if (FAILED(RdxCompile(GCoronaShader, CLen, "BloomCoronas", nullptr, nullptr, "CoronaVS", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Vs.Slot(), GError.Slot())) ||
            FAILED(RdxCompile(GCoronaShader, CLen, "BloomCoronas", nullptr, nullptr, "CoronaPS", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, Ps.Slot(), GError.Slot())))
        {
            RdxLog(TEXT("bloom: the coronas' shaders failed: %s"),
                   GError ? appFromAnsi((const ANSICHAR*)GError->GetBufferPointer()) : TEXT("(no message)"));
            return 0;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC Gp;
        appMemzero(&Gp, sizeof(Gp));
        Gp.pRootSignature     = CoronaRoot.Get();
        Gp.VS.pShaderBytecode = Vs->GetBufferPointer();
        Gp.VS.BytecodeLength  = Vs->GetBufferSize();
        Gp.PS.pShaderBytecode = Ps->GetBufferPointer();
        Gp.PS.BytecodeLength  = Ps->GetBufferSize();
        // Added up: where two coronas overlap, the halo is made of both.
        D3D12_RENDER_TARGET_BLEND_DESC& B = Gp.BlendState.RenderTarget[0];
        B.BlendEnable           = TRUE;
        B.SrcBlend              = D3D12_BLEND_ONE;
        B.DestBlend             = D3D12_BLEND_ONE;
        B.BlendOp               = D3D12_BLEND_OP_ADD;
        B.SrcBlendAlpha         = D3D12_BLEND_ONE;
        B.DestBlendAlpha        = D3D12_BLEND_ONE;
        B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        B.LogicOp               = D3D12_LOGIC_OP_NOOP;
        B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        Gp.SampleMask                            = UINT_MAX;
        Gp.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        Gp.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        Gp.RasterizerState.DepthClipEnable       = TRUE;
        Gp.DepthStencilState.DepthEnable         = FALSE;
        Gp.DepthStencilState.StencilEnable       = FALSE;
        Gp.PrimitiveTopologyType                 = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Gp.NumRenderTargets                      = 1;
        Gp.RTVFormats[0]                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Gp.SampleDesc.Count                      = 1;
        if (FAILED(Device->CreateGraphicsPipelineState(&Gp, __uuidof(ID3D12PipelineState), CoronaDraw.VoidSlot())))
        {
            RdxLog(TEXT("bloom: the coronas' pipeline could not be made"));
            return 0;
        }

        // The own lamps' glows (Record): twelve numbers, nothing else, into the same picture - kept by the most of
        // what is there and what they bring (LampPS).
        D3D12_ROOT_PARAMETER Lp;
        appMemzero(&Lp, sizeof(Lp));
        Lp.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        Lp.Constants.ShaderRegister = 0;
        Lp.Constants.Num32BitValues = 12;
        Lp.ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC Ld;
        appMemzero(&Ld, sizeof(Ld));
        Ld.NumParameters = 1;
        Ld.pParameters   = &Lp;
        TComRef<ID3DBlob> LBlob, LError, LVs, LPs;
        const size_t LLen = strlen(GLampShader);
        if (FAILED(D3D12SerializeRootSignature(&Ld, D3D_ROOT_SIGNATURE_VERSION_1, LBlob.Slot(), LError.Slot())) ||
            FAILED(Device->CreateRootSignature(0, LBlob->GetBufferPointer(), LBlob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), LampRoot.VoidSlot())) ||
            FAILED(RdxCompile(GLampShader, LLen, "BloomLamps", nullptr, nullptr, "LampVS", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, LVs.Slot(), LError.Slot())) ||
            FAILED(RdxCompile(GLampShader, LLen, "BloomLamps", nullptr, nullptr, "LampPS", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, LPs.Slot(), LError.Slot())))
        {
            RdxLog(TEXT("bloom: the lamps' glows failed: %s"),
                   LError ? appFromAnsi((const ANSICHAR*)LError->GetBufferPointer()) : TEXT("(no message)"));
            return 0;
        }
        Gp.pRootSignature     = LampRoot.Get();
        Gp.VS.pShaderBytecode = LVs->GetBufferPointer();
        Gp.VS.BytecodeLength  = LVs->GetBufferSize();
        Gp.PS.pShaderBytecode = LPs->GetBufferPointer();
        Gp.PS.BytecodeLength  = LPs->GetBufferSize();
        B.BlendOp      = D3D12_BLEND_OP_MAX;
        B.BlendOpAlpha = D3D12_BLEND_OP_MAX;
        if (FAILED(Device->CreateGraphicsPipelineState(&Gp, __uuidof(ID3D12PipelineState), LampDraw.VoidSlot())))
        {
            RdxLog(TEXT("bloom: the lamps' glows' pipeline could not be made"));
            return 0;
        }
    }
    return 1;
}

void FBloom::Shutdown()
{
    ReleaseTargets();
    Bright.Release();
    Blur.Release();
    CoronaTake.Release();
    CoronaDraw.Release();
    CoronaRoot.Release();
    LampDraw.Release();
    LampRoot.Release();
    Root.Release();
}

void FBloom::ReleaseTargets()
{
    for (INT i = 0; i < SMALL; i++)
    {
        Small[i].Release();
        bReadable[i] = 0;
        bMade[i]     = 0;
    }
    Heap.Release();
    SmallW = SmallH = 0;
    FullW = FullH = 0;
    PictureSeen = nullptr;
    Glow.Release();
    GlowRtv.Release();
    bGlowReadable = 0;
}

UBOOL FBloom::EnsureTargets(ID3D12Device* Device, INT W, INT H)
{
    if (!Device || W <= 4 || H <= 4)
        return 0;
    if (Small[0] && FullW == W && FullH == H)
        return 1;
    ReleaseTargets();
    RdxViewsChanged();
    FullW  = W;
    FullH  = H;
    SmallW = (W + 3) / 4;
    SmallH = (H + 3) / 4;

    D3D12_HEAP_PROPERTIES Hp;
    appMemzero(&Hp, sizeof(Hp));
    Hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Td;
    appMemzero(&Td, sizeof(Td));
    Td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Td.Width            = (UINT64)SmallW;
    Td.Height           = (UINT)SmallH;
    Td.DepthOrArraySize = 1;
    Td.MipLevels        = 1;
    // Half floats, not the picture's own format: what is kept here is how far ABOVE the line a thing was, and that
    // has no ceiling of one.
    Td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Td.SampleDesc.Count = 1;
    Td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (INT i = 0; i < SMALL; i++)
    {
        if (RdxForcedFailure(TEXT("bloom")) ||
            FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Td,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   __uuidof(ID3D12Resource), Small[i].VoidSlot())))
        {
            RdxLog(TEXT("bloom: the %ix%i working picture could not be made - the pass is skipped"), SmallW, SmallH);
            ReleaseTargets();
            return 0;
        }
    }
    // The coronas' own picture (DrawCoronas): the same size, but DRAWN into - the three above are the compute passes'.
    {
        D3D12_RESOURCE_DESC Gd = Td;
        Gd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE Black;
        appMemzero(&Black, sizeof(Black));
        Black.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D12_DESCRIPTOR_HEAP_DESC Rd;
        appMemzero(&Rd, sizeof(Rd));
        Rd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Rd.NumDescriptors = 1;
        if (FAILED(Device->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Gd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   &Black, __uuidof(ID3D12Resource), Glow.VoidSlot())) ||
            FAILED(Device->CreateDescriptorHeap(&Rd, __uuidof(ID3D12DescriptorHeap), GlowRtv.VoidSlot())))
        {
            RdxLog(TEXT("bloom: the coronas' %ix%i picture could not be made - the pass is skipped"), SmallW, SmallH);
            ReleaseTargets();
            return 0;
        }
        Device->CreateRenderTargetView(Glow.Get(), nullptr, GlowRtv->GetCPUDescriptorHandleForHeapStart());
        bGlowReadable = 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC Hd;
    appMemzero(&Hd, sizeof(Hd));
    Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    Hd.NumDescriptors = VIEWS;
    Hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), Heap.VoidSlot())))
    {
        RdxLog(TEXT("bloom: its view heap could not be made - the pass is skipped"));
        ReleaseTargets();
        return 0;
    }
    HeapStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC Ud;
    appMemzero(&Ud, sizeof(Ud));
    Ud.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_SHADER_RESOURCE_VIEW_DESC Sd;
    appMemzero(&Sd, sizeof(Sd));
    Sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    Sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    Sd.Texture2D.MipLevels     = 1;
    for (INT i = 0; i < SMALL; i++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE U = Heap->GetCPUDescriptorHandleForHeapStart();
        U.ptr += (UINT64)(VIEW_WRITE + i) * HeapStep;
        Device->CreateUnorderedAccessView(Small[i].Get(), nullptr, &Ud, U);
        D3D12_CPU_DESCRIPTOR_HANDLE R = Heap->GetCPUDescriptorHandleForHeapStart();
        R.ptr += (UINT64)(VIEW_READ + i) * HeapStep;
        Device->CreateShaderResourceView(Small[i].Get(), &Sd, R);
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE R = Heap->GetCPUDescriptorHandleForHeapStart();
        R.ptr += (UINT64)VIEW_GLOW * HeapStep;
        Device->CreateShaderResourceView(Glow.Get(), &Sd, R);
    }
    RdxLog(TEXT("bloom: its working picture is %ix%i, a quarter of %ix%i"), SmallW, SmallH, W, H);
    return 1;
}

void FBloom::DrawCoronas(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* TextureHeap, const FBloomCorona* List,
                         INT Count)
{
    if (!Cmd || !CoronaDraw || !CoronaRoot || !Glow || !GlowRtv || !TextureHeap || !List || Count <= 0)
        return;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Glow.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bGlowReadable)
    {
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cmd->ResourceBarrier(1, &B);
        bGlowReadable = 0;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = GlowRtv->GetCPUDescriptorHandleForHeapStart();
    const FLOAT Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    Cmd->ClearRenderTargetView(Rtv, Black, 0, nullptr);
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SmallW, (FLOAT)SmallH, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, (LONG)SmallW, (LONG)SmallH };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    ID3D12DescriptorHeap* Heaps[1] = { TextureHeap };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(CoronaRoot.Get());
    Cmd->SetPipelineState(CoronaDraw.Get());
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    for (INT i = 0; i < Count; i++)
    {
        const FBloomCorona& K = List[i];
        const FLOAT Q[12] = { K.X0, K.Y0, K.X1, K.Y1, K.U0, K.V0, K.U1, K.V1, K.R, K.G, K.B, 1.0f };
        Cmd->SetGraphicsRoot32BitConstants(0, 12, Q, 0);
        Cmd->SetGraphicsRootDescriptorTable(1, K.Texture);
        Cmd->DrawInstanced(4, 1, 0, 0);
    }

    B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);
    bGlowReadable = 1;
}

void FBloom::DrawLamps(ID3D12GraphicsCommandList* Cmd, const FBloomLamp* Lamps, INT Count, UBOOL bClear, INT W, INT H,
                       FLOAT Line)
{
    if (!Cmd || !LampDraw || !LampRoot || !Glow || !GlowRtv || !Lamps || Count <= 0)
        return;
    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Glow.Get();
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bGlowReadable)
    {
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cmd->ResourceBarrier(1, &B);
        bGlowReadable = 0;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = GlowRtv->GetCPUDescriptorHandleForHeapStart();
    Cmd->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
    if (bClear)
    {
        const FLOAT Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cmd->ClearRenderTargetView(Rtv, Black, 0, nullptr);
    }
    D3D12_VIEWPORT Vp = { 0.0f, 0.0f, (FLOAT)SmallW, (FLOAT)SmallH, 0.0f, 1.0f };
    D3D12_RECT     Sc = { 0, 0, (LONG)SmallW, (LONG)SmallH };
    Cmd->RSSetViewports(1, &Vp);
    Cmd->RSSetScissorRects(1, &Sc);
    Cmd->SetGraphicsRootSignature(LampRoot.Get());
    Cmd->SetPipelineState(LampDraw.Get());
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    for (INT i = 0; i < Count; i++)
    {
        const FBloomLamp& K = Lamps[i];
        const FLOAT Q[12] = { K.X0, K.Y0, K.X1, K.Y1, K.R, K.G, K.B, Line, K.Shine, 0.0f, 0.0f, 0.0f };
        Cmd->SetGraphicsRoot32BitConstants(0, 12, Q, 0);
        Cmd->DrawInstanced(4, 1, 0, 0);
    }
    B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Cmd->ResourceBarrier(1, &B);
    bGlowReadable = 1;
    // The caller's viewport and scissor again; its targets are bound again by whoever draws next.
    D3D12_VIEWPORT Full = { 0.0f, 0.0f, (FLOAT)W, (FLOAT)H, 0.0f, 1.0f };
    D3D12_RECT     FullSc = { 0, 0, (LONG)W, (LONG)H };
    Cmd->RSSetViewports(1, &Full);
    Cmd->RSSetScissorRects(1, &FullSc);
}

void FBloom::Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Picture, DXGI_FORMAT Read,
                    INT W, INT H, const FTake* PictureHalo, const FTake* CoronaHalo, const FTake* Ghosts,
                    const FBloomLamp* Lamps, INT LampCount, UBOOL bGlowFresh)
{
    for (INT i = 0; i < SMALL; i++)
        bMade[i] = 0;
    if (!Cmd || !Device || !Bright || !Blur || !CoronaTake || !Heap || !Picture)
        return;
    for (INT i = 0; i < SMALL; i++)
    {
        if (!Small[i])
            return;
    }
    // A texture whose view the heap holds was made again since: the view anew (ViewGeneration.h).
    if (ViewsAt != RdxViewGeneration())
    {
        ViewsAt     = RdxViewGeneration();
        PictureSeen = nullptr;
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

    ID3D12DescriptorHeap* Heaps[1] = { Heap.Get() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetComputeRootSignature(Root.Get());

    auto Table = [&](INT Index) -> D3D12_GPU_DESCRIPTOR_HANDLE
    {
        D3D12_GPU_DESCRIPTOR_HANDLE T = Heap->GetGPUDescriptorHandleForHeapStart();
        T.ptr += (UINT64)Index * HeapStep;
        return T;
    };

    D3D12_RESOURCE_BARRIER B;
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    auto Move = [&](INT Which, D3D12_RESOURCE_STATES Was, D3D12_RESOURCE_STATES Now)
    {
        B.Transition.pResource   = Small[Which].Get();
        B.Transition.StateBefore = Was;
        B.Transition.StateAfter  = Now;
        Cmd->ResourceBarrier(1, &B);
    };

    // Whatever last frame left for FFilmLook to read has to be writable again before anything is written into it.
    for (INT i = 0; i < SMALL; i++)
    {
        if (bReadable[i])
        {
            Move(i, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            bReadable[i] = 0;
        }
    }

    D3D12_RESOURCE_BARRIER U;
    appMemzero(&U, sizeof(U));
    U.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;

    UINT   C[8];
    FLOAT* F = (FLOAT*)C;

    // One small picture, made and then blurred: the extract from Source into Into, across into the scratch, down into
    // Into again - T.Rounds over. The three differ only in WHICH extract runs, from what and on what line; the blur
    // is the same blur.
    auto Make = [&](ID3D12PipelineState* Extract, INT Source, INT Into, const FTake& T)
    {
        appMemzero(C, sizeof(C));
        C[0] = (UINT)SmallW; C[1] = (UINT)SmallH; C[2] = (UINT)W; C[3] = (UINT)H;
        F[4] = T.Line; F[5] = T.Spread;
        Cmd->SetPipelineState(Extract);
        Cmd->SetComputeRoot32BitConstants(0, 8, C, 0);
        Cmd->SetComputeRootDescriptorTable(1, Table(Source));
        Cmd->SetComputeRootDescriptorTable(2, Table(VIEW_WRITE + Into));
        Cmd->Dispatch((UINT)((SmallW + 7) / 8), (UINT)((SmallH + 7) / 8), 1);
        U.UAV.pResource = Small[Into].Get();
        Cmd->ResourceBarrier(1, &U);

        C[2] = (UINT)SmallW; C[3] = (UINT)SmallH;
        Cmd->SetPipelineState(Blur.Get());
        for (INT Pass = 0; Pass < 2 * Max(T.Rounds, 1); Pass++)
        {
            const INT From = ((Pass & 1) == 0) ? Into : (INT)SCRATCH;
            const INT To   = ((Pass & 1) == 0) ? (INT)SCRATCH : Into;
            Move(From, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            F[6] = ((Pass & 1) == 0) ? 1.0f : 0.0f;
            F[7] = ((Pass & 1) == 0) ? 0.0f : 1.0f;
            Cmd->SetComputeRoot32BitConstants(0, 8, C, 0);
            Cmd->SetComputeRootDescriptorTable(1, Table(VIEW_READ + From));
            Cmd->SetComputeRootDescriptorTable(2, Table(VIEW_WRITE + To));
            Cmd->Dispatch((UINT)((SmallW + 7) / 8), (UINT)((SmallH + 7) / 8), 1);
            U.UAV.pResource = Small[To].Get();
            Cmd->ResourceBarrier(1, &U);

            Move(From, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        // Into holds the finished picture and FFilmLook reads it, so it is left readable.
        Move(Into, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        bReadable[Into] = 1;
        bMade[Into]     = 1;
    };

    // Each where it was asked for; the coronas' two only when their picture is there to read (DrawCoronas).
    if (PictureHalo)
        Make(Bright.Get(), VIEW_PICTURE, HALO, *PictureHalo);
    if (CoronaHalo && bGlowReadable)
        Make(CoronaTake.Get(), VIEW_GLOW, CORONA_HALO, *CoronaHalo);
    // The own lamps go into the coronas' picture only now, after the coronas' halo is made of it: the ghosts are
    // theirs too, the halo is not - with no corona drawn this frame (bGlowFresh) the picture is cleared for them.
    if (Ghosts && Lamps && LampCount > 0)
        DrawLamps(Cmd, Lamps, LampCount, !bGlowFresh, W, H, Ghosts->Line);
    if (Ghosts && bGlowReadable)
        Make(CoronaTake.Get(), VIEW_GLOW, GHOSTS, *Ghosts);
}
