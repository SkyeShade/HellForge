#!/usr/bin/env bash
# Runs a command (hf-inner.sh: HellForge's launcher under Proton) inside a network namespace with no route out, without
# root: loopback for the local backend and Steam's bridge, a veth adapter so Wine sees a network, and "prohibit" default
# routes so every outside connection is refused at once (WSAEACCES, as the Windows block's probe expects). The launcher,
# the session runner, the backend and the game all inherit it; a self-test refuses to start if 1.1.1.1 is reachable.
# HELLFORGE_STEAM_UDP=1 (co-op on Linux): the namespace is made by pasta instead, so UDP leaves it (Steam's networking, which
# co-op uses, runs inside the backend's own process and reaches Valve's relays over UDP), while TCP stays refused by a
# policy rule ("ip rule ... ipproto tcp prohibit", the same WSAEACCES). The block is then looser than the default one, but
# UDP only reaches Valve's relays (the list Valve publishes for the game, fetched at launch); every other address is refused.
# From DreamyBranch's LinuxSupport (SkyeShade/HellForge issue 1), with joshua-software-dev; docs/linux.md.
set -euo pipefail
[ $# -gt 0 ] || { echo "usage: $0 command [args...]" >&2; exit 2; }

selftest='
if timeout 3 bash -c "exec 3<>/dev/tcp/1.1.1.1/443" 2>/dev/null; then
  echo "hellforge-offline: outside connection SUCCEEDED, refusing to start" >&2; exit 1
fi
if timeout 3 bash -c "exec 3<>/dev/udp/1.1.1.1/53" 2>/dev/null; then
  echo "hellforge-offline: UDP to a non-relay address SUCCEEDED, refusing to start" >&2; exit 1
fi
exec "$@"'

command -v ip >/dev/null || { echo "need iproute2 (ip)" >&2; exit 1; }
command -v socat >/dev/null || { echo "need socat" >&2; exit 1; }
uid=$(id -u); gid=$(id -g)

# Where only allowed programs may make a user namespace (Ubuntu 24.04 and later), HellForge's own copy of unshare is the
# allowed one (allow-namespace.sh, once with sudo).
unshare=unshare
[ -x /usr/local/lib/hellforge/unshare ] && unshare=/usr/local/lib/hellforge/unshare
if [ "${HELLFORGE_STEAM_UDP:-0}" != 1 ] && [ "$unshare" = unshare ] && [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)" = 1 ]; then
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

# Everything below runs as root of a new user + network namespace. Default: nothing but loopback and a dead end.
# HELLFORGE_STEAM_UDP=1: the namespace is the same, but pasta (userspace networking, no root) is attached to it from
# outside and gives it an interface with a real route; the "ip rule" then refuses every TCP connection the way the
# prohibit routes do, so only UDP gets out. (Running the game under `pasta -- command` instead gave it its own pid, mount,
# ipc and hostname namespaces too, and the game quit during start.)
inner='
  ip link set lo up
  if [ "$HELLFORGE_STEAM_UDP" = 1 ]; then
    for _ in $(seq 1 100); do [ -d /sys/class/net/eth0 ] && ip -4 route show default | grep -q . && break; sleep 0.1; done
    ip -4 route show default | grep -q . || { echo "hellforge-offline: pasta did not bring the network up" >&2; exit 1; }
    ip rule add ipproto tcp prohibit
    ip -6 rule add ipproto tcp prohibit 2>/dev/null || true
    # UDP only to Valve relays (address and port range each), everything else refused; loopback is in the local table.
    while read -r addr lo hi; do
      ip rule add pref 1000 to "$addr/32" ipproto udp dport "$lo-$hi" lookup main
    done <<< "$HELLFORGE_UDP_RELAYS"
    ip rule add pref 2000 prohibit
    ip -6 rule add pref 2000 prohibit 2>/dev/null || true
  else
    ip link add eth0 type veth peer name veth0
    ip addr add 192.168.1.50/24 dev eth0
    ip link set eth0 up
    ip link set veth0 up
    ip route add prohibit default
    ip -6 route add prohibit default 2>/dev/null || true
  fi
  exec unshare --user --map-user="$0" --map-group="$1" \
    env HELLFORGE_LINUX_NETNS=1 bash -c "$2" _ "${@:3}"
'

if [ "${HELLFORGE_STEAM_UDP:-0}" = 1 ]; then
  command -v pasta >/dev/null || { echo "HELLFORGE_STEAM_UDP=1 needs pasta (the passt package)" >&2; exit 1; }
  # Valve's relay list for the game (the one Steam's networking uses), fetched now while this PC still has its network;
  # the last good copy is kept so a launch without a network still works. No list, no start.
  command -v python3 >/dev/null || { echo "HELLFORGE_STEAM_UDP=1 needs python3 (to read Valve's relay list)" >&2; exit 1; }
  relay_cache="${XDG_CACHE_HOME:-$HOME/.cache}/hellforge/sdr-relays.txt"
  mkdir -p "$(dirname "$relay_cache")"
  if curl -fsS --max-time 20 'https://api.steampowered.com/ISteamApps/GetSDRConfig/v1/?appid=553850' 2>/dev/null | python3 -c '
import json, sys
for pop in json.load(sys.stdin)["pops"].values():
    for r in pop.get("relays", []):
        if "ipv4" in r: print(r["ipv4"], r["port_range"][0], r["port_range"][1])' > "$relay_cache.new" && [ -s "$relay_cache.new" ]; then
    mv "$relay_cache.new" "$relay_cache"
  else
    rm -f "$relay_cache.new"
    [ -s "$relay_cache" ] && echo "hellforge-offline: could not fetch Valve's relay list, using the last one ($relay_cache)" >&2
  fi
  [ -s "$relay_cache" ] || { echo "hellforge-offline: no relay list for UDP; refusing to open it" >&2; exit 1; }
  HELLFORGE_UDP_RELAYS=$(<"$relay_cache")
  echo "hellforge-offline: UDP is open to Valve's relays only ($(wc -l < "$relay_cache") addresses) for Steam's networking (co-op); everything else is refused." >&2
  export HELLFORGE_STEAM_UDP HELLFORGE_UDP_RELAYS
  "$unshare" --user --map-root-user --net bash -c "$inner" "$uid" "$gid" "$selftest" "$@" &
  holder=$!
  pasta --foreground --quiet --config-net -4 -I eth0 -t none -T none "$holder" &
  nat=$!
  trap 'kill "$nat" 2>/dev/null || true' EXIT
  trap 'kill "$holder" 2>/dev/null || true; exit 143' INT TERM
  wait "$holder"; status=$?
  exit "$status"
fi
HELLFORGE_STEAM_UDP=0
export HELLFORGE_STEAM_UDP
exec "$unshare" --user --map-root-user --net bash -c "$inner" "$uid" "$gid" "$selftest" "$@"
