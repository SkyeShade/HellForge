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

# Test.ps1's fixture checks make NTFS junctions, so they need Windows and are skipped.
python3 scripts/lint/lint.py check

for script in linux/*.sh; do
  bash -n "$script"
done

echo "All checks passed."
