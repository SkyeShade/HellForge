#!/usr/bin/env bash
# Containers HellForge cannot make its offline network namespace in (Area J). Sourced by hellforge-linux.sh, which asks
# once for every mode, before Steam, Proton or a namespace step. The optional arguments are for the tests: a normal run
# uses the real paths and environment, and nothing in the environment can skip the check.

# The detected container, one line "kind<TAB>name": flatpak (the application id), snap (the snap name),
# pressure-vessel (Steam's runtime container), or other (the container= value). Nothing and status 1 on a plain host.
hf_container_kind() {
  local flatpak_info=${1-/.flatpak-info}
  local manager=${2-/run/host/container-manager}
  local container_value=${3-${container:-}}
  local snap=${4-${SNAP_NAME:-}}
  local app="" manager_name=""
  if [ -n "$flatpak_info" ] && [ -e "$flatpak_info" ]; then
    if [ -r "$flatpak_info" ]; then
      app=$(awk '
        /^\[/ { in_application = ($0 == "[Application]"); next }
        in_application && /^[[:space:]]*name[[:space:]]*=/ {
          line = $0
          sub(/^[[:space:]]*name[[:space:]]*=[[:space:]]*/, "", line)
          print line
          exit
        }' "$flatpak_info" 2>/dev/null || true)
      app=$(printf '%s' "${app%$'\r'}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    fi
    printf 'flatpak\t%s\n' "$app"
    return 0
  fi
  if [ -n "$snap" ]; then
    printf 'snap\t%s\n' "$snap"
    return 0
  fi
  if [ -n "$manager" ] && [ -r "$manager" ]; then
    manager_name=$(tr -d '[:space:]' < "$manager" 2>/dev/null || true)
  fi
  if [ "$container_value" = pressure-vessel ] || [ "$manager_name" = pressure-vessel ]; then
    printf 'pressure-vessel\tpressure-vessel\n'
    return 0
  fi
  if [ -n "$container_value" ]; then
    printf 'other\t%s\n' "$container_value"
    return 0
  fi
  return 1
}

# The reason and the action, one line so the terminal, the launcher and the probe report carry the same words. Status 0
# for a container a session cannot work in, 1 for another container or a plain host.
hf_container_refusal_message() {
  local kind=$1 name=${2-} message
  case $kind in
    flatpak)
      if [ -n "$name" ]; then
        message="HellForge was started inside the Flatpak application \"$name\"."
      else
        message="HellForge was started inside Flatpak."
      fi
      message+=" That container stops HellForge from making the network namespace that keeps the game offline."
      message+=" Start HellForge from its own shortcut, or app/launcher-linux/HellForge.Launcher, on the host instead;"
      message+=" HellForge already finds Flatpak's Steam folder from outside."
      ;;
    snap)
      if [ -n "$name" ]; then
        message="HellForge was started inside the Snap \"$name\"."
      else
        message="HellForge was started inside Snap."
      fi
      message+=" A Snap's confinement can stop HellForge from making the network namespace that keeps the game offline,"
      message+=" and it does not have the host's programs that HellForge uses for it."
      message+=" Start HellForge from its own shortcut, or app/launcher-linux/HellForge.Launcher, on the host instead;"
      message+=" HellForge already finds Snap's Steam folder from outside."
      ;;
    pressure-vessel)
      message="HellForge was started inside Steam's runtime container (pressure-vessel)."
      message+=" HellForge must start on the host: it makes the network namespace that keeps the game offline with the"
      message+=" host's programs, which that container does not have."
      message+=" The Steam entry that started HellForge has a compatibility tool: in its Properties > Compatibility,"
      message+=" untick \"Force the use of a specific Steam Play compatibility tool\"."
      message+=" HellForge's launcher is a native program and runs on the host."
      ;;
    *) return 1 ;;
  esac
  printf '%s\n' "$message"
}

# The refusal for the three containers, or nothing and status 1 when this one can run a session.
hf_container_problem() {
  local detected kind name
  if ! detected=$(hf_container_kind "$@"); then return 1; fi
  kind=${detected%%$'\t'*}
  name=${detected#*$'\t'}
  hf_container_refusal_message "$kind" "$name"
}

# One line for another container, added to an existing namespace failure message: those are not refused, because
# toolbox and distrobox can have the user namespaces a session needs. Nothing and status 1 for anything else.
hf_container_note() {
  local detected kind name
  if ! detected=$(hf_container_kind "$@"); then return 1; fi
  kind=${detected%%$'\t'*}
  name=${detected#*$'\t'}
  [ "$kind" = other ] || return 1
  printf 'HellForge is running inside the container "%s"; if this namespace step failed, the container may not allow unprivileged user and network namespaces. Start HellForge on the host instead.\n' "$name"
}

# The reason the native launcher reads after a wrapper exit, and the setup probe row shows.
hf_container_write_refusal() {
  local install=$1 reason=$2
  [ -n "$install" ] && [ -f "$install/hellforge-install.json" ] || return 0
  mkdir -p -- "$install/logs" 2>/dev/null || return 0
  printf '%s\n' "$reason" > "$install/logs/linux-container-refusal.txt" 2>/dev/null || true
}

# --sandbox-probe's readiness record when the probe could not run: the same reason as firstReason, so the row and the
# report agree. A missing or unwritable report is not fatal to the refusal itself.
hf_container_write_probe_report() {
  local report=$1 reason=$2 escaped timestamp temporary
  [ -n "$report" ] || return 0
  escaped=$(printf '%s' "$reason" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')
  timestamp=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || true)
  mkdir -p -- "$(dirname -- "$report")" 2>/dev/null || return 0
  temporary="$report.tmp"
  if printf '{\n  "schema": 1,\n  "tool": "hellforge-linux.sh",\n  "version": "J.1",\n  "generatedUtc": "%s",\n  "ok": false,\n  "firstReason": "%s",\n  "items": []\n}\n' \
      "$timestamp" "$escaped" > "$temporary" 2>/dev/null && mv -f -- "$temporary" "$report" 2>/dev/null; then
    :
  else
    rm -f -- "$temporary" 2>/dev/null || true
  fi
  return 0
}
