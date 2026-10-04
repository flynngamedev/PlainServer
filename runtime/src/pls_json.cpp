// pls_json.cpp -- implementation of the small JSON reader/writer declared
// in pls_json.h. Recursive-descent parser over a std::string, with a depth
// cap so a hand-edited (or hostile) scene file of nested arrays can't blow
// the C++ stack.
#include "pls/pls_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pls {
namespace json {

namespace {

constexpr int kMaxDepth = 128;

struct Parser {
    const std::string& src;
    size_t pos = 0;
    std::string error;

    explicit Parser(const std::string& s) : src(s) {}

    bool fail(const std::string& msg) {
        if (error.empty()) {
            // Report a line/column rather than a byte offset -- a scene file
            // is hand-editable, so the error needs to point at a place a
            // human can find in a text editor.
            size_t line = 1, col = 1;
            for (size_t i = 0; i < pos && i < src.size(); i++) {
                if (src[i] == '\n') { line++; col = 1; } else { col++; }
            }
            error = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + msg;
        }
        return false;
    }

    void skipWs() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++;
            else break;
        }
    }

    bool atEnd() const { return pos >= src.size(); }
    char peek() const { return pos < src.size() ? src[pos] : '\0'; }

    bool literal(const char* lit) {
        size_t n = std::char_traits<char>::length(lit);
        if (src.compare(pos, n, lit) != 0) return false;
        pos += n;
        return true;
    }

    bool parseValue(Json& out, int depth);

    bool parseString(std::string& out) {
        if (peek() != '"') return fail("expected a string");
        pos++;
        out.clear();
        while (true) {
            if (atEnd()) return fail("unterminated string");
            char c = src[pos++];
            if (c == '"') return true;
            if (c != '\\') {
                // Raw control characters are illegal in JSON strings; catching
                // it here gives a precise message instead of silently
                // producing a file other parsers reject.
                if (static_cast<unsigned char>(c) < 0x20) return fail("raw control character in string");
                out.push_back(c);
                continue;
            }
            if (atEnd()) return fail("unterminated escape sequence");
            char e = src[pos++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!readHex4(cp)) return false;
                    // Surrogate pair: a code point above the BMP is encoded as
                    // two \u escapes, and joining them is required to get the
                    // right UTF-8 out (asset paths with emoji or CJK beyond
                    // the BMP would otherwise be mangled).
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos + 1 < src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                            pos += 2;
                            uint32_t lo = 0;
                            if (!readHex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                // Unpaired high surrogate followed by some other
                                // escape: emit U+FFFD for the lone surrogate and
                                // let the next one be handled on its own.
                                appendUtf8(out, 0xFFFD);
                                cp = lo;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD; // lone low surrogate
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("unknown escape sequence");
            }
        }
    }

    bool readHex4(uint32_t& out) {
        if (pos + 4 > src.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; i++) {
            char c = src[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
        }
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseNumber(Json& out) {
        size_t start = pos;
        if (peek() == '-') pos++;
        if (peek() == '0') {
            pos++;
        } else if (isDigit(peek())) {
            while (isDigit(peek())) pos++;
        } else {
            return fail("expected a number");
        }
        if (peek() == '.') {
            pos++;
            if (!isDigit(peek())) return fail("expected a digit after the decimal point");
            while (isDigit(peek())) pos++;
        }
        if (peek() == 'e' || peek() == 'E') {
            pos++;
            if (peek() == '+' || peek() == '-') pos++;
            if (!isDigit(peek())) return fail("expected a digit in the exponent");
            while (isDigit(peek())) pos++;
        }
        // strtod on the isolated token. Using the C locale-independent path
        // matters: a European locale's LC_NUMERIC would otherwise make
        // strtod stop at the '.' and silently truncate every float in the
        // scene to its integer part.
        std::string tok = src.substr(start, pos - start);
        out = Json(std::strtod(tok.c_str(), nullptr));
        return true;
    }

    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
};

bool Parser::parseValue(Json& out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    skipWs();
    if (atEnd()) return fail("unexpected end of input");

    char c = peek();
    switch (c) {
        case 'n':
            if (!literal("null")) return fail("expected 'null'");
            out = Json(nullptr);
            return true;
        case 't':
            if (!literal("true")) return fail("expected 'true'");
            out = Json(true);
            return true;
        case 'f':
            if (!literal("false")) return fail("expected 'false'");
            out = Json(false);
            return true;
        case '"': {
            std::string s;
            if (!parseString(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        case '[': {
            pos++;
            JsonArray arr;
            skipWs();
            if (peek() == ']') { pos++; out = Json(std::move(arr)); return true; }
            while (true) {
                Json elem;
                if (!parseValue(elem, depth + 1)) return false;
                arr.push_back(std::move(elem));
                skipWs();
                if (peek() == ',') { pos++; continue; }
                if (peek() == ']') { pos++; break; }
                return fail("expected ',' or ']' in array");
            }
            out = Json(std::move(arr));
            return true;
        }
        case '{': {
            pos++;
            JsonObject obj;
            skipWs();
            if (peek() == '}') { pos++; out = Json(std::move(obj)); return true; }
            while (true) {
                skipWs();
                std::string key;
                if (!parseString(key)) return false;
                skipWs();
                if (peek() != ':') return fail("expected ':' after object key");
                pos++;
                Json val;
                if (!parseValue(val, depth + 1)) return false;
                obj[std::move(key)] = std::move(val);
                skipWs();
                if (peek() == ',') { pos++; continue; }
                if (peek() == '}') { pos++; break; }
                return fail("expected ',' or '}' in object");
            }
            out = Json(std::move(obj));
            return true;
        }
        default:
            return parseNumber(out);
    }
}

} // namespace

Json Json::parse(const std::string& text, std::string* error) {
    Parser p(text);
    Json root;
    if (!p.parseValue(root, 0)) {
        if (error) *error = p.error;
        return Json();
    }
    p.skipWs();
    if (!p.atEnd()) {
        p.fail("trailing content after the top-level value");
        if (error) *error = p.error;
        return Json();
    }
    if (error) error->clear();
    return root;
}

void Json::writeEscaped(std::ostringstream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    // Control characters must be escaped. Everything >= 0x20
                    // is passed through as-is, which keeps already-valid UTF-8
                    // byte sequences intact rather than re-encoding them.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

void Json::write(std::ostringstream& os, int indent, int depth) const {
    const bool pretty = indent >= 0;
    auto newlineIndent = [&](int d) {
        if (!pretty) return;
        os << '\n';
        for (int i = 0; i < d * indent; i++) os << ' ';
    };

    switch (type_) {
        case JType::Null: os << "null"; break;
        case JType::Bool: os << (bool_ ? "true" : "false"); break;
        case JType::Number: {
            // Round-trip formatting: %.17g is the shortest precision
            // guaranteed to reproduce any double exactly, but it renders 0.1
            // as 0.10000000000000001 which makes a scene file unpleasant to
            // read. So try %.9g first (enough for float, which is what every
            // number in a scene actually is) and only fall back if it
            // wouldn't round-trip.
            if (std::isnan(num_) || std::isinf(num_)) {
                os << "0"; // JSON has no NaN/Inf; writing 0 keeps the file valid
                break;
            }
            if (num_ == static_cast<double>(static_cast<int64_t>(num_)) &&
                std::fabs(num_) < 1e15) {
                os << static_cast<int64_t>(num_);
                break;
            }
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.9g", num_);
            if (std::strtod(buf, nullptr) != num_) {
                std::snprintf(buf, sizeof(buf), "%.17g", num_);
            }
            os << buf;
            break;
        }
        case JType::String: writeEscaped(os, str_); break;
        case JType::Array: {
            if (arr_.empty()) { os << "[]"; break; }
            // Arrays of plain numbers (every vector and every keyframe track)
            // stay on one line -- pretty-printing a 300-element float array
            // one number per line makes a scene file unreadable and unmergeable.
            bool allScalar = true;
            for (const auto& e : arr_) {
                if (e.type_ == JType::Array || e.type_ == JType::Object) { allScalar = false; break; }
            }
            if (allScalar) {
                os << '[';
                for (size_t i = 0; i < arr_.size(); i++) {
                    if (i) os << (pretty ? ", " : ",");
                    arr_[i].write(os, -1, 0);
                }
                os << ']';
                break;
            }
            os << '[';
            for (size_t i = 0; i < arr_.size(); i++) {
                if (i) os << ',';
                newlineIndent(depth + 1);
                arr_[i].write(os, indent, depth + 1);
            }
            newlineIndent(depth);
            os << ']';
            break;
        }
        case JType::Object: {
            if (obj_.empty()) { os << "{}"; break; }
            os << '{';
            bool first = true;
            for (const auto& kv : obj_) {
                if (!first) os << ',';
                first = false;
                newlineIndent(depth + 1);
                writeEscaped(os, kv.first);
                os << (pretty ? ": " : ":");
                kv.second.write(os, indent, depth + 1);
            }
            newlineIndent(depth);
            os << '}';
            break;
        }
    }
}

} // namespace json
} // namespace pls
