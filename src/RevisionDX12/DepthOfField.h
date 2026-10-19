// SecondRevision - depth of field, for the conversations' cameras only (docs/terv-latvany-roadmap.md 4., "Depth of field").
//
// In play a first-person camera has nothing to focus on - the eye does that itself. A conversation is the exception: the
// game takes the camera, frames the speaker, and there the picture is a film's shot, which a lens with a depth of field
// would give. The focus is the speaker (FBackend::SetConversationFocus); everything nearer or further than that goes out of
// focus by how far it is from it, as through a lens: the blur's circle is |1 - focus / distance| times the largest one.
//
// Three compute passes over the world's picture, before the HUD (the letterbox and the subtitles stay sharp):
//   Down     - the picture at half size, with each pixel's distance beside its colour (the nearest of the four, so that a
//              near edge keeps its place);
//   Gather   - at half size, a spiral of samples out to the largest circle, a sample counting where ITS circle reaches
//              this pixel, into two pictures: the near layer - whatever is out of focus in front of the focus, its
//              colour and how much of the pixel it covers - and the rest under it, where a sharp thing does not smear
//              and what is behind does not spill over what is in front;
//   Combine  - at full size, the sharp picture where the circle is under a pixel, the half-size blurred one where it is
//              larger, with a short ramp between - and over that the near layer, by as much as it covers: a thing
//              blurred in front of the focus lies soft over what is behind it, sharp or not, as a lens lays it.
// The result is written beside the picture and copied back over it, as the motion blur does: a pass never writes into
// what it reads.
#pragma once

class FDepthOfField
{
public:
    FDepthOfField() {}
    ~FDepthOfField() { Shutdown(); }

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Combine.Get() != nullptr; }

    // The working pictures: three at half size (a float colour and its distance, circle or cover), one at full size
    // alike to the scene's so that it can be copied back over it. False if the device refused; the pass is then
    // skipped.
    UBOOL EnsureTargets(ID3D12Device* Device, INT W, INT H, DXGI_FORMAT Storage, DXGI_FORMAT Write);
    void  ReleaseTargets();

    struct FParams
    {
        FLOAT Focus;        // the view distance that is sharp, in world units (along the view axis, as the depth is)
        FLOAT MaxCoc;       // the largest circle, in full-size pixels (far behind the focus)
        FLOAT Band;         // the part of |1 - focus / distance| that still counts as sharp
        FLOAT Amount;       // 0..1: the fade in and out at a conversation's start and end
    };

    // Reads the scene and the world's depth (both in NON_PIXEL_SHADER_RESOURCE), writes the finished picture into the
    // pass's own full-size target (left in UNORDERED_ACCESS). DepthFormat: the depth's readable view format.
    void Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Scene, DXGI_FORMAT SceneFormat,
                ID3D12Resource* Depth, DXGI_FORMAT DepthFormat, INT W, INT H, const FParams& P);

    ID3D12Resource* Target() const { return Work.Get(); }

private:
    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  Down;
    TComRef<ID3D12PipelineState>  Gather;
    TComRef<ID3D12PipelineState>  Combine;
    TComRef<ID3D12Resource>       Half0;     // half size: the colour, and the distance in alpha
    TComRef<ID3D12Resource>       Half1;     // half size: the blurred colour, without the near layer
    TComRef<ID3D12Resource>       Half2;     // half size: the near layer - its colour by its cover, and the cover
    TComRef<ID3D12Resource>       Work;      // full size: the finished picture, the scene's own format
    // [0] the scene, [1] the depth, [2] Half0, [3] Half1, [4] Half2 (all read) | [5] Half0, [6] Half1, [7] Half2,
    // [8] Work (written)
    TComRef<ID3D12DescriptorHeap> Heap;
    UINT                          HeapStep = 0;
    INT                           TargetW = 0, TargetH = 0;
    DXGI_FORMAT                   TargetFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT                   WriteFormat  = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource*               SceneSeen = nullptr;   // whose views the heap holds; new ones need new views
    INT                           ViewsAt   = 0;         // RdxViewGeneration when the views were written
    ID3D12Resource*               DepthSeen = nullptr;
    DXGI_FORMAT                   DepthFormatSeen = DXGI_FORMAT_UNKNOWN;
};
