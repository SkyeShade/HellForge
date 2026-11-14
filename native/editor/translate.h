/* The editor's translations: a word list per language in the editor's folder (translations\<tag>.tsv), looked up where
 * the canvas measures and draws text, so the code keeps its English strings. Format and rules:
 * docs/editor-translations.md. No ImGui here, so the tests build it alone. */
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace tr
{
/* The languages the folder has a word list for, as tags ("zh-cn"), sorted. */
std::vector<std::string> Available(const std::string& folder);
/* The tag "auto" stands for: Windows' display language, or its language alone ("zh"), when the folder has that list;
 * "" (English) otherwise. */
std::string AutoTag(const std::string& folder, const std::string& windowsLocale);
/* Windows' display language as a locale name ("zh-CN"), "" when it cannot be read. */
std::string WindowsLanguage();
/* Loads the folder's list for a tag ("" or "en": English, no list). false when the list cannot be read. */
bool Load(const std::string& folder, const std::string& tag);
const std::string& Tag();
bool Active();
/* Bumped by every Load: the canvas re-measures and rebakes its fonts when it changes. */
unsigned Generation();
/* The text in the loaded language (the same string when nothing matches). */
const std::string& Get(const std::string& english);
/* Every character the loaded list's translations use, sorted: the font atlas bakes these and no others. */
const std::vector<uint32_t>& Characters();
/* Parses a list's text (for the tests; Load reads the file and calls it). */
void LoadText(const std::string& text, const std::string& tag);
} // namespace tr
