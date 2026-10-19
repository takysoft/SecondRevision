// SecondRevision - the flashlight's light off the level's mirrors (FlashlightBeam).
//
// The cone that reaches a mirror goes on from it, as from an eye behind the mirror - the eye the engine draws that
// mirror's view from. What that view shows is therefore exactly what the light off the mirror falls on, and its depth
// says how far along each of the mirror's rays that is. This pass keeps it: at the end of a mirror's view the view's
// depth is written into a map of the scene's size, inside the mirror's own polygons alone, with the mirror's number
// beside it. A surface of the world's view then finds its own image behind the mirror, looks the image's place up in
// the map, and takes the cone's light there if the mirror is at that place and nothing stands nearer along the ray
// (SolidPass.hlsl, BeamOffMirrors).
//
// And the lamp itself as the mirror shows it: a disc of light and its glare at the place of the eye's own image, laid
// over the mirror's view where that place is in sight.
#pragma once

class FBeamBounce
{
public:
    FBeamBounce() {}
    ~FBeamBounce() { Shutdown(); }

    // How much of the flashlight's light a mirror gives back; and the lamp as a mirror shows it - how wide its own
    // disc is, and how far about it its glare is half as strong, in the level's units where the lamp's image stands.
    static constexpr FLOAT MirrorShare = 1.0f;
    static constexpr FLOAT GlassShare  = 0.08f;     // a pane of glass seen face on (SolidPass.hlsl, GLASS_FACE_ON)
    static constexpr FLOAT LampRadius  = 1.5f;
    static constexpr FLOAT GlareRadius = 16.0f;

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return KeepPso.Get() != nullptr; }

    // The map, of the scene's size: made when first asked for, and again when the size changes. False if the device
    // refused - the frame then has no light off its mirrors.
    UBOOL EnsureTarget(ID3D12Device* Device, INT W, INT H);
    void  ReleaseTarget();
    ID3D12Resource* Target() const { return Map.Get(); }
    // Counts the times the map was made: whoever holds a view of it writes the view again when this changes.
    INT   TargetMade() const { return MadeCount; }

    // A frame's start in a frame slot: the slot's vertices are free again, and the map holds nothing.
    void Begin(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, INT FrameIndex);

    // The world's view as the geometry pass's flashlight has it: its centre and its projection in the map's pixels.
    struct FView
    {
        FLOAT Screen[2];
        FLOAT Focal;
        INT   W, H;
    };
    // A mirror's view has ended and Depth holds it (readable): the depth into the map, inside the mirror's polygons -
    // Xyz, three numbers a vertex in the world's view's space, three vertices a triangle - with the mirror's number.
    UBOOL Keep(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Depth, DXGI_FORMAT DepthRead,
               INT FrameIndex, const FLOAT* Xyz, INT Count, INT Number, const FView& View);

    // The map as the only target for whoever else draws into it - the panes of glass, by the geometry pass
    // (PSPaneKeep) - and readable again. False without a map.
    UBOOL BeginDraw(ID3D12GraphicsCommandList* Cmd);
    void  EndDraw(ID3D12GraphicsCommandList* Cmd);

    // The lamp as the mirror shows it, after Keep: where its image is on the screen and how deep, the radii of its
    // disc and of its glare in pixels, its colour and how strongly it shines this way.
    struct FLamp
    {
        FLOAT Centre[2];
        FLOAT Depth;
        FLOAT Core, Halo;
        FLOAT Colour[3];
        FLOAT Strength;
    };
    void Glare(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
               D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv, INT Number, const FLamp& Lamp, const FView& View);
    // A lamp the world's view sees - a weapon lamp shining at the eye (the own lamps) -: the same disc and glare,
    // where the world's depth (Depth, readable, its view's format DepthRead) holds nothing nearer than the lamp
    // about its place. View: the world's view, W and H the scene target's size. Needs no map.
    void LampGlare(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
                   D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv, ID3D12Resource* Depth, DXGI_FORMAT DepthRead,
                   const FLamp& Lamp, const FView& View);

private:
    enum { FRAMES = 3, FIRST_VERTICES = 4096 };
    UBOOL EnsureGlare(ID3D12Device* Device, DXGI_FORMAT SceneFormat);
    // The heap's slot of a depth buffer's view (0 or 1), written when the buffer is first seen or made again.
    INT   DepthSlot(ID3D12Device* Device, ID3D12Resource* Depth, DXGI_FORMAT DepthRead);
    UBOOL HoldVertices(ID3D12Device* Device, INT Slot, INT Wanted);
    void  Turn(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To);

    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3DBlob>             KeepVs, KeepPs, GlareVs, GlarePs, LampGlarePs;
    TComRef<ID3D12PipelineState>  KeepPso, GlarePso, LampGlarePso;
    DXGI_FORMAT                   GlareFormat = DXGI_FORMAT_UNKNOWN;
    TComRef<ID3D12Resource>       Map;
    TComRef<ID3D12DescriptorHeap> Heap;         // [0] [1] the two depth buffers as they come, [2] the map
    TComRef<ID3D12DescriptorHeap> RtvHeap;      // the map's
    UINT                          HeapStep = 0;
    INT                           MapW = 0, MapH = 0;
    INT                           MadeCount = 0;
    ID3D12Resource*               DepthSeen[2]   = { nullptr, nullptr };
    DXGI_FORMAT                   DepthFormat[2] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    INT                           ViewsAt = 0;          // RdxViewGeneration when the depth's views were written
    TComRef<ID3D12Resource>       Vertices[FRAMES];
    void*                         VerticesMapped[FRAMES] = { nullptr, nullptr, nullptr };
    INT                           VerticesRoom[FRAMES]   = { 0, 0, 0 };
    INT                           VerticesUsed[FRAMES]   = { 0, 0, 0 };
    INT                           VerticesWant[FRAMES]   = { 0, 0, 0 };     // what the slot's frame asked for in all
};
