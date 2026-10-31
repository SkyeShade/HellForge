# Authenticode-signs HellForge's own programs and DLLs, when a signing certificate is configured on this PC
# (docs/code-signing.md). Without one it signs nothing, says so once, and the build goes on as before: signing is optional.
# Windows 11's Smart App Control blocks unsigned programs and DLLs it does not know (src/HellForge.Core/SmartAppControl.cs).
# -Path: files or folders (their .exe and .dll files, recursively). A file that already carries a valid signature
# (Microsoft's .NET runtime files, a file signed by an earlier step) is left as it is. Sign before anything records the
# files' SHA-256 (build.json, engine.json): signing changes the file.
# Configured through the environment, never committed:
#   HELLFORGE_SIGN_DLIB, HELLFORGE_SIGN_METADATA  Azure Trusted Signing (Artifact Signing): the path of its
#       Azure.CodeSigning.Dlib.dll and of the metadata.json naming the account and certificate profile. The sign-in is
#       Azure's own (DefaultAzureCredential: az login, or AZURE_CLIENT_ID/AZURE_TENANT_ID/AZURE_CLIENT_SECRET in CI).
#   HELLFORGE_SIGN_CERT_SHA1  or a certificate in the Windows certificate store (an OV or EV certificate, also on a
#       hardware token), by its SHA-1 thumbprint.
#   HELLFORGE_SIGNTOOL        signtool.exe, when it is not in the Windows 10/11 SDK's usual folder.
#   HELLFORGE_SIGN_TIMESTAMP  the RFC 3161 timestamp server (by default Microsoft's for Trusted Signing, DigiCert's otherwise).
param([Parameter(Mandatory)][string[]]$Path)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$dlib=$env:HELLFORGE_SIGN_DLIB;$metadata=$env:HELLFORGE_SIGN_METADATA;$thumbprint=$env:HELLFORGE_SIGN_CERT_SHA1
if(-not(($dlib -and $metadata) -or $thumbprint)){
    if(-not $env:HELLFORGE_SIGN_SKIP_NOTED){Write-Output 'Signing skipped: no signing certificate is configured (docs/code-signing.md).';$env:HELLFORGE_SIGN_SKIP_NOTED='1'}
    return
}
$signtool=$env:HELLFORGE_SIGNTOOL
if(-not $signtool){
    $kits=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $signtool=@(Get-ChildItem -LiteralPath $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue|Where-Object {$_.Directory.Name -eq 'x64'}|Sort-Object FullName -Descending|Select-Object -First 1 -ExpandProperty FullName)
    if(-not $signtool){throw 'Signing is configured but signtool.exe was not found: install the Windows SDK or set HELLFORGE_SIGNTOOL.'}
    $signtool=$signtool[0]
}
$files=foreach($item in $Path){
    if(Test-Path -LiteralPath $item -PathType Container){Get-ChildItem -LiteralPath $item -Recurse -File|Where-Object {$_.Extension -in @('.exe','.dll')}|ForEach-Object FullName}
    elseif(Test-Path -LiteralPath $item){(Get-Item -LiteralPath $item).FullName}
    else{throw ('Nothing to sign at '+$item)}
}
$unsigned=@($files|Where-Object {(Get-AuthenticodeSignature -LiteralPath $_).Status -ne 'Valid'})
if(-not $unsigned.Count){return}
$arguments=@('sign','/fd','SHA256','/td','SHA256')
if($dlib -and $metadata){
    $arguments+=@('/tr',$(if($env:HELLFORGE_SIGN_TIMESTAMP){$env:HELLFORGE_SIGN_TIMESTAMP}else{'http://timestamp.acs.microsoft.com'}),'/dlib',$dlib,'/dmdf',$metadata)
}else{
    $arguments+=@('/tr',$(if($env:HELLFORGE_SIGN_TIMESTAMP){$env:HELLFORGE_SIGN_TIMESTAMP}else{'http://timestamp.digicert.com'}),'/sha1',$thumbprint)
}
# a few files per call: signtool takes many, and a remote signer is slow per call
for($i=0;$i -lt $unsigned.Count;$i+=20){
    $batch=$unsigned[$i..([Math]::Min($i+19,$unsigned.Count-1))]
    & $signtool @arguments @batch|Out-Null
    if($LASTEXITCODE){throw ('Signing failed (signtool exit code '+$LASTEXITCODE+'): '+($batch -join ', '))}
}
foreach($file in $unsigned){
    $status=(Get-AuthenticodeSignature -LiteralPath $file).Status
    if($status -ne 'Valid'){throw ('Signed, but Windows does not accept the signature ('+$status+'): '+$file)}
}
Write-Output ('Signed {0} file(s).' -f $unsigned.Count)
