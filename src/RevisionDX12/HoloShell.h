// SecondRevision - a hologram's nearest surface alone (HdtpHoloFix, On and Old).
//
// The game draws a hologram as it draws any translucent mesh: every triangle laid over what is there, none of them
// hiding another. An original model is a hull, and shows its far side through its near one - the collar behind the
// head across the mouth. An HDTP model has things inside it as well - teeth behind the lips, the inside of the mouth
// - and drawn so they shine through the face. Here a hologram's triangles are laid in two goes: first their depth
// alone, into a copy of the world's depth, so that the copy holds the nearest of them at every pixel - and the world
// where the world is the nearer; then their colour, as the translucent draw lays it, only where a triangle is that
// nearest one. What is inside the model or behind it stays there, and the hologram is as see-through as it was.
#pragma once

class FHoloShell
{
public:
    FHoloShell() {}
    ~FHoloShell() { Shutdown(); }

    // A corner of a triangle as the solid pass has it: the engine's pixels, the reversed depth (one over the view's
    // depth), the texture's place, and the colour the engine lit it in.
    struct FVertex
    {
        FLOAT X, Y, Depth;
        FLOAT U, V;
        FLOAT R, G, B;
    };

    // Triangles of one texture, one after the other in the frame's list of corners.
    struct FRun
    {
        INT                         First;
        INT                         Count;          // corners
        D3D12_GPU_DESCRIPTOR_HANDLE Texture;        // its view in the heap the caller names
        UBOOL                       bNoSmooth;      // the texture unfiltered
    };

    // What the pixels need of the frame, as the solid pass has it: two over the engine's width and height, the
    // world's jitter in clip units, the lamps' strength it lays on every mesh, and the world's textures' mip bias.
    // And how many of the game's translucent layers the nearest surface is laid as (RdxMath::HoloShellLayers).
    struct FFrame
    {
        FLOAT InvHalf[2];
        FLOAT Jitter[2];
        FLOAT LightBoost;
        FLOAT MipBias;
        FLOAT Layers;
    };

    // The root signature, the shaders, the depth view's heap and the first lists; the pipelines come with the scene's
    // and the depth's formats, the depth target with the world's depth (Record).
    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Root.Get() != nullptr && DsvHeap.Get() != nullptr; }

    // Lays the triangles over the scene target (SceneRtv, of SceneFormat). WorldDepth is the copy of the world's
    // depth, in the state it can be copied from, DsvFormat what a depth view of it reads it as: the pass copies it into
    // a depth target of its own and leaves it as it was. TextureHeap holds the runs' textures' views, and is left
    // set. The viewport and the scissor are the caller's. False where it could not be laid: nothing was drawn.
    UBOOL Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, DXGI_FORMAT SceneFormat,
                 D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv, ID3D12Resource* WorldDepth, DXGI_FORMAT DsvFormat,
                 ID3D12DescriptorHeap* TextureHeap, const FFrame& Frame, const FVertex* Verts, INT VertCount,
                 const FRun* Runs, INT RunCount, INT FrameIndex);

private:
    enum { FRAMES = 3, FIRST_CORNERS = 4096 };
    UBOOL HoldCorners(ID3D12Device* Device, INT Slot, INT Wanted);
    UBOOL EnsurePso(ID3D12Device* Device, DXGI_FORMAT SceneFormat, DXGI_FORMAT DsvFormat);
    UBOOL EnsureDepth(ID3D12Device* Device, ID3D12Resource* Like, DXGI_FORMAT DsvFormat);

    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  PsoDepth, PsoColour;
    DXGI_FORMAT                   PsoScene = DXGI_FORMAT_UNKNOWN, PsoDsv = DXGI_FORMAT_UNKNOWN;
    TComRef<ID3DBlob>             Vs, PsDepth, PsColour;    // kept for pipelines made again in other formats
    TComRef<ID3D12DescriptorHeap> DsvHeap;
    TComRef<ID3D12Resource>       Depth;                    // kept as a copy's target between frames
    UINT64                        DepthWidth  = 0;
    UINT                          DepthHeight = 0;
    DXGI_FORMAT                   DepthFormat = DXGI_FORMAT_UNKNOWN, DepthDsv = DXGI_FORMAT_UNKNOWN;
    TComRef<ID3D12Resource>       Upload[FRAMES];           // upload buffers, kept mapped
    void*                         Mapped[FRAMES] = { nullptr, nullptr, nullptr };
    INT                           Room[FRAMES]   = { 0, 0, 0 };     // corners each slot's list holds
};
