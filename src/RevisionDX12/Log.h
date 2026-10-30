// SecondRevision - diagnostics.
//
// Why a separate log file rather than just debugf():
// the engine buffers its log and only flushes it when the buffer fills or the process
// exits cleanly. During development we routinely kill the game with a forced stop, so
// the last - and most interesting - lines never reach Revision.log. A renderer that
// logs sparsely can lose its entire startup trace that way, which is exactly what
// happened on the first bring-up run.
//
// So every message goes to BOTH places: to debugf(), so it appears in Revision.log in
// context with the engine's own lines, and to our own file, flushed immediately, so it
// survives however the process dies.
#pragma once

// Writes to Revision.log and to System\SecondRevision.log, the latter flushed at once.
void RdxLog(const TCHAR* Fmt, ...);

// LogLevel=Debug (SecondRevision.settings.ini): the log takes everything the renderer can tell, and stops at a cap of
// 1024 MB with a line that says so, so a run left going cannot fill the disk. Info, the default, is the log as it
// always was, without a cap.
void  RdxLogSetDebug(UBOOL bOn);
UBOOL RdxLogIsDebug();

// Closes our file. Called from Exit(); safe to call when never opened.
void RdxLogClose();

// A whole line - its line break with it - to our own file alone, flushed: for a thread that is not the engine's, whose
// log is not to be called from another thread. One write, so the render thread's own lines cannot cut it in two.
// Nothing where the file was never opened.
void RdxLogLineFromAnyThread(const char* Line);

// A measurement only (the marker %TEMP%\SecondRevision-fail-alloc.txt, ten minutes fresh): true where the name CONTAINS
// the marker's text, so a guard can report a failure the driver never gave. Out of video memory a create is refused,
// and what the player then sees depends entirely on what the renderer does next - go on without that one feature, or
// stop. There is no way to try that on a card with memory to spare except to say the word 'failed' where the driver
// would have said it. Walked by scripts/fail-alloc.ps1; see docs/nyitott-hibak.md 56.
// Several texts may stand in the marker with ';' between them, and each may end in '*' and a count: then only its
// first so many matches fail and the later ones go through - which is how the way BACK from a refusal is walked
// ("motion blur*1;bloom*1"). And in '@' and a count: so many matches go through before any fails - what the start
// makes, where the refusal wanted is of the same thing made again later ("resource(scene)*2@1": the change of mode
// and the way back from it both refused).
bool RdxForcedFailure(const TCHAR* What);

// The start's clock: what the window before the first picture is made of. A step of the start marked with the time -
// counted from when the game's process began, so the engine's own start before our DLL is asked for anything shows too.
// What must be a string that lives for good (a literal). RdxStartupReport writes the marks once, as one block; marks
// after it are dropped.
void RdxStartupMark(const TCHAR* What);
void RdxStartupReport(const TCHAR* When);
// A shader, for the start's clock: Ms spent on it, compiled (bCompiled) or taken from the cache (RdxCompile). Every mark
// then says how much of its step went on shaders.
void RdxStartupNoteShader(DOUBLE Ms, UBOOL bCompiled);
// How many shaders the process has compiled so far - the ones read from the cache's file not counted: the start's
// caption (FBackend::PresentCompiling).
INT  RdxStartupShaders();

#define RDX_LOG(text)      RdxLog(TEXT("%s"), TEXT(text))
#define RDX_LOGF(fmt, ...) RdxLog(TEXT(fmt), __VA_ARGS__)
