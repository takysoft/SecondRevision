// SecondRevision - the temporal upscale through the 64-bit host. See HostUpscale.h.
#include "Pch.h"
#include "Log.h"
#include "HostUpscale.h"

// How long the host may take to answer at its start (it loads the FSR DLLs, some 65 MB), to make its upscaler for new
// textures (the first time on a machine the driver compiles its shaders - the built-in upscaler draws meanwhile, so this
// can be generous), and to finish a frame our queue waits on before it counts as hung.
static const DWORD GHostStartMs = 20000;
static const DWORD GHostShareMs = 60000;
static const DWORD GHostHangMs  = 3000;

// How long a host frame may take before the host counts as hung: GHostHangMs - or, as a measurement, two minutes with
// the marker SecondRevision-host-patient.txt (written in the last ten minutes).
static const DWORD GHostPatientMs = 120000;
static DWORD HostHangMs()
{
    static DWORD Hang = 0;
    if (Hang)
        return Hang;
    Hang = GHostHangMs;
    WCHAR Path[MAX_PATH];
    const DWORD N = ::GetTempPathW(MAX_PATH, Path);
    WIN32_FILE_ATTRIBUTE_DATA A;
    if (N > 0 && N < MAX_PATH - 40 && wcscat_s(Path, L"SecondRevision-host-patient.txt") == 0 &&
        ::GetFileAttributesExW(Path, GetFileExInfoStandard, &A))
    {
        FILETIME Now;
        ::GetSystemTimeAsFileTime(&Now);
        const ULONGLONG T = ((ULONGLONG)A.ftLastWriteTime.dwHighDateTime << 32) | A.ftLastWriteTime.dwLowDateTime;
        const ULONGLONG C = ((ULONGLONG)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
        if (C >= T && C - T < 10ULL * 60ULL * 10000000ULL)
        {
            Hang = GHostPatientMs;
            RdxLog(TEXT("host: a frame may take two minutes before the host counts as hung (the marker "
                        "SecondRevision-host-patient.txt)"));
        }
    }
    return Hang;
}

// The formats of the textures the host reads besides the colour: the motion target's and the splashes' reactive mask's
// (FFinalPass::InitMotion, FBackend::CreateSplashTargets).
static const DXGI_FORMAT GMotionFormat   = DXGI_FORMAT_R16G16_FLOAT;
static const DXGI_FORMAT GReactiveFormat = DXGI_FORMAT_R8_UNORM;

// The link's lines, into our log.
static void HostLog(const char* Line)
{
    TCHAR Wide[600];
    INT   k = 0;
    for (; Line[k] && k < 599; k++)
        Wide[k] = (TCHAR)(BYTE)Line[k];
    Wide[k] = 0;
    RdxLog(TEXT("%s"), Wide);
}

// A format the host's upscaler can read the colour through. Storage that is typeless here is read as the plain numbers,
// as FSR 3.1 in the renderer reads it (its backend's typeless mapping): the eight-bit scene is sRGB-encoded bytes to
// both.
static DXGI_FORMAT ReadableColour(DXGI_FORMAT F)
{
    switch (F)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                                return F;
    }
}

// Whether the depth plane of a buffer of this format copies into R32_FLOAT: a 32-bit float depth, with or without a
// stencil beside it.
static UBOOL DepthCopies(DXGI_FORMAT F)
{
    return F == DXGI_FORMAT_R32G8X24_TYPELESS || F == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || F == DXGI_FORMAT_R32_TYPELESS ||
           F == DXGI_FORMAT_D32_FLOAT || F == DXGI_FORMAT_R32_FLOAT;
}

FHostUpscale::FHostUpscale()
    : StartDevice(nullptr), bStartDebugLayer(0), StartBackend(0), Restarts(0), bOn(0), bFresh(0), RtvStep(0), RenderW(0), RenderH(0), OutW(0), OutH(0), bFrameGenAsked(0),
      BackFormat(DXGI_FORMAT_UNKNOWN), DisplayW(0), DisplayH(0), FailedW(0), FailedH(0),
      SceneFormat(DXGI_FORMAT_UNKNOWN), OutputFormat(DXGI_FORMAT_UNKNOWN)
{
    Said[0] = 0;
    Link.SetLog(HostLog);
}

FHostUpscale::~FHostUpscale()
{
    Stop();
}

UBOOL FHostUpscale::IsStarting() const  { return bOn && Link.State() == FHost64Link::State_Starting; }
UBOOL FHostUpscale::IsUp() const        { return bOn && Link.State() == FHost64Link::State_Up; }
UBOOL FHostUpscale::IsDown() const      { return bOn && Link.State() == FHost64Link::State_Down; }
UBOOL FHostUpscale::IsPreparing() const { return bOn && Link.Sharing(); }
UINT  FHostUpscale::WorkUs() const      { return bOn ? (UINT)Link.WorkUs() : 0; }

void FHostUpscale::Start(ID3D12Device* Device, UBOOL bDebugLayer, UINT Backend)
{
    StartDevice      = Device;
    bStartDebugLayer = bDebugLayer;
    StartBackend     = Backend;
    Restarts         = 0;
    Launch();
}

void FHostUpscale::Launch()
{
    ID3D12Device* const Device      = StartDevice;
    const UBOOL         bDebugLayer = bStartDebugLayer;
    const UINT          Backend     = StartBackend;
    Stop();
    // Our own DLL's folder, and the host's folder in it.
    WCHAR   Path[MAX_PATH + 64];
    HMODULE Self = nullptr;
    DWORD   N    = 0;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)&GHostStartMs, &Self) && Self)
        N = ::GetModuleFileNameW(Self, Path, MAX_PATH);
    INT Cut = -1;
    for (INT i = 0; i < (INT)N && Path[i]; i++)
        if (Path[i] == L'\\' || Path[i] == L'/')
            Cut = i;
    if (N == 0 || N >= MAX_PATH || Cut < 0)
        Path[0] = 0;
    else
    {
        const WCHAR* const Tail = L"SecondRevisionHost64\\SecondRevisionHost64.exe";
        INT k = Cut + 1;
        for (INT t = 0; Tail[t] && k < MAX_PATH + 63; t++)
            Path[k++] = Tail[t];
        Path[k] = 0;
    }
    bOn = 1;
    Link.SetDebugLayer(bDebugLayer != 0);
    RdxLog(TEXT("host64: starting %s for %s%s"), Path[0] ? (const TCHAR*)Path : TEXT("(no path)"),
           (Backend == Host64::Backend_Dlss) ? TEXT("DLSS") : (Backend == Host64::Backend_Xess) ? TEXT("XeSS") : TEXT("FSR"),
           bDebugLayer ? TEXT(" with the D3D12 debug layer") : TEXT(""));
    // False: it is down already, and Poll carries the reason into Message.
    Link.Start(Device, Path, Backend, 0);
    Poll();
}

void FHostUpscale::Stop()
{
    if (bOn)
    {
        Link.Stop();
        RdxLog(TEXT("host64: stopped"));
    }
    bOn = 0;
    ReleaseTextures();
    Said[0] = 0;
}

void FHostUpscale::Poll()
{
    if (!bOn)
        return;
    const FHost64Link::EState Was = Link.State();
    Link.Poll(GHostStartMs, HostHangMs());
    // Died on its way up, before it could say anything: started once more, twice at most. A tool that hooks D3D12 calls
    // from inside a process can bring it down in its first calls, a hook not finished yet - and the game would go on
    // with the built-in FSR 3.1 without a word. A failure the host reported, or one that took too long, is not tried
    // again.
    if (Was == FHost64Link::State_Starting && Link.State() == FHost64Link::State_Down && Link.EndedWhileStarting() &&
        Restarts < 2)
    {
        Restarts++;
        RdxLog(TEXT("host64: it ended on its way up - starting it again (%i of 2)"), Restarts);
        Launch();
        return;
    }
    const char* const Now = Link.Message();
    if (strcmp(Now, Said) != 0)
    {
        strncpy_s(Said, sizeof(Said), Now, _TRUNCATE);
        // The link logs its own going up and down; what the host says while it is up - what runs - is logged here.
        if (Link.State() == FHost64Link::State_Up && Was == FHost64Link::State_Up && Said[0])
        {
            TCHAR Wide[Host64::MessageChars];
            INT   k = 0;
            for (; Said[k] && k < Host64::MessageChars - 1; k++)
                Wide[k] = (TCHAR)(BYTE)Said[k];
            Wide[k] = 0;
            RdxLog(TEXT("host64 says: %s"), Wide);
        }
    }
}

void FHostUpscale::ReleaseTextures()
{
    for (INT r = 0; r < Host64::Resource_Count; r++)
        Textures[r].Release();
    RenderW = RenderH = OutW = OutH = 0;
    SceneFormat    = DXGI_FORMAT_UNKNOWN;
    OutputFormat   = DXGI_FORMAT_UNKNOWN;
    bFrameGenAsked = 0;
    BackFormat     = DXGI_FORMAT_UNKNOWN;
    DisplayW = DisplayH = 0;
}

UBOOL FHostUpscale::Prepare(ID3D12Device* Device, INT RW, INT RH, INT OW, INT OH, DXGI_FORMAT SceneFmt,
                            DXGI_FORMAT OutputFmt, UBOOL bFrameGen, DXGI_FORMAT BackFmt, INT DW, INT DH)
{
    if (!IsUp() || !Device || RW <= 0 || RH <= 0 || OW <= 0 || OH <= 0)
        return 0;
    const UBOOL bFg = (bFrameGen && BackFmt != DXGI_FORMAT_UNKNOWN && DW > 0 && DH > 0) ? 1 : 0;
    // These, handed over already: the host's once it has taken them, the built-in upscaler's until then.
    if (Textures[Host64::Resource_Color] && RW == RenderW && RH == RenderH && OW == OutW && OH == OutH &&
        SceneFmt == SceneFormat && OutputFmt == OutputFormat && bFg == bFrameGenAsked &&
        (!bFg || (BackFmt == BackFormat && DW == DisplayW && DH == DisplayH)))
        return Link.Shared() ? 1 : 0;
    // Other ones, while a share is still being taken (a resize came in the middle): that one finishes first - the block
    // holds its textures' handles until the host has read them.
    if (Link.Sharing())
        return 0;
    // A refused create stays refused at the same sizes; asking again every frame only costs the asking.
    if (RW == FailedW && RH == FailedH)
        return 0;
    ReleaseTextures();

    struct FMake
    {
        Host64::EResource    Which;
        DXGI_FORMAT          Format;
        INT                  W, H;
        D3D12_RESOURCE_FLAGS Flags;
    };
    const FMake Make[5] = {
        { Host64::Resource_Color,    ReadableColour(SceneFmt), RW, RH, D3D12_RESOURCE_FLAG_NONE },
        { Host64::Resource_Depth,    DXGI_FORMAT_R32_FLOAT,    RW, RH, D3D12_RESOURCE_FLAG_NONE },
        { Host64::Resource_Motion,   GMotionFormat,            RW, RH, D3D12_RESOURCE_FLAG_NONE },
        { Host64::Resource_Reactive, GReactiveFormat,          RW, RH, D3D12_RESOURCE_FLAG_NONE },
        { Host64::Resource_Output,   OutputFmt,                OW, OH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS },
    };
    for (INT m = 0; m < 5; m++)
    {
        D3D12_RESOURCE_DESC Desc;
        appMemzero(&Desc, sizeof(Desc));
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = (UINT64)Make[m].W;
        Desc.Height           = (UINT)Make[m].H;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = Make[m].Format;
        Desc.SampleDesc.Count = 1;
        Desc.Flags            = Make[m].Flags;
        const HRESULT Hr = FHost64Link::MakeShared(Device, Desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   Textures[Make[m].Which].Slot());
        if (FAILED(Hr))
        {
            RdxLog(TEXT("host64: shared texture %i (%ix%i, format %i) could not be made (0x%08X) - the built-in upscaler "
                        "runs, and these sizes are not tried again"), (INT)Make[m].Which, Make[m].W, Make[m].H,
                   (INT)Make[m].Format, (INT)Hr);
            ReleaseTextures();
            FailedW = RW;
            FailedH = RH;
            return 0;
        }
    }
    if (!OutputHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
        appMemzero(&HeapDesc, sizeof(HeapDesc));
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.NumDescriptors = 1;
        HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), OutputHeap.VoidSlot())))
        {
            RdxLog(TEXT("host64: no descriptor heap for the upscaled picture - the built-in upscaler runs"));
            ReleaseTextures();
            FailedW = RW;
            FailedH = RH;
            return 0;
        }
    }
    Device->CreateShaderResourceView(Textures[Host64::Resource_Output].Get(), nullptr,
                                     OutputHeap->GetCPUDescriptorHandleForHeapStart());

    // Frame generation's three. Not having them costs the upscale nothing: they are left out, said once, and not tried
    // again for the same sizes.
    if (bFg)
    {
        const FMake MakeFg[3] = {
            { Host64::Resource_Present,   BackFmt, DW, DH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET },
            { Host64::Resource_HudLess,   BackFmt, DW, DH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET },
            { Host64::Resource_Generated, BackFmt, DW, DH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS },
        };
        UBOOL bMade = 1;
        for (INT m = 0; m < 3 && bMade; m++)
        {
            D3D12_RESOURCE_DESC Desc;
            appMemzero(&Desc, sizeof(Desc));
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = (UINT64)MakeFg[m].W;
            Desc.Height           = (UINT)MakeFg[m].H;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = MakeFg[m].Format;
            Desc.SampleDesc.Count = 1;
            Desc.Flags            = MakeFg[m].Flags;
            const HRESULT Hr = FHost64Link::MakeShared(Device, Desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       Textures[MakeFg[m].Which].Slot());
            if (FAILED(Hr))
            {
                RdxLog(TEXT("frame generation: its texture %i (%ix%i, format %i) could not be made (0x%08X) - the upscale "
                            "goes on without it"), (INT)MakeFg[m].Which, DW, DH, (INT)BackFmt, (INT)Hr);
                bMade = 0;
            }
        }
        if (bMade && !RtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC HeapDesc;
            appMemzero(&HeapDesc, sizeof(HeapDesc));
            HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            HeapDesc.NumDescriptors = 2;
            bMade = SUCCEEDED(Device->CreateDescriptorHeap(&HeapDesc, __uuidof(ID3D12DescriptorHeap), RtvHeap.VoidSlot()));
            RtvStep = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        if (bMade)
        {
            Device->CreateRenderTargetView(Textures[Host64::Resource_Present].Get(), nullptr, PresentRtv());
            Device->CreateRenderTargetView(Textures[Host64::Resource_HudLess].Get(), nullptr, HudLessRtv());
        }
        else
        {
            Textures[Host64::Resource_Present].Release();
            Textures[Host64::Resource_HudLess].Release();
            Textures[Host64::Resource_Generated].Release();
        }
    }

    ID3D12Resource* Shared[Host64::Resource_Count];
    for (INT r = 0; r < Host64::Resource_Count; r++)
        Shared[r] = Textures[r].Get();
    if (!Link.BeginShare(Shared, GHostShareMs))
    {
        ReleaseTextures();
        Poll();
        return 0;
    }
    RenderW      = RW;
    RenderH      = RH;
    OutW         = OW;
    OutH         = OH;
    SceneFormat  = SceneFmt;
    OutputFormat = OutputFmt;
    bFrameGenAsked = bFg;
    BackFormat     = BackFmt;
    DisplayW       = DW;
    DisplayH       = DH;
    FailedW = FailedH = 0;
    bFresh = 1;
    RdxLog(TEXT("host64: shared textures handed over - %ix%i -> %ix%i, colour format %i, output format %i%s; the host makes "
                "its upscaler for them, the built-in one draws meanwhile"), RW, RH, OW, OH, (INT)ReadableColour(SceneFmt),
           (INT)OutputFmt, !bFg ? TEXT("") : (Textures[Host64::Resource_Generated] ? TEXT(", with frame generation's")
                                                                                   : TEXT(", frame generation's left out")));
    return 0;
}

UBOOL FHostUpscale::FrameGenReady() const
{
    return (bOn && Link.Shared() && Textures[Host64::Resource_Present] && Textures[Host64::Resource_HudLess] &&
            Textures[Host64::Resource_Generated] && (Link.Services() & Host64::Service_FrameGen)) ? 1 : 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE FHostUpscale::PresentRtv() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE H;
    H.ptr = RtvHeap ? RtvHeap->GetCPUDescriptorHandleForHeapStart().ptr : 0;
    return H;
}

D3D12_CPU_DESCRIPTOR_HANDLE FHostUpscale::HudLessRtv() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE H = PresentRtv();
    if (H.ptr)
        H.ptr += RtvStep;
    return H;
}

UBOOL FHostUpscale::SubmitFrameGen(ID3D12CommandQueue* Queue, Host64::FFrame& Frame)
{
    return Link.Submit(Queue, Frame, nullptr, false) ? 1 : 0;
}

UINT FHostUpscale::WorkFgUs() const
{
    return bOn ? (UINT)Link.WorkFgUs() : 0;
}

void FHostUpscale::CopyInputs(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* const Sources[4],
                              const D3D12_RESOURCE_STATES States[4])
{
    ID3D12Resource* const Colour   = Sources[0];
    ID3D12Resource* const Depth    = Sources[1];
    ID3D12Resource* const Motion   = Sources[2];
    ID3D12Resource* const Reactive = Sources[3];
    ID3D12Resource* const Theirs[4] = { Textures[Host64::Resource_Color].Get(), Textures[Host64::Resource_Depth].Get(),
                                        Textures[Host64::Resource_Motion].Get(),
                                        Reactive ? Textures[Host64::Resource_Reactive].Get() : nullptr };
    // One batch: the frame's textures to be copied from, the host's to be copied into. A batch is where the GPU may
    // have to wait for what came before; two of them where four would do.
    D3D12_RESOURCE_BARRIER B[8];
    INT N = 0;
    for (INT i = 0; i < 4; i++)
    {
        if (!Sources[i] || !Theirs[i])
            continue;
        appMemzero(&B[N], sizeof(B[N]));
        B[N].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[N].Transition.pResource   = Sources[i];
        B[N].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B[N].Transition.StateBefore = States[i];
        B[N].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        N++;
        B[N]                        = B[N - 1];
        B[N].Transition.pResource   = Theirs[i];
        B[N].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        B[N].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        N++;
    }
    Cmd->ResourceBarrier(N, B);

    // A copy whose formats would not match is left out, once said so: the host then reads what the texture held.
    static INT Told = 0;
    if (Colour && Theirs[0] && ReadableColour(Colour->GetDesc().Format) == Theirs[0]->GetDesc().Format)
        Cmd->CopyResource(Theirs[0], Colour);
    else if (Told++ < 3)
        RdxLog(TEXT("host64: the scene's colour does not match its shared texture - not copied"));
    if (Depth && Theirs[1] && DepthCopies(Depth->GetDesc().Format))
    {
        // The depth plane alone - subresource 0 - into the one float a pixel the host reads.
        D3D12_TEXTURE_COPY_LOCATION Src, Dst;
        appMemzero(&Src, sizeof(Src));
        appMemzero(&Dst, sizeof(Dst));
        Src.pResource        = Depth;
        Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;
        Dst.pResource        = Theirs[1];
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
    }
    else if (Told++ < 3)
        RdxLog(TEXT("host64: the depth buffer's format has no plane that copies into R32_FLOAT - not copied"));
    if (Motion && Theirs[2] && Motion->GetDesc().Format == GMotionFormat)
        Cmd->CopyResource(Theirs[2], Motion);
    else if (Told++ < 3)
        RdxLog(TEXT("host64: the motion vectors do not match their shared texture - not copied"));
    if (Reactive && Theirs[3] && Reactive->GetDesc().Format == GReactiveFormat)
        Cmd->CopyResource(Theirs[3], Reactive);

    // And back: the frame's textures to where they were, the host's to COMMON, where they change hands.
    for (INT i = 0; i < N; i++)
    {
        const D3D12_RESOURCE_STATES Was = B[i].Transition.StateBefore;
        B[i].Transition.StateBefore = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = Was;
    }
    Cmd->ResourceBarrier(N, B);
}

UBOOL FHostUpscale::Handover(ID3D12GraphicsCommandList* Cmd, ID3D12CommandAllocator* Allocator,
                             ID3D12CommandQueue* Queue, Host64::FFrame& Frame)
{
    if (FAILED(Cmd->Close()))
    {
        RdxLog(TEXT("host64: the frame's first half would not close - the frame is lost"));
        return 0;
    }
    ID3D12CommandList* Lists[1] = { Cmd };
    Queue->ExecuteCommandLists(1, Lists);
    if (bFresh)
    {
        Frame.Flags |= Host64::FrameFlag_Reset;
        bFresh = 0;
    }
    // False only when the link went down since Prepare - nothing between the two moves it. The frame then goes on with
    // whatever the output holds, and the next one runs the built-in upscaler.
    Link.Submit(Queue, Frame);
    if (FAILED(Cmd->Reset(Allocator, nullptr)))
    {
        RdxLog(TEXT("host64: the frame's second half would not open - the frame is lost"));
        return 0;
    }
    return 1;
}
