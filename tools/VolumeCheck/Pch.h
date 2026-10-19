// VolumeCheck - what the renderer's own Pch.h gives a pass, without the engine.
//
// The renderer's precompiled header pulls in the engine's headers, and with them the game. A pass that touches the
// engine through a handful of small things only - the number types, a few memory calls, the log - can be built by
// itself with those few things given here, and then run with no game at all (VolumeCheck.cpp says why).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdlib.h>
#include <string.h>

// The engine's names for the types the passes use; Windows has the rest (INT, UINT, FLOAT, DOUBLE, BYTE, DWORD, TCHAR).
typedef int              UBOOL;
typedef unsigned __int64 QWORD;
typedef char             ANSICHAR;

inline void appMemzero(void* Dest, size_t Count) { memset(Dest, 0, Count); }
inline void appMemcpy(void* Dest, const void* Src, size_t Count) { memcpy(Dest, Src, Count); }
inline void* appRealloc(void* Was, DWORD Bytes, const TCHAR*) { return realloc(Was, Bytes); }
inline void appFree(void* Was) { free(Was); }

// One message at a time is all the passes ask of it.
inline const TCHAR* appFromAnsi(const ANSICHAR* Text)
{
    static TCHAR Wide[2048];
    int n = 0;
    for (; Text && Text[n] && n < 2047; n++)
        Wide[n] = (TCHAR)(unsigned char)Text[n];
    Wide[n] = 0;
    return Wide;
}

template <class T> inline T Max(const T A, const T B) { return (A > B) ? A : B; }
template <class T> inline T Min(const T A, const T B) { return (A < B) ? A : B; }
template <class T> inline T Abs(const T A) { return (A < (T)0) ? -A : A; }
template <class T> inline T Clamp(const T X, const T Lo, const T Hi) { return (X < Lo) ? Lo : ((X < Hi) ? X : Hi); }

// The renderer's owning handle for COM objects, as its Pch.h has it.
template <class T>
class TComRef
{
public:
    TComRef() : Ptr(nullptr) {}
    ~TComRef() { Release(); }

    TComRef(const TComRef&) = delete;
    TComRef& operator=(const TComRef&) = delete;

    T*  operator->() const { return Ptr; }
    T*  Get()        const { return Ptr; }
    operator bool()  const { return Ptr != nullptr; }

    T**    Slot()     { Release(); return &Ptr; }
    void** VoidSlot() { Release(); return reinterpret_cast<void**>(&Ptr); }

    void Release()
    {
        if (Ptr)
        {
            Ptr->Release();
            Ptr = nullptr;
        }
    }

private:
    T* Ptr;
};
