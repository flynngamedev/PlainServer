// pls_value.h -- the dynamic value type every PlainS variable compiles
// to. PlainS has no type annotations anywhere in its syntax (var/let/
// const declarations never name a type), so the C++ code generator targets
// this single dynamically-typed `pls::Value` class rather than trying to
// infer static C++ types. Think of it as a small, self-contained
// tagged-union value type in the spirit of a scripting language runtime
// (a bit like a minimal QVariant / JS value / Lua value).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace pls {

class Value;
using ValueArray = std::vector<Value>;
using ValueObject = std::map<std::string, Value>;

enum class ValueType { Null, Bool, Int, Float, String, Array, Object, Handle };

const char* type_name(ValueType t);

// A Handle is an opaque runtime resource id. On the server the only kind in
// practice is "entity", but the field is kept (rather than collapsing
// handles to bare ints) so that a value crossing the wire can be told apart
// from one that names something in the world -- and so this type stays
// interchangeable with PlainVulkan's, which is what lets both ends of a
// connection share one JSON conversion.
struct Handle {
    uint64_t id = 0;
    std::string kind;
    bool operator==(const Handle& o) const { return id == o.id && kind == o.kind; }
};

class Value {
public:
    Value() : data_(std::monostate{}) {}
    Value(std::nullptr_t) : data_(std::monostate{}) {}
    Value(bool b) : data_(b) {}
    Value(int i) : data_(static_cast<int64_t>(i)) {}
    Value(int64_t i) : data_(i) {}
    Value(size_t i) : data_(static_cast<int64_t>(i)) {}
    Value(double d) : data_(d) {}
    Value(float f) : data_(static_cast<double>(f)) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(std::string s) : data_(std::move(s)) {}
    Value(ValueArray arr) : data_(std::make_shared<ValueArray>(std::move(arr))) {}
    Value(Handle h) : data_(std::move(h)) {}

    // A function pointer converts implicitly to bool, so without this
    // deletion `Value(SomeFunction)` would compile and quietly store `true`.
    // Generated code emits user functions as bare C++ names, which makes that
    // an easy mistake (`Print(MyHandler)` instead of `Print(MyHandler())`) and
    // a very hard one to notice. plscc rejects it with a source-level
    // diagnostic; this guarantees it can never silently succeed even if a
    // path there is ever missed.
    //
    // The type parameters are deliberately not short names like `R`/`A`:
    // single-uppercase-letter identifiers are a common macro name in vendored
    // C headers, and a macro-expanded template parameter is a baffling error.
    template <class PlsFnRet, class... PlsFnArgs>
    Value(PlsFnRet (*)(PlsFnArgs...)) = delete;

    static Value MakeArray() { return Value(std::make_shared<ValueArray>()); }
    static Value MakeObject() { return Value(std::make_shared<ValueObject>()); }
    static Value MakeHandle(uint64_t id, std::string kind) { return Value(Handle{id, std::move(kind)}); }

    ValueType type() const;
    bool isNull() const { return type() == ValueType::Null; }
    bool isNumber() const { return type() == ValueType::Int || type() == ValueType::Float; }
    bool isArray() const { return type() == ValueType::Array; }
    bool isObject() const { return type() == ValueType::Object; }
    bool isHandle() const { return type() == ValueType::Handle; }

    bool asBool() const;
    int64_t asInt() const;
    double asFloat() const;
    std::string asString() const; // stringifies numbers/bools too, for Log()/concat convenience
    const Handle& asHandle() const;

    // Mutable access to the underlying containers. Auto-vivifies: calling
    // this on a Null value turns it into an (empty) array/object in place,
    // which is what lets `index_ref`/`member_ref` (below) support
    // `arr[i] = v` / `obj.field = v` even when `arr`/`obj` started out
    // uninitialized ("var arr;" then later indexed).
    ValueArray& arrayRef();
    ValueObject& objectRef();

    bool truthy() const;
    explicit operator bool() const { return truthy(); }

    Value operator-() const; // unary neg
    Value operator!() const; // logical not
    Value operator~() const; // bitwise not (ints only)

    bool equals(const Value& other) const;

    friend Value operator+(const Value& a, const Value& b);
    friend Value operator-(const Value& a, const Value& b);
    friend Value operator*(const Value& a, const Value& b);
    friend Value operator/(const Value& a, const Value& b);
    friend Value operator%(const Value& a, const Value& b);

    // NOTE: deliberately no operator&&/operator|| overloads. Overloaded
    // C++ operators can never short-circuit (both operands always get
    // evaluated first), so codegen emits `Value((bool)a && (bool)b)`
    // using Value's `explicit operator bool()` instead, which *does*
    // short-circuit correctly via native C++ &&/||.

    friend Value operator==(const Value& a, const Value& b);
    friend Value operator!=(const Value& a, const Value& b);
    friend Value operator<(const Value& a, const Value& b);
    friend Value operator>(const Value& a, const Value& b);
    friend Value operator<=(const Value& a, const Value& b);
    friend Value operator>=(const Value& a, const Value& b);

    friend std::ostream& operator<<(std::ostream& os, const Value& v);

    friend Value index_get(const Value& base, const Value& idx);
    friend Value member_get(const Value& base, const std::string& name);

    // The for-in loop's backing. Friends (rather than users of arrayRef/
    // objectRef) specifically so they can inspect a value WITHOUT mutating
    // it: arrayRef auto-vivifies, so iterating a Null through it would turn
    // that Null into an empty array as a side effect of merely looking at
    // it -- and `for (x in someNullField)` must not modify someNullField.
    friend int64_t iter_count(const Value& v);
    friend Value iter_at(const Value& v, int64_t index);

private:
    std::variant<
        std::monostate,
        bool,
        int64_t,
        double,
        std::string,
        std::shared_ptr<ValueArray>,
        std::shared_ptr<ValueObject>,
        Handle>
        data_;

    explicit Value(std::shared_ptr<ValueArray> a) : data_(std::move(a)) {}
    explicit Value(std::shared_ptr<ValueObject> o) : data_(std::move(o)) {}
};

// --- iteration ----------------------------------------------------------
// Backing for the `for (x in y)` loop. Arrays iterate their elements;
// objects iterate their KEYS (so `for (id in state["entities"])` gives ids,
// matching how the wire format is shaped); a string iterates its
// characters; anything else has a count of 0 and iterates zero times rather
// than being an error -- a loop over a null result should do nothing, which
// is what a script means by it.
int64_t iter_count(const Value& v);

// Bounds-checked: returns Null past the end rather than throwing, so a
// container that shrinks mid-loop cannot fault.
Value iter_at(const Value& v, int64_t index);

Value make_array(std::initializer_list<Value> elems);
Value make_object(std::initializer_list<std::pair<std::string, Value>> fields);

// Read helpers: safe (never throw), return Null on any mismatch.
Value index_get(const Value& base, const Value& idx);
Value member_get(const Value& base, const std::string& name);

// Write/reference helpers: auto-vivify (a Null base becomes an array/
// object in place) and return a mutable reference so `a.b[0].c = v`-style
// chains compile to a single chained expression. See codegen's handling
// of `LValue` in codegen.rs for how these get chained.
Value& index_ref(Value& base, const Value& idx);
Value& member_ref(Value& base, const std::string& name);

// --- bare-name builtins -------------------------------------------------
// Callable from a script without a namespace: `Print(x)`, `Len(arr)`.
// These live in their own namespace (rather than in pls::rt with the PlS::
// commands) because they are language conveniences, not server operations,
// and keeping them apart is what lets scripts/check_command_coverage.sh
// hold pls_runtime.h to exactly the documented PlS:: command set.
namespace builtin {

Value Print(const Value& message);
Value Len(const Value& v);
Value Keys(const Value& obj);
Value Has(const Value& obj, const Value& key);
Value Push(const Value& arr, const Value& v);
Value Str(const Value& v);
Value Int(const Value& v);
Value Float(const Value& v);
Value Abs(const Value& v);
Value Min(const Value& a, const Value& b);
Value Max(const Value& a, const Value& b);
Value Sqrt(const Value& v);
Value Floor(const Value& v);
Value Ceil(const Value& v);
Value Random();

} // namespace builtin

} // namespace pls
