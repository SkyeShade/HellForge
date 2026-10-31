# Fetches the tools a code mod is built with into tools/ (pinned, checksummed, nothing installed system-wide): Zig as the
# C/C++ compiler (the same one the mod loader is built with), CMake and Ninja for IDEs that open CMake projects
# (Rider, CLion, VS Code). See docs/modding/tools.md. Usage: scripts/Setup-ModTools.ps1
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
$project=Split-Path -Parent $PSScriptRoot
$tools=Join-Path $project 'tools'
$downloads=Join-Path $tools 'downloads'

function Get-Pinned([string]$Url,[string]$Sha256,[string]$Path){
    if((Test-Path -LiteralPath $Path) -and (Get-FileHash -LiteralPath $Path).Hash -eq $Sha256){return}
    New-Item -ItemType Directory (Split-Path -Parent $Path) -Force|Out-Null
    $partial=$Path+'.partial'
    Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing
    if((Get-FileHash -LiteralPath $partial).Hash -ne $Sha256){Remove-Item -LiteralPath $partial;throw ('Checksum mismatch: '+$Url)}
    Move-Item -LiteralPath $partial -Destination $Path -Force
}
# Unpacks an archive's single top-level folder (or its files) into $Destination, once per archive.
function Install-Archive([string]$Archive,[string]$Destination){
    $marker=Join-Path $Destination '.hellforge-archive-sha256'
    $sha=(Get-FileHash -LiteralPath $Archive).Hash
    if((Test-Path -LiteralPath $marker) -and [IO.File]::ReadAllText($marker) -ceq $sha){return}
    $staging=$Destination+'-staging'
    foreach($folder in @($staging,$Destination)){if(Test-Path -LiteralPath $folder){Remove-Item -LiteralPath $folder -Recurse -Force}}
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive,$staging)
    $entries=@(Get-ChildItem -LiteralPath $staging)
    if($entries.Count -eq 1 -and $entries[0].PSIsContainer){Move-Item -LiteralPath $entries[0].FullName -Destination $Destination;Remove-Item -LiteralPath $staging}
    else{Move-Item -LiteralPath $staging -Destination $Destination}
    [IO.File]::WriteAllText($marker,$sha)
}

# The pins are shared with HellForge.Cli mod-tools (installations): native/mod-sdk/tools.json. This Windows setup keeps
# the Windows-host tools; zig-linux is for a Linux host (HellForge.Cli mod-tools there).
$pins=@((Get-Content -LiteralPath (Join-Path $project 'native/mod-sdk/tools.json') -Raw|ConvertFrom-Json).tools|
    Where-Object {$_.platform -eq 'windows-x64'}|
    ForEach-Object {@{name=$_.name;url=$_.url;sha=$_.sha256;exe=$_.exe}})
foreach($tool in $pins){
    $destination=Join-Path $tools $tool.name
    # Get-BuildTools.ps1 installs the same Zig (with a marker of its own); an existing one of the pinned version is kept.
    if($tool.name -eq 'zig' -and (Test-Path -LiteralPath (Join-Path $destination 'zig.exe')) -and (& (Join-Path $destination 'zig.exe') version) -eq '0.17.0'){continue}
    $archive=Join-Path $downloads (Split-Path -Leaf $tool.url)
    Get-Pinned $tool.url $tool.sha $archive
    Install-Archive $archive $destination
}
foreach($tool in $pins){Write-Output ('{0,-6} {1}' -f $tool.name,(Join-Path (Join-Path $tools $tool.name) $tool.exe))}
