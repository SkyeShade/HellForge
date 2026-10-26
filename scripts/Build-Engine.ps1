# Builds the HellForge engine package, artifacts/engine-package/hellforge-engine-<version>.zip (the version is
# EngineVersion in Directory.Build.props), which scripts/Publish-HellForge.ps1 puts into an installation's app folder:
#   session/     HellForge.Session.exe, the session runner (NativeAOT)
#   backend/     HellForge.LocalBackend.exe, the local backend (NativeAOT)
#   sdk-shims/   the PlayFab SDK stand-ins (PartyWin.dll, PlayFabMultiplayerWin.dll), the steam_api64.dll stand-in, its bridge
#                build and the Steam broker for low-integrity sessions (steam_api64_bridge.dll, hellforge_steam_broker.exe,
#                docs/steam-bridge.md; only with the Steamworks SDK, -SteamSdk), the mod loader
#                (hellforge_modloader.dll, with MinHook 1.3.4: BSD-2, MinHook-LICENSE.txt) and build.json (their pins)
#   scripts/     the network block (Windows Firewall rules) and Steam ownership check, readable as they run
#   engine.json  the version, the game build and every file's SHA-256; LICENSE (GPL-3.0) and LICENSE-EXCEPTION
# Built in this repository, against its Core library, mod SDK headers and mappings (the loader's field table is
# generated from them first). Downloads, each pinned by SHA-256 into the gitignored tools/: Zig and MinHook
# (scripts/Get-BuildTools.ps1), and Microsoft's public PlayFab SDK packages, whose DLLs
# are byte-identical to the ones HD2 ships, so their headers give the exact signatures (nothing from them is committed or
# shipped). The steam_api64.dll stand-in's export table is generated from the game's own steam_api64.dll (-SteamApi, by
# default the one in the Steam library), pinned below and never shipped. The Steam bridge's definitions come from Valve's
# Steamworks SDK 1.55 (-SteamSdk, its public/steam/steam_api.json; by default tools/steamworks-sdk-1.55), which needs a
# Steam sign-in to download: pinned, never committed or shipped, and optional. NativeAOT needs Visual Studio's C++ build tools.
# -SteamCensus packages the Steam census build of that stand-in instead (docs/steam-bridge.md, census step 2), as
# hellforge-engine-<version>-census.zip: for a census session only, never for release.
# -NoSteamApi builds and tests without Valve's steam_api64.dll and Steamworks SDK (CI, .github/workflows/ci.yml, has
# neither): the stand-in and its census build are built against a synthetic export table (New-SyntheticSteamApi), so
# their tests and the mod loader's smoke run, but they are not the game's. No Steam bridge or broker, no build.json and no
# package: the NativeAOT builds go to artifacts/engine-package/no-steam-api, for their check only.
param([switch]$SkipSmoke,[string]$SteamApi,[switch]$SteamCensus,[string]$SteamSdk,[switch]$NoSteamApi)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$ProgressPreference='SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$project=Split-Path -Parent $PSScriptRoot
$open=$project # Core, the mod SDK, the mappings and the example mods are in this repository too
$sdk=Join-Path $open 'native/mod-sdk'
$sources=Join-Path $project 'native/sdk-shims'
$gen=Join-Path $project 'artifacts/sdk-shims/gen'
$out=Join-Path $project 'artifacts/sdk-shims/out'
$census=Join-Path $project 'artifacts/sdk-shims/census'
$version=([xml](Get-Content -LiteralPath (Join-Path $project 'Directory.Build.props') -Raw)).Project.PropertyGroup.EngineVersion
$gameBuild=(Get-Content -LiteralPath (Join-Path $open 'src/HellForge.Core/supported-build.json') -Raw|ConvertFrom-Json).buildId

function Get-Pinned([string]$Url,[string]$Sha256,[string]$Path){
    if((Test-Path -LiteralPath $Path) -and (Get-FileHash -LiteralPath $Path).Hash -eq $Sha256){return}
    New-Item -ItemType Directory (Split-Path -Parent $Path) -Force|Out-Null
    $partial=$Path+'.partial'
    Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing
    if((Get-FileHash -LiteralPath $partial).Hash -ne $Sha256){Remove-Item -LiteralPath $partial;throw ('Checksum mismatch: '+$Url)}
    Move-Item -LiteralPath $partial -Destination $Path -Force
}
function Expand-Fresh([string]$Archive,[string]$Destination){
    $marker=Join-Path $Destination '.hellforge-archive-sha256'
    $sha=(Get-FileHash -LiteralPath $Archive).Hash
    if((Test-Path -LiteralPath $marker) -and [IO.File]::ReadAllText($marker) -ceq $sha){return}
    if(Test-Path -LiteralPath $Destination){Remove-Item -LiteralPath $Destination -Recurse -Force}
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive,$Destination)
    [IO.File]::WriteAllText($marker,$sha)
}
function Assert-Hash([string]$Path,[string]$Sha256,[string]$What){
    if((Get-FileHash -LiteralPath $Path).Hash -ne $Sha256){throw ($What+' does not match its pin: '+$Path)}
}

# -NoSteamApi: a steam_api64.dll with only the export table the stand-in's build and tests need, every export returning
# 0. The stand-in's own functions (steam_cloud.c's HELLFORGE_API, which the generator requires the original to export),
# and those its tests call through it: the forwarded one smoke-cloud resolves (CloudSmoke.cs) and the census test's thunks,
# one of them missing from its fake Steam DLL (census_test.c).
function New-SyntheticSteamApi([string]$Handwritten){
    $names=@([regex]::Matches([IO.File]::ReadAllText($Handwritten),'(?m)^HELLFORGE_API\s+[\w\s\*]+?\b(\w+)\s*\(')|ForEach-Object {$_.Groups[1].Value})
    $names+=@('SteamGameServer_GetHSteamUser','SteamGameServer_GetHSteamPipe','SteamAPI_ISteamUserStats_UpdateAvgRateStat','SteamAPI_ISteamInput_TriggerVibrationExtended')
    $directory=Join-Path $project 'artifacts/sdk-shims/synthetic-steam-api'
    if(Test-Path -LiteralPath $directory){Remove-Item -LiteralPath $directory -Recurse -Force}
    New-Item -ItemType Directory $directory|Out-Null
    $source=Join-Path $directory 'synthetic_steam_api.g.c'
    $lines=@('// Generated by scripts/Build-Engine.ps1 -NoSteamApi: the export table of a steam_api64.dll for builds without Valve''s.')
    $lines+=@($names|Sort-Object -Unique|ForEach-Object {'__declspec(dllexport) void *'+$_+'(void) { return 0; }'})
    [IO.File]::WriteAllLines($source,[string[]]$lines)
    $dll=Join-Path $directory 'steam_api64.dll'
    & $zig cc -target x86_64-windows-gnu -shared -O1 -std=c11 -Wall -Werror -s $source -o $dll
    if($LASTEXITCODE){throw 'Compiling the synthetic steam_api64.dll failed.'}
    $dll
}

$buildTools=& (Join-Path $open 'scripts/Get-BuildTools.ps1') -Root $project
$zig=$buildTools.Zig;$minHook=$buildTools.MinHook;$downloads=$buildTools.Downloads;$tools=$buildTools.Tools

# The loader's field and content tables, generated from the mappings.
Push-Location $open
try{
    dotnet run --project (Join-Path $open 'src/HellForge.Cli') -v q -- sdk-headers --repo $open|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Generating the loader''s tables (sdk-headers) failed.'}
}finally{Pop-Location}

# PlayFab Party 1.10.16 (NuGet) and PlayFab Multiplayer 1.8.7 (GitHub release).
$partyPackage=Join-Path $tools 'sdk-packages/party-1.10.16'
$archive=Join-Path $downloads 'microsoft.playfab.playfabparty.cpp.windows.1.10.16.nupkg'
Get-Pinned 'https://api.nuget.org/v3-flatcontainer/microsoft.playfab.playfabparty.cpp.windows/1.10.16/microsoft.playfab.playfabparty.cpp.windows.1.10.16.nupkg' '3C8A5170C5FFED42478617BD56CC7FD33A66040C925AE84E1467730B5F828D08' $archive
Expand-Fresh $archive $partyPackage
$lobbyPackage=Join-Path $tools 'sdk-packages/multiplayer-1.8.7'
$archive=Join-Path $downloads 'Microsoft.PlayFab.PlayFabMultiplayer.Cpp.Windows-1.8.7.zip'
Get-Pinned 'https://github.com/PlayFab/PlayFabMultiplayer/releases/download/v1.8.7/Microsoft.PlayFab.PlayFabMultiplayer.Cpp.Windows.zip' 'F1331238B1195F7D0180BB1C75B5F546E0515FBBD4CB6DFE33571C452C8CC297' $archive
Expand-Fresh $archive $lobbyPackage
$partyOriginal=Join-Path $partyPackage 'build/native/lib/x64/release/PartyWin.dll'
$lobbyOriginal=Join-Path $lobbyPackage 'bin/PlayFabMultiplayerWin.dll'
# The DLLs HD2 ships (build F5FEE03DCFDB) are byte-identical to these.
Assert-Hash $partyOriginal 'DE34667C2F7084013D9C03CD5D1CB5DC3321BC36BB616A845E35534E4E7995E0' 'Packaged PartyWin.dll'
Assert-Hash $lobbyOriginal '95BC5F958E519F612B479DE4227F3BDFFABA9B2B8DE987EB3F7991960B8CED17' 'Packaged PlayFabMultiplayerWin.dll'
$partyInclude=Join-Path $partyPackage 'build/native/include'
$lobbyInclude=Join-Path $lobbyPackage 'include'
# steam_api64.dll of the supported build (supported-build.json lists it by size and SHA-1).
$steamApiPin='1DB3FD414039D3E5815A5721925DD2E0A3A9F2549603C6CAB7C49B84966A1AF3'
if($NoSteamApi){
    if($SteamApi -or $SteamCensus -or $SteamSdk){throw '-NoSteamApi builds no package: it takes no -SteamApi, -SteamCensus or -SteamSdk.'}
    $SteamApi=New-SyntheticSteamApi (Join-Path $project 'native/steam-cloud/steam_cloud.c')
}
elseif(-not $SteamApi){
    $steamRoot=(Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if($steamRoot){
        $libraries=@($steamRoot)+@([regex]::Matches([IO.File]::ReadAllText((Join-Path $steamRoot 'steamapps/libraryfolders.vdf')),'"path"\s+"([^"]+)"')|ForEach-Object {$_.Groups[1].Value.Replace('\\','\')})
        $SteamApi=@($libraries|ForEach-Object {Join-Path $_ 'steamapps/common/Helldivers 2/bin/steam_api64.dll'}|Where-Object {Test-Path -LiteralPath $_})[0]
    }
    if(-not $SteamApi){throw 'HELLDIVERS 2 was not found in Steam; pass -SteamApi with the game''s bin/steam_api64.dll.'}
}
if(-not $NoSteamApi){Assert-Hash $SteamApi $steamApiPin 'steam_api64.dll'}
# SDK 1.55's steam_api.json: the version whose steam_api64.dll is the pinned one (docs/steam-bridge.md). Without it the
# package has no bridge build and no broker, and a sandboxed session refuses to start. The pin is of the file with LF
# line ends: Valve's zip has CRLF, and a copy may have either, so CR LF counts as LF (the generator reads it as JSON).
$steamSdkPin='80A6C2196F6048749EE35A141C91083B29613FD7C56C2207C9FC73EF87566547'
if(-not $SteamSdk){$SteamSdk=Join-Path $project 'tools/steamworks-sdk-1.55/public/steam/steam_api.json'}
# (the bridge's vtable slots come from Valve's DLL itself, so -NoSteamApi has no bridge)
$bridge=-not $NoSteamApi -and (Test-Path -LiteralPath $SteamSdk)
if($NoSteamApi){Write-Output '-NoSteamApi: building without the Steam bridge and broker (they need Valve''s steam_api64.dll and Steamworks SDK).'}
elseif($bridge){
    $sdkText=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $SteamSdk).Path)
    $sdkLf=[Text.Encoding]::UTF8.GetBytes([Text.Encoding]::UTF8.GetString($sdkText).Replace("`r`n","`n"))
    $sdkHash=-join ([Security.Cryptography.SHA256]::Create().ComputeHash($sdkLf)|ForEach-Object {$_.ToString('X2')})
    if($sdkHash -ne $steamSdkPin){throw ('steam_api.json (Steamworks SDK 1.55) does not match its pin: '+$SteamSdk)}
}
else{Write-Warning ('No Steamworks SDK 1.55 at '+$SteamSdk+': building without the Steam bridge.')}

dotnet build (Join-Path $project 'src/HellForge.SdkShimGen/HellForge.SdkShimGen.csproj') --nologo -v q
if($LASTEXITCODE){throw 'HellForge.SdkShimGen build failed.'}
$generator=Join-Path $project 'src/HellForge.SdkShimGen/bin/Debug/net10.0/HellForge.SdkShimGen.dll'
function Invoke-Generator{& dotnet $generator @args;if($LASTEXITCODE){throw ('HellForge.SdkShimGen '+$args[0]+' failed.')}}

foreach($directory in @($gen,$out,$census)){if(Test-Path -LiteralPath $directory){Remove-Item -LiteralPath $directory -Recurse -Force};New-Item -ItemType Directory $directory|Out-Null}
Invoke-Generator generate party $partyOriginal (Join-Path $sources 'party_shim.cpp') $gen (Join-Path $partyInclude 'Party_c.h')
Invoke-Generator generate lobby $lobbyOriginal (Join-Path $sources 'lobby_shim.cpp') $gen (Join-Path $lobbyInclude 'PFMultiplayer.h') (Join-Path $lobbyInclude 'PFLobby.h') (Join-Path $lobbyInclude 'PFMatchmaking.h')
$flags=@('c++','-target','x86_64-windows-gnu','-shared','-O2','-std=c++17','-fno-exceptions','-fno-rtti','-Wall','-Werror','-Wno-unused-parameter','-I',$sources,'-I',$gen)
foreach($dll in @(
    @{name='PartyWin';prefix='party';include=$partyInclude;defines=@()},
    @{name='PlayFabMultiplayerWin';prefix='lobby';include=$lobbyInclude;defines=@('-DPFMULTIPLAYER_INCLUDE_SERVER_APIS')})){
    & $zig @flags @($dll.defines) '-I' $dll.include (Join-Path $sources 'shim_common.cpp') (Join-Path $sources 'link.cpp') (Join-Path $sources ($dll.prefix+'_shim.cpp')) `
        (Join-Path $gen ($dll.prefix+'_gen.g.cpp')) (Join-Path $gen ($dll.name+'.def')) '-o' (Join-Path $out ($dll.name+'.dll'))
    if($LASTEXITCODE){throw ('Compiling '+$dll.name+'.dll failed.')}
}
& $zig cc -target x86_64-windows-gnu -shared -O2 -std=c11 -Wall -Werror -Wno-unused-parameter -s -I $sdk -I (Join-Path $minHook 'include') -I (Join-Path $minHook 'src') `
    (Join-Path $project 'native/mod-loader/hellforge_modloader.c') (Join-Path $minHook 'src/buffer.c') (Join-Path $minHook 'src/hook.c') `
    (Join-Path $minHook 'src/trampoline.c') (Join-Path $minHook 'src/hde/hde64.c') -o (Join-Path $out 'hellforge_modloader.dll')
if($LASTEXITCODE){throw 'Compiling hellforge_modloader.dll failed.'}
Copy-Item -LiteralPath (Join-Path $minHook 'LICENSE.txt') -Destination (Join-Path $out 'MinHook-LICENSE.txt')
$cloudSource=Join-Path $project 'native/steam-cloud/steam_cloud.c'
Invoke-Generator forward $SteamApi steam_api64_hellforge $cloudSource (Join-Path $gen 'steam_api64.def')
# with the network gate (net_gate.c, MinHook), hooked in its DllMain before the game's own code runs, the Steam census
# (steam_census.c, off unless asked for) and the bridge's game side (bridge_channel.c, used when a session names a broker)
$minHookSources=@('src/buffer.c','src/hook.c','src/trampoline.c','src/hde/hde64.c')|ForEach-Object {Join-Path $minHook $_}
$cloudSources=@('net_gate.c','steam_census.c','bridge_channel.c')|ForEach-Object {Join-Path $project ('native/steam-cloud/'+$_)}
& $zig cc -target x86_64-windows-gnu -shared -O2 -std=c11 -Wall -Werror -Wno-unused-parameter -s -I (Join-Path $minHook 'include') $cloudSource `
    @cloudSources @minHookSources (Join-Path $gen 'steam_api64.def') -ladvapi32 -o (Join-Path $out 'steam_api64.dll')
if($LASTEXITCODE){throw 'Compiling steam_api64.dll failed.'}
# The Steam census build: the same stand-in, but every function export it does not write itself is a counting thunk to
# Valve's, and the interfaces the game gets are counting proxies (HELLFORGE_STEAM_CENSUS_THUNKS). Built and tested every
# time, so it keeps working; packaged only with -SteamCensus.
Invoke-Generator census-thunks $SteamApi steam_api64_hellforge $cloudSource (Join-Path $gen 'steam_api64_census.def') (Join-Path $gen 'census_thunks.g.c')
& $zig cc -target x86_64-windows-gnu -shared -O2 -std=c11 -Wall -Werror -Wno-unused-parameter -s -DHELLFORGE_STEAM_CENSUS_THUNKS -I (Join-Path $minHook 'include') $cloudSource `
    @cloudSources (Join-Path $gen 'census_thunks.g.c') @minHookSources (Join-Path $gen 'steam_api64_census.def') -ladvapi32 -o (Join-Path $census 'steam_api64.dll')
if($LASTEXITCODE){throw 'Compiling the Steam census build of steam_api64.dll failed.'}
Get-ChildItem -LiteralPath $census -Filter '*.lib'|Remove-Item
# The Steam bridge (docs/steam-bridge.md, phase 3): its generated proxies, dispatch and export stubs (SdkShimGen
# steam-bridge, from the SDK, the pinned DLL and the reviewed allow-list), the bridge build of the stand-in, in which no
# export forwards to Valve's DLL, and the broker: Valve's DLL at the player's normal integrity, for a low-integrity game.
$steamCloud=Join-Path $project 'native/steam-cloud'
$bridgeSources=@('bridge_broker.c','bridge_channel.c')|ForEach-Object {Join-Path $steamCloud $_}
foreach($name in @('steam_api64_bridge.dll','hellforge_steam_broker.exe')){$old=Join-Path $out $name;if(Test-Path -LiteralPath $old){Remove-Item -LiteralPath $old}}
if($bridge){
    $bridgeExports=Join-Path $steamCloud 'bridge_exports.c'
    Invoke-Generator steam-bridge $SteamSdk $SteamApi (Join-Path $steamCloud 'bridge-methods.json') ($cloudSource+','+$bridgeExports) $gen
    $bridgeStandIn=@((Join-Path $steamCloud 'bridge_local.c'),$bridgeExports,(Join-Path $gen 'bridge_proxies_gen.c'),(Join-Path $gen 'bridge_exports_gen.c'))
    & $zig cc -target x86_64-windows-gnu -shared -O2 -std=c11 -Wall -Werror -Wno-unused-parameter -s -DHELLFORGE_BRIDGE_BUILD -I (Join-Path $minHook 'include') `
        -I $steamCloud -I $gen $cloudSource @cloudSources @bridgeStandIn @minHookSources (Join-Path $gen 'steam_api64_bridge.def') -ladvapi32 -o (Join-Path $out 'steam_api64_bridge.dll')
    if($LASTEXITCODE){throw 'Compiling the bridge build of steam_api64.dll failed.'}
    $brokerSources=@($bridgeSources)+@((Join-Path $steamCloud 'broker_bridge.c'),(Join-Path $steamCloud 'broker_lobby.c'),(Join-Path $gen 'broker_dispatch_gen.c'))
    & $zig cc -target x86_64-windows-gnu -municode -O2 -std=c11 -Wall -Werror -Wno-unused-parameter -s -I $steamCloud -I $gen (Join-Path $steamCloud 'hellforge_steam_broker.c') `
        @brokerSources -ladvapi32 -lws2_32 -o (Join-Path $out 'hellforge_steam_broker.exe')
    if($LASTEXITCODE){throw 'Compiling hellforge_steam_broker.exe failed.'}
}
# The network block tool: puts the copy's egress block straight into the Windows Filtering Platform (WFP), so the
# block holds with Windows Firewall off and a security suite's own permit sublayer cannot override our BLOCK. It
# installs/removes/reads persistent WFP filters (elevated), doing the firewall rules' old job (NetworkBlock.Common.ps1).
& $zig cc -target x86_64-windows-gnu -municode -O2 -std=c11 -Wall -Werror -Wno-unused-parameter -s `
    (Join-Path $project 'native/network-block/hellforge_wfp.c') -lfwpuclnt -lole32 -lws2_32 -o (Join-Path $out 'hellforge_wfp.exe')
if($LASTEXITCODE){throw 'Compiling hellforge_wfp.exe failed.'}
Get-ChildItem -LiteralPath $out -Filter '*.lib'|Remove-Item
Invoke-Generator verify $partyOriginal (Join-Path $out 'PartyWin.dll')
Invoke-Generator verify $lobbyOriginal (Join-Path $out 'PlayFabMultiplayerWin.dll')
Invoke-Generator verify-forward $SteamApi steam_api64_hellforge $cloudSource (Join-Path $out 'steam_api64.dll')
# The mod loader's test DLLs and two example code mods, then its end-to-end smoke test.
$testOut=Join-Path $project 'artifacts/sdk-shims/test'
New-Item -ItemType Directory $testOut -Force|Out-Null
& $zig cc -target x86_64-windows-gnu -O1 -std=c11 -Wall -Werror -Wno-unused-parameter -I $sdk -I (Join-Path $minHook 'include') -I (Join-Path $minHook 'src') `
    (Join-Path $project 'native/mod-loader/test/stats_test.c') (Join-Path $minHook 'src/buffer.c') (Join-Path $minHook 'src/hook.c') `
    (Join-Path $minHook 'src/trampoline.c') (Join-Path $minHook 'src/hde/hde64.c') -o (Join-Path $testOut 'stats_test.exe')
if($LASTEXITCODE){throw 'Compiling the stats fixture failed.'}
if(-not $SkipSmoke){& (Join-Path $testOut 'stats_test.exe');if($LASTEXITCODE){throw 'Stats fixture failed.'}}
& $zig cc -target x86_64-windows-gnu -O1 -std=c11 -municode -Wall -Werror -Wno-unused-parameter (Join-Path $project 'native/steam-cloud/test/net_gate_test.c') -lws2_32 `
    -o (Join-Path $testOut 'net_gate_test.exe')
if($LASTEXITCODE){throw 'Compiling the network gate test failed.'}
& $zig cc -target x86_64-windows-gnu -O1 -std=c11 -Wall -Werror -Wno-unused-parameter (Join-Path $project 'native/common/test/privacy_test.c') -o (Join-Path $testOut 'privacy_test.exe')
if($LASTEXITCODE){throw 'Compiling the log privacy test failed.'}
if(-not $SkipSmoke){
    & (Join-Path $testOut 'privacy_test.exe')|Out-Null;if($LASTEXITCODE){throw 'Log privacy test failed: native logs name the PC.'}
    & (Join-Path $testOut 'net_gate_test.exe') (Join-Path $out 'steam_api64.dll') (Join-Path $testOut 'net-gate-census.jsonl');if($LASTEXITCODE){throw 'Network gate test failed.'}
    & (Join-Path $testOut 'net_gate_test.exe') (Join-Path $out 'steam_api64.dll') (Join-Path $testOut 'net-gate-dns.jsonl') 'dns';if($LASTEXITCODE){throw 'Network gate group filter test failed.'}
    # The WFP block tool with no arguments prints usage and exits 2: a check it built and starts (install/uninstall
    # need administrator rights, so the round trip is tested by hand, not here).
    & (Join-Path $out 'hellforge_wfp.exe')|Out-Null;if($LASTEXITCODE -ne 2){throw 'Network block tool did not start.'}
}
# The census build next to a fake Steam DLL in its place (steam_api64_hellforge.dll), then its test.
$censusTest=Join-Path $testOut 'census'
if(Test-Path -LiteralPath $censusTest){Remove-Item -LiteralPath $censusTest -Recurse -Force}
New-Item -ItemType Directory $censusTest|Out-Null
Copy-Item -LiteralPath (Join-Path $census 'steam_api64.dll') -Destination $censusTest
& $zig cc -target x86_64-windows-gnu -shared -O1 -std=c11 -Wall -Werror -Wno-unused-parameter -s (Join-Path $project 'native/steam-cloud/test/fake_steam.c') `
    -o (Join-Path $censusTest 'steam_api64_hellforge.dll')
if($LASTEXITCODE){throw 'Compiling the fake Steam DLL failed.'}
& $zig cc -target x86_64-windows-gnu -O1 -std=c11 -municode -Wall -Werror -Wno-unused-parameter (Join-Path $project 'native/steam-cloud/test/census_test.c') `
    -o (Join-Path $testOut 'census_test.exe')
if($LASTEXITCODE){throw 'Compiling the Steam census test failed.'}
Get-ChildItem -LiteralPath $censusTest -Filter '*.lib'|Remove-Item
if(-not $SkipSmoke){& (Join-Path $testOut 'census_test.exe') (Join-Path $censusTest 'steam_api64.dll') (Join-Path $testOut 'steam-census.jsonl');if($LASTEXITCODE){throw 'Steam census test failed.'}}
# The bridge's channel and serving loop, both ends in one test (a second process plays a client the broker must refuse).
& $zig cc -target x86_64-windows-gnu -O1 -std=c11 -municode -Wall -Werror -Wno-unused-parameter (Join-Path $project 'native/steam-cloud/test/broker_test.c') `
    @bridgeSources -ladvapi32 -o (Join-Path $testOut 'broker_test.exe')
if($LASTEXITCODE){throw 'Compiling the Steam broker test failed.'}
# and the bridge build of the stand-in, as the game loads it, with the fake Steam DLL beside it as Valve's, which must
# never load (without the SDK there is no bridge build, and only the channel is tested)
$bridgeTest=Join-Path $testOut 'bridge'
if(Test-Path -LiteralPath $bridgeTest){Remove-Item -LiteralPath $bridgeTest -Recurse -Force}
New-Item -ItemType Directory $bridgeTest|Out-Null
if($bridge){Copy-Item -LiteralPath (Join-Path $out 'steam_api64_bridge.dll') -Destination (Join-Path $bridgeTest 'steam_api64.dll')}
Copy-Item -LiteralPath (Join-Path $censusTest 'steam_api64_hellforge.dll') -Destination $bridgeTest
$brokerTestArgs=if($bridge){@(Join-Path $bridgeTest 'steam_api64.dll')}else{@()}
if(-not $SkipSmoke){& (Join-Path $testOut 'broker_test.exe') @brokerTestArgs;if($LASTEXITCODE){throw 'Steam broker test failed.'}}
# The bridge end to end: the broker's loop with a fake Valve DLL behind the generated dispatch, and the bridge build of the
# stand-in called as the game calls it (bridge_test.c).
if($bridge){
    & $zig cc -target x86_64-windows-gnu -shared -O1 -std=c11 -Wall -Werror -Wno-unused-parameter -s (Join-Path $steamCloud 'test/fake_valve.c') -o (Join-Path $bridgeTest 'fake_valve.dll')
    if($LASTEXITCODE){throw 'Compiling the fake Valve DLL failed.'}
    & $zig cc -target x86_64-windows-gnu -O1 -std=c11 -municode -Wall -Werror -Wno-unused-parameter -I $steamCloud -I $gen (Join-Path $steamCloud 'test/bridge_test.c') `
        @bridgeSources (Join-Path $steamCloud 'broker_bridge.c') (Join-Path $steamCloud 'broker_lobby.c') (Join-Path $gen 'broker_dispatch_gen.c') -ladvapi32 -lws2_32 -o (Join-Path $testOut 'bridge_test.exe')
    if($LASTEXITCODE){throw 'Compiling the Steam bridge test failed.'}
    Get-ChildItem -LiteralPath $bridgeTest -Filter '*.lib'|Remove-Item
    if(-not $SkipSmoke){& (Join-Path $testOut 'bridge_test.exe') (Join-Path $out 'steam_api64_bridge.dll') (Join-Path $bridgeTest 'fake_valve.dll');if($LASTEXITCODE){throw 'Steam bridge test failed.'}}
}
# several installations on one PC: the game's own Steam lobby kept between their brokers (broker_lobby.c, which needs
# nothing generated from the SDK, so it is tested without it too)
& $zig cc -target x86_64-windows-gnu -O1 -std=c11 -municode -Wall -Werror -Wno-unused-parameter -I $steamCloud (Join-Path $steamCloud 'test/lobby_test.c') `
    (Join-Path $steamCloud 'broker_lobby.c') -lws2_32 -o (Join-Path $testOut 'lobby_test.exe')
if($LASTEXITCODE){throw 'Compiling the local Steam lobby test failed.'}
if(-not $SkipSmoke){& (Join-Path $testOut 'lobby_test.exe');if($LASTEXITCODE){throw 'Local Steam lobby test failed.'}}
& $zig cc -target x86_64-windows-gnu -shared -O1 -std=c11 -Wall -Werror -Wno-unused-parameter -s (Join-Path $project 'native/mod-loader/test/fake_game.c') -o (Join-Path $testOut 'fake_game.dll')
if($LASTEXITCODE){throw 'Compiling the fake game failed.'}
& $zig cc -target x86_64-windows-gnu -shared -O2 -std=c11 -Wall -Werror -Wno-unused-parameter -s -I $sdk `
    (Join-Path $project 'native/mod-loader/test/hook_test_mod.c') -o (Join-Path $testOut 'hook_test_mod.dll')
if($LASTEXITCODE){throw 'Compiling the hook test mod failed.'}
Get-ChildItem -LiteralPath $testOut -Filter '*.lib'|Remove-Item
$machineGun=Join-Path $open 'examples/mods/machine-gun-10k-code'
& (Join-Path $open 'scripts/Build-Mod.ps1') $machineGun|Out-Null
$compendium=Join-Path $open 'examples/mods/stratagem-compendium'
& (Join-Path $open 'scripts/Build-Mod.ps1') $compendium|Out-Null
if(-not $SkipSmoke){Invoke-Generator smoke $out;Invoke-Generator smoke-cloud (Join-Path $out 'steam_api64.dll') $SteamApi;Invoke-Generator smoke-modloader $out $testOut $machineGun $compendium}
# The session runner and the local backend, compiled ahead of time to native code (NativeAOT needs the Visual Studio
# C++ linker, which it finds through vswhere).
function Publish-Aot([string]$Staging){
    foreach($app in @(@{folder='session';project='src/HellForge.Session/HellForge.Session.csproj'},@{folder='backend';project='src/HellForge.LocalBackend/HellForge.LocalBackend.csproj'})){
        Write-Output ('Compiling '+$app.folder+' (NativeAOT)')
        dotnet publish (Join-Path $project $app.project) -c Release -p:DebugType=none -p:DebugSymbols=false --nologo -v q -o (Join-Path $Staging $app.folder)
        if($LASTEXITCODE){throw ('Compiling '+$app.folder+' failed.')}
        Get-ChildItem -LiteralPath (Join-Path $Staging $app.folder) -File|Where-Object {$_.Extension -in @('.pdb','.dbg','.map')}|Remove-Item
    }
    if(-not $SkipSmoke){& (Join-Path $PSScriptRoot 'Test-BackendAot.ps1') -Aot (Join-Path $Staging 'backend/HellForge.LocalBackend.exe')|Select-Object -Last 1}
}
$env:PATH+=';'+(Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer')
$package=Join-Path $project 'artifacts/engine-package'
# -NoSteamApi: the stand-in is not the game's, so nothing is packaged, and the NativeAOT builds stay out of the staging
# folder a launcher run from the repository uses
if($NoSteamApi){
    $staging=Join-Path $package 'no-steam-api'
    if(Test-Path -LiteralPath $staging){Remove-Item -LiteralPath $staging -Recurse -Force}
    New-Item -ItemType Directory $staging|Out-Null
    Publish-Aot $staging
    Write-Output 'Built and tested without Valve''s steam_api64.dll (-NoSteamApi): no build.json, no engine package.'
    return
}
$receipt=[ordered]@{builtUtc=[DateTime]::UtcNow.ToString('o');zig='0.17.0';
    originals=[ordered]@{'PartyWin.dll'=(Get-FileHash $partyOriginal).Hash;'PlayFabMultiplayerWin.dll'=(Get-FileHash $lobbyOriginal).Hash;'steam_api64.dll'=$steamApiPin};
    standIns=[ordered]@{'PartyWin.dll'=(Get-FileHash (Join-Path $out 'PartyWin.dll')).Hash;'PlayFabMultiplayerWin.dll'=(Get-FileHash (Join-Path $out 'PlayFabMultiplayerWin.dll')).Hash;
        'steam_api64.dll'=(Get-FileHash (Join-Path $(if($SteamCensus){$census}else{$out}) 'steam_api64.dll')).Hash};
    modLoader=(Get-FileHash (Join-Path $out 'hellforge_modloader.dll')).Hash;
    networkBlock=(Get-FileHash (Join-Path $out 'hellforge_wfp.exe')).Hash}
# a sandboxed session swaps in the bridge build and starts the broker only when both are here, as pinned
if($bridge){
    $receipt.steamBridge=(Get-FileHash (Join-Path $out 'steam_api64_bridge.dll')).Hash
    $receipt.steamBroker=(Get-FileHash (Join-Path $out 'hellforge_steam_broker.exe')).Hash
}
$receipt|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $out 'build.json') -Encoding UTF8

$staging=Join-Path $package 'staging'
if(Test-Path -LiteralPath $staging){Remove-Item -LiteralPath $staging -Recurse -Force}
New-Item -ItemType Directory $staging|Out-Null
Publish-Aot $staging
$shimTarget=New-Item -ItemType Directory (Join-Path $staging 'sdk-shims')
foreach($name in @('build.json','PartyWin.dll','PlayFabMultiplayerWin.dll','steam_api64.dll','hellforge_modloader.dll','MinHook-LICENSE.txt')){Copy-Item -LiteralPath (Join-Path $out $name) -Destination $shimTarget}
if($bridge){foreach($name in @('steam_api64_bridge.dll','hellforge_steam_broker.exe')){Copy-Item -LiteralPath (Join-Path $out $name) -Destination $shimTarget}}
# build.json's standIns pin names this one with -SteamCensus, since the session checks the stand-in it swaps in against it.
if($SteamCensus){Copy-Item -LiteralPath (Join-Path $census 'steam_api64.dll') -Destination $shimTarget -Force}
$scriptTarget=New-Item -ItemType Directory (Join-Path $staging 'scripts')
foreach($name in @('NetworkBlock.Common.ps1','Install-NetworkBlock.ps1','Install-WfpBlock.ps1','Test-SteamOwnership.ps1','OfflineControlNetworkProbe.cs')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $scriptTarget}
# The WFP block tool ships beside the block scripts that drive it.
Copy-Item -LiteralPath (Join-Path $out 'hellforge_wfp.exe') -Destination $scriptTarget
foreach($name in @('LICENSE','LICENSE-EXCEPTION')){Copy-Item -LiteralPath (Join-Path $project $name) -Destination $staging}
# Nothing that ships names the PC it was built on: its user folder (debug paths) or the git author's email.
$private=@($env:USERPROFILE,$env:USERPROFILE.Replace('\','/'),(git -C $project config user.email))|Where-Object {$_}
foreach($file in Get-ChildItem -LiteralPath $staging -Recurse -File){
    $bytes=[IO.File]::ReadAllBytes($file.FullName)
    $texts=@([Text.Encoding]::ASCII.GetString($bytes),[Text.Encoding]::Unicode.GetString($bytes))
    foreach($value in $private){if(@($texts|Where-Object {$_.IndexOf($value,[StringComparison]::OrdinalIgnoreCase) -ge 0}).Count){throw ('The engine names this PC ('+$value+'): '+$file.FullName)}}
}
$files=[ordered]@{}
foreach($file in Get-ChildItem -LiteralPath $staging -Recurse -File|Sort-Object FullName){$files[$file.FullName.Substring($staging.Length+1).Replace('\','/')]=(Get-FileHash -LiteralPath $file.FullName).Hash}
$commit=(git -C $project rev-parse --short HEAD 2>$null)
[ordered]@{version=$version;build=$gameBuild;commit=$commit;steamCensus=[bool]$SteamCensus;builtUtc=[DateTime]::UtcNow.ToString('o');files=$files}|ConvertTo-Json -Depth 4|
    Set-Content -LiteralPath (Join-Path $staging 'engine.json') -Encoding UTF8
$zip=Join-Path $package ('hellforge-engine-'+$version+$(if($SteamCensus){'-census'}else{''})+'.zip')
if(Test-Path -LiteralPath $zip){Remove-Item -LiteralPath $zip}
# Entry names with forward slashes, as zips name folders (Windows PowerShell's CreateFromDirectory writes backslashes).
$archive=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Create)
try{
    foreach($file in Get-ChildItem -LiteralPath $staging -Recurse -File|Sort-Object FullName){
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$file.FullName.Substring($staging.Length+1).Replace('\','/'),[IO.Compression.CompressionLevel]::Optimal)
    }
}finally{$archive.Dispose()}
Write-Output ('Built {0} ({1:N0} MB). SHA-256 {2}' -f $zip,((Get-Item -LiteralPath $zip).Length/1MB),(Get-FileHash -LiteralPath $zip).Hash)
