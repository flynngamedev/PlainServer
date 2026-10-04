// net.cpp -- framing and Value<->JSON conversion for the server side.
//
// This file is the mirror of the corresponding half of PlainVulkan's
// network.cpp. It contains no PlS:: commands: the socket *loop* lives in
// server.cpp, because on the server it is part of the tick, not a
// standalone subsystem.
#include "pls/pls_net.h"

#include <chrono>
#include <cmath>

namespace pls {
namespace net {

namespace {

void appendU32BE(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

uint32_t readU32BE(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])));
}

} // namespace

std::string buildFrame(FrameKind kind, const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 5);
    appendU32BE(out, static_cast<uint32_t>(payload.size() + 1));
    out.push_back(static_cast<char>(kind));
    out += payload;
    return out;
}

bool RxStream::next(FrameKind& kind, std::string& payload, bool& oversize) {
    oversize = false;
    if (buf.size() < 4) return false;
    uint32_t len = readU32BE(buf.data());
    if (len == 0 || len > kMaxFrameBytes) {
        oversize = true;
        return false;
    }
    if (buf.size() < 4 + static_cast<size_t>(len)) return false;
    kind = static_cast<FrameKind>(static_cast<unsigned char>(buf[4]));
    payload.assign(buf, 5, static_cast<size_t>(len) - 1);
    buf.erase(0, 4 + static_cast<size_t>(len));
    return true;
}

uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

json::Json valueToJson(const Value& v) {
    switch (v.type()) {
        case ValueType::Null: return json::Json();
        case ValueType::Bool: return json::Json(v.asBool());
        case ValueType::Int: return json::Json(v.asInt());
        case ValueType::Float: {
            // JSON has no NaN or Infinity. Emitting one produces a document
            // the client's parser will reject, taking down the whole state
            // packet -- so a non-finite number becomes 0 here. Physics that
            // has gone non-finite is already a bug; this keeps it from
            // becoming a disconnect for every player at once.
            double d = v.asFloat();
            if (!std::isfinite(d)) return json::Json(0.0);
            return json::Json(d);
        }
        case ValueType::String: return json::Json(v.asString());
        case ValueType::Handle: return json::Json(static_cast<int64_t>(v.asHandle().id));
        case ValueType::Array: {
            json::JsonArray arr;
            Value& mut = const_cast<Value&>(v);
            for (const Value& e : mut.arrayRef()) arr.push_back(valueToJson(e));
            return json::Json(std::move(arr));
        }
        case ValueType::Object: {
            json::JsonObject obj;
            Value& mut = const_cast<Value&>(v);
            for (const auto& kv : mut.objectRef()) obj[kv.first] = valueToJson(kv.second);
            return json::Json(std::move(obj));
        }
    }
    return json::Json();
}

Value jsonToValue(const json::Json& j) {
    switch (j.type()) {
        case json::JType::Null: return Value();
        case json::JType::Bool: return Value(j.asBool());
        case json::JType::Number: {
            // Preserve integer-ness: player ids and entity ids travel as
            // numbers and get used as lookup keys, and an id that
            // stringifies as "12.0" would not match the "12" the other side
            // wrote.
            double d = j.asDouble();
            double intPart = 0.0;
            if (std::modf(d, &intPart) == 0.0 && d >= -9.2e18 && d <= 9.2e18) {
                return Value(static_cast<int64_t>(d));
            }
            return Value(d);
        }
        case json::JType::String: return Value(j.asString());
        case json::JType::Array: {
            Value out = Value::MakeArray();
            for (const auto& e : j.asArray()) out.arrayRef().push_back(jsonToValue(e));
            return out;
        }
        case json::JType::Object: {
            Value out = Value::MakeObject();
            for (const auto& kv : j.asObject()) out.objectRef()[kv.first] = jsonToValue(kv.second);
            return out;
        }
    }
    return Value();
}

} // namespace net
} // namespace pls
