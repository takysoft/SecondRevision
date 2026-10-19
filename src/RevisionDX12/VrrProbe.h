#pragma once

// Does the screen this window is on run with variable refresh (FreeSync, G-Sync, Adaptive-Sync)?
//
// Windows will not say. DXGI only tells us whether tearing presents are allowed at all, which is an operating system
// and driver matter and true on every modern machine - it says nothing about the screen. The graphics driver knows,
// and each vendor has a library for asking it; this is where those answers meet.
//
// The answer is RdxMath::EVrr: Vrr_On, Vrr_Off, or Vrr_NoData when nothing on this machine would say. The frame rate
// limit treats NoData as On (RdxMath::FpsLimitTarget): backing off three frames costs a fixed-refresh screen almost
// nothing, while running into the top of a variable one is felt.
//
// Asked on a thread of its own: AMD's library alone took 861 ms to start at the game's start, all of it before the first
// picture, and the answer only ever moves an Auto frame rate limit - which runs on NoData's terms until it comes. A
// screen asked about once in this process is answered at once after that (every return from Alt+Tab asks again).

// Asks about the screen the window is on now. The answer comes through RdxTakeVrr - at once where it is already known.
void RdxAskVrr(HWND Window, UBOOL bTearingAllowed);

// True once for each answer that came since the last call, with the answer in State (and its log line written - on the
// caller's thread, not the probe's). Cheap: once a frame is the intended use.
UBOOL RdxTakeVrr(INT& State);

// A probe still running is waited for, at most a few seconds: the renderer's end, before anything it uses may go.
void RdxVrrWait();
