// SecondRevision - a diagnostic: what is a blocked Present call waiting for?
//
// Off unless asked for (see WantPresentWatch in RenderDevice.cpp). When one Present call has
// been blocked for more than a few milliseconds, a watchdog thread suspends the render thread
// for a moment, copies its registers and the top of its stack, and lets it go again. Only
// then, with nothing held, are the return addresses turned into module!export+offset - and
// the line is handed back to the render thread to log: the engine's log is not ours to call
// from another thread.
#pragma once

void RdxPresentWatchStart();                      // render thread: starts the watchdog
void RdxPresentWatchStop();                       // render thread: stops and joins it
void RdxPresentWatchEnter(INT PresentNumber);     // right before Present
void RdxPresentWatchLeave();                      // right after it; logs what was caught

// The stall watch - a diagnostic of the same kind: what is the game doing while no frame comes out?
//
// Off unless asked for (WantStallWatch in RenderDevice.cpp). The render thread stamps the clock at the end of every
// frame; where a quarter of a second passes with no stamp, a watchdog thread samples the render thread as the Present
// watch does, and again every quarter second the stall goes on. A frame that never ends cannot log anything, so the
// watchdog writes the line itself - to the renderer's own file alone (RdxLogLineFromAnyThread). Beside the chain of
// saved frame pointers it lists what on the stack looks like a return address: code built without frame pointers
// ends the chain early, and the list still shows the way in.
void RdxStallWatchStart();                        // render thread: starts the watchdog
void RdxStallWatchStop();                         // render thread: stops and joins it
void RdxStallWatchFrame(INT FrameNumber);         // render thread: a frame has ended
