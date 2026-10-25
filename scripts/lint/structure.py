"""Code structure for lint.py: tokens, functions and metrics of C#, C and C++ sources (Python standard library only).

The rules are Bingus Shared Runtime's R10-R15 (its lint.py and metrics.py) for HellForge's own languages:

- Cognitive complexity, after SonarSource's definition (2017): `if`, `switch` (and C#'s switch expression), `for`,
  `foreach`, `while`, `do`, `catch` and `?:` add 1 plus 1 for each enclosing one; `else`, `else if` and `goto` add 1;
  each run of like `&&`/`||` operators adds 1 (a change of operator starts a new run; brackets nest runs); a function
  that calls itself adds 1, once. As in Bingus, a block lambda (C# `=> {`, C++ `[...]{`), an anonymous method and a C#
  local function are measured on their own, from nesting 0, and not charged to the function around them. An
  expression lambda (`x => a && b`) is part of the function it is written in.
- Code lines: lines holding one of the function's own tokens (a nested function's body counts for that function; the
  line it starts on counts for both).
- Parameters: `void` alone and `...` do not count.
- Stack size (C and C++): the bytes of a function's fixed-size local arrays of scalar or pointer type (`static` ones
  are not on the stack and do not count).
- Duplicated code: windows of `clone_tokens` tokens that occur more than once, merged into regions (as Bingus's).

Preprocessor lines are not tokens. Of an `#if`/`#ifdef`/`#ifndef` group only the first branch is read (the `#else`
branch of `#if 0`); the others are skipped, so code split across branches still balances.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

LIMITS = {
    'cognitive': 15,        # SonarSource's default for a function
    'function_lines': 150,  # a function's own code lines
    'parameters': 7,        # SonarSource's default
    'stack_bytes': 16384,   # C and C++: fixed local arrays per function
    'module_lines': 1000,   # SonarSource's default for a file
    'clone_tokens': 100,    # duplicated block length (PMD CPD's default)
}

LANGUAGES = {'.cs': 'cs', '.c': 'c', '.h': 'cpp', '.cpp': 'cpp', '.cc': 'cpp', '.hpp': 'cpp'}


class ParseError(Exception):
    pass


@dataclass
class Token:
    kind: str    # name, number, string, char, op
    value: str
    line: int


@dataclass
class Function:
    name: str
    line: int                  # the line of its name (a lambda: of its parameters or its `[`)
    parameters: int = 0
    cognitive: int = 0
    nesting: int = 0           # deepest control nesting inside it
    stack_bytes: int = 0       # C and C++: fixed local arrays
    lines: set = field(default_factory=set)
    calls_itself: bool = False
    short: str = ''            # the name a recursive call uses


# ----------------------------------------------------------------------------- tokens

_OPERATORS = sorted((
    '>>>=', '<<=', '>>=', '...', '->*', '??=', '::', '->', '++', '--', '<<', '<=', '>=', '==', '!=', '&&', '||',
    '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '=>', '??', '?.', '.*', '##',
    '+', '-', '*', '/', '%', '<', '>', '=', '!', '~', '&', '|', '^', '?', ':', ';', ',', '.', '(', ')', '[', ']',
    '{', '}', '#', '@', '\\', '$', '`'), key=len, reverse=True)
_NAME = re.compile(r'[A-Za-z_\u0080-￿][A-Za-z0-9_\u0080-￿]*')
_NUMBER = re.compile(r"(?:0[xX][0-9A-Fa-f']+|0[bB][01']+|(?:\d[\d']*)?\.?\d[\d']*(?:[eE][+-]?\d+)?)[A-Za-z0-9_]*")
_RAW_CPP = re.compile(r'(?:u8|[uUL])?R"([^ ()\\\t\n]{0,16})\(')


class _Scanner:
    def __init__(self, text: str, language: str):
        self.text, self.language = text, language
        self.position, self.line = 0, 1
        self.tokens: list[Token] = []
        self.directive_lines: set[int] = set()
        self.skipped_lines: set[int] = set()
        self.conditions: list[str] = []     # per open #if group: 'active', 'skipping', 'done'
        self.line_start = True

    def error(self, message: str) -> ParseError:
        return ParseError(f'line {self.line}: {message}')

    @property
    def active(self) -> bool:
        return all(state == 'active' for state in self.conditions)

    def advance(self, count: int) -> str:
        chunk = self.text[self.position:self.position + count]
        self.line += chunk.count('\n')
        self.position += count
        return chunk

    def run(self) -> None:
        text = self.text
        while self.position < len(text):
            char = text[self.position]
            if char == '\n':
                self.advance(1)
                self.line_start = True
                continue
            if char in ' \t\r\f\v':
                self.advance(1)
                continue
            if text.startswith('//', self.position):
                end = text.find('\n', self.position)
                self.advance((len(text) if end < 0 else end) - self.position)
                continue
            if text.startswith('/*', self.position):
                end = text.find('*/', self.position + 2)
                if end < 0:
                    raise self.error('unterminated comment')
                self.advance(end + 2 - self.position)
                continue
            if char == '#' and self.line_start:
                self.directive()
                continue
            self.line_start = False
            if not self.active:
                self.skipped_lines.add(self.line)
                self.skip_inactive()
                continue
            self.token()

    def skip_inactive(self) -> None:
        """Skips the rest of an inactive line: strings and comments are still read whole."""
        end = self.text.find('\n', self.position)
        self.advance((len(self.text) if end < 0 else end) - self.position)

    def directive(self) -> None:
        start_line, start = self.line, self.position
        text, k = self.text, self.position
        while k < len(text):                          # the directive, its continuation lines and its comments
            if text.startswith('/*', k):
                close = text.find('*/', k + 2)
                k = len(text) if close < 0 else close + 2
                continue
            if text.startswith('//', k):
                close = text.find('\n', k)
                k = len(text) if close < 0 else close
                continue
            if text[k] == '\\' and text.startswith('\n', k + 1) and self.language != 'cs':
                k += 2
                continue
            if text[k] == '\\' and text.startswith('\r\n', k + 1) and self.language != 'cs':
                k += 3
                continue
            if text[k] == '\n':
                break
            k += 1
        self.advance(k - self.position)
        body = re.sub(r'/\*.*?\*/|//[^\n]*', ' ', text[start + 1:self.position], flags=re.S).strip()
        word = re.match(r'\s*(\w*)', body).group(1)
        rest = body[len(word):].strip() if body.startswith(word) else ''
        was_active = self.active
        if word in ('if', 'ifdef', 'ifndef'):
            zero = word == 'if' and re.match(r'\(?\s*0\s*\)?\s*(?://.*|/\*.*)?$', rest) is not None
            self.conditions.append('skipping' if zero else 'active')
        elif word in ('elif', 'else'):
            if not self.conditions:
                raise ParseError(f'line {start_line}: #{word} without #if')
            # the first branch is read; #if 0's #else is read instead
            self.conditions[-1] = 'active' if self.conditions[-1] == 'skipping' and word == 'else' else 'done'
        elif word == 'endif':
            if not self.conditions:
                raise ParseError(f'line {start_line}: #endif without #if')
            self.conditions.pop()
        if was_active or self.active:
            self.directive_lines.update(range(start_line, self.line + 1))

    def token(self) -> None:
        text, position, line = self.text, self.position, self.line
        char = text[position]
        if self.language == 'cs' and char in '$@"':
            match = re.match(r'(\$+@?|@\$*)?"', text[position:])
            if match:
                self.cs_string(match.group(1) or '')
                return
        if self.language != 'cs':
            raw = _RAW_CPP.match(text, position)
            if raw:
                close = text.find(')' + raw.group(1) + '"', raw.end())
                if close < 0:
                    raise self.error('unterminated raw string')
                self.tokens.append(Token('string', 'R""', line))
                self.advance(close + len(raw.group(1)) + 2 - position)
                return
        prefix = re.match(r'(u8|[uUL])(?=["\'])', text[position:]) if self.language != 'cs' else None
        if char in '"\'' or prefix:
            start = position + (len(prefix.group(1)) if prefix else 0)
            quote = text[start]
            end = self.quoted_end(start, quote)
            self.tokens.append(Token('string' if quote == '"' else 'char', text[position:end], line))
            self.advance(end - position)
            return
        match = _NAME.match(text, position)
        if match:
            self.tokens.append(Token('name', match.group(), line))
            self.advance(match.end() - position)
            return
        if char.isdigit() or (char == '.' and position + 1 < len(text) and text[position + 1].isdigit()):
            match = _NUMBER.match(text, position)
            value = match.group() if match and match.group() else char
            if self.language == 'cs':                          # C# has no digit separator but `_`
                value = re.match(r"[^']*", value).group()
            self.tokens.append(Token('number', value, line))
            self.advance(len(value))
            return
        for operator in _OPERATORS:
            if text.startswith(operator, position):
                self.tokens.append(Token('op', operator, line))
                self.advance(len(operator))
                return
        raise self.error(f'unexpected character {char!r}')

    def quoted_end(self, start: int, quote: str) -> int:
        text, k = self.text, start + 1
        while k < len(text):
            char = text[k]
            if char == '\\':
                k += 2
                continue
            if char == quote:
                return k + 1
            if char == '\n':
                break
            k += 1
        if quote == "'" and self.language != 'cs':
            return start + 1                                    # a C++14 digit separator outside a number
        raise ParseError(f'line {self.text.count(chr(10), 0, start) + 1}: unterminated {"string" if quote == chr(34) else "character"}')

    def cs_string(self, prefix: str) -> None:
        """A C# string: regular, verbatim (@), interpolated ($, with {holes}) or raw (three or more quotes)."""
        text, start, line = self.text, self.position, self.line
        k = start + len(prefix)
        quotes = len(re.match(r'"+', text[k:]).group())
        dollars = prefix.count('$')
        if quotes >= 3:                                         # raw: ends at the same number of quotes
            close = text.find('"' * quotes, k + quotes)
            if close < 0:
                raise self.error('unterminated raw string')
            end = close + quotes
        else:
            end = self.cs_body(k + 1, verbatim='@' in prefix, interpolated=dollars > 0)
        self.tokens.append(Token('string', text[start:min(end, start + 40)], line))
        self.advance(end - start)

    def cs_body(self, k: int, verbatim: bool, interpolated: bool) -> int:
        text = self.text
        while k < len(text):
            char = text[k]
            if char == '\\' and not verbatim:
                k += 2
                continue
            if char == '"':
                if verbatim and text.startswith('""', k):
                    k += 2
                    continue
                return k + 1
            if interpolated and char == '{':
                if text.startswith('{{', k):
                    k += 2
                    continue
                k = self.cs_hole(k + 1)
                continue
            if char == '\n' and not verbatim:
                raise ParseError(f'line {text.count(chr(10), 0, k) + 1}: unterminated string')
            k += 1
        raise ParseError('unterminated string')

    def cs_hole(self, k: int) -> int:
        """Past the `}` that closes an interpolation hole (nested strings, braces and comments included)."""
        text, depth = self.text, 0
        while k < len(text):
            char = text[k]
            match = re.match(r'(\$+@?|@\$*)?"', text[k:]) if char in '$@"' else None
            if match:
                prefix = match.group(1) or ''
                inner = k + len(prefix)
                quotes = len(re.match(r'"+', text[inner:]).group())
                if quotes >= 3:
                    close = text.find('"' * quotes, inner + quotes)
                    k = close + quotes
                else:
                    k = self.cs_body(inner + 1, '@' in prefix, '$' in prefix)
                continue
            if char == "'":
                k = self.quoted_end(k, "'")
                continue
            if char == '{':
                depth += 1
            elif char == '}':
                if depth == 0:
                    return k + 1
                depth -= 1
            k += 1
        raise ParseError('unterminated interpolation')


def tokenize(text: str, language: str) -> tuple[list[Token], set[int], set[int]]:
    """(tokens, preprocessor directive lines, lines skipped as inactive branches)."""
    scanner = _Scanner(text, language)
    scanner.run()
    if scanner.conditions:
        raise ParseError('#if without #endif')
    return scanner.tokens, scanner.directive_lines, scanner.skipped_lines


# ----------------------------------------------------------------------------- functions

CONTROL_WORDS = frozenset(('if', 'for', 'foreach', 'while', 'switch', 'catch', 'using', 'lock', 'fixed', 'when',
                           'return', 'sizeof', 'typeof', 'nameof', 'alignof', '_Alignof', 'alignas', 'decltype',
                           'defined', 'throw', 'new', 'await', 'yield', 'do', 'else', 'case', 'goto', 'in', 'is',
                           'as', 'out', 'ref', '__declspec', '__attribute__', '__pragma', '_Pragma', 'static_assert',
                           '_Static_assert', 'noexcept', 'requires', 'base', 'this', 'checked', 'unchecked',
                           'default', 'stackalloc', 'and', 'or', 'not', 'with'))
TYPE_WORDS = frozenset(('class', 'struct', 'record', 'interface', 'enum', 'union', 'namespace'))
CPP_QUALIFIERS = frozenset(('const', 'volatile', 'noexcept', 'override', 'final', 'mutable', 'constexpr', '&', '&&',
                            'throw', 'try', '__attribute__', 'consteval', 'requires'))
ACCESSORS = frozenset(('get', 'set', 'init', 'add', 'remove'))
STATEMENT_ENDS = frozenset((';',))
RUN_BREAKERS = frozenset((',', ';', '=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=', '>>>=',
                          '??=', '?', ':', '=>'))
SCALAR_BYTES = {
    'char': 1, 'signed': 4, 'unsigned': 4, 'short': 2, 'int': 4, 'long': 4, 'float': 4, 'double': 8, 'bool': 1,
    '_Bool': 1, 'wchar_t': 2, 'char8_t': 1, 'char16_t': 2, 'char32_t': 4, 'int8_t': 1, 'uint8_t': 1, 'int16_t': 2,
    'uint16_t': 2, 'int32_t': 4, 'uint32_t': 4, 'int64_t': 8, 'uint64_t': 8, 'size_t': 8, 'ssize_t': 8,
    'ptrdiff_t': 8, 'intptr_t': 8, 'uintptr_t': 8, 'BYTE': 1, 'CHAR': 1, 'UCHAR': 1, 'BOOL': 4, 'BOOLEAN': 1,
    'WCHAR': 2, 'WORD': 2, 'SHORT': 2, 'USHORT': 2, 'DWORD': 4, 'INT': 4, 'UINT': 4, 'LONG': 4, 'ULONG': 4,
    'FLOAT': 4, 'DWORD64': 8, 'ULONG64': 8, 'ULONGLONG': 8, 'LONGLONG': 8, 'QWORD': 8, 'SIZE_T': 8,
    'ULONG_PTR': 8, 'DWORD_PTR': 8, 'UINT_PTR': 8, 'INT_PTR': 8, 'LONG_PTR': 8, 'HANDLE': 8, 'HMODULE': 8,
    'HWND': 8, 'LPVOID': 8, 'PVOID': 8, 'LPSTR': 8, 'LPCSTR': 8, 'LPWSTR': 8, 'LPCWSTR': 8, 'TCHAR': 2,
}
KNOWN_CONSTANTS = {'MAX_PATH': 260}


class _Source:
    def __init__(self, tokens: list[Token], language: str):
        self.tokens, self.language = tokens, language
        self.match = self.brackets()
        self.functions: list[Function] = []
        self.owner: list[Function | None] = [None] * len(tokens)

    def brackets(self) -> list[int]:
        match, stack = [-1] * len(self.tokens), []
        pairs = {')': '(', ']': '[', '}': '{'}
        for index, token in enumerate(self.tokens):
            if token.kind != 'op':
                continue
            if token.value in '([{':
                stack.append(index)
            elif token.value in pairs:
                if not stack or self.tokens[stack[-1]].value != pairs[token.value]:
                    raise ParseError(f'line {token.line}: unbalanced {token.value!r}')
                opening = stack.pop()
                match[opening], match[index] = index, opening
        if stack:
            token = self.tokens[stack[-1]]
            raise ParseError(f'line {token.line}: {token.value!r} never closed')
        return match

    def at(self, index: int) -> Token:
        return self.tokens[index] if 0 <= index < len(self.tokens) else Token('eof', '', 0)

    def op(self, index: int, *values: str) -> bool:
        token = self.at(index)
        return token.kind == 'op' and (not values or token.value in values)

    def word(self, index: int, *values: str) -> bool:
        token = self.at(index)
        return token.kind == 'name' and (not values or token.value in values)

    # ---------------------------------------------------------------- declarations

    def scope(self, start: int, end: int, type_name: str = '') -> None:
        """Namespace, type and file scope: finds function definitions, type bodies and property blocks."""
        i = start
        while i < end:
            token = self.at(i)
            if self.op(i, '{'):
                i = self.brace_at_scope(i, type_name)
                continue
            if self.op(i, '[') and self.language == 'cs':          # an attribute
                i = self.match[i] + 1
                continue
            if self.op(i, '(') and self.match[i] < end:
                taken = self.declaration(i, type_name)
                i = taken if taken else self.match[i] + 1
                continue
            if self.op(i, '=>') and self.language == 'cs':        # an expression-bodied property or indexer
                name = self.at(i - 1).value if self.word(i - 1) else ('this' if self.op(i - 1, ']') else '<member>')
                i = self.expression_member(i, f'{type_name}.{name}' if type_name else name, 0)
                continue
            if self.op(i, '=') and self.language == 'cs':         # a field or property initializer
                i = self.initializer(i + 1, end)
                continue
            if self.op(i, '(', '['):
                i = self.match[i] + 1
                continue
            i += 1

    def initializer(self, i: int, end: int) -> int:
        """A C# field initializer up to its `;`: lambdas in it are functions."""
        stop = self.statement_end(i, end)
        self.expression(i, stop, None, 0)
        return stop + 1

    def brace_at_scope(self, i: int, type_name: str) -> int:
        close = self.match[i]
        if self.op(i - 1, '=', ','):                               # an initializer: lambdas in it are functions
            self.expression(i, close + 1, None, 0)
            return close + 1
        head = self.header_words(i)
        kind = next((w for w in head if w in TYPE_WORDS), None)
        if kind and kind != 'enum':
            name = self.type_name(i, head)
            self.scope(i + 1, close, (f'{type_name}.{name}' if type_name and kind != 'namespace' else name)
                       if kind != 'namespace' else type_name)
        elif self.language != 'cs' and self.at(i - 1).kind == 'string' and self.word(i - 2, 'extern'):
            self.scope(i + 1, close, type_name)                    # extern "C" { ... }
        elif self.language == 'cs' and (self.word(i - 1) or self.op(i - 1, ']')) and not self.op(i - 2, '.') \
                and not self.op(i - 1, '=') and kind is None:
            self.accessors(i, close, type_name, self.at(i - 1).value if self.word(i - 1) else 'this')
        return close + 1

    def header_words(self, i: int) -> list[str]:
        """The names of the declaration before the `{` at i, back to the previous `;`, `}` or `{`."""
        words, k = [], i - 1
        while k >= 0 and not self.op(k, ';', '}', '{'):
            if self.op(k, ')', ']') and self.match[k] >= 0:
                k = self.match[k] - 1
                continue
            if self.word(k):
                words.append(self.at(k).value)
            k -= 1
        return list(reversed(words))

    def type_name(self, i: int, head: list[str]) -> str:
        for index, word in enumerate(head):
            if word in TYPE_WORDS and index + 1 < len(head):
                following = [w for w in head[index + 1:] if w not in TYPE_WORDS and w not in ('sealed', 'final', 'abstract', 'partial', 'readonly', 'ref', 'static', 'unsafe', 'public', 'private', 'internal', 'protected', 'file')]
                return following[0] if following else word
        return head[-1] if head else ''

    def accessors(self, open_: int, close: int, type_name: str, member: str) -> None:
        """A C# property, indexer or event block: get/set/init/add/remove bodies are functions."""
        i = open_ + 1
        while i < close:
            if self.word(i, *ACCESSORS):
                name = f'{type_name}.{member}.{self.at(i).value}' if type_name else f'{member}.{self.at(i).value}'
                if self.op(i + 1, '{'):
                    function = self.new_function(name, self.at(i).line, 0, i)
                    self.body(i + 1, self.match[i + 1], function)
                    i = self.match[i + 1] + 1
                    continue
                if self.op(i + 1, '=>'):
                    i = self.expression_member(i + 1, name, 0)
                    continue
            if self.op(i, '[', '(', '{'):
                i = self.match[i] + 1
                continue
            i += 1

    def name_before(self, paren: int) -> tuple[str, int] | None:
        """(name, index of its first token) of the function whose parameter list opens at paren, or None."""
        k = paren - 1
        if self.op(k, '>') and self.language != 'c':               # generic parameters: Name<T>(
            depth, j = 0, k
            while j >= 0:
                if self.op(j, '>', '>>'):
                    depth += 2 if self.at(j).value == '>>' else 1
                elif self.op(j, '<'):
                    depth -= 1
                    if depth == 0:
                        break
                elif self.op(j, ';', '{', '}', '('):
                    return None
                j -= 1
            k = j - 1
        if self.word(k) and self.at(k).value not in CONTROL_WORDS:
            start, parts = k, [self.at(k).value]
            if self.op(k - 1, '~'):
                start, parts[0] = k - 1, '~' + parts[0]
            while self.op(start - 1, '::') and self.word(start - 2):
                start -= 2
                parts.insert(0, self.at(start).value)
            return '::'.join(parts), start
        if self.language != 'c':                                   # operator+(, operator()(, operator new(
            j = k
            while j >= 0 and not self.word(j, 'operator') and j > k - 3:
                j -= 1
            if self.word(j, 'operator'):
                return 'operator' + ''.join(self.at(x).value for x in range(j + 1, paren)), j
        return None

    def trailer(self, j: int, end: int) -> int:
        """Past what may stand between a parameter list and its body: C++ qualifiers, `-> type`, constructor
        initializers; C# constructor initializers and `where` clauses; GCC attributes."""
        while j < end:
            if self.language == 'cs':
                if self.op(j, ':') or self.word(j, 'where'):
                    while j < end and not self.op(j, '{', '=>', ';'):
                        j = self.match[j] + 1 if self.op(j, '(', '[') else j + 1
                    return j
                return j
            if self.word(j, *CPP_QUALIFIERS) or self.op(j, '&', '&&'):
                j = self.match[j + 1] + 1 if self.op(j + 1, '(') else j + 1
                continue
            if self.op(j, '[') and self.op(j + 1, '['):            # [[attribute]]
                j = self.match[j] + 1
                continue
            if self.op(j, '->'):
                while j < end and not self.op(j, '{', ';', '='):
                    j = self.match[j] + 1 if self.op(j, '(', '[') else j + 1
                return j
            if self.op(j, ':') and self.language == 'cpp':          # member initializers
                j += 1
                while j < end:
                    while self.word(j) or self.op(j, '::', '<', '>', ','):
                        if self.op(j, ','):
                            break
                        j += 1
                    if self.op(j, '(', '{'):
                        j = self.match[j] + 1
                    if self.op(j, ','):
                        j += 1
                        continue
                    return j
            return j
        return j

    def declaration(self, paren: int, type_name: str) -> int | None:
        """A function definition whose parameter list opens at paren: its end, or None when it is not one."""
        found = self.name_before(paren)
        close = self.match[paren]
        if not found:
            return None
        name, first = found
        if self.language == 'cs' and any(word in TYPE_WORDS for word in self.header_words(paren)[:-1]):
            return None                                            # a primary constructor: record R(int A) {
        j = self.trailer(close + 1, len(self.tokens))
        qualified = f'{type_name}.{name}' if type_name and self.language == 'cs' else (
            f'{type_name}::{name}' if type_name and '::' not in name else name)
        if self.op(j, '{'):
            function = self.new_function(qualified, self.at(first).line, self.parameters(paren), first,
                                         short=name.split('::')[-1])
            self.claim(first, j, function)
            self.body(j, self.match[j], function)
            return self.match[j] + 1
        if self.op(j, '=>') and self.language == 'cs':
            function = self.new_function(qualified, self.at(first).line, self.parameters(paren), first,
                                         short=name)
            self.claim(first, j, function)
            return self.expression_member(j, qualified, 0, function)
        return None

    def parameters(self, paren: int) -> int:
        close, count, depth, empty = self.match[paren], 0, 0, True
        segments: list[list[str]] = [[]]
        for k in range(paren + 1, close):
            token = self.at(k)
            if token.kind == 'op' and token.value in '([{':
                depth += 1
            elif token.kind == 'op' and token.value in ')]}':
                depth -= 1
            elif token.kind == 'op' and token.value == '<' and self.language != 'c':
                depth += 1
            elif token.kind == 'op' and token.value in ('>', '>>') and self.language != 'c' and depth > 0:
                depth -= 1 if token.value == '>' else 2
            elif token.kind == 'op' and token.value == ',' and depth == 0:
                segments.append([])
                continue
            segments[-1].append(token.value)
        for segment in segments:
            if not segment or segment == ['void'] or segment == ['...']:
                continue
            count += 1
            empty = False
        return 0 if empty else count

    def new_function(self, name: str, line: int, parameters: int, first: int, short: str = '',
                     outer: Function | None = None) -> Function:
        """A function found at token first; the line it starts on counts for the function around it too."""
        function = Function(name, line, parameters, short=short or name.split('.')[-1].split('::')[-1])
        self.functions.append(function)
        if outer is not None:
            outer.lines.add(self.at(first).line)
        return function

    def claim(self, start: int, stop: int, function: Function) -> None:
        for k in range(start, stop):
            if self.owner[k] is None:
                self.owner[k] = function
                function.lines.add(self.at(k).line)

    def expression_member(self, arrow: int, name: str, nesting: int, function: Function | None = None) -> int:
        """A C# `=> expression;` body (member, accessor or local function): its tokens up to the `;`."""
        stop = self.statement_end(arrow + 1, len(self.tokens))
        function = function or self.new_function(name, self.at(arrow).line, 0, arrow)
        self.claim(arrow, arrow + 1, function)
        self.expression(arrow + 1, stop, function, nesting)
        self.claim(stop, stop + 1, function)
        return stop + 1

    def statement_end(self, i: int, end: int) -> int:
        while i < end and not self.op(i, ';'):
            i = self.match[i] + 1 if self.op(i, '(', '[', '{') else i + 1
        return min(i, end)

    # ---------------------------------------------------------------- bodies

    def body(self, open_: int, close: int, function: Function) -> None:
        self.claim(open_, open_ + 1, function)
        self.statements(open_ + 1, close, function, 0)
        self.claim(close, close + 1, function)

    def add(self, function: Function | None, value: int, nesting: int = 0) -> None:
        if function is not None:
            function.cognitive += value
            function.nesting = max(function.nesting, nesting)

    def statements(self, i: int, end: int, function: Function, nesting: int) -> None:
        while i < end:
            i = self.statement(i, end, function, nesting)

    def own(self, start: int, stop: int, function: Function | None) -> None:
        if function is not None:
            self.claim(start, stop, function)

    def condition(self, i: int, function: Function, nesting: int) -> int:
        """A parenthesised header at i (or none): its expression; returns the index after it."""
        if not self.op(i, '('):
            return i
        self.own(i, i + 1, function)
        self.expression(i + 1, self.match[i], function, nesting)
        self.own(self.match[i], self.match[i] + 1, function)
        return self.match[i] + 1

    def statement(self, i: int, end: int, function: Function, nesting: int) -> int:
        token = self.at(i)
        if self.op(i, '{'):
            self.own(i, i + 1, function)
            self.statements(i + 1, self.match[i], function, nesting)
            self.own(self.match[i], self.match[i] + 1, function)
            return self.match[i] + 1
        if self.op(i, ';'):
            self.own(i, i + 1, function)
            return i + 1
        value = token.value if token.kind == 'name' else ''
        if value == 'await' and self.word(i + 1, 'foreach', 'using'):
            self.own(i, i + 1, function)
            i, value = i + 1, self.at(i + 1).value
        if value == 'if':
            return self.if_statement(i, end, function, nesting)
        if value in ('for', 'foreach', 'while'):
            self.own(i, i + 1, function)
            self.add(function, 1 + nesting, nesting + 1)
            j = self.condition(i + 1, function, nesting)
            return self.statement(j, end, function, nesting + 1)
        if value == 'do' and not self.op(i + 1, '('):
            self.own(i, i + 1, function)
            self.add(function, 1 + nesting, nesting + 1)
            j = self.statement(i + 1, end, function, nesting + 1)
            if self.word(j, 'while'):
                self.own(j, j + 1, function)
                j = self.condition(j + 1, function, nesting)
            if self.op(j, ';'):
                self.own(j, j + 1, function)
                j += 1
            return j
        if value == 'switch' and self.op(i + 1, '('):
            self.own(i, i + 1, function)
            self.add(function, 1 + nesting, nesting + 1)
            j = self.condition(i + 1, function, nesting)
            if self.op(j, '{'):
                self.own(j, j + 1, function)
                self.statements(j + 1, self.match[j], function, nesting + 1)
                self.own(self.match[j], self.match[j] + 1, function)
                return self.match[j] + 1
            return self.statement(j, end, function, nesting + 1)
        if value in ('try', '__try'):
            self.own(i, i + 1, function)
            j = self.statement(i + 1, end, function, nesting)
            while self.word(j, 'catch', '__except'):
                self.own(j, j + 1, function)
                self.add(function, 1 + nesting, nesting + 1)
                j = self.condition(j + 1, function, nesting)
                if self.word(j, 'when'):
                    self.own(j, j + 1, function)
                    j = self.condition(j + 1, function, nesting)
                j = self.statement(j, end, function, nesting + 1)
            if self.word(j, 'finally', '__finally'):
                self.own(j, j + 1, function)
                j = self.statement(j + 1, end, function, nesting)
            return j
        if value == 'goto':
            self.add(function, 1)
            stop = self.statement_end(i, end)
            self.own(i, stop + 1, function)
            return stop + 1
        if value == 'case' or (value == 'default' and self.op(i + 1, ':')):
            j = i + 1
            while j < end and not (self.op(j, ':') and not self.op(j, '::')):
                j = self.match[j] + 1 if self.op(j, '(', '[', '{') else j + 1
            self.own(i, j + 1, function)
            self.expression(i + 1, j, function, nesting)
            return j + 1
        if token.kind == 'name' and self.op(i + 1, ':') and not self.op(i + 2, ':') and value not in CONTROL_WORDS:
            self.own(i, i + 2, function)                             # a label
            return i + 2
        if value in ('using', 'lock', 'fixed') and self.op(i + 1, '('):
            self.own(i, i + 1, function)
            j = self.condition(i + 1, function, nesting)
            return self.statement(j, end, function, nesting)
        if value in ('checked', 'unchecked', 'unsafe') and self.op(i + 1, '{'):
            self.own(i, i + 1, function)
            return self.statement(i + 1, end, function, nesting)
        if value == 'else':                                          # an else whose if was not seen
            self.own(i, i + 1, function)
            self.add(function, 1)
            return self.statement(i + 1, end, function, nesting + 1)
        local = self.local_function(i, end, function)
        if local is not None:
            return local
        if self.language != 'cs' and token.kind == 'name' and self.op(i + 1, '(') and \
                self.op(self.match[i + 1] + 1, '{') and value not in CONTROL_WORDS:
            j = self.match[i + 1] + 1                                # a macro block: FOR_EACH(x) { ... }
            self.own(i, j, function)
            self.expression(i + 2, j - 1, function, nesting)
            return self.statement(j, end, function, nesting)
        stop = self.statement_end(i, end)
        self.own(i, i, function)
        self.expression(i, stop, function, nesting)
        self.own(stop, min(stop + 1, end), function)
        self.stack_arrays(i, stop, function)
        return stop + 1

    def if_statement(self, i: int, end: int, function: Function, nesting: int) -> int:
        self.own(i, i + 1, function)
        self.add(function, 1 + nesting, nesting + 1)
        j = i + 1
        if self.word(j, 'constexpr'):
            self.own(j, j + 1, function)
            j += 1
        j = self.condition(j, function, nesting)
        j = self.statement(j, end, function, nesting + 1)
        while self.word(j, 'else'):
            self.own(j, j + 1, function)
            self.add(function, 1)
            if self.word(j + 1, 'if'):
                k = j + 2
                self.own(j + 1, k, function)
                if self.word(k, 'constexpr'):
                    self.own(k, k + 1, function)
                    k += 1
                k = self.condition(k, function, nesting)
                j = self.statement(k, end, function, nesting + 1)
                continue
            return self.statement(j + 1, end, function, nesting + 1)
        return j

    def local_function(self, i: int, end: int, function: Function) -> int | None:
        """A C# local function at statement i (`[static] [async] Type Name(params) { ... }` or `=> ...;`)."""
        if self.language != 'cs' or self.word(i, *CONTROL_WORDS) or self.word(i, 'var', 'yield', 'return'):
            return None
        k = i
        while k < end and not self.op(k, ';', '{', '=', '=>', '(') :
            if self.op(k, '[', '<'):
                k = self.match[k] + 1 if self.op(k, '[') else k + 1
                continue
            k += 1
        if not self.op(k, '(') or k == i:
            return None
        found = self.name_before(k)
        if not found or found[1] == i:                              # a call: Name(...) at the statement's start
            return None
        if any(self.word(x, 'new') for x in range(i, found[1])):
            return None
        j = self.trailer(self.match[k] + 1, end)
        name = f'{function.name}.{found[0]}'
        if self.op(j, '{'):
            local = self.new_function(name, self.at(found[1]).line, self.parameters(k), found[1], short=found[0],
                                      outer=function)
            self.own(i, i + 1, function)
            self.claim(i, j, local)
            self.body(j, self.match[j], local)
            return self.match[j] + 1
        if self.op(j, '=>'):
            local = self.new_function(name, self.at(found[1]).line, self.parameters(k), found[1], short=found[0],
                                      outer=function)
            self.own(i, i + 1, function)
            self.claim(i, j, local)
            return self.expression_member(j, name, 0, local)
        return None

    def expression(self, i: int, stop: int, function: Function | None, nesting: int) -> None:
        """Boolean runs, ternaries, switch expressions, recursion and nested lambdas in [i, stop)."""
        runs: list[str | None] = [None]
        k = i
        while k < stop:
            token = self.at(k)
            if self.owner[k] is not None and self.owner[k] is not function:
                k += 1                                              # a nested function's token
                continue
            lambda_end = self.nested_lambda(k, stop, function)
            if lambda_end is not None:
                k = lambda_end
                runs[-1] = None
                continue
            if function is not None:
                self.own(k, k + 1, function)
            if token.kind == 'op':
                value = token.value
                if value in ('&&', '||'):
                    if runs[-1] != value:
                        self.add(function, 1)
                    runs[-1] = value
                elif value in '([{':
                    runs.append(None)
                elif value in ')]}':
                    if len(runs) > 1:
                        runs.pop()
                elif value == '?' and self.is_ternary(k, stop):
                    self.add(function, 1 + nesting, nesting + 1)
                    runs[-1] = None
                elif value in RUN_BREAKERS:
                    runs[-1] = None
            elif token.kind == 'name':
                if token.value == 'switch' and self.language == 'cs' and self.op(k + 1, '{'):
                    self.add(function, 1 + nesting, nesting + 1)    # a switch expression
                elif function is not None and not function.calls_itself and token.value == function.short \
                        and self.op(k + 1, '(') and not self.op(k - 1, '.', '->', '::') and \
                        not self.word(k - 1, 'new', 'nameof'):
                    function.calls_itself = True
                    self.add(function, 1)
                elif token.value in ('return', 'throw', 'await', 'new'):
                    runs[-1] = None
            k += 1

    def is_ternary(self, k: int, stop: int) -> bool:
        if self.language != 'cs':
            return True
        following, previous = self.at(k + 1), self.at(k - 1)
        if following.kind == 'op' and following.value in ('>', ',', ')', ']', ';', '[', '=', '>>'):
            return False                                            # int?, List<int?>, (int?)x
        if following.kind == 'name' and self.op(k + 2, '=', ';', ',', ')', '{', '=>') and \
                (previous.kind == 'name' or self.op(k - 1, '>', ']')):
            return False                                            # string? s = ...: a ternary's operand is followed by ':'
        return True

    def nested_lambda(self, k: int, stop: int, function: Function | None) -> int | None:
        """A block lambda or anonymous method starting at k: measured as its own function; returns its end."""
        if self.language == 'cs':
            if self.op(k, '=>') and self.op(k + 1, '{'):
                params = self.lambda_parameters(k)
                name = f'{function.name}.<lambda>' if function else '<lambda>'
                inner = self.new_function(name, self.at(k).line, params, k, outer=function)
                self.claim(k, k + 1, inner)
                self.body(k + 1, self.match[k + 1], inner)
                if function is not None:
                    self.own(k, k + 1, function)
                return self.match[k + 1] + 1
            if self.word(k, 'delegate') and (self.op(k + 1, '{') or (self.op(k + 1, '(') and self.op(self.match[k + 1] + 1, '{'))):
                open_ = k + 1 if self.op(k + 1, '{') else self.match[k + 1] + 1
                params = self.parameters(k + 1) if self.op(k + 1, '(') else 0
                name = f'{function.name}.<delegate>' if function else '<delegate>'
                inner = self.new_function(name, self.at(k).line, params, k, outer=function)
                self.claim(k, open_, inner)
                self.body(open_, self.match[open_], inner)
                if function is not None:
                    self.own(k, k + 1, function)
                return self.match[open_] + 1
            return None
        if self.language == 'cpp' and self.op(k, '[') and not self.op(k + 1, '[') and \
                (k == 0 or self.at(k - 1).kind == 'op' and self.at(k - 1).value not in (')', ']') or
                 self.word(k - 1, 'return')):
            j = self.match[k] + 1
            params = 0
            if self.op(j, '('):
                params = self.parameters(j)
                j = self.match[j] + 1
            j = self.trailer(j, stop)
            if self.op(j, '{'):
                name = f'{function.name}.<lambda>' if function else '<lambda>'
                inner = self.new_function(name, self.at(k).line, params, k, outer=function)
                self.claim(k, j, inner)
                self.body(j, self.match[j], inner)
                if function is not None:
                    self.own(k, k + 1, function)
                return self.match[j] + 1
        return None

    def lambda_parameters(self, arrow: int) -> int:
        if self.op(arrow - 1, ')') and self.match[arrow - 1] >= 0:
            return self.parameters(self.match[arrow - 1])
        return 1 if self.word(arrow - 1) else 0

    def stack_arrays(self, i: int, stop: int, function: Function | None) -> None:
        """C and C++: the bytes of the fixed-size local arrays declared by the statement [i, stop)."""
        if function is None or self.language == 'cs':
            return
        words = []
        k = i
        while k < stop and self.word(k):
            words.append(self.at(k).value)
            k += 1
        if not words or 'static' in words or 'extern' in words or 'typedef' in words:
            return
        starred = self.op(i + len(words), '*')                     # char *names[16]: the name follows the stars
        type_words = words if starred else words[:-1]
        size = next((SCALAR_BYTES[w] for w in reversed(type_words) if w in SCALAR_BYTES), None)
        if size is None:
            return
        k = i + len(words) - (0 if starred else 1)                 # the declared name and what follows
        total = 0
        while k < stop:
            pointer = 0
            while self.op(k, '*'):
                pointer, k = 1, k + 1
            if not self.word(k):
                break
            k += 1
            count, sized = 1, False
            while self.op(k, '['):
                value = self.constant(k + 1, self.match[k])
                if value is None:
                    count, sized = 0, False
                    break
                count, sized = count * value, True
                k = self.match[k] + 1
            if sized:
                total += count * (8 if pointer else size)
            while k < stop and not self.op(k, ','):
                k = self.match[k] + 1 if self.op(k, '(', '[', '{') else k + 1
            k += 1
        function.stack_bytes += total

    def constant(self, start: int, stop: int) -> int | None:
        """An array size written with numbers, known constants and + - * / << >> ( ); None for anything else."""
        parts: list[int | str] = []
        for k in range(start, stop):
            token = self.at(k)
            if token.kind == 'number':
                text = re.sub(r"[uUlL']+$", '', token.value.replace("'", ''))
                try:
                    parts.append(int(text, 0))
                except ValueError:
                    return None
            elif token.kind == 'name' and token.value in KNOWN_CONSTANTS:
                parts.append(KNOWN_CONSTANTS[token.value])
            elif token.kind == 'op' and token.value in _Arithmetic.OPERATORS:
                parts.append(token.value)
            else:
                return None
        value = _Arithmetic(parts).value()
        return value if value is not None and value > 0 else None


class _Arithmetic:
    """C's integer arithmetic over a size's tokens, by precedence (shifts below + -, below * /), with no exponent: two
    operators in a row, as in `9**9`, are not a size. Values from 2**64 on are refused, so no size makes a huge
    number."""
    OPERATORS = ('+', '-', '*', '/', '(', ')', '<<', '>>')
    LIMIT = 1 << 64

    def __init__(self, parts: list):
        self.parts, self.at = parts, 0

    def value(self) -> int | None:
        if not self.parts:
            return None
        try:
            result = self.shift()
        except (ValueError, ZeroDivisionError, RecursionError):
            return None
        return result if self.at == len(self.parts) else None

    def peek(self):
        return self.parts[self.at] if self.at < len(self.parts) else None

    def take(self):
        part = self.peek()
        self.at += 1
        return part

    def bounded(self, value: int) -> int:
        if abs(value) >= self.LIMIT:
            raise ValueError('too large')
        return value

    def shift(self) -> int:
        value = self.additive()
        while self.peek() in ('<<', '>>'):
            operator, count = self.take(), self.additive()
            if not 0 <= count < 64:
                raise ValueError('shift count')
            value = self.bounded(value << count if operator == '<<' else value >> count)
        return value

    def additive(self) -> int:
        value = self.term()
        while self.peek() in ('+', '-'):
            operator, right = self.take(), self.term()
            value = self.bounded(value + right if operator == '+' else value - right)
        return value

    def term(self) -> int:
        value = self.unary()
        while self.peek() in ('*', '/'):
            operator, right = self.take(), self.unary()
            value = self.bounded(value * right if operator == '*' else int(value / right))   # C truncates
        return value

    def unary(self) -> int:
        if self.peek() in ('+', '-'):
            operator = self.take()
            if self.peek() in ('+', '-'):
                raise ValueError('two signs')
            value = self.unary()
            return -value if operator == '-' else value
        return self.primary()

    def primary(self) -> int:
        part = self.take()
        if isinstance(part, int):
            return self.bounded(part)
        if part == '(':
            value = self.shift()
            if self.take() != ')':
                raise ValueError('unclosed (')
            return value
        raise ValueError('not a number')


@dataclass
class Model:
    tokens: list[Token]
    functions: list[Function]
    code_lines: int


def analyse(text: str, language: str) -> Model:
    tokens, directives, _ = tokenize(text, language)
    source = _Source(tokens, language)
    source.scope(0, len(tokens))
    code_lines = len({token.line for token in tokens} | directives)
    return Model(tokens, source.functions, code_lines)


# ----------------------------------------------------------------------------- duplicated code

@dataclass
class Clone:
    file: int          # index into the files list
    start: int         # first token index
    end: int           # one past the last token index
    other_file: int    # the first other occurrence
    other_start: int


def duplicates(files: list[list[Token]], length: int) -> list[Clone]:
    """Regions of at least `length` tokens that also occur elsewhere (another file, or a non-overlapping place in
    the same file). One Clone per maximal region and file, naming the first other occurrence found (Bingus Shared
    Runtime's metrics.duplicates)."""
    ids: dict[tuple[str, str], int] = {}
    sequences = [[ids.setdefault((t.kind, t.value), len(ids)) + 1 for t in tokens] for tokens in files]
    base, modulus = 1_000_003, (1 << 61) - 1
    power = pow(base, length - 1, modulus)
    buckets: dict[int, list[tuple[int, int]]] = {}
    for f, sequence in enumerate(sequences):
        if len(sequence) < length:
            continue
        value = 0
        for item in sequence[:length]:
            value = (value * base + item) % modulus
        buckets.setdefault(value, []).append((f, 0))
        for start in range(1, len(sequence) - length + 1):
            value = ((value - sequence[start - 1] * power) * base + sequence[start + length - 1]) % modulus
            buckets.setdefault(value, []).append((f, start))
    covered: list[dict[int, tuple[int, int]]] = [{} for _ in files]
    for places in buckets.values():
        if len(places) < 2:
            continue
        groups: dict[tuple, list[tuple[int, int]]] = {}
        for f, start in places:                      # hash collisions: group by content
            groups.setdefault(tuple(sequences[f][start:start + length]), []).append((f, start))
        for group in groups.values():
            for f, start in group:
                other = next((p for p in group if p[0] != f or abs(p[1] - start) >= length), None)
                if other is not None:
                    for position in range(start, start + length):
                        covered[f].setdefault(position, other)
    clones = []
    for f, positions in enumerate(covered):
        ordered = sorted(positions)
        start = 0
        for index in range(1, len(ordered) + 1):
            if index == len(ordered) or ordered[index] != ordered[index - 1] + 1:
                first = ordered[start]
                other = positions[first]
                clones.append(Clone(f, first, ordered[index - 1] + 1, other[0], other[1]))
                start = index
    return clones
