// The renderer's buffers in the upload heap, counted by what they are for: each one made carries a small object of ours
// in its private data (SetPrivateDataInterface), which the resource releases when it goes - and that takes its bytes off
// the count. The upload heap is the card's memory in the system's memory, mapped into the 32-bit address space for as
// long as a buffer lives: the memory line at each level's load says by part what holds it (RdxUploadCensus::Say).
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

namespace RdxUploadCensus
{

enum { PARTS = 48 };

struct FPart
{
    const wchar_t*  Name;    // a literal: parts are told apart by the pointer
    volatile LONG64 Bytes;
    volatile LONG   Count;
};

inline FPart* Parts()
{
    static FPart P[PARTS] = {};
    return P;
}
inline volatile LONG& PartsUsed()
{
    static volatile LONG N = 0;
    return N;
}

// The part of that name, made the first time it is asked for; -1 where the table is full.
inline int PartOf(const wchar_t* Name)
{
    FPart* const P = Parts();
    const LONG   N = PartsUsed();
    for (LONG i = 0; i < N && i < PARTS; i++)
        if (P[i].Name == Name)
            return (int)i;
    static SRWLOCK Lock = SRWLOCK_INIT;
    ::AcquireSRWLockExclusive(&Lock);
    int Found = -1;
    for (LONG i = 0; i < PartsUsed() && i < PARTS; i++)
        if (P[i].Name == Name)
            Found = (int)i;
    if (Found < 0 && PartsUsed() < PARTS)
    {
        Found         = (int)PartsUsed();
        P[Found].Name = Name;
        ::InterlockedIncrement(&PartsUsed());
    }
    ::ReleaseSRWLockExclusive(&Lock);
    return Found;
}

// The object a buffer carries: its bytes on the count while it lives. Its memory is the C runtime's, never the engine's
// allocator (which is not thread-safe): the resource lets go of it wherever it is destroyed.
class FWatch : public IUnknown
{
public:
    static void* operator new(size_t Size) noexcept { return malloc(Size); }
    static void  operator delete(void* P) noexcept { free(P); }
    FWatch(int InPart, LONG64 InBytes) : Ref(1), Part(InPart), Bytes(InBytes)
    {
        ::InterlockedExchangeAdd64(&Parts()[Part].Bytes, Bytes);
        ::InterlockedIncrement(&Parts()[Part].Count);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Out) override
    {
        if (!Out)
            return E_POINTER;
        if (Id == __uuidof(IUnknown))
        {
            *Out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *Out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)::InterlockedIncrement(&Ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG Left = ::InterlockedDecrement(&Ref);
        if (Left == 0)
        {
            ::InterlockedExchangeAdd64(&Parts()[Part].Bytes, -Bytes);
            ::InterlockedDecrement(&Parts()[Part].Count);
            delete this;
        }
        return (ULONG)Left;
    }

private:
    volatile LONG Ref;
    int           Part;
    LONG64        Bytes;
};

// {5E3D7A41-2C6B-4F0E-9A57-1D3B8C2E6F90}: the private data's key.
static const GUID WatchKey = { 0x5e3d7a41, 0x2c6b, 0x4f0e, { 0x9a, 0x57, 0x1d, 0x3b, 0x8c, 0x2e, 0x6f, 0x90 } };

// A buffer just made in the upload heap, Bytes long, counted under Name (a literal) until it goes.
inline void Note(ID3D12Resource* Resource, unsigned long long Bytes, const wchar_t* Name)
{
    if (!Resource || !Name)
        return;
    const int Part = PartOf(Name);
    if (Part < 0)
        return;
    FWatch* const W = new FWatch(Part, (LONG64)Bytes);
    if (!W)
        return;
    if (FAILED(Resource->SetPrivateDataInterface(WatchKey, W)))
    {
        W->Release();
        return;
    }
    W->Release();   // the resource's reference is the one left
}

// The parts holding the most, largest first, into Out: "name MB (buffers), ...", and their sum in Total (MB).
inline void Say(wchar_t* Out, int Room, double& TotalMB)
{
    struct FRow
    {
        int    Part;
        LONG64 Bytes;
    };
    FRow      Rows[PARTS];
    int       N = 0;
    LONG64    All = 0;
    FPart* const P = Parts();
    for (LONG i = 0; i < PartsUsed() && i < PARTS; i++)
    {
        // Read whole: in a 32-bit process a plain read of 64 bits is two, and a buffer let go meanwhile on another
        // thread would tear it.
        const LONG64 B = ::InterlockedCompareExchange64(&P[i].Bytes, 0, 0);
        All += B;
        if (B > 0)
            Rows[N++] = { (int)i, B };
    }
    for (int i = 1; i < N; i++)
        for (int j = i; j > 0 && Rows[j].Bytes > Rows[j - 1].Bytes; j--)
        {
            const FRow T = Rows[j];
            Rows[j]      = Rows[j - 1];
            Rows[j - 1]  = T;
        }
    TotalMB = (double)All / (1024.0 * 1024.0);
    int At  = 0;
    Out[0]  = 0;
    for (int r = 0; r < N && At < Room - 64; r++)
    {
        const int Wrote = swprintf_s(Out + At, (size_t)(Room - At), L"%s%s %.1f (%ld)", r ? L", " : L"",
                                     P[Rows[r].Part].Name, (double)Rows[r].Bytes / (1024.0 * 1024.0),
                                     (long)P[Rows[r].Part].Count);
        if (Wrote <= 0)
            break;
        At += Wrote;
    }
}

} // namespace RdxUploadCensus
