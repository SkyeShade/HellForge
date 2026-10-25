"""lint.py: every rule at and over its limit, lint-ok, R14 per language family, the baseline ratchet, the command line.

Each test writes a small repository: units under `src/`, a config, and files whose expected violations are marked
`expect: R<n>` on their lines.
"""
import contextlib
import io
import json
import os
import re
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import lint  # noqa: E402

CONFIG = {'format': 1, 'discover': ['src'], 'default_sources': ['**/*.cs', '**/*.c', '**/*.h', '**/*.cpp'],
          'exclude': ['**/generated/**']}


def branches(count: int, name: str = 'a') -> str:
    """A body of `count` top-level ifs: cognitive complexity `count` (names differ per call: no R14)."""
    return '\n'.join(f'    if ({name}{i} == {i}) {{ {name}_{i}++; }}' for i in range(count))


def lines(count: int, name: str = 'b') -> str:
    return '\n'.join(f'    {name}{i} += {i};' for i in range(count))


class Repository:
    def __init__(self, files: dict[str, str], config: dict | None = None):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        for rel, text in files.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(textwrap.dedent(text).lstrip('\n'), encoding='utf-8')
        (self.root / 'lint-config.json').write_text(json.dumps(config or CONFIG), encoding='utf-8')
        self.baseline = self.root / 'baseline.json'

    def run(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = lint.main([*args, '--root', str(self.root), '--config', str(self.root / 'lint-config.json'),
                              '--baseline', str(self.baseline)])
        return code, out.getvalue()

    def scan(self) -> dict:
        code, out = self.run('scan', '--json')
        assert code == 0, out
        return json.loads(out)

    def expected(self) -> set[tuple[str, int, str]]:
        found = set()
        for path in self.root.rglob('*'):
            if path.suffix in ('.cs', '.c', '.h', '.cpp') and 'generated' not in path.parts:
                rel = path.relative_to(self.root / 'src').as_posix().split('/', 1)[1]
                for number, text in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
                    for rule in re.findall(r'expect: (R\d+)', text):
                        found.add((rel, number, rule))
        return found

    def close(self):
        self.folder.cleanup()


class Rules(unittest.TestCase):
    def setUp(self):
        cs = f'''
        class Sample
        {{
            void AtLimit(int a, int b)
            {{
        {branches(15, 'limit')}
            }}
            void Over(int a, int b) // expect: R10
            {{
        {branches(16, 'over')}
            }}
            void Allowed(int a, int b) // lint-ok: R10 measured in another way
            {{
        {branches(16, 'allowed')}
            }}
            void Seven(int a, int b, int c, int d, int e, int f, int g) {{ }}
            void Eight(int a, int b, int c, int d, int e, int f, int g, int h) {{ }} // expect: R12
            void Long(int b) // expect: R11
            {{
        {lines(148, 'long')}
            }}
            void Fits(int b)
            {{
        {lines(147, 'fits')}
            }}
        }}
        '''
        c = '''
        void stack_at_limit(void) { char buffer[16384]; use(buffer); }
        void stack_over(void) { char buffer[16384]; int more[1]; use(buffer, more); } /* expect: R13 */
        void stack_static(void) { static char kept[1 << 20]; use(kept); }
        '''
        self.repo = Repository({'src/app/Sample.cs': cs, 'src/native/stack.c': c,
                                'src/native/generated/table.c': 'void x(void) { if (a && b || c) { } }'})

    def tearDown(self):
        self.repo.close()

    def test_marked_lines(self):
        result = self.repo.scan()
        found = {(v['file'], v['line'], v['rule']) for v in result['violations']}
        self.assertEqual(found, self.repo.expected())
        text = (self.repo.root / 'src/app/Sample.cs').read_text(encoding='utf-8').splitlines()
        allowed = next(n for n, line in enumerate(text, 1) if 'lint-ok' in line)
        self.assertEqual([(v['rule'], v['line']) for v in result['suppressed']], [('R10', allowed)])
        long = next(v for v in result['violations'] if v['rule'] == 'R11')
        self.assertEqual(long['value'], 151)                     # signature, braces and 148 lines


class Duplicates(unittest.TestCase):
    def test_families_and_files(self):
        body = ' '.join(f'total += values[{i}] * {i};' for i in range(20))
        repo = Repository({
            'src/one/a.c': f'void a(void) {{ {body} }}\n',
            'src/two/b.cpp': f'void b() {{ {body} }}\n',
            'src/three/C.cs': f'class C {{ void D() {{ {body} }} }}\n',
            'src/three/E.cs': 'class E { void F() { } }\n',
        })
        try:
            found = sorted((v['unit'], v['file']) for v in repo.scan()['violations'] if v['rule'] == 'R14')
            self.assertEqual(found, [('one', 'a.c'), ('two', 'b.cpp')])   # C# is compared with C# only
            code, out = repo.run('scan', '--only', 'one', '--list')
            self.assertIn('also at two b.cpp:1', out)                    # a unit outside --only still counts
        finally:
            repo.close()

    def test_module_size(self):
        repo = Repository({'src/big/big.c': '\n'.join(f'int big{i};' for i in range(1001)) + '\n',
                           'src/ok/ok.c': '\n'.join(f'int ok{i};' for i in range(1000)) + '\n'})
        try:
            found = [(v['file'], v['line'], v['value']) for v in repo.scan()['violations']]
            self.assertEqual(found, [('big.c', 1, 1001)])
        finally:
            repo.close()


def params(count: int, name: str = 'p') -> str:
    return ', '.join(f'int {name}{i}' for i in range(count))


def cs(*methods: str) -> str:
    return 'class A {\n' + '\n'.join(methods) + '\n}\n'


class Baseline(unittest.TestCase):
    def setUp(self):
        self.repo = Repository({'src/app/A.cs': cs(f'void F({params(8)}) {{ }}')})
        self.path = self.repo.root / 'src/app/A.cs'

    def tearDown(self):
        self.repo.close()

    def write(self, text: str):
        self.path.write_text(text, encoding='utf-8')

    def test_error_rule(self):
        """R12 is an error: new fails; once known, growth warns; a renamed function is new."""
        code, out = self.repo.run('check')                            # no baseline: F is new
        self.assertEqual(code, 1, out)
        self.assertIn('ERROR R12 parameters  app A.cs:2  A.F  (new, or over the limit for the first time)', out)
        self.assertIn('lint-ok', out)
        self.assertIn('scripts/lint/README.md', out)
        self.assertEqual(self.repo.run('update')[0], 1)               # increases need --allow-increase
        self.assertEqual(self.repo.run('update', '--allow-increase')[0], 0)
        code, out = self.repo.run('check')
        self.assertEqual(code, 0, out)
        self.assertIn('0 error(s), 0 warning(s), 1 known', out)
        self.write(cs(f'void F({params(9)}) {{ }}'))                   # one more parameter: the same function, grown
        code, out = self.repo.run('check')
        self.assertEqual(code, 0, out)
        self.assertIn('WARNING R12 parameters  app A.cs:2  A.F  (known, grew: baseline 8, now 9)', out)
        self.write(cs(f'void G({params(9)}) {{ }}'))                   # renamed: a new function
        code, out = self.repo.run('check')
        self.assertEqual(code, 1, out)
        self.assertIn('ERROR R12 parameters  app A.cs:2  A.G', out)
        self.assertIn('lower - run update: R12 parameters  app A.cs  A.F  (baseline 8, gone)', out)

    def test_warning_rule(self):
        """R10 is a warning: new or over the limit for the first time, check passes and says so."""
        self.write(cs(f'void F(int a) {{\n{branches(16)}\n}}'))
        code, out = self.repo.run('check')
        self.assertEqual(code, 0, out)
        self.assertIn('WARNING R10 cognitive-complexity  app A.cs:2  A.F  (new, or over the limit for the first time)',
                      out)
        self.assertIn('0 error(s), 1 warning(s)', out)

    def test_signature_and_first_line(self):
        """The identity is the function's name: a changed signature line keeps it, and R15 is the file's."""
        fields = '\n'.join(f'int x{i};' for i in range(1000))
        self.write(cs(f'void F(int a) {{\n{branches(16)}\n}}') + fields)
        self.assertEqual(self.repo.run('update', '--allow-increase')[0], 0)
        self.write(cs(f'void F(int a, int b) {{\n{branches(16)}\n}}').replace('class A {', 'sealed class A {') + fields)
        code, out = self.repo.run('check')
        self.assertEqual(code, 0, out)
        self.assertIn('0 error(s), 0 warning(s), 2 known', out)

    def test_same_names(self):
        """Overloads share an identity: their values pair up, and one more is new."""
        self.write(cs(f'void F({params(8)}) {{ }}', f'void F({params(9, "q")}) {{ }}'))
        self.assertEqual(self.repo.run('update', '--allow-increase')[0], 0)
        self.write(cs(f'void F({params(8)}) {{ }}', f'void F({params(9, "q")}) {{ }}',
                      f'void F({params(8, "r")}) {{ }}'))
        code, out = self.repo.run('check')
        self.assertEqual(code, 1, out)
        self.assertEqual(out.count('ERROR R12'), 1)
        self.assertIn('1 error(s), 0 warning(s), 2 known', out)

    def test_fixed(self):
        self.assertEqual(self.repo.run('update', '--allow-increase')[0], 0)
        self.write(cs('void F(int a) { }'))
        code, out = self.repo.run('check')
        self.assertEqual(code, 0, out)
        self.assertIn('lower - run update', out)
        self.assertEqual(self.repo.run('update')[0], 0)
        self.assertEqual(json.loads(self.repo.baseline.read_text(encoding='utf-8'))['entries'], [])

    def test_old_format(self):
        self.repo.baseline.write_text('{"format": 1, "entries": []}', encoding='utf-8')
        code, out = self.repo.run('check')
        self.assertEqual(code, 2)
        self.assertIn('format 1', out)
        self.assertEqual(self.repo.run('update')[0], 2)
        self.assertEqual(self.repo.run('update', '--allow-increase')[0], 0)  # replaced by a format 2 baseline
        self.assertEqual(json.loads(self.repo.baseline.read_text(encoding='utf-8'))['format'], 2)

    def test_severity_config(self):
        self.repo.close()
        self.repo = Repository({'src/app/A.cs': cs(f'void F(int a) {{\n{branches(16)}\n}}')},
                               dict(CONFIG, severity={'R10': 'error'}))
        self.assertEqual(self.repo.run('check')[0], 1)               # R10 made an error
        for severity in ({'R99': 'error'}, {'R10': 'fatal'}):
            other = Repository({'src/app/A.cs': 'class A { }'}, dict(CONFIG, severity=severity))
            try:
                self.assertEqual(other.run('check')[0], 2)
            finally:
                other.close()

    def test_annotations(self):
        """In GitHub Actions, each error and warning is also an annotation on its line."""
        with mock.patch.dict(os.environ, {'GITHUB_ACTIONS': 'true'}):
            code, out = self.repo.run('check')
        self.assertEqual(code, 1)
        self.assertIn('::error file=src/app/A.cs,line=2,title=R12 parameters::A.F: 8 parameters (limit 7)', out)

    def test_only_keeps_other_units(self):
        repo = Repository({'src/a/A.cs': f'class A {{\nvoid F(int a) {{\n{branches(16, "x")}\n}}\n}}\n',
                           'src/b/B.cs': f'class B {{\nvoid G(int a) {{\n{branches(16, "y")}\n}}\n}}\n'})
        try:
            self.assertEqual(repo.run('update', '--allow-increase')[0], 0)
            (repo.root / 'src/a/A.cs').write_text('class A { void F() { } }\n', encoding='utf-8')
            self.assertEqual(repo.run('update', '--only', 'a')[0], 0)
            units = [e['unit'] for e in json.loads(repo.baseline.read_text(encoding='utf-8'))['entries']]
            self.assertEqual(units, ['b'])
        finally:
            repo.close()


class CommandLine(unittest.TestCase):
    def test_errors(self):
        repo = Repository({'src/a/a.c': 'void f(void) { if (a) { }\n'})
        try:
            code, out = repo.run('scan')
            self.assertEqual(code, 2)
            self.assertIn('a/a.c: line', out)
        finally:
            repo.close()
        repo = Repository({'src/a/a.c': 'void f(void) { }\n'}, dict(CONFIG, limits={'nonsense': 1}))
        try:
            code, out = repo.run('scan')
            self.assertEqual(code, 2)
            self.assertIn('unknown limits', out)
            self.assertEqual(repo.run('scan', '--only', 'missing')[0], 2)
        finally:
            repo.close()

    def test_repository_config(self):
        """The repository's own config reads, and its excludes name files that exist."""
        here = Path(__file__).resolve().parent.parent
        config = json.loads((here / 'lint-config.json').read_text(encoding='utf-8'))
        root = here.parent.parent
        for pattern in config['exclude']:
            if '*' not in pattern:
                self.assertTrue((root / pattern).is_file(), pattern)


if __name__ == '__main__':
    unittest.main()
