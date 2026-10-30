#!/usr/bin/env bash
# Runs HellForge's cross-platform checks on Linux: builds the launcher, CLI and tests in Release (warnings are errors),
# runs HellForge.Tests and HellForge.LocalBackend.Tests, the structure lint and bash -n on linux/*.sh. Test temporary
# files stay in artifacts/, except the local backend's run, which needs a short /tmp folder. The tests that need Windows
# skip themselves; the solution's Windows-only projects and Test.ps1's NTFS junction fixture checks are skipped below.
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"

# Test.ps1 keeps its fixtures there, and the tests create their own roots under artifacts/.
mkdir -p artifacts/tmp
export TMPDIR="$project_root/artifacts/tmp"

# HellForge.slnx also holds Windows-only projects, so build the projects that build on Linux, as the Linux CI job does.
dotnet build src/HellForge.Launcher -c Release
dotnet build src/HellForge.Cli -c Release
dotnet build tests/HellForge.Tests -c Release
dotnet run --project tests/HellForge.Tests -c Release --no-build
dotnet build tests/HellForge.LocalBackend.Tests -c Release
# Unix socket paths are limited to 108 characters, so this test run gets a short temp folder instead of artifacts/tmp.
backend_tmp="$(mktemp -d /tmp/hellforge-backend-tests.XXXXXX)"
trap 'rm -rf "$backend_tmp"' EXIT
TMPDIR="$backend_tmp" dotnet run --project tests/HellForge.LocalBackend.Tests -c Release --no-build

# The CLI's strangers setting: yes/no store the choice, default removes it, and the JSON prints the effective value and
# the nullable choice (M.5). A marker-only installation is enough for settings; no session starts.
cli_root="$(mktemp -d "$project_root/artifacts/tmp/cli-strangers.XXXXXX")"
echo '{}' > "$cli_root/hellforge-install.json"
cli_settings() { dotnet run --project src/HellForge.Cli -c Release --no-build -- settings --install "$cli_root" "$@"; }
yes_json="$(cli_settings --sandbox no --strangers yes)"
grep -Fq -- '"playWithStrangers": true' <<<"$yes_json"
grep -Fq -- '"playWithStrangersChosen": true' <<<"$yes_json"
no_json="$(cli_settings --sandbox no --strangers no)"
grep -Fq -- '"playWithStrangers": false' <<<"$no_json"
grep -Fq -- '"playWithStrangersChosen": false' <<<"$no_json"
default_json="$(cli_settings --sandbox no --strangers default)"
grep -Fq -- '"playWithStrangers": false' <<<"$default_json"
grep -Fq -- '"playWithStrangersChosen": null' <<<"$default_json"
! grep -q '"strangers":' "$cli_root/mods/hellforge-mods.json"
! grep -q playWithStrangers "$cli_root/mods/hellforge-mods.json"

# Test.ps1's fixture checks make NTFS junctions, so they need Windows and are skipped.
python3 scripts/lint/lint.py check --warnings-as-errors

for script in linux/*.sh; do
  bash -n "$script"
done

echo "All checks passed."
