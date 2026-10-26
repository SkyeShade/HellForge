"""Engine version check: did the engine's sources change since the commit that set EngineVersion?

  py -3 scripts/engine-version/engine_version.py                        # the engine version and what changed since
  py -3 scripts/engine-version/engine_version.py --base origin/dev      # a pull request's check (CI runs this)
  py -3 scripts/engine-version/engine_version.py --base origin/main --release

(`python3` instead of `py -3` on Linux and macOS.) The engine (scripts/Build-Engine.ps1's package: the session runner,
the local backend, the mod loader, the stand-ins and the scripts it ships) has its own version, EngineVersion in
Directory.Build.props, bumped by hand in the version-bump pull request before each test build or release. This script
finds the commit that set the current EngineVersion and lists the engine sources (ENGINE_SOURCES below) that differ
between it and HEAD: an engine built now from them would ship under that old version.

What it does with them depends on the pull request:
- an ordinary pull request (into dev): a warning when it changes the engine itself, a notice when only earlier merges
  did. It never fails: the bump comes with the next build.
- a version-bump pull request (one that changes HellForge's Version) or a release (--release, a pull request into
  main): an error, and exit code 1, when the engine changed and EngineVersion did not.
Inside GitHub Actions the findings are workflow annotations and a job summary. Exit codes: 0 OK (warnings and notices
allowed); 1 the engine changed without a bump, in a version-bump or release pull request; 2 a git or parse error.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

PROPS = 'Directory.Build.props'
# What goes into the engine package, from scripts/Build-Engine.ps1 and the projects it publishes: a folder ends with /.
# tests/test_engine_version.py checks that every repository path Build-Engine.ps1 and those projects name is either
# here or in BUILD_ONLY.
ENGINE_SOURCES = (
    # native code: the mod loader with the mod SDK's headers and its common header, the PlayFab and Steam stand-ins,
    # the Steam bridge and broker, the network block
    'native/common/',
    'native/mod-loader/',
    'native/mod-sdk/',
    'native/network-block/',
    'native/sdk-shims/',
    'native/steam-cloud/',
    # the session runner and the local backend (NativeAOT), the Core library both are built with, and the generator
    # that writes the stand-ins' export tables
    'src/HellForge.Core/',
    'src/HellForge.LocalBackend/',
    'src/HellForge.SdkShimGen/',
    'src/HellForge.Session/',
    # the loader's field and content tables are generated from the mappings; Core and the backend embed data files
    'mappings/',
    'data/profile-templates.json',
    'data/progression-data.json',
    'data/verified-mods.json',
    # the build and its pins, and the scripts the package ships
    'scripts/Build-Engine.ps1',
    'scripts/Get-BuildTools.ps1',
    'scripts/Install-NetworkBlock.ps1',
    'scripts/Install-WfpBlock.ps1',
    'scripts/NetworkBlock.Common.ps1',
    'scripts/OfflineControlNetworkProbe.cs',
    'scripts/Test-SteamOwnership.ps1',
    'LICENSE',
    'LICENSE-EXCEPTION',
)
# Named by the engine's build but not part of what it ships: the version itself, the tool that runs the table
# generator (its code is in Core), the build's smoke tests and their example mods.
BUILD_ONLY = (
    PROPS,
    'src/HellForge.Cli/',
    'scripts/Build-Mod.ps1',
    'scripts/Test-BackendAot.ps1',
    'examples/',
)
# Never engine sources, wherever they are: tests (native test/ folders) and documentation.
TEST_FOLDERS = frozenset({'test', 'tests'})
DOC_SUFFIXES = ('.md',)
SHOWN_FILES = 10
BUMP_HELP = 'Bump EngineVersion in Directory.Build.props (CONTRIBUTING.md, "The engine\'s version").'
LATER_HELP = ('No bump needed here: say "engine bump with the next build" in the description, and the next '
              'version-bump pull request bumps it (CONTRIBUTING.md, "The engine\'s version").')

ENGINE_VERSION_RE = re.compile(r'<EngineVersion>\s*([^<\s]+)\s*</EngineVersion>')
VERSION_RE = re.compile(r'<Version>\s*([^<\s]+)\s*</Version>')


class CheckError(Exception):
    pass


def is_engine_source(path: str) -> bool:
    """True for a repository path (forward slashes) whose change changes the engine package."""
    parts = path.split('/')
    if TEST_FOLDERS.intersection(parts[:-1]) or path.lower().endswith(DOC_SUFFIXES):
        return False
    return any(path == entry or (entry.endswith('/') and path.startswith(entry)) for entry in ENGINE_SOURCES)


def git(repo: Path, *args: str, check: bool = True) -> str:
    result = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, encoding='utf-8')
    if check and result.returncode != 0:
        raise CheckError(f'git {" ".join(args)} failed: {result.stderr.strip()}')
    return result.stdout


def props_at(repo: Path, commit: str) -> str | None:
    """Directory.Build.props at a commit, or None where it does not exist."""
    result = subprocess.run(['git', '-C', str(repo), 'show', f'{commit}:{PROPS}'], capture_output=True, text=True,
                            encoding='utf-8')
    return result.stdout if result.returncode == 0 else None


def value(pattern: re.Pattern[str], text: str | None) -> str | None:
    match = pattern.search(text) if text is not None else None
    return match.group(1) if match else None


def version_line(repo: Path) -> int:
    """The line of EngineVersion in the working tree's Directory.Build.props, for annotations (1 if not found)."""
    try:
        lines = (repo / PROPS).read_text(encoding='utf-8').splitlines()
    except OSError:
        return 1
    return next((number for number, line in enumerate(lines, 1) if '<EngineVersion>' in line), 1)


def find_bump(repo: Path, head: str, version: str) -> str:
    """The commit that set EngineVersion to `version`: the newest one reachable from `head` where it became that."""
    for commit in git(repo, 'log', '--format=%H', '-G', '<EngineVersion>', head, '--', PROPS).split():
        if value(ENGINE_VERSION_RE, props_at(repo, commit)) != version:
            continue
        parents = git(repo, 'rev-list', '--parents', '-n', '1', commit).split()[1:]
        if all(value(ENGINE_VERSION_RE, props_at(repo, parent)) != version for parent in parents):
            return commit
    raise CheckError(f'No commit before {head} sets EngineVersion {version} in {PROPS}.')


def changed_engine_files(repo: Path, since: str, until: str) -> list[str]:
    names = git(repo, 'diff', '--name-only', '--no-renames', since, until).splitlines()
    return sorted(name for name in names if is_engine_source(name))


@dataclass
class Report:
    engine_version: str
    bump: str
    bump_subject: str
    changed: list[str]                      # engine sources that differ between the bump and HEAD
    in_change: list[str] = field(default_factory=list)  # of those, the ones this pull request changes
    strict: bool = False
    reason: str = ''                        # why the check is strict


def build_report(repo: Path, head: str, base: str | None, release: bool) -> Report:
    head_props = props_at(repo, head)
    engine_version = value(ENGINE_VERSION_RE, head_props)
    if engine_version is None:
        raise CheckError(f'{PROPS} at {head} has no EngineVersion.')
    bump = find_bump(repo, head, engine_version)
    report = Report(engine_version, bump, git(repo, 'log', '-1', '--format=%s', bump).strip(),
                    changed_engine_files(repo, bump, head))
    if base is None:
        return report
    merge_base = git(repo, 'merge-base', base, head).strip()
    report.in_change = sorted(set(report.changed) & set(changed_engine_files(repo, merge_base, head)))
    old_version = value(VERSION_RE, props_at(repo, merge_base))
    new_version = value(VERSION_RE, head_props)
    if release:
        report.strict, report.reason = True, 'a release pull request (into main)'
    elif old_version != new_version:
        report.strict, report.reason = True, f'a version-bump pull request (HellForge {old_version} to {new_version})'
    return report


def file_list(files: list[str]) -> str:
    shown = ', '.join(files[:SHOWN_FILES])
    return shown + (f' and {len(files) - SHOWN_FILES} more' if len(files) > SHOWN_FILES else '')


def verdict(report: Report) -> tuple[str, str]:
    """(level, message): level is ok, notice, warning or error."""
    since = f'engine {report.engine_version} (set in {report.bump[:7]}, "{report.bump_subject.split(":")[0][:80]}")'
    if not report.changed:
        return 'ok', f'The engine is unchanged since {since}.'
    count = f'{len(report.changed)} engine source file{"s" if len(report.changed) != 1 else ""}'
    if report.strict:
        return 'error', (f'This is {report.reason}, and {count} changed since {since} without a bump: '
                         f'{file_list(report.changed)}. {BUMP_HELP}')
    if report.in_change:
        return 'warning', (f'This pull request changes the engine ({file_list(report.in_change)}); EngineVersion is '
                           f'still {report.engine_version}. {LATER_HELP}')
    return 'notice', (f'{count} changed since {since}, in earlier merges: {file_list(report.changed)}. The next build '
                      f'bumps EngineVersion.')


def escape(text: str) -> str:
    """A workflow command's message: %, CR and LF are escaped (GitHub's toolkit does the same)."""
    return text.replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')


def emit(repo: Path, level: str, message: str) -> None:
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        if level != 'ok':
            print(f'::{level} file={PROPS},line={version_line(repo)},title=Engine version::{escape(message)}')
        summary = os.environ.get('GITHUB_STEP_SUMMARY')
        if summary:
            with open(summary, 'a', encoding='utf-8') as out:
                out.write(f'### Engine version\n\n{message}\n')
    print(f'{level}: {message}' if level != 'ok' else message)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parent.parent.parent,
                        help='the repository (default: this one)')
    parser.add_argument('--head', default='HEAD', help='the commit checked (default: HEAD)')
    parser.add_argument('--base', help='a pull request\'s base branch, e.g. origin/dev: separates its own changes and '
                                       'finds a version bump')
    parser.add_argument('--release', action='store_true', help='a release pull request (into main): always strict')
    args = parser.parse_args(argv)
    try:
        report = build_report(args.repo, args.head, args.base, args.release)
    except CheckError as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    level, message = verdict(report)
    emit(args.repo, level, message)
    return 1 if level == 'error' else 0


if __name__ == '__main__':
    sys.exit(main())
