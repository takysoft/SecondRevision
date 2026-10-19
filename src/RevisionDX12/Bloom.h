// SecondRevision - bloom: the halo a bright thing wears, because no lens is perfect and no eye is either.
//
// Laci's worry about this one was that the levels' lights are in the wrong places. That worry does not apply here:
// bloom does not use where a LAMP is, it uses what is BRIGHT ON THE SCREEN - and the coronas the engine draws there.
// Wherever the picture is bright - a lamp, a sign, a reflection, a muzzle flash - is where the halo goes, and that is
// right whatever the level's own light actors are doing. It is off by default all the same, because whether a game
// wants one is taste.
//
// Two halos make it, added together: one of the picture's bright parts, and a wider one of the engine's coronas alone,
// drawn again small (DrawCoronas). The coronas' picture gives the lens flare its ghosts too.
//
// The work is done small: the bright parts are taken at a QUARTER of the width and height, blurred there in two
// passes - across, then down - and read back with a bilinear filter. A halo has no detail to lose, and blurring at
// full size would cost sixteen times as much for a picture nobody can tell apart.
//
// The result is not laid on here. FFilmLook adds it, because that pass already reads the whole picture and writes
// the whole picture, and doing it there costs one add instead of another read and another copy of a 4K frame.
#pragma once

// One of the engine's coronas as the frame drew it (FBackend::NoteCorona): its corners in clip space, the part of its
// texture, its colour, and its texture's view in the texture heap.
struct FBloomCorona
{
    FLOAT X0, Y0, X1, Y1;
    FLOAT U0, V0, U1, V1;
    FLOAT R, G, B;
    D3D12_GPU_DESCRIPTOR_HANDLE Texture;
};

// One of the own lamps (FBackend's weapon lamps and the flashlight's image in a mirror or a pane) as the lens's
// ghosts take it: its quad's corners in clip space - about its place on the screen -, its colour at its brightest
// one, and how strongly it shines at the eye, 0..1 - what a mirror or a pane gives back of it already in that.
struct FBloomLamp
{
    FLOAT X0, Y0, X1, Y1;
    FLOAT R, G, B;
    FLOAT Shine;
};

class FBloom
{
public:
    FBloom() {}
    ~FBloom() { Shutdown(); }

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Bright.Get() != nullptr && Blur.Get() != nullptr && CoronaTake.Get() != nullptr &&
                                   CoronaDraw.Get() != nullptr; }

    UBOOL EnsureTargets(ID3D12Device* Device, INT W, INT H);
    void  ReleaseTargets();

    // The coronas' own picture, for their halo and the lens's ghosts: each drawn again, small, added up over black.
    // Their textures live in the texture heap, so that heap is bound for the draws; the draw leaves its own target,
    // viewport and scissor set, and the caller sets its own again.
    void DrawCoronas(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* TextureHeap, const FBloomCorona* List,
                     INT Count);

    // How one small picture is taken: the line under which nothing counts - what keeps a dim corridor from glowing -
    // how far a tap of the blur steps, and how many times the whole blur runs.
    struct FTake
    {
        FLOAT Line;
        FLOAT Spread;
        INT   Rounds;
    };

    // Fills the small blurred pictures, each only where its FTake is given: the halo of the picture's bright parts,
    // the halo of the coronas' own picture, and the lens's ghosts, of the coronas' bright middles. The coronas' two
    // need DrawCoronas first, and are not made without it.
    // Lamps: the own lamps the ghosts are made of as well, drawn into the coronas' picture after the coronas' halo is
    // made (bGlowFresh: DrawCoronas drew it this frame, so it is not cleared first).
    void Record(ID3D12GraphicsCommandList* Cmd, ID3D12Device* Device, ID3D12Resource* Picture, DXGI_FORMAT Read,
                INT W, INT H, const FTake* PictureHalo, const FTake* CoronaHalo, const FTake* Ghosts,
                const FBloomLamp* Lamps = nullptr, INT LampCount = 0, UBOOL bGlowFresh = 0);

    // The small blurred pictures, each null when the last Record did not make it. FFilmLook makes views of its OWN
    // of these, because a descriptor only means anything in the heap it lives in and that pass reads through its own.
    ID3D12Resource* Target() const       { return bMade[HALO] ? Small[HALO].Get() : nullptr; }
    ID3D12Resource* CoronaTarget() const { return bMade[CORONA_HALO] ? Small[CORONA_HALO].Get() : nullptr; }
    ID3D12Resource* GhostTarget() const  { return bMade[GHOSTS] ? Small[GHOSTS].Get() : nullptr; }

private:
    // The small pictures: the picture's halo, the ghosts', the coronas' halo, and one scratch the two-way blur needs -
    // a blur cannot read and write the same picture. The views: [0] the picture, then the small ones to write, then
    // the same to read, and last the coronas' own picture to read (Glow).
    enum { HALO = 0, GHOSTS = 1, CORONA_HALO = 2, SCRATCH = 3, SMALL = 4,
           VIEW_PICTURE = 0, VIEW_WRITE = 1, VIEW_READ = 1 + SMALL, VIEW_GLOW = 1 + SMALL * 2, VIEWS = 2 + SMALL * 2 };

    TComRef<ID3D12RootSignature>  Root;
    TComRef<ID3D12PipelineState>  Bright;
    TComRef<ID3D12PipelineState>  Blur;
    TComRef<ID3D12PipelineState>  CoronaTake;   // the coronas' picture over its own line, texel by texel
    TComRef<ID3D12RootSignature>  CoronaRoot;   // the coronas' draw: its numbers, a texture, a sampler
    TComRef<ID3D12PipelineState>  CoronaDraw;
    TComRef<ID3D12RootSignature>  LampRoot;     // the own lamps' glows: their numbers alone
    TComRef<ID3D12PipelineState>  LampDraw;
    // The own lamps' glows into the coronas' picture, cleared first where bClear, each on a pedestal at the line the
    // ghosts are taken over (Line); the caller's viewport and scissor (W, H) set again after.
    void DrawLamps(ID3D12GraphicsCommandList* Cmd, const FBloomLamp* Lamps, INT Count, UBOOL bClear, INT W, INT H,
                   FLOAT Line);
    TComRef<ID3D12Resource>       Glow;         // the coronas' own picture, the small size, drawn into
    TComRef<ID3D12DescriptorHeap> GlowRtv;
    UBOOL                         bGlowReadable = 0;   // left for CoronaTake to read (the draw moves it back)
    TComRef<ID3D12Resource>       Small[SMALL];
    UBOOL                         bReadable[SMALL] = {};   // left for FFilmLook to read; Record moves them back
    UBOOL                         bMade[SMALL] = {};       // made by the last Record
    TComRef<ID3D12DescriptorHeap> Heap;
    UINT                          HeapStep = 0;
    INT                           SmallW = 0, SmallH = 0;
    INT                           FullW = 0, FullH = 0;
    ID3D12Resource*               PictureSeen = nullptr;
    INT                           ViewsAt     = 0;      // RdxViewGeneration when the views were written
};
