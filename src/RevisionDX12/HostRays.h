// SecondRevision - the rays through the 64-bit host, the renderer's side (docs/terv-rt-host.md; src/Host64/Host64Rays.cpp).
//
// NVIDIA's driver gives a 32-bit process no ray tracing, so on those cards the renderer's own RT passes cannot run; the
// host runs them - the same shader, on the same numbers. This is what feeds it: the shared buffers and textures, the
// copies into them every frame, the frame's list of bodies, and the split of the frame where the rays are traced.
//
// Nothing of the renderer's own is shared. The level's triangles, the bodies' and the passes' tables lie in upload
// buffers, which cannot be shared; the depth buffers and the light target are drawn into all frame long (HostUpscale.h
// says why such textures are copied rather than shared). So everything the host reads is a copy made on the frame's
// command list, and what it writes - the factor, the occlusion - is copied into the renderer's own targets after the
// split.
//
// Knows the backend's D3D12 objects and the two classes whose data the rays are made of, not the backend: FBackend
// decides when, this does the how.
#pragma once

#include "../Host64/Host64Link.h"

class FRtPass;
class FShadowMaps;

class FHostRayFeed
{
public:
    FHostRayFeed();
    ~FHostRayFeed();

    // What a frame's rays read besides the scene and the tables: the world's depth buffer and the one drawn on top, the
    // light target - each in the state named - and the size of the factor's target.
    struct FViews
    {
        ID3D12Resource*       WorldDepth;
        ID3D12Resource*       OnTopDepth;
        D3D12_RESOURCE_STATES DepthState;
        ID3D12Resource*       Light;
        D3D12_RESOURCE_STATES LightState;
        INT                   FactorW, FactorH;
        // The size of the picture whose pixels may write rays down for the host - the scene's own - where the frame
        // may ask any (the surfaces drawn on the host's rays); 0: none are asked, and the share has no list.
        INT                   AskW, AskH;
        // The see-through water's pack (FBackend's WaterPack), in SurfaceState, for the water surface's factor
        // (Host64::RayPass_Surface) - null where the frame does not ask it - and the two's sizes.
        ID3D12Resource*       SurfacePack;
        D3D12_RESOURCE_STATES SurfaceState;
        INT                   SurfacePackW, SurfacePackH;
        INT                   SurfaceW, SurfaceH;
    };

    // Whether this frame's rays can go through the host: the share made for this level, these sizes and this many
    // bodies handed over AND taken. Another one is made and handed over here - the one before is kept until the frames
    // that may still copy into it are done - and the frame runs its fallbacks meanwhile (False), as it does where the
    // link is down, the host traces no rays, or the driver refused to make something shared. Bodies: the frame's
    // (FRtPass::ListScene).
    UBOOL Prepare(ID3D12Device* Device, FHost64Link& Link, const FRtPass& Rt, const FShadowMaps& Shadow,
                  const FViews& Views, INT Bodies);
    // The frame's copies into the shared ones, on Cmd, and its list for the host - after Prepare said yes, and after
    // the frame's constants and table are written (FBackend::FillShadowConstants, FRtPass::FillShadowExclusion).
    // Passes: Host64::ERayPass bits. Every source is back in its state after. False: the link went down meanwhile.
    UBOOL Feed(ID3D12GraphicsCommandList* Cmd, FHost64Link& Link, const FRtPass& Rt, FShadowMaps& Shadow,
               const FViews& Views, INT FrameIndex, INT Bodies, UINT Passes);
    // The split: Cmd closed and executed on Queue, the frame's rays handed to the host - Queue waits for them - and
    // Cmd opened again on Allocator, with nothing set on it. False: Cmd could not be opened again, and the frame
    // cannot go on.
    UBOOL Handover(ID3D12GraphicsCommandList* Cmd, ID3D12CommandAllocator* Allocator, ID3D12CommandQueue* Queue,
                   FHost64Link& Link);
    // What the host wrote, copied into To - a target of the factor's size and format, in ToState, and back in it
    // after. False (and To untouched): To is not of that shape.
    UBOOL CopyFactor(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState);
    UBOOL CopyOcclusion(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState);
    UBOOL CopySurfaceFactor(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState);
    // Whether the share has what the water surface's factor needs (Host64::RayPass_Surface).
    UBOOL HasSurface() const
    {
        return Shared[Host64::Ray_SurfacePack] && Shared[Host64::Ray_SurfaceFactor];
    }

    // The rays' list (SolidPass.hlsl's RDX_RT_ASK and RDX_RT_HOST builds; RenderLimits.h says what a slot holds):
    // a surface's pixels write down the rays each would trace, the host answers each with what it met, and the
    // surface is drawn reading the answers.
    // BeginAsk: the list emptied - its count back at the start, no block at any pixel - and the requests and the
    // heads as a pixel shader writes them. False: the share has no list.
    UBOOL BeginAsk(ID3D12GraphicsCommandList* Cmd);
    // After the asking draws, in Feed's place for a frame whose one pass is the list's: the requests as they change
    // hands and the heads readable, the scene's copies, the masks, the list pass's own constants - how many slots
    // it may read, and the player's own body among the bodies (OwnBody, -1: not there) - and its table: each body's
    // first vertex among the texture places. And what the host traces on by from what a ray met - the frame's table
    // of the mirror rays (FRtPass::PrepareWater's, filled by now) and the level's surfaces - with the passed
    // surfaces' count back at its start. Handover follows it.
    UBOOL FeedList(ID3D12GraphicsCommandList* Cmd, FHost64Link& Link, const FRtPass& Rt, FShadowMaps& Shadow,
                   INT FrameIndex, INT Bodies, INT OwnBody);
    // What the drawing that reads the answers binds: the pixels' heads - a texture of this process's own - the
    // requests, for a block's key, and the answers; and a number that changes when they are other resources. The
    // two buffers are readable by a pixel shader (bOn) from the hand-over's end until the next hand-over, which
    // takes them back by itself.
    ID3D12Resource* GetHeads() const    { return Heads.Get(); }
    ID3D12Resource* GetRequests() const { return Shared[Host64::Ray_Requests].Get(); }
    ID3D12Resource* GetAnswers() const  { return Shared[Host64::Ray_Answers].Get(); }
    UBOOL HasList() const { return Heads && GetRequests() && GetAnswers() && ListRoom >= 2; }
    INT   GetListRoom() const { return ListRoom; }
    // The see-through surfaces the rays passed, as the host wrote them - null where the share has no buffer of
    // them - and its slots. Readable with the answers.
    ID3D12Resource* GetPassed() const   { return Shared[Host64::Ray_Passed].Get(); }
    INT   GetPassedRoom() const { return PassedRoom; }
    // Right after a list's Handover: how many of those slots the host took, copied out for NoteListTaken.
    void  NotePassed(ID3D12GraphicsCommandList* Cmd, INT FrameIndex);
    // Before a frame's Prepare: how many slots each hand-over of the frame that last used this frame slot took -
    // the counts read back, the GPU being done with them by now -, and how many the host took for the surfaces its
    // rays passed. Where any was more than its buffer holds, Prepare makes a share with a longer one; the frames in
    // between have run their fallbacks (the builds that read see the counts themselves).
    void  NoteListTaken(INT FrameIndex);
    // How many of a frame's hand-overs of the list have their counts read back; the ones past them go uncounted.
    enum { LIST_BACK_MOST = 8 };
    // What the frames' hand-overs of the list held since this was last asked, for the log: how many frames had any
    // and how many hand-overs they made; the requests they asked and the see-through surfaces their rays passed, a
    // frame's hand-overs together - summed over the frames, and the most a frame had; and the requests by which
    // of its frame's hand-overs asked them, the first one first. False: no frame had any.
    struct FListCounts
    {
        INT   Frames, Handovers, AskedMost, PassedMost;
        INT64 Asked, Passed;
        INT64 Each[LIST_BACK_MOST];
    };
    UBOOL TakeListCounts(FListCounts& Out);
    INT   GetShareSerial() const { return ShareSerial; }
    void  ListReadable(ID3D12GraphicsCommandList* Cmd, UBOOL bOn);
    // Once a frame: what an older share held is let go once no frame in flight can still copy into it.
    void  Tick();
    // The host is gone, or about to be started again: what was shared with it is nobody's - let go as an older
    // share's is, a few frames on, and the next Prepare makes another.
    void  Abandon();
    // Everything shared let go at once: the caller has the GPU done with it.
    void  Release();

private:
    // An older share's resources wait in their own places: the shared ones, and the three of this process's own
    // that go with a share - the pixels' heads, their target's heap, the list's upload buffer. Room for three shares.
    enum { OWN_COUNT = 3, OLD_ROOM = 3 * (Host64::Ray_Count + OWN_COUNT) };
    UBOOL MakeShare(ID3D12Device* Device, FHost64Link& Link, const FRtPass& Rt, const FShadowMaps& Shadow,
                    const FViews& Views, INT Bodies);
    UBOOL Retire();
    UBOOL CopyOut(ID3D12GraphicsCommandList* Cmd, INT Which, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState);
    // What every frame's rays are traced against, copied over: the level's triangles once a share, the bodies' positions
    // and texture places - the frame's, and the arena's as far as it is in use.
    void  CopyScene(ID3D12GraphicsCommandList* Cmd, const FRtPass& Rt, const FShadowMaps& Shadow);
    // The frame's bodies into its list, as FRtPass listed them: each where its corners begin in the buffer it lies in.
    void  WriteBodies(Host64::FRayFrame* Frame, const FRtPass& Rt, const FShadowMaps& Shadow, INT Bodies);

    TComRef<ID3D12Resource> Shared[Host64::Ray_Count];
    // What the share was made for: Prepare makes another when any of it no longer holds.
    INT                     StaticSerial;
    INT                     StaticVertices[3];
    UINT64                  FrameBytes;        // the frame's bodies' two buffers hold this much
    UINT64                  ArenaBytes;        // ... and the arena's two
    INT                     BodyRoom;
    UINT64                  DepthW;
    UINT                    DepthH;
    DXGI_FORMAT             DepthFormat, LightFormat;
    INT                     FactorW, FactorH;
    INT                     AskW, AskH;        // the asking picture's size; 0: the share has no list
    UBOOL                   bStaticCopied;     // the level's triangles have gone into this share
    INT                     ShareSerial;       // counts the shares made - see GetShareSerial
    INT                     FrameSerial;       // counts the frames (Tick); the one whose scene was last copied over, and
    INT                     SceneCopiedAt;     // the one a hand-over last had the host make its scene in
    INT                     SceneHandedAt;
    // The list. The pixels' heads: a texture of ours, the asking picture's size, emptied through its render target
    // view and written by the asking pixel shaders; how many slots the requests' and the answers' buffers hold;
    // and the state the heads, the requests and the answers are in on our side.
    TComRef<ID3D12Resource> Heads;
    TComRef<ID3D12DescriptorHeap> HeadsRtvHeap;
    INT                     ListRoom;
    D3D12_RESOURCE_STATES   HeadsState, AskedState, AnswersState;
    // The list pass's constants and its table of the bodies' first vertices: an upload buffer of ours, a frame slot's
    // worth each (ListSlotBytes), copied into the shared tables with the rest; and where that table lies there.
    // After the frame slots' own, the requests' slot 0 as a frame starts with it (ListStartAt).
    TComRef<ID3D12Resource> ListUpload;
    BYTE*                   ListMapped;
    // The requests' slot 0 as each hand-over of a frame slot's last frame left it, read back (NoteListTaken); how
    // many of a frame slot's are there to read, and which the hand-over being recorded is (-1: none); and how many
    // slots the list should hold by what the frames asked.
    TComRef<ID3D12Resource> ListBack;
    INT                     ListBackCount[BACKEND_FRAME_COUNT];
    INT                     ListBackAt;
    INT                     ListWanted;
    FListCounts             Counts;
    // What the host traces on by, and what it writes of it: how many bytes the shared buffers of the mirror rays'
    // table and of the level's surfaces hold, the surfaces' number as last copied (FRtPass::GetMaterialSerial), and
    // the passed surfaces' buffer - its slots, the state it is in on our side, how many slots it should hold by
    // what the frames' rays passed, which of a frame slot's hand-overs have its count to read (a bit each) - and
    // whether the last frame fed had the host trace on, for the log.
    UINT64                  WaterRoom, MaterialRoom;
    INT                     MaterialCopied;
    INT                     PassedRoom, PassedWanted;
    D3D12_RESOURCE_STATES   PassedState;
    UINT                    PassedBackMask[BACKEND_FRAME_COUNT];
    INT                     TracedOn;
    UINT                    ListSlotBytes;
    UINT                    ListStartAt;
    UINT                    AtListGeo;
    // A share the driver refused to make: not asked for again every frame, only when what it is made for changes.
    UBOOL                   bRefused;
    INT                     RefusedSerial, RefusedW, RefusedH;
    // An older share's resources - its own three among them - and the frames each has waited.
    TComRef<IUnknown>       Old[OLD_ROOM];
    INT                     OldAge[OLD_ROOM];
    INT                     Fed;               // frames fed, for the log's occasional line
};
