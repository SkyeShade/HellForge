#!/usr/bin/env bash
# hf-build-mod.sh: build a HellForge mod with a native Linux Zig and install it into HellForge's mods folder.
#
#   ./hf-build-mod.sh <mod-folder>
#
# Reads the mod's CMakeLists.txt for the language and the source list (hellforge_add_mod(...)); without one, uses
# src/*.cpp or src/*.c and the "dll" name in hellforge-mod.json. Compiles to a Windows DLL and installs it, with
# hellforge-mod.json and data/, into <HellForge>/mods/<mod-folder-name>/.
# Compiles with the installation's pinned native Linux Zig (<installation>/tools/zig-linux/zig) first, then ZIG,
# ~/.local/share/zig-native/zig and zig on PATH. A mod already in an installation's mods folder is built into the
# session's cache by the native build agent instead, so Build now and the session share one DLL.
# Needs INSTALL (the HellForge installation; default: the installation this script is in).
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
  local proj="${1:-}" sdk zig is_cxx=0 name="" dll dllname lang_line call inner f s folder target install_root
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
  install_root="$(realpath "$INSTALL")"
  sdk="$(sdk_dir)"
  [ -n "$sdk" ] || die "mod SDK (hellforge_mod.h) not found under $INSTALL"

  # A mod inside the installation is already installed: let the native build agent fill temp/mod-builds, the build a
  # session loads. Mods outside an installation keep the direct compile and copy below.
  if [ -x "$install_root/app/cli-linux/HellForge.Cli" ]; then
    case "$proj" in
      "$install_root"/mods/*)
        echo "building $proj into the installation's source cache..."
        "$install_root/app/cli-linux/HellForge.Cli" build-mod --mod "$proj"
        exit 0
        ;;
    esac
  fi

  zig="$install_root/tools/zig-linux/zig"
  if [ ! -x "$zig" ] && [ -n "${ZIG:-}" ]; then zig="$ZIG"; fi
  if [ ! -x "$zig" ]; then zig="$HOME/.local/share/zig-native/zig"; fi
  if [ ! -x "$zig" ]; then zig="$(command -v zig 2>/dev/null || true)"; fi
  [ -n "$zig" ] && [ -x "$zig" ] || die "native Linux zig not found; install it to <installation>/tools/zig-linux/zig or ~/.local/share/zig-native/zig, have 'zig' in PATH, or set ZIG"
  # Zig's cache stays inside the installation, beside the pinned tools' own cache (ModKit.Build).
  export ZIG_GLOBAL_CACHE_DIR="$install_root/tools/zig-cache-linux"
  mkdir -p "$ZIG_GLOBAL_CACHE_DIR"

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
    "$zig" c++ -std=c++17 -target x86_64-windows-gnu -shared -O2 -Wall -Werror -Wno-unused-parameter -s \
      -I "$sdk" "${src_args[@]}" -o "$dll" || die "build failed"
  else
    "$zig" cc -std=c11 -target x86_64-windows-gnu -shared -O2 -Wall -Werror -Wno-unused-parameter -s \
      -I "$sdk" "${src_args[@]}" -o "$dll" || die "build failed"
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
