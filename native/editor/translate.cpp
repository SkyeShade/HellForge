/* The editor's translations (translate.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <set>
#include <unordered_map>
#include "translate.h"

namespace tr
{
namespace
{
struct Fragment { std::string find, replace; };

std::unordered_map<std::string, std::string> g_exact, g_folded, g_words;
std::vector<Fragment> g_fragments;
std::vector<uint32_t> g_characters;
std::string g_tag, g_result;
unsigned g_generation = 1;

std::string Lower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

/* The key a label keeps however the editor draws it: page titles are drawn upper-cased and without the spaces around
 * their separators ("Handling / Fire" as "HANDLING/FIRE", "Weapon · Gun" as "WEAPON·GUN"). */
std::string Folded(const std::string& s)
{
    std::string out = Lower(s);
    for (const char* separator : {"/", "\xC2\xB7"})
        for (size_t at = 0; (at = out.find(separator, at)) != std::string::npos; at += strlen(separator))
        {
            while (at > 0 && out[at - 1] == ' ') out.erase(--at, 1);
            while (at + strlen(separator) < out.size() && out[at + strlen(separator)] == ' ') out.erase(at + strlen(separator), 1);
        }
    return out;
}

bool AllUpper(const std::string& s)
{
    bool letters = false;
    for (char c : s)
    {
        if (c >= 'a' && c <= 'z') return false;
        if (c >= 'A' && c <= 'Z') letters = true;
    }
    return letters;
}

std::string UpperAscii(std::string s)
{
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

bool WordChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

/* UTF-8 code points, for the font's character set; a broken sequence counts as its first byte */
void AddCharacters(const std::string& s, std::set<uint32_t>* out)
{
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + (size_t)len > s.size()) len = 1;
        uint32_t cp = len == 1 ? c : c & (0xFFu >> (len + 1));
        for (int k = 1; k < len; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3Fu);
        out->insert(cp);
        i += (size_t)len;
    }
}

/* The string as words (letters, digits and '-') and the single characters between them. */
std::vector<std::string> Tokens(const std::string& in)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < in.size();)
    {
        size_t j = i + 1;
        if (WordChar(in[i])) while (j < in.size() && WordChar(in[j])) j++;
        out.push_back(in.substr(i, j - i));
        i = j;
    }
    return out;
}

bool NonLatin(const std::string* s) { return s != nullptr && !s->empty() && (unsigned char)(*s)[0] >= 0x80; }

/* Each word's translation (numbers and other characters: none); false when a word is not in the list. */
bool WordTranslations(const std::vector<std::string>& tokens, std::vector<const std::string*>* shown)
{
    shown->assign(tokens.size(), nullptr);
    for (size_t k = 0; k < tokens.size(); k++)
    {
        if (!WordChar(tokens[k][0]) || (tokens[k][0] >= '0' && tokens[k][0] <= '9')) continue;
        auto found = g_words.find(Lower(tokens[k]));
        if (found == g_words.end()) return false;
        (*shown)[k] = &found->second;
    }
    return std::any_of(shown->begin(), shown->end(), [](const std::string* s) { return s != nullptr; });
}

/* A string whose every word the [words] list knows becomes those words' translations; one unknown word (a weapon's or
 * enemy's name, mostly) keeps the whole string in English, so a name never comes out half translated. */
bool Words(const std::string& in, std::string* out)
{
    if (g_words.empty()) return false;
    std::vector<std::string> tokens = Tokens(in);
    std::vector<const std::string*> shown;
    if (!WordTranslations(tokens, &shown)) return false;
    out->clear();
    for (size_t k = 0; k < tokens.size(); k++)
    {
        /* Chinese and Japanese put no space between words */
        bool gap = tokens[k] == " " && k > 0 && k + 1 < tokens.size() && NonLatin(shown[k - 1]) && NonLatin(shown[k + 1]);
        if (!gap) *out += shown[k] ? *shown[k] : tokens[k];
    }
    return true;
}

enum Section { EXACT, FRAGMENTS, WORDS, NOT_A_HEADER };

/* [rules] and [extra] are the names the first community word list gave its fragments and a second exact block */
Section HeaderOf(const std::string& line)
{
    if (line == "[exact]" || line == "[extra]") return EXACT;
    if (line == "[fragments]" || line == "[rules]") return FRAGMENTS;
    if (line == "[words]") return WORDS;
    return NOT_A_HEADER;
}

void AddLine(Section section, const std::string& line, std::set<uint32_t>* characters)
{
    size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) return;
    std::string english = line.substr(0, tab), shown = line.substr(tab + 1);
    AddCharacters(shown, characters);
    if (section == FRAGMENTS) g_fragments.push_back({english, shown});
    else if (section == WORDS) g_words[Lower(english)] = shown;
    else { g_exact[english] = shown; g_folded.emplace(Folded(english), shown); }
}

/* the text's lines without their line ends, its UTF-8 byte order mark dropped */
std::vector<std::string> Lines(const std::string& raw)
{
    std::string text = raw.compare(0, 3, "\xEF\xBB\xBF") == 0 ? raw.substr(3) : raw;
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();)
    {
        size_t end = std::min(text.find('\n', at), text.size());
        lines.push_back(text.substr(at, end - at));
        if (!lines.back().empty() && lines.back().back() == '\r') lines.back().pop_back();
        at = end + 1;
    }
    return lines;
}
} // namespace

std::vector<std::string> Available(const std::string& folder)
{
    std::vector<std::string> tags;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA((folder + "\\translations\\*.tsv").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return tags;
    do
    {
        std::string name = found.cFileName;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name.size() > 4) tags.push_back(Lower(name.substr(0, name.size() - 4)));
    } while (FindNextFileA(h, &found));
    FindClose(h);
    std::sort(tags.begin(), tags.end());
    return tags;
}

std::string WindowsLanguage()
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0)) return "";
    char out[LOCALE_NAME_MAX_LENGTH * 4];
    int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof out, NULL, NULL);
    return n > 0 ? std::string(out) : std::string();
}

std::string AutoTag(const std::string& folder, const std::string& windowsLocale)
{
    std::vector<std::string> tags = Available(folder);
    std::string want = Lower(windowsLocale);
    if (std::find(tags.begin(), tags.end(), want) != tags.end()) return want;
    std::string language = want.substr(0, want.find('-'));
    if (std::find(tags.begin(), tags.end(), language) != tags.end()) return language;
    return "";
}

void LoadText(const std::string& raw, const std::string& tag)
{
    g_exact.clear(); g_folded.clear(); g_words.clear(); g_fragments.clear(); g_characters.clear();
    g_tag = tag;
    g_generation++;
    Section section = EXACT;
    std::set<uint32_t> characters;
    for (const std::string& line : Lines(raw))
    {
        if (line.empty() || line[0] == '#') continue;
        Section header = HeaderOf(line);
        if (header != NOT_A_HEADER) section = header;
        else AddLine(section, line, &characters);
    }
    /* the longest fragment first, so "fields" never cuts into "3 fields selected" before the whole phrase does */
    std::stable_sort(g_fragments.begin(), g_fragments.end(), [](const Fragment& a, const Fragment& b) { return a.find.size() > b.find.size(); });
    if (!Active()) { g_tag.clear(); return; }
    g_characters.assign(characters.begin(), characters.end());
}

bool Load(const std::string& folder, const std::string& tag)
{
    if (tag.empty() || tag == "en") { LoadText("", ""); return true; }
    std::string text;
    FILE* f = fopen((folder + "\\translations\\" + tag + ".tsv").c_str(), "rb");
    if (!f) { LoadText("", ""); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text.resize(size > 0 ? (size_t)size : 0);
    text.resize(text.empty() ? 0 : fread(&text[0], 1, text.size(), f));
    fclose(f);
    LoadText(text, tag);
    return Active();
}

const std::string& Tag() { return g_tag; }
bool Active() { return !g_exact.empty() || !g_fragments.empty() || !g_words.empty(); }
unsigned Generation() { return g_generation; }
const std::vector<uint32_t>& Characters() { return g_characters; }

const std::string& Get(const std::string& english)
{
    if (english.empty() || !Active()) return english;
    auto found = g_exact.find(english);
    if (found != g_exact.end()) return found->second;
    /* the editor upper-cases some labels before drawing them: one entry serves both, upper-cased again where it can be */
    auto folded = g_folded.find(Folded(english));
    if (folded != g_folded.end())
    {
        if (!AllUpper(english)) return folded->second;
        g_result = UpperAscii(folded->second);
        return g_result;
    }
    g_result = english;
    bool changed = false;
    for (const Fragment& f : g_fragments)
        for (size_t at = 0; (at = g_result.find(f.find, at)) != std::string::npos; at += f.replace.size())
        {
            g_result.replace(at, f.find.size(), f.replace);
            changed = true;
        }
    std::string words;
    if (Words(g_result, &words)) { g_result = words; changed = true; }
    return changed ? g_result : english;
}
} // namespace tr
