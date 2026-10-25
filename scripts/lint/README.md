# Structure lint

`lint.py` measures the structure of HellForge's own C#, C and C++ sources: how complex, long and wide each function
is, how large each file is, and which code is duplicated. It is Bingus Shared Runtime's code-structure lint (rules
R10-R15 of its `lint.py` and `metrics.py`), ported to these languages, with the same rule numbers, commands, baseline
and `lint-ok` comments. Bingus's R1-R9 are about Lua's shared state and do not apply here.

It needs Python 3.10 or newer (CI runs 3.10) and nothing else: it tokenizes and parses the sources itself and never
runs a compiler. On Windows run it with `py -3` (a bare `python` can open the Microsoft Store's alias instead); on
Linux and macOS, with `python3`.

| File | What |
| --- | --- |
| `lint.py` | The rules, units, baseline and command line |
| `structure.py` | Tokens, functions and metrics of C#, C and C++ |
| `lint-config.json` | What is linted (below) |
| `lint-baseline.json` | Known violations, with their measured values |
| `tests/` | `test_structure.py` and `test_lint.py` |

## Rules

| Rule | Catches | Why |
| --- | --- | --- |
| R10 `cognitive-complexity` | A function with cognitive complexity above 15 (below) | Deeply nested branching is hard to follow and to test. The score grows with nesting, not length. Split it into named helpers. |
| R11 `function-size` | A function of more than 150 code lines. Blank and comment-only lines do not count, nor do the bodies of nested lambdas and local functions, which are measured on their own. | A long function does several jobs. |
| R12 `parameters` | A function with more than 7 parameters (`void` and `...` do not count) | Every caller depends on the order of all of them. Pass a record or a struct. |
| R13 `stack-size` (C, C++) | A function whose fixed-size local arrays of scalar or pointer type take more than 16 KiB together (`static` ones do not count) | Native code runs on the game's threads, with unknown stack headroom in hooks and detours, and a stack overflow kills the game. Make the buffer `static`, or allocate it once. |
| R14 `duplicated-code` | A run of at least 100 tokens that also occurs elsewhere, in the same file or another file of the same language family (C#; C and C++) | A fix made in one copy misses the other. Share one copy. |
| R15 `module-size` | A source file of more than 1000 code lines | A long file mixes concerns. |

**Severity.** For `check`, R12, R13 and R14 are errors and R10, R11 and R15 are warnings (`severity` in
`lint-config.json`). What that means is under "The baseline" below.

**Cognitive complexity** follows SonarSource's 2017 definition:
- `if`, `switch` (statements and C#'s switch expressions), `for`, `foreach`, `while`, `do`, `catch` and `?:` each add
  1, plus 1 for every enclosing one of them;
- `else`, `else if` and `goto` add 1 each, without the nesting increment;
- `case`, `default`, `break`, `continue`, `return`, `try` and `finally` add nothing;
- each run of the same `&&` or `||` operator adds 1, so `a && b && c` adds 1 and `a && b || c` adds 2. A bracket or a
  call argument starts a run of its own and leaves the outer run going;
- a function that calls itself adds 1, once.

As in Bingus, and unlike SonarSource, a block lambda (C# `(x) => { }`, C++ `[...](...) { }`), an anonymous method and
a C# local function are measured on their own, from nesting 0, and not charged to the function around them. An
expression lambda (`x => a && b`) is part of the function it is written in.

**Suppression.** Put `// lint-ok: R<n> <reason>` (or `/* lint-ok: R<n> <reason> */`) on the reported line. The reason
is required. To name several rules, write `R10,R11`. R10-R13 are reported on the line of the function's name (a
lambda: where it starts), R14 on the first line of the duplicated run, and R15 on the file's first line.

## What the parser sees

- **Tokens:** comments, preprocessor lines (with continuation lines and their comments), strings and characters (C's
  prefixes and raw strings; C#'s verbatim, interpolated and raw strings, holes included), numbers and operators.
- **Conditional compilation:** of an `#if`/`#ifdef`/`#ifndef` group only the first branch is read (of `#if 0`, its
  `#else`), so code split across branches still balances. Code in the other branches is not measured.
- **Functions:**
  - C and C++: definitions at file, namespace, `extern "C"` and class scope (`Class::Method`, constructors with
    member initializers, `operator...`), and lambdas;
  - C#: methods, constructors, operators, accessors (`get`, `set`, `init`, `add`, `remove`), expression-bodied members,
    local functions, block lambdas and anonymous methods. A record's or class's primary constructor is not a function.
- **C and C++ inside a function:** `NAME(...) { ... }` at the start of a statement is a macro's block (such as
  `FOR_EACH(x) { }`) and adds nothing.
- **Unbalanced brackets are a parse error** (exit code 2).
- **Stack size:** sizes are known for C's and Windows' scalar types and pointers, and `MAX_PATH`. Arrays of structs, and
  sizes that use other names (`buf[COUNT]`), are not counted. A size is C integer arithmetic (`+ - * / << >>` and
  brackets) with no exponent (`9**9` is no size), and values from 2^64 on are refused.
- **C# nullable types:** `T?` before a name and `=`, `;`, `,`, `)`, `{` or `=>`, or before `>`, `,`, `)`, `]`, `;` or `=`,
  is not a `?:`.

## Running

From the repository root:

```
py -3 scripts/lint/lint.py                         # unit x rule table
py -3 scripts/lint/lint.py scan --list             # every violation, with its detail
py -3 scripts/lint/lint.py scan --json             # machine-readable output
py -3 scripts/lint/lint.py check                   # what a change adds against the baseline (CI runs this)
py -3 scripts/lint/lint.py update                  # rewrite the baseline after fixes; refuses increases
py -3 scripts/lint/lint.py update --allow-increase # accept increases: reviewed ones
```

Options:
- `--only NAME` limits the scan, the check and the update to these units. R14 still compares them with every unit.
- `--unit NAME=PATH` lints a folder as unit NAME, for example another checkout of one project.
- `--config`, `--baseline` and `--root` choose other files and the folder config paths start from (default: the
  repository).

## The baseline

`lint-baseline.json` lists the violations the code has today, so the lint can run on code written before it. `check`
compares a scan with it:

- **Identity:** (unit, file, rule, function), with the measured value: complexity, lines, parameters, bytes,
  duplicated tokens or file lines. R14's function is the one its run starts in (none at file scope); R15's identity is
  the file. A changed signature line, an added parameter or a moved function keeps the identity; a renamed function, or
  one moved to another file, has a new one.
- **Same names:** functions that share a name in one file (overloads, lambdas in one method) share an identity. Their
  values are paired largest first, and the ones without a counterpart are new or gone.
- **What fails:** a violation of an error rule (R12, R13, R14) that the baseline does not have: new code over a limit,
  or an existing function pushed over one for the first time. A duplicated run counts at both of its copies.
- **What warns:** the same for a warning rule (R10, R11, R15), and any known violation that grew (`known, grew:
  baseline 16, now 17`). Warnings do not fail `check`.
- **What is fixed:** a known violation that got smaller or went is listed as "lower - run update"; `update` then
  lowers the baseline.
- Each error and warning names its line, function and detail, and ends with a pointer here and to `lint-ok`. In GitHub
  Actions each one is also an annotation on its line in the pull request.
- Then: fix it (the "Why" column above says how), or, when the code is right as it is, mark the reported line
  `// lint-ok: R<n> <reason>`. A reviewed increase is accepted with `update --allow-increase`, which rewrites the
  baseline with what the scan found.
- `update` keeps the entries of units outside `--only`.

Exit codes: 0 means OK (warnings allowed); 1 means errors (`check`) or a refused update; 2 means a configuration or
parse error, or a baseline of an older format (regenerate it with `update --allow-increase`).

## Configuration

```json
{
  "format": 1,
  "discover": ["src", "native", "examples/mods"],
  "units": [{"name": "c-mod-template", "path": "templates/c-mod"}],
  "default_sources": ["**/*.cs", "**/*.c", "**/*.h", "**/*.cpp"],
  "exclude": ["**/bin/**", "**/obj/**", "**/test/**", "**/tests/**", "native/editor/ui_logo.h"],
  "severity": {"R10": "warning", "R11": "warning", "R12": "error", "R13": "error", "R14": "error", "R15": "warning"},
  "limits": {"cognitive": 15, "function_lines": 150, "parameters": 7, "stack_bytes": 16384, "module_lines": 1000,
             "clone_tokens": 100}
}
```

- **`discover`:** every folder under these folders is a unit, named after it: each project under `src/`, each
  component under `native/`, each example mod.
- **`units`:** a unit of its own, by `path`, or other `sources` for a discovered one.
- **`exclude`:** paths relative to the repository (`fnmatch` patterns). Tests are not linted, as in Bingus, and nor are
  generated files: the editor's image headers, the mod loader's generated tables and the SDK's generated headers;
  nor the session runner's copies of `scripts/` experiments kept as sessions compiled them (`EarlyConstructor.cs`,
  `HostBoundedProcess.cs`, `LocalRoutingPatch.cs`), since the `scripts/` originals are not linted either.
- **`severity`:** `error` or `warning` per rule, for `check`; a rule left out keeps its default (above).
- **`limits`:** any key may be left out; an unknown key stops the lint with exit code 2.

## Tests

```
py -3 -m unittest discover -s scripts/lint/tests -v
```

- `test_structure.py`: the tokenizer (directives and their comments, `#if` branches, every string form), which
  constructs are functions in each language, cognitive complexity against SonarSource's examples (`sumOfPrimes` scores
  7, a `switch` 1), `else`, boolean runs, `?:` and C#'s nullable `?`, loops, `catch`, recursion, lambdas measured
  alone, macro blocks, code lines, stack bytes and array sizes, and duplicated code.
- `test_lint.py`: every rule exactly at its limit (silent) beside one just over it (reported), `lint-ok`, R14 within a
  language family and across units, R15, the baseline (an error rule and a warning rule, growth, a changed
  signature and first line, a rename, overloads, a fix, an older format, the severity setting, GitHub
  annotations), `--only`, and the command line's errors.

## In CI

The "Structure lint" job of `.github/workflows/ci.yml` runs these tests and then `check` with Python 3.10 on every pull
request into `dev` or `main` and every push to `dev`, on Linux (the repository's `.gitattributes` keeps every source LF,
so the lint reads the same bytes on any platform). It fails on errors, as above; warnings show as annotations and in
its log. It blocks merging once "Structure lint" is a required check of the `dev` and `main` rulesets, beside "Build
and test" (Settings, Rules, Rulesets: "Require status checks to pass").
