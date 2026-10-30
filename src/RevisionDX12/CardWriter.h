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

// A large block of the CPU's memory that may be refused (null), where the engine's allocator would end the game ("Ran
// out of virtual memory"): a large rounded level's lists are tens of megabytes, each in one piece of the 32-bit address
// space. Freed with RdxFreeLarge.
inline void* RdxAllocLarge(SIZE_T Bytes)
{
    return Bytes ? ::VirtualAlloc(nullptr, Bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr;
}
inline void RdxFreeLarge(void* Block)
{
    if (Block)
        ::VirtualFree(Block, 0, MEM_RELEASE);
}
