#!/usr/bin/env bash
# Builds HellForge's overlay host (native/overlay, docs/modding/overlay.md) on Linux into
# artifacts/overlay/hellforge_overlay.dll, as scripts/Build-Overlay.ps1 does on Windows: Dear ImGui 1.91.9b, pinned by
# SHA-256 into the gitignored tools/, with the HDR shader hook in copies of its DX11 and DX12 renderers.
# With --test it also builds native/overlay/test/overlay_host_test.cpp and runs it under Wine on D3D12 and D3D11 (SDR,
# then HDR10 and scRGB forced), its screenshots in artifacts/overlay/test, on a hidden display (xvfb-run or gamescope).
# Environment: ZIG (else ~/.local/share/zig-native/zig, then zig on PATH); MINHOOK, MinHook 1.3.4's source folder, for
# --test (else tools/minhook-1.3.4/minhook-1.3.4); WINE and WINEPREFIX for --test (a scratch prefix, never a game's,
# with DXVK and vkd3d-proton in it).
set -euo pipefail
project="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test=0
[ "${1:-}" = "--test" ] && test=1

zig="${ZIG:-}"
[ -x "$zig" ] || zig="$HOME/.local/share/zig-native/zig"
[ -x "$zig" ] || zig="$(command -v zig 2>/dev/null || true)"
[ -n "$zig" ] && [ -x "$zig" ] || { echo "zig 0.17.0 not found; set ZIG" >&2; exit 1; }

archive="$project/tools/downloads/imgui-1.91.9b.zip"
sha=FD37507C8476A6D14CC7C4B352401F31BCBD0F0D995D35390811E968C466F46E
matches() { [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1 | tr a-f A-F)" = "$sha" ]; }
if ! matches "$archive"; then
  mkdir -p "$(dirname "$archive")"
  curl -fsSL -o "$archive.partial" https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip
  matches "$archive.partial" || { rm -f "$archive.partial"; echo "Dear ImGui does not match its pinned checksum." >&2; exit 1; }
  mv "$archive.partial" "$archive"
fi
imgui="$project/tools/imgui/imgui-1.91.9b"
marker="$project/tools/imgui/.hellforge-archive-sha256"
if [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$sha" ]; then
  rm -rf "$imgui"
  mkdir -p "$project/tools/imgui"
  unzip -q "$archive" -d "$project/tools/imgui"
  printf '%s' "$sha" > "$marker"
fi

out="$project/artifacts/overlay"
objects="$out/obj"
mkdir -p "$objects"
common=(-target x86_64-windows-gnu -O2 -std=c++17 -fno-exceptions -fno-rtti -I "$project/native/mod-sdk" -I "$project/native/overlay"
  -I "$imgui" -I "$imgui/backends" -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS "-ffile-prefix-map=$project=.")
# HDR: the renderers compile their pixel shader from a fixed string; the host supplies its own while the game draws in
# HDR (render.cpp, g_hellforgePixelShader). The one line declaring it changes in a copy; a version without it once stops.
shader_line='static const char* pixelShader ='
shader_hook='extern const char* g_hellforgePixelShader; const char* pixelShader = g_hellforgePixelShader != nullptr ? g_hellforgePixelShader :'
built=()
for source in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp backends/imgui_impl_win32.cpp \
  backends/imgui_impl_dx11.cpp backends/imgui_impl_dx12.cpp; do
  file="$imgui/$source"
  name="$(basename "$source" .cpp)"
  case "$name" in imgui_impl_dx11|imgui_impl_dx12)
    [ "$(grep -cF "$shader_line" "$file")" = 1 ] || { echo "The HDR shader hook does not fit $file." >&2; exit 1; }
    python3 -I -c 'import sys; t=open(sys.argv[1]).read(); open(sys.argv[2],"w").write(t.replace(sys.argv[3], sys.argv[4]))' \
      "$file" "$objects/$name.cpp" "$shader_line" "$shader_hook"
    file="$objects/$name.cpp" ;;
  esac
  "$zig" c++ "${common[@]}" -w -c "$file" -o "$objects/$name.o"
  built+=("$objects/$name.o")
done
host=()
for source in host draw images slots backdrop render present input; do host+=("$project/native/overlay/$source.cpp"); done
"$zig" c++ "${common[@]}" -DHF_OVERLAY_HOST -shared -Wall -Werror -Wno-unused-parameter -s "${host[@]}" "${built[@]}" \
  -ld3d11 -ld3d12 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -lole32 -luser32 -o "$out/hellforge_overlay.dll"
rm -f "$out"/*.lib
cp "$imgui/LICENSE.txt" "$out/imgui-LICENSE.txt"
echo "Built $out/hellforge_overlay.dll ($(( $(stat -c %s "$out/hellforge_overlay.dll") / 1024 )) KB)"
[ "$test" = 1 ] || exit 0

minhook="${MINHOOK:-$project/tools/minhook-1.3.4/minhook-1.3.4}"
[ -f "$minhook/src/hook.c" ] || { echo "MinHook 1.3.4 not found; set MINHOOK" >&2; exit 1; }
[ -n "${WINE:-}" ] && [ -n "${WINEPREFIX:-}" ] || { echo "--test needs WINE and WINEPREFIX (a scratch prefix)" >&2; exit 1; }
tests="$out/test"
mkdir -p "$tests"
hooks=()
for source in src/buffer.c src/hook.c src/trampoline.c src/hde/hde64.c; do
  object="$tests/mh_$(basename "$source" .c).o"
  "$zig" cc -target x86_64-windows-gnu -O1 -w -I "$minhook/include" -c "$minhook/$source" -o "$object"
  hooks+=("$object")
done
"$zig" c++ -target x86_64-windows-gnu -O1 -std=c++17 -Wall -Werror -Wno-unused-parameter -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -I "$project/native/mod-sdk" -I "$project/native/overlay" \
  -I "$imgui" -I "$minhook/include" "$project/native/overlay/test/overlay_host_test.cpp" "${hooks[@]}" -ld3d11 -ld3d12 -ldxgi -lgdi32 -o "$tests/overlay_host_test.exe"
# The test's window takes the focus and would catch the keys of whoever works at the desktop, so it runs on a hidden
# display. HELLFORGE_OVERLAY_TEST_VISIBLE=1 runs it on the desktop instead.
display=()
if [ "${HELLFORGE_OVERLAY_TEST_VISIBLE:-}" = 1 ]; then :
elif command -v xvfb-run >/dev/null; then display=(xvfb-run -a -s "-screen 0 1920x1080x24")
elif command -v gamescope >/dev/null; then display=(gamescope --backend headless -W 1920 -H 1080 --)
else echo "--test needs xvfb-run or gamescope for a hidden display (or HELLFORGE_OVERLAY_TEST_VISIBLE=1)" >&2; exit 1
fi
# Not kwin_wayland --virtual --xwayland: when it exits it removes the desktop's own /tmp/.X11-unix/X0, and games in the
# Steam container then start without a display.
# The game draws through DXVK and vkd3d-proton, so the test does: Wine's own Direct3D 11 copies a back buffer as black,
# which fails the backdrop checks. The prefix needs their DLLs (and libvkd3d) in system32.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-dxgi,d3d11,d3d10core,d3d12,d3d12core=n}"
run() { # <name> [VAR=value ...]: one run, its screenshot <name>.bmp
  local name="$1"; shift
  echo "== $name"
  "${display[@]}" env "$@" "$WINE" "$tests/overlay_host_test.exe" "$out/hellforge_overlay.dll" "$tests/$name.bmp"
}
run screen12
run screen11 HELLFORGE_OVERLAY_TEST_D3D11=1
run screen12-pq HELLFORGE_OVERLAY_TEST_FORMAT=rgb10a2 HELLFORGE_OVERLAY_HDR=pq:200
run screen11-scrgb HELLFORGE_OVERLAY_TEST_D3D11=1 HELLFORGE_OVERLAY_TEST_FORMAT=fp16 HELLFORGE_OVERLAY_HDR=scrgb:200
echo "The overlay host's tests passed."
