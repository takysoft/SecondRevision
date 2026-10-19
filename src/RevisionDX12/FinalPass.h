// SecondRevision - the last pass of a frame: the finished picture, HUD included, goes
// through the brightness curve on its way to the swap chain.
//
// The stock renderer applies the game's Brightness setting to the whole frame as
// out = in ^ (1 / (2.5 * Brightness)) - RdxMath::OutputExponent says how that was
// measured. It cannot be done per draw: blending happens after the pixel shader, and the
// curve has to come after every blend. So the frame is drawn into a target of its own,
// and this pass copies it to the back buffer through the curve.
#pragma once

// For RDX_CLASSIC and RdxMath::EHdrMode, which together decide the colour formats below. The header is
// inline maths with no dependencies of its own - the tests build it on its own.
#include "RenderMath.h"

// The swap chain's format - the back buffer this pass renders into, when the HDR key is Off. One constant,
// because the swap chain, its resize, the render target views and this pass's pipeline must agree.
static const DXGI_FORMAT RDX_BACKBUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

// The four formats the whole chain has to agree on, chosen by the HDR key (RdxMath::EHdrMode). Four and not
// one because a render target, a shader-visible view of it and a compute target are not described the same way:
//
//   Storage     what the scene targets are CREATED as - typeless, so the same memory can carry both a render
//               target view in Draw's format and a plain view for reading the numbers back.
//   Draw        what is drawn into them and read out of them.
//   BackBuffer  the swap chain. It is never typeless: DXGI owns those buffers.
//   Compute     a target an unordered access view writes (FSR 3.1's output). It has to be a format the hardware
//               is REQUIRED to support for typed stores, which R10G10B10A2 is not - so above eight bits this is
//               always the half-float one. It costs one temporary target's bandwidth and is read by a shader,
//               never copied, so it does not have to match the others.
struct FRdxColours
{
    DXGI_FORMAT Storage;
    DXGI_FORMAT Draw;
    DXGI_FORMAT BackBuffer;
    DXGI_FORMAT Compute;
};

// Storage is TYPELESS only where it earns it. Eight bits has two readings of the same memory - plain numbers and
// sRGB - and the linear pipeline (RDX_CLASSIC 0) needs both, so that one stays as it was. Ten bits has no sRGB
// twin and half floats need none: the meaning is in the numbers, one view is enough, and a typed resource is the
// safer thing to hand to somebody else's code.
//
// It is not only tidiness. FSR 3.1 is given our targets and reads their DESCRIPTION to decide how to view them,
// and its own mapping from typeless to readable knows the formats the SDK was written against. Handing it
// R10G10B10A2_TYPELESS produced a picture of scrambled bits - high-frequency noise over every surface and colours
// that had wrapped round - because the bits were being read back through the wrong width. Named formats cannot be
// guessed wrong.
inline FRdxColours RdxColoursFor(RdxMath::EHdrMode HdrMode)
{
    FRdxColours C;
    if (HdrMode == RdxMath::Hdr_On)
    {
        C.Storage    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        C.Draw       = DXGI_FORMAT_R16G16B16A16_FLOAT;
        C.BackBuffer = DXGI_FORMAT_R16G16B16A16_FLOAT;
        C.Compute    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    else if (HdrMode == RdxMath::Hdr_Ten)
    {
        C.Storage    = DXGI_FORMAT_R10G10B10A2_UNORM;
        C.Draw       = DXGI_FORMAT_R10G10B10A2_UNORM;
        C.BackBuffer = DXGI_FORMAT_R10G10B10A2_UNORM;
        C.Compute    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    else
    {
        C.Storage    = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        C.Draw       = RDX_CLASSIC ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        C.BackBuffer = RDX_BACKBUFFER_FORMAT;
        C.Compute    = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return C;
}

// The view a compute shader writes a copy of the scene through, where that copy then goes back over the scene (the
// motion blur, the lens and the film, the depth of field). Being copied back, it has to BE the scene's format: a typed
// one is written through itself, the typeless eight bits through its named reading. Compute is NOT that - ten bits
// keep a half-float Compute, and a half-float view of a ten-bit texture is a different size of pixel: writing through
// one takes the device away.
inline DXGI_FORMAT RdxCopyBackWriteFormat(const FRdxColours& C)
{
    return (C.Storage == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? C.Compute : C.Storage;
}

class FFinalPass
{
public:
    FFinalPass() { Colours = RdxColoursFor(RdxMath::Hdr_Off); }

    // The formats every pipeline here is built for. Set BEFORE Init and before any of the InitX below: a
    // pipeline carries its render target's format, so changing the mode means Shutdown, SetColours, Init -
    // which is what FBackend::SetHdrMode does between two frames.
    void  SetColours(const FRdxColours& C) { Colours = C; }

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();

    // A full-screen triangle into whatever render target is bound, reading the scene
    // through SceneSrv - a descriptor in the shader-visible heap Heap.
    void  Draw(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
               D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, FLOAT Exponent);

    // Supersampling (docs/terv-latvany-roadmap.md 1.2): the scene target is Scale times the output
    // in each direction, and each output pixel is the average of its Scale x Scale block. A root
    // signature and pipeline of their own, built only when supersampling is on - so the vanilla
    // pass keeps its shader and its draw untouched (docs/szabaly-vanilla.md 2.).
    // bCurve: through the brightness curve, or not (FBackend::ResolveScene - the curve comes after the HUD).
    // bLinear: average in approximately linear light instead of the stored numbers.
    UBOOL InitBox(ID3D12Device* Device);
    void  DrawBox(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                  D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, FLOAT Exponent, INT Scale,
                  UBOOL bCurve, UBOOL bLinear);

    // The last pass under HDR=On, in place of Draw: the brightness curve, then the output transform that turns
    // the engine's display-space numbers into the linear light scRGB carries. Built only in that mode, so the
    // two SDR modes keep the shader and the draw they have always had.
    //
    // Knee and Ceiling are in MULTIPLES OF PAPER WHITE, and the transform is the identity below the knee - so
    // whatever the display can do, the part of the picture the compensation anchors comes out unchanged.
    // PaperWhite is the scRGB scale (nits / 80); on a display that is not in HDR mode it is one, because the
    // compositor takes 1.0 for white there.
    struct FHdrOutput
    {
        FLOAT Exponent;
        FLOAT DecodeGamma;
        FLOAT Knee;
        FLOAT Ceiling;
        FLOAT PaperWhite;
        UBOOL bFalseColour;
    };
    UBOOL InitHdr(ID3D12Device* Device);
    void  DrawHdr(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                  D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, const FHdrOutput& Out);

    // G-buffer, first step (docs/terv-latvany-roadmap.md 1.4): a diagnostic picture of the depth buffer instead of the
    // frame. Mode 1: the distance (the stored 1/W's reciprocal, log scale - 16 units white, 16384 black; nothing drawn
    // dark blue). Mode 2: the surface normal rebuilt from the depth, camera space as colour (n * 0.5 + 0.5). Scale: the
    // depth buffer's size over the output's; TanX, TanY: the view's half-angle tangents. Built only when asked for.
    UBOOL InitDepth(ID3D12Device* Device);
    void  DrawDepth(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                    D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv, INT Mode, INT Scale, FLOAT TanX, FLOAT TanY);

    // Shadow maps (docs/terv-latvany-roadmap.md 3.2), built only when asked for. ShadowFactor: into an R8 target, from the
    // world's depth (t0), the depth of what was drawn after the engine's ClearZ (t1), the world's stencil kinds (t2), the
    // light each surface receives (t3) and FShadowMaps' two cube arrays (t4 the level's, t5 the actors') - its constants
    // in a buffer at Constants, laid out as ShadowFactor.hlsl's cbuffer. ShadowApply: that factor onto the scene target
    // - picture x factor + added x (1 - factor), the added target (FSolidPass's third) the view after the factor's - the factor
    // sampled with a linear filter over the whole target (InvW, InvH: one over the scene target's size). Its table is a
    // run of ten (FBackend's SHADOW_WATER_RUN): the factor, the added target, and what the see-through water's pixels
    // take their own shadows by - read where Water is 1 (NoOnTop 1: nothing was drawn after the engine's ClearZ).
    UBOOL InitShadow(ID3D12Device* Device, DXGI_FORMAT SceneFormat);
    void  DrawShadowFactor(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                           D3D12_GPU_DESCRIPTOR_HANDLE Table, D3D12_GPU_VIRTUAL_ADDRESS Constants);
    void  DrawShadowApply(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                          D3D12_GPU_DESCRIPTOR_HANDLE FactorSrv, FLOAT InvW, FLOAT InvH, FLOAT Water, FLOAT NoOnTop);
    // The water surface's factor's input (GFinalWaterPackShader): the water's own depth and shade (a table of two) at
    // the factor's own samples, into a two-float target of the factor's size.
    UBOOL InitWaterPack(ID3D12Device* Device);
    void  DrawWaterPack(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Table,
                        UINT Scale);

    // Ambient occlusion (RdxMath::EAmbientOcclusion), in three passes and four pipelines, all of them at the shadow
    // factor's size and reading the front of one table: the world's depth (t0), the depth of what was drawn on top of it
    // (t1), the world's depth copied down to this size (t2), and the raw occlusion (t3). Each pass binds only what it
    // reads, so neither target is in the table of the pass that draws it. The constants are RdxMath::AoCbCount root
    // values. DrawAoDepth: the small depth into its R32 target (t0 read). DrawAo: the raw occlusion into its own R8 target
    // (t0-t2). DrawAoBlur: that blurred INTO the shadow factor, by multiplication - or, with bAlone, in place of it, for
    // the view that shows what the occlusion does with the shadows taken away (t0-t3).
    UBOOL InitAo(ID3D12Device* Device);
    void  DrawAoDepth(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Table,
                      const UINT* Constants);
    void  DrawAo(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Table,
                 const UINT* Constants);
    void  DrawAoBlur(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Table,
                     const UINT* Constants, UBOOL bAlone);
    // The RT shadows' factor smoothed (FBackend::ApplyShadows): the factor the rays gave, drawn first into a target of its
    // own (t4 of the occlusion's table), averaged over one four-by-four tile into the shadow factor, in place of it. The
    // occlusion's table and constants: the world's depth (t0) for the weights, what was drawn on top (t1) to leave out.
    UBOOL InitRtFactorBlur(ID3D12Device* Device);
    void  DrawRtFactorBlur(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Table,
                           const UINT* Constants);

    // A short caption (FBackend::ShowCaption): a dark box with light 5x7 dots (RdxMath::CaptionGlyph), alpha-blended into
    // whatever back-buffer-format target is bound. RdxMath::CaptionConstants (64) constants: X Y (the box's top left,
    // pixels), Scale (pixels a dot), Count (characters, at most RdxMath::CaptionMaxChars, 30), then two values a
    // character - its dots as CaptionGlyph packs them. The shader colours every pixel it runs for, so the scissor rectangle
    // must be the box. Built on first use.
    UBOOL InitCaption(ID3D12Device* Device);
    void  DrawCaption(ID3D12GraphicsCommandList* Cmd, const UINT* Constants);

    // Upscaling (docs/terv-latvany-roadmap.md 2.3) where no temporal upscaler runs: the scene target, smaller than the
    // output, scaled up into whatever back-buffer-format target is bound, at the output's viewport - DrawLinear, a plain
    // bilinear filter in one pass (InvW, InvH: one over the output's size). InitUpscale builds it.
    UBOOL InitUpscale(ID3D12Device* Device);

    // FXAA on the finished picture (AntiAliasing=FXAA). DrawFxaa's Span is how many pixels it may reach along an edge.
    UBOOL InitFxaa(ID3D12Device* Device);
    // The MSAA depth resolve. D3D12 resolves colour formats only, so the depth - and, with the shadows on, the
    // surface kind in the stencil beside it - comes down to one sample in a pass of our own, writing sample 0.
    UBOOL InitMsaaDepth(ID3D12Device* Device, INT Samples, UBOOL bStencil);
    void  DrawMsaaDepth(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                        D3D12_GPU_DESCRIPTOR_HANDLE Srv);
    UBOOL HasMsaaDepth() const { return MsaaDepthPipeline ? 1 : 0; }
    // What the engine draws after the world - the weapon in hand - into a copy of the world's depth wherever it is the
    // nearer (FBackend::PutAfterWorldDepth): the volumes' walk stops at that copy, so only what stands in front of
    // the weapon lies over it. Srv: that depth buffer's view; a pipeline a depth format, bStencil picks.
    UBOOL InitDepthNearer(ID3D12Device* Device);
    void  DrawDepthNearer(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE Srv,
                          UBOOL bStencil);
    UBOOL HasDepthNearer(UBOOL bStencil) const { return DepthNearerPipeline[bStencil ? 1 : 0] ? 1 : 0; }

    // SMAA on the finished picture (AntiAliasing=SMAA*): the edge pass, then the blending pass. The blend's table
    // holds TWO views in this order: the colour, then the edges.
    UBOOL InitSmaa(ID3D12Device* Device);
    void  DrawSmaaEdges(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                        D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv, FLOAT InvW, FLOAT InvH, FLOAT Span, FLOAT Threshold);
    void  DrawSmaaBlend(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                        D3D12_GPU_DESCRIPTOR_HANDLE PairSrv, FLOAT InvW, FLOAT InvH, FLOAT Span, FLOAT Threshold);
    void  DrawFxaa(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv,
                   FLOAT InvW, FLOAT InvH, FLOAT Span);
    void  DrawLinear(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE SceneSrv,
                     FLOAT InvW, FLOAT InvH);

    // Motion vectors (docs/terv-latvany-roadmap.md 1.6), into an R16G16 target at the scene's size: from the world's
    // depth and the two cameras - where each pixel was on the previous frame, minus where it is now, in pixels. What
    // moves on its own (a walking character, a door) is not in it yet: it gets the movement of the ground behind it.
    // Constants: 32 values, as GFinalMotionShader's cbuffer lays them out.
    UBOOL InitMotion(ID3D12Device* Device);
    void  DrawMotion(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv,
                     const FLOAT* Constants);

    // The see-through water's own depth (FBackend's WaterSurface) written into the world's depth where the water is in
    // front - for the upscaler, which reads the depth to tell one surface from another. Nothing else: no colour, no
    // stencil. DsvFormat is the depth's own; a change of it makes the pipeline again.
    UBOOL InitWaterStamp(ID3D12Device* Device, DXGI_FORMAT DsvFormat);
    void  DrawWaterStamp(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE WaterSrv);

    // The motion vectors of bodies that move by themselves, drawn over the camera's own field - see
    // GFinalMotionObjShader. DsvFormat is the scene's depth format, because this pass is depth-tested against it.
    enum { MOTION_OBJ_CONSTANTS = 48 };     // the floats its draws are given (GFinalMotionObjShader's cbuffer)
    UBOOL InitMotionObjects(ID3D12Device* Device, DXGI_FORMAT DsvFormat, UBOOL bNoDepthTest);
    void  DrawMotionObject(ID3D12GraphicsCommandList* Cmd, const D3D12_VERTEX_BUFFER_VIEW& Vb,
                           INT First, INT Count, const FLOAT* Constants);
    // The same for a body that comes with where each of its vertices was the frame before (Before, a second stream).
    void  DrawMotionObjectVerts(ID3D12GraphicsCommandList* Cmd, const D3D12_VERTEX_BUFFER_VIEW& Now,
                                const D3D12_VERTEX_BUFFER_VIEW& Before, INT Count, const FLOAT* Constants);

    // The motion vectors as a picture (a diagnostic): grey where nothing moved, red towards the right, green
    // downwards, and blue for the distance - Scale pixels of movement is full blue. ScaleX, ScaleY are the scene's
    // size over the output's, since the picture is drawn at the output's size.
    UBOOL InitMotionView(ID3D12Device* Device);
    void  DrawMotionView(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap,
                         D3D12_GPU_DESCRIPTOR_HANDLE MotionSrv, FLOAT Scale, FLOAT ScaleX, FLOAT ScaleY);

private:
    TComRef<ID3D12RootSignature> LinearRoot;
    TComRef<ID3D12PipelineState> LinearPipeline;
    TComRef<ID3D12RootSignature> FxaaRoot;
    TComRef<ID3D12PipelineState> FxaaPipeline;
    TComRef<ID3D12RootSignature> MsaaDepthRoot;
    TComRef<ID3D12PipelineState> MsaaDepthPipeline;
    INT                          MsaaDepthStencil = -1;    // what it was made for: 1 with the stencil, 0 without
    TComRef<ID3D12RootSignature> DepthNearerRoot;
    TComRef<ID3D12PipelineState> DepthNearerPipeline[2];
    TComRef<ID3D12RootSignature> SmaaEdgeRoot;
    TComRef<ID3D12PipelineState> SmaaEdgePipeline;
    TComRef<ID3D12RootSignature> SmaaBlendRoot;
    TComRef<ID3D12PipelineState> SmaaBlendPipeline;
    TComRef<ID3D12RootSignature> MotionRoot;
    TComRef<ID3D12PipelineState> MotionPipeline;
    TComRef<ID3D12RootSignature> WaterStampRoot;
    TComRef<ID3D12PipelineState> WaterStampPipeline;
    DXGI_FORMAT                  WaterStampFormat = DXGI_FORMAT_UNKNOWN;
    TComRef<ID3D12RootSignature> MotionObjRoot;
    TComRef<ID3D12PipelineState> MotionObjPipeline;
    TComRef<ID3D12PipelineState> MotionObjVertsPipeline;
    DXGI_FORMAT                  MotionObjFormat = DXGI_FORMAT_UNKNOWN;    // the depth format they were made for
    TComRef<ID3D12RootSignature> MotionViewRoot;
    TComRef<ID3D12PipelineState> MotionViewPipeline;
    TComRef<ID3D12RootSignature> CaptionRoot;
    TComRef<ID3D12PipelineState> CaptionPipeline;
    TComRef<ID3D12RootSignature> ShadowFactorRoot;
    TComRef<ID3D12PipelineState> ShadowFactorPipeline;
    TComRef<ID3D12RootSignature> ShadowApplyRoot;
    TComRef<ID3D12PipelineState> ShadowApplyPipeline;
    TComRef<ID3D12RootSignature> WaterPackRoot;
    TComRef<ID3D12PipelineState> WaterPackPipeline;
    TComRef<ID3D12RootSignature> AoDepthRoot;
    TComRef<ID3D12PipelineState> AoDepthPipeline;
    TComRef<ID3D12RootSignature> AoRoot;
    TComRef<ID3D12PipelineState> AoPipeline;
    TComRef<ID3D12RootSignature> AoBlurRoot;
    TComRef<ID3D12PipelineState> AoBlurPipeline;
    TComRef<ID3D12RootSignature> AoAloneRoot;
    TComRef<ID3D12PipelineState> AoAlonePipeline;
    TComRef<ID3D12RootSignature> RtFactorBlurRoot;
    TComRef<ID3D12PipelineState> RtFactorBlurPipeline;
    TComRef<ID3D12RootSignature> DepthRootSignature;
    TComRef<ID3D12PipelineState> DepthPipeline;
    TComRef<ID3D12RootSignature> RootSignature;
    TComRef<ID3D12PipelineState> Pipeline;
    TComRef<ID3D12RootSignature> BoxRootSignature;
    TComRef<ID3D12PipelineState> BoxPipeline;
    TComRef<ID3D12RootSignature> HdrRootSignature;
    TComRef<ID3D12PipelineState> HdrPipeline;

    FRdxColours Colours;
};
