// SecondRevision - the crash report: where a hard fault happened, in the log.
//
// Why: the engine catches every fault itself (guard/unguard) and says only WHICH guarded function it came through - the
// crash on loading saves one after the other (docs/nyitott-hibak.md 26) read "UViewport::Unlock", and everything we do in a
// frame is under that one name. This names the instruction: the exception's code, its address as module+offset, and the
// return addresses found on the stack. scripts/crash_symbols.py turns the offsets into functions and lines from the PDB.
#pragma once

// Adds the vectored exception handler; a second call does nothing. It only reports - the exception goes on to the engine.
void RdxCrashLogInstall();

// Takes it away again. Called from Exit(); safe when never installed.
void RdxCrashLogRemove();

// LogLevel=Debug: the first hard fault of the run also writes the game's memory as a dump beside the log
// (SecondRevision-crash-<date>-<time>.dmp), for the debugger and the PDB to read where the stack scan cannot reach.
void RdxCrashLogDumps(UBOOL bOn);
