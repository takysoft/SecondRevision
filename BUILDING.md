# Building

The renderer is a 32-bit DLL for the Unreal Engine 1 games: it implements `URenderDevice` on top of Direct3D 12.
Beside it runs a 64-bit companion app, `SecondRevisionHost64`, for what has no 32-bit build: FSR 4, DLSS, XeSS, frame
generation and the ray tracing.

## Requirements

Each part below has its own licence (`THIRD-PARTY-NOTICES.md`). What those licences let us pass on is in this
repository, in `build-deps\`: copy its folders into `external\` and the build finds them. The rest - the Deus Ex
SDK, the DLSS SDK and the XeSS SDK - is fetched by whoever builds.

### Tools

| what | version | for |
|---|---|---|
| Visual Studio 2026 with the C++ desktop workload (the Build Tools are enough) | toolset v145 | everything |
| Windows SDK | 10.0 - the newest installed | everything |
| CMake | 3.23 or later | only to build the two 32-bit FidelityFX libraries again (below) |
| Python | 3.12, with `numpy` and `Pillow` | the scripts: the trees' assets, the picture checks |

The scripts look for MSBuild under `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` first;
`run-dx12.ps1`, `test.cmd` and `host64-probe.cmd` then ask the installer's `vswhere.exe`, so any edition does.

### The game

Deus Ex, from any shop; the Revision mod is optional. The scripts find the game in Windows' list of installed
programs, then in Steam's libraries. Where they cannot, the first line of a `game-path.txt` in the project's root
names the folder that holds `Revision\System\Revision.exe` (`python scripts\game_path.py` says what was found). The
renderer itself looks for nothing: it sits beside the game's exe.

### The Deus Ex SDK

The engine's headers and import libraries. Their files carry `Copyright 1997-1999 Epic Games, Inc. All Rights
Reserved.` and no licence that would let this project pass them on. Ion Storm released the SDK for mod making, and
renderer packages for the game bundle the same headers. Put it in place with

    .\scripts\setup-sdk.ps1 -Zip <path to the zip>

which unpacks what the build needs into `external\Games\DeusEx` and says what is missing. Without `-Zip` it only
checks what is already there.

### The vendor SDKs

Each in its own folder under `external\`, holding the files the projects read. The first five are in `build-deps\`
already, with their licences; the last two are not theirs to pass on, so they are fetched:

| folder | what | version | where from | the files the build reads |
|---|---|---|---|---|
| `external\FidelityFX-SDK` | AMD FidelityFX SDK - the FSR 3.1 upscaler, in the renderer | 1.1.4 (FSR 3.1.4) | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK | `include\FidelityFX\...`, `LICENSE.txt`, and `lib\ffx_fsr3upscaler_x86.lib`, `lib\ffx_backend_dx12_x86.lib` - built 32-bit, see below |
| `external\FidelityFX-SDK-2.3` | AMD FSR SDK - FSR 4 and its frame generation, in the host | 2.3.0 | https://gpuopen.com/amd-fidelityfx-sdk/ | `Kits\FidelityFX\api\include`, `Kits\FidelityFX\upscalers\include`, `Kits\FidelityFX\framegeneration\include`, and from `Kits\FidelityFX\signedbin` `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll` |
| `external\AGS_SDK` | AMD GPU Services - the display's VRR, in the renderer | 6.3 | https://github.com/GPUOpen-LibrariesAndSDKs/AGS_SDK | `ags_lib\inc`, `ags_lib\lib\amd_ags_x86_2026_MD.lib` (`_MDd` for a Debug build, `_MT` for `tools\VrrCheck`), `LICENSE.txt` |
| `external\NVAPI` | NVIDIA NVAPI - Reflex and the display's VRR, in the renderer | a current release | https://github.com/NVIDIA/nvapi | the headers (`nvapi.h` and the others in the release's root), `x86\nvapi.lib`, `License.txt` |
| `external\IGCL` | Intel Graphics Control Library - the display's VRR, in the renderer | 1.x, headers only | https://github.com/intel/drivers.gpu.control-library | `include\igcl_api.h`, `LICENSE` |
| `external\DLSS-310.9.1` | NVIDIA DLSS SDK - DLSS and its frame generation, in the host | 310.9.1 | https://github.com/NVIDIA/DLSS | `include`, `lib\Windows_x86_64\x64\nvsdk_ngx_s.lib`, `lib\Windows_x86_64\rel\nvngx_dlss.dll`, `lib\Windows_x86_64\rel\nvngx_dlssg.dll`, `LICENSE.txt` |
| `external\XeSS-SDK\3.0.2` | Intel XeSS SDK - XeSS, in the host | 3.0.2 | https://github.com/intel/xess | `inc`, `bin\libxess.dll`, `LICENSE.txt`, `third-party-programs.txt` |

The DLSS SDK's licence lets its parts be passed on only built into an application, and the XeSS SDK's only its
binary, unchanged - so those two come from NVIDIA and Intel. AMD's Anti-Lag 2 header is part of the sources already
(`src\RevisionDX12\AntiLag2.h`, MIT).

### What is not in `build-deps\`, and where to get it

| folder | what | where to get it | how it goes in |
|---|---|---|---|
| `external\DLSS-310.9.1` | NVIDIA DLSS SDK 310.9.1 | https://github.com/NVIDIA/DLSS - the repository's 310.9.1 release | `include`, `lib` and `LICENSE.txt` directly in the folder |
| `external\XeSS-SDK\3.0.2` | Intel XeSS SDK 3.0.2 | https://github.com/intel/xess - the repository's releases, `XeSS_SDK_3.0.2.zip` | the zip unpacked into the folder: `inc`, `bin`, `LICENSE.txt` and `third-party-programs.txt` directly in it |

The Deus Ex SDK, the third one fetched, is described above.

### The two 32-bit FidelityFX libraries

AMD builds the FidelityFX SDK for x64 only, and the game is a 32-bit process. The two libraries the renderer links are
built from the SDK's v1.1.4 source with three of its CMake files changed. They are in `build-deps\FidelityFX-SDK\lib`
ready to use; to build them again, the changed files, AMD's licence and the steps are in
`build-deps\FidelityFX-SDK-1.1.4-x86`. In short, from a short path:

    cmake -S C:\ffx -B C:\ffx-b -G "Visual Studio 18 2026" -A Win32 -DFFX_API_BACKEND=DX12_X86 -DFFX_API_DX12=ON -DFFX_FSR3UPSCALER=ON
    cmake --build C:\ffx-b --config Release

## Steps

1. Copy the folders of `build-deps\` into `external\`, and put the Deus Ex SDK, the DLSS SDK and the XeSS SDK
   in place (above).
2. Build and install into the game:

       .\scripts\run-dx12.ps1 -Defines '' -NoRun

   This builds `build\Release\SecondRevision.dll` and `SecondRevisionHost64.exe`, copies them into the game's `System`
   folder with what they need (the table below), and points the game's ini at the renderer. Leave off `-NoRun` to start
   the game afterwards.
3. Run the unit tests (no game needed):

       .\scripts\test.cmd

## What ends up in the game folder

| file | what it is |
|---|---|
| `SecondRevision.dll` | the renderer |
| `SecondRevision.int` | the name the game's settings screen shows |
| `SecondRevision.settings.ini` | written by the renderer on its first start, with every setting explained |
| `SecondRevision\Assets\` | what the renderer reads: the trees' leaves and bark, the lamps' and wet floors' lists |
| `SecondRevision\Host64\` | the 64-bit companion app, with AMD's, NVIDIA's and Intel's upscaler DLLs |
| `SecondRevision\Legal\` | the licences this renderer and the DLLs beside it are used under |

The shader cache (`SecondRevision.shadercache`) and the logs are written beside the DLL while the game runs.

The renderer never writes the game's own ini files.

## A note on `external/`

That directory holds what the build reads and reference material - other renderers' sources, the SDKs. No file from
it belongs in a release.
