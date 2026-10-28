#!/usr/bin/env bash
# hf-build-mod.sh: build a HellForge mod with a native Linux Zig and install it into HellForge's mods folder.
#
#   ./hf-build-mod.sh <mod-folder>
#
# Reads the mod's CMakeLists.txt for the language and the source list (hellforge_add_mod(...)); without one, uses
# src/*.cpp or src/*.c and the "dll" name in hellforge-mod.json. Compiles to a Windows DLL and installs it, with
# hellforge-mod.json and data/, into <HellForge>/mods/<mod-folder-name>/.
# Needs INSTALL (the HellForge installation; default: the installation this script is in) and ZIG (the native Linux
# zig; default ~/.local/share/zig-native/zig, else zig from PATH).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- SDK ------------------------------------------------------------------------------------------------------------
sdk_dir() {  # the mod SDK folder (headers such as hellforge_mod.h); empty output if there is none
  local h d
  h="$INSTALL"
  d="$h/app/sdk"
  if [ -f "$d/hellforge_mod.h" ]; then echo "$d"; return 0; fi
  h="$(find "$h" -maxdepth 4 -name hellforge_mod.h 2>/dev/null | head -n 1 || true)"
  [ -z "$h" ] || dirname "$h"
}

# ---- build ----------------------------------------------------------------------------------------------------------
cmd_build() {  # $1 = the mod folder (the one with hellforge-mod.json)
  local proj="${1:-}" sdk zig is_cxx=0 name="" dll dllname lang_line call inner f s folder target
  local sources=() toks=() cpp=() cfiles=() src_args=()
  [ -n "$proj" ] || die "usage: $(basename "$0") <mod-folder>"
  [ -d "$proj" ] || die "$proj is not a folder"
  proj="$(realpath "$proj")"
  [ -f "$proj/hellforge-mod.json" ] || die "no hellforge-mod.json in $proj"

  if [ -z "${INSTALL:-}" ] && [ -f "$here/../../hellforge-install.json" ]; then INSTALL="$(cd "$here/../.." && pwd)"; fi
  if [ -z "${INSTALL:-}" ]; then
    die "INSTALL is not set (the HellForge installation folder with hellforge-install.json); set INSTALL, or run this script from an installation's app/linux folder"
  fi
  [ -f "$INSTALL/hellforge-install.json" ] || die "$INSTALL is not a HellForge installation (no hellforge-install.json); set INSTALL, or run this script from an installation's app/linux folder"
  sdk="$(sdk_dir)"
  [ -n "$sdk" ] || die "mod SDK (hellforge_mod.h) not found under $INSTALL"

  zig="${ZIG:-$HOME/.local/share/zig-native/zig}"
  [ -x "$zig" ] || zig="$(command -v zig 2>/dev/null || true)"
  [ -n "$zig" ] && [ -x "$zig" ] || die "native Linux zig not found; install it to ~/.local/share/zig-native/zig, have 'zig' in PATH, or set ZIG"

  if [ -f "$proj/CMakeLists.txt" ]; then
    lang_line="$(grep -m1 '^project(' "$proj/CMakeLists.txt" || true)"
    case "$lang_line" in *CXX*) is_cxx=1 ;; esac
    # the first hellforge_add_mod(...) call, which may span several lines
    call="$(awk '/hellforge_add_mod\(/{f=1} f{printf "%s ", $0} f&&/\)/{exit}' "$proj/CMakeLists.txt")"
    [ -n "$call" ] || die "couldn't find hellforge_add_mod(...) in CMakeLists.txt"
    inner="${call#*hellforge_add_mod(}"
    inner="${inner%%)*}"
    read -ra toks <<< "$inner" || true
    name="${toks[0]:-}"
    sources=("${toks[@]:1}")
    [ -n "$name" ] && [ "${#sources[@]}" -gt 0 ] || die "couldn't read the mod name and sources from hellforge_add_mod(...)"
  else
    # no CMakeLists.txt: everything comes from hellforge-mod.json and src/*.cpp (or src/*.c)
    dllname="$(grep -Po '"dll"\s*:\s*"\K[^"]+' "$proj/hellforge-mod.json" || true)"
    [ -n "$dllname" ] || die "no \"dll\" entry in $proj/hellforge-mod.json"
    name="${dllname%.dll}"
    mapfile -t cpp < <(find "$proj/src" -maxdepth 1 -name '*.cpp' 2>/dev/null | sort)
    if [ "${#cpp[@]}" -gt 0 ]; then
      is_cxx=1
      for f in "${cpp[@]}"; do sources+=("${f#"$proj"/}"); done
    else
      mapfile -t cfiles < <(find "$proj/src" -maxdepth 1 -name '*.c' 2>/dev/null | sort)
      for f in "${cfiles[@]}"; do [ -z "$f" ] || sources+=("${f#"$proj"/}"); done
    fi
    [ "${#sources[@]}" -gt 0 ] || die "no CMakeLists.txt and no src/*.c or src/*.cpp found"
  fi

  echo "mod: $name  (c++: $is_cxx)"
  echo "sources: ${sources[*]}"
  echo "sdk: $sdk"

  dll="$proj/$name.dll"
  for s in "${sources[@]}"; do src_args+=("$proj/$s"); done

  if [ "$is_cxx" = 1 ]; then
    "$zig" c++ -target x86_64-windows-gnu -shared -std=c++17 -Wall -Werror -Wno-unused-parameter -s \
      -I "$sdk" -o "$dll" "${src_args[@]}" || die "build failed"
  else
    "$zig" cc -target x86_64-windows-gnu -shared -std=c11 -Wall -Werror -Wno-unused-parameter -s \
      -I "$sdk" -o "$dll" "${src_args[@]}" || die "build failed"
  fi

  ! command -v file >/dev/null || file "$dll"

  folder="$(basename "$proj")"
  target="$INSTALL/mods/$folder"
  mkdir -p "$target"
  cp -f -- "$proj/hellforge-mod.json" "$dll" "$target/"
  if [ -d "$proj/data" ]; then
    mkdir -p "$target/data"
    cp -r -- "$proj/data/." "$target/data/"
  fi
  echo "installed to $target"
}

cmd_build "${1:-}"
