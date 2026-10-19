// DXProbe - instrumentation render device for Deus Ex / Revision (engine 1.112fm)
//
// Purpose: this module renders NOTHING. It implements the URenderDevice contract
// and records how the engine actually drives it, so the real renderer can be written
// against measured facts instead of guesses. See docs/spec-01, docs/spec-02.
//
// Written from the Ion Storm / Epic SDK headers only. No third-party renderer source
// was used as a template - see docs/szabaly-licenc-tisztasag.md.
#pragma once

// The engine headers are built for a 4-byte packed, non-native-wchar_t x86 target.
// These are enforced by the project file (/Zp4, /Zc:wchar_t-); assert them here so a
// misconfigured build fails loudly at compile time rather than mysteriously at runtime.
#if !defined(_WIN32) || defined(_WIN64)
#error DXProbe must be built for 32-bit x86: the game is a 32-bit process.
#endif

#if defined(_NATIVE_WCHAR_T_DEFINED)
#error DXProbe requires /Zc:wchar_t- : the engine's TCHAR handling depends on it.
#endif

// UObject::operator delete is declared inline and can throw; the engine headers trip
// this warning on every translation unit. It is the SDK's design, not our bug.
#pragma warning(disable: 4297)

#include <Engine.h>
#include <UnRender.h>
