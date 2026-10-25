# Builds artifacts/package/HellForge-Setup.exe, the one file a new player needs: the app as it goes into an installation
# (scripts/Publish-HellForge.ps1 -PayloadFolder), zipped and embedded in the self-contained setup program (src/HellForge.Setup).
# It contains HellForge only; every player's setup copies the game from their own Steam library.
# -EnginePackage / -SteamApi: as Publish-HellForge.ps1 takes them (by default it builds the engine from this repository).
param([string]$EnginePackage,[string]$SteamApi)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
$project=Split-Path -Parent $PSScriptRoot
$package=Join-Path $project 'artifacts/package'
$payload=Join-Path $package 'payload'
$zip=Join-Path $package 'payload.zip'

$engineArgs=@{};if($EnginePackage){$engineArgs.EnginePackage=$EnginePackage};if($SteamApi){$engineArgs.SteamApi=$SteamApi}
& (Join-Path $PSScriptRoot 'Publish-HellForge.ps1') -PayloadFolder $payload @engineArgs
# Nothing that ships names the PC it was built on: its user folder (debug paths, __FILE__) or the git author's email.
$private=@($env:USERPROFILE,$env:USERPROFILE.Replace('\','/'),(git -C $project config user.email))|Where-Object {$_}
foreach($file in Get-ChildItem -LiteralPath $payload -Recurse -File){
    $bytes=[IO.File]::ReadAllBytes($file.FullName)
    $texts=@([Text.Encoding]::ASCII.GetString($bytes),[Text.Encoding]::Unicode.GetString($bytes))
    foreach($value in $private){if(@($texts|Where-Object {$_.IndexOf($value,[StringComparison]::OrdinalIgnoreCase) -ge 0}).Count){throw ('The payload names this PC ('+$value+'): '+$file.FullName)}}
}
if(Test-Path -LiteralPath $zip){Remove-Item -LiteralPath $zip}
[IO.Compression.ZipFile]::CreateFromDirectory($payload,$zip,[IO.Compression.CompressionLevel]::Optimal,$false)

$setup=Join-Path $package 'setup'
if(Test-Path -LiteralPath $setup){Remove-Item -LiteralPath $setup -Recurse -Force}
dotnet publish (Join-Path $project 'src/HellForge.Setup/HellForge.Setup.csproj') -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true `
    "-p:PayloadZip=$zip" --nologo -v q -o $setup
if($LASTEXITCODE){throw 'Building HellForge-Setup.exe failed.'}
$result=Join-Path $package 'HellForge-Setup.exe'
Copy-Item -LiteralPath (Join-Path $setup 'HellForge-Setup.exe') -Destination $result -Force
$file=Get-Item -LiteralPath $result

# HellForge-Linux-scripts.zip, the release's second file: linux/ and docs/linux.md in a HellForge-Linux folder. Entries are
# named here, with '/': a zip made by Windows PowerShell can name them 'folder\file', which Linux unpacks as one file name
# (a test zip handed out on Discord did).
$linuxZip=Join-Path $package 'HellForge-Linux-scripts.zip'
if(Test-Path -LiteralPath $linuxZip){Remove-Item -LiteralPath $linuxZip}
$archive=[IO.Compression.ZipFile]::Open($linuxZip,[IO.Compression.ZipArchiveMode]::Create)
try{
    $files=@(Get-ChildItem -LiteralPath (Join-Path $project 'linux') -File|Sort-Object Name)+@(Get-Item -LiteralPath (Join-Path $project 'docs/linux.md'))
    foreach($source in $files){
        $entry=$archive.CreateEntry('HellForge-Linux/'+$source.Name,[IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime=$source.LastWriteTime
        $out=$entry.Open();try{$bytes=[IO.File]::ReadAllBytes($source.FullName);$out.Write($bytes,0,$bytes.Length)}finally{$out.Dispose()}
    }
}finally{$archive.Dispose()}
Write-Output ('Built {0} ({1:N0} MB, app archive {2:N0} MB). SHA-256 {3}' -f $file.FullName,($file.Length/1MB),((Get-Item -LiteralPath $zip).Length/1MB),(Get-FileHash -LiteralPath $result).Hash)
