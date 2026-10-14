#pragma once
// A small JSON reader, for web API responses (GameBanana). Read-only and forgiving: anything it
// can't parse reads as null.
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Kind { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } kind = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    // member or element access that yields null when missing
    const Value &operator[](const std::string &key) const;
    const Value &operator[](size_t i) const;
    std::string string(const std::string &fallback = "") const { return kind == STRING ? str : fallback; }
    long long integer(long long fallback = 0) const { return kind == NUMBER ? (long long)num : fallback; }
};

Value parse(const std::string &text);

} // namespace json
