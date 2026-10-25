# Makes a second HellForge installation beside an existing one, for several installations on one PC (test mode,
# docs/multi-instance.md). The supported build's files in game\data are hard links to the existing installation's copy
# (no disk space); game\bin, game\tools and data\settings.ini are real copies, since sessions write those in place. The
# app, mods, profiles and user data are copies. Staged mod patches are left out: each installation stages its own.
#   scripts\New-TestInstallation.ps1 -Main C:\HellForge -Instance C:\HellForge-2
param(
    [string]$Main = 'C:\HellForge',
    [Parameter(Mandatory)][string]$Instance,
    [string]$BuildList = (Join-Path $PSScriptRoot '..\src\HellForge.Core\supported-build.json')
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Instance) { throw "$Instance exists already." }
$build = Get-Content -LiteralPath $BuildList -Raw | ConvertFrom-Json
$files = if ($build.files) { $build.files } else { $build.Files }

New-Item -ItemType Directory -Path $Instance | Out-Null
foreach ($folder in 'app', 'mods', 'profiles', 'userdata') { Copy-Item -LiteralPath (Join-Path $Main $folder) -Destination (Join-Path $Instance $folder) -Recurse }
foreach ($file in 'HellForge.exe', 'hellforge-install.json', 'launcher-settings.json', 'live-war.json') {
    if (Test-Path -LiteralPath (Join-Path $Main $file)) { Copy-Item -LiteralPath (Join-Path $Main $file) -Destination $Instance }
}
New-Item -ItemType Directory -Path (Join-Path $Instance 'temp'), (Join-Path $Instance 'logs') | Out-Null
# the existing copy's verification cache: entries are keyed by path, size and write time, which hard links share
$cache = Join-Path $Main 'temp\verified-game.json'
if (Test-Path -LiteralPath $cache) { Copy-Item -LiteralPath $cache -Destination (Join-Path $Instance 'temp') }

$game = Join-Path $Instance 'game'
Copy-Item -LiteralPath (Join-Path $Main 'game\bin') -Destination (Join-Path $game 'bin') -Recurse
Copy-Item -LiteralPath (Join-Path $Main 'game\tools') -Destination (Join-Path $game 'tools') -Recurse
$links = 0
foreach ($file in $files) {
    $path = if ($file.path) { $file.path } else { $file.Path }
    if (-not $path.StartsWith('data/')) { continue }
    $from = Join-Path $Main ('game\' + $path.Replace('/', '\'))
    $to = Join-Path $game $path.Replace('/', '\')
    New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force | Out-Null
    if ($path -eq 'data/settings.ini') {
        # the game's own settings.ini (a session points it at this installation's user data)
        $original = $from + '.hellforge-original'
        Copy-Item -LiteralPath $(if (Test-Path -LiteralPath $original) { $original } else { $from }) -Destination $to
        if (Test-Path -LiteralPath $original) { Copy-Item -LiteralPath $original -Destination ($to + '.hellforge-original') }
        continue
    }
    New-Item -ItemType HardLink -Path $to -Target $from | Out-Null
    $links++
}
"$Instance made: $links hard-linked game files; bin, tools, settings.ini, app, mods, profiles and user data copied."
