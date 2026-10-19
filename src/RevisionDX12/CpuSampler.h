// SecondRevision - a measurement only (the marker SecondRevision-cpu-samples.txt): where the game's thread spends its
// time, the engine's code and ours alike. A thread of its own stops the game's thread every millisecond, notes where it
// stood, and lets it go on; now and then the game's thread puts the places, by function, into the log - ours by our
// symbols, the engine's by its modules' exports.
#pragma once

// On the game's thread: the sampler started for it.
void RdxCpuSamplerStart();
// On the game's thread, once a frame: every Seconds of samples, a report into the log - the Top functions and the
// modules, with Label (the level, say) - and the count starts again.
void RdxCpuSamplerFrame(double Seconds, int Top, const char* Label);
void RdxCpuSamplerStop();
