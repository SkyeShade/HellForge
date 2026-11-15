#!/bin/bash
# HellForge's Sync Inventory helper on Linux (docs/linux.md, "Account sync"). allow-capture.sh installs this file and
# capture-relay root-owned at /usr/local/lib/hellforge (on SteamOS /opt/hellforge) and gives the player's account a
# sudoers rule for this exact program. The launcher then starts and stops it without another password prompt.
#
#   capture-helper.sh check
#   capture-helper.sh start              (root; reads "<SteamID64> <API IPv4 address>" from stdin)
#   capture-helper.sh stop
#   capture-helper.sh cleanup
#   capture-helper.sh hosts-add --hosts-file <path> [--state-dir <dir>]             (test only, unprivileged)
#   capture-helper.sh hosts-remove --hosts-file <path> [--state-dir <dir>]          (test only, unprivileged)
#   capture-helper.sh watch --pid <pid> --hosts-file <path> [--state-dir <dir>] [--seconds <n>]  (test only)
#
# start adds exactly CaptureHosts.Marker and "127.0.0.1 api.live.prod.thehelldiversgame.com" to /etc/hosts, runs the
# installed root-owned capture-relay (never a file from the installation, which any Wine program can write; it binds
# 127.0.0.1:443 as root, drops to nobody, and forwards to 127.0.0.1:18443 only), and runs the installed root-owned capture
# as the signing service: a systemd DynamicUser whose StateDirectory holds the snapshot signer's key, out of the player's
# reach (SnapshotSigner). Once it has sealed, the account files go to the player in /run/hellforge/capture-out and its
# public key to /var/lib/hellforge (SignerPublicKey). The EXIT/TERM/INT/HUP trap removes the redirect and stops both, and
# the service ends at the capture's timeout plus a margin, so killing HellForge leaves nothing behind. Wine is never started here and
# this helper never becomes root for the game. The test-only host and state arguments are not in the sudoers rule.
set -euo pipefail
# sudo starts this root-owned helper; use only the root-owned system paths even if the caller's PATH differs.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
umask 077

marker='# HellForge-CaptureProxy'
api='api.live.prod.thehelldiversgame.com'
# The proxy's own timeout is 10 minutes; two minutes of margin, and never more than an hour.
max_lifetime=720
state_dir='/run/hellforge'
hosts_file='/etc/hosts'
pid_file="$state_dir/capture-helper.pid"
lock_file="$state_dir/capture-helper.lock"
# allow-capture.sh installs this file and the relay together in one of these root-owned folders (SteamOS cannot write
# /usr/local). The relay run is the one beside this file; started from anywhere else, as the tests do, it is the first.
helper_folders=(/usr/local/lib/hellforge /opt/hellforge /var/lib/hellforge)
helper_folder=${helper_folders[0]}
for candidate in "${helper_folders[@]}"; do
  [ "${BASH_SOURCE[0]}" -ef "$candidate/capture-helper" ] && helper_folder=$candidate
done
helper_path="$helper_folder/capture-helper"
relay_path="$helper_folder/capture-relay"
proxy_dir="$helper_folder/capture-proxy"
signer_unit='hellforge-capture'
signer_run='/run/hellforge-capture'
public_key='/var/lib/hellforge/snapshot-signer.pub'
capture_pid=''
player_uid=''
relay_pid=''
watch_pid=''
cleanup_done=''

die() {
  echo "hellforge-capture-helper: $*" >&2
  exit 1
}

require_root() {
  [ "$(id -u)" = 0 ] || die "run it through sudo: sudo $helper_path $*"
}

# The relay is installed root-owned beside this helper and is the only relay this root process runs. Refuse a missing,
# non-executable or writable copy instead of falling back to anything in the installation.
require_relay() {
  local owner mode
  [ -f "$relay_path" ] || die "the relay is missing ($relay_path): run allow-capture.sh again."
  [ -x "$relay_path" ] || die "the relay is not executable ($relay_path): run allow-capture.sh again."
  owner="$(stat -c %u "$relay_path" 2>/dev/null || true)"
  mode="$(stat -c %a "$relay_path" 2>/dev/null || true)"
  [ "$owner" = 0 ] || die "the relay is not root-owned ($relay_path): run allow-capture.sh again."
  [[ "$mode" =~ ^[0-7]+$ ]] && (( (8#$mode & 8#022) == 0 )) ||
    die "the relay is writable by another account ($relay_path): run allow-capture.sh again."
  id -u nobody >/dev/null 2>&1 || die "the unprivileged 'nobody' account is missing (the relay drops to it after binding)."
}

# The signing service: systemd, and the capture copy allow-capture.sh installed, root's alone and unchanged since.
require_signer() {
  local recorded actual
  command -v systemd-run >/dev/null && command -v systemctl >/dev/null && [ -d /run/systemd/system ] ||
    die "premium content needs a systemd system: the snapshot signer runs as a systemd service."
  [ -d "$proxy_dir" ] && [ ! -L "$proxy_dir" ] || die "the capture copy is missing ($proxy_dir): run allow-capture.sh again."
  [ -z "$(find "$proxy_dir" \( ! -user 0 -o -perm /022 -o -type l \) -print -quit)" ] ||
    die "the capture copy is not root's alone ($proxy_dir): run allow-capture.sh again."
  recorded="$(cat "$proxy_dir.sha256" 2>/dev/null || true)"
  actual="$( (cd "$proxy_dir" && LC_ALL=C find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) | sha256sum | awk '{print $1}')"
  [ -n "$recorded" ] && [ "$recorded" = "$actual" ] || die "the capture copy changed since it was installed: run allow-capture.sh again."
}

require_commands() {
  local name
  for name in flock awk mktemp cmp chmod chown mv tail date sleep stat find sort xargs sha256sum install grep; do
    command -v "$name" >/dev/null || die "$name is missing."
  done
}

read_pid_file() {
  [ -f "$pid_file" ] || return 0
  # Three numeric pids: the helper first, its capture-relay second, the capture proxy third. A two-pid file from an older
  # helper is still readable; a malformed file is treated as stale.
  local helper_pid old_relay_pid old_proxy_pid extra
  if read -r helper_pid old_relay_pid old_proxy_pid extra < "$pid_file" &&
     [[ "$helper_pid" =~ ^[0-9]+$ ]] && [[ "$old_relay_pid" =~ ^[0-9]+$ ]] &&
     { [ -z "${old_proxy_pid:-}" ] || [[ "$old_proxy_pid" =~ ^[0-9]+$ ]]; } && [ -z "${extra:-}" ]; then
    printf '%s %s %s\n' "$helper_pid" "$old_relay_pid" "${old_proxy_pid:-0}"
  fi
}

pid_matches() {
  local pid="$1" text="$2" cmdline
  [ -r "/proc/$pid/cmdline" ] || return 1
  cmdline="$(tr '\0' ' ' < "/proc/$pid/cmdline")" || return 1
  [[ "$cmdline" == *"$text"* ]]
}

# A helper or relay left running by a copy in another of the folders counts too (an install that moved the helper).
pid_is_helper() {
  local folder
  for folder in "${helper_folders[@]}"; do pid_matches "$1" "$folder/capture-helper" && return 0; done
  return 1
}
pid_is_proxy() { pid_matches "$1" "HellForge.CaptureProxy"; }
# The native relay, and the socat listener an older helper may have left behind.
pid_is_relay() {
  local folder
  for folder in "${helper_folders[@]}"; do pid_matches "$1" "$folder/capture-relay" && return 0; done
  pid_matches "$1" "TCP-LISTEN:443,bind=127.0.0.1"
}

kill_pid() {
  local pid="$1"
  [ -n "$pid" ] && [[ "$pid" =~ ^[0-9]+$ ]] && [ "$pid" -gt 1 ] || return 0
  kill -TERM "$pid" 2>/dev/null || true
  local tries=0
  while [ "$tries" -lt 50 ]; do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.1
    tries=$((tries + 1))
  done
  kill -KILL "$pid" 2>/dev/null || true
}

lock_dir() {
  install -d -m 755 "$state_dir"
  pid_file="$state_dir/capture-helper.pid"
  lock_file="$state_dir/capture-helper.lock"
}

acquire_lock_now() {
  lock_dir
  exec 9>"$lock_file"
  flock -n 9
}

acquire_lock_wait() {
  lock_dir
  exec 9>"$lock_file"
  flock -w "$1" 9
}

# /etc/hosts is rewritten next to itself and moved into place, so a kill mid-write cannot truncate it. Mode, owner and
# SELinux label are copied from the old file. Only the helper's exact marker and the mapping directly below it are
# removed: a player's line that merely contains the marker text is kept.
hosts_rewrite() {
  local add="$1" had_file=0 dir tmp
  case "$hosts_file" in
    /*) ;;
    *) die "the hosts path must be absolute: $hosts_file" ;;
  esac
  [ -f "$hosts_file" ] && had_file=1
  if [ "$had_file" = 0 ] && [ "$add" != add ]; then return 0; fi
  dir="$(dirname "$hosts_file")"
  [ -d "$dir" ] || die "the hosts folder is missing: $dir"
  tmp="$(mktemp "$dir/.hosts.hf-capture.XXXXXX")"
  if [ "$had_file" = 1 ]; then
    awk -v marker="$marker" -v mapping="127.0.0.1 $api" '
      $0 == marker { pending = 1; next }
      pending && $0 == mapping { pending = 0; next }
      { pending = 0; print }
    ' "$hosts_file" > "$tmp"
  else
    : > "$tmp"
  fi
  if [ "$add" = add ]; then
    if [ -s "$tmp" ] && [ -n "$(tail -c1 "$tmp")" ]; then
      printf '\n' >> "$tmp"
    fi
    printf '%s\n%s\n' "$marker" "127.0.0.1 $api" >> "$tmp"
  fi
  if [ "$had_file" = 1 ] && cmp -s "$tmp" "$hosts_file"; then
    rm -f "$tmp"
    return 0
  fi
  if [ "$had_file" = 1 ]; then
    chmod --reference="$hosts_file" "$tmp"
    chown --reference="$hosts_file" "$tmp"
  else
    chmod 644 "$tmp"
  fi
  if command -v restorecon >/dev/null 2>&1; then
    restorecon -F "$tmp" >/dev/null 2>&1 || true
  fi
  mv -f -- "$tmp" "$hosts_file"
}

add_hosts() { hosts_rewrite add; }
remove_hosts() { hosts_rewrite remove; }

cleanup_locked() {
  local remove_out="${1:-}"
  set +e
  # Only root's runs own the unit; the unprivileged test modes would wait on a polkit prompt.
  if [ "$EUID" -eq 0 ]; then
    systemctl --no-ask-password stop "$signer_unit.service" >/dev/null 2>&1
    systemctl --no-ask-password reset-failed "$signer_unit.service" >/dev/null 2>&1
  fi
  if [ -n "$capture_pid" ]; then
    kill_pid "$capture_pid"
    wait "$capture_pid" 2>/dev/null
    capture_pid=''
  fi
  if [ -n "$watch_pid" ]; then
    kill_pid "$watch_pid"
    wait "$watch_pid" 2>/dev/null
    watch_pid=''
  fi
  if [ -n "$relay_pid" ]; then
    kill_pid "$relay_pid"
    wait "$relay_pid" 2>/dev/null
    relay_pid=''
  fi
  local pids helper_pid old_relay old_proxy
  pids="$(read_pid_file)"
  if [ -n "$pids" ]; then
    read -r helper_pid old_relay old_proxy < <(printf '%s\n' "$pids")
    # A previous helper killed with SIGKILL can leave its relay. Check the command line first: a stale pid must never
    # kill an unrelated process that reused it.
    if [ "${helper_pid:-}" != "$$" ]; then
      pid_is_relay "${old_relay:-}" && kill_pid "${old_relay:-}"
    fi
  fi
  rm -f "$pid_file"
  remove_hosts
  # The launcher takes the account files before it stops the helper; a helper ending on its own keeps them for it.
  [ -n "$remove_out" ] && rm -rf -- "$state_dir/capture-out" "$signer_run"
  set -e
}

cleanup_once() {
  [ -n "$cleanup_done" ] && return 0
  cleanup_done=1
  cleanup_locked
}

# The relay itself binds as root, drops to nobody (checked) and keeps the listener on loopback. It inherits no lock fd.
start_relay() {
  "$relay_path" 9>&- &
  relay_pid=$!
}

# The capture as the signing service. Its arguments are fixed here; the caller gives only the Steam account and the API's
# address, which the capture's pinned upstream check makes harmless (CaptureUpstreamPins).
start_capture() {
  systemd-run --quiet --wait --collect --unit="$signer_unit" \
    -p DynamicUser=yes -p StateDirectory=hellforge-signer -p StateDirectoryMode=0700 \
    -p RuntimeDirectory=hellforge-capture -p RuntimeDirectoryMode=0755 -p RuntimeDirectoryPreserve=yes \
    -p UMask=0022 -p ProtectHome=yes -p PrivateDevices=yes -p RuntimeMaxSec="$max_lifetime" \
    --setenv=HOME="$signer_run" --setenv=DOTNET_NOLOGO=1 \
    "$proxy_dir/HellForge.CaptureProxy" --host "$api" --timeout-min 10 --sign --steam-account "$1" --shared-evidence \
    --output "$signer_run/private/real-inventory.json" --progression "$signer_run/private/progression.json" \
    --catalogue "$signer_run/private/catalogue" --evidence "$signer_run" --ca-cert "$signer_run/capture-ca.cer" \
    --no-hosts --listen-port 18443 --real-ip "$2" 9>&- &
  capture_pid=$!
}

# Root copies the sealed account files into its own folder, then gives that folder to the player whole: nothing the player
# can change is written as root. seal.json goes last, so the launcher never sees a seal before its files.
hand_over() {
  local from="$signer_run/private" out="$state_dir/capture-out" name
  [ -f "$from/seal.json" ] && [ ! -L "$from/seal.json" ] || return 1
  rm -rf -- "$out"
  install -d -m 700 -o root -g root "$out"
  for name in real-inventory.json progression.json; do
    [ -f "$from/$name" ] && [ ! -L "$from/$name" ] && install -m 600 -o root -g root "$from/$name" "$out/$name"
  done
  if [ -d "$from/catalogue" ] && [ ! -L "$from/catalogue" ]; then
    install -d -m 700 -o root -g root "$out/catalogue"
    for name in "$from/catalogue"/*.json; do
      [ -f "$name" ] && [ ! -L "$name" ] && install -m 600 -o root -g root "$name" "$out/catalogue/"
    done
  fi
  install -m 600 -o root -g root "$from/seal.json" "$out/seal.json"
  publish_key
  chown -hR "$player_uid" "$out"
}

# The signer's public half, where only root writes (SignerPublicKey). Taken from the service's own run folder.
publish_key() {
  local from="$signer_run/snapshot-signer.pub"
  [ -f "$from" ] && [ ! -L "$from" ] && [ "$(stat -c %s "$from")" -lt 4096 ] && grep -q -- '-----BEGIN PUBLIC KEY-----' "$from" || return 0
  install -d -m 755 -o root -g root "$(dirname "$public_key")"
  install -m 644 -o root -g root "$from" "$public_key"
}

watch_capture() {
  while kill -0 "$capture_pid" 2>/dev/null; do
    hand_over && return 0
    sleep 1
  done
}

# A reused proxy pid must not keep the helper alive: the cmdline has to be the capture proxy. The lifetime cap covers a
# proxy that hung after HellForge itself was killed.
watch_proxy() {
  local pid="$1" started now
  started="$(date +%s)"
  while :; do
    pid_is_proxy "$pid" || break
    now="$(date +%s)"
    [ $((now - started)) -lt "$max_lifetime" ] || break
    sleep 1
  done
  kill -TERM "$$" 2>/dev/null || true
}

stop_and_cleanup() {
  local attempt pids helper_pid old_relay old_proxy
  for attempt in 1 2 3; do
    pids="$(read_pid_file)"
    if [ -n "$pids" ]; then
      read -r helper_pid old_relay old_proxy < <(printf '%s\n' "$pids")
      pid_is_helper "$helper_pid" && kill_pid "$helper_pid"
      pid_is_relay "$old_relay" && kill_pid "$old_relay"
    fi
    if acquire_lock_wait 10; then
      cleanup_locked remove-out
      return 0
    fi
  done
  die "another capture helper is still running"
}

parse_test_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --hosts-file) [ $# -ge 2 ] || die "--hosts-file needs a value"; hosts_file="$2"; shift 2 ;;
      --state-dir) [ $# -ge 2 ] || die "--state-dir needs a value"; state_dir="$2"; shift 2 ;;
      --pid) [ $# -ge 2 ] || die "--pid needs a value"; watch_target="$2"; shift 2 ;;
      --seconds) [ $# -ge 2 ] || die "--seconds needs a value"; max_lifetime="$2"; shift 2 ;;
      *) die "unknown test argument: $1" ;;
    esac
  done
  [[ "$max_lifetime" =~ ^[1-9][0-9]*$ ]] || die "--seconds needs a positive number"
}

command="${1:-}"
[ $# -gt 0 ] && shift || true
case "$command" in
  check)
    require_root check
    require_commands
    require_relay
    require_signer
    [ -w /etc/hosts ] || die "/etc/hosts is not writable"
    exit 0
    ;;
  start)
    require_root start
    require_commands
    require_relay
    require_signer
    [ $# -eq 0 ] || die "start takes no arguments; it reads the Steam account and the API's address from stdin"
    steam_account='' real_ip='' extra=''
    IFS=' ' read -r steam_account real_ip extra || true
    [[ "$steam_account" =~ ^[0-9]{1,20}$ ]] || die "start reads the Steam account (SteamID64) first on stdin"
    [[ "$real_ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "start reads the API's IPv4 address second on stdin"
    [ -z "$extra" ] || die "start reads two values on stdin"
    player_uid="${SUDO_UID:-}"
    [[ "$player_uid" =~ ^[0-9]+$ ]] && [ "$player_uid" -gt 0 ] || die "start runs through sudo from the player's account"
    acquire_lock_now || die "another capture helper is running"
    trap 'cleanup_once' EXIT
    trap 'cleanup_once; exit 0' INT TERM HUP
    cleanup_locked remove-out
    add_hosts
    start_relay
    start_capture "$steam_account" "$real_ip"
    printf '%s %s %s\n' "$$" "$relay_pid" "$capture_pid" > "$pid_file"
    chmod 644 "$pid_file"
    watch_capture &
    watch_pid=$!
    set +e
    wait "$capture_pid"
    status=$?
    capture_pid=''
    wait "$watch_pid" 2>/dev/null
    watch_pid=''
    [ -f "$state_dir/capture-out/seal.json" ] || hand_over
    set -e
    cleanup_once
    exit "$status"
    ;;
  stop)
    require_root stop
    [ $# -eq 0 ] || die "stop takes no arguments"
    stop_and_cleanup
    exit 0
    ;;
  cleanup)
    require_root cleanup
    [ $# -eq 0 ] || die "cleanup takes no arguments"
    stop_and_cleanup
    exit 0
    ;;
  hosts-add|hosts-remove)
    watch_target=''
    parse_test_args "$@"
    acquire_lock_now || die "another capture helper is running"
    if [ "$command" = hosts-add ]; then add_hosts; else remove_hosts; fi
    exit 0
    ;;
  watch)
    watch_target=''
    parse_test_args "$@"
    [ -n "$watch_target" ] || die "watch needs --pid"
    [[ "$watch_target" =~ ^[0-9]+$ ]] && [ "$watch_target" -gt 1 ] || die "watch needs a valid pid"
    pid_is_proxy "$watch_target" || die "the watched process is not the capture proxy: $watch_target"
    acquire_lock_now || die "another capture helper is running"
    trap 'cleanup_once' EXIT
    trap 'cleanup_once; exit 0' INT TERM HUP
    cleanup_locked
    add_hosts
    printf '%s 0 %s\n' "$$" "$watch_target" > "$pid_file"
    watch_proxy "$watch_target" &
    watch_pid=$!
    set +e
    wait "$watch_pid"
    set -e
    cleanup_once
    exit 0
    ;;
  *)
    die "usage: $0 check | start | stop | cleanup"
    ;;
esac
