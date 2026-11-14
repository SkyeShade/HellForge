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

1. Save the list as UTF-8 in `mods\hellforge-editor\translations\<tag>.tsv`, where `<tag>` is a language tag such as
   `zh-cn` or `de`.
2. Pick it on the settings page, or reopen the editor if it is already chosen. The editor reads the list when it starts
   and when the choice changes.

A list in the repository goes in `native/editor/translations/<tag>.tsv`, and `scripts/Build-Editor.ps1` ships it with
the editor.

## Write the list

Each line is the English string, a tab, and the translation. Lines that start with `#` are comments. A section header
on a line of its own decides how the lines after it match:

| Section | How a line matches | Use it for |
|---|---|---|
| `[exact]` (the start of the file) | the whole string, then the same string in other letter case | labels, titles, field names |
| `[fragments]` | a part of a string, replaced wherever it appears, the longest part first | sentences the editor builds at run time, such as `3 fields` |
| `[words]` | single words; a string is translated word by word only when every word is listed | field names made of common words |

- The editor upper-cases some labels before drawing them, and draws page titles without the spaces around `/` and `·`.
  One `[exact]` line serves every form, and a Latin translation is upper-cased again.
- `[words]` keeps a string English when one of its words is unknown. A weapon's or enemy's name then stays whole.
- Two translated words with non-Latin text lose the space between them, as Chinese and Japanese are written.
- `[rules]` and `[extra]` are read as `[fragments]` and `[exact]`.

## Fonts

The editor bakes only the characters your list uses. It takes them from a Windows font that has them:

| Tag | Font |
|---|---|
| `zh...` | Microsoft YaHei, else SimHei or SimSun |
| `ja...` | Yu Gothic, else Meiryo or MS Gothic |
| `ko...` | Malgun Gothic, else Gulim |
| any other | Bahnschrift, else Segoe UI |

Long translations wrap between words, and between characters where the language has no spaces.
