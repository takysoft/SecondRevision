// SecondRevision - the player's own body in the pane the mirror probe stands before, where no rays run
// (Reflections=High; MirrorBody.hlsl; docs/tukrozodes-szonda-terv.md 9).
//
// First person never draws the body, and the probe holds no actor - so the glass would show the room behind the player
// with nobody in it. The body's shape and skins are the shadows' own (the caster marked as the own body): mirrored by
// the pane's plane and drawn with the scene's projection into a picture of the scene's size, which the glass's layer
// reads where the pane is (SolidPass.hlsl, GlassMirror) - the body where it stands nearer than what the probe holds.
// One plane a frame: the pane the probe stands before. Every frame the probe runs the picture is cleared first, so a
// frame with no body leaves none in it.
#pragma once

class FMirrorBody
{
public:
    FMirrorBody();
    ~FMirrorBody();

    // The root signature and the pipeline. False, said in the log, where they could not be made.
    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Pipeline.Get() != nullptr; }
    // The picture and its depth, the scene's size: made where they are not, or were made for another size. False where
    // the card refused them.
    UBOOL EnsureTargets(INT Width, INT Height, UBOOL& bMade);
    // The picture's view for the glass's layer into At; a view of nothing where there is no picture yet.
    static void WriteView(ID3D12Device* Device, ID3D12Resource* Picture, D3D12_CPU_DESCRIPTOR_HANDLE At);
    ID3D12Resource* GetPicture() const { return Picture.Get(); }

    // The body as the shadows hold it: its vertices' and their texture places' buffers (twelve bytes a vertex each,
    // read by the vertex's number), its first vertex and how many; its six ways' light and whether that is the
    // engine's own (FShadowMaps::FCasterRt); its skins by key, the texture heap's index plus one.
    struct FBody
    {
        D3D12_GPU_VIRTUAL_ADDRESS Positions;
        D3D12_GPU_VIRTUAL_ADDRESS Places;
        INT                       First;
        INT                       Count;
        DWORD                     Cube[6];
        UBOOL                     bEngineLit;
        DWORD                     Average;      // the skins' average colour (RdxMath::PackRtAlbedo)
        DWORD                     Skins[15];
    };
    // The frame's picture: cleared, and with Body (null: none this frame) drawn into it mirrored by Plane (the world:
    // the unit normal and the distance, dot(N, P) == D), as the camera sees it - Eye and Axes in the world, the scene's
    // tangents and the upscaler's jitter in clip space - its skins read from TextureHeap, by Boost as the lamps are.
    // Leaves the picture readable by a pixel shader, and the command list's targets and viewport the picture's: the
    // caller binds its own again.
    void  Draw(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* TextureHeap, const FBody* Body, const FLOAT* Plane,
               const FLOAT* Eye, const FLOAT* Axes, FLOAT TanX, FLOAT TanY, const FLOAT* Jitter, FLOAT Boost);

private:
    enum { CONSTANTS = 48 };
    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  Pipeline;
    TComRef<ID3D12Resource>       Picture;
    TComRef<ID3D12Resource>       Depth;
    TComRef<ID3D12DescriptorHeap> RtvHeap;
    TComRef<ID3D12DescriptorHeap> DsvHeap;
    D3D12_RESOURCE_STATES         PictureState;
    ID3D12Device*                 Device;
    INT                           Width, Height;
};
