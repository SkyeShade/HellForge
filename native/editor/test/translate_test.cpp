/* The in-game editor's translations (translate.cpp) on word lists of its own: exact lines, a label the editor upper-cases,
 * fragments of a sentence built at run time (the longest first), word-by-word names that stay English when one word is
 * unknown, the characters the font must bake, and the folder's lists for "auto" and the settings' choice; names put into
 * a translated sentence as they are, and tags that are not a list's name refused.
 * Usage: translate_test.exe */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include "../translate.h"

static int g_failures;
static void Check(bool ok, const char* what) { printf("%s %s\n", ok ? "  ok  " : "  FAIL", what); if (!ok) g_failures++; }

static void Write(const std::string& path, const std::string& text)
{
    FILE* file = fopen(path.c_str(), "wb");
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
}

/* UTF-8 escapes keep this file ASCII: 设置 (settings), 字段 (field), 伤害 (damage), 穿甲 (armor penetration) */
#define SETTINGS "\xE8\xAE\xBE\xE7\xBD\xAE"
#define FIELDS "\xE5\xAD\x97\xE6\xAE\xB5"
#define DAMAGE "\xE4\xBC\xA4\xE5\xAE\xB3"
#define PEN "\xE7\xA9\xBF\xE7\x94\xB2"

int main()
{
    const std::string list = "\xEF\xBB\xBF# a word list\r\n"
                             "Settings\t" SETTINGS "\r\n"
                             "Interface size\tSchnittstelle\n"
                             "Handling / Fire\tHandhabung / Feuer\n"
                             "Weapon \xC2\xB7 Gun\tW\n"
                             "Reload\tNachladen\ta note for translators\n"
                             "Fuel\t\xC3\xA9nergie\n"
                             "{1}'s lobby\t{1} LOBBY\n"
                             "{1} and {2}\t{2}+{1}\n"
                             "\n"
                             "[fragments]\n"
                             " fields\t " FIELDS "\n"
                             " fields selected\t selected-" FIELDS "\n"
                             "rate\tR\n"
                             " [Words] \n"
                             "damage\t" DAMAGE "\n"
                             "penetration\t" PEN "\n"
                             "armor\tA\n"
                             "anti-tank\tAT\n"
                             "[extra]\n"
                             "Damage\t" DAMAGE "!\n"
                             "broken line without a tab\n";
    tr::LoadText(list, "zz");
    Check(tr::Active() && tr::Tag() == "zz", "a list loads, its BOM and comments skipped");
    Check(tr::Get("Settings") == SETTINGS, "an exact line");
    Check(tr::Get("SETTINGS") == SETTINGS, "an upper-cased label finds its line");
    Check(tr::Get("INTERFACE SIZE") == "SCHNITTSTELLE", "a Latin translation of an upper-cased label is upper-cased too");
    Check(tr::Get("HANDLING/FIRE") == "HANDHABUNG / FEUER" && tr::Get("WEAPON\xC2\xB7GUN") == "W",
          "a page title drawn without the spaces around its separators finds its line");
    Check(tr::Get("Damage") == DAMAGE "!", "[extra] lines are exact lines");
    Check(tr::Get("3 fields") == "3 " FIELDS, "a fragment inside a sentence built at run time");
    Check(tr::Get("3 fields selected") == "3 selected-" FIELDS, "the longest fragment first");
    Check(tr::Get("Damage penetration") == DAMAGE PEN, "every word known: the words translated, the space between them gone");
    Check(tr::Get("Armor damage 2") == "A " DAMAGE " 2", "a Latin word keeps its space; numbers are not words");
    Check(tr::Get("Liberator damage") == "Liberator damage", "an unknown word keeps the whole name English");
    Check(tr::Get("Nothing here") == "Nothing here", "no match: the same string");
    Check(tr::Get("Reload") == "Nachladen", "a third column is not part of the translation");
    Check(tr::Get("FUEL") == "\xC3\x89NERGIE", "an upper-cased label's translation is upper-cased past ASCII too");
    Check(tr::Get("3 rate") == "3 R" && tr::Get("3 rates") == "3 rates" && tr::Get("Firerate") == "Firerate",
          "a fragment matches at a word's edges, never inside a word");
    Check(tr::Get("Damage -5") == DAMAGE " -5", "a minus sign is not part of a word");
    Check(tr::Get("Anti-tank damage") == "AT " DAMAGE, "a hyphen between letters joins a word");
    Check(tr::Named("{1}'s lobby", {"Damage"}) == "Damage LOBBY", "a name put into a translated sentence is not translated");
    Check(tr::Named("{1} and {2}", {"a", "b"}) == "b+a", "the list can move the names");
    Check(tr::Named("from {1}", {"x{2}"}) == "from x{2}", "a name is put in once, never read for more names");
    bool has = false, latin = false;
    for (uint32_t c : tr::Characters()) { if (c == 0x8BBE) has = true; if (c == 'S') latin = true; }
    Check(has && latin, "the characters the translations use, for the font");
    unsigned before = tr::Generation();
    tr::LoadText("", "");
    Check(!tr::Active() && tr::Get("Settings") == "Settings" && tr::Generation() != before, "English: nothing changes, the generation moves");

    char temp[MAX_PATH];
    GetTempPathA(sizeof temp, temp);
    std::string folder = std::string(temp) + "hellforge-translate-test-" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(folder.c_str(), NULL);
    Check(tr::Available(folder).empty() && tr::AutoTag(folder, "zh-CN").empty(), "no translations folder: English");
    CreateDirectoryA((folder + "\\translations").c_str(), NULL);
    Write(folder + "\\translations\\zh-CN.tsv", "Settings\t" SETTINGS "\n");
    Write(folder + "\\translations\\de.tsv", "Settings\tEinstellungen\n");
    std::vector<std::string> tags = tr::Available(folder);
    Check(tags.size() == 2 && tags[0] == "de" && tags[1] == "zh-cn", "the folder's lists, as lower-case tags");
    Check(tr::AutoTag(folder, "zh-CN") == "zh-cn", "auto: Windows' display language");
    Check(tr::AutoTag(folder, "de-AT") == "de", "auto: its language alone when the region has no list");
    Check(tr::AutoTag(folder, "fr-FR").empty(), "auto: English when there is no list");
    Check(tr::Load(folder, "zh-cn") && tr::Get("Settings") == SETTINGS, "a list loaded from the folder");
    Check(tr::Load(folder, "de") && tr::Get("Settings") == "Einstellungen", "another list: nothing cached from the one before");
    Check(!tr::Load(folder, "ja") && !tr::Active(), "a missing list: English");
    Check(tr::Load(folder, "en") && !tr::Active(), "en: English");
    Check(tr::ValidTag("zh-cn") && !tr::ValidTag("..\\..\\x") && !tr::ValidTag("zh/cn") && !tr::ValidTag("ZH") && !tr::ValidTag(""),
          "a tag is a-z, 0-9 and '-' only");
    Check(!tr::Load(folder, "..\\translations\\zh-cn") && !tr::Active(), "a tag that is a path is refused, not opened");
    DeleteFileA((folder + "\\translations\\zh-CN.tsv").c_str());
    DeleteFileA((folder + "\\translations\\de.tsv").c_str());
    RemoveDirectoryA((folder + "\\translations").c_str());
    RemoveDirectoryA(folder.c_str());

    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
