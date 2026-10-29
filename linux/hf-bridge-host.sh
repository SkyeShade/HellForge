#!/usr/bin/env bash
# From DreamyBranch's LinuxSupport (SkyeShade/HellForge issue 1), with joshua-software-dev; docs/linux.md.
# Run this OUTSIDE the offline wrapper, after Steam is running and online. Leave it running.
# It exposes each loopback TCP port that the Steam client listens on as a unix socket in
# $XDG_RUNTIME_DIR. Unix sockets are files, so they cross the network namespace; hf-inner.sh
# then recreates the same loopback ports inside the namespace. No route to the internet is added.
# The relay itself is HellForge's own native program (app/cli-linux/HellForge.Cli steam-bridge host),
# so socat is not needed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The installation's native CLI, as hellforge-linux.sh and hf-inner.sh use; beside this script when INSTALL is not set
# (the bridge started by hand).
if [ -n "${INSTALL:-}" ]; then cli="$INSTALL/app/cli-linux/HellForge.Cli"; else cli="$here/../cli-linux/HellForge.Cli"; fi
[ -f "$cli" ] || { echo "The native Linux build agent is missing ($cli): reinstall HellForge. The Steam bridge needs it." >&2; exit 1; }
[ -x "$cli" ] || chmod +x "$cli" 2>/dev/null || true
[ -x "$cli" ] || { echo "The native Linux build agent is not executable: $cli" >&2; exit 1; }
exec "$cli" steam-bridge host
