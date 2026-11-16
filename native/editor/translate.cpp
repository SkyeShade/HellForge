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
std::string g_tag;
unsigned g_generation = 1;
/* Get runs on the render thread for every string drawn; a miss costs about a microsecond (fragments, words), a hit a
 * tenth of that. Bounded so typed or logged text cannot grow it without end. */
std::unordered_map<std::string, std::string> g_cache;
const size_t CACHE_MAX = 8192;

std::string Lower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

std::string Trimmed(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
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

/* Windows' upper case, so a translation's accented and Cyrillic letters follow the label too */
std::string Upper(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) return s;
    std::wstring wide((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &wide[0], n);
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(), n, &wide[0], n, NULL, NULL, 0) != n) return s;
    int m = WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, NULL, 0, NULL, NULL);
    if (m <= 0) return s;
    std::string out((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, &out[0], m, NULL, NULL);
    return out;
}

bool Alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

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

/* The string as words and the single characters between them. A '-' joins a word only between letters or digits
 * ("anti-tank"), so "-5" stays a sign and a number. */
std::vector<std::string> Tokens(const std::string& in)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < in.size();)
    {
        size_t j = i + 1;
        if (Alnum(in[i]))
            while (j < in.size() && (Alnum(in[j]) || (in[j] == '-' && j + 1 < in.size() && Alnum(in[j + 1])))) j++;
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
        if (!Alnum(tokens[k][0]) || (tokens[k][0] >= '0' && tokens[k][0] <= '9')) continue;
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

/* A fragment that starts or ends with a letter or digit matches only at a word's edge: "fire" is not inside "Firearm". */
bool AtWordEdges(const std::string& text, size_t at, const std::string& find)
{
    if (Alnum(find.front()) && at > 0 && Alnum(text[at - 1])) return false;
    size_t end = at + find.size();
    return !(Alnum(find.back()) && end < text.size() && Alnum(text[end]));
}

std::string Fragments(const std::string& english, bool* changed)
{
    std::string out = english;
    for (const Fragment& f : g_fragments)
        for (size_t at = 0; (at = out.find(f.find, at)) != std::string::npos;)
        {
            if (!AtWordEdges(out, at, f.find)) { at++; continue; }
            out.replace(at, f.find.size(), f.replace);
            at += f.replace.size();
            *changed = true;
        }
    return out;
}

std::string Translate(const std::string& english)
{
    auto found = g_exact.find(english);
    if (found != g_exact.end()) return found->second;
    /* the editor upper-cases some labels before drawing them: one entry serves both, upper-cased again where it can be */
    auto folded = g_folded.find(Folded(english));
    if (folded != g_folded.end()) return AllUpper(english) ? Upper(folded->second) : folded->second;
    bool changed = false;
    std::string out = Fragments(english, &changed), words;
    if (Words(out, &words)) return words;
    return changed ? out : english;
}

enum Section { EXACT, FRAGMENTS, WORDS, NOT_A_HEADER };

/* [rules] and [extra] are the names the first community word list gave its fragments and a second exact block */
Section HeaderOf(const std::string& raw)
{
    std::string line = Lower(Trimmed(raw));
    if (line == "[exact]" || line == "[extra]") return EXACT;
    if (line == "[fragments]" || line == "[rules]") return FRAGMENTS;
    if (line == "[words]") return WORDS;
    return NOT_A_HEADER;
}

void AddLine(Section section, const std::string& line, std::set<uint32_t>* characters)
{
    size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) return;
    /* a third column (a translator's note) is not part of the translation */
    size_t end = line.find('\t', tab + 1);
    std::string english = line.substr(0, tab), shown = line.substr(tab + 1, end == std::string::npos ? std::string::npos : end - tab - 1);
    if (shown.empty()) return;
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

bool ValidTag(const std::string& tag)
{
    if (tag.empty() || tag.size() > 16 || tag.front() == '-' || tag.back() == '-') return false;
    return std::all_of(tag.begin(), tag.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::vector<std::string> Available(const std::string& folder)
{
    std::vector<std::string> tags;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA((folder + "\\translations\\*.tsv").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return tags;
    do
    {
        std::string name = found.cFileName;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || name.size() <= 4) continue;
        std::string tag = Lower(name.substr(0, name.size() - 4));
        if (ValidTag(tag)) tags.push_back(tag);
    } while (FindNextFileA(h, &found));
    FindClose(h);
    std::sort(tags.begin(), tags.end());
    return tags;
}

std::string WindowsLanguage()
{
    /* the preferred-languages list has a name for every display language, also those without an LCID */
    ULONG count = 0, size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, NULL, &size) || size == 0) return "";
    std::wstring names(size, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, &names[0], &size) || count == 0) return "";
    char out[LOCALE_NAME_MAX_LENGTH * 4];
    int n = WideCharToMultiByte(CP_UTF8, 0, names.c_str(), -1, out, sizeof out, NULL, NULL);
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
    g_exact.clear(); g_folded.clear(); g_words.clear(); g_fragments.clear(); g_characters.clear(); g_cache.clear();
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
    /* the tag comes from editor-settings.json: never a path out of the translations folder */
    if (!ValidTag(tag)) { LoadText("", ""); return false; }
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

std::string Get(const std::string& english)
{
    if (english.empty() || !Active()) return english;
    auto cached = g_cache.find(english);
    if (cached != g_cache.end()) return cached->second;
    if (g_cache.size() >= CACHE_MAX) g_cache.clear();
    return g_cache.emplace(english, Translate(english)).first->second;
}

std::string Named(const std::string& english, const std::vector<std::string>& names)
{
    std::string out = Get(english);
    for (size_t at = 0; (at = out.find('{', at)) != std::string::npos;)
    {
        size_t k = at + 2 < out.size() && out[at + 2] == '}' ? (size_t)(out[at + 1] - '1') : names.size();
        if (k >= names.size()) { at++; continue; }
        out.replace(at, 3, names[k]);
        at += names[k].size();
    }
    return out;
}
} // namespace tr
