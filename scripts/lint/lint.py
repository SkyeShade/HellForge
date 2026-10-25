"""HellForge's structure lint: Bingus Shared Runtime's code-structure rules (R10-R15) for C#, C and C++.

  py -3 scripts/lint/lint.py                         # unit x rule table
  py -3 scripts/lint/lint.py scan --list             # every violation, with its detail
  py -3 scripts/lint/lint.py scan --json             # machine-readable output
  py -3 scripts/lint/lint.py check                   # what a change adds against the baseline (CI runs this)
  py -3 scripts/lint/lint.py update                  # rewrite the baseline after fixes; refuses increases
  py -3 scripts/lint/lint.py update --allow-increase # accept increases (reviewed ones)

(`python3` instead of `py -3` on Linux and macOS.) Rules (structure.py measures them): R10 cognitive complexity, R11
function size, R12 parameters, R13 stack size (C and C++), R14 duplicated code, R15 file size. Suppress one on its
reported line with `// lint-ok: R<n> <reason>`. The baseline holds the measured values per (unit, file, rule,
function). `check` fails on a violation of an error rule (R12, R13, R14) that the baseline does not have: new code, or a
function pushed over a limit for the first time. A new violation of a warning rule (R10, R11, R15), and a known one that
grew, are warnings. Exit codes: 0 OK (warnings allowed); 1 errors, or a refused update; 2 a configuration or parse
error.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import structure  # noqa: E402

DEFAULT_CONFIG = HERE / 'lint-config.json'
DEFAULT_BASELINE = HERE / 'lint-baseline.json'
BASELINE_FORMAT = 2
RULES = {
    'R10': 'cognitive-complexity',
    'R11': 'function-size',
    'R12': 'parameters',
    'R13': 'stack-size',
    'R14': 'duplicated-code',
    'R15': 'module-size',
}
# What `check` does with a violation the baseline does not have; growth in a known one is a warning for every rule.
SEVERITY = {'R10': 'warning', 'R11': 'warning', 'R12': 'error', 'R13': 'error', 'R14': 'error', 'R15': 'warning'}
SUPPRESS_RE = re.compile(r'(?://|/\*|\*)\s*lint-ok:\s*((?:R\d+\s*[, ]\s*)*R\d+)\s+\S')
FAMILIES = {'cs': 'cs', 'c': 'c', 'cpp': 'c'}        # R14 compares within a family: C# alone, C with C++
HELP = ('How to fix one: scripts/lint/README.md ("Rules" says what each rule wants, "The baseline" what check does). '
        'When the code is right as it is, put `// lint-ok: R<n> <reason>` on the reported line; a reviewed increase is '
        'accepted with `py -3 scripts/lint/lint.py update --allow-increase`.')


class LintError(Exception):
    pass


@dataclass
class Unit:
    name: str
    path: Path
    sources: list[str]


@dataclass
class Violation:
    unit: str
    rule: str
    file: str
    line: int
    text: str
    detail: str
    value: int
    function: str = ''     # R10-R13: the function; R14: the one its run starts in ('' at file scope); R15: ''
    path: str = ''         # the file from the repository's root, for annotations

    def key(self) -> Key:
        return (self.unit, self.file, self.rule, self.function)

    def as_json(self) -> dict:
        return {'unit': self.unit, 'rule': self.rule, 'name': RULES[self.rule], 'file': self.file, 'line': self.line,
                'function': self.function, 'text': self.text, 'detail': self.detail, 'value': self.value}


Key = tuple[str, str, str, str]  # unit, file, rule, function


@dataclass
class Scan:
    units: list[Unit]
    violations: list[Violation] = field(default_factory=list)
    suppressed: list[Violation] = field(default_factory=list)
    files: int = 0
    functions: int = 0


@dataclass
class Parsed:
    unit: Unit
    rel: str
    path: str
    lines: list[str]
    model: structure.Model
    language: str


def suppressed_rules(line: str) -> set[str]:
    match = SUPPRESS_RE.search(line)
    return set(re.findall(r'R\d+', match.group(1))) if match else set()


# ----------------------------------------------------------------------------- rules

def structure_findings(model: structure.Model, language: str, limits: dict) -> list[tuple]:
    """R10-R13 and R15 for one file: (rule, line, detail, measured value, function)."""
    found = []
    for function in model.functions:
        where, name = f'{function.name}: ', function.name
        if function.cognitive > limits['cognitive']:
            found.append(('R10', function.line, f"{where}cognitive complexity {function.cognitive} (limit "
                          f"{limits['cognitive']}), nesting depth {function.nesting}", function.cognitive, name))
        size = len(function.lines)
        if size > limits['function_lines']:
            found.append(('R11', function.line, f"{where}{size} code lines (limit {limits['function_lines']})", size,
                          name))
        if function.parameters > limits['parameters']:
            found.append(('R12', function.line, f"{where}{function.parameters} parameters (limit "
                          f"{limits['parameters']})", function.parameters, name))
        if language != 'cs' and function.stack_bytes > limits['stack_bytes']:
            found.append(('R13', function.line, f"{where}{function.stack_bytes} bytes of local arrays on the stack "
                          f"(limit {limits['stack_bytes']}); make them static or allocate them once",
                          function.stack_bytes, name))
    if model.code_lines > limits['module_lines']:
        found.append(('R15', 1, f"{model.code_lines} code lines in one file (limit {limits['module_lines']})",
                      model.code_lines, ''))
    return found


def enclosing(functions: list[structure.Function], line: int) -> str:
    """The innermost function with code on this line ('' at file scope): R14's identity."""
    holders = [f for f in functions if line in f.lines]
    return min(holders, key=lambda f: len(f.lines)).name if holders else ''


def violations(item: Parsed, findings: list[tuple]) -> tuple[list[Violation], list[Violation]]:
    """One violation per rule, line and function (details joined, values added); those marked lint-ok are
    suppressed."""
    merged: dict[tuple[str, int, str], tuple[list[str], int]] = {}
    for rule, line, detail, value, function in findings:
        details, total = merged.get((rule, line, function), ([], 0))
        if detail not in details:
            details.append(detail)
        merged[(rule, line, function)] = (details, total + value)
    kept, dropped = [], []
    for (rule, line, function), (details, value) in sorted(merged.items(), key=lambda item: (item[0][1], item[0][0])):
        text = item.lines[line - 1] if 0 < line <= len(item.lines) else ''
        violation = Violation(item.unit.name, rule, item.rel, line, text.strip(), '; '.join(details), value, function,
                              item.path)
        (dropped if rule in suppressed_rules(text) else kept).append(violation)
    return kept, dropped


def duplicated_code(parsed: list[Parsed], length: int, report: set[str]) -> tuple[list[Violation], list[Violation]]:
    """R14: regions of at least `length` tokens that occur again in a file of the same language family."""
    kept, dropped = [], []
    for family in sorted(set(FAMILIES.values())):
        group = [p for p in parsed if FAMILIES[p.language] == family]
        files = [p.model.tokens for p in group]
        findings: dict[int, list[tuple]] = {}
        for clone in structure.duplicates(files, length):
            here = group[clone.file]
            if here.unit.name not in report:
                continue
            other = group[clone.other_file]
            other_line = files[clone.other_file][clone.other_start].line
            where = other.rel if (other.unit.name, other.rel) == (here.unit.name, here.rel) else \
                f'{other.unit.name} {other.rel}'
            tokens = clone.end - clone.start
            line = files[clone.file][clone.start].line
            findings.setdefault(clone.file, []).append((
                'R14', line, f'{tokens} tokens also at {where}:{other_line}; share one copy', tokens,
                enclosing(here.model.functions, line)))
        for index, found in findings.items():
            k, d = violations(group[index], found)
            kept += k
            dropped += d
    return kept, dropped


# ----------------------------------------------------------------------------- scanning

def unit_files(unit: Unit, excludes: list[str], root: Path) -> list[Path]:
    if not unit.path.is_dir():
        raise LintError(f'{unit.name}: folder not found: {unit.path}')
    found: set[Path] = set()
    for pattern in unit.sources:
        found.update(p for p in unit.path.glob(pattern) if p.is_file() and p.suffix.lower() in structure.LANGUAGES)
    kept = []
    for path in sorted(found):
        if not any(fnmatch.fnmatch(from_root(path, root), pattern) for pattern in excludes):
            kept.append(path)
    return kept


def from_root(path: Path, root: Path) -> str:
    full = path.resolve()
    return full.relative_to(root).as_posix() if full.is_relative_to(root) else path.as_posix()


def parse(unit: Unit, path: Path, root: Path) -> Parsed:
    text = path.read_text(encoding='utf-8-sig', errors='replace')
    language = structure.LANGUAGES[path.suffix.lower()]
    rel = path.relative_to(unit.path).as_posix()
    try:
        model = structure.analyse(text, language)
    except structure.ParseError as problem:
        raise LintError(f'{unit.name}/{rel}: {problem}') from None
    return Parsed(unit, rel, from_root(path, root), text.splitlines(), model, language)


def scan(units: list[Unit], corpus: list[Unit], limits: dict, excludes: list[str], root: Path) -> Scan:
    """Scans units; R14 compares them with every unit of the corpus, whatever --only selected."""
    result = Scan(units)
    selected = {unit.name for unit in units}
    parsed: list[Parsed] = []
    for unit in corpus:
        for path in unit_files(unit, excludes, root):
            item = parse(unit, path, root)
            parsed.append(item)
            if unit.name not in selected:
                continue
            kept, dropped = violations(item, structure_findings(item.model, item.language, limits))
            result.violations += kept
            result.suppressed += dropped
            result.files += 1
            result.functions += len(item.model.functions)
    kept, dropped = duplicated_code(parsed, limits['clone_tokens'], selected)
    result.violations += kept
    result.suppressed += dropped
    result.violations.sort(key=lambda v: (v.unit, v.file, v.line, v.rule))
    return result


# ----------------------------------------------------------------------------- configuration

@dataclass
class Config:
    selected: list[Unit]
    corpus: list[Unit]
    limits: dict
    excludes: list[str]
    severity: dict


def read_severity(config: dict, path: Path) -> dict:
    given = config.get('severity', {})
    unknown = set(given) - set(RULES)
    if unknown:
        raise LintError(f'unknown rules in {path.name} severity: {", ".join(sorted(unknown))}')
    wrong = {rule: level for rule, level in given.items() if level not in ('error', 'warning')}
    if wrong:
        raise LintError(f'{path.name} severity: {", ".join(sorted(wrong))} must be "error" or "warning"')
    return dict(SEVERITY, **given)


def read_units(config: dict, root: Path, overrides: list[str]) -> dict[str, Unit]:
    """Each folder under a `discover` folder, those named in `units`, and --unit's."""
    sources = list(config.get('default_sources', ['**/*']))
    units: dict[str, Unit] = {}
    for folder in config.get('discover', []):
        base = root / folder
        if not base.is_dir():
            raise LintError(f'discover folder not found: {base}')
        for child in sorted(p for p in base.iterdir() if p.is_dir()):
            units[child.name] = Unit(child.name, child, sources)
    for entry in config.get('units', []):
        unit_path = (root / entry['path']) if 'path' in entry else units[entry['name']].path
        units[entry['name']] = Unit(entry['name'], unit_path, list(entry.get('sources', sources)))
    for override in overrides:
        name, sep, folder = override.partition('=')
        if not sep or not name or not folder:
            raise LintError(f'--unit expects NAME=PATH, got {override!r}')
        units[name] = Unit(name, Path(folder).resolve(), units[name].sources if name in units else sources)
    return units


def load_config(path: Path, root: Path, overrides: list[str], only: list[str]) -> Config:
    try:
        config = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as problem:
        raise LintError(f'cannot read config {path}: {problem}') from None
    unknown = set(config.get('limits', {})) - set(structure.LIMITS)
    if unknown:
        raise LintError(f'unknown limits in {path.name}: {", ".join(sorted(unknown))}')
    limits = dict(structure.LIMITS, **config.get('limits', {}))
    units = read_units(config, root, overrides)
    corpus = list(units.values())
    selected = corpus
    if only:
        wanted = {name for item in only for name in item.split(',') if name}
        missing = wanted - set(units)
        if missing:
            raise LintError(f'--only names unknown units: {", ".join(sorted(missing))}')
        selected = [unit for unit in corpus if unit.name in wanted]
    return Config(selected, corpus, limits, list(config.get('exclude', [])), read_severity(config, path))


# ----------------------------------------------------------------------------- baseline

def read_baseline(path: Path, replacing: bool = False) -> dict[Key, list[int]]:
    """The baseline's entries; with `replacing` (update --allow-increase), one of an older format reads as empty."""
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        if data.get('format') != BASELINE_FORMAT and replacing:
            return {}
        if data.get('format') != BASELINE_FORMAT:
            raise LintError(f'{path.name} is baseline format {data.get("format")}, this lint reads format '
                            f'{BASELINE_FORMAT} (keyed by function): regenerate it with `update --allow-increase`')
        entries: dict[Key, list[int]] = {}
        for e in data['entries']:
            entries.setdefault((e['unit'], e['file'], e['rule'], e['function']), []).append(int(e['value']))
        return entries
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as problem:
        raise LintError(f'cannot read baseline {path}: {problem}') from None


def write_baseline(path: Path, entries: dict[Key, list[int]]) -> None:
    rows = [json.dumps({'unit': k[0], 'file': k[1], 'rule': k[2], 'function': k[3], 'value': n}, ensure_ascii=False)
            for k, values in sorted(entries.items()) for n in sorted(values, reverse=True)]
    body = ('{\n  "format": ' + str(BASELINE_FORMAT) + ',\n'
            '  "note": "Known structure violations per (unit, file, rule, function) with the measured values. '
            '`py -3 scripts/lint/lint.py check` reports what is new or worse (scripts/lint/README.md); `update` '
            'rewrites this file.",\n'
            '  "entries": [' + ('\n    ' + ',\n    '.join(rows) + '\n  ' if rows else '') + ']\n}\n')
    path.write_text(body, encoding='utf-8', newline='\n')


def by_key(found: list[Violation]) -> dict[Key, list[Violation]]:
    result: dict[Key, list[Violation]] = {}
    for violation in found:
        result.setdefault(violation.key(), []).append(violation)
    return result


@dataclass
class Change:
    kind: str                  # 'new', 'grew' or 'lower' (lower or gone)
    key: Key
    old: int                   # 0 when new
    new: int                   # 0 when gone
    violation: Violation | None = None


def compare(baseline: dict[Key, list[int]], current: dict[Key, list[Violation]], scope: set[str]):
    """(changes, known count). Same-named functions in a file (overloads, lambdas) share a key: their values are paired
    largest first, and the ones without a counterpart are new or gone."""
    changes, known = [], 0
    for key in sorted(set(baseline) | set(current)):
        if key[0] not in scope:
            continue
        olds = sorted(baseline.get(key, []), reverse=True)
        news = sorted(current.get(key, []), key=lambda v: v.value, reverse=True)
        for i, violation in enumerate(news):
            if i >= len(olds):
                changes.append(Change('new', key, 0, violation.value, violation))
            elif violation.value > olds[i]:
                changes.append(Change('grew', key, olds[i], violation.value, violation))
            else:
                known += 1
                if violation.value < olds[i]:
                    changes.append(Change('lower', key, olds[i], violation.value, violation))
        changes += [Change('lower', key, old, 0) for old in olds[len(news):]]
    return changes, known


# ----------------------------------------------------------------------------- output

def summary_table(result: Scan) -> str:
    rules = list(RULES)
    table = {unit.name: {rule: 0 for rule in rules} for unit in result.units}
    for violation in result.violations:
        table[violation.unit][violation.rule] += 1
    width = max([len('unit')] + [len(name) for name in table]) + 2
    lines = ['unit'.ljust(width) + ''.join(r.rjust(5) for r in rules) + 'total'.rjust(7)]
    totals = {rule: 0 for rule in rules}
    for name, row in table.items():
        lines.append(name.ljust(width) + ''.join(str(row[r]).rjust(5) for r in rules) + str(sum(row.values())).rjust(7))
        for rule in rules:
            totals[rule] += row[rule]
    lines.append('total'.ljust(width) + ''.join(str(totals[r]).rjust(5) for r in rules) + str(sum(totals.values())).rjust(7))
    return '\n'.join(lines)


def describe(violation: Violation) -> str:
    return (f'{violation.rule} {RULES[violation.rule]}  {violation.unit} {violation.file}:{violation.line}\n'
            f'    {violation.text}\n    -> {violation.detail}')


def what(change: Change) -> str:
    """Why a change is reported, in words."""
    rule = change.key[2]
    if change.kind == 'grew':
        return f'known, grew: baseline {change.old}, now {change.new}'
    if rule == 'R15':
        return 'over the limit, not in the baseline'
    return 'new, or over the limit for the first time'


def report(level: str, change: Change) -> None:
    unit, file, rule, function = change.key
    violation = change.violation
    line = f':{violation.line}' if violation else ''
    name = f'  {function}' if function else ''
    print(f'{level.upper()} {rule} {RULES[rule]}  {unit} {file}{line}{name}  ({what(change)})')
    if violation:
        print(f'    {violation.text}\n    -> {violation.detail}')
        if level in ('error', 'warning') and os.environ.get('GITHUB_ACTIONS') == 'true':
            # an annotation on the line in the pull request's files
            print(f'::{level} file={violation.path},line={violation.line},title={rule} {RULES[rule]}::'
                  f'{violation.detail} ({what(change)}). Fix it, or mark it lint-ok: scripts/lint/README.md')


def report_lower(change: Change) -> None:
    unit, file, rule, function = change.key
    name = f'  {function}' if function else ''
    now = f'now {change.new}' if change.new else 'gone'
    print(f'lower - run update: {rule} {RULES[rule]}  {unit} {file}{name}  (baseline {change.old}, {now})')


# ----------------------------------------------------------------------------- commands

def command_scan(result: Scan, config: Config, args) -> int:
    if args.json:
        summary: dict[str, dict[str, int]] = {unit.name: {} for unit in result.units}
        for violation in result.violations:
            summary[violation.unit][violation.rule] = summary[violation.unit].get(violation.rule, 0) + 1
        print(json.dumps({'rules': RULES, 'severity': config.severity, 'files': result.files,
                          'functions': result.functions, 'summary': summary,
                          'violations': [v.as_json() for v in result.violations],
                          'suppressed': [v.as_json() for v in result.suppressed]}, indent=2, ensure_ascii=False))
        return 0
    if args.list:
        for violation in result.violations:
            print(describe(violation))
        print()
    print(summary_table(result))
    print(f'\n{len(result.violations)} violation(s) in {result.files} file(s), {result.functions} function(s); '
          f'{len(result.suppressed)} suppressed by lint-ok')
    return 0


def command_check(result: Scan, config: Config, args) -> int:
    changes, known = compare(read_baseline(args.baseline), by_key(result.violations), {u.name for u in result.units})
    errors = [c for c in changes if c.kind == 'new' and config.severity[c.key[2]] == 'error']
    warnings = [c for c in changes if c.kind == 'grew' or (c.kind == 'new' and config.severity[c.key[2]] == 'warning')]
    lower = [c for c in changes if c.kind == 'lower']
    for change in errors:
        report('error', change)
    for change in warnings:
        report('warning', change)
    for change in lower:
        report_lower(change)
    if errors or warnings:
        print(f'\n{HELP}')
    print(f'lint check: {len(errors)} error(s), {len(warnings)} warning(s), {known} known, {len(lower)} lower or gone '
          f'in {len(result.units)} unit(s); baseline {args.baseline.name}')
    return 1 if errors else 0


def command_update(result: Scan, config: Config, args) -> int:
    baseline = read_baseline(args.baseline, replacing=args.allow_increase)
    scope = {unit.name for unit in result.units}
    changes, _ = compare(baseline, by_key(result.violations), scope)
    increases = [c for c in changes if c.kind in ('new', 'grew')]
    if increases and not args.allow_increase:
        for change in increases:
            report('increase', change)
        print(f'lint update refused: {len(increases)} value(s) would be new or higher; fix them, add a '
              f'`// lint-ok: R<n> <reason>` comment, or rerun with --allow-increase (scripts/lint/README.md)')
        return 1
    entries = {key: values for key, values in baseline.items() if key[0] not in scope}
    for key, found in by_key(result.violations).items():
        entries[key] = [v.value for v in found]
    write_baseline(args.baseline, entries)
    lower = sum(1 for c in changes if c.kind == 'lower')
    print(f'baseline updated: {sum(len(v) for v in entries.values())} violation(s); {lower} lower or gone, '
          f'{len(increases)} new or higher than before')
    return 0


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')
    parser = argparse.ArgumentParser(prog='lint.py', description=__doc__.split('\n\n')[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', nargs='?', default='scan', choices=('scan', 'check', 'update'))
    parser.add_argument('--unit', action='append', default=[], metavar='NAME=PATH', help='lint this folder as NAME')
    parser.add_argument('--only', action='append', default=[], metavar='NAME', help='limit to these units')
    parser.add_argument('--json', action='store_true', help='scan: machine-readable output')
    parser.add_argument('--list', action='store_true', help='scan: print every violation')
    parser.add_argument('--allow-increase', action='store_true', help='update: accept new or higher values')
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--baseline', type=Path, default=DEFAULT_BASELINE)
    parser.add_argument('--root', type=Path, default=HERE.parent.parent,
                        help='the folder config paths start from (default: the repository)')
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        config = load_config(args.config, root, args.unit, args.only)
        result = scan(config.selected, config.corpus, config.limits, config.excludes, root)
        return {'scan': command_scan, 'check': command_check, 'update': command_update}[args.command](result, config,
                                                                                                    args)
    except LintError as problem:
        print(f'lint.py: {problem}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
