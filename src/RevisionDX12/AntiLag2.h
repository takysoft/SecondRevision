// SecondRevision - AMD Radeon Anti-Lag 2 for a process of either width.
//
// AMD's own header (external/AntiLag2-SDK/ffx_antilag2_dx12.h, MIT licence, the notice below) is written for 64-bit
// games: it asks the driver's 64-bit extension (amdxc64.dll) for the interface and holds the version 1 state block to
// its 64-bit size. The game this renderer draws for is a 32-bit process, where the same driver's extension is
// amdxc32.dll - it exports the same AmdExtD3DCreateInterface - and the block is a pointer's four bytes smaller. This
// is that header with those two things taken from the process's own width; everything else - the interface, the
// blocks, the calls and their order - is AMD's.
//
// What it does: Update, called once a frame just before the game reads its input, lets the driver hold the game's
// thread back until the GPU is nearly through the frames before - so the input is read as late as it can be and the
// frame made from it does not wait behind others. Where the driver has no Anti-Lag 2 (another card, an older driver,
// a 32-bit driver without it) Initialize says so and every call does nothing.
//
// ---------------------------------------------------------------------------------------------------------------------
// Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ---------------------------------------------------------------------------------------------------------------------

#pragma once

#include <d3d12.h>

namespace RdxAntiLag2
{
    // The driver's Anti-Lag interface.
    MIDL_INTERFACE("44085fbe-e839-40c5-bf38-0ebc5ab4d0a6")
    IAmdExtAntiLagApi : public IUnknown
    {
    public:
        virtual HRESULT UpdateAntiLagState(VOID* pData) = 0;
    };

    // One per device, zeroed before Initialize. What the driver answered, step by step, kept for the log; and whether
    // it took the version 1 block in the 64-bit layout (m_wide) rather than this process's own.
    struct Context
    {
        IAmdExtAntiLagApi* m_pAntiLagAPI = nullptr;
        bool               m_enabled     = false;
        unsigned int       m_maxFPS      = 0;
        HRESULT            m_createHr    = E_FAIL;
        HRESULT            m_stateHr     = E_FAIL;
        HRESULT            m_wideHr      = E_FAIL;
        bool               m_wide        = false;
    };

    // The state block, version 1: 32 bytes in a 64-bit process, 24 in a 32-bit one (the control string's pointer).
    struct APIData_v1
    {
        unsigned int uiSize;
        unsigned int uiVersion;
        unsigned int eMode;
        const char*  sControlStr;
        unsigned int uiControlStrLength;
        unsigned int maxFPS;
    };
    static_assert(sizeof(APIData_v1) == ((sizeof(void*) == 8) ? 32 : 24), "the version 1 block: 32 bytes in 64 bits, 24 in 32");

    // The same block laid out as a 64-bit process lays it out, for a 32-bit driver that wants that one.
    struct APIData_v1_Wide
    {
        unsigned int     uiSize;
        unsigned int     uiVersion;
        unsigned int     eMode;
        unsigned int     uiPad;
        unsigned __int64 sControlStr;
        unsigned int     uiControlStrLength;
        unsigned int     maxFPS;
    };
    static_assert(sizeof(APIData_v1_Wide) == 32, "the version 1 block, the 64-bit layout");

    // The version 1 block sent in the layout the driver took: eMode 1 on, 2 off.
    inline HRESULT SendState(Context* context, unsigned int eMode, unsigned int maxFPS)
    {
        if (context->m_wide)
        {
            APIData_v1_Wide data = {};
            data.uiSize          = sizeof(data);
            data.uiVersion       = 1;
            data.eMode           = eMode;
            data.maxFPS          = maxFPS;
            return context->m_pAntiLagAPI->UpdateAntiLagState(&data);
        }
        APIData_v1 data         = {};
        data.uiSize             = sizeof(data);
        data.uiVersion          = 1;
        data.eMode              = eMode;
        data.sControlStr        = nullptr;
        data.uiControlStrLength = 0;
        data.maxFPS             = maxFPS;
        return context->m_pAntiLagAPI->UpdateAntiLagState(&data);
    }

    // Version 2: the frame generation's signals; the same 176 bytes in either width.
    struct APIData_v2
    {
        unsigned int uiSize;
        unsigned int uiVersion;
        struct Flags
        {
            unsigned int unused0               : 1;
            unsigned int unused1               : 1;
            unsigned int signalFgFrameType     : 1;
            unsigned int isInterpolatedFrame   : 1;
            unsigned int signalGetUserInputIdx : 1;
            unsigned int signalEndOfFrameIdx   : 1;
            unsigned int reserved              : 26;
        } flags;
        unsigned __int64 iiFrameIdx;
        unsigned __int64 uiiReserved[19];
    };
    static_assert(sizeof(APIData_v2) == 176, "the version 2 block");

    inline ULONG DeInitialize(Context* context)
    {
        ULONG refCount = 0;
        if (context)
        {
            if (context->m_pAntiLagAPI)
            {
                refCount               = context->m_pAntiLagAPI->Release();
                context->m_pAntiLagAPI = nullptr;
            }
            context->m_enabled = false;
        }
        return refCount;
    }

    // Once, with the game's device: S_OK where the driver has Anti-Lag 2.
    inline HRESULT Initialize(Context* context, ID3D12Device* device)
    {
        HRESULT hr = E_INVALIDARG;
        if (context && device && context->m_pAntiLagAPI == nullptr)
        {
            HMODULE hModule = GetModuleHandleA(sizeof(void*) == 8 ? "amdxc64.dll" : "amdxc32.dll");
            if (hModule)
            {
                typedef HRESULT(__cdecl * PFNAmdExtD3DCreateInterface)(IUnknown* pOuter, REFIID riid, void** ppvObject);
                PFNAmdExtD3DCreateInterface AmdExtD3DCreateInterface =
                    reinterpret_cast<PFNAmdExtD3DCreateInterface>(GetProcAddress(hModule, "AmdExtD3DCreateInterface"));
                if (AmdExtD3DCreateInterface)
                {
                    hr = AmdExtD3DCreateInterface(device, __uuidof(IAmdExtAntiLagApi), (void**)&context->m_pAntiLagAPI);
                    context->m_createHr = hr;
                    if (hr == S_OK && context->m_pAntiLagAPI)
                    {
                        // Off while it starts - in this process's own layout first, then in the 64-bit one.
                        hr                 = SendState(context, 2, 0);
                        context->m_stateHr = hr;
                        if (hr != S_OK && sizeof(void*) == 4)
                        {
                            context->m_wide   = true;
                            hr                = SendState(context, 2, 0);
                            context->m_wideHr = hr;
                            if (hr != S_OK)
                                context->m_wide = false;
                        }
                    }
                    if (hr != S_OK)
                        DeInitialize(context);
                }
                else
                    hr = E_NOINTERFACE;
            }
            else
                hr = E_HANDLE;
        }
        return hr;
    }

    // Once a frame, just before the game reads its input: the driver's delay, where it is on.
    inline HRESULT Update(Context* context, bool enabled, unsigned int maxFPS)
    {
        if (!context || !context->m_pAntiLagAPI)
            return E_NOINTERFACE;
        if (context->m_enabled != enabled || context->m_maxFPS != maxFPS)
        {
            context->m_enabled = enabled;
            context->m_maxFPS  = maxFPS;
            SendState(context, enabled ? 1 : 2, maxFPS);
        }
        const HRESULT hr = context->m_pAntiLagAPI->UpdateAntiLagState(nullptr);
        return (hr == S_OK || hr == S_FALSE) ? S_OK : hr;
    }

    inline HRESULT SetFrameGenParamsInternal(Context* context, APIData_v2::Flags flags)
    {
        if (!context || !context->m_pAntiLagAPI)
            return E_NOINTERFACE;
        APIData_v2 data = {};
        data.uiSize     = sizeof(data);
        data.uiVersion  = 2;
        data.flags      = flags;
        data.iiFrameIdx = 0;
        return context->m_pAntiLagAPI->UpdateAntiLagState(&data);
    }

    // On the rendering thread, once a frame's work is submitted and before it is presented.
    inline HRESULT MarkEndOfFrameRendering(Context* context)
    {
        APIData_v2::Flags flags   = {};
        flags.signalEndOfFrameIdx = 1;
        return SetFrameGenParamsInternal(context, flags);
    }

    // On the presenting thread, just before each Present: whether the picture is a generated one.
    inline HRESULT SetFrameGenFrameType(Context* context, bool bInterpolatedFrame)
    {
        APIData_v2::Flags flags   = {};
        flags.signalFgFrameType   = 1;
        flags.isInterpolatedFrame = bInterpolatedFrame ? 1 : 0;
        return SetFrameGenParamsInternal(context, flags);
    }
}
