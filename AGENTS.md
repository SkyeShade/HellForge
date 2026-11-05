# Instructions for coding agents

For AI agents that change this repository. People get the same rules from [CONTRIBUTING.md](CONTRIBUTING.md).

## Before you write

- Read [CONTRIBUTING.md](CONTRIBUTING.md), "Writing docs and comments" in particular.
- Find the page your change belongs on in [docs/README.md](docs/README.md). Change that page instead of adding text
  somewhere new.

## Docs

- One page is one [Diátaxis](https://diataxis.fr/) type: tutorial, how-to, reference or explanation. Keep its
  `<!-- diataxis: ... -->` marker true.
- The answer first. At most 35 words a sentence and 100 a paragraph. Use lists and tables for steps and facts.
- Write what is true now. Leave out how the work went: no research diaries, dates of findings or test logs.
- Put placeholders such as `<installation>` in backticks.

## Code comments

- Say why the code is this way, not what it does. Two or three lines are usually enough; more than 10 fail CI (R16).
- No history: no pull request numbers, reviewers or "previously". That goes in the commit message.

## Before you finish

Run both lints and fix what they report:

```text
python3 scripts/docs-lint/docs_lint.py check --warnings-as-errors
python3 scripts/lint/lint.py check --warnings-as-errors
```

Shorten the text until they pass. Do not raise a baseline (`update --allow-increase`) or add a `lint-ok` or
`docs-lint-ok` marker to make a check pass; those are for a person to decide.
