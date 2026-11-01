"""engine_version.py: which paths are engine sources, the commit that set the version, and each kind of pull request.

The scenario tests build small git repositories: a Directory.Build.props and a few files, committed on a dev branch and
on pull request branches. One test reads this repository's own Build-Engine.ps1 and engine projects.
"""
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import engine_version as ev  # noqa: E402

REPOSITORY = HERE.parent.parent.parent
PROPS = """<Project>
  <PropertyGroup>
    <Version>{version}</Version>
    <EngineVersion>{engine}</EngineVersion>
  </PropertyGroup>
</Project>
"""


class Repository:
    """A git repository in a temporary folder, on branch dev."""

    def __init__(self) -> None:
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.git('init', '-q', '-b', 'dev')
        self.props('0.1.0', '0.0.1')
        self.write('native/mod-loader/loader.c', 'int a;')
        self.write('docs/engine.md', 'docs')
        self.commit('HellForge 0.1.0, engine 0.0.1: first')

    def close(self) -> None:
        self.folder.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(['git', '-C', str(self.root), '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                               '-c', 'commit.gpgsign=false', *args], check=True, capture_output=True, text=True).stdout

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def props(self, version: str, engine: str) -> None:
        self.write(ev.PROPS, PROPS.format(version=version, engine=engine))

    def commit(self, message: str) -> str:
        self.git('add', '-A')
        self.git('commit', '-q', '-m', message)
        return self.git('rev-parse', 'HEAD').strip()

    def change(self, rel: str, message: str = 'change') -> str:
        path = self.root / rel
        self.write(rel, (path.read_text(encoding='utf-8') if path.exists() else '') + '\n// more')
        return self.commit(message)

    def bump(self, version: str, engine: str) -> str:
        self.props(version, engine)
        return self.commit(f'HellForge {version}, engine {engine}: a build')

    def branch(self, name: str, start: str = 'dev') -> None:
        self.git('checkout', '-q', '-b', name, start)

    def checkout(self, name: str) -> None:
        self.git('checkout', '-q', name)

    def merge(self, name: str) -> None:
        self.git('merge', '-q', '--no-ff', '-m', f'Merge {name}', name)

    def run(self, *args: str, actions: bool = False) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        env = {'GITHUB_ACTIONS': 'true' if actions else 'false', 'GITHUB_STEP_SUMMARY': ''}
        with mock.patch.dict(os.environ, env), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ev.main(['--repo', str(self.root), *args])
        return code, out.getvalue(), err.getvalue()


class EngineSources(unittest.TestCase):
    def test_engine_paths(self):
        for path in ('native/mod-loader/hellforge_modloader.c', 'native/steam-cloud/bridge-methods.json',
                     'native/mod-sdk/hellforge_mod.h', 'src/HellForge.Core/Mods.cs', 'src/HellForge.Session/Program.cs',
                     'src/HellForge.LocalBackend/war-data.json', 'mappings/components.json', 'data/verified-mods.json',
                     'scripts/Install-WfpBlock.ps1', 'scripts/Build-Engine.ps1', 'LICENSE'):
            with self.subTest(path=path):
                self.assertTrue(ev.is_engine_source(path))

    def test_not_engine_paths(self):
        for path in ('native/editor/app.cpp', 'native/launcher-stub/launcher_stub.c', 'native/mod-loader/test/stats_test.c',
                     'native/steam-cloud/test/broker_test.c', 'native/common/test/privacy_test.c',
                     'src/HellForge.Launcher/MainForm.cs', 'src/HellForge.Cli/Program.cs', 'tests/HellForge.Tests/A.cs',
                     'src/HellForge.Core/README.md', 'mappings/README.md', 'docs/engine.md', 'Directory.Build.props',
                     'data/war-presets.json', 'scripts/Test.ps1', 'LICENSE-SDK', 'native/mod-loader-old/a.c'):
            with self.subTest(path=path):
                self.assertFalse(ev.is_engine_source(path))

    def test_every_path_the_engine_build_names_is_classified(self):
        """Build-Engine.ps1's repository paths and the engine projects' ../../ includes are engine sources or BUILD_ONLY."""
        script = (REPOSITORY / 'scripts/Build-Engine.ps1').read_text(encoding='utf-8')
        named = set(re.findall(r"Join-Path \$(?:project|open) '([^']+)'", script))
        named |= {'scripts/' + name for name in re.findall(r"Join-Path \$PSScriptRoot '([^']+)'", script)}
        shipped = re.search(r"foreach\(\$name in @\(([^)]*)\)\)\{Copy-Item -LiteralPath \(Join-Path \$PSScriptRoot", script)
        self.assertIsNotNone(shipped, 'Build-Engine.ps1 no longer lists the scripts it ships the way this test reads')
        named |= {'scripts/' + name for name in re.findall(r"'([^']+)'", shipped.group(1))}
        for project in ('HellForge.Core', 'HellForge.Session', 'HellForge.LocalBackend', 'HellForge.SdkShimGen'):
            text = (REPOSITORY / 'src' / project / f'{project}.csproj').read_text(encoding='utf-8')
            named |= {rel for rel in re.findall(r'Include="\.\./\.\./([^"]+)"', text)}
        named = {path for path in named if not path.startswith(('artifacts/', 'tools/'))}
        self.assertIn('scripts/Test-SteamOwnership.ps1', named)
        for path in sorted(named):
            probe = path if (REPOSITORY / path).is_file() else path.rstrip('/') + '/probe.c'
            build_only = any(probe == entry or (entry.endswith('/') and probe.startswith(entry)) for entry in ev.BUILD_ONLY)
            test_path = bool(ev.TEST_FOLDERS.intersection(probe.split('/')[:-1]))
            with self.subTest(path=path):
                self.assertTrue(ev.is_engine_source(probe) or build_only or test_path,
                                f'{path}: add it to ENGINE_SOURCES or BUILD_ONLY in engine_version.py')


class Scenarios(unittest.TestCase):
    def setUp(self):
        self.repo = Repository()
        self.addCleanup(self.repo.close)

    def test_unchanged_since_the_bump(self):
        self.repo.change('docs/engine.md')
        code, out, _ = self.repo.run()
        self.assertEqual(code, 0)
        self.assertIn('unchanged since engine 0.0.1', out)

    def test_ordinary_pull_request_changing_the_engine_warns(self):
        self.repo.branch('feature')
        self.repo.change('native/mod-loader/loader.c')
        code, out, _ = self.repo.run('--base', 'dev', actions=True)
        self.assertEqual(code, 0)
        self.assertRegex(out, r'^::warning file=Directory\.Build\.props,line=4,title=Engine version::This pull request '
                              r'changes the engine \(native/mod-loader/loader\.c\)')

    def test_ordinary_pull_request_after_unbumped_engine_merges_is_a_notice(self):
        self.repo.change('native/mod-loader/loader.c')
        self.repo.branch('feature')
        self.repo.change('docs/engine.md')
        self.repo.change('native/mod-loader/test/loader_test.c')
        code, out, _ = self.repo.run('--base', 'dev', actions=True)
        self.assertEqual(code, 0)
        self.assertIn('::notice ', out)
        self.assertIn('in earlier merges: native/mod-loader/loader.c', out)

    def test_version_bump_without_the_engine_fails(self):
        self.repo.change('src/HellForge.Session/PlaySession.cs')
        self.repo.branch('build')
        self.repo.props('0.2.0', '0.0.1')
        self.repo.commit('HellForge 0.2.0')
        code, out, _ = self.repo.run('--base', 'dev', actions=True)
        self.assertEqual(code, 1)
        self.assertIn('::error ', out)
        self.assertIn('a version-bump pull request (HellForge 0.1.0 to 0.2.0)', out)
        self.assertIn('src/HellForge.Session/PlaySession.cs', out)

    def test_version_bump_with_the_engine_passes(self):
        self.repo.change('native/mod-loader/loader.c')
        self.repo.branch('build')
        self.repo.bump('0.2.0', '0.0.2')
        self.assertEqual(self.repo.run('--base', 'dev')[0], 0)

    def test_version_bump_with_the_engine_unchanged_passes(self):
        self.repo.change('src/HellForge.Launcher/MainForm.cs')
        self.repo.branch('build')
        self.repo.props('0.2.0', '0.0.1')
        self.repo.commit('HellForge 0.2.0')
        self.assertEqual(self.repo.run('--base', 'dev')[0], 0)

    def test_engine_change_after_the_bump_in_the_same_pull_request_fails(self):
        self.repo.branch('build')
        self.repo.bump('0.2.0', '0.0.2')
        self.repo.change('native/mod-loader/loader.c')
        code, out, _ = self.repo.run('--base', 'dev')
        self.assertEqual(code, 1)
        self.assertIn('since engine 0.0.2', out)

    def test_release_fails_on_an_unbumped_engine(self):
        self.repo.branch('main')
        self.repo.checkout('dev')
        self.repo.change('mappings/components.json')
        code, out, _ = self.repo.run('--base', 'main', '--release')
        self.assertEqual(code, 1)
        self.assertIn('a release pull request (into main)', out)

    def test_release_with_the_bump_passes(self):
        self.repo.branch('main')
        self.repo.checkout('dev')
        self.repo.change('mappings/components.json')
        self.repo.bump('0.2.0', '0.0.2')
        self.assertEqual(self.repo.run('--base', 'main', '--release')[0], 0)

    def test_branch_from_before_the_bump_merged_after_it_counts(self):
        self.repo.branch('feature')
        self.repo.change('native/steam-cloud/bridge.c')
        self.repo.checkout('dev')
        self.repo.bump('0.2.0', '0.0.2')
        self.repo.merge('feature')
        code, out, _ = self.repo.run()
        self.assertEqual(code, 0)
        self.assertIn('native/steam-cloud/bridge.c', out)
        self.assertIn('since engine 0.0.2', out)

    def test_the_bump_is_where_the_value_changed_not_a_reformat(self):
        bump = self.repo.bump('0.2.0', '0.0.2')
        self.repo.change('native/mod-loader/loader.c')
        self.repo.write(ev.PROPS, PROPS.format(version='0.2.0', engine='0.0.2').replace(
            '<EngineVersion>0.0.2</EngineVersion>', '<EngineVersion> 0.0.2 </EngineVersion>'))
        self.repo.commit('Reformat')
        report = ev.build_report(self.repo.root, 'HEAD', None, False)
        self.assertEqual(report.bump, bump)
        self.assertEqual(report.changed, ['native/mod-loader/loader.c'])

    def test_a_missing_engine_version_is_an_error(self):
        self.repo.write(ev.PROPS, '<Project />')
        self.repo.commit('No version')
        code, _, err = self.repo.run()
        self.assertEqual(code, 2)
        self.assertIn('has no EngineVersion', err)

    def test_many_files_are_shortened(self):
        for index in range(ev.SHOWN_FILES + 3):
            self.repo.write(f'native/mod-loader/file{index:02}.c', 'int b;')
        self.repo.commit('Many')
        _, out, _ = self.repo.run()
        self.assertIn('and 3 more', out)


if __name__ == '__main__':
    unittest.main()
