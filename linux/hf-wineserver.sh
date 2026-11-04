# Sourced by hellforge-offline.sh (and its tests, ContainerCheckTests' way): whether a wineserver of the session's prefix
# still runs outside the network namespace. Wine opens every socket of a Windows program in the prefix's wineserver, so
# the network a program gets is the wineserver's: one still running outside would give the programs started inside the
# internet (a VM test of 2026-10-21 reached 1.1.1.1 that way). A wineserver ends a few seconds after its last program
# (the live war fetch and the mod-tools fetch run Proton in the same prefix just before), so it is waited for, then
# refused. Wine names the socket after the prefix folder's device and inode (LinuxCapture.WineserverServer); Wine 10 keeps
# it in $XDG_RUNTIME_DIR/wine, older Wine and Proton in /tmp.

# hf_wineserver_state <cli> <server> <uid> <prefix>: 0 one of the server's sockets accepted a connection, 1 none did
# (missing or refused), 3 a socket's state cannot be told (the reason on stderr). <cli> is the native HellForge.Cli,
# whose `steam-bridge check <socket>` gives the same three codes.
hf_wineserver_state() {
  local cli="$1" server="$2" uid="$3" prefix="$4" s status
  for s in "${XDG_RUNTIME_DIR:-/run/user/$uid}/wine/$server/socket" "/tmp/.wine-$uid/$server/socket"; do
    if "$cli" steam-bridge check "$s"; then
      return 0
    else
      status=$?
    fi
    case "$status" in
      1) ;;
      *)
        echo "hellforge-offline: cannot tell whether a wineserver of $prefix is running outside the namespace: steam-bridge check exited with code $status." >&2
        return 3
        ;;
    esac
  done
  return 1
}

# hf_wineserver_wait <cli> <prefix> <uid> [polls]: 0 when no wineserver of the prefix runs (or there is no prefix yet), so
# the namespace may be made; 1 when one still runs after the polls (one a second, 20 by default) or its state cannot be
# told, with the reason on stderr. The optional polls are for the tests, not an environment hook.
hf_wineserver_wait() {
  local cli="$1" prefix="$2" uid="$3" polls="${4:-20}" server state=0 _
  [ -d "$prefix" ] || return 0
  server="server-$(stat -L -c %D "$prefix")-$(printf %x "$(stat -L -c %i "$prefix")")"
  # polled while one runs (0); gone (1) goes on at once, and cannot tell (3) refuses at once
  for _ in $(seq 1 "$polls"); do
    state=0
    hf_wineserver_state "$cli" "$server" "$uid" "$prefix" || state=$?
    [ "$state" = 0 ] || break
    sleep 1
  done
  case "$state" in
    1) return 0 ;;
    0)
      echo "A Windows program is still running in the Wine prefix $prefix outside HellForge's network namespace;" >&2
      echo "programs started inside would share its network. Close it (or give HellForge a prefix of its own: docs/linux.md)." >&2
      return 1
      ;;
    *)
      echo "hellforge-offline: cannot tell whether a wineserver of $prefix is running outside the namespace; refusing to start." >&2
      return 1
      ;;
  esac
}
