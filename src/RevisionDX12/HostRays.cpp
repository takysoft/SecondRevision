// SecondRevision - the rays through the 64-bit host, the renderer's side. See HostRays.h.
#include "Pch.h"
#include "Log.h"
#include "Backend.h"
#include "HostRays.h"

// How long the host may take to take a share: it opens the resources and maps the lists - no library is made for it.
static const DWORD GRayShareMs = 20000;

// Where the passes' constants and tables lie in the shared tables' buffer: the factor's constants first (the shadow
// maps' lookup constants, whole), then the rays' own (FRtPass's exclusion constants), then the list pass's, then the
// shadows' table of the geometries - and after that, the list pass's own (AtListGeo). Constants begin on a multiple
// of 256 bytes.
static const UINT GAtFactor = 0;
static const UINT GAtRays   = (UINT)FShadowMaps::LOOKUP_CONSTANT_BYTES;
static const UINT GAtList   = GAtRays + 256;
static const UINT GAtGeo    = GAtList + 256;

// A frame in flight may still copy into what a share before this one held: kept this many frames after it.
static const INT GOldFrames = BACKEND_FRAME_COUNT + 2;

// The list's length: half a slot for every pixel of the asking picture, and never fewer than this many. A surface
// that fills the picture with one ray a pixel asks twice that - the frame then runs its fallbacks, and says so.
static const INT GListLeast = 1 << 16;
// ... and never more than four slots for every pixel of the asking picture, however much the frames ask.
static const INT GListMostAPixel = 4;
// The see-through surfaces the rays pass: an eighth of a slot for every pixel at first, never fewer than this many,
// and never more than two slots a pixel.
static const INT GPassedLeast = 1 << 14;
static const INT GPassedMostAPixel = 2;

static D3D12_RESOURCE_DESC RayBufferDesc(UINT64 Bytes)
{
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    // A whole number of the 64 KB a buffer is made of, and never nothing.
    Desc.Width            = (Max<UINT64>(Bytes, 1) + 65535) & ~(UINT64)65535;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return Desc;
}

// A buffer a shader writes: the requests' on this side, the answers' on the host's.
static D3D12_RESOURCE_DESC RayListDesc(UINT64 Bytes)
{
    D3D12_RESOURCE_DESC Desc = RayBufferDesc(Bytes);
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return Desc;
}

static D3D12_RESOURCE_DESC RayTargetDesc(INT W, INT H, DXGI_FORMAT Format)
{
    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = (UINT64)W;
    Desc.Height           = (UINT)H;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = Format;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return Desc;
}

static void RayBarrier(D3D12_RESOURCE_BARRIER& B, ID3D12Resource* Res, D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To)
{
    appMemzero(&B, sizeof(B));
    B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    B.Transition.pResource   = Res;
    B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    B.Transition.StateBefore = From;
    B.Transition.StateAfter  = To;
}

FHostRayFeed::FHostRayFeed()
{
    StaticSerial = -1;
    for (INT k = 0; k < 3; k++)
        StaticVertices[k] = 0;
    FrameBytes    = ArenaBytes = 0;
    BodyRoom      = 0;
    DepthW        = 0;
    DepthH        = 0;
    DepthFormat   = LightFormat = DXGI_FORMAT_UNKNOWN;
    FactorW       = FactorH = 0;
    AskW          = AskH = 0;
    bStaticCopied = 0;
    ArenaCopied   = 0;
    ArenaCopiedGen = -1;
    ShareSerial   = 0;
    FrameSerial   = 0;
    SceneCopiedAt = -1;
    SceneHandedAt = -1;
    ListRoom      = 0;
    ListWanted    = 0;
    WaterRoom     = MaterialRoom = 0;
    MaterialCopied = -1;
    PassedRoom    = PassedWanted = 0;
    TracedOn      = -1;
    ListBackAt    = -1;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        ListBackCount[f]  = 0;
        PassedBackMask[f] = 0;
    }
    appMemzero(&Counts, sizeof(Counts));
    HeadsState    = AskedState = AnswersState = PassedState = D3D12_RESOURCE_STATE_COMMON;
    ListMapped    = nullptr;
    ListSlotBytes = 0;
    ListStartAt   = 0;
    AtListGeo     = 0;
    bRefused      = 0;
    RefusedSerial = RefusedW = RefusedH = -1;
    for (INT i = 0; i < OLD_ROOM; i++)
        OldAge[i] = 0;
    Fed = 0;
}

FHostRayFeed::~FHostRayFeed()
{
    Release();
}

void FHostRayFeed::Release()
{
    for (INT r = 0; r < Host64::Ray_Count; r++)
        Shared[r].Release();
    for (INT i = 0; i < OLD_ROOM; i++)
        Old[i].Release();
    if (ListUpload && ListMapped)
        ListUpload->Unmap(0, nullptr);
    ListUpload.Release();
    ListMapped = nullptr;
    HeadsRtvHeap.Release();
    Heads.Release();
    Low.Release();
    LowState    = D3D12_RESOURCE_STATE_COMMON;
    bLowRefused = 0;
    ListBack.Release();
    ListBackAt = -1;
    for (INT f = 0; f < BACKEND_FRAME_COUNT; f++)
    {
        ListBackCount[f]  = 0;
        PassedBackMask[f] = 0;
        LowBackMask[f]    = 0;
    }
    StaticSerial  = -1;
    FrameBytes    = ArenaBytes = 0;
    BodyRoom      = 0;
    AskW          = AskH = 0;
    ListRoom      = 0;
    WaterRoom     = MaterialRoom = 0;
    PassedRoom    = 0;
    bStaticCopied = 0;
    ArenaCopied   = 0;
    ArenaCopiedGen = -1;
    HeadsState    = AskedState = AnswersState = PassedState = D3D12_RESOURCE_STATE_COMMON;
    bRefused      = 0;
}

void FHostRayFeed::Abandon()
{
    // Where the older shares leave no room, these stay where they are, and the next Prepare - which finds that they no
    // longer fit - moves them on when it can.
    Retire();
    StaticSerial  = -1;
    bStaticCopied = 0;
    ArenaCopied   = 0;
    ArenaCopiedGen = -1;
    bRefused      = 0;
}

void FHostRayFeed::Tick()
{
    FrameSerial++;
    for (INT i = 0; i < OLD_ROOM; i++)
        if (Old[i] && ++OldAge[i] > GOldFrames)
            Old[i].Release();
}

// The share's resources into the old ones' places - the shared ones, and this process's own that go with a share:
// the frames in flight draw with the pixels' heads and RTLow's own list, and copy out of the list's upload buffer as
// they copy into what is shared, and none of it may be let go under them. False: no room there - older shares still
// wait - and nothing is moved.
UBOOL FHostRayFeed::Retire()
{
    INT Held = (Heads ? 1 : 0) + (HeadsRtvHeap ? 1 : 0) + (ListUpload ? 1 : 0) + (Low ? 1 : 0), Free = 0;
    for (INT r = 0; r < Host64::Ray_Count; r++)
        Held += Shared[r] ? 1 : 0;
    for (INT i = 0; i < OLD_ROOM; i++)
        Free += Old[i] ? 0 : 1;
    if (Held > Free)
        return 0;
    INT To = 0;
    const auto Keep = [this, &To](IUnknown* What) {
        while (Old[To])
            To++;
        What->AddRef();
        *Old[To].Slot() = What;
        OldAge[To]      = 0;
    };
    for (INT r = 0; r < Host64::Ray_Count; r++)
    {
        if (!Shared[r])
            continue;
        Keep(Shared[r].Get());
        Shared[r].Release();
    }
    // Written no more from here: what a frame in flight copies out of the upload buffer is in it already.
    if (ListUpload && ListMapped)
        ListUpload->Unmap(0, nullptr);
    ListMapped = nullptr;
    if (Heads)
    {
        Keep(Heads.Get());
        Heads.Release();
    }
    if (HeadsRtvHeap)
    {
        Keep(HeadsRtvHeap.Get());
        HeadsRtvHeap.Release();
    }
    if (ListUpload)
    {
        Keep(ListUpload.Get());
        ListUpload.Release();
    }
    if (Low)
    {
        Keep(Low.Get());
        Low.Release();
    }
    ListRoom    = 0;
    PassedRoom  = 0;
    HeadsState  = AskedState = AnswersState = PassedState = LowState = D3D12_RESOURCE_STATE_COMMON;
    bLowRefused = 0;
    return 1;
}

UBOOL FHostRayFeed::EnsureLow(ID3D12Device* Device)
{
    if (Low)
        return 1;
    if (!Device || !HasList() || bLowRefused)
        return 0;
    D3D12_HEAP_PROPERTIES Own;
    appMemzero(&Own, sizeof(Own));
    Own.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC Desc = RayListDesc((UINT64)ListRoom * RDX_RAY_SLOT_BYTES);
    if (FAILED(Device->CreateCommittedResource(&Own, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               __uuidof(ID3D12Resource), Low.VoidSlot())))
    {
        Low.Release();
        bLowRefused = 1;
        RdxLog(TEXT("rays (RTLow): the card refused the own list (%.0f MB) - every ray asked is traced, as at RT"),
               (DOUBLE)Desc.Width / (1024.0 * 1024.0));
        return 0;
    }
    LowState = D3D12_RESOURCE_STATE_COMMON;
    RdxLog(TEXT("rays (RTLow): the own list made - %i slots, %.0f MB"), ListRoom, (DOUBLE)Desc.Width / (1024.0 * 1024.0));
    return 1;
}

UBOOL FHostRayFeed::Prepare(ID3D12Device* Device, FHost64Link& Link, const FRtPass& Rt, const FShadowMaps& Shadow,
                            const FViews& Views, INT Bodies)
{
    if (!Device || !Link.IsUp() || !Link.RayPasses() || !Views.WorldDepth || !Views.OnTopDepth || !Views.Light ||
        Views.FactorW <= 0 || Views.FactorH <= 0 || Bodies < 0)
        return 0;
    const D3D12_RESOURCE_DESC Depth = Views.WorldDepth->GetDesc();
    const UINT64 FrameNeed = (UINT64)Shadow.GetFrameVertices() * 12;
    const UBOOL  bFits = Shared[Host64::Ray_Tables] && StaticSerial == Rt.GetStaticSerial() && FrameNeed <= FrameBytes &&
                         Bodies <= BodyRoom && Depth.Width == DepthW && Depth.Height == DepthH &&
                         Depth.Format == DepthFormat && Views.Light->GetDesc().Format == LightFormat &&
                         Views.FactorW == FactorW && Views.FactorH == FactorH && Views.AskW == AskW &&
                         Views.AskH == AskH &&
                         (AskW <= 0 || (ListWanted <= ListRoom && PassedWanted <= PassedRoom &&
                                        Rt.GetWaterTableBytes(Bodies) <= WaterRoom &&
                                        Rt.GetMaterialBytes() <= MaterialRoom));
    if (bFits && Link.RaysShared())
        return 1;
    // While a share is still being taken, that one finishes first.
    if (Link.SharingRays())
        return 0;
    // One the host answered and could not take is not handed over again as it is.
    if (bFits && Link.RaysRefused() && !bRefused)
    {
        RdxLog(TEXT("rays (host): the host could not take the share (its log says why) - the RT settings run their ")
               TEXT("fallbacks until the level or the sizes change"));
        bRefused      = 1;
        RefusedSerial = Rt.GetStaticSerial();
        RefusedW      = Views.FactorW;
        RefusedH      = Views.FactorH;
    }
    if (bRefused && RefusedSerial == Rt.GetStaticSerial() && RefusedW == Views.FactorW && RefusedH == Views.FactorH)
        return 0;
    // What is left: nothing shared yet, something no longer fits, or a host started again that has never had ours.
    if (!Retire())
        return 0;
    if (!MakeShare(Device, Link, Rt, Shadow, Views, Bodies))
    {
        for (INT r = 0; r < Host64::Ray_Count; r++)
            Shared[r].Release();
        StaticSerial  = -1;
        bRefused      = 1;
        RefusedSerial = Rt.GetStaticSerial();
        RefusedW      = Views.FactorW;
        RefusedH      = Views.FactorH;
    }
    // Handed over now, taken some frames later.
    return 0;
}

UBOOL FHostRayFeed::MakeShare(ID3D12Device* Device, FHost64Link& Link, const FRtPass& Rt, const FShadowMaps& Shadow,
                              const FViews& Views, INT Bodies)
{
    const D3D12_RESOURCE_STATES Common = D3D12_RESOURCE_STATE_COMMON;
    HRESULT Hr = S_OK;
    INT     Failed = -1;
    const auto Make = [&](INT Which, const D3D12_RESOURCE_DESC& Desc) {
        if (FAILED(Hr))
            return;
        Hr = FHost64Link::MakeShared(Device, Desc, Common, nullptr, Shared[Which].Slot());
        if (FAILED(Hr))
            Failed = Which;
    };
    // The level's three, as long as their triangles; the bodies' with room to spare, so that a frame with a body more
    // does not ask for another share; the arena's as long as the arena itself.
    for (INT k = 0; k < 3; k++)
    {
        StaticVertices[k] = Rt.GetStaticBuffer(k) ? Rt.GetStaticVertices(k) : 0;
        if (StaticVertices[k] >= 3)
            Make(Host64::Ray_Level + k, RayBufferDesc((UINT64)StaticVertices[k] * 12));
    }
    const UINT64 FrameNeed = (UINT64)Shadow.GetFrameVertices() * 12;
    FrameBytes = RayBufferDesc(Max<UINT64>(FrameNeed + FrameNeed / 2, 1u << 20)).Width;
    Make(Host64::Ray_FrameXyz, RayBufferDesc(FrameBytes));
    Make(Host64::Ray_FrameUvw, RayBufferDesc(FrameBytes));
    ID3D12Resource* const Arena = Shadow.GetCasterBuffer(1, 0);
    ArenaBytes = (Arena && Shadow.GetCasterBuffer(1, 1)) ? Arena->GetDesc().Width : 0;
    if (ArenaBytes)
    {
        Make(Host64::Ray_ArenaXyz, RayBufferDesc(ArenaBytes));
        Make(Host64::Ray_ArenaUvw, RayBufferDesc(ArenaBytes));
    }
    // The tables: the three sets of constants, then the shadows' table - a uint a body and a row of bits a light
    // (FRtPass's) - and the list pass's own, a uint a body.
    BodyRoom = Max(Bodies + Bodies / 2, 256);
    const UINT64 GeoBytes = ((UINT64)BodyRoom + (UINT64)RdxMath::ShadowMaxLights * ShadowCasterWords(BodyRoom) + 4) * 4;
    AtListGeo = GAtGeo + (UINT)GeoBytes;
    Make(Host64::Ray_Tables, RayBufferDesc((UINT64)AtListGeo + (UINT64)BodyRoom * 4));
    // The masks, the two depth buffers and the light target: textures of the same shape as the renderer's own, which
    // are copied into them whole. A depth buffer stays one - its stencil plane comes with it.
    if (Shadow.GetMaskArray())
    {
        D3D12_RESOURCE_DESC Masks = Shadow.GetMaskArray()->GetDesc();
        Masks.Flags = D3D12_RESOURCE_FLAG_NONE;
        Make(Host64::Ray_Masks, Masks);
    }
    const D3D12_RESOURCE_DESC Depth = Views.WorldDepth->GetDesc();
    Make(Host64::Ray_WorldDepth, Depth);
    Make(Host64::Ray_OnTopDepth, Views.OnTopDepth->GetDesc());
    D3D12_RESOURCE_DESC Light = Views.Light->GetDesc();
    Light.Flags = D3D12_RESOURCE_FLAG_NONE;
    Make(Host64::Ray_SceneLight, Light);
    // What the host writes: the factor and the occlusion ...
    Make(Host64::Ray_Factor, RayTargetDesc(Views.FactorW, Views.FactorH, DXGI_FORMAT_R8_UNORM));
    Make(Host64::Ray_Occlusion, RayTargetDesc(Views.FactorW, Views.FactorH, DXGI_FORMAT_R8_UNORM));
    // ... the water surface's factor, and what it reads: the water's pack (FBackend::EnsureWaterFactor). The rest goes
    // without them: should they not be made, the water takes the shadow of what lies under it.
    if (SUCCEEDED(Hr) && Views.SurfaceW > 0 && Views.SurfaceH > 0 && Views.SurfacePackW > 0 && Views.SurfacePackH > 0)
    {
        D3D12_RESOURCE_DESC SurfacePack = RayTargetDesc(Views.SurfacePackW, Views.SurfacePackH, DXGI_FORMAT_R32G32_FLOAT);
        SurfacePack.Flags = D3D12_RESOURCE_FLAG_NONE;
        const D3D12_RESOURCE_DESC SurfaceFactor = RayTargetDesc(Views.SurfaceW, Views.SurfaceH, DXGI_FORMAT_R8_UNORM);
        if (FAILED(FHost64Link::MakeShared(Device, SurfacePack, Common, nullptr,
                                           Shared[Host64::Ray_SurfacePack].Slot())) ||
            FAILED(FHost64Link::MakeShared(Device, SurfaceFactor, Common, nullptr,
                                           Shared[Host64::Ray_SurfaceFactor].Slot())))
        {
            Shared[Host64::Ray_SurfacePack].Release();
            Shared[Host64::Ray_SurfaceFactor].Release();
            RdxLog(TEXT("rays (host): the water surface's factor's textures would not be shared - the see-through ")
                   TEXT("water takes the shadow of what lies under it"));
        }
    }
    // ... and, where the frame may ask rays of its own, their list: the requests' buffer, which this side's pixel
    // shaders write, and the answers', which the host's pass writes - the same length, a slot a ray. The pixels'
    // heads are this side's alone. The share before took its own with it (Retire); should any of them still be
    // here, no share is made over it - making one would let it go under the frames in flight.
    if (Heads || HeadsRtvHeap || ListUpload || Low)
        return 0;
    ListMapped = nullptr;
    ListRoom   = 0;
    WaterRoom  = MaterialRoom = 0;
    PassedRoom = 0;
    if (Views.AskW > 0 && Views.AskH > 0)
    {
        // As long as the frames have asked, half as much again - between half a slot and GListMostAPixel slots for
        // every pixel of the asking picture.
        const INT64 Pixels = (INT64)Views.AskW * Views.AskH;
        const INT64 Asked  = (INT64)ListWanted + ListWanted / 2;
        ListRoom   = (INT)Clamp<INT64>(Max<INT64>(Asked, Pixels / 2), GListLeast, Pixels * GListMostAPixel);
        ListWanted = Min(ListWanted, ListRoom);
        const UINT64 ListBytes = (UINT64)ListRoom * RDX_RAY_SLOT_BYTES;
        Make(Host64::Ray_Requests, RayListDesc(ListBytes));
        Make(Host64::Ray_Answers, RayListDesc(ListBytes));
        // What the host traces on by from what a ray met: the mirror rays' table - twice what the lists' room of
        // bodies and the level's lamps of now ask - and the level's surfaces, where they are on the card. And what
        // it writes of the see-through surfaces the rays pass: as many slots as the frames have had of them, half
        // as much again, within its own least and most.
        WaterRoom    = RayBufferDesc(Rt.GetWaterTableBytes(BodyRoom) * 2).Width;
        Make(Host64::Ray_Water, RayBufferDesc(WaterRoom));
        MaterialRoom = Rt.GetMaterialBytes() ? RayBufferDesc(Rt.GetMaterialBytes()).Width : 0;
        if (MaterialRoom)
            Make(Host64::Ray_Material, RayBufferDesc(MaterialRoom));
        const INT64 Passed = (INT64)PassedWanted + PassedWanted / 2;
        PassedRoom   = (INT)Clamp<INT64>(Max<INT64>(Passed, Pixels / 8), GPassedLeast,
                                         Min<INT64>(Pixels * GPassedMostAPixel, RDX_RAY_MORE_FIRST_MASK));
        PassedWanted = Min(PassedWanted, PassedRoom);
        Make(Host64::Ray_Passed, RayListDesc((UINT64)PassedRoom * RDX_RAY_SLOT_BYTES));
        D3D12_HEAP_PROPERTIES Own;
        appMemzero(&Own, sizeof(Own));
        Own.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC HeadsDesc = RayTargetDesc(Views.AskW, Views.AskH, DXGI_FORMAT_R32_UINT);
        HeadsDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_CLEAR_VALUE Empty;
        appMemzero(&Empty, sizeof(Empty));
        Empty.Format = DXGI_FORMAT_R32_UINT;
        D3D12_DESCRIPTOR_HEAP_DESC Hd;
        appMemzero(&Hd, sizeof(Hd));
        Hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Hd.NumDescriptors = 1;
        D3D12_HEAP_PROPERTIES Up;
        appMemzero(&Up, sizeof(Up));
        Up.Type = D3D12_HEAP_TYPE_UPLOAD;
        ListSlotBytes = (256u + (UINT)BodyRoom * 4u + 255u) & ~255u;
        ListStartAt   = ListSlotBytes * BACKEND_FRAME_COUNT;
        D3D12_RESOURCE_DESC UpDesc = RayBufferDesc((UINT64)ListStartAt + RDX_RAY_SLOT_BYTES * 2);
        if (SUCCEEDED(Hr))
            Hr = Device->CreateCommittedResource(&Own, D3D12_HEAP_FLAG_NONE, &HeadsDesc,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Empty,
                                                 __uuidof(ID3D12Resource), Heads.VoidSlot());
        if (SUCCEEDED(Hr))
            Hr = Device->CreateDescriptorHeap(&Hd, __uuidof(ID3D12DescriptorHeap), HeadsRtvHeap.VoidSlot());
        if (SUCCEEDED(Hr))
            Hr = Device->CreateCommittedResource(&Up, D3D12_HEAP_FLAG_NONE, &UpDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, __uuidof(ID3D12Resource), ListUpload.VoidSlot());
        if (SUCCEEDED(Hr))
            Hr = ListUpload->Map(0, nullptr, (void**)&ListMapped);
        // The counts' read-back, the requests' and the passed surfaces': two slots for each hand-over a frame
        // slot's frame may make. Without it the buffers stay as long as they were made.
        if (SUCCEEDED(Hr) && !ListBack)
        {
            D3D12_HEAP_PROPERTIES Back;
            appMemzero(&Back, sizeof(Back));
            Back.Type = D3D12_HEAP_TYPE_READBACK;
            const D3D12_RESOURCE_DESC BackDesc =
                RayBufferDesc((UINT64)RDX_RAY_SLOT_BYTES * 2 * LIST_BACK_MOST * BACKEND_FRAME_COUNT);
            if (FAILED(Device->CreateCommittedResource(&Back, D3D12_HEAP_FLAG_NONE, &BackDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       __uuidof(ID3D12Resource), ListBack.VoidSlot())))
                ListBack.Release();
        }
        if (SUCCEEDED(Hr))
        {
            Device->CreateRenderTargetView(Heads.Get(), nullptr, HeadsRtvHeap->GetCPUDescriptorHandleForHeapStart());
            // The requests' slot 0 as every frame starts with it: one slot taken - itself - and how many there are.
            // And the passed surfaces' after it, the same way.
            appMemzero(ListMapped + ListStartAt, RDX_RAY_SLOT_BYTES * 2);
            ((DWORD*)(ListMapped + ListStartAt))[0] = 1u;
            ((DWORD*)(ListMapped + ListStartAt))[1] = (DWORD)ListRoom;
            ((DWORD*)(ListMapped + ListStartAt + RDX_RAY_SLOT_BYTES))[0] = 1u;
            ((DWORD*)(ListMapped + ListStartAt + RDX_RAY_SLOT_BYTES))[1] = (DWORD)PassedRoom;
        }
        else if (Failed < 0)
            Failed = Host64::Ray_Requests;
    }
    if (FAILED(Hr))
    {
        RdxLog(TEXT("rays (host): shared resource %i could not be made (0x%08X) - the RT settings run their fallbacks until "
                    "the level or the sizes change"), Failed, (INT)Hr);
        return 0;
    }
    ID3D12Resource* Raw[Host64::Ray_Count];
    for (INT r = 0; r < Host64::Ray_Count; r++)
        Raw[r] = Shared[r].Get();
    if (!Link.BeginShareRays(Raw, (uint32_t)StaticVertices[0], (uint32_t)StaticVertices[1], (uint32_t)StaticVertices[2],
                             (uint32_t)BodyRoom, GRayShareMs))
    {
        RdxLog(TEXT("rays (host): the share could not be handed over - the RT settings run their fallbacks until the level "
                    "or the sizes change"));
        return 0;
    }
    StaticSerial  = Rt.GetStaticSerial();
    DepthW        = Depth.Width;
    DepthH        = Depth.Height;
    DepthFormat   = Depth.Format;
    LightFormat   = Light.Format;
    FactorW       = Views.FactorW;
    FactorH       = Views.FactorH;
    AskW          = Views.AskW;
    AskH          = Views.AskH;
    bStaticCopied = 0;
    ArenaCopied   = 0;
    ArenaCopiedGen = -1;
    SceneCopiedAt = -1;
    SceneHandedAt = -1;
    bRefused      = 0;
    HeadsState    = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    AskedState    = AnswersState = PassedState = Common;
    MaterialCopied = -1;
    ShareSerial++;
    RdxLog(TEXT("rays (host): a share handed over - the level %i triangles, the sky's walls %i, the see-through surfaces %i; ")
           TEXT("room for %i bodies and %i of their vertices a frame; the factor %ix%i, the rays' list %i slots for a ")
           TEXT("picture of %ix%i, %i slots for what the rays pass, the level's surfaces %.1f MB"),
           StaticVertices[0] / 3, StaticVertices[1] / 3, StaticVertices[2] / 3, BodyRoom, (INT)(FrameBytes / 12), FactorW,
           FactorH, ListRoom, AskW, AskH, PassedRoom, (DOUBLE)MaterialRoom / (1024.0 * 1024.0));
    return 1;
}

void FHostRayFeed::CopyScene(ID3D12GraphicsCommandList* Cmd, const FRtPass& Rt, const FShadowMaps& Shadow)
{
    // Once a frame: the frame's bodies are listed once (FRtPass::ListScene), and a hand-over later in the same frame -
    // the shadows' after the mirror rays' - traces against the same list, so against the same copies.
    if (SceneCopiedAt == FrameSerial)
        return;
    SceneCopiedAt = FrameSerial;
    if (!bStaticCopied)
    {
        for (INT k = 0; k < 3; k++)
            if (Shared[Host64::Ray_Level + k] && Rt.GetStaticBuffer(k))
                Cmd->CopyBufferRegion(Shared[Host64::Ray_Level + k].Get(), 0, Rt.GetStaticBuffer(k), 0,
                                      (UINT64)StaticVertices[k] * 12);
        bStaticCopied = 1;
    }
    // The frame's bodies all of them, every time: a body's texture places there may be written after its positions.
    // The arena only as far as it is new to this share: a shape goes into it once, its positions and its texture
    // places together where its room is handed out (FShadowMaps::ArenaAlloc), and nothing in it changes until the
    // arena is emptied - a new ArenaGen, and then all of it again.
    const UINT64 FrameUsed = Min<UINT64>((UINT64)Shadow.GetFrameVertices() * 12, FrameBytes);
    const UINT64 ArenaUsed = Min<UINT64>((UINT64)Shadow.GetArenaVertices() * 12, ArenaBytes);
    if (FrameUsed && Shadow.GetCasterBuffer(0, 0) && Shadow.GetCasterBuffer(0, 1))
    {
        Cmd->CopyBufferRegion(Shared[Host64::Ray_FrameXyz].Get(), 0, Shadow.GetCasterBuffer(0, 0), 0, FrameUsed);
        Cmd->CopyBufferRegion(Shared[Host64::Ray_FrameUvw].Get(), 0, Shadow.GetCasterBuffer(0, 1), 0, FrameUsed);
    }
    if (ArenaCopiedGen != Shadow.GetArenaGen() || ArenaCopied > ArenaUsed)
    {
        ArenaCopied    = 0;
        ArenaCopiedGen = Shadow.GetArenaGen();
    }
    if (ArenaUsed > ArenaCopied && Shared[Host64::Ray_ArenaXyz])
    {
        const UINT64 At = ArenaCopied, New = ArenaUsed - ArenaCopied;
        Cmd->CopyBufferRegion(Shared[Host64::Ray_ArenaXyz].Get(), At, Shadow.GetCasterBuffer(1, 0), At, New);
        Cmd->CopyBufferRegion(Shared[Host64::Ray_ArenaUvw].Get(), At, Shadow.GetCasterBuffer(1, 1), At, New);
        ArenaCopied = ArenaUsed;
    }
}

void FHostRayFeed::WriteBodies(Host64::FRayFrame* Frame, const FRtPass& Rt, const FShadowMaps& Shadow, INT Bodies)
{
    ID3D12Resource* const FrameXyz  = Shadow.GetCasterBuffer(0, 0);
    ID3D12Resource* const ArenaXyz  = Shadow.GetCasterBuffer(1, 0);
    const UINT64          FrameBase = FrameXyz ? FrameXyz->GetGPUVirtualAddress() : 0;
    const UINT64          ArenaBase = ArenaXyz ? ArenaXyz->GetGPUVirtualAddress() : 0;
    Host64::FRayBody* const Out = (Host64::FRayBody*)(Frame + 1);
    Frame->Bodies = (uint32_t)Bodies;
    for (INT g = 0; g < Bodies; g++)
    {
        const D3D12_RAYTRACING_GEOMETRY_DESC& Geo = Rt.GetBody(g);
        const UINT64 At     = Geo.Triangles.VertexBuffer.StartAddress;
        const UBOOL  bArena = (ArenaBase && At >= ArenaBase && At < ArenaBase + ArenaBytes) ? 1 : 0;
        Out[g].Offset   = (uint32_t)(At - (bArena ? ArenaBase : FrameBase));
        Out[g].Stride   = (uint32_t)Geo.Triangles.VertexBuffer.StrideInBytes;
        Out[g].Vertices = Geo.Triangles.VertexCount;
        Out[g].Flags    = (bArena ? (uint32_t)Host64::RayBody_Arena : 0u) |
                          ((Geo.Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) ? 0u : (uint32_t)Host64::RayBody_Asks);
    }
}

UBOOL FHostRayFeed::Feed(ID3D12GraphicsCommandList* Cmd, FHost64Link& Link, const FRtPass& Rt, FShadowMaps& Shadow,
                         const FViews& Views, INT FrameIndex, INT Bodies, UINT Passes)
{
    Host64::FRayFrame* const Frame = Link.RayFrame();
    if (!Cmd || !Frame || Bodies > BodyRoom)
        return 0;
    CopyScene(Cmd, Rt, Shadow);
    // The passes' constants and table.
    ID3D12Resource* const Tables   = Shared[Host64::Ray_Tables].Get();
    const UINT64          GeoBytes = Rt.GetShadowTableBytes(FrameIndex);
    if (Shadow.GetLookupBuffer(FrameIndex))
        Cmd->CopyBufferRegion(Tables, GAtFactor, Shadow.GetLookupBuffer(FrameIndex), 0, FShadowMaps::LOOKUP_CONSTANT_BYTES);
    if (Rt.GetExclusionBuffer())
        Cmd->CopyBufferRegion(Tables, GAtRays, Rt.GetExclusionBuffer(), Rt.GetExclusionOffset(FrameIndex),
                              Rt.GetExclusionBytes());
    if (GeoBytes && Rt.GetShadowTableBuffer(FrameIndex))
        Cmd->CopyBufferRegion(Tables, GAtGeo, Rt.GetShadowTableBuffer(FrameIndex), 0, GeoBytes);

    // The textures: ours readable by a copy, theirs written by one, and everything back after - theirs in COMMON, as
    // every shared texture is when it changes hands.
    // The water's pack with them, where the frame asks the water surface's factor.
    const UBOOL bSurface = (Passes & Host64::RayPass_Surface) != 0;
    if (bSurface && !(Views.SurfacePack && HasSurface()))
        return 0;
    ID3D12Resource* const Ours[4]   = { Views.WorldDepth, Views.OnTopDepth, Views.Light,
                                        bSurface ? Views.SurfacePack : nullptr };
    ID3D12Resource* const Theirs[5] = { Shared[Host64::Ray_WorldDepth].Get(), Shared[Host64::Ray_OnTopDepth].Get(),
                                        Shared[Host64::Ray_SceneLight].Get(),
                                        bSurface ? Shared[Host64::Ray_SurfacePack].Get() : nullptr,
                                        Shared[Host64::Ray_Masks].Get() };
    const D3D12_RESOURCE_STATES Was[4] = { Views.DepthState, Views.DepthState, Views.LightState, Views.SurfaceState };
    D3D12_RESOURCE_BARRIER B[9];
    INT N = 0;
    for (INT i = 0; i < 4; i++)
        if (Ours[i])
            RayBarrier(B[N++], Ours[i], Was[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (INT i = 0; i < 5; i++)
        if (Theirs[i])
            RayBarrier(B[N++], Theirs[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    Cmd->ResourceBarrier((UINT)N, B);
    for (INT i = 0; i < 4; i++)
        if (Ours[i])
            Cmd->CopyResource(Theirs[i], Ours[i]);
    if (Theirs[4])
        Shadow.CopyMasks(Cmd, Theirs[4]);
    for (INT i = 0; i < N; i++)
    {
        const D3D12_RESOURCE_STATES From = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = B[i].Transition.StateBefore;
        B[i].Transition.StateBefore = From;
    }
    Cmd->ResourceBarrier((UINT)N, B);

    // The frame's list: the passes, where their tables are, and the bodies - the scene made again only where no
    // hand-over of this frame has had the host make it already.
    Frame->Passes         = Passes;
    Frame->Width          = (uint32_t)FactorW;
    Frame->Height         = (uint32_t)FactorH;
    Frame->FactorConstsAt = GAtFactor;
    Frame->RayConstsAt    = GAtRays;
    Frame->GeoAt          = GAtGeo;
    Frame->ListConstsAt   = GAtList;
    Frame->Flags          = (SceneHandedAt == FrameSerial) ? (uint32_t)Host64::RayFrame_SameScene : 0u;
    Frame->ListRoom       = 0;
    Frame->WaterBytes     = 0;
    Frame->PassedRoom     = 0;
    WriteBodies(Frame, Rt, Shadow, Bodies);
    if (Fed++ % 600 == 0)
        RdxLog(TEXT("rays (host): frame fed - %i bodies, %i vertices of the frame's and %i of the arena's, passes 0x%X; the ")
               TEXT("host's GPU time for a frame's rays %.2f ms"), Bodies, Shadow.GetFrameVertices(),
               Shadow.GetArenaVertices(), Passes, (DOUBLE)Link.WorkRayUs() / 1000.0);
    return 1;
}

UBOOL FHostRayFeed::BeginAsk(ID3D12GraphicsCommandList* Cmd, UBOOL bLow)
{
    if (!Cmd || !HasList() || !HeadsRtvHeap || !ListMapped || (bLow && !Low))
        return 0;
    // The answers of the hand-over before, if this side still reads them: back as they change hands.
    ListReadable(Cmd, 0);
    ID3D12Resource* const Asked = GetRequests();
    D3D12_RESOURCE_BARRIER B[3];
    RayBarrier(B[0], Heads.Get(), HeadsState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    RayBarrier(B[1], Asked, AskedState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bLow)
        RayBarrier(B[2], Low.Get(), LowState, D3D12_RESOURCE_STATE_COPY_DEST);
    Cmd->ResourceBarrier(bLow ? 3 : 2, B);
    // No block at any pixel, and the count back at its start - the own list's too, which the pixels write into.
    static const FLOAT None[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Cmd->ClearRenderTargetView(HeadsRtvHeap->GetCPUDescriptorHandleForHeapStart(), None, 0, nullptr);
    Cmd->CopyBufferRegion(Asked, 0, ListUpload.Get(), ListStartAt, RDX_RAY_SLOT_BYTES);
    if (bLow)
        Cmd->CopyBufferRegion(Low.Get(), 0, ListUpload.Get(), ListStartAt, RDX_RAY_SLOT_BYTES);
    RayBarrier(B[0], Heads.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RayBarrier(B[1], Asked, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (bLow)
        RayBarrier(B[2], Low.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Cmd->ResourceBarrier(bLow ? 3 : 2, B);
    HeadsState = AskedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (bLow)
        LowState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return 1;
}

UBOOL FHostRayFeed::FeedList(ID3D12GraphicsCommandList* Cmd, FHost64Link& Link, const FRtPass& Rt, FShadowMaps& Shadow,
                             INT FrameIndex, INT Bodies, INT OwnBody, UBOOL bLow)
{
    if (!Cmd || !HasList())
        return 0;
    bLow = (bLow && Low) ? 1 : 0;
    // The requests as they change hands and the heads readable, whatever comes of the rest: BeginAsk took them out.
    // RTLow's own list stays this side's: the pass that read it is done, the build that reads the answers finds its
    // blocks in it - readable once the count is read back below.
    D3D12_RESOURCE_BARRIER B[2];
    RayBarrier(B[0], Heads.Get(), HeadsState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    RayBarrier(B[1], GetRequests(), AskedState, D3D12_RESOURCE_STATE_COMMON);
    Cmd->ResourceBarrier(2, B);
    HeadsState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    AskedState = D3D12_RESOURCE_STATE_COMMON;
    Host64::FRayFrame* const Frame = Link.RayFrame();
    ListBackAt = -1;
    if (!Frame || Bodies > BodyRoom || !ListMapped || FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT)
        return 0;
    // How many slots the hand-over took, for NoteListTaken a few frames on. Where the pixels wrote into the own list
    // (RTLow) that one's count - what the buffers have to hold - with the host's beside it, in its third uint: of the
    // requests asked, the ones the host traces.
    if (ListBack && ListBackCount[FrameIndex] < LIST_BACK_MOST)
    {
        ListBackAt = ListBackCount[FrameIndex]++;
        RayBarrier(B[0], GetRequests(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Cmd->ResourceBarrier(1, B);
        if (bLow)
        {
            RayBarrier(B[1], Low.Get(), LowState, D3D12_RESOURCE_STATE_COPY_DEST);
            Cmd->ResourceBarrier(1, &B[1]);
            Cmd->CopyBufferRegion(Low.Get(), 8, GetRequests(), 0, 4);
            RayBarrier(B[1], Low.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Cmd->ResourceBarrier(1, &B[1]);
            LowState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            LowBackMask[FrameIndex] |= 1u << ListBackAt;
        }
        else
            LowBackMask[FrameIndex] &= ~(1u << ListBackAt);
        Cmd->CopyBufferRegion(ListBack.Get(),
                              (UINT64)RDX_RAY_SLOT_BYTES * 2 * (LIST_BACK_MOST * FrameIndex + ListBackAt),
                              bLow ? Low.Get() : GetRequests(), 0, RDX_RAY_SLOT_BYTES);
        RayBarrier(B[0], GetRequests(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Cmd->ResourceBarrier(1, B);
    }
    if (bLow)
    {
        RayBarrier(B[1], Low.Get(), LowState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Cmd->ResourceBarrier(1, &B[1]);
        LowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    CopyScene(Cmd, Rt, Shadow);
    if (ID3D12Resource* const Masks = Shared[Host64::Ray_Masks].Get())
    {
        RayBarrier(B[0], Masks, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        Cmd->ResourceBarrier(1, B);
        Shadow.CopyMasks(Cmd, Masks);
        RayBarrier(B[0], Masks, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Cmd->ResourceBarrier(1, B);
    }
    // The passed surfaces' count back at its start, whatever comes of the rest: a count a frame left there would
    // be read as this frame's.
    ID3D12Resource* const Passed = GetPassed();
    if (Passed)
    {
        RayBarrier(B[0], Passed, PassedState, D3D12_RESOURCE_STATE_COPY_DEST);
        Cmd->ResourceBarrier(1, B);
        Cmd->CopyBufferRegion(Passed, 0, ListUpload.Get(), (UINT64)ListStartAt + RDX_RAY_SLOT_BYTES,
                              RDX_RAY_SLOT_BYTES);
        RayBarrier(B[0], Passed, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Cmd->ResourceBarrier(1, B);
        PassedState = D3D12_RESOURCE_STATE_COMMON;
    }
    // What the host traces on by from what a ray met: the frame's table of the mirror rays as PrepareWater filled
    // it, and the level's surfaces where any of them changed since they were last copied - the whole of them, a
    // few megabytes a level. Without either, or without the passed surfaces' buffer, the pass only answers what
    // the rays met, and the layers take the lamps' light with nothing in its way.
    ID3D12Resource* const Water    = Shared[Host64::Ray_Water].Get();
    ID3D12Resource* const Material = Shared[Host64::Ray_Material].Get();
    ID3D12Resource* const Table    = Rt.GetWaterTableBuffer(FrameIndex);
    const UINT64 WaterBytes    = Rt.GetWaterTableBytes(Bodies);
    const UINT64 MaterialBytes = Rt.GetMaterialBytes();
    UBOOL bOn = (Water && Material && Passed && Table && WaterBytes <= WaterRoom &&
                 WaterBytes <= Table->GetDesc().Width && MaterialBytes != 0 && MaterialBytes <= MaterialRoom) ? 1 : 0;
    if (bOn)
    {
        RayBarrier(B[0], Water, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        Cmd->ResourceBarrier(1, B);
        Cmd->CopyBufferRegion(Water, 0, Table, 0, WaterBytes);
        RayBarrier(B[0], Water, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Cmd->ResourceBarrier(1, B);
        if (MaterialCopied != Rt.GetMaterialSerial())
        {
            RayBarrier(B[0], Material, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            Cmd->ResourceBarrier(1, B);
            if (Rt.CopyMaterial(Cmd, Material))
                MaterialCopied = Rt.GetMaterialSerial();
            RayBarrier(B[0], Material, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Cmd->ResourceBarrier(1, B);
        }
        bOn = (MaterialCopied == Rt.GetMaterialSerial()) ? 1 : 0;
    }
    if (TracedOn != (INT)bOn)
    {
        TracedOn = (INT)bOn;
        if (bOn)
            RdxLog(TEXT("rays (host): the host traces on from what the mirror rays meet - the lamps' light there, ")
                   TEXT("and the see-through surfaces on the rays' way (the mirror rays' table %i bytes, the level's ")
                   TEXT("surfaces %.1f MB, %i slots for what the rays pass)"),
                   (INT)WaterBytes, (DOUBLE)MaterialBytes / (1024.0 * 1024.0), PassedRoom);
        else
            RdxLog(TEXT("rays (host): the host does NOT trace on from what the mirror rays meet (the mirror rays' ")
                   TEXT("table %i bytes of %i, the level's surfaces %i bytes of %i, a buffer for what they pass: %s) ")
                   TEXT("- ")
                   TEXT("the lamps' light there comes with nothing in its way, and nothing the rays pass is laid on"),
                   (INT)WaterBytes, (INT)WaterRoom, (INT)MaterialBytes, (INT)MaterialRoom,
                   Passed ? TEXT("yes") : TEXT("no"));
    }
    // The list pass's constants - how many slots it may read, which of the bodies is the player's own, for the
    // rays that pass it, how much of the mirror rays' table it has to trace on by (nought: it does not), and how
    // many slots it has for what the rays pass - and its table:
    // each body's first vertex among the texture places, bit 31 for the arena's, which is where the body's corners
    // begin in its buffer, in vertices.
    BYTE* const Slot = ListMapped + (SIZE_T)ListSlotBytes * FrameIndex;
    appMemzero(Slot, 256);
    ((DWORD*)Slot)[0] = (DWORD)ListRoom;
    ((DWORD*)Slot)[1] = (OwnBody >= 0) ? (DWORD)OwnBody : 0xFFFFFFFFu;
    ((DWORD*)Slot)[2] = bOn ? (DWORD)WaterBytes : 0u;
    ((DWORD*)Slot)[3] = bOn ? (DWORD)PassedRoom : 0u;
    Frame->Passes         = Host64::RayPass_List;
    Frame->Width          = (uint32_t)AskW;
    Frame->Height         = (uint32_t)AskH;
    Frame->FactorConstsAt = GAtFactor;
    Frame->RayConstsAt    = GAtRays;
    Frame->GeoAt          = AtListGeo;
    Frame->ListConstsAt   = GAtList;
    Frame->Flags          = (SceneHandedAt == FrameSerial) ? (uint32_t)Host64::RayFrame_SameScene : 0u;
    Frame->ListRoom       = (uint32_t)ListRoom;
    Frame->WaterBytes     = bOn ? (uint32_t)WaterBytes : 0u;
    Frame->PassedRoom     = bOn ? (uint32_t)PassedRoom : 0u;
    WriteBodies(Frame, Rt, Shadow, Bodies);
    const Host64::FRayBody* const Listed = (const Host64::FRayBody*)(Frame + 1);
    DWORD* const Geo = (DWORD*)(Slot + 256);
    for (INT g = 0; g < Bodies; g++)
        Geo[g] = ((Listed[g].Offset / Max<DWORD>(Listed[g].Stride, 1)) & 0x7FFFFFFFu) |
                 ((Listed[g].Flags & Host64::RayBody_Arena) ? 0x80000000u : 0u);
    ID3D12Resource* const Tables = Shared[Host64::Ray_Tables].Get();
    Cmd->CopyBufferRegion(Tables, GAtList, ListUpload.Get(), (UINT64)ListSlotBytes * FrameIndex, 256);
    if (Bodies > 0)
        Cmd->CopyBufferRegion(Tables, AtListGeo, ListUpload.Get(), (UINT64)ListSlotBytes * FrameIndex + 256,
                              (UINT64)Bodies * 4);
    return 1;
}

void FHostRayFeed::NotePassed(ID3D12GraphicsCommandList* Cmd, INT FrameIndex)
{
    ID3D12Resource* const Passed = GetPassed();
    if (!Cmd || !Passed || !ListBack || FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT || ListBackAt < 0)
        return;
    D3D12_RESOURCE_BARRIER B;
    RayBarrier(B, Passed, PassedState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Cmd->ResourceBarrier(1, &B);
    Cmd->CopyBufferRegion(ListBack.Get(),
                          (UINT64)RDX_RAY_SLOT_BYTES * (2 * (LIST_BACK_MOST * FrameIndex + ListBackAt) + 1),
                          Passed, 0, RDX_RAY_SLOT_BYTES);
    RayBarrier(B, Passed, D3D12_RESOURCE_STATE_COPY_SOURCE, PassedState);
    Cmd->ResourceBarrier(1, &B);
    PassedBackMask[FrameIndex] |= 1u << ListBackAt;
    ListBackAt = -1;
}

void FHostRayFeed::NoteListTaken(INT FrameIndex)
{
    if (!ListBack || FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT || ListBackCount[FrameIndex] <= 0)
        return;
    const INT  Handovers = ListBackCount[FrameIndex];
    const UINT PassedOf  = PassedBackMask[FrameIndex];
    const UINT LowOf     = LowBackMask[FrameIndex];
    ListBackCount[FrameIndex]  = 0;
    PassedBackMask[FrameIndex] = 0;
    LowBackMask[FrameIndex]    = 0;
    const SIZE_T      At   = (SIZE_T)RDX_RAY_SLOT_BYTES * 2 * LIST_BACK_MOST * FrameIndex;
    const D3D12_RANGE Read = { At, At + (SIZE_T)RDX_RAY_SLOT_BYTES * 2 * Handovers };
    const D3D12_RANGE None = { 0, 0 };
    BYTE* Back = nullptr;
    if (FAILED(ListBack->Map(0, &Read, (void**)&Back)) || !Back)
        return;
    // Each hand-over's two counts, a count being the first slot not taken (slot 0 is the count's own): the most
    // of them is what the buffers have to hold, and the frame's together go to the log. A count that is no count -
    // the buffer's first use, a hand-over that never asked - is none.
    DWORD Taken = 0, Past = 0;
    INT64 Asked = 0, Passed = 0, Traced = 0;
    for (INT k = 0; k < Handovers; k++)
    {
        const BYTE* const Two = Back + Read.Begin + (SIZE_T)RDX_RAY_SLOT_BYTES * 2 * k;
        const DWORD T = *(const DWORD*)Two;
        const DWORD P = (PassedOf & (1u << k)) ? *(const DWORD*)(Two + RDX_RAY_SLOT_BYTES) : 0u;
        // RTLow's: the host's count beside the own list's (FeedList).
        const DWORD H = (LowOf & (1u << k)) ? ((const DWORD*)Two)[2] : T;
        if (T >= 1 && T <= 0x40000000u)
        {
            Taken  = Max(Taken, T);
            Asked += (INT64)T - 1;
            Counts.Each[k] += (INT64)T - 1;
            if (H >= 1 && H <= T)
                Traced += (INT64)H - 1;
        }
        if (P >= 1 && P <= 0x40000000u)
        {
            Past    = Max(Past, P);
            Passed += (INT64)P - 1;
        }
    }
    ListBack->Unmap(0, &None);
    Counts.Frames++;
    Counts.Handovers += Handovers;
    Counts.Asked     += Asked;
    Counts.Passed    += Passed;
    Counts.Traced    += Traced;
    Counts.AskedMost  = Max(Counts.AskedMost, (INT)Min<INT64>(Asked, 0x7FFFFFFF));
    Counts.PassedMost = Max(Counts.PassedMost, (INT)Min<INT64>(Passed, 0x7FFFFFFF));
    const INT64 Pixels = (INT64)AskW * AskH;
    // A buffer as long as it may be stays as it is: asked for more, the frames go on with their fallbacks.
    if (Taken >= 1 && Taken <= 0x40000000u && (INT64)ListRoom < Pixels * GListMostAPixel && (INT)Taken > ListRoom &&
        (INT)Taken > ListWanted)
    {
        RdxLog(TEXT("rays (host): a frame asked %u slots of the rays' list and it holds %i - the surfaces on the ")
               TEXT("host's rays ran their fallbacks for it; a longer list is shared"), Taken, ListRoom);
        ListWanted = (INT)Taken;
    }
    if (Past >= 1 && Past <= 0x40000000u && (INT64)PassedRoom < Pixels * GPassedMostAPixel && (INT)Past > PassedRoom &&
        (INT)Past > PassedWanted)
    {
        RdxLog(TEXT("rays (host): a frame's rays passed %u see-through surfaces and their buffer holds %i - the ")
               TEXT("surfaces on the host's rays ran their fallbacks for it; a longer one is shared"), Past,
               PassedRoom);
        PassedWanted = (INT)Past;
    }
}

UBOOL FHostRayFeed::TakeListCounts(FListCounts& Out)
{
    Out = Counts;
    appMemzero(&Counts, sizeof(Counts));
    return Out.Frames > 0;
}

void FHostRayFeed::ListReadable(ID3D12GraphicsCommandList* Cmd, UBOOL bOn)
{
    const D3D12_RESOURCE_STATES Want = bOn ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
    if (!Cmd || !HasList())
        return;
    D3D12_RESOURCE_BARRIER B[3];
    INT N = 0;
    // The requests only from where they changed hands: while the pixel shaders write them they are nobody's to read.
    if (AskedState != Want && AskedState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        RayBarrier(B[N++], GetRequests(), AskedState, Want);
        AskedState = Want;
    }
    if (AnswersState != Want)
    {
        RayBarrier(B[N++], GetAnswers(), AnswersState, Want);
        AnswersState = Want;
    }
    if (GetPassed() && PassedState != Want)
    {
        RayBarrier(B[N++], GetPassed(), PassedState, Want);
        PassedState = Want;
    }
    if (N)
        Cmd->ResourceBarrier((UINT)N, B);
}

UBOOL FHostRayFeed::Handover(ID3D12GraphicsCommandList* Cmd, ID3D12CommandAllocator* Allocator, ID3D12CommandQueue* Queue,
                             FHost64Link& Link)
{
    // The host's answers of the hand-over before, if this side still reads them: back as they change hands.
    ListReadable(Cmd, 0);
    if (FAILED(Cmd->Close()))
    {
        RdxLog(TEXT("rays (host): the frame's part before the rays would not close - the frame is lost"));
        return 0;
    }
    ID3D12CommandList* Lists[1] = { Cmd };
    Queue->ExecuteCommandLists(1, Lists);
    // False only when the link went down since Prepare: the frame then goes on with whatever the host's textures hold.
    Host64::FFrame Frame;
    appMemzero(&Frame, sizeof(Frame));
    Frame.Services = Host64::Service_Rays;
    if (Link.Submit(Queue, Frame))
        SceneHandedAt = FrameSerial;
    if (FAILED(Cmd->Reset(Allocator, nullptr)))
    {
        RdxLog(TEXT("rays (host): the frame's part after the rays would not open - the frame is lost"));
        return 0;
    }
    return 1;
}

UBOOL FHostRayFeed::CopyOut(ID3D12GraphicsCommandList* Cmd, INT Which, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState)
{
    ID3D12Resource* const From = Shared[Which].Get();
    if (!Cmd || !From || !To)
        return 0;
    const D3D12_RESOURCE_DESC A = From->GetDesc(), Z = To->GetDesc();
    if (A.Width != Z.Width || A.Height != Z.Height || A.Format != Z.Format || Z.MipLevels != 1)
    {
        static INT Told = 0;
        if (Told++ < 3)
            RdxLog(TEXT("rays (host): what the host wrote (%ix%i, format %i) is not the shape of the target it goes into ")
                   TEXT("(%ix%i, format %i) - not copied"), (INT)A.Width, (INT)A.Height, (INT)A.Format, (INT)Z.Width,
                   (INT)Z.Height, (INT)Z.Format);
        return 0;
    }
    D3D12_RESOURCE_BARRIER B[2];
    RayBarrier(B[0], From, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    RayBarrier(B[1], To, ToState, D3D12_RESOURCE_STATE_COPY_DEST);
    Cmd->ResourceBarrier(2, B);
    Cmd->CopyResource(To, From);
    RayBarrier(B[0], From, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    RayBarrier(B[1], To, D3D12_RESOURCE_STATE_COPY_DEST, ToState);
    Cmd->ResourceBarrier(2, B);
    return 1;
}

UBOOL FHostRayFeed::CopyFactor(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState)
{
    return CopyOut(Cmd, Host64::Ray_Factor, To, ToState);
}

UBOOL FHostRayFeed::CopyOcclusion(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState)
{
    return CopyOut(Cmd, Host64::Ray_Occlusion, To, ToState);
}

UBOOL FHostRayFeed::CopySurfaceFactor(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* To, D3D12_RESOURCE_STATES ToState)
{
    return CopyOut(Cmd, Host64::Ray_SurfaceFactor, To, ToState);
}
