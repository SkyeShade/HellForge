#!/usr/bin/env bash
# From DreamyBranch's LinuxSupport (SkyeShade/HellForge issue 1), with joshua-software-dev; docs/linux.md.
# Run INSIDE the offline wrapper (hellforge-linux.sh does: hellforge-offline.sh hf-inner.sh).
# For every socket made by hf-bridge-host.sh it opens the same loopback TCP port inside the
# namespace and forwards it to Steam with HellForge's native relay (steam-bridge inner). Then it starts HellForge with
# Proton: HellForge.exe, or the Windows program and arguments it was given (another launcher's HellForge.Cli play,
# docs/external-launchers.md).
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
# The native supervisor compares host paths byte for byte. Resolve parent links (Fedora Atomic's /home -> /var/home)
# before the publication and request are written; the trusted Windows side already gets these resolved paths.
INSTALL="$(readlink -f -- "$INSTALL")"
STEAM_COMPAT_DATA_PATH="$(readlink -f -- "$STEAM_COMPAT_DATA_PATH")"
cleanup() {
  local status=$?
  echo "hf-inner exiting with code $status." >&2
  kill $(jobs -p) 2>/dev/null || true
}
trap cleanup EXIT
trap 'echo "hf-inner received SIGINT." >&2; exit 130' INT
trap 'echo "hf-inner received SIGTERM." >&2; exit 143' TERM
: "${PROTON:?PROTON is not set}" "${INSTALL:?INSTALL is not set}"
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
cliNative="$INSTALL/app/cli-linux/HellForge.Cli"
[ -f "$cliNative" ] || { echo "hf-inner: the native Linux build agent is missing ($cliNative): reinstall HellForge." >&2; exit 1; }
[ -x "$cliNative" ] || chmod +x "$cliNative" 2>/dev/null || true
[ -x "$cliNative" ] || { echo "hf-inner: the native Linux build agent is not executable: $cliNative" >&2; exit 1; }
# Resolve before HELLFORGE_WORKING_DIRECTORY can change the working directory, so a relative PROTON still finds its
# wineserver after a timeout.
proton_dir="$(cd "$(dirname -- "$PROTON")" && pwd)"

# The wrapper resolved the runtime and exported its fields. hf-inner reports the same mode for its own log and, in
# container mode, builds the entry-point argv from those exported commandlines instead of starting Proton on the host.
runtime_helper="$INSTALL/app/linux/hf-steam.sh"
if [ -f "$runtime_helper" ]; then . "$runtime_helper"; fi

hf_inner_runtime_report() {
  case "${HELLFORGE_STEAM_RUNTIME_MODE:-}" in
    container)
      printf 'hf-inner: Steam Linux Runtime: container "%s" %s at %s.\n' \
        "${HELLFORGE_STEAM_RUNTIME_NAME:-}" "${HELLFORGE_STEAM_RUNTIME_VERSION:-}" \
        "${HELLFORGE_STEAM_RUNTIME_FOLDER:-}" >&2
      ;;
    host)
      printf 'hf-inner: Steam Linux Runtime: host mode: %s.\n' \
        "${HELLFORGE_STEAM_RUNTIME_REASON:-unknown}" >&2
      ;;
  esac
}

hf_inner_proton_command() {
  local -a tail_args=("$@")
  HF_INNER_COMMAND=()
  if [ "${HELLFORGE_STEAM_RUNTIME_MODE:-}" != container ]; then
    if [ "${HELLFORGE_STEAM_RUNTIME_MODE:-}" = host ] && [ -f "$runtime_helper" ]; then
      HF_RUNTIME_MODE=host
      HF_RUNTIME_PROTON="${HELLFORGE_STEAM_RUNTIME_PROTON:-$PROTON}"
      HF_RUNTIME_PROTON_COMMANDLINE="${HELLFORGE_STEAM_RUNTIME_PROTON_COMMANDLINE:-}"
      hf_runtime_arguments run "${tail_args[@]}"
      HF_INNER_COMMAND=("${HF_RUNTIME_ARGV[@]}")
    else
      HF_INNER_COMMAND=("$PROTON" run "${tail_args[@]}")
    fi
    return 0
  fi
  if [ ! -f "$runtime_helper" ]; then
    echo "hf-inner: the Steam Linux Runtime commandline helper is missing ($runtime_helper); the session cannot start." >&2
    return 1
  fi
  HF_RUNTIME_MODE=container
  HF_RUNTIME_ENTRYPOINT="${HELLFORGE_STEAM_RUNTIME_ENTRYPOINT:-}"
  HF_RUNTIME_COMMANDLINE="${HELLFORGE_STEAM_RUNTIME_COMMANDLINE:-}"
  HF_RUNTIME_PROTON="${HELLFORGE_STEAM_RUNTIME_PROTON:-$PROTON}"
  HF_RUNTIME_PROTON_COMMANDLINE="${HELLFORGE_STEAM_RUNTIME_PROTON_COMMANDLINE:-}"
  if [ -z "$HF_RUNTIME_ENTRYPOINT" ] || [ -z "$HF_RUNTIME_COMMANDLINE" ] ||
     [ -z "$HF_RUNTIME_PROTON_COMMANDLINE" ]; then
    echo "hf-inner: the Steam Linux Runtime mode was selected without its commandlines; the session cannot start." >&2
    return 1
  fi
  hf_runtime_arguments run "${tail_args[@]}"
  HF_INNER_COMMAND=("${HF_RUNTIME_ARGV[@]}")
}

hf_inner_runtime_report

# Processes of this installation only. The wrapper and its parents have the Windows exe path in their arguments, so leave
# them out. Wine shows Windows paths (with backslashes) in command lines; the environment works for both forms. This is
# also the scan a Proton timeout uses to end what timeout(1) left behind (H.9).
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
  text="$({ tr '\0' '\n' < "/proc/$pid/cmdline"; } 2>/dev/null || true)"
  case "$text" in *"$INSTALL"*|*"$windows_install"*) return 0 ;; esac
  text="$({ tr '\0' '\n' < "/proc/$pid/environ"; } 2>/dev/null || true)"
  case $'\n'"$text"$'\n' in *$'\n'"INSTALL=$INSTALL"$'\n'*) return 0 ;; esac
  case "$(readlink "/proc/$pid/cwd" 2>/dev/null || true)" in "$INSTALL"|"$INSTALL"/*) return 0 ;; esac
  return 1
}
matching_pids() {
  local pattern=$1 pid
  while IFS= read -r pid; do
    [ -n "$pid" ] && of_this_install "$pid" && printf '%s\n' "$pid"
  done < <(pgrep -u "$(id -u)" -f "$pattern" 2>/dev/null || true)
  return 0
}
matching_process() {
  local found
  found="$(matching_pids "$1")"
  [ -n "$found" ]
}

# Proton uses $STEAM_COMPAT_DATA_PATH/pfx; WINEPREFIX is only the fallback (as in hellforge-offline.sh).
if [ -n "${STEAM_COMPAT_DATA_PATH:-}" ]; then wineprefix="$STEAM_COMPAT_DATA_PATH/pfx"; else wineprefix="${WINEPREFIX:-}"; fi

find_wineserver() {
  local candidate
  for candidate in "$proton_dir/files/bin/wineserver" "$proton_dir/dist/bin/wineserver" "$proton_dir/bin/wineserver"; do
    [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
  done
  return 1
}

# timeout(1) ends Proton's script; a game that script started can outlive it in the prefix's wineserver. TERM the matching
# process, give it five seconds, then KILL and wait five more. The whole scan is bounded to ten seconds.
kill_leftovers() {
  local pattern=$1 round
  local -a victims=()
  for round in 1 2; do
    mapfile -t victims < <(matching_pids "$pattern")
    [ "${#victims[@]}" -gt 0 ] || return 0
    if [ "$round" = 1 ]; then
      kill -TERM "${victims[@]}" 2>/dev/null || true
    else
      kill -KILL "${victims[@]}" 2>/dev/null || true
    fi
    for _ in $(seq 1 50); do
      mapfile -t victims < <(matching_pids "$pattern")
      [ "${#victims[@]}" -gt 0 ] || return 0
      sleep 0.1
    done
  done
  echo "hf-inner: these session processes did not end after the Proton timeout: ${victims[*]}" >&2
  return 0
}

end_timed_out_session() {
  local wineserver
  echo "hf-inner: ending the session's Wine processes after the Proton timeout." >&2
  if [ -n "$wineprefix" ] && [ -d "$wineprefix" ]; then
    if wineserver="$(find_wineserver)"; then
      WINEPREFIX="$wineprefix" timeout -k 2 10 "$wineserver" -k >/dev/null 2>&1 || true
    else
      echo "hf-inner: Proton's wineserver was not found; ending the session's leftover processes instead." >&2
    fi
  fi
  kill_leftovers 'HellForge\.(Session|Cli|SteamProbe)|LocalBackend|helldivers2'
}

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
  n=$((n+1))
done
[ "$n" -gt 0 ] || { echo "no bridge sockets found; start hf-bridge-host.sh first" >&2; exit 1; }
"$cliNative" steam-bridge inner &
bridge_pid=$!
sleep 1
kill -0 "$bridge_pid" 2>/dev/null || { echo "hf-inner: the Steam bridge relay did not start; Steam's loopback ports are not bridged." >&2; exit 1; }
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
# An explicit --sandbox on either session command, or HELLFORGE_SANDBOX=1 from SessionRunner, asks for the Linux domain.
# The flag has no value for HellForge.Session.exe and takes yes/no for HellForge.Cli.exe.
sandbox_requested=0
if [ "${HELLFORGE_SANDBOX:-}" = 1 ]; then sandbox_requested=1; fi
for ((i = 1; i <= $#; i++)); do
  if [ "${!i}" != --sandbox ]; then continue; fi
  next=$((i + 1))
  if [ "$next" -le $# ] && [ "${!next}" = no ]; then sandbox_requested=0; else sandbox_requested=1; fi
done
session_file="$INSTALL/logs/play-session.json"
previous_run=""
read_run_id() {
  [ -f "$session_file" ] || return 0
  sed -n 's/.*"runId"[[:space:]]*:[[:space:]]*"\([[:xdigit:]-]*\)".*/\1/p' "$session_file" | head -n 1
}
if [ "$session_run" = 1 ]; then
  previous_run="$(read_run_id)"
fi
# A sandboxed game lives in the overlay prefix's own wineserver, which H.9's real-prefix cleanup cannot reach. The
# supervisor owns that domain. Ask it through the same stop-observation signal STOP uses (it writes the domain stop file
# and then TERM/KILLs the domain group), wait for it here, and only then run H.9's real-prefix cleanup.
stop_sandbox_domain_after_timeout() {
  [ "$session_run" = 1 ] && [ "$sandbox_requested" = 1 ] && [ -n "$supervisor_pid" ] || return 0
  local run_id="" signal_file=""
  for _ in $(seq 1 50); do
    run_id="$(read_run_id)"
    if [[ "$run_id" =~ ^[[:xdigit:]]{32}$ ]] && [ "$run_id" != "$previous_run" ]; then break; fi
    run_id=""
    sleep 0.1
  done
  if [ -z "$run_id" ]; then
    echo "hf-inner: the sandboxed session did not publish a new run before the Proton timeout; there is no domain stop file to write." >&2
    return 0
  fi
  signal_file="$INSTALL/logs/sessions/$run_id/stop-observation.signal"
  echo "hf-inner: asking the native sandbox supervisor to stop run $run_id after the Proton timeout." >&2
  if ! printf 'stop\n' >"$signal_file" 2>/dev/null; then
    echo "hf-inner: could not write the sandbox domain stop signal at $signal_file." >&2
    return 0
  fi
  for _ in $(seq 1 300); do
    kill -0 "$supervisor_pid" 2>/dev/null || return 0
    sleep 0.1
  done
  echo "hf-inner: the native sandbox supervisor did not stop the domain within 30 seconds after the Proton timeout." >&2
}
program="${1:-$INSTALL/HellForge.exe}"
if [ "$session_run" = 1 ]; then steam_shader_caches; fi
# Proton itself is bounded as well as the session wait: a runner stuck in preparation must not live forever when an
# external launcher (with no native SessionRunner.Kill) waits for it. HELLFORGE_PROTON_TIMEOUT is in seconds.
wait_timeout="${HELLFORGE_WAIT_TIMEOUT:-43200}"
[[ "$wait_timeout" =~ ^[0-9]+$ ]] || { echo "hf-inner: HELLFORGE_WAIT_TIMEOUT must be a number of seconds." >&2; exit 1; }
proton_timeout="${HELLFORGE_PROTON_TIMEOUT:-$wait_timeout}"
[[ "$proton_timeout" =~ ^[0-9]+$ ]] || { echo "hf-inner: HELLFORGE_PROTON_TIMEOUT must be a number of seconds." >&2; exit 1; }
# The wrapper's self-test is authoritative: a kernel, mount, AppArmor or package change since setup must refuse the
# session, not surprise it halfway through. It never starts Proton or the game.
if [ "$session_run" = 1 ] && [ "$sandbox_requested" = 1 ]; then
  preflight="$INSTALL/logs/sandbox-preflight.txt"
  mkdir -p "$INSTALL/logs"
  echo "hf-inner: checking the Linux sandbox before the session." >&2
  if ! (unset HELLFORGE_WRAPPER_LOG; bash "$INSTALL/app/linux/hellforge-linux.sh" --sandbox-self-test) >"$preflight" 2>&1; then
    cat "$preflight" >&2
    echo "hf-inner: the Linux sandbox could not start: the self-test failed. The game was not started. To run without the sandbox, turn off Settings > Sandbox." >&2
    exit 1
  fi
fi
# The native supervisor is a sibling of Proton and outlives the first wineserver. It validates the runner's request and
# builds the domain; hf-inner must not wait for it here, because the game can run for hours.
supervisor_pid=""
if [ "$session_run" = 1 ] && [ "$sandbox_requested" = 1 ]; then
  cli_native="$INSTALL/app/cli-linux/HellForge.Cli"
  [ ! -f "$cli_native" ] || [ -x "$cli_native" ] || chmod +x "$cli_native" 2>/dev/null || true
  [ -x "$cli_native" ] || { echo "hf-inner: the native Linux agent is missing (app/cli-linux/HellForge.Cli); the sandbox cannot start." >&2; exit 1; }
  supervisor_log="$INSTALL/logs/sandbox-supervisor.log"
  HELLFORGE_PROTON_TIMEOUT="$proton_timeout" "$cli_native" sandbox-supervise --install "$INSTALL" --prefix "$STEAM_COMPAT_DATA_PATH" \
    --session-file "$session_file" --parent-pid "$$" >>"$supervisor_log" 2>&1 &
  supervisor_pid=$!
  sleep 0.5
  if ! kill -0 "$supervisor_pid" 2>/dev/null; then
    wait "$supervisor_pid" 2>/dev/null || true
    echo "hf-inner: the native sandbox supervisor could not start; the game was not started. See $supervisor_log." >&2
    exit 1
  fi
fi
echo "hf-inner: launching $(basename -- "$program") through Proton." >&2
proton_status=0
if [ $# -gt 0 ]; then launch_args=("$@"); else launch_args=("$program"); fi
hf_inner_proton_command "${launch_args[@]}" || exit 1
timeout -k 10 "$proton_timeout" "${HF_INNER_COMMAND[@]}" || proton_status=$?
# timeout(1) returns 124 when its own deadline fired. 137 means its -k then had to KILL the script; the game may still be
# in the prefix either way, so first stop a sandboxed domain through its supervisor, then end the real prefix's
# wineserver and the installation's leftovers.
if [ "$proton_status" = 124 ] || [ "$proton_status" = 137 ]; then
  echo "hf-inner: Proton did not return within ${proton_timeout} seconds; timeout ended it." >&2
  stop_sandbox_domain_after_timeout
  end_timed_out_session
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
# The supervisor answers the trusted runner's copy-back request and then exits. Give it the same bounded ending as the
# bridge; its own parent-death signal handles the path where hf-inner is killed first.
if [ -n "$supervisor_pid" ]; then
  for _ in $(seq 1 60); do
    kill -0 "$supervisor_pid" 2>/dev/null || break
    sleep 1
  done
  if kill -0 "$supervisor_pid" 2>/dev/null; then
    echo "hf-inner: the native sandbox supervisor did not stop within 60 seconds; ending it." >&2
    kill -TERM "$supervisor_pid" 2>/dev/null || true
    for _ in $(seq 1 50); do kill -0 "$supervisor_pid" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$supervisor_pid" 2>/dev/null || true
  fi
  wait "$supervisor_pid" 2>/dev/null || true
fi
exit "$proton_status"
