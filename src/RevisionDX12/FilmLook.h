// SecondRevision - the three things a camera does to a picture that a renderer otherwise does not: the lens splitting
// the colours towards the edges, the lens darkening the corners, and the film's own grain.
//
// One pass for all three, because all three belong in the SAME place - on the finished picture, after the
// antialiasing and after the upscaler, and BEFORE the HUD. That order is not a preference:
//
//   - after the antialiasing, because chromatic aberration moves the red and the blue apart and an edge-finder
//     reads those fringes as edges and fights them. The FSR upscaler's colour tests break on them outright.
//   - before the HUD, because none of this belongs to a health bar. A grained, vignetted, colour-split HUD is what
//     happens when these are put at the very end and nobody asks.
//
// Each is separately switchable; with all three off nothing here runs at all.
#pragma once

class FFilmLook
{
public:
    FFilmLook() {}
    ~FFilmLook() { Shutdown(); }

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Look.Get() != nullptr; }

    // THREE formats, not one: the picture's own may be typeless (it has two readings), and a view has to name
    // the reading it wants. A view whose format does not match its resource is not a wrong picture, it is a
    // device removed - which this renderer has already paid for once tonight.
    UBOOL EnsureTargets(ID3D12Device* Device, INT W, INT H, DXGI_FORMAT Storage, DXGI_FORMAT Write);
    void  ReleaseTargets();

    struct FLook
    {
        FLOAT Aberration;   // how far apart the colours are pulled at the very corner, in pixels; 0 is off
        FLOAT Vignette;     // how much the corner is darkened, 0..1; 0 is off
        FLOAT Grain;        // how much grain, 0..1; 0 is off
        FLOAT Bloom;        // how much of the picture's halo is added; 0 is off (and then Halo is not read at all)
        FLOAT BloomCoronas; // how much of the coronas' halo is added; 0 is off
        FLOAT Flare;        // how much of the lens's ghosts is added; 0 is off
        INT   Frame;        // so the grain is not the same speckles every frame
    };

    // Reads Picture, writes the finished one into the pass's own target. The caller copies that back: a pass never
    // writes into what it is reading.
    // Halo, CoronaHalo and Ghosts may each be null: then that one is not added and the rest is done as it was.
    void Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Picture, DXGI_FORMAT Read,
                ID3D12Resource* Halo, ID3D12Resource* CoronaHalo, ID3D12Resource* Ghosts, INT W, INT H,
                const FLook& Look);

    ID3D12Resource* Target() const { return Work.Get(); }

private:
    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  Look;
    TComRef<ID3D12Resource>       Work;
    // [0] the picture, [1] the working copy (UAV), [2] the halo, [3] the ghosts, [4] the coronas' halo
    TComRef<ID3D12DescriptorHeap> Heap;
    UINT                          HeapStep = 0;
    INT                           TargetW = 0, TargetH = 0;
    DXGI_FORMAT                   TargetFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT                   WriteFormat  = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource*               PictureSeen = nullptr;
    INT                           ViewsAt     = 0;      // RdxViewGeneration when the views were written
    ID3D12Resource*               HaloSeen    = nullptr;
    ID3D12Resource*               GhostSeen   = nullptr;
    ID3D12Resource*               CoronaSeen  = nullptr;
};
