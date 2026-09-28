#pragma once

// Minimal JSON reader/writer for the chiptool command-line utility (not part of chipdsp).
// Subset: objects, arrays, strings, numbers, booleans, null. Objects are std::map so that
// serialisation is deterministic (sorted keys). Throws std::runtime_error on bad input.
// Tool code only: never used on the audio thread.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace minijson
{

class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    Value() = default;
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), number_(d) {}
    Value(int i) : type_(Type::Number), number_(static_cast<double>(i)) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), array_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), object_(std::move(o)) {}

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const { require(Type::Bool); return bool_; }
    double asNumber() const { require(Type::Number); return number_; }
    const std::string& asString() const { require(Type::String); return string_; }
    const Array& asArray() const { require(Type::Array); return array_; }
    Array& asArray() { require(Type::Array); return array_; }
    const Object& asObject() const { require(Type::Object); return object_; }
    Object& asObject() { require(Type::Object); return object_; }

    // Object member lookup; nullptr when absent or when this is not an object.
    const Value* find(const std::string& key) const
    {
        if (type_ != Type::Object)
            return nullptr;
        auto it = object_.find(key);
        return it == object_.end() ? nullptr : &it->second;
    }

    // Insert-or-access on an object (a null value becomes an empty object).
    Value& operator[](const std::string& key)
    {
        if (type_ == Type::Null)
            type_ = Type::Object;
        require(Type::Object);
        return object_[key];
    }

    void push(Value v)
    {
        if (type_ == Type::Null)
            type_ = Type::Array;
        require(Type::Array);
        array_.push_back(std::move(v));
    }

private:
    void require(Type t) const
    {
        if (type_ != t)
            throw std::runtime_error("minijson: wrong value type");
    }

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

// ----- writer -----------------------------------------------------------------------------------

namespace detail
{
    inline void writeString(std::string& out, const std::string& s)
    {
        out += '"';
        for (unsigned char c : s)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    inline void writeNumber(std::string& out, double d)
    {
        if (d != d || d > 1e300 || d < -1e300)
        {
            out += "null"; // JSON has no NaN/Inf
            return;
        }
        char buf[32];
        if (d == static_cast<double>(static_cast<long long>(d)) && d > -1e15 && d < 1e15)
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        else
            std::snprintf(buf, sizeof(buf), "%.9g", d);
        out += buf;
    }

    inline void indent(std::string& out, int level)
    {
        out.append(static_cast<size_t>(level) * 2, ' ');
    }

    inline void write(std::string& out, const Value& v, int level)
    {
        switch (v.type())
        {
            case Value::Type::Null:   out += "null"; break;
            case Value::Type::Bool:   out += v.asBool() ? "true" : "false"; break;
            case Value::Type::Number: writeNumber(out, v.asNumber()); break;
            case Value::Type::String: writeString(out, v.asString()); break;
            case Value::Type::Array:
            {
                const auto& a = v.asArray();
                if (a.empty()) { out += "[]"; break; }
                out += "[\n";
                for (size_t i = 0; i < a.size(); ++i)
                {
                    indent(out, level + 1);
                    write(out, a[i], level + 1);
                    out += (i + 1 < a.size()) ? ",\n" : "\n";
                }
                indent(out, level);
                out += ']';
                break;
            }
            case Value::Type::Object:
            {
                const auto& o = v.asObject();
                if (o.empty()) { out += "{}"; break; }
                out += "{\n";
                size_t i = 0;
                for (const auto& [key, member] : o)
                {
                    indent(out, level + 1);
                    writeString(out, key);
                    out += ": ";
                    write(out, member, level + 1);
                    out += (++i < o.size()) ? ",\n" : "\n";
                }
                indent(out, level);
                out += '}';
                break;
            }
        }
    }
} // namespace detail

// Pretty-printed (2-space indent, sorted keys) with a trailing newline.
inline std::string serialize(const Value& v)
{
    std::string out;
    detail::write(out, v, 0);
    out += '\n';
    return out;
}

// ----- parser -----------------------------------------------------------------------------------

class Parser
{
public:
    explicit Parser(const std::string& text) : s(text) {}

    Value parseDocument()
    {
        skipWs();
        Value v = parseValue();
        skipWs();
        if (pos != s.size())
            fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) const
    {
        throw std::runtime_error(std::string("minijson: ") + what + " at offset " + std::to_string(pos));
    }

    void skipWs()
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    bool consume(const char* literal)
    {
        const size_t n = std::strlen(literal);
        if (s.compare(pos, n, literal) == 0)
        {
            pos += n;
            return true;
        }
        return false;
    }

    Value parseValue()
    {
        if (pos >= s.size())
            fail("unexpected end of input");
        const char c = s[pos];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value(parseString());
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        if (consume("true")) return Value(true);
        if (consume("false")) return Value(false);
        if (consume("null")) return Value();
        fail("unexpected character");
    }

    Value parseObject()
    {
        Value::Object o;
        ++pos; // '{'
        skipWs();
        if (pos < s.size() && s[pos] == '}') { ++pos; return Value(std::move(o)); }
        for (;;)
        {
            skipWs();
            if (pos >= s.size() || s[pos] != '"')
                fail("expected string key");
            std::string key = parseString();
            skipWs();
            if (pos >= s.size() || s[pos] != ':')
                fail("expected ':'");
            ++pos;
            skipWs();
            o[key] = parseValue();
            skipWs();
            if (pos >= s.size())
                fail("unterminated object");
            if (s[pos] == ',') { ++pos; continue; }
            if (s[pos] == '}') { ++pos; return Value(std::move(o)); }
            fail("expected ',' or '}'");
        }
    }

    Value parseArray()
    {
        Value::Array a;
        ++pos; // '['
        skipWs();
        if (pos < s.size() && s[pos] == ']') { ++pos; return Value(std::move(a)); }
        for (;;)
        {
            skipWs();
            a.push_back(parseValue());
            skipWs();
            if (pos >= s.size())
                fail("unterminated array");
            if (s[pos] == ',') { ++pos; continue; }
            if (s[pos] == ']') { ++pos; return Value(std::move(a)); }
            fail("expected ',' or ']'");
        }
    }

    unsigned parseHex4()
    {
        if (pos + 4 > s.size())
            fail("truncated \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = s[pos++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string parseString()
    {
        std::string out;
        ++pos; // opening quote
        for (;;)
        {
            if (pos >= s.size())
                fail("unterminated string");
            const char c = s[pos++];
            if (c == '"')
                return out;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (pos >= s.size())
                fail("truncated escape");
            const char e = s[pos++];
            switch (e)
            {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u':
                {
                    unsigned cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && consume("\\u"))
                    {
                        const unsigned low = parseHex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    Value parseNumber()
    {
        const size_t start = pos;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' || s[pos] == 'e' ||
                                  s[pos] == 'E' || s[pos] == '+' || s[pos] == '-'))
            ++pos;
        const std::string text = s.substr(start, pos - start);
        char* end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (end == nullptr || *end != '\0' || text.empty())
            fail("bad number");
        return Value(d);
    }

    const std::string& s;
    size_t pos = 0;
};

inline Value parse(const std::string& text)
{
    return Parser(text).parseDocument();
}

} // namespace minijson
