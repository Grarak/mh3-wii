// The debug port (development builds on the hosts, docs/debug-port.md): a
// TCP line protocol for driving and inspecting a running game.
// MH3_DEBUG_PORT=PORT (on 127.0.0.1) or ADDR:PORT; each line is a command,
// each answer one line starting with "ok" or "error".
#pragma once

struct CpuContext;

namespace DebugPort {

// Game thread, once a logic step before the tasks run: starts the listener on
// the first call, then runs the queued commands while none is waiting.
void Step(CpuContext* ctx);

} // namespace DebugPort
