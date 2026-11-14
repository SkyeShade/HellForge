# Builds HellForge's overlay host (native/overlay, docs/modding/overlay.md) into artifacts/overlay/hellforge_overlay.dll,
# which ships beside the mod loader in <installation>/app/sdk-shims. Dear ImGui 1.91.9b (MIT) is fetched from GitHub
# once, pinned by SHA-256, into tools/imgui, as Build-Editor.ps1 does; it is compiled with the host by the pinned Zig.
# With -Test it also runs native/overlay/test/overlay_host_test.cpp: the host on a real D3D12 swap chain, then D3D11, then
# forced into HDR10 and scRGB, in a window of its own (a few seconds), with a screenshot check of every HfDraw call.
# It needs a GPU and a desktop to draw on. scripts/build-overlay.sh is the same on Linux.
# Usage: scripts/Build-Overlay.ps1 [-Test]
param([switch]$Test)
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
$out=Join-Path $project 'artifacts/overlay'
$objects=Join-Path $out 'obj'
New-Item -ItemType Directory $objects -Force|Out-Null
# The same ImGui configuration as the editor's: C++ screens that share the host's context must build theirs alike.
$common=@('-target','x86_64-windows-gnu','-O2','-std=c++17','-fno-exceptions','-fno-rtti','-I',(Join-Path $project 'native/mod-sdk'),'-I',(Join-Path $project 'native/overlay'),
    '-I',$imgui,'-I',(Join-Path $imgui 'backends'),'-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS',('-ffile-prefix-map='+$project+'=.'))
# HDR: the renderers compile their pixel shader from a fixed string; the host supplies its own while the game draws in
# HDR (render.cpp, g_hellforgePixelShader). The one line declaring it changes in a copy; a version without it once stops.
$shaderLine='static const char* pixelShader ='
$shaderHook='extern const char* g_hellforgePixelShader; const char* pixelShader = g_hellforgePixelShader != nullptr ? g_hellforgePixelShader :'
$sources=@('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','imgui_demo.cpp','backends/imgui_impl_win32.cpp','backends/imgui_impl_dx11.cpp','backends/imgui_impl_dx12.cpp')|ForEach-Object {Join-Path $imgui $_}
$built=@()
foreach($source in $sources){
    if([IO.Path]::GetFileName($source) -in @('imgui_impl_dx11.cpp','imgui_impl_dx12.cpp')){
        $text=[IO.File]::ReadAllText($source)
        if(([regex]::Matches($text,[regex]::Escape($shaderLine))).Count -ne 1){throw ('The HDR shader hook does not fit '+$source)}
        $copy=Join-Path $objects ([IO.Path]::GetFileName($source))
        [IO.File]::WriteAllText($copy,$text.Replace($shaderLine,$shaderHook))
        $source=$copy
    }
    $object=Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($source)+'.o')
    & $zig c++ @common -w -c $source -o $object
    if($LASTEXITCODE){throw ('Dear ImGui did not compile: '+$source)}
    $built+=$object
}
$hostSources=@('host','draw','images','slots','backdrop','render','present','input')|ForEach-Object{Join-Path $project ('native/overlay/'+$_+'.cpp')}
$dll=Join-Path $out 'hellforge_overlay.dll'
& $zig c++ @common -DHF_OVERLAY_HOST -shared -Wall -Werror -Wno-unused-parameter -s @hostSources @built `
    -ld3d11 -ld3d12 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -lole32 -luser32 -o $dll
if($LASTEXITCODE){throw 'The overlay host did not compile.'}
Get-ChildItem -LiteralPath $out -Filter '*.lib'|Remove-Item
Copy-Item -LiteralPath (Join-Path $imgui 'LICENSE.txt') -Destination (Join-Path $out 'imgui-LICENSE.txt')
Write-Output ('Built {0} ({1:N0} KB)' -f $dll,((Get-Item $dll).Length/1KB))
if(-not $Test){return}

$minHook=$buildTools.MinHook
$tests=Join-Path $out 'test'
New-Item -ItemType Directory $tests -Force|Out-Null
$hookObjects=@()
foreach($source in @('src/buffer.c','src/hook.c','src/trampoline.c','src/hde/hde64.c')){
    $object=Join-Path $tests ('mh_'+[IO.Path]::GetFileNameWithoutExtension($source)+'.o')
    & $zig cc -target x86_64-windows-gnu -O1 -w -I (Join-Path $minHook 'include') -c (Join-Path $minHook $source) -o $object
    if($LASTEXITCODE){throw 'MinHook did not compile.'}
    $hookObjects+=$object
}
$exe=Join-Path $tests 'overlay_host_test.exe'
& $zig c++ -target x86_64-windows-gnu -O1 -std=c++17 -Wall -Werror -Wno-unused-parameter -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -I (Join-Path $project 'native/mod-sdk') `
    -I (Join-Path $project 'native/overlay') -I $imgui -I (Join-Path $minHook 'include') (Join-Path $project 'native/overlay/test/overlay_host_test.cpp') @hookObjects `
    -ld3d11 -ld3d12 -ldxgi -lgdi32 -o $exe
if($LASTEXITCODE){throw 'The overlay host test did not compile.'}
# The mouse pointer's test needs a window but no swap chain: input.cpp with ImGui's core and Win32 backend.
$core=@('imgui','imgui_draw','imgui_tables','imgui_widgets','imgui_demo','imgui_impl_win32')|ForEach-Object{Join-Path $objects ($_+'.o')}
$cursorExe=Join-Path $tests 'overlay_cursor_test.exe'
& $zig c++ @common -Wall -Werror -Wno-unused-parameter (Join-Path $project 'native/overlay/test/overlay_cursor_test.cpp') (Join-Path $project 'native/overlay/input.cpp') `
    @core -ldwmapi -lgdi32 -luser32 -o $cursorExe
if($LASTEXITCODE){throw 'The overlay cursor test did not compile.'}
Write-Output '== cursor'
& $cursorExe
if($LASTEXITCODE){throw 'The overlay cursor test failed.'}
# name, then the environment of that run: SDR on both APIs, then HDR10 and scRGB forced (an SDR desktop shows the values)
$runs=@(
    @('screen12',@{}),
    @('screen11',@{HELLFORGE_OVERLAY_TEST_D3D11='1'}),
    @('screen12-pq',@{HELLFORGE_OVERLAY_TEST_FORMAT='rgb10a2';HELLFORGE_OVERLAY_HDR='pq:200'}),
    @('screen11-scrgb',@{HELLFORGE_OVERLAY_TEST_D3D11='1';HELLFORGE_OVERLAY_TEST_FORMAT='fp16';HELLFORGE_OVERLAY_HDR='scrgb:200'}))
foreach($run in $runs){
    Write-Output ('== '+$run[0])
    foreach($name in $run[1].Keys){Set-Item -Path ('Env:'+$name) -Value $run[1][$name]}
    try { & $exe $dll (Join-Path $tests ($run[0]+'.bmp')) }
    finally { foreach($name in $run[1].Keys){Remove-Item -Path ('Env:'+$name)} }
    if($LASTEXITCODE){throw ('The overlay host test failed: '+$run[0])}
}
Write-Output "The overlay host's tests passed."
