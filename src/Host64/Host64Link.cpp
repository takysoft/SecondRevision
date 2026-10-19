// SecondRevision - the renderer's end of the 64-bit host. See Host64Link.h.

#include "Host64Link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// What the host's fence is set to when the link goes down: above every frame number, so every wait the renderer's queue
// holds for the host lets go.
static const uint64_t GLetGo = ~0ull >> 1;

// Frames the host's backend may refuse in a row before the link gives up on it: for that many frames the output has not
// been the frame's own picture.
static const uint32_t GRefuseLimit = 10;

void FHost64Link::Log(const char* Format, ...)
{
    if (!LogFunc)
        return;
    char    Line[512];
    va_list Args;
    va_start(Args, Format);
    vsnprintf(Line, sizeof(Line), Format, Args);
    va_end(Args);
    LogFunc(Line);
}

// The link goes down: nothing is submitted any more, and every GPU wait for the host lets go at once - the renderer's
// queue may already hold a wait for a frame the host will never finish. A host still running is ended: one that hung
// could otherwise wake up later and signal a frame number below the one set here, which would stop the renderer's queue
// again (Poll sets it again should that happen in the moment the process takes to end).
void FHost64Link::Down(const char* Why)
{
    if (Why && Why[0])
    {
        strncpy_s(Text, sizeof(Text), Why, _TRUNCATE);
        Log("host64: %s", Why);
    }
    Now = State_Down;
    if (Process && ::WaitForSingleObject(Process, 0) == WAIT_TIMEOUT)
        ::TerminateProcess(Process, 1);
    if (ToRenderer)
        ToRenderer->Signal(GLetGo);
}

// A handle of ours, duplicated into the host's process for it to open; ours is closed. The host closes its copy
// when it has what it needs.
bool FHost64Link::GiveHandle(HANDLE Mine, uint64_t& Theirs)
{
    HANDLE Copy = nullptr;
    const BOOL bOk = ::DuplicateHandle(::GetCurrentProcess(), Mine, Process, &Copy, 0, FALSE,
                                       DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    Theirs = bOk ? (uint64_t)(uintptr_t)Copy : 0;
    return bOk != FALSE;
}

HRESULT FHost64Link::MakeShared(ID3D12Device* Dev, const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES Initial,
                                const D3D12_CLEAR_VALUE* Clear, ID3D12Resource** Out)
{
    D3D12_HEAP_PROPERTIES Heap = {};
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    return Dev->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_SHARED, &Desc, Initial, Clear, IID_PPV_ARGS(Out));
}

const char* FHost64Link::Message()
{
    if (Block && Now == State_Up)
    {
        ::MemoryBarrier();
        strncpy_s(Text, sizeof(Text), Block->Message, _TRUNCATE);
    }
    return Text;
}

bool FHost64Link::Start(ID3D12Device* Dev, const wchar_t* HostExe, uint32_t Want, DWORD TimeoutMs)
{
    Stop();
    Text[0] = 0;
    Now     = State_Off;
    Device  = Dev;
    bEndedStarting = false;
    if (!Device || !HostExe || !HostExe[0])
    {
        Down("no device, or no host to start");
        return false;
    }
    if (::GetFileAttributesW(HostExe) == INVALID_FILE_ATTRIBUTES)
    {
        Down("SecondRevisionHost64.exe is not where the renderer looks for it");
        return false;
    }

    // The two fences, shared; the event that wakes the host.
    if (FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ToHost))) ||
        FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ToRenderer))))
    {
        Down("the shared fences could not be made");
        Release();
        return false;
    }
    Wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The block: page-file backed, inheritable - it is the one handle the host inherits.
    SECURITY_ATTRIBUTES Inherit = {};
    Inherit.nLength        = sizeof(Inherit);
    Inherit.bInheritHandle = TRUE;
    Mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &Inherit, PAGE_READWRITE, 0, sizeof(Host64::FBlock), nullptr);
    Block   = Mapping ? (Host64::FBlock*)::MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Host64::FBlock))
                      : nullptr;
    if (!Wake || !Block)
    {
        Down("the host's block could not be made");
        Release();
        return false;
    }
    memset(Block, 0, sizeof(*Block));
    Block->Magic       = Host64::Magic;
    Block->Version     = Host64::ProtocolVersion;
    Block->RendererPid = ::GetCurrentProcessId();
    const LUID Luid    = Device->GetAdapterLuid();
    Block->AdapterLuidLow  = Luid.LowPart;
    Block->AdapterLuidHigh = Luid.HighPart;
    Block->WantBackend     = Want;

    // The job: when the game's process ends, for whatever reason, the host ends with it.
    Job = ::CreateJobObjectW(nullptr, nullptr);
    if (Job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limit = {};
        Limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limit, sizeof(Limit));
    }

    // Started suspended with the block as its only inherited handle, put in the job, then let go.
    SIZE_T ListBytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &ListBytes);
    LPPROC_THREAD_ATTRIBUTE_LIST List = (LPPROC_THREAD_ATTRIBUTE_LIST)::HeapAlloc(::GetProcessHeap(), 0, ListBytes);
    if (!List || !::InitializeProcThreadAttributeList(List, 1, 0, &ListBytes) ||
        !::UpdateProcThreadAttribute(List, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &Mapping, sizeof(Mapping), nullptr,
                                     nullptr))
    {
        if (List)
            ::HeapFree(::GetProcessHeap(), 0, List);
        Down("the host's start-up could not be prepared");
        Release();
        return false;
    }
    wchar_t Command[MAX_PATH + 64];
    swprintf_s(Command, L"\"%s\" %llu %lu%s", HostExe, (unsigned long long)(uintptr_t)Mapping,
               (unsigned long)::GetCurrentProcessId(), bDebugLayer ? L" debug" : L"");
    wchar_t Folder[MAX_PATH];
    wcscpy_s(Folder, HostExe);
    wchar_t* Slash = wcsrchr(Folder, L'\\');
    if (Slash)
        *Slash = 0;
    STARTUPINFOEXW Info = {};
    Info.StartupInfo.cb    = sizeof(Info);
    Info.lpAttributeList   = List;
    PROCESS_INFORMATION Pi = {};
    const BOOL bMade = ::CreateProcessW(HostExe, Command, nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        Slash ? Folder : nullptr, &Info.StartupInfo, &Pi);
    ::DeleteProcThreadAttributeList(List);
    ::HeapFree(::GetProcessHeap(), 0, List);
    if (!bMade)
    {
        char Why[128];
        sprintf_s(Why, "the host would not start (error %lu)", ::GetLastError());
        Down(Why);
        Release();
        return false;
    }
    Process = Pi.hProcess;
    if (Job && !::AssignProcessToJobObject(Job, Process))
        Log("host64: not in a job of its own (error %lu) - it still leaves when the game does", ::GetLastError());

    // The fences and the event, into the host's process.
    HANDLE FenceA = nullptr, FenceB = nullptr, WakeCopy = nullptr;
    const bool bHandles =
        SUCCEEDED(Device->CreateSharedHandle(ToHost, nullptr, GENERIC_ALL, nullptr, &FenceA)) &&
        SUCCEEDED(Device->CreateSharedHandle(ToRenderer, nullptr, GENERIC_ALL, nullptr, &FenceB)) &&
        GiveHandle(FenceA, Block->FenceToHost) && GiveHandle(FenceB, Block->FenceToRenderer) &&
        ::DuplicateHandle(::GetCurrentProcess(), Wake, Process, &WakeCopy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    Block->WakeEvent = (uint64_t)(uintptr_t)WakeCopy;
    ::MemoryBarrier();
    ::ResumeThread(Pi.hThread);
    ::CloseHandle(Pi.hThread);
    if (!bHandles)
    {
        Down("the fences could not be handed to the host");
        Stop();
        return false;
    }
    Now             = State_Starting;
    StartedAt       = ::GetTickCount64();
    ShareGeneration = 0;
    bSharing        = false;
    if (TimeoutMs == 0)
        return true;

    // Waited for here: Ready, Failed, its process gone, or out of time.
    while (Poll(TimeoutMs, 0) == State_Starting)
        ::Sleep(1);
    return Now == State_Up;
}

FHost64Link::EState FHost64Link::Poll(DWORD StartMs, DWORD HangMs)
{
    if (Now == State_Starting)
    {
        const uint32_t State = *(volatile uint32_t*)&Block->HostState;
        if (State == Host64::HostState_Ready)
        {
            ::MemoryBarrier();
            strncpy_s(Text, sizeof(Text), Block->Message, _TRUNCATE);
            Log("host64: up after %llu ms (pid %u, build %u): %s", (unsigned long long)(::GetTickCount64() - StartedAt),
                Block->HostPid, Block->HostVersion, Text);
            Now       = State_Up;
            NextFrame = 1;
            LagSince  = 0;
            LagDone   = 0;
        }
        else if (State == Host64::HostState_Failed || ::WaitForSingleObject(Process, 0) == WAIT_OBJECT_0 ||
                 ::GetTickCount64() - StartedAt > StartMs)
        {
            ::MemoryBarrier();
            char Why[Host64::MessageChars + 32];
            const bool bEnded = State != Host64::HostState_Failed && ::WaitForSingleObject(Process, 0) == WAIT_OBJECT_0;
            DWORD      Code   = 0;
            if (bEnded)
                ::GetExitCodeProcess(Process, &Code);
            if (State == Host64::HostState_Failed && Block->Message[0])
                sprintf_s(Why, "%s", Block->Message);
            else if (bEnded)
                sprintf_s(Why, "the host ended before it was ready (exit code 0x%08lX)", (unsigned long)Code);
            else
                sprintf_s(Why, "%s", "the host did not answer in time");
            bEndedStarting = bEnded;
            Down(Why);
        }
    }
    else if (Now == State_Up)
    {
        if (bSharing)
            PollShare();
        if (Now == State_Up && bRaySharing)
            PollRayShare();
        if (Now == State_Up && HangMs)
            CheckAlive(HangMs);
    }
    else if (Now == State_Down && ToRenderer && ToRenderer->GetCompletedValue() < GLetGo)
        ToRenderer->Signal(GLetGo);
    return Now;
}

// A share handed over: taken (the host answered with its generation), or down - the host failed making its context for
// the textures, died, or did not answer in its time.
void FHost64Link::PollShare()
{
    if (*(volatile uint32_t*)&Block->ResourceAck == ShareGeneration)
    {
        bSharing = false;
        Log("host64: textures of generation %u taken in %llu ms", ShareGeneration,
            (unsigned long long)(::GetTickCount64() - ShareSince));
        return;
    }
    if (*(volatile uint32_t*)&Block->HostState != Host64::HostState_Ready ||
        ::WaitForSingleObject(Process, 0) == WAIT_OBJECT_0 || ::GetTickCount64() - ShareSince > ShareMs)
    {
        ::MemoryBarrier();
        Down(Block->Message[0] ? Block->Message : "the host did not take the shared textures in time");
    }
}

bool FHost64Link::Share(ID3D12Resource* const Resources[Host64::Resource_Count], DWORD TimeoutMs)
{
    if (!BeginShare(Resources, TimeoutMs))
        return false;
    while (Now == State_Up && bSharing)
    {
        PollShare();
        if (bSharing)
            ::Sleep(1);
    }
    return Now == State_Up;
}

bool FHost64Link::BeginShare(ID3D12Resource* const Resources[Host64::Resource_Count], DWORD TimeoutMs)
{
    // One share at a time: the block holds the last one's handles until the host has read them.
    if (Now != State_Up || bSharing)
        return false;
    for (int r = 0; r < Host64::Resource_Count; r++)
    {
        Host64::FResource& Out = Block->Resources[r];
        memset(&Out, 0, sizeof(Out));
        ID3D12Resource* const Res = Resources[r];
        if (!Res)
            continue;
        HANDLE Shared = nullptr;
        if (FAILED(Device->CreateSharedHandle(Res, nullptr, GENERIC_ALL, nullptr, &Shared)) ||
            !GiveHandle(Shared, Out.Handle))
        {
            Down("a texture could not be shared with the host");
            return false;
        }
        const D3D12_RESOURCE_DESC D = Res->GetDesc();
        Out.Width  = (uint32_t)D.Width;
        Out.Height = D.Height;
        Out.Format = (uint32_t)D.Format;
    }
    ShareGeneration = Block->ResourceGeneration + 1;
    ::MemoryBarrier();
    Block->ResourceGeneration = ShareGeneration;
    ::SetEvent(Wake);
    bSharing   = true;
    ShareSince = ::GetTickCount64();
    ShareMs    = TimeoutMs;
    return true;
}

// The rays' share, as the textures': the host lets the last one go, opens this one and answers. The frames' lists are a
// mapping of the share's own, as long as its room asks - the host maps the same memory, and holds the last share's until
// it takes this one.
bool FHost64Link::BeginShareRays(ID3D12Resource* const Resources[Host64::Ray_Count], uint32_t LevelVertices,
                                 uint32_t SkyVertices, uint32_t ThinVertices, uint32_t BodyRoom, DWORD TimeoutMs)
{
    if (Now != State_Up || bRaySharing || !(Block->Services & Host64::Service_Rays))
        return false;
    // Every handle made here first: something the driver will not share stops the share with nothing handed over,
    // and the link - the upscaler's - stays as it is.
    HANDLE Mine[Host64::Ray_Count] = {};
    bool   bMade = true;
    for (int r = 0; r < Host64::Ray_Count && bMade; r++)
        if (Resources[r] && FAILED(Device->CreateSharedHandle(Resources[r], nullptr, GENERIC_ALL, nullptr, &Mine[r])))
        {
            Log("host64: the rays' resource %d could not be shared - no rays' share is handed over", r);
            Mine[r] = nullptr;
            bMade   = false;
        }
    const uint32_t Bytes = Host64::RayFramesBytes(BodyRoom);
    const HANDLE   Lists = bMade ? ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, Bytes, nullptr)
                                 : nullptr;
    void* const    View  = Lists ? ::MapViewOfFile(Lists, FILE_MAP_ALL_ACCESS, 0, 0, Bytes) : nullptr;
    if (!View)
    {
        if (bMade)
            Log("host64: the rays' lists could not be made (%u bytes) - no rays' share is handed over", Bytes);
        if (Lists)
            ::CloseHandle(Lists);
        for (int r = 0; r < Host64::Ray_Count; r++)
            if (Mine[r])
                ::CloseHandle(Mine[r]);
        return false;
    }
    memset(View, 0, Bytes);
    if (RayFrames)
        ::UnmapViewOfFile(RayFrames);
    if (RayMapping)
        ::CloseHandle(RayMapping);
    RayMapping = Lists;
    RayFrames  = View;
    RayRoom    = BodyRoom;

    Host64::FRayShare& Out = Block->Rays;
    memset(&Out, 0, sizeof(Out));
    bool   bGiven   = true;
    HANDLE ListCopy = nullptr;
    for (int r = 0; r < Host64::Ray_Count; r++)
    {
        if (!Mine[r])
            continue;
        // Once one could not be given the rest are only let go; giving closes ours whatever comes of it.
        if (!bGiven)
        {
            ::CloseHandle(Mine[r]);
            continue;
        }
        if (!GiveHandle(Mine[r], Out.Resources[r].Handle))
        {
            bGiven = false;
            continue;
        }
        const D3D12_RESOURCE_DESC D = Resources[r]->GetDesc();
        Out.Resources[r].Width  = (uint32_t)D.Width;
        Out.Resources[r].Height = D.Height;
        Out.Resources[r].Format = (uint32_t)D.Format;
    }
    if (!bGiven ||
        !::DuplicateHandle(::GetCurrentProcess(), RayMapping, Process, &ListCopy, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        Down("the rays' share could not be handed to the host");
        return false;
    }
    Out.Frames        = (uint64_t)(uintptr_t)ListCopy;
    Out.BodyRoom      = BodyRoom;
    Out.LevelVertices = LevelVertices;
    Out.SkyVertices   = SkyVertices;
    Out.ThinVertices  = ThinVertices;
    RayGeneration     = Block->RayGeneration + 1;
    ::MemoryBarrier();
    Block->RayGeneration = RayGeneration;
    ::SetEvent(Wake);
    bRaySharing   = true;
    RayShareSince = ::GetTickCount64();
    RayShareMs    = TimeoutMs;
    return true;
}

// The rays' share handed over: answered - taken, or acknowledged and not taken, which the host's log explains and
// RaysShared tells - or the link is down: the host failed, died, or did not answer in its time.
void FHost64Link::PollRayShare()
{
    if (*(volatile uint32_t*)&Block->RayAck == RayGeneration)
    {
        bRaySharing = false;
        Log("host64: the rays' share of generation %u %s in %llu ms", RayGeneration,
            (*(volatile uint32_t*)&Block->RayTaken == RayGeneration) ? "taken" : "NOT taken by the host (its log says why)",
            (unsigned long long)(::GetTickCount64() - RayShareSince));
        return;
    }
    if (*(volatile uint32_t*)&Block->HostState != Host64::HostState_Ready ||
        ::WaitForSingleObject(Process, 0) == WAIT_OBJECT_0 || ::GetTickCount64() - RayShareSince > RayShareMs)
    {
        ::MemoryBarrier();
        Down(Block->Message[0] ? Block->Message : "the host did not answer the rays' share in time");
    }
}

bool FHost64Link::ShareRays(ID3D12Resource* const Resources[Host64::Ray_Count], uint32_t LevelVertices,
                            uint32_t SkyVertices, uint32_t ThinVertices, uint32_t BodyRoom, DWORD TimeoutMs)
{
    if (!BeginShareRays(Resources, LevelVertices, SkyVertices, ThinVertices, BodyRoom, TimeoutMs))
        return false;
    while (Now == State_Up && bRaySharing)
    {
        PollRayShare();
        if (bRaySharing)
            ::Sleep(1);
    }
    return RaysShared();
}

// A slot is written again only once the host has read what it held. With the slots there are that does not come up; a
// host a whole round behind is waited for a moment, and past that it counts as fallen behind.
bool FHost64Link::SlotFree(uint64_t Frame)
{
    for (int Tries = 0; Frame > Host64::FrameSlots &&
                        (uint64_t)::InterlockedCompareExchange64((volatile LONG64*)&Block->Taken, 0, 0) <
                            Frame - Host64::FrameSlots;
         Tries++)
    {
        if (Tries >= 100)
        {
            Down("the host fell a whole round of frames behind");
            return false;
        }
        ::Sleep(1);
    }
    return true;
}

Host64::FRayFrame* FHost64Link::RayFrame()
{
    if (!RaysShared() || !RayFrames || !SlotFree(NextFrame))
        return nullptr;
    return (Host64::FRayFrame*)((char*)RayFrames +
                                (size_t)(NextFrame % Host64::FrameSlots) * Host64::RayFrameBytes(RayRoom));
}

bool FHost64Link::Submit(ID3D12CommandQueue* Queue, Host64::FFrame& Frame, ID3D12CommandQueue* WaitQueue, bool bWait)
{
    if (Now != State_Up || bSharing || !Queue || ((Frame.Services & Host64::Service_Rays) && !RaysShared()))
        return false;
    const uint64_t N = NextFrame;
    if (!SlotFree(N))
        return false;
    NextFrame++;
    Frame.Frame = N;
    Queue->Signal(ToHost, N);
    Block->Frames[N % Host64::FrameSlots] = Frame;
    ::InterlockedExchange64((volatile LONG64*)&Block->Submitted, (LONG64)N);
    ::SetEvent(Wake);
    if (bWait)
        (WaitQueue ? WaitQueue : Queue)->Wait(ToRenderer, N);
    return true;
}

bool FHost64Link::CheckAlive(DWORD HangMs)
{
    if (Now != State_Up)
        return false;
    if (::WaitForSingleObject(Process, 0) == WAIT_OBJECT_0)
    {
        Down("the host's process ended");
        return false;
    }
    if (*(volatile uint32_t*)&Block->HostState != Host64::HostState_Ready)
    {
        ::MemoryBarrier();
        Down(Block->Message[0] ? Block->Message : "the host gave up");
        return false;
    }
    // A long run of refused frames: all that time the output was not the frame's own picture - an old one, or nothing.
    const uint32_t Refused = *(volatile uint32_t*)&Block->FailedInARow;
    if (Refused >= GRefuseLimit)
    {
        ::MemoryBarrier();
        char Why[Host64::MessageChars + 64];
        sprintf_s(Why, "the host refused %u frames in a row (%s)", Refused, Block->Message);
        Down(Why);
        return false;
    }
    // Hung, judged where it hurts: a frame whose inputs our GPU has finished (our fence reached it) and whose result the
    // host has not signalled - our queue is waiting on it. That is a host that stopped taking frames and one whose GPU
    // work never finishes (a lost device) alike, where the host's own word may never come. Behind for a moment is normal;
    // behind with nothing of the host's finishing for HangMs is not.
    const uint64_t Ready = ToHost->GetCompletedValue();
    const uint64_t Done  = ToRenderer->GetCompletedValue();
    const uint64_t Now64 = ::GetTickCount64();
    if (Done >= Ready)
        LagSince = 0;
    else if (!LagSince || Done != LagDone)
    {
        LagSince = Now64;
        LagDone  = Done;
    }
    else if (Now64 - LagSince > HangMs)
    {
        Down("the host's work for a frame did not finish");
        return false;
    }
    return true;
}

// Everything the link holds, let go. The host's fence is set past every frame first, once its process can no longer
// set it back.
void FHost64Link::Release()
{
    if (ToRenderer)
        ToRenderer->Signal(GLetGo);
    if (Process)
        ::CloseHandle(Process);
    Process = nullptr;
    if (Job)
        ::CloseHandle(Job);
    Job = nullptr;
    if (RayFrames)
        ::UnmapViewOfFile(RayFrames);
    RayFrames = nullptr;
    if (RayMapping)
        ::CloseHandle(RayMapping);
    RayMapping    = nullptr;
    RayRoom       = 0;
    RayGeneration = 0;
    bRaySharing   = false;
    if (Block)
        ::UnmapViewOfFile(Block);
    Block = nullptr;
    if (Mapping)
        ::CloseHandle(Mapping);
    Mapping = nullptr;
    if (Wake)
        ::CloseHandle(Wake);
    Wake = nullptr;
    if (ToHost)
        ToHost->Release();
    ToHost = nullptr;
    if (ToRenderer)
        ToRenderer->Release();
    ToRenderer = nullptr;
    Device = nullptr;
}

// A host that runs is asked to leave, and ended if it does not within two seconds; either way it is gone before the
// fence is set for the last time. A link that went down stays down (its reason stays readable) until the next Start.
void FHost64Link::Stop()
{
    if (Process && ::WaitForSingleObject(Process, 0) == WAIT_TIMEOUT)
    {
        if (Block)
        {
            Block->QuitRequested = 1;
            ::SetEvent(Wake);
        }
        if (::WaitForSingleObject(Process, 2000) != WAIT_OBJECT_0)
        {
            ::TerminateProcess(Process, 1);
            ::WaitForSingleObject(Process, 2000);
        }
    }
    Release();
    if (Now != State_Down)
        Now = State_Off;
}
