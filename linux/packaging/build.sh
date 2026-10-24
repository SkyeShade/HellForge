#!/usr/bin/env bash
# Builds the native Linux launcher (docs/linux.md, "Native launcher") and the Arch package. Needs a .NET 10 SDK: DOTNET=<dir with dotnet> or on PATH.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="${DOTNET:-$HOME/.dotnet-sdk}:$PATH" DOTNET_ROOT="${DOTNET:-$HOME/.dotnet-sdk}" DOTNET_CLI_TELEMETRY_OPTOUT=1
out="$here/dist/hellforge"; rm -rf "$here/dist"; mkdir -p "$out/linux"
dotnet publish "$here/../../src/HellForge.Launcher/HellForge.Launcher.csproj" -c Release -r linux-x64 --self-contained -o "$out"
cp "$here"/../*.sh "$out/linux/"
install -Dm755 "$here/hellforge" "$here/dist/usr/bin/hellforge"
install -Dm644 "$here/hellforge.desktop" "$here/dist/usr/share/applications/hellforge.desktop"
install -Dm644 "$here/../../src/HellForge.Launcher/Assets/hellforge-icon.png" "$here/dist/usr/share/icons/hicolor/256x256/apps/hellforge.png"
tar -C "$here/dist" -czf "$here/hellforge-linux-x64.tar.gz" hellforge usr
echo "Built $here/hellforge-linux-x64.tar.gz"
if [ "${1:-}" = pkg ]; then (cd "$here" && makepkg -f --nodeps); fi
