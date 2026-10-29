#!/bin/bash
# HellForge's Sync Inventory helper on Linux (docs/linux.md, "Account sync"). allow-capture.sh installs this file
# root-owned at /usr/local/lib/hellforge/capture-helper and gives the player's account a sudoers rule for this exact
# program. The launcher then starts and stops it without another password prompt.
#
#   capture-helper.sh check
#   capture-helper.sh start              (root; reads the capture proxy pid from stdin)
#   capture-helper.sh stop
#   capture-helper.sh cleanup
#   capture-helper.sh relay --listen-port <1024-65535> --target-port <1024-65535>   (self-test, unprivileged)
#   capture-helper.sh hosts-add --hosts-file <path> [--state-dir <dir>]             (test only, unprivileged)
#   capture-helper.sh hosts-remove --hosts-file <path> [--state-dir <dir>]          (test only, unprivileged)
#   capture-helper.sh watch --pid <pid> --hosts-file <path> [--state-dir <dir>] [--seconds <n>]  (test only)
#
# start adds exactly CaptureHosts.Marker and "127.0.0.1 api.live.prod.thehelldiversgame.com" to /etc/hosts, then runs a
# 443 -> fixed 18443 relay bound to 127.0.0.1 only. The EXIT/TERM/INT/HUP trap removes both. The helper watches the native
# proxy's pid and also ends itself at the proxy's timeout plus a margin, so killing HellForge does not leave the redirect
# behind. cleanup does the same after a killed run. Wine is never started here and this helper never becomes root for the
# game. The test-only host and state arguments are not in the sudoers rule.
set -euo pipefail
# sudo starts this root-owned helper; use only the root-owned system paths even if the caller's PATH differs.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
umask 077

marker='# HellForge-CaptureProxy'
api='api.live.prod.thehelldiversgame.com'
proxy_port=18443
# The proxy's own timeout is 10 minutes; two minutes of margin, and never more than an hour.
max_lifetime=720
state_dir='/run/hellforge'
hosts_file='/etc/hosts'
pid_file="$state_dir/capture-helper.pid"
lock_file="$state_dir/capture-helper.lock"
helper_path='/usr/local/lib/hellforge/capture-helper'
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

require_socat() {
  command -v socat >/dev/null || die "socat is missing (it is one of HellForge's Linux dependencies)."
  id -u nobody >/dev/null 2>&1 || die "the unprivileged 'nobody' account is missing (the relay drops to it after binding)."
}

require_commands() {
  local name
  for name in flock awk mktemp cmp chmod chown mv tail date sleep; do
    command -v "$name" >/dev/null || die "$name is missing."
  done
}

read_pid_file() {
  [ -f "$pid_file" ] || return 0
  # Three numeric pids: the helper first, its socat relay second, the capture proxy third. A two-pid file from an older
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

pid_is_helper() { pid_matches "$1" "$helper_path"; }
pid_is_proxy() { pid_matches "$1" "HellForge.CaptureProxy"; }
pid_is_relay() { pid_matches "$1" "TCP-LISTEN:443,bind=127.0.0.1"; }

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
  set +e
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
  set -e
}

cleanup_once() {
  [ -n "$cleanup_done" ] && return 0
  cleanup_done=1
  cleanup_locked
}

# The helper is root but the relay process that handles game connections runs as nobody; the listener stays on loopback.
start_relay() {
  socat "TCP-LISTEN:443,bind=127.0.0.1,su=nobody,fork,reuseaddr" "TCP:127.0.0.1:$proxy_port" &
  relay_pid=$!
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
      cleanup_locked
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
    require_socat
    require_commands
    [ -w /etc/hosts ] || die "/etc/hosts is not writable"
    exit 0
    ;;
  start)
    require_root start
    require_socat
    require_commands
    [ $# -eq 0 ] || die "start takes no arguments; its proxy pid is read from stdin"
    proxy_pid=''
    IFS= read -r proxy_pid || true
    [[ "$proxy_pid" =~ ^[0-9]+$ ]] && [ "$proxy_pid" -gt 1 ] || die "start reads the capture proxy pid on stdin"
    pid_is_proxy "$proxy_pid" || die "the capture proxy process is not running: $proxy_pid"
    acquire_lock_now || die "another capture helper is running"
    trap 'cleanup_once' EXIT
    trap 'cleanup_once; exit 0' INT TERM HUP
    cleanup_locked
    add_hosts
    start_relay
    printf '%s %s %s\n' "$$" "$relay_pid" "$proxy_pid" > "$pid_file"
    chmod 644 "$pid_file"
    watch_proxy "$proxy_pid" &
    watch_pid=$!
    set +e
    wait "$relay_pid"
    status=$?
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
  relay)
    # The automated relay test uses this on high ports, with no hosts changes. It is not used by the root start mode.
    require_socat
    listen_port=''
    target_port=''
    while [ $# -gt 0 ]; do
      case "$1" in
        --listen-port) [ $# -ge 2 ] || die "--listen-port needs a value"; listen_port="$2"; shift 2 ;;
        --target-port) [ $# -ge 2 ] || die "--target-port needs a value"; target_port="$2"; shift 2 ;;
        *) die "unknown relay argument: $1" ;;
      esac
    done
    [ -n "$listen_port" ] && [ -n "$target_port" ] || die "relay needs --listen-port and --target-port"
    [[ "$listen_port" =~ ^[0-9]+$ ]] && [ "$listen_port" -ge 1024 ] && [ "$listen_port" -le 65535 ] ||
      die "--listen-port needs a port from 1024 to 65535: $listen_port"
    [[ "$target_port" =~ ^[0-9]+$ ]] && [ "$target_port" -ge 1024 ] && [ "$target_port" -le 65535 ] ||
      die "--target-port needs a port from 1024 to 65535: $target_port"
    exec socat "TCP-LISTEN:$listen_port,bind=127.0.0.1,reuseaddr" "TCP:127.0.0.1:$target_port"
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
    die "usage: $0 check | start | stop | cleanup | relay --listen-port <n> --target-port <n>"
    ;;
esac
