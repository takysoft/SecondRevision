// SecondRevision - two pictures a frame on the screen. See FramePacer.h.
#include "Pch.h"
#include "Log.h"
#include "FramePacer.h"

// CreateWaitableTimerExW's high-resolution flag (Windows 10 1803 and later), spelled out for older SDK headers.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// The most the real picture is held back after the generated one. A frame that took longer than twice this - a stall, a
// level loading - is not a rhythm to keep.
static const DOUBLE GPaceMaxSec = 0.05;

// How long Submit waits for the thread before it leaves a frame out, and Flush before it gives up on it, in ms.
static const ULONGLONG GSubmitWaitMs = 100;
static const ULONGLONG GFlushWaitMs  = 2000;

// The thread's own clock: the performance counter, not the engine's, which is not the thread's to call.
static DOUBLE PacerSeconds()
{
    static LARGE_INTEGER Freq = { 0 };
    if (!Freq.QuadPart)
        ::QueryPerformanceFrequency(&Freq);
    LARGE_INTEGER Now;
    ::QueryPerformanceCounter(&Now);
    return (DOUBLE)Now.QuadPart / (DOUBLE)Freq.QuadPart;
}

FFramePacer::FFramePacer()
    : FenceValue(0), Submitted(0), NextSlot(0), ListEvent(nullptr), FlushEvent(nullptr), Thread(nullptr), Wake(nullptr),
      Drained(nullptr), Hurry(nullptr), ThreadEvent(nullptr), Timer(nullptr), bQuit(0), Head(0), Tail(0), Result(S_OK),
      Dropped(0), LastGenAt(0.0), GenThroughAt(0.0), LastRealAt(0.0), IntervalSec(0.0)
{
    for (INT s = 0; s < Slots; s++)
        SlotDone[s] = 0;
    appMemzero(Ring, sizeof(Ring));
    appMemzero(Costs, sizeof(Costs));
}

FFramePacer::~FFramePacer()
{
    Shutdown();
}

void FFramePacer::AddCost(INT Which, DOUBLE Seconds)
{
    FCost& C = Costs[Which];
    C.Sum += Seconds;
    if (Seconds > C.Max)
        C.Max = Seconds;
    C.Count++;
}

void FFramePacer::TakeCosts(FCost* Out)
{
    for (INT k = 0; k < Cost_Count; k++)
    {
        Out[k] = Costs[k];
        Costs[k].Sum   = 0.0;
        Costs[k].Max   = 0.0;
        Costs[k].Count = 0;
    }
}

UBOOL FFramePacer::Init(ID3D12Device* Dev)
{
    if (Queue)
        return 1;
    if (!Dev)
        return 0;
    Dev->AddRef();
    *Device.Slot() = Dev;

    // A high-priority queue: what it does is small, and all of it is about when a picture reaches the screen.
    D3D12_COMMAND_QUEUE_DESC Q;
    appMemzero(&Q, sizeof(Q));
    Q.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(Device->CreateCommandQueue(&Q, __uuidof(ID3D12CommandQueue), Queue.VoidSlot())))
    {
        Q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(Device->CreateCommandQueue(&Q, __uuidof(ID3D12CommandQueue), Queue.VoidSlot())))
        {
            RdxLog(TEXT("frame generation: no queue to present from - it cannot run"));
            Shutdown();
            return 0;
        }
    }
    if (FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), Fence.VoidSlot())) ||
        FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), CopyFence.VoidSlot())))
    {
        RdxLog(TEXT("frame generation: no fences to present with - it cannot run"));
        Shutdown();
        return 0;
    }
    for (INT s = 0; s < Slots; s++)
    {
        if (FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                  Allocators[s].VoidSlot())) ||
            FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocators[s].Get(), nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), Lists[s].VoidSlot())))
        {
            RdxLog(TEXT("frame generation: no command lists to present with - it cannot run"));
            Shutdown();
            return 0;
        }
        Lists[s]->Close();
        SlotDone[s] = 0;
    }
    ListEvent   = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    FlushEvent  = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Wake        = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Drained     = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Hurry       = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ThreadEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Timer       = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const UBOOL bFineTimer = Timer ? 1 : 0;
    if (!Timer)
        Timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    bQuit       = 0;
    Head = Tail = 0;
    Result      = S_OK;
    Dropped     = 0;
    Submitted    = 0;
    LastGenAt    = 0.0;
    GenThroughAt = 0.0;
    LastRealAt   = 0.0;
    IntervalSec  = 0.0;
    if (ListEvent && FlushEvent && Wake && Drained && Hurry && ThreadEvent)
        Thread = ::CreateThread(nullptr, 0, &FFramePacer::ThreadMain, this, 0, nullptr);
    if (!Thread)
    {
        RdxLog(TEXT("frame generation: the presenting thread would not start - it cannot run"));
        Shutdown();
        return 0;
    }
    // Asleep nearly all the time, and when it wakes, a picture's time on the screen depends on it.
    ::SetThreadPriority(Thread, THREAD_PRIORITY_HIGHEST);
    RdxLog(TEXT("frame generation: presenting from a queue of its own (priority %s) and a thread of its own (%s timer)"),
           (Q.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH) ? TEXT("high") : TEXT("normal"),
           bFineTimer ? TEXT("a high-resolution") : TEXT("an ordinary"));
    return 1;
}

void FFramePacer::Shutdown()
{
    if (Thread)
    {
        // What was handed over goes out first; then the thread stops.
        Flush();
        ::InterlockedExchange(&bQuit, 1);
        ::SetEvent(Hurry);
        ::SetEvent(Wake);
        if (::WaitForSingleObject(Thread, 2000) != WAIT_OBJECT_0)
            RdxLog(TEXT("frame generation: the presenting thread did not stop in time"));
        ::CloseHandle(Thread);
        Thread = nullptr;
    }
    // Whatever the rendering queue may still wait for, let go; and the queue emptied before anything it uses goes.
    if (CopyFence)
        CopyFence->Signal(~0ull >> 1);
    if (Queue && Fence && FenceValue && FlushEvent && Fence->GetCompletedValue() < FenceValue &&
        SUCCEEDED(Fence->SetEventOnCompletion(FenceValue, FlushEvent)))
        ::WaitForSingleObject(FlushEvent, 5000);
    for (INT s = 0; s < Slots; s++)
    {
        Lists[s].Release();
        Allocators[s].Release();
        SlotDone[s] = 0;
    }
    CopyFence.Release();
    Fence.Release();
    Queue.Release();
    Device.Release();
    HANDLE* const Handles[7] = { &ListEvent, &FlushEvent, &Wake, &Drained, &Hurry, &ThreadEvent, &Timer };
    for (INT h = 0; h < 7; h++)
        if (*Handles[h])
        {
            ::CloseHandle(*Handles[h]);
            *Handles[h] = nullptr;
        }
    FenceValue = 0;
    Submitted  = 0;
    NextSlot   = 0;
    Head = Tail = 0;
}

UINT64 FFramePacer::Signal()
{
    FenceValue++;
    Queue->Signal(Fence.Get(), FenceValue);
    return FenceValue;
}

// The next command list, its allocator reset - once the GPU is done with what it last held.
ID3D12GraphicsCommandList* FFramePacer::NextList()
{
    const INT S = NextSlot;
    NextSlot = (NextSlot + 1) % Slots;
    const DOUBLE T0 = PacerSeconds();
    if (Fence->GetCompletedValue() < SlotDone[S] && SUCCEEDED(Fence->SetEventOnCompletion(SlotDone[S], ListEvent)))
        ::WaitForSingleObject(ListEvent, 1000);
    AddCost(Cost_ListWait, PacerSeconds() - T0);
    Allocators[S]->Reset();
    Lists[S]->Reset(Allocators[S].Get(), nullptr);
    return Lists[S].Get();
}

void FFramePacer::Flush()
{
    if (!Queue)
        return;
    // The thread lets its hold go and presents what it has; then the queue empties. The thread's last act on the queue
    // is a Signal of its fence, so once no job is left, that value reached means the queue is idle.
    ::SetEvent(Hurry);
    ::SetEvent(Wake);
    const ULONGLONG Since = ::GetTickCount64();
    while (Tail != Head && ::GetTickCount64() - Since < GFlushWaitMs)
        ::WaitForSingleObject(Drained, 1);
    if (Tail != Head)
        RdxLog(TEXT("frame generation: the presenting thread did not finish in time (%i frames left)"), (INT)(Head - Tail));
    ::ResetEvent(Hurry);
    const UINT64 Last = FenceValue;
    if (Last && Fence->GetCompletedValue() < Last && SUCCEEDED(Fence->SetEventOnCompletion(Last, FlushEvent)) &&
        ::WaitForSingleObject(FlushEvent, 5000) == WAIT_TIMEOUT)
        RdxLog(TEXT("frame generation: the presenting queue did not empty in time (waited for %I64u, reached %I64u)"),
               Last, Fence->GetCompletedValue());
}

// A picture into a back buffer: both in COMMON (a back buffer's PRESENT is the same state), and both back in it after.
static void CopyPicture(ID3D12GraphicsCommandList* List, ID3D12Resource* From, ID3D12Resource* To)
{
    D3D12_RESOURCE_BARRIER B[2];
    appMemzero(B, sizeof(B));
    for (INT i = 0; i < 2; i++)
    {
        B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    }
    B[0].Transition.pResource  = From;
    B[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    B[1].Transition.pResource  = To;
    B[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    List->ResourceBarrier(2, B);
    List->CopyResource(To, From);
    for (INT i = 0; i < 2; i++)
    {
        B[i].Transition.StateBefore = B[i].Transition.StateAfter;
        B[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    }
    List->ResourceBarrier(2, B);
}

HRESULT FFramePacer::Submit(IDXGISwapChain3* Chain, ID3D12Resource* const* BackBuffers, UINT BufferCount,
                            ID3D12Resource* Generated, ID3D12Fence* GenFence, UINT64 GenValue,
                            ID3D12Resource* Real, ID3D12Fence* RealFence, UINT64 RealValue, UINT Sync, UINT Flags)
{
    if (!Queue || !Thread || !Chain || !BackBuffers || !Real || !RealFence)
        return E_FAIL;
    // Two frames waiting already: a moment for one of them to go, and past that this one is left out - the engine is not
    // held for the screen.
    const DOUBLE    T0    = PacerSeconds();
    const ULONGLONG Since = ::GetTickCount64();
    while (Head - Tail >= MaxWaiting)
    {
        if (::GetTickCount64() - Since > GSubmitWaitMs)
        {
            ::InterlockedIncrement(&Dropped);
            AddCost(Cost_Submit, PacerSeconds() - T0);
            return (HRESULT)::InterlockedExchange(&Result, S_OK);
        }
        ::WaitForSingleObject(Drained, 1);
    }
    AddCost(Cost_Submit, PacerSeconds() - T0);
    FJob& J       = Ring[Head % Jobs];
    J.Chain       = Chain;
    J.BackBuffers = BackBuffers;
    J.BufferCount = BufferCount;
    J.Generated   = Generated;
    J.GenFence    = GenFence;
    J.GenValue    = GenValue;
    J.Real        = Real;
    J.RealFence   = RealFence;
    J.RealValue   = RealValue;
    J.Sync        = Sync;
    J.Flags       = Flags;
    J.Frame       = ++Submitted;
    J.HandedAt    = PacerSeconds();
    ::InterlockedExchange(&Head, Head + 1);
    ::SetEvent(Wake);
    return (HRESULT)::InterlockedExchange(&Result, S_OK);
}

UBOOL FFramePacer::TakeStamp(FStamp& Out)
{
    const LONG Written = StampsWritten;
    if (StampsTaken == Written)
        return 0;
    if (Written - StampsTaken > StampRoom)
        StampsTaken = Written - StampRoom;
    Out = Stamps[StampsTaken % StampRoom];
    StampsTaken++;
    return 1;
}

void FFramePacer::NoteResult(HRESULT Hr)
{
    if (Hr != S_OK)
        ::InterlockedExchange(&Result, (LONG)Hr);
}

UBOOL FFramePacer::CopyToBackBuffer(const FJob& J, ID3D12Resource* Picture)
{
    const UINT B = J.Chain->GetCurrentBackBufferIndex();
    if (B >= J.BufferCount || !J.BackBuffers[B])
    {
        NoteResult(E_FAIL);
        return 0;
    }
    const INT Slot = NextSlot;
    ID3D12GraphicsCommandList* const L = NextList();
    CopyPicture(L, Picture, J.BackBuffers[B]);
    L->Close();
    ID3D12CommandList* Lists1[1] = { L };
    Queue->ExecuteCommandLists(1, Lists1);
    SlotDone[Slot] = Signal();
    return 1;
}

UINT64 FFramePacer::PresentNow(const FJob& J, INT Cost)
{
    const DOUBLE T0 = PacerSeconds();
    NoteResult(J.Chain->Present(J.Sync, J.Flags));
    AddCost(Cost, PacerSeconds() - T0);
    return Signal();
}

void FFramePacer::WaitThrough(UINT64 Value)
{
    if (Value && Fence->GetCompletedValue() < Value && SUCCEEDED(Fence->SetEventOnCompletion(Value, ThreadEvent)))
        ::WaitForSingleObject(ThreadEvent, 500);
}

// The generated picture, then - half the frames' interval after it went through - the real one. The real one is copied
// into its back buffer at once, right after the generated one's Present, and only its Present waits for the hold: so
// both of the frame's pictures are out of the shared textures before the hold, and the rendering queue (which waits for
// that before it draws the next frame's picture - GetCopyFence) never waits for the hold. Were it to, every hold would
// hold the next frame's work back by as much, the frames would take longer, the interval with them, and the hold again:
// measured with the real picture copied after the hold and no frame limit, 22 frames a second where the card does 74.
// The interval is measured here, on the pictures themselves, and smoothed: a frame that took long is a stall, not a new
// rhythm.
void FFramePacer::PresentFrame(const FJob& J)
{
    UINT64        GenDone = 0;
    LARGE_INTEGER GenAt;
    GenAt.QuadPart = 0;
    if (J.Generated)
    {
        if (J.GenFence)
            Queue->Wait(J.GenFence, J.GenValue);
        if (CopyToBackBuffer(J, J.Generated))
        {
            if (RdxAntiLag2::Context* const A = AntiLagCtx)
                RdxAntiLag2::SetFrameGenFrameType(A, true);
            GenDone = PresentNow(J, Cost_PresentGen);
            if (bStamping)
                ::QueryPerformanceCounter(&GenAt);
        }
    }
    Queue->Wait(J.RealFence, J.RealValue);
    const UBOOL bReal = CopyToBackBuffer(J, J.Real);
    Queue->Signal(CopyFence.Get(), J.Frame);
    GenThroughAt = 0.0;
    if (GenDone)
    {
        WaitThrough(GenDone);
        const DOUBLE Now = PacerSeconds();
        AddCost(Cost_GenThrough, Now - J.HandedAt);
        if (LastRealAt > 0.0 && Now - LastRealAt < 2.0 * GPaceMaxSec)
            AddCost(Cost_GapRealGen, Now - LastRealAt);
        GenThroughAt = Now;
        DOUBLE Hold = 0.0;
        if (LastGenAt > 0.0)
        {
            const DOUBLE D = Now - LastGenAt;
            if (D > 0.0 && D < 2.0 * GPaceMaxSec)
                IntervalSec = (IntervalSec > 0.0) ? IntervalSec * 0.8 + D * 0.2 : D;
            Hold = IntervalSec * 0.5;
            if (Hold > GPaceMaxSec)
                Hold = GPaceMaxSec;
        }
        LastGenAt = Now;
        // No holding back for Flush. A frame already waiting behind this one is no reason to let the real picture out at
        // once: where the GPU sets the pace the engine is always a frame or two ahead, and the real picture would go
        // out next to the generated one instead of half a frame after it. The hold is half the interval the pictures
        // themselves keep, so it never makes this thread the slower one.
        if (Hold > 0.0 && ::WaitForSingleObject(Hurry, 0) == WAIT_TIMEOUT)
        {
            LARGE_INTEGER Due;
            Due.QuadPart = -(LONGLONG)(Hold * 10000000.0);    // relative, in 100 ns
            if (Timer && ::SetWaitableTimer(Timer, &Due, 0, nullptr, nullptr, FALSE))
            {
                HANDLE Either[2] = { Timer, Hurry };
                ::WaitForMultipleObjects(2, Either, FALSE, (DWORD)(Hold * 1000.0) + 50);
            }
            else
                ::WaitForSingleObject(Hurry, (DWORD)(Hold * 1000.0));
            AddCost(Cost_Hold, PacerSeconds() - Now);
        }
    }
    if (!bReal)
        return;
    // When the real picture went through too, for the gaps the log gives - how evenly the two go out. Waiting for it
    // here delays nothing: the queue presents in order, and the next generated picture cannot pass this one.
    if (RdxAntiLag2::Context* const A = AntiLagCtx)
        RdxAntiLag2::SetFrameGenFrameType(A, false);
    const UINT64 RealDone = PresentNow(J, Cost_PresentReal);
    if (bStamping)
    {
        LARGE_INTEGER RealAt;
        ::QueryPerformanceCounter(&RealAt);
        FStamp& S   = Stamps[StampsWritten % StampRoom];
        S.Frame     = J.Frame;
        S.Generated = GenAt.QuadPart;
        S.Real      = RealAt.QuadPart;
        ::InterlockedIncrement(&StampsWritten);
    }
    WaitThrough(RealDone);
    LastRealAt = PacerSeconds();
    if (GenThroughAt > 0.0)
        AddCost(Cost_GapGenReal, LastRealAt - GenThroughAt);
}

DWORD WINAPI FFramePacer::ThreadMain(void* Self)
{
    ((FFramePacer*)Self)->Run();
    return 0;
}

// The jobs in the order they came. A job's slot in the ring is not written again until the thread has moved past it
// (Submit keeps at most MaxWaiting of them), so it is read in place.
void FFramePacer::Run()
{
    for (;;)
    {
        ::WaitForSingleObject(Wake, 100);
        while (Tail != Head)
        {
            PresentFrame(Ring[Tail % Jobs]);
            ::InterlockedIncrement(&Tail);
            ::SetEvent(Drained);
        }
        if (bQuit)
            return;
    }
}
