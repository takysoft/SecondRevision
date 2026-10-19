#pragma once

// NVIDIA Reflex for the renderer: the AntiLag key on NVIDIA's cards (AMD's take Anti-Lag 2, AntiLag2.h). The calls are
// Reflex.h's - the very code tools/ReflexProbe runs - and they live in ReflexLink.cpp: NVAPI's header and the engine's
// clock() macros cannot meet in one file (VrrProbe.cpp). One device at a time, as the backend has.
//
// The frame, as the game's thread walks it: RdxReflexSleep at its very start, just before the engine reads its input
// (where the low latency mode is on the driver holds the thread there; it also begins the frame's number and marks
// its simulation's start); the simulation's end and the rendering's start where the engine starts drawing (Lock); the
// rendering's end before the Present; the Present wrapped. With frame generation the Present the game's thread makes
// is the hand-over to the pacer, and that is what the marks wrap - as a game wraps its Present to a frame generation's
// own swap chain.

// NVAPI's own numbers for the marks (NV_LATENCY_MARKER_TYPE; ReflexLink.cpp holds them to it).
enum ERdxReflexMark
{
    RdxReflexMark_SimulationEnd = 1,
    RdxReflexMark_SubmitStart   = 2,
    RdxReflexMark_SubmitEnd     = 3,
    RdxReflexMark_PresentStart  = 4,
    RdxReflexMark_PresentEnd    = 5,
};

// Once, with the device: where the driver takes the sleep mode Reflex runs here, and the log says what NVAPI answered.
UBOOL RdxReflexStart(IUnknown* Device);

// Before the device goes.
void RdxReflexStop();

// Whether Reflex runs on this device.
UBOOL RdxReflexUp();

// The low latency mode on or off (the AntiLag key; the menu's line).
void RdxReflexSetOn(UBOOL bOn);

// The frame's very start, every frame, on or off.
void RdxReflexSleep();

// A stage of this frame (ERdxReflexMark).
void RdxReflexMark(INT Mark);
