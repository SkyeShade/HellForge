"""Nightly builds (.github/workflows/nightly.yml, CONTRIBUTING.md "Nightly builds"): a nightly's version and its notes.

  python scripts/nightly/nightly.py version --date 20261102
      The version a nightly carries, the next release's number with -nightly.<date>: "0.4.0-nightly.20261102". The next
      release's number is Version in Directory.Build.props without its label ("0.4.0-dev.3" gives 0.4.0), or the next
      minor version once that Version is released (its tag v<Version> exists: 0.3.0-beta gives 0.4.0). The build appends
      the commit, so the launcher shows "HellForge 0.4.0-nightly.20261102 (e092804)".

  python scripts/nightly/nightly.py notes --version V --commit SHA [--previous SHA] --repository OWNER/NAME
      The nightly pre-release's notes, as Markdown on standard output: that it is an untested development build, the
      version and commit, how it differs from a release, the pull requests and commits since the previous nightly
      (--previous, the commit the nightly tag is on) or, for the first one, since the last release, and CHANGELOG.md's
      Unreleased section.

Run from anywhere: it reads the repository it is in (--repo to read another). It needs Python 3.10 or newer and git,
nothing else. Its tests: python -m unittest discover -s scripts/nightly/tests -v
"""
from __future__ import annotations

import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROPS = 'Directory.Build.props'
CHANGELOG = 'CHANGELOG.md'
# as scripts/engine-version/engine_version.py reads it: the first <Version> element, without attributes
VERSION_RE = re.compile(r'<Version>\s*([^<\s]+)\s*</Version>')
CORE_RE = re.compile(r'(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.+-]*)?')
MERGE_RE = re.compile(r'Merge pull request #(\d+) from \S+')
DATE_RE = re.compile(r'\d{8}')
# the pull requests and commits listed in the notes; the rest are behind the comparison link
MAX_CHANGES = 60
# GitHub takes release notes of up to 125,000 characters
MAX_NOTES = 120_000


class NightlyError(Exception):
    pass


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, encoding='utf-8')
    if result.returncode != 0:
        raise NightlyError(f'git {" ".join(args)} failed: {result.stderr.strip()}')
    return result.stdout


def git_ok(repo: Path, *args: str) -> bool:
    return subprocess.run(['git', '-C', str(repo), *args], capture_output=True).returncode == 0


def next_release(version: str, released: bool) -> str:
    """The next release's major.minor.patch: the version's own, or the next minor version once it is released."""
    match = CORE_RE.fullmatch(version)
    if not match:
        raise NightlyError(f'{PROPS} has a Version that does not start with major.minor.patch: {version!r}')
    major, minor, patch = (int(part) for part in match.groups())
    return f'{major}.{minor + 1}.0' if released else f'{major}.{minor}.{patch}'


def real_date(date: str) -> bool:
    try:
        datetime.datetime.strptime(date, '%Y%m%d')
    except ValueError:
        return False
    return True


def nightly_version(repo: Path, date: str) -> str:
    if not DATE_RE.fullmatch(date) or not real_date(date):
        raise NightlyError(f'--date takes a date as YYYYMMDD, not {date!r}')
    props = (repo / PROPS).read_text(encoding='utf-8')
    match = VERSION_RE.search(props)
    if not match:
        raise NightlyError(f'{PROPS} has no <Version>.')
    version = match.group(1)
    released = git_ok(repo, 'rev-parse', '--quiet', '--verify', f'refs/tags/v{version}')
    return f'{next_release(version, released)}-nightly.{date}'


def unreleased(changelog: str) -> str | None:
    """CHANGELOG.md's Unreleased section without its heading, its ### headings made ####; None if it has none."""
    lines = changelog.splitlines()
    start = next((index for index, line in enumerate(lines) if line.strip() == '## Unreleased'), None)
    if start is None:
        return None
    end = next((index for index in range(start + 1, len(lines)) if lines[index].startswith('## ')), len(lines))
    body = '\n'.join('#' + line if line.startswith('### ') else line for line in lines[start + 1:end]).strip()
    return body or None


def changes(repo: Path, base: str, commit: str) -> list[str]:
    """dev's own history from base to commit, newest first: a pull request's merge as "#N: its title", any other
    commit as "its subject (short hash)"."""
    log = git(repo, 'log', '--first-parent', '--format=%H%x00%s%x00%b%x1e', f'{base}..{commit}')
    lines = []
    for record in log.split('\x1e'):
        record = record.strip('\n')
        if not record:
            continue
        sha, subject, body = record.split('\x00', 2)
        merge = MERGE_RE.fullmatch(subject.strip())
        if merge:
            title = next((line.strip() for line in body.splitlines() if line.strip()), '')
            lines.append(f'- #{merge.group(1)}: {title}' if title else f'- #{merge.group(1)}')
        else:
            lines.append(f'- {subject.strip()} ({sha[:7]})')
    return lines


def last_release(repo: Path, commit: str) -> str | None:
    """The newest release tag (v...) that commit contains, or None."""
    result = subprocess.run(['git', '-C', str(repo), 'describe', '--tags', '--abbrev=0', '--match', 'v*', commit],
                            capture_output=True, text=True, encoding='utf-8')
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def change_section(repo: Path, commit: str, previous: str | None, release: str | None, url: str) -> list[str]:
    if previous and git_ok(repo, 'merge-base', '--is-ancestor', previous, commit):
        heading, base = 'Changes since the previous nightly', previous
    elif release:
        heading, base = f'Changes since {release}', release
    else:
        return []
    listed = changes(repo, base, commit)
    shown_base = base if base == release else base[:7]
    out = [f'### {heading}', '', f'[`{shown_base}...{commit[:7]}`]({url}/compare/{base}...{commit})', '']
    if not listed:
        out.append('Nothing new on `dev`: this build was started by hand.')
    out += listed[:MAX_CHANGES]
    if len(listed) > MAX_CHANGES:
        out.append(f'- and {len(listed) - MAX_CHANGES} more: see the comparison above.')
    return out + ['']


def notes(repo: Path, version: str, commit: str, previous: str | None, repository: str) -> str:
    url = f'https://github.com/{repository}'
    short = commit[:7]
    release = last_release(repo, commit)
    out = [
        '> [!WARNING]',
        '> **An untested development build.** It is made automatically from the `dev` branch and has not been tried in',
        '> game. It can break your installation, profiles or mods, and the next nightly replaces it. To play, install a',
        f'> numbered release from the [Releases page]({url}/releases) instead.',
        '',
        f'**HellForge `{version}`**, from [`{short}`]({url}/commit/{commit}) on `dev`. The launcher\'s corner shows',
        f'`HellForge {version} ({short})`: name it in every bug report.',
        '',
        '- Download `HellForge-Nightly-Setup.exe` and run it. Install it into a folder of its own, not over the',
        '  installation you play on: a newer build can change your profiles in ways an older one does not read.',
        '- Co-op needs everyone on the same version: play with players on this same nightly.',
        '- `HellForge-Nightly-Linux-scripts.zip` has the Linux (Proton) scripts and their guide, as a release has them.',
        '- `HellForge-Nightly-SHA256SUMS.txt` has every file\'s SHA-256.',
        '',
        '### How it differs from a release',
        '',
        '- It is built with the release\'s own package script, on GitHub\'s Windows build machines, which have no game',
        '  files and no Valve Steamworks SDK. So it has no Steam bridge or broker: on Windows the launcher does not offer',
        '  the sandbox (Settings > Sandbox), and the game runs unsandboxed.',
        '- Nobody has tested it in game before it was published.',
        '',
    ]
    out += change_section(repo, commit, previous, release, url)
    changelog_path = repo / CHANGELOG
    section = unreleased(changelog_path.read_text(encoding='utf-8')) if changelog_path.is_file() else None
    if section:
        since = f' since {release}' if release else ''
        out += [f'### What\'s new{since} ([CHANGELOG.md]({url}/blob/{commit}/{CHANGELOG}), Unreleased)', '', section, '']
    footer = ['---', '',
              f'This pre-release is rebuilt every night that `dev` changed, and its `nightly` tag moves to the commit it '
              f'was built from ([CONTRIBUTING.md]({url}/blob/dev/CONTRIBUTING.md#nightly-builds)).']
    text = '\n'.join(out + footer) + '\n'
    if len(text) > MAX_NOTES:
        cut = '\n'.join(out)[:MAX_NOTES - 2_000].rsplit('\n', 1)[0]
        text = '\n'.join([cut, '', f'(Cut short: the rest is in [CHANGELOG.md]({url}/blob/{commit}/{CHANGELOG}).)', '',
                          *footer]) + '\n'
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='A nightly build\'s version and release notes.')
    parser.add_argument('--repo', type=Path, default=ROOT, help='the repository (default: the one this script is in)')
    commands = parser.add_subparsers(dest='command', required=True)
    version = commands.add_parser('version', help='the nightly version of the checked-out Directory.Build.props')
    version.add_argument('--date', required=True, help='the build date, YYYYMMDD (UTC)')
    notes_parser = commands.add_parser('notes', help='the release notes, as Markdown')
    notes_parser.add_argument('--version', required=True)
    notes_parser.add_argument('--commit', required=True, help='the full hash of the commit built')
    notes_parser.add_argument('--previous', default='', help='the commit the previous nightly was built from, if any')
    notes_parser.add_argument('--repository', required=True, help='OWNER/NAME on GitHub')
    args = parser.parse_args(argv)
    try:
        if args.command == 'version':
            print(nightly_version(args.repo, args.date))
        else:
            sys.stdout.write(notes(args.repo, args.version, args.commit, args.previous or None, args.repository))
    except (NightlyError, OSError) as error:
        print(f'nightly.py: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
