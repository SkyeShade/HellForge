#!/usr/bin/env bash
# Starts HellForge on Linux (Proton), with no way out to the internet: docs/linux.md. One command, with Steam running and
# signed in: bash hellforge-linux.sh (or ./hellforge-linux.sh once it is executable)
# Another launcher (docs/external-launchers.md) passes the Windows program to run in place of HellForge.exe, with its
# arguments, e.g. ./hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes
# What it does: checks the programs it needs, finds Steam, the prefix and Proton, then bridges Steam's local ports
# (hf-bridge-host.sh, unless one is running already), fetches the live galactic war while this PC still has its network
# (the session that follows has none and serves this copy), and starts the requested program inside a network namespace
# with loopback only (hellforge-offline.sh), where hf-inner.sh forwards Steam's ports and runs it through Proton.
# A bridge it starts is stopped by the last wrapper using it; one started by hand is left alone.
# When started as the launcher, it also prepares mod tools once; when called with a program and arguments it runs that
# request through Proton.
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
# installation so failures before HellForge.Session creates its own run folder are still visible to the player. Only
# the native launcher asks for this (HELLFORGE_WRAPPER_LOG=1, set by SessionRunner, the Steam ownership check and the
# setup's mod-tools step): an external launcher needs the command's own stdout and stderr, so they stay with the caller.
if [ "${HELLFORGE_WRAPPER_LOG:-}" = 1 ] && [ -n "${INSTALL:-}" ] && [ -f "$INSTALL/hellforge-install.json" ]; then
  if mkdir -p "$INSTALL/logs"; then
    : > "$INSTALL/logs/linux-session-wrapper.log"
    exec >> "$INSTALL/logs/linux-session-wrapper.log" 2>&1
  else
    echo "Could not create the Linux session log folder: $INSTALL/logs" >&2
  fi
fi

# The programs these scripts run, with the install command for the package manager this system has.
missing=""
for program in socat ip ss unshare pgrep flock; do command -v "$program" >/dev/null || missing="$missing $program"; done
if [ -n "$missing" ]; then
  echo "HellForge on Linux needs these programs, which were not found:$missing" >&2
  if command -v pacman >/dev/null; then echo "Install them with: sudo pacman -S --needed iproute2 socat util-linux procps-ng" >&2
  elif command -v apt-get >/dev/null; then echo "Install them with: sudo apt install iproute2 socat util-linux procps" >&2
  elif command -v dnf >/dev/null; then echo "Install them with: sudo dnf install iproute socat util-linux procps-ng" >&2
  elif command -v zypper >/dev/null; then echo "Install them with: sudo zypper install iproute2 socat util-linux procps" >&2
  else echo "Install them with your package manager: ip and ss are in iproute2 (iproute on Fedora), unshare and flock in util-linux, pgrep in procps, and socat in socat." >&2
  fi
  exit 1
fi

# bsdtar (libarchive) unpacks .7z and .rar mods; zips and folders need no extra program, so this is only a warning.
if ! command -v bsdtar >/dev/null; then
  echo "Warning: bsdtar was not found; it is needed only to add .7z or .rar mods, so zips and folders still work." >&2
  if command -v pacman >/dev/null; then echo "Install it with: sudo pacman -S --needed libarchive" >&2
  elif command -v apt-get >/dev/null; then echo "Install it with: sudo apt install libarchive-tools" >&2
  elif command -v dnf >/dev/null; then echo "Install it with: sudo dnf install bsdtar" >&2
  elif command -v zypper >/dev/null; then echo "Install it with: sudo zypper install bsdtar" >&2
  else echo "Install it with your package manager: bsdtar is in libarchive (libarchive-tools on Debian and Ubuntu)." >&2
  fi
fi

# Co-op over Steam relays (#62) needs pasta (passt) and python3 for Valve's relay list. As with bsdtar, warn here;
# hellforge-offline.sh refuses before it makes the namespace when either is missing.
if [ "${HELLFORGE_STEAM_UDP:-0}" = 1 ]; then
  missing=""
  for program in pasta python3; do command -v "$program" >/dev/null || missing="$missing $program"; done
  if [ -n "$missing" ]; then
    echo "Warning: HELLFORGE_STEAM_UDP=1 needs these programs, which were not found:$missing. Co-op over Steam relays cannot start without them." >&2
    if command -v pacman >/dev/null; then echo "Install them with: sudo pacman -S --needed passt python" >&2
    elif command -v apt-get >/dev/null; then echo "Install them with: sudo apt install passt python3" >&2
    elif command -v dnf >/dev/null; then echo "Install them with: sudo dnf install passt python3" >&2
    elif command -v zypper >/dev/null; then echo "Install them with: sudo zypper install passt python3" >&2
    else echo "Install them with your package manager: passt provides pasta, and Python 3 provides python3." >&2
    fi
  fi
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
# GE-Proton needs a game ID when HellForge's prefix path contains no app ID.
export STEAM_COMPAT_DATA_PATH SteamGameId="${SteamGameId-553850}"

if [ -z "${PROTON:-}" ]; then
  mapfile -t protons < <(ls -d "$STEAM_COMPAT_CLIENT_INSTALL_PATH"/steamapps/common/Proton*/proton "$STEAM_COMPAT_CLIENT_INSTALL_PATH"/compatibilitytools.d/*/proton 2>/dev/null | sort -V)
  if [ "${#protons[@]}" -gt 0 ]; then PROTON="${protons[-1]}"; fi
  if [ "${#protons[@]}" -gt 1 ]; then
    echo "Proton: using $PROTON. Also found (set PROTON to use one of them):" >&2
    printf '  %s\n' "${protons[@]:0:${#protons[@]}-1}" >&2
  fi
fi
: "${PROTON:?PROTON is not set and no Proton was found in Steam: install one in Steam, or set PROTON to a proton script, e.g. .../compatibilitytools.d/<name>/proton}"
[ -x "$PROTON" ] || { echo "PROTON is not a proton script that can be run: $PROTON" >&2; exit 1; }
export PROTON
: "${STEAM_COMPAT_APP_ID:=553850}"
export STEAM_COMPAT_APP_ID
echo "HellForge: $INSTALL with $PROTON (prefix $STEAM_COMPAT_DATA_PATH)" >&2
echo "Steam compatibility app ID: $STEAM_COMPAT_APP_ID" >&2

# Where only allowed programs may make a user namespace (Ubuntu 24.04 and later), say so now rather than after the war
# fetch; hellforge-offline.sh checks the same.
if [ ! -x /usr/local/lib/hellforge/unshare ] && [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)" = 1 ]; then
  echo "This system only lets allowed programs make the network namespace HellForge runs in. Run this once, then start HellForge again:" >&2
  echo "  sudo bash $here/allow-namespace.sh   (docs/linux.md)" >&2
  exit 1
fi

# Steam's bridge. The state lock serializes checking for a bridge, starting one, and stopping the last one. Each wrapper
# holds the users lock shared until it exits; only the last user gets it exclusively and may stop the bridge. A bridge
# started by hand has no PID file and is left alone.
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
lock="$dir/hf-bridge.lock"
users_lock="$dir/hf-bridge-users.lock"
pidfile="$dir/hf-bridge.pid"
uid=$(id -u)
state_lock=0
user_lock=0
exec 8>"$lock"
exec 9>"$users_lock"

bridge_pid_running() {
  local pid=$1 state
  [ -r "/proc/$pid/stat" ] || return 1
  read -r _ _ state _ < "/proc/$pid/stat" 2>/dev/null || return 1
  [ "$state" != Z ]
}

bridge_pid_ok() {
  local pid=$1 owner arg
  [ -n "$pid" ] && [ -r "/proc/$pid/cmdline" ] || return 1
  owner=$(stat -c %u "/proc/$pid" 2>/dev/null 8>&- 9>&- || true)
  [ "$owner" = "$uid" ] || return 1
  while IFS= read -r -d '' arg; do
    [ "${arg##*/}" = "hf-bridge-host.sh" ] && return 0
  done < "/proc/$pid/cmdline" 2>/dev/null
  return 1
}

read_bridge_pid() {
  local pid
  pid=$(cat "$pidfile" 2>/dev/null 8>&- 9>&- || true)
  case "$pid" in ''|*[!0-9]*) return 1 ;; esac
  bridge_pid_ok "$pid" || return 1
  printf '%s\n' "$pid"
}

stop_bridge() {
  local pid=$1
  kill -TERM "$pid" 2>/dev/null 8>&- 9>&- || true
  for _ in $(seq 1 50); do
    bridge_pid_running "$pid" || break
    bridge_pid_ok "$pid" || break
    sleep 0.1 8>&- 9>&-
  done
  if bridge_pid_running "$pid" && bridge_pid_ok "$pid"; then
    echo "Steam bridge did not stop after 5 seconds; killing it." >&2
    kill -KILL "$pid" 2>/dev/null 8>&- 9>&- || true
  fi
  wait "$pid" 2>/dev/null || true
}

cleanup() {
  local status=$? pid
  echo "HellForge wrapper exiting with code $status." >&2
  if [ "$state_lock" = 1 ] || [ "$user_lock" = 1 ]; then
    if [ "$state_lock" != 1 ]; then
      exec 8>"$lock"
      flock -x 8
      state_lock=1
    fi
    if [ "$user_lock" = 1 ]; then
      flock -u 9
      user_lock=0
    fi
    if flock -x -n 9; then
      if pid="$(read_bridge_pid)"; then stop_bridge "$pid"; fi
      rm -f "$pidfile" 8>&- 9>&-
      flock -u 9
    fi
    exec 8>&-
    state_lock=0
  fi
}
trap cleanup EXIT
trap 'echo "HellForge wrapper received SIGINT." >&2; exit 130' INT
trap 'echo "HellForge wrapper received SIGTERM." >&2; exit 143' TERM
# a bridge answers on its sockets (ones left by a bridge that was killed do not)
bridged() { local s; for s in "$dir"/hf-steam-*.sock; do [ -S "$s" ] && socat -u OPEN:/dev/null UNIX-CONNECT:"$s" 2>/dev/null 8>&- 9>&- && return 0; done; return 1; }
flock -x 8
state_lock=1
flock -s 9
user_lock=1
if ! bridged; then
  bash "$here/hf-bridge-host.sh" 8>&- 9>&- >&2 &
  host=$!
  printf '%s\n' "$host" > "$pidfile"
  for _ in $(seq 1 20); do bridged && break; kill -0 "$host" 2>/dev/null || break; sleep 0.5 8>&- 9>&-; done
fi
if ! bridged; then
  echo "Steam's local ports could not be bridged: is Steam running and signed in?" >&2
  exit 1
fi
flock -u 8
exec 8>&-
state_lock=0

# The live war, now, while there is a network (Wine sees / as Z:). A failure is not fatal: the session then serves the
# last war fetched, or HellForge's built-in one. The ownership probe does not need the war fetch and skips it so the
# probe's bounded timeout covers only starting Proton and checking Steam.
if [ "${HELLFORGE_SKIP_LIVE_WAR:-}" != 1 ]; then
  backend="$INSTALL/app/backend/HellForge.LocalBackend.exe"
  cache="Z:$(printf '%s' "$INSTALL/live-war.json" | tr '/' '\\')"
  if timeout -k 10 60 "$PROTON" run "$backend" fetch-live-war "$cache" 9>&- 1>&2; then echo "Fetched the live galactic war." >&2
  else echo "Could not fetch the live galactic war; the session serves the last one fetched (or the built-in war)." >&2; fi
  # (proton can return before a Windows program ends: the fetch finishes before the namespace is entered)
  for _ in $(seq 1 30); do pgrep -f "LocalBackend.exe fetch-live-war" >/dev/null || break; sleep 1; done
fi

# The mod tools, once, also now while there is a network: source mods are compiled before a session starts, where the
# download is refused inside the namespace (issue 20). Two pins: the Windows Zig (about 90 MB) for an installation that is
# also used from Windows, fetched through Proton as before, and the native Linux Zig (about 55 MB, .tar.xz) that builds
# source mods on this host. Fetched again only when a pin changes. Not fatal either: without them source mods stay off.
pin="$(grep '"name": "zig"' "$INSTALL/app/sdk/tools.json" 2>/dev/null | sed -E 's/.*"sha256": "([0-9A-Fa-f]+)".*/\1/' || true)"
zigReady() { [ -f "$INSTALL/tools/zig/zig.exe" ] && [ "$(cat "$INSTALL/tools/zig/.hellforge-archive-sha256" 2>/dev/null)" = "$pin" ]; }
nativePin="$(grep '"name": "zig-linux"' "$INSTALL/app/sdk/tools.json" 2>/dev/null | sed -E 's/.*"sha256": "([0-9A-Fa-f]+)".*/\1/' || true)"
nativeZigReady() { [ -f "$INSTALL/tools/zig-linux/zig" ] && [ "$(cat "$INSTALL/tools/zig-linux/.hellforge-archive-sha256" 2>/dev/null)" = "$nativePin" ]; }
cliNative="$INSTALL/app/cli-linux/HellForge.Cli"
# An installation unpacked on Windows may have lost the agent's execute bit; the package's Linux installer sets it too.
[ ! -f "$cliNative" ] || [ -x "$cliNative" ] || chmod +x "$cliNative" 2>/dev/null || true
if [ -n "$pin" ] && ! zigReady; then
  if [ "${HELLFORGE_SKIP_MOD_TOOLS:-}" = 1 ]; then
    echo "Skipping the mod-tools download for the Steam ownership check." >&2
  else
    echo "Fetching the mod tools (Zig, about 90 MB, once) to build source mods..." >&2
    root="Z:$(printf '%s' "$INSTALL" | tr '/' '\\')"
    timeout -k 10 900 "$PROTON" run "$INSTALL/app/cli/HellForge.Cli.exe" mod-tools --install "$root" --ide no >/dev/null 9>&- || true
    for _ in $(seq 1 900); do pgrep -f "HellForge.Cli.exe mod-tools" >/dev/null || break; sleep 1; done
    if zigReady; then echo "Fetched the mod tools." >&2
    else echo "Could not fetch the mod tools; source mods stay off until they are (start HellForge again to retry)." >&2; fi
  fi
fi
if [ -n "$nativePin" ] && ! nativeZigReady; then
  if [ "${HELLFORGE_SKIP_MOD_TOOLS:-}" = 1 ]; then
    echo "Skipping the native mod-tools download for the Steam ownership check." >&2
  elif [ -x "$cliNative" ]; then
    echo "Fetching the native Linux mod tools (Zig, about 55 MB, once) to build source mods..." >&2
    timeout -k 10 900 "$cliNative" mod-tools --install "$INSTALL" --ide no >/dev/null 9>&- || true
    if nativeZigReady; then echo "Fetched the native Linux mod tools." >&2
    else echo "Could not fetch the native Linux mod tools; source mods stay off until they are (start HellForge again to retry)." >&2; fi
  else
    echo "The native Linux build agent is missing (app/cli-linux/HellForge.Cli); reinstall HellForge. Source mods stay off." >&2
  fi
fi

# Setup calls the wrapper after Steam confirms ownership to prepare the optional tools without launching a session.
if [ "${HELLFORGE_PREPARE_MOD_TOOLS_ONLY:-}" = 1 ]; then
  if [ -n "$nativePin" ] && nativeZigReady; then
    echo "Mod tools are ready." >&2
    exit 0
  fi
  echo "Could not prepare the pinned mod tools." >&2
  exit 1
fi

# Which commands start a session runner (the same test hf-inner.sh makes): HellForge.Session.exe play, and
# HellForge.Cli.exe play unless --dry-run yes.
session_command() {
  local program="${1:-}" i next
  [ "${2:-}" = play ] || return 1
  case "${program,,}" in
    *hellforge.session.exe) return 0 ;;
    *hellforge.cli.exe)
      for ((i = 3; i < $#; i++)); do
        next=$((i + 1))
        if [ "${!i}" = --dry-run ] && [ "${!next}" = yes ]; then return 1; fi
      done
      return 0 ;;
  esac
  return 1
}

# Source mods are compiled here, natively, before the namespace: inside it the session's Wine cannot run the pinned
# Windows Zig (issue 81). The session then finds the builds in temp/mod-builds and never starts zig.exe.
if session_command "$@"; then
  if [ "${HELLFORGE_SKIP_MOD_BUILDS:-}" = 1 ]; then
    echo "Skipping the source-mod build." >&2
  elif [ -x "$cliNative" ]; then
    echo "Building the enabled source mods with the native Linux Zig..." >&2
    timeout -k 10 1800 "$cliNative" build-source-mods --install "$INSTALL" 9>&- >&2 || echo "The source-mod build did not finish; the session reports any mod it could not build." >&2
  else
    echo "The native Linux build agent is missing (app/cli-linux/HellForge.Cli); source mods stay off." >&2
  fi
fi
# ModBuilds reads this in the Wine session: a source mod without a prebuilt record is reported, never compiled with zig.exe.
export HELLFORGE_NATIVE_MOD_BUILDS=1

# Run through bash, so the scripts work without their executable bit (lost when copied from Windows or unpacked by setup).
echo "Entering the offline network namespace." >&2
if bash "$here/hellforge-offline.sh" bash "$here/hf-inner.sh" "$@" 9>&-; then
  echo "Offline namespace command completed." >&2
else
  status=$?
  echo "Offline namespace command exited with code $status." >&2
  exit "$status"
fi
