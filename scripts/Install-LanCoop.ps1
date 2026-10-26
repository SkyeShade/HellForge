# One-time, elevated: lets the local network reach this installation's backend, for LAN co-op (Settings > Multiplayer).
# The copied game stays behind the network block and talks only to the backend; the backend is what talks to the other
# players, so only it is opened, and only to the local subnet. A session runs this with one Windows prompt when the
# player turns LAN co-op on; the marker (network-block/lan-coop.json) then tells every later session it is done.
# -Uninstall removes the rules and the marker (the launcher's uninstall runs this).
param([switch]$Uninstall,[string]$ReportPath,[Parameter(Mandatory)][string]$InstallRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$HellForgeInstallRoot=[IO.Path]::GetFullPath($InstallRoot)
if(-not(Test-Path -LiteralPath (Join-Path $HellForgeInstallRoot 'hellforge-install.json')) -and -not(Test-Path -LiteralPath (Join-Path $HellForgeInstallRoot 'bts-install.json'))){throw 'Not a HellForge installation: '+$HellForgeInstallRoot}
$env:TEMP=$env:TMP=(New-Item -ItemType Directory (Join-Path $HellForgeInstallRoot 'temp') -Force).FullName
. (Join-Path $PSScriptRoot 'NetworkBlock.Common.ps1')
# The block's own group tag names this installation; the LAN rules share it so several copies never share rules.
$HellForgeLanGroup=$HellForgeBlockGroup.Replace('NetworkBlock','LanCoop')
$HellForgeLanMarker=Join-Path $HellForgeBlockRoot 'lan-coop.json'
$backend=Join-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'backend') 'HellForge.LocalBackend.exe'
$state=[ordered]@{action=$(if($Uninstall){'uninstall'}else{'install'});status='preparing';group=$HellForgeLanGroup}
function No-Links([string]$Path){
    $item=Get-Item -LiteralPath $Path -Force
    while($item){if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Linked target path.'};$item=if($item -is [IO.DirectoryInfo]){$item.Parent}else{$item.Directory}}
}
try{
    if(-not([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run this script as administrator.'}
    Get-NetFirewallRule -PolicyStore PersistentStore -Group $HellForgeLanGroup -ErrorAction SilentlyContinue|Remove-NetFirewallRule
    if($Uninstall){
        if(Test-Path -LiteralPath $HellForgeLanMarker){Remove-Item -LiteralPath $HellForgeLanMarker -Force}
        $state.rulesRemoved=(@(Get-NetFirewallRule -PolicyStore ActiveStore -Group $HellForgeLanGroup -ErrorAction SilentlyContinue).Count -eq 0)
        if(-not $state.rulesRemoved){throw 'Some LAN co-op rules remain.'}
        $state.status='uninstalled'
    }else{
        if(-not(Test-Path -LiteralPath $backend)){throw 'The backend is missing: '+$backend}
        No-Links $backend
        No-Links $HellForgeBlockRoot
        # The backend serves the game on loopback (always open) and the other players on the local network: allow only
        # the local subnet, TCP for the frames and UDP for finding each other, in both protocols for its whole lifetime.
        foreach($protocol in @('TCP','UDP')){
            New-NetFirewallRule -Name ($HellForgeLanGroup+'-'+$protocol) -DisplayName ('HellForge LAN co-op ('+$protocol+')') -Group $HellForgeLanGroup `
                -Direction Inbound -Action Allow -Enabled True -Profile Any -Program $backend -Protocol $protocol -RemoteAddress LocalSubnet `
                -PolicyStore PersistentStore|Out-Null
        }
        $rules=@(Get-NetFirewallRule -PolicyStore PersistentStore -Group $HellForgeLanGroup -ErrorAction SilentlyContinue)
        if($rules.Count -ne 2){throw 'The LAN co-op rules were not added.'}
        $state.rules=@($rules|ForEach-Object{$_.DisplayName})
        [IO.File]::WriteAllText($HellForgeLanMarker,([ordered]@{status='installed';group=$HellForgeLanGroup;backend=$backend;rules=$state.rules}|ConvertTo-Json -Depth 4))
        $state.status='installed'
    }
}catch{
    $state.status='failed'
    $state.failure=$_.Exception.Message
}
if($ReportPath){[IO.File]::WriteAllText($ReportPath,($state|ConvertTo-Json -Depth 4))}
if($state.status -eq 'failed'){throw $state.failure}
$state|ConvertTo-Json -Depth 4|Write-Output
