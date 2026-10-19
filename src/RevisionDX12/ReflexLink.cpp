#include "Pch.h"
#include "ReflexLink.h"
#include "Log.h"

// The engine's own clock()/unclock() macros break anything that includes the C runtime's headers, and NVAPI's does
// (VrrProbe.cpp). They go away for the length of this file.
#undef clock
#undef unclock

#include "Reflex.h"         // NVIDIA Reflex through NVAPI (MIT - external/NVAPI), shared with tools/ReflexProbe

static_assert(RdxReflexMark_SimulationEnd == SIMULATION_END && RdxReflexMark_SubmitStart == RENDERSUBMIT_START &&
                  RdxReflexMark_SubmitEnd == RENDERSUBMIT_END && RdxReflexMark_PresentStart == PRESENT_START &&
                  RdxReflexMark_PresentEnd == PRESENT_END,
              "the marks are NVAPI's own numbers");

static RdxReflex::FState GReflex;

UBOOL RdxReflexStart(IUnknown* Device)
{
    RdxReflex::Start(GReflex, Device);
    if (!GReflex.bUp)
    {
        RDX_LOGF("reflex: not here (NVAPI %i, the sleep mode %i) - another card, or a driver without it",
                 (INT)GReflex.StartStatus, (INT)GReflex.ModeStatus);
        return 0;
    }
    NV_GET_SLEEP_STATUS_PARAMS St;
    const NvAPI_Status         R = RdxReflex::Status(GReflex, St);
    RDX_LOGF("reflex: the driver has it (its status %i: low latency mode %i, variable refresh %i, vsync forced in its "
             "control panel %i, its sleep interval %u us, the game's sleep used %i)",
             (INT)R, (INT)St.bLowLatencyMode, (INT)St.bFsVrr, (INT)St.bCplVsyncOn, (UINT)St.sleepIntervalUs,
             (INT)St.bUseGameSleep);
    return 1;
}

void RdxReflexStop()
{
    RdxReflex::Stop(GReflex);
}

UBOOL RdxReflexUp()
{
    return GReflex.bUp ? 1 : 0;
}

void RdxReflexSetOn(UBOOL bOn)
{
    if (!GReflex.bUp || (GReflex.bOn ? 1 : 0) == (bOn ? 1 : 0))
        return;
    const NvAPI_Status R = RdxReflex::SetOn(GReflex, bOn != 0, false);
    if (R != NVAPI_OK)
        RDX_LOGF("reflex: the driver would not turn the low latency mode %s (NVAPI %i)", bOn ? TEXT("on") : TEXT("off"),
                 (INT)R);
}

void RdxReflexSleep()
{
    RdxReflex::Sleep(GReflex);
}

void RdxReflexMark(INT Mark)
{
    RdxReflex::Mark(GReflex, (NV_LATENCY_MARKER_TYPE)Mark);
}
