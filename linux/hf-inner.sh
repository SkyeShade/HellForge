#!/usr/bin/env bash
# From DreamyBranch's LinuxSupport (SkyeShade/HellForge issue 1), with joshua-software-dev; docs/linux.md.
# Run INSIDE the offline wrapper (hellforge-linux.sh does: hellforge-offline.sh hf-inner.sh).
# For every socket made by hf-bridge-host.sh it opens the same loopback TCP port inside the
# namespace and forwards it to Steam. Then it starts HellForge with Proton: HellForge.exe, or the Windows program and
# arguments it was given (another launcher's HellForge.Cli play, docs/external-launchers.md).
# Needs PROTON, INSTALL, STEAM_COMPAT_DATA_PATH and STEAM_COMPAT_CLIENT_INSTALL_PATH in the environment.

# Steam's shader log prefixes every message with a timestamp. grep filters the growing log in C; only its Setting
# lines are parsed here, and each logged cache path is resolved before it is compared with ours.
shader_cache_read_only() {
  local shader_dir=$1 log=$2 read_only="steam_cache,steam_precompiled"
  local shader_real line cache_dir cache_real value
  shader_real=$(readlink -m -- "$shader_dir" 2>/dev/null || printf '%s\n' "$shader_dir")
  [ -f "$log" ] || { printf '%s\n' "$read_only"; return 0; }
  while IFS= read -r line; do
    line=${line#*] }
    case "$line" in
      "Setting MESA_GLSL_CACHE_DIR="*" MESA_DISK_CACHE_READ_ONLY_FOZ_DBS="*) ;;
      *) continue ;;
    esac
    cache_dir=${line#Setting MESA_GLSL_CACHE_DIR=}
    cache_dir=${cache_dir%% MESA_DISK_CACHE_READ_ONLY_FOZ_DBS=*}
    cache_real=$(readlink -m -- "$cache_dir" 2>/dev/null || printf '%s\n' "$cache_dir")
    [ "$cache_real" = "$shader_real" ] || continue
    value=${line#*MESA_DISK_CACHE_READ_ONLY_FOZ_DBS=}
    value=${value%%[[:space:]]*}
    read_only=${value:-steam_cache,steam_precompiled}
  done < <(grep -F -- "] Setting MESA_GLSL_CACHE_DIR=" "$log" 2>/dev/null || true)
  printf '%s\n' "$read_only"
}

# The G.1 test sources this file to run the parser against a real log; a normal run continues below.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then return 0; fi
set -euo pipefail
cleanup() {
  local status=$?
  echo "hf-inner exiting with code $status." >&2
  kill $(jobs -p) 2>/dev/null || true
}
trap cleanup EXIT
trap 'echo "hf-inner received SIGINT." >&2; exit 130' INT
trap 'echo "hf-inner received SIGTERM." >&2; exit 143' TERM
command -v socat >/dev/null || { echo "need socat" >&2; exit 1; }
: "${PROTON:?PROTON is not set}" "${INSTALL:?INSTALL is not set}"
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# Steam points the GPU driver, Mesa, DXVK and Fossilize at a per-app cache in the library that holds HELLDIVERS 2.
# Starting Proton directly would otherwise compile into the driver's default cache instead of reusing Steam's.
set_default() {
  local name=$1 value=$2
  [[ -v $name ]] || export "$name=$value"
}

steam_shader_caches() {
  local steam=$STEAM_COMPAT_CLIENT_INSTALL_PATH
  local libraries=("$steam") library library_path shader_dir="" missing_library=""
  local line read_only data_home layer_manifest layer_library
  local filter="$steam/fossilize_engine_filters.json"

  if [ -f "$steam/steamapps/libraryfolders.vdf" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      line=${line%$'\r'}
      if [[ $line =~ ^[[:space:]]*\"path\"[[:space:]]*\"(.*)\"[[:space:]]*$ ]]; then
        library_path=${BASH_REMATCH[1]}
        [ -n "$library_path" ] && libraries+=("$library_path")
      fi
    done < "$steam/steamapps/libraryfolders.vdf"
  fi

  for library in "${libraries[@]}"; do
    [ -f "$library/steamapps/appmanifest_553850.acf" ] || continue
    if [ -d "$library/steamapps/shadercache/553850" ]; then
      shader_dir="$library/steamapps/shadercache/553850"
      break
    fi
    [ -n "$missing_library" ] || missing_library=$library
  done
  if [ -z "$shader_dir" ]; then
    if [ -n "$missing_library" ]; then
      echo "hf-inner: Steam's HELLDIVERS 2 shader cache is missing under $missing_library; shader pre-caching may be off." >&2
    else
      echo "hf-inner: no Steam library has HELLDIVERS 2's appmanifest; Steam's shader caches are not used." >&2
    fi
    return 0
  fi
  echo "hf-inner: using Steam's HELLDIVERS 2 shader caches at $shader_dir." >&2

  read_only=$(shader_cache_read_only "$shader_dir" "$steam/logs/shader_log.txt")

  set_default STEAM_COMPAT_SHADER_PATH "$shader_dir"
  set_default __GL_SHADER_DISK_CACHE_PATH "$shader_dir/nvidiav1"
  set_default __GL_SHADER_DISK_CACHE_APP_NAME steamapp_shader_cache
  set_default __GL_SHADER_DISK_CACHE_READ_ONLY_APP_NAME 'steam_shader_cache;steamapp_merged_shader_cache'
  set_default __GL_SHADER_DISK_CACHE_SKIP_CLEANUP 1
  set_default MESA_SHADER_CACHE_DIR "$shader_dir"
  set_default MESA_GLSL_CACHE_DIR "$shader_dir"
  set_default MESA_SHADER_CACHE_MAX_SIZE 5G
  set_default MESA_GLSL_CACHE_MAX_SIZE 5G
  set_default MESA_DISK_CACHE_SINGLE_FILE 1
  set_default MESA_DISK_CACHE_READ_ONLY_FOZ_DBS "$read_only"
  set_default AMD_VK_PIPELINE_CACHE_PATH "$shader_dir/AMDv1"
  set_default AMD_VK_PIPELINE_CACHE_FILENAME steamapp_shader_cache
  set_default DXVK_STATE_CACHE_PATH "$shader_dir/DXVK_state_cache"

  if [[ -v DISABLE_VK_LAYER_VALVE_steam_fossilize_1 ]]; then
    echo "hf-inner: Fossilize recording is disabled by DISABLE_VK_LAYER_VALVE_steam_fossilize_1." >&2
    return 0
  fi
  data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
  layer_manifest="$data_home/vulkan/implicit_layer.d/steamfossilize_x86_64.json"
  if [ ! -f "$filter" ]; then
    echo "hf-inner: Fossilize recording is off because Steam's filter $filter was not found." >&2
    return 0
  fi
  if [ ! -f "$layer_manifest" ]; then
    echo "hf-inner: Fossilize recording is off because the Vulkan layer manifest $layer_manifest was not found." >&2
    return 0
  fi
  layer_library=$(sed -n -E 's/.*"library_path"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' "$layer_manifest" | tail -n 1)
  if [ -z "$layer_library" ]; then
    echo "hf-inner: Fossilize recording is off because $layer_manifest does not name a library_path." >&2
    return 0
  fi
  case "$layer_library" in
    /*) ;;
    *) layer_library="$(dirname -- "$layer_manifest")/$layer_library" ;;
  esac
  if [ ! -f "$layer_library" ]; then
    echo "hf-inner: Fossilize recording is off because the Vulkan layer library $layer_library was not found." >&2
    return 0
  fi
  set_default ENABLE_VK_LAYER_VALVE_steam_fossilize_1 1
  set_default STEAM_FOSSILIZE_DUMP_PATH "$shader_dir/fozpipelinesv6/steamapprun_pipeline_cache"
  set_default STEAM_FOSSILIZE_DUMP_PATH_READ_ONLY '$bucketdir/steam_pipeline_cache.foz;$bucketdir/steamapp_pipeline_cache.foz'
  set_default FOSSILIZE_APPLICATION_INFO_FILTER_PATH "$filter"
}

n=0
echo "hf-inner: setting up Steam port forwarding." >&2
for f in "$dir"/hf-steam-*.sock; do
  [ -S "$f" ] || continue
  p=${f##*hf-steam-}; p=${p%.sock}
  socat TCP-LISTEN:"$p",bind=127.0.0.1,fork,reuseaddr,nodelay UNIX-CONNECT:"$f" &
  echo "forwarding 127.0.0.1:$p -> Steam" >&2
  n=$((n+1))
done
[ "$n" -gt 0 ] || { echo "no bridge sockets found; start hf-bridge-host.sh first" >&2; exit 1; }
sleep 1
if [ -n "${HELLFORGE_WORKING_DIRECTORY:-}" ]; then cd "$HELLFORGE_WORKING_DIRECTORY"; fi
# Which commands start a session runner: HellForge.Session.exe play, and HellForge.Cli.exe play unless --dry-run yes
# (src/HellForge.Cli/Launch.cs starts the runner only for play without a dry run). Every other command just passes
# Proton's output and exit code through, as an external launcher expects (docs/external-launchers.md).
session_run=0
cli_run=0
program="${1:-}"
case "${program,,}" in
  *hellforge.cli.exe) cli_run=1 ;;
esac
if [ "${2:-}" = play ]; then
  case "${program,,}" in
    *hellforge.session.exe) session_run=1 ;;
    *hellforge.cli.exe)
      session_run=1
      for ((i = 3; i < $#; i++)); do
        next=$((i + 1))
        if [ "${!i}" = --dry-run ] && [ "${!next}" = yes ]; then session_run=0; fi
      done
      ;;
  esac
fi
session_file="$INSTALL/logs/play-session.json"
previous_run=""
read_run_id() {
  [ -f "$session_file" ] || return 0
  sed -n 's/.*"runId"[[:space:]]*:[[:space:]]*"\([[:xdigit:]-]*\)".*/\1/p' "$session_file" | head -n 1
}
if [ "$session_run" = 1 ]; then
  previous_run="$(read_run_id)"
fi
program="${1:-$INSTALL/HellForge.exe}"
if [ "$session_run" = 1 ]; then steam_shader_caches; fi
# Proton itself is bounded as well as the session wait: a runner stuck in preparation must not live forever when an
# external launcher (with no native SessionRunner.Kill) waits for it. HELLFORGE_PROTON_TIMEOUT is in seconds.
wait_timeout="${HELLFORGE_WAIT_TIMEOUT:-43200}"
[[ "$wait_timeout" =~ ^[0-9]+$ ]] || { echo "hf-inner: HELLFORGE_WAIT_TIMEOUT must be a number of seconds." >&2; exit 1; }
proton_timeout="${HELLFORGE_PROTON_TIMEOUT:-$wait_timeout}"
[[ "$proton_timeout" =~ ^[0-9]+$ ]] || { echo "hf-inner: HELLFORGE_PROTON_TIMEOUT must be a number of seconds." >&2; exit 1; }
echo "hf-inner: launching $(basename -- "$program") through Proton." >&2
proton_status=0
if [ $# -gt 0 ]; then timeout -k 10 "$proton_timeout" "$PROTON" run "$@" || proton_status=$?; else timeout -k 10 "$proton_timeout" "$PROTON" run "$program" || proton_status=$?; fi
if [ "$proton_status" = 124 ]; then
  echo "hf-inner: Proton did not return within ${proton_timeout} seconds; timeout ended it." >&2
fi
echo "Proton returned exit code $proton_status." >&2

# A command that does not start a session is a pass-through: Proton's stdout, stderr and exit code belong to the caller.
if [ "$session_run" != 1 ] && [ "$cli_run" = 1 ]; then
  exit "$proton_status"
fi

# "proton run" returns as soon as the launcher has started, but the launcher keeps running (and HellForge.Cli play
# returns once the session has started).
# Only a command that starts a session waits for the run record; the default HellForge.exe launch waits for the
# installation's processes and every other explicit program keeps its Proton result. A session writes completedUtc only
# after its game job and backend are stopped and cleanup is done. Use that per-run record while the session runner is
# alive, rather than process-name scans alone: bash wrapper command lines contain the Windows exe argument, and
# independent Proton probes can otherwise keep an unrelated session's bridge alive forever. If the runner exits without
# writing completedUtc, wait only for a game or backend it left behind. The overall wait is bounded by
# HELLFORGE_WAIT_TIMEOUT seconds (default 12 hours).
if [ "${HELLFORGE_WAIT_FOR_PROCESSES:-1}" != 0 ]; then
  wait_started=$SECONDS
  # Processes of this installation only. The wrapper and its parents have the Windows exe path in their arguments, so
  # leave them out. Wine shows Windows paths (with backslashes) in command lines; the environment works for both forms.
  wrapper_pids=" $$"
  parent=$PPID
  while [[ "$parent" =~ ^[0-9]+$ ]] && [ "$parent" -gt 1 ]; do
    wrapper_pids+=" $parent"
    parent="$(ps -o ppid= -p "$parent" 2>/dev/null | tr -d '[:space:]')"
  done
  windows_install="Z:${INSTALL//\//\\}"
  case "$INSTALL" in
    */pfx/drive_c/*)
      windows_install="C:\\${INSTALL#*/pfx/drive_c/}"
      windows_install="${windows_install//\//\\}"
      ;;
  esac
  of_this_install() {
    local pid=$1 text
    case " $wrapper_pids " in *" $pid "*) return 1 ;; esac
    text="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null || true)"
    case "$text" in *"$INSTALL"*|*"$windows_install"*) return 0 ;; esac
    text="$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null || true)"
    case $'\n'"$text"$'\n' in *$'\n'"INSTALL=$INSTALL"$'\n'*) return 0 ;; esac
    case "$(readlink "/proc/$pid/cwd" 2>/dev/null || true)" in "$INSTALL"|"$INSTALL"/*) return 0 ;; esac
    return 1
  }
  matching_process() {
    local pattern=$1 pid
    while IFS= read -r pid; do
      [ -n "$pid" ] && of_this_install "$pid" && return 0
    done < <(pgrep -u "$(id -u)" -f "$pattern" 2>/dev/null || true)
    return 1
  }
  timed_out() { [ $((SECONDS - wait_started)) -ge "$wait_timeout" ]; }

  if [ "$session_run" = 1 ]; then
    run_id=""
    for _ in $(seq 1 60); do
      run_id="$(read_run_id)"
      if [[ "$run_id" =~ ^[[:xdigit:]]{32}$ ]] && [ "$run_id" != "$previous_run" ]; then break; fi
      run_id=""
      sleep 1
    done
    if [ -z "$run_id" ]; then
      echo "hf-inner: the session runner did not publish a new run record within 60 seconds; cannot confirm session cleanup (Proton exit code $proton_status)." >&2
      exit 1
    fi
    echo "hf-inner: session runner published run $run_id; waiting for its result record." >&2
    result="$INSTALL/logs/sessions/$run_id/result.json"
    runner_pat='HellForge\.(Session|Cli)\.exe'
    leftover_pat='HellForge\.LocalBackend|helldivers2'
    runner_gone_at=""
    while :; do
      if [ -f "$result" ] && grep -q '"completedUtc"[[:space:]]*:' "$result"; then
        echo "hf-inner: session run $run_id completed." >&2
        break
      fi
      if matching_process "$runner_pat"; then
        runner_gone_at=""
      elif ! matching_process "$leftover_pat"; then
        [ -n "$runner_gone_at" ] || runner_gone_at=$SECONDS
        if [ $((SECONDS - runner_gone_at)) -ge 10 ]; then
          echo "hf-inner: the session runner exited without writing completedUtc and no game or backend process of this installation remains; stopped waiting for run $run_id." >&2
          break
        fi
      else
        runner_gone_at=""
      fi
      if timed_out; then
        echo "hf-inner: stopped waiting for session run $run_id after ${wait_timeout} seconds without a completedUtc record; the session runner or one of its processes is still running." >&2
        break
      fi
      sleep 2
    done
  else
    pat='HellForge\.(Session|Cli|SteamProbe)|LocalBackend|helldivers2'
    for _ in $(seq 1 30); do matching_process "$pat" && break; sleep 1; done
    while matching_process "$pat"; do
      if timed_out; then
        echo "hf-inner: stopped waiting for this installation's processes after ${wait_timeout} seconds; one is still running." >&2
        break
      fi
      sleep 2
    done
  fi
fi
exit "$proton_status"
