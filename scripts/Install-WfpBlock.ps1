# One-time, elevated: installs HellForge's network block for an installation straight into the Windows Filtering
# Platform (WFP) with hellforge_wfp, instead of Windows Firewall rules (Install-NetworkBlock.ps1). The copied game's
# programs can then reach only the local backend's loopback port (-BackendPort, chosen by setup: InstallLayout's
# ComputedBackendPort); the block holds even with Windows Firewall off and a
# security suite's own permit cannot override it. Setup runs this and does not finish until it reports "installed".
# -Uninstall removes the WFP filters, the marker and the block's probe. A successful install drops a marker
# (network-block/wfp-block.json) so a session knows the block is WFP and does not require the firewall rules.
param([switch]$Uninstall,[string]$ReportPath,[Parameter(Mandatory)][string]$InstallRoot,[int]$BackendPort)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$HellForgeInstallRoot=[IO.Path]::GetFullPath($InstallRoot)
if(-not(Test-Path -LiteralPath (Join-Path $HellForgeInstallRoot 'hellforge-install.json')) -and -not(Test-Path -LiteralPath (Join-Path $HellForgeInstallRoot 'bts-install.json'))){throw 'Not a HellForge installation: '+$HellForgeInstallRoot}
$env:TEMP=$env:TMP=(New-Item -ItemType Directory (Join-Path $HellForgeInstallRoot 'temp') -Force).FullName
$copy=Join-Path $HellForgeInstallRoot 'game'
. (Join-Path $PSScriptRoot 'NetworkBlock.Common.ps1')
$state=[ordered]@{action=$(if($Uninstall){'uninstall'}else{'install'});status='preparing';block='wfp'}
# Runs the tool and returns its report object; throws with the tool's own words on failure.
function Invoke-Wfp([string[]]$Arguments){
    $report=Join-Path $HellForgeBlockRoot 'wfp-tool.json'
    & $HellForgeWfpTool @Arguments $report|Out-Null
    $r=if(Test-Path -LiteralPath $report){Get-Content -Raw -LiteralPath $report|ConvertFrom-Json}else{$null}
    if(-not $r){throw 'The WFP block tool wrote no report.'}
    return $r
}
try{
    $state.group=$HellForgeWfpProvider
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    if(-not([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run this script as administrator.'}
    if(-not(Test-Path -LiteralPath $HellForgeWfpTool)){throw 'The WFP block tool is missing: '+$HellForgeWfpTool}
    # The Base Filtering Engine carries WFP filters; unlike the firewall rules, Windows Firewall itself need not be on.
    if((Get-Service BFE).Status -ne 'Running'){throw 'The Windows Base Filtering Engine (BFE) service is not running; HellForge needs it to place the network block.'}
    if($Uninstall){
        $r=Invoke-Wfp @('uninstall',$HellForgeWfpProvider)
        if($r.status -ne 'uninstalled'){throw ('The WFP filters were not removed: '+$r.failure)}
        if(Test-Path -LiteralPath $HellForgeBlockRoot){Remove-Item -LiteralPath $HellForgeBlockRoot -Recurse -Force}
        $state.status='uninstalled'
    }else{
        if($BackendPort -lt 1 -or $BackendPort -gt 65535){throw 'Install needs -BackendPort, the local backend''s port (1-65535).'}
        No-Links $copy
        New-Item -ItemType Directory $HellForgeBlockRoot -Force|Out-Null
        No-Links $HellForgeBlockRoot
        # The probe is one of the blocked programs, so it must exist before the program list is gathered. Built every
        # time: a probe from an earlier HellForge lacks the other-loopback-port check below.
        New-HellForgeBlockProbe
        # WFP keys filters to a program's application id, read from the file, so only existing files are listed.
        $programs=@(Get-HellForgeBlockedPrograms $copy|Where-Object {Test-Path -LiteralPath $_})
        foreach($path in $programs){No-Links $path}
        $programsFile=Join-Path $HellForgeBlockRoot 'wfp-programs.txt'
        [IO.File]::WriteAllLines($programsFile,$programs)
        $r=Invoke-Wfp @('install',$HellForgeWfpProvider,'HellForge network block',$programsFile,[string]$BackendPort)
        if($r.status -ne 'installed'){throw ('The WFP block was not installed: '+$r.failure)}
        # Prove the block enforces: the probe (one of the blocked programs) must be refused reaching the outside and any
        # other loopback port (WSAEACCES, 10013), while the backend's port is let through. Nothing listens there yet, so
        # that connect is refused by the stack (10061, or 0 if something does listen), not by the block.
        $probeReport=Join-Path $HellForgeBlockRoot 'wfp-egress.json'
        $otherPort=if($BackendPort -lt 65535){$BackendPort+1}else{$BackendPort-1}
        & $HellForgeBlockProbe $BackendPort $probeReport $otherPort|Out-Null
        $egress=if(Test-Path -LiteralPath $probeReport){Get-Content -Raw -LiteralPath $probeReport|ConvertFrom-Json}else{$null}
        if(-not $egress -or $egress.externalIpv4TcpError -ne 10013){
            throw 'The network block is not stopping outside connections (another firewall, a VPN or a security suite may be letting traffic past the WFP filters).'
        }
        if($egress.otherLoopbackTcpError -ne 10013){throw ('The network block lets the copy reach loopback ports other than the backend''s ('+$otherPort+').')}
        if($egress.loopbackTcpError -eq 10013){throw ('The network block refuses the backend''s own port ('+$BackendPort+').')}
        $marker=[ordered]@{block='wfp';provider=$HellForgeWfpProvider;port=$BackendPort;filters=$r.filters;programs=$programs.Count;installedUtc=[DateTime]::UtcNow.ToString('o')}
        [IO.File]::WriteAllText($HellForgeWfpMarker,($marker|ConvertTo-Json))
        $state.programs=$programs.Count;$state.filters=$r.filters;$state.egressBlocked=$true;$state.status='installed'
    }
}catch{$state.status='failed';$state.failure=$_.Exception.Message}
$state.completedUtc=[DateTime]::UtcNow.ToString('o')
$json=$state|ConvertTo-Json -Depth 4
if($ReportPath){[IO.File]::WriteAllText($ReportPath,$json)}else{$json}
if($state.status -eq 'failed'){exit 1}
