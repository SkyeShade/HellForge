# One-time, elevated: adds persistent Windows firewall rules that block the research copy's programs from
# everything but loopback (see NetworkBlock.Common.ps1). Sessions then start without a Windows prompt.
# -Uninstall removes the rules and the block's probe. Rerun after the research copy is prepared again.
# -InstallRoot blocks a HellForge installation's game instead, with that installation's own rule group.
param([switch]$Uninstall,[string]$ReportPath,[string]$InstallRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if($InstallRoot){
    $HellForgeInstallRoot=[IO.Path]::GetFullPath($InstallRoot)
    # bts-install.json: an installation from before the rename (HD2 Beyond The Stars).
    if(-not(Test-Path -LiteralPath (Join-Path $HellForgeInstallRoot 'hellforge-install.json')) -and -not(Test-Path -LiteralPath (Join-Path $HellForgeInstallRoot 'bts-install.json'))){throw 'Not a HellForge installation: '+$HellForgeInstallRoot}
    # The probe is compiled here; keep the compiler's temporary files inside the installation.
    $env:TEMP=$env:TMP=(New-Item -ItemType Directory (Join-Path $HellForgeInstallRoot 'temp') -Force).FullName
    $copy=Join-Path $HellForgeInstallRoot 'game'
}else{
    $copy=Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts/host-startup/research-copy/game'
}
. (Join-Path $PSScriptRoot 'NetworkBlock.Common.ps1')
$state=[ordered]@{action=$(if($Uninstall){'uninstall'}else{'install'});status='preparing'}
try{
    if(-not $InstallRoot -and $env:HELLFORGE_RESEARCH -ne '1'){throw 'Without -InstallRoot this is the research copy: set HELLFORGE_RESEARCH=1 to use it.'}
    $state.group=$HellForgeBlockGroup
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    if(-not([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run this script as administrator.'}
    Get-NetFirewallRule -PolicyStore PersistentStore -Group $HellForgeBlockGroup -ErrorAction SilentlyContinue|Remove-NetFirewallRule
    # The same block under its name from before the rename.
    Get-NetFirewallRule -PolicyStore PersistentStore -Group $HellForgeLegacyBlockGroup -ErrorAction SilentlyContinue|Remove-NetFirewallRule
    if($Uninstall){
        if(Test-Path -LiteralPath $HellForgeBlockRoot){Remove-Item -LiteralPath $HellForgeBlockRoot -Recurse -Force}
        $state.rulesRemoved=(@(Get-NetFirewallRule -PolicyStore ActiveStore -Group $HellForgeBlockGroup -ErrorAction SilentlyContinue).Count -eq 0)
        if(-not $state.rulesRemoved){throw 'Some network block rules remain.'}
        $state.status='uninstalled'
    }else{
        No-Links $copy
        New-Item -ItemType Directory $HellForgeBlockRoot -Force|Out-Null
        No-Links $HellForgeBlockRoot
        if(-not(Test-Path -LiteralPath $HellForgeBlockProbe)){
            New-HellForgeBlockProbe
        }
        $programs=Get-HellForgeBlockedPrograms $copy
        foreach($path in $programs){No-Links $path}
        $index=0
        foreach($path in $programs){foreach($direction in @('Inbound','Outbound')){
            $name=$HellForgeBlockGroup+'-'+($index++).ToString('000')
            New-NetFirewallRule -Name $name -DisplayName $name -Group $HellForgeBlockGroup -Direction $direction -Action Block -Enabled True -Profile Any -Program $path -Protocol Any -RemoteAddress $HellForgeBlockRemote -PolicyStore PersistentStore|Out-Null
        }}
        $problems=@(Test-HellForgeNetworkBlock $programs)
        if($problems.Count){throw ('Rule verification failed: '+$problems[0])}
        $state.programs=$programs;$state.ruleCount=$index;$state.status='installed'
    }
}catch{$state.status='failed';$state.failure=$_.Exception.Message}
$state.completedUtc=[DateTime]::UtcNow.ToString('o')
$json=$state|ConvertTo-Json -Depth 4
if($ReportPath){[IO.File]::WriteAllText($ReportPath,$json)}else{$json}
if($state.status -eq 'failed'){exit 1}
