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
