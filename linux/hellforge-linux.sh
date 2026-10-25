#!/usr/bin/env bash
# Starts HellForge on Linux (Proton), with no way out to the internet: docs/linux.md. One command, with Steam running and
# signed in: bash hellforge-linux.sh (or ./hellforge-linux.sh once it is executable)
# Another launcher (docs/external-launchers.md) passes the Windows program to run in place of HellForge.exe, with its
# arguments, e.g. ./hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes
# What it does: checks the programs it needs, finds Steam, the prefix and Proton, and, until the game is copied, moves
# Steam's download of the supported build into the prefix (where setup reads it). Then it bridges Steam's local ports
# (hf-bridge-host.sh, unless one is running already), fetches the live galactic war while this PC still has its network
# (the session that follows has none and serves this copy) and, once, the mod tools that build source mods, and starts the
# launcher inside a network namespace with
# loopback only (hellforge-offline.sh), where hf-inner.sh forwards Steam's ports and runs HellForge.exe through Proton.
# The bridge it started stops when HellForge closes.
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
# the Steam folder search, the prefix found from the installation and the depot move follow DreamyBranch's guided
# install (issue 11).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

# The prefix: an installation made by HellForge-Setup.exe under Proton is inside its prefix, so that one is meant.
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
echo "HellForge: $INSTALL with $PROTON (prefix $STEAM_COMPAT_DATA_PATH)"

# Where only allowed programs may make a user namespace (Ubuntu 24.04 and later), say so now rather than after the war
# fetch; hellforge-offline.sh checks the same.
if [ ! -x /usr/local/lib/hellforge/unshare ] && [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)" = 1 ]; then
  echo "This system only lets allowed programs make the network namespace HellForge runs in. Run this once, then start HellForge again:" >&2
  echo "  sudo bash $here/allow-namespace.sh   (docs/linux.md)" >&2
  exit 1
fi

# Until the game is copied: Steam's download of the supported build (the wizard's download_depot commands) lands in Steam's
# own folder, which setup cannot read under Wine (the Linux folders are on Wine's Z: drive, which it refuses as a linked
# path). Setup reads it from Steam's folder inside the prefix, so offer to move it there: a rename on the same
# filesystem, no extra space. Only for an installation inside this prefix (made there by HellForge-Setup.exe).
# The depots are the supported build's (src/HellForge.Core/supported-build.json).
if [ -n "$inside" ] && [ "$inside" -ef "$STEAM_COMPAT_DATA_PATH" ] && [ ! -f "$INSTALL/game/bin/helldivers2.exe" ]; then
  depots="553851 553853 553854"
  target="$STEAM_COMPAT_DATA_PATH/pfx/drive_c/Program Files (x86)/Steam/steamapps/content/app_553850"
  # true for a folder that holds every depot the prefix still lacks
  holds_rest() {
    local d n=0
    for d in $depots; do
      [ -d "$target/depot_$d" ] && continue
      [ -d "$1/depot_$d" ] || return 1
      n=$((n + 1))
    done
    [ "$n" -gt 0 ]
  }
  download=""
  # the Linux client downloads relative to the folder its program runs from (docs/linux.md)
  for client in . ubuntu12_32 ubuntu12_64 steamrt64; do
    candidate="$STEAM_COMPAT_CLIENT_INSTALL_PATH/$client/steamapps/content/app_553850"
    if holds_rest "$candidate"; then download="$(cd "$candidate" && pwd)"; break; fi
  done
  if [ -n "$download" ]; then
    echo "Steam's download of the supported HELLDIVERS 2 build is in $download."
    echo "Setup reads it only from inside the prefix: $target"
    before="$(du -sb "$download" | cut -f1)"; sleep 5; after="$(du -sb "$download" | cut -f1)"
    if [ "$before" != "$after" ]; then
      echo "Steam is still writing it. Once the console says \"Depot download complete\" for all three commands, start HellForge again to move it." >&2
    elif [ -t 0 ]; then
      mkdir -p "$target"
      if [ "$(stat -c %d "$download")" != "$(stat -c %d "$target")" ]; then
        echo "The prefix is on another filesystem than Steam: moving copies about 24 GB and needs that much free space there."
      fi
      read -r -p "Move it into the prefix now? Only if all three download_depot commands said \"Depot download complete\". [y/N] " answer || answer=""
      if [[ "$answer" =~ ^[Yy] ]]; then
        for d in $depots; do
          if [ -d "$download/depot_$d" ] && [ ! -e "$target/depot_$d" ]; then echo "Moving depot_$d ..."; mv "$download/depot_$d" "$target/depot_$d"; fi
        done
        rmdir "$download" 2>/dev/null || true
        echo "Moved. In HellForge's setup, press Check again (or Start setup) to copy the game from it."
      fi
    else
      echo "Run this script in a terminal to move it, or move its depot_* folders there yourself (docs/linux.md)." >&2
    fi
  fi
  # Setup looks for Steam where the prefix's registry says (SteamPath), else in C:\Program Files (x86)\Steam: point it at
  # that folder, the one inside the prefix (a Steam folder on the Z: drive is refused as a linked path).
  "$PROTON" run reg add 'HKCU\Software\Valve\Steam' /v SteamPath /t REG_SZ /d 'C:\Program Files (x86)\Steam' /f >/dev/null 2>&1 \
    || echo "Could not set Steam's folder in the prefix's registry (SteamPath); setup may not find the download." >&2
fi

# Steam's bridge: the one running, else one of our own for as long as HellForge runs.
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
bridge=""
cleanup() { if [ -n "$bridge" ]; then kill "$bridge" 2>/dev/null || true; wait "$bridge" 2>/dev/null || true; fi; }
trap cleanup EXIT
trap 'exit 130' INT TERM
# a bridge answers on its sockets (ones left by a bridge that was killed do not)
bridged() { for s in "$dir"/hf-steam-*.sock; do [ -S "$s" ] && socat -u OPEN:/dev/null UNIX-CONNECT:"$s" 2>/dev/null && return 0; done; return 1; }
if ! bridged; then
  bash "$here/hf-bridge-host.sh" &
  bridge=$!
  for _ in $(seq 1 20); do bridged && break; kill -0 "$bridge" 2>/dev/null || break; sleep 0.5; done
fi
bridged || { echo "Steam's local ports could not be bridged: is Steam running and signed in?" >&2; exit 1; }

# The live war, now, while there is a network (Wine sees / as Z:). A failure is not fatal: the session then serves the
# last war fetched, or HellForge's built-in one.
backend="$INSTALL/app/backend/HellForge.LocalBackend.exe"
cache="Z:$(printf '%s' "$INSTALL/live-war.json" | tr '/' '\\')"
if timeout 60 "$PROTON" run "$backend" fetch-live-war "$cache"; then echo "Fetched the live galactic war."
else echo "Could not fetch the live galactic war; the session serves the last one fetched (or the built-in war)." >&2; fi
# (proton can return before a Windows program ends: the fetch finishes before the namespace is entered)
for _ in $(seq 1 30); do pgrep -f "LocalBackend.exe fetch-live-war" >/dev/null || break; sleep 1; done

# The mod tools (HellForge's pinned Zig, about 90 MB from ziglang.org), once, also now: source mods are compiled when a
# session starts, inside the namespace, where the download is refused (issue 20). Fetched again only when the pin changes.
# Not fatal either: without them source mods stay off.
pin="$(grep '"name": "zig"' "$INSTALL/app/sdk/tools.json" 2>/dev/null | sed -E 's/.*"sha256": "([0-9A-Fa-f]+)".*/\1/' || true)"
zigReady() { [ -f "$INSTALL/tools/zig/zig.exe" ] && [ "$(cat "$INSTALL/tools/zig/.hellforge-archive-sha256" 2>/dev/null)" = "$pin" ]; }
if [ -n "$pin" ] && ! zigReady; then
  echo "Fetching the mod tools (Zig, about 90 MB, once) to build source mods..."
  root="Z:$(printf '%s' "$INSTALL" | tr '/' '\\')"
  timeout 900 "$PROTON" run "$INSTALL/app/cli/HellForge.Cli.exe" mod-tools --install "$root" --ide no >/dev/null || true
  for _ in $(seq 1 900); do pgrep -f "HellForge.Cli.exe mod-tools" >/dev/null || break; sleep 1; done
  if zigReady; then echo "Fetched the mod tools."
  else echo "Could not fetch the mod tools; source mods stay off until they are (start HellForge again to retry)." >&2; fi
fi

# Run through bash, so the scripts work without their executable bit (lost when copied from Windows or unpacked by setup).
bash "$here/hellforge-offline.sh" bash "$here/hf-inner.sh" "$@"
