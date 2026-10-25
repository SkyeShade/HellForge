$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    # A single non-reused worker also works in restricted agent environments.
    dotnet build HellForge.slnx --nologo -m:1 -nr:false -p:UseSharedCompilation=false
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    dotnet run --project tests/HellForge.Tests --no-build
    if ($LASTEXITCODE -ne 0) { throw 'Integration checks failed.' }
    dotnet run --project tests/HellForge.LocalBackend.Tests --no-build
    if ($LASTEXITCODE -ne 0) { throw 'Local backend checks failed.' }

    # NTFS junctions need no symbolic-link privilege. Keep all fixtures in artifacts.
    $fixtureRoot = Join-Path $projectRoot ('artifacts/path-tests/' + [guid]::NewGuid().ToString('N'))
    $steamApps = Join-Path $fixtureRoot 'Steam/steamapps'
    $source = Join-Path $steamApps 'common/Helldivers 2'
    New-Item -ItemType Directory -Path (Join-Path $source 'bin'), (Join-Path $source 'data/game') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $source 'bin/helldivers2.exe') -Value 'synthetic executable'
    Set-Content -LiteralPath (Join-Path $source 'data/game/game.dll') -Value 'synthetic game library'
    $manifest = Join-Path $steamApps 'appmanifest_553850.acf'
    Set-Content -LiteralPath $manifest -Value '"AppState" { "appid" "553850" "StateFlags" "4" "installdir" "Helldivers 2" "buildid" "12345" }'
    $cli = Join-Path $projectRoot 'src/HellForge.Cli/bin/Debug/net10.0/HellForge.Cli.dll'
    $report = (& dotnet $cli inspect --manifest $manifest | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0) { throw 'Fixture inspection failed.' }

    $linkedParent = Join-Path $fixtureRoot 'linked-parent'
    New-Item -ItemType Junction -Path $linkedParent -Target $source | Out-Null
    & dotnet $cli prepare --manifest $manifest --destination (Join-Path $linkedParent 'HellForge') --build $report.pin.buildId --exe-sha256 $report.pin.exeSha256 --game-sha256 $report.pin.gameDllSha256
    if ($LASTEXITCODE -ne 1) { throw 'Linked destination was accepted.' }
    if (Test-Path -LiteralPath (Join-Path $source 'HellForge')) { throw 'Linked destination modified source.' }
    Write-Output 'PASS linked destination rejected without source writes'

    $outside = Join-Path $fixtureRoot 'outside'
    New-Item -ItemType Directory -Path $outside | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $source 'linked-data') -Target $outside | Out-Null
    & dotnet $cli inspect --manifest $manifest
    if ($LASTEXITCODE -ne 1) { throw 'Linked source subtree was accepted.' }
    Write-Output 'PASS linked source subtree rejected'

    & dotnet $cli inspect --manfiest $manifest
    if ($LASTEXITCODE -ne 1) { throw 'Unknown CLI option was accepted.' }
    Write-Output 'PASS unknown CLI option rejected'

    # The verified list's entry for a mod: its content hash and class, and the entry to copy.
    $hashed = (& dotnet $cli mod-hash --mod (Join-Path $projectRoot 'examples/mods/machine-gun-10k-code') | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $hashed.hash -notmatch '^[0-9a-f]{64}$' -or $hashed.class -ne 'code' -or
        $hashed.entry.hash -ne $hashed.hash) { throw 'mod-hash did not report the example mod.' }
    Write-Output 'PASS mod-hash reports a mod''s content hash, class and verified-list entry'
    Write-Output 'All checks passed. Fixtures remain under artifacts for review.'
}
finally {
    Pop-Location
}
