#pragma once

// The renderer's own screenshots (Screenshot=On, RenderDevice.cpp's ShotPairFrame): a frame read back from the card,
// written out as a PNG by Windows' own image coder (WIC) on a thread of its own, so the game does not wait for the
// coding. No engine header behind it - the engine's macros and the coder's COM headers stay apart - and nothing in it
// logs: the log is the game thread's, so what has finished is asked for from there (RdxShotWriteDone).

// Room for a picture of W x H pixels, four bytes each - blue, green, red and one unused, the order FBackend::ReadScene
// leaves them in - or null.
void* RdxShotAlloc(int W, int H);

// Gives back what RdxShotAlloc gave, where it was not handed to RdxShotWriteStart.
void RdxShotFree(void* Pixels);

// Starts writing the picture into Path as a PNG and takes the pixels over: they are freed when the writing is done -
// or at once where it cannot start (every writer busy, no thread), and then the answer is 0.
int RdxShotWriteStart(void* Pixels, int W, int H, const wchar_t* Path);

// One write finished since the last call: 1, with its file and whether it was written (bOk); 0 where none has.
int RdxShotWriteDone(wchar_t* Path, int PathChars, int* bOk);

// Waits for the writes still under way - the renderer's exit, so that none is cut off half written.
void RdxShotWriteWait();
