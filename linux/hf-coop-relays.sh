#!/usr/bin/env bash
# Turns co-op over Steam relays on in a running session (the lobby browser's ask, which the launcher carries out here,
# outside the session's network namespace): the same network a session that starts with it has (hellforge-offline.sh),
# put in place while the game runs. The session's keeper (HELLFORGE_NETNS_KEEPER, written by hellforge-offline.sh)
# stays in the user namespace that owns the session's network namespace, so this PC's user may change that network.
#   1. The relay policy first (hf-relays.sh): from the moment a route exists, only UDP to Valve's relays leaves.
#   2. The dead end goes: the veth adapter and the prohibit default routes.
#   3. pasta is attached (as at a session's start) and brings up eth0 with a route.
#   4. The session start's self-test: TCP and UDP to 1.1.1.1 must fail, UDP to a relay must have its route.
# Anything that fails puts the dead end back. pasta then runs until the session's keeper is gone (a watcher ends it).
# Usage: hf-coop-relays.sh   (INSTALL: the installation; else the one this script is in)
# What it does goes to stderr; its last stdout line is "OPEN", or "FAILED <reason>" (exit 1).
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
install="${INSTALL:-$(cd "$here/../.." && pwd)}"
keeper_file="$install/temp/linux-netns-keeper"

fail() { echo "FAILED $*"; exit 1; }

command -v pasta >/dev/null || fail "pasta (the passt package) is not installed"
command -v nsenter >/dev/null || fail "nsenter (util-linux) is not installed"
[ -s "$keeper_file" ] || fail "this session has no network keeper (started without HellForge's launcher or session runner, or by an older HellForge)"
read -r keeper start < "$keeper_file"
alive() { local s; read -r -a s < "/proc/$keeper/stat" 2>/dev/null && [ "${s[21]}" = "$start" ]; }
alive || fail "the session's network is gone (the session ended?)"
[ "$(readlink "/proc/$keeper/ns/net")" != "$(readlink /proc/self/ns/net)" ] || fail "the session's keeper is not in a network namespace of its own"

. "$here/hf-relays.sh"
reason_file=$(mktemp)
if ! HELLFORGE_UDP_RELAYS=$(hf_relay_list 2>"$reason_file"); then
  reason=$(tail -n 1 "$reason_file"); rm -f "$reason_file"
  fail "$reason"
fi
cat "$reason_file" >&2; rm -f "$reason_file"
export HELLFORGE_UDP_RELAYS HF_RELAY_RULES

# As root of the namespace's own user namespace (this user owns it), in its network.
ns() { nsenter -t "$keeper" -U -n --preserve-credentials -- "$@"; }

dead_end='
  [ -e /sys/class/net/eth0 ] || { ip link add eth0 type veth peer name veth0 && ip addr add 192.168.1.50/24 dev eth0 &&
    ip link set eth0 up && ip link set veth0 up; }
  ip route add prohibit default 2>/dev/null; ip -6 route add prohibit default 2>/dev/null; true'
nat=""

echo "hf-coop-relays: setting the relay policy ($(grep -c . <<< "$HELLFORGE_UDP_RELAYS") relays)" >&2
# The kernel checks a new route's gateway with a lookup through the rules that names no protocol, which the policy's
# last rule (pref 2000, refuse everything) refuses, so pasta could not set its routes. While pasta sets them, rules that
# name UDP and ICMP refuse those instead (TCP's own rule stays), and the last rule comes back as soon as the route is
# there: the running game never gets out but to the relays.
during='ip rule add pref 1500 ipproto udp prohibit && ip rule add pref 1501 ipproto icmp prohibit'
after='ip rule del pref 1500 ipproto udp prohibit 2>/dev/null; ip rule del pref 1501 ipproto icmp prohibit 2>/dev/null; true'
ns bash -c "$after; $during" || fail "the session's network could not be changed"
ns bash -c 'eval "$HF_RELAY_RULES" && hf_relay_rules' || { ns bash -c "$after"; fail "the relay rules could not be set in the session's network"; }
last_rule() { ns bash -c 'ip rule show pref 2000 | grep -q prohibit || ip rule add pref 2000 prohibit'; }
restore() {
  [ -n "$nat" ] && kill "$nat" 2>/dev/null
  last_rule
  ns bash -c "ip link del eth0 2>/dev/null; $dead_end; $after" || true
}
ns bash -c 'ip rule del pref 2000 prohibit; ip link del eth0 2>/dev/null; ip route del prohibit default 2>/dev/null; ip -6 route del prohibit default 2>/dev/null; true'

# The same pasta as a session's start (hellforge-offline.sh), attached to the keeper's namespaces; it forks into the
# background once the namespace's network is set up, and writes its pid.
pid_file=$(mktemp); err_file=$(mktemp)
if ! pasta --quiet --no-netns-quit --config-net -4 -I eth0 -t none -T none -u none -U none -P "$pid_file" "$keeper" \
    </dev/null >/dev/null 2>"$err_file"; then
  reason=$(head -c 300 "$err_file" | tr '\n' ' '); rm -f "$pid_file" "$err_file"
  restore
  fail "pasta could not attach to the session's network: ${reason:-no message}"
fi
nat=$(cat "$pid_file" 2>/dev/null); rm -f "$pid_file" "$err_file"

for _ in $(seq 1 100); do ns ip -4 route show default 2>/dev/null | grep -q . && break; sleep 0.1; done
ns ip -4 route show default 2>/dev/null | grep -q . || { restore; fail "pasta did not bring the session's network up"; }
last_rule || { restore; fail "the relay policy's last rule could not be put back"; }
ns bash -c "$after"

read -r relay port _ <<< "$(head -n 1 <<< "$HELLFORGE_UDP_RELAYS")"
check='
  if timeout -k 1 3 bash -c "exec 3<>/dev/tcp/1.1.1.1/443" 2>/dev/null; then echo "TCP to 1.1.1.1 got out"; exit 1; fi
  if timeout -k 1 3 bash -c "exec 3<>/dev/udp/1.1.1.1/53" 2>/dev/null; then echo "UDP to a non-relay address got out"; exit 1; fi
  timeout -k 1 3 bash -c "exec 3<>/dev/udp/$0/$1" 2>/dev/null || { echo "UDP to a relay ($0) has no route"; exit 1; }'
if ! problem=$(ns bash -c "$check" "$relay" "$port"); then
  restore
  fail "the session's network check failed: $problem"
fi

# pasta goes with the session: --no-netns-quit (Fedora's passt SELinux policy) keeps it from watching the namespace.
setsid bash -c 'while read -r -a s < "/proc/$0/stat" 2>/dev/null && [ "${s[21]}" = "$1" ]; do sleep 2; done; kill "$2" 2>/dev/null' \
  "$keeper" "$start" "$nat" </dev/null >/dev/null 2>&1 &
echo "hf-coop-relays: UDP is open to Valve's relays only for this session; TCP and every other address stay refused" >&2
echo "OPEN"
