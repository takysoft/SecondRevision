// SecondRevision - what the machine is, for LogLevel=Debug.
//
// Why: the renderer has run on two machines, and a player's fault report is worth only as much as what it says about
// the machine it came from. Windows and its update, the processor, the memory and this process's address space, every
// graphics card with its driver's version, every screen with its mode and scaling, and every module loaded into the
// game - overlays, injected wrappers and a d3d12.dll that is not Windows' own among them - each with its file version.
// Written once, at the start, before the device is made, so it is in the log whatever happens next.
#pragma once

void RdxLogSystemReport();
