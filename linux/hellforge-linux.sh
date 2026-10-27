#!/usr/bin/env bash
# Starts HellForge on Linux (Proton), with no way out to the internet: docs/linux.md. One command, with Steam running and
# signed in: bash hellforge-linux.sh (or ./hellforge-linux.sh once it is executable)
# Another launcher (docs/external-launchers.md) passes the Windows program to run in place of HellForge.exe, with its
# arguments, e.g. ./hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes
# What it does: checks the programs it needs, finds Steam, the prefix and Proton, then bridges Steam's local ports
# (hf-bridge-host.sh, unless one is running already), fetches the live galactic war while this PC still has its network
# (the session that follows has none and serves this copy), and starts the requested program inside a network namespace
# with loopback only (hellforge-offline.sh), where hf-inner.sh forwards Steam's ports and runs it through Proton.
# The bridge it started stops when the wrapped program and its session processes close.
# When started as the launcher, it also prepares mod tools once; when called with a program and arguments it runs that
# request through Proton. The bridge it started stops when the wrapped program and its session processes close.
#
# Settings (environment variables; each has a default that is found):
#   PROTON                            the proton script to run HellForge with (default: one the Steam client has, the last
#                                     by version sort; it says which, and which others it found)
#   INSTALL                           the HellForge installation (default: the one this script is in, app/linux)
#   STEAM_COMPAT_CLIENT_INSTALL_PATH  the Steam client (default: the first with a steamapps folder of ~/.local/share/Steam,
#                                     ~/.steam/debian-installation, ~/.steam/steam, Flatpak's and Snap's)
#   STEAM_COMPAT_DATA_PATH            the Wine prefix (default: the prefix the installation is in, <prefix>/pfx/drive_c/...;
#                                     else HELLDIVERS 2's, steamapps/compatdata/553850)
#
# Linux support is the work of DreamyBranch and joshua-software-dev (SkyeShade/HellForge issue 1). The dependency hints,
# the Steam folder search and the prefix found from the installation follow DreamyBranch's guided
# install (issue 11).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# A native GUI launcher has no terminal to show wrapper/Proton output. Keep the latest launch diagnostics in the
# installation so failures before HellForge.Session creates its own run folder are still visible to the player.
if [ -n "${INSTALL:-}" ] && [ -f "$INSTALL/hellforge-install.json" ]; then
  if mkdir -p "$INSTALL/logs"; then
    : > "$INSTALL/logs/linux-session-wrapper.log"
    exec >> "$INSTALL/logs/linux-session-wrapper.log" 2>&1
  else
    echo "Could not create the Linux session log folder: $INSTALL/logs" >&2
  fi
fi

# The programs these scripts run, with the install command for the package manager this system has.
missing=""
for program in socat ip ss unshare pgrep; do command -v "$program" >/dev/null || missing="$missing $program"; done
if [ -n "$missing" ]; then
  echo "HellForge on Linux needs these programs, which were not found:$missing" >&2
  if command -v pacman >/dev/null; then echo "Install them with: sudo pacman -S --needed iproute2 socat util-linux procps-ng" >&2
  elif command -v apt-get >/dev/null; then echo "Install them with: sudo apt install iproute2 socat util-linux procps" >&2
  elif command -v dnf >/dev/null; then echo "Install them with: sudo dnf install iproute socat util-linux procps-ng" >&2
  elif command -v zypper >/dev/null; then echo "Install them with: sudo zypper install iproute2 socat util-linux procps" >&2
  else echo "Install them with your package manager: ip and ss are in iproute2 (iproute on Fedora), unshare in util-linux, pgrep in procps, and socat in socat." >&2
  fi
  exit 1
fi

if [ -z "${INSTALL:-}" ] && [ -f "$here/../../hellforge-install.json" ]; then INSTALL="$(cd "$here/../.." && pwd)"; fi
: "${INSTALL:?INSTALL is not set: the HellForge installation folder (the one with hellforge-install.json)}"
[ -f "$INSTALL/hellforge-install.json" ] || { echo "$INSTALL is not a HellForge installation (no hellforge-install.json)" >&2; exit 1; }
export INSTALL

# The Steam client: the usual native folders first, then Flatpak's and Snap's (those two are untested).
if [ -z "${STEAM_COMPAT_CLIENT_INSTALL_PATH:-}" ]; then
  for steam in "$HOME/.local/share/Steam" "$HOME/.steam/debian-installation" "$HOME/.steam/steam" \
               "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
    if [ -d "$steam/steamapps" ]; then STEAM_COMPAT_CLIENT_INSTALL_PATH="$(cd "$steam" && pwd -P)"; break; fi
  done
fi
: "${STEAM_COMPAT_CLIENT_INSTALL_PATH:?Steam was not found (no steamapps folder in ~/.local/share/Steam, ~/.steam or Flatpak's or Snap's Steam folder): set STEAM_COMPAT_CLIENT_INSTALL_PATH to the folder that holds steamapps}"
export STEAM_COMPAT_CLIENT_INSTALL_PATH

# The prefix: installations inside a Proton prefix are meant to use that prefix.
inside=""
case "$INSTALL" in
  */pfx/drive_c/*) inside="${INSTALL%%/pfx/drive_c/*}" ;;
  # Wine shows the Linux folders as its Z: drive, which HellForge refuses as a linked path (issue 1)
  *) echo "Note: $INSTALL is not inside a Wine prefix's C: drive (<prefix>/pfx/drive_c/...); HellForge may refuse it as a linked path (docs/linux.md)." >&2 ;;
esac
if [ -z "${STEAM_COMPAT_DATA_PATH:-}" ]; then
  STEAM_COMPAT_DATA_PATH="${inside:-$STEAM_COMPAT_CLIENT_INSTALL_PATH/steamapps/compatdata/553850}"
elif [ -n "$inside" ] && ! [ "$inside" -ef "$STEAM_COMPAT_DATA_PATH" ]; then
  echo "Note: the installation is in the prefix $inside, but STEAM_COMPAT_DATA_PATH is $STEAM_COMPAT_DATA_PATH." >&2
fi
export STEAM_COMPAT_DATA_PATH

if [ -z "${PROTON:-}" ]; then
  mapfile -t protons < <(ls -d "$STEAM_COMPAT_CLIENT_INSTALL_PATH"/steamapps/common/Proton*/proton "$STEAM_COMPAT_CLIENT_INSTALL_PATH"/compatibilitytools.d/*/proton 2>/dev/null | sort -V)
  if [ "${#protons[@]}" -gt 0 ]; then PROTON="${protons[-1]}"; fi
  if [ "${#protons[@]}" -gt 1 ]; then
    echo "Proton: using $PROTON. Also found (set PROTON to use one of them):"
    printf '  %s\n' "${protons[@]:0:${#protons[@]}-1}"
  fi
fi
: "${PROTON:?PROTON is not set and no Proton was found in Steam: install one in Steam, or set PROTON to a proton script, e.g. .../compatibilitytools.d/<name>/proton}"
[ -x "$PROTON" ] || { echo "PROTON is not a proton script that can be run: $PROTON" >&2; exit 1; }
export PROTON
: "${STEAM_COMPAT_APP_ID:=553850}"
export STEAM_COMPAT_APP_ID
echo "HellForge: $INSTALL with $PROTON (prefix $STEAM_COMPAT_DATA_PATH)"
echo "Steam compatibility app ID: $STEAM_COMPAT_APP_ID"

# Where only allowed programs may make a user namespace (Ubuntu 24.04 and later), say so now rather than after the war
# fetch; hellforge-offline.sh checks the same.
if [ ! -x /usr/local/lib/hellforge/unshare ] && [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)" = 1 ]; then
  echo "This system only lets allowed programs make the network namespace HellForge runs in. Run this once, then start HellForge again:" >&2
  echo "  sudo bash $here/allow-namespace.sh   (docs/linux.md)" >&2
  exit 1
fi

# Steam's bridge: the one running, else one of our own for as long as HellForge runs.
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
bridge=""
cleanup() {
  local status=$?
  echo "HellForge wrapper exiting with code $status."
  if [ -n "$bridge" ]; then kill "$bridge" 2>/dev/null || true; wait "$bridge" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'echo "HellForge wrapper received SIGINT."; exit 130' INT
trap 'echo "HellForge wrapper received SIGTERM."; exit 143' TERM
# a bridge answers on its sockets (ones left by a bridge that was killed do not)
bridged() { for s in "$dir"/hf-steam-*.sock; do [ -S "$s" ] && socat -u OPEN:/dev/null UNIX-CONNECT:"$s" 2>/dev/null && return 0; done; return 1; }
if ! bridged; then
  bash "$here/hf-bridge-host.sh" &
  bridge=$!
  for _ in $(seq 1 20); do bridged && break; kill -0 "$bridge" 2>/dev/null || break; sleep 0.5; done
fi
bridged || { echo "Steam's local ports could not be bridged: is Steam running and signed in?" >&2; exit 1; }

# The live war, now, while there is a network (Wine sees / as Z:). A failure is not fatal: the session then serves the
# last war fetched, or HellForge's built-in one. The ownership probe does not need the war fetch and skips it so the
# probe's bounded timeout covers only starting Proton and checking Steam.
if [ "${HELLFORGE_SKIP_LIVE_WAR:-}" != 1 ]; then
  backend="$INSTALL/app/backend/HellForge.LocalBackend.exe"
  cache="Z:$(printf '%s' "$INSTALL/live-war.json" | tr '/' '\\')"
  if timeout 60 "$PROTON" run "$backend" fetch-live-war "$cache"; then echo "Fetched the live galactic war."
  else echo "Could not fetch the live galactic war; the session serves the last one fetched (or the built-in war)." >&2; fi
  # (proton can return before a Windows program ends: the fetch finishes before the namespace is entered)
  for _ in $(seq 1 30); do pgrep -f "LocalBackend.exe fetch-live-war" >/dev/null || break; sleep 1; done
fi

# The mod tools (HellForge's pinned Zig, about 90 MB from ziglang.org), once, also now: source mods are compiled when a
# session starts, inside the namespace, where the download is refused (issue 20). Fetched again only when the pin changes.
# Not fatal either: without them source mods stay off.
pin="$(grep '"name": "zig"' "$INSTALL/app/sdk/tools.json" 2>/dev/null | sed -E 's/.*"sha256": "([0-9A-Fa-f]+)".*/\1/' || true)"
zigReady() { [ -f "$INSTALL/tools/zig/zig.exe" ] && [ "$(cat "$INSTALL/tools/zig/.hellforge-archive-sha256" 2>/dev/null)" = "$pin" ]; }
if [ -n "$pin" ] && ! zigReady; then
  if [ "${HELLFORGE_SKIP_MOD_TOOLS:-}" = 1 ]; then
    echo "Skipping the mod-tools download for the Steam ownership check."
  else
    echo "Fetching the mod tools (Zig, about 90 MB, once) to build source mods..."
    root="Z:$(printf '%s' "$INSTALL" | tr '/' '\\')"
    timeout 900 "$PROTON" run "$INSTALL/app/cli/HellForge.Cli.exe" mod-tools --install "$root" --ide no >/dev/null || true
    for _ in $(seq 1 900); do pgrep -f "HellForge.Cli.exe mod-tools" >/dev/null || break; sleep 1; done
    if zigReady; then echo "Fetched the mod tools."
    else echo "Could not fetch the mod tools; source mods stay off until they are (start HellForge again to retry)." >&2; fi
  fi
fi

# Setup calls the wrapper after Steam confirms ownership to prepare the optional tools without launching a session.
if [ "${HELLFORGE_PREPARE_MOD_TOOLS_ONLY:-}" = 1 ]; then
  if [ -n "$pin" ] && zigReady; then
    echo "Mod tools are ready."
    exit 0
  fi
  echo "Could not prepare the pinned mod tools." >&2
  exit 1
fi

# Run through bash, so the scripts work without their executable bit (lost when copied from Windows or unpacked by setup).
echo "Entering the offline network namespace."
if bash "$here/hellforge-offline.sh" bash "$here/hf-inner.sh" "$@"; then
  echo "Offline namespace command completed."
else
  status=$?
  echo "Offline namespace command exited with code $status." >&2
  exit "$status"
fi
