// runtime_tests.cpp -- behavioural tests for the parts of the PlainS runtime
// that need neither a socket nor a running tick loop: the dynamic Value type,
// the JSON codec, and the wire framing.
//
// Assertion-based and dependency-free, so it builds anywhere with a C++17
// compiler:
//
//   c++ -std=c++17 -Iruntime/include -o runtime_tests \
//       runtime/tests/runtime_tests.cpp runtime/src/pls_value.cpp \
//       runtime/src/pls_json.cpp runtime/src/net.cpp && ./runtime_tests
//
// The framing tests matter more than their size suggests: the frame layout is
// shared byte-for-byte with PlainVulkan's client (pv_net.h), and a mismatch
// there produces no compile error, no link error and no exception -- just a
// connection that hangs or delivers garbage.
#include "pls/pls_json.h"
#include "pls/pls_net.h"
#include "pls/pls_value.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace pls;

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond, msg)                                                              \
    do {                                                                              \
        gChecks++;                                                                    \
        if (!(cond)) {                                                                \
            std::printf("  FAIL: %s\n        (%s, line %d)\n", msg, #cond, __LINE__); \
            gFailures++;                                                              \
        }                                                                             \
    } while (0)

// ======================================================================
// Value container semantics. Every case here corresponds to a bug that was
// live in the runtime. On a server these are not cosmetic: every inbound
// player input arrives as one of these objects.
static void testValueSemantics() {
    std::printf("value semantics\n");

    // A string subscript is a KEY. index_get already treated `o["k"]` on an
    // object as a member read; index_ref did not, so writing through it
    // replaced the whole object with a one-element array and every other
    // field vanished with no diagnostic.
    {
        Value o;
        member_ref(o, "action") = Value(std::string("move"));
        member_ref(o, "x") = Value(1.5);
        index_ref(o, Value(std::string("action"))) = Value(std::string("shoot"));

        CHECK(o.isObject(), "object survives a string-subscript write");
        CHECK(index_get(o, Value(std::string("action"))).asString() == "shoot",
              "string-subscript write updates the right key");
        CHECK(index_get(o, Value(std::string("x"))).asFloat() == 1.5,
              "string-subscript write leaves sibling keys intact");
        CHECK(o.objectRef().size() == 2, "no keys were dropped");
    }

    // `o["k"] = v` on a fresh variable must produce the same thing as
    // `o.k = v`, not an array.
    {
        Value a, b;
        index_ref(a, Value(std::string("k"))) = Value(static_cast<int64_t>(1));
        member_ref(b, "k") = Value(static_cast<int64_t>(1));
        CHECK(a.isObject() && b.isObject(), "string subscript auto-vivifies an object");
        CHECK(index_get(a, Value(std::string("k"))).asInt() == 1, "auto-vivified key readable");
    }

    // A numeric subscript still auto-vivifies an array.
    {
        Value arr;
        index_ref(arr, Value(static_cast<int64_t>(2))) = Value(static_cast<int64_t>(7));
        CHECK(arr.isArray(), "numeric subscript auto-vivifies an array");
        CHECK(arr.arrayRef().size() == 3, "array grew to hold the index");
        CHECK(index_get(arr, Value(static_cast<int64_t>(2))).asInt() == 7, "value stored at index");
        CHECK(index_get(arr, Value(static_cast<int64_t>(0))).isNull(), "gap elements are null");
    }

    // An out-of-range index used to ask std::vector for a multi-gigabyte
    // allocation, and the resulting std::bad_alloc aborted the process. On a
    // server that index can come straight from a client packet, so this has
    // to be refused and survivable. (Intentionally logs one error line.)
    {
        std::printf("  (expect one intentional [ERROR] line below)\n");
        Value arr;
        index_ref(arr, Value(static_cast<int64_t>(5000000000LL))) = Value(static_cast<int64_t>(1));
        CHECK(arr.arrayRef().size() == 0, "absurd index is discarded, not allocated");
    }

    // Negative indices clamp to 0 rather than wrapping to a huge size_t.
    {
        Value arr;
        index_ref(arr, Value(static_cast<int64_t>(-5))) = Value(static_cast<int64_t>(3));
        CHECK(arr.arrayRef().size() == 1, "negative index clamps to slot 0");
    }

    // Reads never throw and never go out of bounds.
    {
        Value arr = make_array({Value(static_cast<int64_t>(1))});
        CHECK(index_get(arr, Value(static_cast<int64_t>(99))).isNull(), "out-of-range read is null");
        CHECK(index_get(arr, Value(static_cast<int64_t>(-1))).isNull(), "negative read is null");
        CHECK(member_get(Value(), "nope").isNull(), "member read on null is null");
    }

    // Arithmetic on a zero denominator yields 0 instead of trapping. An
    // integer division by zero is SIGFPE, which would kill the server.
    {
        CHECK((Value(static_cast<int64_t>(1)) / Value(static_cast<int64_t>(0))).asInt() == 0,
              "integer divide by zero yields 0");
        CHECK((Value(static_cast<int64_t>(1)) % Value(static_cast<int64_t>(0))).asInt() == 0,
              "integer modulo by zero yields 0");
    }

    // Strings that aren't numbers coerce to 0 rather than throwing out of
    // std::stoll/std::stod -- reachable from any client-supplied string.
    {
        CHECK(Value(std::string("abc")).asInt() == 0, "non-numeric string asInt is 0");
        CHECK(Value(std::string("abc")).asFloat() == 0.0, "non-numeric string asFloat is 0");
        CHECK(Value(std::string("")).asInt() == 0, "empty string asInt is 0");
    }
}

// ======================================================================
static void testJson() {
    std::printf("json\n");

    {
        std::string err;
        json::Json j = json::Json::parse(
            "{\"type\":\"state\",\"tick\":42,\"entities\":{\"1\":{\"health\":100}}}", &err);
        CHECK(err.empty(), "valid state packet parses");
        Value v = net::jsonToValue(j);
        CHECK(v.isObject(), "state packet decodes to an object");
        CHECK(member_get(v, "tick").asInt() == 42, "tick round-trips");
        Value ents = member_get(v, "entities");
        CHECK(ents.isObject(), "entities is an object keyed by id");
        CHECK(member_get(member_get(ents, "1"), "health").asInt() == 100, "nested field round-trips");
    }

    // Malformed input must report an error, never throw or crash: this is a
    // decoder fed directly from the socket.
    {
        const char* bad[] = {"{", "[1,", "{\"a\":}", "\"unterminated", "tru", "", "{\"a\":1,}"};
        for (const char* s : bad) {
            std::string err;
            json::Json::parse(s, &err);
            CHECK(!err.empty(), "malformed JSON is rejected with an error");
        }
    }

    // Deep nesting must be refused rather than recursing until the stack
    // overflows -- a two-line packet can otherwise kill the process.
    {
        std::string deep(4000, '[');
        std::string err;
        json::Json::parse(deep, &err);
        CHECK(!err.empty(), "deeply nested JSON is refused, not stack-overflowed");
    }

    // Value -> JSON -> Value round trip for the shapes actually broadcast.
    {
        Value entity = Value::MakeObject();
        entity.objectRef()["id"] = Value(static_cast<int64_t>(7));
        entity.objectRef()["position"] = make_array({Value(1.0), Value(2.5), Value(-3.0)});
        std::string encoded = net::valueToJson(entity).dump(-1);
        std::string err;
        Value back = net::jsonToValue(json::Json::parse(encoded, &err));
        CHECK(err.empty(), "encoded entity re-parses");
        CHECK(member_get(back, "id").asInt() == 7, "id survives the round trip");
        Value pos = member_get(back, "position");
        CHECK(pos.isArray() && pos.arrayRef().size() == 3, "position stays a 3-element array");
        CHECK(index_get(pos, Value(static_cast<int64_t>(1))).asFloat() == 2.5, "coordinates survive");
    }
}

// ======================================================================
// The frame layout is shared with PlainVulkan's client. These assert the
// exact bytes, not just that a round trip works, so a change on either side
// shows up here rather than as a hung connection.
static void testFraming() {
    std::printf("framing\n");

    {
        std::string f = net::buildFrame(net::FRAME_JSON, "hi");
        CHECK(f.size() == 7, "frame is 4-byte length + 1 kind + payload");
        CHECK(static_cast<unsigned char>(f[0]) == 0 && static_cast<unsigned char>(f[1]) == 0 &&
                  static_cast<unsigned char>(f[2]) == 0 && static_cast<unsigned char>(f[3]) == 3,
              "length prefix is big-endian and counts kind + payload");
        CHECK(static_cast<unsigned char>(f[4]) == 0, "FRAME_JSON is kind 0");
        CHECK(f.substr(5) == "hi", "payload follows verbatim");
    }

    // The kind byte values are part of the cross-project contract.
    {
        CHECK(net::FRAME_JSON == 0, "FRAME_JSON == 0");
        CHECK(net::FRAME_RAW == 1, "FRAME_RAW == 1");
        CHECK(net::FRAME_PING == 2, "FRAME_PING == 2");
        CHECK(net::FRAME_PONG == 3, "FRAME_PONG == 3");
        CHECK(net::FRAME_HANDSHAKE == 4, "FRAME_HANDSHAKE == 4");
        CHECK(net::FRAME_AEAD == 5, "FRAME_AEAD == 5");
    }

    // Three frames arriving coalesced in one recv().
    {
        net::RxStream rx;
        rx.buf = net::buildFrame(net::FRAME_JSON, "a") + net::buildFrame(net::FRAME_RAW, "bb") +
                 net::buildFrame(net::FRAME_PING, "cccc");
        net::FrameKind kind;
        std::string payload;
        bool oversize = false;
        int n = 0;
        std::string seen;
        while (rx.next(kind, payload, oversize)) {
            seen += payload;
            n++;
        }
        CHECK(n == 3, "three coalesced frames are all drained");
        CHECK(seen == "abbcccc", "payloads come out in order and intact");
        CHECK(rx.buf.empty(), "buffer is fully consumed");
    }

    // One frame split across two recv() calls, byte by byte.
    {
        std::string whole = net::buildFrame(net::FRAME_JSON, "{\"action\":\"move\"}");
        net::RxStream rx;
        net::FrameKind kind;
        std::string payload;
        bool oversize = false;
        bool got = false;
        for (size_t i = 0; i < whole.size(); ++i) {
            rx.buf.push_back(whole[i]);
            if (rx.next(kind, payload, oversize)) {
                got = true;
                CHECK(i == whole.size() - 1, "frame completes exactly on the last byte");
            }
        }
        CHECK(got, "a frame split one byte at a time still reassembles");
        CHECK(payload == "{\"action\":\"move\"}", "reassembled payload is intact");
    }

    // An empty payload is a legal frame (length prefix of 1: just the kind).
    {
        net::RxStream rx;
        rx.buf = net::buildFrame(net::FRAME_PONG, "");
        net::FrameKind kind;
        std::string payload = "stale";
        bool oversize = false;
        CHECK(rx.next(kind, payload, oversize), "zero-length payload frame is accepted");
        CHECK(kind == net::FRAME_PONG && payload.empty(), "kind survives, payload is empty");
    }

    // A hostile length prefix must be refused, not allocated for.
    {
        net::RxStream rx;
        rx.buf.assign(4, '\xFF');   // 0xFFFFFFFF
        rx.buf.push_back('\x00');
        net::FrameKind kind;
        std::string payload;
        bool oversize = false;
        CHECK(!rx.next(kind, payload, oversize), "oversize frame yields no payload");
        CHECK(oversize, "oversize frame is flagged so the caller can drop the connection");
    }

    // A zero length prefix is invalid (every frame has at least a kind byte)
    // and must not spin the caller's drain loop forever.
    {
        net::RxStream rx;
        rx.buf.assign(4, '\0');
        rx.buf.push_back('\x00');
        net::FrameKind kind;
        std::string payload;
        bool oversize = false;
        CHECK(!rx.next(kind, payload, oversize), "zero-length frame yields no payload");
        CHECK(oversize, "zero-length frame is flagged as a protocol error");
    }
}

// ======================================================================
int main() {
    std::printf("PlainS runtime tests\n\n");
    testValueSemantics();
    testJson();
    testFraming();

    std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
    if (gFailures == 0) std::printf("PASS\n");
    return gFailures == 0 ? 0 : 1;
}
