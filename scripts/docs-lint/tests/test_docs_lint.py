"""docs_lint.py: Markdown blocks, words and sentences, every rule at and over its limit, docs-lint-ok, the baseline
ratchet and the command line.

Each test writes a small repository (not a git work tree: files are found by walking it) with its own config.
"""
import contextlib
import io
import json
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import docs_lint  # noqa: E402

CONFIG = {'format': 1, 'include': ['*.md', '**/*.md'], 'typed': ['docs/*.md', 'docs/**/*.md'],
          'untyped': ['docs/README.md'], 'index': 'docs/README.md',
          'limits': {'paragraph_words': 10, 'sentence_words': 6, 'page_words': 40},
          'page_words_by_type': {'reference': None}}


def text_of(count: int, word: str = 'word') -> str:
    """`count` words, with no full stop."""
    return ' '.join([word] * count)


class Repository:
    def __init__(self, files: dict[str, str], config: dict | None = None):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.write(files)
        (self.root / 'config.json').write_text(json.dumps(config or CONFIG), encoding='utf-8')
        self.baseline = self.root / 'baseline.json'

    def write(self, files: dict[str, str]) -> None:
        for rel, text in files.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(textwrap.dedent(text).lstrip('\n'), encoding='utf-8')

    def run(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = docs_lint.main([*args, '--root', str(self.root), '--config', str(self.root / 'config.json'),
                                   '--baseline', str(self.baseline)])
        return code, out.getvalue()

    def scan(self) -> dict:
        code, out = self.run('scan', '--json')
        assert code == 0, out
        return json.loads(out)

    def found(self) -> set[tuple[str, int, str]]:
        return {(v['file'], v['line'], v['rule']) for v in self.scan()['violations']}

    def close(self):
        self.folder.cleanup()


def page(kind: str = 'how-to', body: str = '') -> str:
    return f'# Title\n\n<!-- diataxis: {kind} -->\n\n{body}\n'


INDEX = '# Docs\n\n- [A](a.md)\n- [B](sub/b.md#part)\n'


class Markdown(unittest.TestCase):
    def test_blocks(self):
        source = textwrap.dedent('''
            ---
            front: matter
            ---
            # Title

            Intro line one
            continues here.

            - item one
              continues
              - nested item
            1. numbered

            > a quote
            > goes on

            | table | row |
            |---|---|

            ```text
            code. Not prose.
            ```

            <details>
            <!-- a comment
            over lines -->
            ***
            ## Second
            Last.
            ''').lstrip('\n')
        blocks = docs_lint.parse_page('docs/x.md', source).blocks
        self.assertEqual([(b.kind, b.line, b.section) for b in blocks],
                         [('paragraph', 6, ''), ('item', 9, ''), ('item', 11, ''), ('item', 12, ''),
                          ('quote', 14, ''), ('paragraph', 29, 'Second')])
        self.assertEqual(blocks[1].text, 'item one\ncontinues')

    def test_type_and_links(self):
        parsed = docs_lint.parse_page('docs/sub/p.md', '# T\n\n<!-- diataxis: reference -->\n\n'
                                      '[a](../a.md#x) [b](b.md) [web](https://example.com) [here](#y)\n')
        self.assertEqual((parsed.type, parsed.type_line), ('reference', 3))
        self.assertEqual(parsed.links, {'docs/a.md', 'docs/sub/b.md'})

    def test_words(self):
        self.assertEqual(docs_lint.words('Run `a b c` with [two words](x.md) and **bold** \\<host> - 3.'), 9)

    def test_sentences(self):
        text = 'One two, e.g. three. Four! Is it 5? Yes (it is). The file a.md is here. lower case. End `x`.'
        self.assertEqual(docs_lint.sentences(text), ['One two, e.g. three.', 'Four!', 'Is it 5?', 'Yes (it is).',
                                                     'The file a.md is here. lower case.', 'End CODE.'])


class Rules(unittest.TestCase):
    def test_page_rules(self):
        repo = Repository({
            'docs/README.md': INDEX,
            'docs/a.md': page(),
            'docs/sub/b.md': page('wrong'),
            'docs/c.md': '# C\n\nNo type.\n',
            'README.md': '# Front page, no type needed\n',
        })
        try:
            self.assertEqual(repo.found(), {('docs/sub/b.md', 3, 'D1'), ('docs/c.md', 1, 'D1'),
                                            ('docs/c.md', 1, 'D2')})
        finally:
            repo.close()

    def test_sizes_at_and_over_the_limits(self):
        body = '\n\n'.join([
            'One two three four five six. Seven eight nine ten.',   # line 5: 10 words, a sentence of 6: at both
            'One two three four five six. Seven eight nine ten eleven.',   # line 7: 11 words: D3
            'Seven words in this one right here.',                  # line 9: D4
        ])
        repo = Repository({'docs/README.md': '# D\n\n[a](a.md)\n', 'docs/a.md': page(body=body)})
        try:
            self.assertEqual(repo.found(), {('docs/a.md', 7, 'D3'), ('docs/a.md', 9, 'D4')})
        finally:
            repo.close()

    def test_page_size_by_type(self):
        body = '\n\n'.join(text_of(5, f'w{i}') + '.' for i in range(9))   # 45 words over the page limit of 40
        repo = Repository({'docs/README.md': '# D\n\n[a](a.md) [b](b.md)\n', 'docs/a.md': page(body=body),
                           'docs/b.md': page('reference', body)})
        try:
            self.assertEqual(repo.found(), {('docs/a.md', 1, 'D5')})
        finally:
            repo.close()

    def test_suppression(self):
        body = ('<!-- docs-lint-ok: D3,D4 a quoted log line -->\n' + text_of(11) + '\n\n'
                '<!-- docs-lint-ok: D3 -->\n' + text_of(11, 'noreason') + '\n')
        repo = Repository({'docs/README.md': '# D\n\n[a](a.md)\n',
                           'docs/a.md': page(body=body) + '\n<!-- docs-lint-ok: D5 a long page on purpose -->\n'},
                          dict(CONFIG, limits={'paragraph_words': 10, 'sentence_words': 6, 'page_words': 5}))
        try:
            result = repo.scan()
            self.assertEqual({(v['line'], v['rule']) for v in result['violations']}, {(9, 'D3'), (9, 'D4')})
            self.assertEqual({(v['line'], v['rule']) for v in result['suppressed']},
                             {(1, 'D5'), (6, 'D3'), (6, 'D4')})
        finally:
            repo.close()


class Baseline(unittest.TestCase):
    def setUp(self):
        self.repo = Repository({'docs/README.md': '# D\n\n[a](a.md)\n',
                                'docs/a.md': page(body='## One\n\n' + text_of(11) + '\n')})

    def tearDown(self):
        self.repo.close()

    def test_ratchet(self):
        code, out = self.repo.run('check')
        self.assertEqual(code, 0)                                  # D3 and D4 are warnings
        self.assertIn('WARNING D3 paragraph-size  docs/a.md:7  [One]  (new)', out)
        self.assertEqual(self.repo.run('check', '--warnings-as-errors')[0], 1)
        self.assertEqual(self.repo.run('update')[0], 1)            # refuses increases
        self.assertEqual(self.repo.run('update', '--allow-increase')[0], 0)
        self.assertEqual(self.repo.run('check', '--warnings-as-errors')[0], 0)
        self.repo.write({'docs/a.md': page(body='## One\n\n' + text_of(12) + '\n')})
        code, out = self.repo.run('check', '--warnings-as-errors')
        self.assertEqual(code, 1)
        self.assertIn('known, grew: baseline 11, now 12', out)
        self.repo.write({'docs/a.md': page(body='## One\n\n' + text_of(5) + '.\n')})
        code, out = self.repo.run('check', '--warnings-as-errors')
        self.assertEqual(code, 0)
        self.assertIn('lower - run update: D3 paragraph-size  docs/a.md  [One]  (baseline 11, gone)', out)
        self.assertEqual(self.repo.run('update')[0], 0)
        self.assertEqual(json.loads(self.repo.baseline.read_text(encoding='utf-8'))['entries'], [])

    def test_identity_is_the_h2_section(self):
        self.repo.run('update', '--allow-increase')
        moved = page(body='## One\n\n### A new subsection\n\n' + text_of(11) + '\n')
        self.repo.write({'docs/a.md': moved})
        self.assertEqual(self.repo.run('check', '--warnings-as-errors')[0], 0)
        self.repo.write({'docs/a.md': page(body='## Renamed\n\n' + text_of(11) + '\n')})
        self.assertEqual(self.repo.run('check', '--warnings-as-errors')[0], 1)

    def test_error_rule_and_annotations(self):
        self.repo.write({'docs/b.md': '# B\n'})
        with mock.patch.dict('os.environ', {'GITHUB_ACTIONS': 'true'}):
            code, out = self.repo.run('check')
        self.assertEqual(code, 1)
        self.assertIn('::error file=docs/b.md,line=1,title=D1 page-type::', out)
        self.assertIn('::error file=docs/b.md,line=1,title=D2 index::', out)


class CommandLine(unittest.TestCase):
    def test_errors(self):
        repo = Repository({'docs/README.md': '# D\n'}, dict(CONFIG, limits={'nonsense': 1}))
        try:
            code, out = repo.run('scan')
            self.assertEqual(code, 2)
            self.assertIn('unknown limits', out)
        finally:
            repo.close()

    def test_repository_config(self):
        """The repository's own config reads, its index exists, and its untyped pages exist."""
        here = Path(__file__).resolve().parent.parent
        config = docs_lint.load_config(here / 'docs-lint-config.json')
        root = here.parent.parent
        self.assertTrue((root / config.index).is_file())
        for pattern in config.untyped + config.exclude:
            if '*' not in pattern:
                self.assertTrue((root / pattern).is_file(), pattern)


if __name__ == '__main__':
    unittest.main()
