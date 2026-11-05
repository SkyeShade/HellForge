# Docs lint

Checks that keep HellForge's documentation and code comments short enough to read. CI's "Docs lint" job runs them on
every pull request. How to write so they pass is in [CONTRIBUTING.md](../../CONTRIBUTING.md#writing-docs-and-comments).

| Tool | Checks | Fails CI on |
| --- | --- | --- |
| `docs_lint.py` (this folder) | Page types, paragraph, sentence and page sizes | Anything new or worse than the baseline |
| [markdownlint](https://github.com/DavidAnson/markdownlint) | Markdown structure: headings, lists, code blocks, line length | Any problem |
| [lychee](https://lychee.cli.rs/) | Relative links and `#anchors` (offline) | Any broken link |
| [Vale](https://vale.sh/) | Filler and hedge words, history in code comments, long asides | Errors only |
| `scripts/lint` R16 | Code comment blocks over 10 lines | New or longer ones ([README](../lint/README.md)) |

## Page types

Every page under `docs/` is one of four types, after [Diátaxis](https://diataxis.fr/). Each type answers a different
need, so mixing two on one page makes both harder to use.

| Type | The reader wants to | It reads like |
| --- | --- | --- |
| `tutorial` | Learn by doing, start to finish | A lesson with one path that works |
| `how-to` | Get one task done | Numbered steps toward a goal |
| `reference` | Look a fact up | Tables and lists, no story |
| `explanation` | Understand why it works this way | Background, design and decisions |

A page states its type under its title:

```markdown
# Troubleshooting

<!-- diataxis: how-to -->
```

The marker is an HTML comment, so GitHub does not show it. [docs/README.md](../../docs/README.md) lists every page.

## Rules

| Rule | Catches | Limit | Severity |
| --- | --- | --- | --- |
| D1 `page-type` | A page under `docs/` without a valid type marker | | error |
| D2 `index` | A page under `docs/` that `docs/README.md` does not link | | error |
| D3 `paragraph-size` | A paragraph, list item or quote over the limit | 100 words | warning |
| D4 `sentence-length` | A sentence over the limit | 35 words | warning |
| D5 `page-size` | A page with more prose than its budget | 5000 words; none for `reference` | warning |

Code spans count as one word each, and a link counts by its text. Code blocks, tables and HTML are not prose. Moved-page
stubs and the index have no type (`untyped` in the config). `docs/sdk-reference.md` is generated, so the checks skip it.

CI runs `check --warnings-as-errors`, so every rule fails CI there.

## The baseline

`docs-lint-baseline.json` lists the violations the docs had when the lint arrived. It works like
[the structure lint's baseline](../lint/README.md#the-baseline):

- **Identity** is the file, the rule and the `##` section the text is under. Renaming a `##` heading makes its
  violations new.
- **New** fails: a long paragraph or sentence where the baseline has none.
- **Grew** fails: a known paragraph or sentence that got longer.
- **Lower or gone** passes, and `update` then lowers the baseline.
- **D5** counts the page once: a page over budget stays known until it is split.

To shrink a page, fix its violations and run `update`. It refuses increases unless you pass `--allow-increase`.

## When the text is right as it is

Put a marker on the line above the paragraph, with a reason:

```markdown
<!-- docs-lint-ok: D3 the launcher's message, quoted as it shows it -->
```

For a page rule (D1, D2, D5), the marker can be anywhere on the page.

## Vale

Vale's rules are in `.vale/styles/HellForge/`, and `.vale.ini` says which files get which rules.

| Rule | Level | Catches |
| --- | --- | --- |
| `Filler` | error | Phrases that add length and no information: `please note`, `in order to`, `robust` |
| `Hedges` | warning | `simply`, `basically`, `actually` and other words that tell the reader how to feel |
| `History` | warning | Code comments that tell the change's story: `previously`, `the reviewer`, `PR #12` |
| `Parentheses` | warning | An aside in brackets of 20 words or more |
| `Semicolons` | warning | Two or more semicolons in one sentence |
| `Latin` | warning | `e.g.`, `i.e.`, `etc.` and `and/or` |
| `SentenceLength`, `ParagraphLength` | suggestion | D4 and D3, shown while you write |

Errors fail CI in every file. Warnings appear on the files a pull request changes, as annotations, and do not fail.
Suggestions appear only in an editor with a Vale plugin.

## Running

From the repository root (`python3` instead of `py -3` on Linux and macOS):

```text
py -3 scripts/docs-lint/docs_lint.py                            # file x rule table
py -3 scripts/docs-lint/docs_lint.py scan --list                # every violation
py -3 scripts/docs-lint/docs_lint.py check --warnings-as-errors # what CI runs
py -3 scripts/docs-lint/docs_lint.py update                     # lower the baseline after fixes
py -3 scripts/docs-lint/docs_lint.py stats docs/linux.md        # words, blocks and sentences of pages
py -3 -m unittest discover -s scripts/docs-lint/tests -v        # its tests
npx markdownlint-cli2@0.23.3 "**/*.md"                          # Markdown structure
vale docs/linux.md                                              # Vale, on any files
lychee --offline --include-fragments --root-dir "$PWD" docs/*.md   # links
```

`docs_lint.py` needs Python 3.10 or newer and nothing else. It reads the files git tracks, plus new files git does
not ignore. CI pins Vale 3.24.0, lychee 0.24.2 and markdownlint-cli2 0.23.3 (`.github/workflows/ci.yml`).

Vale and lychee are single programs. Get them from their releases on GitHub, or from a package manager:
`winget install errata-ai.Vale`, `brew install vale lychee`. Vale's editor plugins show its alerts as you type.

## Configuration

`docs-lint-config.json`:

| Key | Holds |
| --- | --- |
| `include`, `exclude` | Which Markdown files are linted (`fnmatch` patterns from the repository root) |
| `typed`, `untyped` | Which pages need a type marker |
| `index` | The page that must link every typed page |
| `severity` | `error` or `warning` per rule, for a new violation |
| `limits` | `paragraph_words`, `sentence_words` and `page_words` |
| `page_words_by_type` | A page budget per type; `null` means none |

markdownlint's rules are in `.markdownlint.jsonc`, and the files it reads in `.markdownlint-cli2.jsonc`.
