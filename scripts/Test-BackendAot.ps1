# Compares the NativeAOT backend (artifacts/aot-backend) with the JIT build on the same requests: every answer must be
# byte-identical (session ids, mission ids and a new profile's ship name aside, which are random by design). A check that the ahead-of-time build
# behaves as the tested one does.
param([string]$Aot)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
if(-not $Aot){$Aot=Join-Path $project 'artifacts/aot-backend/HellForge.LocalBackend.exe'}
$jit=Join-Path $project 'src/HellForge.LocalBackend/bin/Debug/net10.0/win-x64/HellForge.LocalBackend.dll'
# The JIT build to compare with, built now from the same sources: a fresh clone has none, and one left from earlier would
# compare the AOT build with older code.
$jitBuild=@(dotnet build (Join-Path $project 'src/HellForge.LocalBackend/HellForge.LocalBackend.csproj') -c Debug --nologo -v q)
if($LASTEXITCODE -ne 0){throw ('Building the JIT backend failed: '+($jitBuild -join "`n"))}
$work=Join-Path $project ('artifacts/backend-aot-test/'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work|Out-Null
function Start-Backend([string]$Name,[string]$File,[string[]]$Prefix){
    $evidence=Join-Path $work ($Name+'-evidence');$profileDir=Join-Path $work ($Name+'-profile')
    # A synthetic game catalogue: HellForge ships none (GameCatalog), and any will do for comparing the two builds.
    $catalogue=Join-Path $project 'tests/fixtures/synthetic-catalogue'
    $arguments=@($Prefix)+@('--port','0','--startup-fixtures','true','--lifetime-seconds','60','--evidence',('"'+$evidence+'"'),'--profile-dir',('"'+$profileDir+'"'),'--profile-template','unlock-all','--catalogue',('"'+$catalogue+'"'))
    $process=Start-Process $File -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $work ($Name+'-out.txt')) -RedirectStandardError (Join-Path $work ($Name+'-err.txt'))
    $script:started+=$process
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while(-not(Test-Path (Join-Path $evidence 'ready.json'))){if($process.HasExited -or $clock.Elapsed.TotalSeconds -gt 30){throw ($Name+' backend did not become ready: '+(Get-Content (Join-Path $work ($Name+'-err.txt')) -Raw))};Start-Sleep -Milliseconds 100}
    Start-Sleep -Milliseconds 100
    $ready=Get-Content (Join-Path $evidence 'ready.json') -Raw|ConvertFrom-Json
    [pscustomobject]@{Process=$process;Url=$ready.urls[0];Ready=$ready}
}
# Both backends stop however the check ends: one left running keeps its program locked, and the next engine build cannot
# replace it.
$script:started=@()
try{
$a=Start-Backend 'aot' $Aot @()
$j=Start-Backend 'jit' 'dotnet' @(('"'+$jit+'"'))
Add-Type -AssemblyName System.Net.Http
$client=[Net.Http.HttpClient]::new()
function Ask($Base,[string]$Method,[string]$Path,[string]$Body){
    $request=[Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method),$Base+$Path)
    if($Body){$request.Content=[Net.Http.StringContent]::new($Body,[Text.Encoding]::UTF8,'application/json')}
    $response=$client.SendAsync($request).GetAwaiter().GetResult()
    [pscustomobject]@{Status=[int]$response.StatusCode;Body=$response.Content.ReadAsStringAsync().GetAwaiter().GetResult().TrimEnd()}
}
$requests=@(
    @('GET','/config/locator.json',''),@('POST','/config/locator.json',''),
    @('POST','/Account/Login','{"gmiStr":"x"}'),@('POST','/Account/Login','[1]'),
    @('GET','/Progression','') ,@('GET','/Progression/items',''),@('GET','/Progression/inventory',''),@('GET','/Progression/Items/Customize/v2',''),
    @('GET','/WarSeason/801/timeSinceStart',''),@('GET','/Account/Keys?id=12345',''),
    @('POST','/Operation/Create','{"operations":[{"id":7,"difficulty":5}]}'),@('POST','/Operation/Mission/Activity','{}'),
    @('POST','/Operation/Mission/end','{"operationId":7,"missionSuccess":true,"objectives":[{"succeeded":true}],"lootedItems":[{"mixId":3608481516,"amount":3}]}'),
    @('PUT','/Progression/ShipName','{"name":"Test"}'),@('POST','/Account/ReportPosition','{"x":1}'),@('GET','/does/not/exist',''))
$failed=0
foreach($r in $requests){
    $x=Ask $a.Url $r[0] $r[1] $r[2];$y=Ask $j.Url $r[0] $r[1] $r[2]
    $bx=$x.Body -replace 'hellforge-local-[0-9a-f]{32}','SESSION' -replace '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}','GUID' -replace '"secondsSinceStart":\d+','"secondsSinceStart":N' -replace '127\.0\.0\.1:\d+','127.0.0.1:PORT' -replace '"shipName":\[\d+,\d+\]','"shipName":RANDOM'
    $by=$y.Body -replace 'hellforge-local-[0-9a-f]{32}','SESSION' -replace '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}','GUID' -replace '"secondsSinceStart":\d+','"secondsSinceStart":N' -replace '127\.0\.0\.1:\d+','127.0.0.1:PORT' -replace '"shipName":\[\d+,\d+\]','"shipName":RANDOM'
    if($r[1] -eq '/Progression'){[IO.File]::WriteAllText((Join-Path $work 'progression-dump-aot.json'),$bx);[IO.File]::WriteAllText((Join-Path $work 'progression-dump-jit.json'),$by)}
    if($x.Status -ne $y.Status -or $bx -cne $by){$failed++;Write-Output ("DIFF {0} {1}: aot {2} {3} | jit {4} {5}" -f $r[0],$r[1],$x.Status,$bx.Substring(0,[Math]::Min(160,$bx.Length)),$y.Status,$by.Substring(0,[Math]::Min(160,$by.Length)))}
    else{Write-Output ("same {0} {1} -> {2} ({3} bytes)" -f $r[0],$r[1],$x.Status,$x.Body.Length)}
}
$audit=@(Get-Content (Join-Path $work 'aot-evidence/requests.jsonl')|ForEach-Object {$_|ConvertFrom-Json})
Write-Output ("aot audit: {0} lines, first: {1}" -f $audit.Count,(Get-Content (Join-Path $work 'aot-evidence/requests.jsonl')|Select-Object -First 1))
}finally{foreach($p in $script:started){if(-not $p.HasExited){$p.Kill();[void]$p.WaitForExit(5000)}}}
if($failed){throw "$failed answer(s) differ."}
Write-Output 'The AOT backend answers as the JIT build does.'




