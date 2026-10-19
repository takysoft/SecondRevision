// VolumeCheck - the renderer's log, to the console (the renderer's own Log.h writes to the game's log files).
#pragma once

void RdxLog(const TCHAR* Fmt, ...);

// The renderer asks this before each allocation that may fail, so a refusal can be tried without the driver giving
// one: true where the name holds the word this run was started with (-fail).
bool RdxForcedFailure(const TCHAR* What);

#define RDX_LOG(text)      RdxLog(TEXT("%s"), TEXT(text))
#define RDX_LOGF(fmt, ...) RdxLog(TEXT(fmt), __VA_ARGS__)
