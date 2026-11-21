"""HellForge's docs lint: page types and prose sizes of the repository's Markdown files.

  py -3 scripts/docs-lint/docs_lint.py                          # file x rule table
  py -3 scripts/docs-lint/docs_lint.py scan --list              # every violation, with its detail
  py -3 scripts/docs-lint/docs_lint.py scan --json              # machine-readable output
  py -3 scripts/docs-lint/docs_lint.py check                    # what a change adds against the baseline
  py -3 scripts/docs-lint/docs_lint.py check --warnings-as-errors  # the same, warnings fail too (CI runs this)
  py -3 scripts/docs-lint/docs_lint.py update                   # rewrite the baseline after fixes
  py -3 scripts/docs-lint/docs_lint.py update --allow-increase  # accept new or higher values (reviewed ones)
  py -3 scripts/docs-lint/docs_lint.py stats FILE...            # words, paragraphs and sentences of pages

(`python3` instead of `py -3` on Linux and macOS.) Rules: D1 page type, D2 index, D3 paragraph size, D4 sentence
length, D5 page size. scripts/docs-lint/README.md says what each wants and how the baseline works.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_CONFIG = HERE / 'docs-lint-config.json'
DEFAULT_BASELINE = HERE / 'docs-lint-baseline.json'
BASELINE_FORMAT = 1
RULES = {
    'D1': 'page-type',
    'D2': 'index',
    'D3': 'paragraph-size',
    'D4': 'sentence-length',
    'D5': 'page-size',
}
PAGE_RULES = ('D1', 'D2', 'D5')
SEVERITY = {'D1': 'error', 'D2': 'error', 'D3': 'warning', 'D4': 'warning', 'D5': 'warning'}
LIMITS = {'paragraph_words': 100, 'sentence_words': 35, 'page_words': 5000}
TYPES = ('tutorial', 'how-to', 'reference', 'explanation')
TYPE_RE = re.compile(r'<!--\s*diataxis:\s*([a-z-]+)\s*-->')
SUPPRESS_RE = re.compile(r'<!--\s*docs-lint-ok:\s*((?:D\d+\s*[, ]\s*)*D\d+)\s+\S[^>]*-->')
HELP = ('How to fix one: scripts/docs-lint/README.md ("Rules" says what each rule wants). When the text is right as it '
        'is, put `<!-- docs-lint-ok: D<n> <reason> -->` on the line above it; a reviewed increase is accepted with '
        '`py -3 scripts/docs-lint/docs_lint.py update --allow-increase`.')


class DocsLintError(Exception):
    pass


# ----------------------------------------------------------------------------- Markdown

@dataclass
class Block:
    kind: str          # paragraph, item (a list item's text), quote
    line: int          # first line, from 1
    text: str          # the raw text, markers removed
    section: str       # the H2 heading it is under ('' above the first)


@dataclass
class Page:
    path: str                      # from the repository's root, with /
    lines: list[str]
    blocks: list[Block]
    type: str | None               # the diataxis marker's value, or None
    type_line: int                 # where the marker is (0: none)
    links: set[str] = field(default_factory=set)   # relative link targets, resolved from the root, no anchors


FENCE_RE = re.compile(r'^\s*(`{3,}|~{3,})')
HEADING_RE = re.compile(r'^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$')
ITEM_RE = re.compile(r'^(\s*)([-*+]|\d{1,9}[.)])\s+(.*)$')
RULE_LINE_RE = re.compile(r'^\s{0,3}([-*_])(\s*\1){2,}\s*$')
LINK_RE = re.compile(r'\[([^\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)')


def parse_page(path: str, text: str) -> Page:
    lines = text.splitlines()
    blocks: list[Block] = []
    headings: list[tuple[int, str]] = []
    current: list[str] = []
    start, kind = 0, ''
    page_type, type_line = None, 0
    fence = ''
    in_comment = False

    def section() -> str:
        return next((title for level, title in reversed(headings) if level == 2), '')

    def flush() -> None:
        nonlocal current, kind
        if current:
            blocks.append(Block(kind, start, '\n'.join(current), section()))
        current, kind = [], ''

    number = 0
    if lines and lines[0].strip() == '---':                     # front matter
        for number in range(1, len(lines)):
            if lines[number].strip() in ('---', '...'):
                break
        number += 1
    while number < len(lines):
        raw = lines[number]
        number += 1
        if fence:
            if raw.strip().startswith(fence):
                fence = ''
            continue
        if in_comment:
            if '-->' in raw:
                in_comment = False
            continue
        match = TYPE_RE.search(raw)
        if match and page_type is None:
            page_type, type_line = match.group(1), number
        stripped = raw.strip()
        if stripped.startswith('<!--'):
            flush()
            in_comment = '-->' not in stripped
            continue
        fence_match = FENCE_RE.match(raw)
        if fence_match:
            flush()
            fence = fence_match.group(1)[0] * 3
            continue
        if not stripped or RULE_LINE_RE.match(raw):
            flush()
            continue
        heading = HEADING_RE.match(raw)
        if heading:
            flush()
            level = len(heading.group(1))
            headings = [h for h in headings if h[0] < level] + [(level, heading.group(2))]
            continue
        if stripped.startswith('|') or (stripped.startswith('<') and not stripped.startswith('<http')):
            flush()                                             # tables and HTML blocks are not prose blocks
            continue
        item = ITEM_RE.match(raw)
        if item:
            flush()
            start, kind = number, 'item'
            current = [item.group(3)]
            continue
        if stripped.startswith('>'):
            if kind != 'quote':
                flush()
                start, kind = number, 'quote'
            current.append(stripped.lstrip('>').strip())
            continue
        if not current:
            start, kind = number, 'paragraph'
        current.append(stripped)
    flush()
    page = Page(path, lines, blocks, page_type, type_line)
    folder = path.rsplit('/', 1)[0] if '/' in path else ''
    for _, target in LINK_RE.findall(text):
        if re.match(r'^[a-z][a-z0-9+.-]*:', target) or target.startswith('#'):
            continue
        target = target.split('#', 1)[0]
        resolved = os.path.normpath(os.path.join(folder, target)).replace('\\', '/')
        page.links.add(resolved)
    return page


CODE_SPAN_RE = re.compile(r'(`+)(.+?)\1')
IMAGE_RE = re.compile(r'!\[([^\]]*)\]\([^)]*\)')
REF_LINK_RE = re.compile(r'\[([^\]]+)\]\[[^\]]*\]')
TAG_RE = re.compile(r'</?[A-Za-z][^>]*>')
ABBREVIATIONS = ('e.g.', 'i.e.', 'etc.', 'vs.', 'cf.', 'approx.', 'no.', 'Fig.', 'Dr.', 'Mr.', 'Ms.', 'St.')
HOLD = '․'    # stands in for an abbreviation's full stops while sentences are split
# A sentence ends at . ! or ? (and closing quotes or brackets) before a space and a capital, digit, quote or bracket.
SENTENCE_END_RE = re.compile(r'(?<=[.!?])["\'”’)\]]*\s+(?=[A-Z0-9"\'“‘(\[])')


def plain(text: str) -> str:
    """Prose words only: a code span is one word, a link its text, emphasis and tags gone."""
    text = text.replace('\\<', '‹')                        # an escaped placeholder such as \<host> is a word
    text = CODE_SPAN_RE.sub('CODE', text)
    text = IMAGE_RE.sub(r'\1', text)
    text = LINK_RE.sub(r'\1', text)
    text = REF_LINK_RE.sub(r'\1', text)
    text = TAG_RE.sub(' ', text)
    text = re.sub(r'(\*\*|__|\*|(?<!\w)_|_(?!\w))', '', text)
    return ' '.join(text.split())


def words(text: str) -> int:
    return sum(1 for word in plain(text).split() if re.search(r'[\w]', word))


def sentences(text: str) -> list[str]:
    prose = plain(text)
    for abbreviation in ABBREVIATIONS:
        prose = prose.replace(abbreviation, abbreviation.replace('.', HOLD))
    return [part.replace(HOLD, '.') for part in SENTENCE_END_RE.split(prose) if part.strip()]


# ----------------------------------------------------------------------------- rules

@dataclass
class Violation:
    rule: str
    file: str
    line: int
    section: str
    detail: str
    value: int
    text: str = ''

    def key(self) -> Key:
        return (self.file, self.rule, self.section)

    def as_json(self) -> dict:
        return {'rule': self.rule, 'name': RULES[self.rule], 'file': self.file, 'line': self.line,
                'section': self.section, 'detail': self.detail, 'value': self.value}


Key = tuple[str, str, str]   # file, rule, section ('' for page rules)


@dataclass
class Config:
    include: list[str]
    exclude: list[str]
    typed: list[str]
    untyped: list[str]
    index: str
    limits: dict
    page_words_by_type: dict
    severity: dict


def matches(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def suppressed(page: Page, rule: str, line: int) -> bool:
    if rule in PAGE_RULES:
        candidates = page.lines
    else:
        candidates = page.lines[max(0, line - 2):line]           # the line above, and the line itself
    for text in candidates:
        match = SUPPRESS_RE.search(text)
        if match and rule in re.findall(r'D\d+', match.group(1)):
            return True
    return False


def page_findings(page: Page, config: Config, indexed: set[str] | None) -> list[Violation]:
    found: list[Violation] = []
    limits = config.limits
    first = page.lines[0].strip() if page.lines else ''
    typed = matches(page.path, config.typed) and not matches(page.path, config.untyped)
    if typed and page.type not in TYPES:
        what = f'"{page.type}" is not a type' if page.type else 'no type'
        found.append(Violation('D1', page.path, page.type_line or 1, '', f'{what}: add '
                               f'`<!-- diataxis: {"|".join(TYPES)} -->` under the title', 1, first))
    if typed and indexed is not None and page.path != config.index and page.path not in indexed:
        found.append(Violation('D2', page.path, 1, '', f'not linked from {config.index}', 1, first))
    for block in page.blocks:
        count = words(block.text)
        if count > limits['paragraph_words']:
            noun = 'list item' if block.kind == 'item' else block.kind
            found.append(Violation('D3', page.path, block.line, block.section, f'{count} words in one {noun} (limit '
                                   f"{limits['paragraph_words']}); split it, or make it a list", count,
                                   page.lines[block.line - 1].strip()))
        for sentence in sentences(block.text):
            length = words(sentence)
            if length > limits['sentence_words']:
                found.append(Violation('D4', page.path, block.line, block.section, f'a sentence of {length} words '
                                       f"(limit {limits['sentence_words']}): \"{sentence[:60]}...\"", length,
                                       page.lines[block.line - 1].strip()))
    budget = config.page_words_by_type.get(page.type, limits['page_words']) if page.type else limits['page_words']
    total = sum(words(block.text) for block in page.blocks)
    if budget is not None and total > budget:
        # value 1: a page known to be over is known until it is split; its size is in the detail
        found.append(Violation('D5', page.path, 1, '', f'{total} words of prose on one page (limit {budget}); split '
                               'it by type or by task', 1, first))
    return [v for v in found if not suppressed(page, v.rule, v.line)], \
        [v for v in found if suppressed(page, v.rule, v.line)]


# ----------------------------------------------------------------------------- scanning

def listed_files(root: Path) -> list[str] | None:
    """Tracked and untracked, not ignored, files from git; None when root is not a work tree."""
    try:
        out = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=root, capture_output=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    if not (root / '.git').exists():
        return None
    return sorted({name for name in out.decode('utf-8', 'replace').split('\0') if name})


def markdown_files(root: Path, config: Config) -> list[str]:
    names = listed_files(root)
    if names is None:
        names = sorted(p.relative_to(root).as_posix() for p in root.rglob('*.md') if p.is_file())
    return [n for n in names if n.endswith('.md') and matches(n, config.include) and not matches(n, config.exclude)
            and (root / n).is_file()]


@dataclass
class Scan:
    files: int = 0
    words: int = 0
    violations: list[Violation] = field(default_factory=list)
    suppressed: list[Violation] = field(default_factory=list)


def scan(root: Path, config: Config) -> Scan:
    result = Scan()
    pages = [parse_page(name, (root / name).read_text(encoding='utf-8-sig', errors='replace'))
             for name in markdown_files(root, config)]
    index = next((p for p in pages if p.path == config.index), None)
    indexed = index.links if index else None
    for page in pages:
        kept, dropped = page_findings(page, config, indexed)
        result.violations += kept
        result.suppressed += dropped
        result.files += 1
        result.words += sum(words(block.text) for block in page.blocks)
    result.violations.sort(key=lambda v: (v.file, v.line, v.rule))
    return result


def load_config(path: Path) -> Config:
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as problem:
        raise DocsLintError(f'cannot read config {path}: {problem}') from None
    unknown = set(data.get('limits', {})) - set(LIMITS)
    if unknown:
        raise DocsLintError(f'unknown limits in {path.name}: {", ".join(sorted(unknown))}')
    severity = data.get('severity', {})
    if set(severity) - set(RULES) or any(level not in ('error', 'warning') for level in severity.values()):
        raise DocsLintError(f'{path.name} severity: rules are {", ".join(RULES)}, levels "error" or "warning"')
    by_type = data.get('page_words_by_type', {})
    if set(by_type) - set(TYPES):
        raise DocsLintError(f'{path.name} page_words_by_type: types are {", ".join(TYPES)}')
    return Config(list(data.get('include', ['**/*.md', '*.md'])), list(data.get('exclude', [])),
                  list(data.get('typed', ['docs/**/*.md', 'docs/*.md'])), list(data.get('untyped', [])),
                  data.get('index', 'docs/README.md'), dict(LIMITS, **data.get('limits', {})), by_type,
                  dict(SEVERITY, **severity))


# ----------------------------------------------------------------------------- baseline

def read_baseline(path: Path, replacing: bool = False) -> dict[Key, list[int]]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        if data.get('format') != BASELINE_FORMAT:
            if replacing:
                return {}
            raise DocsLintError(f'{path.name} is baseline format {data.get("format")}, this lint reads format '
                                f'{BASELINE_FORMAT}: regenerate it with `update --allow-increase`')
        entries: dict[Key, list[int]] = {}
        for e in data['entries']:
            entries.setdefault((e['file'], e['rule'], e['section']), []).append(int(e['value']))
        return entries
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as problem:
        raise DocsLintError(f'cannot read baseline {path}: {problem}') from None


def write_baseline(path: Path, entries: dict[Key, list[int]]) -> None:
    rows = [json.dumps({'file': k[0], 'rule': k[1], 'section': k[2], 'value': n}, ensure_ascii=False)
            for k, values in sorted(entries.items()) for n in sorted(values, reverse=True)]
    body = ('{\n  "format": ' + str(BASELINE_FORMAT) + ',\n'
            '  "note": "Known docs violations per (file, rule, section) with the measured values. '
            '`py -3 scripts/docs-lint/docs_lint.py check` reports what is new or worse (scripts/docs-lint/README.md); '
            '`update` rewrites this file.",\n'
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
    old: int
    new: int
    violation: Violation | None = None


def compare(baseline: dict[Key, list[int]], current: dict[Key, list[Violation]]) -> tuple[list[Change], int]:
    """(changes, known count). Violations of one rule in one section share a key: their values are paired largest
    first, and the ones without a counterpart are new or gone."""
    changes, known = [], 0
    for key in sorted(set(baseline) | set(current)):
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

def where(key: Key) -> str:
    return f'{key[0]}' + (f'  [{key[2]}]' if key[2] else '')


def report(level: str, change: Change) -> None:
    file, rule, _ = change.key
    violation = change.violation
    why = f'known, grew: baseline {change.old}, now {change.new}' if change.kind == 'grew' else 'new'
    line = f':{violation.line}' if violation else ''
    print(f'{level.upper()} {rule} {RULES[rule]}  {where(change.key).replace(file, file + line, 1)}  ({why})')
    if violation:
        print(f'    {violation.text[:110]}\n    -> {violation.detail}')
        if level in ('error', 'warning') and os.environ.get('GITHUB_ACTIONS') == 'true':
            print(f'::{level} file={violation.file},line={violation.line},title={rule} {RULES[rule]}::'
                  f'{violation.detail} ({why}). scripts/docs-lint/README.md says how to fix it')


def summary_table(result: Scan) -> str:
    table: dict[str, dict[str, int]] = {}
    for violation in result.violations:
        table.setdefault(violation.file, {rule: 0 for rule in RULES})[violation.rule] += 1
    width = max([len('file')] + [len(name) for name in table]) + 2
    lines = ['file'.ljust(width) + ''.join(r.rjust(5) for r in RULES) + 'total'.rjust(7)]
    totals = {rule: 0 for rule in RULES}
    for name, row in sorted(table.items()):
        lines.append(name.ljust(width) + ''.join(str(row[r]).rjust(5) for r in RULES) + str(sum(row.values())).rjust(7))
        for rule in RULES:
            totals[rule] += row[rule]
    lines.append('total'.ljust(width) + ''.join(str(totals[r]).rjust(5) for r in RULES)
                 + str(sum(totals.values())).rjust(7))
    return '\n'.join(lines)


def command_scan(result: Scan, config: Config, args) -> int:
    if args.json:
        print(json.dumps({'rules': RULES, 'severity': config.severity, 'limits': config.limits, 'files': result.files,
                          'words': result.words, 'violations': [v.as_json() for v in result.violations],
                          'suppressed': [v.as_json() for v in result.suppressed]}, indent=2, ensure_ascii=False))
        return 0
    if args.list:
        for v in result.violations:
            print(f'{v.rule} {RULES[v.rule]}  {v.file}:{v.line}' + (f'  [{v.section}]' if v.section else '')
                  + f'\n    -> {v.detail}')
        print()
    print(summary_table(result))
    print(f'\n{len(result.violations)} violation(s) in {result.files} file(s), {result.words} words of prose; '
          f'{len(result.suppressed)} suppressed by docs-lint-ok')
    return 0


def command_check(result: Scan, config: Config, args) -> int:
    changes, known = compare(read_baseline(args.baseline), by_key(result.violations))
    errors = [c for c in changes if c.kind == 'new' and config.severity[c.key[1]] == 'error']
    warnings = [c for c in changes if c.kind == 'grew' or (c.kind == 'new' and config.severity[c.key[1]] == 'warning')]
    lower = [c for c in changes if c.kind == 'lower']
    strict = args.warnings_as_errors
    for change in errors:
        report('error', change)
    for change in warnings:
        report('error' if strict else 'warning', change)
    for change in lower:
        now = f'now {change.new}' if change.new else 'gone'
        print(f'lower - run update: {change.key[1]} {RULES[change.key[1]]}  {where(change.key)}  '
              f'(baseline {change.old}, {now})')
    if errors or warnings:
        print(f'\n{HELP}')
    print(f'docs lint check: {len(errors)} error(s), {len(warnings)} warning(s), {known} known, {len(lower)} lower or '
          f'gone in {result.files} file(s); baseline {args.baseline.name}'
          + ('; warnings are errors' if strict else ''))
    return 1 if errors or (strict and warnings) else 0


def command_update(result: Scan, config: Config, args) -> int:
    baseline = read_baseline(args.baseline, replacing=args.allow_increase)
    changes, _ = compare(baseline, by_key(result.violations))
    increases = [c for c in changes if c.kind in ('new', 'grew')]
    if increases and not args.allow_increase:
        for change in increases:
            report('increase', change)
        print(f'docs lint update refused: {len(increases)} value(s) would be new or higher; fix them, mark them '
              '`<!-- docs-lint-ok: D<n> <reason> -->`, or rerun with --allow-increase (scripts/docs-lint/README.md)')
        return 1
    entries = {key: [v.value for v in found] for key, found in by_key(result.violations).items()}
    write_baseline(args.baseline, entries)
    lower = sum(1 for c in changes if c.kind == 'lower')
    print(f'baseline updated: {sum(len(v) for v in entries.values())} violation(s); {lower} lower or gone, '
          f'{len(increases)} new or higher than before')
    return 0


def command_stats(root: Path, config: Config, files: list[str]) -> int:
    print(f'{"file":40} {"words":>7} {"blocks":>7} {"avg":>5} {"max":>5} {">limit":>7} {"sent.max":>9} {"sent.avg":>9}')
    for name in files:
        path = Path(name)
        rel = path.resolve().relative_to(root).as_posix() if path.resolve().is_relative_to(root) else name
        page = parse_page(rel, path.read_text(encoding='utf-8-sig', errors='replace'))
        sizes = [words(b.text) for b in page.blocks] or [0]
        lengths = [words(s) for b in page.blocks for s in sentences(b.text)] or [0]
        over = sum(1 for n in sizes if n > config.limits['paragraph_words'])
        print(f'{rel[-40:]:40} {sum(sizes):7} {len(page.blocks):7} {sum(sizes) // len(sizes):5} {max(sizes):5} '
              f'{over:7} {max(lengths):9} {sum(lengths) / len(lengths):9.1f}')
    return 0


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')
    parser = argparse.ArgumentParser(prog='docs_lint.py', description=__doc__.split('\n\n')[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', nargs='?', default='scan', choices=('scan', 'check', 'update', 'stats'))
    parser.add_argument('files', nargs='*', help='stats: the pages to measure')
    parser.add_argument('--json', action='store_true', help='scan: machine-readable output')
    parser.add_argument('--list', action='store_true', help='scan: print every violation')
    parser.add_argument('--allow-increase', action='store_true', help='update: accept new or higher values')
    parser.add_argument('--warnings-as-errors', action='store_true', help='check: fail on warnings too')
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--baseline', type=Path, default=DEFAULT_BASELINE)
    parser.add_argument('--root', type=Path, default=HERE.parent.parent,
                        help='the folder paths start from (default: the repository)')
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        config = load_config(args.config)
        if args.command == 'stats':
            return command_stats(root, config, args.files)
        result = scan(root, config)
        return {'scan': command_scan, 'check': command_check, 'update': command_update}[args.command](result, config,
                                                                                                    args)
    except DocsLintError as problem:
        print(f'docs_lint.py: {problem}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
