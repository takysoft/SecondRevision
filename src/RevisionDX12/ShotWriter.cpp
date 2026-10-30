#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include "ShotWriter.h"

#pragma comment(lib, "ole32.lib")

// WIC's names for its factory, the PNG container and the 24-bit blue-green-red pixel - written out here, so that no
// library has to bring them.
static const GUID ShotFactoryClass = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID ShotPngContainer = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID ShotBgr24        = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };

// A writer: free, writing, or done - well or not - and not yet asked about. Four of them: a pair takes two, and a pair
// asked for again before the first one is written takes two more.
enum { ShotFree = 0, ShotWriting = 1, ShotWritten = 2, ShotFailed = 3 };
struct FShotJob
{
    volatile LONG  State;
    HANDLE         Thread;
    unsigned char* Pixels;
    int            W, H;
    wchar_t        Path[MAX_PATH];
};
static FShotJob GShotJobs[4];

void* RdxShotAlloc(int W, int H)
{
    if (W <= 0 || H <= 0 || W > 16384 || H > 16384)
        return nullptr;
    return ::HeapAlloc(::GetProcessHeap(), 0, (SIZE_T)W * (SIZE_T)H * 4);
}

void RdxShotFree(void* Pixels)
{
    if (Pixels)
        ::HeapFree(::GetProcessHeap(), 0, Pixels);
}

// The picture into its file. The coder takes three bytes a pixel, so the four are squeezed to three in place first:
// every byte moves down, never up, so none is written over before it has been read.
static bool WritePng(FShotJob& J)
{
    const SIZE_T Count = (SIZE_T)J.W * (SIZE_T)J.H;
    for (SIZE_T i = 0; i < Count; i++)
    {
        J.Pixels[i * 3 + 0] = J.Pixels[i * 4 + 0];
        J.Pixels[i * 3 + 1] = J.Pixels[i * 4 + 1];
        J.Pixels[i * 3 + 2] = J.Pixels[i * 4 + 2];
    }
    const UINT             Stride  = (UINT)J.W * 3;
    IWICImagingFactory*    Factory = nullptr;
    IWICStream*            Stream  = nullptr;
    IWICBitmapEncoder*     Coder   = nullptr;
    IWICBitmapFrameEncode* Frame   = nullptr;
    WICPixelFormatGUID     Format  = ShotBgr24;
    const bool bOk = SUCCEEDED(::CoCreateInstance(ShotFactoryClass, nullptr, CLSCTX_INPROC_SERVER,
                                                  __uuidof(IWICImagingFactory), (void**)&Factory)) &&
                     SUCCEEDED(Factory->CreateStream(&Stream)) &&
                     SUCCEEDED(Stream->InitializeFromFilename(J.Path, GENERIC_WRITE)) &&
                     SUCCEEDED(Factory->CreateEncoder(ShotPngContainer, nullptr, &Coder)) &&
                     SUCCEEDED(Coder->Initialize(Stream, WICBitmapEncoderNoCache)) &&
                     SUCCEEDED(Coder->CreateNewFrame(&Frame, nullptr)) &&
                     SUCCEEDED(Frame->Initialize(nullptr)) &&
                     SUCCEEDED(Frame->SetSize((UINT)J.W, (UINT)J.H)) &&
                     SUCCEEDED(Frame->SetPixelFormat(&Format)) && ::IsEqualGUID(Format, ShotBgr24) &&
                     SUCCEEDED(Frame->WritePixels((UINT)J.H, Stride, Stride * (UINT)J.H, J.Pixels)) &&
                     SUCCEEDED(Frame->Commit()) &&
                     SUCCEEDED(Coder->Commit());
    if (Frame)
        Frame->Release();
    if (Coder)
        Coder->Release();
    if (Stream)
        Stream->Release();
    if (Factory)
        Factory->Release();
    // A file the coder gave up on half way is not left behind to look like a picture.
    if (!bOk)
        ::DeleteFileW(J.Path);
    return bOk;
}

static DWORD WINAPI ShotThread(LPVOID Arg)
{
    FShotJob& J = *(FShotJob*)Arg;
    // The coder in this thread's own apartment, so the game thread's is left as the game made it.
    const HRESULT Com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool    bOk = SUCCEEDED(Com) && WritePng(J);
    if (SUCCEEDED(Com))
        ::CoUninitialize();
    RdxShotFree(J.Pixels);
    J.Pixels = nullptr;
    ::InterlockedExchange(&J.State, bOk ? ShotWritten : ShotFailed);
    return 0;
}

int RdxShotWriteStart(void* Pixels, int W, int H, const wchar_t* Path)
{
    if (Pixels && Path && W > 0 && H > 0)
        for (FShotJob& J : GShotJobs)
        {
            if (J.State != ShotFree)
                continue;
            J.Pixels = (unsigned char*)Pixels;
            J.W      = W;
            J.H      = H;
            ::lstrcpynW(J.Path, Path, MAX_PATH);
            J.State  = ShotWriting;
            J.Thread = ::CreateThread(nullptr, 0, ShotThread, &J, 0, nullptr);
            if (J.Thread)
                return 1;
            J.Pixels = nullptr;
            J.State  = ShotFree;
            break;
        }
    RdxShotFree(Pixels);
    return 0;
}

int RdxShotWriteDone(wchar_t* Path, int PathChars, int* bOk)
{
    for (FShotJob& J : GShotJobs)
    {
        // A plain read, fenced: the state is the writer thread's to change.
        const LONG S = ::InterlockedCompareExchange(&J.State, ShotFree, ShotFree);
        if (S != ShotWritten && S != ShotFailed)
            continue;
        // Its thread is past its last line: waiting for it to end takes no time, and its handle can go.
        ::WaitForSingleObject(J.Thread, INFINITE);
        ::CloseHandle(J.Thread);
        J.Thread = nullptr;
        if (Path && PathChars > 0)
            ::lstrcpynW(Path, J.Path, PathChars);
        if (bOk)
            *bOk = (S == ShotWritten) ? 1 : 0;
        ::InterlockedExchange(&J.State, ShotFree);
        return 1;
    }
    return 0;
}

void RdxShotWriteWait()
{
    for (FShotJob& J : GShotJobs)
        if (J.Thread)
            ::WaitForSingleObject(J.Thread, 10000);
}
