// builtins.cpp -- the bare-name functions a `.pls` script can call without
// a namespace: Print, Len, Keys, Has, Push, Str/Int/Float, and a small
// numeric set.
//
// These are deliberately NOT in pls::rt with the PlS:: commands. They are
// language conveniences rather than server operations -- `Sqrt` has nothing
// to do with running a server -- and the separation is also what lets
// scripts/check_command_coverage.sh hold pls_runtime.h to exactly the
// documented PlS:: command set without an ever-growing exclusion list.
//
// Every one of these is total: given a value of the wrong type they return
// a sensible zero/empty/false rather than throwing. A script is dynamically
// typed and has no way to catch an exception, so a throw here would be an
// unconditional server crash -- and taking the whole match down because one
// player's packet had a string where a number was expected is exactly the
// failure mode a game server must not have.
#include "pls/pls_value.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace pls {
namespace builtin {

Value Print(const Value& message) {
    // stdout, line-buffered flush. A server's log is usually being watched
    // through a pipe (systemd, docker logs, a tail), where the default full
    // buffering would hold output back for 4KB at a time and make the log
    // useless for watching a live server.
    std::string text = message.asString();
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return Value();
}

Value Len(const Value& v) {
    return Value(iter_count(v));
}

Value Keys(const Value& obj) {
    Value out = Value::MakeArray();
    if (!obj.isObject()) return out;
    Value& mut = const_cast<Value&>(obj);
    for (const auto& kv : mut.objectRef()) out.arrayRef().push_back(Value(kv.first));
    return out;
}

Value Has(const Value& obj, const Value& key) {
    if (!obj.isObject()) return Value(false);
    Value& mut = const_cast<Value&>(obj);
    const auto& o = mut.objectRef();
    return Value(o.find(key.asString()) != o.end());
}

Value Push(const Value& arr, const Value& v) {
    // Mutates in place and also returns the array. Value's array payload is
    // a shared_ptr, so every copy of the script-level variable observes the
    // push -- which is what a script means by `Push(list, x)`.
    Value& mut = const_cast<Value&>(arr);
    mut.arrayRef().push_back(v); // auto-vivifies Null -> empty array
    return arr;
}

Value Str(const Value& v) {
    return Value(v.asString());
}

Value Int(const Value& v) {
    return Value(v.asInt());
}

Value Float(const Value& v) {
    return Value(v.asFloat());
}

Value Abs(const Value& v) {
    // Preserves int-ness: Abs of an entity id or a tick delta should stay an
    // integer, not silently become a float that stringifies as "3.0".
    if (v.type() == ValueType::Int) {
        int64_t i = v.asInt();
        return Value(i < 0 ? -i : i);
    }
    return Value(std::fabs(v.asFloat()));
}

Value Min(const Value& a, const Value& b) {
    if (a.type() == ValueType::Int && b.type() == ValueType::Int) {
        return Value(a.asInt() < b.asInt() ? a.asInt() : b.asInt());
    }
    return Value(a.asFloat() < b.asFloat() ? a.asFloat() : b.asFloat());
}

Value Max(const Value& a, const Value& b) {
    if (a.type() == ValueType::Int && b.type() == ValueType::Int) {
        return Value(a.asInt() > b.asInt() ? a.asInt() : b.asInt());
    }
    return Value(a.asFloat() > b.asFloat() ? a.asFloat() : b.asFloat());
}

Value Sqrt(const Value& v) {
    double d = v.asFloat();
    // Negative input yields 0 rather than NaN: a NaN would propagate
    // silently through a position calculation and end up on the wire, where
    // it is not even representable in JSON.
    return Value(d <= 0.0 ? 0.0 : std::sqrt(d));
}

Value Floor(const Value& v) {
    return Value(static_cast<int64_t>(std::floor(v.asFloat())));
}

Value Ceil(const Value& v) {
    return Value(static_cast<int64_t>(std::ceil(v.asFloat())));
}

Value Random() {
    // Seeded once from the clock. Not cryptographic, and not meant to be --
    // it is for spawn jitter and loot rolls. Anything security-relevant
    // (session tokens, matchmaking secrets) must not come from here.
    static std::mt19937_64 engine(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return Value(dist(engine));
}

} // namespace builtin
} // namespace pls
