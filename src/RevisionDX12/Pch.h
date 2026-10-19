// SecondRevision - a Direct3D 12 render device for Deus Ex: Revision (engine 1.112fm).
//
// Written from the Ion Storm / Epic SDK headers and from measurements taken with our
// own DXProbe instrument (docs/spec-01..04, docs/meres-01). No third-party renderer
// source is used as a template - see docs/szabaly-licenc-tisztasag.md.
#pragma once

#if !defined(_WIN32) || defined(_WIN64)
#error SecondRevision must be built for 32-bit x86: the game is a 32-bit process.
#endif

#if defined(_NATIVE_WCHAR_T_DEFINED)
#error SecondRevision requires /Zc:wchar_t- : the engine's TCHAR handling depends on it.
#endif

// ---------------------------------------------------------------------------
// Structure packing: the one thing that must be got right before anything else
// ---------------------------------------------------------------------------
//
// The engine's headers and its Core.lib / Engine.lib were built with 4-byte packing,
// so our declarations of its structs must match or every field lands at the wrong
// offset. The Windows and Direct3D headers, in contrast, REQUIRE default packing and
// say so with a static assert.
//
// Setting /Zp4 for the whole translation unit satisfies the first and breaks the
// second. So we do not set it globally: the system headers are included first, with
// default packing, and only the engine headers are wrapped in an explicit pack region.
// Order matters here - anything the engine headers pull in for the first time would
// otherwise be parsed inside the packed region.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
// 1.6 and not 1.5: IDXGIOutput6 and DXGI_OUTPUT_DESC1 are what say whether the display the window is on is in
// HDR mode (FBackend::ReadDisplayHdr). It is a superset of 1.5, so nothing else changes.
#include <dxgi1_6.h>
// For the process's memory use, logged at every level load. It belongs among the system headers: it has to be
// compiled with the default packing, like the rest of them.
#include <psapi.h>

// UObject::operator delete is declared inline and can throw. That is the SDK's design,
// not our bug, and it fires on every translation unit that includes Engine.h.
#pragma warning(disable: 4297)

#pragma pack(push, 4)
#include <Engine.h>
#include <UnRender.h>
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Minimal owning handle for COM objects
// ---------------------------------------------------------------------------
// The engine's containers predate modern C++ and we do not want to drag standard
// library semantics into its packed structs, so this is deliberately tiny.
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

    // Address of the slot, for the many D3D12 calls that fill an out-parameter.
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
