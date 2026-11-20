# Translating the in-game editor

<!-- diataxis: how-to -->

The in-game editor (F8) shows its text in another language when its folder has a word list for that language. A word
list is a text file of English strings and their translations. The editor's code keeps its English strings and looks
each one up where it draws text.

## Choose the language

1. Open the editor (F8) and go to **SETTINGS**.
2. Click **Language** until it shows the language you want.

| Choice | What the editor shows |
|---|---|
| AUTO (the default) | the list for Windows' display language (`zh-CN`), or for its language alone (`zh`); English when there is none |
| ENGLISH | the editor's own strings |
| a tag such as ZH-CN | that list |

The choice is saved in `editor-settings.json` as `language`.

## Add a word list

1. Save the list as UTF-8 in `mods\hellforge-editor\translations\<tag>.tsv`. The `<tag>` is a language tag such as
   `zh-cn` or `de`: up to 16 lower-case letters, digits and `-`. The editor ignores files with other names.
2. Click **Language** on the settings page until it shows the list.

The editor reads the list when it starts and on each click of **Language**. To see a changed list, click through the
choices back to it.

A list in the repository goes in `native/editor/translations/<tag>.tsv`. `scripts/Build-Editor.ps1` builds it into the
editor's folder, and every HellForge release ships it. An update replaces a player's file of the same name, so a list of
your own keeps a tag that HellForge does not ship.

## Write the list

Each line is the English string, a tab, and the translation. A third column, after another tab, is a note for
translators and is not shown. Lines that start with `#` are comments. A section header on a line of its own decides how
the lines after it match:

| Section | How a line matches | Use it for |
|---|---|---|
| `[exact]` (the start of the file) | the whole string, then the same string in other letter case | labels, titles, field names |
| `[fragments]` | a part of a string, replaced wherever it appears, the longest part first | sentences the editor builds at run time, such as `3 fields` |
| `[words]` | single words; a string is translated word by word only when every word is listed | field names made of common words |

- The editor upper-cases some labels before drawing them, and draws page titles without the spaces around `/` and `·`.
  One `[exact]` line serves every form, and a translation is upper-cased again.
- A fragment that starts or ends with a letter or digit matches only at the edge of a word: `rate` never matches inside
  `Firerate`.
- In `[words]`, a `-` between letters is part of a word (`anti-tank`). A `-` before a number is a sign, not a word.
- `[words]` keeps a string English when one of its words is unknown. A weapon's or enemy's name then stays whole.
- Two translated words in Chinese or Japanese lose the space between them. Korean and every other script keep it.
- Section headers ignore letter case and the spaces around them. `[rules]` and `[extra]` are read as `[fragments]` and
  `[exact]`.

### Sentences with names

Some sentences hold a name: a mod, a player or a lobby host. Their English string has `{1}` and `{2}` where the names
go, and the translation puts them where the language needs them:

<!-- markdownlint-disable MD010 -->
```text
from {1}'s lobby	来自 {1} 的大厅
```
<!-- markdownlint-enable MD010 -->

The editor puts each name in as it is. It never translates text that is not its own: what the player types, names of
mods, presets, players and fields, file paths, and log lines.

## Fonts

The editor bakes only the characters your list uses. It takes them from a Windows font that has them:

| Tag | Font |
|---|---|
| `zh...` | Microsoft YaHei, else SimHei or SimSun |
| `ja...` | Yu Gothic, else Meiryo or MS Gothic |
| `ko...` | Malgun Gothic, else Gulim |
| any other | Bahnschrift, else Segoe UI |

A list with characters past Latin-1 keeps the font atlas small:

- Titles are drawn from the body font, made bold.
- Text is baked at every other size and scaled to the sizes between them.
- When the atlas still does not fit, the glyphs are baked smaller and drawn larger. The editor's log says so.

Long translations wrap between words. Chinese and Japanese text also wraps between characters.

Scripts written right to left (Arabic, Hebrew) and scripts whose letters change shape with their neighbours (Thai,
Devanagari) do not show correctly: the editor's text drawing (Dear ImGui) supports neither.
