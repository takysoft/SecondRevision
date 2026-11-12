// SecondRevision - buffers a level keeps for its whole stay, on the card's own memory (FBackend writes them).
#pragma once

// The shadow maps' copy of a level's triangles and the rays' copy go on the card's own memory (a default heap), not in
// an upload heap: a buffer there takes as much of the process's 32-bit address space as it holds, for as long as it
// lives, and a large rounded level's copies are tens of megabytes each.
class FCardWriter
{
public:
    // A buffer of Bytes on the card's own memory, in the common state - from there a buffer goes by itself to whatever
    // a list uses it for. Null, logged, where it could not be made.
    virtual ID3D12Resource* MakeCardBuffer(UINT64 Bytes, const TCHAR* What) = 0;
    // Bytes of Data into such a buffer from At on: through a small staging buffer, a piece at a time, each piece on a
    // list of its own, sent and waited for - so it may be asked in a frame as well as between frames. False, logged,
    // where it could not.
    virtual UBOOL WriteCardBuffer(ID3D12Resource* Dest, UINT64 At, const void* Data, UINT64 Bytes) = 0;
};

// Whether such a block may be taken (set by the renderer at its start - RenderDevice.cpp's SphereRoomFor, the lists'
// own): one that would leave the process too little of its address space for the engine is refused too. None: the
// system's to give or not.
typedef bool (*FRdxLargeRoomFor)(size_t Bytes);
inline FRdxLargeRoomFor& RdxLargeRoomFor()
{
    static FRdxLargeRoomFor Asked = nullptr;
    return Asked;
}

// A large block of the CPU's memory that may be refused (null), where the engine's allocator would end the game ("Ran
// out of virtual memory"): a large rounded level's lists are tens of megabytes, each in one piece of the 32-bit address
// space. Freed with RdxFreeLarge.
// The bytes the large blocks hold at once, for the log's memory line at each level's load.
inline volatile LONG64& RdxLargeBytesHeld()
{
    static volatile LONG64 Held = 0;
    return Held;
}
inline void* RdxAllocLarge(SIZE_T Bytes)
{
    if (!Bytes || (RdxLargeRoomFor() && !RdxLargeRoomFor()((size_t)Bytes)))
        return nullptr;
    void* const Block = ::VirtualAlloc(nullptr, Bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (Block)
    {
        // Counted as RdxFreeLarge takes it off: the whole pages the block holds, not the bytes asked for - else every
        // block let go takes up to a page more off than it put on, and the count runs below nothing.
        MEMORY_BASIC_INFORMATION Mbi;
        const SIZE_T             Held = (::VirtualQuery(Block, &Mbi, sizeof(Mbi)) == sizeof(Mbi)) ? Mbi.RegionSize : Bytes;
        ::InterlockedExchangeAdd64(&RdxLargeBytesHeld(), (LONG64)Held);
    }
    return Block;
}
inline void RdxFreeLarge(void* Block)
{
    if (!Block)
        return;
    MEMORY_BASIC_INFORMATION Mbi;
    SIZE_T                   Bytes = 0;
    if (::VirtualQuery(Block, &Mbi, sizeof(Mbi)) == sizeof(Mbi))
        Bytes = Mbi.RegionSize;
    ::VirtualFree(Block, 0, MEM_RELEASE);
    ::InterlockedExchangeAdd64(&RdxLargeBytesHeld(), -(LONG64)Bytes);
}
