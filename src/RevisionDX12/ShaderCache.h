#pragma once

#include <d3dcompiler.h>

// D3DCompile, remembered. What the compiler makes is kept - in memory for the rest of the process, and in a file beside
// this DLL for the next start - under a key of everything that decides the result: the source text, the macros, the entry
// point, the target and the flags. The same question then gets the same bytes back without the compiler. The start
// spent most of a second in the compiler, and so did every return from Alt+Tab, where the engine makes the renderer
// anew; a pass made later costs its compile in the middle of the game, as a stall where it is first needed.
//
// The file outlives the build: a new DLL compiles only the shaders whose text it changed. The one exception is a text
// that #includes others through the caller's own handler - those texts live inside the DLL and are not in the key, so
// this DLL's build is. What nobody asks for any more is left out of the file after a month, and the file is held to a
// size that takes every mode's shaders (ShaderCache.cpp's MaxKeptBytes, RdxMath::ShaderCacheWritten).
//
// The file is only ever a shortcut: where it cannot be read or written (a damaged file, no write access anywhere) the
// shaders are compiled as they always were. Where the DLL's folder cannot take it - a game installed where the user may
// not write - it goes to the user's own folder, %LOCALAPPDATA%\SecondRevision, and is read from there too when the DLL's
// folder holds none of use. The arguments and the result are exactly D3DCompile's.
HRESULT RdxCompile(LPCVOID Source, SIZE_T Length, LPCSTR SourceName, const D3D_SHADER_MACRO* Macros, ID3DInclude* Include,
                   LPCSTR Entry, LPCSTR Target, UINT Flags1, UINT Flags2, ID3DBlob** Code, ID3DBlob** Errors);

// Measurement only (the marker SecondRevision-no-shader-cache.txt): the file is neither read nor written, and nothing is
// kept in memory - every shader goes through the compiler, as before the cache. Set before the first compile.
void RdxShaderCacheDisable();
// The start's warm-up is over (FBackend::WarmShaders): every shader compiled from here on is named in the log, as one
// the warm-up should have made.
void RdxShaderWarmDone();
// How many shaders the start read from the cache's file: none where there was no file (a first start, the file
// deleted, the cache off) - then the start compiles every shader the renderer has.
INT  RdxShaderCacheRead();

// Somebody to tell before every shader that goes through the compiler - not one read from the file - on the thread
// that asked for it: the start's caption, which says that shaders are being compiled (FBackend::PresentCompiling).
// Null: nobody. With a Wait as well, the compiler then runs on a thread of its own, and the thread that asked waits in
// Wait until it is done (Compiling: that thread, signalled when it ends) - the start's window answers Windows meanwhile
// (FBackend::WaitCompiling). Without one, or where no thread can be made, the compiler runs on the asking thread.
typedef void (*FRdxCompileWatch)(void* Who);
typedef void (*FRdxCompileWait)(void* Who, HANDLE Compiling);
void RdxCompileWatch(FRdxCompileWatch Watch, void* Who, FRdxCompileWait Wait = nullptr);

// Measurement only (the marker SecondRevision-shader-cache-elsewhere.txt, its text a folder): as if the DLL's folder could
// not be written - the file is read from and written to that folder, the place the user's own folder takes otherwise.
// Set before the first compile.
void RdxShaderCacheElsewhere(const WCHAR* Folder);

// Writes the file when something was compiled since it was last read or written, and the compiler has been quiet for a
// few seconds (a burst of compiles is written once, at its end). Cheap when there is nothing to write - once a frame is
// fine. bNow writes at once, whatever the time (the renderer's end).
void RdxShaderCacheSave(UBOOL bNow);
