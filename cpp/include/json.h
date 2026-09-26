// ASQS - minimal JSON: parse + serialize (pretty & canonical).
// Canonical form is normative for all embedded hashes (PROOF SPEC):
//   keys sorted, no whitespace, strings escaped exactly like Python's
//   json.dumps(..., sort_keys=True, separators=(',',':'), ensure_ascii=False).
// Doubles are FORBIDDEN in canonical form (enforced: jcanonical returns "" if
// a double is encountered). Integers are int64.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace asqs {

struct JValue {
    enum class Type { Null, Bool, Int, Dbl, Str, Arr, Obj };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj; // insertion order preserved

    static JValue mk_null() { return JValue(); }
    static JValue mk_bool(bool v) { JValue j; j.type = Type::Bool; j.b = v; return j; }
    static JValue mk_int(int64_t v) { JValue j; j.type = Type::Int; j.i = v; return j; }
    static JValue mk_dbl(double v) { JValue j; j.type = Type::Dbl; j.d = v; return j; }
    static JValue mk_str(std::string v) { JValue j; j.type = Type::Str; j.s = std::move(v); return j; }
    static JValue mk_arr() { JValue j; j.type = Type::Arr; return j; }
    static JValue mk_obj() { JValue j; j.type = Type::Obj; return j; }

    bool is_obj() const { return type == Type::Obj; }
    bool is_arr() const { return type == Type::Arr; }
    bool is_str() const { return type == Type::Str; }
    bool is_int() const { return type == Type::Int; }
    bool is_num() const { return type == Type::Int || type == Type::Dbl; }
    bool is_null() const { return type == Type::Null; }

    bool has(const std::string& k) const;
    const JValue* get(const std::string& k) const;   // nullptr if absent
    JValue& set(const std::string& k, JValue v);     // insert or replace
    void push(JValue v) { arr.push_back(std::move(v)); }

    int64_t as_int(int64_t def = 0) const;
    std::string as_str(const std::string& def = "") const;
    bool as_bool(bool def = false) const;
    double as_double(double def = 0) const;
};

// Parse a complete JSON document. Returns "" on success, error description otherwise.
std::string jparse(const std::string& text, JValue& out);

// Serialize. pretty=true -> indented human format (files); pretty=false -> compact.
// Canonical (normative for hashing): sorted keys + compact.
std::string jserialize(const JValue& v, bool pretty);
std::string jcanonical(const JValue& v); // "" if a double is present

} // namespace asqs
