#!/usr/bin/env fish
# build-hf-mod.fish <project-dir>
# Builds a HellForge mod with a native Linux Zig instead of HellForge's own
# bundled Windows zig.exe (which hits a Wine NtLockFile bug: manifest_create
# Unexpected on every build). Reads the mod's own CMakeLists.txt for the
# source list and language when there is one; falls back to a single
# src/<dll-name>.c file (from hellforge-mod.json) for mods with no CMake
# project. Compiles to a DLL and installs it into mods/.

set HF "~/.local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge/app/sdk"
set ZIG "$HOME/.local/share/zig-native/zig"

if test (count $argv) -ne 1
    echo "usage: build-hf-mod.fish <project-dir>"
    exit 1
end

set PROJ (realpath "$argv[1]")

if not test -f "$PROJ/hellforge-mod.json"
    echo "error: no hellforge-mod.json in $PROJ"
    exit 1
end

set IS_CXX 0
set NAME ""
set SOURCES

if test -f "$PROJ/CMakeLists.txt"
    set LANG_LINE (grep -m1 '^project(' "$PROJ/CMakeLists.txt")
    if string match -q '*CXX*' -- "$LANG_LINE"
        set IS_CXX 1
    end

    set CALL (grep -Pzo '(?s)hellforge_add_mod\([^)]*\)' "$PROJ/CMakeLists.txt" | tr -d '\0' | tr '\n' ' ')
    if test -z "$CALL"
        echo "error: couldn't find hellforge_add_mod(...) in CMakeLists.txt"
        exit 1
    end
    set INNER (string replace -r '^hellforge_add_mod\(' '' -- "$CALL" | string replace -r '\)\s*$' '')
    set TOKENS (string split ' ' -- (string trim -- "$INNER") | string match -v '')
    set NAME $TOKENS[1]
    set SOURCES $TOKENS[2..-1]
else
    # No CMakeLists.txt: derive everything from hellforge-mod.json + src/*.c(pp)
    set DLLNAME (grep -Po '"dll"\s*:\s*"\K[^"]+' "$PROJ/hellforge-mod.json")
    set NAME (string replace -r '\.dll$' '' -- "$DLLNAME")

    set CPPFILES (find "$PROJ/src" -maxdepth 1 -name '*.cpp' 2>/dev/null)
    if test (count $CPPFILES) -gt 0
        set IS_CXX 1
        for f in $CPPFILES
            set SOURCES $SOURCES (string replace "$PROJ/" '' -- "$f")
        end
    else
        set CFILES (find "$PROJ/src" -maxdepth 1 -name '*.c' 2>/dev/null)
        for f in $CFILES
            set SOURCES $SOURCES (string replace "$PROJ/" '' -- "$f")
        end
    end

    if test (count $SOURCES) -eq 0
        echo "error: no CMakeLists.txt and no src/*.c or src/*.cpp found"
        exit 1
    end
end

echo "mod: $NAME  (c++: $IS_CXX)"
echo "sources: $SOURCES"

set DLL "$PROJ/$NAME.dll"
set SRC_ARGS
for s in $SOURCES
    set SRC_ARGS $SRC_ARGS "$PROJ/$s"
end

if test $IS_CXX -eq 1
    $ZIG c++ -target x86_64-windows-gnu -shared -std=c++17 \
        -Wall -Werror -Wno-unused-parameter -s \
        -I "$HF/app/sdk" -o "$DLL" $SRC_ARGS
else
    $ZIG cc -target x86_64-windows-gnu -shared -std=c11 \
        -Wall -Werror -Wno-unused-parameter -s \
        -I "$HF/app/sdk" -o "$DLL" $SRC_ARGS
end

if test $status -ne 0
    echo "build failed"
    exit 1
end

file "$DLL"

set FOLDER (basename "$PROJ")
set TARGET "$HF/mods/$FOLDER"
mkdir -p "$TARGET"
cp "$PROJ/hellforge-mod.json" "$DLL" "$TARGET/"
if test -d "$PROJ/data"
    cp -r "$PROJ/data" "$TARGET/data"
end

echo "installed to $TARGET"
