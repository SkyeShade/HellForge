# Builds HellForge's in-game editor (native/editor, docs/sdk-roadmap.md phase 5a) into artifacts/editor/hellforge-editor, a
# mod folder (manifest, DLL, Dear ImGui's licence) to copy into <installation>/mods. Dear ImGui 1.91.9b (MIT) is fetched
# from GitHub once, pinned by SHA-256, into tools/imgui; it is compiled with the editor by the project's pinned Zig.
# With -Test it also runs native/editor/test/overlay_test.cpp: the editor against a real D3D12 swap chain (then a D3D11 one) in a window of
# its own (shown for a few seconds), with F8, resizes and a screenshot check (then forced into HDR10 and scRGB, the
# colours checked PQ-encoded), and native/editor/test/local_stats_test.cpp,
# its saving into the Local Stats mod.
# Usage: scripts/Build-Editor.ps1 [-Install <installation>] [-Test [-SkipOverlayTest]]   (with -Install, the folder is copied into
# its mods, with the game icons extracted from that installation's game copy; -SkipOverlayTest leaves out overlay_test, which
# needs a GPU and a desktop to draw on, as on CI's hosted runners)
param([string]$Install,[switch]$Test,[switch]$SkipOverlayTest)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
$project=Split-Path -Parent $PSScriptRoot
$buildTools=& (Join-Path $PSScriptRoot 'Get-BuildTools.ps1')
$zig=$buildTools.Zig
$archive=Join-Path $project 'tools/downloads/imgui-1.91.9b.zip'
$sha='FD37507C8476A6D14CC7C4B352401F31BCBD0F0D995D35390811E968C466F46E'
if(-not(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive).Hash -ne $sha){
    New-Item -ItemType Directory (Split-Path -Parent $archive) -Force|Out-Null
    Invoke-WebRequest -Uri 'https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip' -OutFile ($archive+'.partial') -UseBasicParsing
    if((Get-FileHash -LiteralPath ($archive+'.partial')).Hash -ne $sha){Remove-Item -LiteralPath ($archive+'.partial');throw 'Dear ImGui does not match its pinned checksum.'}
    Move-Item -LiteralPath ($archive+'.partial') -Destination $archive -Force
}
$imgui=Join-Path $project 'tools/imgui/imgui-1.91.9b'
$marker=Join-Path $project 'tools/imgui/.hellforge-archive-sha256'
if(-not(Test-Path -LiteralPath $marker) -or [IO.File]::ReadAllText($marker) -ne $sha){
    if(Test-Path -LiteralPath $imgui){Remove-Item -LiteralPath $imgui -Recurse -Force}
    New-Item -ItemType Directory (Join-Path $project 'tools/imgui') -Force|Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($archive,(Join-Path $project 'tools/imgui'))
    [IO.File]::WriteAllText($marker,$sha)
}
$out=Join-Path $project 'artifacts/editor/hellforge-editor'
New-Item -ItemType Directory $out -Force|Out-Null
$sources=@('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','imgui_demo.cpp','backends/imgui_impl_win32.cpp','backends/imgui_impl_dx11.cpp','backends/imgui_impl_dx12.cpp')|ForEach-Object {Join-Path $imgui $_}
$common=@('-target','x86_64-windows-gnu','-O2','-std=c++17','-fno-exceptions','-fno-rtti','-I',(Join-Path $project 'native/mod-sdk'),'-I',$imgui,'-I',(Join-Path $imgui 'backends'),'-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS',('-ffile-prefix-map='+$project+'=.')) # source paths in the DLL are relative: it ships
# Dear ImGui as it is (its own warnings off), then the editor with the warnings every HellForge mod is built with.
$objects=Join-Path $project 'artifacts/editor/obj'
New-Item -ItemType Directory $objects -Force|Out-Null
$built=@()
# HDR: ImGui's DX11 and DX12 renderers compile their pixel shader from a fixed string. The editor supplies its own while
# the game draws in HDR (native/editor/editor.cpp, g_hellforgePixelShader), so the one line declaring that string is
# changed in a copy beside the objects; the pinned source stays as it is. A version where the line is not there once stops.
$shaderLine='static const char* pixelShader ='
$shaderHook='extern const char* g_hellforgePixelShader; const char* pixelShader = g_hellforgePixelShader != nullptr ? g_hellforgePixelShader :'
$sources=foreach($source in $sources){
    if([IO.Path]::GetFileName($source) -notin @('imgui_impl_dx11.cpp','imgui_impl_dx12.cpp')){$source;continue}
    $text=[IO.File]::ReadAllText($source)
    if(([regex]::Matches($text,[regex]::Escape($shaderLine))).Count -ne 1){throw ('The HDR shader hook does not fit '+$source+': '+$shaderLine+' is not there once.')}
    $copy=Join-Path $objects ([IO.Path]::GetFileName($source))
    [IO.File]::WriteAllText($copy,$text.Replace($shaderLine,$shaderHook))
    $copy
}
foreach($source in $sources){
    $object=Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($source)+'.o')
    & $zig c++ @common -w -c $source -o $object
    if($LASTEXITCODE){throw ('Dear ImGui did not compile: '+$source)}
    $built+=$object
}
$editorSources=@('editor.cpp','app.cpp','app_values.cpp','app_staging.cpp','app_widgets.cpp','app_page.cpp','app_presets.cpp','app_mods_logs.cpp','app_sync.cpp','app_keys.cpp','app_browse.cpp','app_views.cpp','canvas.cpp','catalog.cpp','icons.cpp','json.cpp','local_stats.cpp','sounds.cpp')|ForEach-Object{Join-Path $project ('native/editor/'+$_)}
& $zig c++ @common -shared -Wall -Werror -Wno-unused-parameter -s @editorSources @built `
    -ld3d11 -ld3d12 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -lole32 -luser32 -o (Join-Path $out 'hellforge_editor.dll')
if($LASTEXITCODE){throw 'The editor did not compile.'}
Get-ChildItem -LiteralPath $out -Filter '*.lib'|Remove-Item
Copy-Item -LiteralPath (Join-Path $project 'native/editor/hellforge-mod.json') -Destination $out
# its picture on the launcher's Mods page (the manifest's "icon")
Copy-Item -LiteralPath (Join-Path $project 'native/editor/assets/editor-logo.png') -Destination (Join-Path $out 'icon.png')
# The catalogue (HellForge's EditorCatalog.cs), and with -Install the icons and fonts from that installation's game copy (EditorIcons.cs, EditorFonts.cs:
# the player's own game files, read there; they are never part of a build anyone else gets).
$dataArgs=@('editor-data','--out',$out)
if($Install){$dataArgs+=@('--install',$Install)}
& dotnet run --project (Join-Path $project 'src/HellForge.Cli') -v q -- @dataArgs|Out-Null
if($LASTEXITCODE){throw 'The editor catalogue could not be made.'}
Copy-Item -LiteralPath (Join-Path $imgui 'LICENSE.txt') -Destination (Join-Path $out 'imgui-LICENSE.txt')
Write-Output ('Built {0} ({1:N0} KB)' -f (Join-Path $out 'hellforge_editor.dll'),((Get-Item (Join-Path $out 'hellforge_editor.dll')).Length/1KB))
if($Test -and -not $SkipOverlayTest){
    $minHook=$buildTools.MinHook
    $testOut=Join-Path $project 'artifacts/editor/test'
    New-Item -ItemType Directory $testOut -Force|Out-Null
    $hookObjects=@()
    foreach($source in @('src/buffer.c','src/hook.c','src/trampoline.c','src/hde/hde64.c')){
        $object=Join-Path $testOut ('mh_'+[IO.Path]::GetFileNameWithoutExtension($source)+'.o')
        & $zig cc -target x86_64-windows-gnu -O1 -w -I (Join-Path $minHook 'include') -c (Join-Path $minHook $source) -o $object
        if($LASTEXITCODE){throw 'MinHook did not compile.'}
        $hookObjects+=$object
    }
    & $zig c++ -target x86_64-windows-gnu -O1 -std=c++17 -Wall -Werror -Wno-unused-parameter -I (Join-Path $project 'native/mod-sdk') -I (Join-Path $minHook 'include') `
        (Join-Path $project 'native/editor/test/overlay_test.cpp') @hookObjects -ld3d11 -ld3d12 -ldxgi -lgdi32 -o (Join-Path $testOut 'overlay_test.exe')
    if($LASTEXITCODE){throw 'The overlay test did not compile.'}
    # a big HD2R Editor preset, where there is one (artifacts/presets; never committed): the editor must stay fast with it
    $big=Join-Path $project 'artifacts/presets/Black-Wolfs-balance.hd2rpreset.json'
    $env:HF_TEST_BIG_PRESET=if(Test-Path -LiteralPath $big){$big}else{''}
    & (Join-Path $testOut 'overlay_test.exe') (Join-Path $out 'hellforge_editor.dll') (Join-Path $testOut 'screen.bmp') $out
    if($LASTEXITCODE){throw 'The overlay test failed.'}
    # again on a Direct3D 11 swap chain, as the game draws with --use-d3d11
    $env:HELLFORGE_EDITOR_TEST_D3D11='1'
    try { & (Join-Path $testOut 'overlay_test.exe') (Join-Path $out 'hellforge_editor.dll') (Join-Path $testOut 'screen11.bmp') $out }
    finally { Remove-Item Env:HELLFORGE_EDITOR_TEST_D3D11 }
    if($LASTEXITCODE){throw 'The overlay test failed on Direct3D 11.'}
    # HDR, forced (an SDR desktop shows the values as they are): HDR10 on Direct3D 12, whose colours the test checks are
    # PQ-encoded at 200 nits, and scRGB on Direct3D 11 (the other renderer and shader compile and draw)
    $env:HELLFORGE_EDITOR_HDR='pq:200'
    try { & (Join-Path $testOut 'overlay_test.exe') (Join-Path $out 'hellforge_editor.dll') (Join-Path $testOut 'screen-hdr.bmp') $out }
    finally { Remove-Item Env:HELLFORGE_EDITOR_HDR }
    if($LASTEXITCODE){throw 'The overlay test failed in HDR10.'}
    $env:HELLFORGE_EDITOR_HDR='scrgb:200'; $env:HELLFORGE_EDITOR_TEST_D3D11='1'
    try { & (Join-Path $testOut 'overlay_test.exe') (Join-Path $out 'hellforge_editor.dll') (Join-Path $testOut 'screen11-scrgb.bmp') $out }
    finally { Remove-Item Env:HELLFORGE_EDITOR_HDR; Remove-Item Env:HELLFORGE_EDITOR_TEST_D3D11 }
    if($LASTEXITCODE){throw 'The overlay test failed in scRGB on Direct3D 11.'}
}
if($Test){
    $testOut=Join-Path $project 'artifacts/editor/test'
    New-Item -ItemType Directory $testOut -Force|Out-Null
    & $zig c++ -target x86_64-windows-gnu -O1 -std=c++17 -Wall -Werror (Join-Path $project 'native/editor/test/local_stats_test.cpp') `
        (Join-Path $project 'native/editor/local_stats.cpp') (Join-Path $project 'native/editor/json.cpp') -o (Join-Path $testOut 'local_stats_test.exe')
    if($LASTEXITCODE){throw 'The Local Stats test did not compile.'}
    & (Join-Path $testOut 'local_stats_test.exe')
    if($LASTEXITCODE){throw 'The Local Stats test failed.'}
}
if($Install){
    $target=Join-Path $Install 'mods/hellforge-editor'
    New-Item -ItemType Directory $target -Force|Out-Null
    Copy-Item -Path (Join-Path $out '*') -Destination $target -Force
    Write-Output ('Copied into '+$target)
}
