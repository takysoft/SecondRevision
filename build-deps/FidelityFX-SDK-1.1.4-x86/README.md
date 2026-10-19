# The FidelityFX SDK for a 32-bit build

AMD builds the FidelityFX SDK for x64 only, and Deus Ex is a 32-bit process. The renderer links two 32-bit libraries of
it - `ffx_fsr3upscaler_x86.lib` and `ffx_backend_dx12_x86.lib` - built from the SDK's v1.1.4 source with the three
CMake files in this folder in place of the SDK's own. They are AMD's files (MIT licence, `LICENSE.txt`), changed at
the places marked `RevisionDX12`:

- `toolchain.cmake` - a `DX12_X86` backend that builds for Win32 (the stock branches force x64);
- `CMakeLists.txt` - the `DX12_X86` project, the DX12 backend added for it, and no PIX (it has no 32-bit runtime);
- `src/backends/dx12/CMakeLists.txt` - the frame interpolation swap chain only when frame interpolation is built: its
  sources need a header that only the frame interpolation's shader build makes.

## Steps

1. Get the FidelityFX SDK v1.1.4 source (https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) and unpack it
   to a SHORT path, `C:\ffx` say: under a deep one the generated shader headers' paths overflow Windows' 260
   characters, and the build fails on a header that is there.
2. Copy the three files over the SDK's own, keeping their places.
3. Configure and build, with CMake 3.23 or later and Visual Studio 2026:

       cmake -S C:\ffx -B C:\ffx-b -G "Visual Studio 18 2026" -A Win32 -DFFX_API_BACKEND=DX12_X86 -DFFX_API_DX12=ON -DFFX_FSR3UPSCALER=ON
       cmake --build C:\ffx-b --config Release

4. The two libraries land in the SDK's `bin\ffx_sdk`. Put them in `external\FidelityFX-SDK\lib`, and the SDK's
   `include` folder and `LICENSE.txt` in `external\FidelityFX-SDK`.
