// debug.cpp -- logging, statistics, and the tick recorder.
#include "pls/pls_runtime.h"
#include "pls/pls_server.h"

#include <string>

namespace pls {
namespace rt {

Value Print(const Value& message) {
    // Routed through the same builtin as a bare `Print(...)` so both spellings
    // produce byte-identical output. The reference's example server uses
    // both, and having them differ in format would make a log impossible to
    // parse consistently.
    return builtin::Print(message);
}

Value PrintStats() {
    Server& s = server();
    logLine("STATS", "tick=" + std::to_string(s.tick) +
                         " players=" + std::to_string(s.players.size()) +
                         " entities=" + std::to_string(s.entities.size()) +
                         " tickrate=" + std::to_string(s.tickrate) +
                         " up=" + std::to_string(static_cast<int64_t>(GetElapsedTime().asFloat())) +
                         "s rx=" + std::to_string(s.bytesReceived) +
                         "B tx=" + std::to_string(s.bytesSent) + "B");
    return Value(true);
}

Value EnableDebugLogging(const Value& enabled) {
    server().debugLogging = enabled.truthy();
    return Value(true);
}

Value GetServerStats() {
    Server& s = server();
    Value out = Value::MakeObject();
    auto& o = out.objectRef();
    o["tick"] = Value(static_cast<int64_t>(s.tick));
    o["players"] = Value(static_cast<int64_t>(s.players.size()));
    o["entities"] = Value(static_cast<int64_t>(s.entities.size()));
    o["tickrate"] = Value(static_cast<int64_t>(s.tickrate));
    o["uptime"] = GetElapsedTime();
    o["bytesSent"] = Value(static_cast<int64_t>(s.bytesSent));
    o["bytesReceived"] = Value(static_cast<int64_t>(s.bytesReceived));
    o["port"] = Value(static_cast<int64_t>(s.port));
    return out;
}

Value RecordTick(const Value& tick_number) {
    // Snapshots the whole world under the given tick number, into a ring
    // buffer. A ring rather than an unbounded log because this is meant to
    // be left on while reproducing a bug -- an unbounded one would consume
    // memory proportional to uptime and eventually be the bug.
    Server& s = server();
    RecordedTick r;
    r.tick = static_cast<uint64_t>(tick_number.asInt());
    r.elapsed = GetElapsedTime().asFloat();
    r.world = SerializeAllEntities();
    s.recorded.push_back(r);
    while (s.recorded.size() > s.maxRecorded) s.recorded.pop_front();
    return Value(true);
}

Value ReplayTick(const Value& tick_number) {
    // Returns the recorded snapshot rather than restoring it. Restoring
    // would silently diverge the server from every connected client, whose
    // own state cannot be rewound -- so the snapshot is handed back for the
    // script (or a human reading the log) to inspect, which is what the
    // "step-through debugging" use actually needs.
    Server& s = server();
    uint64_t want = static_cast<uint64_t>(tick_number.asInt());
    for (const RecordedTick& r : s.recorded) {
        if (r.tick != want) continue;
        Value out = Value::MakeObject();
        out.objectRef()["tick"] = Value(static_cast<int64_t>(r.tick));
        out.objectRef()["elapsed"] = Value(r.elapsed);
        out.objectRef()["world"] = r.world;
        return out;
    }
    logLine("WARN", "PlS::ReplayTick: tick " + std::to_string(want) +
                        " is not in the recording buffer (holding " +
                        std::to_string(s.recorded.size()) + " ticks)");
    return Value();
}

} // namespace rt
} // namespace pls
