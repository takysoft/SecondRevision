// VolumeCheck - the renderer's shader compile, straight through the compiler (the renderer's own keeps a cache).
#pragma once

#include <d3dcompiler.h>

HRESULT RdxCompile(LPCVOID Source, SIZE_T Length, LPCSTR SourceName, const D3D_SHADER_MACRO* Macros,
                   ID3DInclude* Include, LPCSTR Entry, LPCSTR Target, UINT Flags1, UINT Flags2, ID3DBlob** Code,
                   ID3DBlob** Errors);
