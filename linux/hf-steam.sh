#!/usr/bin/env bash
# Steam and Proton lookup shared by hellforge-linux.sh. The same search order is in src/HellForge.Core/SteamLocator.cs:
# every Steam library's steamapps/common/Proton*, the Steam root's compatibilitytools.d, each folder in
# STEAM_EXTRA_COMPAT_TOOLS_PATHS, and the system compatibilitytools.d folders. Sourced, not run.

# The folders inside a home where Steam installs itself. .steam/root and .steam/steam are links Steam keeps to the folder
# it runs from, so they also find an installation elsewhere.
hf_steam_home_paths() {
  local home=$1
  printf '%s\n' \
    "$home/.local/share/Steam" \
    "$home/.steam/debian-installation" \
    "$home/.steam/steam" \
    "$home/.steam/root" \
    "$home/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
    "$home/snap/steam/common/.local/share/Steam"
}

# Steam's folder: the configured one when it holds steamapps, else the first usual folder that does. Links are resolved.
hf_steam_root() {
  local configured=${2:-} path resolved
  if [ -n "$configured" ] && [ -d "$configured/steamapps" ]; then
    resolved=$(readlink -f -- "$configured" 2>/dev/null) || resolved=$configured
    printf '%s\n' "$resolved"
    return 0
  fi
  while IFS= read -r path; do
    [ -d "$path/steamapps" ] || continue
    resolved=$(readlink -f -- "$path" 2>/dev/null) || resolved=$path
    printf '%s\n' "$resolved"
    return 0
  done < <(hf_steam_home_paths "$1")
  return 1
}

# Steam's own folder and every library in libraryfolders.vdf, one per line (the current "path" form and the older
# numbered entries). The current form is found wherever it appears, so a file written on one line works too.
hf_steam_libraries() {
  local steam=$1 file=$1/steamapps/libraryfolders.vdf line path
  printf '%s\n' "$steam"
  [ -f "$file" ] || return 0
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line%$'\r'}
    if [[ $line =~ ^[[:space:]]*\"[0-9]+\"[[:space:]]*\"([^\"]*)\" ]]; then
      path=${BASH_REMATCH[1]}
      [ -n "$path" ] && printf '%s\n' "$path"
    fi
  done < "$file"
  while IFS= read -r path; do
    [ -n "$path" ] && printf '%s\n' "$path"
  done < <(grep -oE '"path"[[:space:]]*"[^"]*"' "$file" | sed -E 's/^"path"[[:space:]]*"([^"]*)"$/\1/')
}

# The same sort key SteamLocator.CompareProton builds: numbered versions first, Proton - Experimental last, then by
# version, by name and by path.
hf_proton_sort_key() {
  local script=$1 folder name experimental version lower
  folder=$(dirname -- "$script")
  name=$(basename -- "$folder")
  lower=${name,,}
  case $lower in
    *experimental*) experimental=1 ;;
    *) experimental=0 ;;
  esac
  # GE-Proton's 10-25 is 10.25, and a bare major number is padded, as in SteamLocator.CompareProton.
  if [[ $name =~ [0-9]+([.-][0-9]+)* ]]; then version=${BASH_REMATCH[0]//-/.}; else version=0; fi
  [[ $version == *.* ]] || version=$version.0
  printf '%s\t%s\t%s\t%s\n' "$experimental" "$version" "$name" "$script"
}

# Every Proton script the search finds, in the default order, one per line. Optional arguments: extra tool folders and
# system tool folders (colon-separated), for the tests; a normal run uses STEAM_EXTRA_COMPAT_TOOLS_PATHS and the system
# folders. A compatibilitytools.d entry counts only with a proton script and a compatibilitytool.vdf.
hf_proton_scripts() {
  local steam=$1
  local extra=${2-${STEAM_EXTRA_COMPAT_TOOLS_PATHS:-}}
  local system=${3-/usr/share/steam/compatibilitytools.d:/usr/local/share/steam/compatibilitytools.d}
  local library folder tools

  {
    while IFS= read -r library; do
      [ -n "$library" ] || continue
      library=$(readlink -f -- "$library" 2>/dev/null) || library=$library
      for folder in "$library"/steamapps/common/Proton*; do
        [ -f "$folder/proton" ] || continue
        hf_proton_sort_key "$folder/proton"
      done
    done < <(hf_steam_libraries "$steam")

    {
      printf '%s\n' "$steam/compatibilitytools.d"
      printf '%s\n' "$extra" | tr ':' '\n'
      printf '%s\n' "$system" | tr ':' '\n'
    } | while IFS= read -r tools; do
      [ -n "$tools" ] || continue
      for folder in "$tools"/*; do
        [ -f "$folder/proton" ] && [ -f "$folder/compatibilitytool.vdf" ] || continue
        hf_proton_sort_key "$folder/proton"
      done
    done
  } | LC_ALL=C sort -t $'\t' -k1,1n -k2,2V -k3,3 -k4 | awk -F '\t' '!seen[$4]++ { print $4 }'
}

# The last one, which is the default (Proton - Experimental when it is installed), or nothing.
hf_default_proton() {
  hf_proton_scripts "$@" | tail -n 1
}

# A Steam folder the player chose, saved in the installation (steam-selection.json). Returns 1 when there is none or it
# no longer holds steamapps, so the usual search runs instead.
hf_saved_steam_root() {
  local file=$1/steam-selection.json root
  [ -f "$file" ] || return 1
  root=$(sed -n -E 's/.*"steamRoot"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' "$file" | head -n 1)
  [ -n "$root" ] && [ -d "$root/steamapps" ] || return 1
  readlink -f -- "$root" 2>/dev/null || printf '%s\n' "$root"
}

# The proton script saved in the installation (proton-selection.json), when it is still executable.
hf_saved_proton() {
  local file=$1/proton-selection.json proton
  [ -f "$file" ] || return 1
  proton=$(sed -n -E 's/.*"scriptPath"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' "$file" | head -n 1)
  [ -n "$proton" ] && [ -x "$proton" ] || return 1
  printf '%s\n' "$proton"
}

# --- Steam Linux Runtime resolver -------------------------------------------------------------------------------
# The same two-manifest chain as SteamRuntimeLocator in src/HellForge.Core: the Proton's toolmanifest.vdf names a
# runtime app through require_tool_appid, that app's own toolmanifest.vdf supplies the entry point commandline, and the
# final argv is <entry point> --verb=<verb> -- <proton commandline with %verb%>. Every failure selects host mode with a
# short reason; a present runtime that fails later is never silently retried on the host.

hf_runtime_reset() {
  HF_RUNTIME_MODE=""
  HF_RUNTIME_REASON=""
  HF_RUNTIME_PROTON=""
  HF_RUNTIME_PROTON_FOLDER=""
  HF_RUNTIME_STEAM=""
  HF_RUNTIME_APPID=""
  HF_RUNTIME_FOLDER=""
  HF_RUNTIME_ENTRYPOINT=""
  HF_RUNTIME_COMMANDLINE=""
  HF_RUNTIME_PROTON_COMMANDLINE=""
  HF_RUNTIME_NAME=""
  HF_RUNTIME_VERSION=""
  HF_RUNTIME_VERSIONS_SHA256=""
  HF_RUNTIME_VERSIONS_READABLE=0
  HF_RUNTIME_IDENTITY=""
  HF_RUNTIME_ARGV=()
}

# First value of a simple quoted KeyValues key in a small Steam file. The files read here have no duplicate keys.
hf_runtime_vdf_value() {
  local file=$1 key=$2
  [ -f "$file" ] || return 1
  sed -n -E 's/.*"'"$key"'"[[:space:]]*"([^"]*)".*/\1/p' "$file" | head -n 1
}

# Whitespace separates tokens and double quotes group them. ValveData has already unescaped \" in the value.
hf_runtime_split_words() {
  local text=${1:-} token="" quote="" char="" i
  for ((i = 0; i < ${#text}; i++)); do
    char=${text:i:1}
    case "$char" in
      '"')
        if [ "$quote" = '"' ]; then quote=""; else quote='"'; fi
        ;;
      ' '|$'\t')
        if [ -z "$quote" ]; then
          if [ -n "$token" ]; then printf '%s\n' "$token"; token=""; fi
        else
          token+=$char
        fi
        ;;
      *) token+=$char ;;
    esac
  done
  [ -z "$token" ] || printf '%s\n' "$token"
}

# A Proton toolmanifest commandline is usable only when its first token names the selected proton script.
hf_runtime_manifest_commandline() {
  local manifest=$1 folder=$2 proton=$3 inner first candidate
  HF_RUNTIME_PROTON_COMMANDLINE=""
  [ -f "$manifest" ] || return 0
  [ "$(hf_runtime_vdf_value "$manifest" version || true)" = "2" ] || return 0
  inner=$(hf_runtime_vdf_value "$manifest" commandline || true)
  [ -n "$inner" ] || return 0
  case "$inner" in *%verb%*) ;; *) return 0 ;; esac
  first=$(hf_runtime_split_words "$inner" | head -n 1)
  case "$first" in /*) ;; *) return 0 ;; esac
  candidate=$(readlink -f -- "$folder/${first#/}" 2>/dev/null || true)
  [ -n "$candidate" ] && [ "$candidate" = "$(readlink -f -- "$proton" 2>/dev/null || true)" ] || return 0
  HF_RUNTIME_PROTON_COMMANDLINE=$inner
}

# The installed folder of a Steam app in one library, from appmanifest_<appid>.acf. The checks match
# LocalInstall.Inspect: appid agrees, installdir cannot escape steamapps/common, and a present StateFlags says installed
# and neither missing, corrupt, running nor busy.
hf_runtime_app_folder() {
  local library=$1 appid=$2 manifest name flags folder busy
  [ -n "$library" ] && [ -n "$appid" ] || return 1
  manifest="$library/steamapps/appmanifest_$appid.acf"
  [ -f "$manifest" ] || return 1
  [ "$(hf_runtime_vdf_value "$manifest" appid || true)" = "$appid" ] || return 1
  name=$(hf_runtime_vdf_value "$manifest" installdir || true)
  case "$name" in
    ''|.|..|*/*|*\\*|*:*|[[:cntrl:]]*) return 1 ;;
  esac
  flags=$(hf_runtime_vdf_value "$manifest" StateFlags || true)
  if [ -n "$flags" ]; then
    case "$flags" in *[!0-9]*) return 1 ;; esac
    [ $((flags & 4)) -ne 0 ] || return 1
    [ $((flags & (32 | 128 | 64))) -eq 0 ] || return 1
    busy=$((256 | 1024 | 2048 | 4096 | 65536 | 131072 | 262144 | 524288 | 1048576 | 2097152 | 4194304 | 8388608))
    [ $((flags & busy)) -eq 0 ] || return 1
  fi
  folder="$library/steamapps/common/$name"
  [ -d "$folder" ] || return 1
  readlink -f -- "$folder"
}

# VERSIONS.txt is a tab-separated table. The row whose Runtime column is set names the runtime; the depot row supplies
# the overall version. A present, readable file that cannot be parsed still counts, with "unknown" as its version.
hf_runtime_versions() {
  local folder=$1 file="$1/VERSIONS.txt" name version hash table runtime depot
  name=$(basename -- "$folder")
  version=unknown
  HF_RUNTIME_VERSIONS_READABLE=0
  HF_RUNTIME_VERSIONS_SHA256=""
  if [ -r "$file" ]; then
    HF_RUNTIME_VERSIONS_READABLE=1
    table=$(awk -F '\t' '
      /^[[:space:]]*#/ { next }
      NF < 2 { next }
      {
        first = $1; gsub(/^[ \t]+|[ \t]+$/, "", first)
        second = $2; gsub(/^[ \t]+|[ \t]+$/, "", second)
        third = $3; gsub(/^[ \t]+|[ \t]+$/, "", third)
        if (!got_depot && tolower(first) == "depot") { depot = second; got_depot = 1 }
        if (!got_runtime && first != "" && third != "") { runtime = first; got_runtime = 1 }
      }
      END { printf "%s\n%s\n", runtime, depot }
    ' "$file" 2>/dev/null || true)
    runtime=$(printf '%s\n' "$table" | sed -n '1p')
    depot=$(printf '%s\n' "$table" | sed -n '2p')
    [ -z "$runtime" ] || name=$runtime
    [ -z "$depot" ] || version=$depot
    if command -v sha256sum >/dev/null 2>&1; then
      hash=$(sha256sum "$file" | awk '{ print toupper($1) }')
    fi
  fi
  HF_RUNTIME_NAME=$name
  HF_RUNTIME_VERSION=$version
  HF_RUNTIME_VERSIONS_SHA256=$hash
  HF_RUNTIME_IDENTITY="$folder|${hash:-no-hash}"
}

hf_runtime_resolve() {
  local proton=${1:-} steam=${2:-} manifest version appid library folder runtime_manifest commandline first
  local relative entry
  hf_runtime_reset
  [ -n "$proton" ] || return 2
  [ -n "$steam" ] || return 2
  HF_RUNTIME_PROTON=$(readlink -f -- "$proton" 2>/dev/null || printf '%s\n' "$proton")
  HF_RUNTIME_STEAM=$steam
  HF_RUNTIME_PROTON_FOLDER=$(cd "$(dirname -- "$HF_RUNTIME_PROTON")" && pwd)
  manifest="$HF_RUNTIME_PROTON_FOLDER/toolmanifest.vdf"

  if [ "${HELLFORGE_STEAM_RUNTIME:-}" = "off" ]; then
    hf_runtime_manifest_commandline "$manifest" "$HF_RUNTIME_PROTON_FOLDER" "$HF_RUNTIME_PROTON"
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="HELLFORGE_STEAM_RUNTIME=off"
    return 0
  fi
  if [ ! -f "$manifest" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="no toolmanifest.vdf beside $(basename -- "$proton")"
    return 0
  fi
  version=$(hf_runtime_vdf_value "$manifest" version || true)
  if [ "$version" != "2" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="unsupported Proton toolmanifest version '${version:-missing}'"
    return 0
  fi
  hf_runtime_manifest_commandline "$manifest" "$HF_RUNTIME_PROTON_FOLDER" "$HF_RUNTIME_PROTON"
  appid=$(hf_runtime_vdf_value "$manifest" require_tool_appid || true)
  if [ -z "$appid" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="the Proton toolmanifest has no require_tool_appid"
    return 0
  fi
  case "$appid" in
    *[!0-9]*)
      HF_RUNTIME_MODE=host
      HF_RUNTIME_REASON="the Proton toolmanifest has a non-numeric require_tool_appid '$appid'"
      return 0
      ;;
  esac

  folder=""
  while IFS= read -r library; do
    [ -n "$library" ] || continue
    if folder=$(hf_runtime_app_folder "$library" "$appid"); then break; fi
    folder=""
  done < <(hf_steam_libraries "$steam" | while IFS= read -r library; do
      [ -n "$library" ] || continue
      readlink -f -- "$library" 2>/dev/null || printf '%s\n' "$library"
    done | LC_ALL=C sort -u)
  if [ -z "$folder" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="runtime app $appid is not installed in any Steam library"
    return 0
  fi

  runtime_manifest="$folder/toolmanifest.vdf"
  if [ ! -f "$runtime_manifest" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="runtime app $appid has no toolmanifest.vdf"
    return 0
  fi
  version=$(hf_runtime_vdf_value "$runtime_manifest" version || true)
  if [ "$version" != "2" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="unsupported runtime toolmanifest version '${version:-missing}' for app $appid"
    return 0
  fi
  if [ -n "$(hf_runtime_vdf_value "$runtime_manifest" require_tool_appid || true)" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="runtime app $appid requires another tool; Steam has one runtime layer"
    return 0
  fi
  commandline=$(hf_runtime_vdf_value "$runtime_manifest" commandline || true)
  if [ -z "$commandline" ] || [[ $commandline != *%verb%* ]]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="runtime app $appid has no commandline containing %verb%"
    return 0
  fi
  first=$(hf_runtime_split_words "$commandline" | head -n 1)
  case "$first" in
    /*) ;;
    *)
      HF_RUNTIME_MODE=host
      HF_RUNTIME_REASON="the runtime commandline does not start with an absolute entry point: $commandline"
      return 0
      ;;
  esac
  relative=${first#/}
  case "/$relative/" in
    *"/../"*|*"/./"*|*"//"*)
      HF_RUNTIME_MODE=host
      HF_RUNTIME_REASON="the runtime entry point path has a traversal: $first"
      return 0
      ;;
  esac
  entry="$folder/$relative"
  if [ ! -x "$entry" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="the runtime entry point is missing or not executable: $entry"
    return 0
  fi
  entry=$(readlink -f -- "$entry" 2>/dev/null || true)
  case "$entry" in
    "$folder"/*) ;;
    *)
      HF_RUNTIME_MODE=host
      HF_RUNTIME_REASON="the runtime entry point resolves outside its app folder: $entry"
      return 0
      ;;
  esac
  if [ -z "$HF_RUNTIME_PROTON_COMMANDLINE" ]; then
    HF_RUNTIME_MODE=host
    HF_RUNTIME_REASON="the Proton toolmanifest has no usable commandline"
    return 0
  fi

  HF_RUNTIME_MODE=container
  HF_RUNTIME_APPID=$appid
  HF_RUNTIME_FOLDER=$folder
  HF_RUNTIME_ENTRYPOINT=$entry
  HF_RUNTIME_COMMANDLINE=$commandline
  hf_runtime_versions "$folder"
}

hf_runtime_append_commandline() {
  local entry=$1 text=$2 verb=$3 token first=1
  while IFS= read -r token; do
    token=${token//%verb%/$verb}
    if [ "$first" = 1 ]; then token=$entry; first=0; fi
    HF_RUNTIME_ARGV+=("$token")
  done < <(hf_runtime_split_words "$text")
}

# Full argv for a Proton invocation, in HF_RUNTIME_ARGV. Container mode is entry point -> runtime commandline, then
# the selected proton script -> its manifest commandline, then the caller's arguments.
hf_runtime_arguments() {
  local verb=${1:-}
  [ $# -gt 0 ] && shift
  HF_RUNTIME_ARGV=()
  if [ "$HF_RUNTIME_MODE" = "container" ]; then
    hf_runtime_append_commandline "$HF_RUNTIME_ENTRYPOINT" "$HF_RUNTIME_COMMANDLINE" "$verb"
    hf_runtime_append_commandline "$HF_RUNTIME_PROTON" "$HF_RUNTIME_PROTON_COMMANDLINE" "$verb"
  elif [ -n "$HF_RUNTIME_PROTON_COMMANDLINE" ]; then
    hf_runtime_append_commandline "$HF_RUNTIME_PROTON" "$HF_RUNTIME_PROTON_COMMANDLINE" "$verb"
  else
    HF_RUNTIME_ARGV+=("$HF_RUNTIME_PROTON" "$verb")
  fi
  HF_RUNTIME_ARGV+=("$@")
}

hf_runtime_mode_line() {
  if [ "$HF_RUNTIME_MODE" = "container" ]; then
    printf 'HellForge: Steam Linux Runtime: container "%s" %s at %s; Proton runs inside it.\n' \
      "$HF_RUNTIME_NAME" "$HF_RUNTIME_VERSION" "$HF_RUNTIME_FOLDER"
  else
    printf 'HellForge: Steam Linux Runtime: host mode: %s; Proton runs on the host.\n' "${HF_RUNTIME_REASON:-unknown}"
  fi
}

# A command that runs through the runtime wrapper but not through Proton (the sandbox self-test): the entry point and
# its runtime commandline, then the caller's tail. Host mode runs the tail directly.
hf_runtime_entrypoint_arguments() {
  local verb=${1:-}
  [ $# -gt 0 ] && shift
  HF_RUNTIME_ARGV=()
  if [ "$HF_RUNTIME_MODE" = "container" ]; then
    hf_runtime_append_commandline "$HF_RUNTIME_ENTRYPOINT" "$HF_RUNTIME_COMMANDLINE" "$verb"
  fi
  HF_RUNTIME_ARGV+=("$@")
}

# The mode and identity the wrapper resolved. hf-inner and the readiness metadata read these names, so there is one
# spelling of the selected chain outside hf-steam.sh too.
hf_runtime_export() {
  export HELLFORGE_STEAM_RUNTIME_MODE="$HF_RUNTIME_MODE"
  export HELLFORGE_STEAM_RUNTIME_REASON="$HF_RUNTIME_REASON"
  export HELLFORGE_STEAM_RUNTIME_APPID="$HF_RUNTIME_APPID"
  export HELLFORGE_STEAM_RUNTIME_FOLDER="$HF_RUNTIME_FOLDER"
  export HELLFORGE_STEAM_RUNTIME_ENTRYPOINT="$HF_RUNTIME_ENTRYPOINT"
  export HELLFORGE_STEAM_RUNTIME_COMMANDLINE="$HF_RUNTIME_COMMANDLINE"
  export HELLFORGE_STEAM_RUNTIME_PROTON_COMMANDLINE="$HF_RUNTIME_PROTON_COMMANDLINE"
  export HELLFORGE_STEAM_RUNTIME_PROTON="$HF_RUNTIME_PROTON"
  export HELLFORGE_STEAM_RUNTIME_NAME="$HF_RUNTIME_NAME"
  export HELLFORGE_STEAM_RUNTIME_VERSION="$HF_RUNTIME_VERSION"
  export HELLFORGE_STEAM_RUNTIME_IDENTITY="${HF_RUNTIME_IDENTITY:-host}"
}

hf_runtime_json_escape() {
  local value=${1-}
  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  value=${value//$'\n'/\\n}
  value=${value//$'\r'/\\r}
  value=${value//$'\t'/\\t}
  printf '%s' "$value"
}

# Written after the self-test ends, partial then rename. A missing file is a readiness failure, never a pass.
hf_runtime_write_mode_file() {
  local folder=$1 path partial mode identity name version
  mkdir -p -- "$folder" || return 1
  path="$folder/sandbox-self-test-mode.json"
  partial="$path.partial"
  if [ "$HF_RUNTIME_MODE" = "container" ]; then
    mode=container
    identity=$HF_RUNTIME_IDENTITY
    name=$HF_RUNTIME_NAME
    version=$HF_RUNTIME_VERSION
  else
    mode=host
    identity=host
    name=""
    version=""
  fi
  printf '{"schemaVersion":1,"mode":"%s","runtimeName":"%s","runtimeVersion":"%s","runtimeIdentity":"%s"}\n' \
    "$(hf_runtime_json_escape "$mode")" "$(hf_runtime_json_escape "$name")" \
    "$(hf_runtime_json_escape "$version")" "$(hf_runtime_json_escape "$identity")" >"$partial" || return 1
  mv -f -- "$partial" "$path"
}
