/* The editor's translations: a word list per language in the editor's folder (translations\<tag>.tsv), looked up where
 * the canvas measures and draws text, so the code keeps its English strings. Format and rules:
 * docs/editor-translations.md. No ImGui here, so the tests build it alone. */
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace tr
{
/* A tag a list may have: 1 to 16 of a-z, 0-9 and '-' ("zh-cn"). Nothing else is ever opened. */
bool ValidTag(const std::string& tag);
/* The languages the folder has a word list for, as tags ("zh-cn"), sorted. Reads the folder: callers cache it. */
std::vector<std::string> Available(const std::string& folder);
/* The tag "auto" stands for: Windows' display language, or its language alone ("zh"), when the folder has that list;
 * "" (English) otherwise. */
std::string AutoTag(const std::string& folder, const std::string& windowsLocale);
/* Windows' first display language as a locale name ("zh-CN"), "" when it cannot be read. */
std::string WindowsLanguage();
/* Loads the folder's list for a tag ("" or "en": English, no list). false when the tag is not valid or the list cannot
 * be read. */
bool Load(const std::string& folder, const std::string& tag);
const std::string& Tag();
bool Active();
/* Bumped by every Load: the canvas re-measures and rebakes its fonts when it changes. */
unsigned Generation();
/* The text in the loaded language (the same string when nothing matches). */
std::string Get(const std::string& english);
/* A sentence with names in it: the sentence translated with {1}, {2} where the names go, then the names put in as they
 * are, so a list can move them and a name is never translated. */
std::string Named(const std::string& english, const std::vector<std::string>& names);
/* A character of a script written without spaces (Chinese, Japanese): words translated into it lose the space between
 * them, and a line breaks between any two of them. Korean and every other script keep spaces and break at words. */
bool Unspaced(uint32_t codePoint);
bool HasUnspaced(const std::string& text);
/* Every character the loaded list's translations use, sorted: the font atlas bakes these and no others. */
const std::vector<uint32_t>& Characters();
/* Parses a list's text (for the tests; Load reads the file and calls it). */
void LoadText(const std::string& text, const std::string& tag);
} // namespace tr
