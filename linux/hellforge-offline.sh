#!/usr/bin/env bash
# Runs a command (normally hf-inner.sh) inside a network namespace with no route out, without root: loopback for the local
# backend and Steam's bridge, a veth adapter so Wine sees a network, and "prohibit" default routes so every outside
# connection is refused at once (WSAEACCES, as the Windows block's probe expects). A self-test refuses to start if
# 1.1.1.1 is reachable.
# From DreamyBranch's LinuxSupport (SkyeShade/HellForge issue 1), with joshua-software-dev; docs/linux.md.
set -euo pipefail
[ $# -gt 0 ] || { echo "usage: $0 command [args...]" >&2; exit 2; }

selftest='
if timeout 3 bash -c "exec 3<>/dev/tcp/1.1.1.1/443" 2>/dev/null; then
  echo "hellforge-offline: outside connection SUCCEEDED, refusing to start" >&2; exit 1
fi
exec "$@"'

command -v ip >/dev/null || { echo "need iproute2 (ip)" >&2; exit 1; }
command -v socat >/dev/null || { echo "need socat" >&2; exit 1; }
uid=$(id -u); gid=$(id -g)

# Where only allowed programs may make a user namespace (Ubuntu 24.04 and later), HellForge's own copy of unshare is the
# allowed one (allow-namespace.sh, once with sudo).
unshare=unshare
[ -x /usr/local/lib/hellforge/unshare ] && unshare=/usr/local/lib/hellforge/unshare
if [ "$unshare" = unshare ] && [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)" = 1 ]; then
  echo "This system only lets allowed programs make the network namespace HellForge runs in." >&2
  echo "Run this once: sudo bash $(dirname "$0")/allow-namespace.sh   (docs/linux.md)" >&2
  exit 1
fi

# Wine opens every socket of a Windows program in the prefix's wineserver, so the network a program gets is the
# wineserver's: a wineserver of this prefix still running outside the namespace would give the programs started inside
# the internet (a VM test of 2026-10-21 reached 1.1.1.1 that way). One ends a few seconds after its last program: wait
# for that, else refuse. (Wine 10 keeps the server's socket in $XDG_RUNTIME_DIR/wine, older Wine and Proton in /tmp.)
# Proton always uses $STEAM_COMPAT_DATA_PATH/pfx, whatever WINEPREFIX says.
if [ -n "${PROTON:-}" ] && [ -n "${STEAM_COMPAT_DATA_PATH:-}" ]; then prefix="$STEAM_COMPAT_DATA_PATH/pfx"
else prefix="${WINEPREFIX:-${STEAM_COMPAT_DATA_PATH:+$STEAM_COMPAT_DATA_PATH/pfx}}"; fi
prefix="${prefix:-$HOME/.wine}"
if [ -d "$prefix" ]; then
  server="server-$(stat -L -c %D "$prefix")-$(printf %x "$(stat -L -c %i "$prefix")")"
  wineserver_running() {
    for s in "${XDG_RUNTIME_DIR:-/run/user/$uid}/wine/$server/socket" "/tmp/.wine-$uid/$server/socket"; do
      [ -S "$s" ] && socat -u OPEN:/dev/null UNIX-CONNECT:"$s" 2>/dev/null && return 0
    done
    return 1
  }
  for _ in $(seq 1 20); do wineserver_running || break; sleep 1; done
  if wineserver_running; then
    echo "A Windows program is still running in the Wine prefix $prefix outside HellForge's network namespace;" >&2
    echo "programs started inside would share its network. Close it (or give HellForge a prefix of its own: docs/linux.md)." >&2
    exit 1
  fi
fi

exec "$unshare" --user --map-root-user --net bash -c '
  # 1. Bring up loopback for local backend and Steam socat bridge
  ip link set lo up

  # 2. Create a virtual network adapter so Wine/the game sees a valid network interface
  ip link add eth0 type veth peer name veth0
  ip addr add 192.168.1.50/24 dev eth0
  ip link set eth0 up
  ip link set veth0 up

  # 3. Block all WAN traffic with prohibit (returns 10013 WSAEACCES for NetworkProbe)
  ip route add prohibit default
  ip -6 route add prohibit default 2>/dev/null || true

  # 4. Drop back to your normal user account
  exec unshare --user --map-user="$0" --map-group="$1" \
    env HELLFORGE_LINUX_NETNS=1 bash -c "$2" _ "${@:3}"
' "$uid" "$gid" "$selftest" "$@"
