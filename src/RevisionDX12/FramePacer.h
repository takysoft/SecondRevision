// SecondRevision - two pictures a frame on the screen, for frame generation (docs/terv-fsr4.md, M4).
//
// With frame generation every frame the engine draws goes out as two pictures: the one generated between the last frame
// and this, then this frame's own, half a frame later. A thread of this class's own presents both, from a queue of its
// own; the engine's thread only hands a frame's pictures over (Submit) and goes on. It has to be a thread of its own:
// DXGI's Present waits until a back buffer is free, and with two presents a frame, the second held back until its time,
// there often is none - measured on the engine's thread, 10 to 18 ms in each Present, and the engine at 25 frames a
// second under a limit of 28. The rendering queue never touches a back buffer either: it draws the finished picture into
// a texture, and this queue copies the pictures into the back buffers - both at once, the real one into the buffer after
// the generated one's. The thread keeps the half frame too: it waits for the generated picture's present to go through,
// then half the frames' interval more, and only then presents the real one.
//
// Without frame generation none of this exists: the swap chain is on the rendering queue and presents as it always has.
#pragma once

#include "AntiLag2.h"

class FFramePacer
{
public:
    FFramePacer();
    ~FFramePacer();

    // The queue, its fences and command lists, and the thread. False: frame generation cannot present.
    UBOOL Init(ID3D12Device* Device);
    // Everything handed over presented, the thread stopped and everything let go. Safe to call twice.
    void  Shutdown();
    UBOOL IsUp() const { return Queue ? 1 : 0; }
    ID3D12CommandQueue* GetQueue() const { return Queue.Get(); }

    // Waits until every frame handed over is presented and the queue is idle; the thread holds nothing back meanwhile.
    // Before anything is done to the swap chain or its back buffers (a resize, a new chain, the fullscreen state, a test
    // present) or to the pictures handed over: while frames are in flight, the thread is the swap chain's only user.
    void  Flush();

    // One frame's pictures, handed to the thread. Nothing here waits for the GPU or for DXGI - only for the thread, when
    // two frames are waiting for it already, and then a tenth of a second at most before this frame is left out.
    //   Generated: the picture generated before this frame (null: none), ready once GenFence reaches GenValue - the
    //              thread's queue waits for that.
    //   Real:      this frame's finished picture, ready once RealFence reaches RealValue.
    // Both stay in COMMON. Sync and Flags are the Present's. Returns the latest status other than S_OK a Present of the
    // thread's gave since the last call (S_OK: none) - an occluded window or a lost device is found out a frame late.
    HRESULT Submit(IDXGISwapChain3* Chain, ID3D12Resource* const* BackBuffers, UINT BufferCount,
                   ID3D12Resource* Generated, ID3D12Fence* GenFence, UINT64 GenValue,
                   ID3D12Resource* Real, ID3D12Fence* RealFence, UINT64 RealValue, UINT Sync, UINT Flags);

    // The fence the queue signals with a frame's number once both of its pictures are copied out of the textures they
    // came in (into back buffers) - before the hold, which only holds back the real picture's Present. RealReleased is
    // the last frame handed over: the rendering queue waits for it before it draws into the finished picture again, and
    // so before the host makes the next generated picture into the texture the last one was copied from.
    ID3D12Fence* GetCopyFence() const { return CopyFence.Get(); }
    UINT64       RealReleased() const { return Submitted; }

    // What the pacing measured: the interval between two generated pictures going out, in seconds (0: not yet).
    DOUBLE       Interval() const { return IntervalSec; }

    // What presenting cost, for the log. The engine's: waiting in Submit for the thread. The thread's: waiting for a free
    // command list, the generated picture's Present, the real picture's, how long a generated picture took to go through
    // after it was handed over, and the hold after it. And how evenly the pictures go out: from a generated picture going
    // through to its real one, and from a real one to the next generated one - the two are equal where the pacing works.
    // Each in seconds, summed, with its longest and how many. TakeCosts hands them over and starts them again; an entry
    // the thread adds meanwhile may be lost, which a log can bear.
    enum ECost { Cost_Submit, Cost_ListWait, Cost_PresentGen, Cost_PresentReal, Cost_GenThrough, Cost_Hold,
                 Cost_GapGenReal, Cost_GapRealGen, Cost_Count };
    struct FCost { DOUBLE Sum; DOUBLE Max; INT Count; };
    void         TakeCosts(FCost* Out);
    // Frames left out because the thread was that far behind, since the last call.
    INT          TakeDropped() { return (INT)::InterlockedExchange(&Dropped, 0); }

    // The moments a frame's pictures were handed to DXGI - QueryPerformanceCounter, as each Present returned; Generated
    // 0 where the frame had none - for the measurement of the input's way to the screen (the marker
    // SecondRevision-input-latency.txt). The thread writes them while stamping is on; TakeStamp hands them over in
    // order, and past StampRoom unread the oldest are lost.
    struct FStamp { UINT64 Frame; LONGLONG Generated; LONGLONG Real; };
    void         SetStamping(UBOOL bOn) { bStamping = bOn ? 1 : 0; }
    UBOOL        TakeStamp(FStamp& Out);

    // Anti-Lag 2 while it is on (null: off): each picture's kind told to it just before its Present, as AMD asks of a
    // frame generation's presenting thread.
    void         SetAntiLag(RdxAntiLag2::Context* Ctx) { AntiLagCtx = Ctx; }

private:
    enum { Slots = 8 };        // command lists in flight: two a frame, four frames' worth
    enum { Jobs = 4 };         // the frames handed over, waiting for the thread
    enum { MaxWaiting = 2 };   // ... of which at most this many at once
    enum { StampRoom = 64 };   // the stamps not yet taken (TakeStamp)

    FStamp                             Stamps[StampRoom];
    volatile LONG                      StampsWritten = 0;   // the thread's count
    LONG                               StampsTaken   = 0;   // the taker's
    volatile LONG                      bStamping     = 0;
    RdxAntiLag2::Context* volatile     AntiLagCtx    = nullptr;

    struct FJob
    {
        IDXGISwapChain3*       Chain;
        ID3D12Resource* const* BackBuffers;
        UINT                   BufferCount;
        ID3D12Resource*        Generated;
        ID3D12Fence*           GenFence;
        UINT64                 GenValue;
        ID3D12Resource*        Real;
        ID3D12Fence*           RealFence;
        UINT64                 RealValue;
        UINT                   Sync;
        UINT                   Flags;
        UINT64                 Frame;          // its number: see GetCopyFence
        DOUBLE                 HandedAt;       // when Submit took it (PacerSeconds)
    };

    void   AddCost(INT Which, DOUBLE Seconds);
    ID3D12GraphicsCommandList* NextList();
    UINT64 Signal();
    // One frame's two pictures, copied and presented, the hold between them kept. The thread's.
    void   PresentFrame(const FJob& J);
    // A picture copied into the back buffer the chain presents next. False: there is none to copy into.
    UBOOL  CopyToBackBuffer(const FJob& J, ID3D12Resource* Picture);
    // The chain's Present, timed as Cost; returns the queue's fence value once it has gone through.
    UINT64 PresentNow(const FJob& J, INT Cost);
    // Until the queue has reached Value (0: nothing), half a second at most.
    void   WaitThrough(UINT64 Value);
    void   NoteResult(HRESULT Hr);
    static DWORD WINAPI ThreadMain(void* Self);
    void   Run();

    TComRef<ID3D12Device>              Device;
    TComRef<ID3D12CommandQueue>        Queue;
    TComRef<ID3D12Fence>               Fence;        // this queue's progress
    UINT64                             FenceValue;
    TComRef<ID3D12Fence>               CopyFence;    // see GetCopyFence
    UINT64                             Submitted;    // the last frame handed over (its number), the engine's thread's
    TComRef<ID3D12CommandAllocator>    Allocators[Slots];
    TComRef<ID3D12GraphicsCommandList> Lists[Slots];
    UINT64                             SlotDone[Slots];   // the fence value the slot's last list was done at
    INT                                NextSlot;
    HANDLE                             ListEvent;         // the thread's waits for a slot
    HANDLE                             FlushEvent;        // Flush's own wait for the queue

    // The thread and its jobs (Submit writes Head, the thread Tail).
    HANDLE                             Thread;
    HANDLE                             Wake;              // a job handed over, or the thread asked to stop
    HANDLE                             Drained;           // the thread finished a job
    HANDLE                             Hurry;             // manual reset: Flush wants everything out, no holding back
    HANDLE                             ThreadEvent;       // the thread's own fence waits
    HANDLE                             Timer;             // a high-resolution waitable timer, where Windows has one
    volatile LONG                      bQuit;
    volatile LONG                      Head, Tail;
    volatile LONG                      Result;            // see Submit
    volatile LONG                      Dropped;           // see TakeDropped
    FJob                               Ring[Jobs];
    DOUBLE                             LastGenAt;         // the thread's: when the last generated picture went out
    DOUBLE                             GenThroughAt;      // ... this frame's generated picture (0: it had none)
    DOUBLE                             LastRealAt;        // ... and the last real picture
    DOUBLE                             IntervalSec;
    FCost                              Costs[Cost_Count]; // see TakeCosts
};
