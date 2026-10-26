/* A small JSON reader and writer for the editor's files (editor-catalog.json, Local Stats, its settings): a document
 * keeps its members in order and its numbers as their text, so a file written back keeps everything it did not change. */
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace hfjson
{
struct Value
{
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    std::string text; /* a number's text, or a string */
    std::vector<std::pair<std::string, Value>> members;
    std::vector<Value> items;

    const Value* Find(const std::string& name) const;
    Value* Find(const std::string& name);
    const Value* Find(const std::string& name, Kind kind) const; /* the member when it is of that kind, else nullptr */
    Value& At(const std::string& name); /* the member, made (an empty object) when missing */
    void Erase(const std::string& name);

    bool IsNull() const { return kind == Null; }
    double AsNumber(double fallback = 0) const;
    const std::string& Str() const { return text; }
    /* A member's value, or a fallback when it is missing or of another kind. */
    std::string GetString(const std::string& name, const std::string& fallback = "") const;
    double GetNumber(const std::string& name, double fallback = 0) const;
    bool GetBool(const std::string& name, bool fallback = false) const;

    static Value MakeNumber(double value, bool whole);
    static Value MakeString(const std::string& text);
    static Value MakeBool(bool value);
};

/* Parses text; false (and *error) when it is not one valid JSON value. */
bool Parse(const std::string& text, Value* out, std::string* error);
/* Two-space indented, as the launcher writes its files. */
std::string Write(const Value& value);
bool ReadFile(const std::wstring& path, std::string* text);
bool WriteAtomically(const std::wstring& path, const std::string& text);
std::wstring Wide(const std::string& text);
} // namespace hfjson
