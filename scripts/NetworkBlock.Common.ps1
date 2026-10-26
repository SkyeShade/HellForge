# Persistent network block for the research copy. Scripts/Install-NetworkBlock.ps1 adds the rules once
# (administrator consent); every session then only reads them back here, without elevation.
# Only the copy's own programs and the block's network probe are listed, never the installed game or
# Steam, so the normal game is unaffected. Loopback stays reachable for the local backend.
# A HellForge installation ($HellForgeInstallRoot, set by the caller before dot-sourcing) has its own rule group named after its
# folder, so blocks for several copies never replace each other, and keeps the block's probe inside the installation.
# Without it the research copy's group and folder are used. $HellForgeLegacyBlockGroup is the same group's name from before
# the rename (HD2 Beyond The Stars, "BTS-"), removed when the block is installed again.
$hellforgeInstall=Get-Variable -Name HellForgeInstallRoot -ValueOnly -ErrorAction SilentlyContinue
if($hellforgeInstall){
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$hash=$sha.ComputeHash([Text.Encoding]::UTF8.GetBytes([IO.Path]::GetFullPath($hellforgeInstall).ToLowerInvariant()))}
    finally{$sha.Dispose()}
    $tag=[BitConverter]::ToString($hash).Replace('-','').Substring(0,12)
    $HellForgeBlockGroup='HellForge-NetworkBlock-'+$tag
    $HellForgeLegacyBlockGroup='BTS-NetworkBlock-'+$tag
    $HellForgeBlockRoot=Join-Path $hellforgeInstall 'network-block'
    # The WFP block (hellforge_wfp) identifies this installation's filters by a provider GUID made from the same hash.
    $HellForgeWfpProvider=[Guid]::new([byte[]]($hash[0..15])).ToString('B')
}else{
    $HellForgeBlockGroup='HellForge-NetworkBlock'
    $HellForgeLegacyBlockGroup='BTS-NetworkBlock'
    $HellForgeBlockRoot=Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts/host-startup/network-block'
    $HellForgeWfpProvider='{00000000-0000-0000-0000-000000000000}'
}
$HellForgeBlockRemote=@('0.0.0.0-126.255.255.255','128.0.0.0-255.255.255.255','::2-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff')
$HellForgeBlockProbe=Join-Path $HellForgeBlockRoot 'NetworkProbe.exe'
# The WFP block tool ships beside these scripts; its marker, dropped on a successful install, tells a session the
# block is WFP (so it does not require the Windows Firewall rules). Removed on uninstall.
$HellForgeWfpTool=Join-Path $PSScriptRoot 'hellforge_wfp.exe'
$HellForgeWfpMarker=Join-Path $HellForgeBlockRoot 'wfp-block.json'

# Compiles the block's probe (OfflineControlNetworkProbe.cs) to $HellForgeBlockProbe. Windows PowerShell's compiler
# (csc) gets its paths in the system's ANSI code page, so an installation folder with other characters (a Chinese folder
# name on an English Windows) fails ("The filename, directory name, or volume label syntax is incorrect"): it compiles in a
# plain folder under Windows\Temp (the scripts run as administrator) and the probe is copied into place.
function New-HellForgeBlockProbe{
    $build=Join-Path $env:SystemRoot ('Temp\hellforge-probe-'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $build|Out-Null
    $temp=$env:TEMP;$tmp=$env:TMP
    try{
        $env:TEMP=$env:TMP=$build
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'OfflineControlNetworkProbe.cs') -Destination (Join-Path $build 'probe.cs')
        Add-Type -Path (Join-Path $build 'probe.cs') -OutputAssembly (Join-Path $build 'NetworkProbe.exe') -OutputType ConsoleApplication
        # copied, not moved: a moved file keeps Windows\Temp's permissions, which do not let the player run it
        Copy-Item -LiteralPath (Join-Path $build 'NetworkProbe.exe') -Destination $HellForgeBlockProbe -Force
    }finally{
        $env:TEMP=$temp;$env:TMP=$tmp
        Remove-Item -LiteralPath $build -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Refuses a path when it or a folder above it is a link (a reparse point), which could send the block's files or rules
# elsewhere. It names what it found, since the player has to undo it: junctions, symbolic links, OneDrive and other cloud
# placeholders, and files compressed with CompactGUI or compact /exe are all reparse points.
function No-Links([string]$Path){
    $item=Get-Item -LiteralPath $Path -Force
    while($item){
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){
            throw ('Linked target path: '+$item.FullName+' is a link or a special file (a junction or symbolic link, a OneDrive or other '+
                'cloud placeholder, or a file compressed with CompactGUI or compact /exe). Move HellForge to a plain folder, or undo the '+
                'link or compression there, then run Settings > Installation.')
        }
        $item=if($item -is [IO.DirectoryInfo]){$item.Parent}else{$item.Directory}
    }
}

function Get-HellForgeBlockedPrograms([string]$Copy){
    $programs=[Collections.Generic.List[string]]::new()
    foreach($file in Get-ChildItem -LiteralPath $Copy -File -Recurse){
        if($file.Extension -in @('.exe','.des','.now')){$programs.Add($file.FullName)}
    }
    $programs.Add([IO.Path]::GetFullPath($HellForgeBlockProbe))
    return @($programs|Sort-Object -Unique)
}

# Returns the reasons the block is not effective (empty when every program has both directions blocked).
function Test-HellForgeNetworkBlock([string[]]$Programs){
    $problems=[Collections.Generic.List[string]]::new()
    foreach($name in @('BFE','MpsSvc')){if((Get-Service $name).Status -ne 'Running'){$problems.Add("Firewall service $name is not running.")}}
    if(@(Get-NetFirewallProfile -PolicyStore ActiveStore|Where-Object Enabled -ne True).Count){$problems.Add('Windows Firewall is off for a network type (Domain, Private or Public). HellForge needs it on for all of them; another security program may have switched it off (Windows Security > Firewall & network protection).')}
    # Rules outlive a deleted folder; a reinstall at the same path then finds them but not the probe they list.
    if(-not(Test-Path -LiteralPath $HellForgeBlockProbe)){$problems.Add('The network block probe is missing: '+$HellForgeBlockProbe)}
    $rules=@(Get-NetFirewallRule -PolicyStore ActiveStore -Group $HellForgeBlockGroup -ErrorAction SilentlyContinue)
    if(-not $rules.Count){$problems.Add('The network block is not installed.');return $problems.ToArray()}
    $applications=@{};$addresses=@{};$ports=@{}
    foreach($filter in @($rules|Get-NetFirewallApplicationFilter)){$applications[$filter.InstanceID]=$filter.Program}
    foreach($filter in @($rules|Get-NetFirewallAddressFilter)){$addresses[$filter.InstanceID]=@($filter.RemoteAddress)}
    foreach($filter in @($rules|Get-NetFirewallPortFilter)){$ports[$filter.InstanceID]=$filter.Protocol}
    $covered=@{}
    foreach($rule in $rules){
        if($rule.Enabled -ne 'True' -or $rule.Action -ne 'Block' -or $rule.Profile -ne 'Any' -or $ports[$rule.Name] -ne 'Any'){continue}
        if(@(Compare-Object ($HellForgeBlockRemote|Sort-Object) (@($addresses[$rule.Name])|Sort-Object)).Count){continue}
        $covered[''+$applications[$rule.Name]+'|'+$rule.Direction]=$true
    }
    foreach($program in $Programs){foreach($direction in @('Inbound','Outbound')){
        if(-not $covered[$program+'|'+$direction]){$problems.Add("Not blocked ($direction): $program")}
    }}
    return $problems.ToArray()
}
