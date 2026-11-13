#!/usr/bin/env bash
# Runs a command (normally hf-inner.sh) inside a network namespace with no route out, without root: loopback for the local
# backend and Steam's bridge, a veth adapter so Wine sees a network, and "prohibit" default routes so every outside
# connection is refused at once (WSAEACCES, as the Windows block's probe expects). A self-test refuses to start if
# 1.1.1.1 is reachable.
# HELLFORGE_STEAM_UDP=1 (co-op over Steam relays, on unless the player turned it off): pasta is attached to the
# namespace and gives it a route, so UDP leaves it (Steam's networking, which co-op uses, runs inside the backend's own
# process and reaches Valve's relays over UDP), while TCP stays refused by a policy rule ("ip rule ... ipproto tcp
# prohibit", the same WSAEACCES). UDP only reaches Valve's relays (the list Valve publishes for the game, fetched at
# launch); every other address is refused. When it cannot open (no pasta, no python3, no relay list, pasta did not bring
# the network up), the session starts without it: HELLFORGE_STEAM_UDP=0 and HELLFORGE_STEAM_UDP_PROBLEM say why, for the
# backend, whose lobby browser tells the player.
# HELLFORGE_NETNS_KEEPER (a session, SessionRunner): when the session starts without the route, a small process that
# lives as long as the session stays in the namespace's own user namespace, and its pid and start time go to that file,
# so the launcher can turn co-op over Steam relays on while the session runs (hf-coop-relays.sh).
# From DreamyBranch's LinuxSupport (SkyeShade/HellForge issue 1), with joshua-software-dev; docs/linux.md.
set -euo pipefail
[ $# -gt 0 ] || { echo "usage: $0 command [args...]" >&2; exit 2; }

selftest='
if timeout -k 1 3 bash -c "exec 3<>/dev/tcp/1.1.1.1/443" 2>/dev/null; then
  echo "hellforge-offline: outside connection SUCCEEDED, refusing to start" >&2; exit 1
fi
if timeout -k 1 3 bash -c "exec 3<>/dev/udp/1.1.1.1/53" 2>/dev/null; then
  echo "hellforge-offline: UDP to a non-relay address SUCCEEDED, refusing to start" >&2; exit 1
fi
exec "$@"'

command -v ip >/dev/null || { echo "need iproute2 (ip)" >&2; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The installation's native CLI, as hellforge-linux.sh and hf-inner.sh use; beside this script when INSTALL is not set
# (the bridge started by hand).
if [ -n "${INSTALL:-}" ]; then cli="$INSTALL/app/cli-linux/HellForge.Cli"; else cli="$here/../cli-linux/HellForge.Cli"; fi
[ -f "$cli" ] || { echo "The native Linux build agent is missing ($cli): reinstall HellForge." >&2; exit 1; }
[ -x "$cli" ] || chmod +x "$cli" 2>/dev/null || true
[ -x "$cli" ] || { echo "The native Linux build agent is not executable: $cli" >&2; exit 1; }
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

# A wineserver of the session's prefix still running outside the namespace would give the programs started inside the
# internet: waited for, then refused (hf-wineserver.sh). Proton always uses $STEAM_COMPAT_DATA_PATH/pfx, whatever
# WINEPREFIX says.
if [ -n "${PROTON:-}" ] && [ -n "${STEAM_COMPAT_DATA_PATH:-}" ]; then prefix="$STEAM_COMPAT_DATA_PATH/pfx"
else prefix="${WINEPREFIX:-${STEAM_COMPAT_DATA_PATH:+$STEAM_COMPAT_DATA_PATH/pfx}}"; fi
prefix="${prefix:-$HOME/.wine}"
. "$here/hf-wineserver.sh"
hf_wineserver_wait "$cli" "$prefix" "$uid" || exit 1

# Everything below runs as root of a new user + network namespace. Default: nothing but loopback and a dead end.
# HELLFORGE_STEAM_UDP=1: the namespace is the same, but pasta (userspace networking, no root) is attached to it from
# outside and gives it an interface with a real route; the relay policy (hf-relays.sh) follows before the session's
# command starts, so only UDP to the relays ever gets out of it. (Running the game under `pasta -- command` instead gave it its own
# pid, mount, ipc and hostname namespaces too, and the game quit during start.)
inner='
  ip link set lo up
  if [ "$HELLFORGE_STEAM_UDP" = 1 ]; then
    # pasta sets the route first: the kernel checks the default route'"'"'s gateway through the rules, which the relay
    # policy would refuse. Nothing runs in the namespace yet, so the policy follows before the session starts.
    for _ in $(seq 1 100); do [ -d /sys/class/net/eth0 ] && ip -4 route show default | grep -q . && break; sleep 0.1; done
    if ! ip -4 route show default | grep -q .; then
      HELLFORGE_STEAM_UDP_PROBLEM="pasta did not bring the session'"'"'s network up"
    else
      eval "$HF_RELAY_RULES"
      if ! hf_relay_rules; then
        HELLFORGE_STEAM_UDP_PROBLEM="the relay rules could not be set in the session'"'"'s network"
        ip route flush default 2>/dev/null || true
      fi
    fi
    if [ -n "${HELLFORGE_STEAM_UDP_PROBLEM:-}" ]; then
      echo "hellforge-offline: co-op over Steam relays is off for this session: $HELLFORGE_STEAM_UDP_PROBLEM (pasta'"'"'s own message, if any, is above)" >&2
      HELLFORGE_STEAM_UDP=0
      export HELLFORGE_STEAM_UDP HELLFORGE_STEAM_UDP_PROBLEM
    fi
  fi
  if [ "$HELLFORGE_STEAM_UDP" != 1 ]; then
    if [ ! -e /sys/class/net/eth0 ]; then
      ip link add eth0 type veth peer name veth0
      ip addr add 192.168.1.50/24 dev eth0
      ip link set eth0 up
      ip link set veth0 up
    fi
    ip route add prohibit default
    ip -6 route add prohibit default 2>/dev/null || true
    # The keeper: in this user namespace (the one that owns the network namespace) for as long as the session runs,
    # which this shell becomes below, and gone at most 2 s after it.
    if [ -n "${HELLFORGE_NETNS_KEEPER:-}" ]; then
      holder=$$
      ( while kill -0 "$holder" 2>/dev/null; do sleep 2; done ) </dev/null >/dev/null 2>&1 &
      read -r -a stat < "/proc/$!/stat"
      echo "$! ${stat[21]}" > "$HELLFORGE_NETNS_KEEPER.partial" && mv -f "$HELLFORGE_NETNS_KEEPER.partial" "$HELLFORGE_NETNS_KEEPER"
    fi
  fi
  exec unshare --user --map-user="$0" --map-group="$1" \
    env HELLFORGE_LINUX_NETNS=1 bash -c "$2" _ "${@:3}"
'

. "$here/hf-relays.sh"
export HF_RELAY_RULES
if [ -n "${HELLFORGE_NETNS_KEEPER:-}" ]; then
  rm -f "$HELLFORGE_NETNS_KEEPER"
  export HELLFORGE_NETNS_KEEPER
fi
unset HELLFORGE_STEAM_UDP_PROBLEM

# Co-op over Steam relays needs pasta, and python3 for Valve's relay list: without either, or without a list, the
# session starts without it and says why.
if [ "${HELLFORGE_STEAM_UDP:-0}" = 1 ]; then
  problem=""
  if ! command -v pasta >/dev/null; then problem="pasta (the passt package) is not installed"
  else
    relay_file=$(mktemp)
    if problem=$(hf_relay_list 2>&1 >"$relay_file"); then
      [ -n "$problem" ] && echo "$problem" >&2
      problem=""
      HELLFORGE_UDP_RELAYS=$(<"$relay_file")
    else
      problem=$(tail -n 1 <<< "$problem")
    fi
    rm -f "$relay_file"
  fi
  if [ -n "$problem" ]; then
    echo "hellforge-offline: co-op over Steam relays is off for this session: $problem" >&2
    HELLFORGE_STEAM_UDP=0
    HELLFORGE_STEAM_UDP_PROBLEM=$problem
    export HELLFORGE_STEAM_UDP_PROBLEM
  fi
fi

if [ "${HELLFORGE_STEAM_UDP:-0}" = 1 ]; then
  echo "hellforge-offline: UDP is open to Valve's relays only ($(grep -c . <<< "$HELLFORGE_UDP_RELAYS") addresses) for Steam's networking (co-op); everything else is refused." >&2
  export HELLFORGE_STEAM_UDP HELLFORGE_UDP_RELAYS
  "$unshare" --user --map-root-user --net bash -c "$inner" "$uid" "$gid" "$selftest" "$@" &
  holder=$!
  # pasta reads the holder's namespace once: wait until unshare(2) has run, or it would attach to the host's.
  for _ in $(seq 1 50); do [ "$(readlink /proc/$holder/ns/net 2>/dev/null)" != "$(readlink /proc/self/ns/net)" ] && break; sleep 0.05; done
  # No port forwarding in either direction, TCP or UDP: the relay rules are the only way out. --no-netns-quit: pasta
  # otherwise watches the holder's /proc/<pid>/ns folder to quit with it, which Fedora's passt SELinux policy denies
  # ("netns dir open: Permission denied", so no network and co-op never starts); the EXIT trap below ends it instead.
  pasta --foreground --quiet --no-netns-quit --config-net -4 -I eth0 -t none -T none -u none -U none "$holder" &
  nat=$!
  trap 'kill "$nat" 2>/dev/null || true' EXIT
  trap 'kill "$holder" 2>/dev/null || true; exit 143' INT TERM
  wait "$holder"; status=$?
  exit "$status"
fi
HELLFORGE_STEAM_UDP=0
export HELLFORGE_STEAM_UDP
exec "$unshare" --user --map-root-user --net bash -c "$inner" "$uid" "$gid" "$selftest" "$@"
