// pls_json.h -- a small, self-contained JSON reader/writer used by the
// scene format and the project file.
//
// Why not vendor nlohmann/json or RapidJSON: the runtime already carries
// five vendored single-header libraries totalling ~5MB, and this needs to
// parse and emit network payloads and nothing else -- no streaming, no SAX, no
// arbitrary-precision numbers, no schema validation. ~400 lines that
// compile in a blink is a better trade here than another megabyte of
// header that every translation unit including the runtime would pay for.
//
// Conformance: parses the RFC 8259 grammar including nested containers,
// all six escape forms plus \uXXXX (with surrogate-pair joining, encoded
// out as UTF-8), and the full number grammar. It does *not* accept
// comments, trailing commas, or NaN/Infinity -- writing those into a scene
// file would produce something other tools can't read.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace pls {
namespace json {

class Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::map<std::string, Json>; // ordered, so output is stable and diffable

enum class JType { Null, Bool, Number, String, Array, Object };

class Json {
public:
    Json() : type_(JType::Null) {}
    Json(std::nullptr_t) : type_(JType::Null) {}
    Json(bool b) : type_(JType::Bool), bool_(b) {}
    Json(int v) : type_(JType::Number), num_(static_cast<double>(v)) {}
    Json(int64_t v) : type_(JType::Number), num_(static_cast<double>(v)) {}
    Json(uint64_t v) : type_(JType::Number), num_(static_cast<double>(v)) {}
    Json(float v) : type_(JType::Number), num_(static_cast<double>(v)) {}
    Json(double v) : type_(JType::Number), num_(v) {}
    Json(const char* s) : type_(JType::String), str_(s) {}
    Json(std::string s) : type_(JType::String), str_(std::move(s)) {}
    Json(JsonArray a) : type_(JType::Array), arr_(std::move(a)) {}
    Json(JsonObject o) : type_(JType::Object), obj_(std::move(o)) {}

    static Json array() { return Json(JsonArray{}); }
    static Json object() { return Json(JsonObject{}); }

    JType type() const { return type_; }
    bool isNull() const { return type_ == JType::Null; }
    bool isBool() const { return type_ == JType::Bool; }
    bool isNumber() const { return type_ == JType::Number; }
    bool isString() const { return type_ == JType::String; }
    bool isArray() const { return type_ == JType::Array; }
    bool isObject() const { return type_ == JType::Object; }

    // Typed accessors with defaults. Every one of these is total -- reading
    // a missing or wrong-typed field yields the fallback rather than
    // throwing, because a scene file that lost one field to a format change
    // should still load with a sane value for it instead of failing whole.
    bool asBool(bool def = false) const { return type_ == JType::Bool ? bool_ : def; }
    double asDouble(double def = 0.0) const { return type_ == JType::Number ? num_ : def; }
    float asFloat(float def = 0.0f) const {
        return type_ == JType::Number ? static_cast<float>(num_) : def;
    }
    int asInt(int def = 0) const { return type_ == JType::Number ? static_cast<int>(num_) : def; }
    uint64_t asUInt64(uint64_t def = 0) const {
        return type_ == JType::Number ? static_cast<uint64_t>(num_) : def;
    }
    std::string asString(const std::string& def = "") const {
        return type_ == JType::String ? str_ : def;
    }

    const JsonArray& asArray() const {
        static const JsonArray kEmpty;
        return type_ == JType::Array ? arr_ : kEmpty;
    }
    const JsonObject& asObject() const {
        static const JsonObject kEmpty;
        return type_ == JType::Object ? obj_ : kEmpty;
    }

    // Mutating access. Auto-vivifies to the right container type, so
    // building a document reads as straight-line assignment.
    Json& operator[](const std::string& key) {
        if (type_ != JType::Object) { type_ = JType::Object; obj_.clear(); }
        return obj_[key];
    }
    const Json& operator[](const std::string& key) const {
        static const Json kNull;
        if (type_ != JType::Object) return kNull;
        auto it = obj_.find(key);
        return it == obj_.end() ? kNull : it->second;
    }
    const Json& operator[](size_t i) const {
        static const Json kNull;
        if (type_ != JType::Array || i >= arr_.size()) return kNull;
        return arr_[i];
    }

    bool has(const std::string& key) const {
        return type_ == JType::Object && obj_.find(key) != obj_.end();
    }

    void push(Json v) {
        if (type_ != JType::Array) { type_ = JType::Array; arr_.clear(); }
        arr_.push_back(std::move(v));
    }

    size_t size() const {
        if (type_ == JType::Array) return arr_.size();
        if (type_ == JType::Object) return obj_.size();
        return 0;
    }

    // `indent < 0` writes compact output; >= 0 pretty-prints with that many
    // spaces per level. Scenes are saved pretty-printed so they diff well.
    std::string dump(int indent = 2) const {
        std::ostringstream os;
        write(os, indent, 0);
        return os.str();
    }

    // Returns Null and sets `error` on malformed input.
    static Json parse(const std::string& text, std::string* error = nullptr);

private:
    JType type_;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    JsonArray arr_;
    JsonObject obj_;

    void write(std::ostringstream& os, int indent, int depth) const;
    static void writeEscaped(std::ostringstream& os, const std::string& s);
};

// --- convenience builders for the vector/quaternion types --------------
// Vectors serialize as flat arrays ([x, y, z]) rather than objects with
// x/y/z keys: it's a third the bytes, and a scene file is mostly vectors.

template <typename V3>
inline Json vec3ToJson(const V3& v) {
    JsonArray a{Json(v.x), Json(v.y), Json(v.z)};
    return Json(std::move(a));
}
template <typename V3>
inline V3 jsonToVec3(const Json& j, V3 def = V3{}) {
    if (!j.isArray() || j.size() < 3) return def;
    return V3{j[0].asFloat(def.x), j[1].asFloat(def.y), j[2].asFloat(def.z)};
}

template <typename V4>
inline Json vec4ToJson(const V4& v) {
    JsonArray a{Json(v.x), Json(v.y), Json(v.z), Json(v.w)};
    return Json(std::move(a));
}
template <typename V4>
inline V4 jsonToVec4(const Json& j, V4 def = V4{}) {
    if (!j.isArray() || j.size() < 4) return def;
    return V4{j[0].asFloat(def.x), j[1].asFloat(def.y), j[2].asFloat(def.z), j[3].asFloat(def.w)};
}

} // namespace json
} // namespace pls
