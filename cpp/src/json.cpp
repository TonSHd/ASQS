#include "json.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace asqs {

bool JValue::has(const std::string& k) const {
    if (type != Type::Obj) return false;
    for (auto& kv : obj) if (kv.first == k) return true;
    return false;
}

const JValue* JValue::get(const std::string& k) const {
    if (type != Type::Obj) return nullptr;
    for (auto& kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
}

JValue& JValue::set(const std::string& k, JValue v) {
    for (auto& kv : obj) {
        if (kv.first == k) { kv.second = std::move(v); return kv.second; }
    }
    obj.emplace_back(k, std::move(v));
    return obj.back().second;
}

int64_t JValue::as_int(int64_t def) const {
    if (type == Type::Int) return i;
    if (type == Type::Dbl) return int64_t(d);
    if (type == Type::Bool) return b ? 1 : 0;
    return def;
}
std::string JValue::as_str(const std::string& def) const { return type == Type::Str ? s : def; }
bool JValue::as_bool(bool def) const { return type == Type::Bool ? b : def; }
double JValue::as_double(double def) const {
    if (type == Type::Dbl) return d;
    if (type == Type::Int) return double(i);
    return def;
}

// ---------------- parser ----------------
namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;
    std::string err;

    bool fail(const std::string& m) {
        if (err.empty()) err = m;
        return false;
    }
    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool parse(JValue& out) {
        if (++depth > 64) return fail("nesting too deep");
        skip_ws();
        if (p >= end) return fail("unexpected end");
        bool ok = false;
        char c = *p;
        if (c == '{') ok = parse_obj(out);
        else if (c == '[') ok = parse_arr(out);
        else if (c == '"') { out = JValue::mk_null(); ok = parse_str(out.s); out.type = JValue::Type::Str; }
        else if (c == 't') ok = parse_lit("true", out, JValue::mk_bool(true));
        else if (c == 'f') ok = parse_lit("false", out, JValue::mk_bool(false));
        else if (c == 'n') ok = parse_lit("null", out, JValue::mk_null());
        else ok = parse_num(out);
        --depth;
        return ok;
    }
    bool parse_lit(const char* lit, JValue& out, JValue v) {
        size_t n = std::strlen(lit);
        if (size_t(end - p) < n || std::strncmp(p, lit, n) != 0) return fail("bad literal");
        p += n;
        out = std::move(v);
        return true;
    }
    bool parse_num(JValue& out) {
        const char* start = p;
        if (p < end && *p == '-') ++p;
        bool isfloat = false;
        while (p < end) {
            char c = *p;
            if (c >= '0' && c <= '9') ++p;
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { isfloat = true; ++p; }
            else break;
        }
        if (p == start || (p == start + 1 && *start == '-')) return fail("bad number");
        std::string tok(start, p);
        if (isfloat) {
            out = JValue::mk_dbl(std::strtod(tok.c_str(), nullptr));
        } else {
            errno = 0;
            char* e = nullptr;
            long long v = std::strtoll(tok.c_str(), &e, 10);
            if (errno == ERANGE) out = JValue::mk_dbl(std::strtod(tok.c_str(), nullptr));
            else out = JValue::mk_int(int64_t(v));
        }
        return true;
    }
    bool parse_str(std::string& out) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        out.clear();
        while (p < end) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') { ++p; return true; }
            if (c == '\\') {
                ++p;
                if (p >= end) return fail("bad escape");
                char e = *p++;
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
                        if (!parse_hex4(cp)) return fail("bad \\u");
                        if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            uint32_t lo = 0;
                            if (!parse_hex4(lo)) return fail("bad \\u");
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else return fail("bad surrogate");
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return fail("bad escape char");
                }
            } else if (c < 0x20) {
                return fail("control char in string");
            } else {
                out.push_back(char(c));
                ++p;
            }
        }
        return fail("unterminated string");
    }
    bool parse_hex4(uint32_t& out) {
        if (end - p < 4) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }
    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out.push_back(char(cp));
        else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }
    bool parse_arr(JValue& out) {
        out = JValue::mk_arr();
        ++p; // [
        skip_ws();
        if (p < end && *p == ']') { ++p; return true; }
        while (true) {
            JValue v;
            if (!parse(v)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return fail("expected , or ]");
        }
    }
    bool parse_obj(JValue& out) {
        out = JValue::mk_obj();
        ++p; // {
        skip_ws();
        if (p < end && *p == '}') { ++p; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_str(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected :");
            ++p;
            JValue v;
            if (!parse(v)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return fail("expected , or }");
        }
    }
};

void escape_string(const std::string& s, std::string& out) {
    // Matches Python json.dumps(ensure_ascii=False) escaping.
    static const char* hexd = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hexd[c >> 4]);
                    out.push_back(hexd[c & 0xf]);
                } else {
                    out.push_back(char(c));
                }
        }
    }
    out.push_back('"');
}

bool serialize_impl(const JValue& v, bool pretty, int indent, bool canonical, std::string& out,
                    std::string& err) {
    auto nl = [&](int level) {
        if (pretty) {
            out.push_back('\n');
            out.append(size_t(level) * 2, ' ');
        }
    };
    switch (v.type) {
        case JValue::Type::Null: out += "null"; break;
        case JValue::Type::Bool: out += v.b ? "true" : "false"; break;
        case JValue::Type::Int: {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
            out += buf;
            break;
        }
        case JValue::Type::Dbl:
            if (canonical) { err = "double in canonical form"; return false; }
            {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.17g", v.d);
                out += buf;
            }
            break;
        case JValue::Type::Str: escape_string(v.s, out); break;
        case JValue::Type::Arr: {
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out.push_back(',');
                nl(indent + 1);
                if (!serialize_impl(v.arr[i], pretty, indent + 1, canonical, out, err)) return false;
            }
            if (!v.arr.empty()) nl(indent);
            out.push_back(']');
            break;
        }
        case JValue::Type::Obj: {
            out.push_back('{');
            std::vector<const std::pair<std::string, JValue>*> kvs;
            kvs.reserve(v.obj.size());
            for (auto& kv : v.obj) kvs.push_back(&kv);
            if (canonical) {
                std::sort(kvs.begin(), kvs.end(),
                          [](auto* a, auto* b) { return a->first < b->first; });
            }
            for (size_t i = 0; i < kvs.size(); ++i) {
                if (i) out.push_back(',');
                nl(indent + 1);
                escape_string(kvs[i]->first, out);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                if (!serialize_impl(kvs[i]->second, pretty, indent + 1, canonical, out, err))
                    return false;
            }
            if (!kvs.empty()) nl(indent);
            out.push_back('}');
            break;
        }
    }
    return true;
}

} // namespace

std::string jparse(const std::string& text, JValue& out) {
    Parser ps{text.data(), text.data() + text.size(), 0, ""};
    if (!ps.parse(out)) return ps.err.empty() ? "parse error" : ps.err;
    ps.skip_ws();
    if (ps.p != ps.end) return "trailing characters";
    return "";
}

std::string jserialize(const JValue& v, bool pretty) {
    std::string out, err;
    if (!serialize_impl(v, pretty, 0, false, out, err)) return "";
    return out;
}

std::string jcanonical(const JValue& v) {
    std::string out, err;
    if (!serialize_impl(v, false, 0, true, out, err)) return "";
    return out;
}

} // namespace asqs
