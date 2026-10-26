/* The editor's JSON (json.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

namespace hfjson
{
const Value* Value::Find(const std::string& name) const
{
    for (auto& m : members) if (m.first == name) return &m.second;
    return nullptr;
}
Value* Value::Find(const std::string& name)
{
    for (auto& m : members) if (m.first == name) return &m.second;
    return nullptr;
}
const Value* Value::Find(const std::string& name, Kind kind) const
{
    const Value* found = Find(name);
    return found != nullptr && found->kind == kind ? found : nullptr;
}
Value& Value::At(const std::string& name)
{
    if (Value* found = Find(name)) return *found;
    members.emplace_back(name, Value{});
    members.back().second.kind = Object;
    return members.back().second;
}
void Value::Erase(const std::string& name)
{
    for (size_t i = 0; i < members.size(); i++) if (members[i].first == name) { members.erase(members.begin() + (long)i); return; }
}
double Value::AsNumber(double fallback) const { return kind == Number ? strtod(text.c_str(), nullptr) : fallback; }
std::string Value::GetString(const std::string& name, const std::string& fallback) const
{
    const Value* v = Find(name);
    return v != nullptr && v->kind == String ? v->text : fallback;
}
double Value::GetNumber(const std::string& name, double fallback) const
{
    const Value* v = Find(name);
    return v != nullptr ? v->AsNumber(fallback) : fallback;
}
bool Value::GetBool(const std::string& name, bool fallback) const
{
    const Value* v = Find(name);
    return v != nullptr && v->kind == Bool ? v->boolean : fallback;
}
Value Value::MakeNumber(double value, bool whole)
{
    Value v;
    v.kind = Number;
    char number[64];
    snprintf(number, sizeof number, whole ? "%.0f" : "%.9g", value);
    v.text = number;
    return v;
}
Value Value::MakeString(const std::string& text)
{
    Value v;
    v.kind = String;
    v.text = text;
    return v;
}
Value Value::MakeBool(bool value)
{
    Value v;
    v.kind = Bool;
    v.boolean = value;
    return v;
}

namespace
{
/* A \u escape's code point (below 0x10000) in UTF-8. */
void AppendUtf8(std::string& out, unsigned code)
{
    if (code < 0x80) out += (char)code;
    else if (code < 0x800) { out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
    else { out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
}

/* The character a one-letter escape stands for (any other letter stands for itself). */
char Unescaped(char c) { return c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c; }

struct Reader
{
    const char* at;
    const char* end;
    bool ok = true;

    void Space() { while (at < end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')) at++; }
    bool Eat(char c) { Space(); if (at < end && *at == c) { at++; return true; } return false; }
    /* The escape after a backslash (at is past it), into out. */
    void Escape(std::string& out)
    {
        char c = *at++;
        if (c == 'u' && at + 4 <= end)
        {
            unsigned code = (unsigned)strtoul(std::string(at, 4).c_str(), nullptr, 16);
            at += 4;
            AppendUtf8(out, code);
        }
        else out += Unescaped(c);
    }
    std::string Text()
    {
        std::string out;
        if (!Eat('"')) { ok = false; return out; }
        while (at < end && *at != '"')
        {
            if (*at == '\\' && at + 1 < end) { at++; Escape(out); }
            else out += *at++;
        }
        if (at >= end) ok = false; else at++;
        return out;
    }
    /* An object's members, past its '{'. */
    void Members(Value& v, int depth)
    {
        v.kind = Value::Object;
        if (Eat('}')) return;
        do { std::string name = Text(); if (!Eat(':')) { ok = false; return; } v.members.emplace_back(name, Parse(depth + 1)); } while (ok && Eat(','));
        if (!Eat('}')) ok = false;
    }
    /* An array's items, past its '['. */
    void Items(Value& v, int depth)
    {
        v.kind = Value::Array;
        if (Eat(']')) return;
        do v.items.push_back(Parse(depth + 1)); while (ok && Eat(','));
        if (!Eat(']')) ok = false;
    }
    /* true, false, null or a number. */
    void Literal(Value& v)
    {
        if (end - at >= 4 && !strncmp(at, "true", 4)) { v.kind = Value::Bool; v.boolean = true; at += 4; }
        else if (end - at >= 5 && !strncmp(at, "false", 5)) { v.kind = Value::Bool; at += 5; }
        else if (end - at >= 4 && !strncmp(at, "null", 4)) at += 4;
        else
        {
            const char* start = at;
            while (at < end && strchr("+-0123456789.eE", *at)) at++;
            if (at == start) ok = false;
            v.kind = Value::Number; v.text.assign(start, at);
        }
    }
    Value Parse(int depth)
    {
        Value v;
        Space();
        if (at >= end || depth > 64) { ok = false; return v; }
        if (*at == '{') { at++; Members(v, depth); }
        else if (*at == '[') { at++; Items(v, depth); }
        else if (*at == '"') { v.kind = Value::String; v.text = Text(); }
        else Literal(v);
        return v;
    }
};

void Quote(std::string& out, const std::string& text)
{
    out += '"';
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char escaped[8]; snprintf(escaped, sizeof escaped, "\\u%04x", c); out += escaped; }
        else out += (char)c;
    }
    out += '"';
}

void WriteValue(std::string& out, const Value& v, int indent);

void WriteItems(std::string& out, const Value& v, int indent)
{
    if (v.items.empty()) { out += "[]"; return; }
    std::string pad((size_t)indent * 2, ' '), inner((size_t)(indent + 1) * 2, ' ');
    out += "[\n";
    for (size_t i = 0; i < v.items.size(); i++) { out += inner; WriteValue(out, v.items[i], indent + 1); out += i + 1 < v.items.size() ? ",\n" : "\n"; }
    out += pad + "]";
}

void WriteMembers(std::string& out, const Value& v, int indent)
{
    if (v.members.empty()) { out += "{}"; return; }
    std::string pad((size_t)indent * 2, ' '), inner((size_t)(indent + 1) * 2, ' ');
    out += "{\n";
    for (size_t i = 0; i < v.members.size(); i++)
    {
        out += inner; Quote(out, v.members[i].first); out += ": ";
        WriteValue(out, v.members[i].second, indent + 1);
        out += i + 1 < v.members.size() ? ",\n" : "\n";
    }
    out += pad + "}";
}

void WriteValue(std::string& out, const Value& v, int indent)
{
    switch (v.kind)
    {
    case Value::Null: out += "null"; break;
    case Value::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Number: out += v.text; break;
    case Value::String: Quote(out, v.text); break;
    case Value::Array: WriteItems(out, v, indent); break;
    case Value::Object: WriteMembers(out, v, indent); break;
    }
}
} // namespace

bool Parse(const std::string& text, Value* out, std::string* error)
{
    Reader reader{text.data(), text.data() + text.size()};
    *out = reader.Parse(0);
    reader.Space();
    if (reader.ok && reader.at == reader.end) return true;
    if (error) *error = "not valid JSON near byte " + std::to_string(reader.at - text.data());
    return false;
}

std::string Write(const Value& value)
{
    std::string out;
    WriteValue(out, value, 0);
    return out + "\n";
}

std::wstring Wide(const std::string& text)
{
    std::wstring out(text.size() + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], (int)out.size());
    out.resize(n > 0 ? (size_t)n - 1 : 0);
    return out;
}

bool ReadFile(const std::wstring& path, std::string* text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (64 << 20);
    if (ok)
    {
        text->resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = size.QuadPart == 0 || (::ReadFile(file, &(*text)[0], (DWORD)size.QuadPart, &read, nullptr) && read == size.QuadPart);
    }
    CloseHandle(file);
    return ok;
}

bool WriteAtomically(const std::wstring& path, const std::string& text)
{
    std::wstring temporary = path + L".ingame.tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    CloseHandle(file);
    ok = ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}
} // namespace hfjson
