# Valve's Steam relay list for the game, for co-op over Steam relays (#62): sourced by hellforge-offline.sh (a session
# that starts with it) and hf-coop-relays.sh (a running session that turns it on). Fetched while this PC still has its
# network, from the list Valve publishes for the game (ISteamApps/GetSDRConfig/v1, app 553850): one "address low high"
# line a relay. The last good copy is kept, so a launch without a network still works.

# Prints the list on stdout; returns 1 with the reason on stderr when there is none (no python3 to read it, or no fetch
# and no saved copy).
hf_relay_list() {
  local cache="${XDG_CACHE_HOME:-$HOME/.cache}/hellforge/sdr-relays.txt"
  command -v python3 >/dev/null || { echo "python3 is not installed (it reads Valve's relay list)" >&2; return 1; }
  mkdir -p "$(dirname "$cache")"
  if curl -fsS --max-time 20 'https://api.steampowered.com/ISteamApps/GetSDRConfig/v1/?appid=553850' 2>/dev/null | python3 -c '
import json, sys
for pop in json.load(sys.stdin)["pops"].values():
    for r in pop.get("relays", []):
        if "ipv4" in r: print(r["ipv4"], r["port_range"][0], r["port_range"][1])' > "$cache.new" 2>/dev/null && [ -s "$cache.new" ]; then
    mv "$cache.new" "$cache"
  else
    rm -f "$cache.new"
    [ -s "$cache" ] && echo "hellforge: could not fetch Valve's relay list, using the last one ($cache)" >&2
  fi
  [ -s "$cache" ] || { echo "no relay list: Valve's could not be fetched and none was saved before" >&2; return 1; }
  cat "$cache"
}

# The policy that lets only UDP to the relays out, as a function for a namespace's root to define (eval) and run
# (hf_relay_rules): TCP refused the way the prohibit routes refuse it (WSAEACCES), UDP to each relay's address and port
# range looked up in the main table, everything else refused; loopback is in the local table, before all of it.
# HELLFORGE_UDP_RELAYS holds the list. A rule already there (a second try in the same namespace) is not an error.
HF_RELAY_RULES='
hf_relay_rules() {
  local out addr lo hi
  hf_rule() { out=$(ip "$@" 2>&1) || case "$out" in *"File exists"*) ;; *) echo "$out" >&2; return 1 ;; esac; }
  hf_rule rule add ipproto tcp prohibit || return 1
  ip -6 rule add ipproto tcp prohibit 2>/dev/null || true
  while read -r addr lo hi; do
    [ -n "$addr" ] || continue
    hf_rule rule add pref 1000 to "$addr/32" ipproto udp dport "$lo-$hi" lookup main || return 1
  done <<< "$HELLFORGE_UDP_RELAYS"
  hf_rule rule add pref 2000 prohibit || return 1
  ip -6 rule add pref 2000 prohibit 2>/dev/null || true
}
'
