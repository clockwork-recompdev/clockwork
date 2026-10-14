#include "json.h"

#include <cstdint>
#include <cstdlib>

namespace json {
namespace {

const Value null_value;

struct Parser {
    const std::string &s;
    size_t i = 0;
    int depth = 0;

    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    bool eat(char c) {
        space();
        if (i < s.size() && s[i] == c) return i++, true;
        return false;
    }

    static void utf8(std::string &out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        else
            out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F)),
                out += char(0x80 | (c & 0x3F));
    }
    uint32_t hex4() {
        if (i + 4 > s.size()) return 0;
        uint32_t v = uint32_t(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
        i += 4;
        return v;
    }
    bool string(std::string &out) {
        if (!eat('"')) return false;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            switch (char e = s[i++]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t c1 = hex4();
                if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    uint32_t c2 = hex4();
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                utf8(out, c1);
                break;
            }
            default: out += e; // \" \\ \/
            }
        }
        return eat('"');
    }
    Value value() {
        Value v;
        space();
        if (i >= s.size() || ++depth > 200) return v;
        char c = s[i];
        if (c == '{') {
            i++;
            v.kind = Value::OBJECT;
            if (!eat('}')) {
                do {
                    std::string key;
                    if (!string(key) || !eat(':')) return Value{};
                    v.obj[key] = value();
                } while (eat(','));
                if (!eat('}')) return Value{};
            }
        } else if (c == '[') {
            i++;
            v.kind = Value::ARRAY;
            if (!eat(']')) {
                do v.arr.push_back(value());
                while (eat(','));
                if (!eat(']')) return Value{};
            }
        } else if (c == '"') {
            v.kind = Value::STRING;
            if (!string(v.str)) return Value{};
        } else if (s.compare(i, 4, "true") == 0) {
            i += 4, v.kind = Value::BOOL, v.b = true;
        } else if (s.compare(i, 5, "false") == 0) {
            i += 5, v.kind = Value::BOOL;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            char *end = nullptr;
            v.num = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) return Value{};
            v.kind = Value::NUMBER;
            i = size_t(end - s.c_str());
        }
        depth--;
        return v;
    }
};

} // namespace

const Value &Value::operator[](const std::string &key) const {
    if (kind != OBJECT) return null_value;
    auto it = obj.find(key);
    return it == obj.end() ? null_value : it->second;
}

const Value &Value::operator[](size_t i) const { return kind == ARRAY && i < arr.size() ? arr[i] : null_value; }

Value parse(const std::string &text) {
    Parser p{text};
    return p.value();
}

} // namespace json
