// SecondRevision - NVIDIA Reflex's low latency mode, through NVAPI: the AntiLag key on NVIDIA's cards (AMD's take
// Anti-Lag 2, AntiLag2.h). The renderer and tools/ReflexProbe use this very code, so the probe - run on a machine with
// an NVIDIA card - tries exactly what the game does, and writes down what the driver answered.
//
// What Reflex asks of a game, frame by frame: Sleep at the very start of the frame, before the input is read - the
// driver waits there so that the CPU does not run ahead of the GPU - and the frame's stages marked with the frame's
// number: the simulation's start and end, the rendering's submission, the Present. From the marks the driver also
// measures each frame itself (Latency): from the simulation's start to the GPU's end, the queues between. Both every
// frame, with the low latency mode off too, as NVIDIA asks: the sleep is then where a frame rate limit set in its
// control panel waits, and the marks are its measure.
//
// Every call is NVAPI's own; where there is no NVIDIA driver, or it does not take the sleep mode on this device (an
// older driver, perhaps a 32-bit game), Start says so and nothing else does anything.
#pragma once

#include <d3d12.h>
#include "nvapi.h"

namespace RdxReflex
{
    struct FState
    {
        IUnknown*    Device      = nullptr;       // the device the sleep and the marks are for
        bool         bNvapi      = false;         // NVAPI started (and so unloaded at Stop)
        bool         bUp         = false;         // the driver took the sleep mode on this device: Reflex runs here
        bool         bOn         = false;         // the low latency mode is on
        NvU64        Frame       = 1;             // this frame's number, the marks'
        NvAPI_Status StartStatus = NVAPI_ERROR;   // what NvAPI_Initialize answered
        NvAPI_Status ModeStatus  = NVAPI_ERROR;   // what the first NvAPI_D3D_SetSleepMode answered
    };

    // The sleep mode's block: the low latency mode, the boost (the GPU kept at its highest clock), no frame limit.
    inline NV_SET_SLEEP_MODE_PARAMS ModeParams(bool bOn, bool bBoost)
    {
        NV_SET_SLEEP_MODE_PARAMS P = {};
        P.version               = NV_SET_SLEEP_MODE_PARAMS_VER;
        P.bLowLatencyMode       = bOn ? 1 : 0;
        P.bLowLatencyBoost      = bBoost ? 1 : 0;
        P.minimumIntervalUs     = 0;
        P.bUseMarkersToOptimize = bOn ? 1 : 0;
        return P;
    }

    // Once, with the device: NVAPI started and the sleep mode set, off. Where the driver takes it, Reflex runs here.
    inline NvAPI_Status Start(FState& S, IUnknown* Device)
    {
        S             = FState();
        S.Device      = Device;
        S.StartStatus = NvAPI_Initialize();
        if (S.StartStatus != NVAPI_OK)
            return S.StartStatus;
        S.bNvapi                   = true;
        NV_SET_SLEEP_MODE_PARAMS P = ModeParams(false, false);
        S.ModeStatus               = NvAPI_D3D_SetSleepMode(Device, &P);
        S.bUp                      = (S.ModeStatus == NVAPI_OK);
        return S.ModeStatus;
    }

    // Before the device goes: the mode off, NVAPI let go.
    inline void Stop(FState& S)
    {
        if (S.bUp)
        {
            NV_SET_SLEEP_MODE_PARAMS P = ModeParams(false, false);
            NvAPI_D3D_SetSleepMode(S.Device, &P);
        }
        if (S.bNvapi)
            NvAPI_Unload();
        S = FState();
    }

    // The low latency mode on or off, and the boost.
    inline NvAPI_Status SetOn(FState& S, bool bOn, bool bBoost)
    {
        if (!S.bUp)
            return NVAPI_NOT_SUPPORTED;
        NV_SET_SLEEP_MODE_PARAMS P = ModeParams(bOn, bBoost);
        const NvAPI_Status       R = NvAPI_D3D_SetSleepMode(S.Device, &P);
        if (R == NVAPI_OK)
            S.bOn = bOn;
        return R;
    }

    // A stage of this frame marked.
    inline NvAPI_Status Mark(FState& S, NV_LATENCY_MARKER_TYPE Type)
    {
        if (!S.bUp)
            return NVAPI_NOT_SUPPORTED;
        NV_LATENCY_MARKER_PARAMS P = {};
        P.version                  = NV_LATENCY_MARKER_PARAMS_VER;
        P.frameID                  = S.Frame;
        P.markerType               = Type;
        return NvAPI_D3D_SetLatencyMarker(S.Device, &P);
    }

    // At the very start of a frame, before the input is read: the driver's wait, then the new frame's number and its
    // simulation's start - Reflex wants SIMULATION_START first in a frame, after the frame before's Sleep.
    inline NvAPI_Status Sleep(FState& S)
    {
        if (!S.bUp)
            return NVAPI_NOT_SUPPORTED;
        const NvAPI_Status R = NvAPI_D3D_Sleep(S.Device);
        S.Frame++;
        Mark(S, SIMULATION_START);
        return R;
    }

    // The driver's own measure of the last 64 frames, the newest last.
    inline NvAPI_Status Latency(FState& S, NV_LATENCY_RESULT_PARAMS& Out)
    {
        Out         = NV_LATENCY_RESULT_PARAMS();
        Out.version = NV_LATENCY_RESULT_PARAMS_VER;
        if (!S.bUp)
            return NVAPI_NOT_SUPPORTED;
        return NvAPI_D3D_GetLatency(S.Device, &Out);
    }

    // What the driver says it is doing.
    inline NvAPI_Status Status(FState& S, NV_GET_SLEEP_STATUS_PARAMS& Out)
    {
        Out         = NV_GET_SLEEP_STATUS_PARAMS();
        Out.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
        if (!S.bNvapi)
            return NVAPI_API_NOT_INITIALIZED;
        return NvAPI_D3D_GetSleepStatus(S.Device, &Out);
    }
}
