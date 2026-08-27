// Getting events out of the game process.
//
// Two channels, and both are used every time:
//
//   1. A loopback UDP datagram to 127.0.0.1:29001. This is the real path. It
//      is non-blocking, so a middleware that is not running, is paused in a
//      debugger, or has fallen over cannot stall the game's frame loop - which
//      matters more here than it would in a script, because this code runs
//      inside GameFrame().
//
//   2. Msg() to the console, tagged "[PSVR2H]". Costs almost nothing, is
//      readable by a human, and is picked up by the middleware's console.log
//      tailer if the socket could not be opened. It is also what makes a
//      problem diagnosable without attaching anything: the events are simply
//      there in the console.
//
// The wire format is the one the Half-Life: Alyx integration already uses, so
// recordings from either game are the same kind of file and the middleware
// needed no new parser:
//
//     [PSVR2H] <EVENT>:<field>,<field>,...

#pragma once

#include <cstdarg>

namespace psvr2h {

// Opens the socket. Failure is not fatal - the console channel still works.
bool EmitInit(unsigned short port = 29001);
void EmitShutdown();

// True when the datagram channel is live. Reported once at load so a user can
// tell which path they are on.
bool EmitHasSocket();

// Sends one event. `params` may be null or empty for a bare event.
void Emit(const char* event, const char* params);

// printf-style convenience for the common case of formatting fields.
void EmitF(const char* event, const char* fmt, ...);

// Console echo control. On by default; a user who finds the console noisy can
// turn it off with the psvr2_haptics_console cvar, and the UDP channel is
// unaffected.
void EmitSetConsole(bool enabled);

} // namespace psvr2h
