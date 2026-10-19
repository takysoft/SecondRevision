// RtCheck: a standalone console tool that asks this machine's graphics driver - from a 32-bit process, as the renderer
// is one - whether the card traces rays in hardware (DirectX Raytracing): the tier each device reports, whether the
// interface the ray tracing calls live on is there, and the highest shader model (inline ray tracing, the RayQuery the
// renderer's passes would use, needs 6.5). It needs no game and nothing installed, and writes RtCheck.log beside itself.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static FILE* GLog = nullptr;

static void Say(const char* Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    vprintf(Format, Args);
    va_end(Args);
    if (GLog)
    {
        va_start(Args, Format);
        vfprintf(GLog, Format, Args);
        va_end(Args);
    }
}

static const char* TierName(D3D12_RAYTRACING_TIER Tier)
{
    switch (Tier)
    {
    case D3D12_RAYTRACING_TIER_NOT_SUPPORTED: return "NOT SUPPORTED";
    case D3D12_RAYTRACING_TIER_1_0:           return "1.0 (ray tracing pipelines)";
    case D3D12_RAYTRACING_TIER_1_1:           return "1.1 (inline ray tracing too)";
    default:                                  return "above 1.1";
    }
}

// The highest shader model the device takes: asked from the top down, as a runtime that does not know a model rejects
// the question instead of answering it with a lower one.
static int HighestShaderModel(ID3D12Device* Device)
{
    for (int Minor = 9; Minor >= 0; Minor--)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL Model;
        Model.HighestShaderModel = (D3D_SHADER_MODEL)(0x60 + Minor);
        if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &Model, sizeof(Model))))
            return (int)Model.HighestShaderModel;
    }
    return 0x51;
}

int main()
{
    char Path[MAX_PATH];
    GetModuleFileNameA(nullptr, Path, MAX_PATH);
    char* Slash = strrchr(Path, '\\');
    if (Slash)
        strcpy(Slash + 1, "RtCheck.log");
    GLog = fopen(Path, "w");
    Say("RtCheck - a %d-bit process\n", (int)(sizeof(void*) * 8));

    IDXGIFactory6* Factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&Factory)))
    {
        Say("no DXGI factory 6 - too old a Windows for this question\n");
        return 1;
    }
    for (UINT i = 0;; i++)
    {
        IDXGIAdapter1* Adapter = nullptr;
        if (Factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                (void**)&Adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 Desc;
        Adapter->GetDesc1(&Desc);
        char Name[256];
        WideCharToMultiByte(CP_UTF8, 0, Desc.Description, -1, Name, sizeof(Name), nullptr, nullptr);
        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            Say("adapter %u: %s - software, skipped\n", i, Name);
            Adapter->Release();
            continue;
        }
        ID3D12Device* Device = nullptr;
        if (FAILED(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&Device)))
        {
            Say("adapter %u: %s - no D3D12 device\n", i, Name);
            Adapter->Release();
            continue;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options5;
        memset(&Options5, 0, sizeof(Options5));
        const HRESULT Asked5 = Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &Options5, sizeof(Options5));
        ID3D12Device5* Device5 = nullptr;
        const HRESULT Has5 = Device->QueryInterface(__uuidof(ID3D12Device5), (void**)&Device5);
        const int Model = HighestShaderModel(Device);
        Say("adapter %u: %s\n", i, Name);
        Say("  ray tracing tier: %s\n", FAILED(Asked5) ? "(the question failed)" : TierName(Options5.RaytracingTier));
        Say("  ID3D12Device5 (where the ray tracing calls are): %s\n", SUCCEEDED(Has5) ? "yes" : "no");
        Say("  highest shader model: %d.%d\n", Model >> 4, Model & 15);
        if (Device5)
            Device5->Release();
        Device->Release();
        Adapter->Release();
    }
    Factory->Release();
    if (GLog)
        fclose(GLog);
    return 0;
}
