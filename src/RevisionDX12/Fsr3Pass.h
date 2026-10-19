// SecondRevision - AMD FidelityFX Super Resolution 3.1, the SDK's upscaler component
// (docs/terv-latvany-roadmap.md 2.3).
//
// A temporal upscaler: it does not guess the missing pixels from one frame the way a spatial
// upscaler does, it collects them over many. Every frame the scene is drawn a fraction of a pixel
// off (FBackend's jitter), so each frame samples the world in a slightly different place;
// the upscaler then takes the previous frames' picture, moves it to where the world is
// now - that is what the motion vectors are for - and adds this frame's samples to it.
// What it needs from us every frame:
//   - the colour, the depth and the motion vectors, all at the scene's size
//   - the jitter the geometry pass used, in pixels
//   - the time since the previous frame, and the view's vertical angle
// Our depth is 1/W with an infinite far plane, which is exactly the SDK's DEPTH_INVERTED
// plus DEPTH_INFINITE - see Init.
//
// The FidelityFX headers are deliberately not included here. They need the default
// structure packing, and this header is pulled in by the backend, so they would end up
// being parsed far more widely than they should be. The context, the backend interface
// and its scratch memory are therefore kept as opaque pointers: only Fsr3Pass.cpp knows
// what they really are.
#pragma once

class FFsr3Pass
{
public:
    FFsr3Pass();
    ~FFsr3Pass();

    // Builds the upscaler for a scene of RenderW x RenderH going out at OutW x OutH.
    // Returns 0 if anything failed - the caller then falls back to another upscaling
    // pass, so a missing or broken SDK costs the picture nothing but the sharpness.
    // bDebugChecking: the SDK's own parameter checking, which reports through our log. It
    // belongs to a validation run and NOT to a measurement - it costs time on every frame.
    UBOOL Init(ID3D12Device* Device, INT RenderW, INT RenderH, INT OutW, INT OutH, UBOOL bDebugChecking);
    void  Shutdown();

    UBOOL IsReady() const { return bReady; }
    UBOOL Matches(INT RenderW, INT RenderH, INT OutW, INT OutH) const
    {
        return bReady && RenderW == SceneW && RenderH == SceneH && OutW == OutputW && OutH == OutputH;
    }

    // One frame's upscale. Colour, Depth and Motion are at the scene's size, Output at the
    // output's. The caller brings them in the states the SDK is told they are in - the
    // three inputs in NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS - and the
    // SDK leaves them that way, so the caller's own barriers stay correct. JitterX/JitterY:
    // the offset the geometry pass used, in pixels. FrameMs: the time since the previous
    // frame. FovY: the view's vertical angle, in radians. bReset: the camera jumped (a
    // level change, a teleport), so the collected history is thrown away instead of smeared.
    // Reactive: where things move without motion vectors of their own (the water's splashes), how much less of the frames
    // before to keep - null for none.
    UBOOL Dispatch(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Colour, ID3D12Resource* Depth,
                   ID3D12Resource* Motion, ID3D12Resource* Output,
                   FLOAT JitterX, FLOAT JitterY, FLOAT FrameMs, FLOAT FovY, UBOOL bReset, FLOAT Sharpness,
                   ID3D12Resource* Reactive = nullptr);

private:
    UBOOL CreateShared(ID3D12Device* Device);

    void* Context;      // FfxFsr3UpscalerContext
    void* Interface;    // FfxInterface - the DX12 backend's function pointers
    void* Scratch;      // the memory that backend keeps its own bookkeeping in

    // The three the SDK asks us to own rather than owning itself, so that a later effect
    // (frame interpolation) could read the same ones - see ffxFsr3UpscalerGetSharedResourceDescriptions.
    TComRef<ID3D12Resource> PrevDepth;       // reconstructedPrevNearestDepth
    TComRef<ID3D12Resource> DilatedDepth;
    TComRef<ID3D12Resource> DilatedMotion;

    INT   SceneW, SceneH, OutputW, OutputH;
    UBOOL bReady;
    INT   Frames;       // frames that went through the upscaler: the log says so, rather than us assuming it
};
