// SecondRevision - motion blur: the picture smeared along what MOVED in it, not along the whole screen.
//
// The renderer already works out, for every pixel, where that pixel was on the previous frame (FFinalPass::DrawMotion,
// the MotionTarget). That is the hard half. This pass reads it and takes a few samples back along it, which is what a
// camera's shutter does: it gathers the light of everything that passed through the pixel while it was open.
//
// Taken BACKWARDS along the vector and not both ways: forwards is where the pixel is GOING, and a shutter cannot see
// the future. Blurring both ways is the cheap version and it shows - edges gain a halo ahead of them.
//
// A pass of its own rather than a branch in the final one: it wants the picture as a texture and produces another
// picture, and everything in the final pass writes into the target it was given.
#pragma once

class FMotionBlur
{
public:
    FMotionBlur() {}
    ~FMotionBlur() { Shutdown(); }

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Blur.Get() != nullptr; }

    // The working picture, made alike to the scene's so that it can simply be copied back over it. A change of size
    // or format makes it again. False if the device refused, and then the pass is skipped for good.
    // Three formats, for the reason FilmLook.h gives: the scene's own may be typeless, and every view has to
    // name the reading it wants.
    UBOOL EnsureTargets(ID3D12Device* Device, INT W, INT H, DXGI_FORMAT Storage, DXGI_FORMAT Write);
    void  ReleaseTargets();

    struct FBlur
    {
        FLOAT Strength;      // how much of one frame's movement is smeared; 1 is the whole of it
        FLOAT MaxPixels;     // no sample reaches further than this, whatever the vector says
        FLOAT ScaleX, ScaleY;// the motion target's size over this picture's, where they differ
        INT   Taps;
    };

    // Reads Scene and the motion vectors, writes the blurred picture into the pass's own target. The caller then
    // copies that back over the scene - this pass never writes into what it is reading.
    void Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Scene, DXGI_FORMAT SceneFormat,
                ID3D12Resource* Motion, INT W, INT H, const FBlur& Blur);

    ID3D12Resource* Target() const { return Work.Get(); }

private:
    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  Blur;
    TComRef<ID3D12Resource>       Work;      // the blurred picture, the scene's own size and format
    TComRef<ID3D12DescriptorHeap> Heap;      // [0] the scene, [1] the motion vectors, [2] the working picture (UAV)
    UINT                          HeapStep = 0;
    INT                           TargetW = 0, TargetH = 0;
    DXGI_FORMAT                   TargetFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT                   WriteFormat  = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource*               SceneSeen  = nullptr;   // whose views the heap holds; new ones need new views
    INT                           ViewsAt    = 0;         // RdxViewGeneration when the views were written
    ID3D12Resource*               MotionSeen = nullptr;
};
