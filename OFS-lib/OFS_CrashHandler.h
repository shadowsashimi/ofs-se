#pragma once

// Writes a symbolised stack trace to the preferences directory when the process
// dies from an unhandled SEH exception or a call to std::terminate. Several
// hot paths in OFS are marked noexcept while calling things that can throw
// (std::any_cast in the state manager, allocations in the draw lists), so a
// fault otherwise disappears with no message at all.
//
// Also watches for hangs. A frozen process leaves even less behind than a crash
// does, so a watchdog thread dumps the main thread's stack once it stops making
// progress. The clock starts at Install(), which means a hang during startup
// (before the first frame is ever drawn) is caught too.
namespace OFS_CrashHandler
{
// Installs the handlers and starts the watchdog. Call once, early in main().
void Install() noexcept;

// Call once per frame from the main loop. Missing heartbeats are what the
// watchdog treats as a hang.
void Heartbeat() noexcept;
}
