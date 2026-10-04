#include "pls/pls_value.h"

#include <iterator>

#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace pls {

const char* type_name(ValueType t) {
    switch (t) {
        case ValueType::Null: return "null";
        case ValueType::Bool: return "bool";
        case ValueType::Int: return "int";
        case ValueType::Float: return "float";
        case ValueType::String: return "string";
        case ValueType::Array: return "array";
        case ValueType::Object: return "object";
        case ValueType::Handle: return "handle";
    }
    return "unknown";
}

ValueType Value::type() const {
    switch (data_.index()) {
        case 0: return ValueType::Null;
        case 1: return ValueType::Bool;
        case 2: return ValueType::Int;
        case 3: return ValueType::Float;
        case 4: return ValueType::String;
        case 5: return ValueType::Array;
        case 6: return ValueType::Object;
        case 7: return ValueType::Handle;
    }
    return ValueType::Null;
}

bool Value::asBool() const {
    if (auto* b = std::get_if<bool>(&data_)) return *b;
    return truthy();
}

int64_t Value::asInt() const {
    if (auto* i = std::get_if<int64_t>(&data_)) return *i;
    if (auto* d = std::get_if<double>(&data_)) return static_cast<int64_t>(*d);
    if (auto* b = std::get_if<bool>(&data_)) return *b ? 1 : 0;
    if (auto* s = std::get_if<std::string>(&data_)) {
        try { return std::stoll(*s); } catch (...) { return 0; }
    }
    return 0;
}

double Value::asFloat() const {
    if (auto* d = std::get_if<double>(&data_)) return *d;
    if (auto* i = std::get_if<int64_t>(&data_)) return static_cast<double>(*i);
    if (auto* b = std::get_if<bool>(&data_)) return *b ? 1.0 : 0.0;
    if (auto* s = std::get_if<std::string>(&data_)) {
        try { return std::stod(*s); } catch (...) { return 0.0; }
    }
    return 0.0;
}

std::string Value::asString() const {
    switch (type()) {
        case ValueType::Null: return "null";
        case ValueType::Bool: return asBool() ? "true" : "false";
        case ValueType::Int: return std::to_string(std::get<int64_t>(data_));
        case ValueType::Float: {
            std::ostringstream ss;
            ss << std::get<double>(data_);
            return ss.str();
        }
        case ValueType::String: return std::get<std::string>(data_);
        case ValueType::Array: {
            std::ostringstream ss;
            ss << "[";
            auto& arr = *std::get<std::shared_ptr<ValueArray>>(data_);
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) ss << ", ";
                ss << arr[i].asString();
            }
            ss << "]";
            return ss.str();
        }
        case ValueType::Object: {
            std::ostringstream ss;
            ss << "{";
            auto& obj = *std::get<std::shared_ptr<ValueObject>>(data_);
            bool first = true;
            for (auto& [k, v] : obj) {
                if (!first) ss << ", ";
                first = false;
                ss << k << ": " << v.asString();
            }
            ss << "}";
            return ss.str();
        }
        case ValueType::Handle: {
            auto& h = std::get<Handle>(data_);
            return "<" + h.kind + "#" + std::to_string(h.id) + ">";
        }
    }
    return "";
}

const Handle& Value::asHandle() const {
    static Handle invalid{0, "invalid"};
    if (auto* h = std::get_if<Handle>(&data_)) return *h;
    return invalid;
}

ValueArray& Value::arrayRef() {
    if (!std::holds_alternative<std::shared_ptr<ValueArray>>(data_)) {
        data_ = std::make_shared<ValueArray>();
    }
    return *std::get<std::shared_ptr<ValueArray>>(data_);
}

ValueObject& Value::objectRef() {
    if (!std::holds_alternative<std::shared_ptr<ValueObject>>(data_)) {
        data_ = std::make_shared<ValueObject>();
    }
    return *std::get<std::shared_ptr<ValueObject>>(data_);
}

bool Value::truthy() const {
    switch (type()) {
        case ValueType::Null: return false;
        case ValueType::Bool: return std::get<bool>(data_);
        case ValueType::Int: return std::get<int64_t>(data_) != 0;
        case ValueType::Float: return std::get<double>(data_) != 0.0;
        case ValueType::String: return !std::get<std::string>(data_).empty();
        case ValueType::Array: return !std::get<std::shared_ptr<ValueArray>>(data_)->empty();
        case ValueType::Object: return !std::get<std::shared_ptr<ValueObject>>(data_)->empty();
        case ValueType::Handle: return std::get<Handle>(data_).id != 0;
    }
    return false;
}

static bool either_is_float(const Value& a, const Value& b) {
    return a.type() == ValueType::Float || b.type() == ValueType::Float;
}
static bool either_is_string(const Value& a, const Value& b) {
    return a.type() == ValueType::String || b.type() == ValueType::String;
}

Value operator+(const Value& a, const Value& b) {
    if (either_is_string(a, b)) return Value(a.asString() + b.asString());
    if (a.isArray() && b.isArray()) {
        ValueArray out = *std::get<std::shared_ptr<ValueArray>>(a.data_);
        auto& rb = *std::get<std::shared_ptr<ValueArray>>(b.data_);
        out.insert(out.end(), rb.begin(), rb.end());
        return Value(out);
    }
    if (either_is_float(a, b)) return Value(a.asFloat() + b.asFloat());
    return Value(a.asInt() + b.asInt());
}
Value operator-(const Value& a, const Value& b) {
    if (either_is_float(a, b)) return Value(a.asFloat() - b.asFloat());
    return Value(a.asInt() - b.asInt());
}
Value operator*(const Value& a, const Value& b) {
    if (either_is_float(a, b)) return Value(a.asFloat() * b.asFloat());
    return Value(a.asInt() * b.asInt());
}
Value operator/(const Value& a, const Value& b) {
    if (either_is_float(a, b)) {
        double denom = b.asFloat();
        return Value(denom == 0.0 ? 0.0 : a.asFloat() / denom);
    }
    int64_t denom = b.asInt();
    return Value(denom == 0 ? int64_t{0} : a.asInt() / denom);
}
Value operator%(const Value& a, const Value& b) {
    if (either_is_float(a, b)) {
        double denom = b.asFloat();
        return Value(denom == 0.0 ? 0.0 : std::fmod(a.asFloat(), denom));
    }
    int64_t denom = b.asInt();
    return Value(denom == 0 ? int64_t{0} : a.asInt() % denom);
}

Value Value::operator-() const {
    if (type() == ValueType::Float) return Value(-std::get<double>(data_));
    return Value(-asInt());
}
Value Value::operator!() const { return Value(!truthy()); }
Value Value::operator~() const { return Value(~asInt()); }

bool Value::equals(const Value& other) const {
    if (isNumber() && other.isNumber()) {
        if (type() == ValueType::Float || other.type() == ValueType::Float) {
            return asFloat() == other.asFloat();
        }
        return asInt() == other.asInt();
    }
    if (type() != other.type()) return false;
    switch (type()) {
        case ValueType::Null: return true;
        case ValueType::Bool: return asBool() == other.asBool();
        case ValueType::String: return std::get<std::string>(data_) == std::get<std::string>(other.data_);
        case ValueType::Handle: return std::get<Handle>(data_) == std::get<Handle>(other.data_);
        case ValueType::Array: return std::get<std::shared_ptr<ValueArray>>(data_) == std::get<std::shared_ptr<ValueArray>>(other.data_);
        case ValueType::Object: return std::get<std::shared_ptr<ValueObject>>(data_) == std::get<std::shared_ptr<ValueObject>>(other.data_);
        default: return false;
    }
}

Value operator==(const Value& a, const Value& b) { return Value(a.equals(b)); }
Value operator!=(const Value& a, const Value& b) { return Value(!a.equals(b)); }
Value operator<(const Value& a, const Value& b) {
    if (either_is_string(a, b)) return Value(a.asString() < b.asString());
    return Value(a.asFloat() < b.asFloat());
}
Value operator>(const Value& a, const Value& b) {
    if (either_is_string(a, b)) return Value(a.asString() > b.asString());
    return Value(a.asFloat() > b.asFloat());
}
Value operator<=(const Value& a, const Value& b) {
    if (either_is_string(a, b)) return Value(a.asString() <= b.asString());
    return Value(a.asFloat() <= b.asFloat());
}
Value operator>=(const Value& a, const Value& b) {
    if (either_is_string(a, b)) return Value(a.asString() >= b.asString());
    return Value(a.asFloat() >= b.asFloat());
}

std::ostream& operator<<(std::ostream& os, const Value& v) {
    return os << v.asString();
}

Value make_array(std::initializer_list<Value> elems) {
    return Value(ValueArray(elems));
}

Value make_object(std::initializer_list<std::pair<std::string, Value>> fields) {
    Value out = Value::MakeObject();
    auto& obj = out.objectRef();
    // Insertion order matters here only for duplicate keys: later wins,
    // matching how the codegen warning describes it (and how every language
    // with object literals behaves). operator[] assignment gives that for
    // free; emplace would silently keep the FIRST.
    for (const auto& f : fields) obj[f.first] = f.second;
    return out;
}

int64_t iter_count(const Value& v) {
    switch (v.type()) {
        case ValueType::Array:
            return static_cast<int64_t>(std::get<std::shared_ptr<ValueArray>>(v.data_)->size());
        case ValueType::Object:
            return static_cast<int64_t>(std::get<std::shared_ptr<ValueObject>>(v.data_)->size());
        case ValueType::String:
            return static_cast<int64_t>(std::get<std::string>(v.data_).size());
        default:
            // Null, numbers, bools, handles: zero iterations rather than an
            // error. `for (x in PlS::GetCollisionsForEntity(id))` on a frame
            // with no collisions should simply not run its body.
            return 0;
    }
}

Value iter_at(const Value& v, int64_t index) {
    if (index < 0) return Value();
    switch (v.type()) {
        case ValueType::Array: {
            auto& arr = *std::get<std::shared_ptr<ValueArray>>(v.data_);
            if (static_cast<size_t>(index) >= arr.size()) return Value();
            return arr[static_cast<size_t>(index)];
        }
        case ValueType::Object: {
            // Objects iterate their KEYS. ValueObject is a std::map, so the
            // order is the keys' sort order -- stable across runs and across
            // both ends of a connection, which matters because a script that
            // iterates a state packet's entities on two different machines
            // should visit them in the same order.
            auto& obj = *std::get<std::shared_ptr<ValueObject>>(v.data_);
            if (static_cast<size_t>(index) >= obj.size()) return Value();
            auto it = obj.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(index));
            return Value(it->first);
        }
        case ValueType::String: {
            auto& str = std::get<std::string>(v.data_);
            if (static_cast<size_t>(index) >= str.size()) return Value();
            return Value(std::string(1, str[static_cast<size_t>(index)]));
        }
        default:
            return Value();
    }
}

Value index_get(const Value& base, const Value& idx) {
    if (base.isArray()) {
        auto& arr = *std::get<std::shared_ptr<ValueArray>>(base.data_);
        int64_t i = idx.asInt();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) return Value();
        return arr[static_cast<size_t>(i)];
    }
    if (base.isObject()) {
        return member_get(base, idx.asString());
    }
    if (base.type() == ValueType::String) {
        auto& s = std::get<std::string>(base.data_);
        int64_t i = idx.asInt();
        if (i < 0 || static_cast<size_t>(i) >= s.size()) return Value();
        return Value(std::string(1, s[static_cast<size_t>(i)]));
    }
    return Value();
}

Value member_get(const Value& base, const std::string& name) {
    if (!base.isObject()) return Value();
    auto& obj = *std::get<std::shared_ptr<ValueObject>>(base.data_);
    auto it = obj.find(name);
    if (it == obj.end()) return Value();
    return it->second;
}

// Upper bound on how far `arr[i] = v` will grow an array in one step.
// index_ref must resize to reach element i, so an index derived from a bad
// computation -- or, on a server, from a hostile client packet -- would
// otherwise request a multi-gigabyte allocation and abort the whole process
// with an uncaught std::bad_alloc. For a long-running authoritative server
// that is a remote denial of service, not just a crashed script.
static constexpr int64_t kMaxArrayGrowth = 16 * 1024 * 1024;

// Returned by reference when a write cannot be honoured. Writes to it are
// discarded, keeping the failure non-fatal: the tick logs an error and the
// server keeps serving every other player.
static Value& discardSlot() {
    static Value scratch;
    scratch = Value();
    return scratch;
}

Value& index_ref(Value& base, const Value& idx) {
    // A string subscript is a *key*, not a slot. index_get already treats
    // `o["name"]` on an object as a member read, so the write path has to
    // match -- otherwise arrayRef() below silently discards the entire object
    // and replaces it with a one-element array, losing every other field with
    // no diagnostic. This matters more in PlainS than in the client: `.pls`
    // has object literals as a first-class construct, so objects are
    // everywhere, and every inbound player input is one.
    // Null auto-vivifies to an object (rather than an array) for the same
    // reason: `o["k"] = 1` and `o.k = 1` must produce the same thing.
    if (base.isObject() || (base.isNull() && idx.type() == ValueType::String)) {
        return member_ref(base, idx.asString());
    }
    auto& arr = base.arrayRef(); // auto-vivifies Null -> empty array
    int64_t i = idx.asInt();
    if (i < 0) i = 0;
    if (i >= kMaxArrayGrowth) {
        std::fprintf(stderr,
                     "[PlainS][ERROR] array index %lld is beyond the maximum supported size "
                     "(%lld); the assignment was discarded. If this index came from client "
                     "input, validate it before use.\n",
                     static_cast<long long>(i), static_cast<long long>(kMaxArrayGrowth));
        return discardSlot();
    }
    if (static_cast<size_t>(i) >= arr.size()) {
        try {
            arr.resize(static_cast<size_t>(i) + 1);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[PlainS][ERROR] could not grow array to %lld elements (%s); the "
                         "assignment was discarded.\n",
                         static_cast<long long>(i) + 1, e.what());
            return discardSlot();
        }
    }
    return arr[static_cast<size_t>(i)];
}

Value& member_ref(Value& base, const std::string& name) {
    auto& obj = base.objectRef(); // auto-vivifies Null -> empty object
    return obj[name];
}

} // namespace pls
