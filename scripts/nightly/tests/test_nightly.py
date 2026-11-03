"""Tests of scripts/nightly/nightly.py: the nightly version, the Unreleased section and the release notes, against
small repositories made in a temporary folder. Run: python -m unittest discover -s scripts/nightly/tests -v"""
import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nightly  # noqa: E402

PROPS = """<Project>
  <PropertyGroup>
    <Version>{version}</Version>
    <EngineVersion>0.1.41</EngineVersion>
  </PropertyGroup>
</Project>
"""

CHANGELOG = """# Changelog

Intro.

## Unreleased

On `dev`, since 0.3.0-beta.

### Added

- A thing (#1).

### Fixed

- A bug (#2).

## 0.3.0-beta (2026-10-26)

### Added

- Old news.
"""


class Repo:
    """A git repository in a temporary folder."""

    def __init__(self, folder: Path):
        self.path = folder
        self.git('init', '-q', '-b', 'dev')

    def git(self, *args: str) -> str:
        result = subprocess.run(['git', '-C', str(self.path), '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                                 '-c', 'commit.gpgsign=false', '-c', 'tag.gpgsign=false', *args],
                                capture_output=True, text=True, check=True)
        return result.stdout.strip()

    def commit(self, message: str, files: dict[str, str] | None = None) -> str:
        for name, text in (files or {}).items():
            (self.path / name).write_text(text, encoding='utf-8')
            self.git('add', name)
        self.git('commit', '-q', '--allow-empty', '-m', message)
        return self.git('rev-parse', 'HEAD')

    def merge_pr(self, number: int, title: str) -> str:
        """A pull request's branch with one commit, merged into dev as GitHub merges one."""
        base = self.git('rev-parse', 'HEAD')
        self.git('checkout', '-q', '-b', f'pr{number}')
        self.commit(f'Work for {title}')
        self.git('checkout', '-q', 'dev')
        self.git('merge', '-q', '--no-ff', f'pr{number}', '-m', f'Merge pull request #{number} from SkyeShade/pr{number}',
                 '-m', title)
        assert base != self.git('rev-parse', 'HEAD')
        return self.git('rev-parse', 'HEAD')


class TempRepoTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Repo(Path(self.temp.name))


class NextReleaseTest(unittest.TestCase):
    def test_unreleased_version_keeps_its_number(self):
        self.assertEqual(nightly.next_release('0.4.0-dev.3', released=False), '0.4.0')
        self.assertEqual(nightly.next_release('0.4.0-beta', released=False), '0.4.0')
        self.assertEqual(nightly.next_release('1.2.3', released=False), '1.2.3')

    def test_released_version_is_followed_by_the_next_minor(self):
        self.assertEqual(nightly.next_release('0.3.0-beta', released=True), '0.4.0')
        self.assertEqual(nightly.next_release('0.1.13-beta', released=True), '0.2.0')

    def test_a_version_without_three_numbers_is_refused(self):
        for version in ('0.4', 'beta', '0.4.x', ''):
            with self.subTest(version=version), self.assertRaises(nightly.NightlyError):
                nightly.next_release(version, released=False)


class NightlyVersionTest(TempRepoTest):
    def test_released_version_gives_the_next_minor(self):
        self.repo.commit('props', {'Directory.Build.props': PROPS.format(version='0.3.0-beta')})
        self.repo.git('tag', 'v0.3.0-beta')
        self.assertEqual(nightly.nightly_version(self.repo.path, '20261102'), '0.4.0-nightly.20261102')

    def test_unreleased_version_keeps_its_number(self):
        self.repo.commit('props', {'Directory.Build.props': PROPS.format(version='0.4.0-dev.2')})
        self.repo.git('tag', 'v0.3.0-beta')
        self.assertEqual(nightly.nightly_version(self.repo.path, '20261231'), '0.4.0-nightly.20261231')

    def test_the_nightly_tag_is_not_a_release(self):
        self.repo.commit('props', {'Directory.Build.props': PROPS.format(version='0.4.0-beta')})
        self.repo.git('tag', 'nightly')
        self.assertEqual(nightly.nightly_version(self.repo.path, '20261102'), '0.4.0-nightly.20261102')

    def test_engine_version_is_not_read(self):
        props = '<Project><PropertyGroup><EngineVersion>9.9.9</EngineVersion></PropertyGroup></Project>'
        self.repo.commit('props', {'Directory.Build.props': props})
        with self.assertRaises(nightly.NightlyError):
            nightly.nightly_version(self.repo.path, '20261102')

    def test_bad_dates_are_refused(self):
        self.repo.commit('props', {'Directory.Build.props': PROPS.format(version='0.4.0')})
        for date in ('2026112', '202611021', '20261302', '20260230', '2026-11-02', 'today'):
            with self.subTest(date=date), self.assertRaises(nightly.NightlyError):
                nightly.nightly_version(self.repo.path, date)

    def test_command_line(self):
        self.repo.commit('props', {'Directory.Build.props': PROPS.format(version='0.4.0-dev.1')})
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(nightly.main(['--repo', str(self.repo.path), 'version', '--date', '20261102']), 0)
            self.assertEqual(nightly.main(['--repo', str(self.repo.path), 'version', '--date', '2026']), 1)
        self.assertEqual(out.getvalue(), '0.4.0-nightly.20261102\n')
        self.assertIn('YYYYMMDD', err.getvalue())


class UnreleasedTest(unittest.TestCase):
    def test_the_section_without_its_heading_and_with_smaller_headings(self):
        section = nightly.unreleased(CHANGELOG)
        self.assertTrue(section.startswith('On `dev`, since 0.3.0-beta.'))
        self.assertIn('#### Added\n\n- A thing (#1).', section)
        self.assertIn('#### Fixed\n\n- A bug (#2).', section)
        self.assertNotIn('Old news', section)
        self.assertNotIn('## Unreleased', section)

    def test_last_section_runs_to_the_end(self):
        self.assertEqual(nightly.unreleased('# C\n\n## Unreleased\n\n### Added\n\n- New.\n'), '#### Added\n\n- New.')

    def test_missing_or_empty_section(self):
        self.assertIsNone(nightly.unreleased('# Changelog\n\n## 0.3.0-beta\n'))
        self.assertIsNone(nightly.unreleased('# Changelog\n\n## Unreleased\n\n## 0.3.0-beta\n'))


class NotesTest(TempRepoTest):
    def setUp(self):
        super().setUp()
        self.release = self.repo.commit('The release', {'Directory.Build.props': PROPS.format(version='0.3.0-beta'),
                                                        'CHANGELOG.md': CHANGELOG})
        self.repo.git('tag', 'v0.3.0-beta')
        self.first = self.repo.merge_pr(10, 'Mods: the first change')
        self.direct = self.repo.commit('Changelog: a fix-up')
        self.last = self.repo.merge_pr(11, 'Editor: the second change')

    def notes(self, previous=None, commit=None):
        return nightly.notes(self.repo.path, '0.4.0-nightly.20261102', commit or self.last, previous, 'SkyeShade/HellForge')

    def test_says_what_it_is(self):
        text = self.notes()
        self.assertTrue(text.startswith('> [!WARNING]\n> **An untested development build.**'))
        self.assertIn('**HellForge `0.4.0-nightly.20261102`**', text)
        self.assertIn(f'`HellForge 0.4.0-nightly.20261102 ({self.last[:7]})`', text)
        self.assertIn(f'https://github.com/SkyeShade/HellForge/commit/{self.last}', text)
        self.assertIn('HellForge-Nightly-Setup.exe', text)
        self.assertIn('no Steam bridge or broker', text)

    def test_changes_since_the_previous_nightly(self):
        text = self.notes(previous=self.first)
        self.assertIn('### Changes since the previous nightly', text)
        self.assertIn(f'/compare/{self.first}...{self.last}', text)
        self.assertIn('- #11: Editor: the second change', text)
        self.assertIn(f'- Changelog: a fix-up ({self.direct[:7]})', text)
        self.assertNotIn('#10', text)
        # dev's own history only: the pull request's own commits are not listed
        self.assertNotIn('Work for', text)
        self.assertLess(text.index('#11'), text.index('Changelog: a fix-up'))

    def test_first_nightly_lists_the_changes_since_the_release(self):
        text = self.notes()
        self.assertIn('### Changes since v0.3.0-beta', text)
        self.assertIn(f'/compare/v0.3.0-beta...{self.last}', text)
        self.assertIn('- #10: Mods: the first change', text)
        self.assertIn('- #11: Editor: the second change', text)

    def test_a_previous_nightly_off_dev_falls_back_to_the_release(self):
        self.repo.git('checkout', '-q', '-b', 'elsewhere', self.release)
        elsewhere = self.repo.commit('Not on dev')
        self.repo.git('checkout', '-q', 'dev')
        text = self.notes(previous=elsewhere)
        self.assertIn('### Changes since v0.3.0-beta', text)
        self.assertNotIn('### Changes since the previous nightly', text)

    def test_a_rebuild_of_the_same_commit(self):
        self.assertIn('Nothing new on `dev`', self.notes(previous=self.last))

    def test_the_changelog_section(self):
        text = self.notes()
        self.assertIn('### What\'s new since v0.3.0-beta', text)
        self.assertIn(f'/blob/{self.last}/CHANGELOG.md', text)
        self.assertIn('#### Added\n\n- A thing (#1).', text)
        self.assertNotIn('Old news', text)

    def test_long_lists_are_cut(self):
        for number in range(20, 20 + nightly.MAX_CHANGES + 5):
            self.repo.merge_pr(number, f'Change {number}')
        head = self.repo.git('rev-parse', 'HEAD')
        text = self.notes(previous=self.last, commit=head)
        self.assertIn('- and 5 more: see the comparison above.', text)
        self.assertEqual(text.count('\n- #'), nightly.MAX_CHANGES)

    def test_notes_stay_under_githubs_limit(self):
        huge = CHANGELOG.replace('- A thing (#1).', '\n'.join(f'- Line {n} ' + 'x' * 100 for n in range(2_000)))
        head = self.repo.commit('A long changelog', {'CHANGELOG.md': huge})
        text = self.notes(commit=head)
        self.assertLessEqual(len(text), nightly.MAX_NOTES)
        self.assertIn('Cut short', text)
        self.assertTrue(text.rstrip().endswith('#nightly-builds)).'))


if __name__ == '__main__':
    unittest.main()
